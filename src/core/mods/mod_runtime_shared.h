// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Private header for the runtime's area files: mod_runtime.cpp, mod_manifest.cpp, mod_state.cpp,
// mod_actions.cpp, mod_input.cpp, mod_pages.cpp, mod_map.cpp, mod_redraw.cpp and
// mod_guest_bridge.cpp, plus the dev-tools files mod_re_tools.cpp and mod_console.cpp. Not
// included by anything outside src/core/mods.
//   - Anonymous namespace, so each including file gets its own copy: ModTickHz,
//     MillisecondsToModTicks, the RuntimeStageStats / RuntimeStageTimer profiling helpers
//     (EDEN_DSMOD_PROFILE; each timed site keeps its own stats), StaticFieldsToken,
//     MaxTextLength, HapticStrengthNames / HapticKindNames. All constexpr or classes with inline
//     members, so an unused copy costs nothing.
//   - Declarations of the two helpers with external linkage: ParseButton (defined in
//     mod_actions.cpp) and NormaliseToType (defined in mod_state.cpp).
// Flow and threads: none of its own. The stage timers run on the tick thread (tick, page binds,
// transitions, dirty scan) and on the redraw worker (redraw-worker, map rebuilds).

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "common/common_types.h"
#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "core/mods/mod_types.h"
#include "input_common/drivers/virtual_gamepad.h"

namespace Core::Mods {

namespace {

constexpr u64 ModTickHz = 60;

// CPU wall time only: no GPU query, readback or performance-counter reset. A few summaries
// per five seconds make Android slowdown reports useful without per-frame log traffic.
struct RuntimeStageStats {
    using Clock = std::chrono::steady_clock;
    Clock::time_point window = Common::DSMod::DevToolsEnabled ? Clock::now() : Clock::time_point{};
    double total_ms{}, max_ms{};
    u64 calls{}, over_budget{};
};

/// EDEN_DSMOD_PROFILE: the periodic CPU stage summaries (on unless set to 0/false).
inline bool RuntimeProfileEnabled() {
    static const bool enabled = [] {
        const char* value = Common::DSMod::DevEnvironment("EDEN_DSMOD_PROFILE");
        return Common::DSMod::DevToolsEnabled &&
               (!value || (std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
                           std::strcmp(value, "FALSE") != 0));
    }();
    return enabled;
}

#if EDEN_DSMOD_BUILD_DEV_TOOLS
class RuntimeStageTimer {
public:
    RuntimeStageTimer(RuntimeStageStats& stats_, const char* stage_)
        : stats{stats_}, stage{stage_} {
        active = RuntimeProfileEnabled();
        if (active) {
            start = RuntimeStageStats::Clock::now();
        }
    }
    ~RuntimeStageTimer() {
        if (!active) {
            return;
        }
        const auto now = RuntimeStageStats::Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - start).count();
        stats.total_ms += ms;
        stats.max_ms = std::max(stats.max_ms, ms);
        ++stats.calls;
        stats.over_budget += ms > 1000.0 / 60.0;
        if (now - stats.window >= std::chrono::seconds{5}) {
            LOG_INFO(Core, "DSMod perf CPU {}: calls={} avg={:.3f}ms max={:.3f}ms over16.7ms={}",
                     stage, stats.calls, stats.total_ms / stats.calls, stats.max_ms,
                     stats.over_budget);
            stats = {};
        }
    }

private:
    RuntimeStageStats& stats;
    const char* stage;
    RuntimeStageStats::Clock::time_point start;
    bool active{};
};

#else
class RuntimeStageTimer {
public:
    RuntimeStageTimer(RuntimeStageStats&, const char*) {}
};
#endif

/// A guest float as an integer, defined for every input: NaN -> 0, out of range -> the nearest
/// s64 (what arm64's fcvtzs does). A plain cast is UB there, and x86 and arm64 disagree on it, so
/// loading-screen garbage (NaN, 1e30) read differently on the desktop and the handheld.
constexpr s64 SaturatingToS64(f64 value) {
    if (value != value) {
        return 0;
    }
    if (value >= 9223372036854775808.0) {
        return std::numeric_limits<s64>::max();
    }
    if (value < -9223372036854775808.0) {
        return std::numeric_limits<s64>::min();
    }
    return static_cast<s64>(value);
}

constexpr u64 MillisecondsToModTicks(u64 milliseconds) {
    return std::max<u64>(1, (milliseconds * ModTickHz + 999) / 1000);
}

/// A chain hop written as "static_fields" instead of a number: resolved at read time from the
/// class itself, so a data file does not have to encode a Unity-version constant.
constexpr s64 StaticFieldsToken = std::numeric_limits<s64>::min();
constexpr u32 MaxTextLength = 256;

constexpr std::array<const char*, 6> HapticStrengthNames{"off",     "light", "click",
                                                         "confirm", "heavy", "reject"};
constexpr std::array<const char*, static_cast<size_t>(HapticKind::Count)> HapticKindNames{
    "tap", "write", "select", "drag", "drop", "marker", "refused", "hold", "swipe"};
} // namespace

InputCommon::VirtualGamepad::VirtualButton ParseButton(const std::string& name, bool& ok);
s64 NormaliseToType(ValueType type, s64 value);

} // namespace Core::Mods
