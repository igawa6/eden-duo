// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import okhttp3.OkHttpClient
import okhttp3.Request
import org.yuzu.yuzu_emu.YuzuApplication
import java.io.File
import java.io.IOException
import java.util.Locale
import java.util.concurrent.TimeUnit

/**
 * Eden Duo: names and GameTDB cover codes of the games that have a dual-screen companion
 * (assets/duo/companion_catalog.json, extended by the online catalog.json of the companions
 * repository), for the library's "not in your library" section.
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
    /** Published with every companion release, so new games show without an APK update. */
    const val ONLINE_URL =
        "https://raw.githubusercontent.com/igawa6/eden-duo-companions/main/catalog.json"
    private const val CACHE = "duo/companion_catalog_online.json"
    private val json = Json { ignoreUnknownKeys = true }
    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build()

    /** The bundled catalog by upper-case title id; empty if the asset cannot be read. */
    private val bundled: Map<String, Entry> by lazy {
        runCatching {
            YuzuApplication.appContext.assets.open(ASSET).bufferedReader().use { parse(it.readText()) }
        }.getOrDefault(emptyMap())
    }

    private val cacheFile get() = File(YuzuApplication.appContext.filesDir, CACHE)

    /** The last online catalog (kept across launches); empty until one was fetched. */
    @Volatile
    private var online: Map<String, Entry> = runCatching { parse(cacheFile.readText()) }
        .getOrDefault(emptyMap())

    private val _revision = MutableStateFlow(0)

    /** Bumped whenever [entries] changes, so the library can refilter. */
    val revision = _revision.asStateFlow()

    /** Bundled entries overridden and extended by the online catalog. */
    val entries: Map<String, Entry> get() = bundled + online

    /**
     * Fetches the online catalog ([read] on an IO thread) and keeps it when it parses to at
     * least one entry; an empty or broken answer keeps the previous one. True when it changed.
     */
    fun refreshOnline(read: () -> String = ::download): Boolean {
        val text = read()
        val fetched = parse(text)
        if (fetched.isEmpty() || fetched == online) return false
        runCatching {
            cacheFile.parentFile?.mkdirs()
            cacheFile.writeText(text)
        }
        online = fetched
        _revision.value++
        return true
    }

    private fun download(): String {
        val request = Request.Builder().url(ONLINE_URL).build()
        return client.newCall(request).execute().use { response ->
            if (!response.isSuccessful) throw IOException("catalog: HTTP ${response.code}")
            response.body?.string() ?: throw IOException("catalog: empty response")
        }
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
