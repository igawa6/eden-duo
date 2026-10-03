// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.Activity
import android.app.ActivityManager
import android.app.ActivityOptions
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.drawable.Drawable
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Display
import androidx.core.content.edit
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import java.lang.ref.WeakReference
import java.text.Collator
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.activities.AuxCompanionActivity
import org.yuzu.yuzu_emu.features.settings.model.AbstractSetting
import org.yuzu.yuzu_emu.features.settings.model.AbstractStringSetting
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.view.StringSingleChoiceSetting
import org.yuzu.yuzu_emu.views.AuxPresentation

/**
 * Eden Duo "No Companion: App": for a game without a companion, Eden Duo puts up no
 * second-screen window (as with Off) and instead opens an app the user picked on the screen the
 * companion would have used -- the second display, or the main one with Swap Screens on.
 *
 *  - The app is a launcher activity, stored as a flattened ComponentName: one global choice
 *    ("companion_app") and an optional per-game one ("companion_app@<title id>", the
 *    SecondScreenPerGame way) that falls back to the global one. An empty per-game value means
 *    "no app for this game". With no app chosen, App works exactly like Off.
 *  - It is started once per game session (EmulationFragment, after the game booted): never again
 *    for display events, Presentation retries or a ROM swap, until the game activity finishes.
 *  - Starting an app on a display moves input focus there (per-display focus, as on the AYN
 *    Thor), so the game's task is brought back to the front right after (REORDER_TASKS) -- but
 *    only while the game is still started: never over the user's Home / Recents.
 *  - Any failure (no second display, app gone, launch refused) falls back to Icon for that boot.
 *  - Where the app really opened cannot be read back without hidden APIs (another app's task and
 *    its display are not visible to us). The one public signal is the game itself: an app placed
 *    on the second display leaves the game started (visible), while one Android put on the
 *    game's own display covers it, and the game stops. That is checked when the focus timer fires
 *    and logged as a warning (the user can switch No Companion to Icon or Off); nothing is
 *    retried, as the stop may also be the user leaving the game.
 */
object CompanionApp {
    const val KEY = "companion_app"

    /** Return focus at the latest this long after the launch... */
    private const val FOCUS_RETURN_MS = 400L

    /** ...and once more if the app still takes it within this window (a cold start). */
    private const val FOCUS_LATE_WINDOW_MS = 7000L

    /** A lost top-resumed state is acted on after this delay, if the game is still started. */
    private const val FOCUS_LOST_DELAY_MS = 150L

    private val prefs get() = SecondScreenPerGame.prefs(YuzuApplication.appContext)

    // ---- stored choice ----

    fun globalValue(): String = prefs.getString(KEY, "") ?: ""

    /** [programId]'s own value ("" = none for this game), or null when it uses the global one. */
    fun perGameValue(programId: String?): String? {
        val key = SecondScreenPerGame.key(KEY, programId) ?: return null
        val prefs = prefs
        return if (prefs.contains(key)) prefs.getString(key, "") ?: "" else null
    }

    /**
     * Forgets the chosen app: [programId]'s own (that game then uses the global one), or the
     * global one when null. Called when No Companion leaves App, so coming back to App asks for
     * an app again instead of reopening the old one.
     */
    fun forgetChoice(programId: String?) {
        val key = if (programId != null) SecondScreenPerGame.key(KEY, programId) else KEY
        key?.let { prefs.edit { remove(it) } }
    }

    /** The global No Companion was set to [value] (the per-game one calls [forgetChoice] itself). */
    fun onGlobalNoCompanionSet(setting: AbstractSetting, value: Int) {
        if (setting === IntSetting.NO_COMPANION && value != NoCompanion.APP) forgetChoice(null)
    }

    /** The app in effect for [programId] (null: global), flattened, or null for none. */
    fun flattened(programId: String?): String? =
        CompanionAppRules.effective(perGameValue(programId), globalValue())

    // ---- launch, once per game session ----

    enum class Result {
        /** Not App, or App with no app chosen: Off behaviour. */
        NOT_USED,
        LAUNCHED,
        /** This session already started the app (ROM swap, a second program, a retry). */
        ALREADY_LAUNCHED,
        /** Could not start it: the caller shows the Icon page for this boot. */
        FAILED
    }

    /** The app this game session started (flattened), until the game activity finishes. */
    @Volatile
    private var sessionLaunched: String? = null

    /**
     * Starts the chosen app for the game in [game] (its per-game values: [programId], null for
     * global). Call on the main thread once the game booted with no second-screen window.
     */
    fun launchForBoot(game: Activity, programId: String?): Result {
        if (NoCompanion.value(programId) != NoCompanion.APP) return Result.NOT_USED
        val flat = flattened(programId)
        if (flat == null) {
            Log.info("[CompanionApp] No Companion is App but no app is chosen: Off")
            return Result.NOT_USED
        }
        if (sessionLaunched == flat) {
            Log.info("[CompanionApp] $flat already started for this game session")
            return Result.ALREADY_LAUNCHED
        }
        val component = ComponentName.unflattenFromString(flat)
        if (component == null) {
            Log.warning("[CompanionApp] stored app \"$flat\" is not a component; Icon instead")
            return Result.FAILED
        }
        if (component.packageName == game.packageName) {
            // Eden Duo itself (a stale or hand-edited value): it would start over the game.
            Log.warning("[CompanionApp] stored app \"$flat\" is Eden Duo itself; Icon instead")
            return Result.FAILED
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            Log.warning("[CompanionApp] launch on a chosen display needs Android 8; Icon instead")
            return Result.FAILED
        }
        val displayId = targetDisplay(game)
        if (displayId == null) {
            Log.warning("[CompanionApp] no second display for $flat; Icon instead")
            return Result.FAILED
        }
        val intent = Intent(Intent.ACTION_MAIN)
            .addCategory(Intent.CATEGORY_LAUNCHER)
            .setComponent(component)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) // never MULTIPLE_TASK: reuse a running app
        if (intent.resolveActivityInfo(game.packageManager, 0) == null) {
            Log.warning("[CompanionApp] $flat is not installed; Icon instead")
            return Result.FAILED
        }
        val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)
        try {
            game.startActivity(intent, options.toBundle())
        } catch (e: Exception) {
            // SecurityException (a private display), ActivityNotFoundException, ...
            Log.warning("[CompanionApp] launch of $flat on display $displayId refused: $e")
            return Result.FAILED
        }
        sessionLaunched = flat
        Log.info("[CompanionApp] started $flat on display $displayId")
        armFocusReturn(game, flat, displayId)
        return Result.LAUNCHED
    }

    /** The game activity finished: the next game start opens the app again. */
    fun endSession() {
        sessionLaunched = null
        cancelFocusReturn()
    }

    /**
     * The display the companion would use: the default one when the game sits on a secondary
     * display (Swap Screens), else the best real second panel, as AuxPresentation picks it.
     */
    private fun targetDisplay(game: Activity): Int? {
        val own = AuxCompanionActivity.activityDisplayId(game)
        if (own != Display.DEFAULT_DISPLAY) {
            return if (AuxCompanionActivity.isWantedFor(game)) Display.DEFAULT_DISPLAY else null
        }
        val display = AuxPresentation.auxDisplayCandidates(game, own)
            .firstOrNull { !AuxPresentation.isSimulatedDisplay(it) } ?: return null
        if ((display.flags and Display.FLAG_PRIVATE) != 0) {
            Log.warning("[CompanionApp] display ${display.displayId} is private")
            return null
        }
        return display.displayId
    }

    // ---- input focus back to the game ----

    private class FocusReturn(
        val taskId: Int,
        val since: Long,
        val game: WeakReference<Activity>,
        val app: String,
        val displayId: Int
    ) {
        var byTimer = false
    }

    private val handler = Handler(Looper.getMainLooper())
    private var focusReturn: FocusReturn? = null
    private var focusContext: Context? = null
    private val focusTimer = Runnable { onFocusTimer() }
    private val focusExpire = Runnable { focusReturn = null }

    private fun armFocusReturn(game: Activity, app: String, displayId: Int) {
        cancelFocusReturn()
        focusReturn = FocusReturn(
            game.taskId,
            SystemClock.uptimeMillis(),
            WeakReference(game),
            app,
            displayId
        )
        focusContext = game.applicationContext
        handler.postDelayed(focusTimer, FOCUS_RETURN_MS)
        handler.postDelayed(focusExpire, FOCUS_LATE_WINDOW_MS)
    }

    private fun cancelFocusReturn() {
        handler.removeCallbacks(focusTimer)
        handler.removeCallbacks(focusExpire)
        focusReturn = null
    }

    /**
     * True while [game] is alive and at least STARTED: bringing its task to the front is a focus
     * fix then, not a jump back over the user's Home / Recents / another app.
     */
    private fun gameStarted(game: Activity?): Boolean {
        if (game == null || game.isFinishing || game.isDestroyed) return false
        val owner = game as? LifecycleOwner ?: return true
        return owner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
    }

    private fun onFocusTimer() {
        val pending = focusReturn ?: return
        pending.byTimer = true
        val game = pending.game.get()
        if (!gameStarted(game)) {
            // Landing check (see the class comment): an app on the second display leaves the game
            // visible; a stop this soon after the launch most likely means Android opened the app
            // over the game, on its display, not on the chosen one.
            if (game != null && !game.isFinishing && !game.isDestroyed) {
                Log.warning(
                    "[CompanionApp] the game stopped right after ${pending.app} started: it " +
                        "probably opened on the game's display, not display " +
                        "${pending.displayId}. Set No Companion to Icon or Off if this repeats."
                )
            }
            cancelFocusReturn()
            return
        }
        moveToFront(pending.taskId, "timer")
        // Still armed until focusExpire: a cold-starting app may take focus only now.
    }

    /** From EmulationActivity.onTopResumedActivityChanged. */
    fun onGameTopResumedChanged(game: Activity, isTop: Boolean) {
        if (isTop) return
        val pending = focusReturn ?: return
        if (game.taskId != pending.taskId) return
        if (SystemClock.uptimeMillis() - pending.since > FOCUS_LATE_WINDOW_MS) {
            cancelFocusReturn()
            return
        }
        // The launched app took the focus: once is enough. Wait a moment first: if this was the
        // user pressing Home or Recents, the game stops meanwhile and is left alone.
        cancelFocusReturn()
        handler.postDelayed({
            if (gameStarted(pending.game.get())) {
                moveToFront(pending.taskId, if (pending.byTimer) "late" else "lost")
            } else {
                Log.info("[CompanionApp] game no longer started: focus left where the user put it")
            }
        }, FOCUS_LOST_DELAY_MS)
    }

    private fun moveToFront(taskId: Int, why: String) {
        val context = focusContext ?: YuzuApplication.appContext
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return
        try {
            am.moveTaskToFront(taskId, 0)
            Log.info("[CompanionApp] game task $taskId back in front ($why)")
        } catch (e: Exception) {
            Log.warning("[CompanionApp] cannot bring the game back in front: $e")
            try {
                am.appTasks.firstOrNull { it.taskInfo.taskId == taskId }?.moveToFront()
            } catch (_: Exception) {
            }
        }
    }

    // ---- picker ----

    /** The picker's app list, queried once per settings screen (clearPickerCache). */
    @Volatile
    private var pickerCache: List<CompanionAppRules.Entry>? = null

    /** The settings screen closed: the next one lists the apps again (one may be new). */
    fun clearPickerCache() {
        pickerCache = null
        iconCache.clear()
    }

    /** Launcher icons for the picker dialog, by component; cleared with the app list. */
    private val iconCache = HashMap<String, Drawable?>()

    /** [component]'s launcher icon for the picker, or null when it is not a listed app. */
    fun pickerIcon(context: Context, component: String): Drawable? {
        if (launcherApps(context).none { it.component == component }) return null
        return iconCache.getOrPut(component) {
            try {
                ComponentName.unflattenFromString(component)
                    ?.let { context.packageManager.getActivityIcon(it) }
            } catch (_: Exception) {
                null
            }
        }
    }

    /**
     * Launcher apps other than Eden Duo, sorted by label (labels made unique). Cached until
     * [clearPickerCache]: the settings list is rebuilt on every change of a row that shows or
     * hides the picker, and the package query is not cheap.
     */
    fun launcherApps(context: Context): List<CompanionAppRules.Entry> =
        pickerCache ?: queryLauncherApps(context).also { pickerCache = it }

    private fun queryLauncherApps(context: Context): List<CompanionAppRules.Entry> {
        val pm = context.packageManager
        val query = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val found = try {
            pm.queryIntentActivities(query, PackageManager.MATCH_ALL)
        } catch (e: Exception) {
            Log.warning("[CompanionApp] cannot list launcher apps: $e")
            emptyList()
        }
        val entries = found.mapNotNull { info ->
            val activity = info.activityInfo ?: return@mapNotNull null
            val label = try {
                info.loadLabel(pm).toString()
            } catch (_: Exception) {
                activity.packageName
            }
            CompanionAppRules.Entry(
                label,
                ComponentName(activity.packageName, activity.name).flattenToString(),
                activity.packageName
            )
        }
        val collator = Collator.getInstance()
        return CompanionAppRules.forPicker(entries, context.packageName) { a, b ->
            collator.compare(a, b)
        }
    }

    /**
     * The "App" picker for the Second Screen settings: global when [programId] is null, else that
     * game's (clearing it falls back to global).
     */
    fun pickerItem(context: Context, programId: String?): StringSingleChoiceSetting {
        val setting = if (programId != null) perGameSetting(programId) else globalSetting
        val apps = launcherApps(context)
        val labels = ArrayList<String>()
        val values = ArrayList<String>()
        apps.forEach {
            labels += it.label
            values += it.component
        }
        val current = setting.getString()
        if (current.isNotEmpty() && current !in values) {
            val pkg = ComponentName.unflattenFromString(current)?.packageName ?: current
            labels += context.getString(R.string.companion_app_missing, pkg)
            values += current
        }
        val description = if (flattened(programId) == null) {
            R.string.companion_app_description_none
        } else {
            R.string.companion_app_description
        }
        return StringSingleChoiceSetting(
            setting,
            titleId = R.string.companion_app,
            descriptionId = description,
            choices = labels.toTypedArray(),
            values = values.toTypedArray()
        )
    }

    /** The label a settings row shows for [setting]'s value, or null when it is not the picker. */
    fun rowLabel(item: StringSingleChoiceSetting): String? {
        if (item.setting.key != KEY) return null
        val index = item.selectedValueIndex
        return if (index >= 0) {
            item.choices[index]
        } else {
            YuzuApplication.appContext.getString(R.string.companion_app_choose)
        }
    }

    private val globalSetting: AbstractStringSetting = object : AbstractStringSetting {
        override fun getString(needsGlobal: Boolean): String = globalValue()
        override fun setString(value: String) = prefs.edit { putString(KEY, value) }
        override val key: String = KEY
        override val defaultValue: Any = ""
        override val isRuntimeModifiable: Boolean = true // EmulationFragment.reapplyNoCompanion
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override fun getValueAsString(needsGlobal: Boolean): String = getString(needsGlobal)
        override fun reset() = prefs.edit { remove(KEY) }
    }

    private fun perGameSetting(programId: String): AbstractStringSetting =
        object : AbstractStringSetting {
            private val gameKey = SecondScreenPerGame.key(KEY, programId)!!

            override fun getString(needsGlobal: Boolean): String =
                if (needsGlobal) globalValue() else perGameValue(programId) ?: globalValue()

            override fun setString(value: String) = prefs.edit { putString(gameKey, value) }

            override val key: String = KEY
            override val defaultValue: Any = ""
            override val isRuntimeModifiable: Boolean = true // EmulationFragment.reapplyNoCompanion
            override val pairedSettingKey: String = ""
            override val isSwitchable: Boolean = true
            override var global: Boolean
                get() = !prefs.contains(gameKey)
                set(value) {
                    if (value) {
                        prefs.edit { remove(gameKey) }
                    } else if (global) {
                        setString(globalValue())
                    }
                }
            override val isSaveable: Boolean = true
            override fun getValueAsString(needsGlobal: Boolean): String = getString(needsGlobal)
            override fun reset() = prefs.edit { remove(gameKey) }
        }

    /** True for the settings rows whose change shows or hides the picker. */
    fun affectsPicker(setting: AbstractSetting): Boolean =
        setting.key == KEY || setting.key == NoCompanion.KEY
}

/** The Android-free rules of CompanionApp (unit tested). */
object CompanionAppRules {
    class Entry(val label: String, val component: String, val packageName: String)

    /** Per-game value when set ("" = none), else the global one; null for no app. */
    fun effective(perGame: String?, global: String?): String? =
        (perGame ?: global)?.takeIf { it.isNotBlank() }

    /**
     * Picker entries: [ownPackage] left out, one entry per component, sorted by label with
     * [compare]; a label shared by several apps gets its package name added.
     */
    fun forPicker(
        entries: List<Entry>,
        ownPackage: String,
        compare: (String, String) -> Int
    ): List<Entry> {
        val unique = entries.filter { it.packageName != ownPackage }.distinctBy { it.component }
        val shared = unique.groupBy { it.label }.filterValues { it.size > 1 }.keys
        return unique
            .map {
                if (it.label in shared) {
                    Entry("${it.label} (${it.packageName})", it.component, it.packageName)
                } else {
                    it
                }
            }
            .sortedWith { a, b ->
                compare(a.label, b.label).takeIf { it != 0 } ?: a.component.compareTo(b.component)
            }
    }
}
