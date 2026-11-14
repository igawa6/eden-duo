// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Test

class DualScreenModeResolverTest {
    @Test
    fun noExplicitChoiceFollowsSecondScreenDetection() {
        assertEquals(
            DualScreenMode.PHYSICAL,
            DualScreenModeResolver.resolveActive(
                requested = null,
                secondScreenDetected = true,
                companionAvailable = true
            )
        )
        assertEquals(
            DualScreenMode.SIMULATED,
            DualScreenModeResolver.resolveActive(
                requested = null,
                secondScreenDetected = false,
                companionAvailable = true
            )
        )
    }

    @Test
    fun noExplicitChoiceIgnoresCompanionAvailability() {
        // The default rule makes no exception for a companion-less title.
        assertEquals(
            DualScreenMode.PHYSICAL,
            DualScreenModeResolver.resolveActive(
                requested = null,
                secondScreenDetected = true,
                companionAvailable = false
            )
        )
        assertEquals(
            DualScreenMode.SIMULATED,
            DualScreenModeResolver.resolveActive(
                requested = null,
                secondScreenDetected = false,
                companionAvailable = false
            )
        )
    }

    @Test
    fun requestedPhysicalFallsBackToSimulatedWithoutASecondScreen() {
        assertEquals(
            DualScreenMode.SIMULATED,
            DualScreenModeResolver.resolveActive(
                requested = DualScreenMode.PHYSICAL,
                secondScreenDetected = false,
                companionAvailable = true
            )
        )
    }

    @Test
    fun requestedPhysicalStaysPhysicalWithASecondScreen() {
        assertEquals(
            DualScreenMode.PHYSICAL,
            DualScreenModeResolver.resolveActive(
                requested = DualScreenMode.PHYSICAL,
                secondScreenDetected = true,
                companionAvailable = true
            )
        )
    }

    @Test
    fun requestedSimulatedStaysSimulatedEvenWithASecondScreen() {
        // An explicit choice is kept: Physical being available does not pull
        // an explicit Simulated choice back to Physical.
        assertEquals(
            DualScreenMode.SIMULATED,
            DualScreenModeResolver.resolveActive(
                requested = DualScreenMode.SIMULATED,
                secondScreenDetected = true,
                companionAvailable = true
            )
        )
    }

    @Test
    fun requestedOffStaysOffRegardlessOfEverythingElse() {
        for (secondScreen in listOf(true, false)) {
            for (companion in listOf(true, false)) {
                assertEquals(
                    DualScreenMode.OFF,
                    DualScreenModeResolver.resolveActive(
                        requested = DualScreenMode.OFF,
                        secondScreenDetected = secondScreen,
                        companionAvailable = companion
                    )
                )
            }
        }
    }
}
