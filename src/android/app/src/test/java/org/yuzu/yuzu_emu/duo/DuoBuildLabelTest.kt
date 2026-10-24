// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Test

class DuoBuildLabelTest {
    @Test
    fun releaseIsNamedByItsVersion() {
        assertEquals("Eden Duo 1.3.1", DuoBuildLabel.overlay("1.3.1", "085545c96a-duo-1.3.1"))
        assertEquals("Eden Duo 2.0.0", DuoBuildLabel.overlay("2.0.0", "v2.0.0"))
        assertEquals("Eden Duo 1.3.2.1", DuoBuildLabel.overlay("1.3.2.1", "26b4b90086-duo-1.3.2.1"))
    }

    @Test
    fun otherBuildsKeepTheCommitId() {
        assertEquals("085545c96a", DuoBuildLabel.overlay("1.3.1-relWithDebInfo", "085545c96a-duo-1.3.1"))
        assertEquals("085545c96a", DuoBuildLabel.overlay("1.3.1-debug", "085545c96a-duo-1.3.1"))
        assertEquals("1.3.0", DuoBuildLabel.overlay("1.3.1-beta.17", "1.3.0-beta.17-3-gabc"))
        assertEquals("", DuoBuildLabel.overlay("", ""))
    }
}
