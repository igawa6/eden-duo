// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.yuzu.yuzu_emu.duo.DuoUpdate.InstallOutcome

class DuoUpdateTest {
    private val sha = "e356526f90546ede25b7893bc8d69ac488388aa64ed8005550768a04f200b56e"

    /** GitHub's releases/latest answer for v1.3.4, trimmed to the fields that matter. */
    private fun latestJson(
        tag: String = "v1.3.4",
        prerelease: Boolean = false,
        draft: Boolean = false,
        assets: String = asset("EdenDuo-1.3.4.apk", tag)
    ) = """
        {
          "html_url": "https://github.com/igawa6/eden-duo/releases/tag/$tag",
          "tag_name": "$tag",
          "name": "Eden Duo ${tag.removePrefix("v")}",
          "draft": $draft,
          "prerelease": $prerelease,
          "published_at": "2026-11-02T12:13:28Z",
          "assets": [$assets],
          "body": "Eden Duo is updated to the latest Eden nightly ([v1793569440.7a6d055094](https://git.eden-emu.dev/eden-ci/nightly/releases/tag/v1793569440.7a6d055094), 2026-11-02).\r\n\r\nUpdate: install over your current Eden Duo.\r\n\r\n- Requires Android 13+ (arm64) and a device with a second display.\r\n- **Companions**: https://github.com/igawa6/eden-duo-companions"
        }
    """.trimIndent()

    private fun asset(name: String, tag: String = "v1.3.4", repo: String = "igawa6/eden-duo") =
        """
        {
          "name": "$name",
          "content_type": "application/vnd.android.package-archive",
          "size": 22911033,
          "digest": "sha256:$sha",
          "browser_download_url": "https://github.com/$repo/releases/download/$tag/$name"
        }
        """.trimIndent()

    @Test
    fun ordersReleaseVersions() {
        assertTrue(DuoUpdate.compare("1.3.5", "1.3.4")!! > 0)
        assertTrue(DuoUpdate.compare("1.3.4", "1.3.5")!! < 0)
        assertTrue(DuoUpdate.compare("1.10.0", "1.9.9")!! > 0)
        assertTrue(DuoUpdate.compare("2.0", "1.99.99")!! > 0)
        assertEquals(0, DuoUpdate.compare("1.3.4", "1.3.4"))
        assertEquals(0, DuoUpdate.compare("v1.3.4", "1.3.4"))
        assertEquals(0, DuoUpdate.compare("1.3.4", "1.3.4.0"))
    }

    @Test
    fun ordersFourPartHotfixes() {
        assertTrue(DuoUpdate.compare("1.3.2.2", "1.3.3")!! < 0)
        assertTrue(DuoUpdate.compare("1.3.2.2", "1.3.2.1")!! > 0)
        assertTrue(DuoUpdate.compare("1.3.2.2", "1.3.2")!! > 0)
        assertTrue(DuoUpdate.isNewer("v1.3.3", "1.3.2.2"))
        assertFalse(DuoUpdate.isNewer("v1.3.2.2", "1.3.3"))
    }

    @Test
    fun preReleaseIsOlderThanItsRelease() {
        // A 1.3.5-beta.1 user is offered 1.3.5, but not 1.3.4.
        assertTrue(DuoUpdate.isNewer("1.3.5", "1.3.5-beta.1"))
        assertFalse(DuoUpdate.isNewer("1.3.4", "1.3.5-beta.1"))
        assertTrue(DuoUpdate.compare("1.3.5-beta.2", "1.3.5-beta.1")!! > 0)
        assertTrue(DuoUpdate.compare("1.3.5-beta.10", "1.3.5-beta.2")!! > 0)
        assertTrue(DuoUpdate.compare("1.3.5-rc.1", "1.3.5-beta.9")!! > 0)
        assertTrue(DuoUpdate.compare("1.3.5-beta.1", "1.3.5-test.1")!! < 0)
        assertTrue(DuoUpdate.compare("1.3.5-beta", "1.3.5-beta.1")!! < 0)
        assertEquals(0, DuoUpdate.compare("1.3.5+abc", "1.3.5+def"))
    }

    @Test
    fun buildTypeSuffixesAreNotPreReleases() {
        // Dev builds append upstream's build type to versionName.
        assertEquals(0, DuoUpdate.compare("1.3.5-relWithDebInfo", "1.3.5"))
        assertEquals(0, DuoUpdate.compare("1.3.5-debug", "1.3.5"))
        assertEquals(0, DuoUpdate.compare("1.3.5-beta.2-relWithDebInfo", "1.3.5-beta.2"))
        assertTrue(DuoUpdate.isNewer("1.3.5", "1.3.5-beta.2-debug"))
        assertFalse(DuoUpdate.isNewer("1.3.5", "1.3.5-relWithDebInfo"))
        // Other dev/test suffixes are pre-releases of that version.
        assertTrue(DuoUpdate.isNewer("1.3.5", "1.3.5-test.1"))
        assertTrue(DuoUpdate.isNewer("1.3.5", "1.3.5-dev"))
        assertFalse(DuoUpdate.isNewer("1.3.4", "1.3.5-dev"))
    }

    @Test
    fun unknownVersionsAreNeverOffered() {
        assertNull(DuoUpdate.compare("nightly", "1.3.4"))
        assertNull(DuoUpdate.compare("1.3.4", "e58e17dc69-duo-1.3.5"))
        assertNull(DuoUpdate.compare("1.2.3.4.5", "1.0.0"))
        assertNull(DuoUpdate.compare("", "1.0.0"))
        assertFalse(DuoUpdate.isNewer("1.3.5", "nightly"))
        assertFalse(DuoUpdate.isNewer("garbage", "1.3.4"))
    }

    @Test
    fun parsesTheLatestRelease() {
        val release = DuoUpdate.parseLatest(latestJson())
        assertNotNull(release)
        release!!
        assertEquals("1.3.4", release.version)
        assertEquals("v1.3.4", release.tag)
        assertEquals("EdenDuo-1.3.4.apk", release.assetName)
        assertEquals(
            "https://github.com/igawa6/eden-duo/releases/download/v1.3.4/EdenDuo-1.3.4.apk",
            release.downloadUrl
        )
        assertEquals(22911033L, release.size)
        assertEquals(sha, release.sha256)
        assertEquals("https://github.com/igawa6/eden-duo/releases/tag/v1.3.4", release.pageUrl)
        assertTrue(release.notes.startsWith("Eden Duo is updated"))
    }

    @Test
    fun parsesFourPartTagsAndPrefersTheNamedAsset() {
        val assets = listOf(
            asset("EdenDuo-1.3.2.1.apk", "v1.3.2.2"),
            asset("EdenDuo-1.3.2.2.apk", "v1.3.2.2"),
            asset("1.3.2.2.zip", "v1.3.2.2")
        ).joinToString(",")
        val release = DuoUpdate.parseLatest(latestJson(tag = "v1.3.2.2", assets = assets))!!
        assertEquals("1.3.2.2", release.version)
        assertEquals("EdenDuo-1.3.2.2.apk", release.assetName)
    }

    @Test
    fun rejectsReleasesThatCannotBeInstalled() {
        assertNull(DuoUpdate.parseLatest(latestJson(prerelease = true)))
        assertNull(DuoUpdate.parseLatest(latestJson(draft = true)))
        assertNull(DuoUpdate.parseLatest(latestJson(tag = "nightly-2026")))
        assertNull(DuoUpdate.parseLatest(latestJson(assets = "")))
        assertNull(DuoUpdate.parseLatest(latestJson(assets = asset("Eden-1.3.4.apk"))))
        // An APK served from another repository is not Eden Duo's.
        assertNull(
            DuoUpdate.parseLatest(
                latestJson(assets = asset("EdenDuo-1.3.4.apk", repo = "someone/else"))
            )
        )
        assertNull(DuoUpdate.parseLatest("""{"message": "API rate limit exceeded"}"""))
        assertNull(DuoUpdate.parseLatest("[]"))
        assertNull(DuoUpdate.parseLatest("not json"))
        assertNull(DuoUpdate.parseLatest(""))
    }

    @Test
    fun missingDigestStillParses() {
        val noDigest = asset("EdenDuo-1.3.4.apk").replace(Regex(""""digest": "[^"]*",\s*"""), "")
        val release = DuoUpdate.parseLatest(latestJson(assets = noDigest))!!
        assertNull(release.sha256)
    }

    @Test
    fun offersOnlyNewerUnskippedReleases() {
        val r134 = DuoUpdate.parseLatest(latestJson())!!
        val r135 = DuoUpdate.parseLatest(
            latestJson(tag = "v1.3.5", assets = asset("EdenDuo-1.3.5.apk", "v1.3.5"))
        )!!
        // An installed 1.3.5-beta.2 while v1.3.4 is the latest stable release: nothing.
        assertFalse(DuoUpdate.shouldOffer(r134, "1.3.5-beta.2", null))
        assertFalse(DuoUpdate.shouldOffer(r134, "1.3.4", null))
        assertTrue(DuoUpdate.shouldOffer(r134, "1.3.3", null))
        assertTrue(DuoUpdate.shouldOffer(r134, "1.3.2.2", null))
        assertTrue(DuoUpdate.shouldOffer(r135, "1.3.5-beta.2", null))
        assertFalse(DuoUpdate.shouldOffer(r135, "1.3.5-beta.2", "1.3.5"))
        assertFalse(DuoUpdate.shouldOffer(r135, "1.3.5-beta.2", "v1.3.5"))
        assertTrue(DuoUpdate.shouldOffer(r135, "1.3.4", "1.3.4.9"))
        assertFalse(DuoUpdate.shouldOffer(null, "1.3.4", null))
    }

    @Test
    fun checksAtMostOnceADay() {
        val day = DuoUpdate.CHECK_INTERVAL_MS
        assertTrue(DuoUpdate.isDue(0, 1_000))
        assertFalse(DuoUpdate.isDue(10 * day, 10 * day + day - 1))
        assertTrue(DuoUpdate.isDue(10 * day, 11 * day))
        // A clock set back does not silence the check for days.
        assertTrue(DuoUpdate.isDue(10 * day, 9 * day))
    }

    @Test
    fun releaseNotesBecomePlainText() {
        val notes = DuoUpdate.plainNotes(DuoUpdate.parseLatest(latestJson())!!.notes)
        assertEquals(
            "Eden Duo is updated to the latest Eden nightly (v1793569440.7a6d055094, " +
                "2026-11-02).\n\nUpdate: install over your current Eden Duo.\n\n" +
                "• Requires Android 13+ (arm64) and a device with a second display.\n" +
                "• Companions: https://github.com/igawa6/eden-duo-companions",
            notes
        )
        assertEquals("Fixed\n\nA b c", DuoUpdate.plainNotes("## Fixed\n\n\n\nA `b` c"))
    }

    @Test
    fun longNotesAreCutAtAWord() {
        val long = (1..400).joinToString(" ") { "word$it" }
        val cut = DuoUpdate.plainNotes(long, maxChars = 100)
        assertTrue(cut.length <= 101)
        assertTrue(cut.endsWith("…"))
        assertTrue(long.startsWith(cut.removeSuffix("…")))
        assertFalse(cut.removeSuffix("…").endsWith(" "))
        assertEquals("short", DuoUpdate.plainNotes("short", maxChars = 100))
    }

    @Test
    fun mapsInstallerStatuses() {
        // PackageInstaller.STATUS_* values (public API constants).
        assertEquals(InstallOutcome.SUCCESS, DuoUpdate.installOutcome(0, null))
        assertEquals(InstallOutcome.NEEDS_CONFIRMATION, DuoUpdate.installOutcome(-1, null))
        assertEquals(InstallOutcome.FAILED, DuoUpdate.installOutcome(1, "INSTALL_FAILED_INTERNAL"))
        assertEquals(InstallOutcome.BLOCKED, DuoUpdate.installOutcome(2, null))
        assertEquals(InstallOutcome.CANCELLED, DuoUpdate.installOutcome(3, null))
        assertEquals(InstallOutcome.INVALID, DuoUpdate.installOutcome(4, "INSTALL_PARSE_FAILED"))
        assertEquals(InstallOutcome.CONFLICT, DuoUpdate.installOutcome(5, "UPDATE_INCOMPATIBLE"))
        assertEquals(InstallOutcome.STORAGE, DuoUpdate.installOutcome(6, null))
        assertEquals(InstallOutcome.INCOMPATIBLE, DuoUpdate.installOutcome(7, null))
        assertEquals(InstallOutcome.FAILED, DuoUpdate.installOutcome(8, null))
        assertEquals(InstallOutcome.FAILED, DuoUpdate.installOutcome(42, null))
    }

    @Test
    fun aLowerVersionCodeIsADowngrade() {
        // Android's message for an older versionCode (status STATUS_FAILURE_INCOMPATIBLE).
        val message = "INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected: Update version " +
            "code 34201485 is older than current 34254234"
        assertEquals(InstallOutcome.DOWNGRADE, DuoUpdate.installOutcome(7, message))
        assertEquals(InstallOutcome.DOWNGRADE, DuoUpdate.installOutcome(1, message))
        assertEquals(
            InstallOutcome.INCOMPATIBLE,
            DuoUpdate.installOutcome(7, "INSTALL_FAILED_OLDER_SDK")
        )
    }
}
