// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import android.provider.Settings
import android.view.Display
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.views.AuxPresentation

/**
 * Eden Duo Stage 2: whether a real second-screen display exists, as a context-only question
 * usable from Settings (no running game, no Activity) as well as from the running game itself.
 * [DualScreenEmulationController] keeps its own richer, session-aware
 * `hasSecondScreen(Activity)` (an aux window already up counts too, even mid-eject-retry) and
 * delegates its last-resort check here.
 */
object SecondScreenDetection {
    private const val COUNT_SIMULATED_KEY = "eden_duo_count_simulated_displays"

    /**
     * A candidate real second-screen display exists right now (the window may be down).
     * [ownDisplayId] is the display this check should not itself count -- the default display
     * for a Settings-only caller with no running game, or the game's own current display
     * (Swap Screens) when called from the game.
     */
    fun hasSecondScreen(context: Context, ownDisplayId: Int = Display.DEFAULT_DISPLAY): Boolean =
        AuxPresentation.auxDisplayCandidates(context, ownDisplayId).isNotEmpty()

    /**
     * Developer setting, default off: count the developer-options overlay display (or another
     * virtual/"Simulated" display, e.g. Cuttlefish) as a second screen too. Off by default so a
     * stray virtual display never doubles as an unintended companion target (RESEARCH.md E6);
     * on, so Cuttlefish and the overlay-display developer workflow keep working regardless of
     * whether that display happens to carry FLAG_PRESENTATION. No UI for it yet (see
     * [InlineDualScreen] for the sibling hidden switch and its adb recipe) -- Settings UI for
     * dual screen is already a large surface this stage; set with:
     *   adb shell settings put global eden_duo_count_simulated_displays 1
     */
    fun countsSimulatedDisplays(context: Context): Boolean = try {
        Settings.Global.getInt(context.contentResolver, COUNT_SIMULATED_KEY, 0) != 0
    } catch (e: Exception) {
        Log.error("[SecondScreenDetection] could not read $COUNT_SIMULATED_KEY: ${e.message}")
        false
    }

    /**
     * The Android-free part of [AuxPresentation.auxDisplayCandidates]' hardening (unit tested):
     * does a display with these [flags], matching the "simulated" name heuristic or not
     * ([isSimulatedName]), belong in the candidate list, given [countSimulated]
     * ([countsSimulatedDisplays])? A simulated-named display is exempt from the flag check (as
     * before) but only counts at all when the developer opted in; any other display must carry
     * FLAG_PRESENTATION and not FLAG_PRIVATE (RESEARCH.md E6).
     */
    fun isAcceptableCandidate(flags: Int, isSimulatedName: Boolean, countSimulated: Boolean): Boolean {
        if (isSimulatedName) {
            return countSimulated
        }
        val presentation = (flags and Display.FLAG_PRESENTATION) != 0
        val private = (flags and Display.FLAG_PRIVATE) != 0
        return presentation && !private
    }
}
