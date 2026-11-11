// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.view.Display
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SecondScreenDetectionTest {
    @Test
    fun realPanelWithPresentationAndNotPrivateIsAccepted() {
        assertTrue(
            SecondScreenDetection.isAcceptableCandidate(
                flags = Display.FLAG_PRESENTATION,
                isSimulatedName = false,
                countSimulated = false
            )
        )
    }

    @Test
    fun realNameWithoutPresentationFlagIsRejected() {
        // E6: a plain union of getDisplays() used to admit anything with a neutral name.
        assertFalse(
            SecondScreenDetection.isAcceptableCandidate(
                flags = 0,
                isSimulatedName = false,
                countSimulated = false
            )
        )
    }

    @Test
    fun publicVirtualDisplayWithPresentationButPrivateIsRejected() {
        assertFalse(
            SecondScreenDetection.isAcceptableCandidate(
                flags = Display.FLAG_PRESENTATION or Display.FLAG_PRIVATE,
                isSimulatedName = false,
                countSimulated = false
            )
        )
    }

    @Test
    fun simulatedNamedDisplayIgnoresFlagsAndFollowsTheDeveloperSetting() {
        // The overlay/Cuttlefish display may or may not carry FLAG_PRESENTATION; only the
        // developer setting decides whether it counts.
        assertFalse(
            SecondScreenDetection.isAcceptableCandidate(
                flags = Display.FLAG_PRESENTATION,
                isSimulatedName = true,
                countSimulated = false
            )
        )
        assertTrue(
            SecondScreenDetection.isAcceptableCandidate(
                flags = 0,
                isSimulatedName = true,
                countSimulated = true
            )
        )
    }
}
