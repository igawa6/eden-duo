// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

/** What the second-screen window of the running game is, as the companion toggle sees it. */
enum class CompanionWindow {
    /** A window is wanted: a loaded companion (or its refusal page) or the Icon / Black page. */
    SHOWN_WHEN_WANTED,

    /** No companion and No Companion Off or App: the game put up no window. */
    NONE_NO_COMPANION,

    /** As [NONE_NO_COMPANION], because the game's companion package is turned off in Add-ons. */
    NONE_ADDON_OFF
}

/** The result of one companion toggle. */
enum class CompanionToggleOutcome {
    HIDDEN,
    SHOWN,

    /** Within the debounce time of the previous toggle: nothing happens, no message. */
    IGNORED,
    NO_COMPANION,
    ADDON_OFF,
    NO_SECOND_SCREEN
}

/**
 * Eden Duo: the Android-free part of the in-game companion toggle (unit tested). One per game
 * session, in memory only: a fresh boot always starts with the companion shown.
 *
 * Decision table -- the toggle never loads a companion that is not running:
 *
 * | Window                                 | Toggle                                       |
 * |----------------------------------------|----------------------------------------------|
 * | companion loaded / refusal / Icon page | hide: close the window; show: open it again  |
 * | none: no companion, Off or App         | nothing; "No companion for this game"        |
 * | none: companion turned off in Add-ons  | nothing; "turned off ... in Add-ons"         |
 * | no second display                      | nothing; "No second screen" (state kept)     |
 *
 * While [hiddenByUser] is true nothing shows the window automatically ([allowsAutoShow]): only
 * another toggle brings it back.
 */
class CompanionToggleState {
    companion object {
        /** Toggles closer together than this are one toggle. */
        const val DEBOUNCE_MS = 500L

        fun decide(
            window: CompanionWindow,
            hasSecondScreen: Boolean,
            hiddenByUser: Boolean
        ): CompanionToggleOutcome = when {
            window == CompanionWindow.NONE_ADDON_OFF -> CompanionToggleOutcome.ADDON_OFF
            window == CompanionWindow.NONE_NO_COMPANION -> CompanionToggleOutcome.NO_COMPANION
            !hasSecondScreen -> CompanionToggleOutcome.NO_SECOND_SCREEN
            hiddenByUser -> CompanionToggleOutcome.SHOWN
            else -> CompanionToggleOutcome.HIDDEN
        }
    }

    var hiddenByUser = false
        private set

    private var lastToggleAt: Long? = null

    /** Whether display events, retries, onStart and No Companion changes may show the window. */
    fun allowsAutoShow(): Boolean = !hiddenByUser

    /** One toggle at [nowMs] (a monotonic clock); applies the outcome to [hiddenByUser]. */
    fun toggle(nowMs: Long, window: CompanionWindow, hasSecondScreen: Boolean): CompanionToggleOutcome {
        val last = lastToggleAt
        if (last != null && nowMs - last < DEBOUNCE_MS) return CompanionToggleOutcome.IGNORED
        lastToggleAt = nowMs
        val outcome = decide(window, hasSecondScreen, hiddenByUser)
        when (outcome) {
            CompanionToggleOutcome.HIDDEN -> hiddenByUser = true
            CompanionToggleOutcome.SHOWN -> hiddenByUser = false
            else -> Unit
        }
        return outcome
    }
}
