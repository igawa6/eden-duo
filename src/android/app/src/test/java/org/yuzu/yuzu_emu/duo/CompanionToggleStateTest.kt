// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CompanionToggleStateTest {
    private val shown = CompanionWindow.SHOWN_WHEN_WANTED

    @Test
    fun companionOrIdlePageHidesThenShows() {
        val state = CompanionToggleState()
        assertFalse(state.hiddenByUser)
        assertEquals(CompanionToggleOutcome.HIDDEN, state.toggle(1000, shown, true))
        assertTrue(state.hiddenByUser)
        assertEquals(CompanionToggleOutcome.SHOWN, state.toggle(2000, shown, true))
        assertFalse(state.hiddenByUser)
    }

    @Test
    fun noCompanionWithOffOrAppChangesNothing() {
        val state = CompanionToggleState()
        assertEquals(
            CompanionToggleOutcome.NO_COMPANION,
            state.toggle(1000, CompanionWindow.NONE_NO_COMPANION, true)
        )
        assertFalse(state.hiddenByUser)
    }

    @Test
    fun companionTurnedOffInAddonsIsNamed() {
        val state = CompanionToggleState()
        assertEquals(
            CompanionToggleOutcome.ADDON_OFF,
            state.toggle(1000, CompanionWindow.NONE_ADDON_OFF, true)
        )
        assertFalse(state.hiddenByUser)
    }

    @Test
    fun noSecondScreenKeepsTheState() {
        val state = CompanionToggleState()
        assertEquals(CompanionToggleOutcome.NO_SECOND_SCREEN, state.toggle(1000, shown, false))
        assertFalse(state.hiddenByUser)
        state.toggle(2000, shown, true)
        assertTrue(state.hiddenByUser)
        // The lid closed while hidden: showing is refused and it stays hidden.
        assertEquals(CompanionToggleOutcome.NO_SECOND_SCREEN, state.toggle(3000, shown, false))
        assertTrue(state.hiddenByUser)
    }

    @Test
    fun noAutoShowWhileHidden() {
        val state = CompanionToggleState()
        assertTrue(state.allowsAutoShow())
        state.toggle(1000, shown, true)
        assertFalse(state.allowsAutoShow())
        // Refused toggles do not bring it back either.
        state.toggle(2000, shown, false)
        state.toggle(3000, CompanionWindow.NONE_NO_COMPANION, true)
        assertFalse(state.allowsAutoShow())
        state.toggle(4000, shown, true)
        assertTrue(state.allowsAutoShow())
    }

    @Test
    fun togglesWithinTheDebounceTimeAreIgnored() {
        val state = CompanionToggleState()
        assertEquals(CompanionToggleOutcome.HIDDEN, state.toggle(1000, shown, true))
        assertEquals(CompanionToggleOutcome.IGNORED, state.toggle(1200, shown, true))
        assertEquals(CompanionToggleOutcome.IGNORED, state.toggle(1499, shown, true))
        assertTrue(state.hiddenByUser)
        assertEquals(CompanionToggleOutcome.SHOWN, state.toggle(1500, shown, true))
    }

    @Test
    fun theFirstToggleIsNeverDebounced() {
        assertEquals(CompanionToggleOutcome.HIDDEN, CompanionToggleState().toggle(0, shown, true))
    }
}
