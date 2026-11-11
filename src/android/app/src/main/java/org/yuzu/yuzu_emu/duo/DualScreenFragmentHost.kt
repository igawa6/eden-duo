// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.res.Configuration
import android.util.Rational
import android.view.ViewGroup
import androidx.fragment.app.Fragment
import androidx.window.layout.FoldingFeature
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.FragmentEmulationBinding
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
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

    /** Routes touches inside the companion pane to NativeInput instead of the emulated input. */
    val companionPaneTouch = CompanionPaneTouch { currentLayout?.companionRect }

    override fun pushLayout() = updateLayout()
    override fun onModeChangedAcrossOff() = showModeRestartPrompt()
    override fun onFirstSimulatedCompanion() = showFirstRunNotice()
    override fun companionPaneOwns(pointerId: Int) = companionPaneTouch.isOwned(pointerId)
    override fun onDisplayEjected(kind: DisplayHotplugRules.RemovedDisplayKind) = showEjectDialog(kind)
    override fun onEjectReconnected() = resumeAfterDisplayEjectIfPaused()
    override fun onSimulatedStaysHint() = showSimulatedStaysHint()

    /** EmulationFragment.onResume: wires the companion pane's touch routing into the overlay. */
    fun onResume() {
        binding()?.surfaceInputOverlay?.companionPaneTouch = companionPaneTouch
    }

    /** EmulationFragment.onDestroyView: unwires the overlay and drops any in-flight touch. */
    fun onDestroyView() {
        binding()?.surfaceInputOverlay?.companionPaneTouch = null
        companionPaneTouch.clear()
        currentLayout = null
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
            val userLayout = DualScreenUserLayout(
                choice = DualScreenLayoutChoice.entries.getOrElse(
                    IntSetting.COMPANION_LAYOUT.getInt()
                ) { DualScreenLayoutChoice.AUTOMATIC },
                dividerPercent = IntSetting.DIVIDER_POSITION.getInt()
            )
            val gameAspect =
                DualScreenGeometry.aspectForRendererSetting(IntSetting.RENDERER_ASPECT_RATIO.getInt())
            DualScreenGeometry.compute(
                window, currentHinge, hasPhysicalControllerConnected(), userLayout, gameAspect
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
