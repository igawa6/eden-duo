// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.yuzu.yuzu_emu.duo.SecondScreenBrightness.Device
import org.yuzu.yuzu_emu.duo.SecondScreenBrightness.Outcome

class SecondScreenBrightnessTest {
    private val plainPhone =
        Device(null, aynAssistant = false, manufacturer = "AYANEO", brand = "AYANEO")

    // The owner's Thor: its firmware keeps the second-screen level in Settings.System (#8).
    private val thorWithLevel =
        Device(128, aynAssistant = true, manufacturer = "AYN", brand = "AYN")

    // Thor batch 7: no such key, the assistant drives per-display brightness (eden-duo #18).
    private val thorBatch7 = Device(null, aynAssistant = true, manufacturer = "AYN", brand = "AYN")

    @Test
    fun autoFollowsWhereNothingLightsThePanel() {
        val d = SecondScreenBrightness.decide(SecondScreenBrightness.AUTO) { plainPhone }
        assertEquals(Outcome.FOLLOW, d.outcome)
        assertEquals("following main", d.toString())
    }

    @Test
    fun autoLeavesTheDualScreenLevelKeyAlone() {
        val d = SecondScreenBrightness.decide(SecondScreenBrightness.AUTO) { thorWithLevel }
        assertEquals(Outcome.DEVICE_LIT, d.outcome)
        assertEquals("device-lit (dual_screen_brightness_level 128)", d.toString())
    }

    @Test
    fun autoLeavesBatch7ThorAloneWithoutTheKey() {
        val d = SecondScreenBrightness.decide(SecondScreenBrightness.AUTO) { thorBatch7 }
        assertFalse(d.follows)
        assertEquals("device-lit (com.odin.dualscreen.assistant installed)", d.toString())
    }

    @Test
    fun aynMakerOrBrandAloneCountsAsDeviceLit() {
        assertEquals(
            "manufacturer AYN",
            SecondScreenBrightness.deviceLit(Device(null, false, "ayn", "Qualcomm"))
        )
        assertEquals(
            "brand AYN",
            SecondScreenBrightness.deviceLit(Device(null, false, "Qualcomm", "Ayn"))
        )
    }

    @Test
    fun onFollowsEvenOnDeviceLitHardwareWithoutProbing() {
        val d = SecondScreenBrightness.decide(SecondScreenBrightness.ON) {
            throw AssertionError("device probed under On")
        }
        assertTrue(d.follows)
    }

    @Test
    fun offNeverFollowsWithoutProbing() {
        val d = SecondScreenBrightness.decide(SecondScreenBrightness.OFF) {
            throw AssertionError("device probed under Off")
        }
        assertEquals(Outcome.OFF, d.outcome)
        assertEquals("off by setting", d.toString())
    }
}
