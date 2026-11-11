// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DisplayHotplugRulesTest {

    // ---- classifyRemoval ----

    @Test
    fun classifyRemovalMatchesCompanionFirst() {
        assertEquals(
            DisplayHotplugRules.RemovedDisplayKind.COMPANION,
            DisplayHotplugRules.classifyRemoval(5, companionDisplayId = 5, gameDisplayId = 5)
        )
    }

    @Test
    fun classifyRemovalMatchesGameWhenNotTheCompanion() {
        assertEquals(
            DisplayHotplugRules.RemovedDisplayKind.GAME,
            DisplayHotplugRules.classifyRemoval(7, companionDisplayId = 5, gameDisplayId = 7)
        )
    }

    @Test
    fun classifyRemovalIsUnrelatedWhenNeitherMatches() {
        assertEquals(
            DisplayHotplugRules.RemovedDisplayKind.UNRELATED,
            DisplayHotplugRules.classifyRemoval(9, companionDisplayId = 5, gameDisplayId = 7)
        )
        assertEquals(
            DisplayHotplugRules.RemovedDisplayKind.UNRELATED,
            DisplayHotplugRules.classifyRemoval(9, companionDisplayId = null, gameDisplayId = null)
        )
    }

    // ---- isEject ----

    @Test
    fun unrelatedDisplayNeverEjectsRegardlessOfMode() {
        for (mode in DualScreenMode.entries) {
            assertFalse(
                "mode=$mode",
                DisplayHotplugRules.isEject(DisplayHotplugRules.RemovedDisplayKind.UNRELATED, mode)
            )
        }
    }

    @Test
    fun gameDisplayAlwaysEjectsRegardlessOfMode() {
        for (mode in DualScreenMode.entries) {
            assertTrue(
                "mode=$mode",
                DisplayHotplugRules.isEject(DisplayHotplugRules.RemovedDisplayKind.GAME, mode)
            )
        }
    }

    @Test
    fun companionDisplayEjectsOnlyWhilePhysicalWasActive() {
        assertTrue(
            DisplayHotplugRules.isEject(
                DisplayHotplugRules.RemovedDisplayKind.COMPANION,
                DualScreenMode.PHYSICAL
            )
        )
        assertFalse(
            DisplayHotplugRules.isEject(
                DisplayHotplugRules.RemovedDisplayKind.COMPANION,
                DualScreenMode.SIMULATED
            )
        )
        assertFalse(
            DisplayHotplugRules.isEject(
                DisplayHotplugRules.RemovedDisplayKind.COMPANION,
                DualScreenMode.OFF
            )
        )
    }

    // ---- sessionOverrideFor ----

    @Test
    fun continueOnThisScreenOverridesToSimulated() {
        assertEquals(
            DualScreenMode.SIMULATED,
            DisplayHotplugRules.sessionOverrideFor(DisplayHotplugRules.EjectChoice.CONTINUE_SIMULATED)
        )
    }

    @Test
    fun continueGameOnlyAndWaitForReconnectLeaveModeUnoverridden() {
        assertNull(
            DisplayHotplugRules.sessionOverrideFor(DisplayHotplugRules.EjectChoice.CONTINUE_GAME_ONLY)
        )
        assertNull(
            DisplayHotplugRules.sessionOverrideFor(DisplayHotplugRules.EjectChoice.WAIT_FOR_RECONNECT)
        )
    }

    // ---- onDisplayAdded ----

    @Test
    fun unsetRequestedSwitchesLiveToPhysicalOnceADisplayAppears() {
        assertEquals(
            DisplayHotplugRules.DisplayAddedOutcome.SWITCH_TO_PHYSICAL,
            DisplayHotplugRules.onDisplayAdded(
                requested = null,
                modeBeforeEvent = DualScreenMode.SIMULATED,
                hasSecondScreenNow = true
            )
        )
    }

    @Test
    fun explicitPhysicalRequestSwitchesLiveOnceADisplayAppears() {
        assertEquals(
            DisplayHotplugRules.DisplayAddedOutcome.SWITCH_TO_PHYSICAL,
            DisplayHotplugRules.onDisplayAdded(
                requested = DualScreenMode.PHYSICAL,
                modeBeforeEvent = DualScreenMode.SIMULATED,
                hasSecondScreenNow = true
            )
        )
    }

    @Test
    fun explicitSimulatedRequestStaysAndShowsTheOneTimeHint() {
        assertEquals(
            DisplayHotplugRules.DisplayAddedOutcome.SHOW_SIMULATED_HINT,
            DisplayHotplugRules.onDisplayAdded(
                requested = DualScreenMode.SIMULATED,
                modeBeforeEvent = DualScreenMode.SIMULATED,
                hasSecondScreenNow = true
            )
        )
    }

    @Test
    fun offRequestIsUntouchedByADisplayAppearing() {
        assertEquals(
            DisplayHotplugRules.DisplayAddedOutcome.NONE,
            DisplayHotplugRules.onDisplayAdded(
                requested = DualScreenMode.OFF,
                modeBeforeEvent = DualScreenMode.OFF,
                hasSecondScreenNow = true
            )
        )
    }

    @Test
    fun alreadyPhysicalIsANoOpWhenAnotherDisplayAppears() {
        assertEquals(
            DisplayHotplugRules.DisplayAddedOutcome.NONE,
            DisplayHotplugRules.onDisplayAdded(
                requested = null,
                modeBeforeEvent = DualScreenMode.PHYSICAL,
                hasSecondScreenNow = true
            )
        )
    }
}
