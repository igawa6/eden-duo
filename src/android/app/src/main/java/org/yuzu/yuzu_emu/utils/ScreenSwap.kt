// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.Activity
import android.app.ActivityManager
import android.app.ActivityOptions
import android.content.Context
import android.content.Intent
import android.os.Build
import android.provider.DocumentsContract
import android.view.Display
import androidx.core.content.edit
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.activities.AuxCompanionActivity
import org.yuzu.yuzu_emu.features.settings.model.AbstractBooleanSetting
import org.yuzu.yuzu_emu.views.AuxPresentation

/**
 * Eden Duo "Swap screens": the game opens on the second display and the companion on the main
 * one. A window cannot move between displays once it is up, so the choice is made when a game
 * starts -- EmulationActivity reopens a fresh launch on the second display before doing anything
 * else -- and the companion then follows wherever the game actually landed
 * (AuxCompanionActivity), so a refused swap simply leaves today's layout. A game can override
 * the global value (SecondScreenPerGame); the override is read at the same moment.
 */
object ScreenSwap {
    private const val KEY_SWAP = "swap_screens"
    private const val EXTRA_RELAUNCHED = "org.yuzu.yuzu_emu.SCREEN_SWAP_RELAUNCHED"

    private fun prefs(context: Context) = SecondScreenPerGame.prefs(context)

    fun isEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_SWAP, false)

    /** The value for [programId]: its per-game value when it has one, else the global one. */
    fun isEnabled(context: Context, programId: String?): Boolean {
        val key = SecondScreenPerGame.key(KEY_SWAP, programId) ?: return isEnabled(context)
        val prefs = prefs(context)
        return if (prefs.contains(key)) prefs.getBoolean(key, false) else isEnabled(context)
    }

    fun setEnabled(context: Context, value: Boolean) =
        prefs(context).edit { putBoolean(KEY_SWAP, value) }

    /**
     * The per-game switch for [programId] in that game's settings: no stored value means "use
     * global" (the clear button removes it). Read when the game starts, so not editable mid-game.
     */
    fun perGameSetting(programId: String): AbstractBooleanSetting =
        object : AbstractBooleanSetting {
            private val context get() = YuzuApplication.appContext
            private val gameKey = SecondScreenPerGame.key(KEY_SWAP, programId)!!

            override fun getBoolean(needsGlobal: Boolean): Boolean =
                if (needsGlobal) isEnabled(context) else isEnabled(context, programId)

            override fun setBoolean(value: Boolean) =
                prefs(context).edit { putBoolean(gameKey, value) }

            override val key: String = KEY_SWAP
            override val isRuntimeModifiable: Boolean = false
            override val pairedSettingKey: String = ""
            override val isSwitchable: Boolean = true
            override var global: Boolean
                get() = !prefs(context).contains(gameKey)
                set(value) {
                    if (value) {
                        prefs(context).edit { remove(gameKey) }
                    } else if (global) {
                        setBoolean(isEnabled(context))
                    }
                }
            override val isSaveable: Boolean = true
            override val defaultValue: Boolean = false

            override fun getValueAsString(needsGlobal: Boolean): String =
                getBoolean(needsGlobal).toString()

            override fun reset() = prefs(context).edit { remove(gameKey) }
        }

    /** App-level switch for the settings list; stored here, not in the native config. */
    val setting: AbstractBooleanSetting = object : AbstractBooleanSetting {
        override fun getBoolean(needsGlobal: Boolean): Boolean =
            isEnabled(YuzuApplication.appContext)

        override fun setBoolean(value: Boolean) = setEnabled(YuzuApplication.appContext, value)

        override val key: String = KEY_SWAP
        override val isRuntimeModifiable: Boolean = true
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override val defaultValue: Boolean = false

        override fun getValueAsString(needsGlobal: Boolean): String =
            getBoolean(needsGlobal).toString()

        override fun reset() = setBoolean(defaultValue)
    }

    /**
     * Called first thing in EmulationActivity.onCreate for a fresh launch. Returns true when the
     * launch was handed elsewhere and the caller must finish without setting anything up:
     *  - a game is already running in another task (one swapped onto the second display, while
     *    this launch came from the main screen's task): the intent goes to that game instead of
     *    a second EmulationActivity starting beside it;
     *  - the swap is on and this activity opened on the default display: the same launch is
     *    started again on the second display;
     *  - the swap is on but the game's own Swap Screens is off and this activity opened on a
     *    secondary display: the same launch is started again on the default display.
     * Returns false (launch here, as usual) whenever neither applies or is possible.
     */
    fun redirectLaunch(activity: Activity, hasEmulationSession: Boolean): Boolean {
        val intent = activity.intent ?: return false
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
        return if (hasEmulationSession) {
            forwardToRunningGame(activity, intent)
        } else {
            relaunchOnSecondDisplay(activity, intent)
        }
    }

    private fun forwardToRunningGame(activity: Activity, intent: Intent): Boolean {
        val running = NativeLibrary.sEmulationActivity.get() ?: return false
        if (running === activity || running.isFinishing || running.taskId == activity.taskId) {
            return false
        }
        // A game on the default display shares its task with the main screen, as upstream has
        // it; leave that path exactly as it is. One this class moved there (its own Swap Off)
        // has a task of its own, like a swapped game.
        if (AuxCompanionActivity.activityDisplayId(running) == Display.DEFAULT_DISPLAY &&
            running.intent?.getBooleanExtra(EXTRA_RELAUNCHED, false) != true
        ) {
            return false
        }
        val forward = intentWithLastingAccess(activity, intent) ?: return false
        val am = activity.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            ?: return false
        val task = try {
            am.appTasks.firstOrNull { it.taskInfo.taskId == running.taskId }
        } catch (e: Exception) {
            null
        } ?: return false
        return try {
            // Starts it in the game's task, where EmulationActivity is on top: onNewIntent.
            task.startActivity(activity, forwardedIntent(forward), null)
            Log.info("[ScreenSwap] launch handed to the running game in task ${running.taskId}")
            if (SecondScreenPerGame.hasOverrides(activity)) {
                Log.info(
                    "[ScreenSwap] per-game Swap Screens not applied: the running game's window " +
                        "stays on display ${AuxCompanionActivity.activityDisplayId(running)}"
                )
            }
            finishQuietly(activity)
            true
        } catch (e: Exception) {
            Log.warning("[ScreenSwap] cannot reach the running game: ${e.message}")
            false
        }
    }

    private fun relaunchOnSecondDisplay(activity: Activity, intent: Intent): Boolean {
        if (intent.getBooleanExtra(EXTRA_RELAUNCHED, false)) return false
        // Which game this is matters only when some game has its own value, or for No
        // Companion below: without overrides, exactly the global behaviour.
        val launch by lazy { SecondScreenPerGame.resolveLaunch(intent) }
        val overrides = SecondScreenPerGame.hasOverrides(activity)
        val swap = if (overrides) isEnabled(activity, launch.perGameId) else isEnabled(activity)
        if (!swap) {
            if (!isEnabled(activity)) return false
            Log.info(
                "[ScreenSwap] Swap Screens is off for title " +
                    "${SecondScreenPerGame.titleHex(launch.perGameId)} (its own setting)"
            )
            // A game with its own Swap Off goes to the main display wherever the launch came
            // from: an Eden window left on the second display (a frontend opened it there)
            // would otherwise keep it on that screen.
            if (AuxCompanionActivity.activityDisplayId(activity) == Display.DEFAULT_DISPLAY) {
                return false
            }
            return relaunchOn(activity, intent, Display.DEFAULT_DISPLAY, "main")
        }
        if (AuxCompanionActivity.activityDisplayId(activity) != Display.DEFAULT_DISPLAY) {
            Log.info("[ScreenSwap] launch already on a secondary display; left there")
            return false
        }
        // Only a game known to have no companion stays on the main display, and only when No
        // Companion would then leave that screen empty (Off, or App with no app picked). An
        // unknown title or an unreadable add-on folder swaps.
        val perGameId = if (overrides) launch.perGameId else null
        val keepMain = ScreenSwapRules.keepsMainDisplay(
            NoCompanion.value(perGameId),
            CompanionApp.flattened(perGameId) != null
        ) { launch.programId?.let { NoCompanion.hasPackage(it) } }
        if (keepMain) {
            Log.info("[ScreenSwap] game has no companion; launching on the main display")
            return false
        }
        val target = AuxPresentation.auxDisplayCandidates(activity, Display.DEFAULT_DISPLAY)
            .firstOrNull { !AuxPresentation.isSimulatedDisplay(it) }
        if (target == null) {
            Log.info("[ScreenSwap] no second display; launching on the main one")
            return false
        }
        return relaunchOn(activity, intent, target.displayId, target.name)
    }

    /** Starts the same launch again on [displayId] and finishes this one; false leaves it here. */
    private fun relaunchOn(
        activity: Activity,
        intent: Intent,
        displayId: Int,
        name: String
    ): Boolean {
        val forward = intentWithLastingAccess(activity, intent)
        if (forward == null) {
            Log.warning(
                "[ScreenSwap] the launch holds a one-time grant for ${intent.data}; " +
                    "launching on the current display"
            )
            return false
        }
        val relaunch = forwardedIntent(forward).addFlags(
            Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK
        )
        val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)
        return try {
            activity.startActivity(relaunch, options.toBundle())
            Log.info("[ScreenSwap] game launched on display $displayId ($name)")
            finishQuietly(activity)
            true
        } catch (e: Exception) {
            // Some firmwares refuse app launches on another display. The game where it is beats
            // no game at all.
            Log.warning("[ScreenSwap] launch on display $displayId refused: ${e.message}")
            false
        }
    }

    /** The same launch, marked as already placed, keeping only its URI grants. */
    private fun forwardedIntent(intent: Intent): Intent = Intent(intent).apply {
        putExtra(EXTRA_RELAUNCHED, true)
        flags = (intent.flags and GRANT_FLAGS) or Intent.FLAG_ACTIVITY_NO_ANIMATION
    }

    private fun finishQuietly(activity: Activity) {
        activity.finish()
        @Suppress("DEPRECATION")
        activity.overridePendingTransition(0, 0)
    }

    private const val GRANT_FLAGS = Intent.FLAG_GRANT_READ_URI_PERMISSION or
        Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or
        Intent.FLAG_GRANT_PREFIX_URI_PERMISSION

    /**
     * A content URI handed over with a one-time grant (a frontend's ACTION_VIEW) is readable only
     * while the activity that received it lives, and this one is about to finish. Returns the
     * launch to forward when access does not hang on that grant: no grant, our own provider, a
     * URI under a folder this app holds a persisted permission for, or a document inside such a
     * folder that the frontend addressed through a wider tree of its own (CocoonFE hands over
     * tree/primary:ROMs/document/primary:ROMs/switch/... while we hold tree/primary:ROMs/switch).
     * The latter is re-addressed through our tree. Null when only the one-time grant gives access.
     */
    private fun intentWithLastingAccess(context: Context, intent: Intent): Intent? {
        if (intent.flags and
            (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION) == 0
        ) {
            return intent
        }
        val uri = intent.data ?: return intent
        if (uri.scheme != "content") return intent
        if (uri.authority?.startsWith(context.packageName) == true) return intent
        val held = try {
            context.contentResolver.persistedUriPermissions.filter { it.isReadPermission }
        } catch (e: Exception) {
            return null
        }
        val target = uri.toString()
        if (held.any { target == it.uri.toString() || target.startsWith("${it.uri}/") }) {
            return intent
        }
        val documentId = SecondScreenPerGame.documentId(uri) ?: return null
        for (permission in held) {
            val tree = permission.uri
            if (tree.authority != uri.authority || !DocumentsContract.isTreeUri(tree)) continue
            val treeId = DocumentsContract.getTreeDocumentId(tree)
            if (documentId != treeId && !documentId.startsWith("$treeId/")) continue
            val ours = DocumentsContract.buildDocumentUriUsingTree(tree, documentId)
            Log.info("[ScreenSwap] ${intent.data} re-addressed through $tree")
            return Intent(intent).apply {
                setDataAndType(ours, intent.type)
                clipData = null
                flags = flags and GRANT_FLAGS.inv()
            }
        }
        return null
    }
}

/** The Android-free decision of ScreenSwap (unit tested). */
object ScreenSwapRules {
    const val ICON = 0
    const val BLACK = 1
    const val OFF = 2
    const val APP = 3

    /**
     * True when a game without a companion leaves its second screen showing nothing of ours
     * under No Companion [mode]: Off, or App with no app picked ([appPicked]).
     */
    fun leavesScreenEmpty(mode: Int, appPicked: Boolean): Boolean =
        mode == OFF || (mode == APP && !appPicked)

    /**
     * True when a swapped launch stays on the main display instead: only when the game
     * positively has no companion ([hasCompanion] false; null = unknown) and [mode] would leave
     * the screen empty. [hasCompanion] is asked only when that matters (it reads the disk).
     */
    fun keepsMainDisplay(mode: Int, appPicked: Boolean, hasCompanion: () -> Boolean?): Boolean =
        leavesScreenEmpty(mode, appPicked) && hasCompanion() == false
}
