// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import androidx.core.content.edit
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame

/**
 * Eden Duo Stage 2: the one-time notice shown the first time a companion appears in Simulated
 * mode (what it is, where the setting lives, that Off turns it off) -- owner decision 4 turns
 * the companion on for everyone by default, so this is information, not a choice to make.
 */
object DualScreenFirstRunNotice {
    private const val KEY = "dual_screen_simulated_notice_shown"

    private fun prefs(context: Context) = SecondScreenPerGame.prefs(context)

    fun shouldShow(context: Context): Boolean = !prefs(context).getBoolean(KEY, false)

    fun markShown(context: Context) {
        prefs(context).edit { putBoolean(KEY, true) }
    }
}

/**
 * Eden Duo Stage 4 (RESEARCH.md §0 item 2 / E4): the one-time hint shown the first time a
 * display appears mid-game while the user had explicitly chosen Simulated -- unlike
 * [DualScreenFirstRunNotice], which is about the companion showing up at all, this is purely
 * "a second screen exists now; Dual screen settings is where to use it", so it needs its own
 * shown-once flag.
 */
object DualScreenSimulatedStaysHint {
    private const val KEY = "dual_screen_simulated_stays_hint_shown"

    private fun prefs(context: Context) = SecondScreenPerGame.prefs(context)

    fun shouldShow(context: Context): Boolean = !prefs(context).getBoolean(KEY, false)

    fun markShown(context: Context) {
        prefs(context).edit { putBoolean(KEY, true) }
    }
}
