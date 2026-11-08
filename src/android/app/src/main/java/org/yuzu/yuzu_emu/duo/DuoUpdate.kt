// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import java.math.BigInteger

/**
 * Eden Duo: the pure part of the in-app update (DuoUpdater does the network, prompt and
 * install). Eden Duo's own releases are read from GitHub; upstream's update checker stays off
 * (duo.gradle) because it reads upstream Eden's feed.
 */
object DuoUpdate {
    const val REPOSITORY = "igawa6/eden-duo"
    const val LATEST_URL = "https://api.github.com/repos/$REPOSITORY/releases/latest"
    private const val DOWNLOAD_PREFIX = "https://github.com/$REPOSITORY/releases/download/"

    /** The automatic check runs at most once in this many milliseconds. */
    const val CHECK_INTERVAL_MS = 24L * 60 * 60 * 1000

    /** The longest release notes the prompt shows; longer notes are cut at a word. */
    const val NOTES_MAX_CHARS = 700

    data class Release(
        /** The release version without the tag's "v", e.g. "1.3.4" or "1.3.2.2". */
        val version: String,
        val tag: String,
        val notes: String,
        val pageUrl: String,
        val assetName: String,
        val downloadUrl: String,
        val size: Long,
        /** The asset's SHA-256 from GitHub's digest field, lower case; null when not given. */
        val sha256: String?
    )

    private val mapper = jacksonObjectMapper()

    // Up to four numeric parts (Eden Duo hotfixes have four: 1.3.2.2), then an optional
    // SemVer pre-release ("-beta.1") and build metadata ("+...").
    private val versionPattern =
        Regex("""^[vV]?(\d+(?:\.\d+){0,3})(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$""")

    // Build-type suffixes upstream's build.gradle.kts appends to versionName; they say how a
    // build was made, not that it is older than the release of the same version.
    private val buildTypeSuffix = Regex("""-(relWithDebInfo|debug)$""", RegexOption.IGNORE_CASE)

    private val assetPattern = Regex("""^EdenDuo-[0-9A-Za-z.+-]+\.apk$""")

    /**
     * Orders two Eden Duo versions: negative when [left] is older, 0 when equal, positive when
     * newer; null when either is not a version (an unknown build is never offered anything).
     * Missing numeric parts count as 0 ("1.3.4" == "1.3.4.0"); a pre-release is older than the
     * release of the same version and pre-releases compare by SemVer identifiers.
     */
    fun compare(left: String, right: String): Int? {
        val a = parse(left) ?: return null
        val b = parse(right) ?: return null
        val an = a.groupValues[1].split('.').map(::BigInteger)
        val bn = b.groupValues[1].split('.').map(::BigInteger)
        for (i in 0 until maxOf(an.size, bn.size)) {
            val difference = an.getOrElse(i) { BigInteger.ZERO }
                .compareTo(bn.getOrElse(i) { BigInteger.ZERO })
            if (difference != 0) return difference
        }
        return comparePreRelease(a.groupValues[2], b.groupValues[2])
    }

    private fun parse(version: String): MatchResult? =
        versionPattern.matchEntire(version.trim().replace(buildTypeSuffix, ""))

    private fun comparePreRelease(left: String, right: String): Int {
        if (left == right) return 0
        if (left.isEmpty()) return 1
        if (right.isEmpty()) return -1
        val a = left.split('.')
        val b = right.split('.')
        for (i in 0 until minOf(a.size, b.size)) {
            val an = a[i].toBigIntegerOrNull()
            val bn = b[i].toBigIntegerOrNull()
            val difference = when {
                an != null && bn != null -> an.compareTo(bn)
                an != null -> -1
                bn != null -> 1
                else -> a[i].compareTo(b[i])
            }
            if (difference != 0) return difference
        }
        return a.size.compareTo(b.size)
    }

    /** Whether [latest] is newer than the installed [current] version. */
    fun isNewer(latest: String, current: String): Boolean = (compare(latest, current) ?: 0) > 0

    /**
     * Whether to offer [release] to a user on [current]: it must be newer, and the automatic
     * check also leaves out the version the user chose to skip ([skipped]).
     */
    fun shouldOffer(release: Release?, current: String, skipped: String?): Boolean =
        release != null && isNewer(release.version, current) &&
            (skipped == null || compare(release.version, skipped) != 0)

    fun isDue(lastCheckMs: Long, nowMs: Long): Boolean =
        lastCheckMs <= 0 || nowMs < lastCheckMs || nowMs - lastCheckMs >= CHECK_INTERVAL_MS

    /**
     * The release in GitHub's releases/latest answer ([json]), or null when it is a draft or a
     * pre-release, its tag is not a version, or it has no Eden Duo APK from this repository.
     */
    fun parseLatest(json: String): Release? {
        val release = runCatching { mapper.readTree(json) }.getOrNull() ?: return null
        if (!release.isObject) return null
        if (release.path("draft").asBoolean() || release.path("prerelease").asBoolean()) {
            return null
        }
        val tag = release.path("tag_name").asText("")
        if (versionPattern.matchEntire(tag) == null) return null
        val version = tag.removePrefix("v").removePrefix("V")
        val assets = release.path("assets").filter { asset ->
            assetPattern.matches(asset.path("name").asText("")) &&
                asset.path("browser_download_url").asText("").startsWith(DOWNLOAD_PREFIX)
        }
        // Prefer the asset named after the release (EdenDuo-1.3.4.apk) should there be several.
        val asset = assets.firstOrNull { it.path("name").asText() == "EdenDuo-$version.apk" }
            ?: assets.firstOrNull() ?: return null
        val digest = asset.path("digest").asText("")
        val sha256 = digest.removePrefix("sha256:").lowercase()
            .takeIf { digest.startsWith("sha256:") && it.matches(Regex("[0-9a-f]{64}")) }
        return Release(
            version = version,
            tag = tag,
            notes = release.path("body").asText(""),
            pageUrl = release.path("html_url").asText("")
                .takeIf { it.startsWith("https://github.com/$REPOSITORY/") }
                ?: "https://github.com/$REPOSITORY/releases/tag/$tag",
            assetName = asset.path("name").asText(),
            downloadUrl = asset.path("browser_download_url").asText(),
            size = asset.path("size").asLong(0),
            sha256 = sha256
        )
    }

    /**
     * Release notes (GitHub Markdown) as plain text for the prompt: links keep their text,
     * emphasis, heading and code marks go, list dashes become bullets, and the text is cut to
     * [maxChars] at a word boundary.
     */
    fun plainNotes(markdown: String, maxChars: Int = NOTES_MAX_CHARS): String {
        val text = markdown.replace("\r\n", "\n")
            .replace(Regex("""!\[([^\]]*)]\([^)]*\)"""), "$1")
            .replace(Regex("""\[([^\]]+)]\([^)]*\)"""), "$1")
            .replace(Regex("""(?m)^#{1,6}[ \t]*"""), "")
            .replace(Regex("""(?m)^[ \t]*[-*][ \t]+"""), "• ")
            .replace(Regex("""\*\*|__|`"""), "")
            .replace(Regex("""\n{3,}"""), "\n\n")
            .trim()
        if (text.length <= maxChars) return text
        val cut = text.substring(0, maxChars)
        val end = cut.lastIndexOfAny(charArrayOf(' ', '\n')).takeIf { it > maxChars / 2 }
            ?: maxChars
        return cut.substring(0, end).trimEnd() + "…"
    }
}
