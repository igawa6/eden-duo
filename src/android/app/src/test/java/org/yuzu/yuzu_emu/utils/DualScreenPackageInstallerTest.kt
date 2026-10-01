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
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.json.JSONObject

class DualScreenPackageInstallerTest {
    private companion object {
        /** Core::Mods::DualScreenRuntimeVersion of this tree. */
        const val PUBLISHED_RUNTIME = 15
    }

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
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
        )

        File(root, "package.json").writeText("{\"padding\":\"${"x".repeat(4 * 1024 * 1024)}\"}")
        assertEquals(
            DualScreenPackageInstaller.Error.ArchiveTooLarge,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
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
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
        )
        library.appendBytes(byteArrayOf(5))
        assertEquals(
            DualScreenPackageInstaller.Error.ChecksumMismatch,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
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
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
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
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
        )
    }

    @Test
    fun requiredModuleCannotBeOmitted() {
        writeMetadata(requiresModule = true)
        assertEquals(
            DualScreenPackageInstaller.Error.InvalidMetadata,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
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

        assertEquals("DualScreen-$titleId", installedFolder(result))
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
            "DualScreen-$titleId",
            installedFolder(
                DualScreenPackageInstaller.installArchive(
                        archive.inputStream(), "$titleId.dsmod.zip", archive.size.toLong(), titleId,
                        addonDirectory
                )
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

        assertEquals(
            DualScreenPackageInstaller.Result.Installed(
                "TestDS-1.0.0",
                removed = listOf("DualScreen-$titleId", "TestDS-0.9.0")
            ),
            result
        )
        assertEquals("data", File(addonDirectory, "TestDS-1.0.0/dualscreen/data.json").readText())
        assertFalse(File(addonDirectory, "DualScreen-$titleId").exists())
        assertFalse(File(addonDirectory, "TestDS-0.9.0").exists())
        assertEquals(true, File(addonDirectory, "OtherMod/romfs").isDirectory)
        assertFalse(File(root, "load/.TestDS-1.0.0.backup").exists())
        assertEquals(listOf(titleId), File(root, "load").list()?.toList())
    }

    @Test
    fun otherDualScreenFoldersForThisGameAreReported() {
        val addonDirectory = File(root, "load/$titleId")
        val pages = """"pages":[{"id":"p"}]"""
        fun folder(name: String, manifest: String) =
            File(addonDirectory, "$name/dualscreen/manifest.json").apply {
                checkNotNull(parentFile).mkdirs()
                writeText(manifest)
            }
        folder("MetroidDreadDS", """{"title_id":"$titleId",$pages}""") // hand-copied
        folder("AAMixed", """{$pages}""")
        File(addonDirectory, "AAMixed/romfs").mkdirs()
        folder("OtherGame", """{"title_id":"010093801237C001",$pages}""")
        folder("Broken", "not json")
        File(addonDirectory, "OnlyRomfs/romfs").mkdirs()

        val archive = validArchive()
        val result = DualScreenPackageInstaller.installArchive(
            archive.inputStream(), "$titleId-TestDS-1.0.0.dsmod.zip", archive.size.toLong(),
            titleId, addonDirectory
        ) as DualScreenPackageInstaller.Result.Installed
        assertEquals(
            listOf(
                DualScreenPackageInstaller.OtherPackage("AAMixed", onlyDualScreen = false),
                DualScreenPackageInstaller.OtherPackage("MetroidDreadDS", onlyDualScreen = true)
            ),
            result.otherPackages
        )
        assertTrue(result.removed.isEmpty() && result.notRemoved.isEmpty())
        // Nothing without package.json is deleted.
        assertTrue(File(addonDirectory, "MetroidDreadDS/dualscreen/manifest.json").isFile)
    }

    @Test
    fun undeletableOldVersionIsReportedNotSilentlyKept() {
        val addonDirectory = File(root, "load/$titleId")
        val old = File(addonDirectory, "TestDS-0.9.0")
        File(old, "dualscreen").mkdirs()
        File(old, "package.json").writeText(packageJson("0.9.0"))
        File(old, "dualscreen/manifest.json").writeText("""{"pages":[{"id":"p"}]}""")
        assumeTrue(File(old, "dualscreen").setWritable(false))
        try {
            // Running as root ignores modes: nothing to test then.
            assumeTrue(runCatching { File(old, "dualscreen/probe").createNewFile() }.isFailure)
            val archive = validArchive()
            val result = DualScreenPackageInstaller.installArchive(
                archive.inputStream(), "$titleId-TestDS-1.0.0.dsmod.zip", archive.size.toLong(),
                titleId, addonDirectory
            ) as DualScreenPackageInstaller.Result.Installed
            assertEquals(listOf("TestDS-0.9.0"), result.notRemoved)
            assertTrue(result.otherPackages.isEmpty())
        } finally {
            File(old, "dualscreen").setWritable(true)
        }
    }

    @Test
    fun interruptedUpdateIsRecoveredPerTitle() {
        val addonDirectory = File(root, "load/$titleId")
        addonDirectory.mkdirs()
        // An update of TestDS-1.0.0 stopped after moving the old folder aside.
        val backup = File(root, "load/.DualScreen-$titleId-TestDS-1.0.0.backup")
        File(backup, "dualscreen").mkdirs()
        File(backup, "package.json").writeText(packageJson("1.0.0"))
        File(backup, "dualscreen/manifest.json").writeText("""{"pages":[{"id":"p"}]}""")
        // Another game's backup with the same folder name must not be touched.
        val foreign = File(root, "load/.DualScreen-010093801237C001-TestDS-1.0.0.backup")
        File(foreign, "dualscreen").mkdirs()

        val archive = packageArchive(version = "1.1.0")
        val result = DualScreenPackageInstaller.installArchive(
            archive.inputStream(), "$titleId-TestDS-1.1.0.dsmod.zip", archive.size.toLong(),
            titleId, addonDirectory
        ) as DualScreenPackageInstaller.Result.Installed
        // The old version was put back first, then replaced as a superseded install.
        assertEquals("TestDS-1.1.0", result.folderName)
        assertEquals(listOf("TestDS-1.0.0"), result.removed)
        assertFalse(backup.exists())
        assertTrue(foreign.isDirectory)
    }

    @Test
    fun namedZipVersionMustMatchPackageVersion() {
        val addonDirectory = File(root, "load/$titleId")
        val archive = validArchive()
        val result = DualScreenPackageInstaller.installArchive(
            archive.inputStream(), "$titleId-TestDS-2.0.0.dsmod.zip", archive.size.toLong(),
            titleId, addonDirectory
        )
        assertEquals(DualScreenPackageInstaller.Error.VersionMismatch, errorOf(result))
        assertFalse(File(addonDirectory, "TestDS-2.0.0").exists())
    }

    @Test
    fun failedReplacementRetainsPreviousInstalledPackage() {
        val addonDirectory = File(root, "load/$titleId")
        val archive = validArchive()
        assertEquals(
            "DualScreen-$titleId",
            installedFolder(
                DualScreenPackageInstaller.installArchive(
                        archive.inputStream(), "$titleId.dsmod.zip", archive.size.toLong(), titleId,
                        addonDirectory
                )
            )
        )
        val installed = File(addonDirectory, "DualScreen-$titleId/dualscreen/data.json")
        assertEquals("data", installed.readText())

        val invalid = invalidMetadataArchive()
        assertEquals(
            DualScreenPackageInstaller.Error.InvalidMetadata,
            errorOf(
                DualScreenPackageInstaller.installArchive(
                    invalid.inputStream(), "$titleId.dsmod.zip", invalid.size.toLong(), titleId,
                    addonDirectory
                )
            )
        )
        assertEquals("data", installed.readText())
    }

    @Test
    fun renamedDownloadsInstallFromPackageJson() {
        val archive = packageArchive(version = "1.0.0")
        fun install(name: String) = DualScreenPackageInstaller.installArchive(
            archive.inputStream(), name, archive.size.toLong(), titleId, File(root, "load/$titleId")
        )
        fun folderOf(name: String) = installedFolder(install(name))
        // A browser's copy suffix, before or inside the extension.
        assertEquals("TestDS-1.0.0", folderOf("$titleId-TestDS-1.0.0 (1).dsmod.zip"))
        assertEquals("TestDS-1.0.0", folderOf("$titleId-TestDS-1.0.0.dsmod (2).zip"))
        // The legacy name in lower case.
        assertEquals("DualScreen-$titleId", folderOf("${titleId.lowercase()}.dsmod.zip"))
        // Any other name: the folder comes from package.json's name and version.
        assertEquals("Test-1.0.0", folderOf("pkg.zip"))
        assertEquals("Test-1.0.0", folderOf(""))
        assertEquals("Test-1.0.0", folderOf("$titleId-TestDS-1.0.0-final.dsmod.zip"))
        // A title ID in the name must still be this game's.
        assertEquals(
            DualScreenPackageInstaller.Error.TitleMismatch,
            errorOf(install("010093801237C001-TestDS-1.0.0.dsmod.zip"))
        )
        assertEquals(
            DualScreenPackageInstaller.Error.TitleMismatch,
            errorOf(install("010093801237c001 companion.zip"))
        )
        // One package per title: only the last install is left.
        assertEquals(
            listOf("Test-1.0.0"),
            File(root, "load/$titleId").list()?.sorted()
        )
    }

    @Test
    fun archiveNamesAreParsedLeniently() {
        fun parse(name: String) = DualScreenPackageInstaller.parseArchiveName(name)
        assertEquals(
            DualScreenPackageInstaller.ArchiveName(titleId, "TestDS", "1.0.0", exact = true),
            parse("$titleId-TestDS-1.0.0.dsmod.zip")
        )
        assertEquals(
            DualScreenPackageInstaller.ArchiveName(titleId, exact = true),
            parse("$titleId.dsmod.zip")
        )
        assertEquals(DualScreenPackageInstaller.ArchiveName(titleId), parse("$titleId notes.zip"))
        assertEquals(DualScreenPackageInstaller.ArchiveName(), parse("my companion.zip"))
        assertEquals(DualScreenPackageInstaller.ArchiveName(), parse("${titleId}0.dsmod.zip"))
    }

    @Test
    fun progressFollowsArchiveBytesAndCancelKeepsInstalledPackage() {
        val addonDirectory = File(root, "load/$titleId")
        val random = java.util.Random(1)
        val big = buildString { repeat(1_500_000) { append('a' + random.nextInt(26)) } }
        val archive = packageArchive("1.0.0", "", "", "dualscreen/big.txt" to big)
        val name = "$titleId-TestDS-1.0.0.dsmod.zip"
        val size = archive.size.toLong()

        val reports = mutableListOf<Pair<Long, Long>>()
        assertEquals(
            "TestDS-1.0.0",
            installedFolder(
                DualScreenPackageInstaller.installArchive(
                        archive.inputStream(), name, size, titleId, addonDirectory,
                        { max, progress -> reports += max to progress; false }
                )
            )
        )
        assertTrue(reports.size > 3)
        assertTrue(reports.all { it.first == size && it.second in 0..size })
        assertTrue(reports.last().second > size / 2)

        // Unknown size: (0, 0) keeps the bar indeterminate.
        reports.clear()
        DualScreenPackageInstaller.installArchive(
            archive.inputStream(), name, -1L, titleId, addonDirectory,
            { max, progress -> reports += max to progress; false }
        )
        assertTrue(reports.isNotEmpty() && reports.all { it == 0L to 0L })

        // Cancelling inside the large entry stops the install and keeps what was installed.
        File(addonDirectory, "TestDS-1.0.0/dualscreen/marker").writeText("kept")
        assertEquals(
            DualScreenPackageInstaller.Error.Cancelled,
            errorOf(
                DualScreenPackageInstaller.installArchive(
                    archive.inputStream(), name, size, titleId, addonDirectory,
                    { _, progress -> progress > size / 3 }
                )
            )
        )
        assertEquals("kept", File(addonDirectory, "TestDS-1.0.0/dualscreen/marker").readText())
        assertFalse(File(root, "load/.DualScreen-$titleId.staging").exists())
    }

    @Test
    fun archiverJunkIsSkipped() {
        val addonDirectory = File(root, "load/$titleId")
        val archive = packageArchive(
            "1.0.0", "", "",
            "__MACOSX/" to "",
            "__MACOSX/dualscreen/._manifest.json" to "x",
            ".DS_Store" to "x",
            "dualscreen/.DS_Store" to "x",
            "dualscreen/._data.json" to "x",
            "dualscreen/Thumbs.db" to "x"
        )
        assertEquals(
            "TestDS-1.0.0",
            installedFolder(
                DualScreenPackageInstaller.installArchive(
                        archive.inputStream(), "$titleId-TestDS-1.0.0.dsmod.zip", archive.size.toLong(),
                        titleId, addonDirectory
                )
            )
        )
        val installed = File(addonDirectory, "TestDS-1.0.0")
        assertEquals(listOf("dualscreen", "package.json"), installed.list()?.sorted())
        assertEquals(
            listOf("data.json", "manifest.json"),
            File(installed, "dualscreen").list()?.sorted()
        )
        assertNull(
            DualScreenPackageInstaller.validateArchiveEntriesForTest(
                listOf("__MACOSX/", "__MACOSX/x", "package.json", "Thumbs.db")
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
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, emptyList())?.error
        )

        val library = File(root, "dualscreen/modules/android-arm64-v8a/$titleId.so")
        checkNotNull(library.parentFile).mkdirs()
        library.writeBytes(byteArrayOf(1, 2, 3, 4))
        writeMetadata(module = moduleJson(sha256(library)))
        assertEquals(
            DualScreenPackageInstaller.Error.MissingPlatformLibrary,
            DualScreenPackageInstaller.validateExtractedPackage(root, titleId, listOf("x86_64"))?.error
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

    @Test
    fun packageMinRuntimeFollowsTheCoreRule() {
        fun required(manifest: String?, packageJson: String? = null) =
            DualScreenPackageInstaller.packageMinRuntime(
                manifest?.let { JSONObject(it) },
                packageJson?.let { JSONObject(it) }
            )
        val unknown = DualScreenPackageInstaller.UNKNOWN_RUNTIME
        assertEquals(0L, required("{}"))
        assertEquals(0L, required(null, null))
        assertEquals(0L, required("""{"min_runtime":null}"""))
        assertEquals(12L, required("""{"min_runtime":12}"""))
        assertEquals(12L, required("""{"min_runtime":"12"}"""))
        assertEquals(15L, required("""{"min_runtime":11}""", """{"min_runtime":15}"""))
        assertEquals(15L, required("""{"min_runtime":"15"}""", """{"min_runtime":11}"""))
        assertEquals(unknown, required("""{"min_runtime":"12a"}"""))
        assertEquals(unknown, required("""{"min_runtime":""}"""))
        assertEquals(unknown, required("""{"min_runtime":"1234567890"}"""))
        assertEquals(unknown, required("""{"min_runtime":-1}"""))
        assertEquals(unknown, required("""{"min_runtime":12.5}"""))
        assertEquals(unknown, required("""{"min_runtime":true}"""))
        assertEquals(unknown, required("""{"min_runtime":[12]}"""))
        assertEquals(unknown, required("""{"min_runtime":99999999999}"""))
    }

    @Test
    fun packageForNewerRuntimeIsRefusedAndInstalledVersionKept() {
        val addonDirectory = File(root, "load/$titleId")
        val v1 = packageArchive(version = "1.0.0", manifestExtra = ""","min_runtime":12""")
        assertEquals(
            "TestDS-1.0.0",
            installedFolder(
                DualScreenPackageInstaller.installArchive(
                        v1.inputStream(), "$titleId-TestDS-1.0.0.dsmod.zip", v1.size.toLong(), titleId,
                        addonDirectory, runtimeVersion = 14
                )
            )
        )

        val v2 = packageArchive(version = "1.1.0", manifestExtra = ""","min_runtime":15""")
        val refused = DualScreenPackageInstaller.installArchive(
            v2.inputStream(), "$titleId-TestDS-1.1.0.dsmod.zip", v2.size.toLong(), titleId,
            addonDirectory, runtimeVersion = 14
        ) as DualScreenPackageInstaller.Result.Failed
        assertEquals(DualScreenPackageInstaller.Error.RuntimeTooOld, refused.error)
        assertEquals(15L, refused.requiredRuntime)
        assertEquals(14, refused.runtime)
        assertTrue(File(addonDirectory, "TestDS-1.0.0/dualscreen/manifest.json").isFile)
        assertFalse(File(addonDirectory, "TestDS-1.1.0").exists())

        // An unreadable requirement is refused too; one equal to the app's runtime installs.
        val v3 = packageArchive(version = "1.1.0", packageExtra = ""","min_runtime":"x"""")
        assertEquals(
            DualScreenPackageInstaller.Error.RuntimeTooOld,
            errorOf(
                DualScreenPackageInstaller.installArchive(
                    v3.inputStream(), "$titleId-TestDS-1.1.0.dsmod.zip", v3.size.toLong(),
                    titleId, addonDirectory, runtimeVersion = 14
                )
            )
        )
        val v4 = packageArchive(version = "1.1.0", packageExtra = ""","min_runtime":"14"""")
        assertEquals(
            "TestDS-1.1.0",
            installedFolder(
                DualScreenPackageInstaller.installArchive(
                        v4.inputStream(), "$titleId-TestDS-1.1.0.dsmod.zip", v4.size.toLong(), titleId,
                        addonDirectory, runtimeVersion = 14
                )
            )
        )
        assertFalse(File(addonDirectory, "TestDS-1.0.0").exists())
    }

    @Test
    fun declarativeManifestFollowsTheRuntimeRules() {
        File(root, "dualscreen").mkdirs()
        File(root, "package.json").writeText(packageJson("1.0.0"))
        val manifest = File(root, "dualscreen/manifest.json")

        // title_id and format are optional without a module; debug_page alone is a usable page.
        manifest.writeText("""{"debug_page":true}""")
        assertNull(validate())
        manifest.writeText("""{"title_id":"${titleId.lowercase()}","pages":[{"id":"p"}]}""")
        assertNull(validate())

        manifest.writeText("""{"format":1,"pages":[]}""")
        assertEquals(DualScreenPackageInstaller.Error.InvalidMetadata, validate())
        manifest.writeText("""{"format":2,"pages":[{"id":"p"}]}""")
        assertEquals(DualScreenPackageInstaller.Error.InvalidMetadata, validate())
        manifest.writeText("""{"title_id":"010093801237C001","pages":[{"id":"p"}]}""")
        assertEquals(DualScreenPackageInstaller.Error.TitleMismatch, validate())
    }

    @Test
    fun moduleNeedsManifestTitleIdAndAcceptsEitherHashCase() {
        val library = File(root, "dualscreen/modules/android-arm64-v8a/$titleId.so")
        checkNotNull(library.parentFile).mkdirs()
        library.writeBytes(byteArrayOf(1, 2, 3, 4))
        writeMetadata(module = moduleJson(sha256(library).uppercase()))
        assertNull(validate())

        val module = moduleJson(sha256(library))
        File(root, "dualscreen/manifest.json").writeText(
            """{"format":1,"pages":[{"id":"main"}],"module":$module}"""
        )
        assertEquals(DualScreenPackageInstaller.Error.InvalidMetadata, validate())
    }

    @Test
    fun unwritableAddonDirectoryIsReportedWithItsPath() {
        val load = File(root, "load")
        load.mkdirs()
        assumeTrue(load.setWritable(false))
        try {
            val probe = File(load, "probe")
            assumeTrue(!probe.mkdir()) // running as root: permissions are not enforced
            val archive = validArchive()
            val result = DualScreenPackageInstaller.installArchive(
                archive.inputStream(), "$titleId.dsmod.zip", archive.size.toLong(), titleId,
                File(load, titleId)
            ) as DualScreenPackageInstaller.Result.Failed
            assertEquals(DualScreenPackageInstaller.Error.CannotWrite, result.error)
            assertEquals(File(load, titleId).path, result.detail)
        } finally {
            load.setWritable(true)
        }
    }

    /**
     * Regression check against real release archives: set DSMOD_PUBLISHED_PACKAGES to a folder
     * of published .dsmod.zip files (skipped otherwise). Each must install under this app's
     * runtime as its "<Name>-<version>" folder and carry an arm64 module when it has one.
     */
    @Test
    fun publishedPackagesStillInstall() {
        val folder = System.getenv("DSMOD_PUBLISHED_PACKAGES")?.let { File(it) }
        assumeTrue(folder != null && folder.isDirectory)
        val archives = checkNotNull(folder).listFiles { f -> f.name.endsWith(".dsmod.zip") }
            .orEmpty().sortedBy { it.name }
        assumeTrue(archives.isNotEmpty())
        for (archive in archives) {
            val name = DualScreenPackageInstaller.parseArchiveName(archive.name)
            val game = checkNotNull(name.titleId) { archive.name }
            val addonDirectory = File(root, "load/$game")
            val result = archive.inputStream().use {
                DualScreenPackageInstaller.installArchive(
                    it, archive.name, archive.length(), game, addonDirectory,
                    runtimeVersion = PUBLISHED_RUNTIME
                )
            }
            assertEquals(archive.name, "${name.label}-${name.version}", installedFolder(result))
            assertNull(
                archive.name,
                DualScreenPackageInstaller.validateExtractedPackage(
                    File(addonDirectory, "${name.label}-${name.version}"), game,
                    listOf("arm64-v8a")
                )
            )
        }
    }

    private fun validate(abis: List<String> = emptyList()) =
        DualScreenPackageInstaller.validateExtractedPackage(root, titleId, abis)?.error

    private fun packageJson(version: String, extra: String = "") =
        """{"format":1,"type":"dual-screen-mod","title_id":"$titleId","name":"Test","version":"$version"$extra}"""

    private fun packageArchive(
        version: String = "1.0.0",
        packageExtra: String = "",
        manifestExtra: String = "",
        vararg extraEntries: Pair<String, String>
    ): ByteArray = zip(
        "package.json" to packageJson(version, packageExtra),
        "dualscreen/" to "",
        "dualscreen/manifest.json" to
            """{"format":1,"title_id":"$titleId","pages":[{"id":"main"}]$manifestExtra}""",
        "dualscreen/data.json" to "data",
        *extraEntries
    )

    private fun installedFolder(result: DualScreenPackageInstaller.Result) =
        (result as? DualScreenPackageInstaller.Result.Installed)?.folderName

    private fun errorOf(result: DualScreenPackageInstaller.Result) =
        (result as? DualScreenPackageInstaller.Result.Failed)?.error

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
