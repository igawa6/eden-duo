// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.views

import android.annotation.SuppressLint
import android.app.Presentation
import android.content.Context
import android.content.res.Resources
import android.database.ContentObserver
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.provider.Settings
import android.view.Display
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import java.lang.ref.WeakReference
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.utils.Log

/**
 * DSMod second screen: a Presentation on the non-primary display holding one SurfaceView.
 * The native side presents the aux output (bound guest layer / mirror) to this surface and
 * receives its touches as aux touch for the dsm:u service.
 *
 * Window rules learned on the AYN Thor: NOT_FOCUSABLE (a focusable second window steals the
 * controller), KEEP_SCREEN_ON (the main window's flag does not cover it).
 */
class AuxPresentation(context: Context, display: Display) :
    Presentation(context, display), SurfaceHolder.Callback {

    private lateinit var surfaceView: SurfaceView

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window?.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
        )
        // The two panels are lit independently on this hardware -- the same reason the flag above
        // has to be set here at all -- so the second screen keeps its own brightness unless it is
        // told to follow. Left alone it sits at full while the main screen dims, which on a
        // handheld reads as a fault and costs battery all evening.
        applyBrightness(systemBrightness())
        brightnessObserver = object : ContentObserver(Handler(Looper.getMainLooper())) {
            override fun onChange(selfChange: Boolean) = applyBrightness(systemBrightness())
        }.also { observer ->
            try {
                context.contentResolver.registerContentObserver(
                    Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS), false, observer
                )
                context.contentResolver.registerContentObserver(
                    Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS_MODE), false,
                    observer
                )
            } catch (e: Exception) {
                Log.warning("[AuxPresentation] cannot watch brightness: ${e.message}")
            }
        }
        surfaceView = SurfaceView(context)
        surfaceView.holder.addCallback(this)
        surfaceView.setOnTouchListener { _, event -> onAuxTouch(event) }
        surfaceView.isHapticFeedbackEnabled = true
        setContentView(surfaceView)
        current = WeakReference(this)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        // All work happens in surfaceChanged, which always follows creation.
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.info("[AuxPresentation] surface changed: ${width}x${height} on display ${display.displayId}")
        NativeLibrary.auxSurfaceChanged(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.info("[AuxPresentation] surface destroyed")
        NativeLibrary.auxSurfaceDestroyed()
    }

    private var brightnessObserver: ContentObserver? = null

    /// The system's brightness as a window value in 0..1, or BRIGHTNESS_OVERRIDE_NONE when it
    /// cannot be known -- under automatic brightness the stored number is not what is on screen,
    /// and guessing would fight the light sensor rather than follow it.
    private fun systemBrightness(): Float = try {
        val automatic = Settings.System.getInt(
            context.contentResolver, Settings.System.SCREEN_BRIGHTNESS_MODE,
            Settings.System.SCREEN_BRIGHTNESS_MODE_MANUAL
        ) == Settings.System.SCREEN_BRIGHTNESS_MODE_AUTOMATIC
        if (automatic) {
            WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        } else {
            val raw = Settings.System.getInt(
                context.contentResolver, Settings.System.SCREEN_BRIGHTNESS, -1
            )
            if (raw < 0) {
                WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
            } else {
                (raw.toFloat() / brightnessMax.toFloat()).coerceIn(0.01f, 1.0f)
            }
        }
    } catch (e: Exception) {
        WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
    }

    /// Full scale is not 255 everywhere -- panels reporting 1023 and 2047 are both common, and
    /// assuming 255 on one of those pins the second screen at maximum no matter the setting.
    private val brightnessMax: Int by lazy {
        try {
            val id = Resources.getSystem()
                .getIdentifier("config_screenBrightnessSettingMaximum", "integer", "android")
            if (id != 0) Resources.getSystem().getInteger(id).coerceAtLeast(1) else 255
        } catch (e: Exception) {
            255
        }
    }

    /** Sets this screen's brightness. Public so the emulation view can push its own value. */
    fun applyBrightness(value: Float) {
        window?.let { w ->
            val params = w.attributes
            if (params.screenBrightness != value) {
                params.screenBrightness = value
                w.attributes = params
            }
        }
    }

    override fun onStop() {
        if (current?.get() === this) {
            current = null
        }
        brightnessObserver?.let {
            try {
                context.contentResolver.unregisterContentObserver(it)
            } catch (e: Exception) {
                Log.warning("[AuxPresentation] brightness observer already gone: ${e.message}")
            }
        }
        brightnessObserver = null
        super.onStop()
    }

    // --- DSMod haptics -----------------------------------------------------------------------

    private val mainHandler = Handler(Looper.getMainLooper())

    private val vibrator: Vibrator? by lazy {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)
                    ?.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
            }
        } catch (e: Exception) {
            Log.warning("[AuxPresentation] no vibrator service: ${e.message}")
            null
        }
    }

    /// The user's "touch feedback" switch (Settings > Sound & vibration). Unknown counts as on.
    @Suppress("DEPRECATION")
    private fun systemHapticsEnabled(): Boolean = try {
        Settings.System.getInt(
            context.contentResolver, Settings.System.HAPTIC_FEEDBACK_ENABLED, 1
        ) != 0
    } catch (e: Exception) {
        true
    }

    /**
     * Plays one runtime haptic (UI thread). The view path is preferred: it follows the system's
     * haptic settings and the device's own tuned effects. When the view declines although the
     * user has touch feedback on (a window the system does not vibrate for), or the package asks
     * to play regardless of the setting, the default vibrator plays the matching predefined effect.
     */
    private fun performHaptic(strength: Int, kind: Int, flags: Int) {
        if (!this::surfaceView.isInitialized) return
        val respectSystem = (flags and 1) != 0
        val constant = when (strength) {
            1 -> HapticFeedbackConstants.CONTEXT_CLICK
            2 -> HapticFeedbackConstants.KEYBOARD_TAP
            3 -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                HapticFeedbackConstants.CONFIRM
            } else {
                HapticFeedbackConstants.VIRTUAL_KEY
            }
            4 -> HapticFeedbackConstants.LONG_PRESS
            5 -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                HapticFeedbackConstants.REJECT
            } else {
                HapticFeedbackConstants.LONG_PRESS
            }
            else -> return
        }
        @Suppress("DEPRECATION")
        val viewFlags =
            if (respectSystem) 0 else HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING
        val systemOn = systemHapticsEnabled()
        var path = "view"
        if (!surfaceView.performHapticFeedback(constant, viewFlags)) {
            path = if (respectSystem && !systemOn) {
                "skipped (system touch feedback off)"
            } else if (vibrateFallback(strength)) {
                "vibrator"
            } else {
                "unavailable"
            }
        }
        if (hapticLogBudget > 0) {
            hapticLogBudget--
            Log.info(
                "[AuxPresentation] haptic strength $strength kind $kind -> $path " +
                    "(system touch feedback ${if (systemOn) "on" else "off"}, " +
                    "respect_system $respectSystem, display ${display.displayId})"
            )
        }
    }

    private fun vibrateFallback(strength: Int): Boolean {
        val vib = vibrator ?: return false
        if (!vib.hasVibrator() || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
        val effectId = when (strength) {
            1 -> VibrationEffect.EFFECT_TICK
            2, 3 -> VibrationEffect.EFFECT_CLICK
            4 -> VibrationEffect.EFFECT_HEAVY_CLICK
            5 -> VibrationEffect.EFFECT_DOUBLE_CLICK
            else -> return false
        }
        return try {
            val effect = VibrationEffect.createPredefined(effectId)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                vib.vibrate(
                    effect,
                    VibrationAttributes.createForUsage(VibrationAttributes.USAGE_TOUCH)
                )
            } else {
                @Suppress("DEPRECATION")
                vib.vibrate(effect)
            }
            true
        } catch (e: Exception) {
            Log.warning("[AuxPresentation] vibrator failed: ${e.message}")
            false
        }
    }

    private fun onAuxTouch(event: MotionEvent): Boolean {
        val action = event.actionMasked
        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> Unit
            else -> return false
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            NativeInput.onAuxTouchEvent(IntArray(0), FloatArray(0), FloatArray(0), 0, 0)
            return true
        }
        // Report every pointer still down; on POINTER_UP the lifted pointer is dropped.
        val lifting = if (action == MotionEvent.ACTION_POINTER_UP) event.actionIndex else -1
        val n = event.pointerCount
        val ids = IntArray(n)
        val xs = FloatArray(n)
        val ys = FloatArray(n)
        var count = 0
        var startMask = 0
        for (i in 0 until n) {
            if (i == lifting) continue
            ids[count] = event.getPointerId(i)
            xs[count] = event.getX(i)
            ys[count] = event.getY(i)
            if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) &&
                i == event.actionIndex
            ) {
                startMask = startMask or (1 shl count)
            }
            count++
        }
        NativeInput.onAuxTouchEvent(ids, xs, ys, count, startMask)
        return true
    }

    companion object {
        /** The presentation currently on screen, for native haptic requests. */
        @Volatile
        private var current: WeakReference<AuxPresentation>? = null

        /** The first haptics of a session are logged with the path that played them. */
        @Volatile
        private var hapticLogBudget = 24

        /**
         * A haptic from the dual-screen runtime (any thread): played on the UI thread by the
         * second screen's view. Dropped while no presentation is showing.
         */
        @JvmStatic
        fun playHaptic(strength: Int, kind: Int, flags: Int) {
            val presentation = current?.get() ?: return
            presentation.mainHandler.post { presentation.performHaptic(strength, kind, flags) }
        }

        /**
         * Candidate aux displays, best first. Physical second panels come first: the AYN Thor's
         * "Screen-2" and Cuttlefish's second built-in screen do NOT carry FLAG_PRESENTATION,
         * while simulated/overlay displays do -- picking by that flag alone lands on the wrong
         * screen. Some displays still refuse app windows, so callers must fall back down the list.
         */
        fun auxDisplayCandidates(context: Context, ownDisplayId: Int): List<Display> {
            val dm = context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
            // getDisplays() alone does not list every panel: on the AYN Thor the second screen
            // shows up only under DISPLAY_CATEGORY_PRESENTATION, while Cuttlefish's second
            // built-in screen shows up only in the plain list. Union both, dedup by id.
            val all = LinkedHashMap<Int, Display>()
            for (d in dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
                all[d.displayId] = d
            }
            for (d in dm.displays) {
                all.putIfAbsent(d.displayId, d)
            }
            for (d in all.values) {
                Log.info(
                    "[AuxPresentation] display ${d.displayId} \"${d.name}\" state=${d.state} " +
                        "flags=0x${Integer.toHexString(d.flags)} own=$ownDisplayId"
                )
            }
            val candidates = all.values.filter {
                it.displayId != ownDisplayId && it.state != Display.STATE_OFF
            }
            val simulated = candidates.filter {
                it.name.contains("Overlay", ignoreCase = true) ||
                    it.name.contains("Simulated", ignoreCase = true) ||
                    it.name.contains("Virtual", ignoreCase = true)
            }
            val physical = candidates - simulated.toSet()
            return (physical.sortedByDescending { it.name.contains("Screen-2", ignoreCase = true) } +
                simulated)
        }

        fun pickAuxDisplay(context: Context, ownDisplayId: Int): Display? =
            auxDisplayCandidates(context, ownDisplayId).firstOrNull()

        /**
         * Shows the aux presentation on the first candidate display that accepts a window.
         * WindowManager throws InvalidDisplayException ("the specified display can not be found")
         * for displays that refuse app windows, so try them in order.
         */
        fun showOnBestDisplay(context: Context): AuxPresentation? {
            val ownDisplayId = try {
                @Suppress("DEPRECATION")
                (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager)
                    .defaultDisplay.displayId
            } catch (e: Exception) {
                Display.DEFAULT_DISPLAY
            }
            val candidates = auxDisplayCandidates(context, ownDisplayId)
            if (candidates.isEmpty()) {
                Log.info("[AuxPresentation] no secondary display available")
                return null
            }
            for (display in candidates) {
                try {
                    val presentation = AuxPresentation(context, display)
                    presentation.show()
                    Log.info(
                        "[AuxPresentation] shown on display ${display.displayId} (${display.name})"
                    )
                    return presentation
                } catch (e: Exception) {
                    Log.warning(
                        "[AuxPresentation] display ${display.displayId} (${display.name}) " +
                            "refused the window: ${e.message}"
                    )
                }
            }
            Log.error("[AuxPresentation] no display accepted the aux presentation")
            return null
        }
    }
}
