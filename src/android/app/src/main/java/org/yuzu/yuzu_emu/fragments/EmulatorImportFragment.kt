// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.text.format.Formatter
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.RadioGroup
import android.widget.ScrollView
import com.google.android.material.radiobutton.MaterialRadioButton
import com.google.android.material.textview.MaterialTextView
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.viewModels
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.FragmentEmulatorImportBinding
import org.yuzu.yuzu_emu.features.settings.ui.SettingsSubscreenActivity
import org.yuzu.yuzu_emu.model.EmulatorImportViewModel
import org.yuzu.yuzu_emu.utils.EmulatorImportConfig
import org.yuzu.yuzu_emu.utils.EmulatorImportSources
import org.yuzu.yuzu_emu.utils.collect

class EmulatorImportFragment : Fragment() {
    private var _binding: FragmentEmulatorImportBinding? = null
    private val binding get() = _binding!!
    private val model: EmulatorImportViewModel by viewModels()
    private var displayedFolders: List<EmulatorImportViewModel.FolderChoice>? = null
    private var foldersWereBusy = false

    private val sourcePicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri?.let { persistAccess(it); model.prepare(it, isZip = false) }
    }
    private val zipPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { persistAccess(it); model.prepare(it, isZip = true) }
    }
    private val folderPicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri?.let { persistAccess(it); model.copyFolder(it) }
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentEmulatorImportBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        binding.toolbar.setNavigationOnClickListener { leaveOrCancel() }
        requireActivity().onBackPressedDispatcher.addCallback(
            viewLifecycleOwner,
            object : OnBackPressedCallback(
                true
            ) {
                override fun handleOnBackPressed() = leaveOrCancel()
            }
        )
        binding.source.setOnClickListener {
            val sources = EmulatorImportSources.installed(requireContext())
            val group = RadioGroup(requireContext()).apply {
                orientation = RadioGroup.VERTICAL
                setPadding(dp(24), dp(8), dp(24), dp(8))
            }
            // One generated view id per row; row i is sources[i], the last row is "other folder".
            val rowIds = (sources + listOf(null)).map { source ->
                val row = MaterialRadioButton(requireContext()).apply {
                    id = View.generateViewId()
                    text = source?.let { "${it.label}\n${it.packageName}" }
                        ?: getString(R.string.emulator_import_other_folder)
                    minHeight = dp(72)
                    setPadding(0, dp(8), 0, dp(8))
                    compoundDrawablePadding = dp(12)
                    val icon = source?.let {
                        runCatching { requireContext().packageManager.getApplicationIcon(it.packageName) }.getOrNull()
                    } ?: androidx.core.content.ContextCompat.getDrawable(requireContext(), R.drawable.ic_folder_open)
                    icon?.setBounds(0, 0, dp(36), dp(36))
                    setCompoundDrawablesRelative(icon, null, null, null)
                }
                group.addView(row, RadioGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
                row.id
            }
            group.check(rowIds.first())
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.emulator_import_choose_source)
                .setView(ScrollView(requireContext()).apply { addView(group) })
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    val source = sources.getOrNull(rowIds.indexOf(group.checkedRadioButtonId))
                    model.pendingSource = source
                    model.pendingUseExisting = false
                    sourcePicker.launch(source?.initialUri)
                }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
        binding.backup.setOnClickListener {
            model.pendingUseExisting = false
            zipPicker.launch(
                arrayOf("application/zip", "application/octet-stream")
            )
        }
        binding.addFolder.setOnClickListener {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.emulator_import_add_folder)
                .setItems(arrayOf(getString(R.string.games), getString(R.string.updates_and_dlc))) { _, index ->
                    model.pendingFolder = null
                    model.pendingAdditionalKind = if (index == 0) EmulatorImportConfig.Kind.GAMES else EmulatorImportConfig.Kind.CONTENT
                    chooseFolderMode(requireNotNull(model.pendingAdditionalKind), null)
                }.show()
        }
        binding.apply.setOnClickListener { confirmImport(restore = false) }
        binding.restore.setOnClickListener { confirmImport(restore = true) }
        binding.cancelCopy.setOnClickListener { model.cancel() }
        model.state.collect(viewLifecycleOwner) { state ->
            binding.source.isEnabled = !state.busy
            binding.backup.isEnabled = !state.busy
            binding.progress.isVisible = state.busy
            binding.cancelCopy.isVisible = state.busy && !state.applying
            binding.status.text = state.message
            binding.summary.isVisible = state.files > 0
            binding.summary.text = getString(
                R.string.emulator_import_summary,
                state.files,
                Formatter.formatFileSize(requireContext(), state.bytes)
            )
            binding.apply.isEnabled = state.ready && !state.busy && state.folders.all { it.resolved }
            binding.addFolder.isVisible = state.ready && !state.complete
            binding.addFolder.isEnabled = !state.busy && !state.reviewLocked
            binding.restore.isVisible = !state.busy && state.backupAvailable
            binding.restore.text = if (state.backupTime > 0) {
                getString(
                    R.string.emulator_import_restore_detail,
                    android.text.format.DateUtils.formatDateTime(
                        requireContext(),
                        state.backupTime,
                        android.text.format.DateUtils.FORMAT_SHOW_DATE or android.text.format.DateUtils.FORMAT_SHOW_TIME
                    ),
                    Formatter.formatShortFileSize(requireContext(), state.backupBytes)
                )
            } else {
                getString(R.string.emulator_import_restore)
            }
            if (state.askProfile) {
                model.profileShown()
                chooseProfile()
            }
            if (displayedFolders != state.folders || foldersWereBusy != state.busy) {
                renderFolders(state)
            }
        }
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { root, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            root.updatePadding(
                left = bars.left,
                top = bars.top,
                right = bars.right,
                bottom = bars.bottom
            )
            insets
        }
    }

    private fun renderFolders(state: EmulatorImportViewModel.State) {
        displayedFolders = state.folders
        foldersWereBusy = state.busy
        binding.folders.removeAllViews()
        state.folders.forEach { choice ->
            binding.folders.addView(
                MaterialButton(requireContext()).apply {
                    text = folderLabel(choice)
                    isEnabled = !state.busy && !choice.resolved && !state.complete
                    setOnClickListener {
                        model.pendingFolder = choice.folder
                        model.pendingAdditionalKind = null
                        chooseFolderMode(
                            choice.folder.kind,
                            model.initialFolderUri(choice.folder)
                        )
                    }
                }
            )
            if (!choice.resolved && !state.complete && choice.folder.kind != EmulatorImportConfig.Kind.SAVES) {
                binding.folders.addView(
                    MaterialButton(
                        requireContext(),
                        null,
                        com.google.android.material.R.attr.borderlessButtonStyle
                    ).apply {
                        setText(R.string.emulator_import_skip_folder)
                        isEnabled = !state.busy
                        setOnClickListener {
                            MaterialAlertDialogBuilder(requireContext())
                                .setMessage(R.string.emulator_import_skip_warning)
                                .setPositiveButton(R.string.emulator_import_skip_folder) { _, _ ->
                                    model.skipFolder(
                                        choice.folder
                                    )
                                }
                                .setNegativeButton(android.R.string.cancel, null).show()
                        }
                    }
                )
            }
        }
    }

    private fun folderLabel(choice: EmulatorImportViewModel.FolderChoice): String {
        val name = if (choice.folder.original == "default:saves") {
            getString(R.string.emulator_import_source_saves)
        } else Uri.decode(choice.folder.original).substringAfterLast('/').takeLast(100)
        val kind = when (choice.folder.kind) {
            EmulatorImportConfig.Kind.GAMES -> R.string.games
            EmulatorImportConfig.Kind.CONTENT -> R.string.updates_and_dlc
            EmulatorImportConfig.Kind.NAND -> R.string.emulator_import_nand
            EmulatorImportConfig.Kind.SDMC -> R.string.emulator_import_sdmc
            EmulatorImportConfig.Kind.SAVES -> R.string.save_data
        }
        return getString(
            if (choice.linkedUri != null) {
                R.string.emulator_import_folder_linked
            } else if (choice.copied) {
                R.string.emulator_import_folder_copied
            } else if (choice.skipped) {
                R.string.emulator_import_folder_skipped
            } else {
                R.string.emulator_import_folder_copy
            },
            getString(kind),
            name
        )
    }

    /** After an import with several profiles: saves are per profile, so let the player pick. */
    private fun chooseProfile() {
        val users = runCatching { NativeLibrary.getAllUsers()?.toList() }.getOrNull().orEmpty()
        if (users.size < 2) return
        val names = users.map { NativeLibrary.getUserUsername(it)?.takeIf { n -> n.isNotBlank() } ?: it }
        val current = users.indexOf(NativeLibrary.getCurrentUser()).coerceAtLeast(0)
        var selected = current
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.emulator_import_profile_title)
            .setMessage(getString(R.string.emulator_import_profile_description, names[current]))
            .setSingleChoiceItems(names.toTypedArray(), current) { _, which -> selected = which }
            .setPositiveButton(android.R.string.ok) { _, _ ->
                if (selected != current) NativeLibrary.setCurrentUser(users[selected])
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private fun chooseFolderMode(kind: EmulatorImportConfig.Kind, initial: Uri?) {
        val initialDocument = initial?.let { uri ->
            runCatching {
                if (DocumentsContract.isTreeUri(uri)) {
                    DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri))
                } else uri
            }.getOrDefault(uri)
        }
        if (kind !in setOf(EmulatorImportConfig.Kind.GAMES, EmulatorImportConfig.Kind.CONTENT, EmulatorImportConfig.Kind.SAVES)) {
            MaterialAlertDialogBuilder(requireContext())
                .setMessage(R.string.emulator_import_copy_only)
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    model.pendingUseExisting = false
                    folderPicker.launch(initialDocument)
                }
                .setNegativeButton(android.R.string.cancel, null).show()
            return
        }
        val choices = RadioGroup(requireContext()).apply {
            addView(MaterialRadioButton(context).apply {
                id = View.generateViewId()
                setText(R.string.emulator_import_use_existing)
                minHeight = dp(56)
            })
            addView(MaterialRadioButton(context).apply {
                id = View.generateViewId()
                setText(R.string.emulator_import_copy_files)
                minHeight = dp(56)
            })
            // Saves default to a separate copy: sharing them lets either app overwrite progress.
            check(getChildAt(if (kind == EmulatorImportConfig.Kind.SAVES) 1 else 0).id)
        }
        val content = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
            addView(choices)
            addView(MaterialTextView(context).apply {
                setText(if (kind == EmulatorImportConfig.Kind.SAVES) R.string.emulator_import_saves_explanation
                    else R.string.emulator_import_folder_explanation)
                setPadding(0, dp(12), 0, dp(8))
            })
        }
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.emulator_import_folder_mode)
            .setView(ScrollView(requireContext()).apply { addView(content) })
            .setPositiveButton(android.R.string.ok) { _, _ ->
                model.pendingUseExisting = choices.checkedRadioButtonId == choices.getChildAt(0).id
                if (!model.pendingUseExisting && model.useCopiedSaves()) return@setPositiveButton
                folderPicker.launch(initialDocument)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun confirmImport(restore: Boolean) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(
                if (restore) R.string.emulator_import_restore else R.string.emulator_import_apply
            )
            .setMessage(
                if (restore) {
                    getString(R.string.emulator_import_replace_warning)
                } else {
                    val folders = model.state.value.folders
                    getString(R.string.emulator_import_replace_warning) + "\n\n" +
                        if (folders.isEmpty()) {
                            getString(R.string.emulator_import_review_none)
                        } else {
                            getString(R.string.emulator_import_review_list) + "\n" +
                                folders.joinToString("\n") { "• " + folderLabel(it) }
                        }
                }
            )
            .setPositiveButton(
                if (restore) R.string.emulator_import_restore else R.string.emulator_import_apply
            ) { _, _ ->
                model.applyImport(restore)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun persistAccess(uri: Uri) {
        runCatching {
            requireContext().contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    (if (model.pendingUseExisting && model.pendingFolder?.kind == EmulatorImportConfig.Kind.SAVES)
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
            )
        }
    }

    private fun leaveOrCancel() {
        if (model.state.value.busy) model.cancel() else (requireActivity() as SettingsSubscreenActivity).navigateBack()
    }

    override fun onDestroyView() {
        displayedFolders = null
        _binding = null
        super.onDestroyView()
    }
}
