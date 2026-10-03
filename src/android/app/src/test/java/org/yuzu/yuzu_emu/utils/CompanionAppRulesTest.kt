// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CompanionAppRulesTest {
    @Test
    fun perGameFallsBackToGlobal() {
        assertEquals("a/.A", CompanionAppRules.effective(null, "a/.A"))
        assertEquals("b/.B", CompanionAppRules.effective("b/.B", "a/.A"))
        // An empty per-game value is "no app for this game", not "use global".
        assertNull(CompanionAppRules.effective("", "a/.A"))
        assertNull(CompanionAppRules.effective(null, ""))
        assertNull(CompanionAppRules.effective(null, null))
    }

    @Test
    fun pickerDropsOwnAppSortsAndDisambiguates() {
        val entries = listOf(
            CompanionAppRules.Entry("Spotify", "com.spotify/.Main", "com.spotify"),
            CompanionAppRules.Entry("Eden Duo", "dev.igawa6.edenduo/.Main", "dev.igawa6.edenduo"),
            CompanionAppRules.Entry("Browser", "org.b/.Main", "org.b"),
            CompanionAppRules.Entry("Browser", "com.a/.Main", "com.a"),
            CompanionAppRules.Entry("Spotify", "com.spotify/.Main", "com.spotify")
        )
        val picked = CompanionAppRules.forPicker(entries, "dev.igawa6.edenduo") { a, b ->
            a.compareTo(b, ignoreCase = true)
        }
        assertEquals(
            listOf("Browser (com.a)", "Browser (org.b)", "Spotify"),
            picked.map { it.label }
        )
        assertEquals("com.a/.Main", picked[0].component)
    }

    /** Swap Screens on: where the game goes, by companion present/absent/unknown x No Companion. */
    @Test
    fun swapStaysOnMainOnlyWhenNoCompanionIsCertainAndScreenWouldBeEmpty() {
        val r = ScreenSwapRules
        val modes = listOf(
            "Icon" to (r.ICON to false),
            "Black" to (r.BLACK to false),
            "Off" to (r.OFF to false),
            "App, no app" to (r.APP to false),
            "App, app picked" to (r.APP to true)
        )
        for ((name, mode) in modes) {
            val (value, picked) = mode
            // A companion, or not knowing (URI launch not in the game list, unreadable folder):
            // always swapped.
            assertFalse("$name, companion", r.keepsMainDisplay(value, picked) { true })
            assertFalse("$name, unknown", r.keepsMainDisplay(value, picked) { null })
            // No companion: stays on the main display only when that screen would be empty.
            val empty = name == "Off" || name == "App, no app"
            assertEquals("$name, no companion", empty, r.keepsMainDisplay(value, picked) { false })
        }
    }

    @Test
    fun companionIsLookedUpOnlyWhenTheScreenWouldBeEmpty() {
        var asked = 0
        val lookup: () -> Boolean? = { asked++; false }
        assertFalse(ScreenSwapRules.keepsMainDisplay(ScreenSwapRules.ICON, false, lookup))
        assertFalse(ScreenSwapRules.keepsMainDisplay(ScreenSwapRules.APP, true, lookup))
        assertEquals(0, asked)
        assertTrue(ScreenSwapRules.keepsMainDisplay(ScreenSwapRules.OFF, true, lookup))
        assertEquals(1, asked)
    }
}
