// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import android.net.Uri
import android.os.Build
import androidx.documentfile.provider.DocumentFile
import org.json.JSONArray
import org.json.JSONObject
import org.yuzu.yuzu_emu.NativeLibrary
import java.io.File
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream
import java.math.BigDecimal
import java.math.BigInteger
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream

/** Installs the standalone, declarative dual-screen package format. */
object DualScreenPackageInstaller {
    private const val PACKAGE_TYPE = "dual-screen-mod"
    private const val PACKAGE_FORMAT = 1
    private const val MANIFEST_FORMAT = 1
    private const val MAX_ARCHIVE_BYTES = 512L * 1024 * 1024
    private const val MAX_EXPANDED_BYTES = 1024L * 1024 * 1024
    private const val MAX_ENTRY_BYTES = 512L * 1024 * 1024
    private const val MAX_ENTRIES = 10000
    private const val MAX_METADATA_BYTES = 4L * 1024 * 1024
    private const val PROGRESS_STEP = 256L * 1024
    private const val BACKUP_PREFIX = ".DualScreen-"
    private const val BACKUP_SUFFIX = ".backup"
    private const val PACKAGE_JSON = "package.json"
    private const val DUALSCREEN_DIR = "dualscreen"
    private const val MANIFEST_JSON = "$DUALSCREEN_DIR/manifest.json"
    private const val MODULE_DIR = "$DUALSCREEN_DIR/modules"

    // The file name is advisory: package.json carries the identity. The published names are
    // <TITLEID>-<Name>-<version>.dsmod.zip (installed as the add-on folder "<Name>-<version>",
    // whose version must equal package.json's) and the older <TITLEID>.dsmod.zip (installed as
    // "DualScreen-<TITLEID>"). A browser's " (1)" copy suffix is ignored. Any other name installs
    // as "<package name>-<version>"; a title ID at the start of the name must match the game.
    private val namedArchive =
        Regex("^([0-9A-Fa-f]{16})(?:-([A-Za-z0-9_]{1,64})-([0-9]+\\.[0-9]+\\.[0-9]+))?$")
    private val leadingTitleId = Regex("^([0-9A-Fa-f]{16})(?![0-9A-Za-z])")
    private val copySuffix = Regex("\\s*\\(\\d+\\)")
    private const val LEGACY_FOLDER_PREFIX = "DualScreen-"
    private val titleId = Regex("^[0-9A-F]{16}$")
    private val version = Regex("^[0-9]+\\.[0-9]+\\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")

    /** A package's "min_runtime" that cannot be read as a number (the runtime's u32 max). */
    const val UNKNOWN_RUNTIME = 0xFFFFFFFFL

    enum class Error {
        ArchiveTooLarge,
        MalformedArchive,
        UnsafeEntry,
        DuplicateEntry,
        InvalidLayout,
        InvalidMetadata,
        TitleMismatch,

        /** The package's min_runtime is newer than this app's dual-screen runtime. */
        RuntimeTooOld,

        /** The package has a native module but no build of it for this device. */
        MissingPlatformLibrary,

        /** A native module file does not match its declared SHA-256. */
        ChecksumMismatch,

        /** The version in the file name differs from package.json's. */
        VersionMismatch,

        /** The add-on directory cannot be written ([Result.Failed.detail] = the path). */
        CannotWrite,
        Cancelled,
        InstallFailed
    }

    sealed class Result {
        /**
         * The package is active as [folderName]. [removed] are this title's earlier installs that
         * were deleted; [notRemoved] could not be deleted (for example files left by adb) and
         * may still be picked instead. [otherPackages] are other dual-screen folders for this
         * title that the runtime could pick instead of the new one (it takes the first enabled
         * folder by name); the caller disables them.
         */
        data class Installed(
            val folderName: String,
            val removed: List<String> = emptyList(),
            val notRemoved: List<String> = emptyList(),
            val otherPackages: List<OtherPackage> = emptyList()
        ) : Result()

        /**
         * [detail] names what was wrong (a path, key, platform or hash) for the message and the
         * log. [requiredRuntime] and [runtime] are set for [Error.RuntimeTooOld].
         */
        data class Failed(
            val error: Error,
            val detail: String = "",
            val requiredRuntime: Long = -1L,
            val runtime: Int = -1
        ) : Result()
    }

    private fun failed(error: Error, detail: String = "") = Result.Failed(error, detail)

    /**
     * A dual-screen folder the installer did not create for this install. [onlyDualScreen] is
     * true when it holds nothing but the dual-screen package, so disabling it cannot switch off
     * an unrelated romfs/exefs mod or cheat in the same folder.
     */
    data class OtherPackage(val folderName: String, val onlyDualScreen: Boolean)

    /** What a file name says about the package; every part is optional. */
    internal data class ArchiveName(
        val titleId: String? = null,
        val label: String? = null,
        val version: String? = null,
        /** The whole name is one of the published forms. */
        val exact: Boolean = false
    ) {
        /** Exactly "<TITLEID>.dsmod.zip": the original single-package name. */
        val isLegacy get() = exact && label == null
    }

    internal fun parseArchiveName(filename: String): ArchiveName {
        val cleaned = filename.replace(copySuffix, "").trim()
        val stem = when {
            cleaned.endsWith(".dsmod.zip", ignoreCase = true) -> cleaned.dropLast(10)
            cleaned.endsWith(".zip", ignoreCase = true) -> cleaned.dropLast(4)
            else -> cleaned
        }
        namedArchive.matchEntire(stem)?.let { match ->
            return ArchiveName(
                match.groupValues[1].uppercase(),
                match.groupValues[2].ifEmpty { null },
                match.groupValues[3].ifEmpty { null },
                exact = true
            )
        }
        return ArchiveName(leadingTitleId.find(stem)?.groupValues?.get(1)?.uppercase())
    }

    /** The add-on folder a package installs as (see [namedArchive]). */
    private fun folderNameFor(name: ArchiveName, gameId: String, packageRoot: File): String {
        if (name.label != null && name.version != null) return "${name.label}-${name.version}"
        if (name.isLegacy) return "$LEGACY_FOLDER_PREFIX$gameId"
        val json = readJsonObject(File(packageRoot, PACKAGE_JSON))
        val label = (json?.opt("name") as? String).orEmpty()
            .filter { it in 'A'..'Z' || it in 'a'..'z' || it in '0'..'9' || it == '_' }
            .take(64)
        val packageVersion = json?.opt("version") as? String
        return if (label.isEmpty() || packageVersion == null) {
            "$LEGACY_FOLDER_PREFIX$gameId"
        } else {
            "$label-$packageVersion"
        }
    }

    /** An I/O failure on a known path of the add-on directory. */
    private class WriteException(val path: File, message: String) : IOException(message)

    private data class Library(val path: String, val sha256: String)

    private data class ModuleDeclaration(
        val buildIds: List<String>,
        val libraries: Map<String, Library>
    )

    private fun log(message: String, error: Throwable? = null) {
        val text = "[DSModInstaller] $message" + (error?.let { ": $it" } ?: "")
        try {
            if (error != null) Log.error(text) else Log.info(text)
        } catch (_: Throwable) {
            // No native logger (JVM unit tests).
        }
    }

    /** The app's dual-screen runtime version, or null when it cannot be asked. */
    private fun appRuntimeVersion(): Int? = try {
        NativeLibrary.dualScreenRuntimeVersion()
    } catch (e: Throwable) {
        log("cannot read the dual-screen runtime version", e)
        null
    }

    /**
     * The runtime a package asks for: the larger "min_runtime" of the manifest and package.json,
     * each an integer or a digit string. Same rule as the core's PackageMinRuntime: a value that
     * is present but not readable counts as [UNKNOWN_RUNTIME].
     */
    internal fun packageMinRuntime(manifest: JSONObject?, packageJson: JSONObject?): Long =
        maxOf(minRuntimeOf(manifest), minRuntimeOf(packageJson))

    private fun minRuntimeOf(json: JSONObject?): Long {
        if (json == null || !json.has("min_runtime")) return 0L
        return when (val value = json.opt("min_runtime")) {
            null, JSONObject.NULL -> 0L
            is Int, is Long, is Short, is Byte -> {
                val v = (value as Number).toLong()
                if (v < 0) UNKNOWN_RUNTIME else minOf(v, UNKNOWN_RUNTIME)
            }
            is BigInteger ->
                if (value.signum() < 0 || value.bitLength() > 32) {
                    UNKNOWN_RUNTIME
                } else {
                    minOf(value.toLong(), UNKNOWN_RUNTIME)
                }
            is String ->
                if (value.isEmpty() || value.length > 9 || value.any { it !in '0'..'9' }) {
                    UNKNOWN_RUNTIME
                } else {
                    value.toLong()
                }
            is Double, is Float, is BigDecimal -> UNKNOWN_RUNTIME
            else -> UNKNOWN_RUNTIME
        }
    }

    /**
     * Validates and installs [uri] into the game's add-on directory. Extraction happens in a
     * sibling staging directory, and the existing package is retained until the new one has been
     * completely validated and atomically renamed into place.
     */
    fun install(
        context: Context,
        uri: Uri,
        gameTitleId: String,
        addonDirectory: File,
        progressCallback: (max: Long, progress: Long) -> Boolean = { _, _ -> false }
    ): Result {
        return try {
            val filename = DocumentFile.fromSingleUri(context, uri)?.name.orEmpty()
            val archiveSize = DocumentFile.fromSingleUri(context, uri)?.length() ?: -1L
            val input = context.contentResolver.openInputStream(uri)
                ?: return failed(Error.InstallFailed, "cannot open $filename")
                    .also { log("cannot open $uri") }
            installArchive(
                input, filename, archiveSize, gameTitleId, addonDirectory, progressCallback,
                appRuntimeVersion()
            )
        } catch (e: Exception) {
            log("install of $uri failed", e)
            failed(Error.InstallFailed, e.message ?: e.javaClass.simpleName)
        }
    }

    /**
     * [runtimeVersion] is the app's dual-screen runtime: a package whose min_runtime is newer is
     * refused and the installed version kept. Null skips the check (the runtime still gates).
     */
    @Synchronized
    internal fun installArchive(
        input: InputStream,
        filename: String,
        archiveSize: Long,
        gameTitleId: String,
        addonDirectory: File,
        progressCallback: (max: Long, progress: Long) -> Boolean = { _, _ -> false },
        runtimeVersion: Int? = null
    ): Result {
        val result = installArchiveLocked(
            input, filename, archiveSize, gameTitleId, addonDirectory, progressCallback,
            runtimeVersion
        )
        when (result) {
            is Result.Installed -> log("installed $filename as ${result.folderName}")
            is Result.Failed -> if (result.error != Error.Cancelled) {
                log("$filename not installed: ${result.error} ${result.detail}".trimEnd())
            }
        }
        return result
    }

    private fun installArchiveLocked(
        input: InputStream,
        filename: String,
        archiveSize: Long,
        gameTitleId: String,
        addonDirectory: File,
        progressCallback: (max: Long, progress: Long) -> Boolean,
        runtimeVersion: Int?
    ): Result {
        input.use {
            if (archiveSize > MAX_ARCHIVE_BYTES) {
                return failed(Error.ArchiveTooLarge, "$archiveSize bytes")
            }

            val gameId = gameTitleId.uppercase()
            if (!titleId.matches(gameId)) {
                return failed(Error.TitleMismatch, "game title ID $gameTitleId")
            }
            val archiveName = parseArchiveName(filename)
            if (archiveName.titleId != null && archiveName.titleId != gameId) {
                return failed(Error.TitleMismatch, "file is for ${archiveName.titleId}")
            }
            // Keep recovery artifacts beside the title directory. PatchManager scans only the
            // title directory, so an interrupted install can never be presented as an add-on.
            val recoveryDirectory = addonDirectory.parentFile
                ?: return failed(Error.CannotWrite, addonDirectory.path)
            val staging = File(recoveryDirectory, ".DualScreen-$gameId.staging")

            return try {
                if (!addonDirectory.exists() && !addonDirectory.mkdirs()) {
                    return failed(Error.CannotWrite, addonDirectory.path)
                }
                recoverInterruptedInstalls(addonDirectory, recoveryDirectory, gameId)
                if (staging.exists() && !staging.deleteRecursively()) {
                    return failed(Error.CannotWrite, staging.path)
                }
                if (!staging.mkdir()) {
                    return failed(Error.CannotWrite, staging.path)
                }

                val extraction = extract(
                    ArchiveInputStream(input, MAX_ARCHIVE_BYTES),
                    archiveSize,
                    staging,
                    progressCallback
                )
                if (extraction != null) {
                    staging.deleteRecursively()
                    return extraction
                }

                // The compatibility gate comes first: a package for a newer runtime may use keys
                // this installer does not know. Refusing here keeps the installed version.
                if (runtimeVersion != null) {
                    runtimeGate(staging, gameId, runtimeVersion)?.let {
                        staging.deleteRecursively()
                        return it
                    }
                }
                val metadataError = validateExtractedPackage(staging, gameId)
                if (metadataError != null) {
                    staging.deleteRecursively()
                    return metadataError
                }
                val packageVersion = packageVersionOf(staging)
                if (archiveName.version != null && packageVersion != archiveName.version) {
                    staging.deleteRecursively()
                    return failed(
                        Error.VersionMismatch,
                        "file name ${archiveName.version}, package.json $packageVersion"
                    )
                }

                val folderName = folderNameFor(archiveName, gameId, staging)
                val destination = File(addonDirectory, folderName)
                // Builds before the title-scoped name used ".<folder>.backup".
                recoverInterruptedInstall(
                    destination,
                    File(recoveryDirectory, ".$folderName.backup")
                )
                replaceAtomically(
                    destination,
                    staging,
                    backupFor(recoveryDirectory, gameId, folderName)
                )
                val superseded = removeSupersededPackages(addonDirectory, destination, gameId)
                Result.Installed(
                    folderName,
                    superseded.removed,
                    superseded.notRemoved,
                    otherDualScreenPackages(addonDirectory, destination, gameId)
                        .filter { it.folderName !in superseded.notRemoved }
                )
            } catch (_: ArchiveTooLargeException) {
                staging.deleteRecursively()
                failed(Error.ArchiveTooLarge)
            } catch (e: java.util.zip.ZipException) {
                log("$filename is not a readable ZIP", e)
                staging.deleteRecursively()
                failed(Error.MalformedArchive, e.message ?: "")
            } catch (e: SecurityException) {
                log("$filename: access denied", e)
                staging.deleteRecursively()
                failed(Error.CannotWrite, e.message ?: "")
            } catch (e: WriteException) {
                log("$filename: cannot write ${e.path}", e)
                staging.deleteRecursively()
                failed(Error.CannotWrite, e.path.path)
            } catch (e: IOException) {
                log("$filename: I/O error", e)
                staging.deleteRecursively()
                failed(Error.InstallFailed, e.message ?: e.javaClass.simpleName)
            } catch (e: Exception) {
                log("$filename: install failed", e)
                staging.deleteRecursively()
                failed(Error.InstallFailed, e.message ?: e.javaClass.simpleName)
            }
        }
    }

    private fun packageVersionOf(root: File): String? = try {
        JSONObject(File(root, PACKAGE_JSON).readText(Charsets.UTF_8)).opt("version") as? String
    } catch (_: Exception) {
        null
    }

    private fun readJsonObject(file: File): JSONObject? = try {
        if (file.isFile && file.length() <= MAX_METADATA_BYTES) {
            JSONObject(file.readText(Charsets.UTF_8))
        } else {
            null
        }
    } catch (_: Exception) {
        null
    }

    /**
     * Refuses a package for this game whose min_runtime is newer than [runtimeVersion]. Anything
     * else is left to [validateExtractedPackage].
     */
    internal fun runtimeGate(root: File, gameId: String, runtimeVersion: Int): Result.Failed? {
        val packageJson = readJsonObject(File(root, PACKAGE_JSON)) ?: return null
        if (packageJson.opt("title_id") != gameId) return null
        val manifestJson = readJsonObject(File(root, MANIFEST_JSON))
        val required = packageMinRuntime(manifestJson, packageJson)
        if (required <= runtimeVersion.toLong()) return null
        val shown = if (required == UNKNOWN_RUNTIME) "?" else required.toString()
        return Result.Failed(
            Error.RuntimeTooOld,
            "needs runtime $shown, have $runtimeVersion",
            required,
            runtimeVersion
        )
    }

    /**
     * One dual-screen package per title: after activating [keep], remove the title's other
     * installer-created package folders (the legacy "DualScreen-<TITLEID>" folder and earlier
     * "<Name>-<version>" installs). Only folders holding a package.json for this title are
     * touched, so a hand-made add-on folder without one is left alone.
     */
    internal data class Superseded(val removed: List<String>, val notRemoved: List<String>)

    internal fun removeSupersededPackages(
        addonDirectory: File,
        keep: File,
        gameId: String
    ): Superseded {
        val removed = mutableListOf<String>()
        val notRemoved = mutableListOf<String>()
        val siblings = addonDirectory.listFiles() ?: return Superseded(removed, notRemoved)
        for (dir in siblings.sortedBy { it.name }) {
            if (!dir.isDirectory || dir.name == keep.name) continue
            val pkg = File(dir, PACKAGE_JSON)
            if (!pkg.isFile || !File(dir, MANIFEST_JSON).isFile) continue
            val title = try {
                JSONObject(pkg.readText(Charsets.UTF_8)).opt("title_id") as? String
            } catch (_: Exception) {
                null
            }
            if (title != gameId || packageVersionOf(dir) == null) continue
            if (dir.deleteRecursively()) {
                removed += dir.name
            } else {
                log("could not delete the superseded package folder ${dir.path}")
                notRemoved += dir.name
            }
        }
        return Superseded(removed, notRemoved)
    }

    /**
     * This title's other folders with a dual-screen manifest the runtime would accept for this
     * game (hand-deployed folders without package.json included). The runtime uses the first
     * enabled one by name, so any of them can shadow [keep].
     */
    internal fun otherDualScreenPackages(
        addonDirectory: File,
        keep: File,
        gameId: String
    ): List<OtherPackage> {
        val siblings = addonDirectory.listFiles() ?: return emptyList()
        return siblings.sortedBy { it.name }.mapNotNull { dir ->
            if (!dir.isDirectory || dir.name == keep.name || dir.name.startsWith(".")) {
                return@mapNotNull null
            }
            val manifest = readJsonObject(File(dir, MANIFEST_JSON)) ?: return@mapNotNull null
            if (manifest.has("title_id")) {
                val title = manifest.opt("title_id") as? String ?: return@mapNotNull null
                if (!title.equals(gameId, ignoreCase = true)) return@mapNotNull null
            }
            val contents = dir.list().orEmpty().filterNot { it.startsWith(".") }
            OtherPackage(dir.name, contents.all { it == DUALSCREEN_DIR || it == PACKAGE_JSON })
        }
    }

    /** Title-scoped, so two games' packages with the same folder name never share a backup. */
    private fun backupFor(recoveryDirectory: File, gameId: String, folderName: String) =
        File(recoveryDirectory, "$BACKUP_PREFIX$gameId-$folderName$BACKUP_SUFFIX")

    /**
     * Finishes any interrupted update of this title: a backup whose folder is missing is put
     * back, one whose folder exists is deleted. Backups of other titles are never touched.
     */
    private fun recoverInterruptedInstalls(
        addonDirectory: File,
        recoveryDirectory: File,
        gameId: String
    ) {
        val prefix = "$BACKUP_PREFIX$gameId-"
        val backups = recoveryDirectory.listFiles { file ->
            file.name.startsWith(prefix) && file.name.endsWith(BACKUP_SUFFIX)
        } ?: return
        for (backup in backups) {
            val folderName = backup.name.removePrefix(prefix).removeSuffix(BACKUP_SUFFIX)
            if (folderName.isEmpty() || folderName.startsWith(".") || folderName.contains('/')) {
                continue
            }
            recoverInterruptedInstall(File(addonDirectory, folderName), backup)
        }
    }

    private fun recoverInterruptedInstall(destination: File, backup: File) {
        when {
            !destination.exists() && backup.exists() -> {
                if (!backup.renameTo(destination)) {
                    throw WriteException(destination, "Could not recover previous dual-screen package")
                }
            }
            destination.exists() && backup.exists() -> backup.deleteRecursively()
        }
    }

    /**
     * Files that archivers add on their own (macOS Finder's __MACOSX/ and ._* resource forks,
     * .DS_Store, Windows' Thumbs.db/desktop.ini). They are skipped, not extracted.
     */
    private fun isArchiverJunk(path: String): Boolean {
        val parts = path.split('/')
        val last = parts.last()
        return parts.first() == "__MACOSX" || last == ".DS_Store" || last.startsWith("._") ||
            last.equals("Thumbs.db", ignoreCase = true) ||
            last.equals("desktop.ini", ignoreCase = true)
    }

    /**
     * Progress is the compressed bytes read against [archiveSize] when the size is known;
     * otherwise the callback gets (0, 0), which leaves the bar indeterminate, and is only asked
     * whether the user cancelled. It is asked at every entry and every [PROGRESS_STEP] bytes,
     * so a single large entry can be cancelled too.
     */
    private fun extract(
        input: ArchiveInputStream,
        archiveSize: Long,
        staging: File,
        progressCallback: (max: Long, progress: Long) -> Boolean
    ): Result.Failed? {
        val seen = HashSet<String>()
        var entries = 0
        var expandedBytes = 0L
        var reportedAt = 0L
        fun cancelled(): Boolean {
            reportedAt = input.bytesRead
            return if (archiveSize > 0) {
                progressCallback(archiveSize, minOf(input.bytesRead, archiveSize))
            } else {
                progressCallback(0, 0)
            }
        }
        ZipInputStream(input).use { zis ->
            var entry: ZipEntry? = zis.nextEntry
            while (entry != null) {
                if (++entries > MAX_ENTRIES) {
                    return failed(Error.ArchiveTooLarge, "more than $MAX_ENTRIES entries")
                }
                if (cancelled()) return failed(Error.Cancelled)

                val path = validateEntryName(entry.name)
                    ?: return failed(Error.UnsafeEntry, entry.name)
                if (isArchiverJunk(path)) {
                    entry = zis.nextEntry
                    continue
                }
                if (!seen.add(path)) return failed(Error.DuplicateEntry, path)

                val isDirectory = entry.isDirectory || entry.name.endsWith('/')
                validateLayout(path, isDirectory)?.let { return failed(it, path) }

                val output = File(staging, path)
                val canonicalStaging = staging.canonicalFile
                if (!output.canonicalFile.path.startsWith(canonicalStaging.path + File.separator)) {
                    return failed(Error.UnsafeEntry, path)
                }
                if (isDirectory) {
                    if (!output.exists() && !output.mkdirs()) {
                        return failed(Error.CannotWrite, output.path)
                    }
                } else {
                    val parent = output.parentFile ?: return failed(Error.UnsafeEntry, path)
                    if (!parent.exists() && !parent.mkdirs()) {
                        return failed(Error.CannotWrite, parent.path)
                    }
                    val expectedSize = entry.size
                    if (expectedSize > MAX_ENTRY_BYTES) return failed(Error.ArchiveTooLarge, path)
                    var bytes = 0L
                    openForWrite(output).use { file ->
                        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                        while (true) {
                            val read = zis.read(buffer)
                            if (read < 0) break
                            if (input.bytesRead - reportedAt >= PROGRESS_STEP && cancelled()) {
                                return failed(Error.Cancelled)
                            }
                            bytes += read
                            if (bytes > MAX_ENTRY_BYTES ||
                                bytes > MAX_EXPANDED_BYTES - expandedBytes
                            ) {
                                return failed(Error.ArchiveTooLarge, path)
                            }
                            file.write(buffer, 0, read)
                        }
                    }
                    expandedBytes += bytes
                }
                entry = zis.nextEntry
            }
        }
        return null
    }

    private fun validateEntryName(rawName: String): String? {
        if (rawName.isEmpty() || rawName.startsWith('/') || rawName.startsWith('\\') ||
            rawName.contains('\\') || rawName.contains('\u0000')
        ) {
            return null
        }
        val name = rawName.removeSuffix("/")
        if (name.isEmpty()) return null
        val parts = name.split('/')
        if (parts.any { it.isEmpty() || it == "." || it == ".." }) return null
        return parts.joinToString("/")
    }

    private fun validateLayout(path: String, directory: Boolean): Error? {
        val topLevel = path.substringBefore('/')
        if (topLevel != PACKAGE_JSON && topLevel != DUALSCREEN_DIR) return Error.InvalidLayout
        if (path == PACKAGE_JSON && directory) return Error.InvalidLayout
        if (path == MANIFEST_JSON && directory) return Error.InvalidLayout
        if (path.startsWith("$DUALSCREEN_DIR/modules/")) {
            val parts = path.split('/')
            if (parts.size == 4 && !directory &&
                parts[1] == "modules" && parts[2].isNotEmpty() &&
                parts[3].removeSuffix(".so").matches(titleId) &&
                parts[3].removeSuffix(".so") ==
                    parts[3].removeSuffix(".so").uppercase() && parts[3].endsWith(".so")
            ) {
                return null
            }
            if (!directory && parts.size == 4 && parts[1] == "modules") {
                return Error.InvalidLayout
            }
        }
        return null
    }

    /**
     * Checks the extracted package against the rules the core runtime applies when it loads it
     * (PACKAGE_FORMAT.md sections 3.1-3.2), plus the installer's own identity rules for
     * package.json. Returns null when the package is installable.
     */
    internal fun validateExtractedPackage(
        root: File,
        gameId: String,
        supportedAbis: List<String>? = null
    ): Result.Failed? {
        val packageFile = File(root, PACKAGE_JSON)
        val manifestFile = File(root, MANIFEST_JSON)
        if (!packageFile.isFile) return failed(Error.InvalidLayout, "$PACKAGE_JSON is missing")
        if (!manifestFile.isFile) return failed(Error.InvalidLayout, "$MANIFEST_JSON is missing")
        if (packageFile.length() > MAX_METADATA_BYTES) {
            return failed(Error.ArchiveTooLarge, PACKAGE_JSON)
        }
        if (manifestFile.length() > MAX_METADATA_BYTES) {
            return failed(Error.ArchiveTooLarge, MANIFEST_JSON)
        }

        val packageJson = try {
            JSONObject(packageFile.readText(Charsets.UTF_8))
        } catch (e: Exception) {
            return failed(Error.InvalidMetadata, "$PACKAGE_JSON is not a JSON object (${e.message})")
        }
        val manifestJson = try {
            JSONObject(manifestFile.readText(Charsets.UTF_8))
        } catch (e: Exception) {
            return failed(Error.InvalidMetadata, "$MANIFEST_JSON is not a JSON object (${e.message})")
        }

        // package.json: the installer's identity for the archive.
        when (val packageTitle = packageJson.opt("title_id")) {
            gameId -> Unit
            is String -> return if (packageTitle.equals(gameId, ignoreCase = true)) {
                failed(Error.InvalidMetadata, "$PACKAGE_JSON title_id must be upper case")
            } else if (titleId.matches(packageTitle.uppercase())) {
                failed(Error.TitleMismatch, "$PACKAGE_JSON title_id $packageTitle")
            } else {
                failed(Error.InvalidMetadata, "$PACKAGE_JSON title_id \"$packageTitle\"")
            }
            else -> return failed(Error.InvalidMetadata, "$PACKAGE_JSON has no title_id")
        }
        if (packageJson.opt("format") != PACKAGE_FORMAT) {
            return failed(
                Error.InvalidMetadata,
                "$PACKAGE_JSON format ${packageJson.opt("format")} (this app reads $PACKAGE_FORMAT)"
            )
        }
        if (packageJson.opt("type") != PACKAGE_TYPE) {
            return failed(Error.InvalidMetadata, "$PACKAGE_JSON type is not \"$PACKAGE_TYPE\"")
        }
        val packageName = packageJson.opt("name") as? String
        if (packageName.isNullOrBlank() || packageName.length > 256) {
            return failed(Error.InvalidMetadata, "$PACKAGE_JSON name is missing or too long")
        }
        val packageVersion = packageJson.opt("version") as? String
        if (packageVersion == null || !version.matches(packageVersion)) {
            return failed(Error.InvalidMetadata, "$PACKAGE_JSON version \"$packageVersion\"")
        }

        // dualscreen/manifest.json: the same acceptance rules as the runtime.
        val hasModule = manifestJson.has("module") && manifestJson.opt("module") != JSONObject.NULL
        if (manifestJson.has("title_id")) {
            val manifestTitle = manifestJson.opt("title_id") as? String
                ?: return failed(Error.InvalidMetadata, "$MANIFEST_JSON title_id is not a string")
            if (!manifestTitle.equals(gameId, ignoreCase = true)) {
                return failed(Error.TitleMismatch, "$MANIFEST_JSON title_id $manifestTitle")
            }
        } else if (hasModule) {
            return failed(Error.InvalidMetadata, "$MANIFEST_JSON needs title_id with a module")
        }
        when (val format = manifestJson.opt("format")) {
            null, JSONObject.NULL, MANIFEST_FORMAT -> Unit
            else -> return failed(
                Error.InvalidMetadata,
                "$MANIFEST_JSON format $format (this app reads $MANIFEST_FORMAT)"
            )
        }
        val hasPages = (manifestJson.opt("pages") as? JSONArray)?.let { it.length() > 0 } ?: false
        if (!hasPages && manifestJson.opt("debug_page") != true) {
            return failed(Error.InvalidMetadata, "$MANIFEST_JSON has no pages")
        }

        val packageModule = try {
            parseModule(packageJson.opt("module"), gameId)
        } catch (e: IllegalArgumentException) {
            return failed(Error.InvalidMetadata, "$PACKAGE_JSON module: ${e.message}")
        }
        val manifestModule = try {
            parseModule(manifestJson.opt("module"), gameId)
        } catch (e: IllegalArgumentException) {
            return failed(Error.InvalidMetadata, "$MANIFEST_JSON module: ${e.message}")
        }
        if (packageModule != manifestModule) {
            return failed(
                Error.InvalidMetadata,
                "module differs between $PACKAGE_JSON and $MANIFEST_JSON"
            )
        }

        // Packages whose title-specific reader is required must carry a native module. This
        // keeps a declarative-only archive from appearing installable for a package that cannot
        // provide its required game data without the reader.
        val requiresModule = when (val value = manifestJson.opt("requires_module")) {
            null, JSONObject.NULL -> false
            is Boolean -> value
            else -> return failed(Error.InvalidMetadata, "$MANIFEST_JSON requires_module")
        }
        if (requiresModule && packageModule == null) {
            return failed(Error.InvalidMetadata, "$MANIFEST_JSON requires a module; none declared")
        }

        val moduleFiles = root.walkTopDown().filter { it.isFile }
            .map { it.relativeTo(root).path.replace(File.separatorChar, '/') }
            .filter { it.startsWith("$MODULE_DIR/") }
            .toList()
        if (moduleFiles.isNotEmpty() && packageModule == null) {
            return failed(Error.InvalidMetadata, "$MODULE_DIR/ has files but no module is declared")
        }
        if (packageModule != null && moduleFiles.isEmpty()) {
            return failed(Error.InvalidLayout, "the declared module has no files in $MODULE_DIR/")
        }
        moduleFiles.firstOrNull { path ->
            val parts = path.split('/')
            parts.size != 4 || parts[1] != "modules" || parts[3] != "$gameId.so" ||
                packageModule?.libraries?.get(parts[2])?.path != "modules/${parts[2]}/$gameId.so"
        }?.let { return failed(Error.InvalidLayout, it) }
        if (packageModule != null) {
            val platforms = moduleFiles.map { it.split('/')[2] }.toSet()
            if (packageModule.libraries.keys != platforms) {
                return failed(
                    Error.InvalidLayout,
                    "declared ${packageModule.libraries.keys.sorted()}, files ${platforms.sorted()}"
                )
            }
            val deviceAbis = supportedAbis ?: runCatching {
                Build.SUPPORTED_ABIS.toList()
            }.getOrDefault(emptyList())
            val androidPlatform = deviceAbis.firstNotNullOfOrNull { abi ->
                when (abi) {
                    "arm64-v8a" -> "android-arm64-v8a"
                    "x86_64" -> "android-x86_64"
                    "armeabi-v7a" -> "android-armeabi-v7a"
                    "x86" -> "android-x86"
                    else -> null
                }
            }
            if (deviceAbis.isNotEmpty() && androidPlatform == null) {
                return failed(Error.MissingPlatformLibrary, deviceAbis.joinToString())
            }
            if (androidPlatform != null && !packageModule.libraries.containsKey(androidPlatform)) {
                return failed(Error.MissingPlatformLibrary, androidPlatform)
            }
            for ((platform, library) in packageModule.libraries) {
                val file = File(root, "$MODULE_DIR/$platform/$gameId.so")
                if (!file.isFile) return failed(Error.InvalidLayout, "$MODULE_DIR/$platform/$gameId.so")
                val actual = sha256(file)
                if (actual != library.sha256) {
                    return failed(
                        Error.ChecksumMismatch,
                        "$platform: expected ${library.sha256}, file $actual"
                    )
                }
            }
        }
        return null
    }

    internal fun validateArchiveEntriesForTest(names: List<String>): Error? {
        val seen = HashSet<String>()
        for (name in names) {
            val path = validateEntryName(name) ?: return Error.UnsafeEntry
            if (isArchiverJunk(path)) continue
            if (!seen.add(path)) return Error.DuplicateEntry
            validateLayout(path, name.endsWith('/'))?.let { return it }
        }
        return null
    }

    /** Null when [value] is absent; throws IllegalArgumentException naming the bad field. */
    private fun parseModule(value: Any?, gameId: String): ModuleDeclaration? {
        if (value == null || value == JSONObject.NULL) return null
        require(value is JSONObject) { "not an object" }
        require(value.opt("abi") == 1) { "abi ${value.opt("abi")} (this app reads 1)" }
        val buildIdsJson =
            requireNotNull(value.optJSONArray("build_ids")) { "build_ids is missing" }
        val buildIds = (0 until buildIdsJson.length()).map { index ->
            buildIdsJson.opt(index) as? String ?: throw IllegalArgumentException(
                "build_ids[$index] is not a string"
            )
        }
        require(buildIds.isNotEmpty()) { "build_ids is empty" }
        buildIds.firstOrNull { !it.matches(Regex("^[0-9A-F]{16}(?:[0-9A-F]{48})?$")) }?.let {
            throw IllegalArgumentException("build ID \"$it\"")
        }
        val librariesJson =
            requireNotNull(value.optJSONObject("libraries")) { "libraries is missing" }
        val libraries = mutableMapOf<String, Library>()
        for (platform in librariesJson.keys()) {
            require(platform.matches(Regex("^[A-Za-z0-9._-]+$"))) { "platform \"$platform\"" }
            val libraryJson = requireNotNull(librariesJson.optJSONObject(platform)) {
                "libraries.$platform is not an object"
            }
            val path = "modules/$platform/$gameId.so"
            val declaredPath = libraryJson.opt("path") as? String
            require(declaredPath == path) { "libraries.$platform.path \"$declaredPath\"" }
            val hash = libraryJson.opt("sha256") as? String
            if (hash == null || !hash.matches(Regex("^[0-9a-fA-F]{64}$"))) {
                throw IllegalArgumentException("libraries.$platform.sha256")
            }
            // Either case, like the runtime.
            libraries[platform] = Library(path, hash.lowercase())
        }
        require(libraries.isNotEmpty()) { "libraries is empty" }
        return ModuleDeclaration(buildIds, libraries)
    }

    private fun openForWrite(file: File) = try {
        file.outputStream()
    } catch (e: java.io.FileNotFoundException) {
        throw WriteException(file, e.message ?: "cannot open for writing")
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val read = input.read(buffer)
                if (read < 0) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") {
            (it.toInt() and 0xff).toString(16).padStart(2, '0')
        }
    }

    private class ArchiveTooLargeException : IOException()

    private class ArchiveInputStream(input: InputStream, private val limit: Long) :
        FilterInputStream(input) {
        var bytesRead = 0L
            private set

        override fun read(): Int {
            val value = super.read()
            if (value >= 0 && ++bytesRead > limit) throw ArchiveTooLargeException()
            return value
        }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            val count = super.read(buffer, offset, length)
            if (count > 0) {
                bytesRead += count
                if (bytesRead > limit) throw ArchiveTooLargeException()
            }
            return count
        }
    }

    internal fun replaceAtomically(destination: File, staging: File, backup: File) {
        if (backup.exists() && !backup.deleteRecursively()) {
            throw WriteException(backup, "Could not remove old package backup")
        }
        if (destination.exists() && !destination.renameTo(backup)) {
            throw WriteException(destination, "Could not stage old package")
        }
        if (!staging.renameTo(destination)) {
            if (backup.exists()) backup.renameTo(destination)
            throw WriteException(destination, "Could not activate package")
        }
        // If cleanup is interrupted, the next install sees the active destination and removes
        // this recoverable backup before staging the replacement.
        backup.deleteRecursively()
    }
}
