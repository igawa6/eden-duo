// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CompanionCatalogTest {
    private val catalog = CompanionCatalog.parse(
        """
        [
          {"titleId": "010093801237c000", "name": "Metroid Dread", "gametdb": "AYL8A"},
          {"titleId": "0100C2901153C000", "name": "Yoru, Tomosu", "gametdb": "AXNMA", "region": "JA"},
          {"titleId": "01006F8002326000", "name": "Animal Crossing: New Horizons", "gametdb": "ACBAA",
           "future": true},
          {"titleId": "0100BA4026E94000", "name": "Castlevania: Belmont's Curse"},
          {"titleId": "short", "name": "Broken"},
          {"titleId": "0100000000000001", "name": " "}
        ]
        """
    )

    @Test
    fun parsesEntriesByUpperCaseTitleId() {
        assertEquals(4, catalog.size)
        assertEquals("AYL8A", catalog["010093801237C000"]?.gametdb)
        assertEquals("JA", catalog["0100C2901153C000"]?.region)
        assertEquals("", catalog["0100BA4026E94000"]?.gametdb)
        assertEquals("", catalog["010093801237C000"]?.region)
    }

    @Test
    fun unownedIsSupportedMinusOwnedByName() {
        val supported = setOf(
            "010093801237C000",
            "0100C2901153C000",
            "01006F8002326000",
            "0100BA4026E94000",
            "0100FFFFFFFFF000" // published after this build: no name, left out
        )
        val unowned = CompanionCatalog.unowned(supported, listOf("0100c2901153c000", "0"), catalog)
        assertEquals(
            listOf("Animal Crossing: New Horizons", "Castlevania: Belmont's Curse", "Metroid Dread"),
            unowned.map { it.name }
        )
        assertTrue(CompanionCatalog.unowned(supported, supported, catalog).isEmpty())
        assertTrue(CompanionCatalog.unowned(emptySet(), emptyList(), catalog).isEmpty())
    }

    @Test
    fun sectionHidesWhileSearchingOrSwitchedOff() {
        assertTrue(CompanionCatalog.sectionVisible(true, "", true))
        assertTrue(CompanionCatalog.sectionVisible(true, "  ", true))
        assertFalse(CompanionCatalog.sectionVisible(true, "zel", true))
        assertFalse(CompanionCatalog.sectionVisible(false, "", true))
        assertFalse(CompanionCatalog.sectionVisible(true, "", false))
    }

    @Test
    fun bundledCatalogCoversEverySupportedTitle() {
        val res = File("src/main/res/values/dual_screen_companions.xml").readText()
        val ids = Regex("<item>([0-9A-F]{16})</item>").findAll(res).map { it.groupValues[1] }.toList()
        assertTrue(ids.isNotEmpty())
        val bundled = CompanionCatalog.parse(File("src/main/assets/duo/companion_catalog.json").readText())
        ids.forEach { assertTrue("$it has no catalog entry", it in bundled) }
        bundled.values.forEach { entry ->
            assertTrue(entry.name, entry.gametdb.isEmpty() || Regex("[A-Z0-9]{5}").matches(entry.gametdb))
        }
    }
}
