// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Actions: what a tap, a drop, a page bind, an enforce rule or a console command makes happen.
//   - RunAction: runs one manifest Action by kind -- write / slot_write (guest memory through
//     WritePointValue, mod_state.cpp), button (virtual-pad presses), page (switch, optionally
//     animated), call / sequence (guest-call bridge, mod_guest_bridge.cpp), flag / set_value,
//     view_reset, module (the native module's on_action) and map_select.
//   - Action operands: ResolveActionRef ("$name"), ExpandActionName ("{$name}"),
//     ResolveElementPoint ("<array><i>"), FindWidgetRect (transition origins).
//   - Virtual gamepad: ParseButton, PressToken, UpdateHeldButtons (ages presses and stick holds),
//     ReleaseHeldInputs (teardown).
//   - Headless input drivers: DriveInputScript (EDEN_DSMOD_INPUT) and DriveLiveInput
//     (EDEN_DSMOD_INPUT_LIVE).
// Not here: deciding which action a touch runs (DrainTaps, mod_input.cpp), automatic page switches
// (DrivePageBinds, mod_pages.cpp), haptics (mod_input.cpp).
// Flow: the input/actions stage. RunAction is called from DrainTaps (mod_input.cpp),
// DrivePageBinds (mod_pages.cpp) and ApplyEnforceRules (mod_state.cpp); the drivers and
// UpdateHeldButtons run at the top of Tick, before sampling. Everything here runs on the tick
// thread; view_mutex is taken for view_reset. ParseButton has external linkage (declared in
// mod_runtime_shared.h) for Tick's autostart presses and the dev-tools console.

#include <algorithm>
#include <array>
#include <cctype>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "common/logging.h"
#include "core/core.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "input_common/drivers/virtual_gamepad.h"

namespace Core::Mods {

InputCommon::VirtualGamepad::VirtualButton ParseButton(const std::string& name, bool& ok) {
    using VB = InputCommon::VirtualGamepad::VirtualButton;
    ok = true;
    if (name == "A")
        return VB::ButtonA;
    if (name == "B")
        return VB::ButtonB;
    if (name == "X")
        return VB::ButtonX;
    if (name == "Y")
        return VB::ButtonY;
    if (name == "L")
        return VB::TriggerL;
    if (name == "R")
        return VB::TriggerR;
    if (name == "ZL")
        return VB::TriggerZL;
    if (name == "ZR")
        return VB::TriggerZR;
    if (name == "Plus")
        return VB::ButtonPlus;
    if (name == "Minus")
        return VB::ButtonMinus;
    if (name == "DUp")
        return VB::ButtonUp;
    if (name == "DDown")
        return VB::ButtonDown;
    if (name == "DLeft")
        return VB::ButtonLeft;
    if (name == "DRight")
        return VB::ButtonRight;
    ok = false;
    return VB::ButtonA;
}

std::optional<ModRuntime::ActionScalar> ModRuntime::ResolveActionRef(
    const std::string& ref, const StateSnapshot& snapshot, std::optional<s64> payload) const {
    const auto from_float = [](f64 f) { return ActionScalar{static_cast<s64>(f), f}; };
    if (ref == "payload") {
        return payload ? std::optional<ActionScalar>{ActionScalar{*payload, std::nullopt}}
                       : std::nullopt;
    }
    if (ref == "map_x" || ref == "map_y" || ref == "world_x" || ref == "world_y") {
        if (!map_tap_ctx.active) {
            return std::nullopt;
        }
        return from_float(ref == "map_x"     ? map_tap_ctx.map_x
                          : ref == "map_y"   ? map_tap_ctx.map_y
                          : ref == "world_x" ? map_tap_ctx.world_x
                                             : map_tap_ctx.world_y);
    }
    if (ref == "marker_index") {
        if (!map_tap_ctx.active || map_tap_ctx.marker_index < 0) {
            return std::nullopt;
        }
        return ActionScalar{map_tap_ctx.marker_index, std::nullopt};
    }
    // Live runtime state first: the snapshot of this tick predates the taps being handled.
    const auto live = [&](const std::string& prefix,
                          const std::unordered_map<std::string, s64>& table,
                          s64 missing) -> std::optional<ActionScalar> {
        if (!ref.starts_with(prefix)) {
            return std::nullopt;
        }
        const auto it = table.find(ref.substr(prefix.size()));
        return ActionScalar{it == table.end() ? missing : it->second, std::nullopt};
    };
    if (auto v = live("@flag:", flags, 0)) {
        return v;
    }
    if (auto v = live("@map_sel:", map_selections, -1)) {
        return v;
    }
    if (auto v = live("@sel:", selections, -1)) {
        return v;
    }
    if (ref.starts_with("@last:")) {
        if (auto v = live("@last:", last_selection, -1); v && v->i != -1) {
            return v;
        }
    }
    if (const auto f = snapshot.floats.find(ref); f != snapshot.floats.end()) {
        return from_float(f->second);
    }
    if (const auto i = snapshot.ints.find(ref); i != snapshot.ints.end()) {
        return ActionScalar{i->second, std::nullopt};
    }
    return std::nullopt;
}

std::optional<std::string> ModRuntime::ExpandActionName(const std::string& text,
                                                        const StateSnapshot& snapshot,
                                                        std::optional<s64> payload) const {
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t open = text.find("{$", pos);
        if (open == std::string::npos) {
            out.append(text, pos, std::string::npos);
            return out;
        }
        const size_t close = text.find('}', open);
        if (close == std::string::npos) {
            return std::nullopt;
        }
        const std::string name = text.substr(open + 2, close - open - 2);
        auto v = ResolveActionRef(name, snapshot, payload);
        if (!v && !name.starts_with('@')) {
            v = ResolveActionRef("@" + name, snapshot, payload); // {$map_sel:pins}
        }
        if (!v || v->i < 0) {
            return std::nullopt;
        }
        out.append(text, pos, open - pos);
        out += std::to_string(v->i);
        pos = close + 1;
    }
}

const DataPoint* ModRuntime::ResolveElementPoint(const std::string& name, s64& index) const {
    index = 0;
    if (const auto it = manifest.points.find(name); it != manifest.points.end()) {
        return &it->second;
    }
    size_t digits = name.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) {
        --digits;
    }
    if (digits == 0 || digits == name.size() || name.size() - digits > 9) {
        return nullptr;
    }
    const auto base = manifest.points.find(name.substr(0, digits));
    if (base == manifest.points.end()) {
        return nullptr;
    }
    const s64 i = std::stoll(name.substr(digits));
    if (i >= std::max<s64>(1, base->second.count) && base->second.count_bind.empty()) {
        return nullptr;
    }
    index = i;
    return &base->second;
}

bool ModRuntime::PressToken(const std::string& name, u32 frames) {
    auto* const pad = system.GetInputSubsystem() != nullptr
                          ? system.GetInputSubsystem()->GetVirtualGamepad()
                          : nullptr;
    if (pad == nullptr || name.empty()) {
        return false;
    }
    // "RX+", "LY-" and friends name a stick shove rather than a button.
    if (name.size() >= 3 && (name[0] == 'L' || name[0] == 'R') &&
        (name[1] == 'X' || name[1] == 'Y') && (name.back() == '+' || name.back() == '-')) {
        const bool right = name[0] == 'R';
        const float v = name.back() == '+' ? 1.0f : -1.0f;
        const float sx = name[1] == 'X' ? v : 0.0f;
        const float sy = name[1] == 'Y' ? v : 0.0f;
        const auto which = right ? InputCommon::VirtualGamepad::VirtualStick::Right
                                 : InputCommon::VirtualGamepad::VirtualStick::Left;
        for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
            pad->SetStickPosition(pl, which, sx, sy);
        }
        const u32 hold = std::max(2u, frames);
        if (auto it = std::find_if(held_sticks.begin(), held_sticks.end(),
                                   [right](const HeldStick& held) { return held.right == right; });
            it != held_sticks.end()) {
            it->ticks_left = std::max(it->ticks_left, hold);
        } else {
            held_sticks.push_back(HeldStick{right, hold});
        }
        return true;
    }
    bool ok{};
    const auto button = ParseButton(name, ok);
    if (!ok) {
        return false;
    }
    for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
        pad->SetButtonState(pl, button, true);
    }
    const u32 hold = std::max(2u, frames);
    if (auto it = std::find_if(held_buttons.begin(), held_buttons.end(),
                               [&name](const HeldButton& held) { return held.name == name; });
        it != held_buttons.end()) {
        it->ticks_left = std::max(it->ticks_left, hold);
    } else {
        held_buttons.push_back(HeldButton{name, hold});
    }
    return true;
}

void ModRuntime::ReleaseHeldInputs() {
    auto* const pad = system.GetInputSubsystem() != nullptr
                          ? system.GetInputSubsystem()->GetVirtualGamepad()
                          : nullptr;
    if (pad != nullptr) {
        for (const auto& held : held_buttons) {
            bool ok{};
            const auto button = ParseButton(held.name, ok);
            if (!ok) {
                continue;
            }
            for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
                pad->SetButtonState(pl, button, false);
            }
        }
        for (const auto& held : held_sticks) {
            const auto which = held.right ? InputCommon::VirtualGamepad::VirtualStick::Right
                                          : InputCommon::VirtualGamepad::VirtualStick::Left;
            for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
                pad->SetStickPosition(pl, which, 0.0f, 0.0f);
            }
        }
        for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
            if (live_held_button >= 0) {
                pad->SetButtonState(pl, live_held_button, false);
            }
            if (live_held_stick) {
                pad->SetStickPosition(pl,
                                      live_stick_right
                                          ? InputCommon::VirtualGamepad::VirtualStick::Right
                                          : InputCommon::VirtualGamepad::VirtualStick::Left,
                                      0.0f, 0.0f);
            }
        }
    }
    held_buttons.clear();
    held_sticks.clear();
    press_queue.clear();
    live_hold_left = 0;
    live_held_button = -1;
    live_held_stick = false;
    live_held_x = live_held_y = 0.0f;
}

void ModRuntime::UpdateHeldButtons() {
    if (!press_queue.empty()) {
        auto* const pad = system.GetInputSubsystem() != nullptr
                              ? system.GetInputSubsystem()->GetVirtualGamepad()
                              : nullptr;
        for (auto it = press_queue.begin(); it != press_queue.end();) {
            if (it->at_tick > tick_count) {
                ++it;
                continue;
            }
            PressToken(it->button, it->frames);
            it = press_queue.erase(it);
        }
    }
    if (!held_sticks.empty()) {
        auto* const spad = system.GetInputSubsystem() != nullptr
                               ? system.GetInputSubsystem()->GetVirtualGamepad()
                               : nullptr;
        for (auto& hs : held_sticks) {
            if (hs.ticks_left > 0) {
                --hs.ticks_left;
            }
            if (hs.ticks_left == 0 && spad != nullptr) {
                const auto which = hs.right ? InputCommon::VirtualGamepad::VirtualStick::Right
                                            : InputCommon::VirtualGamepad::VirtualStick::Left;
                for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
                    spad->SetStickPosition(pl, which, 0.0f, 0.0f);
                }
            }
        }
        std::erase_if(held_sticks, [](const HeldStick& h) { return h.ticks_left == 0; });
    }
    if (held_buttons.empty()) {
        return;
    }
    auto* pad =
        system.GetInputSubsystem() ? system.GetInputSubsystem()->GetVirtualGamepad() : nullptr;
    for (auto& held : held_buttons) {
        if (held.ticks_left > 0) {
            --held.ticks_left;
        }
        if (held.ticks_left == 0 && pad) {
            bool ok{};
            const auto button = ParseButton(held.name, ok);
            if (ok) {
                pad->SetButtonState(0, button, false);
                pad->SetButtonState(8, button, false);
            }
        }
    }
    std::erase_if(held_buttons, [](const HeldButton& h) { return h.ticks_left == 0; });
}

std::optional<std::array<s32, 4>> ModRuntime::FindWidgetRect(size_t page_index,
                                                             const std::string& id,
                                                             const StateSnapshot& snapshot) const {
    if (id.empty() || page_index >= manifest.pages.size()) {
        return std::nullopt;
    }
    return WidgetRectById(ExpandWidgets(manifest.pages[page_index], snapshot), id);
}

ModRuntime::ActionResult ModRuntime::RunAction(const Action& action, const StateSnapshot& snapshot,
                                               std::optional<s64> payload,
                                               const std::array<s32, 4>* tap_origin) {
    const auto trace = [](const std::string& line) {
        if (const char* const p = std::getenv("EDEN_DSMOD_CMD")) {
            std::ofstream f(std::string(p) + ".out", std::ios::app);
            if (f) {
                f << line << '\n';
            }
        }
    };
    if (!action.enabled.Empty()) {
        std::optional<f64> gate;
        if (const auto v = ResolveActionRef(action.enabled.point, snapshot, std::nullopt)) {
            gate = v->f.value_or(static_cast<f64>(v->i));
        }
        // Fail closed: a gate that cannot be read (missing point, unpublished derived value)
        // refuses the action whatever its negation -- a write lock must never open by accident.
        const bool open = gate.has_value() && (*gate != 0.0) != action.enabled.negate;
        if (!open) {
            const std::string line =
                fmt::format("DSMod: action '{}' refused: enabled_bind '{}{}' = {}", action.name,
                            action.enabled.negate ? "!" : "", action.enabled.point,
                            gate ? fmt::format("{}", *gate) : std::string{"missing"});
            LOG_INFO(Core, "{}", line);
            trace(line);
            return ActionResult::Refused;
        }
    }
    if (action.value_from_payload && !payload.has_value()) {
        LOG_WARNING(Core, "DSMod: action '{}' wants $payload but none was carried",
                    action.name.empty()
                        ? (action.kind == ActionKind::Module ? action.module_action : action.point)
                        : action.name);
        return ActionResult::Skipped;
    }
    // The value a write / flag / select names: payload, a "$" source, or the constant.
    const auto action_value = [&]() -> std::optional<ActionScalar> {
        if (action.value_from_payload) {
            return ActionScalar{*payload, std::nullopt};
        }
        if (!action.value_ref.empty()) {
            auto v = ResolveActionRef(action.value_ref, snapshot, payload);
            if (!v) {
                LOG_WARNING(Core, "DSMod: action '{}': value '${}' is not available", action.name,
                            action.value_ref);
            }
            return v;
        }
        return ActionScalar{action.value, action.value_float};
    };
    switch (action.kind) {
    case ActionKind::SlotWrite: {
        const auto slot = manifest.points.find(action.slot);
        if (slot == manifest.points.end()) {
            LOG_WARNING(Core, "DSMod: slot_write '{}': no point '{}'", action.name, action.slot);
            return ActionResult::Skipped;
        }
        const s64 n = action.count > 0 ? action.count : std::max<s64>(1, slot->second.count);
        const s64 free_value = NormaliseToType(slot->second.type, action.free_value);
        s64 found = -1;
        s64 used = 0;
        for (s64 i = 0; i < n; ++i) {
            s64 v{};
            if (!ReadPoint(slot->second, v, i)) {
                continue;
            }
            if (NormaliseToType(slot->second.type, v) == free_value) {
                if (found < 0) {
                    found = i;
                }
            } else {
                ++used;
            }
        }
        if (found < 0) {
            const std::string line =
                fmt::format("DSMod: slot_write '{}' refused: no free slot in '{}' ({} of {} used)",
                            action.name, action.slot, used, n);
            LOG_INFO(Core, "{}", line);
            trace(line);
            return ActionResult::Refused;
        }
        struct Planned {
            const DataPoint* point;
            s64 index;
            ActionScalar value;
            std::string name;
        };
        std::vector<Planned> plan;
        for (const auto& [pattern, source] : action.writes) {
            std::string name = pattern;
            bool indexed = false;
            if (const size_t at = name.find("{i}"); at != std::string::npos) {
                name.replace(at, 3, std::to_string(found));
                indexed = true;
            }
            if (name.find("{$") != std::string::npos) {
                const auto expanded = ExpandActionName(name, snapshot, payload);
                if (!expanded) {
                    LOG_WARNING(Core, "DSMod: slot_write '{}': '{}' has no usable index",
                                action.name, pattern);
                    return ActionResult::Skipped;
                }
                name = *expanded;
                indexed = true;
            }
            s64 index = found;
            const DataPoint* point = nullptr;
            if (indexed) {
                point = ResolveElementPoint(name, index);
            } else if (const auto it = manifest.points.find(name); it != manifest.points.end()) {
                point = &it->second;
                name += std::to_string(found);
            }
            if (point == nullptr) {
                LOG_WARNING(Core, "DSMod: slot_write '{}': no point '{}'", action.name, name);
                return ActionResult::Skipped;
            }
            std::optional<ActionScalar> value = ActionScalar{source.value, source.as_float};
            if (!source.ref.empty()) {
                value = ResolveActionRef(source.ref, snapshot, payload);
            }
            if (!value) {
                LOG_WARNING(Core, "DSMod: slot_write '{}': value '${}' is not available",
                            action.name, source.ref);
                return ActionResult::Skipped;
            }
            plan.push_back({point, index, *value, name});
        }
        std::string written;
        for (const auto& p : plan) {
            if (!WritePointValue(*p.point, p.value.i, p.index, p.value.f)) {
                LOG_WARNING(Core, "DSMod: slot_write '{}': writing '{}' failed", action.name,
                            p.name);
                return ActionResult::Skipped;
            }
            written += p.value.f ? fmt::format(" {}={:.3f}", p.name, *p.value.f)
                                 : fmt::format(" {}={}", p.name, p.value.i);
        }
        if (!action.select.empty()) {
            map_selections[action.select] = found;
            map_selection_tick[action.select] = tick_count;
        }
        const std::string line =
            fmt::format("DSMod: slot_write '{}' slot {}:{}", action.name, found, written);
        LOG_INFO(Core, "{}", line);
        trace(line);
        return ActionResult::Done;
    }
    case ActionKind::MapSelect: {
        const auto v = action_value();
        if (!v || action.group.empty()) {
            return ActionResult::Skipped;
        }
        if (v->i < 0) {
            map_selections.erase(action.group);
        } else {
            map_selections[action.group] = v->i;
            map_selection_tick[action.group] = tick_count;
        }
        LOG_INFO(Core, "DSMod: map selection '{}' = {}", action.group, v->i < 0 ? -1 : v->i);
        return ActionResult::Done;
    }
    case ActionKind::Module:
        RunModuleAction(action.module_action, action.value_from_payload ? *payload : action.value);
        break;
    case ActionKind::Write: {
        std::string point_name = action.point;
        if (point_name.find("{$") != std::string::npos) {
            const auto expanded = ExpandActionName(point_name, snapshot, payload);
            if (!expanded) {
                const std::string line =
                    fmt::format("DSMod: action '{}' skipped: '{}' has no usable index", action.name,
                                action.point);
                LOG_INFO(Core, "{}", line);
                trace(line);
                return ActionResult::Skipped;
            }
            point_name = *expanded;
        }
        s64 element = 0;
        const DataPoint* const target_ptr = ResolveElementPoint(point_name, element);
        if (target_ptr == nullptr) {
            return ActionResult::Skipped;
        }
        const auto resolved = action_value();
        if (!resolved) {
            return ActionResult::Skipped;
        }
        const s64 value = resolved->i;
        const DataPoint& target = *target_ptr;
        if (!action.swap_point.empty()) {
            // An exclusive pair (LA's X/Y): equipping what the other slot holds moves this slot's
            // old value over there instead of leaving the item on both.
            const auto other = manifest.points.find(action.swap_point);
            s64 previous{};
            s64 other_value{};
            if (other != manifest.points.end() && ReadPoint(target, previous, element) &&
                ReadPoint(other->second, other_value) &&
                NormaliseToType(other->second.type, other_value) ==
                    NormaliseToType(other->second.type, value) &&
                NormaliseToType(target.type, previous) != NormaliseToType(target.type, value)) {
                if (WritePointValue(other->second, previous)) {
                    LOG_INFO(Core, "DSMod: swap {} = {} (it held {})", action.swap_point, previous,
                             other_value);
                }
            }
        }
        if (!WritePointValue(target, value, element, resolved->f)) {
            return ActionResult::Skipped;
        }
        if (resolved->f) {
            LOG_INFO(Core, "DSMod: wrote {} = {:.3f}", point_name, *resolved->f);
            trace(fmt::format("DSMod: wrote {} = {:.3f}", point_name, *resolved->f));
        } else {
            LOG_INFO(Core, "DSMod: wrote {} = {}", point_name, value);
            trace(fmt::format("DSMod: wrote {} = {}", point_name, value));
        }
        break;
    }
    case ActionKind::Button: {
        auto* pad =
            system.GetInputSubsystem() ? system.GetInputSubsystem()->GetVirtualGamepad() : nullptr;
        if (!pad) {
            LOG_WARNING(Core, "DSMod: button action '{}' ignored: no input subsystem",
                        action.button);
            return ActionResult::Skipped;
        }
        // A stick shove ("RX+") is a legitimate action token even though it is not a button.
        const auto is_stick = [](const std::string& n) {
            return n.size() >= 3 && (n[0] == 'L' || n[0] == 'R') && (n[1] == 'X' || n[1] == 'Y') &&
                   (n.back() == '+' || n.back() == '-');
        };
        bool ok{};
        ParseButton(action.button, ok);
        if (!ok && !is_stick(action.button)) {
            return ActionResult::Skipped;
        }
        // Press on both the handheld and player-1 ports; npad polls at 1 kHz, so hold a few ticks.
        // How many steps to send. With a counter the tap names a destination, and the walk there
        // is however many presses the game needs -- nothing is written to the game's memory.
        u32 times = std::max(1u, action.repeat);
        std::string token = action.button;
        if (!action.counter.empty()) {
            const auto known = counters.find(action.counter);
            const s64 current = known == counters.end() ? 0 : known->second;
            if (action.modulo <= 0) {
                // A row that does not wrap: walk the short way and press the other direction to
                // go back, rather than running off the end.
                const s64 want = action.use_delta ? current + action.delta : action.target;
                const s64 diff = want - current;
                counters[action.counter] = want;
                if (diff == 0) {
                    LOG_INFO(Core, "DSMod: '{}' already at {}", action.counter, want);
                    return ActionResult::Done;
                }
                if (diff < 0 && !action.button_neg.empty()) {
                    token = action.button_neg;
                }
                times = static_cast<u32>(diff < 0 ? -diff : diff);
                if (!action.hold_map.empty()) {
                    // One deflection long enough to auto-repeat across the whole distance.
                    const size_t idx = std::min<size_t>(times, action.hold_map.size() - 1);
                    const u32 burst = std::max(2u, action.hold_map[idx]);
                    PressToken(token, burst);
                    LOG_INFO(Core, "DSMod: '{}' -> {} via {} burst {} ticks ({} steps)",
                             action.counter, want, token, burst, times);
                    return ActionResult::Done;
                }
                const u32 hold0 = std::max(2u, action.frames);
                PressToken(token, hold0);
                for (u32 i = 1; i < times; ++i) {
                    press_queue.push_back(ScheduledPress{
                        token, tick_count + static_cast<u64>(i) * action.gap, hold0});
                }
                LOG_INFO(Core, "DSMod: '{}' -> {} via {} x{}", action.counter, want, token, times);
                return ActionResult::Done;
            }
            const s64 want = action.use_delta ? current + action.delta : action.target;
            const s64 steps = ((want - current) % action.modulo + action.modulo) % action.modulo;
            counters[action.counter] = ((want % action.modulo) + action.modulo) % action.modulo;
            if (steps == 0) {
                LOG_INFO(Core, "DSMod: '{}' already at {}", action.counter, action.target);
                return ActionResult::Done;
            }
            times = static_cast<u32>(steps);
        }
        const u32 hold = std::max(2u, action.frames);
        PressToken(token, hold);
        for (u32 i = 1; i < times; ++i) {
            press_queue.push_back(
                ScheduledPress{token, tick_count + static_cast<u64>(i) * action.gap, hold});
        }
        LOG_INFO(Core, "DSMod: pressed virtual button '{}' x{} ({} ticks each)", action.button,
                 times, hold);
        break;
    }
    case ActionKind::Sequence: {
        const auto it = manifest.sequences.find(action.sequence);
        if (it == manifest.sequences.end()) {
            return ActionResult::Skipped;
        }
        StartSequence(action.sequence, it->second, snapshot);
        return ActionResult::Done;
    }
    case ActionKind::Call:
        RequestCall(action, snapshot);
        break;
    case ActionKind::Page: {
        const size_t before = current_page;
        bool found = false;
        for (size_t i = 0; i < manifest.pages.size(); ++i) {
            if (manifest.pages[i].id == action.page) {
                current_page = i;
                found = true;
                break;
            }
        }
        if (!found) {
            return ActionResult::Skipped;
        }
        if (current_page != before && action.transition != PageTransition::None &&
            action.duration_ms > 0) {
            std::array<s32, 4> origin{}; // {0,0,0,0} = unresolved; DrivePageTransition falls
                                         // back to the canvas centre (Grow/Shrink only)
            if (action.transition == PageTransition::Grow ||
                action.transition == PageTransition::Shrink) {
                if (!action.origin.empty()) {
                    // Named explicitly (Close shrinking into a tab on the page below): the
                    // destination page first (that's where such a widget usually lives), then the
                    // source page.
                    if (const auto r = FindWidgetRect(current_page, action.origin, snapshot)) {
                        origin = *r;
                    } else if (const auto r2 = FindWidgetRect(before, action.origin, snapshot)) {
                        origin = *r2;
                    } else {
                        LOG_WARNING(Core,
                                    "DSMod: page transition origin widget '{}' not found on '{}' "
                                    "or '{}'",
                                    action.origin, manifest.pages[current_page].id,
                                    manifest.pages[before].id);
                    }
                } else if (tap_origin != nullptr) {
                    origin = *tap_origin; // default: the widget that was tapped
                }
            }
            // Started by the next publish, which still holds the page being left.
            page_anim_request = PageAnimRequest{
                before,        current_page, action.transition, action.duration_ms, action.easing,
                action.shadow, origin};
        } else if (current_page != before) {
            page_anim_request.reset();
        }
        break;
    }
    case ActionKind::ViewReset: {
        // Ease the widget's view home over the next ticks (GlideViews) rather than snapping.
        std::scoped_lock lk{view_mutex};
        auto& view = view_state[action.view];
        view.gliding = true;
        view.goal_zoom = 1.0f;
        view.goal_pan_x = 0.0f;
        view.goal_pan_y = 0.0f;
        LOG_INFO(Core, "DSMod: view '{}' gliding home from zoom {:.2f} pan ({:.0f},{:.0f})",
                 action.view, view.zoom, view.pan_x, view.pan_y);
        break;
    }
    case ActionKind::Flag: {
        if (action.flag.empty()) {
            return ActionResult::Skipped;
        }
        s64& current = flags[action.flag];
        if (action.cycle > 0) {
            current = current < 0 ? 0 : (current + 1) % action.cycle;
        } else if (action.flag_has_int) {
            const auto v = action_value();
            if (!v) {
                return ActionResult::Skipped;
            }
            current = v->i;
        } else if (action.flag_value >= 0) {
            current = action.flag_value;
        } else {
            current = current != 0 ? 0 : 1;
        }
        LOG_INFO(Core, "DSMod: flag '{}' = {}", action.flag, current);
        trace(fmt::format("DSMod: flag '{}' = {}", action.flag, current));
        break;
    }
    case ActionKind::None:
        return ActionResult::Skipped;
    }
    (void)snapshot;
    return ActionResult::Done;
}

void ModRuntime::DriveLiveInput() {
    static const char* const path = std::getenv("EDEN_DSMOD_INPUT_LIVE");
    if (path == nullptr) {
        return;
    }
    auto* const pad = system.GetInputSubsystem() != nullptr
                          ? system.GetInputSubsystem()->GetVirtualGamepad()
                          : nullptr;
    if (pad == nullptr) {
        return;
    }
    auto assert_input = [&](bool down) {
        // Re-assert every tick while held -- the virtual gamepad is not sticky, so setting it
        // once and returning (as this used to) gave a one-tick blip the game often missed.
        for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
            if (live_held_button >= 0) {
                pad->SetButtonState(pl, live_held_button, down);
            }
            if (live_held_stick) {
                pad->SetStickPosition(pl,
                                      live_stick_right
                                          ? InputCommon::VirtualGamepad::VirtualStick::Right
                                          : InputCommon::VirtualGamepad::VirtualStick::Left,
                                      down ? live_held_x : 0.0f, down ? live_held_y : 0.0f);
            }
        }
    };
    if (live_hold_left > 0) {
        assert_input(true); // keep holding
        if (--live_hold_left == 0) {
            assert_input(false); // clean release
            live_held_button = -1;
            live_held_stick = false;
        }
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return;
    }
    std::ifstream f(path);
    std::string tok;
    f >> tok;
    f.close();
    if (tok.empty()) {
        return; // leave the (empty) file alone
    }
    {
        std::ofstream trunc(path, std::ios::trunc);
    } // consume a real token

    const auto colon = tok.find(':');
    const std::string name = tok.substr(0, colon);
    live_hold_left =
        colon == std::string::npos ? 6 : std::strtoull(tok.c_str() + colon + 1, nullptr, 0);
    live_held_button = -1;
    live_held_stick = false;
    live_held_x = live_held_y = 0.0f;
    if (name.size() >= 3 && (name[0] == 'L' || name[0] == 'R') &&
        (name[1] == 'X' || name[1] == 'Y') && (name.back() == '+' || name.back() == '-')) {
        live_held_stick = true;
        live_stick_right = name[0] == 'R';
        const float v = name.back() == '+' ? 1.0f : -1.0f;
        if (name[1] == 'X')
            live_held_x = v;
        else
            live_held_y = v;
    } else {
        bool ok{};
        const auto button = ParseButton(name, ok);
        if (ok) {
            live_held_button = static_cast<int>(button);
        }
    }
    assert_input(true); // press this frame
    LOG_INFO(Core, "DSMod live input: {}", tok);
}

void ModRuntime::DriveInputScript() {
    auto* const pad = system.GetInputSubsystem() != nullptr
                          ? system.GetInputSubsystem()->GetVirtualGamepad()
                          : nullptr;
    if (pad == nullptr || input_step >= input_script.size()) {
        return;
    }
    // Do not begin until the title is actually up. Boot speed varies run to run, so a leading
    // wait measured from tick 0 lands the first press on a different screen each time. Gate on a
    // fixed floor (overridable) counted from boot -- coarse but stable enough to reach a menu.
    static const u64 delay = [] {
        const char* const v = std::getenv("EDEN_DSMOD_INPUT_DELAY");
        return v != nullptr ? std::strtoull(v, nullptr, 0) : u64{0};
    }();
    if (tick_count < delay) {
        return;
    }
    // Each step: hold for `hold` ticks, release for `Gap`, then advance. A colon overrides hold.
    constexpr u64 Gap = 10;
    const std::string& tok = input_script[input_step];
    const auto colon = tok.find(':');
    const std::string name = tok.substr(0, colon);
    const u64 hold =
        colon == std::string::npos ? 8 : std::strtoull(tok.c_str() + colon + 1, nullptr, 0);
    const u64 phase = input_step_tick;
    const bool pressed = phase < hold;

    auto set_stick = [&](float x, float y) {
        for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
            pad->SetStickPosition(pl, InputCommon::VirtualGamepad::VirtualStick::Left, x, y);
        }
    };
    if (name == "LX+" || name == "LX-" || name == "LY+" || name == "LY-") {
        const float v = pressed ? (name.back() == '+' ? 1.0f : -1.0f) : 0.0f;
        if (name[1] == 'X')
            set_stick(v, 0.0f);
        else
            set_stick(0.0f, v);
    } else if (name == "wait") {
        // hold nothing; just let time pass (use the colon for how long)
    } else {
        bool ok{};
        const auto button = ParseButton(name, ok);
        if (ok) {
            for (const std::size_t pl : {std::size_t{0}, std::size_t{8}}) {
                pad->SetButtonState(pl, button, pressed);
            }
        }
    }
    if (++input_step_tick >= hold + Gap) {
        input_step_tick = 0;
        ++input_step;
        if (input_step >= input_script.size()) {
            LOG_INFO(Core, "DSMod: input script complete ({} steps)", input_script.size());
        }
    }
}

} // namespace Core::Mods
