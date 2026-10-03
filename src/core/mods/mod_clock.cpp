// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 16 clock and timer points (mod_clock.h). Called from ModRuntime::Tick on the tick thread.

#include "core/mods/mod_clock.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Core::Mods {

namespace {
ClockReading FromTm(s64 epoch, const std::tm& tm) {
    ClockReading r;
    r.epoch = epoch;
    r.year = tm.tm_year + 1900;
    r.month = tm.tm_mon + 1;
    r.day = tm.tm_mday;
    r.hour = tm.tm_hour;
    r.minute = tm.tm_min;
    r.second = tm.tm_sec;
    r.wday = tm.tm_wday;
    return r;
}

s64 SystemEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

ClockReading UtcClockReading(s64 epoch) {
    const auto t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &t) != 0) {
        return ClockReading{.epoch = epoch};
    }
#else
    if (gmtime_r(&t, &tm) == nullptr) {
        return ClockReading{.epoch = epoch};
    }
#endif
    return FromTm(epoch, tm);
}

ClockReading LocalClockReading(s64 epoch) {
    const auto t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) {
        return UtcClockReading(epoch);
    }
#else
    if (localtime_r(&t, &tm) == nullptr) {
        return UtcClockReading(epoch);
    }
#endif
    return FromTm(epoch, tm);
}

bool JsonReferencesClockKeys(const nlohmann::json& json) {
    const auto names_clock = [](std::string_view text) {
        return text.find("@clock.") != std::string_view::npos ||
               text.find("@game.") != std::string_view::npos;
    };
    if (json.is_string()) {
        return names_clock(json.get_ref<const std::string&>());
    }
    if (json.is_object()) {
        for (const auto& [key, value] : json.items()) {
            if (key == "countdown" || names_clock(key) || JsonReferencesClockKeys(value)) {
                return true;
            }
        }
        return false;
    }
    if (json.is_array()) {
        for (const auto& value : json) {
            if (JsonReferencesClockKeys(value)) {
                return true;
            }
        }
    }
    return false;
}

bool IsClockKey(const std::string& name) {
    return name.starts_with("@clock.") || name.starts_with("@game.");
}

std::optional<f64> EvaluateCountdown(const DerivedPoint& d, const ExprLookup& lookup) {
    const auto target = d.countdown_target.is_const
                            ? std::optional<f64>(d.countdown_target.const_value)
                            : (d.countdown_target.name.empty() || !lookup
                                   ? std::nullopt
                                   : lookup(d.countdown_target.name));
    const auto now = d.countdown_now.empty() || !lookup ? std::nullopt : lookup(d.countdown_now);
    if (!target || !now) {
        return std::nullopt;
    }
    return std::max(0.0, *target - *now);
}

void ClockPublisher::Publish(StateSnapshot& snapshot) {
    const s64 epoch = source.epoch_now ? source.epoch_now() : SystemEpoch();
    if (!cached_epoch || *cached_epoch != epoch) {
        cached = source.breakdown ? source.breakdown(epoch) : LocalClockReading(epoch);
        cached.epoch = epoch;
        cached_epoch = epoch;
        ++breakdowns;
    }
    snapshot.ints[ClockKeys::Hour] = cached.hour;
    snapshot.ints[ClockKeys::Minute] = cached.minute;
    snapshot.ints[ClockKeys::Second] = cached.second;
    snapshot.ints[ClockKeys::Day] = cached.day;
    snapshot.ints[ClockKeys::Month] = cached.month;
    snapshot.ints[ClockKeys::Year] = cached.year;
    snapshot.ints[ClockKeys::Wday] = cached.wday;
    snapshot.ints[ClockKeys::Epoch] = cached.epoch;
    snapshot.ints[ClockKeys::GameSeconds] = source.game_seconds ? source.game_seconds() : 0;
}

} // namespace Core::Mods
