// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException
import org.yuzu.yuzu_emu.utils.EmulatorImportSources

/** An installed app saves can be copied from, and its persisted read grant (a tree URI) if any. */
data class SourceApp(val source: EmulatorImportSources.Source, val grantedTree: String?)

object SaveSources {
    val EDEN_FAMILY = listOf(
        "dev.eden.eden_emulator",
        "dev.eden.eden_emulator.nightly",
        "dev.igawa6.edenduo",
        "dev.igawa6.edenduo.dev"
    )
    const val DUET_PLUS = "com.tapionallanaire.duetplus"

    fun allowed(packageName: String, selfPackage: String, includeDuetPlus: Boolean): Boolean =
        packageName != selfPackage &&
            (packageName in EDEN_FAMILY || includeDuetPlus && packageName == DUET_PLUS)

    fun needsAccess(app: SourceApp): Boolean = app.grantedTree == null

    fun available(context: Context, includeDuetPlus: Boolean): List<SourceApp> {
        val grants = context.contentResolver.persistedUriPermissions.filter { it.isReadPermission }
        return EmulatorImportSources.installed(context)
            .filter { allowed(it.packageName, context.packageName, includeDuetPlus) }
            .map { source ->
                SourceApp(source, grants.firstOrNull { it.uri.authority == source.authority }?.uri?.toString())
            }
    }
}

/** Reads one source app's Switch saves through its documents provider; never writes to it. */
class SourceReader(context: Context, tree: String) {
    private val resolver = context.contentResolver
    private val treeUri: Uri = Uri.parse(tree)

    private data class Doc(val id: String, val name: String, val isDir: Boolean, val modified: Long, val size: Long)

    private fun children(documentId: String): List<Doc> {
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId)
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
            DocumentsContract.Document.COLUMN_SIZE
        )
        return resolver.query(uri, projection, null, null, null)?.use { c ->
            buildList {
                while (c.moveToNext()) {
                    add(
                        Doc(
                            id = c.getString(0),
                            name = c.getString(1).orEmpty(),
                            isDir = c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR,
                            modified = if (c.isNull(3)) 0 else c.getLong(3),
                            size = if (c.isNull(4)) 0 else c.getLong(4)
                        )
                    )
                }
            }
        } ?: throw IOException("Cannot read the source app's files")
    }

    /** `nand/user/save/0000000000000000`, or null when this app has no saves yet. */
    private fun saveRoot(): String? =
        SAVE_PATH.fold(DocumentsContract.getTreeDocumentId(treeUri) as String?) { id, name ->
            id?.let { parent -> children(parent).firstOrNull { it.isDir && it.name == name }?.id }
        }

    private fun titleFolders(): List<Pair<String, Doc>> {
        val root = saveRoot() ?: return emptyList()
        return children(root).filter { it.isDir }.flatMap { profile ->
            children(profile.id).filter { it.isDir && SaveCatalog.isTitleId(it.name) }.map { profile.name to it }
        }
    }

    fun saves(): List<SaveEntry> = titleFolders().map { (profile, title) ->
        val files = children(title.id).filter { !it.isDir }
        SaveEntry(
            titleId = title.name.uppercase(),
            profile = profile,
            lastModified = files.maxOfOrNull { it.modified } ?: title.modified,
            bytes = files.sumOf { it.size },
            fileCount = files.size
        )
    }

    /** Copies that game's save files (flat, as Switch saves are) into [into]. */
    fun copyTitle(entry: SaveEntry, into: File) {
        val (_, title) = titleFolders().firstOrNull { (profile, doc) ->
            profile == entry.profile && doc.name.equals(entry.titleId, ignoreCase = true)
        } ?: throw IOException("The save for ${entry.titleId} is gone from the source app")
        for (file in children(title.id).filter { !it.isDir }) {
            if ('/' in file.name || file.name.isEmpty()) throw IOException("Invalid save file name")
            val input = resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(treeUri, file.id))
                ?: throw IOException("Cannot read ${file.name}")
            input.use { src -> File(into, file.name).outputStream().use { src.copyTo(it) } }
        }
    }

    private companion object {
        val SAVE_PATH = listOf("nand", "user", "save", "0000000000000000")
    }
}
