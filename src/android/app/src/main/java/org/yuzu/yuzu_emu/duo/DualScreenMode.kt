// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import androidx.core.content.edit
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.AbstractIntSetting
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame

/**
 * Eden Duo Stage 2: the dual-screen mode -- Physical (a real second screen/Presentation),
 * Simulated (the companion drawn into a pane of the game's own window, see
 * [DualScreenFragmentHost]) or Off (no companion package selected for the launch at all, see
 * `Core::Mods::SetCompanionsDisabledForLaunch`).
 */
enum class DualScreenMode(val value: Int) {
    PHYSICAL(0),
    SIMULATED(1),
    OFF(2);

    companion object {
        fun fromValue(value: Int): DualScreenMode? = entries.firstOrNull { it.value == value }
    }
}

/**
 * Eden Duo Stage 2: the saved dual-screen Mode -- global, with an optional per-title override,
 * absent meaning "inherit" (same shape as [org.yuzu.yuzu_emu.utils.NoCompanion] and
 * [org.yuzu.yuzu_emu.utils.ScreenSwap], stored in the same [SecondScreenPerGame] preferences
 * file: Mode must be known before the game window opens, same reason those two are there and not
 * in the native per-game config). `null` (no stored value, -1) at the global level means the
 * user made no explicit choice yet; [DualScreenModeResolver] then picks one. Once the user picks
 * any mode -- global or per-title -- the choice is explicit and kept.
 */
object DualScreenModeSetting {
    const val KEY = "dual_screen_mode"
    internal const val UNSET = -1

    /** Mode decides which Dual screen rows exist (Simulated/Physical groups), so a change must
     *  rebuild the settings list right away (otherwise the Layout row stays missing). */
    fun affectsSettingsList(setting: org.yuzu.yuzu_emu.features.settings.model.AbstractSetting): Boolean =
        setting.key == KEY

    private val prefs get() = SecondScreenPerGame.prefs(YuzuApplication.appContext)

    /** The saved global value, or null when the user made no explicit choice. */
    fun global(): DualScreenMode? = DualScreenMode.fromValue(prefs.getInt(KEY, UNSET))

    fun setGlobal(mode: DualScreenMode?) =
        prefs.edit { if (mode == null) remove(KEY) else putInt(KEY, mode.value) }

    /** [programId]'s own stored value: null when it has none (it uses the global one), [UNSET]
     *  when it explicitly picked Automatic, else the mode's value. */
    private fun perGameStored(programId: String?): Int? {
        val key = SecondScreenPerGame.key(KEY, programId) ?: return null
        return if (prefs.contains(key)) prefs.getInt(key, UNSET) else null
    }

    /**
     * The requested mode for [programId]: its own choice if it has one, else the global value.
     * Null means Automatic -- either picked explicitly (for this game or globally) or no
     * explicit choice at either level.
     */
    fun requested(programId: String?): DualScreenMode? = requestedFrom(perGameStored(programId), global())

    /**
     * Android-free core of [requested] (unit tested): [perGameStored] is the per-game stored value
     * (null = no per-game key, inherit [global]; [UNSET] = Automatic for this game, regardless of
     * the global choice).
     */
    internal fun requestedFrom(perGameStored: Int?, global: DualScreenMode?): DualScreenMode? =
        if (perGameStored != null) DualScreenMode.fromValue(perGameStored) else global

    /** What a per-game pick of [value] stores: the mode's own value, or [UNSET] for Automatic --
     *  kept as an explicit per-game choice, unlike "use global", which removes the key. */
    internal fun perGameStoredValue(value: Int): Int = DualScreenMode.fromValue(value)?.value ?: UNSET

    /** The per-game choice for [programId] in that game's settings; see NoCompanion's own. */
    fun perGameSetting(programId: String): AbstractIntSetting = object : AbstractIntSetting {
        private val gameKey = SecondScreenPerGame.key(KEY, programId)!!

        override fun getInt(needsGlobal: Boolean): Int =
            (if (needsGlobal) global() else requested(programId))?.value ?: UNSET

        override fun setInt(value: Int) {
            prefs.edit { putInt(gameKey, perGameStoredValue(value)) }
        }

        override val key: String = KEY
        override val isRuntimeModifiable: Boolean = false // read once, before the game boots
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = true
        override var global: Boolean
            get() = !prefs.contains(gameKey)
            set(value) {
                if (value) {
                    reset()
                } else if (global) {
                    setInt(requested(programId)?.value ?: UNSET)
                }
            }
        override val isSaveable: Boolean = true
        override val defaultValue: Int = UNSET

        override fun getValueAsString(needsGlobal: Boolean): String = getInt(needsGlobal).toString()

        override fun reset() {
            prefs.edit { remove(gameKey) }
        }
    }

    /** The global choice for the app-level setting list. */
    val setting: AbstractIntSetting = object : AbstractIntSetting {
        override fun getInt(needsGlobal: Boolean): Int = global()?.value ?: UNSET
        override fun setInt(value: Int) = setGlobal(DualScreenMode.fromValue(value))
        override val key: String = KEY
        override val isRuntimeModifiable: Boolean = true
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override val defaultValue: Int = UNSET

        override fun getValueAsString(needsGlobal: Boolean): String = getInt(needsGlobal).toString()

        override fun reset() = setGlobal(null)
    }
}

/**
 * Eden Duo Stage 2: the Android-free dual-screen mode resolver (unit tested). The requested mode
 * is saved (global + per-title, absent = inherit, see [DualScreenModeSetting]); the active mode
 * is never saved, resolved again at every launch.
 *
 * Rules:
 *  - Physical is offered only when a second screen is detected: a requested Physical with none
 *    resolves to Simulated instead (the same "no display -> fall back" shape as the physical
 *    aux path's own), never to Off.
 *  - With no explicit choice (requested null), Physical wins when a second screen is detected,
 *    else Simulated.
 *  - An explicit choice, once made, is kept -- Off always stays Off; an explicit Simulated
 *    always stays Simulated even with a second screen attached (Physical/Simulated still switch
 *    live from Settings, see DualScreenEmulationController, but the resolver itself never
 *    second-guesses an explicit choice at launch).
 *
 * [companionAvailable] takes no part in this decision today: NoCompanion already decides what a
 * companion-less game's second screen shows (Icon/Black/Off/App), the same way in Physical and
 * in Simulated, so there is no case here where knowing it would change Physical/Simulated/Off.
 * Kept as a parameter (a) because the binding spec names it as one of this function's inputs and
 * (b) so a caller is not tempted to skip computing it on the (mistaken) assumption it is unused
 * by every rule forever.
 */
object DualScreenModeResolver {
    fun resolveActive(
        requested: DualScreenMode?,
        secondScreenDetected: Boolean,
        @Suppress("UNUSED_PARAMETER") companionAvailable: Boolean
    ): DualScreenMode = when (requested) {
        DualScreenMode.OFF -> DualScreenMode.OFF
        DualScreenMode.SIMULATED -> DualScreenMode.SIMULATED
        DualScreenMode.PHYSICAL ->
            if (secondScreenDetected) DualScreenMode.PHYSICAL else DualScreenMode.SIMULATED
        null -> if (secondScreenDetected) DualScreenMode.PHYSICAL else DualScreenMode.SIMULATED
    }
}
