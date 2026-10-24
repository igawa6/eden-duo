// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import org.yuzu.yuzu_emu.YuzuApplication
import java.util.Locale

/**
 * Eden Duo: names and GameTDB cover codes of the games that have a dual-screen companion
 * (assets/duo/companion_catalog.json), for the library's "not in your library" section.
 * [Entry.gametdb] is GameTDB's five-character product code, not a title id; empty means no
 * cover (the tile shows the dual-screen glyph). [Entry.region] is the GameTDB art region to
 * try first.
 */
object CompanionCatalog {
    @Serializable
    data class Entry(
        val titleId: String,
        val name: String,
        val gametdb: String = "",
        val region: String = ""
    )

    private const val ASSET = "duo/companion_catalog.json"
    private val json = Json { ignoreUnknownKeys = true }

    /** The bundled catalog by upper-case title id; empty if the asset cannot be read. */
    val entries: Map<String, Entry> by lazy {
        runCatching {
            YuzuApplication.appContext.assets.open(ASSET).bufferedReader().use { parse(it.readText()) }
        }.getOrDefault(emptyMap())
    }

    fun parse(text: String): Map<String, Entry> =
        json.decodeFromString<List<Entry>>(text)
            .filter { it.titleId.length == 16 && it.name.isNotBlank() }
            .associateBy { it.titleId.uppercase(Locale.ROOT) }

    /**
     * The games with a companion ([supported]) that are not among [owned] title ids, by name.
     * Titles missing from [catalog] are left out: without a name there is nothing to show.
     */
    fun unowned(
        supported: Set<String>,
        owned: Collection<String>,
        catalog: Map<String, Entry> = entries
    ): List<Entry> {
        val ownedIds = owned.mapTo(HashSet()) { it.uppercase(Locale.ROOT) }
        return supported.asSequence()
            .map { it.uppercase(Locale.ROOT) }
            .filter { it !in ownedIds }
            .mapNotNull { catalog[it] }
            .distinct()
            .sortedBy { it.name.lowercase(Locale.ROOT) }
            .toList()
    }

    /**
     * Whether the library shows the section: the setting is on, nothing is typed in the search
     * box and the list is in its full order (not "recently played/added").
     */
    fun sectionVisible(enabled: Boolean, searchTerm: String, fullList: Boolean): Boolean =
        enabled && searchTerm.isBlank() && fullList
}
