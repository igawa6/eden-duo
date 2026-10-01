// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Horizontal swipe on the second screen (runtime 14): a widget with "on_swipe_left" /
// "on_swipe_right" runs that action once when a single finger that landed on it travels at least
// "swipe_px" (default 60 canvas px) sideways, mostly horizontally (|dx| > 2 |dy|), and lifts.
// Vertical swipe (unreleased runtime 15 addition): "on_swipe_up" / "on_swipe_down" mirror them on
// the other axis (|dy| > 2 |dx|, same distance, speed and cancel rules). UpdateGestures never arms
// a vertical swipe over a scroll region that can scroll (the list owns vertical drags there).
//   - Direction lock: when the finger first leaves the 12 px tap slop, the gesture is judged. A
//     horizontal start (|dx| > 2 |dy|) makes it a horizontal swipe candidate, a vertical start
//     (|dy| > 2 |dx|) a vertical one, each only when the widget has an action on that axis; the
//     swipe then owns the gesture (a scroll region beneath does not start scrolling); anything
//     else disarms the swipe for good, so vertical drags keep scrolling lists.
//   - Speed: the threshold must be reached within SwipeMaxMs (600 ms) of that lock; a slow drag
//     is not a swipe.
//   - Fires once, at the lift, when the lift point still qualifies (distance and dominance). The
//     lift that ends a fired swipe is not a tap (it moved past the slop anyway).
//   - Cancelled by a second finger, a page transition or page change, a fired hold, a drag.
// Kept free of ModRuntime so it can be unit tested (src/tests/core/mods/runtime14.cpp; the
// vertical axis in runtime15.cpp).

#include <cstdint>
#include <cstdlib>

#include "core/mods/mod_types_page.h"

namespace Core::Mods {

inline constexpr std::int32_t DefaultSwipePx = 60;
inline constexpr std::int64_t SwipeMaxMs = 600;
/// Travel from the landing point that ends the tap slop and locks the swipe's direction (the
/// same 12 px as UpdateGestures' TapSlop).
inline constexpr std::int32_t SwipeLockSlop = 12;

enum class SwipeDir : std::uint8_t { None, Left, Right, Up, Down };

/// Widgets the swipe hit test stops at (topmost first): swipe widgets, and those that keep the
/// swipe from reaching anything beneath -- an input_block, a map, a pan_zoom widget. Chosen
/// independently of on_tap, like on_hold: a card-sized swipe area under small buttons gets the
/// swipe while the buttons keep their taps.
inline bool SwipeHitFilter(const Widget& w) {
    return !w.on_swipe_left.empty() || !w.on_swipe_right.empty() || !w.on_swipe_up.empty() ||
           !w.on_swipe_down.empty() || w.input_block || w.pan_zoom || w.type == WidgetType::Map;
}
/// The widget the swipe hit test found arms the swipe: it names a swipe action and is not an
/// input_block, a map or a pan_zoom widget (a map pans; a swipe there would fight it).
inline bool SwipeArms(const Widget& w) {
    return (!w.on_swipe_left.empty() || !w.on_swipe_right.empty() || !w.on_swipe_up.empty() ||
            !w.on_swipe_down.empty()) &&
           !w.input_block && !w.pan_zoom && w.type != WidgetType::Map;
}

class SwipeTracker {
public:
    /// A new gesture's first finger landed at (x, y) at `now_ms` on page `page`. `left` / `right`
    /// / `up` / `down`: the swipe widget under it has that on_swipe_* action (all false = not
    /// armed). `swipe_px` <= 0 means DefaultSwipePx.
    void Down(std::int64_t now_ms, std::int32_t x, std::int32_t y, bool left, bool right,
              std::int32_t swipe_px, std::uint64_t page = 0, bool up = false, bool down = false) {
        armed = left || right || up || down;
        has_left = left;
        has_right = right;
        has_up = up;
        has_down = down;
        horizontal = false;
        vertical = false;
        locked = false;
        reached = false;
        x0 = x;
        y0 = y;
        last_x = x;
        last_y = y;
        lock_ms = now_ms;
        need_px = swipe_px > 0 ? swipe_px : DefaultSwipePx;
        start_page = page;
    }
    /// Once per frame while fingers are down, with the first finger's position. `cancel`: the
    /// gesture stopped qualifying (a second finger, a drag, a page transition, a fired hold).
    void Move(std::int64_t now_ms, std::int32_t x, std::int32_t y, bool cancel,
              std::uint64_t page = 0) {
        if (!armed) {
            return;
        }
        if (cancel || page != start_page) {
            armed = false;
            return;
        }
        last_x = x;
        last_y = y;
        const std::int64_t dx = static_cast<std::int64_t>(x) - x0;
        const std::int64_t dy = static_cast<std::int64_t>(y) - y0;
        if (!locked) {
            if (dx * dx + dy * dy <= static_cast<std::int64_t>(SwipeLockSlop) * SwipeLockSlop) {
                return; // still inside the tap slop
            }
            locked = true;
            lock_ms = now_ms;
            horizontal = (has_left || has_right) && Dominant(dx, dy);
            vertical = (has_up || has_down) && Dominant(dy, dx);
            if (!horizontal && !vertical) {
                // a start on an axis without an action (or diagonal): a scroll or nothing
                armed = false;
                return;
            }
        }
        const bool far = horizontal ? std::llabs(dx) >= need_px && Dominant(dx, dy)
                                    : std::llabs(dy) >= need_px && Dominant(dy, dx);
        if (!reached && far) {
            if (now_ms - lock_ms > SwipeMaxMs) {
                armed = false; // too slow: a drag, not a swipe
                return;
            }
            reached = true;
        }
    }
    /// The last finger lifted. Returns the direction of a fired swipe (only a direction the widget
    /// has an action for), else None; resets for the next gesture.
    SwipeDir Up() {
        SwipeDir dir = SwipeDir::None;
        if (armed && reached) {
            const std::int64_t dx = static_cast<std::int64_t>(last_x) - x0;
            const std::int64_t dy = static_cast<std::int64_t>(last_y) - y0;
            if (horizontal && std::llabs(dx) >= need_px && Dominant(dx, dy)) {
                if (dx < 0 && has_left) {
                    dir = SwipeDir::Left;
                } else if (dx > 0 && has_right) {
                    dir = SwipeDir::Right;
                }
            } else if (vertical && std::llabs(dy) >= need_px && Dominant(dy, dx)) {
                if (dy < 0 && has_up) {
                    dir = SwipeDir::Up;
                } else if (dy > 0 && has_down) {
                    dir = SwipeDir::Down;
                }
            }
        }
        armed = false;
        reached = false;
        locked = false;
        horizontal = false;
        vertical = false;
        return dir;
    }
    bool Armed() const {
        return armed;
    }
    /// The gesture left the slop on an axis the armed swipe has an action for: the swipe owns
    /// it, so a scroll region under the finger must not start a vertical drag (stays true for the
    /// rest of the gesture even if the swipe later disarms, so the list does not jump
    /// mid-gesture).
    bool OwnsGesture() const {
        return horizontal || vertical;
    }

private:
    /// `a` dominates `b`: |a| > 2 |b|.
    static bool Dominant(std::int64_t a, std::int64_t b) {
        return std::llabs(a) > 2 * std::llabs(b);
    }

    bool armed{false};
    bool has_left{false};
    bool has_right{false};
    bool has_up{false};
    bool has_down{false};
    bool locked{false};
    bool horizontal{false};
    bool vertical{false};
    bool reached{false};
    std::int32_t x0{0};
    std::int32_t y0{0};
    std::int32_t last_x{0};
    std::int32_t last_y{0};
    std::int64_t lock_ms{0};
    std::int32_t need_px{DefaultSwipePx};
    std::uint64_t start_page{0};
};

} // namespace Core::Mods
