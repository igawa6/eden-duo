// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import androidx.core.content.edit
import kotlin.math.roundToInt

/** A window's size in pixels, plus the density needed to convert to dp. Android-free. */
data class DualScreenWindow(val widthPx: Int, val heightPx: Int, val densityDpi: Int) {
    val widthDp: Float get() = if (densityDpi <= 0) widthPx.toFloat() else widthPx * 160f / densityDpi
    val heightDp: Float get() = if (densityDpi <= 0) heightPx.toFloat() else heightPx * 160f / densityDpi
    val isLandscape: Boolean get() = widthPx >= heightPx
}

/** Material-style width breakpoints, used only to key persisted geometry (see [DualScreenGeometryStore]). */
enum class DualScreenWindowSizeClass { COMPACT, MEDIUM, EXPANDED }

fun DualScreenWindow.sizeClass(): DualScreenWindowSizeClass = when {
    widthDp < 600f -> DualScreenWindowSizeClass.COMPACT
    widthDp < 840f -> DualScreenWindowSizeClass.MEDIUM
    else -> DualScreenWindowSizeClass.EXPANDED
}

enum class DualScreenHingeOrientation { VERTICAL, HORIZONTAL }

/**
 * The parts of `androidx.window.layout.FoldingFeature` the layout engine needs, decoupled so
 * this module stays plain, unit-testable Kotlin. The Android side (EmulationFragment) builds
 * one from the real `FoldingFeature` it already tracks for the existing tabletop split.
 */
data class DualScreenHinge(
    val isSeparating: Boolean,
    val orientation: DualScreenHingeOrientation,
    val boundsLeft: Int,
    val boundsTop: Int,
    val boundsRight: Int,
    val boundsBottom: Int
)

enum class DualScreenLayoutChoice { AUTOMATIC, STACKED, SIDE_BY_SIDE }

/**
 * The user's Simulated layout choice and the divider's position. [dividerPercent] (1..99) is
 * only used by an *explicit* [DualScreenLayoutChoice] (Stacked or Side by side): the budget
 * share, along the split axis, of the box the game pane is fit into. Automatic ignores it --
 * it aspect-locks and maximises both panes itself (see [DualScreenGeometry.compute]).
 */
data class DualScreenUserLayout(
    val choice: DualScreenLayoutChoice = DualScreenLayoutChoice.AUTOMATIC,
    val dividerPercent: Int = 60
)

/** A rectangle in window pixels. [EMPTY] marks "nothing here" (pane collapsed/not carved out). */
data class DualScreenRect(val x: Int, val y: Int, val w: Int, val h: Int) {
    val isEmpty: Boolean get() = w <= 0 || h <= 0

    companion object {
        val EMPTY = DualScreenRect(0, 0, 0, 0)
    }
}

/** True when ([px], [py]) (window/pane pixels) falls inside [rect]; always false for [DualScreenRect.EMPTY]. */
fun DualScreenRect.contains(px: Int, py: Int): Boolean =
    !isEmpty && px >= x && py >= y && px < x + w && py < y + h

/** [rect] as a 0..1 fraction of a [windowW] x [windowH] window, for `NativeLibrary.setDualScreenInlineLayout`. */
fun DualScreenRect.toFrac(windowW: Int, windowH: Int): FloatArray {
    if (windowW <= 0 || windowH <= 0) {
        return floatArrayOf(0f, 0f, 0f, 0f)
    }
    return floatArrayOf(
        x.toFloat() / windowW,
        y.toFloat() / windowH,
        w.toFloat() / windowW,
        h.toFloat() / windowH
    )
}

/** The engine's output: where the game, the companion and (sometimes) a dedicated controls band sit. */
data class DualScreenLayout(
    val gameRect: DualScreenRect,
    /** [DualScreenRect.EMPTY] when the pane collapsed (too small, see [DualScreenGeometry.MIN_PANE_SHORT_EDGE_DP]) or was never carved out (the no-controller tabletop case). Reachable either way through the companion toggle. */
    val companionRect: DualScreenRect,
    /** Non-empty only for the no-controller Automatic portrait/stacked 3-band case and the no-controller tabletop case; empty everywhere else, meaning the input overlay sits over [gameRect] instead, as it already does today outside dual screen. */
    val controlsRect: DualScreenRect
)

/** An aspect ratio as a whole-number fraction (e.g. 16:9). `null` elsewhere means "unlocked". */
data class DualScreenAspect(val w: Int, val h: Int)

/**
 * Eden Duo Stage 2: the Simulated layout engine (unit tested with a device table). Pure Kotlin,
 * no Android dependency.
 *
 * Owner decision (revises this stage's original spec, which had no aspect-lock and a plain
 * Fit-or-collapse rule): both panes are **aspect-locked**, like a DS/3DS emulator's two screens.
 * The game pane's aspect is [gameAspect] -- Eden's own `aspect_ratio` setting (16:9/4:3/21:9/
 * 16:10), or `null` for Stretch, meaning the game pane is not aspect-locked and simply takes
 * whatever box it is given (see [aspectForRendererSetting]). The companion pane's aspect is
 * always the project's default companion canvas, 1240x1080 (~8:7 for every companion shipped so
 * far; a runtime 25 companion with `canvas_alt` would follow that canvas's own aspect instead,
 * out of this stage's scope). Automatic sizes both panes at their own aspect, as large as
 * possible with no bars inside either one, shrinking both together (never changing either
 * aspect) only if their natural full-extent sizes do not both fit; any window space left over is
 * background, same as a letterboxed single screen already is outside dual screen. A manual
 * [DualScreenLayoutChoice] (not Automatic) instead budgets the split axis by
 * [DualScreenUserLayout.dividerPercent] and Fits each pane (centred, possibly with bars) inside
 * its own budgeted box -- Fit is kept in the native draw/touch path as that safety net, not as a
 * normal-use outcome.
 *
 * Rules (RESEARCH.md §4.1/§4.2/§4.4, simplified to this stage's exact spec):
 *  - An explicit choice (not Automatic) ignores any hinge.
 *  - Automatic: a vertical separating hinge (book posture) splits game/companion at the hinge --
 *    the hinge's own position is a physical constraint, not an aspect choice, so this one case
 *    is not aspect-locked. A horizontal hinge (tabletop) gives the companion the whole lower
 *    half only with a controller connected, else today's existing game/controls split with the
 *    companion not carved out (still reachable through the toggle). Otherwise landscape is side
 *    by side, portrait is stacked, both aspect-locked and maximised as above.
 *  - Stacked without a controller is three bands (game / companion / controls): both panes at
 *    their natural (full window width, un-shrunk) aspect height, the remainder to controls --
 *    RESEARCH.md I1's own phone illustration. With a controller, stacked is the aspect-locked
 *    two-band maximise, just vertical instead of horizontal.
 *  - Whatever the above computes, a companion pane whose short edge is under
 *    [MIN_PANE_SHORT_EDGE_DP] collapses: the game absorbs its space, companion stays reachable
 *    through the toggle. With both panes aspect-locked the reported rect already has no internal
 *    bars, so its own short edge is the relevant one directly (no separate Fit step needed here).
 */
object DualScreenGeometry {
    /** The project's default companion canvas (CLAUDE.md "default new companion work"). */
    private val CANVAS_ASPECT = DualScreenAspect(1240, 1080)

    /** Only for budgeting the no-controller 3-band split when the game itself is unlocked
     *  (Stretch) -- it only ever changes how large the controls band ends up, never the game's
     *  own rendering, which still uses whatever box it is given. */
    private val DEFAULT_GAME_ASPECT = DualScreenAspect(16, 9)

    /**
     * RESEARCH.md §4.1 F4 / §4.2 R4, lowered in Stage 4 (owner decision 5: landscape default is
     * side by side) -- 320 dp collapsed the common case the owner wants visible: a 16:9 game
     * beside the 8:7 companion canvas needs 3160 px of full-height width, so on a phone
     * (2400x1080) or the Thor's own top screen (1920x1080) Automatic shrinks both panes to fit
     * and the companion's short edge lands at ~312 dp / ~262 dp respectively (see
     * DualScreenGeometryTest) -- just under the old floor. 200 dp clears both while still
     * collapsing a genuinely cramped manual-divider budget or hinge split.
     */
    const val MIN_PANE_SHORT_EDGE_DP = 200f

    /**
     * Stage 4: a second, independent floor on the *window* itself. A single per-pane floor low
     * enough to keep the phone/Thor case above visible cannot, by itself, also collapse a
     * flip-phone cover screen (904x2316 in the device table): that window's own companion pane
     * lands at ~300 dp -- above a 200 dp per-pane floor -- even though the window carrying it
     * (344 dp on its short edge) is too small to read anything on. 360 dp sits strictly between
     * every cover-screen-class window in the table (344 dp) and every normal phone/Thor/tablet
     * one (411-800 dp), so it catches the former without touching the latter.
     */
    const val MIN_WINDOW_SHORT_EDGE_DP = 360f

    /** Eden's `aspect_ratio` IntSetting (0 16:9, 1 4:3, 2 21:9, 3 16:10, 4 Stretch) as a lock. */
    fun aspectForRendererSetting(value: Int): DualScreenAspect? = when (value) {
        0 -> DualScreenAspect(16, 9)
        1 -> DualScreenAspect(4, 3)
        2 -> DualScreenAspect(21, 9)
        3 -> DualScreenAspect(16, 10)
        else -> null // 4 = Stretch (or anything unrecognised): the pane as given.
    }

    fun compute(
        window: DualScreenWindow,
        hinge: DualScreenHinge?,
        controllerConnected: Boolean,
        userLayout: DualScreenUserLayout,
        gameAspect: DualScreenAspect?
    ): DualScreenLayout {
        val divider = userLayout.dividerPercent.coerceIn(1, 99) / 100f
        val raw = when {
            userLayout.choice == DualScreenLayoutChoice.SIDE_BY_SIDE ->
                sideBySide(window, divider, gameAspect, automatic = false)

            userLayout.choice == DualScreenLayoutChoice.STACKED ->
                stacked(window, divider, controllerConnected, gameAspect, automatic = false)

            hinge != null && hinge.isSeparating &&
                hinge.orientation == DualScreenHingeOrientation.VERTICAL -> hingeSplit(window, hinge)

            hinge != null && hinge.isSeparating &&
                hinge.orientation == DualScreenHingeOrientation.HORIZONTAL ->
                tabletop(window, hinge, controllerConnected)

            window.isLandscape -> sideBySide(window, divider, gameAspect, automatic = true)
            else -> stacked(window, divider, controllerConnected, gameAspect, automatic = true)
        }
        return collapseIfTooSmall(raw, window)
    }

    // ---- side by side (landscape default) ----

    private fun sideBySide(
        window: DualScreenWindow,
        divider: Float,
        gameAspect: DualScreenAspect?,
        automatic: Boolean
    ): DualScreenLayout {
        if (!automatic) {
            val gameBudgetW = (window.widthPx * divider).roundToInt().coerceIn(0, window.widthPx)
            val game = fitWithinBox(0, 0, gameBudgetW, window.heightPx, gameAspect)
            val companion =
                fitWithinBox(gameBudgetW, 0, window.widthPx - gameBudgetW, window.heightPx, CANVAS_ASPECT)
            return DualScreenLayout(game, companion, DualScreenRect.EMPTY)
        }
        val h = window.heightPx
        val companionW0 = h.toDouble() * CANVAS_ASPECT.w / CANVAS_ASPECT.h
        if (gameAspect == null) {
            // Stretch: the game simply takes whatever is left of the companion's own full-height
            // natural width -- "the pane as given".
            val companionW = companionW0.roundToInt().coerceIn(0, window.widthPx)
            val gameW = window.widthPx - companionW
            return DualScreenLayout(
                DualScreenRect(0, 0, gameW, h),
                DualScreenRect(gameW, 0, companionW, h),
                DualScreenRect.EMPTY
            )
        }
        val gameW0 = h.toDouble() * gameAspect.w / gameAspect.h
        val total0 = gameW0 + companionW0
        val scale = if (total0 > window.widthPx) window.widthPx / total0 else 1.0
        val gameW = (gameW0 * scale).roundToInt().coerceAtLeast(1)
        val companionW = (companionW0 * scale).roundToInt().coerceAtLeast(1)
        val scaledH = (h * scale).roundToInt().coerceAtLeast(1)
        val top = (window.heightPx - scaledH) / 2
        return DualScreenLayout(
            DualScreenRect(0, top, gameW, scaledH),
            DualScreenRect(gameW, top, companionW, scaledH),
            DualScreenRect.EMPTY
        )
    }

    // ---- stacked (portrait default) ----

    private fun stacked(
        window: DualScreenWindow,
        divider: Float,
        controllerConnected: Boolean,
        gameAspect: DualScreenAspect?,
        automatic: Boolean
    ): DualScreenLayout {
        if (!automatic) {
            val gameBudgetH = (window.heightPx * divider).roundToInt().coerceIn(0, window.heightPx)
            val game = fitWithinBox(0, 0, window.widthPx, gameBudgetH, gameAspect)
            val companion =
                fitWithinBox(0, gameBudgetH, window.widthPx, window.heightPx - gameBudgetH, CANVAS_ASPECT)
            return DualScreenLayout(game, companion, DualScreenRect.EMPTY)
        }
        val w = window.widthPx
        val companionH0 = w.toDouble() * CANVAS_ASPECT.h / CANVAS_ASPECT.w
        if (!controllerConnected) {
            // Three bands: both panes at their natural (full-width) aspect height; the
            // remainder to controls. Not scaled-to-fit (see the class doc): realistic windows
            // always leave room, and a too-short one simply collapses the companion below.
            val budgetAspect = gameAspect ?: DEFAULT_GAME_ASPECT
            val gameH0 = w.toDouble() * budgetAspect.h / budgetAspect.w
            val gameH = gameH0.roundToInt().coerceIn(0, window.heightPx)
            val companionH = companionH0.roundToInt().coerceIn(0, window.heightPx - gameH)
            val controlsH = (window.heightPx - gameH - companionH).coerceAtLeast(0)
            return DualScreenLayout(
                DualScreenRect(0, 0, w, gameH),
                DualScreenRect(0, gameH, w, companionH),
                DualScreenRect(0, gameH + companionH, w, controlsH)
            )
        }
        if (gameAspect == null) {
            val companionH = companionH0.roundToInt().coerceIn(0, window.heightPx)
            val gameH = window.heightPx - companionH
            return DualScreenLayout(
                DualScreenRect(0, 0, w, gameH),
                DualScreenRect(0, gameH, w, companionH),
                DualScreenRect.EMPTY
            )
        }
        val gameH0 = w.toDouble() * gameAspect.h / gameAspect.w
        val total0 = gameH0 + companionH0
        val scale = if (total0 > window.heightPx) window.heightPx / total0 else 1.0
        val gameH = (gameH0 * scale).roundToInt().coerceAtLeast(1)
        val companionH = (companionH0 * scale).roundToInt().coerceAtLeast(1)
        val scaledW = (w * scale).roundToInt().coerceAtLeast(1)
        val left = (window.widthPx - scaledW) / 2
        return DualScreenLayout(
            DualScreenRect(left, 0, scaledW, gameH),
            DualScreenRect(left, gameH, scaledW, companionH),
            DualScreenRect.EMPTY
        )
    }

    // ---- folds: a physical hinge decides the split point, not either pane's aspect ----

    private fun hingeSplit(window: DualScreenWindow, hinge: DualScreenHinge): DualScreenLayout {
        val splitLeft = hinge.boundsLeft.coerceIn(0, window.widthPx)
        val splitRight = hinge.boundsRight.coerceIn(splitLeft, window.widthPx)
        val game = DualScreenRect(0, 0, splitLeft, window.heightPx)
        val companion = DualScreenRect(splitRight, 0, window.widthPx - splitRight, window.heightPx)
        return DualScreenLayout(game, companion, DualScreenRect.EMPTY)
    }

    private fun tabletop(
        window: DualScreenWindow,
        hinge: DualScreenHinge,
        controllerConnected: Boolean
    ): DualScreenLayout {
        val splitTop = hinge.boundsTop.coerceIn(0, window.heightPx)
        val splitBottom = hinge.boundsBottom.coerceIn(splitTop, window.heightPx)
        val game = DualScreenRect(0, 0, window.widthPx, splitTop)
        if (controllerConnected) {
            val companion =
                DualScreenRect(0, splitBottom, window.widthPx, window.heightPx - splitBottom)
            return DualScreenLayout(game, companion, DualScreenRect.EMPTY)
        }
        // No controller: today's existing foldable split (game above the hinge, controls below).
        // The companion is not carved out here -- reachable only through the companion toggle.
        val controls = DualScreenRect(0, splitBottom, window.widthPx, window.heightPx - splitBottom)
        return DualScreenLayout(game, DualScreenRect.EMPTY, controls)
    }

    // ---- shared helpers ----

    /** The largest [aspect]-shaped rect, with no internal bars, centred within a box; the whole
     *  box when [aspect] is null (Stretch/unlocked) or the box has no area. Used only by an
     *  explicit (non-Automatic) layout choice's manual divider budget. */
    private fun fitWithinBox(
        boxX: Int,
        boxY: Int,
        boxW: Int,
        boxH: Int,
        aspect: DualScreenAspect?
    ): DualScreenRect {
        if (aspect == null || boxW <= 0 || boxH <= 0) {
            return DualScreenRect(boxX, boxY, boxW.coerceAtLeast(0), boxH.coerceAtLeast(0))
        }
        val boxCross = boxW.toLong() * aspect.h
        val aspectCross = boxH.toLong() * aspect.w
        return if (boxCross >= aspectCross) {
            val w = ((aspectCross + aspect.h / 2) / aspect.h).toInt().coerceIn(1, boxW)
            DualScreenRect(boxX + (boxW - w) / 2, boxY, w, boxH)
        } else {
            val h = ((boxCross + aspect.w / 2) / aspect.w).toInt().coerceIn(1, boxH)
            DualScreenRect(boxX, boxY + (boxH - h) / 2, boxW, h)
        }
    }

    private fun collapseIfTooSmall(
        layout: DualScreenLayout,
        window: DualScreenWindow
    ): DualScreenLayout {
        if (layout.companionRect.isEmpty) {
            return layout
        }
        if (minOf(window.widthDp, window.heightDp) < MIN_WINDOW_SHORT_EDGE_DP) {
            return collapse(layout)
        }
        val density = if (window.densityDpi <= 0) 1f else window.densityDpi / 160f
        val shortEdgeDp = minOf(layout.companionRect.w, layout.companionRect.h) / density
        if (shortEdgeDp >= MIN_PANE_SHORT_EDGE_DP) {
            return layout
        }
        return collapse(layout)
    }

    private fun collapse(layout: DualScreenLayout): DualScreenLayout = layout.copy(
        gameRect = mergeAdjacent(layout.gameRect, layout.companionRect),
        companionRect = DualScreenRect.EMPTY
    )

    /** The bounding box of two rects that share an edge (every branch above produces exactly that). */
    private fun mergeAdjacent(a: DualScreenRect, b: DualScreenRect): DualScreenRect {
        val x0 = minOf(a.x, b.x)
        val y0 = minOf(a.y, b.y)
        val x1 = maxOf(a.x + a.w, b.x + b.w)
        val y1 = maxOf(a.y + a.h, b.y + b.h)
        return DualScreenRect(x0, y0, x1 - x0, y1 - y0)
    }
}

/**
 * Persists the user's divider drag per (orientation x window size class) -- RESEARCH.md's
 * melonDS-pattern cascade is not attempted (Stage 2's exact spec drops the hinge dimension and
 * the similarity fallback PLAN.md proposed); an exact bucket miss falls back to [default].
 */
object DualScreenGeometryStore {
    private const val PREFS = "eden_duo_dual_screen_geometry"

    private fun prefs(context: Context) = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun key(window: DualScreenWindow): String {
        val orientation = if (window.isLandscape) "landscape" else "portrait"
        return "divider_${orientation}_${window.sizeClass()}"
    }

    fun dividerPercent(context: Context, window: DualScreenWindow, default: Int = 60): Int =
        prefs(context).getInt(key(window), default)

    fun setDividerPercent(context: Context, window: DualScreenWindow, percent: Int) {
        prefs(context).edit { putInt(key(window), percent.coerceIn(1, 99)) }
    }
}
