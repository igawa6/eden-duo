// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.activities

import android.app.Activity
import android.app.ActivityOptions
import android.content.Intent
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.Display
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import android.window.OnBackInvokedCallback
import android.window.OnBackInvokedDispatcher
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.lang.ref.WeakReference
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.utils.InputHandler
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.views.AuxScreenHost

/**
 * DSMod second screen on the DEFAULT display, for when the game runs on a secondary one (the
 * "Swap screens" setting, or a launcher that opened Eden Duo on the add-on screen).
 *
 * A Presentation cannot do this: the window manager refuses presentation windows on displays
 * without FLAG_PRESENTATION, and the default display never has it. So the companion is an
 * ordinary activity, started on the default display in a task of its own (the game's task is
 * pinned to the display the game is on).
 *
 * Unlike AuxPresentation this window is focusable: only a focusable window can hide the system
 * bars, and on the default display the real status and navigation bars live. Touching it moves
 * key focus here, so controller input is handed back to the game.
 */
class AuxCompanionActivity : Activity() {

    private var host: AuxScreenHost? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Only the game starts this. Recreated by the system after a process restart or once
        // the game let it go, there is no game to be the second screen of. A recreation for a
        // configuration change not listed in the manifest is kept: the game still wants it,
        // and nothing would start it again until the next display event.
        if (!launchRequested) {
            Log.info("[AuxCompanion] not started by a game; closing")
            finishCompanion(this)
            return
        }
        try {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            window.decorView.setBackgroundColor(Color.BLACK)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                window.attributes = window.attributes.apply {
                    layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                }
            }
            hideSystemBars()
            val newHost = AuxScreenHost(
                context = this,
                window = { window },
                displayId = { displayIdCompat() },
                // The default display follows the system brightness by itself.
                followSystemBrightness = displayIdCompat() != Display.DEFAULT_DISPLAY,
                logTag = "[AuxCompanion]"
            )
            setContentView(newHost.createView())
            host = newHost
        } catch (e: Exception) {
            // Same process as the game: a crash here would take the game down with it.
            Log.error("[AuxCompanion] cannot host the second screen: ${e.message}")
            finishCompanion(this)
            return
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                backCallback
            )
        }
        instance = this
        launchPendingSince = 0L
        Log.info("[AuxCompanion] up on display ${displayIdCompat()}")
    }

    override fun onStart() {
        super.onStart()
        host?.start()
    }

    override fun onStop() {
        host?.stop()
        super.onStop()
    }

    override fun onDestroy() {
        if (instance === this) {
            instance = null
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            onBackInvokedDispatcher.unregisterOnBackInvokedCallback(backCallback)
        }
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).let { controller ->
            controller.hide(WindowInsetsCompat.Type.systemBars())
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    @Suppress("DEPRECATION")
    private fun displayIdCompat(): Int = try {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.displayId ?: Display.DEFAULT_DISPLAY
        } else {
            windowManager.defaultDisplay.displayId
        }
    } catch (e: Exception) {
        Display.DEFAULT_DISPLAY
    }

    /**
     * Controller input that lands here belongs to the game: touching the companion moves key
     * focus to this window, which would otherwise leave the controller dead until the game
     * screen is touched again. Volume keys stay with this window.
     */
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode != KeyEvent.KEYCODE_VOLUME_UP &&
            event.keyCode != KeyEvent.KEYCODE_VOLUME_DOWN &&
            event.keyCode != KeyEvent.KEYCODE_VOLUME_MUTE &&
            InputHandler.isPhysicalGameController(event.device)
        ) {
            val game = NativeLibrary.sEmulationActivity.get()
            if (game != null && game.dispatchKeyEvent(event)) {
                return true
            }
        }
        return super.dispatchKeyEvent(event)
    }

    /** Sticks and triggers, for the same reason as dispatchKeyEvent. */
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (InputHandler.isPhysicalGameController(event.device)) {
            val game = NativeLibrary.sEmulationActivity.get()
            if (game != null && game.dispatchGenericMotionEvent(event)) {
                return true
            }
        }
        return super.dispatchGenericMotionEvent(event)
    }

    /**
     * Back on this screen means back in the game (its menu), not closing the companion while
     * the game keeps running.
     */
    private fun forwardBack() {
        NativeLibrary.sEmulationActivity.get()?.onBackPressedDispatcher?.onBackPressed()
    }

    private val backCallback: OnBackInvokedCallback by lazy {
        OnBackInvokedCallback { forwardBack() }
    }

    @Deprecated("Deprecated in Java")
    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        forwardBack()
    }

    companion object {
        /**
         * How long a launch may stay outstanding before it is taken as lost and another is
         * allowed. A launch the system accepts and then drops would otherwise block the second
         * screen for the rest of the session.
         */
        private const val LAUNCH_TIMEOUT_MS = 5000L

        /** The live companion; it shares the game's process and is started and finished by it. */
        @Volatile
        private var instance: AuxCompanionActivity? = null

        @Volatile
        private var launchPendingSince = 0L

        /** Set from launch() until dismiss(): a companion is wanted by a running game. */
        @Volatile
        private var launchRequested = false

        /** The EmulationFragment that last asked for the companion. */
        @Volatile
        private var owner: WeakReference<Any>? = null

        /** True while a companion is up or a launch is still plausibly on its way. */
        fun isUpOrPending(): Boolean {
            if (instance != null) return true
            val since = launchPendingSince
            if (since == 0L) return false
            if (SystemClock.uptimeMillis() - since < LAUNCH_TIMEOUT_MS) return true
            Log.warning("[AuxCompanion] launch never landed; allowing another attempt")
            launchPendingSince = 0L
            return false
        }

        /**
         * True when the game activity sits on a secondary display, so its second screen belongs
         * on the default display -- hosted here, since a Presentation cannot go there. Android 11
         * and later only, as for the swap itself (ScreenSwap.redirectLaunch).
         */
        fun isWantedFor(game: Activity): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
            return activityDisplayId(game) != Display.DEFAULT_DISPLAY
        }

        @Suppress("DEPRECATION")
        fun activityDisplayId(activity: Activity): Int = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                activity.display?.displayId ?: Display.DEFAULT_DISPLAY
            } else {
                activity.windowManager.defaultDisplay.displayId
            }
        } catch (e: Exception) {
            Display.DEFAULT_DISPLAY
        }

        /**
         * Starts the companion on the default display, for [requester] (taking over one that is
         * already up). False when the system refused.
         */
        fun launch(game: Activity, requester: Any): Boolean {
            owner = WeakReference(requester)
            if (isUpOrPending()) return true
            val intent = Intent(game, AuxCompanionActivity::class.java).addFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK or
                    Intent.FLAG_ACTIVITY_MULTIPLE_TASK or
                    Intent.FLAG_ACTIVITY_NO_ANIMATION
            )
            val options = ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY)
            return try {
                launchRequested = true
                game.startActivity(intent, options.toBundle())
                // instance is set only once the companion's onCreate runs; until then this keeps
                // the next display event or onStart from starting a second one.
                launchPendingSince = SystemClock.uptimeMillis()
                Log.info("[AuxCompanion] started on display ${Display.DEFAULT_DISPLAY}")
                true
            } catch (e: Exception) {
                Log.warning("[AuxCompanion] launch on the default display refused: ${e.message}")
                launchRequested = false
                false
            }
        }

        /**
         * Takes the companion down (its own task, so it does not go with the game's) -- unless
         * another requester has taken it over since, as the next EmulationFragment of a ROM swap
         * may before the previous one is torn down.
         */
        fun dismiss(requester: Any) {
            val current = owner?.get()
            if (current != null && current !== requester) return
            owner = null
            launchRequested = false
            launchPendingSince = 0L
            val companion = instance ?: return
            instance = null
            finishCompanion(companion)
        }

        private fun finishCompanion(activity: Activity) {
            activity.finishAndRemoveTask()
            @Suppress("DEPRECATION")
            activity.overridePendingTransition(0, 0)
        }
    }
}
