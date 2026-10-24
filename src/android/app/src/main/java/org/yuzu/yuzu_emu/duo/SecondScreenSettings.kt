// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.view.HeaderSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.SingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SwitchSetting
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.CompanionApp
import org.yuzu.yuzu_emu.utils.NativeConfig
import org.yuzu.yuzu_emu.utils.NoCompanion
import org.yuzu.yuzu_emu.utils.ScreenSwap
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame

/** Eden Duo: the Second Screen settings section. */
object SecondScreenSettings {
    /**
     * The Graphics page's Second Screen options (SettingsFragmentPresenter). [game] is the game
     * whose per-game settings are shown, null for global. Per game, Companion Ratio is a native
     * per-game setting; Swap Screens and No Companion keep the game's value in Eden Duo's own
     * store (read before the game window opens), so they need a real title id.
     */
    fun addTo(sl: ArrayList<SettingsItem>, game: Game?, context: Context) {
        val gameId = game?.let { SecondScreenPerGame.usableProgramId(it) }
        val perGame = game != null
        if (!NativeConfig.isPerGameConfigLoaded() && !NativeLibrary.isRunning()) {
            IntSetting.COMPANION_RATIO.global = true
        }
        sl.apply {
            add(HeaderSetting(R.string.second_screen))
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
}
