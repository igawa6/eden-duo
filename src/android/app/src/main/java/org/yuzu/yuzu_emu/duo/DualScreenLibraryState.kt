// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import androidx.preference.PreferenceManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.EmulatorImportViewModel
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.DocumentFileAccess
import org.yuzu.yuzu_emu.utils.DualScreenModRepository
import org.yuzu.yuzu_emu.utils.DualScreenModStatus
import org.yuzu.yuzu_emu.utils.Log
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Eden Duo: the game library's dual-screen state, owned by GamesViewModel -- which titles have a
 * companion package (the release catalog, cached), each game's install state (missing, current,
 * update available) and the library reload after an emulator data import.
 *
 * [games] is the current library, [isReloading] whether a library reload runs, and
 * [reloadAfterImport] reloads the game folders and the library.
 */
class DualScreenLibraryState(
    private val scope: CoroutineScope,
    private val games: () -> List<Game>,
    private val isReloading: () -> Boolean,
    private val reloadAfterImport: () -> Unit
) {
    private companion object {
        const val DUAL_SCREEN_TITLE_IDS = "DualScreenCompanionTitleIds"
    }

    private val preferences =
        PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)

    private var lastImportGeneration = preferences.getLong(
        EmulatorImportViewModel.IMPORT_GENERATION,
        0
    )

    fun refreshAfterEmulatorImport() {
        val generation = preferences.getLong(EmulatorImportViewModel.IMPORT_GENERATION, 0)
        if (generation != lastImportGeneration && !isReloading()) {
            lastImportGeneration = generation
            reloadAfterImport()
        }
    }

    /** The titles this APK shipped with; a GitHub answer never removes them. */
    private val bundledTitleIds = YuzuApplication.appContext.resources
        .getStringArray(R.array.dual_screen_supported_title_ids).toSet()
    private val _dualScreenTitleIds = MutableStateFlow(
        (preferences.getStringSet(DUAL_SCREEN_TITLE_IDS, null)?.toSet() ?: emptySet()) +
            bundledTitleIds
    )
    val dualScreenTitleIds = _dualScreenTitleIds.asStateFlow()
    private val _dualScreenInstallStates = MutableStateFlow(emptyMap<String, DualScreenModStatus.State>())
    val dualScreenInstallStates = _dualScreenInstallStates.asStateFlow()
    private var statusJob: Job? = null
    private var latestVersions: Map<String, String> = runCatching {
        Json.decodeFromString<Map<String, String>>(preferences.getString("DualScreenVersions", "{}")!!)
    }.getOrDefault(emptyMap())

    fun refreshInstalledCompanions() {
        scope.launch {
            statusJob?.cancel()
            statusJob = scope.launch {
                val supported = _dualScreenTitleIds.value
                val versions = latestVersions
                val library = games().toList()
                val states = withContext(Dispatchers.IO) {
                    library.filter { it.programIdHex in supported }.associate { game ->
                        val state = runCatching {
                            val path = game.addonDir
                            if (path.startsWith("content://")) {
                                // The installer cannot write to a linked folder, so never offer a
                                // download/update there; an installed current companion still shows.
                                DualScreenModStatus.evaluate(
                                    DocumentFileAccess.companionPackages(path),
                                    game.programIdHex, versions[game.programIdHex]
                                ).takeIf { it == DualScreenModStatus.State.CURRENT }
                                    ?: DualScreenModStatus.State.UNKNOWN
                            } else DualScreenModStatus.scan(File(path), game.programIdHex, versions[game.programIdHex])
                        }.getOrDefault(DualScreenModStatus.State.UNKNOWN)
                        game.programIdHex to state
                    }
                }
                _dualScreenInstallStates.value = states
                Log.info(
                    "[DualScreen] install states: ${states.size} supported of ${library.size} " +
                        "games; " + states.entries.groupingBy { it.value }.eachCount()
                )
            }
        }
    }

    private val catalogRefreshing = AtomicBoolean(false)
    private var lastCatalogAttempt = 0L

    /** Show cached support immediately and refresh once per minute when the library opens. */
    fun refreshDualScreenSupport(force: Boolean = false) {
        refreshInstalledCompanions()
        val now = android.os.SystemClock.elapsedRealtime()
        if ((!force && lastCatalogAttempt != 0L && now - lastCatalogAttempt < 60_000L) ||
            !catalogRefreshing.compareAndSet(false, true)
        ) {
            return
        }
        lastCatalogAttempt = now
        scope.launch {
            try {
                val releases = withContext(Dispatchers.IO) { DualScreenModRepository.fetchAll() }
                val latest = releases.groupBy { DualScreenModRepository.titleIdForAsset(it.assetName) }
                    .filterKeys { it != null }.mapValues { it.value.first() }
                val fetched = latest.keys.filterNotNull().toSet()
                // GitHub's unauthenticated release list sometimes answers 200 with an empty
                // page; taking that as "no companions" cleared every badge and the "not in
                // your library" section, so an empty answer keeps the last catalog.
                if (fetched.isEmpty()) {
                    Log.warning(
                        "[DualScreen] GitHub returned ${releases.size} releases and no companion; " +
                            "keeping ${_dualScreenTitleIds.value.size} cached titles"
                    )
                    return@launch
                }
                val ids = fetched + bundledTitleIds
                Log.info(
                    "[DualScreen] catalog: ${releases.size} releases, ${fetched.size} titles " +
                        "online, ${ids.size} with the bundled list"
                )
                latestVersions = latest.entries.mapNotNull { (id, release) ->
                    DualScreenModStatus.releaseVersion(release)?.let { id!! to it }
                }.toMap()
                preferences.edit().putStringSet(DUAL_SCREEN_TITLE_IDS, ids)
                    .putString("DualScreenVersions", Json.encodeToString(latestVersions)).apply()
                _dualScreenTitleIds.value = ids
                refreshInstalledCompanions()
            } catch (e: kotlinx.coroutines.CancellationException) {
                throw e
            } catch (e: Exception) {
                // Keep the last successful catalog when offline or GitHub is unavailable.
                Log.warning("[DualScreen] catalog refresh failed: ${e.message}")
            } finally {
                catalogRefreshing.set(false)
            }
        }
    }
}
