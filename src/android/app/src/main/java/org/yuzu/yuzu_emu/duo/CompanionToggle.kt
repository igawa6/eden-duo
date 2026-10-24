// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.ComponentName
import android.content.Context
import android.os.Build
import android.os.SystemClock
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import android.view.MotionEvent
import android.widget.Toast
import androidx.annotation.RequiresApi
import androidx.core.content.edit
import java.lang.ref.WeakReference
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.features.settings.model.AbstractIntSetting
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame

/**
 * Eden Duo: the in-game companion toggle. It closes the second-screen window of the running game
 * so that screen can be used for another app, and brings it back where it was. The runtime keeps
 * running without the window and repaints it when it comes back; a companion restores its own
 * HUD state.
 *
 * Three triggers, all ending in [toggle]: the gesture on the game screen ([GameTouch]), the
 * Quick Settings tile ([CompanionTileService]) and the in-game menu. The running game's
 * DualScreenEmulationController registers itself here; the tile lives in the same process.
 * The hidden state belongs to the game session (CompanionToggleState) and is never stored.
 */
object CompanionToggle {
    private const val KEY_GESTURE = "companion_toggle_gesture"

    private var active: WeakReference<DualScreenEmulationController>? = null

    private val prefs get() = SecondScreenPerGame.prefs(YuzuApplication.appContext)

    /** The running game's controller, or null when no game runs. */
    private val controller: DualScreenEmulationController? get() = active?.get()

    val isGameRunning: Boolean get() = controller != null

    val isHidden: Boolean get() = controller?.isCompanionHiddenByUser == true

    fun register(controller: DualScreenEmulationController) {
        active = WeakReference(controller)
        refreshTile()
    }

    fun unregister(controller: DualScreenEmulationController) {
        if (active?.get() !== controller) return
        active = null
        refreshTile()
    }

    /** Toggles the running game's companion and says what happened. Main thread. */
    fun toggle(context: Context) {
        val running = controller
        if (running == null) {
            toast(context, R.string.companion_toggle_no_game)
            return
        }
        val message = when (running.toggleCompanion(SystemClock.uptimeMillis())) {
            CompanionToggleOutcome.HIDDEN -> R.string.companion_toggle_hidden
            CompanionToggleOutcome.SHOWN -> R.string.companion_toggle_shown
            CompanionToggleOutcome.NO_COMPANION -> R.string.companion_toggle_no_companion
            CompanionToggleOutcome.ADDON_OFF -> R.string.companion_toggle_addon_off
            CompanionToggleOutcome.NO_SECOND_SCREEN -> R.string.companion_toggle_no_screen
            CompanionToggleOutcome.IGNORED -> return
        }
        toast(context, message)
        refreshTile()
    }

    private fun toast(context: Context, message: Int) {
        Toast.makeText(context.applicationContext, message, Toast.LENGTH_SHORT).show()
    }

    /** Asks the system to update the Quick Settings tile, if the user added it. */
    fun refreshTile() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) return
        val context = YuzuApplication.appContext
        try {
            TileService.requestListeningState(
                context,
                ComponentName(context, CompanionTileService::class.java)
            )
        } catch (e: Exception) {
            Log.warning("[CompanionToggle] tile refresh refused: $e")
        }
    }

    // ---- gesture setting ----

    fun gesture(): CompanionGesture =
        CompanionGesture.fromValue(prefs.getInt(KEY_GESTURE, CompanionGesture.FOUR_FINGER_TAP.value))

    /** "Companion toggle gesture" for the Second Screen settings; stored here, not natively. */
    val gestureSetting: AbstractIntSetting = object : AbstractIntSetting {
        override fun getInt(needsGlobal: Boolean): Int = gesture().value
        override fun setInt(value: Int) = prefs.edit { putInt(KEY_GESTURE, value) }
        override val key: String = KEY_GESTURE
        override val isRuntimeModifiable: Boolean = true // read at every touch sequence
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override val defaultValue: Int = CompanionGesture.FOUR_FINGER_TAP.value
        override fun getValueAsString(needsGlobal: Boolean): String = getInt(needsGlobal).toString()
        override fun reset() = prefs.edit { remove(KEY_GESTURE) }
    }

    // ---- the game screen's touches ----

    /**
     * Feeds the game screen's touches to a [CompanionGestureRecognizer] and keeps a gesture in
     * progress away from the emulated touchscreen. [onToggle] runs when the gesture completes.
     */
    class GameTouch(pxPerDp: Float, private val onToggle: () -> Unit) {
        private val recognizer = CompanionGestureRecognizer(pxPerDp)

        /** True to pass [event] on to the emulated touchscreen. */
        fun filter(event: MotionEvent): Boolean {
            val action = when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    recognizer.gesture = gesture()
                    CompanionGestureRecognizer.Action.DOWN
                }
                MotionEvent.ACTION_POINTER_DOWN -> CompanionGestureRecognizer.Action.POINTER_DOWN
                MotionEvent.ACTION_MOVE -> CompanionGestureRecognizer.Action.MOVE
                MotionEvent.ACTION_POINTER_UP -> CompanionGestureRecognizer.Action.POINTER_UP
                MotionEvent.ACTION_UP -> CompanionGestureRecognizer.Action.UP
                MotionEvent.ACTION_CANCEL -> CompanionGestureRecognizer.Action.CANCEL
                else -> return !recognizer.isSwallowing
            }
            val pointers = (0 until event.pointerCount).map {
                CompanionGestureRecognizer.Pointer(event.getPointerId(it), event.getX(it), event.getY(it))
            }
            val decision = recognizer.onEvent(
                action,
                event.getPointerId(event.actionIndex),
                pointers,
                event.eventTime
            )
            // Harmless for a pointer the touchscreen never got (an overlay button had it).
            decision.release.forEach { NativeInput.onTouchReleased(it) }
            if (decision.toggle) onToggle()
            return decision.forward
        }
    }
}

/**
 * Eden Duo: the "Companion" Quick Settings tile. Active while the running game's companion is
 * shown, inactive while it is hidden or no game runs (a tap then says so). It reaches the game
 * through CompanionToggle, so it also works while another app is in front on the second screen.
 */
@RequiresApi(Build.VERSION_CODES.N)
class CompanionTileService : TileService() {
    override fun onStartListening() {
        super.onStartListening()
        update()
    }

    override fun onClick() {
        super.onClick()
        CompanionToggle.toggle(this)
        update()
    }

    private fun update() {
        val tile = qsTile ?: return
        val running = CompanionToggle.isGameRunning
        val hidden = CompanionToggle.isHidden
        tile.label = getString(R.string.companion_tile)
        tile.state = if (running && !hidden) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            tile.subtitle = getString(
                when {
                    !running -> R.string.companion_tile_no_game
                    hidden -> R.string.companion_tile_hidden
                    else -> R.string.companion_tile_shown
                }
            )
        }
        tile.updateTile()
    }
}
