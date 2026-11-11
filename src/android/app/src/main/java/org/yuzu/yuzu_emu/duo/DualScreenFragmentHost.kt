// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.annotation.SuppressLint
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.util.Rational
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.OnBackPressedCallback
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.fragment.app.Fragment
import androidx.window.layout.FoldingFeature
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.FragmentEmulationBinding
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.features.settings.model.Settings.EmulationOrientation
import org.yuzu.yuzu_emu.features.settings.model.Settings.EmulationVerticalAlignment
import org.yuzu.yuzu_emu.overlay.model.OverlayLayout
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig
import kotlin.reflect.KMutableProperty0

/**
 * Eden Duo Stage 2/4: everything EmulationFragment needs to drive Simulated (inline) dual screen
 * -- the DualScreenGeometry push, companion-pane touch routing, the no-controller controls band,
 * and the restart/first-run/eject/simulated-stays dialogs -- behind call sites small enough to
 * leave inline in that upstream-owned file.
 *
 * EmulationFragment owns one, constructs [DualScreenEmulationController] with it as [hooks]
 * (its callbacks land here directly instead of bouncing through the fragment), and forwards a
 * handful of lifecycle/config/surface points to [updateLayout], [onFoldingFeatureChanged],
 * [onResume] or [onDestroyView]. [fragment] is only used for context/isAdded/resources, exactly
 * like [DualScreenEmulationController] already does.
 */
class DualScreenFragmentHost(
    private val fragment: Fragment,
    private val binding: () -> FragmentEmulationBinding?,
    private val dualScreen: () -> DualScreenEmulationController,
    private val hasPhysicalControllerConnected: () -> Boolean,
    private val isInFoldableLayout: KMutableProperty0<Boolean>,
    private val isEmulationRunning: () -> Boolean,
    private val pauseEmulationAndCaptureFrame: () -> Unit,
    private val resumeEmulationFromUi: () -> Unit,
    private val clearPausedFrame: () -> Unit,
    private val stopEmulation: () -> Unit,
    private val setEmulationStopping: () -> Unit
) : DualScreenFragmentHooks {
    // Eden Duo Stage 2: the last seen separating FoldingFeature (either orientation), for
    // DualScreenGeometry; Stage 4: true while emulation is paused for the eject dialog --
    // distinguishes "we paused it, resume once the user chooses" from the user having already
    // paused manually before the eject happened.
    private var currentHinge: DualScreenHinge? = null
    private var currentLayout: DualScreenLayout? = null
    private var pausedForDisplayEject = false

    // Eden Duo 1.4.0: the freeform layout editor (LAYOUT-EDITOR-SPEC.md). Both non-null exactly
    // while editing; orientationLockedForEditor mirrors EmulationFragment's own
    // startConfiguringControls/stopConfiguringControls pattern for the controls editor.
    // layoutEditorBackCallback: Back while editing must act as Cancel (steward review,
    // beta.4) -- registered only while editing and removed the moment it ends either way, so
    // it never outlives the session or shadows the drawer-toggle callback EmulationFragment
    // itself already registers once at setup.
    private var layoutEditorView: DualScreenLayoutEditorView? = null
    private var layoutEditorToolbar: View? = null
    private var orientationLockedForEditor = false
    private var layoutEditorBackCallback: OnBackPressedCallback? = null

    /** Routes touches inside the companion pane to NativeInput instead of the emulated input. */
    val companionPaneTouch = CompanionPaneTouch { currentLayout?.companionRect }

    override fun pushLayout() = updateLayout()
    override fun onModeChangedAcrossOff() = showModeRestartPrompt()
    override fun onFirstSimulatedCompanion() = showFirstRunNotice()
    override fun companionPaneOwns(pointerId: Int) = companionPaneTouch.isOwned(pointerId)
    override fun onDisplayEjected(kind: DisplayHotplugRules.RemovedDisplayKind) = showEjectDialog(kind)
    override fun onEjectReconnected() = resumeAfterDisplayEjectIfPaused()
    override fun onSimulatedStaysHint() = showSimulatedStaysHint()

    /** EmulationFragment.onResume: wires the companion pane's touch routing into the overlay and
     *  (1.4.0) keeps the in-game menu's "Edit dual screen layout" entry's visibility current. */
    fun onResume() {
        binding()?.surfaceInputOverlay?.companionPaneTouch = companionPaneTouch
        binding()?.inGameMenu?.menu?.findItem(R.id.menu_edit_dual_screen_layout)?.isVisible =
            dualScreen().isSimulatedActive() && !isEditingLayout
    }

    /** EmulationFragment.onDestroyView: unwires the overlay, drops any in-flight touch and (1.4.0)
     *  tears down the layout editor if it was still open (no save -- same as Cancel). */
    fun onDestroyView() {
        binding()?.surfaceInputOverlay?.companionPaneTouch = null
        companionPaneTouch.clear()
        currentLayout = null
        if (isEditingLayout) stopEditingLayout(save = false)
    }

    /**
     * Eden Duo Stage 1: while the hidden developer switch ([InlineDualScreen]) is on, the
     * SurfaceView must fill the whole window -- the native renderer does its own game/companion
     * split and fits the game inside its own rect (RendererVulkan::Composite), so a Top/Bottom
     * [normal] alignment with a fixed aspect ratio would otherwise shrink the surface (and the
     * Vulkan swapchain behind it) to that shape before the native split ever sees it. EmulationFragment's
     * updateScreenLayout feeds this (and [aspectRatioOverride]) its normal, switch-off answer.
     */
    fun verticalAlignmentOverride(normal: EmulationVerticalAlignment): EmulationVerticalAlignment =
        if (inlineActive()) EmulationVerticalAlignment.Center else normal

    /** See [verticalAlignmentOverride]: the matching aspect-ratio override (none while inline). */
    fun aspectRatioOverride(normal: Rational?): Rational? = if (inlineActive()) null else normal

    private fun inlineActive(): Boolean =
        fragment.context?.let { InlineDualScreen.isEnabled(it) } == true

    /**
     * EmulationFragment.updateFoldableLayout: DualScreenGeometry's own hinge split (book posture,
     * a vertical separating hinge) -- the pre-existing fold code there only ever reacted to a
     * horizontal one.
     */
    fun onFoldingFeatureChanged(feature: FoldingFeature?) {
        currentHinge = feature?.takeIf { it.isSeparating }?.let {
            DualScreenHinge(
                isSeparating = true,
                orientation = if (it.orientation == FoldingFeature.Orientation.HORIZONTAL) {
                    DualScreenHingeOrientation.HORIZONTAL
                } else {
                    DualScreenHingeOrientation.VERTICAL
                },
                boundsLeft = it.bounds.left,
                boundsTop = it.bounds.top,
                boundsRight = it.bounds.right,
                boundsBottom = it.bounds.bottom
            )
        }
        updateLayout()
    }

    /**
     * Recomputes DualScreenGeometry's layout from the current window size, [currentHinge] and
     * controller connection, and pushes the game/companion rects to the native renderer; disables
     * the inline path instead when Simulated is not active or the companion is hidden by the user
     * (CompanionToggle) or PiP, in which case the game gets the full window exactly as if dual
     * screen were off. Called from EmulationFragment's updateScreenLayout/updateFoldableLayout/
     * surfaceChanged and from [DualScreenFragmentHooks.pushLayout] (settings closed, companion
     * toggled, onGameResolved, an eject choice).
     */
    fun updateLayout() {
        val b = binding() ?: return
        if (!dualScreen().isSimulatedActive()) {
            currentLayout = null
            NativeLibrary.setDualScreenInlineLayout(false, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f)
            applyControlsBand(DualScreenRect.EMPTY)
            return
        }
        val windowW = b.surfaceEmulation.width
        val windowH = b.surfaceEmulation.height
        if (windowW <= 0 || windowH <= 0) {
            return // Not measured yet; the next trigger (surfaceChanged, a later layout pass) retries.
        }
        // Eden Duo Stage 4 item 3 (RESEARCH.md R5): PiP shows the game only -- the companion pane
        // is hidden through this same full-window path, not paused, exactly like the user's own
        // toggle. Physical is unaffected (it lives on its own display, untouched by PiP).
        val pip = fragment.activity?.isInPictureInPictureMode == true
        val layout = if (dualScreen().isCompanionHiddenByUser || pip) {
            DualScreenLayout(
                DualScreenRect(0, 0, windowW, windowH),
                DualScreenRect.EMPTY,
                DualScreenRect.EMPTY
            )
        } else {
            val window = DualScreenWindow(windowW, windowH, fragment.resources.displayMetrics.densityDpi)
            val gameAspect =
                DualScreenGeometry.aspectForRendererSetting(IntSetting.RENDERER_ASPECT_RATIO.getInt())
            DualScreenGeometry.compute(
                window, currentHinge, hasPhysicalControllerConnected(), currentUserLayout(window), gameAspect
            )
        }
        currentLayout = layout
        val gameFrac = layout.gameRect.toFrac(windowW, windowH)
        val companionFrac = layout.companionRect.toFrac(windowW, windowH)
        NativeLibrary.setDualScreenInlineLayout(
            true,
            gameFrac[0], gameFrac[1], gameFrac[2], gameFrac[3],
            companionFrac[0], companionFrac[1], companionFrac[2], companionFrac[3]
        )
        applyControlsBand(layout.controlsRect)
    }

    /**
     * The Simulated layout engine's input for the live window [window]: the saved Layout choice,
     * plus (1.4.0) Custom's own saved fractions for this orientation/title when that is the
     * choice. Shared by [updateLayout] and [startEditingLayout] so both always agree on what
     * "the current layout" means. 1.4.0: Stacked/Side by side no longer read the retired Divider
     * position setting -- they use [DualScreenUserLayout]'s own fixed ~60% default (see its
     * class doc).
     */
    private fun currentUserLayout(window: DualScreenWindow): DualScreenUserLayout {
        val choice = DualScreenLayoutChoice.entries.getOrElse(
            IntSetting.COMPANION_LAYOUT.getInt()
        ) { DualScreenLayoutChoice.AUTOMATIC }
        val customLayout = if (choice == DualScreenLayoutChoice.CUSTOM) {
            fragment.context?.let {
                DualScreenCustomLayoutStore.load(it, window.isLandscape, dualScreen().currentSecondScreenTitle())
            }
        } else {
            null
        }
        return DualScreenUserLayout(choice = choice, customLayout = customLayout)
    }

    // ---- 1.4.0: the freeform layout editor (LAYOUT-EDITOR-SPEC.md) ----

    /** True while the "Edit dual screen layout" editor is open over the game. */
    val isEditingLayout: Boolean get() = layoutEditorView != null

    /**
     * EmulationFragment's "Edit dual screen layout" in-game menu entry (Simulated only): opens
     * the freeform editor over the running game, seeded from whatever is on screen right now
     * (today's resolved layout, Automatic or otherwise) -- reuses InputOverlay's own edit-mode
     * shape (a view added over the game, a Done button, an orientation lock) rather than
     * inventing a new one. The live native split is untouched until Done commits it.
     */
    @SuppressLint("SourceLockedOrientationActivity")
    fun startEditingLayout() {
        if (isEditingLayout) return
        val b = binding() ?: return
        val context = fragment.context ?: return
        val windowW = b.surfaceEmulation.width
        val windowH = b.surfaceEmulation.height
        if (windowW <= 0 || windowH <= 0) return
        val window = DualScreenWindow(windowW, windowH, fragment.resources.displayMetrics.densityDpi)
        val gameAspect = DualScreenGeometry.aspectForRendererSetting(IntSetting.RENDERER_ASPECT_RATIO.getInt())
        val companionAspect = DualScreenGeometry.CANVAS_ASPECT
        var current = DualScreenGeometry.compute(
            window, currentHinge, hasPhysicalControllerConnected(), currentUserLayout(window), gameAspect
        )
        if (current.companionRect.isEmpty) {
            // The companion is collapsed (today's Automatic pane floor, or the toggle) -- there
            // would be nothing to see or drag. Seed from the exempt-from-the-floor explicit
            // arrangement instead (same shape "Reset" uses), so the editor always opens with a
            // visible, draggable companion rect.
            current = DualScreenGeometry.compute(
                window, null, hasPhysicalControllerConnected(),
                DualScreenUserLayout(
                    if (window.isLandscape) DualScreenLayoutChoice.SIDE_BY_SIDE else DualScreenLayoutChoice.STACKED
                ),
                gameAspect
            )
        }
        // Lock orientation while editing, same as EmulationFragment's own controls editor
        // (startConfiguringControls) -- a rotation mid-edit would invalidate the window this
        // editor was seeded from.
        if (IntSetting.RENDERER_SCREEN_LAYOUT.getInt() == EmulationOrientation.Unspecified.int) {
            fragment.activity?.let {
                it.requestedOrientation = if (
                    fragment.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
                ) {
                    ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT
                } else {
                    ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE
                }
            }
            orientationLockedForEditor = true
        }
        val companionBox = companionSafeBox(windowW, windowH)
        val editorView = DualScreenLayoutEditorView(context, null).apply {
            configure(
                window, DualScreenRect(0, 0, windowW, windowH), companionBox,
                gameAspect, companionAspect,
                current.gameRect.takeUnless { it.isEmpty } ?: DualScreenRect(0, 0, windowW, windowH),
                current.companionRect
            )
        }
        b.overlayContainer.addView(
            editorView, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        )
        val toolbar = LayoutInflater.from(context)
            .inflate(R.layout.view_dual_screen_layout_editor_toolbar, b.overlayContainer, false)
        wireToolbar(toolbar, editorView, window, gameAspect, companionAspect)
        b.overlayContainer.addView(toolbar)
        layoutEditorView = editorView
        layoutEditorToolbar = toolbar
        binding()?.inGameMenu?.menu?.findItem(R.id.menu_edit_dual_screen_layout)?.isVisible = false
        // Steward review (beta.4): back must act as Cancel, never fall through to the drawer
        // toggle or exit the game -- the same OnBackPressedCallback shape
        // EmulationFragment.setupOverlayGamelessEditMode already uses for its own edit mode.
        // Kept in a field (not fire-and-forget like that one call site) so stopEditingLayout can
        // remove() it itself when Cancel/Done/Done close the editor instead of back, rather than
        // leaving an enabled callback sitting on the dispatcher after editing ends;
        // viewLifecycleOwner also tears it down automatically if the view is destroyed first.
        val backCallback = object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = stopEditingLayout(save = false)
        }
        layoutEditorBackCallback = backCallback
        fragment.activity?.onBackPressedDispatcher?.addCallback(fragment.viewLifecycleOwner, backCallback)
    }

    private fun wireToolbar(
        toolbar: View,
        editorView: DualScreenLayoutEditorView,
        window: DualScreenWindow,
        gameAspect: DualScreenAspect?,
        companionAspect: DualScreenAspect
    ) {
        toolbar.findViewById<View>(R.id.dual_screen_layout_editor_swap)
            ?.setOnClickListener { editorView.swap() }
        toolbar.findViewById<View>(R.id.dual_screen_layout_editor_reset)
            ?.setOnClickListener {
                val automatic = DualScreenGeometry.compute(
                    window, currentHinge, hasPhysicalControllerConnected(),
                    DualScreenUserLayout(DualScreenLayoutChoice.AUTOMATIC), gameAspect
                )
                editorView.reset(automatic)
            }
        toolbar.findViewById<View>(R.id.dual_screen_layout_editor_cancel)
            ?.setOnClickListener { stopEditingLayout(save = false) }
        toolbar.findViewById<View>(R.id.dual_screen_layout_editor_done)
            ?.setOnClickListener { stopEditingLayout(save = true) }
    }

    /**
     * Closes the editor; [save] (Done, true) persists its current rects as the saved Custom
     * layout and sets Layout = Custom, mirroring how Eden's own per-game settings decide global
     * vs. per-title (LAYOUT-EDITOR-SPEC.md item 3): per-title exactly when this game session has
     * a per-game config loaded ([NativeConfig.isPerGameConfigLoaded]), global otherwise -- the
     * same condition the app's own per-game settings save path already uses. Cancel (false) --
     * including via the back button/gesture, which [startEditingLayout]'s own callback routes
     * here too (steward review, beta.4: back must act as Cancel, never leave the editor
     * half-open or exit the game) -- discards the in-progress edit; the live native layout was
     * never touched either way, so "discard" is simply "do nothing further", not a restore.
     */
    private fun stopEditingLayout(save: Boolean) {
        val editorView = layoutEditorView ?: return
        val b = binding()
        layoutEditorBackCallback?.remove()
        layoutEditorBackCallback = null
        if (save) {
            val context = fragment.context
            val perGame = NativeConfig.isPerGameConfigLoaded()
            val gameId = if (perGame) dualScreen().currentSecondScreenTitle() else null
            if (context != null) {
                DualScreenCustomLayoutStore.save(
                    context, editorView.window.isLandscape, gameId,
                    DualScreenCustomLayout(
                        editorView.gameRect.toNormRect(editorView.window),
                        editorView.companionRect.toNormRect(editorView.window)
                    )
                )
            }
            IntSetting.COMPANION_LAYOUT.global = !perGame
            IntSetting.COMPANION_LAYOUT.setInt(DualScreenLayoutChoice.CUSTOM.ordinal)
            if (perGame) NativeConfig.savePerGameConfig() else NativeConfig.saveGlobalConfig()
        }
        b?.overlayContainer?.removeView(editorView)
        layoutEditorToolbar?.let { b?.overlayContainer?.removeView(it) }
        layoutEditorView = null
        layoutEditorToolbar = null
        if (orientationLockedForEditor) {
            fragment.activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
            orientationLockedForEditor = false
        }
        if (save) updateLayout()
        binding()?.inGameMenu?.menu?.findItem(R.id.menu_edit_dual_screen_layout)?.isVisible =
            dualScreen().isSimulatedActive()
    }

    /**
     * The companion pane's safe-drawing box (spec item 2: "safe-drawing insets for the
     * companion"), approximated from the system bars + display cutout of the current root window
     * insets; the full window when insets are not available yet (same fallback behaviour as
     * showing no inset at all).
     */
    private fun companionSafeBox(windowW: Int, windowH: Int): DualScreenRect {
        val root = binding()?.root
        val insets = root?.let { ViewCompat.getRootWindowInsets(it) }
            ?.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
        val left = insets?.left ?: 0
        val top = insets?.top ?: 0
        val right = insets?.right ?: 0
        val bottom = insets?.bottom ?: 0
        return DualScreenRect(left, top, (windowW - left - right).coerceAtLeast(0), (windowH - top - bottom).coerceAtLeast(0))
    }

    /**
     * Resizes `FragmentEmulationBinding.emulationContainer`/`inputContainer` to [controlsRect]
     * (the no-controller Automatic stacked case's third band), or back to full-window when
     * [controlsRect] is empty -- the same container-resize EmulationFragment.updateFoldableLayout
     * already does for the existing horizontal-hinge split, which this defers to entirely
     * whenever [currentHinge] says a real fold is separating (both own the same two containers;
     * only one of them is ever the active reason at a time).
     */
    private fun applyControlsBand(controlsRect: DualScreenRect) {
        if (currentHinge?.isSeparating == true) return
        val b = binding() ?: return
        if (controlsRect.isEmpty) {
            if (!isInFoldableLayout.get()) return // nothing this function set up needs undoing
            b.emulationContainer.layoutParams.height = ViewGroup.LayoutParams.MATCH_PARENT
            b.inputContainer.layoutParams.height = ViewGroup.LayoutParams.MATCH_PARENT
            isInFoldableLayout.set(false)
            b.surfaceInputOverlay.layout = if (
                fragment.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
            ) {
                OverlayLayout.Portrait
            } else {
                OverlayLayout.Landscape
            }
        } else {
            b.emulationContainer.layoutParams.height = controlsRect.y
            b.inputContainer.layoutParams.height = controlsRect.h
            isInFoldableLayout.set(true)
            b.surfaceInputOverlay.layout = OverlayLayout.Foldable
        }
        b.emulationContainer.requestLayout()
        b.inputContainer.requestLayout()
    }

    /**
     * Eden Duo Stage 2 item 8 / Stage 4 item 5: Mode just crossed to or from Off, which only
     * applies at the next launch. "Exit to game list" reuses EmulationFragment's existing
     * menu_exit path verbatim rather than attempting to relaunch the game itself -- see
     * STAGE4-IMPLEMENTATION.md for why a same-game relaunch was not wired up.
     */
    private fun showModeRestartPrompt() {
        if (!fragment.isAdded || binding() == null) return
        MaterialAlertDialogBuilder(fragment.requireContext())
            .setTitle(R.string.dual_screen_mode_restart_title)
            .setMessage(R.string.dual_screen_mode_restart_message)
            .setPositiveButton(R.string.dual_screen_mode_restart_exit) { _, _ ->
                val b = binding() ?: return@setPositiveButton
                clearPausedFrame()
                stopEmulation()
                NativeConfig.reloadGlobalConfig()
                setEmulationStopping()
                b.drawerLayout.close()
            }
            .setNegativeButton(R.string.dual_screen_mode_restart_later, null)
            .show()
    }

    /** Eden Duo Stage 2 item 9: the one-time notice the first time a companion appears in Simulated. */
    private fun showFirstRunNotice() {
        if (!fragment.isAdded) return
        MaterialAlertDialogBuilder(fragment.requireContext())
            .setTitle(R.string.dual_screen_first_run_title)
            .setMessage(R.string.dual_screen_first_run_message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    /**
     * Eden Duo Stage 4 item 1 (RESEARCH.md §0 decision 3): the display hosting the companion or
     * the game was just removed. Pauses emulation (unless the user had already paused it
     * themselves -- [pausedForDisplayEject] remembers which) and shows the three-choice dialog,
     * non-cancelable so it can never be dismissed without a choice ("Never auto-resume without a
     * choice"). [kind] only decides the log line here; all three choices behave the same
     * regardless of whether the companion's own display or the game's (Swap Screens) was lost.
     */
    private fun showEjectDialog(kind: DisplayHotplugRules.RemovedDisplayKind) {
        if (!fragment.isAdded || binding() == null) return
        Log.info("[EmulationFragment] DSMod: eject dialog for $kind")
        pausedForDisplayEject = isEmulationRunning()
        if (pausedForDisplayEject) {
            pauseEmulationAndCaptureFrame()
        }
        MaterialAlertDialogBuilder(fragment.requireContext())
            .setTitle(R.string.dual_screen_eject_title)
            .setMessage(R.string.dual_screen_eject_message)
            .setCancelable(false)
            .setPositiveButton(R.string.dual_screen_eject_continue_simulated) { _, _ ->
                dualScreen().ejectChoiceContinueSimulated()
                resumeAfterDisplayEjectIfPaused()
            }
            .setNegativeButton(R.string.dual_screen_eject_continue_game_only) { _, _ ->
                dualScreen().ejectChoiceContinueGameOnly()
                resumeAfterDisplayEjectIfPaused()
            }
            .setNeutralButton(R.string.dual_screen_eject_wait_reconnect) { _, _ ->
                // Stays paused: resumed via resumeAfterDisplayEjectIfPaused once Physical
                // actually shows again (DualScreenFragmentHooks.onEjectReconnected).
                dualScreen().ejectChoiceWaitForReconnect()
            }
            .show()
    }

    /** Resumes emulation only if [showEjectDialog] was the one that paused it. */
    private fun resumeAfterDisplayEjectIfPaused() {
        if (!pausedForDisplayEject) return
        pausedForDisplayEject = false
        if (!fragment.isAdded || binding() == null) return
        resumeEmulationFromUi()
    }

    /** Eden Duo Stage 4 item 2: a display appeared while Mode stayed Simulated (the user's own explicit choice). */
    private fun showSimulatedStaysHint() {
        if (!fragment.isAdded) return
        MaterialAlertDialogBuilder(fragment.requireContext())
            .setTitle(R.string.dual_screen_simulated_stays_hint_title)
            .setMessage(R.string.dual_screen_simulated_stays_hint_message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }
}
