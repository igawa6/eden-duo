// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Routing for touches on emulation_container in the 3-band layout: phone 1080x2400 portrait, no
 * controller (the same layout as DualScreenGeometryTest.phonePortraitStaysVisibleBothControllerStates:
 * game 0,0 1080x608, companion 0,608 1080x941, controls band from y = 1549). The container and
 * its surface are shrunk to the 1549 px above the band.
 */
class PaneTouchFrameTest {
    private val window = DualScreenWindow(widthPx = 1080, heightPx = 2400, densityDpi = 420)
    private val layout = DualScreenGeometry.compute(
        window, null, false, DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC), DualScreenAspect(16, 9)
    )
    private val surfaceH = layout.controlsRect.y
    private val frame = PaneTouchFrame(layout, 0, 0, 0, 0, window.widthPx, surfaceH)

    @Test
    fun theLayoutIsTheThreeBandOne() {
        assertEquals(DualScreenRect(0, 0, 1080, 608), layout.gameRect)
        assertEquals(DualScreenRect(0, 608, 1080, 941), layout.companionRect)
        assertEquals(1549, surfaceH)
    }

    @Test
    fun aDownOnTheGamePaneIsGame() {
        assertEquals(PaneTouchTarget.GAME, frame.targetAt(540f, 300f))
        assertEquals(PaneTouchTarget.GAME, frame.targetAt(0f, 0f))
        assertEquals(PaneTouchTarget.GAME, frame.targetAt(1079f, 607.9f))
    }

    @Test
    fun aDownOnTheCompanionPaneIsCompanion() {
        assertEquals(PaneTouchTarget.COMPANION, frame.targetAt(540f, 608f))
        assertEquals(PaneTouchTarget.COMPANION, frame.targetAt(540f, 1548f))
    }

    @Test
    fun aDownOutsideBothPanesIsNone() {
        // Past the panes (the controls band's own space, which InputOverlay keeps) and off-window.
        assertEquals(PaneTouchTarget.NONE, frame.targetAt(540f, 1549f))
        assertEquals(PaneTouchTarget.NONE, frame.targetAt(-1f, 300f))
        assertEquals(PaneTouchTarget.NONE, frame.targetAt(1080f, 300f))
    }

    @Test
    fun aHiddenCompanionLeavesOnlyTheGame() {
        val noCompanion = frame.copy(layout = layout.copy(companionRect = DualScreenRect.EMPTY))
        assertEquals(PaneTouchTarget.GAME, noCompanion.targetAt(540f, 300f))
        assertEquals(PaneTouchTarget.NONE, noCompanion.targetAt(540f, 900f))
    }

    @Test
    fun aContainerNotAtTheAreasOriginIsTranslatedIntoLayoutCoordinates() {
        // The container 100 px down the area: its own y = 520 is the area's 620, the companion.
        val shifted = frame.copy(originY = 100)
        assertEquals(PaneTouchTarget.COMPANION, shifted.targetAt(540f, 520f))
        assertEquals(PaneTouchTarget.GAME, shifted.targetAt(540f, 500f))
    }

    @Test
    fun theGamePanesCentreLandsOnTheCentreOfTheRectTheNativeSideMapsFor() {
        // The native split is sent as fractions of the shrunk surface (DualScreenFragmentHost
        // .updateLayout); EmuWindow_Android turns them back into surface pixels with the surface's
        // own size and maps a touch into that rect. A touch at the game pane's centre must land
        // at the centre of that rect.
        val frac = layout.gameRect.toFrac(window.widthPx, surfaceH)
        val nativeX = frac[0] * window.widthPx
        val nativeY = frac[1] * surfaceH
        val nativeW = frac[2] * window.widthPx
        val nativeH = frac[3] * surfaceH
        val centreX = layout.gameRect.x + layout.gameRect.w / 2f
        val centreY = layout.gameRect.y + layout.gameRect.h / 2f
        assertEquals(nativeX + nativeW / 2f, frame.toSurfaceX(centreX), 0.5f)
        assertEquals(nativeY + nativeH / 2f, frame.toSurfaceY(centreY), 0.5f)
    }

    @Test
    fun surfaceCoordinatesSubtractTheSurfacesOffsetInsideTheContainer() {
        val offset = frame.copy(surfaceLeft = 10, surfaceTop = 20, surfaceWidth = 1060, surfaceHeight = 1500)
        assertEquals(90f, offset.toSurfaceX(100f), 0f)
        assertEquals(80f, offset.toSurfaceY(100f), 0f)
    }

    @Test
    fun aFingerThatSlidesOffTheSurfaceStaysOnItsEdge() {
        // Moving down into the controls band, or up/left past the window edge, never sends a
        // point outside the surface (the native side casts it to an unsigned pixel).
        assertEquals((surfaceH - 1).toFloat(), frame.toSurfaceY(2000f), 0f)
        assertEquals(0f, frame.toSurfaceY(-40f), 0f)
        assertEquals(0f, frame.toSurfaceX(-5f), 0f)
        assertEquals(1079f, frame.toSurfaceX(1200f), 0f)
    }
}
