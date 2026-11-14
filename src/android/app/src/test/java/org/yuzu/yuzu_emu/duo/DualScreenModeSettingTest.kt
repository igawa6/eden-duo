// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** How a per-game Mode choice combines with the global one (DualScreenModeSetting). */
class DualScreenModeSettingTest {
    private val unset = DualScreenModeSetting.UNSET

    @Test
    fun noPerGameValueInheritsTheGlobalChoice() {
        assertEquals(DualScreenMode.OFF, DualScreenModeSetting.requestedFrom(null, DualScreenMode.OFF))
        assertNull(DualScreenModeSetting.requestedFrom(null, null))
    }

    @Test
    fun perGameAutomaticWinsOverAGlobalOffOrSimulated() {
        // Picking Automatic for one game must mean Automatic for it, not "whatever global is".
        assertNull(DualScreenModeSetting.requestedFrom(unset, DualScreenMode.OFF))
        assertNull(DualScreenModeSetting.requestedFrom(unset, DualScreenMode.SIMULATED))
        assertEquals(
            DualScreenMode.PHYSICAL,
            DualScreenModeResolver.resolveActive(
                DualScreenModeSetting.requestedFrom(unset, DualScreenMode.SIMULATED),
                secondScreenDetected = true,
                companionAvailable = true
            )
        )
    }

    @Test
    fun perGameExplicitModeWinsOverTheGlobalChoice() {
        assertEquals(
            DualScreenMode.SIMULATED,
            DualScreenModeSetting.requestedFrom(DualScreenMode.SIMULATED.value, DualScreenMode.OFF)
        )
    }

    @Test
    fun aPerGameAutomaticPickIsStoredExplicitly() {
        assertEquals(unset, DualScreenModeSetting.perGameStoredValue(unset))
        assertEquals(DualScreenMode.OFF.value, DualScreenModeSetting.perGameStoredValue(DualScreenMode.OFF.value))
    }
}
