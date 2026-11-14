// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import org.yuzu.yuzu_emu.R

/**
 * Eden Duo 1.4.0: the freeform "Edit dual screen layout" editor's drawing/touch surface
 * (LAYOUT-EDITOR-SPEC.md), added over the running game by [DualScreenFragmentHost] exactly like
 * [org.yuzu.yuzu_emu.overlay.InputOverlay]'s own edit mode is added over it -- a thin wrapper
 * around [DualScreenLayoutEditor]'s pure functions: every touch event here is translated into one
 * call to a pure function and nothing else is computed in this class. The live game/companion
 * split on the native renderer is untouched while editing (only "Done" pushes a new layout, see
 * [DualScreenFragmentHost]) -- this view only draws a preview of where the two rects would go.
 */
class DualScreenLayoutEditorView(context: Context, attrs: AttributeSet?) : View(context, attrs) {
    private enum class Pane { GAME, COMPANION }

    /** Window geometry for this editing session; set once by the host before the view is shown. */
    var window: DualScreenWindow = DualScreenWindow(1, 1, 160)

    var gameAspect: DualScreenAspect? = DualScreenAspect(16, 9)
    var companionAspect: DualScreenAspect = DualScreenAspect(1240, 1080)

    /** The box the game may occupy (full window -- it may be full-bleed). */
    var gameBox: DualScreenRect = DualScreenRect.EMPTY

    /** The box the companion must stay inside (window minus safe-drawing insets). */
    var companionBox: DualScreenRect = DualScreenRect.EMPTY

    var gameRect: DualScreenRect = DualScreenRect.EMPTY
        set(value) {
            field = value
            invalidate()
        }

    var companionRect: DualScreenRect = DualScreenRect.EMPTY
        set(value) {
            field = value
            invalidate()
        }

    private var minGameW = 1
    private var minCompanionW = 1
    private var minCompanionH = 1

    private var activePane: Pane? = null
    private var activeHandle: DualScreenEditorHandle? = null
    private var dragStartRect: DualScreenRect = DualScreenRect.EMPTY
    private var dragStartX = 0f
    private var dragStartY = 0f
    private var activePointerId = MotionEvent.INVALID_POINTER_ID

    private val handleRadiusPx get() = DualScreenLayoutEditor.dpToPx(HANDLE_HIT_RADIUS_DP, window)

    private val gamePaint = framePaint(Color.argb(220, 100, 181, 246))
    private val companionPaint = framePaint(Color.argb(220, 255, 183, 77))
    private val fillPaint = Paint().apply { style = Paint.Style.FILL }
    private val handlePaint = Paint().apply { style = Paint.Style.FILL; color = Color.WHITE }
    private val labelPaint = Paint().apply {
        color = Color.WHITE
        isAntiAlias = true
        textSize = 36f
        setShadowLayer(4f, 0f, 0f, Color.BLACK)
    }

    private fun framePaint(color: Int) = Paint().apply {
        style = Paint.Style.STROKE
        strokeWidth = 6f
        isAntiAlias = true
        this.color = color
    }

    private val scaleDetector = ScaleGestureDetector(
        context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
                val pane = paneAt(detector.focusX.toInt(), detector.focusY.toInt()) ?: return false
                activePane = pane
                activeHandle = null
                return true
            }

            override fun onScale(detector: ScaleGestureDetector): Boolean {
                val pane = activePane ?: return false
                applyConstrained(pane, resizeFromPinch(pane, detector.scaleFactor))
                return true
            }

            override fun onScaleEnd(detector: ScaleGestureDetector) {
                activePane = null
            }
        }
    )

    /** Call once before showing the view: seeds the working rects and per-pane minimums. */
    fun configure(
        window: DualScreenWindow,
        gameBox: DualScreenRect,
        companionBox: DualScreenRect,
        gameAspect: DualScreenAspect?,
        companionAspect: DualScreenAspect,
        gameRect: DualScreenRect,
        companionRect: DualScreenRect
    ) {
        this.window = window
        this.gameBox = gameBox
        this.companionBox = companionBox
        this.gameAspect = gameAspect
        this.companionAspect = companionAspect
        minGameW = (window.widthPx * DualScreenLayoutEditor.MIN_GAME_WIDTH_FRACTION).toInt().coerceAtLeast(1)
        val minCompanionShortEdgePx =
            DualScreenLayoutEditor.dpToPx(DualScreenLayoutEditor.MIN_COMPANION_SHORT_EDGE_DP, window).toInt()
        minCompanionW = minCompanionShortEdgePx.coerceAtLeast(1)
        minCompanionH = minCompanionShortEdgePx.coerceAtLeast(1)
        this.gameRect = gameRect
        this.companionRect = companionRect
    }

    private fun rectFor(pane: Pane) = if (pane == Pane.GAME) gameRect else companionRect
    private fun boxFor(pane: Pane) = if (pane == Pane.GAME) gameBox else companionBox
    private fun aspectFor(pane: Pane): DualScreenAspect? = if (pane == Pane.GAME) gameAspect else companionAspect
    private fun otherRect(pane: Pane) = if (pane == Pane.GAME) companionRect else gameRect
    private fun minWFor(pane: Pane) = if (pane == Pane.GAME) minGameW else minCompanionW
    private fun minHFor(pane: Pane) = if (pane == Pane.GAME) 1 else minCompanionH

    private fun setRect(pane: Pane, rect: DualScreenRect) {
        if (pane == Pane.GAME) gameRect = rect else companionRect = rect
    }

    private fun resizeFromPinch(pane: Pane, scaleFactor: Float): DualScreenRect =
        DualScreenLayoutEditor.resizeFromPinch(rectFor(pane), scaleFactor, aspectFor(pane), minWFor(pane), minHFor(pane))

    /** Clamp + no-overlap pass every gesture result goes through before it is shown;
     *  a step that cannot satisfy both keeps the pane where it was. */
    private fun constrain(pane: Pane, candidate: DualScreenRect, snap: Boolean = false): DualScreenRect =
        DualScreenLayoutEditor.constrain(
            candidate, boxFor(pane), otherRect(pane), rectFor(pane), if (snap) window else null
        )

    private fun applyConstrained(pane: Pane, candidate: DualScreenRect) {
        setRect(pane, constrain(pane, candidate))
    }

    // ---- toolbar actions (called by the host) ----

    fun swap() {
        val (newGame, newCompanion) = DualScreenLayoutEditor.swap(window, gameRect, companionRect, gameAspect, companionAspect)
        gameRect = newGame
        companionRect = newCompanion
    }

    /** Seeds both rects from a fresh Automatic computation (the toolbar's own "Reset"). */
    fun reset(automatic: DualScreenLayout) {
        gameRect = automatic.gameRect
        companionRect = automatic.companionRect
    }

    // ---- hit testing ----

    private fun paneAt(x: Int, y: Int): Pane? = when {
        gameRect.contains(x, y) -> Pane.GAME
        companionRect.contains(x, y) -> Pane.COMPANION
        else -> null
    }

    private fun handleAt(rect: DualScreenRect, x: Int, y: Int): DualScreenEditorHandle? {
        val r = handleRadiusPx
        fun near(hx: Int, hy: Int) = kotlin.math.hypot((x - hx).toDouble(), (y - hy).toDouble()) <= r
        return when {
            near(rect.x, rect.y) -> DualScreenEditorHandle.TOP_LEFT
            near(rect.x + rect.w, rect.y) -> DualScreenEditorHandle.TOP_RIGHT
            near(rect.x, rect.y + rect.h) -> DualScreenEditorHandle.BOTTOM_LEFT
            near(rect.x + rect.w, rect.y + rect.h) -> DualScreenEditorHandle.BOTTOM_RIGHT
            else -> null
        }
    }

    private fun hitTest(x: Int, y: Int): Pair<Pane, DualScreenEditorHandle?>? {
        // Handles first (companion drawn on top visually when overlapping is impossible by
        // invariant, so draw order does not matter for hit testing), then the rect body.
        for (pane in Pane.entries) {
            handleAt(rectFor(pane), x, y)?.let { return pane to it }
        }
        paneAt(x, y)?.let { return it to null }
        return null
    }

    // ---- touch ----

    // Every touch is consumed while editing, hit or miss, so none falls through to the game's
    // input overlay underneath.
    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(event)
        if (scaleDetector.isInProgress) return true
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val hit = hitTest(event.x.toInt(), event.y.toInt()) ?: return true
                activePane = hit.first
                activeHandle = hit.second
                dragStartRect = rectFor(hit.first)
                dragStartX = event.x
                dragStartY = event.y
                activePointerId = event.getPointerId(0)
                return true
            }

            MotionEvent.ACTION_MOVE -> {
                val pane = activePane ?: return true
                val idx = event.findPointerIndex(activePointerId)
                if (idx < 0) return true
                val dx = (event.getX(idx) - dragStartX).toInt()
                val dy = (event.getY(idx) - dragStartY).toInt()
                val handle = activeHandle
                val candidate = if (handle != null) {
                    DualScreenLayoutEditor.resizeFromHandle(
                        dragStartRect, handle, dx, dy, aspectFor(pane), minWFor(pane), minHFor(pane)
                    )
                } else {
                    DualScreenLayoutEditor.move(dragStartRect, dx, dy)
                }
                // Snapping only applies to a move; a resize honours the finger exactly.
                setRect(pane, constrain(pane, candidate, snap = handle == null))
                return true
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL, MotionEvent.ACTION_POINTER_UP -> {
                if (event.actionMasked != MotionEvent.ACTION_POINTER_UP ||
                    event.getPointerId(event.actionIndex) == activePointerId
                ) {
                    activePane = null
                    activeHandle = null
                    activePointerId = MotionEvent.INVALID_POINTER_ID
                }
                return true
            }
        }
        return true
    }

    // ---- drawing ----

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        drawPane(canvas, companionRect, companionPaint, resources.getString(R.string.dual_screen_layout_editor_companion))
        drawPane(canvas, gameRect, gamePaint, resources.getString(R.string.dual_screen_layout_editor_game))
    }

    private fun drawPane(canvas: Canvas, rect: DualScreenRect, paint: Paint, label: String) {
        if (rect.isEmpty) return
        val r = android.graphics.RectF(rect.x.toFloat(), rect.y.toFloat(), (rect.x + rect.w).toFloat(), (rect.y + rect.h).toFloat())
        fillPaint.color = Color.argb(40, Color.red(paint.color), Color.green(paint.color), Color.blue(paint.color))
        canvas.drawRect(r, fillPaint)
        canvas.drawRect(r, paint)
        canvas.drawText(label, r.left + 16f, r.top + 48f, labelPaint)
        val handleR = handleRadiusPx / 3f
        for ((hx, hy) in listOf(
            r.left to r.top, r.right to r.top, r.left to r.bottom, r.right to r.bottom
        )) {
            canvas.drawCircle(hx, hy, handleR, handlePaint)
            canvas.drawCircle(hx, hy, handleR, paint)
        }
    }

    private companion object {
        const val HANDLE_HIT_RADIUS_DP = 28f
    }
}
