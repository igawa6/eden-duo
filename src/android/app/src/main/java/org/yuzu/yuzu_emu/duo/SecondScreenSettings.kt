// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.view.HeaderSetting
import org.yuzu.yuzu_emu.features.settings.model.view.IntSingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.SingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SwitchSetting
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.CompanionApp
import org.yuzu.yuzu_emu.utils.NativeConfig
import org.yuzu.yuzu_emu.utils.NoCompanion
import org.yuzu.yuzu_emu.utils.ScreenSwap
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame

/** Eden Duo: the Graphics -> Dual screen settings section. */
object SecondScreenSettings {
    /**
     * The Graphics page's Dual screen options (SettingsFragmentPresenter). [game] is the game
     * whose per-game settings are shown, null for global.
     *
     * Mode (this file's [DualScreenMode]) and Swap Screens/No Companion keep their value in Eden
     * Duo's own store (read before the game window opens, see [SecondScreenPerGame]), so they
     * need a real title id to have a per-game value at all. Layout and Companion scaling are
     * native per-game settings like most others in this app -- always added, since the per-game
     * settings screen already offers a "use global" control for those generically; they are read
     * continuously by the renderer/layout engine, never before the window opens, so they do not
     * need Eden Duo's own store. 1.4.0: Custom's own freeform rects are a third kind -- neither
     * native nor read before the window opens -- see [DualScreenCustomLayoutStore].
     */
    fun addTo(sl: ArrayList<SettingsItem>, game: Game?, context: Context) {
        val gameId = game?.let { SecondScreenPerGame.usableProgramId(it) }
        val perGame = game != null
        if (!NativeConfig.isPerGameConfigLoaded() && !NativeLibrary.isRunning()) {
            IntSetting.COMPANION_RATIO.global = true
            IntSetting.COMPANION_LAYOUT.global = true
        }
        val hasSecondScreen = SecondScreenDetection.hasSecondScreen(context)
        val effectiveMode = DualScreenModeResolver.resolveActive(
            DualScreenModeSetting.requested(gameId),
            hasSecondScreen,
            companionAvailable = true
        )
        sl.apply {
            add(HeaderSetting(R.string.second_screen))
            if (!perGame || gameId != null) {
                add(modeSetting(context, gameId, hasSecondScreen))
            }
            if (effectiveMode == DualScreenMode.SIMULATED) {
                add(HeaderSetting(R.string.dual_screen_mode_simulated))
                // 1.4.0: the Divider position slider is gone (superseded by the in-game
                // freeform layout editor) -- Stacked/Side by side now use a fixed split
                // (see DualScreenFragmentHost.updateLayout); IntSetting.DIVIDER_POSITION itself
                // is untouched so an old .ini that still has the key keeps loading harmlessly,
                // it is just never read here any more. Custom shows where to edit it instead of
                // a second, settings-page preview editor.
                add(
                    SingleChoiceSetting(
                        IntSetting.COMPANION_LAYOUT,
                        titleId = R.string.dual_screen_layout,
                        descriptionString = if (IntSetting.COMPANION_LAYOUT.getInt() ==
                            DualScreenLayoutChoice.CUSTOM.ordinal
                        ) {
                            context.getString(R.string.dual_screen_layout_edit_in_game)
                        } else {
                            context.getString(R.string.dual_screen_layout_description)
                        },
                        choicesId = R.array.dualScreenLayoutEntries,
                        valuesId = R.array.dualScreenLayoutValues
                    )
                )
            }
            if (effectiveMode == DualScreenMode.PHYSICAL) {
                add(HeaderSetting(R.string.dual_screen_mode_physical))
                if (!perGame || gameId != null) {
                    add(
                        SwitchSetting(
                            if (gameId != null) {
                                ScreenSwap.perGameSetting(gameId)
                            } else {
                                ScreenSwap.setting
                            },
                            titleId = R.string.swap_screens,
                            descriptionId = R.string.swap_screens_description
                        )
                    )
                }
                add(
                    SingleChoiceSetting(
                        IntSetting.COMPANION_RATIO,
                        titleId = R.string.companion_ratio,
                        descriptionId = R.string.companion_ratio_description,
                        choicesId = R.array.companionRatioEntries,
                        valuesId = R.array.companionRatioValues
                    )
                )
                if (!perGame) {
                    add(
                        SingleChoiceSetting(
                            SecondScreenBrightness.setting,
                            titleId = R.string.second_screen_brightness,
                            descriptionId = R.string.second_screen_brightness_description,
                            choicesId = R.array.secondScreenBrightnessEntries,
                            valuesId = R.array.secondScreenBrightnessValues
                        )
                    )
                }
            }
            if (!perGame || gameId != null) {
                add(
                    SingleChoiceSetting(
                        if (gameId != null) {
                            NoCompanion.perGameSetting(gameId)
                        } else {
                            IntSetting.NO_COMPANION
                        },
                        titleId = R.string.no_companion,
                        descriptionId = R.string.no_companion_description,
                        choicesId = R.array.noCompanionEntries,
                        valuesId = R.array.noCompanionValues
                    )
                )
                // No Companion "App": which app (CompanionApp), shown while App is chosen.
                if (NoCompanion.value(gameId) == NoCompanion.APP) {
                    add(CompanionApp.pickerItem(context, gameId))
                }
            }
            if (!perGame) {
                add(
                    SingleChoiceSetting(
                        CompanionToggle.gestureSetting,
                        titleId = R.string.companion_toggle_gesture,
                        descriptionId = R.string.companion_toggle_gesture_description,
                        choicesId = R.array.companionToggleGestureEntries,
                        valuesId = R.array.companionToggleGestureValues
                    )
                )
            }
        }
    }

    /**
     * The Mode picker: Automatic (unset, -1) / Physical / Simulated / Off. Physical is left out
     * of the choice list entirely (rather than shown disabled -- IntSingleChoiceSetting has no
     * per-item disabled state, and excluding it keeps the dialog simple) whenever
     * [hasSecondScreen] is false; its own note is appended to the description instead, so the
     * option is still discoverable. A Physical choice already saved stays listed then, labelled
     * as unavailable, so the row's summary still names it instead of going blank.
     */
    private fun modeSetting(
        context: Context,
        gameId: String?,
        hasSecondScreen: Boolean
    ): IntSingleChoiceSetting {
        val choices = ArrayList<String>(4)
        val values = ArrayList<Int>(4)
        choices.add(context.getString(R.string.dual_screen_mode_automatic))
        values.add(-1)
        val current =
            if (gameId != null) DualScreenModeSetting.requested(gameId) else DualScreenModeSetting.global()
        if (hasSecondScreen) {
            choices.add(context.getString(R.string.dual_screen_mode_physical))
            values.add(DualScreenMode.PHYSICAL.value)
        } else if (current == DualScreenMode.PHYSICAL) {
            choices.add(context.getString(R.string.dual_screen_mode_physical_unavailable))
            values.add(DualScreenMode.PHYSICAL.value)
        }
        choices.add(context.getString(R.string.dual_screen_mode_simulated))
        values.add(DualScreenMode.SIMULATED.value)
        choices.add(context.getString(R.string.dual_screen_mode_off))
        values.add(DualScreenMode.OFF.value)
        val description = if (hasSecondScreen) {
            context.getString(R.string.dual_screen_mode_description)
        } else {
            context.getString(R.string.dual_screen_mode_description) + " " +
                context.getString(R.string.dual_screen_mode_no_second_screen)
        }
        return IntSingleChoiceSetting(
            if (gameId != null) DualScreenModeSetting.perGameSetting(gameId) else DualScreenModeSetting.setting,
            titleId = R.string.dual_screen_mode,
            descriptionString = description,
            choices = choices.toTypedArray(),
            values = values.toTypedArray()
        )
    }
}
