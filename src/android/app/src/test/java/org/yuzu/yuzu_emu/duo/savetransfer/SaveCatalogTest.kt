// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SaveCatalogTest {
    @Test
    fun skipsEmptyFolders() {
        val empty = SaveEntry("0100606026F12000", "A", 10, 0, 0)
        assertTrue(SaveCatalog.newestPerTitle(listOf(empty)).isEmpty())
    }

    @Test
    fun newestProfileWins() {
        val old = SaveEntry("0100606026F12000", "A", 10, 5, 1)
        val new = SaveEntry("0100606026F12000", "B", 20, 5, 1)
        assertEquals(listOf(new), SaveCatalog.newestPerTitle(listOf(old, new)))
    }

    @Test
    fun newestFirst() {
        val a = SaveEntry("010055D009F78000", "A", 10, 5, 1)
        val b = SaveEntry("0100606026F12000", "A", 30, 5, 1)
        assertEquals(listOf(b, a), SaveCatalog.newestPerTitle(listOf(a, b)))
    }

    @Test
    fun destinationStates() {
        val s = SaveEntry("0100606026F12000", "A", 100_000, 5, 1)
        assertEquals(DestinationState.NONE, SaveCatalog.state(s, null))
        assertEquals(DestinationState.OLDER, SaveCatalog.state(s, s.copy(lastModified = 50_000)))
        assertEquals(DestinationState.SAME, SaveCatalog.state(s, s.copy(lastModified = 101_500)))
        assertEquals(DestinationState.NEWER, SaveCatalog.state(s, s.copy(lastModified = 200_000)))
    }

    @Test
    fun titleIds() {
        assertTrue(SaveCatalog.isTitleId("0100606026f12000"))
        assertTrue(SaveCatalog.isTitleId("0100606026F12000"))
        assertFalse(SaveCatalog.isTitleId(".yuzu_save_size"))
        assertFalse(SaveCatalog.isTitleId("0100606026F1200"))
    }
}
