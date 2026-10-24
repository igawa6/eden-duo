// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import kotlin.math.abs
import kotlin.math.hypot

/**
 * Eden Duo: the "Companion toggle gesture" on the game screen (Settings -> Graphics -> Second
 * Screen). [value] is the stored setting value, [fingers] the pointer count from which a touch
 * sequence belongs to the gesture and is kept away from the emulated touchscreen.
 */
enum class CompanionGesture(val value: Int, val fingers: Int) {
    FOUR_FINGER_TAP(0, 4),
    FIVE_FINGER_PINCH(1, 5),
    THREE_FINGER_SWIPE_DOWN(2, 3),
    OFF(3, Int.MAX_VALUE);

    companion object {
        fun fromValue(value: Int): CompanionGesture =
            values().firstOrNull { it.value == value } ?: FOUR_FINGER_TAP
    }
}

/**
 * The Android-free recognizer of [CompanionGesture] (unit tested). The game screen feeds it every
 * touch event as (action, pointer id, all pointers down, time) and follows each [Decision]:
 *
 *  - Fewer fingers than the gesture needs: every event goes to the emulated touchscreen as usual,
 *    so a game played with one or two fingers never notices the recognizer.
 *  - The moment the gesture's finger count is down, the pointers already passed on are released
 *    ([Decision.release]) and every event is kept back until the last finger is up.
 *  - [Decision.toggle] is true once per sequence, when the gesture completed.
 *
 * [pxPerDp] converts the dp thresholds to the event coordinates.
 */
class CompanionGestureRecognizer(private val pxPerDp: Float) {
    companion object {
        /** A tap: every finger down and up again within this time... */
        const val TAP_TIMEOUT_MS = 300L

        /** ...and no finger moved further than this from where it landed. */
        const val TAP_SLOP_DP = 24f

        /** A pinch: the fingers' spread shrank by this fraction since the fifth one landed. */
        const val PINCH_SHRINK = 0.35f

        /** A swipe down: every finger moved down this far since the third one landed... */
        const val SWIPE_DISTANCE_DP = 120f

        /** ...and sideways by at most this fraction of that. */
        const val SWIPE_MAX_SIDEWAYS = 0.5f
    }

    enum class Action { DOWN, POINTER_DOWN, MOVE, POINTER_UP, UP, CANCEL }

    class Pointer(val id: Int, val x: Float, val y: Float)

    /**
     * [forward]: pass this event to the emulated touchscreen. [release]: pointer ids to release
     * there first (they were passed on before the gesture took over). [toggle]: the gesture
     * completed with this event.
     */
    class Decision(
        val forward: Boolean,
        val release: List<Int> = emptyList(),
        val toggle: Boolean = false
    )

    private class Track(val downX: Float, val downY: Float) {
        var x = downX
        var y = downY
        var refX = downX
        var refY = downY
    }

    var gesture = CompanionGesture.FOUR_FINGER_TAP

    private val down = LinkedHashMap<Int, Track>()
    private val forwarded = LinkedHashSet<Int>()
    private var swallowing = false
    private var sequenceStart = 0L
    private var maxDown = 0
    private var moved = false
    private var fired = false
    private var refSpread = 0f

    /** True while the current sequence is kept away from the emulated touchscreen. */
    val isSwallowing: Boolean get() = swallowing

    fun onEvent(action: Action, pointerId: Int, pointers: List<Pointer>, timeMs: Long): Decision {
        if (gesture == CompanionGesture.OFF && !swallowing) {
            reset()
            return Decision(forward = true)
        }
        return when (action) {
            Action.DOWN -> {
                reset()
                sequenceStart = timeMs
                land(pointerId, pointers)
            }

            Action.POINTER_DOWN -> land(pointerId, pointers)

            Action.MOVE -> {
                follow(pointers)
                Decision(forward = !swallowing, toggle = completesOnMove())
            }

            Action.POINTER_UP -> {
                follow(pointers)
                down.remove(pointerId)
                forwarded.remove(pointerId)
                Decision(forward = !swallowing)
            }

            Action.UP -> {
                follow(pointers)
                val tap = completesTap(timeMs)
                val decision = Decision(forward = !swallowing, toggle = tap)
                reset()
                decision
            }

            Action.CANCEL -> {
                val decision = Decision(forward = !swallowing)
                reset()
                decision
            }
        }
    }

    private fun reset() {
        down.clear()
        forwarded.clear()
        swallowing = false
        maxDown = 0
        moved = false
        fired = false
        refSpread = 0f
    }

    private fun land(pointerId: Int, pointers: List<Pointer>): Decision {
        val p = pointers.firstOrNull { it.id == pointerId }
        down[pointerId] = Track(p?.x ?: 0f, p?.y ?: 0f)
        follow(pointers)
        maxDown = maxOf(maxDown, down.size)
        // More fingers than the gesture needs: still kept back, but it no longer completes.
        if (swallowing) return Decision(forward = false)
        if (down.size < gesture.fingers) {
            forwarded.add(pointerId)
            return Decision(forward = true)
        }
        swallowing = true
        val release = forwarded.toList()
        forwarded.clear()
        down.values.forEach { it.refX = it.x; it.refY = it.y }
        refSpread = spread()
        return Decision(forward = false, release = release)
    }

    private fun follow(pointers: List<Pointer>) {
        val slop = TAP_SLOP_DP * pxPerDp
        for (p in pointers) {
            val track = down[p.id] ?: continue
            track.x = p.x
            track.y = p.y
            if (hypot(p.x - track.downX, p.y - track.downY) > slop) moved = true
        }
    }

    /** True while exactly the gesture's fingers are down and none came on top of them. */
    private fun exactFingers(): Boolean =
        swallowing && !fired && maxDown == gesture.fingers && down.size == gesture.fingers

    private fun completesOnMove(): Boolean {
        if (!exactFingers()) return false
        val done = when (gesture) {
            CompanionGesture.FIVE_FINGER_PINCH ->
                refSpread > 0f && spread() <= refSpread * (1f - PINCH_SHRINK)

            CompanionGesture.THREE_FINGER_SWIPE_DOWN -> {
                val distance = SWIPE_DISTANCE_DP * pxPerDp
                down.values.all {
                    val dy = it.y - it.refY
                    dy >= distance && abs(it.x - it.refX) <= dy * SWIPE_MAX_SIDEWAYS
                }
            }

            else -> false
        }
        if (done) fired = true
        return done
    }

    private fun completesTap(timeMs: Long): Boolean =
        gesture == CompanionGesture.FOUR_FINGER_TAP && swallowing && !fired &&
            maxDown == gesture.fingers && !moved && timeMs - sequenceStart <= TAP_TIMEOUT_MS

    /** Mean distance of the fingers down from their centre. */
    private fun spread(): Float {
        if (down.isEmpty()) return 0f
        val cx = down.values.sumOf { it.x.toDouble() } / down.size
        val cy = down.values.sumOf { it.y.toDouble() } / down.size
        return (down.values.sumOf { hypot(it.x - cx, it.y - cy) } / down.size).toFloat()
    }
}
