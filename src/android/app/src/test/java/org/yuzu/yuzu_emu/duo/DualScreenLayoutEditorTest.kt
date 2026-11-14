// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Pure, unit-tested geometry for the freeform layout editor. */
class DualScreenLayoutEditorTest {
    private val game169 = DualScreenAspect(16, 9)
    private val canvas = DualScreenAspect(1240, 1080)

    // ---- move ----

    @Test
    fun moveTranslatesWithoutChangingSize() {
        val rect = DualScreenRect(10, 20, 300, 200)
        assertEquals(DualScreenRect(60, -5, 300, 200), DualScreenLayoutEditor.move(rect, 50, -25))
    }

    // ---- resizeFromHandle: aspect lock ----

    @Test
    fun resizeFromHandleKeepsLockedAspect() {
        val rect = DualScreenRect(0, 0, 160, 90) // 16:9
        val resized = DualScreenLayoutEditor.resizeFromHandle(
            rect, DualScreenEditorHandle.BOTTOM_RIGHT, dx = 160, dy = 0, aspect = game169, minW = 1, minH = 1
        )
        assertAspectLocked(resized, game169)
        // Anchor (top-left) stays fixed; the box grew to at least cover the drag.
        assertEquals(0, resized.x)
        assertEquals(0, resized.y)
        assertTrue(resized.w >= 320)
    }

    @Test
    fun resizeFromHandleAnchorsTheOppositeCorner() {
        val rect = DualScreenRect(100, 100, 160, 90)
        val resized = DualScreenLayoutEditor.resizeFromHandle(
            rect, DualScreenEditorHandle.TOP_LEFT, dx = -80, dy = -45, aspect = game169, minW = 1, minH = 1
        )
        // TOP_LEFT handle: the bottom-right corner (260, 190) is the fixed anchor.
        assertEquals(260, resized.x + resized.w)
        assertEquals(190, resized.y + resized.h)
        assertAspectLocked(resized, game169)
    }

    @Test
    fun resizeFromHandleNeverGoesBelowTheMinimumSize() {
        val rect = DualScreenRect(0, 0, 160, 90)
        val shrunk = DualScreenLayoutEditor.resizeFromHandle(
            rect, DualScreenEditorHandle.BOTTOM_RIGHT, dx = -158, dy = -88, aspect = game169, minW = 160, minH = 90
        )
        assertTrue(shrunk.w >= 160)
        assertTrue(shrunk.h >= 90)
        assertAspectLocked(shrunk, game169)
    }

    @Test
    fun resizeFromHandleWithoutAspectResizesFreely() {
        val rect = DualScreenRect(0, 0, 100, 100)
        val resized = DualScreenLayoutEditor.resizeFromHandle(
            rect, DualScreenEditorHandle.BOTTOM_RIGHT, dx = 50, dy = 10, aspect = null, minW = 1, minH = 1
        )
        assertEquals(150, resized.w)
        assertEquals(110, resized.h)
    }

    // ---- resizeFromPinch ----

    @Test
    fun resizeFromPinchScalesAroundTheCentreAndKeepsAspect() {
        val rect = DualScreenRect(100, 100, 160, 90)
        val scaled = DualScreenLayoutEditor.resizeFromPinch(rect, 2f, game169, minW = 1, minH = 1)
        assertAspectLocked(scaled, game169)
        assertEquals(rect.x + rect.w / 2, scaled.x + scaled.w / 2)
        assertEquals(rect.y + rect.h / 2, scaled.y + scaled.h / 2)
        assertTrue(scaled.w > rect.w)
    }

    @Test
    fun resizeFromPinchIgnoresNonPositiveOrNonFiniteScale() {
        val rect = DualScreenRect(10, 10, 160, 90)
        assertEquals(rect, DualScreenLayoutEditor.resizeFromPinch(rect, 0f, game169, 1, 1))
        assertEquals(rect, DualScreenLayoutEditor.resizeFromPinch(rect, -1f, game169, 1, 1))
        assertEquals(rect, DualScreenLayoutEditor.resizeFromPinch(rect, Float.NaN, game169, 1, 1))
    }

    @Test
    fun resizeFromPinchRespectsMinimumSize() {
        val rect = DualScreenRect(0, 0, 160, 90)
        val shrunk = DualScreenLayoutEditor.resizeFromPinch(rect, 0.1f, game169, minW = 160, minH = 90)
        assertTrue(shrunk.w >= 160)
        assertTrue(shrunk.h >= 90)
    }

    // ---- clampToBox ----

    @Test
    fun clampToBoxKeepsARectThatAlreadyFitsUnchanged() {
        val box = DualScreenRect(0, 0, 1000, 800)
        val rect = DualScreenRect(100, 100, 200, 150)
        assertEquals(rect, DualScreenLayoutEditor.clampToBox(rect, box))
    }

    @Test
    fun clampToBoxTranslatesBackInsideWhenDraggedPastTheEdge() {
        val box = DualScreenRect(0, 0, 1000, 800)
        val rect = DualScreenRect(-50, 900, 200, 150)
        val clamped = DualScreenLayoutEditor.clampToBox(rect, box)
        assertEquals(0, clamped.x)
        assertEquals(650, clamped.y)
        assertEquals(200, clamped.w)
        assertEquals(150, clamped.h)
    }

    @Test
    fun clampToBoxShrinksARectLargerThanTheBoxItself() {
        val box = DualScreenRect(0, 0, 100, 80)
        val rect = DualScreenRect(0, 0, 500, 500)
        val clamped = DualScreenLayoutEditor.clampToBox(rect, box)
        // Scaled down together (square stays square) until it fits the box's short side.
        assertEquals(80, clamped.w)
        assertEquals(80, clamped.h)
    }

    @Test
    fun clampToBoxKeepsTheAspectOfAnAspectLockedRectWhenShrinking() {
        val box = DualScreenRect(0, 0, 1000, 1000)
        val rect = DualScreenRect(0, 0, 1600, 900) // 16:9, too wide for the box
        val clamped = DualScreenLayoutEditor.clampToBox(rect, box)
        assertEquals(1000, clamped.w)
        assertEquals(562, clamped.h)
        assertAspectLocked(clamped, game169)
    }

    // ---- constrain: the full per-gesture pass ----

    @Test
    fun constrainNeverPushesADraggedRectOutsideTheWindow() {
        // Dragging the companion across the game: the shorter push (down, past the window's
        // bottom edge, or left to a negative x) would leave the window, so the pass picks the
        // nearest push that keeps it inside -- here back to the right of the game.
        val window = DualScreenWindow(2400, 1080, 420)
        val box = DualScreenRect(0, 0, 2400, 1080)
        val game = DualScreenRect(0, 129, 1461, 822)
        val lastValid = DualScreenRect(1461, 131, 939, 818)
        val dragged = DualScreenLayoutEditor.move(lastValid, -1261, 0) // x = 200
        val result = DualScreenLayoutEditor.constrain(dragged, box, game, lastValid, window)
        assertInsideWindow(result, window)
        assertNoOverlap(result, game)
        assertEquals(939, result.w)
        assertEquals(818, result.h)
    }

    @Test
    fun constrainKeepsTheLastValidRectWhenNoPlacementFits() {
        // A resize that grows the companion past the only free strip beside the game: no
        // translation keeps it inside and clear, so the step is not applied.
        val box = DualScreenRect(0, 0, 1000, 500)
        val game = DualScreenRect(0, 0, 700, 500)
        val lastValid = DualScreenRect(700, 0, 300, 261)
        val grown = DualScreenRect(600, 0, 400, 348)
        val result = DualScreenLayoutEditor.constrain(grown, box, game, lastValid)
        assertEquals(lastValid, result)
    }

    @Test
    fun separateIsANoOpForAnAlreadyValidPlacement() {
        val box = DualScreenRect(0, 0, 1000, 500)
        val game = DualScreenRect(0, 0, 600, 338)
        val companion = DualScreenRect(650, 0, 300, 261)
        assertEquals(companion, DualScreenLayoutEditor.separate(companion, game, box))
    }

    @Test
    fun clampToBoxRespectsANonZeroOriginBox() {
        val box = DualScreenRect(50, 60, 200, 150)
        val rect = DualScreenRect(0, 0, 50, 50)
        val clamped = DualScreenLayoutEditor.clampToBox(rect, box)
        assertEquals(50, clamped.x)
        assertEquals(60, clamped.y)
    }

    // ---- resolveOverlap: "move into the other rect stops at its edge" ----

    @Test
    fun resolveOverlapLeavesNonOverlappingRectsUnchanged() {
        val moving = DualScreenRect(0, 0, 100, 100)
        val other = DualScreenRect(200, 0, 100, 100)
        assertEquals(moving, DualScreenLayoutEditor.resolveOverlap(moving, other))
    }

    @Test
    fun resolveOverlapStopsAMoveAtTheOtherRectsLeftEdge() {
        // The moving rect drifted right into `other`; the smaller correction is along X.
        val other = DualScreenRect(300, 0, 200, 800)
        val moving = DualScreenRect(250, 0, 100, 100) // overlaps [300,350) x [0,100)
        val resolved = DualScreenLayoutEditor.resolveOverlap(moving, other)
        assertEquals(other.x - moving.w, resolved.x)
        assertEquals(moving.y, resolved.y)
        // Touching, not overlapping, and not reshaped.
        assertEquals(moving.w, resolved.w)
        assertEquals(moving.h, resolved.h)
        assertEquals(0, minOf(resolved.x + resolved.w, other.x + other.w) - maxOf(resolved.x, other.x))
    }

    @Test
    fun resolveOverlapStopsAMoveAtTheOtherRectsTopEdge() {
        val other = DualScreenRect(0, 300, 800, 200)
        val moving = DualScreenRect(0, 250, 100, 100) // overlaps on Y more cheaply than X
        val resolved = DualScreenLayoutEditor.resolveOverlap(moving, other)
        assertEquals(other.y - moving.h, resolved.y)
        assertEquals(moving.x, resolved.x)
    }

    @Test
    fun resolveOverlapPushesTowardTheSideTheRectIsAlreadyCloserTo() {
        val other = DualScreenRect(100, 0, 100, 100)
        val movingFromRight = DualScreenRect(150, 0, 100, 100)
        val resolved = DualScreenLayoutEditor.resolveOverlap(movingFromRight, other)
        assertEquals(other.x + other.w, resolved.x)
    }

    @Test
    fun resolveOverlapIsANoOpForAnEmptyRect() {
        assertEquals(DualScreenRect.EMPTY, DualScreenLayoutEditor.resolveOverlap(DualScreenRect.EMPTY, DualScreenRect(0, 0, 10, 10)))
    }

    // ---- snap ----

    @Test
    fun snapAlignsFlushAgainstTheOtherRectsEdgeWithinThreshold() {
        val window = DualScreenWindow(2000, 1000, 420) // 8 dp = 21 px
        val other = DualScreenRect(1000, 0, 500, 1000)
        // Flush would put this rect's right edge at other.x (990 is the matching x); start 10 px
        // short of that (right edge at 990, 10 px shy of other's left edge at 1000).
        val rect = DualScreenRect(690, 10, 300, 300)
        val snapped = DualScreenLayoutEditor.snap(rect, window, other)
        assertEquals(other.x - rect.w, snapped.x)
    }

    @Test
    fun snapToWindowEdgeWithinThreshold() {
        val window = DualScreenWindow(2000, 1000, 420)
        val other = DualScreenRect(1500, 0, 500, 1000)
        val rect = DualScreenRect(8, 10, 300, 300) // 8 px from the left window edge
        val snapped = DualScreenLayoutEditor.snap(rect, window, other)
        assertEquals(0, snapped.x)
    }

    @Test
    fun snapDoesNothingOutsideTheThreshold() {
        val window = DualScreenWindow(2000, 1000, 420)
        val other = DualScreenRect(1500, 0, 500, 1000)
        val rect = DualScreenRect(123, 77, 300, 300) // not close to any candidate
        assertEquals(rect, DualScreenLayoutEditor.snap(rect, window, other))
    }

    @Test
    fun snapToEqualGapCentring() {
        // Window 1000 wide, other rect 300 wide at x=700..1000; a 200-wide rect centred in the
        // 700 px of room to the left of `other` (0..700) sits at x=250 (gap 250 on each side).
        val window = DualScreenWindow(1000, 800, 420)
        val other = DualScreenRect(700, 0, 300, 800)
        val rect = DualScreenRect(248, 0, 200, 800)
        val snapped = DualScreenLayoutEditor.snap(rect, window, other)
        assertEquals(250, snapped.x)
    }

    // ---- swap: "swap fits" ----

    @Test
    fun swapExchangesPositionsAndRefitsEachToItsOwnAspect() {
        val window = DualScreenWindow(2000, 1000, 420)
        val game = DualScreenRect(0, 0, 800, 450) // 16:9, on the left
        val companion = DualScreenRect(900, 0, 620, 540) // ~1240:1080, on the right
        val (newGame, newCompanion) = DualScreenLayoutEditor.swap(window, game, companion, game169, canvas)
        assertAspectLocked(newGame, game169)
        assertAspectLocked(newCompanion, canvas)
        // The game now fits inside the companion's old box, and vice versa.
        assertTrue(newGame.x >= companion.x - 1 && newGame.x + newGame.w <= companion.x + companion.w + 1)
        assertTrue(newCompanion.x >= game.x - 1 && newCompanion.x + newCompanion.w <= game.x + game.w + 1)
        assertNoOverlap(newGame, newCompanion)
    }

    @Test
    fun swapNeverLeavesTheResultOverlappingOrOutsideTheWindow() {
        val window = DualScreenWindow(1200, 900, 420)
        val game = DualScreenRect(0, 0, 500, 281)
        val companion = DualScreenRect(500, 0, 420, 366)
        val (newGame, newCompanion) = DualScreenLayoutEditor.swap(window, game, companion, game169, canvas)
        assertInsideWindow(newGame, window)
        assertInsideWindow(newCompanion, window)
        assertNoOverlap(newGame, newCompanion)
    }

    // ---- normalize / denormalize ----

    @Test
    fun toNormRectAndDenormalizeRoundTrip() {
        val window = DualScreenWindow(1920, 1080, 420)
        val rect = DualScreenRect(480, 270, 960, 540)
        val frac = rect.toNormRect(window)
        assertEquals(0.25f, frac.x, 0.001f)
        assertEquals(0.25f, frac.y, 0.001f)
        assertEquals(0.5f, frac.w, 0.001f)
        assertEquals(0.5f, frac.h, 0.001f)
        assertEquals(rect, frac.denormalize(window))
    }

    @Test
    fun toNormRectIsAllZeroForAZeroSizedWindow() {
        val window = DualScreenWindow(0, 0, 420)
        val rect = DualScreenRect(10, 10, 10, 10)
        assertEquals(DualScreenNormRect(0f, 0f, 0f, 0f), rect.toNormRect(window))
    }

    // ---- refitOnWindowChange: "stored fractions on a different window size still non-overlapping and inside" ----

    @Test
    fun refitOnWindowChangeKeepsFractionsAspectLockedAndNonOverlappingOnTheSameWindow() {
        val window = DualScreenWindow(2000, 1000, 420)
        val gameBox = DualScreenRect(0, 0, 1200, 1000)
        val companionBox = DualScreenRect(1200, 0, 800, 1000)
        val custom = DualScreenCustomLayout(gameBox.toNormRect(window), companionBox.toNormRect(window))
        val layout = DualScreenLayoutEditor.refitOnWindowChange(custom, window, game169, canvas)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, canvas)
        assertInsideWindow(layout.gameRect, window)
        assertInsideWindow(layout.companionRect, window)
        assertNoOverlap(layout.gameRect, layout.companionRect)
    }

    @Test
    fun refitOnWindowChangeStaysNonOverlappingAndInsideOnAMuchSmallerWindow() {
        // Captured on a wide tablet-sized window...
        val original = DualScreenWindow(2560, 1600, 320)
        val gameBox = DualScreenRect(0, 0, 1536, 1600)
        val companionBox = DualScreenRect(1536, 0, 1024, 1600)
        val custom = DualScreenCustomLayout(gameBox.toNormRect(original), companionBox.toNormRect(original))

        // ...then applied to a much narrower phone window: both fractional boxes shrink together,
        // but each pane still must stay aspect-locked, inside the window and non-overlapping.
        val narrow = DualScreenWindow(1200, 1080, 420)
        val layout = DualScreenLayoutEditor.refitOnWindowChange(custom, narrow, game169, canvas)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, canvas)
        assertInsideWindow(layout.gameRect, narrow)
        assertInsideWindow(layout.companionRect, narrow)
        assertNoOverlap(layout.gameRect, layout.companionRect)
    }

    @Test
    fun refitOnWindowChangeStaysNonOverlappingAndInsideOnARotatedWindow() {
        val landscape = DualScreenWindow(2400, 1080, 420)
        val gameBox = DualScreenRect(0, 0, 1440, 1080)
        val companionBox = DualScreenRect(1440, 0, 960, 1080)
        val custom = DualScreenCustomLayout(gameBox.toNormRect(landscape), companionBox.toNormRect(landscape))

        // The same fractions, applied after a 90-degree rotation (portrait window).
        val portrait = DualScreenWindow(1080, 2400, 420)
        val layout = DualScreenLayoutEditor.refitOnWindowChange(custom, portrait, game169, canvas)
        assertInsideWindow(layout.gameRect, portrait)
        assertInsideWindow(layout.companionRect, portrait)
        assertNoOverlap(layout.gameRect, layout.companionRect)
    }

    @Test
    fun refitOnWindowChangeSeparatesAnOverlappingSavedLayoutInsideTheWindow() {
        // A saved companion box sitting mostly on top of the game: the preferred push would
        // leave the window, so it must land on the free side instead -- never outside.
        val window = DualScreenWindow(2400, 1080, 420)
        val custom = DualScreenCustomLayout(
            DualScreenNormRect(0f, 0.12f, 0.61f, 0.76f),
            DualScreenNormRect(0.08f, 0.12f, 0.39f, 0.76f)
        )
        val layout = DualScreenLayoutEditor.refitOnWindowChange(custom, window, game169, canvas)
        assertInsideWindow(layout.gameRect, window)
        assertInsideWindow(layout.companionRect, window)
        assertNoOverlap(layout.gameRect, layout.companionRect)
    }

    @Test
    fun refitOnWindowChangeFallsBackToAFreeStripWhenNoPushFits() {
        // The game fills the window's full height; the saved companion sits in its middle and no
        // push clears it inside the window, so it is re-fit into the larger free side strip.
        val window = DualScreenWindow(2400, 1080, 420)
        val custom = DualScreenCustomLayout(
            DualScreenNormRect(0.1f, 0f, 0.8f, 1f),
            DualScreenNormRect(0.4f, 0.2f, 0.2f, 0.5f)
        )
        val layout = DualScreenLayoutEditor.refitOnWindowChange(custom, window, game169, canvas)
        assertTrue(!layout.companionRect.isEmpty)
        assertAspectLocked(layout.companionRect, canvas)
        assertInsideWindow(layout.companionRect, window)
        assertNoOverlap(layout.gameRect, layout.companionRect)
    }

    // ---- DualScreenGeometry.compute with CUSTOM ----

    @Test
    fun computeWithCustomAndNoSavedLayoutFallsBackToAutomatic() {
        val window = DualScreenWindow(2400, 1080, 420)
        val custom = DualScreenUserLayout(DualScreenLayoutChoice.CUSTOM, customLayout = null)
        val automatic = DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC)
        assertEquals(
            DualScreenGeometry.compute(window, null, false, automatic, game169),
            DualScreenGeometry.compute(window, null, false, custom, game169)
        )
    }

    @Test
    fun computeWithCustomAndNoSavedLayoutAlsoInheritsAutomaticsPerPaneFloorCollapse() {
        // Same window as DualScreenGeometryTest.splitScreenHalfAspectLockedThenCollapses, where
        // Automatic's own pane floor (not the window floor) is what collapses the companion --
        // the null-fallback must reach the same outcome, not the Custom/explicit exemption from
        // that floor (that exemption only applies once the user has actually saved a layout).
        val window = DualScreenWindow(1200, 1080, 420)
        val custom = DualScreenUserLayout(DualScreenLayoutChoice.CUSTOM, customLayout = null)
        val automatic = DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC)
        val automaticResult = DualScreenGeometry.compute(window, null, false, automatic, game169)
        val customResult = DualScreenGeometry.compute(window, null, false, custom, game169)
        assertTrue(automaticResult.companionRect.isEmpty)
        assertEquals(automaticResult, customResult)
    }

    @Test
    fun computeWithCustomAppliesTheSavedFractionsAndStaysAspectLockedAndNonOverlapping() {
        val window = DualScreenWindow(2000, 1000, 420)
        val gameBox = DualScreenRect(0, 0, 1200, 1000)
        val companionBox = DualScreenRect(1200, 0, 800, 1000)
        val savedLayout = DualScreenCustomLayout(gameBox.toNormRect(window), companionBox.toNormRect(window))
        val userLayout = DualScreenUserLayout(DualScreenLayoutChoice.CUSTOM, customLayout = savedLayout)
        val layout = DualScreenGeometry.compute(window, null, false, userLayout, game169)
        assertAspectLocked(layout.gameRect, game169)
        assertAspectLocked(layout.companionRect, canvas)
        assertNoOverlap(layout.gameRect, layout.companionRect)
        assertEquals(DualScreenRect.EMPTY, layout.controlsRect)
    }

    @Test
    fun computeWithCustomIsExemptFromThePerPaneFloorButNotTheWindowFloor() {
        // Same fold-cover window as DualScreenGeometryTest.foldCoverCollapsesRegardlessOfController:
        // its own short edge is under MIN_WINDOW_SHORT_EDGE_DP, so Custom still collapses there.
        val window = DualScreenWindow(904, 2316, 420)
        val gameBox = DualScreenRect(0, 0, 904, 1296)
        val companionBox = DualScreenRect(0, 1296, 904, 787)
        val savedLayout = DualScreenCustomLayout(gameBox.toNormRect(window), companionBox.toNormRect(window))
        val userLayout = DualScreenUserLayout(DualScreenLayoutChoice.CUSTOM, customLayout = savedLayout)
        val layout = DualScreenGeometry.compute(window, null, false, userLayout, game169)
        assertTrue(layout.companionRect.isEmpty)
    }

    // ---- shared assertions ----

    private fun assertAspectLocked(rect: DualScreenRect, aspect: DualScreenAspect) {
        val actual = rect.w.toDouble() / rect.h
        val expected = aspect.w.toDouble() / aspect.h
        assertTrue(
            "rect $rect is not locked to aspect $aspect (actual=$actual expected=$expected)",
            kotlin.math.abs(actual - expected) < 0.01
        )
    }

    private fun assertNoOverlap(a: DualScreenRect, b: DualScreenRect) {
        val overlapX = minOf(a.x + a.w, b.x + b.w) - maxOf(a.x, b.x)
        val overlapY = minOf(a.y + a.h, b.y + b.h) - maxOf(a.y, b.y)
        assertTrue("rects overlap: $a, $b", overlapX <= 0 || overlapY <= 0)
    }

    private fun assertInsideWindow(rect: DualScreenRect, window: DualScreenWindow) {
        assertTrue("rect $rect starts outside the window", rect.x >= 0 && rect.y >= 0)
        assertTrue(
            "rect $rect extends past the window ($window)",
            rect.x + rect.w <= window.widthPx && rect.y + rect.h <= window.heightPx
        )
    }
}
