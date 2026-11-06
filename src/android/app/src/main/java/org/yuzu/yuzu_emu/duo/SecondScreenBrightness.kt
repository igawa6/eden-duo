// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.provider.Settings
import androidx.core.content.edit
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.AbstractIntSetting
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.SecondScreenPerGame

/**
 * Eden Duo: whether the second-screen window copies the main screen's brightness
 * (AuxScreenHost). On most dual-screen hardware the second panel is not dimmed by the system,
 * so it is copied; on a device that lights that panel from its own slider, a window brightness
 * on top of it drags the panel to the main screen's level and the slider stops working
 * (eden-duo #8, #18). The Second Screen setting decides: Auto (detect such devices), On, Off.
 */
object SecondScreenBrightness {
    private const val KEY_MODE = "second_screen_follows_brightness"

    const val AUTO = 0
    const val ON = 1
    const val OFF = 2

    /** AYN's dual-screen assistant, which applies its own second-screen slider to the panel. */
    const val AYN_ASSISTANT = "com.odin.dualscreen.assistant"

    /// The second-screen level some AYN firmware keeps in Settings.System (the owner's Thor);
    /// newer Thor firmware (batch 7) drives the panel through per-display brightness instead and
    /// has no such key, which is why the assistant and the maker are checked as well.
    private const val DUAL_SCREEN_LEVEL = "dual_screen_brightness_level"

    private val prefs get() = SecondScreenPerGame.prefs(YuzuApplication.appContext)

    fun mode(): Int = prefs.getInt(KEY_MODE, AUTO).takeIf { it in AUTO..OFF } ?: AUTO

    /** "Second screen follows main brightness" for the Second Screen settings; stored here. */
    val setting: AbstractIntSetting = object : AbstractIntSetting {
        override fun getInt(needsGlobal: Boolean): Int = mode()
        override fun setInt(value: Int) = prefs.edit { putInt(KEY_MODE, value) }
        override val key: String = KEY_MODE
        override val isRuntimeModifiable: Boolean = false // read when the second screen opens
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override val defaultValue: Int = AUTO
        override fun getValueAsString(needsGlobal: Boolean): String = getInt(needsGlobal).toString()
        override fun reset() = prefs.edit { remove(KEY_MODE) }
    }

    /** What the device says about how it lights its second panel. */
    data class Device(
        /** Settings.System dual_screen_brightness_level, or null where it does not exist. */
        val dualScreenLevel: Int?,
        val aynAssistant: Boolean,
        val manufacturer: String,
        val brand: String
    )

    enum class Outcome { FOLLOW, DEVICE_LIT, OFF }

    data class Decision(val outcome: Outcome, val reason: String = "") {
        val follows: Boolean get() = outcome == Outcome.FOLLOW

        override fun toString(): String = when (outcome) {
            Outcome.FOLLOW -> "following main" + if (reason.isEmpty()) "" else " ($reason)"
            Outcome.DEVICE_LIT -> "device-lit ($reason)"
            Outcome.OFF -> "off by setting"
        }
    }

    /** The decision for [mode]; [device] is only looked at under Auto. */
    fun decide(mode: Int, device: () -> Device): Decision = when (mode) {
        ON -> Decision(Outcome.FOLLOW, "on by setting")
        OFF -> Decision(Outcome.OFF)
        else -> deviceLit(device())?.let { Decision(Outcome.DEVICE_LIT, it) }
            ?: Decision(Outcome.FOLLOW)
    }

    /** Why [device] lights its second panel itself, or null when nothing says it does. */
    fun deviceLit(device: Device): String? = when {
        device.dualScreenLevel != null -> "$DUAL_SCREEN_LEVEL ${device.dualScreenLevel}"
        device.aynAssistant -> "$AYN_ASSISTANT installed"
        device.manufacturer.equals("AYN", ignoreCase = true) -> "manufacturer AYN"
        device.brand.equals("AYN", ignoreCase = true) -> "brand AYN"
        else -> null
    }

    @Volatile
    private var lastLogged: String? = null

    /** The decision for this device and setting; logged when it differs from the last one. */
    fun decision(context: Context): Decision {
        val decision = decide(mode()) { probe(context) }
        val line = "second screen brightness: $decision"
        if (line != lastLogged) {
            lastLogged = line
            Log.info("[SecondScreenBrightness] $line")
        }
        return decision
    }

    private fun probe(context: Context): Device = Device(
        dualScreenLevel = try {
            Settings.System.getInt(context.contentResolver, DUAL_SCREEN_LEVEL, -1)
                .takeIf { it >= 0 }
        } catch (e: Exception) {
            null
        },
        aynAssistant = isInstalled(context, AYN_ASSISTANT),
        manufacturer = Build.MANUFACTURER ?: "",
        brand = Build.BRAND ?: ""
    )

    /// Needs the package in the manifest's <queries> on Android 11 and later.
    private fun isInstalled(context: Context, packageName: String): Boolean = try {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.packageManager.getPackageInfo(
                packageName, PackageManager.PackageInfoFlags.of(0)
            )
        } else {
            @Suppress("DEPRECATION")
            context.packageManager.getPackageInfo(packageName, 0)
        }
        true
    } catch (e: Exception) {
        false
    }
}
