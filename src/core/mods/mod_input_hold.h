// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Press-and-hold on the second screen (runtime 13): a widget with "on_hold" runs that action once
// when a single finger has stayed down on it, still (within the tap slop), for "hold_ms"
// (default 600 ms). The lift that ends a fired hold is not a tap, so a hold never also runs the
// on_tap of whatever is under the finger. Kept free of ModRuntime so it can be unit tested
// (src/tests/core/mods/runtime13.cpp).

#include <cstdint>

namespace Core::Mods {

inline constexpr std::int32_t DefaultHoldMs = 600;

class HoldTracker {
public:
    /// A new gesture's first finger landed at `now_ms` on page `page`. `has_target`: an on_hold
    /// widget is under it; `hold_ms` its hold time (<= 0 means the default).
    void Down(std::int64_t now_ms, bool has_target, std::int32_t hold_ms, std::uint64_t page = 0) {
        armed = has_target;
        fired = false;
        start_ms = now_ms;
        need_ms = hold_ms > 0 ? hold_ms : DefaultHoldMs;
        start_page = page;
    }
    /// Once per frame while fingers are down. `cancel`: the gesture stopped qualifying (moved past
    /// the slop, a second finger, a drag/scroll/pan took it, a page transition). Returns true on
    /// exactly the frame the hold fires. `page`: the page shown now; a page switch under the still
    /// finger (no transition, e.g. a page bind) cancels too, so the page's action never fires on
    /// the next one.
    bool Update(std::int64_t now_ms, bool cancel, std::uint64_t page = 0) {
        if (!armed || fired) {
            return false;
        }
        if (cancel || page != start_page) {
            armed = false;
            return false;
        }
        if (now_ms - start_ms >= need_ms) {
            fired = true;
            return true;
        }
        return false;
    }
    /// The last finger lifted: true when this gesture fired a hold (so the lift is not a tap).
    bool Up() {
        const bool consumed = fired;
        armed = false;
        fired = false;
        return consumed;
    }
    bool Fired() const {
        return fired;
    }
    bool Armed() const {
        return armed;
    }

private:
    bool armed{false};
    bool fired{false};
    std::int64_t start_ms{0};
    std::int32_t need_ms{DefaultHoldMs};
    std::uint64_t start_page{0};
};

} // namespace Core::Mods
