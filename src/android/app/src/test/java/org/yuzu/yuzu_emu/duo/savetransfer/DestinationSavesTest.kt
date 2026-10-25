// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import java.io.File
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class DestinationSavesTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val title = "0100606026F12000"
    private val profile = "EBABE35DEA2FD45AB95E2BFCBDCCDC30"

    /** `<user>/save/0000000000000000/<profile>`, the folder DestinationSaves works in. */
    private fun profileRoot(): File =
        File(tmp.root, "nand/user/save/0000000000000000/$profile").apply { mkdirs() }

    private fun backupRoot() = File(tmp.root, "nand/user/save.before-transfer/$title")

    @Test
    fun listsTitlesWithNewestTimeAndSize() {
        val root = profileRoot()
        File(root, title).mkdirs()
        File(root, "$title/a").writeBytes(ByteArray(3))
        File(root, "$title/b").writeBytes(ByteArray(4))
        File(root, "$title/a").setLastModified(1_000_000)
        File(root, "$title/b").setLastModified(2_000_000)
        File(root, ".yuzu_save_size").writeText("x")
        File(root, "$title/.yuzu_save_size").writeText("12345678")
        File(root, "$title/.yuzu_save_size").setLastModified(9_000_000)

        val listed = DestinationSaves(root).list()
        assertEquals(setOf(title), listed.keys)
        assertEquals(SaveEntry(title, profile, 2_000_000, 7, 2), listed[title])
    }

    @Test
    fun backupKeepsThree() {
        val root = profileRoot()
        File(root, title).mkdirs()
        File(root, "$title/a").writeText("live")
        val saves = DestinationSaves(root)
        repeat(4) { saves.backup(title) }

        assertEquals(listOf("2", "3", "4"), backupRoot().list()!!.sorted())
        assertEquals("live", File(backupRoot(), "4/$profile/$title/a").readText())
        assertEquals("live", File(root, "$title/a").readText())
    }

    @Test
    fun backupNothing() {
        assertNull(DestinationSaves(profileRoot()).backup(title))
    }

    @Test
    fun backupIsSeparateFromImport() {
        val root = profileRoot()
        File(root, title).mkdirs()
        File(root, "$title/a").writeText("live")
        val importBackup = File(tmp.root, "nand/user/save.before-import-1/keep").apply {
            parentFile!!.mkdirs()
            writeText("import")
        }
        DestinationSaves(root).backup(title)
        assertEquals("import", importBackup.readText())
    }

    @Test
    fun replaceSwapsWholeFolder() {
        val root = profileRoot()
        File(root, title).mkdirs()
        File(root, "$title/a").writeText("old")
        DestinationSaves(root).replace(title) { File(it, "b").writeText("new") }

        assertEquals(listOf("b"), File(root, title).list()!!.toList())
        assertEquals("new", File(root, "$title/b").readText())
        assertEquals(listOf(title), root.list()!!.toList())
    }

    @Test
    fun replaceCreatesMissingTitle() {
        val root = profileRoot()
        DestinationSaves(root).replace(title) { File(it, "b").writeText("new") }
        assertEquals("new", File(root, "$title/b").readText())
    }

    @Test
    fun failedCopyKeepsOldSave() {
        val root = profileRoot()
        File(root, title).mkdirs()
        File(root, "$title/a").writeText("old")
        try {
            DestinationSaves(root).replace(title) {
                File(it, "half").writeText("x")
                throw IOException("disk full")
            }
            fail("replace should rethrow")
        } catch (e: IOException) {
            assertEquals("disk full", e.message)
        }
        assertEquals("old", File(root, "$title/a").readText())
        assertEquals(listOf("a"), File(root, title).list()!!.toList())
        assertFalse(root.list()!!.any { it.startsWith(".") })
        assertTrue(File(root, title).isDirectory)
    }

    @Test
    fun transfersOfOneGameDontOverlap() {
        // A screen closed mid-copy keeps copying; a reopened screen must not share its staging folder.
        val root = profileRoot()
        File(root, title).mkdirs()
        File(root, "$title/a").writeText("old")
        val firstWriting = CountDownLatch(1)
        val releaseFirst = CountDownLatch(1)
        val secondStarted = AtomicBoolean(false)
        val first = thread {
            DestinationSaves(root).transfer(title) {
                File(it, "first").writeText("1")
                firstWriting.countDown()
                releaseFirst.await()
            }
        }
        firstWriting.await()
        val second = thread {
            DestinationSaves(root).transfer(title) {
                secondStarted.set(true)
                File(it, "second").writeText("2")
            }
        }
        Thread.sleep(300)
        assertFalse("second transfer started while the first was copying", secondStarted.get())
        releaseFirst.countDown()
        first.join()
        second.join()

        assertEquals(listOf("second"), File(root, title).list()!!.toList())
        assertEquals("1", File(backupRoot(), "2/$profile/$title/first").readText())
        assertEquals("old", File(backupRoot(), "1/$profile/$title/a").readText())
    }
}
