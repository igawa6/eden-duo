// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.SharedPreferences
import android.net.Uri
import android.provider.DocumentsContract
import androidx.preference.PreferenceManager
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
import org.yuzu.yuzu_emu.duo.GameLibraryContent

object GameHelper {
    private const val KEY_OLD_GAME_PATH = "game_path"
    const val KEY_GAMES = "Games"

    var cachedGameList = mutableListOf<Game>()

    private lateinit var preferences: SharedPreferences

    // Eden Duo: update/DLC registration at boot and the data-import lock (GameLibraryContent).
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

    fun <T> withDataImport(action: () -> T): T = GameLibraryContent.withDataImportWrite(action)

    fun isEmulationActive(): Boolean = emulationActive

    /**
     * Called on the emulation thread right before the native boot reads the filesystem provider.
     */
    fun onEmulationStarting(gamePath: String, programId: String) =
        GameLibraryContent.withDataImportRead {
            // Keys may have been added since the app started (the intent path used to reload them).
            NativeLibrary.reloadKeys()
            synchronized(filesystemProviderLock) {
                emulationActive = true
                try {
                    GameLibraryContent.registerContentForBoot(
                        gamePath,
                        programId,
                        externalContentMounted,
                        libraryScanCompleted,
                        externalContentExtensions,
                        ::mountExternalContentDirectories,
                        ::mountGameFolderContent
                    )
                } catch (e: Exception) {
                    Log.warning("[GameHelper] Failed to register content for boot: ${e.message}")
                }
            }
        }

    fun onEmulationStopped() {
        emulationActive = false
    }

    fun invalidateAfterDataImport() = GameLibraryContent.withDataImportWrite {
        synchronized(filesystemProviderLock) {
            cachedGameList.clear()
            externalContentMounted = false
            libraryScanCompleted = false
            GameMetadata.resetMetadata()
            PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext).edit {
                remove(KEY_GAMES)
                GameLibraryContent.forget(this)
                remove(KEY_OLD_GAME_PATH)
            }
        }
    }

    fun getGames(): List<Game> = GameLibraryContent.withDataImportRead {
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
        val contentIndex = GameLibraryContent.ScanIndex()
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
        mountedContainerUris.forEach { contentIndex.record(it, 0L, false) }

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
        preferences.edit() {
            remove(KEY_GAMES)
                .putStringSet(KEY_GAMES, serializedGames)
            GameLibraryContent.store(this, contentIndex)
        }
        GameLibraryContent.remember(contentIndex)
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
    private val externalContentExtensions = setOf("nsp", "xci", "nsz", "xcz")

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
                val extension = ContainerNameRules.extension(it.filename)
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
        contentIndex: GameLibraryContent.ScanIndex
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
                val extension = ContainerNameRules.extension(it.filename)
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
                        contentIndex.record(filePath, programId ?: 0L, true)
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
                // The native VFS opens plain paths only (see getGame): a file:// URI here was
                // silently never mounted, so an update next to a file:// launched game was lost.
                val containerUri = sibling.absolutePath
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
