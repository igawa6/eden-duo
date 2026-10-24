// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.ZipInputStream

/**
 * Eden Duo: reads a save zip for one game without trusting its layout.
 *
 * Eden's per-game export writes `<TITLEID>/...`, but a zip made by hand or by another tool can
 * put the title folder deeper (`save/0000000000000000/<user>/<TITLEID>/...`), spell it in lower
 * case or use backslashes. The game's folder is looked up anywhere in the zip (the shallowest
 * match wins), only its entries are unpacked into a staging folder, and every failure comes
 * back as a [Result] instead of an exception.
 */
object SaveZipImport {
    private val titleIdPattern = Regex("^[0-9A-Fa-f]{16}$")
    private const val EMPTY_TITLE_ID = "0000000000000000"

    sealed class Result {
        /** The staging folder holds the contents of the game's save folder. */
        data class Staged(val fileCount: Int) : Result()

        /** No folder for the game; [otherTitleIds] are title-ID folders the zip does contain. */
        data class NotFound(val otherTitleIds: List<String>) : Result()

        data class Failed(val error: Exception) : Result()
    }

    /** The entry name with `/` separators and no leading `/` or `./`. */
    fun normalize(entryName: String): String {
        var name = entryName.trim().replace('\\', '/')
        while (name.startsWith("/") || name.startsWith("./")) {
            name = name.removePrefix("./").removePrefix("/")
        }
        return name
    }

    /** The folder names of an entry: every segment of a directory, all but the last of a file. */
    private fun folderSegments(entryName: String): List<String> {
        val name = normalize(entryName)
        val segments = name.split('/').filter { it.isNotEmpty() }
        return if (name.endsWith("/")) segments else segments.dropLast(1)
    }

    /**
     * The path (`a/b/<TITLEID>/`, normalized) of the shallowest folder named [titleId], compared
     * case-insensitively, or null when no entry lies in such a folder.
     */
    fun findTitleFolder(entryNames: Iterable<String>, titleId: String): String? {
        var best: List<String>? = null
        for (entryName in entryNames) {
            val folders = folderSegments(entryName)
            val index = folders.indexOfFirst { it.equals(titleId, ignoreCase = true) }
            if (index >= 0 && (best == null || index < best.size - 1)) {
                best = folders.subList(0, index + 1)
            }
        }
        return best?.joinToString("/", postfix = "/")
    }

    /** Folders in the zip that look like another game's title ID (upper case, sorted). */
    fun otherTitleIds(entryNames: Iterable<String>, titleId: String): List<String> =
        entryNames.flatMap { folderSegments(it) }
            .filter { titleIdPattern.matches(it) }
            .map { it.uppercase() }
            .filter { it != EMPTY_TITLE_ID && !it.equals(titleId, ignoreCase = true) }
            .distinct()
            .sorted()

    /**
     * The path of [entryName] below [titleFolder] (as returned by [findTitleFolder]), compared
     * case-insensitively; "" for the folder itself, null for an entry outside it.
     */
    fun relativePath(entryName: String, titleFolder: String): String? {
        val name = normalize(entryName)
        if (name.equals(titleFolder.removeSuffix("/"), ignoreCase = true)) {
            return ""
        }
        if (!name.startsWith(titleFolder, ignoreCase = true)) {
            return null
        }
        return name.substring(titleFolder.length)
    }

    /**
     * Unpacks the save folder of [titleId] from the zip [open] returns (opened twice: once to
     * find the folder, once to unpack it) into [stagingDir], which is emptied first.
     */
    fun stage(open: () -> InputStream, titleId: String, stagingDir: File): Result {
        return try {
            val names = mutableListOf<String>()
            ZipInputStream(open()).use { zis ->
                var entry = zis.nextEntry
                while (entry != null) {
                    names.add(entry.name)
                    entry = zis.nextEntry
                }
            }

            val titleFolder = findTitleFolder(names, titleId)
                ?: return Result.NotFound(otherTitleIds(names, titleId))

            stagingDir.deleteRecursively()
            if (!stagingDir.mkdirs()) {
                throw IOException("Failed to create directory $stagingDir")
            }
            val root = stagingDir.canonicalPath + File.separator
            var fileCount = 0
            ZipInputStream(open()).use { zis ->
                var entry = zis.nextEntry
                while (entry != null) {
                    val relative = relativePath(entry.name, titleFolder)
                    if (!relative.isNullOrEmpty()) {
                        val newFile = File(stagingDir, relative)
                        if (!newFile.canonicalPath.startsWith(root)) {
                            throw SecurityException(
                                "Zip file attempted path traversal! ${entry.name}"
                            )
                        }
                        val isDirectory = entry.isDirectory || normalize(entry.name).endsWith("/")
                        val directory = if (isDirectory) newFile else newFile.parentFile!!
                        if (!directory.isDirectory && !directory.mkdirs()) {
                            throw IOException("Failed to create directory $directory")
                        }
                        if (!isDirectory) {
                            newFile.outputStream().use { zis.copyTo(it) }
                            fileCount++
                        }
                    }
                    entry = zis.nextEntry
                }
            }
            Result.Staged(fileCount)
        } catch (e: Exception) {
            Result.Failed(e)
        }
    }

    /**
     * Replaces [target] with the contents of [staged] and returns whether that worked. The old
     * [target] is moved aside until the new one is in place and put back if that fails. Only
     * where the storage refuses to rename [target] is it replaced in place (delete, then copy).
     */
    fun replaceDirectory(staged: File, target: File): Boolean {
        val backup = File(target.parentFile, target.name + ".duo-old")
        backup.deleteRecursively()
        target.parentFile?.mkdirs()
        if (target.exists() && !target.renameTo(backup)) {
            // The storage refuses the rename: keep a copy of the old data before deleting it, so
            // a failed copy of the new data can still be undone.
            return try {
                if (!target.copyRecursively(backup, overwrite = true)) return false
                target.deleteRecursively()
                staged.copyRecursively(target, overwrite = true).also { copied ->
                    if (copied) backup.deleteRecursively() else restore(backup, target)
                }
            } catch (e: Exception) {
                restore(backup, target)
                false
            }
        }
        if (staged.renameTo(target)) {
            backup.deleteRecursively()
            return true
        }
        return try {
            staged.copyRecursively(target, overwrite = true)
            backup.deleteRecursively()
            true
        } catch (e: Exception) {
            target.deleteRecursively()
            backup.renameTo(target)
            false
        }
    }

    /** Puts [backup] back as [target] after a failed replacement (rename, else copy). */
    private fun restore(backup: File, target: File) {
        if (!backup.exists()) return
        target.deleteRecursively()
        if (!backup.renameTo(target)) {
            runCatching { backup.copyRecursively(target, overwrite = true) }
        }
    }
}
