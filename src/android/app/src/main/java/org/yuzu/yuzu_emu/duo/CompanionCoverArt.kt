// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.LruCache
import android.widget.ImageView
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication

/**
 * Eden Duo: GameTDB cover art for the library's "not in your library" tiles.
 *
 * One download per title (coverM, plus coverHQ when the cover is the eShop template so its small
 * key art stays sharp), cut to the tile square by [CoverArtCrop] and kept in colour on disk next
 * to the game icon cache; the greyscale is applied when the tile is drawn. A failed download is
 * retried at most once per app start, and a failure only ever leaves the glyph tile.
 */
object CompanionCoverArt {
    private const val ART_URL = "https://art.gametdb.com/switch"
    private const val TILE_PX = 384
    private const val MAX_ATTEMPTS = 2

    private val directory: File by lazy {
        File(YuzuApplication.appContext.cacheDir, "companion-covers").apply { mkdirs() }
    }
    private val client by lazy {
        OkHttpClient.Builder()
            .connectTimeout(10, TimeUnit.SECONDS)
            .readTimeout(20, TimeUnit.SECONDS)
            .callTimeout(60, TimeUnit.SECONDS)
            .build()
    }
    private val userAgent = "EdenDuo/${BuildConfig.VERSION_NAME}"
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val memory = object : LruCache<String, Bitmap>(8 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val attempts = ConcurrentHashMap<String, Int>()
    private val inFlight = ConcurrentHashMap.newKeySet<String>()

    private fun fileFor(entry: CompanionCatalog.Entry) =
        File(directory, "${entry.titleId}-${entry.gametdb}.jpg")

    /**
     * Shows [entry]'s art in [view] once it is available; [shown] tells the caller whether the
     * view now holds art (true) or should keep its glyph fallback (false). Runs on the main
     * thread; [shown] is not called when the view was rebound to another title meanwhile.
     */
    fun load(entry: CompanionCatalog.Entry, view: ImageView, shown: (Boolean) -> Unit) {
        view.setTag(R.id.companion_cover_title, entry.titleId)
        if (entry.gametdb.isEmpty()) {
            shown(false)
            return
        }
        memory.get(entry.titleId)?.let {
            view.setImageBitmap(it)
            shown(true)
            return
        }
        shown(false)
        scope.launch {
            val bitmap = runCatching { cached(entry) ?: download(entry) }.getOrNull()
            withContext(Dispatchers.Main) {
                if (view.getTag(R.id.companion_cover_title) != entry.titleId || bitmap == null) {
                    return@withContext
                }
                view.setImageBitmap(bitmap)
                shown(true)
            }
        }
    }

    private fun cached(entry: CompanionCatalog.Entry): Bitmap? {
        val file = fileFor(entry)
        if (!file.isFile) return null
        return BitmapFactory.decodeFile(file.path)?.also { memory.put(entry.titleId, it) }
    }

    private fun download(entry: CompanionCatalog.Entry): Bitmap? {
        val count = attempts[entry.titleId] ?: 0
        if (count >= MAX_ATTEMPTS || !inFlight.add(entry.titleId)) return null
        attempts[entry.titleId] = count + 1
        try {
            val regions = listOf(entry.region, "US", "EN").filter { it.isNotEmpty() }.distinct()
            for (region in regions) {
                val cover = fetch("$ART_URL/coverM/$region/${entry.gametdb}.jpg") ?: continue
                val square = tileSquare(cover, entry, region)
                val file = fileFor(entry)
                val temp = File(directory, file.name + ".tmp")
                temp.outputStream().use { square.compress(Bitmap.CompressFormat.JPEG, 90, it) }
                if (!temp.renameTo(file)) temp.delete()
                memory.put(entry.titleId, square)
                return square
            }
            return null
        } finally {
            inFlight.remove(entry.titleId)
        }
    }

    /** The colour tile square of [cover]; the eShop template's key art comes from coverHQ. */
    private fun tileSquare(cover: Bitmap, entry: CompanionCatalog.Entry, region: String): Bitmap {
        var source = cover
        if (CoverArtCrop.eshopInner(pixelsOf(cover), cover.width, cover.height) != null) {
            fetch("$ART_URL/coverHQ/$region/${entry.gametdb}.jpg")?.let { source = it }
        }
        val area = CoverArtCrop.tileSquare(pixelsOf(source), source.width, source.height)
        val cut = Bitmap.createBitmap(source, area.left, area.top, area.width, area.height)
        return if (cut.width > TILE_PX) Bitmap.createScaledBitmap(cut, TILE_PX, TILE_PX, true) else cut
    }

    private fun pixelsOf(bitmap: Bitmap): IntArray =
        IntArray(bitmap.width * bitmap.height).also {
            bitmap.getPixels(it, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
        }

    private fun fetch(url: String): Bitmap? {
        val request = Request.Builder().url(url).header("User-Agent", userAgent).build()
        return client.newCall(request).execute().use { response ->
            if (!response.isSuccessful) return null
            val bytes = response.body?.bytes() ?: return null
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        }
    }
}
