// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Device table (Stage 2 spec): phone 2400x1080 both orientations, Fold inner 2176x1812, Fold
 * cover 904x2316, Thor top 1920x1080, tablet 2560x1600, split-screen half 1200x1080. Densities
 * are chosen per device family, documented at each test, to make the numbers exactly
 * hand-verifiable; they do not need to match one specific real unit's reported dpi.
 *
 * Stage 4 (landscape default is side by side): a 16:9 game beside the
 * project's 8:7 companion, side by side at full height, needs 3160 px of width (1920 + 1240) for
 * every landscape window in this table except the tablet. On a phone (2400 px wide) or the
 * Thor's own top screen (1920 px wide), shrinking both to fit used to collapse the companion
 * through the old 320 dp per-pane floor; [DualScreenGeometry.MIN_PANE_SHORT_EDGE_DP] is now 200
 * dp, so both stay visible -- see [phoneLandscapeAspectLockedStaysSideBySide] and
 * [thorTopAspectLockedStaysSideBySide]. The flip-phone cover screen case still collapses, now
 * through the new [DualScreenGeometry.MIN_WINDOW_SHORT_EDGE_DP] window-level floor instead -- see
 * [foldCoverCollapsesRegardlessOfController]. Portrait stacking (full window width instead) never
 * hit this on the same phone -- see [phonePortraitStaysVisibleBothControllerStates].
 */
class DualScreenGeometryTest {
    private val sideBySide = DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC)
    private val game169 = DualScreenAspect(16, 9)

    @Test
    fun aspectForRendererSettingMapsEveryKnownValueAndFallsBackForStretch() {
        assertEquals(DualScreenAspect(16, 9), DualScreenGeometry.aspectForRendererSetting(0))
        assertEquals(DualScreenAspect(4, 3), DualScreenGeometry.aspectForRendererSetting(1))
        assertEquals(DualScreenAspect(21, 9), DualScreenGeometry.aspectForRendererSetting(2))
        assertEquals(DualScreenAspect(16, 10), DualScreenGeometry.aspectForRendererSetting(3))
        assertNull(DualScreenGeometry.aspectForRendererSetting(4)) // Stretch
        assertNull(DualScreenGeometry.aspectForRendererSetting(99)) // unrecognised: as given
    }

    // ---- phone 2400x1080, landscape: side by side, aspect-locked, stays visible (Stage 4) ----

    @Test
    fun phoneLandscapeAspectLockedStaysSideBySide() {
        // density 420: a believable ~6.3" panel at 2400x1080 (matches named phones in this
        // class, e.g. Pixel 7). Both panes are sized at their own locked aspect with no bars
        // (1920+1240 game+companion naturally needs 3160 px at full 1080 px height; scaled down
        // by 2400/3160 = 60/79 to fit): game 1458x820, companion 942x820. Window short edge
        // (1080 px = 411.4 dp) clears the 360 dp window floor; companion short edge (820 px =
        // 312.4 dp) clears the 200 dp per-pane floor (Stage 4 lowers it from 320) -- both panes
        // stay visible, matching the side-by-side-by-default rule.
        val window = DualScreenWindow(widthPx = 2400, heightPx = 1080, densityDpi = 420)
        val layout = DualScreenGeometry.compute(window, null, false, sideBySide, game169)
        assertEquals(DualScreenRect(0, 130, 1458, 820), layout.gameRect)
        assertEquals(DualScreenRect(1458, 130, 942, 820), layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, DualScreenAspect(1240, 1080))
    }

    // ---- Thor top 1920x1080, landscape: same shape, narrower, stays visible too (Stage 4) ----

    @Test
    fun thorTopAspectLockedStaysSideBySide() {
        // density 400 (approximation of the Thor's own ~6" 1920x1080 top panel). Scale =
        // 1920/3160 = 48/79: game 1167x656, companion 753x656. Window short edge (1080 px =
        // 432 dp) clears the 360 dp window floor; companion short edge (656 px = 262.4 dp)
        // clears the 200 dp per-pane floor.
        val window = DualScreenWindow(widthPx = 1920, heightPx = 1080, densityDpi = 400)
        val layout = DualScreenGeometry.compute(window, null, false, sideBySide, game169)
        assertEquals(DualScreenRect(0, 212, 1167, 656), layout.gameRect)
        assertEquals(DualScreenRect(1167, 212, 753, 656), layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, DualScreenAspect(1240, 1080))
    }

    // ---- split-screen half 1200x1080, landscape: narrower still ----

    @Test
    fun splitScreenHalfAspectLockedThenCollapses() {
        val window = DualScreenWindow(widthPx = 1200, heightPx = 1080, densityDpi = 420)
        val layout = DualScreenGeometry.compute(window, null, false, sideBySide, game169)
        assertTrue(layout.companionRect.isEmpty)
        assertEquals(DualScreenRect(0, 335, 1200, 410), layout.gameRect)
    }

    // ---- tablet 2560x1600, landscape: wide enough that both stay visible ----

    @Test
    fun tabletLandscapeBothPanesStayAspectLockedAndVisible() {
        // density 320 (xhdpi): a believable 10-11" tablet. Scale = 2560/4681.48... = 216/395;
        // companion ends up 1005x875 px = 437.5 dp short edge, comfortably above the floor.
        val window = DualScreenWindow(widthPx = 2560, heightPx = 1600, densityDpi = 320)
        val layout = DualScreenGeometry.compute(window, null, false, sideBySide, game169)
        assertEquals(DualScreenRect(0, 362, 1555, 875), layout.gameRect)
        assertEquals(DualScreenRect(1555, 362, 1005, 875), layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
        // Both panes keep their own locked aspect exactly (no internal bars).
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, DualScreenAspect(1240, 1080))
    }

    // ---- phone 1080x2400, portrait: stacked, three bands, companion stays visible ----

    @Test
    fun phonePortraitStaysVisibleBothControllerStates() {
        // Same phone as the landscape case above, rotated; same density. Full window *width*
        // this time (1080 px) budgets each pane's height: game 1080*9/16 = 608 px, companion
        // 1080*1080/1240 = 941 px (both comfortably fit the 2400 px height, no shrink needed) --
        // companion short edge 941/2.625 = 358.5 dp, over the floor.
        val window = DualScreenWindow(widthPx = 1080, heightPx = 2400, densityDpi = 420)
        val stacked = DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC)

        val noController = DualScreenGeometry.compute(window, null, false, stacked, game169)
        assertEquals(DualScreenRect(0, 0, 1080, 608), noController.gameRect)
        assertEquals(DualScreenRect(0, 608, 1080, 941), noController.companionRect)
        assertEquals(DualScreenRect(0, 1549, 1080, 851), noController.controlsRect)

        // A controller removes the dedicated controls band but does not resize either pane --
        // the leftover height below the companion is simply idle background instead.
        val withController = DualScreenGeometry.compute(window, null, true, stacked, game169)
        assertEquals(noController.gameRect, withController.gameRect)
        assertEquals(noController.companionRect, withController.companionRect)
        assertEquals(DualScreenRect.EMPTY, withController.controlsRect)
    }

    // ---- Fold cover 904x2316, portrait: collapses regardless of controller ----

    @Test
    fun foldCoverCollapsesRegardlessOfController() {
        // density 420 (phone-family cover screen). game 904*9/16 = 509 px, companion
        // 904*1080/1240 = 787 px -- companion short edge 787/2.625 = 299.8 dp, which actually
        // clears the Stage 4 200 dp per-pane floor on its own. It still collapses because the
        // *window* itself is the real problem here: its own short edge (width 904/2.625 =
        // 344.4 dp) is under MIN_WINDOW_SHORT_EDGE_DP (360 dp) -- this is exactly why Stage 4
        // needs that second, window-level floor: a per-pane floor alone, lowered enough to keep
        // the Thor top case (262.4 dp) visible, could not also collapse this one (299.8 dp).
        // Collapses whether or not a controller is connected (pane sizing does not depend on it).
        val window = DualScreenWindow(widthPx = 904, heightPx = 2316, densityDpi = 420)
        val stacked = DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC)

        val noController = DualScreenGeometry.compute(window, null, false, stacked, game169)
        assertTrue(noController.companionRect.isEmpty)
        assertEquals(DualScreenRect(0, 0, 904, 1296), noController.gameRect)
        assertEquals(DualScreenRect(0, 1296, 904, 1020), noController.controlsRect)

        val withController = DualScreenGeometry.compute(window, null, true, stacked, game169)
        assertTrue(withController.companionRect.isEmpty)
        assertEquals(DualScreenRect(0, 0, 904, 1296), withController.gameRect)
        assertEquals(DualScreenRect.EMPTY, withController.controlsRect)
    }

    // ---- Fold inner 2176x1812, book posture: split at the hinge, not aspect-locked ----

    @Test
    fun verticalHingeSplitsAtTheHingeIgnoringAspect() {
        val window = DualScreenWindow(widthPx = 2176, heightPx = 1812, densityDpi = 420)
        val hinge = DualScreenHinge(
            isSeparating = true,
            orientation = DualScreenHingeOrientation.VERTICAL,
            boundsLeft = 1080,
            boundsTop = 0,
            boundsRight = 1096,
            boundsBottom = 1812
        )
        val layout = DualScreenGeometry.compute(
            window, hinge, controllerConnected = false,
            userLayout = DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC), gameAspect = game169
        )
        assertEquals(DualScreenRect(0, 0, 1080, 1812), layout.gameRect)
        assertEquals(DualScreenRect(1096, 0, 1080, 1812), layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
    }

    // ---- tabletop (horizontal hinge): companion only with a controller ----

    @Test
    fun horizontalHingeGivesTheCompanionTheLowerHalfOnlyWithAController() {
        val window = DualScreenWindow(widthPx = 2176, heightPx = 1812, densityDpi = 420)
        val hinge = DualScreenHinge(
            isSeparating = true,
            orientation = DualScreenHingeOrientation.HORIZONTAL,
            boundsLeft = 0,
            boundsTop = 896,
            boundsRight = 2176,
            boundsBottom = 912
        )
        val userLayout = DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC)

        val withController =
            DualScreenGeometry.compute(window, hinge, controllerConnected = true, userLayout, game169)
        assertEquals(DualScreenRect(0, 0, 2176, 896), withController.gameRect)
        assertEquals(DualScreenRect(0, 912, 2176, 900), withController.companionRect)
        assertEquals(DualScreenRect.EMPTY, withController.controlsRect)

        // No controller: today's existing split -- game above the hinge, controls below; the
        // companion is not carved out (still reachable through the toggle).
        val noController =
            DualScreenGeometry.compute(window, hinge, controllerConnected = false, userLayout, game169)
        assertEquals(DualScreenRect(0, 0, 2176, 896), noController.gameRect)
        assertTrue(noController.companionRect.isEmpty)
        assertEquals(DualScreenRect(0, 912, 2176, 900), noController.controlsRect)
    }

    // ---- an explicit (non-Automatic) layout budgets the split axis and Fits within it ----

    @Test
    fun explicitSideBySideBudgetsByDividerAndFitsEachPaneWithinItsBudget() {
        // Height 1080 exactly matches the companion canvas's own height, so a 1240-wide budget
        // fits it with zero bars; width 2000, divider 38% -> game budget 760, companion budget
        // 1240 (clean numbers by construction, to make the Fit math exactly verifiable).
        val window = DualScreenWindow(widthPx = 2000, heightPx = 1080, densityDpi = 420)
        val manual = DualScreenUserLayout(DualScreenLayoutChoice.SIDE_BY_SIDE, dividerPercent = 38)
        // gameAspect is irrelevant here: Stretch (null) means the game pane is the budget as given.
        val layout = DualScreenGeometry.compute(window, null, false, manual, gameAspect = null)
        assertEquals(DualScreenRect(0, 0, 760, 1080), layout.gameRect)
        assertEquals(DualScreenRect(760, 0, 1240, 1080), layout.companionRect)
    }

    @Test
    fun explicitSideBySideLetterboxesTheCompanionWhenItsBudgetDoesNotMatchTheCanvasAspect() {
        // Companion budget 1000 px wide x 1080 tall does not match 1240x1080 -- Fit centres a
        // 871-tall companion inside it (1000*1080/1240 = 871.46, truncated as FitCompanion does).
        val window = DualScreenWindow(widthPx = 1760, heightPx = 1080, densityDpi = 420)
        val manual = DualScreenUserLayout(DualScreenLayoutChoice.SIDE_BY_SIDE, dividerPercent = 43)
        val layout = DualScreenGeometry.compute(window, null, false, manual, gameAspect = null)
        // gameBudgetW = round(1760*0.43) = 757; companion budget = 1760-757 = 1003 (not 1000 --
        // kept the arithmetic exact rather than hand-picking a divider for a round number).
        assertEquals(DualScreenRect(0, 0, 757, 1080), layout.gameRect)
        assertEquals(757, layout.companionRect.x)
        assertTrue(layout.companionRect.h < 1080) // letterboxed: bars above/below within the budget
    }

    // ---- Beta 3: an explicit layout is exempt from the per-pane floor ----

    @Test
    fun explicitStackedOnTheThorTopScreenStaysVisibleAtTheDefaultDivider() {
        // Thor top screen, 1920x1080, density 416 (~2.6, matching the real device) -- the Beta 2
        // regression: Stacked with the default (unset, 60%) divider budgets the companion band at
        // only 432 px tall, whose short edge (432 px = 166.2 dp) is under the 200 dp per-pane
        // floor that every branch applied through Beta 2, collapsing the companion to nothing.
        // An explicit choice is now exempt from that floor -- only the window-itself floor
        // applies, and this window (415.4 dp short edge) clears it, so both panes stay visible.
        val window = DualScreenWindow(widthPx = 1920, heightPx = 1080, densityDpi = 416)
        val stacked = DualScreenUserLayout(DualScreenLayoutChoice.STACKED) // dividerPercent defaults to 60
        val layout = DualScreenGeometry.compute(window, null, false, stacked, game169)
        assertEquals(DualScreenRect(384, 0, 1152, 648), layout.gameRect)
        assertEquals(DualScreenRect(712, 648, 496, 432), layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, DualScreenAspect(1240, 1080))
    }

    @Test
    fun explicitStackedOnTheThorTopScreenStaysVisibleAtADifferentDivider() {
        // Same window, divider moved to 50% (game/companion get equal height budgets). Still
        // comfortably exempt from the per-pane floor as an explicit choice.
        val window = DualScreenWindow(widthPx = 1920, heightPx = 1080, densityDpi = 416)
        val stacked = DualScreenUserLayout(DualScreenLayoutChoice.STACKED, dividerPercent = 50)
        val layout = DualScreenGeometry.compute(window, null, false, stacked, game169)
        assertEquals(DualScreenRect(480, 0, 960, 540), layout.gameRect)
        assertEquals(DualScreenRect(650, 540, 620, 540), layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, DualScreenAspect(1240, 1080))
    }

    @Test
    fun explicitStackedOnALandscapePhoneStaysVisibleEvenThoughAutomaticWouldHaveUsedSideBySide() {
        // Phone 2400x1080 landscape, density 420 -- same per-pane-floor-would-have-collapsed-it
        // shape as the Thor top case (companion band short edge 432 px = 164.6 dp, under the 200
        // dp floor), proving the Beta 3 fix is not Thor-specific. The user explicitly chose
        // Stacked despite the landscape orientation (Automatic would pick Side by side here
        // instead -- see [phoneLandscapeAspectLockedStaysSideBySide]); their explicit choice is
        // honoured and stays visible.
        val window = DualScreenWindow(widthPx = 2400, heightPx = 1080, densityDpi = 420)
        val stacked = DualScreenUserLayout(DualScreenLayoutChoice.STACKED)
        val layout = DualScreenGeometry.compute(window, null, false, stacked, game169)
        assertEquals(DualScreenRect(624, 0, 1152, 648), layout.gameRect)
        assertEquals(DualScreenRect(952, 648, 496, 432), layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, DualScreenAspect(1240, 1080))
    }

    @Test
    fun explicitStackedStillCollapsesOnAWindowBelowTheWindowFloor() {
        // The fold cover window (904x2316, density 420) from
        // [foldCoverCollapsesRegardlessOfController] -- its own short edge (344.4 dp) is under
        // MIN_WINDOW_SHORT_EDGE_DP regardless of the per-pane floor exemption, so an explicit
        // Stacked choice still collapses here: the Beta 3 rule exempts explicit
        // layouts from the per-pane floor only, not the window-itself floor.
        val window = DualScreenWindow(widthPx = 904, heightPx = 2316, densityDpi = 420)
        val stacked = DualScreenUserLayout(DualScreenLayoutChoice.STACKED)
        val layout = DualScreenGeometry.compute(window, null, false, stacked, game169)
        assertTrue(layout.companionRect.isEmpty)
        // The merge is the bounding box of the two (letterboxed, non-touching-at-every-edge)
        // budgeted bands, not a clean full-window rect -- pre-existing collapse() behaviour,
        // unchanged by this fix.
        assertEquals(DualScreenRect(0, 440, 904, 1806), layout.gameRect)
    }

    private fun assertAspectLocked(rect: DualScreenRect, aspect: DualScreenAspect) {
        // Integer rounding on both sides can disagree by a touch; allow it loosely rather than
        // asserting exact cross-multiplication equality.
        val actual = rect.w.toDouble() / rect.h
        val expected = aspect.w.toDouble() / aspect.h
        assertTrue(
            "rect $rect is not locked to aspect $aspect (actual=$actual expected=$expected)",
            kotlin.math.abs(actual - expected) < 0.01
        )
    }
}
