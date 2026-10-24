// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.model

import android.app.Application
import android.net.Uri
import android.provider.DocumentsContract
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import androidx.preference.PreferenceManager
import java.io.File
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.utils.DirectoryInitialization
import org.yuzu.yuzu_emu.utils.EmulatorDataImporter
import org.yuzu.yuzu_emu.utils.EmulatorImportConfig
import org.yuzu.yuzu_emu.utils.EmulatorImportSources
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.NativeConfig

class EmulatorImportViewModel(application: Application) : AndroidViewModel(application) {
    data class FolderChoice(
        val folder: EmulatorImportConfig.Folder,
        val copied: Boolean = false,
        val skipped: Boolean = false,
        val linkedUri: String? = null
    ) {
        val resolved get() = copied || skipped || linkedUri != null
    }
    data class State(
        val busy: Boolean = false,
        val applying: Boolean = false,
        val ready: Boolean = false,
        val complete: Boolean = false,
        val reviewLocked: Boolean = false,
        val message: String = "",
        val files: Long = 0,
        val bytes: Long = 0,
        val folders: List<FolderChoice> = emptyList(),
        val backupAvailable: Boolean = false,
        /** When the restorable backup was made (epoch ms) and its size, for the Restore button. */
        val backupTime: Long = 0,
        val backupBytes: Long = 0,
        /** Set once by a finished import when more than one user profile exists. */
        val askProfile: Boolean = false
    )

    private val context get() = getApplication<Application>()
    private val userDirectory get() = File(requireNotNull(DirectoryInitialization.userDirectory))
    private val _state = MutableStateFlow(State())
    val state = _state.asStateFlow()
    private val cancelled = AtomicBoolean(false)
    private val cleared = AtomicBoolean(false)
    private var session: EmulatorDataImporter.Session? = null
    private var sourceAuthority: String? = null
    private var configPrepared = false
    private var lastProgressTime = 0L
    var pendingUseExisting = false
    var pendingSource: EmulatorImportSources.Source? = null
    var pendingFolder: EmulatorImportConfig.Folder? = null
    var pendingAdditionalKind: EmulatorImportConfig.Kind? = null

    init {
        viewModelScope.launch(Dispatchers.IO) { refreshBackupAvailable() }
    }

    /** Disk I/O: call off the main thread. */
    private fun refreshBackupAvailable() {
        val backup = runCatching { EmulatorDataImporter.latestBackup(userDirectory) }.getOrNull()
        val time = backup?.let { File(it.directory, "state").lastModified() } ?: 0L
        val bytes = backup?.let { session ->
            runCatching { session.previous.walkTopDown().filter { it.isFile }.sumOf { it.length() } }
                .getOrDefault(0L)
        } ?: 0L
        _state.update { it.copy(backupAvailable = backup != null, backupTime = time, backupBytes = bytes) }
    }

    /** Fails before a long copy instead of after it: the replacement needs no running game. */
    private fun requireNoGameRunning() {
        if (GameHelper.isEmulationActive() || NativeLibrary.isRunning()) {
            throw IOException(context.getString(R.string.emulator_import_game_running))
        }
    }

    fun profileShown() {
        _state.update { it.copy(askProfile = false) }
    }

    fun prepare(tree: Uri, isZip: Boolean) {
        if (_state.value.busy) return
        val source = if (isZip) null else pendingSource
        runOperation(reset = true) {
            requireNoGameRunning()
            if (source != null &&
                (
                    tree.authority != source.authority || DocumentsContract.getTreeDocumentId(tree).trimEnd(
                            '/'
                        ) != "root"
                    )
            ) {
                throw IOException(
                    context.getString(R.string.emulator_import_wrong_folder, source.label)
                )
            }
            session?.discard()
            val candidate = EmulatorDataImporter.create(userDirectory)
            session = candidate
            configPrepared = false
            sourceAuthority = source?.authority
            if (isZip) {
                val input = context.contentResolver.openInputStream(tree) ?: throw IOException(
                    "Cannot open the backup"
                )
                EmulatorDataImporter.unpack(candidate, input, ::progress)
            } else {
                EmulatorImportSources.copyTree(context, tree, candidate, progress = ::progress)
                EmulatorDataImporter.validate(candidate)
            }
            val folders = EmulatorImportConfig.requestedFolders(candidate.payload, sourceAuthority).map {
                FolderChoice(
                    it
                )
            }
            _state.value = State(
                ready = true,
                message = context.getString(R.string.emulator_import_review),
                files = candidate.files,
                bytes = candidate.bytes,
                folders = folders
            )
        }
    }

    fun initialFolderUri(folder: EmulatorImportConfig.Folder): Uri? {
        if (folder.original.startsWith("content://")) return Uri.parse(folder.original)
        val relative = EmulatorImportConfig.internalRelative(folder.original, sourceAuthority)
            ?: if (folder.original == "default:saves") "nand" else return null
        return sourceAuthority?.let { Uri.parse(EmulatorImportConfig.treeUri(it, relative)) }
    }

    fun useCopiedSaves(): Boolean {
        val folder = pendingFolder ?: return false
        if (folder.kind != EmulatorImportConfig.Kind.SAVES ||
            (EmulatorImportConfig.internalRelative(folder.original, sourceAuthority) == null && folder.original != "default:saves")) return false
        _state.value = _state.value.copy(folders = _state.value.folders.map {
            if (it.folder == folder) it.copy(copied = true) else it
        })
        pendingFolder = null
        return true
    }

    fun copyFolder(tree: Uri) {
        val candidate = session ?: return
        val requested = pendingFolder
        val additional = pendingAdditionalKind
        if (_state.value.busy || _state.value.reviewLocked || (requested == null && additional == null)) return
        val folder = requested ?: EmulatorImportConfig.Folder(
            "additional:${System.nanoTime()}",
            "imported/extra-${System.nanoTime()}",
            requireNotNull(additional)
        )
        if (pendingUseExisting) {
            if (folder.kind !in setOf(EmulatorImportConfig.Kind.GAMES, EmulatorImportConfig.Kind.CONTENT, EmulatorImportConfig.Kind.SAVES) ||
                tree.authority == "${context.packageName}.user" ||
                context.contentResolver.persistedUriPermissions.none { it.uri == tree && it.isReadPermission &&
                    (folder.kind != EmulatorImportConfig.Kind.SAVES || it.isWritePermission) }) {
                _state.value = _state.value.copy(message = context.getString(if (folder.kind == EmulatorImportConfig.Kind.SAVES) R.string.emulator_import_save_access_failed else R.string.emulator_import_link_failed))
                return
            }
            val choice = FolderChoice(folder, linkedUri = tree.toString())
            _state.value = _state.value.copy(folders = if (requested == null) _state.value.folders + choice else
                _state.value.folders.map { if (it.folder == folder) choice else it })
            pendingFolder = null
            pendingAdditionalKind = null
            return
        }
        runOperation {
            requireNoGameRunning()
            try {
                EmulatorImportSources.copyTree(
                    context,
                    tree,
                    candidate,
                    folder.destination,
                    ::progress
                )
            } catch (e: Exception) {
                candidate.removeFolder(folder.destination)
                throw e
            }
            val old = _state.value.folders
            val updated = if (requested == null) {
                old + FolderChoice(folder, copied = true)
            } else {
                old.map {
                    if (it.folder == folder) it.copy(copied = true, skipped = false) else it
                }
            }
            _state.value = _state.value.copy(
                busy = false,
                ready = true,
                message = context.getString(R.string.emulator_import_review),
                folders = updated,
                files = candidate.files,
                bytes = candidate.bytes
            )
            pendingFolder = null
            pendingAdditionalKind = null
        }
    }

    fun skipFolder(folder: EmulatorImportConfig.Folder) {
        if (_state.value.busy) return
        _state.value = _state.value.copy(
            folders = _state.value.folders.map {
                if (it.folder == folder) it.copy(skipped = true) else it
            }
        )
    }

    fun applyImport(restore: Boolean = false) {
        if (_state.value.busy) return
        if (!restore && _state.value.folders.any { !it.resolved }) return
        runOperation(applying = true) {
            if (!restore && !configPrepared) {
                _state.value = _state.value.copy(reviewLocked = true)
                val candidate = requireNotNull(session)
                val copied = _state.value.folders.filter { it.copied || it.linkedUri != null }
                EmulatorImportConfig.rewrite(
                    candidate.payload,
                    userDirectory,
                    "${context.packageName}.user",
                    sourceAuthority,
                    copied.associate { it.folder.selectionKey to (it.linkedUri ?: it.folder.destination) },
                    copied.filter { it.folder.original.startsWith("additional:") }.map { it.folder.kind to (it.linkedUri ?: it.folder.destination) }
                )
                configPrepared = true
            }
            GameHelper.withDataImport {
                synchronized(NativeConfig) {
                    if (GameHelper.isEmulationActive() || NativeLibrary.isRunning() || NativeConfig.isPerGameConfigLoaded()) {
                        throw IOException(context.getString(R.string.emulator_import_game_running))
                    }
                    NativeConfig.saveGlobalConfig()
                    NativeConfig.unloadGlobalConfig()
                    try {
                        if (restore) {
                            val backup = EmulatorDataImporter.latestBackup(userDirectory)
                                ?: throw IOException("The previous data backup is unavailable")
                            val restored = EmulatorDataImporter.restore(
                                backup,
                                userDirectory,
                                ::reloadNative
                            )
                            // A staged import still under review is replaced by the restore.
                            session?.discard()
                            session = restored
                        } else {
                            EmulatorDataImporter.apply(
                                requireNotNull(session),
                                userDirectory,
                                ::reloadNative
                            )
                        }
                    } catch (e: Throwable) {
                        runCatching { reloadNative() }
                        throw e
                    }
                    GameHelper.invalidateAfterDataImport()
                }
            }
            // Only the backup made by this replacement can be restored; drop older sessions.
            runCatching { EmulatorDataImporter.prune(userDirectory, session?.directory) }
            val preferences = PreferenceManager.getDefaultSharedPreferences(context)
            preferences.edit().putLong(IMPORT_GENERATION, System.currentTimeMillis()).apply()
            _state.value = _state.value.copy(
                busy = false,
                applying = false,
                ready = false,
                complete = true,
                askProfile = !restore && runCatching { NativeLibrary.getUserCount() > 1 }.getOrDefault(false),
                message = context.getString(
                    if (restore) R.string.emulator_import_restored else R.string.emulator_import_complete
                )
            )
        }
    }

    private fun reloadNative() {
        NativeLibrary.setAppDirectory(userDirectory.path)
        NativeConfig.initializeGlobalConfig()
        NativeLibrary.initializeSystem(true)
        NativeLibrary.reloadKeys()
        NativeLibrary.reloadProfiles()
    }

    private fun runOperation(reset: Boolean = false, applying: Boolean = false, action: () -> Unit) {
        cancelled.set(false)
        _state.value = (if (reset) State() else _state.value).copy(
            busy = true,
            applying = applying,
            complete = false,
            message = context.getString(
                if (applying) R.string.emulator_import_applying else R.string.emulator_import_copying
            )
        )
        viewModelScope.launch(Dispatchers.IO) {
            try {
                action()
            } catch (e: Throwable) {
                // Also an Error (out of memory during a large copy): busy must clear either way.
                if (reset) {
                    session?.discard()
                    session = null
                }
                _state.value = _state.value.copy(
                    busy = false,
                    applying = false,
                    message = if (e is EmulatorDataImporter.Cancelled) {
                        context.getString(
                            R.string.emulator_import_cancelled
                        )
                    } else {
                        context.getString(R.string.emulator_import_failed, e.message.orEmpty())
                    }
                )
            } finally {
                if (cleared.get()) session?.discard()
                refreshBackupAvailable()
            }
        }
    }

    private fun progress(files: Long, bytes: Long, path: String): Boolean {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastProgressTime > 200) {
            _state.value = _state.value.copy(files = files, bytes = bytes, message = path)
            lastProgressTime = now
        }
        return cancelled.get()
    }

    fun cancel() {
        if (!_state.value.applying) cancelled.set(true)
    }

    override fun onCleared() {
        cleared.set(true)
        cancelled.set(true)
        if (!_state.value.busy) session?.discard()
        super.onCleared()
    }

    companion object {
        const val IMPORT_GENERATION = "EmulatorDataImportGeneration"
    }
}
