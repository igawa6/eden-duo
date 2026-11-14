// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.app.Activity
import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.view.Display
import androidx.fragment.app.Fragment
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.activities.AuxCompanionActivity
import org.yuzu.yuzu_emu.features.settings.model.IntSetting
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.CompanionApp
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NoCompanion
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame
import org.yuzu.yuzu_emu.views.AuxPresentation

/**
 * Eden Duo Stage 2/4: the fragment-side reactions [DualScreenEmulationController] needs but cannot
 * perform itself -- the native Simulated layout push, the restart/first-run/eject/simulated-stays
 * dialogs, and companion-pane touch ownership. [DualScreenFragmentHost] is EmulationFragment's
 * implementation, passed in as a single object instead of one lambda per reaction.
 */
interface DualScreenFragmentHooks {
    /** Recomputes and pushes the Simulated layout (EmulationFragment owns the window/fold/controller state this needs). */
    fun pushLayout() {}

    /** Mode just changed across Off in the in-game settings, which only takes effect at the next launch. */
    fun onModeChangedAcrossOff() {}

    /** A companion just appeared in Simulated mode for the first time ever (see [DualScreenFirstRunNotice]). */
    fun onFirstSimulatedCompanion() {}

    /** True while [pointerId] already belongs to the Simulated companion pane (RESEARCH.md I2). */
    fun companionPaneOwns(pointerId: Int): Boolean = false

    /** The display hosting the companion or the game was just removed: shows the eject dialog;
     *  true when it is actually showing (false when the fragment cannot show one right now). */
    fun onDisplayEjected(kind: DisplayHotplugRules.RemovedDisplayKind): Boolean = false

    /** The "Wait for reconnect" eject choice just resolved on its own. */
    fun onEjectReconnected() {}

    /** A display appeared mid-game while Mode stayed Simulated by the user's own explicit choice. */
    fun onSimulatedStaysHint() {}

    companion object {
        val NONE: DualScreenFragmentHooks = object : DualScreenFragmentHooks {}
    }
}

/**
 * Eden Duo: the second screen of the emulation screen -- the DSMod Presentation on a second panel
 * (AYN Thor bottom panel, AYANEO Pocket DS, ...), AuxCompanionActivity when the game itself is on
 * a secondary display, and the "No Companion" setting (Icon, Black, Off, App).
 *
 * EmulationFragment owns one and forwards its lifecycle to it. The lambdas read the fragment's
 * state at call time: [emulationActivity] is the game activity, [programId] the running game's id,
 * [secondScreenTitle] the title whose per-game Second Screen values apply (null for global) and
 * [emulationStarted] whether the game finished booting.
 *
 * The user can hide and show the window during the game (CompanionToggle); while it is hidden,
 * nothing here shows it again on its own.
 */
class DualScreenEmulationController(
    private val fragment: Fragment,
    private val emulationActivity: () -> Activity?,
    private val programId: () -> String?,
    private val secondScreenTitle: () -> String?,
    private val emulationStarted: () -> Boolean,
    /**
     * The fragment-side reactions this controller cannot perform itself -- the Simulated layout
     * push, the restart/first-run/eject/simulated-stays dialogs and companion-pane touch
     * ownership. [DualScreenFragmentHost] is EmulationFragment's implementation; the default is a
     * no-op for callers that have not wired Simulated support up.
     */
    private val hooks: DualScreenFragmentHooks = DualScreenFragmentHooks.NONE
) {
    private companion object {
        const val AUX_RETRY_DELAY_MS = 300L
        const val IDLE_RECHECK_ATTEMPTS = 20 // x AUX_RETRY_DELAY_MS
    }

    // DSMod: second-screen Presentation. It lives from onStart to onStop, so it stays up while the
    // activity is only paused (PiP, a dialog on top), and a DisplayListener brings it back when a
    // panel appears or turns on.
    private var auxPresentation: AuxPresentation? = null
    private var auxWanted = false
    // Eden Duo "No Companion: Off": this game gets no second-screen window (see NoCompanion).
    // Icon and Black need nothing here: the runtime builds the idle page when the game boots.
    // A change made in the in-game settings applies on return (reapplyNoCompanion), except
    // Icon <-> Black, which shows from the next game start. A frontend's intent launch resolves
    // the game asynchronously (EmulationFragment.resolveIntentGameAsync): onStart can show the
    // window before isAuxOff has a programId to answer for, and onGameResolved then closes it
    // once Off is decided for the resolved game. Known limitation: a ROM swap inside a swapped
    // session (game on the second display) into a game without a companion, with Off, closes the
    // companion window and may leave the main display showing the Eden UI.
    private var auxOff = false
    private var auxOffChecked = false
    // Eden Duo "No Companion: App": this boot already tried the chosen app (CompanionApp).
    private var companionAppTried = false
    // No Companion and its app as this game session last applied them (null before boot).
    private var noCompanionApplied: Pair<Int, String?>? = null
    // Eden Duo Stage 2: the resolved Mode as this game session last applied it (null before boot).
    private var modeApplied: DualScreenMode? = null
    // Eden Duo Stage 4: forced for the rest of this game session by the eject dialog's
    // "Continue on this screen" choice -- never persisted (RESEARCH.md decision 3: "Requested
    // mode stays saved as is"); resolvedMode() returns it ahead of the normal resolver. Cleared
    // by nothing short of a fresh game session (a new DualScreenEmulationController).
    private var sessionModeOverride: DualScreenMode? = null
    // Eden Duo Stage 4: true while the eject dialog's "Wait for reconnect" choice is in effect --
    // showAuxPresentation's next success (any display reconnecting, not just the one lost)
    // resumes emulation via DualScreenFragmentHooks.onEjectReconnected and clears this.
    private var awaitingReconnectAfterEject = false
    // Eden Duo Stage 4: true while an eject dialog is up (or should be) for this session --
    // guards onDisplayRemoved against firing hooks.onDisplayEjected twice for the same disconnect.
    private var ejectPending = false
    // Eden Duo Stage 4: the game Activity's own display id while Swap Screens has it on a
    // secondary one, refreshed opportunistically in showAuxPresentation (null on the default
    // display or before it is known) -- lets onDisplayRemoved tell the game's own display apart
    // from an unrelated one going away.
    private var lastKnownGameDisplayId: Int? = null
    // dropIdleCompanion asked before the game finished loading: ask again a little later.
    private var idleRecheckLeft = 0
    private val idleRecheck = Runnable { dropIdleCompanion() }
    private val auxHandler = Handler(Looper.getMainLooper())
    // Eden Duo companion toggle (CompanionToggle): hidden by the user for this game session.
    // While hidden nothing shows the window automatically -- showAuxPresentation is the one gate.
    private val toggleState = CompanionToggleState()
    private val auxRetry = Runnable { if (auxWanted) showAuxPresentation() }
    private val auxDisplayStates = HashMap<Int, Int>()
    private val auxDisplayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayAdded(displayId: Int) {
            AuxPresentation.forgetQuarantine(displayId)
            displayState(displayId)?.let { auxDisplayStates[displayId] = it }
            scheduleAuxRetry()
            reapplyModeForDisplayEvent()
        }

        override fun onDisplayRemoved(displayId: Int) {
            AuxPresentation.forgetQuarantine(displayId)
            auxDisplayStates.remove(displayId)
            // Eden Duo Stage 4 (RESEARCH.md §0 decision 3): a display hosting the companion or
            // the game going away is an eject -- pause and ask, never auto-resume. Never for an
            // unrelated display (item 3: STATE_OFF never reaches onDisplayRemoved at all, so a
            // lid closing or a TV going to standby is not handled here either way).
            val kind = DisplayHotplugRules.classifyRemoval(
                displayId, auxPresentation?.displayId, lastKnownGameDisplayId
            )
            if (kind == DisplayHotplugRules.RemovedDisplayKind.COMPANION) {
                Log.info("[EmulationFragment] DSMod: second-screen display $displayId removed")
                dismissAuxPresentation()
            } else if (kind == DisplayHotplugRules.RemovedDisplayKind.GAME) {
                // The companion's own default-display window cannot share it with the game,
                // which is about to fall back there -- the eject choices replace it (Simulated
                // inline, or hidden) rather than leaving it to collide.
                AuxCompanionActivity.dismiss(fragment)
            }
            val ejects = modeApplied?.let { DisplayHotplugRules.isEject(kind, it) } == true
            if (ejects && !ejectPending) {
                Log.info("[EmulationFragment] DSMod: pausing and asking ($kind, display $displayId)")
                // Pending only while a dialog is really up -- one that could not be shown must
                // not block every later eject for the rest of the session.
                if (hooks.onDisplayEjected(kind)) {
                    ejectPending = true
                    return
                }
            }
            if (kind == DisplayHotplugRules.RemovedDisplayKind.COMPANION) {
                scheduleAuxRetry()
            } else if (AuxCompanionActivity.isUpOrPending()) {
                // The game's own display may be the one gone, moving it under the companion.
                scheduleAuxRetry()
            }
        }

        override fun onDisplayChanged(displayId: Int) {
            val state = displayState(displayId) ?: return
            val previous = auxDisplayStates.put(displayId, state)
            // Only a display turning on matters: it may be the second panel coming back. Item 3:
            // a transition to STATE_OFF (Thor lid, TV standby) never enters this branch, so it
            // is never treated as an eject or a reconnect either.
            if (auxPresentation == null && state != Display.STATE_OFF &&
                (previous == null || previous == Display.STATE_OFF)
            ) {
                scheduleAuxRetry()
                reapplyModeForDisplayEvent()
            }
        }
    }

    /**
     * Before the game's configuration loads: Companion Ratio is per-game, so a previous game's
     * override is dropped first (the per-game load sets it again when this game has one).
     */
    fun onGameConfigLoading() {
        IntSetting.COMPANION_RATIO.global = true
    }

    /**
     * The game's configuration is loaded; [customConfigGame] is the game whose per-game config
     * applies, null for global. Hands this game's No Companion to the idle page the runtime
     * builds at boot, and the resolved dual-screen Mode to the launch gate (Off) -- both must be
     * known before [org.yuzu.yuzu_emu.NativeLibrary.run], and this is the first point where
     * [secondScreenTitle] reliably is (see [onGameResolved] for the async-resolution race).
     */
    fun onGameConfigLoaded(customConfigGame: Game?) {
        SecondScreenPerGame.runningConfigGame = customConfigGame
        NativeLibrary.setNoCompanionOverride(NoCompanion.perGameValue(secondScreenTitle()) ?: -1)
        NativeLibrary.setCompanionsDisabledForLaunch(resolvedMode() == DualScreenMode.OFF)
    }

    /**
     * The fragment's `game` is now known after an async intent resolution (see
     * EmulationFragment.resolveIntentGameAsync); [isAuxOff] (and the Mode resolver, which also
     * reads [secondScreenTitle] for a per-title override) could not answer for this game before
     * now, so [onStart] may have shown the second-screen window on the assumption that it was
     * wanted, on the global Mode. Closes it if No Companion or the now-known Mode hide it for the
     * resolved game, and re-sends the launch gate in case [onGameConfigLoaded] ran with it still
     * unresolved. launchCompanionApp is not called here: it only runs after boot, from
     * dropIdleCompanion.
     */
    fun onGameResolved() {
        NativeLibrary.setCompanionsDisabledForLaunch(resolvedMode() == DualScreenMode.OFF)
        hooks.pushLayout()
        val decidedBefore = auxOffChecked
        val hidesNow = isAuxOff()
        val closesForNoCompanion = AuxWindowRules.closesWindowOnResolve(decidedBefore, hidesNow)
        val closesForMode = resolvedMode() != DualScreenMode.PHYSICAL
        if (!closesForNoCompanion && !closesForMode) return
        Log.info(
            "[EmulationFragment] DSMod: the second screen is hidden for this game (No Companion " +
                "or Mode); closing the window shown before the title was known"
        )
        dismissAuxPresentation()
        AuxCompanionActivity.dismiss(fragment)
    }

    /**
     * The resolved dual-screen Mode for the running (or about to run) game -- never cached
     * (cheap to recompute: no disk access, unlike [isAuxOff]), so it always reflects the latest
     * second-screen detection and requested-mode setting. [secondScreenTitle] may still be
     * unknown (null) when this is asked from [onStart]; [DualScreenModeSetting.requested] then
     * falls back to the global value, same as a real global launch would.
     */
    private fun resolvedMode(): DualScreenMode {
        // Eden Duo Stage 4: an eject dialog's "Continue on this screen" choice wins outright for
        // the rest of this session, regardless of what detection or the saved setting would
        // otherwise resolve to (see sessionOverrideFor).
        sessionModeOverride?.let { return it }
        val context = fragment.context
        val hasSecondScreen = context != null && SecondScreenDetection.hasSecondScreen(context)
        return DualScreenModeResolver.resolveActive(
            DualScreenModeSetting.requested(secondScreenTitle()),
            hasSecondScreen,
            companionAvailable = true
        )
    }

    /**
     * True when the resolved Mode is Simulated: EmulationFragment should drive the native inline
     * layout from [DualScreenGeometry] instead of leaving it disabled.
     */
    fun isSimulatedActive(): Boolean = resolvedMode() == DualScreenMode.SIMULATED

    /** The game finished booting. */
    fun onEmulationStarted() {
        idleRecheckLeft = IDLE_RECHECK_ATTEMPTS
        noCompanionApplied = noCompanionNow()
        modeApplied = resolvedMode()
        dropIdleCompanion()
        maybeShowFirstRunNotice()
    }

    /**
     * Eden Duo Stage 2: the one-time notice (item 9), the first time a companion appears in
     * Simulated mode for this install. [NoCompanion.hasPackage] is the same disk check
     * [org.yuzu.yuzu_emu.utils.NoCompanion.wantsWindow] already makes for Off/App, so this adds
     * no new I/O pattern.
     */
    private fun maybeShowFirstRunNotice() {
        if (resolvedMode() != DualScreenMode.SIMULATED) return
        val id = programId() ?: return
        if (NoCompanion.hasPackage(id) != true) return
        val context = fragment.context ?: return
        if (!DualScreenFirstRunNotice.shouldShow(context)) return
        DualScreenFirstRunNotice.markShown(context)
        hooks.onFirstSimulatedCompanion()
    }

    /** A multi-program title switched program. */
    fun onProgramChanged() {
        // A multi-program title's next program decides No Companion afresh.
        auxOff = false
        auxOffChecked = false
        companionAppTried = false
        // A second screen the previous program dropped comes back if this one wants it.
        scheduleAuxRetry()
    }

    fun onStart() {
        auxWanted = true
        CompanionToggle.register(this)
        displayManager()?.let { dm ->
            auxDisplayStates.clear()
            for (display in dm.displays) auxDisplayStates[display.displayId] = display.state
            dm.registerDisplayListener(auxDisplayListener, auxHandler)
        }
        showAuxPresentation()
        hooks.pushLayout()
    }

    fun onStop() {
        auxWanted = false
        auxHandler.removeCallbacks(auxRetry)
        auxHandler.removeCallbacks(idleRecheck)
        displayManager()?.unregisterDisplayListener(auxDisplayListener)
        dismissAuxPresentation()
        AuxCompanionActivity.dismiss(fragment)
    }

    fun onResume() {
        reapplyNoCompanion()
        reapplyMode()
        // Settings closed: Mode, Layout, divider position or the aspect ratio may have changed.
        hooks.pushLayout()
    }

    /**
     * Mode may have changed in the in-game settings: decided again without a restart, same shape
     * as [reapplyNoCompanion]. Simulated <-> Physical switch live here (both reuse the aux path
     * -- Physical's own Presentation/AuxCompanionActivity, Simulated the inline push
     * [DualScreenFragmentHooks.pushLayout] does separately); a change to or from Off cannot (the
     * launch gate is read once, before the game loads), so [DualScreenFragmentHooks.onModeChangedAcrossOff]
     * offers a restart instead.
     */
    private fun reapplyMode() {
        val before = modeApplied ?: return
        if (!emulationStarted()) return
        val now = resolvedMode()
        if (now == before) return
        modeApplied = now
        Log.info("[EmulationFragment] DSMod: Mode changed in game: $before -> $now")
        if (now == DualScreenMode.PHYSICAL) {
            if (auxWanted) showAuxPresentation()
        } else {
            dismissAuxPresentation()
            AuxCompanionActivity.dismiss(fragment)
        }
        if (before == DualScreenMode.OFF || now == DualScreenMode.OFF) {
            hooks.onModeChangedAcrossOff()
        }
    }

    /**
     * Eden Duo Stage 4 (RESEARCH.md E4 / §0 item 2): a display was added or turned back on --
     * the same live mode-switch [reapplyMode] already does for a settings change, triggered by a
     * display event instead, with the owner's "display added mid-game" rule layered on top:
     * promotes live to Physical when the resolver says so, or shows a one-time hint when an
     * explicit Simulated choice is the reason Mode stays put. A session eject override
     * ([sessionModeOverride]) wins outright and skips this entirely -- "Continue on this screen"
     * means Simulated for the rest of the session, reconnect or not; [ejectChoiceWaitForReconnect]
     * has its own resolution in [showAuxPresentation], keyed to Physical actually showing again
     * rather than any display event.
     */
    private fun reapplyModeForDisplayEvent() {
        if (sessionModeOverride != null) return
        if (!emulationStarted()) return
        val before = modeApplied ?: return
        val context = fragment.context ?: return
        val requested = DualScreenModeSetting.requested(secondScreenTitle())
        val hasSecondScreen = SecondScreenDetection.hasSecondScreen(context)
        when (DisplayHotplugRules.onDisplayAdded(requested, before, hasSecondScreen)) {
            DisplayHotplugRules.DisplayAddedOutcome.SWITCH_TO_PHYSICAL -> {
                reapplyMode()
                hooks.pushLayout()
            }

            DisplayHotplugRules.DisplayAddedOutcome.SHOW_SIMULATED_HINT -> maybeShowSimulatedStaysHint()
            DisplayHotplugRules.DisplayAddedOutcome.NONE -> Unit
        }
    }

    private fun maybeShowSimulatedStaysHint() {
        val context = fragment.context ?: return
        if (!DualScreenSimulatedStaysHint.shouldShow(context)) return
        DualScreenSimulatedStaysHint.markShown(context)
        hooks.onSimulatedStaysHint()
    }

    /**
     * Eden Duo Stage 4: "Continue on this screen" -- Simulated for the rest of this session,
     * companion state preserved (nothing here touches the runtime's module state, which keeps
     * running regardless of aux visibility, see mod_runtime.cpp).
     */
    fun ejectChoiceContinueSimulated() {
        ejectPending = false
        awaitingReconnectAfterEject = false
        val mode = DisplayHotplugRules.sessionOverrideFor(DisplayHotplugRules.EjectChoice.CONTINUE_SIMULATED)
        sessionModeOverride = mode
        modeApplied = mode
        dismissAuxPresentation()
        AuxCompanionActivity.dismiss(fragment)
        hooks.pushLayout()
    }

    /**
     * Eden Duo Stage 4: "Continue game only" -- the same mechanism as the user's own companion
     * toggle, so `aux.present` goes false and HUD-hiding companions restore (RESEARCH.md C1).
     */
    fun ejectChoiceContinueGameOnly() {
        ejectPending = false
        awaitingReconnectAfterEject = false
        toggleState.forceHidden()
        dismissAuxPresentation()
        AuxCompanionActivity.dismiss(fragment)
        hooks.pushLayout()
    }

    /**
     * Eden Duo Stage 4: "Wait for reconnect" -- stays paused; the next [showAuxPresentation] that
     * actually shows Physical again (any display, not necessarily the one lost) resumes it via
     * [DualScreenFragmentHooks.onEjectReconnected].
     */
    fun ejectChoiceWaitForReconnect() {
        ejectPending = false
        awaitingReconnectAfterEject = true
    }

    fun onDestroyView() {
        CompanionToggle.unregister(this)
        ejectPending = false
        auxWanted = false
        auxHandler.removeCallbacks(auxRetry)
        auxHandler.removeCallbacks(idleRecheck)
        dismissAuxPresentation()
        AuxCompanionActivity.dismiss(fragment)
    }

    /** True while the user hid the second-screen window (CompanionToggle). */
    val isCompanionHiddenByUser: Boolean get() = toggleState.hiddenByUser

    /** The game screen's touch filter for the companion toggle gesture (InputOverlay). */
    val gameTouch: CompanionToggle.GameTouch by lazy {
        val density = fragment.context?.resources?.displayMetrics?.density ?: 1f
        CompanionToggle.GameTouch(density, hooks::companionPaneOwns) {
            fragment.context?.let { CompanionToggle.toggle(it) }
        }
    }

    /**
     * One companion toggle (gesture, Quick Settings tile or in-game menu): hides or shows the
     * companion where it was, per CompanionToggleState's decision table. Never loads a companion
     * and never starts the No Companion app. Main thread.
     *
     * Eden Duo Stage 2: in Simulated this collapses or restores the inline pane (item 7) --
     * [DualScreenFragmentHooks.pushLayout] does the actual recompute, reading [isCompanionHiddenByUser] to
     * force the game full-window while hidden, exactly as if dual screen were off; "a second
     * screen" always exists for this purpose (the pane shares the game's own window), so unlike
     * Physical this never reports [CompanionToggleOutcome.NO_SECOND_SCREEN].
     */
    fun toggleCompanion(nowMs: Long): CompanionToggleOutcome {
        if (isSimulatedActive()) {
            val outcome = toggleState.toggle(nowMs, windowState(), hasSecondScreen = true)
            Log.info("[EmulationFragment] DSMod: companion toggle (Simulated): $outcome")
            if (outcome == CompanionToggleOutcome.HIDDEN || outcome == CompanionToggleOutcome.SHOWN) {
                hooks.pushLayout()
            }
            return outcome
        }
        val game = emulationActivity() ?: fragment.activity
        val outcome = toggleState.toggle(nowMs, windowState(), game != null && hasSecondScreen(game))
        Log.info("[EmulationFragment] DSMod: companion toggle: $outcome")
        when (outcome) {
            CompanionToggleOutcome.HIDDEN -> {
                auxHandler.removeCallbacks(auxRetry)
                val swapped = AuxCompanionActivity.isUpOrPending()
                dismissAuxPresentation()
                AuxCompanionActivity.dismiss(fragment)
                // The companion activity (Swap Screens) had the focus on its display: give it
                // back to the game rather than to whatever is left there.
                if (swapped && game != null) CompanionApp.returnFocusToGame(game, "companion hidden")
            }

            CompanionToggleOutcome.SHOWN -> {
                if (auxWanted) showAuxPresentation()
                // Another app the user opened on the second screen may hold the focus.
                if (game != null) CompanionApp.returnFocusToGame(game, "companion shown")
            }

            else -> Unit
        }
        return outcome
    }

    private fun windowState(): CompanionWindow {
        if (!isAuxOff()) return CompanionWindow.SHOWN_WHEN_WANTED
        val id = programId()
        return if (id != null && NoCompanion.hasDisabledPackage(id)) {
            CompanionWindow.NONE_ADDON_OFF
        } else {
            CompanionWindow.NONE_NO_COMPANION
        }
    }

    /** A display for the second-screen window exists and is on (the window may be down). */
    private fun hasSecondScreen(game: Activity): Boolean {
        if (auxPresentation != null || AuxCompanionActivity.isUpOrPending()) return true
        if (AuxCompanionActivity.isWantedFor(game)) return true // the default display
        val own = AuxCompanionActivity.activityDisplayId(game)
        return SecondScreenDetection.hasSecondScreen(game, own)
    }

    private fun displayManager(): DisplayManager? =
        fragment.context?.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager

    private fun displayState(displayId: Int): Int? = displayManager()?.getDisplay(displayId)?.state

    /** Coalesces bursts of display events into one attempt. */
    private fun scheduleAuxRetry() {
        if (!auxWanted) return
        auxHandler.removeCallbacks(auxRetry)
        auxHandler.postDelayed(auxRetry, AUX_RETRY_DELAY_MS)
    }

    /** True when No Companion is Off and this game has no companion: decided once per game. */
    private fun isAuxOff(): Boolean {
        if (!auxOffChecked) {
            val programId = programId() ?: return false
            auxOffChecked = true
            auxOff = !NoCompanion.wantsWindow(programId, secondScreenTitle())
        }
        return auxOff
    }

    /**
     * After boot: with No Companion Off, a game whose package the runtime did not take (it is on
     * the idle page) gives its second screen back as well. With App, the chosen app opens there.
     */
    private fun dropIdleCompanion() {
        auxHandler.removeCallbacks(idleRecheck)
        if (!emulationStarted()) return
        if (auxOff) {
            // Closes the window regardless of how onGameResolved and showAuxPresentation raced
            // against boot, so every ordering ends with it down.
            dismissAuxPresentation()
            AuxCompanionActivity.dismiss(fragment)
            launchCompanionApp()
            return
        }
        // App that fell back to the Icon page keeps it for this boot.
        if (companionAppTried || !NoCompanion.hidesWindow(secondScreenTitle())) return
        when (NativeLibrary.isCompanionIdle()) {
            1 -> Unit
            0 -> return
            else -> {
                // No game loaded yet (inconclusive): ask again shortly, a bounded number of times.
                if (idleRecheckLeft-- > 0) {
                    auxHandler.postDelayed(idleRecheck, AUX_RETRY_DELAY_MS)
                }
                return
            }
        }
        Log.info("[EmulationFragment] DSMod: no companion in use; closing the second screen")
        auxOff = true
        auxOffChecked = true
        dismissAuxPresentation()
        AuxCompanionActivity.dismiss(fragment)
        launchCompanionApp()
    }

    private fun noCompanionNow(): Pair<Int, String?> =
        NoCompanion.value(secondScreenTitle()) to CompanionApp.flattened(secondScreenTitle())

    /**
     * No Companion or its app changed in the in-game settings: decided again without a restart.
     * Off / App close the second-screen window (App opens the newly chosen app), Icon / Black
     * bring it back.
     */
    private fun reapplyNoCompanion() {
        val before = noCompanionApplied ?: return
        if (!emulationStarted()) return
        val now = noCompanionNow()
        if (now == before) return
        noCompanionApplied = now
        Log.info("[EmulationFragment] No Companion changed in game: ${now.first} ${now.second ?: ""}")
        NativeLibrary.setNoCompanionOverride(NoCompanion.perGameValue(secondScreenTitle()) ?: -1)
        auxOff = false
        auxOffChecked = false
        companionAppTried = false
        if (NoCompanion.hidesWindow(secondScreenTitle())) {
            idleRecheckLeft = IDLE_RECHECK_ATTEMPTS
            dropIdleCompanion()
        } else {
            scheduleAuxRetry()
        }
    }

    /** No Companion App: opens the chosen app, once per boot; on failure, the Icon page. */
    private fun launchCompanionApp() {
        if (companionAppTried) return
        val game = emulationActivity() ?: return
        companionAppTried = true
        if (CompanionApp.launchForBoot(game, secondScreenTitle()) != CompanionApp.Result.FAILED) {
            return
        }
        auxOff = false
        auxOffChecked = true
        if (auxWanted) showAuxPresentation()
    }

    private fun showAuxPresentation() {
        // Eden Duo Stage 4: kept fresh here regardless of mode/toggle state below (unlike the
        // early returns that follow, Swap Screens placing the game on a secondary display is
        // independent of Mode and of whether the companion is currently shown) so
        // onDisplayRemoved can recognise the game's own display going away even in Simulated.
        refreshGameDisplayId()
        if (!toggleState.allowsAutoShow()) return
        if (isAuxOff()) return
        // Eden Duo Stage 2: Simulated (the companion drawn into the game's own surface) and Off
        // (no companion runtime at all) equally open no Presentation/AuxCompanionActivity here;
        // Physical is the only mode this function's own logic applies to.
        if (resolvedMode() != DualScreenMode.PHYSICAL) return
        // Eden Duo: with the game on a secondary display the second screen is the default one,
        // where a Presentation cannot go; AuxCompanionActivity hosts it there instead.
        val game = emulationActivity() ?: fragment.activity ?: return
        if (AuxCompanionActivity.isWantedFor(game)) {
            dismissAuxPresentation()
            AuxCompanionActivity.launch(game, fragment)
            return
        }
        AuxCompanionActivity.dismiss(fragment) // the game is back on the default display
        if (auxPresentation != null) return
        val presentation = AuxPresentation.showOnBestDisplay(game) ?: return
        auxPresentation = presentation
        // Eden Duo Stage 4: "Wait for reconnect" resolves the moment Physical actually shows
        // again -- not on every display event, which may not be the one that was lost.
        if (awaitingReconnectAfterEject) {
            awaitingReconnectAfterEject = false
            hooks.onEjectReconnected()
        }
        // Presentation cancels itself when its display goes away; drop it and look again.
        presentation.setOnDismissListener {
            if (auxPresentation === presentation) {
                auxPresentation = null
                scheduleAuxRetry()
            }
        }
    }

    /**
     * Eden Duo Stage 4: the game Activity's own display id when Swap Screens has it on a
     * secondary one, cached for [onDisplayRemoved] (see [lastKnownGameDisplayId]); null on the
     * default display, where it is never the one that can be "ejected".
     */
    private fun refreshGameDisplayId() {
        val game = emulationActivity() ?: fragment.activity ?: return
        lastKnownGameDisplayId =
            AuxCompanionActivity.activityDisplayId(game).takeIf { it != Display.DEFAULT_DISPLAY }
    }

    private fun dismissAuxPresentation() {
        val presentation = auxPresentation ?: return
        auxPresentation = null
        try {
            presentation.dismiss()
        } catch (e: Exception) {
            Log.error("[EmulationFragment] DSMod: dismiss failed: ${e.message}")
        }
    }
}

/** The Android-free decision of DualScreenEmulationController.onGameResolved (unit tested). */
object AuxWindowRules {
    /**
     * True when a window went up without knowing No Companion for this game ([decidedBefore]
     * false) and it now turns out to hide the window ([hidesNow] true): that window must close.
     * False when the decision was already made before the window was shown (an in-app launch, or
     * a game whose title was known up front), or when No Companion resolves to Icon or Black.
     */
    fun closesWindowOnResolve(decidedBefore: Boolean, hidesNow: Boolean): Boolean =
        !decidedBefore && hidesNow
}
