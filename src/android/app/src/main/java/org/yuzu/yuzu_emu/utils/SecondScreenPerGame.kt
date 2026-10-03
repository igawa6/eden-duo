// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import androidx.core.content.edit
import androidx.core.os.BundleCompat
import androidx.preference.PreferenceManager
import kotlinx.serialization.json.Json
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.utils.SettingsFile
import org.yuzu.yuzu_emu.model.Game

/**
 * Eden Duo: per-game values for the Second Screen settings that must be known before a game's
 * window opens (Swap Screens, No Companion), when the per-game .ini is not loaded yet. They live
 * in Eden Duo's own SharedPreferences as "<setting>@<16-digit title id>"; no key means the game
 * uses the global value. Companion Ratio is not here: it is a native per-game setting.
 */
object SecondScreenPerGame {
    const val PREFS = "eden_duo_dual_screen"

    // Navigation arguments of the emulation activity (home_navigation.xml).
    private const val ARG_GAME = "game"
    private const val ARG_CUSTOM = "custom"

    fun prefs(context: Context) = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** The canonical sixteen-digit title id of [programId] (decimal), or null for none. */
    fun titleHex(programId: String?): String? =
        programId?.toULongOrNull()?.takeIf { it != 0UL }
            ?.toString(16)?.uppercase()?.padStart(16, '0')

    /** The per-game key of [setting] for [programId], or null when there is no real title id. */
    fun key(setting: String, programId: String?): String? =
        titleHex(programId)?.let { "$setting@$it" }

    /**
     * The game whose per-game .ini the running emulation booted with, or null when it runs on
     * global settings. Set by EmulationFragment at boot; only meaningful while a game runs.
     */
    @Volatile
    var runningConfigGame: Game? = null

    /**
     * Puts the running game's Companion Ratio back after a game's settings page was open mid-game:
     * that page loads its own title's .ini over the in-memory settings, which would otherwise
     * reach the running game on the next applySettings. Done as EmulationFragment does at boot,
     * so editing the running game's own .ini still applies (it is read back from the saved file).
     */
    fun restoreRunningGameConfig() {
        IntSetting.COMPANION_RATIO.global = true
        val game = runningConfigGame ?: return
        SettingsFile.loadCustomConfig(game)
        NativeConfig.unloadPerGameConfig()
    }

    fun hasOverrides(context: Context): Boolean = prefs(context).all.keys.any { '@' in it }

    /**
     * Drops the global values kept here (Swap Screens, the No Companion app) for a reset of the
     * global settings; per-game values stay, as the games' own .ini files do.
     */
    fun clearGlobal(context: Context) {
        val prefs = prefs(context)
        val keys = prefs.all.keys.filter { '@' !in it }
        if (keys.isNotEmpty()) prefs.edit { keys.forEach { remove(it) } }
    }

    /** Drops every per-game Second Screen value of [programId] (the game uses global again). */
    fun clearTitle(context: Context, programId: String?) {
        val hex = titleHex(programId) ?: return
        val prefs = prefs(context)
        val keys = prefs.all.keys.filter { it.endsWith("@$hex") }
        if (keys.isNotEmpty()) prefs.edit { keys.forEach { remove(it) } }
    }

    /**
     * Who a launch starts, told cheaply before the game screen exists (the ROM itself is never
     * parsed here: this runs on the UI thread in EmulationActivity.onCreate).
     *  [programId]: the title, or null when unknown;
     *  [perGame]: false for a "launch with global settings", which ignores per-game values.
     */
    class Launch(val programId: String?, val perGame: Boolean) {
        /** The title whose per-game values apply, or null for global. */
        val perGameId: String? get() = programId.takeIf { perGame }
    }

    fun resolveLaunch(intent: Intent): Launch {
        try {
            val extras = intent.extras
            if (extras != null) {
                BundleCompat.getParcelable(extras, ARG_GAME, Game::class.java)?.let {
                    return Launch(usableProgramId(it), extras.getBoolean(ARG_CUSTOM, false))
                }
                BundleCompat.getParcelable(
                    extras,
                    EmulationActivity.EXTRA_SELECTED_GAME,
                    Game::class.java
                )?.let { return Launch(usableProgramId(it), true) }
            }
            if (intent.action == CustomSettingsHandler.CUSTOM_CONFIG_ACTION) {
                val hex = intent.getStringExtra(CustomSettingsHandler.EXTRA_TITLE_ID)
                val id = hex?.toULongOrNull(16)?.takeIf { it != 0UL }?.toString()
                return Launch(id, true)
            }
            val uri = intent.data ?: return Launch(null, true)
            val id = cachedProgramId(uri)
            if (id == null) {
                val doc = documentId(uri) ?: "(${uri.scheme} uri)"
                Log.info("[ScreenSwap] per-game lookup: no cached game for document $doc")
            }
            return Launch(id, true)
        } catch (e: Exception) {
            return Launch(null, true)
        }
    }

    /**
     * The document id of a SAF URI (a tree-addressed document or a plain document URI), or null.
     * Two URIs naming one document through different trees share it: a frontend such as CocoonFE
     * hands over tree/primary:ROMs/document/primary:ROMs/switch/x.nsp for the game the library
     * holds as tree/primary:ROMs/switch/document/primary:ROMs/switch/x.nsp.
     */
    fun documentId(uri: Uri): String? {
        if (uri.scheme != "content") return null
        return try {
            DocumentsContract.getDocumentId(uri)
        } catch (e: Exception) {
            null
        }
    }

    /** The game list cache decoded once: path -> game, and "authority|document id" -> game. */
    private class GameIndex(
        val source: Set<String>,
        val byPath: Map<String, Game>,
        val byDocument: Map<String, Game>
    )

    @Volatile
    private var gameIndex: GameIndex? = null

    private fun documentKey(uri: Uri): String? =
        documentId(uri)?.let { "${uri.authority}|$it" }

    /**
     * The decoded game list cache, rebuilt only when the cached set changes (SharedPreferences
     * hands back the same set object until the game list is saved again).
     */
    private fun gameIndex(): GameIndex? {
        val cached = PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)
            .getStringSet(GameHelper.KEY_GAMES, null) ?: return null
        gameIndex?.let { if (it.source === cached) return it }
        val byPath = HashMap<String, Game>()
        val byDocument = HashMap<String, Game>()
        for (entry in cached) {
            val game = try {
                Json.decodeFromString<Game>(entry)
            } catch (e: Exception) {
                continue
            }
            byPath.putIfAbsent(game.path, game)
            documentKey(Uri.parse(game.path))?.let { byDocument.putIfAbsent(it, game) }
        }
        return GameIndex(cached, byPath, byDocument).also { gameIndex = it }
    }

    /** The game list cache entry for [uri]: same path, or the same document by any tree. */
    private fun cachedProgramId(uri: Uri): String? {
        val path = if (uri.scheme == "file") (uri.path ?: uri.toString()) else uri.toString()
        val index = gameIndex() ?: return null
        index.byPath[path]?.let { return usableProgramId(it) }
        val docKey = documentKey(uri) ?: return null
        return index.byDocument[docKey]?.let {
            Log.info("[ScreenSwap] per-game lookup: document ${documentId(uri)} is in the game list")
            usableProgramId(it)
        }
    }

    /** A real title id (GameHelper falls back to the file name when a ROM has none). */
    fun usableProgramId(game: Game): String? =
        game.programId.takeIf { (it.toULongOrNull() ?: 0UL) != 0UL }
}
