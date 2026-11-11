// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.view.MotionEvent
import org.yuzu.yuzu_emu.features.input.NativeInput

/**
 * Eden Duo Stage 2: routes touches that land inside the Simulated companion pane to the existing
 * companion touch path ([NativeInput.onAuxTouchEvent]), in pane-local pixel coordinates -- the
 * same convention `AuxScreenHost.onAuxTouch` already uses for Physical (this class mirrors its
 * per-pointer-index/startMask construction exactly), since the native side treats both
 * identically once routed: `AuxRouting::width`/`height` is the companion pane's own pixel size
 * while Simulated is active (`vk_dsmod_aux.cpp` `RenderInline`), not a real aux window's.
 *
 * A pointer stays with whichever pane it went down in (RESEARCH.md I2): once claimed here on its
 * own DOWN, it is tracked until released, even past the pane's edge, up to the point it actually
 * leaves the pane -- which this class treats as a lift, clamping is left to the native side for
 * whatever briefly lands exactly on the edge. [InputOverlay] excludes an owned pointer from its
 * own button/touchscreen dispatch via [isOwned], so the two paths never both claim one.
 */
class CompanionPaneTouch(private val paneRect: () -> DualScreenRect?) {
    private companion object {
        const val MAX_TOUCH = 16
    }

    private val ownedIds = HashSet<Int>()
    private val ids = IntArray(MAX_TOUCH)
    private val xs = FloatArray(MAX_TOUCH)
    private val ys = FloatArray(MAX_TOUCH)

    /** True while [pointerId] belongs to the companion pane (claimed on its own DOWN). */
    fun isOwned(pointerId: Int): Boolean = pointerId in ownedIds

    /** Feeds [event] (the full, unfiltered overlay touch event); updates native companion touch. */
    fun onEvent(event: MotionEvent) {
        val action = event.actionMasked
        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> Unit

            else -> return
        }
        val pane = paneRect()
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            val index = event.actionIndex
            val id = event.getPointerId(index)
            if (pane != null && pane.contains(event.getX(index).toInt(), event.getY(index).toInt())) {
                ownedIds.add(id)
            }
        }
        if (ownedIds.isEmpty()) {
            return // Nothing of ours in play; the normal game/overlay path handles everything.
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            ownedIds.clear()
            NativeInput.onAuxTouchEvent(ids, xs, ys, 0, 0)
            return
        }
        if (action == MotionEvent.ACTION_POINTER_UP) {
            ownedIds.remove(event.getPointerId(event.actionIndex))
        }
        if (pane == null) {
            // The pane just disappeared (collapsed by the toggle, or too small) mid-touch.
            ownedIds.clear()
            NativeInput.onAuxTouchEvent(ids, xs, ys, 0, 0)
            return
        }
        if (action == MotionEvent.ACTION_MOVE) {
            // A moving owned pointer that leaves the pane is treated as a lift (RESEARCH.md I2).
            val leaving = ownedIds.filterNot { id ->
                val i = event.findPointerIndex(id)
                i >= 0 && pane.contains(event.getX(i).toInt(), event.getY(i).toInt())
            }
            ownedIds.removeAll(leaving.toSet())
        }
        if (ownedIds.isEmpty()) {
            NativeInput.onAuxTouchEvent(ids, xs, ys, 0, 0)
            return
        }
        var count = 0
        var startMask = 0
        for (i in 0 until event.pointerCount) {
            val id = event.getPointerId(i)
            if (id !in ownedIds) continue
            if (count == MAX_TOUCH) break
            ids[count] = id
            xs[count] = (event.getX(i) - pane.x).coerceIn(0f, (pane.w - 1).coerceAtLeast(0).toFloat())
            ys[count] = (event.getY(i) - pane.y).coerceIn(0f, (pane.h - 1).coerceAtLeast(0).toFloat())
            if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) &&
                i == event.actionIndex
            ) {
                startMask = startMask or (1 shl count)
            }
            count++
        }
        NativeInput.onAuxTouchEvent(ids, xs, ys, count, startMask)
    }

    /** The pane disappeared without a touch event to notice it (mode switch, program change). */
    fun clear() {
        if (ownedIds.isEmpty()) {
            return
        }
        ownedIds.clear()
        NativeInput.onAuxTouchEvent(ids, xs, ys, 0, 0)
    }
}
