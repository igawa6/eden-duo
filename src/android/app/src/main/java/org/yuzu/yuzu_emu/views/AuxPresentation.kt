// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.views

import android.app.Presentation
import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.view.Display
import android.view.WindowManager
import org.yuzu.yuzu_emu.utils.Log

/**
 * DSMod second screen: a Presentation on the non-primary display holding the AuxScreenHost
 * view. The native side presents the aux output (bound guest layer / mirror) to its surface and
 * receives its touches as aux touch for the dsm:u service.
 *
 * Window rules learned on the AYN Thor: NOT_FOCUSABLE (a focusable second window steals the
 * controller), KEEP_SCREEN_ON (the main window's flag does not cover it).
 *
 * A Presentation can only go to a display carrying FLAG_PRESENTATION, which the default display
 * never does; with the game on a secondary display, AuxCompanionActivity hosts the same view on
 * the default one instead.
 */
class AuxPresentation(outerContext: Context, display: Display) :
    Presentation(outerContext, display) {

    // Built on the Presentation's own context (the target display's), not the Activity's.
    private val host by lazy {
        AuxScreenHost(
            context = context,
            window = { window },
            displayId = { getDisplay().displayId },
            followSystemBrightness = true,
            logTag = "[AuxPresentation]"
        )
    }

    /** The display this presentation is on. */
    val displayId: Int get() = display.displayId

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window?.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
        )
        setContentView(host.createView())
    }

    override fun onStart() {
        super.onStart()
        host.start()
    }

    override fun onStop() {
        host.stop()
        super.onStop()
    }

    /**
     * Undoes what show() set up before the window manager refused the display: Dialog.show()
     * runs onCreate and onStart before adding the window, and onStop never runs for a window
     * that was never shown, which left the brightness observer, the Presentation's display
     * listener and this object (with its Activity) registered.
     */
    private fun cleanupAfterFailedShow() {
        try {
            onStop()
        } catch (e: Exception) {
            Log.warning("[AuxPresentation] cleanup after a refused display: ${e.message}")
        }
    }

    companion object {
        /**
         * Candidate aux displays, best first. Physical second panels come first and simulated or
         * overlay displays last: both kinds can carry FLAG_PRESENTATION (the AYN Thor's
         * "Screen-2" does, which is why a Presentation works there), so that flag alone does not
         * find the real panel. A Presentation is refused on any display without the flag -- the
         * default display never has it -- and some displays refuse app windows anyway, so
         * callers must fall back down the list.
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
            val simulated = candidates.filter { isSimulatedDisplay(it) }
            val physical = candidates - simulated.toSet()
            return (physical.sortedByDescending { it.name.contains("Screen-2", ignoreCase = true) } +
                simulated)
        }

        /** A developer-options overlay or a virtual display rather than a real panel. */
        fun isSimulatedDisplay(display: Display): Boolean =
            display.name.contains("Overlay", ignoreCase = true) ||
                display.name.contains("Simulated", ignoreCase = true) ||
                display.name.contains("Virtual", ignoreCase = true)

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
                var presentation: AuxPresentation? = null
                try {
                    presentation = AuxPresentation(context, display)
                    presentation.show()
                    Log.info(
                        "[AuxPresentation] shown on display ${display.displayId} (${display.name})"
                    )
                    return presentation
                } catch (e: Exception) {
                    presentation?.cleanupAfterFailedShow()
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
