// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import android.content.res.Configuration
import android.provider.Settings
import org.yuzu.yuzu_emu.utils.Log

/**
 * Eden Duo Stage 1: Simulated ("inline", single-surface) dual screen. The companion is drawn as
 * a second rectangle of the game's own window by the native renderer (AuxRouting::SetInlineLayout
 * / SetInlineLayoutSideSplit, see aux_routing.h) instead of a real second screen/Activity.
 *
 * This is a hidden developer switch, not a user setting -- there is no UI for it anywhere in the
 * app. Turn it on with:
 *   adb shell settings put global eden_duo_inline_dual_screen 1
 * and off again with:
 *   adb shell settings delete global eden_duo_inline_dual_screen
 *
 * [DualScreenEmulationController] checks [isEnabled] before opening a Presentation or
 * AuxCompanionActivity and skips it while this is on; the layout is a fixed split for this
 * prototype (landscape: game left ~60%, companion right ~40%; portrait: game top, companion
 * below), applied once per boot via [NativeLibrary.setInlineDualScreen]. No companion touch
 * routing yet.
 */
object InlineDualScreen {
    private const val KEY = "eden_duo_inline_dual_screen"

    fun isEnabled(context: Context): Boolean = try {
        Settings.Global.getInt(context.contentResolver, KEY, 0) != 0
    } catch (e: Exception) {
        Log.error("[InlineDualScreen] could not read $KEY: ${e.message}")
        false
    }

    fun isLandscape(context: Context): Boolean =
        context.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
}
