// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.yuzu.yuzu_emu.utils.EmulatorImportSources

class SaveSourcesTest {
    private val calamansi = "org.azahar_emu.azahar.fatesfix"
    private val duetPlusDev = SaveSources.DUET_PLUS + ".dev"

    @Test
    fun edenFamilyOnly() {
        for (pkg in SaveSources.EDEN_FAMILY) assertTrue(pkg, SaveSources.allowed(pkg, duetPlusDev, false))
        assertFalse(SaveSources.allowed("org.citron.citron_emu", duetPlusDev, false))
        assertFalse(SaveSources.allowed("dev.eden.eden_emulator.dualscreen", duetPlusDev, false))
    }

    @Test
    fun duetPlusOnlyInCalamansi() {
        assertTrue(SaveSources.allowed(SaveSources.DUET_PLUS, calamansi, true))
        assertFalse(SaveSources.allowed(SaveSources.DUET_PLUS, duetPlusDev, false))
    }

    @Test
    fun neverSelf() {
        assertFalse(SaveSources.allowed("dev.igawa6.edenduo.dev", "dev.igawa6.edenduo.dev", true))
    }

    @Test
    fun ungrantedSourceNeedsAccess() {
        val src = EmulatorImportSources.Source("Eden Duo Dev", "dev.igawa6.edenduo.dev.user", "dev.igawa6.edenduo.dev")
        assertTrue(SaveSources.needsAccess(SourceApp(src, null)))
        assertFalse(
            SaveSources.needsAccess(
                SourceApp(src, "content://dev.igawa6.edenduo.dev.user/tree/root")
            )
        )
    }
}
