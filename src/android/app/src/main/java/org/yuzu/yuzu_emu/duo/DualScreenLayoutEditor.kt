// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import androidx.core.content.edit
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt

/** Which corner handle of a rect is being dragged to resize it. */
enum class DualScreenEditorHandle { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT }

/** A rect as 0..1 fractions of the window it was captured in -- what [DualScreenCustomLayoutStore]
 *  persists, and what [DualScreenLayoutEditor.refitOnWindowChange] re-applies to a new window. */
data class DualScreenNormRect(val x: Float, val y: Float, val w: Float, val h: Float)

/** The editor's saved result: both panes, normalised. */
data class DualScreenCustomLayout(val gameFrac: DualScreenNormRect, val companionFrac: DualScreenNormRect)

/** [rect] as a fraction of [window]; all-zero for a zero-sized window. */
fun DualScreenRect.toNormRect(window: DualScreenWindow): DualScreenNormRect {
    if (window.widthPx <= 0 || window.heightPx <= 0) return DualScreenNormRect(0f, 0f, 0f, 0f)
    return DualScreenNormRect(
        x.toFloat() / window.widthPx,
        y.toFloat() / window.heightPx,
        w.toFloat() / window.widthPx,
        h.toFloat() / window.heightPx
    )
}

/** The inverse of [toNormRect]: [this] applied to [window]'s own pixel size. */
fun DualScreenNormRect.denormalize(window: DualScreenWindow): DualScreenRect = DualScreenRect(
    (x * window.widthPx).roundToInt(),
    (y * window.heightPx).roundToInt(),
    (w * window.widthPx).roundToInt(),
    (h * window.heightPx).roundToInt()
)

/**
 * Eden Duo 1.4.0: pure geometry for the freeform "Edit dual screen layout" editor (owner request
 * 2026-11-11, supersedes the Divider position slider -- see LAYOUT-EDITOR-SPEC.md). Every
 * function here is Android-free, Kotlin-only, and shares [DualScreenRect]/[DualScreenAspect]/
 * [DualScreenWindow] with [DualScreenGeometry] so its CUSTOM layout branch and this editor speak
 * one vocabulary; the UI ([DualScreenLayoutEditorView]/[DualScreenFragmentHost]) is a thin
 * touch-event wrapper that calls into these one gesture at a time and nothing else.
 *
 * Shared invariant every function here keeps or restores: both rects stay inside whatever box
 * they are given (the window for the game, a narrower safe-drawing box for the companion) and
 * never overlap each other. A gesture that would break either rule is *clamped*, not rejected
 * (LAYOUT-EDITOR-SPEC.md item 2): [clampToBox] then [resolveOverlap], applied in that order after
 * every move or resize, is what the UI calls to keep the invariant true on every touch event.
 */
object DualScreenLayoutEditor {
    /**
     * Spec: "companion short edge >= 160 dp". Lower than Automatic's own collapse floor
     * ([DualScreenGeometry.MIN_PANE_SHORT_EDGE_DP], 200 dp) on purpose: that floor exists to stop
     * an *unsupervised* shrink-to-fit from landing somewhere unreadable, while this one is a live
     * drag the user is watching -- it only needs to stop the rect shrinking to a size with no
     * room left to grab its own resize handles again.
     */
    const val MIN_COMPANION_SHORT_EDGE_DP = 160f

    /**
     * Spec: "game >= 25% of window width". A fraction rather than a dp floor, since the game
     * pane's usable minimum scales with the window (half a tiny phone window is already small;
     * half a tablet window is still plenty), unlike the companion's text/icons.
     */
    const val MIN_GAME_WIDTH_FRACTION = 0.25f

    /** Spec: "~8 dp" snap threshold. */
    const val SNAP_THRESHOLD_DP = 8f

    fun dpToPx(dp: Float, window: DualScreenWindow): Float =
        if (window.densityDpi <= 0) dp else dp * window.densityDpi / 160f

    // ---- move ----

    /** Translates [rect] by ([dx],[dy]) in window pixels; a move never changes aspect or size. */
    fun move(rect: DualScreenRect, dx: Int, dy: Int): DualScreenRect =
        rect.copy(x = rect.x + dx, y = rect.y + dy)

    // ---- aspect-locked resize ----

    /** [w] x [h], adjusted to [aspect] exactly (unchanged when null) and never smaller than
     *  [minW] x [minH] -- grown by scaling both dimensions together, never by cropping one. */
    private fun lockToAspect(w: Int, h: Int, aspect: DualScreenAspect?, minW: Int, minH: Int): Pair<Int, Int> {
        if (aspect == null || aspect.w <= 0 || aspect.h <= 0) {
            return w.coerceAtLeast(minW) to h.coerceAtLeast(minH)
        }
        // The minimums expressed along the aspect's own diagonal, so a floor on one dimension
        // cannot leave the other below its own floor once aspect is restored.
        val floorWFromH = if (minH > 0) (minH.toLong() * aspect.w / aspect.h).toInt() else 0
        val floorHFromW = if (minW > 0) (minW.toLong() * aspect.h / aspect.w).toInt() else 0
        val floorW = max(minW, floorWFromH)
        val floorH = max(minH, floorHFromW)
        val scale = max(
            max(w.toFloat() / aspect.w, h.toFloat() / aspect.h),
            max(floorW.toFloat() / aspect.w, floorH.toFloat() / aspect.h)
        )
        return (scale * aspect.w).roundToInt() to (scale * aspect.h).roundToInt()
    }

    /**
     * Resizes [rect] by dragging [handle] by ([dx],[dy]) window pixels from [rect]'s own start
     * position (not an incremental per-frame delta -- the caller tracks the gesture's start rect
     * and total delta, same shape as InputOverlay's own button-drag tracking). The opposite
     * corner is the fixed anchor; the new size covers at least as much as the drag in both
     * directions (so the dragged corner never trails behind the finger), locked to [aspect] (null
     * = free, e.g. the game pane under Stretch) and never below [minW] x [minH].
     */
    fun resizeFromHandle(
        rect: DualScreenRect,
        handle: DualScreenEditorHandle,
        dx: Int,
        dy: Int,
        aspect: DualScreenAspect?,
        minW: Int,
        minH: Int
    ): DualScreenRect {
        val growLeft = handle == DualScreenEditorHandle.TOP_LEFT || handle == DualScreenEditorHandle.BOTTOM_LEFT
        val growUp = handle == DualScreenEditorHandle.TOP_LEFT || handle == DualScreenEditorHandle.TOP_RIGHT
        val anchorX = if (growLeft) rect.x + rect.w else rect.x
        val anchorY = if (growUp) rect.y + rect.h else rect.y
        val draggedX = (if (growLeft) rect.x else rect.x + rect.w) + dx
        val draggedY = (if (growUp) rect.y else rect.y + rect.h) + dy
        val (w, h) = lockToAspect(abs(draggedX - anchorX), abs(draggedY - anchorY), aspect, minW, minH)
        val newX = if (growLeft) anchorX - w else anchorX
        val newY = if (growUp) anchorY - h else anchorY
        return DualScreenRect(newX, newY, w, h)
    }

    /**
     * Resizes [rect] by a two-finger pinch's [scaleFactor] (new finger span / old span, as
     * `ScaleGestureDetector` reports it -- the UI applies this once per callback, incrementally),
     * anchored at [rect]'s own centre, locked to [aspect] and never below [minW] x [minH].
     */
    fun resizeFromPinch(
        rect: DualScreenRect,
        scaleFactor: Float,
        aspect: DualScreenAspect?,
        minW: Int,
        minH: Int
    ): DualScreenRect {
        if (scaleFactor <= 0f || !scaleFactor.isFinite()) return rect
        val cx = rect.x + rect.w / 2
        val cy = rect.y + rect.h / 2
        val (w, h) = lockToAspect(
            (rect.w * scaleFactor).roundToInt(), (rect.h * scaleFactor).roundToInt(), aspect, minW, minH
        )
        return DualScreenRect(cx - w / 2, cy - h / 2, w, h)
    }

    // ---- clamp / no-overlap ----

    /** [rect], translated and shrunk only as much as needed to fit entirely inside [box]. A
     *  shrink scales both dimensions together, so an aspect-locked rect stays (to the pixel)
     *  aspect-locked. */
    fun clampToBox(rect: DualScreenRect, box: DualScreenRect): DualScreenRect {
        var w = rect.w.coerceIn(0, box.w)
        var h = rect.h.coerceIn(0, box.h)
        if (rect.w > 0 && rect.h > 0 && (rect.w > box.w || rect.h > box.h)) {
            val scale = minOf(box.w.toDouble() / rect.w, box.h.toDouble() / rect.h)
            w = (rect.w * scale).toInt().coerceIn(0, box.w)
            h = (rect.h * scale).toInt().coerceIn(0, box.h)
        }
        val x = rect.x.coerceIn(box.x, box.x + box.w - w)
        val y = rect.y.coerceIn(box.y, box.y + box.h - h)
        return DualScreenRect(x, y, w, h)
    }

    /** True when [rect] lies entirely inside [box]. */
    private fun isInside(rect: DualScreenRect, box: DualScreenRect): Boolean =
        rect.x >= box.x && rect.y >= box.y &&
            rect.x + rect.w <= box.x + box.w && rect.y + rect.h <= box.y + box.h

    private fun overlaps(a: DualScreenRect, b: DualScreenRect): Boolean {
        if (a.isEmpty || b.isEmpty) return false
        val overlapX = minOf(a.x + a.w, b.x + b.w) - maxOf(a.x, b.x)
        val overlapY = minOf(a.y + a.h, b.y + b.h) - maxOf(a.y, b.y)
        return overlapX > 0 && overlapY > 0
    }

    /** True when [rect] is inside [box] and does not overlap [other] -- the editor's invariant. */
    fun isValidPlacement(rect: DualScreenRect, box: DualScreenRect, other: DualScreenRect): Boolean =
        isInside(rect, box) && !overlaps(rect, other)

    /**
     * [moving] placed inside [box] without overlapping [other]: as it is when that already
     * holds, else [resolveOverlap]'s own push, else the push along the other axis or the other
     * direction (shortest first). Null when no translation-only push satisfies both rules (the
     * caller then keeps its last valid rect, or picks its own fallback).
     */
    fun separate(moving: DualScreenRect, other: DualScreenRect, box: DualScreenRect): DualScreenRect? {
        if (isValidPlacement(moving, box, other)) return moving
        val preferred = resolveOverlap(moving, other)
        if (isValidPlacement(preferred, box, other)) return preferred
        if (moving.isEmpty || other.isEmpty) return null
        val pushes = listOf(
            moving.copy(x = other.x - moving.w),
            moving.copy(x = other.x + other.w),
            moving.copy(y = other.y - moving.h),
            moving.copy(y = other.y + other.h)
        ).sortedBy { abs(it.x - moving.x) + abs(it.y - moving.y) }
        return pushes.firstOrNull { isValidPlacement(it, box, other) }
    }

    /**
     * The full constraint pass for one gesture step of the editor: [candidate] clamped into
     * [box], snapped against [snapWindow]'s edges and [other] when given (a move; a resize passes
     * null and honours the finger exactly), re-clamped, then separated from [other]
     * ([separate]). When no placement keeps it inside [box] and clear of [other], [lastValid]
     * (the rect before this step) is kept, so a drag can never push a rect out of the window.
     */
    fun constrain(
        candidate: DualScreenRect,
        box: DualScreenRect,
        other: DualScreenRect,
        lastValid: DualScreenRect,
        snapWindow: DualScreenWindow? = null
    ): DualScreenRect {
        var rect = clampToBox(candidate, box)
        if (snapWindow != null) {
            rect = clampToBox(snap(rect, snapWindow, other), box)
        }
        return separate(rect, other, box) ?: lastValid
    }

    /**
     * [moving], pushed (translated only, never resized) just far enough along whichever axis
     * needs the smaller shift to stop overlapping [other] -- "stop at the other rect's edge"
     * (spec item 2), not a rejection. Returns [moving] unchanged when they do not overlap.
     */
    fun resolveOverlap(moving: DualScreenRect, other: DualScreenRect): DualScreenRect {
        if (moving.isEmpty || other.isEmpty) return moving
        val overlapX = minOf(moving.x + moving.w, other.x + other.w) - maxOf(moving.x, other.x)
        val overlapY = minOf(moving.y + moving.h, other.y + other.h) - maxOf(moving.y, other.y)
        if (overlapX <= 0 || overlapY <= 0) return moving
        return if (overlapX < overlapY) {
            if (moving.x + moving.w / 2 < other.x + other.w / 2) {
                moving.copy(x = other.x - moving.w)
            } else {
                moving.copy(x = other.x + other.w)
            }
        } else {
            if (moving.y + moving.h / 2 < other.y + other.h / 2) {
                moving.copy(y = other.y - moving.h)
            } else {
                moving.copy(y = other.y + other.h)
            }
        }
    }

    // ---- snapping ----

    /** The snapped position for a [size]-long span currently at [pos] along one axis: window
     *  edges, the window's own centre, flush against the other rect's near/far edge on this axis,
     *  and the position that splits the leftover room evenly between this rect and the other
     *  (equal-gap centring) -- whichever candidate is closest, within [thresholdPx]; [pos]
     *  unchanged when none are close enough. */
    private fun snapAxis(pos: Int, size: Int, windowSize: Int, otherPos: Int, otherSize: Int, thresholdPx: Int): Int {
        val candidates = intArrayOf(
            0,
            windowSize - size,
            (windowSize - size) / 2,
            otherPos - size,
            otherPos + otherSize,
            (windowSize - size - otherSize) / 2
        )
        var best = pos
        var bestDist = thresholdPx + 1
        for (c in candidates) {
            val d = abs(c - pos)
            if (d <= thresholdPx && d < bestDist) {
                best = c
                bestDist = d
            }
        }
        return best
    }

    /**
     * Snaps a just-moved [rect] against [window]'s edges/centre and [other]'s edges/equal-gap
     * centring (spec item 2), independently on each axis, within [SNAP_THRESHOLD_DP].
     */
    fun snap(rect: DualScreenRect, window: DualScreenWindow, other: DualScreenRect): DualScreenRect {
        val thresholdPx = dpToPx(SNAP_THRESHOLD_DP, window).roundToInt().coerceAtLeast(1)
        val x = snapAxis(rect.x, rect.w, window.widthPx, other.x, other.w, thresholdPx)
        val y = snapAxis(rect.y, rect.h, window.heightPx, other.y, other.h, thresholdPx)
        return rect.copy(x = x, y = y)
    }

    // ---- swap ----

    /**
     * Exchanges the two panes' positions, each re-fit to its own locked aspect inside the box the
     * *other* pane used to occupy (spec: "exchange positions, re-fit each to its aspect"), then
     * clamped to [window] and separated if the re-fit happened to overlap.
     */
    fun swap(
        window: DualScreenWindow,
        game: DualScreenRect,
        companion: DualScreenRect,
        gameAspect: DualScreenAspect?,
        companionAspect: DualScreenAspect
    ): Pair<DualScreenRect, DualScreenRect> {
        val windowBox = DualScreenRect(0, 0, window.widthPx, window.heightPx)
        val newGame = clampToBox(
            DualScreenGeometry.fitWithinBox(companion.x, companion.y, companion.w, companion.h, gameAspect),
            windowBox
        )
        val newCompanion = clampToBox(
            DualScreenGeometry.fitWithinBox(game.x, game.y, game.w, game.h, companionAspect),
            windowBox
        )
        // A swap that cannot be placed inside the window without overlap is not applied.
        val separated = separate(newCompanion, newGame, windowBox) ?: return game to companion
        return newGame to separated
    }

    // ---- re-fit a stored (fractional) layout onto a (possibly different) window ----

    /**
     * Applying a stored Custom layout to a window (spec item 3): keep the saved fractions, re-fit
     * each rect to its own locked aspect inside its fractional box ([DualScreenGeometry
     * .fitWithinBox] -- the largest aspect-shaped rect, no bars, centred in that box), clamp both
     * to the window and re-run the no-overlap pass. Used both by [DualScreenGeometry.compute]'s
     * CUSTOM branch (every frame, same as every other layout choice) and by the editor itself
     * when it opens on a window whose size differs from the one the layout was captured on.
     */
    fun refitOnWindowChange(
        custom: DualScreenCustomLayout,
        window: DualScreenWindow,
        gameAspect: DualScreenAspect?,
        companionAspect: DualScreenAspect
    ): DualScreenLayout {
        val windowBox = DualScreenRect(0, 0, window.widthPx, window.heightPx)
        val gameBox = custom.gameFrac.denormalize(window)
        val companionBox = custom.companionFrac.denormalize(window)
        val game = clampToBox(
            DualScreenGeometry.fitWithinBox(gameBox.x, gameBox.y, gameBox.w, gameBox.h, gameAspect),
            windowBox
        )
        val companion = clampToBox(
            DualScreenGeometry.fitWithinBox(
                companionBox.x, companionBox.y, companionBox.w, companionBox.h, companionAspect
            ),
            windowBox
        )
        val placed = separate(companion, game, windowBox) ?: fitBeside(game, window, companionAspect)
        return DualScreenLayout(game, placed, DualScreenRect.EMPTY)
    }

    /**
     * The largest [aspect]-shaped rect in whichever free strip of [window] beside [game] (left,
     * right, above or below it) holds the biggest one -- [refitOnWindowChange]'s fallback when
     * the saved companion cannot be pushed clear of the game inside the window. EMPTY when the
     * game leaves no strip at all.
     */
    private fun fitBeside(game: DualScreenRect, window: DualScreenWindow, aspect: DualScreenAspect): DualScreenRect {
        val w = window.widthPx
        val h = window.heightPx
        val right = game.x + game.w
        val bottom = game.y + game.h
        return listOf(
            DualScreenGeometry.fitWithinBox(0, 0, game.x, h, aspect),
            DualScreenGeometry.fitWithinBox(right, 0, w - right, h, aspect),
            DualScreenGeometry.fitWithinBox(0, 0, w, game.y, aspect),
            DualScreenGeometry.fitWithinBox(0, bottom, w, h - bottom, aspect)
        ).filterNot { it.isEmpty }.maxByOrNull { it.w.toLong() * it.h } ?: DualScreenRect.EMPTY
    }
}

/**
 * Eden Duo 1.4.0: persists the in-game layout editor's result as window fractions, per
 * (landscape/portrait x optional per-title id) -- the same "<key>@<16-digit title id>", absent =
 * inherit-global shape [org.yuzu.yuzu_emu.utils.NoCompanion]/[DualScreenModeSetting] already use
 * ([SecondScreenPerGame.key]), sharing their own preferences file rather than opening a new one.
 * A simpler shape than [DualScreenGeometryStore]'s window-size-class buckets (the old divider):
 * Custom is a deliberate, named per-title choice far more often than the divider ever was, and
 * whatever window it next meets is handled by [DualScreenLayoutEditor.refitOnWindowChange]
 * regardless of its size, so bucketing by size class would add a dimension this feature does not
 * need.
 */
object DualScreenCustomLayoutStore {
    private const val BASE_KEY = "custom_layout"

    private fun prefs(context: Context) = SecondScreenPerGame.prefs(context)

    private fun fieldKey(base: String, landscape: Boolean, field: String): String =
        "${base}_${if (landscape) "landscape" else "portrait"}_$field"

    /** [gameId]'s own saved layout for this orientation when it has one, else the global one,
     *  else null (Custom picked but the editor never ran for this orientation yet). */
    fun load(context: Context, landscape: Boolean, gameId: String?): DualScreenCustomLayout? {
        val titleBase = gameId?.let { SecondScreenPerGame.key(BASE_KEY, it) }
        if (titleBase != null) {
            read(context, titleBase, landscape)?.let { return it }
        }
        return read(context, BASE_KEY, landscape)
    }

    /** Saves [layout] for this orientation, per-title when [gameId] names a real title, else global. */
    fun save(context: Context, landscape: Boolean, gameId: String?, layout: DualScreenCustomLayout) {
        val base = gameId?.let { SecondScreenPerGame.key(BASE_KEY, it) } ?: BASE_KEY
        write(context, base, landscape, layout)
    }

    private fun read(context: Context, base: String, landscape: Boolean): DualScreenCustomLayout? {
        val p = prefs(context)
        fun f(field: String) = p.getFloat(fieldKey(base, landscape, field), Float.NaN)
        val gx = f("gx")
        val gy = f("gy")
        val gw = f("gw")
        val gh = f("gh")
        val cx = f("cx")
        val cy = f("cy")
        val cw = f("cw")
        val ch = f("ch")
        if (gx.isNaN() || gy.isNaN() || gw.isNaN() || gh.isNaN() ||
            cx.isNaN() || cy.isNaN() || cw.isNaN() || ch.isNaN()
        ) {
            return null
        }
        return DualScreenCustomLayout(DualScreenNormRect(gx, gy, gw, gh), DualScreenNormRect(cx, cy, cw, ch))
    }

    private fun write(context: Context, base: String, landscape: Boolean, layout: DualScreenCustomLayout) {
        prefs(context).edit {
            putFloat(fieldKey(base, landscape, "gx"), layout.gameFrac.x)
            putFloat(fieldKey(base, landscape, "gy"), layout.gameFrac.y)
            putFloat(fieldKey(base, landscape, "gw"), layout.gameFrac.w)
            putFloat(fieldKey(base, landscape, "gh"), layout.gameFrac.h)
            putFloat(fieldKey(base, landscape, "cx"), layout.companionFrac.x)
            putFloat(fieldKey(base, landscape, "cy"), layout.companionFrac.y)
            putFloat(fieldKey(base, landscape, "cw"), layout.companionFrac.w)
            putFloat(fieldKey(base, landscape, "ch"), layout.companionFrac.h)
        }
    }
}
