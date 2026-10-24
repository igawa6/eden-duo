// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException
import java.io.InputStream
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
    private val rootId: String = DocumentsContract.getTreeDocumentId(treeUri)

    private val saveTree = SaveTree(
        children = { id ->
            val uri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, id)
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED,
                DocumentsContract.Document.COLUMN_SIZE
            )
            resolver.query(uri, projection, null, null, null)?.use { c ->
                buildList {
                    while (c.moveToNext()) {
                        add(
                            SaveTree.Doc(
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
        },
        open = { id ->
            resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(treeUri, id))
                ?: throw IOException("Cannot read a save file")
        }
    )

    fun saves(): List<SaveEntry> = saveTree.saves(rootId)

    fun copyTitle(entry: SaveEntry, into: File) = saveTree.copyTitle(rootId, entry, into)
}

/**
 * A source app's `nand/user/save/0000000000000000/<profile>/<title>` folders, walked through
 * [children] and [open] so the walk itself doesn't depend on the documents provider.
 */
class SaveTree(
    private val children: (String) -> List<Doc>,
    private val open: (String) -> InputStream
) {
    data class Doc(val id: String, val name: String, val isDir: Boolean, val modified: Long, val size: Long)

    private fun saveRoot(rootId: String): String? =
        SAVE_PATH.fold(rootId as String?) { id, name ->
            id?.let { parent -> children(parent).firstOrNull { it.isDir && it.name == name }?.id }
        }

    private fun titleFolders(rootId: String): List<Pair<String, Doc>> {
        val root = saveRoot(rootId) ?: return emptyList()
        return children(root).filter { it.isDir }.flatMap { profile ->
            children(profile.id).filter { it.isDir && SaveCatalog.isTitleId(it.name) }.map { profile.name to it }
        }
    }

    /** Every file under [dirId], keyed by its path relative to it. */
    private fun files(dirId: String, prefix: String = "", depth: Int = 0): List<Pair<String, Doc>> {
        if (depth > MAX_DEPTH) throw IOException("A save has too many nested folders")
        return children(dirId).flatMap { doc ->
            if (doc.name.isEmpty() || '/' in doc.name || doc.name == "." || doc.name == "..") {
                throw IOException("Invalid save file name")
            }
            val path = prefix + doc.name
            if (doc.isDir) files(doc.id, "$path/", depth + 1) else listOf(path to doc)
        }
    }

    fun saves(rootId: String): List<SaveEntry> = titleFolders(rootId).map { (profile, title) ->
        // Eden writes its size marker into every save folder, even one the game never wrote to.
        val files = files(title.id).map { it.second }.filter { it.name != SaveCatalog.SIZE_MARKER }
        SaveEntry(
            titleId = title.name.uppercase(),
            profile = profile,
            lastModified = files.maxOfOrNull { it.modified } ?: title.modified,
            bytes = files.sumOf { it.size },
            fileCount = files.size
        )
    }

    /** Copies that game's whole save folder, subfolders included, into [into]. */
    fun copyTitle(rootId: String, entry: SaveEntry, into: File) {
        val (_, title) = titleFolders(rootId).firstOrNull { (profile, doc) ->
            profile == entry.profile && doc.name.equals(entry.titleId, ignoreCase = true)
        } ?: throw IOException("The save for ${entry.titleId} is gone from the source app")
        for ((path, doc) in files(title.id)) {
            val target = File(into, path)
            target.parentFile?.mkdirs()
            open(doc.id).use { src -> target.outputStream().use { src.copyTo(it) } }
            if (doc.modified > 0) target.setLastModified(doc.modified)
        }
    }

    private companion object {
        val SAVE_PATH = listOf("nand", "user", "save", "0000000000000000")
        const val MAX_DEPTH = 32
    }
}
