// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import kotlin.math.abs

/** One game's save folder in one profile: `nand/user/save/0000000000000000/<profile>/<titleId>`. */
data class SaveEntry(
    val titleId: String,
    val profile: String,
    val lastModified: Long,
    val bytes: Long,
    val fileCount: Int
)

/** How the save already in this app compares with the one being copied in. */
enum class DestinationState { NONE, OLDER, SAME, NEWER }

object SaveCatalog {
    /** File times differ by a second or two between copies and file systems. */
    private const val SAME_WINDOW_MS = 2_000L
    private val titleId = Regex("[0-9A-Fa-f]{16}")

    /** One entry per game: the profile with the newest non-empty save, newest games first. */
    fun newestPerTitle(entries: List<SaveEntry>): List<SaveEntry> =
        entries.filter { it.fileCount > 0 }
            .groupBy { it.titleId.uppercase() }
            .map { (_, saves) -> saves.maxBy { it.lastModified } }
            .sortedByDescending { it.lastModified }

    fun state(source: SaveEntry, destination: SaveEntry?): DestinationState = when {
        destination == null -> DestinationState.NONE
        abs(destination.lastModified - source.lastModified) <= SAME_WINDOW_MS -> DestinationState.SAME
        destination.lastModified < source.lastModified -> DestinationState.OLDER
        else -> DestinationState.NEWER
    }

    fun isTitleId(name: String): Boolean = titleId.matches(name)
}
