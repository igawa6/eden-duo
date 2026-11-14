// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** The stored Custom layout's validation (DualScreenCustomLayoutStore.fromFractions). */
class DualScreenCustomLayoutStoreTest {
    private val game = DualScreenNormRect(0f, 0.1f, 0.6f, 0.75f)
    private val companion = DualScreenNormRect(0.6f, 0.1f, 0.4f, 0.8f)

    @Test
    fun acceptsAValidPair() {
        assertEquals(
            DualScreenCustomLayout(game, companion),
            DualScreenCustomLayoutStore.fromFractions(game, companion)
        )
    }

    @Test
    fun rejectsAMissingValue() {
        assertNull(DualScreenCustomLayoutStore.fromFractions(game.copy(x = Float.NaN), companion))
    }

    @Test
    fun rejectsValuesOutsideZeroToOne() {
        assertNull(DualScreenCustomLayoutStore.fromFractions(game.copy(x = -0.1f), companion))
        assertNull(DualScreenCustomLayoutStore.fromFractions(game, companion.copy(w = 1.5f)))
    }

    @Test
    fun rejectsAZeroSize() {
        assertNull(DualScreenCustomLayoutStore.fromFractions(game.copy(w = 0f), companion))
        assertNull(DualScreenCustomLayoutStore.fromFractions(game, companion.copy(h = 0f)))
    }
}
