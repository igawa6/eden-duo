// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.view.MotionEvent
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.features.settings.model.BooleanSetting

/** Which Simulated pane a touch going down on emulation_container belongs to. */
enum class PaneTouchTarget { GAME, COMPANION, NONE }

/**
 * Where emulation_container and its SurfaceView sit while the 3-band layout is applied: [layout]
 * in the coordinates of the full area it was computed for, the container's top-left corner at
 * ([originX], [originY]) of that area, and the surface at ([surfaceLeft], [surfaceTop]) inside the
 * container, [surfaceWidth] x [surfaceHeight] pixels. Pure, so the routing is unit-tested.
 */
data class PaneTouchFrame(
    val layout: DualScreenLayout,
    val originX: Int,
    val originY: Int,
    val surfaceLeft: Int,
    val surfaceTop: Int,
    val surfaceWidth: Int,
    val surfaceHeight: Int
) {
    /** The pane under container point ([x], [y]); the companion is tested first, exactly as
     *  [CompanionPaneTouch] claims it. */
    fun targetAt(x: Float, y: Float): PaneTouchTarget {
        val px = x.toInt() + originX
        val py = y.toInt() + originY
        return when {
            layout.companionRect.contains(px, py) -> PaneTouchTarget.COMPANION
            layout.gameRect.contains(px, py) -> PaneTouchTarget.GAME
            else -> PaneTouchTarget.NONE
        }
    }

    /**
     * Container x in the surface's own pixels, the space InputOverlay's touches are in when it
     * covers the whole window and the space EmuWindow_Android maps into the fitted game picture.
     * Clamped to the surface: a finger that slides into the controls band stays on its edge.
     */
    fun toSurfaceX(x: Float): Float = (x - surfaceLeft).coerceIn(0f, (surfaceWidth - 1).coerceAtLeast(0).toFloat())

    /** Container y in the surface's own pixels; see [toSurfaceX]. */
    fun toSurfaceY(y: Float): Float = (y - surfaceTop).coerceIn(0f, (surfaceHeight - 1).coerceAtLeast(0).toFloat())
}

/**
 * Eden Duo: the emulated touchscreen for touches on the Simulated game pane that InputOverlay
 * never sees. In the 3-band layout (portrait, no controller) input_container, and the overlay in
 * it, is shrunk to the controls band below both panes, so the game pane above it is only covered
 * by emulation_container. Fed by that container's touch listener, this sends what InputOverlay
 * sends in the other layouts: per-pointer press/move/release under the pointer's own id, in the
 * surface's pixels, through the same companion toggle gesture filter ([gestureFilter]).
 *
 * A pointer is claimed on its own DOWN when it lands on the game pane and [isOwnedElsewhere]
 * (the companion pane's own claim) does not hold it, then followed until it lifts, as InputOverlay
 * does. Does nothing while [frame] is null, i.e. outside the 3-band layout.
 */
class GamePaneTouch(
    private val frame: () -> PaneTouchFrame?,
    private val isOwnedElsewhere: (Int) -> Boolean,
    private val gestureFilter: (MotionEvent) -> Boolean
) {
    private val ownedIds = HashSet<Int>()

    /** True while [pointerId] belongs to the game pane (claimed on its own DOWN). */
    fun isOwned(pointerId: Int): Boolean = pointerId in ownedIds

    fun onEvent(event: MotionEvent) {
        val action = event.actionMasked
        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> Unit

            else -> return
        }
        val frame = frame()
        if (frame == null) {
            clear() // The controls band went away mid-touch; the overlay covers the game again.
            return
        }
        val forward = gestureFilter(event) && BooleanSetting.TOUCHSCREEN.getBoolean()
        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val index = event.actionIndex
                val id = event.getPointerId(index)
                val x = event.getX(index)
                val y = event.getY(index)
                if (isOwnedElsewhere(id) || frame.targetAt(x, y) != PaneTouchTarget.GAME) return
                ownedIds.add(id)
                if (forward) NativeInput.onTouchPressed(id, frame.toSurfaceX(x), frame.toSurfaceY(y))
            }

            MotionEvent.ACTION_MOVE -> if (forward) {
                for (i in 0 until event.pointerCount) {
                    val id = event.getPointerId(i)
                    if (id !in ownedIds) continue
                    NativeInput.onTouchMoved(id, frame.toSurfaceX(event.getX(i)), frame.toSurfaceY(event.getY(i)))
                }
            }

            // Released even when the filter held the event back: harmless for a pointer the
            // touchscreen never got, and nothing is left pressed.
            MotionEvent.ACTION_POINTER_UP -> {
                val id = event.getPointerId(event.actionIndex)
                if (ownedIds.remove(id)) NativeInput.onTouchReleased(id)
            }

            else -> clear() // ACTION_UP, ACTION_CANCEL
        }
    }

    /** Releases every claimed pointer (gesture end, layout change, view teardown). */
    fun clear() {
        ownedIds.forEach { NativeInput.onTouchReleased(it) }
        ownedIds.clear()
    }
}
