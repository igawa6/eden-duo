// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.views

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.Resources
import android.database.ContentObserver
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.provider.Settings
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.Window
import android.view.WindowManager
import java.lang.ref.WeakReference
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.utils.Log

/**
 * DSMod second screen contents, shared by the two windows that can host them: AuxPresentation
 * (the second panel, while the game is on the main one) and AuxCompanionActivity (the main
 * panel, while the game is on the second one). One SurfaceView whose surface goes to the
 * native aux output, whose touches become aux touch for the dsm:u service, and which plays the
 * runtime's haptics; plus, on a panel the system does not dim, the system brightness.
 *
 * The host calls [createView] once, then [start]/[stop] with its own visibility.
 */
class AuxScreenHost(
    private val context: Context,
    private val window: () -> Window?,
    private val displayId: () -> Int,
    /** Copy the system brightness onto this window (a panel the system does not dim itself). */
    private val followSystemBrightness: Boolean,
    private val logTag: String
) : SurfaceHolder.Callback {

    private lateinit var surfaceView: SurfaceView

    /** Between start and stop: haptics are played only while the screen is up. */
    @Volatile
    private var active = false

    @SuppressLint("ClickableViewAccessibility")
    fun createView(): SurfaceView {
        surfaceView = SurfaceView(context)
        surfaceView.holder.addCallback(this)
        surfaceView.setOnTouchListener { _, event -> onAuxTouch(event) }
        surfaceView.isHapticFeedbackEnabled = true
        return surfaceView
    }

    fun start() {
        if (followSystemBrightness) {
            // The two panels are lit independently on this hardware -- the same reason the
            // window flags have to be set on the second screen at all -- so it keeps its own
            // brightness unless it is told to follow. Left alone it sits at full while the main
            // screen dims, which on a handheld reads as a fault and costs battery all evening.
            applyBrightness(systemBrightness())
            if (brightnessObserver == null) {
                brightnessObserver = object : ContentObserver(Handler(Looper.getMainLooper())) {
                    override fun onChange(selfChange: Boolean) =
                        applyBrightness(systemBrightness())
                }.also { observer ->
                    try {
                        context.contentResolver.registerContentObserver(
                            Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS), false,
                            observer
                        )
                        context.contentResolver.registerContentObserver(
                            Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS_MODE),
                            false, observer
                        )
                    } catch (e: Exception) {
                        Log.warning("$logTag cannot watch brightness: ${e.message}")
                    }
                }
            }
        }
        active = true
        current = WeakReference(this)
    }

    fun stop() {
        active = false
        if (current?.get() === this) {
            current = null
        }
        brightnessObserver?.let {
            try {
                context.contentResolver.unregisterContentObserver(it)
            } catch (e: Exception) {
                Log.warning("$logTag brightness observer already gone: ${e.message}")
            }
        }
        brightnessObserver = null
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        // All work happens in surfaceChanged, which always follows creation.
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.info("$logTag surface changed: ${width}x$height on display ${displayId()}")
        surfaceOwner = WeakReference(this)
        NativeLibrary.auxSurfaceChanged(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // An activity's window goes away asynchronously after finish(), possibly after its
        // replacement already handed native a new surface; only the owner may take it away.
        if (surfaceOwner?.get() !== this) {
            Log.info("$logTag surface destroyed (already replaced)")
            return
        }
        surfaceOwner = null
        Log.info("$logTag surface destroyed")
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

    /** Sets this screen's brightness (a window value in 0..1, or BRIGHTNESS_OVERRIDE_NONE). */
    private fun applyBrightness(value: Float) {
        window()?.let { w ->
            val params = w.attributes
            if (params.screenBrightness != value) {
                params.screenBrightness = value
                w.attributes = params
            }
        }
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
            Log.warning("$logTag no vibrator service: ${e.message}")
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
        // Posted from a native thread: the screen may have been dismissed since.
        if (!active || !this::surfaceView.isInitialized) return
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
                "$logTag haptic strength $strength kind $kind -> $path " +
                    "(system touch feedback ${if (systemOn) "on" else "off"}, " +
                    "respect_system $respectSystem, display ${displayId()})"
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
            Log.warning("$logTag vibrator failed: ${e.message}")
            false
        }
    }

    private val touchIds = IntArray(MAX_TOUCH)
    private val touchXs = FloatArray(MAX_TOUCH)
    private val touchYs = FloatArray(MAX_TOUCH)

    private fun onAuxTouch(event: MotionEvent): Boolean {
        val action = event.actionMasked
        when (action) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_MOVE, MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> Unit
            else -> return false
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            NativeInput.onAuxTouchEvent(touchIds, touchXs, touchYs, 0, 0)
            return true
        }
        // Report every pointer still down; on POINTER_UP the lifted pointer is dropped. The
        // buffers are reused: the native side copies them before returning.
        val lifting = if (action == MotionEvent.ACTION_POINTER_UP) event.actionIndex else -1
        val n = event.pointerCount
        val ids = touchIds
        val xs = touchXs
        val ys = touchYs
        var count = 0
        var startMask = 0
        for (i in 0 until n) {
            if (i == lifting) continue
            if (count == MAX_TOUCH) break
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
        /** Touch points the native side takes (AuxRouting::MaxTouch). */
        private const val MAX_TOUCH = 16

        /** The second screen currently up, for native haptic requests. */
        @Volatile
        private var current: WeakReference<AuxScreenHost>? = null

        /** The host whose surface native currently draws to. */
        @Volatile
        private var surfaceOwner: WeakReference<AuxScreenHost>? = null

        /** The first haptics of a session are logged with the path that played them. */
        @Volatile
        private var hapticLogBudget = 24

        /**
         * A haptic from the dual-screen runtime (any thread): played on the UI thread by the
         * second screen's view. Dropped while no second screen is up.
         */
        @JvmStatic
        fun playHaptic(strength: Int, kind: Int, flags: Int) {
            val host = current?.get() ?: return
            host.mainHandler.post { host.performHaptic(strength, kind, flags) }
        }
    }
}
