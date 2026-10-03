// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import androidx.core.content.edit
import java.io.File
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.AbstractIntSetting
import org.yuzu.yuzu_emu.features.settings.model.IntSetting

/**
 * Eden Duo "No Companion" (Settings -> Graphics -> Second Screen): what the second screen shows
 * for a game without a usable companion package. Icon and Black are drawn by the runtime's idle
 * page, which is built when a game boots: a change takes effect on the next game start. With
 * Off, Eden Duo puts up no second-screen window at all, leaving that screen to other apps.
 *
 * Whether a game has a companion is decided before its window is shown, from the add-on folders
 * the runtime scans (the same rules as ModRuntime::Discover's first pass: an enabled folder with
 * dualscreen/manifest.json). A package the runtime then rejects still boots on the idle page,
 * which the game screen checks once after boot (NativeLibrary.isCompanionIdle).
 *
 * The global value is the native setting; a game can override it (SecondScreenPerGame). The
 * per-game value is read here for Off, and handed to the runtime for Icon/Black right before
 * the game boots (NativeLibrary.setNoCompanionOverride).
 *
 * App (3) is Off for the window and the runtime, plus an app the user picked opened on that
 * screen once the game booted (CompanionApp); with no app picked it is just Off.
 */
object NoCompanion {
    const val KEY = "no_companion"
    private const val OFF = 2
    const val APP = 3

    private val prefs get() = SecondScreenPerGame.prefs(YuzuApplication.appContext)

    /** [programId]'s own value, or null when it uses the global one (or is unknown). */
    fun perGameValue(programId: String?): Int? {
        val key = SecondScreenPerGame.key(KEY, programId) ?: return null
        val prefs = prefs
        return if (prefs.contains(key)) prefs.getInt(key, 0) else null
    }

    /** The value in effect for [programId] (null: global). */
    fun value(programId: String?): Int =
        perGameValue(programId) ?: IntSetting.NO_COMPANION.getInt()

    /** Off or App: no second-screen window for a game without a companion. */
    fun hidesWindow(programId: String?): Boolean = value(programId).let { it == OFF || it == APP }

    /** The per-game choice for [programId] in that game's settings; see ScreenSwap's. */
    fun perGameSetting(programId: String): AbstractIntSetting =
        object : AbstractIntSetting {
            private val gameKey = SecondScreenPerGame.key(KEY, programId)!!

            override fun getInt(needsGlobal: Boolean): Int =
                if (needsGlobal) IntSetting.NO_COMPANION.getInt() else value(programId)

            override fun setInt(value: Int) {
                prefs.edit { putInt(gameKey, value) }
                if (value != APP) CompanionApp.forgetChoice(programId)
            }

            override val key: String = KEY
            override val isRuntimeModifiable: Boolean = true // EmulationFragment.reapplyNoCompanion
            override val pairedSettingKey: String = ""
            override val isSwitchable: Boolean = true
            override var global: Boolean
                get() = !prefs.contains(gameKey)
                set(value) {
                    if (value) {
                        reset()
                    } else if (global) {
                        setInt(IntSetting.NO_COMPANION.getInt())
                    }
                }
            override val isSaveable: Boolean = true
            override val defaultValue: Int = 0

            override fun getValueAsString(needsGlobal: Boolean): String =
                getInt(needsGlobal).toString()

            override fun reset() {
                prefs.edit { remove(gameKey) }
                CompanionApp.forgetChoice(programId)
            }
        }

    /**
     * True when the add-on folder for [programId] holds an enabled companion package, false
     * when it holds none, null when the folder cannot be read (the runtime may still see one).
     */
    fun hasPackage(programId: String): Boolean? {
        val path = try {
            NativeLibrary.getModLoadDirectory(programId)
        } catch (e: Exception) {
            return null
        }
        if (path.isEmpty()) return false // no add-on root for this title: nothing to discover
        val root = File(path)
        val folders = root.listFiles() ?: return if (root.exists()) null else false
        val disabled = NativeConfig.getDisabledAddons(programId).toSet()
        return folders.any {
            it.isDirectory && it.name !in disabled && File(it, "dualscreen/manifest.json").isFile
        }
    }

    /**
     * False only when No Companion is Off (or App) and [programId] certainly has no companion.
     * [perGameId] is the title whose per-game value applies (null: global).
     */
    fun wantsWindow(programId: String, perGameId: String?): Boolean {
        if (!hidesWindow(perGameId)) return true
        val found = hasPackage(programId)
        Log.info("[NoCompanion] title $programId: companion package ${found ?: "unknown"}")
        return found != false
    }
}
