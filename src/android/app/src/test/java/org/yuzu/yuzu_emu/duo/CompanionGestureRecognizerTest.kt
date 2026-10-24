// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.yuzu.yuzu_emu.duo.CompanionGestureRecognizer.Action
import org.yuzu.yuzu_emu.duo.CompanionGestureRecognizer.Decision
import org.yuzu.yuzu_emu.duo.CompanionGestureRecognizer.Pointer

class CompanionGestureRecognizerTest {
    /** Drives a recognizer at 2 px per dp with pointers kept as a map, like a MotionEvent. */
    private class Driver(gesture: CompanionGesture) {
        val recognizer = CompanionGestureRecognizer(2f).also { it.gesture = gesture }
        val down = LinkedHashMap<Int, Pair<Float, Float>>()
        val decisions = ArrayList<Decision>()
        var time = 0L

        private fun pointers() = down.map { (id, p) -> Pointer(id, p.first, p.second) }

        private fun send(action: Action, id: Int): Decision =
            recognizer.onEvent(action, id, pointers(), time).also { decisions += it }

        fun press(id: Int, x: Float, y: Float): Decision {
            down[id] = x to y
            return send(if (down.size == 1) Action.DOWN else Action.POINTER_DOWN, id)
        }

        fun move(vararg moves: Triple<Int, Float, Float>): Decision {
            moves.forEach { (id, x, y) -> down[id] = x to y }
            return send(Action.MOVE, moves.first().first)
        }

        fun lift(id: Int): Decision {
            val action = if (down.size == 1) Action.UP else Action.POINTER_UP
            val decision = send(action, id)
            down.remove(id)
            return decision
        }

        val toggles get() = decisions.count { it.toggle }
    }

    private fun fourFingerTap(d: Driver, liftAt: Long) {
        for (i in 0 until 4) d.press(i, 100f + i * 150f, 300f)
        d.time = liftAt
        for (i in 0 until 4) d.lift(i)
    }

    @Test
    fun fourFingerTapToggles() {
        val d = Driver(CompanionGesture.FOUR_FINGER_TAP)
        fourFingerTap(d, liftAt = 200)
        assertEquals(1, d.toggles)
        assertTrue(d.decisions.last().toggle)
    }

    @Test
    fun slowFourFingerHoldDoesNotToggle() {
        val d = Driver(CompanionGesture.FOUR_FINGER_TAP)
        fourFingerTap(d, liftAt = 900)
        assertEquals(0, d.toggles)
    }

    @Test
    fun fourFingersThatMoveDoNotToggle() {
        val d = Driver(CompanionGesture.FOUR_FINGER_TAP)
        for (i in 0 until 4) d.press(i, 100f + i * 150f, 300f)
        d.move(Triple(0, 100f, 400f)) // 100 px = 50 dp, beyond the 24 dp slop
        d.time = 150
        for (i in 0 until 4) d.lift(i)
        assertEquals(0, d.toggles)
    }

    @Test
    fun fiveFingersAreNotAFourFingerTap() {
        val d = Driver(CompanionGesture.FOUR_FINGER_TAP)
        for (i in 0 until 5) d.press(i, 100f + i * 150f, 300f)
        d.time = 150
        for (i in 0 until 5) d.lift(i)
        assertEquals(0, d.toggles)
    }

    @Test
    fun twoFingerPinchIsForwardedAndNeverToggles() {
        for (gesture in CompanionGesture.values()) {
            val d = Driver(gesture)
            d.press(0, 100f, 300f)
            d.press(1, 700f, 300f)
            for (step in 1..10) {
                d.time = step * 20L
                d.move(Triple(0, 100f + step * 25f, 300f), Triple(1, 700f - step * 25f, 300f))
            }
            d.lift(0)
            d.lift(1)
            assertEquals(gesture.name, 0, d.toggles)
            assertTrue(gesture.name, d.decisions.all { it.forward && it.release.isEmpty() })
        }
    }

    @Test
    fun fourthFingerReleasesTheForwardedOnesAndSwallowsTheRest() {
        val d = Driver(CompanionGesture.FOUR_FINGER_TAP)
        assertTrue(d.press(0, 100f, 300f).forward)
        assertTrue(d.press(1, 250f, 300f).forward)
        assertTrue(d.press(2, 400f, 300f).forward)
        val fourth = d.press(3, 550f, 300f)
        assertFalse(fourth.forward)
        assertEquals(listOf(0, 1, 2), fourth.release)
        d.time = 100
        assertFalse(d.move(Triple(0, 101f, 300f)).forward)
        for (i in 0 until 4) assertFalse(d.lift(i).forward)
        // The next touch is a fresh sequence, forwarded again.
        d.time = 1000
        assertTrue(d.press(0, 100f, 300f).forward)
        assertTrue(d.lift(0).forward)
    }

    @Test
    fun liftingSomeFingersStillSwallowsUntilAllAreUp() {
        val d = Driver(CompanionGesture.FOUR_FINGER_TAP)
        for (i in 0 until 4) d.press(i, 100f + i * 150f, 300f)
        d.time = 1000
        d.lift(0)
        d.lift(1)
        d.lift(2)
        assertFalse(d.move(Triple(3, 600f, 400f)).forward)
        assertFalse(d.lift(3).forward)
        assertEquals(0, d.toggles)
    }

    @Test
    fun fiveFingerPinchToggles() {
        val d = Driver(CompanionGesture.FIVE_FINGER_PINCH)
        val start = listOf(400f to 100f, 600f to 250f, 550f to 500f, 250f to 500f, 200f to 250f)
        start.forEachIndexed { i, (x, y) -> d.press(i, x, y) }
        assertEquals(listOf(0, 1, 2, 3), d.decisions.last().release)
        // Every finger moves 40 % of the way to the centre (400, 320).
        for (step in 1..4) {
            d.time = step * 30L
            val f = 0.1f * step
            d.move(*start.mapIndexed { i, (x, y) ->
                Triple(i, x + (400f - x) * f, y + (320f - y) * f)
            }.toTypedArray())
        }
        assertEquals(1, d.toggles)
        for (i in 0 until 5) assertFalse(d.lift(i).forward)
        assertEquals(1, d.toggles)
    }

    @Test
    fun fiveFingersHeldStillDoNotPinch() {
        val d = Driver(CompanionGesture.FIVE_FINGER_PINCH)
        for (i in 0 until 5) d.press(i, 100f + i * 100f, 300f)
        d.time = 100
        d.move(Triple(0, 110f, 300f))
        for (i in 0 until 5) d.lift(i)
        assertEquals(0, d.toggles)
    }

    @Test
    fun fourFingersAreForwardedWhenThePinchIsChosen() {
        val d = Driver(CompanionGesture.FIVE_FINGER_PINCH)
        for (i in 0 until 4) assertTrue(d.press(i, 100f + i * 100f, 300f).forward)
    }

    @Test
    fun threeFingerSwipeDownToggles() {
        val d = Driver(CompanionGesture.THREE_FINGER_SWIPE_DOWN)
        assertTrue(d.press(0, 100f, 100f).forward)
        assertTrue(d.press(1, 250f, 100f).forward)
        val third = d.press(2, 400f, 100f)
        assertFalse(third.forward)
        assertEquals(listOf(0, 1), third.release)
        for (step in 1..6) {
            d.time = step * 30L
            val y = 100f + step * 45f // 270 px = 135 dp
            d.move(Triple(0, 100f + step, y), Triple(1, 250f, y), Triple(2, 400f - step, y))
        }
        assertEquals(1, d.toggles)
    }

    @Test
    fun sidewaysThreeFingerSwipeDoesNotToggle() {
        val d = Driver(CompanionGesture.THREE_FINGER_SWIPE_DOWN)
        for (i in 0 until 3) d.press(i, 100f + i * 150f, 100f)
        d.move(*(0 until 3).map { Triple(it, 500f + it * 150f, 350f) }.toTypedArray())
        assertEquals(0, d.toggles)
    }

    @Test
    fun threeFingerSwipeUpDoesNotToggle() {
        val d = Driver(CompanionGesture.THREE_FINGER_SWIPE_DOWN)
        for (i in 0 until 3) d.press(i, 100f + i * 150f, 600f)
        d.move(*(0 until 3).map { Triple(it, 100f + it * 150f, 100f) }.toTypedArray())
        assertEquals(0, d.toggles)
    }

    @Test
    fun offForwardsEverything() {
        val d = Driver(CompanionGesture.OFF)
        fourFingerTap(d, liftAt = 100)
        assertEquals(0, d.toggles)
        assertTrue(d.decisions.all { it.forward && it.release.isEmpty() })
    }

    @Test
    fun cancelEndsTheSequence() {
        val d = Driver(CompanionGesture.FOUR_FINGER_TAP)
        for (i in 0 until 4) d.press(i, 100f + i * 150f, 300f)
        assertFalse(d.recognizer.onEvent(Action.CANCEL, 0, emptyList(), 50).forward)
        assertFalse(d.recognizer.isSwallowing)
        assertTrue(d.recognizer.onEvent(Action.DOWN, 0, listOf(Pointer(0, 1f, 1f)), 60).forward)
    }

    @Test
    fun gestureFromValueFallsBackToTheDefault() {
        assertEquals(CompanionGesture.THREE_FINGER_SWIPE_DOWN, CompanionGesture.fromValue(2))
        assertEquals(CompanionGesture.FOUR_FINGER_TAP, CompanionGesture.fromValue(42))
    }
}
