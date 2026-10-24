// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import android.view.KeyEvent
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ControllerKeysTest {
    @Test
    fun aAndXActOnSavesOnlyWhileTheCursorIsOnASave() {
        for (key in listOf(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_X)) {
            assertTrue(ownsControllerKey(key, focusOnSave = true))
            // Give access, toolbar Back: A must reach the button (Android turns it into a click).
            assertFalse(ownsControllerKey(key, focusOnSave = false))
        }
    }

    @Test
    fun shouldersAndBackWorkAnywhere() {
        for (key in listOf(KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_B)) {
            assertTrue(ownsControllerKey(key, focusOnSave = false))
        }
        assertFalse(ownsControllerKey(KeyEvent.KEYCODE_DPAD_LEFT, focusOnSave = true))
    }
}
