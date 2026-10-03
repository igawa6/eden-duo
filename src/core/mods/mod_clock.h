// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 16: the built-in clock and timer points, published every tick before the derived pass
// like any other source:
//   @clock.hour   0-23      @clock.day    1-31      @clock.wday   0-6, Sunday = 0
//   @clock.minute 0-59      @clock.month  1-12      @clock.epoch  Unix seconds (UTC)
//   @clock.second 0-60      @clock.year   e.g. 2026
//   @game.seconds whole seconds since the emulated system started
// The wall-clock fields are the device's local time. A value only changes once a second, so a
// widget bound to one is dirty (and redrawn) at most once a second. A package that never names
// them (Manifest::uses_clock_keys, JsonReferencesClockKeys) does not get them at all, so its
// UiSignature does not change once a second for nothing.
// The time sources are injectable so tests are deterministic.

#pragma once

#include <functional>

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

#include "common/common_types.h"
#include "core/mods/mod_expr.h"
#include "core/mods/mod_types.h"

namespace Core::Mods {

/// The local-time breakdown of one epoch second.
struct ClockReading {
    s64 epoch{0};
    s32 year{1970};
    s32 month{1};
    s32 day{1};
    s32 hour{0};
    s32 minute{0};
    s32 second{0};
    s32 wday{4}; ///< 1970-01-01 was a Thursday
};

/// Device local time (localtime_r / localtime_s); falls back to UTC if that fails.
ClockReading LocalClockReading(s64 epoch);
/// UTC breakdown (the tests' deterministic converter).
ClockReading UtcClockReading(s64 epoch);

/// Where the clock points come from. Every member may be empty: the default (system clock, device
/// local time, a game time of 0) is used then.
struct ClockSource {
    std::function<s64()> epoch_now;                  ///< Unix seconds
    std::function<ClockReading(s64)> breakdown;       ///< epoch -> local fields
    std::function<s64()> game_seconds;                ///< seconds since the game started
};

/// Publishes the @clock.* / @game.* points into a snapshot. The breakdown is only recomputed when
/// the epoch second changes (it is a libc call, and the tick runs at 60 Hz).
class ClockPublisher {
public:
    void SetSource(ClockSource source_) {
        source = std::move(source_);
        cached_epoch.reset();
    }
    void Publish(StateSnapshot& snapshot);
    /// How many times the breakdown ran (tests: once per distinct second).
    [[nodiscard]] u64 Breakdowns() const {
        return breakdowns;
    }

private:
    ClockSource source;
    std::optional<s64> cached_epoch;
    ClockReading cached{};
    u64 breakdowns{0};
};

/// The published key names (runtime 16).
namespace ClockKeys {
inline constexpr const char* Hour = "@clock.hour";
inline constexpr const char* Minute = "@clock.minute";
inline constexpr const char* Second = "@clock.second";
inline constexpr const char* Day = "@clock.day";
inline constexpr const char* Month = "@clock.month";
inline constexpr const char* Year = "@clock.year";
inline constexpr const char* Wday = "@clock.wday";
inline constexpr const char* Epoch = "@clock.epoch";
inline constexpr const char* GameSeconds = "@game.seconds";
} // namespace ClockKeys

/// Whether `name` is one of the clock/timer keys (or anything under "@clock." / "@game."):
/// published from the host clock, never from interaction, so not "volatile" for the post-tap
/// derived pass.
[[nodiscard]] bool IsClockKey(const std::string& name);

/// Whether a manifest / data file can read the clock keys: a string or an object key anywhere in
/// it containing "@clock." or "@game." (binds, derived sources, expressions, gates), or a
/// "countdown" key (whose "now" defaults to @clock.epoch).
[[nodiscard]] bool JsonReferencesClockKeys(const nlohmann::json& json);

/// The derived "countdown" form: max(0, target - now), nullopt when either side is missing.
[[nodiscard]] std::optional<f64> EvaluateCountdown(const DerivedPoint& d, const ExprLookup& lookup);

} // namespace Core::Mods
