// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 5: actions. What a tap, drop, page bind or enforce rule runs: the
// action kinds and their parameters. Dispatched by RunAction (mod_actions.cpp).

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/mods/mod_types_page.h"
#include "core/mods/mod_types_points.h"

namespace Core::Mods {

enum class ActionKind {
    None,
    Write,     ///< write a constant into a data point
    Button,    ///< press a virtual controller button for a few frames
    Page,      ///< switch the visible page
    Call,      ///< call a guest function on the game's own thread
    Sequence,  ///< run a named chain of guest calls, each seeing the last one's result
    Flag,      ///< set or toggle a named runtime flag that gates enforce rules
    ViewReset, ///< glide a pannable widget's view back to its default (zoom 1, no pan)
    Module,    ///< send a semantic action to the installed native module
    SlotWrite, ///< write several points of the first free element of an array
    MapSelect, ///< set / clear a map marker selection
};

/// A value an action writes: an integer constant, a float constant, or a published / contextual
/// value named by "$name" (payload, map_x, marker_index, @flag:x, ...).
struct ActionValue {
    s64 value{0};
    std::optional<f64> as_float; ///< a JSON float constant
    std::string ref;             ///< "$name" without the '$' (empty = constant)
};

struct Action {
    ActionKind kind{ActionKind::None};
    std::string name; ///< the action's own key (for logs)
    /// Skipped (and logged) while this reads 0.
    PointGate enabled;
    /// Write / flag: a "$name" value other than "$payload", or a float constant.
    std::string value_ref;
    std::optional<f64> value_float;
    /// SlotWrite: array `slot` element i is free while it equals `free_value`; `writes` then run
    /// in order on element i; `select` names a map group that selects the new slot.
    std::string slot;
    s64 count{0};
    s64 free_value{0};
    std::vector<std::pair<std::string, ActionValue>> writes;
    std::string select;
    /// MapSelect: the group; the value comes from `value` / payload / value_ref.
    std::string group;
    /// Flag: step the value 0..cycle-1 instead of setting it.
    s64 cycle{0};
    bool flag_has_int{false}; ///< Flag: `flag_int` is the value to set
    s64 flag_int{0};
    std::string view; ///< ViewReset: the widget id whose view glides home
    std::string module_action;
    std::string point; ///< Write: target data point
    s64 value{0};      ///< Write: value
    /// Write/Module: `"value"`/`"argument"` was "$payload" -- use the dragged or selected
    /// widget's payload instead of the constant (the action is skipped when there is none).
    bool value_from_payload{false};
    /// Write: before writing V to `point`, if this point currently equals V, it receives
    /// `point`'s previous value (an exclusive pair of equip slots swapping like a pause menu).
    std::string swap_point;
    std::string button; ///< Button: A, B, X, Y, L, R, ZL, ZR, Plus, Minus, DUp, ...
    u32 frames{4};      ///< Button: how many runtime ticks to hold
    /// Button: press repeatedly. When `counter` is set the number of presses is worked out from
    /// where the game currently is -- (target - counter) wrapped by `modulo` -- so a tap on the
    /// fourth tool sends exactly the steps needed to walk there through the game's own cycling.
    u32 repeat{1};
    std::string counter;
    s64 target{0};
    s64 modulo{0};
    std::string button_neg; ///< Button+counter: pressed instead when the walk goes backwards
    s64 delta{0};           ///< Button+counter: move this many steps from where we are
    bool use_delta{false};
    u32 gap{18}; ///< ticks between repeated presses
    /// Button+counter: instead of one press per step, send ONE press this long for a walk of N
    /// steps -- the stick and d-pad auto-repeat while held, so a single deflection covers several
    /// slots and the jump lands in a fraction of a second. hold_map[N] is that length.
    std::vector<u32> hold_map;
    std::string page; ///< Page: page id
    /// Page: how the switch is animated.
    PageTransition transition{PageTransition::None};
    u32 duration_ms{220};
    Easing easing{Easing::EaseOut};
    float shadow{0.3f}; ///< darkness of the edge shadow on the page underneath (0 = none)
    /// Page (Grow/Shrink only): id of the widget whose rect is the small end of the scale.
    /// Empty = the widget that was tapped to run this action, else the canvas centre.
    std::string origin;
    /// Haptic strength when this action runs from a tap / drop / map tap (-1 = by kind).
    HapticOverride haptic{-1};
    // Call: the function to run, its x0..x7 arguments, and a per-frame function to borrow the
    // game thread at. Values prefixed "$" name a data point resolved at call time.
    s64 call_fn{};
    std::string call_fn_symbol; ///< "$name" resolved from the data file's symbol table
    std::vector<std::string> args;
    std::string sequence; ///< Sequence: which chain to run
    std::string flag;     ///< Flag: which flag to change
    int flag_value{-1};   ///< Flag: 0/1 to set, -1 to toggle
};

} // namespace Core::Mods
