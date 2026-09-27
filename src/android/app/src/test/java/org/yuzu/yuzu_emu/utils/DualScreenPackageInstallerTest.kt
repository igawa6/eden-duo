// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test

class DualScreenPackageInstallerTest {
    private val titleId = "010093801237C000"
    private lateinit var root: File

    @Before
    fun setUp() {
        root = Files.createTempDirectory("dsmod-test").toFile()
    }

    @After
    fun tearDown() {
        root.deleteRecursively()
    }

    @Test
    fun archiveEntriesRejectTraversalDuplicateAndUnexpectedRoot() {
        assertEquals(
            DualScreenPackageInstaller.Error.UnsafeEntry,
            DualScreenPackageInstaller.validateArchiveEntriesForTest(listOf("../evil"))
        )
        assertEquals(
            DualScreenPackageInstaller.Error.UnsafeEntry,
            DualScreenPackageInstaller.validateArchiveEntriesForTest(listOf("/evil"))
        )
        assertEquals(
            DualScreenPackageInstaller.Error.DuplicateEntry,
            DualScreenPackageInstaller.validateArchiveEntriesForTest(
                listOf("package.json", "package.json")
            )
        )
        assertEquals(
            DualScreenPackageInstaller.Error.InvalidLayout,
            DualScreenPackageInstaller.validateArchiveEntriesForTest(listOf("README.txt"))
        )
        assertNull(
            DualScreenPackageInstaller.validateArchiveEntriesForTest(
                listOf("package.json", "dualscreen/", "dualscreen/manifest.json")
            )
        )
    }

    @Test
    fun metadataRequiresMatchingTitleAndRejectsOversizedMetadata() {
        writeMetadata(packageTitleId = "010093801237C001")
        assertEquals(
            DualScreenPackageInstaller.Error.TitleMismatch,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )

        File(root, "package.json").writeText("{\"padding\":\"${"x".repeat(4 * 1024 * 1024)}\"}")
        assertEquals(
            DualScreenPackageInstaller.Error.ArchiveTooLarge,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )
    }

    @Test
    fun moduleDeclarationAndHashAreValidated() {
        val library = File(root, "dualscreen/modules/android-arm64-v8a/$titleId.so")
        checkNotNull(library.parentFile).mkdirs()
        library.writeBytes(byteArrayOf(1, 2, 3, 4))
        val hash = sha256(library)
        val module = moduleJson(hash)
        writeMetadata(module = module)

        assertNull(
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )
        library.appendBytes(byteArrayOf(5))
        assertEquals(
            DualScreenPackageInstaller.Error.InvalidMetadata,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )
    }

    @Test
    fun moduleDeclarationMustMatchManifest() {
        val library = File(root, "dualscreen/modules/android-arm64-v8a/$titleId.so")
        checkNotNull(library.parentFile).mkdirs()
        library.writeBytes(byteArrayOf(1, 2, 3, 4))
        val module = moduleJson(sha256(library))
        writeMetadata(module = module, manifestModule = module.replace("\"abi\":1", "\"abi\":2"))

        assertEquals(
            DualScreenPackageInstaller.Error.InvalidMetadata,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )
    }

    @Test
    fun moduleMetadataFieldsMustUseDeclaredJsonTypes() {
        val library = File(root, "dualscreen/modules/android-arm64-v8a/$titleId.so")
        checkNotNull(library.parentFile).mkdirs()
        library.writeBytes(byteArrayOf(1, 2, 3, 4))
        val module = moduleJson(sha256(library)).replace(
            "\"646761F643AFEBB3\"",
            "1234567890123456"
        )
        writeMetadata(module = module)

        assertEquals(
            DualScreenPackageInstaller.Error.InvalidMetadata,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )
    }

    @Test
    fun requiredModuleCannotBeOmitted() {
        writeMetadata(requiresModule = true)
        assertEquals(
            DualScreenPackageInstaller.Error.InvalidMetadata,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )
    }

    @Test
    fun validZipIsExtractedAndActivated() {
        val addonDirectory = File(root, "load/$titleId")
        val archive = validArchive()

        val result = DualScreenPackageInstaller.installArchive(
            archive.inputStream(),
            "$titleId.dsmod.zip",
            archive.size.toLong(),
            titleId,
            addonDirectory
        )

        assertEquals(DualScreenPackageInstaller.Result.Installed(titleId), result)
        assertEquals(
            "Test",
            File(addonDirectory, "DualScreen-$titleId/package.json").readText()
                .let { org.json.JSONObject(it).getString("name") }
        )
        assertEquals("data", File(addonDirectory, "DualScreen-$titleId/dualscreen/data.json").readText())
        assertFalse(File(root, "load/.DualScreen-$titleId.staging").exists())
        assertFalse(File(root, "load/.DualScreen-$titleId.backup").exists())
    }

    @Test
    fun namedZipInstallsAsNameVersionFolderAndReplacesOlderInstalls() {
        val addonDirectory = File(root, "load/$titleId")
        val archive = validArchive()
        assertEquals(
            DualScreenPackageInstaller.Result.Installed(titleId),
            DualScreenPackageInstaller.installArchive(
                archive.inputStream(), "$titleId.dsmod.zip", archive.size.toLong(), titleId,
                addonDirectory
            )
        )
        File(addonDirectory, "TestDS-0.9.0/dualscreen").mkdirs()
        File(addonDirectory, "TestDS-0.9.0/package.json").writeText(
            """{"format":1,"type":"dual-screen-mod","title_id":"$titleId","name":"Old","version":"0.9.0"}"""
        )
        File(addonDirectory, "TestDS-0.9.0/dualscreen/manifest.json").writeText("{}")
        File(addonDirectory, "OtherMod/romfs").mkdirs()

        val result = DualScreenPackageInstaller.installArchive(
            archive.inputStream(), "$titleId-TestDS-1.0.0.dsmod.zip", archive.size.toLong(),
            titleId, addonDirectory
        )

        assertEquals(DualScreenPackageInstaller.Result.Installed(titleId), result)
        assertEquals("data", File(addonDirectory, "TestDS-1.0.0/dualscreen/data.json").readText())
        assertFalse(File(addonDirectory, "DualScreen-$titleId").exists())
        assertFalse(File(addonDirectory, "TestDS-0.9.0").exists())
        assertEquals(true, File(addonDirectory, "OtherMod/romfs").isDirectory)
        assertFalse(File(root, "load/.TestDS-1.0.0.backup").exists())
    }

    @Test
    fun namedZipVersionMustMatchPackageVersion() {
        val addonDirectory = File(root, "load/$titleId")
        val archive = validArchive()
        val result = DualScreenPackageInstaller.installArchive(
            archive.inputStream(), "$titleId-TestDS-2.0.0.dsmod.zip", archive.size.toLong(),
            titleId, addonDirectory
        )
        assertEquals(
            DualScreenPackageInstaller.Result.Failed(DualScreenPackageInstaller.Error.InvalidMetadata),
            result
        )
        assertFalse(File(addonDirectory, "TestDS-2.0.0").exists())
    }

    @Test
    fun failedReplacementRetainsPreviousInstalledPackage() {
        val addonDirectory = File(root, "load/$titleId")
        val archive = validArchive()
        assertEquals(
            DualScreenPackageInstaller.Result.Installed(titleId),
            DualScreenPackageInstaller.installArchive(
                archive.inputStream(), "$titleId.dsmod.zip", archive.size.toLong(), titleId,
                addonDirectory
            )
        )
        val installed = File(addonDirectory, "DualScreen-$titleId/dualscreen/data.json")
        assertEquals("data", installed.readText())

        val invalid = invalidMetadataArchive()
        assertEquals(
            DualScreenPackageInstaller.Result.Failed(
                DualScreenPackageInstaller.Error.InvalidMetadata
            ),
            DualScreenPackageInstaller.installArchive(
                invalid.inputStream(), "$titleId.dsmod.zip", invalid.size.toLong(), titleId,
                addonDirectory
            )
        )
        assertEquals("data", installed.readText())
    }

    @Test
    fun lowercaseFilenameIsRejected() {
        val archive = validArchive()
        assertEquals(
            DualScreenPackageInstaller.Result.Failed(
                DualScreenPackageInstaller.Error.InvalidFilename
            ),
            DualScreenPackageInstaller.installArchive(
                archive.inputStream(), "${titleId.lowercase()}.dsmod.zip", archive.size.toLong(),
                titleId, File(root, "load/$titleId")
            )
        )
    }

    @Test
    fun missingOrUnsupportedNativeLibraryIsRejected() {
        val hash = "00".repeat(32)
        val module = moduleJson(hash)
        writeMetadata(module = module)
        assertEquals(
            DualScreenPackageInstaller.Error.InvalidLayout,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())
        )

        val library = File(root, "dualscreen/modules/android-arm64-v8a/$titleId.so")
        checkNotNull(library.parentFile).mkdirs()
        library.writeBytes(byteArrayOf(1, 2, 3, 4))
        writeMetadata(module = moduleJson(sha256(library)))
        assertEquals(
            DualScreenPackageInstaller.Error.InvalidLayout,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, listOf("x86_64"))
        )
    }

    @Test
    fun activationFailureRestoresExistingPackage() {
        val destination = File(root, "load/DualScreen-$titleId")
        checkNotNull(destination.parentFile).mkdirs()
        File(destination, "package.json").apply {
            checkNotNull(parentFile).mkdirs()
            writeText("old package")
        }
        val staging = File(root, "staging-that-does-not-exist")
        val backup = File(root, "backup")

        try {
            DualScreenPackageInstaller.replaceAtomically(destination, staging, backup)
        } catch (_: java.io.IOException) {
            // Expected: the missing staging directory forces the activation rename to fail.
        }

        assertEquals("old package", File(destination, "package.json").readText())
        assertFalse(backup.exists())
    }

    private fun writeMetadata(
        packageTitleId: String = titleId,
        module: String? = null,
        manifestModule: String? = module,
        requiresModule: Boolean = false
    ) {
        val packageModule = module?.let { ",\"module\":$it" } ?: ""
        val manifestModuleValue = manifestModule?.let { ",\"module\":$it" } ?: ""
        File(root, "dualscreen").mkdirs()
        File(root, "package.json").writeText(
            """{"format":1,"type":"dual-screen-mod","title_id":"$packageTitleId","name":"Test","version":"1.0.0"$packageModule}"""
        )
        File(root, "dualscreen/manifest.json").writeText(
            """{"format":1,"title_id":"$titleId","pages":[{"id":"main"}]${if (requiresModule) ",\"requires_module\":true" else ""}$manifestModuleValue}"""
        )
    }

    private fun moduleJson(hash: String): String =
        """{"abi":1,"build_ids":["646761F643AFEBB3"],"libraries":{"android-arm64-v8a":{"path":"modules/android-arm64-v8a/$titleId.so","sha256":"$hash"}}}"""

    private fun validArchive(): ByteArray = zip(
        "package.json" to """{"format":1,"type":"dual-screen-mod","title_id":"$titleId","name":"Test","version":"1.0.0"}""",
        "dualscreen/" to "",
        "dualscreen/manifest.json" to """{"format":1,"title_id":"$titleId","pages":[{"id":"main"}]}""",
        "dualscreen/data.json" to "data"
    )

    private fun invalidMetadataArchive(): ByteArray = zip(
        "package.json" to "not-json",
        "dualscreen/manifest.json" to """{"format":1,"title_id":"$titleId","pages":[{"id":"main"}]}"""
    )

    private fun zip(vararg entries: Pair<String, String>): ByteArray {
        val output = java.io.ByteArrayOutputStream()
        ZipOutputStream(output).use { zip ->
            for ((name, contents) in entries) {
                zip.putNextEntry(ZipEntry(name))
                zip.write(contents.toByteArray())
                zip.closeEntry()
            }
        }
        return output.toByteArray()
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        digest.update(file.readBytes())
        return digest.digest().joinToString("") {
            (it.toInt() and 0xff).toString(16).padStart(2, '0')
        }
    }
}
