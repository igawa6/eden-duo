// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

/**
 * Eden Duo Stage 4: the Android-free eject / reconnect / display-added decision (unit tested).
 * RESEARCH.md Â§4.3, owner decision 3 ("pause and ask" on eject) and decision 4 (the existing
 * [DualScreenModeResolver]: Physical wins when a second screen is detected, an explicit choice
 * is kept). Everything here is a pure function of state [DualScreenEmulationController] already
 * tracks or is told by a display event; the Android side -- which display id plays which role,
 * pausing emulation, showing the dialog -- stays there.
 */
object DisplayHotplugRules {

    /** Which role, if any, the display `DisplayManager` just reported removed was playing. */
    enum class RemovedDisplayKind { COMPANION, GAME, UNRELATED }

    /**
     * [companionDisplayId] is the display the active Physical `AuxPresentation` is on (null
     * outside Physical, or before one has ever shown); [gameDisplayId] is the game Activity's
     * own display when Swap Screens has it on a secondary one (null otherwise, including the
     * ordinary default-display case). Checked in this order so a coincidental match against a
     * stale/absent id never misclassifies the companion's own display as the game's.
     */
    fun classifyRemoval(
        removedDisplayId: Int,
        companionDisplayId: Int?,
        gameDisplayId: Int?
    ): RemovedDisplayKind = when {
        companionDisplayId == removedDisplayId -> RemovedDisplayKind.COMPANION
        gameDisplayId == removedDisplayId -> RemovedDisplayKind.GAME
        else -> RemovedDisplayKind.UNRELATED
    }

    /**
     * True when losing [kind] is an eject the owner wants paused and asked about, rather than
     * ignored. Never for an unrelated display. The companion's own display only matters while
     * Physical was actually the mode showing it -- Simulated's companion pane has no display of
     * its own to lose, and Off has no companion at all (in practice a caller only ever has a
     * non-null companion display id while Physical is active, but this keeps the function
     * correct on its own terms rather than relying on that invariant). The game's own display
     * always ejects regardless of Mode: Swap Screens is an independent setting.
     */
    fun isEject(kind: RemovedDisplayKind, modeBeforeEvent: DualScreenMode): Boolean = when (kind) {
        RemovedDisplayKind.UNRELATED -> false
        RemovedDisplayKind.GAME -> true
        RemovedDisplayKind.COMPANION -> modeBeforeEvent == DualScreenMode.PHYSICAL
    }

    /** The three choices on the "Second screen disconnected" dialog (RESEARCH.md Â§0 decision 3). */
    enum class EjectChoice { CONTINUE_SIMULATED, CONTINUE_GAME_ONLY, WAIT_FOR_RECONNECT }

    /**
     * The session-only Mode override each choice leaves behind (null = none, the resolver keeps
     * deciding normally). Never persisted -- "Requested mode stays saved as is" -- so this is
     * read by [DualScreenEmulationController] in memory only, cleared by nothing short of a new
     * game session. [EjectChoice.WAIT_FOR_RECONNECT] returns null because it is not a Mode
     * change at all: the session simply stays paused until a later display event resolves
     * Physical again on its own (see [onDisplayAdded]).
     */
    fun sessionOverrideFor(choice: EjectChoice): DualScreenMode? = when (choice) {
        EjectChoice.CONTINUE_SIMULATED -> DualScreenMode.SIMULATED
        EjectChoice.CONTINUE_GAME_ONLY, EjectChoice.WAIT_FOR_RECONNECT -> null
    }

    /** Outcomes of a display appearing/turning on mid-game (RESEARCH.md E4, owner decision 4). */
    enum class DisplayAddedOutcome { SWITCH_TO_PHYSICAL, SHOW_SIMULATED_HINT, NONE }

    /**
     * [requested] and [hasSecondScreenNow] feed the existing [DualScreenModeResolver] exactly as
     * a settings change would; [modeBeforeEvent] is what was actually applied just before this
     * event ([DualScreenEmulationController]'s own `modeApplied`). [DisplayAddedOutcome.SHOW_SIMULATED_HINT]
     * fires only when the user's own explicit Simulated choice is the reason Mode does not
     * switch -- not for Off (which the resolver also leaves alone, for an unrelated reason) and
     * not when nothing changes because Physical was already showing.
     */
    fun onDisplayAdded(
        requested: DualScreenMode?,
        modeBeforeEvent: DualScreenMode,
        hasSecondScreenNow: Boolean
    ): DisplayAddedOutcome {
        val resolved =
            DualScreenModeResolver.resolveActive(requested, hasSecondScreenNow, companionAvailable = true)
        return when {
            resolved == DualScreenMode.PHYSICAL && modeBeforeEvent != DualScreenMode.PHYSICAL ->
                DisplayAddedOutcome.SWITCH_TO_PHYSICAL

            requested == DualScreenMode.SIMULATED && resolved == DualScreenMode.SIMULATED ->
                DisplayAddedOutcome.SHOW_SIMULATED_HINT

            else -> DisplayAddedOutcome.NONE
        }
    }
}
