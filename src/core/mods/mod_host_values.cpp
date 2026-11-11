// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 21 device and emulator values (mod_host_values.h). Called from ModRuntime::Tick and the
// module host's get_i64 / get_f64 on the tick thread.

#include "core/mods/mod_host_values.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "common/device_power_state.h"
#include "common/dsmod_dev_tools.h"

namespace Core::Mods {

namespace {
s64 SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

s64 Rounded(f64 value) {
    return static_cast<s64>(std::llround(std::clamp(value, -9.2e18, 9.2e18)));
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<s64> ParseInt(std::string_view text) {
    text = Trim(text);
    s64 value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}
} // namespace

std::optional<PowerReading> ParseFakeBattery(std::string_view text) {
    text = Trim(text);
    if (text == "none") {
        return PowerReading{};
    }
    const auto comma = text.find(',');
    const auto pct = ParseInt(text.substr(0, comma));
    if (!pct) {
        return std::nullopt;
    }
    bool charging = false;
    if (comma != std::string_view::npos) {
        const auto flag = ParseInt(text.substr(comma + 1));
        if (!flag || (*flag != 0 && *flag != 1)) {
            return std::nullopt;
        }
        charging = *flag == 1;
    }
    if (*pct < 0) {
        return PowerReading{};
    }
    return PowerReading{
        .present = true, .pct = static_cast<s32>(std::min<s64>(*pct, 100)), .charging = charging};
}

PowerReading ReadDevicePower() {
    if (const char* const fake = Common::DSMod::DevEnvironment("EDEN_DSMOD_FAKE_BATTERY")) {
        if (const auto reading = ParseFakeBattery(fake)) {
            return *reading;
        }
    }
    const Common::PowerStatus status = Common::GetPowerStatus();
    if (!status.has_battery || status.percentage < 0) {
        return PowerReading{};
    }
    return PowerReading{
        .present = true, .pct = std::clamp(status.percentage, 0, 100), .charging = status.charging};
}

void HostValues::SetSource(HostValueSource source_) {
    source = std::move(source_);
    last_update_ns.reset();
    last_power_ns.reset();
    baseline.reset();
}

void HostValues::Rebaseline(s64 now) {
    baseline.reset();
    if (source.perf) {
        if (const auto sample = source.perf()) {
            baseline = Baseline{now, *sample};
        }
    }
}

void HostValues::Update() {
    const s64 now = source.now_ns ? source.now_ns() : SteadyNowNs();
    if (!last_power_ns || now - *last_power_ns >= PowerIntervalNs || now < *last_power_ns) {
        power = source.power ? source.power() : ReadDevicePower();
        if (!power.present) {
            power = PowerReading{};
        }
        have_power = true;
        last_power_ns = now;
        ++power_reads;
    }
    if (source.screen) {
        // Runtime 25: plain atomic reads already done elsewhere every tick (panel_w/panel_h in
        // PublishUi) -- no interval to throttle, unlike the battery/perf reads above.
        const ScreenPane pane = source.screen();
        screen_w = pane.width;
        screen_h = pane.height;
        orientation_text = pane.width >= pane.height ? "landscape" : "portrait";
        mode_text = pane.simulated ? "simulated" : "physical";
        have_screen = true;
    }
    if (!source.perf) {
        last_update_ns = now;
        return;
    }
    // The tick stopped (emulation paused, second screen hidden): the interval since the baseline
    // is not representative, start a new one.
    const bool gap = last_update_ns && (now - *last_update_ns > PerfGapNs || now < *last_update_ns);
    last_update_ns = now;
    if (!baseline || gap) {
        Rebaseline(now);
        return;
    }
    const s64 wall_ns = now - baseline->wall_ns;
    if (wall_ns < PerfIntervalNs) {
        return;
    }
    const auto sample = source.perf();
    if (!sample) {
        baseline.reset();
        return;
    }
    const PerfSample& base = baseline->sample;
    if (wall_ns > PerfGapNs || sample->game_frames < base.game_frames ||
        sample->system_frames < base.system_frames || sample->frametime_ns < base.frametime_ns ||
        sample->emulated_us < base.emulated_us) {
        baseline = Baseline{now, *sample}; // a new PerfStats, or a stall: keep the last values
        return;
    }
    const f64 wall_s = static_cast<f64>(wall_ns) / 1e9;
    fps = static_cast<f64>(sample->game_frames - base.game_frames) / wall_s;
    speed_pct = static_cast<f64>(sample->emulated_us - base.emulated_us) / 1e6 / wall_s * 100.0;
    if (const u64 frames = sample->system_frames - base.system_frames; frames != 0) {
        frametime_ms = static_cast<f64>(sample->frametime_ns - base.frametime_ns) /
                       static_cast<f64>(frames) / 1e6;
        have_frametime = true;
    }
    have_perf = true;
    baseline = Baseline{now, *sample};
}

void HostValues::Publish(StateSnapshot& snapshot) const {
    if (have_power) {
        snapshot.ints[HostKeys::BatteryPresent] = power.present ? 1 : 0;
        snapshot.ints[HostKeys::BatteryPct] = power.pct;
        snapshot.ints[HostKeys::Charging] = power.charging ? 1 : 0;
    }
    if (have_perf) {
        snapshot.floats[HostKeys::Fps] = fps;
        snapshot.ints[HostKeys::Fps] = Rounded(fps);
        snapshot.floats[HostKeys::SpeedPct] = speed_pct;
        snapshot.ints[HostKeys::SpeedPct] = Rounded(speed_pct);
    }
    if (have_frametime) {
        snapshot.floats[HostKeys::FrametimeMs] = frametime_ms;
        snapshot.ints[HostKeys::FrametimeMs] = Rounded(frametime_ms);
        snapshot.ints[HostKeys::FrametimeX10] = Rounded(frametime_ms * 10.0);
    }
    if (have_screen) {
        snapshot.ints[HostKeys::PaneW] = static_cast<s64>(screen_w);
        snapshot.ints[HostKeys::PaneH] = static_cast<s64>(screen_h);
        snapshot.texts[HostKeys::Orientation] = orientation_text;
        snapshot.texts[HostKeys::Mode] = mode_text;
    }
}

std::optional<s64> HostValues::GetI64(std::string_view name) const {
    if (have_power) {
        if (name == HostKeys::BatteryPresent) {
            return power.present ? 1 : 0;
        }
        if (name == HostKeys::BatteryPct) {
            return power.pct;
        }
        if (name == HostKeys::Charging) {
            return power.charging ? 1 : 0;
        }
    }
    if (have_perf) {
        if (name == HostKeys::Fps) {
            return Rounded(fps);
        }
        if (name == HostKeys::SpeedPct) {
            return Rounded(speed_pct);
        }
    }
    if (have_frametime) {
        if (name == HostKeys::FrametimeMs) {
            return Rounded(frametime_ms);
        }
        if (name == HostKeys::FrametimeX10) {
            return Rounded(frametime_ms * 10.0);
        }
    }
    if (have_screen) {
        if (name == HostKeys::PaneW) {
            return static_cast<s64>(screen_w);
        }
        if (name == HostKeys::PaneH) {
            return static_cast<s64>(screen_h);
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> HostValues::GetText(std::string_view name) const {
    if (!have_screen) {
        return std::nullopt;
    }
    if (name == HostKeys::Orientation) {
        return std::string_view{orientation_text};
    }
    if (name == HostKeys::Mode) {
        return std::string_view{mode_text};
    }
    return std::nullopt;
}

std::optional<f64> HostValues::GetF64(std::string_view name) const {
    if (have_perf) {
        if (name == HostKeys::Fps) {
            return fps;
        }
        if (name == HostKeys::SpeedPct) {
            return speed_pct;
        }
    }
    if (have_frametime && name == HostKeys::FrametimeMs) {
        return frametime_ms;
    }
    // The integer keys read as numbers too (the snapshot's derived lookup does the same).
    if (const auto value = GetI64(name)) {
        return static_cast<f64>(*value);
    }
    return std::nullopt;
}

bool IsHostValueKey(std::string_view name) {
    return name.starts_with("@device.") || name.starts_with("@perf.") ||
           name.starts_with("@screen.");
}

bool JsonReferencesHostKeys(const nlohmann::json& json) {
    const auto names_host = [](std::string_view text) {
        return text.find("@device.") != std::string_view::npos ||
               text.find("@perf.") != std::string_view::npos ||
               text.find("@screen.") != std::string_view::npos;
    };
    if (json.is_string()) {
        return names_host(json.get_ref<const std::string&>());
    }
    if (json.is_object()) {
        for (const auto& [key, value] : json.items()) {
            if (names_host(key) || JsonReferencesHostKeys(value)) {
                return true;
            }
        }
        return false;
    }
    if (json.is_array()) {
        for (const auto& value : json) {
            if (JsonReferencesHostKeys(value)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace Core::Mods
