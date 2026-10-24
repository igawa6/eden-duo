// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.util.zip.ZipEntry
import java.util.zip.ZipException
import java.util.zip.ZipOutputStream

class SaveZipImportTest {
    @get:Rule
    val temp = TemporaryFolder()

    private val title = "0100F2C0115B6000"
    private val otherTitle = "01006A800016E000"

    /** A zip of [entries] (name to content; a name ending in "/" is a directory entry). */
    private fun zip(vararg entries: Pair<String, String>): ByteArray {
        val bytes = ByteArrayOutputStream()
        ZipOutputStream(bytes).use { zos ->
            for ((name, content) in entries) {
                zos.putNextEntry(ZipEntry(name))
                if (!name.endsWith("/")) {
                    zos.write(content.toByteArray())
                }
                zos.closeEntry()
            }
        }
        return bytes.toByteArray()
    }

    private fun stage(bytes: ByteArray, staging: File = File(temp.root, "staging")) =
        SaveZipImport.stage({ ByteArrayInputStream(bytes) }, title, staging)

    private fun files(dir: File): Map<String, String> =
        dir.walkTopDown().filter { it.isFile }
            .associate { it.relativeTo(dir).invariantSeparatorsPath to it.readText() }

    @Test
    fun androidPerGameExportLayout() {
        val staging = File(temp.root, "staging")
        val result = stage(
            zip("$title/" to "", "$title/data.bin" to "save", "$title/sub/slot1" to "one"),
            staging
        )
        assertEquals(SaveZipImport.Result.Staged(2), result)
        assertEquals(mapOf("data.bin" to "save", "sub/slot1" to "one"), files(staging))
    }

    @Test
    fun nestedUserSaveLayout() {
        val user = "1234567890ABCDEF1234567890ABCDEF"
        val staging = File(temp.root, "staging")
        val result = stage(
            zip(
                "save/0000000000000000/$user/$title/main" to "m",
                "save/0000000000000000/$user/$title/backup/b" to "b"
            ),
            staging
        )
        assertEquals(SaveZipImport.Result.Staged(2), result)
        assertEquals(mapOf("main" to "m", "backup/b" to "b"), files(staging))
    }

    @Test
    fun lowercaseTitleFolder() {
        val staging = File(temp.root, "staging")
        val result = stage(zip("${title.lowercase()}/file" to "x"), staging)
        assertEquals(SaveZipImport.Result.Staged(1), result)
        assertEquals(mapOf("file" to "x"), files(staging))
    }

    @Test
    fun backslashSeparators() {
        val staging = File(temp.root, "staging")
        val result = stage(zip("saves\\$title\\dir\\file" to "x"), staging)
        assertEquals(SaveZipImport.Result.Staged(1), result)
        assertEquals(mapOf("dir/file" to "x"), files(staging))
    }

    @Test
    fun multiGameZipExtractsOnlyTheGame() {
        val staging = File(temp.root, "staging")
        val result = stage(
            zip(
                "$otherTitle/other" to "no",
                "$title/mine" to "yes",
                "readme.txt" to "no"
            ),
            staging
        )
        assertEquals(SaveZipImport.Result.Staged(1), result)
        assertEquals(mapOf("mine" to "yes"), files(staging))
    }

    @Test
    fun shallowestMatchWins() {
        assertEquals(
            "$title/",
            SaveZipImport.findTitleFolder(listOf("a/b/$title/x", "$title/y"), title)
        )
        assertEquals(
            "a/$title/",
            SaveZipImport.findTitleFolder(listOf("a/$title/$title/x"), title)
        )
        assertNull(SaveZipImport.findTitleFolder(listOf(title, "x/$title"), title))
    }

    @Test
    fun relativePathBelowTheTitleFolder() {
        assertEquals("a/b", SaveZipImport.relativePath("/save/$title/a/b", "save/$title/"))
        assertEquals("", SaveZipImport.relativePath("save/$title/", "save/$title/"))
        assertNull(SaveZipImport.relativePath("save/$otherTitle/a", "save/$title/"))
    }

    @Test
    fun missingGameNamesOtherTitles() {
        val result = stage(
            zip(
                "save/0000000000000000/$otherTitle/a" to "1",
                "${otherTitle.lowercase()}/b" to "2",
                "notes/c" to "3"
            )
        )
        assertEquals(SaveZipImport.Result.NotFound(listOf(otherTitle)), result)
        assertFalse(File(temp.root, "staging").exists())
    }

    @Test
    fun missingGameWithoutTitleFolders() {
        assertEquals(SaveZipImport.Result.NotFound(emptyList()), stage(zip("a/b" to "c")))
        // Not a zip at all: no entries.
        assertEquals(
            SaveZipImport.Result.NotFound(emptyList()),
            stage("not a zip".toByteArray())
        )
    }

    @Test
    fun corruptZipIsAFailure() {
        // Cut inside the first entry's compressed data (local header + name = 51 bytes).
        val bytes = zip("$title/data" to "x".repeat(4096))
        val result = stage(bytes.copyOf(60))
        assertTrue(result is SaveZipImport.Result.Failed)
        assertTrue((result as SaveZipImport.Result.Failed).error is IOException)
    }

    @Test
    fun unreadableZipIsAFailure() {
        val result = SaveZipImport.stage(
            { throw ZipException("broken") },
            title,
            File(temp.root, "staging")
        )
        assertTrue(result is SaveZipImport.Result.Failed)
    }

    @Test
    fun pathTraversalIsRejected() {
        val staging = File(temp.root, "deep/staging")
        val result = stage(zip("$title/../../../evil" to "x"), staging)
        assertTrue(result is SaveZipImport.Result.Failed)
        assertTrue((result as SaveZipImport.Result.Failed).error is SecurityException)
        assertFalse(File(temp.root, "evil").exists())
    }

    @Test
    fun replaceDirectorySwapsContents() {
        val staged = temp.newFolder("staged")
        File(staged, "new").writeText("new")
        val target = File(temp.root, "user/save/$title")
        target.mkdirs()
        File(target, "old").writeText("old")

        assertTrue(SaveZipImport.replaceDirectory(staged, target))
        assertEquals(mapOf("new" to "new"), files(target))
        assertFalse(File(target.parentFile, "$title.duo-old").exists())
    }

    @Test
    fun replaceDirectoryCreatesAMissingTarget() {
        val staged = temp.newFolder("staged")
        File(staged, "new").writeText("new")
        val target = File(temp.root, "user/save/$title")

        assertTrue(SaveZipImport.replaceDirectory(staged, target))
        assertEquals(mapOf("new" to "new"), files(target))
    }

    @Test
    fun throwingTaskEndsWithAFailure() = runBlocking {
        var logged: Exception? = null
        val result = runGuardedTask({ logged = it }) { throw IOException("disk full") }
        assertTrue(result is DuoTaskFailure)
        assertEquals("disk full", (result as DuoTaskFailure).error.message)
        assertEquals(result.error, logged)
        assertEquals("ok", runGuardedTask({}) { "ok" })
    }
}
