// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.documentfile.provider.DocumentFile
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.navigation.fragment.navArgs
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.AddonAdapter
import org.yuzu.yuzu_emu.databinding.FragmentAddonsBinding
import org.yuzu.yuzu_emu.model.AddonViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.utils.AddonUtil
import org.yuzu.yuzu_emu.utils.DualScreenPackageInstaller
import org.yuzu.yuzu_emu.utils.FileUtil.copyFilesTo
import org.yuzu.yuzu_emu.utils.InstallableActions
import org.yuzu.yuzu_emu.utils.NativeConfig
import org.yuzu.yuzu_emu.utils.ViewUtils.updateMargins
import org.yuzu.yuzu_emu.utils.collect
import java.io.File

class AddonsFragment : Fragment() {
    private var _binding: FragmentAddonsBinding? = null
    private val binding get() = _binding!!

    private val homeViewModel: HomeViewModel by activityViewModels()
    private val addonViewModel: AddonViewModel by activityViewModels()

    private val args by navArgs<AddonsFragmentArgs>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        addonViewModel.onAddonsViewCreated(args.game)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentAddonsBinding.inflate(inflater)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setStatusBarShadeVisibility(false)

        binding.toolbarAddons.setNavigationOnClickListener {
            requireActivity().onBackPressedDispatcher.onBackPressed()
        }

        binding.toolbarAddons.title = getString(R.string.addons_game, args.game.title)

        binding.listAddons.apply {
            layoutManager = LinearLayoutManager(requireContext())
            adapter = AddonAdapter(addonViewModel)
        }

        addonViewModel.addonList.collect(viewLifecycleOwner) {
            (binding.listAddons.adapter as AddonAdapter).submitList(it)
        }
        addonViewModel.showModInstallPicker.collect(
            viewLifecycleOwner,
            resetState = { addonViewModel.showModInstallPicker(false) }
        ) { if (it) installAddon.launch(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).data) }
        addonViewModel.showModNoticeDialog.collect(
            viewLifecycleOwner,
            resetState = { addonViewModel.showModNoticeDialog(false) }
        ) {
            if (it) {
                MessageDialogFragment.newInstance(
                    requireActivity(),
                    titleId = R.string.addon_notice,
                    descriptionId = R.string.addon_notice_description,
                    dismissible = false,
                    positiveAction = { addonViewModel.showModInstallPicker(true) },
                    negativeAction = {},
                    negativeButtonTitleId = R.string.close
                ).show(parentFragmentManager, MessageDialogFragment.TAG)
            }
        }
        addonViewModel.addonToDelete.collect(
            viewLifecycleOwner,
            resetState = { addonViewModel.setAddonToDelete(null) }
        ) {
            if (it != null) {
                MessageDialogFragment.newInstance(
                    requireActivity(),
                    titleId = R.string.confirm_uninstall,
                    descriptionId = R.string.confirm_uninstall_description,
                    positiveAction = { addonViewModel.onDeleteAddon(it) },
                    negativeAction = {}
                ).show(parentFragmentManager, MessageDialogFragment.TAG)
            }
        }
        parentFragmentManager.setFragmentResultListener(
            ContentTypeSelectionDialogFragment.REQUEST_INSTALL_GAME_UPDATE,
            viewLifecycleOwner
        ) { _, _ ->
            installGameUpdate.launch(arrayOf("*/*"))
        }
        parentFragmentManager.setFragmentResultListener(
            ContentTypeSelectionDialogFragment.REQUEST_INSTALL_DUAL_SCREEN_MOD,
            viewLifecycleOwner
        ) { _, _ ->
            installDualScreenPackageFile.launch(
                arrayOf("application/zip", "application/octet-stream", "*/*")
            )
        }

        binding.buttonInstall.setOnClickListener {
            ContentTypeSelectionDialogFragment().show(
                parentFragmentManager,
                ContentTypeSelectionDialogFragment.TAG
            )
        }

        setInsets()
    }

    override fun onResume() {
        super.onResume()
        addonViewModel.onAddonsViewStarted(args.game)
    }

    override fun onDestroy() {
        if (activity?.isChangingConfigurations != true) {
            addonViewModel.onCloseAddons()
        }
        super.onDestroy()
    }

    private val installAddon =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { result ->
            if (result == null) {
                return@registerForActivityResult
            }

            val externalAddonDirectory = DocumentFile.fromTreeUri(requireContext(), result)
            if (externalAddonDirectory == null) {
                MessageDialogFragment.newInstance(
                    requireActivity(),
                    titleId = R.string.invalid_directory,
                    descriptionId = R.string.invalid_directory_description
                ).show(parentFragmentManager, MessageDialogFragment.TAG)
                return@registerForActivityResult
            }

            val isValid = externalAddonDirectory.listFiles()
                .any { AddonUtil.validAddonDirectories.contains(it.name?.lowercase()) }
            val errorMessage = MessageDialogFragment.newInstance(
                requireActivity(),
                titleId = R.string.invalid_directory,
                descriptionId = R.string.invalid_directory_description
            )
            if (isValid) {
                ProgressDialogFragment.newInstance(
                    requireActivity(),
                    R.string.installing_game_content,
                    false
                ) { progressCallback, _ ->
                    val parentDirectoryName = externalAddonDirectory.name
                    val internalAddonDirectory =
                        File(args.game.addonDir + parentDirectoryName)
                    try {
                        externalAddonDirectory.copyFilesTo(internalAddonDirectory, progressCallback)
                    } catch (_: Exception) {
                        return@newInstance errorMessage
                    }
                    addonViewModel.persistAddonStates()
                    addonViewModel.refreshAddons(force = true)
                    return@newInstance getString(R.string.addon_installed_successfully)
                }.show(parentFragmentManager, ProgressDialogFragment.TAG)
            } else {
                errorMessage.show(parentFragmentManager, MessageDialogFragment.TAG)
            }
        }

    private val installGameUpdate =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { documents ->
            InstallableActions.verifyAndInstallContent(
                activity = requireActivity(),
                fragmentManager = parentFragmentManager,
                addonViewModel = addonViewModel,
                documents = documents,
                programId = args.game.programId
            )
        }

    private val installDualScreenPackageFile =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { result ->
            if (result != null) {
                installDualScreenPackage(result)
            }
        }

    private fun installDualScreenPackage(uri: Uri) {
        val activity = requireActivity()
        val game = args.game
        ProgressDialogFragment.newInstance(
            activity,
            R.string.installing_dual_screen_mod,
            true
        ) { progressCallback, _ ->
            when (
                val result = DualScreenPackageInstaller.install(
                    context = activity,
                    uri = uri,
                    gameTitleId = game.programIdHex,
                    addonDirectory = File(game.addonDir),
                    progressCallback = progressCallback
                )
            ) {
                is DualScreenPackageInstaller.Result.Installed -> {
                    addonViewModel.persistAddonStates()
                    val notes = applyDualScreenAddonStates(activity, game.programId, result)
                    addonViewModel.refreshAddons(force = true)
                    if (notes.isEmpty()) {
                        activity.getString(R.string.dual_screen_mod_installed)
                    } else {
                        MessageDialogFragment.newInstance(
                            activity,
                            titleId = R.string.dual_screen_mod_installed,
                            descriptionString = notes
                        )
                    }
                }

                is DualScreenPackageInstaller.Result.Failed -> {
                    if (result.error == DualScreenPackageInstaller.Error.Cancelled) {
                        activity.getString(R.string.dual_screen_mod_install_cancelled)
                    } else {
                        MessageDialogFragment.newInstance(
                            activity,
                            titleId = R.string.dual_screen_mod_install_failed,
                            descriptionString = dualScreenInstallError(activity, result)
                        )
                    }
                }
            }
        }.show(parentFragmentManager, ProgressDialogFragment.TAG)
    }

    /**
     * Keeps the second screen on the package just installed, through the add-on on/off list
     * (reversible from the Add-ons screen):
     * - an update keeps the user's choice: if the version it replaced was off, the new one is too;
     * - an older version that could not be deleted is turned off;
     * - other folders holding only a dual-screen package for this game (hand-copied ones
     *   included) are turned off, because the runtime uses the first enabled one by name.
     * Folders that also hold other mods are left alone and named in the returned note.
     */
    private fun applyDualScreenAddonStates(
        context: Context,
        programId: String,
        result: DualScreenPackageInstaller.Result.Installed
    ): String {
        val disabled = NativeConfig.getDisabledAddons(programId).toMutableList()
        val replaced = result.removed + result.notRemoved
        if (replaced.any { it in disabled } && result.folderName !in disabled) {
            disabled += result.folderName
        }
        disabled.removeAll(result.removed.toSet())

        val turnedOff = mutableListOf<String>()
        for (name in result.notRemoved +
            result.otherPackages.filter { it.onlyDualScreen }.map { it.folderName }) {
            if (name !in disabled) {
                disabled += name
                turnedOff += name
            }
        }
        val mayShadow = result.otherPackages
            .filter { !it.onlyDualScreen && it.folderName !in disabled }
            .map { it.folderName }
            .filter { it < result.folderName }
        NativeConfig.setDisabledAddons(programId, disabled.toTypedArray())
        NativeConfig.saveGlobalConfig()

        val notes = mutableListOf<String>()
        if (result.notRemoved.isNotEmpty()) {
            notes += context.getString(
                R.string.dual_screen_mod_old_not_removed,
                result.notRemoved.joinToString()
            )
        }
        val othersOff = turnedOff - result.notRemoved.toSet()
        if (othersOff.isNotEmpty()) {
            notes += context.getString(
                R.string.dual_screen_mod_others_disabled,
                othersOff.joinToString()
            )
        }
        if (mayShadow.isNotEmpty()) {
            notes += context.getString(
                R.string.dual_screen_mod_others_enabled,
                mayShadow.joinToString()
            )
        }
        return notes.joinToString("\n\n")
    }

    /** The message for a failed install: what went wrong, then the installer's detail. */
    private fun dualScreenInstallError(
        context: Context,
        result: DualScreenPackageInstaller.Result.Failed
    ): String {
        val detail = result.detail
        val message = when (result.error) {
            DualScreenPackageInstaller.Error.RuntimeTooOld -> return context.getString(
                R.string.dual_screen_mod_runtime_too_old,
                if (result.requiredRuntime == DualScreenPackageInstaller.UNKNOWN_RUNTIME) {
                    "?"
                } else {
                    result.requiredRuntime.toString()
                },
                result.runtime
            )
            DualScreenPackageInstaller.Error.MissingPlatformLibrary ->
                return context.getString(R.string.dual_screen_mod_missing_platform_library, detail)
            DualScreenPackageInstaller.Error.CannotWrite ->
                return context.getString(R.string.dual_screen_mod_cannot_write, detail)
            DualScreenPackageInstaller.Error.VersionMismatch ->
                return context.getString(R.string.dual_screen_mod_version_mismatch, detail)
            DualScreenPackageInstaller.Error.ChecksumMismatch ->
                context.getString(R.string.dual_screen_mod_checksum_mismatch)
            DualScreenPackageInstaller.Error.ArchiveTooLarge ->
                context.getString(R.string.dual_screen_mod_archive_too_large)
            DualScreenPackageInstaller.Error.MalformedArchive ->
                context.getString(R.string.dual_screen_mod_malformed_archive)
            DualScreenPackageInstaller.Error.UnsafeEntry,
            DualScreenPackageInstaller.Error.DuplicateEntry,
            DualScreenPackageInstaller.Error.InvalidLayout,
            DualScreenPackageInstaller.Error.InvalidMetadata ->
                context.getString(R.string.dual_screen_mod_invalid_package)
            DualScreenPackageInstaller.Error.TitleMismatch ->
                context.getString(R.string.dual_screen_mod_title_mismatch)
            DualScreenPackageInstaller.Error.Cancelled,
            DualScreenPackageInstaller.Error.InstallFailed ->
                context.getString(R.string.dual_screen_mod_install_failed_description)
        }
        return if (detail.isEmpty()) {
            message
        } else {
            context.getString(R.string.dual_screen_mod_error_detail, message, detail)
        }
    }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { _: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            val leftInsets = barInsets.left + cutoutInsets.left
            val rightInsets = barInsets.right + cutoutInsets.right

            binding.toolbarAddons.updateMargins(left = leftInsets, right = rightInsets)
            binding.listAddons.updateMargins(left = leftInsets, right = rightInsets)
            binding.listAddons.updatePadding(
                bottom = barInsets.bottom +
                    resources.getDimensionPixelSize(R.dimen.spacing_bottom_list_fab)
            )

            val fabSpacing = resources.getDimensionPixelSize(R.dimen.spacing_fab)
            binding.buttonInstall.updateMargins(
                left = leftInsets + fabSpacing,
                right = rightInsets + fabSpacing,
                bottom = barInsets.bottom + fabSpacing
            )

            windowInsets
        }
}
