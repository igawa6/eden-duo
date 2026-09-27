// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import android.net.Uri
import android.os.Build
import androidx.documentfile.provider.DocumentFile
import org.json.JSONObject
import java.io.File
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream

/** Installs the standalone, declarative dual-screen package format. */
object DualScreenPackageInstaller {
    private const val PACKAGE_SUFFIX = ".dsmod.zip"
    private const val PACKAGE_TYPE = "dual-screen-mod"
    private const val PACKAGE_FORMAT = 1
    private const val MANIFEST_FORMAT = 1
    private const val MAX_ARCHIVE_BYTES = 512L * 1024 * 1024
    private const val MAX_EXPANDED_BYTES = 1024L * 1024 * 1024
    private const val MAX_ENTRY_BYTES = 512L * 1024 * 1024
    private const val MAX_ENTRIES = 10000
    private const val MAX_METADATA_BYTES = 4L * 1024 * 1024
    private const val PACKAGE_JSON = "package.json"
    private const val DUALSCREEN_DIR = "dualscreen"
    private const val MANIFEST_JSON = "$DUALSCREEN_DIR/manifest.json"
    private const val MODULE_DIR = "$DUALSCREEN_DIR/modules"

    // <TITLEID>.dsmod.zip, or <TITLEID>-<Name>-<version>.dsmod.zip (e.g.
    // 01006BB00C6F0000-LinkAwakeningDS-1.0.0.dsmod.zip). The named form installs as the add-on
    // folder "<Name>-<version>" and its version must equal package.json's.
    private val archiveName =
        Regex("^([0-9A-F]{16})(?:-([A-Za-z0-9_]{1,64})-([0-9]+\\.[0-9]+\\.[0-9]+))?\\Q$PACKAGE_SUFFIX\\E$")
    private const val LEGACY_FOLDER_PREFIX = "DualScreen-"
    private val titleId = Regex("^[0-9A-F]{16}$")
    private val version = Regex("^[0-9]+\\.[0-9]+\\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")

    enum class Error {
        InvalidFilename,
        ArchiveTooLarge,
        MalformedArchive,
        UnsafeEntry,
        DuplicateEntry,
        InvalidLayout,
        InvalidMetadata,
        TitleMismatch,
        Cancelled,
        InstallFailed
    }

    sealed class Result {
        data class Installed(val titleId: String) : Result()
        data class Failed(val error: Error) : Result()
    }

    private data class Library(val path: String, val sha256: String)

    private data class ModuleDeclaration(
        val abi: Int,
        val buildIds: List<String>,
        val libraries: Map<String, Library>
    )

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
            val filename = DocumentFile.fromSingleUri(context, uri)?.name
                ?: return Result.Failed(Error.InvalidFilename)
            val archiveSize = DocumentFile.fromSingleUri(context, uri)?.length() ?: -1L
            val input = context.contentResolver.openInputStream(uri)
                ?: return Result.Failed(Error.InstallFailed)
            installArchive(input, filename, archiveSize, gameTitleId, addonDirectory, progressCallback)
        } catch (_: Exception) {
            Result.Failed(Error.InstallFailed)
        }
    }

    @Synchronized
    internal fun installArchive(
        input: InputStream,
        filename: String,
        archiveSize: Long,
        gameTitleId: String,
        addonDirectory: File,
        progressCallback: (max: Long, progress: Long) -> Boolean = { _, _ -> false }
    ): Result {
        input.use {
            if (archiveSize > MAX_ARCHIVE_BYTES) {
                return Result.Failed(Error.ArchiveTooLarge)
            }

            val gameId = gameTitleId.uppercase()
            if (!titleId.matches(gameId)) {
                return Result.Failed(Error.TitleMismatch)
            }
            val match = archiveName.matchEntire(filename) ?: return Result.Failed(Error.InvalidFilename)
            if (match.groupValues[1] != gameId) {
                return Result.Failed(Error.TitleMismatch)
            }

            val packageLabel = match.groupValues[2]
            val fileVersion = match.groupValues[3]
            val folderName =
                if (packageLabel.isEmpty()) "$LEGACY_FOLDER_PREFIX$gameId" else "$packageLabel-$fileVersion"
            val destination = File(addonDirectory, folderName)
            // Keep recovery artifacts beside the title directory. PatchManager scans only the
            // title directory, so an interrupted install can never be presented as an add-on.
            val recoveryDirectory = addonDirectory.parentFile
                ?: return Result.Failed(Error.InstallFailed)
            val staging = File(recoveryDirectory, ".DualScreen-$gameId.staging")
            val backup = File(recoveryDirectory, ".$folderName.backup")

            return try {
                if (!addonDirectory.exists() && !addonDirectory.mkdirs()) {
                    return Result.Failed(Error.InstallFailed)
                }

                recoverInterruptedInstall(destination, staging, backup)
                if (staging.exists() && !staging.deleteRecursively()) {
                    return Result.Failed(Error.InstallFailed)
                }
                if (!staging.mkdir()) {
                    return Result.Failed(Error.InstallFailed)
                }

                val extraction = extract(
                    ArchiveInputStream(input, MAX_ARCHIVE_BYTES),
                    staging,
                    progressCallback
                )
                if (extraction != null) {
                    staging.deleteRecursively()
                    return Result.Failed(extraction)
                }

                val metadataError = validateExtractedPackage(staging, gameId)
                if (metadataError != null) {
                    staging.deleteRecursively()
                    return Result.Failed(metadataError)
                }
                if (fileVersion.isNotEmpty() && packageVersionOf(staging) != fileVersion) {
                    staging.deleteRecursively()
                    return Result.Failed(Error.InvalidMetadata)
                }

                replaceAtomically(destination, staging, backup)
                removeSupersededPackages(addonDirectory, destination, gameId)
                Result.Installed(gameId)
            } catch (_: ArchiveTooLargeException) {
                staging.deleteRecursively()
                Result.Failed(Error.ArchiveTooLarge)
            } catch (_: java.util.zip.ZipException) {
                staging.deleteRecursively()
                Result.Failed(Error.MalformedArchive)
            } catch (_: SecurityException) {
                staging.deleteRecursively()
                Result.Failed(Error.UnsafeEntry)
            } catch (_: IOException) {
                staging.deleteRecursively()
                Result.Failed(Error.InstallFailed)
            } catch (_: Exception) {
                staging.deleteRecursively()
                Result.Failed(Error.InstallFailed)
            }
        }
    }

    private fun packageVersionOf(root: File): String? = try {
        JSONObject(File(root, PACKAGE_JSON).readText(Charsets.UTF_8)).opt("version") as? String
    } catch (_: Exception) {
        null
    }

    /**
     * One dual-screen package per title: after activating [keep], remove the title's other
     * installer-created package folders (the legacy "DualScreen-<TITLEID>" folder and earlier
     * "<Name>-<version>" installs). Only folders holding a package.json for this title are
     * touched, so a hand-made add-on folder without one is left alone.
     */
    internal fun removeSupersededPackages(addonDirectory: File, keep: File, gameId: String) {
        val siblings = addonDirectory.listFiles() ?: return
        for (dir in siblings) {
            if (!dir.isDirectory || dir.name == keep.name) continue
            val pkg = File(dir, PACKAGE_JSON)
            if (!pkg.isFile || !File(dir, MANIFEST_JSON).isFile) continue
            val title = try {
                JSONObject(pkg.readText(Charsets.UTF_8)).opt("title_id") as? String
            } catch (_: Exception) {
                null
            }
            if (title == gameId && (packageVersionOf(dir) != null)) dir.deleteRecursively()
        }
    }

    private fun recoverInterruptedInstall(destination: File, staging: File, backup: File) {
        when {
            !destination.exists() && backup.exists() -> {
                if (!backup.renameTo(destination)) {
                    throw IOException("Could not recover previous dual-screen package")
                }
            }
            destination.exists() && backup.exists() -> backup.deleteRecursively()
        }
        if (staging.exists() && !staging.deleteRecursively()) {
            throw IOException("Could not remove interrupted staging directory")
        }
    }

    private fun extract(
        input: InputStream,
        staging: File,
        progressCallback: (max: Long, progress: Long) -> Boolean
    ): Error? {
        val seen = HashSet<String>()
        var entries = 0
        var expandedBytes = 0L
        ZipInputStream(input).use { zis ->
            var entry: ZipEntry? = zis.nextEntry
            while (entry != null) {
                if (++entries > MAX_ENTRIES) return Error.ArchiveTooLarge
                if (progressCallback(MAX_ENTRIES.toLong(), entries.toLong())) {
                    return Error.Cancelled
                }

                val path = validateEntryName(entry.name) ?: return Error.UnsafeEntry
                if (!seen.add(path)) return Error.DuplicateEntry

                val isDirectory = entry.isDirectory || entry.name.endsWith('/')
                validateLayout(path, isDirectory)?.let { return it }

                val output = File(staging, path)
                val canonicalStaging = staging.canonicalFile
                if (!output.canonicalFile.path.startsWith(canonicalStaging.path + File.separator)) {
                    return Error.UnsafeEntry
                }
                if (isDirectory) {
                    if (!output.exists() && !output.mkdirs()) return Error.InstallFailed
                } else {
                    val parent = output.parentFile ?: return Error.UnsafeEntry
                    if (!parent.exists() && !parent.mkdirs()) return Error.InstallFailed
                    val expectedSize = entry.size
                    if (expectedSize > MAX_ENTRY_BYTES) return Error.ArchiveTooLarge
                    var bytes = 0L
                    output.outputStream().use { file ->
                        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                        while (true) {
                            val read = zis.read(buffer)
                            if (read < 0) break
                            bytes += read
                            if (bytes > MAX_ENTRY_BYTES ||
                                bytes > MAX_EXPANDED_BYTES - expandedBytes
                            ) {
                                return Error.ArchiveTooLarge
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

    internal fun validateExtractedPackage(
        root: File,
        gameId: String,
        supportedAbis: List<String>? = null
    ): Error? {
        val packageFile = File(root, PACKAGE_JSON)
        val manifestFile = File(root, MANIFEST_JSON)
        if (!packageFile.isFile || !manifestFile.isFile) return Error.InvalidLayout
        if (packageFile.length() > MAX_METADATA_BYTES || manifestFile.length() > MAX_METADATA_BYTES) {
            return Error.ArchiveTooLarge
        }

        val packageJson = try {
            JSONObject(packageFile.readText(Charsets.UTF_8))
        } catch (_: Exception) {
            return Error.InvalidMetadata
        }
        val manifestJson = try {
            JSONObject(manifestFile.readText(Charsets.UTF_8))
        } catch (_: Exception) {
            return Error.InvalidMetadata
        }

        val packageTitle = packageJson.opt("title_id") as? String
        val packageName = packageJson.opt("name") as? String
        val packageVersion = packageJson.opt("version") as? String
        val manifestTitle = manifestJson.opt("title_id") as? String
        if (packageJson.opt("format") != PACKAGE_FORMAT ||
            packageJson.opt("type") != PACKAGE_TYPE ||
            packageTitle != gameId ||
            packageName.isNullOrBlank() ||
            packageName.length > 256 ||
            packageVersion == null ||
            !version.matches(packageVersion) ||
            manifestTitle != gameId ||
            manifestJson.opt("format") != MANIFEST_FORMAT ||
            (manifestJson.optJSONArray("pages")?.length() ?: 0) == 0
        ) {
            return if (packageTitle != gameId || manifestTitle != gameId
            ) Error.TitleMismatch else Error.InvalidMetadata
        }

        val packageModule = parseModule(packageJson.opt("module"), gameId) ?:
            if (packageJson.has("module")) return Error.InvalidMetadata else null
        val manifestModule = parseModule(manifestJson.opt("module"), gameId) ?:
            if (manifestJson.has("module")) return Error.InvalidMetadata else null
        if (packageModule != manifestModule) return Error.InvalidMetadata

        // Packages whose title-specific reader is required must carry a native module. This
        // keeps a declarative-only archive from appearing installable for a package that cannot
        // provide its required game data without the reader.
        val requiresModule = when (val value = manifestJson.opt("requires_module")) {
            null, JSONObject.NULL -> false
            is Boolean -> value
            else -> return Error.InvalidMetadata
        }
        if (requiresModule && packageModule == null) return Error.InvalidMetadata

        val moduleFiles = root.walkTopDown().filter { it.isFile }
            .map { it.relativeTo(root).path.replace(File.separatorChar, '/') }
            .filter { it.startsWith("$MODULE_DIR/") }
            .toList()
        if (moduleFiles.isNotEmpty() && packageModule == null) return Error.InvalidMetadata
        if (packageModule != null && moduleFiles.isEmpty()) return Error.InvalidLayout
        if (moduleFiles.any { path ->
                val parts = path.split('/')
                parts.size != 4 || parts[1] != "modules" ||
                    !parts[3].removeSuffix(".so").matches(titleId) ||
                    parts[3].removeSuffix(".so") !=
                    parts[3].removeSuffix(".so").uppercase() || parts[3] != "$gameId.so" ||
                    packageModule?.libraries?.get(parts[2])?.path !=
                    "modules/${parts[2]}/$gameId.so"
            }
        ) return Error.InvalidLayout
        if (packageModule != null) {
            if (packageModule.libraries.keys != moduleFiles.map { it.split('/')[2] }.toSet()) {
                return Error.InvalidLayout
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
            if (deviceAbis.isNotEmpty() && androidPlatform == null) return Error.InvalidLayout
            if (androidPlatform != null && !packageModule.libraries.containsKey(androidPlatform)) {
                return Error.InvalidLayout
            }
            for ((platform, library) in packageModule.libraries) {
                val file = File(root, "$MODULE_DIR/$platform/$gameId.so")
                if (!file.isFile || sha256(file) != library.sha256) return Error.InvalidMetadata
            }
        }
        return null
    }

    internal fun validateArchiveEntriesForTest(names: List<String>): Error? {
        val seen = HashSet<String>()
        for (name in names) {
            val path = validateEntryName(name) ?: return Error.UnsafeEntry
            if (!seen.add(path)) return Error.DuplicateEntry
            validateLayout(path, name.endsWith('/'))?.let { return it }
        }
        return null
    }

    private fun parseModule(value: Any?, gameId: String): ModuleDeclaration? {
        if (value == null || value == JSONObject.NULL) return null
        if (value !is JSONObject || value.opt("abi") != 1) return null
        val buildIdsJson = value.optJSONArray("build_ids") ?: return null
        val buildIds = buildIdsJson.let { array ->
            (0 until array.length()).map { index -> array.opt(index) as? String ?: return null }
        }
        if (buildIds.isEmpty() || buildIds.any {
                !it.matches(Regex("^[0-9A-F]{16}(?:[0-9A-F]{48})?$"))
            }
        ) {
            return null
        }
        val librariesJson = value.optJSONObject("libraries") ?: return null
        val libraries = mutableMapOf<String, Library>()
        for (platform in librariesJson.keys()) {
            if (!platform.matches(Regex("^[A-Za-z0-9._-]+$"))) return null
            val libraryJson = librariesJson.optJSONObject(platform) ?: return null
            val path = libraryJson.opt("path") as? String ?: return null
            val hash = libraryJson.opt("sha256") as? String ?: return null
            if (path != "modules/$platform/$gameId.so" ||
                !hash.matches(Regex("^[0-9a-f]{64}$"))
            ) return null
            libraries[platform] = Library(path, hash)
        }
        if (libraries.isEmpty()) return null
        return ModuleDeclaration(1, buildIds, libraries)
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
        private var bytesRead = 0L

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
            throw IOException("Could not remove old package backup")
        }
        if (destination.exists() && !destination.renameTo(backup)) {
            throw IOException("Could not stage old package")
        }
        if (!staging.renameTo(destination)) {
            if (backup.exists()) backup.renameTo(destination)
            throw IOException("Could not activate package")
        }
        // If cleanup is interrupted, the next install sees the active destination and removes
        // this recoverable backup before staging the replacement.
        backup.deleteRecursively()
    }
}
