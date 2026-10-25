// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.os.bundleOf
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.io.File
import java.text.DateFormat
import java.util.Date
import kotlin.coroutines.resume
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.FragmentSaveTransferBinding
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

/**
 * PS2 memory-card style save transfer: the installed Eden apps' saves on the left, this app's on
 * the right. Copies one way, into this app's active profile, backing up whatever it replaces.
 */
class SaveTransferFragment : Fragment() {
    private var _binding: FragmentSaveTransferBinding? = null
    private val binding get() = _binding!!

    private val sourceAdapter = SaveTileAdapter { toggle(it) }
    private val destinationAdapter = SaveTileAdapter()

    private var sources: List<SourceApp> = emptyList()
    private var sourceIndex = 0
    private var sourceSaves: List<SaveEntry> = emptyList()
    private var destinationEntries: Map<String, SaveEntry> = emptyMap()
    private var destination: DestinationSaves? = null
    private var games: Map<String, Game> = emptyMap()
    private val selected = mutableSetOf<String>()
    private val copied = mutableSetOf<String>()
    private var busy = false
    private val heldKeys = mutableSetOf<Int>()

    private val currentSource: SourceApp? get() = sources.getOrNull(sourceIndex)

    private val pickTree = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) {
        onTreePicked(it)
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        _binding = FragmentSaveTransferBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        val span = 3 // games per row on each side
        binding.sourceGrid.layoutManager = GridLayoutManager(requireContext(), span)
        binding.sourceGrid.adapter = sourceAdapter
        binding.destinationGrid.layoutManager = GridLayoutManager(requireContext(), span)
        binding.destinationGrid.adapter = destinationAdapter

        binding.toolbar.setNavigationOnClickListener { requireActivity().finish() }
        binding.sourcePrevious.setOnClickListener { cycleSource(-1) }
        binding.sourceNext.setOnClickListener { cycleSource(1) }
        binding.giveAccess.setOnClickListener { askForAccess() }
        binding.transferButton.setOnClickListener { transfer(selectedEntries()) }
        binding.destinationName.text =
            requireContext().applicationInfo.loadLabel(requireContext().packageManager)
    }

    /** Also picks up grants given or revoked, and apps installed or removed, while away. */
    override fun onResume() {
        super.onResume()
        if (!busy) viewLifecycleOwner.lifecycleScope.launch { reload() }
    }

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    /**
     * Controller buttons, forwarded by the host activity from dispatchKeyEvent so they act the
     * same whichever tile has focus. A press we take, we take with its release; one we leave
     * (A on Give access or Back) goes on to Android, which turns A into a click.
     */
    fun onControllerKey(event: KeyEvent): Boolean {
        if (_binding == null) return false
        return when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                if (event.repeatCount == 0 && ownsControllerKey(event.keyCode, focusOnSave())) {
                    heldKeys.add(event.keyCode)
                    handleKey(event.keyCode)
                }
                event.keyCode in heldKeys
            }
            KeyEvent.ACTION_UP -> heldKeys.remove(event.keyCode)
            else -> false
        }
    }

    private fun focusOnSave(): Boolean {
        val focused = requireActivity().currentFocus ?: return true
        return binding.sourceGrid.findContainingViewHolder(focused) != null ||
            binding.destinationGrid.findContainingViewHolder(focused) != null
    }

    private fun handleKey(keyCode: Int) {
        when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_L1 -> cycleSource(-1)
            KeyEvent.KEYCODE_BUTTON_R1 -> cycleSource(1)
            KeyEvent.KEYCODE_BUTTON_X -> focusedSourceTile()?.let { toggle(it) }
            KeyEvent.KEYCODE_BUTTON_A -> {
                // Marked games, or else the one under the cursor.
                val entries = selectedEntries().ifEmpty { listOfNotNull(focusedSourceTile()?.entry) }
                transfer(entries)
            }
            KeyEvent.KEYCODE_BUTTON_B -> requireActivity().finish()
        }
    }

    private fun focusedSourceTile(): SaveTile? {
        val focused = requireActivity().currentFocus ?: return null
        val holder = binding.sourceGrid.findContainingViewHolder(focused) ?: return null
        return sourceAdapter.tiles.getOrNull(holder.bindingAdapterPosition)
    }

    private fun selectedEntries(): List<SaveEntry> = sourceSaves.filter { it.titleId in selected }

    private suspend fun reload() {
        val context = requireContext().applicationContext
        val selectedPackage = currentSource?.source?.packageName
        val includeDuetPlus = requireArguments().getBoolean(ARG_INCLUDE_DUET_PLUS)
        withContext(Dispatchers.IO) {
            games = cachedGames()
            destination = DestinationSaves(
                File(NativeConfig.getSaveDir() + NativeLibrary.getDefaultProfileSaveDataRoot(false))
            )
            destinationEntries = destination!!.list()
            sources = SaveSources.available(context, includeDuetPlus)
        }
        sourceIndex = sources.indexOfFirst { it.source.packageName == selectedPackage }.coerceAtLeast(0)
        if (currentSource?.source?.packageName != selectedPackage) {
            selected.clear()
            copied.clear()
        }
        showDestination()
        loadSource()
    }

    /** This app's game list cache, by title ID, for tile names and icons. */
    private fun cachedGames(): Map<String, Game> =
        PreferenceManager.getDefaultSharedPreferences(requireContext().applicationContext)
            .getStringSet(GameHelper.KEY_GAMES, emptySet()).orEmpty()
            .mapNotNull { runCatching { Json.decodeFromString<Game>(it) }.getOrNull() }
            .associateBy { it.programIdHex.uppercase() }

    private fun cycleSource(step: Int) {
        if (busy || sources.size < 2) return
        sourceIndex = (sourceIndex + step).mod(sources.size)
        selected.clear()
        copied.clear()
        viewLifecycleOwner.lifecycleScope.launch { loadSource() }
    }

    private suspend fun loadSource() {
        val app = currentSource
        binding.sourcePrevious.isEnabled = sources.size > 1
        binding.sourceNext.isEnabled = sources.size > 1
        sourceSaves = emptyList()
        showSource()
        if (app == null) {
            binding.sourceName.text = getString(R.string.save_transfer)
            showSourceMessage(getString(R.string.save_transfer_no_sources), access = false)
            return
        }
        binding.sourceName.text = app.source.label
        val tree = app.grantedTree
        if (tree == null) {
            showSourceMessage(getString(R.string.save_transfer_needs_access, app.source.label), access = true)
            return
        }
        binding.sourceLoading.visibility = View.VISIBLE
        val context = requireContext().applicationContext
        val saves = withContext(Dispatchers.IO) {
            try {
                SaveCatalog.newestPerTitle(SourceReader(context, tree).saves())
            } catch (e: Exception) {
                // Revoked access, an uninstalled app, or a provider that went away.
                Log.warning("[SaveTransfer] Cannot read ${app.source.packageName}: ${e.message}")
                null
            }
        }
        if (_binding == null || currentSource != app) return
        binding.sourceLoading.visibility = View.GONE
        if (saves == null) {
            showSourceMessage(getString(R.string.save_transfer_lost_access, app.source.label), access = true)
            return
        }
        sourceSaves = saves
        showSource()
        if (saves.isEmpty()) {
            showSourceMessage(getString(R.string.save_transfer_empty, app.source.label), access = false)
        } else {
            binding.sourceGrid.post { binding.sourceGrid.getChildAt(0)?.requestFocus() }
        }
    }

    private fun showSourceMessage(message: String, access: Boolean) {
        binding.sourceLoading.visibility = View.GONE
        binding.sourceMessage.visibility = View.VISIBLE
        binding.sourceMessageText.text = message
        binding.giveAccess.visibility = if (access) View.VISIBLE else View.GONE
        if (access) binding.giveAccess.requestFocus()
    }

    private fun showSource() {
        binding.sourceMessage.visibility = View.GONE
        sourceAdapter.submit(
            sourceSaves.map {
                SaveTile(
                    entry = it,
                    game = games[it.titleId],
                    state = SaveCatalog.state(it, destinationEntries[it.titleId]),
                    selected = it.titleId in selected,
                    copied = it.titleId in copied
                )
            }
        )
        binding.transferButton.isEnabled = !busy && selected.isNotEmpty()
        binding.transferButton.text = if (selected.isEmpty()) {
            getString(R.string.save_transfer_button)
        } else {
            "${getString(R.string.save_transfer_button)} (${selected.size})"
        }
    }

    private fun showDestination() {
        val tiles = destinationEntries.values.sortedByDescending { it.lastModified }
            .map { SaveTile(it, games[it.titleId]) }
        destinationAdapter.submit(tiles)
        binding.destinationEmpty.visibility = if (tiles.isEmpty()) View.VISIBLE else View.GONE
    }

    private fun toggle(tile: SaveTile) {
        if (busy) return
        val id = tile.entry.titleId
        if (!selected.remove(id)) selected.add(id)
        val focusedPosition = sourceAdapter.tiles.indexOfFirst { it.entry.titleId == id }
        showSource()
        binding.sourceGrid.post {
            binding.sourceGrid.findViewHolderForAdapterPosition(focusedPosition)?.itemView?.requestFocus()
        }
    }

    private fun askForAccess() {
        val app = currentSource ?: return
        pickTree.launch(app.source.initialUri)
    }

    private fun onTreePicked(uri: Uri?) {
        val app = currentSource ?: return
        if (uri == null) return
        if (uri.authority != app.source.authority) {
            // A toast: onResume, which comes next, redraws the source pane.
            Toast.makeText(
                requireContext(),
                getString(R.string.save_transfer_wrong_folder, app.source.label),
                Toast.LENGTH_LONG
            ).show()
            return
        }
        try {
            requireContext().contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (e: SecurityException) {
            Log.warning("[SaveTransfer] Cannot keep access to ${app.source.packageName}: ${e.message}")
        }
        // onResume, which comes next, reloads the sources with the new grant.
    }

    private fun transfer(entries: List<SaveEntry>) {
        if (busy || entries.isEmpty()) return
        val app = currentSource ?: return
        val tree = app.grantedTree ?: return
        val target = destination ?: return
        viewLifecycleOwner.lifecycleScope.launch {
            if (NativeLibrary.isRunning()) {
                Toast.makeText(requireContext(), R.string.save_transfer_running, Toast.LENGTH_LONG).show()
                return@launch
            }
            val confirmed = entries.filter { entry ->
                val here = destinationEntries[entry.titleId]
                here == null || confirmOverwrite(entry, here)
            }
            if (confirmed.isEmpty()) return@launch

            setBusy(true)
            val context = requireContext().applicationContext
            val failures = withContext(Dispatchers.IO) {
                val reader = SourceReader(context, tree)
                confirmed.mapNotNull { entry ->
                    try {
                        if (NativeLibrary.isRunning()) error(context.getString(R.string.save_transfer_running))
                        target.transfer(entry.titleId) { reader.copyTitle(entry, it) }
                        copied.add(entry.titleId)
                        null
                    } catch (e: Exception) {
                        Log.error("[SaveTransfer] ${entry.titleId}: ${e.message}")
                        entry to (e.message ?: e.javaClass.simpleName)
                    }
                }.also { destinationEntries = target.list() }
            }
            if (_binding == null) return@launch
            selected.clear()
            setBusy(false)
            showDestination()
            showSource()
            val done = confirmed.size - failures.size
            if (done > 0) {
                Toast.makeText(
                    requireContext(),
                    resources.getQuantityString(R.plurals.save_transfer_done, done, done),
                    Toast.LENGTH_SHORT
                ).show()
            }
            if (failures.isNotEmpty()) {
                MaterialAlertDialogBuilder(requireContext())
                    .setTitle(R.string.save_transfer_failed_title)
                    .setMessage(
                        failures.joinToString("\n") { (entry, reason) ->
                            getString(R.string.save_transfer_failed, nameOf(entry), reason)
                        }
                    )
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
        }
    }

    private fun setBusy(value: Boolean) {
        busy = value
        binding.transferProgress.visibility = if (value) View.VISIBLE else View.INVISIBLE
        binding.transferButton.isEnabled = !value && selected.isNotEmpty()
    }

    private fun nameOf(entry: SaveEntry): String =
        games[entry.titleId]?.title?.takeIf { it.isNotBlank() } ?: entry.titleId

    /** Skip is the default (and focused) answer; Overwrite has to be picked on purpose. */
    private suspend fun confirmOverwrite(entry: SaveEntry, here: SaveEntry): Boolean =
        suspendCancellableCoroutine { continuation ->
            val format = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)
            val newerHere = SaveCatalog.state(entry, here) == DestinationState.NEWER
            val message = (if (newerHere) getString(R.string.save_transfer_overwrite_newer) else "") +
                getString(
                    R.string.save_transfer_overwrite_message,
                    format.format(Date(entry.lastModified)),
                    format.format(Date(here.lastModified))
                )
            var answered = false
            fun answer(value: Boolean) {
                if (!answered && continuation.isActive) {
                    answered = true
                    continuation.resume(value)
                }
            }
            val dialog = MaterialAlertDialogBuilder(requireContext())
                .setTitle(getString(R.string.save_transfer_overwrite_title, nameOf(entry)))
                .setMessage(message)
                .setPositiveButton(R.string.save_transfer_skip) { _, _ -> answer(false) }
                .setNegativeButton(R.string.save_transfer_overwrite) { _, _ -> answer(true) }
                .setOnDismissListener { answer(false) }
                .show()
            dialog.getButton(android.app.AlertDialog.BUTTON_NEGATIVE)?.setTextColor(
                MaterialColors.getColor(dialog.context, com.google.android.material.R.attr.colorError, 0)
            )
            dialog.getButton(android.app.AlertDialog.BUTTON_POSITIVE)?.requestFocus()
            continuation.invokeOnCancellation { dialog.dismiss() }
        }

    companion object {
        const val ARG_INCLUDE_DUET_PLUS = "include_duet_plus"
        const val TAG = "SaveTransferFragment"

        fun newInstance(includeDuetPlus: Boolean) = SaveTransferFragment().apply {
            arguments = bundleOf(ARG_INCLUDE_DUET_PLUS to includeDuetPlus)
        }
    }
}

/**
 * Whether the screen takes a controller button. L1/R1/B work anywhere; A and X act on saves, so
 * they're the screen's only while the cursor is on a save (or nowhere): on a button, A clicks it.
 */
internal fun ownsControllerKey(keyCode: Int, focusOnSave: Boolean): Boolean = when (keyCode) {
    KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_B -> true
    KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_X -> focusOnSave
    else -> false
}
