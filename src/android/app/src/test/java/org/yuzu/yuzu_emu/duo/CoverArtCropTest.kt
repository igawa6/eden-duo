// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CoverArtCropTest {
    private val eshopRed = 0xFFC00010.toInt()
    private val art = 0xFF336699.toInt()

    /** A [w] x [h] cover filled with [fill] and [inner] painted as a rectangle. */
    private fun cover(w: Int, h: Int, fill: Int, inner: CoverArtCrop.Area? = null): IntArray =
        IntArray(w * h) { i ->
            val x = i % w
            val y = i / w
            if (inner != null && x in inner.left until inner.left + inner.width &&
                y in inner.top until inner.top + inner.height
            ) art else fill
        }

    @Test
    fun boxCoverGivesItsCentreSquare() {
        val pixels = cover(352, 570, 0xFF202020.toInt())
        assertNull(CoverArtCrop.eshopInner(pixels, 352, 570))
        assertEquals(CoverArtCrop.Area(0, 109, 352, 352), CoverArtCrop.tileSquare(pixels, 352, 570))
    }

    @Test
    fun eshopTemplateGivesTheCentreOfItsKeyArt() {
        // GameTDB coverM eShop template: 16:9 key art of 282x159 at (35,200).
        val key = CoverArtCrop.Area(35, 200, 282, 159)
        val pixels = cover(352, 570, eshopRed, key)
        // Same bounds as the mockup renderer: one pixel in from the first non-red row/column.
        assertEquals(CoverArtCrop.Area(36, 201, 280, 157), CoverArtCrop.eshopInner(pixels, 352, 570))
        assertEquals(CoverArtCrop.Area(97, 201, 157, 157), CoverArtCrop.tileSquare(pixels, 352, 570))
    }

    @Test
    fun redCoverWithoutKeyArtIsNotTheTemplate() {
        assertNull(CoverArtCrop.eshopInner(cover(352, 570, eshopRed), 352, 570))
        // A reddish box cover that fails the probe (too much green) stays a box cover.
        assertNull(CoverArtCrop.eshopInner(cover(352, 570, 0xFFC05010.toInt()), 352, 570))
    }

    @Test
    fun tinyOrShortInputIsIgnored() {
        assertNull(CoverArtCrop.eshopInner(IntArray(4), 2, 2))
        assertNull(CoverArtCrop.eshopInner(IntArray(10), 352, 570))
    }

    @Test
    fun centreSquareOfWideAndTallAreas() {
        assertEquals(CoverArtCrop.Area(10, 0, 20, 20), CoverArtCrop.centreSquare(CoverArtCrop.Area(0, 0, 40, 20)))
        assertEquals(CoverArtCrop.Area(5, 15, 10, 10), CoverArtCrop.centreSquare(CoverArtCrop.Area(5, 5, 10, 30)))
    }
}
