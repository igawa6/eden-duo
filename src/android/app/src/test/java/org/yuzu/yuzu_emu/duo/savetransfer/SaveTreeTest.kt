// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SaveTreeTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val title = "01007EF00011E000"
    private val profile = "EBABE35DEA2FD45AB95E2BFCBDCCDC30"

    /** An in-memory documents tree: id -> children; files carry their bytes and time. */
    private val dirs = mutableMapOf<String, MutableList<SaveTree.Doc>>()
    private val bytes = mutableMapOf<String, ByteArray>()

    private fun dir(parent: String?, name: String): String {
        val id = if (parent == null) name else "$parent/$name"
        dirs.getOrPut(id) { mutableListOf() }
        if (parent != null) dirs.getValue(parent).add(SaveTree.Doc(id, name, true, 0, 0))
        return id
    }

    private fun file(parent: String, name: String, data: String, modified: Long) {
        val id = "$parent/$name"
        bytes[id] = data.toByteArray()
        dirs.getValue(parent).add(SaveTree.Doc(id, name, false, modified, data.length.toLong()))
    }

    private fun tree() = SaveTree(
        children = { dirs[it] ?: throw IOException("no $it") },
        open = { ByteArrayInputStream(bytes.getValue(it)) }
    )

    /** root/nand/user/save/0000000000000000/<profile>, returning the profile folder id. */
    private fun profileDir(): String {
        var id = dir(null, "root")
        for (name in listOf("nand", "user", "save", "0000000000000000", profile)) id = dir(id, name)
        return id
    }

    @Test
    fun nestedFoldersCountAndCopy() {
        val game = dir(profileDir(), title)
        file(game, "option.sav", "o", 1_000)
        val slot = dir(game, "0")
        file(slot, "game_data.sav", "slot0", 5_000)

        val saves = tree().saves("root")
        assertEquals(listOf(SaveEntry(title, profile, 5_000, 6, 2)), saves)

        val into = tmp.newFolder("into")
        tree().copyTitle("root", saves.single(), into)
        assertEquals("o", File(into, "option.sav").readText())
        assertEquals("slot0", File(into, "0/game_data.sav").readText())
        // Copies keep the source's times, so the two saves compare as the same afterwards.
        assertEquals(5_000, File(into, "0/game_data.sav").lastModified())
    }

    @Test
    fun sizeMarkerAloneIsEmpty() {
        val game = dir(profileDir(), title)
        file(game, ".yuzu_save_size", "12345678", 9_000)
        val saves = tree().saves("root")
        assertEquals(0, saves.single().fileCount)
        assertTrue(SaveCatalog.newestPerTitle(saves).isEmpty())
    }

    @Test
    fun noSaveFolderMeansNoSaves() {
        dir(null, "root")
        assertTrue(tree().saves("root").isEmpty())
    }
}
