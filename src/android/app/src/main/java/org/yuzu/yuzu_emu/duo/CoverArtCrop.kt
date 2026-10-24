// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

/**
 * Eden Duo: where the square tile art comes from in a GameTDB cover, over plain ARGB pixels
 * (no Android Bitmap, so it is unit-testable).
 *
 * A box cover gives its centre square. For an eShop-only game GameTDB's cover is Nintendo's red
 * eShop template -- a red field with a small 16:9 key art in the middle and a "Nintendo eShop"
 * footer; then the square comes from the centre of that key art instead.
 */
object CoverArtCrop {
    data class Area(val left: Int, val top: Int, val width: Int, val height: Int)

    private fun red(argb: Int) = (argb shr 16 and 0xFF) > 150 && (argb shr 8 and 0xFF) < 50 &&
        (argb and 0xFF) < 60

    /**
     * The key art inside an eShop template cover ([pixels] is [width] x [height], row-major
     * ARGB), or null when the cover is not the template. Probes the template's red field at the
     * centre column, 22 % down, then the first and last non-red pixel of the middle column
     * between 20 % and 70 % of the height, and of the row through their middle.
     */
    fun eshopInner(pixels: IntArray, width: Int, height: Int): Area? {
        if (width < 8 || height < 8 || pixels.size < width * height) return null
        val cx = width / 2
        val probe = pixels[(height * 22 / 100) * width + cx]
        if (!((probe shr 16 and 0xFF) > 150 && (probe shr 8 and 0xFF) < 40 && (probe and 0xFF) < 50)) {
            return null
        }
        var top = -1
        var bottom = -1
        for (y in height * 20 / 100 until height * 70 / 100) {
            if (!red(pixels[y * width + cx])) {
                if (top < 0) top = y
                bottom = y
            }
        }
        if (top < 0) return null
        val row = (top + bottom) / 2 * width
        var left = -1
        var right = -1
        for (x in 0 until width) {
            if (!red(pixels[row + x])) {
                if (left < 0) left = x
                right = x
            }
        }
        if (left < 0) return null
        // Same bounds as the mockup renderer: one pixel in from the first non-red edge.
        val area = Area(left + 1, top + 1, right - left - 1, bottom - top - 1)
        return area.takeIf { it.width >= 4 && it.height >= 4 }
    }

    /** The largest centred square inside [area]. */
    fun centreSquare(area: Area): Area {
        val side = minOf(area.width, area.height)
        return Area(area.left + (area.width - side) / 2, area.top + (area.height - side) / 2, side, side)
    }

    /** The square to cut from a cover: the eShop key art's centre, else the cover's centre. */
    fun tileSquare(pixels: IntArray, width: Int, height: Int): Area =
        centreSquare(eshopInner(pixels, width, height) ?: Area(0, 0, width, height))
}
