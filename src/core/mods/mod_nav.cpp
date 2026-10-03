// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 17: controller navigation of the second screen.
//   - The pure parts declared in mod_nav.h: chord parsing, the Controller state machine, focus
//     geometry and nav_order.
//   - ModRuntime::UpdateNav, the input stage after UpdateGestures and before DrainTaps: reads
//     player 1 / handheld from the EmulatedControllers (the real pad, before the HID gate),
//     toggles the mode, moves the focus among the page's tappable widgets (scrolling a list when
//     the next row is out of view), queues A as a tap at the focused widget's centre (so it runs
//     the same DrainTaps path as a finger: hit test, selection, action, haptic), turns the HID
//     gate (hid_core/resources/npad/dsmod_pad_gate.h) on or off, and publishes "@nav.*".
//   - NavOff: leaves the mode (the second screen went away, teardown).
// Threads: the tick thread. tap_mutex for pending_taps.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>

#include <fmt/format.h>

#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/mods/mod_nav.h"
#include "core/mods/mod_runtime.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_util.h"
#include "hid_core/resources/npad/dsmod_pad_gate.h"

namespace Core::Mods::Nav {

namespace {
bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

s32 CentreX(const std::array<s32, 4>& r) {
    return r[0] + r[2] / 2;
}
s32 CentreY(const std::array<s32, 4>& r) {
    return r[1] + r[3] / 2;
}
} // namespace

u64 ButtonBit(std::string_view name) {
    struct Name {
        std::string_view text;
        u64 bit;
    };
    static constexpr Name names[] = {
        {"A", 1ULL << 0},       {"B", 1ULL << 1},       {"X", 1ULL << 2},
        {"Y", 1ULL << 3},       {"LS", BtnStickL},      {"L3", BtnStickL},
        {"StickL", BtnStickL},  {"RS", BtnStickR},      {"R3", BtnStickR},
        {"StickR", BtnStickR},  {"L", 1ULL << 6},       {"R", 1ULL << 7},
        {"ZL", 1ULL << 8},      {"ZR", 1ULL << 9},      {"Plus", 1ULL << 10},
        {"Minus", 1ULL << 11},  {"DLeft", BtnLeft},     {"Left", BtnLeft},
        {"DUp", BtnUp},         {"Up", BtnUp},          {"DRight", BtnRight},
        {"Right", BtnRight},    {"DDown", BtnDown},     {"Down", BtnDown},
    };
    name = Trim(name);
    for (const auto& n : names) {
        if (EqualsNoCase(n.text, name)) {
            return n.bit;
        }
    }
    return 0;
}

std::optional<u64> ParseChord(std::string_view text) {
    u64 mask = 0;
    int parts = 0;
    while (true) {
        const size_t plus = text.find('+');
        const u64 bit = ButtonBit(text.substr(0, plus));
        if (bit == 0) {
            return std::nullopt;
        }
        if ((mask & bit) == 0) {
            ++parts;
        }
        mask |= bit;
        if (plus == std::string_view::npos) {
            break;
        }
        text.remove_prefix(plus + 1);
    }
    if (parts < 2) {
        return std::nullopt;
    }
    return mask;
}

Dir HeldDir(u64 buttons) {
    if (buttons & BtnUp) {
        return Dir::Up;
    }
    if (buttons & BtnDown) {
        return Dir::Down;
    }
    if (buttons & BtnLeft) {
        return Dir::Left;
    }
    if (buttons & BtnRight) {
        return Dir::Right;
    }
    if (buttons & BtnStickLUp) {
        return Dir::Up;
    }
    if (buttons & BtnStickLDown) {
        return Dir::Down;
    }
    if (buttons & BtnStickLLeft) {
        return Dir::Left;
    }
    if (buttons & BtnStickLRight) {
        return Dir::Right;
    }
    return Dir::None;
}

Controller::Events Controller::Step(u64 buttons, s64 now_ms, u64 toggle_mask) {
    Events ev;
    const u64 mask = toggle_mask != 0 ? toggle_mask : DefaultToggleMask;
    const bool chord = (buttons & mask) == mask;
    const bool chord_edge = chord && !chord_prev;
    const u64 pressed = buttons & ~prev;
    chord_prev = chord;
    prev = buttons;
    if (!active) {
        if (chord_edge) {
            active = true;
            ev.entered = true;
            // Whatever is already held (a chord with a direction in it) is not a fresh press.
            held = HeldDir(buttons);
            next_repeat = now_ms + RepeatDelayMs;
        }
        return ev;
    }
    // B leaves unless the chord itself is being completed with it.
    if (chord_edge || ((pressed & BtnB) != 0 && (mask & BtnB) == 0)) {
        active = false;
        ev.left = true;
        held = Dir::None;
        return ev;
    }
    if (chord) {
        return ev; // still holding the chord that opened the mode: nothing else counts
    }
    ev.activate = (pressed & BtnA) != 0;
    const Dir dir = HeldDir(buttons);
    if (dir == Dir::None) {
        held = Dir::None;
    } else if (dir != held) {
        held = dir;
        ev.move = dir;
        next_repeat = now_ms + RepeatDelayMs;
    } else if (now_ms >= next_repeat) {
        ev.move = dir;
        next_repeat = now_ms + RepeatPeriodMs;
    }
    return ev;
}

void Controller::ForceOff() {
    active = false;
    held = Dir::None;
}

std::vector<size_t> Order(std::span<const Candidate> candidates,
                          const std::vector<std::string>& nav_order) {
    std::vector<size_t> order;
    if (nav_order.empty()) {
        order.resize(candidates.size());
        for (size_t i = 0; i < candidates.size(); ++i) {
            order[i] = i;
        }
        return order;
    }
    std::vector<char> taken(candidates.size(), 0);
    for (const auto& name : nav_order) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate& c = candidates[i];
            if (taken[i] == 0 && !name.empty() && (c.id == name || c.template_id == name)) {
                taken[i] = 1;
                order.push_back(i);
            }
        }
    }
    return order;
}

std::optional<size_t> MoveGeometric(std::span<const Candidate> candidates,
                                    std::span<const size_t> allowed, size_t from, Dir dir) {
    if (from >= candidates.size() || dir == Dir::None) {
        return std::nullopt;
    }
    const auto& f = candidates[from].shown;
    const s64 fx = CentreX(f), fy = CentreY(f);
    const bool vertical = dir == Dir::Up || dir == Dir::Down;
    std::optional<size_t> best;
    s64 best_score = std::numeric_limits<s64>::max();
    s64 best_offset = std::numeric_limits<s64>::max();
    for (const size_t i : allowed) {
        if (i == from || i >= candidates.size()) {
            continue;
        }
        const auto& c = candidates[i].shown;
        const s64 cx = CentreX(c), cy = CentreY(c);
        s64 along = 0;
        switch (dir) {
        case Dir::Up:
            along = fy - cy;
            break;
        case Dir::Down:
            along = cy - fy;
            break;
        case Dir::Left:
            along = fx - cx;
            break;
        case Dir::Right:
            along = cx - fx;
            break;
        case Dir::None:
            break;
        }
        if (along <= 0) {
            continue;
        }
        // Distance across the axis between the two spans; 0 while they overlap.
        const s64 a0 = vertical ? f[0] : f[1];
        const s64 a1 = a0 + (vertical ? f[2] : f[3]);
        const s64 b0 = vertical ? c[0] : c[1];
        const s64 b1 = b0 + (vertical ? c[2] : c[3]);
        const s64 across = b1 <= a0 ? a0 - b1 : (b0 >= a1 ? b0 - a1 : 0);
        const s64 score = along + 2 * across;
        // Equal scores (a row of cells over one wide bar): the one most in line wins.
        const s64 offset = vertical ? (cx > fx ? cx - fx : fx - cx) : (cy > fy ? cy - fy : fy - cy);
        if (score < best_score || (score == best_score && offset < best_offset)) {
            best_score = score;
            best_offset = offset;
            best = i;
        }
    }
    return best;
}

std::optional<size_t> MoveOrdered(std::span<const size_t> order, size_t from, Dir dir) {
    if (order.empty() || dir == Dir::None) {
        return std::nullopt;
    }
    const auto it = std::find(order.begin(), order.end(), from);
    if (it == order.end()) {
        return order.front();
    }
    const size_t pos = static_cast<size_t>(it - order.begin());
    const bool forward = dir == Dir::Down || dir == Dir::Right;
    const size_t next = forward ? (pos + 1) % order.size() : (pos + order.size() - 1) % order.size();
    if (next == pos) {
        return std::nullopt;
    }
    return order[next];
}

std::optional<size_t> FirstFocus(std::span<const Candidate> candidates,
                                 std::span<const size_t> order, bool ordered) {
    if (order.empty()) {
        return std::nullopt;
    }
    if (ordered) {
        return order.front();
    }
    size_t best = order.front();
    for (const size_t i : order) {
        const auto& a = candidates[i].shown;
        const auto& b = candidates[best].shown;
        if (a[1] < b[1] || (a[1] == b[1] && a[0] < b[0])) {
            best = i;
        }
    }
    return best;
}

std::optional<size_t> Nearest(std::span<const Candidate> candidates,
                              std::span<const size_t> allowed, s32 cx, s32 cy) {
    std::optional<size_t> best;
    s64 best_d2 = std::numeric_limits<s64>::max();
    for (const size_t i : allowed) {
        const s64 dx = CentreX(candidates[i].shown) - cx;
        const s64 dy = CentreY(candidates[i].shown) - cy;
        const s64 d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

std::optional<size_t> Find(std::span<const Candidate> candidates, size_t source, s64 element) {
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].source == source && candidates[i].element == element) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace Core::Mods::Nav

namespace Core::Mods {

namespace {
s64 NavSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void NavOutLine(const std::string& line) {
    LOG_INFO(Core, "{}", line);
    if (const char* const p = Common::DSMod::DevEnvironment("EDEN_DSMOD_CMD")) {
        std::ofstream f(std::string(p) + ".out", std::ios::app);
        if (f) {
            f << line << '\n';
        }
    }
}

/// What a tap at a widget's centre could land on (DrainTaps' widest hit-test predicate).
bool TapTarget(const Widget& w) {
    return !w.on_tap.empty() || !w.select_group.empty() || !w.drop_action.empty() ||
           w.input_block ||
           (w.type == WidgetType::Map && w.map_extras && w.map_extras->Tappable());
}

/// What the focus can land on: a widget a tap runs something for.
bool NavFocusable(const Widget& w) {
    return !w.on_tap.empty() || !w.select_group.empty() ||
           (!w.drop_action.empty() && !w.accept_group.empty());
}

std::array<s32, 4> Intersect(const std::array<s32, 4>& a, const std::array<s32, 4>& b) {
    const s32 x0 = std::max(a[0], b[0]);
    const s32 y0 = std::max(a[1], b[1]);
    const s32 x1 = std::min(a[0] + a[2], b[0] + b[2]);
    const s32 y1 = std::min(a[1] + a[3], b[1] + b[3]);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}
} // namespace

std::vector<Nav::Candidate> NavCandidates(const Page& page, const StateSnapshot& snapshot,
                                          s32 canvas_w, s32 canvas_h) {
    // Expanded exactly as ExpandWidgets does (same order), remembering each copy's page widget
    // and element so the focus survives a scroll or a moving widget.
    std::vector<Widget> expanded;
    std::vector<size_t> sources;
    std::vector<s64> elements;
    std::vector<Widget> slots;
    std::vector<s64> slot_index;
    for (size_t si = 0; si < page.widgets.size(); ++si) {
        const Widget& source = page.widgets[si];
        if (source.repeat <= 0) {
            Widget w = source;
            w.rect[0] = ResolveBindOffset(source.rect[0], source.x_bind, source.x_scale, snapshot);
            w.rect[1] = ResolveBindOffset(source.rect[1], source.y_bind, source.y_scale, snapshot);
            ApplyAutoWidth(w, snapshot); // auto_w: focus and tap the box as drawn
            expanded.push_back(std::move(w));
            sources.push_back(si);
            elements.push_back(-1);
            continue;
        }
        slot_index.clear();
        const size_t n = ExpandRepeatTemplateInto(page, source, snapshot, slots, &slot_index);
        for (size_t k = 0; k < n; ++k) {
            expanded.push_back(slots[k]);
            sources.push_back(si);
            elements.push_back(k < slot_index.size() ? slot_index[k] : static_cast<s64>(k));
        }
    }
    const std::array<s32, 4> screen{0, 0, canvas_w > 0 ? canvas_w : 1 << 15,
                                    canvas_h > 0 ? canvas_h : 1 << 15};
    std::vector<Nav::Candidate> out;
    for (size_t i = 0; i < expanded.size(); ++i) {
        const Widget& w = expanded[i];
        if (!NavFocusable(w) || w.rect[2] <= 0 || w.rect[3] <= 0 || WidgetHidden(w, snapshot)) {
            continue;
        }
        std::array<s32, 4> shown = Intersect(w.rect, screen);
        if (w.scroll_clip[2] > 0 && w.scroll_clip[3] > 0) {
            shown = Intersect(shown, w.scroll_clip);
        }
        if (shown[2] <= 0 || shown[3] <= 0) {
            continue;
        }
        // A tap at the centre must reach this widget (not a panel or popup drawn over it).
        const s32 cx = shown[0] + shown[2] / 2;
        const s32 cy = shown[1] + shown[3] / 2;
        if (HitTestIndex(expanded, snapshot, cx, cy, TapTarget) != static_cast<s64>(i)) {
            continue;
        }
        Nav::Candidate c;
        c.rect = w.rect;
        c.clip = w.scroll_clip;
        c.shown = shown;
        c.source = sources[i];
        c.element = elements[i];
        c.id = w.id;
        c.template_id = page.widgets[sources[i]].id;
        c.scroll = w.scroll;
        out.push_back(std::move(c));
    }
    return out;
}

u64 ModRuntime::ReadNavPad() const {
    u64 raw = 0;
    for (const auto id : {Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld}) {
        const auto* const controller = system.HIDCore().GetEmulatedController(id);
        if (controller != nullptr && controller->IsConnected()) {
            raw |= static_cast<u64>(controller->GetNpadButtons().raw);
        }
    }
    return raw;
}

void ModRuntime::LatchNavPad() const {
    for (const auto id : {Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld}) {
        const auto* const controller = system.HIDCore().GetEmulatedController(id);
        if (controller != nullptr && controller->IsConnected()) {
            Core::HID::DSModPadGate::Latch(Service::HID::NpadIdTypeToIndex(id),
                                           static_cast<u64>(controller->GetNpadButtons().raw));
        }
    }
}

void ModRuntime::NavOff(const char* why) {
    if (nav_state.controller.Active()) {
        nav_state.controller.ForceOff();
        NavOutLine(fmt::format("DSMod nav: off ({})", why));
    }
    if (nav_state.gate_on) {
        LatchNavPad();
        Core::HID::DSModPadGate::Set(false);
        nav_state.gate_on = false;
    }
    nav_state.has_focus = false;
}

void ModRuntime::UpdateNav(StateSnapshot& snapshot) {
    if (!manifest.nav.enabled || current_page >= manifest.pages.size()) {
        NavOff("disabled");
        return;
    }
    // What the companion itself holds for the game (a tapped button action) goes through the
    // gate and is not read as navigation.
    u64 pass = 0;
    for (const auto& held : held_buttons) {
        for (size_t at = 0; at <= held.name.size();) {
            const size_t plus = held.name.find('+', at);
            const size_t end = plus == std::string::npos ? held.name.size() : plus;
            pass |= Nav::ButtonBit(std::string_view{held.name}.substr(at, end - at));
            at = end + 1;
        }
    }
    bool pass_left = false, pass_right = false;
    for (const auto& held : held_sticks) {
        (held.right ? pass_right : pass_left) = true;
    }
    const u64 raw = ReadNavPad();
    const u64 buttons = raw & ~pass;
    const auto ev = nav_state.controller.Step(buttons, NavSteadyMs(), manifest.nav.toggle_mask);
    if (ev.entered) {
        // Nothing to focus on this page: stay out (the game keeps its pad, the chord included).
        const Page& entry_page = manifest.pages[current_page];
        const auto entry_cands =
            NavCandidates(entry_page, snapshot, static_cast<s32>(canvas.Width()),
                          static_cast<s32>(canvas.Height()));
        if (Nav::Order(entry_cands, entry_page.nav_order).empty()) {
            nav_state.controller.ForceOff();
            NavOutLine(fmt::format("DSMod nav: not entered (page '{}' has nothing to focus)",
                                   entry_page.id));
            snapshot.ints["@nav.active"] = 0;
            return;
        }
        nav_state.has_focus = false;
        NavOutLine(fmt::format("DSMod nav: on (page '{}')", manifest.pages[current_page].id));
    }
    if (ev.left) {
        NavOutLine("DSMod nav: off");
        LatchNavPad(); // the chord / B stay hidden until released
        Core::HID::DSModPadGate::Set(false);
        nav_state.gate_on = false;
        nav_state.has_focus = false;
    }
    if (!nav_state.controller.Active()) {
        snapshot.ints["@nav.active"] = 0;
        return;
    }
    Core::HID::DSModPadGate::Set(true, pass, pass_left, pass_right);
    nav_state.gate_on = true;

    const Page& page = manifest.pages[current_page];
    if (nav_state.page != current_page) {
        nav_state.page = current_page; // a new page starts at its first widget
        nav_state.has_focus = false;
    }
    const s32 cw = static_cast<s32>(canvas.Width());
    const s32 ch = static_cast<s32>(canvas.Height());
    const bool ordered = !page.nav_order.empty();
    std::vector<Nav::Candidate> cands = NavCandidates(page, snapshot, cw, ch);
    std::vector<size_t> order = Nav::Order(cands, page.nav_order);
    std::optional<size_t> focus;
    const auto resolve = [&] {
        focus.reset();
        if (nav_state.has_focus) {
            focus = Nav::Find(cands, nav_state.source, nav_state.element);
            if (focus && std::find(order.begin(), order.end(), *focus) == order.end()) {
                focus.reset();
            }
            if (!focus) {
                // Gone (hidden, scrolled away): the nearest one takes over.
                const auto& r = nav_state.last_shown;
                focus = Nav::Nearest(cands, order, r[0] + r[2] / 2, r[1] + r[3] / 2);
            }
        }
        if (!focus) {
            focus = Nav::FirstFocus(cands, order, ordered);
        }
    };
    const auto rebuild = [&] {
        cands = NavCandidates(page, snapshot, cw, ch);
        order = Nav::Order(cands, page.nav_order);
        resolve();
    };
    const auto remember = [&] {
        if (focus) {
            nav_state.has_focus = true;
            nav_state.source = cands[*focus].source;
            nav_state.element = cands[*focus].element;
            nav_state.last_shown = cands[*focus].shown;
        }
    };
    // Moves the scroll region `id` by `delta` px; false when it is already at that end.
    const auto scroll_by = [&](const std::string& id, s32 delta) {
        const ScrollRegion* const region = FindScrollRegion(page, id);
        if (region == nullptr || delta == 0) {
            return false;
        }
        auto& st = scroll_state[id];
        const s32 max_offset = MeasureScroll(page, *region, snapshot).max_offset;
        const float before = st.offset;
        st.offset = std::clamp(st.offset + static_cast<float>(delta), 0.0f,
                               static_cast<float>(std::max(0, max_offset)));
        st.flinging = false;
        st.velocity = 0.0f;
        if (st.offset == before) {
            return false;
        }
        snapshot.ints[ScrollOffsetKey(id)] = static_cast<s64>(std::lround(st.offset));
        return true;
    };
    resolve();
    const bool transition = page_anim.active || page_anim_request.has_value();
    if (ev.move != Nav::Dir::None && focus && !transition) {
        std::optional<size_t> target = ordered ? Nav::MoveOrdered(order, *focus, ev.move)
                                               : Nav::MoveGeometric(cands, order, *focus, ev.move);
        // The next row of a list is not materialised until it scrolls into view: scroll one row
        // and look again.
        const bool vertical = ev.move == Nav::Dir::Up || ev.move == Nav::Dir::Down;
        if (!ordered && vertical && !cands[*focus].scroll.empty()) {
            const std::string region_id = cands[*focus].scroll;
            const bool leaves_list = !target || cands[*target].scroll != region_id;
            const ScrollRegion* const region = FindScrollRegion(page, region_id);
            if (leaves_list && region != nullptr) {
                const s32 row = std::max(1, MeasureScroll(page, *region, snapshot).row_h);
                remember();
                if (scroll_by(region_id, ev.move == Nav::Dir::Down ? row : -row)) {
                    rebuild();
                    target = focus ? Nav::MoveGeometric(cands, order, *focus, ev.move)
                                   : std::nullopt;
                }
            }
        }
        if (target && target != focus) {
            focus = target;
            if (manifest.nav.haptic >= 0) {
                QueueHaptic(HapticKind::Tap, manifest.nav.haptic, -1, "nav move");
            }
        }
    }
    // Keep a focused list row wholly inside its viewport.
    if (focus && !cands[*focus].scroll.empty()) {
        const auto& c = cands[*focus];
        s32 delta = 0;
        if (c.rect[1] < c.clip[1]) {
            delta = c.rect[1] - c.clip[1];
        } else if (c.rect[1] + c.rect[3] > c.clip[1] + c.clip[3]) {
            delta = std::min(c.rect[1] + c.rect[3] - (c.clip[1] + c.clip[3]), c.rect[1] - c.clip[1]);
        }
        if (delta != 0) {
            remember();
            if (scroll_by(c.scroll, delta)) {
                rebuild();
            }
        }
    }
    remember();
    if (ev.activate && focus && !transition) {
        const auto& c = cands[*focus];
        const s32 x = c.shown[0] + c.shown[2] / 2;
        const s32 y = c.shown[1] + c.shown[3] / 2;
        NavOutLine(fmt::format("DSMod nav: A on '{}' -> tap ({},{})",
                               c.id.empty() ? fmt::format("#{}", c.source) : c.id, x, y));
        std::scoped_lock lk{tap_mutex};
        pending_taps.push_back(PendingTap{x, y, {}, {}});
    }
    snapshot.ints["@nav.active"] = 1;
    snapshot.ints["@nav.count"] = static_cast<s64>(order.size());
    s64 index = -1;
    std::array<s32, 4> shown{};
    if (focus) {
        shown = cands[*focus].shown;
        index = static_cast<s64>(std::find(order.begin(), order.end(), *focus) - order.begin());
    }
    snapshot.ints["@nav.index"] = index;
    snapshot.ints["@nav.x"] = shown[0];
    snapshot.ints["@nav.y"] = shown[1];
    snapshot.ints["@nav.w"] = shown[2];
    snapshot.ints["@nav.h"] = shown[3];
}

} // namespace Core::Mods
