// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import java.io.File
import java.io.IOException

/**
 * This app's saves for one profile, `<user>/save/0000000000000000/<profile>`. Writes replace one
 * game's folder whole, after a copy of the old one goes to `<user>/save.before-transfer/<title>/<n>`
 * (newest [KEEP] per game). The Eden import's `save.before-import-<n>` backups are never touched.
 */
class DestinationSaves(private val profileRoot: File) {
    private val profile: String = profileRoot.name
    private val userRoot: File? = profileRoot.parentFile?.parentFile?.parentFile

    fun list(): Map<String, SaveEntry> =
        profileRoot.listFiles().orEmpty()
            .filter { it.isDirectory && SaveCatalog.isTitleId(it.name) }
            .associate { dir ->
                val files = dir.walkTopDown().filter { it.isFile && it.name != SaveCatalog.SIZE_MARKER }.toList()
                val title = dir.name.uppercase()
                title to SaveEntry(
                    titleId = title,
                    profile = profile,
                    lastModified = files.maxOfOrNull { it.lastModified() } ?: dir.lastModified(),
                    bytes = files.sumOf { it.length() },
                    fileCount = files.size
                )
            }

    /** Copies the live save aside; null when the game has no save here. */
    fun backup(titleId: String): File? {
        val live = File(profileRoot, titleId)
        if (!live.isDirectory) return null
        val games = File(requireNotNull(userRoot) { "Not a save folder: $profileRoot" }, "save.before-transfer/$titleId")
        val numbered = games.listFiles().orEmpty().mapNotNull { it.name.toIntOrNull() }
        val backup = File(games, "${(numbered.maxOrNull() ?: 0) + 1}")
        if (!live.copyRecursively(File(backup, "$profile/$titleId"))) {
            backup.deleteRecursively()
            throw IOException("Cannot back up $titleId")
        }
        (numbered + backup.name.toInt()).sortedDescending().drop(KEEP)
            .forEach { File(games, "$it").deleteRecursively() }
        return backup
    }

    /**
     * Backs up then replaces one game's save. One transfer at a time in this process: a screen
     * closed mid-copy keeps copying, and a reopened one must not share its staging folder.
     */
    // ponytail: one process-wide lock; per-title locks if parallel transfers ever matter.
    fun transfer(titleId: String, write: (File) -> Unit): File? = synchronized(LOCK) {
        backup(titleId).also { replace(titleId, write) }
    }

    /** Writes the new save beside the old one, then swaps it in; on failure the old save stays. */
    fun replace(titleId: String, write: (File) -> Unit) {
        val live = File(profileRoot, titleId)
        val incoming = File(profileRoot, ".$titleId.incoming")
        val old = File(profileRoot, ".$titleId.old")
        incoming.deleteRecursively()
        old.deleteRecursively()
        if (!incoming.mkdirs()) throw IOException("Cannot create $incoming")
        try {
            write(incoming)
        } catch (e: Exception) {
            incoming.deleteRecursively()
            throw e
        }
        if (live.exists() && !live.renameTo(old)) {
            incoming.deleteRecursively()
            throw IOException("Cannot move the old $titleId save aside")
        }
        if (!incoming.renameTo(live)) {
            old.renameTo(live)
            incoming.deleteRecursively()
            throw IOException("Cannot put the new $titleId save in place")
        }
        old.deleteRecursively()
    }

    companion object {
        const val KEEP = 3
        private val LOCK = Any()
    }
}
