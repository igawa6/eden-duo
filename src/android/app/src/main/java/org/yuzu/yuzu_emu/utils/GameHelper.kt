// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.SharedPreferences
import android.net.Uri
import android.provider.DocumentsContract
import androidx.preference.PreferenceManager
import kotlinx.serialization.Serializable
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.File
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GameDir
import org.yuzu.yuzu_emu.model.MinimalDocumentFile
import androidx.core.content.edit
import androidx.core.net.toUri

object GameHelper {
    private const val KEY_OLD_GAME_PATH = "game_path"
    const val KEY_GAMES = "Games"
    private const val KEY_GAME_CONTENT_CONTAINERS = "GameContentContainers"

    // FileSys::GetBaseTitleID: updates (+0x800), DLC (+0x1000 + n) and extra programs of a
    // multi-program title all share their base game's id once the low 13 bits are cleared.
    private const val BASE_TITLE_ID_MASK = 0x1FFFL.inv()

    var cachedGameList = mutableListOf<Game>()

    // The startup game-list scan runs on a background thread and can overlap a game launch
    // (the cached list is tappable at once, and front-ends launch straight into a cold app).
    // The lock keeps "clear the filesystem provider + re-add update/DLC folders" atomic
    // against a game boot, and once a game is booting the scan no longer clears the provider,
    // so the booting game can't lose its update.
    private val filesystemProviderLock = Any()

    @Volatile
    private var emulationActive = false

    // Whether the external content dirs are mounted, and whether every games folder has been
    // walked since the provider was last cleared; until then a booting game mounts those itself.
    @Volatile
    private var externalContentMounted = false

    @Volatile
    private var libraryScanCompleted = false

    // Update/DLC containers seen by the last full library scan, keyed by base title id. The scan
    // registers games-folder containers one by one as it walks, so a game booted mid-scan (cold
    // start from the cached list, a shortcut or a front-end) re-registers its own from here.
    @Serializable
    private data class ContentContainer(val uri: String, val gameFolder: Boolean)

    @Volatile
    private var contentContainers: Map<Long, List<ContentContainer>>? = null

    /**
     * Called on the emulation thread right before the native boot reads the filesystem provider.
     */
    fun onEmulationStarting(gamePath: String, programId: String) {
        // Keys may have been added since the app started (the intent path used to reload them).
        NativeLibrary.reloadKeys()
        synchronized(filesystemProviderLock) {
            emulationActive = true
            try {
                registerContentForBoot(gamePath, programId)
            } catch (e: Exception) {
                Log.warning("[GameHelper] Failed to register content for boot: ${e.message}")
            }
        }
    }

    private fun registerContentForBoot(gamePath: String, programId: String) {
        val mountedContainerUris = mutableSetOf<String>()
        if (!externalContentMounted) {
            mountExternalContentDirectories(mountedContainerUris)
        }
        val externalCount = mountedContainerUris.size

        val baseTitleId = (programId.toLongOrNull() ?: 0L) and BASE_TITLE_ID_MASK
        val known = if (baseTitleId != 0L) {
            loadContentContainers()[baseTitleId].orEmpty()
        } else {
            emptyList()
        }
        val registered = mutableListOf<String>()
        known.forEach {
            if (it.uri != gamePath && mountedContainerUris.add(it.uri)) {
                if (it.gameFolder) {
                    NativeLibrary.addGameFolderFileToFilesystemProvider(it.uri)
                } else {
                    NativeLibrary.addFileToFilesystemProvider(it.uri)
                }
                registered.add(it.uri)
            }
        }
        val beforeSiblings = mountedContainerUris.size
        // Before the first scan, or for a game the scan has no content for (a frontend launch of a
        // game outside the library folders), its own folder is the only place left to look.
        if (!libraryScanCompleted || known.isEmpty()) {
            mountGameFolderContent(Uri.parse(gamePath), mountedContainerUris)
        }

        Log.info(
            "[GameHelper] Boot content for ${baseTitleId.toString(16).padStart(16, '0')}: " +
                "${registered.size} known container(s) $registered, " +
                "${mountedContainerUris.size - beforeSiblings} folder sibling(s), " +
                "$externalCount external container(s)"
        )
    }

    private fun loadContentContainers(): Map<Long, List<ContentContainer>> {
        contentContainers?.let { return it }
        val stored = PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)
            .getString(KEY_GAME_CONTENT_CONTAINERS, null)
        val loaded = stored?.let {
            try {
                Json.decodeFromString<Map<Long, List<ContentContainer>>>(it)
            } catch (_: Exception) {
                null
            }
        } ?: emptyMap()
        contentContainers = loaded
        return loaded
    }

    private fun recordContentContainer(
        index: MutableMap<Long, MutableSet<ContentContainer>>,
        uri: String,
        programId: Long,
        gameFolder: Boolean
    ) {
        val baseTitleIds = if (programId != 0L) {
            longArrayOf(programId and BASE_TITLE_ID_MASK)
        } else {
            // DLC-only containers have no program id; read the title ids from the container.
            NativeLibrary.getContainerBaseTitleIds(uri)
        }
        baseTitleIds.forEach {
            if (it != 0L) {
                index.getOrPut(it) { linkedSetOf() }.add(ContentContainer(uri, gameFolder))
            }
        }
    }

    fun onEmulationStopped() {
        emulationActive = false
    }

    private lateinit var preferences: SharedPreferences

    fun getGames(): List<Game> {
        val games = mutableListOf<Game>()
        val gamesByProgramId = mutableMapOf<String, Game>()
        val context = YuzuApplication.appContext
        preferences = PreferenceManager.getDefaultSharedPreferences(context)

        val gameDirs = mutableListOf<GameDir>()
        val oldGamesDir = preferences.getString(KEY_OLD_GAME_PATH, "") ?: ""
        if (oldGamesDir.isNotEmpty()) {
            gameDirs.add(GameDir(oldGamesDir, true))
            preferences.edit() { remove(KEY_OLD_GAME_PATH) }
        }
        gameDirs.addAll(NativeConfig.getGameDirs())

        // Ensure keys are loaded so that ROM metadata can be decrypted.
        NativeLibrary.reloadKeys()

        // Reset metadata so we don't use stale information
        GameMetadata.resetMetadata()

        val mountedContainerUris = mutableSetOf<String>()
        val contentIndex = mutableMapOf<Long, MutableSet<ContentContainer>>()
        synchronized(filesystemProviderLock) {
            // Remove previous filesystem provider information so we can get up to date version
            // info, unless a game is booting or running off these entries.
            if (!emulationActive) {
                NativeLibrary.clearFilesystemProvider()
                libraryScanCompleted = false
            }
            mountExternalContentDirectories(mountedContainerUris)
            externalContentMounted = true
        }
        // Outside the lock: reading the title ids opens every container again.
        mountedContainerUris.forEach { recordContentContainer(contentIndex, it, 0L, false) }

        val badDirs = mutableListOf<Int>()
        gameDirs.forEachIndexed { index: Int, gameDir: GameDir ->
            val gameDirUri = gameDir.uriString.toUri()
            val isValid = FileUtil.isTreeUriValid(gameDirUri)
            if (isValid) {
                val scanDepth = if (gameDir.deepScan) 3 else 1

                addGamesRecursive(
                    games,
                    gamesByProgramId,
                    FileUtil.listFiles(gameDirUri),
                    scanDepth,
                    mountedContainerUris,
                    contentIndex
                )
            } else {
                badDirs.add(index)
            }
        }

        // Remove all game dirs with insufficient permissions from config
        if (badDirs.isNotEmpty()) {
            var offset = 0
            badDirs.forEach {
                gameDirs.removeAt(it - offset)
                offset++
            }
        }
        NativeConfig.setGameDirs(gameDirs.toTypedArray())

        // Cache list of games found on disk
        val serializedGames = mutableSetOf<String>()
        games.forEach {
            serializedGames.add(Json.encodeToString(it))
        }
        val containersByTitle: Map<Long, List<ContentContainer>> =
            contentIndex.mapValues { it.value.toList() }
        preferences.edit() {
            remove(KEY_GAMES)
                .putStringSet(KEY_GAMES, serializedGames)
                .putString(KEY_GAME_CONTENT_CONTAINERS, Json.encodeToString(containersByTitle))
        }
        contentContainers = containersByTitle
        libraryScanCompleted = true

        cachedGameList = games.toMutableList()
        return games.toList()
    }

    fun restoreContentForGame(game: Game) {
        NativeLibrary.reloadKeys()

        synchronized(filesystemProviderLock) {
            val mountedContainerUris = mutableSetOf<String>()
            mountExternalContentDirectories(mountedContainerUris)
            mountGameFolderContent(Uri.parse(game.path), mountedContainerUris)
            NativeLibrary.addFileToFilesystemProvider(game.path)
        }
    }

    // File extensions considered as external content, buuut should
    // be done better imo.
    private val externalContentExtensions = setOf("nsp", "xci")

    private fun scanContentContainersRecursive(
        files: Array<MinimalDocumentFile>,
        depth: Int,
        onContainerFound: (MinimalDocumentFile) -> Unit
    ) {
        if (depth <= 0) {
            return
        }

        files.forEach {
            if (it.isDirectory) {
                scanContentContainersRecursive(
                    FileUtil.listFiles(it.uri),
                    depth - 1,
                    onContainerFound
                )
            } else {
                val extension = FileUtil.getExtension(it.uri).lowercase()
                if (externalContentExtensions.contains(extension)) {
                    onContainerFound(it)
                }
            }
        }
    }

    private fun addGamesRecursive(
        games: MutableList<Game>,
        gamesByProgramId: MutableMap<String, Game>,
        files: Array<MinimalDocumentFile>,
        depth: Int,
        mountedContainerUris: MutableSet<String>,
        contentIndex: MutableMap<Long, MutableSet<ContentContainer>>
    ) {
        if (depth <= 0) {
            return
        }

        files.forEach {
            if (it.isDirectory) {
                addGamesRecursive(
                    games,
                    gamesByProgramId,
                    FileUtil.listFiles(it.uri),
                    depth - 1,
                    mountedContainerUris,
                    contentIndex
                )
            } else {
                val extension = FileUtil.getExtension(it.uri).lowercase()
                val filePath = it.uri.toString()

                val mountedContainer = externalContentExtensions.contains(extension) &&
                    mountedContainerUris.add(filePath)
                if (mountedContainer) {
                    NativeLibrary.addGameFolderFileToFilesystemProvider(filePath)
                }

                if (Game.extensions.contains(extension)) {
                    val game = getGame(it.uri, true, false)
                    if (game != null) {
                        games.add(game)
                        if (game.programId != "0") {
                            gamesByProgramId[game.programId] = game
                        }
                    } else if (mountedContainer) {
                        val programId = GameMetadata.getProgramId(filePath).toLongOrNull()
                        recordContentContainer(contentIndex, filePath, programId ?: 0L, true)
                        programId?.let {
                            gamesByProgramId[(it and 0x800L.inv()).toString()]
                        }?.let { existingGame ->
                            NativeLibrary.getPatchesForFile(existingGame.path, existingGame.programId)
                            existingGame.version = GameMetadata.getVersion(
                                existingGame.path,
                                true
                            )
                            GameIconUtils.refreshGameIcon(existingGame)
                        }
                    }
                }
            }
        }
    }

    private fun mountExternalContentDirectories(mountedContainerUris: MutableSet<String>) {
        val uniqueExternalContentDirs = linkedSetOf<String>()
        NativeConfig.getExternalContentDirs().forEach { externalDir ->
            if (externalDir.isNotEmpty()) {
                uniqueExternalContentDirs.add(externalDir)
            }
        }

        for (externalDir in uniqueExternalContentDirs) {
            val externalDirUri = externalDir.toUri()
            if (FileUtil.isTreeUriValid(externalDirUri)) {
                scanContentContainersRecursive(FileUtil.listFiles(externalDirUri), 3) {
                    val containerUri = it.uri.toString()
                    if (mountedContainerUris.add(containerUri)) {
                        NativeLibrary.addFileToFilesystemProvider(containerUri)
                    }
                }
            }
        }
    }

    private fun mountGameFolderContent(gameUri: Uri, mountedContainerUris: MutableSet<String>) {
        if (gameUri.scheme == "content") {
            val parentUri = getParentDocumentUri(gameUri) ?: return
            scanContentContainersRecursive(FileUtil.listFiles(parentUri), 1) {
                val containerUri = it.uri.toString()
                if (mountedContainerUris.add(containerUri)) {
                    NativeLibrary.addGameFolderFileToFilesystemProvider(containerUri)
                }
            }
            return
        }

        val gameFile = File(gameUri.path ?: gameUri.toString())
        val parentDir = gameFile.parentFile ?: return
        parentDir.listFiles()?.forEach { sibling ->
            if (!sibling.isFile) {
                return@forEach
            }

            val extension = sibling.extension.lowercase()
            if (externalContentExtensions.contains(extension)) {
                val containerUri = Uri.fromFile(sibling).toString()
                if (mountedContainerUris.add(containerUri)) {
                    NativeLibrary.addGameFolderFileToFilesystemProvider(containerUri)
                }
            }
        }
    }

    private fun getParentDocumentUri(uri: Uri): Uri? {
        return try {
            val documentId = DocumentsContract.getDocumentId(uri)
            val separatorIndex = documentId.lastIndexOf('/')
            if (separatorIndex == -1) {
                null
            } else {
                val parentDocumentId = documentId.substring(0, separatorIndex)
                DocumentsContract.buildDocumentUriUsingTree(uri, parentDocumentId)
            }
        } catch (_: Exception) {
            null
        }
    }

    fun getGame(
        uri: Uri,
        addedToLibrary: Boolean,
        registerFilesystemProvider: Boolean = true
    ): Game? {
        // A file:// URI (adb `am start -d file://...`, some front-ends) must reach the native VFS
        // as a plain path: it only special-cases content:// and would otherwise try to open the
        // literal "file:///..." string.
        val filePath = if (uri.scheme == "file") (uri.path ?: uri.toString()) else uri.toString()
        if (!GameMetadata.getIsValid(filePath)) {
            return null
        }

        if (registerFilesystemProvider) {
            // Needed to update installed content information
            NativeLibrary.addFileToFilesystemProvider(filePath)
        }

        var name = GameMetadata.getTitle(filePath)

        // If the game's title field is empty, use the filename.
        if (name.isEmpty()) {
            name = FileUtil.getFilename(uri)
        }
        var programId = GameMetadata.getProgramId(filePath)

        // If the game's ID field is empty, use the filename without extension.
        if (programId.isEmpty()) {
            programId = name.substring(0, name.lastIndexOf("."))
        }

        val newGame = Game(
            name,
            filePath,
            programId,
            GameMetadata.getDeveloper(filePath),
            GameMetadata.getVersion(filePath, false),
            GameMetadata.getIsHomebrew(filePath)
        )

        if (addedToLibrary) {
            val addedTime = preferences.getLong(newGame.keyAddedToLibraryTime, 0L)
            if (addedTime == 0L) {
                preferences.edit()
                    .putLong(newGame.keyAddedToLibraryTime, System.currentTimeMillis())
                    .apply()
            }
        }

        return newGame
    }
}
