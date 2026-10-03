// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 17: controller navigation of the second screen (manifest "nav", page "nav_order").
//   - Chords: ParseChord / ButtonBit name Core::HID::NpadButton bits ("ZL+ZR", "LS+RS").
//   - Controller: the pure button state machine -- the toggle chord's edge enters and leaves, B
//     leaves, A activates, the D-pad (or the left stick's digital directions) moves, with key
//     repeat while a direction is held.
//   - Focus geometry: Candidate (one tappable widget as expanded on the page), Order (nav_order),
//     MoveGeometric / MoveOrdered, FirstFocus, Nearest.
// The ModRuntime side (reading the pad, building the candidates, scrolling a list, tapping, the
// HID gate and the published "@nav.*" values) is ModRuntime::UpdateNav in mod_nav.cpp.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods::Nav {

/// NpadButton bits the navigation reads (hid_core/hid_types.h).
inline constexpr u64 BtnA = 1ULL << 0;
inline constexpr u64 BtnB = 1ULL << 1;
inline constexpr u64 BtnStickL = 1ULL << 4;
inline constexpr u64 BtnStickR = 1ULL << 5;
inline constexpr u64 BtnLeft = 1ULL << 12;
inline constexpr u64 BtnUp = 1ULL << 13;
inline constexpr u64 BtnRight = 1ULL << 14;
inline constexpr u64 BtnDown = 1ULL << 15;
inline constexpr u64 BtnStickLLeft = 1ULL << 16;
inline constexpr u64 BtnStickLUp = 1ULL << 17;
inline constexpr u64 BtnStickLRight = 1ULL << 18;
inline constexpr u64 BtnStickLDown = 1ULL << 19;

/// The global default chord: both stick clicks together. Games bind L3 and R3 one at a time
/// (sprint, crouch, camera reset) but almost never ask for both at once, and every Switch
/// controller and handheld has them, so the default steals nothing a game expects.
inline constexpr std::string_view DefaultToggle = "LS+RS";
inline constexpr u64 DefaultToggleMask = BtnStickL | BtnStickR;

/// One button name -> its NpadButton bit, 0 when unknown. Accepts the action button names
/// (A B X Y L R ZL ZR Plus Minus DUp DDown DLeft DRight) plus Up/Down/Left/Right, LS/RS,
/// L3/R3 and StickL/StickR; case-insensitive.
[[nodiscard]] u64 ButtonBit(std::string_view name);
/// "ZL+ZR" -> the chord's bits. nullopt unless every part is a known button and there are at
/// least two different ones (a single button would be stolen from the game outright).
[[nodiscard]] std::optional<u64> ParseChord(std::string_view text);

enum class Dir : u8 { None, Up, Down, Left, Right };

/// The direction the buttons hold (D-pad first, then the left stick's digital directions; up,
/// down, left, right in that priority), None when none.
[[nodiscard]] Dir HeldDir(u64 buttons);

/// The pure toggle / move / activate state machine, fed the pad once per tick.
class Controller {
public:
    static constexpr s64 RepeatDelayMs = 400;
    static constexpr s64 RepeatPeriodMs = 120;

    struct Events {
        bool entered{false};
        bool left{false};
        Dir move{Dir::None};
        bool activate{false};
    };

    /// `buttons`: the controller's buttons this tick; `toggle_mask`: the chord (0 = default).
    Events Step(u64 buttons, s64 now_ms, u64 toggle_mask);
    /// Leaves without an event (the second screen went away, the package was unloaded).
    void ForceOff();
    [[nodiscard]] bool Active() const {
        return active;
    }

private:
    bool active{false};
    u64 prev{0};
    bool chord_prev{false};
    Dir held{Dir::None};
    s64 next_repeat{0};
};

/// A tappable widget as expanded on the current page.
struct Candidate {
    std::array<s32, 4> rect{};  ///< drawn rect (x, y, w, h)
    std::array<s32, 4> clip{};  ///< the scroll row's viewport, w/h <= 0 = none
    std::array<s32, 4> shown{}; ///< rect inside clip and canvas: the frame and the tap centre
    size_t source{0};           ///< index in Page::widgets
    s64 element{-1};            ///< repeat element, -1 for a plain widget
    std::string id;             ///< expanded id ("{i}" substituted)
    std::string template_id;    ///< the page widget's own id, as written
    std::string scroll;         ///< scroll region id, empty when not a scrolled row
};

/// The focusable candidates, in focus order: `nav_order` entries match a candidate's id or its
/// template's id ("slot_{i}" = every element, in element order); unlisted ones are left out. An
/// empty `nav_order` keeps every candidate, in page order.
[[nodiscard]] std::vector<size_t> Order(std::span<const Candidate> candidates,
                                        const std::vector<std::string>& nav_order);
/// The nearest candidate of `allowed` in direction `dir` from `from`'s centre: it must lie beyond
/// that centre along the axis; the score is the distance along the axis plus twice the distance
/// across it (0 while the two overlap across it). Ties go to the one whose centre is most in line,
/// then to the earlier candidate.
[[nodiscard]] std::optional<size_t> MoveGeometric(std::span<const Candidate> candidates,
                                                  std::span<const size_t> allowed, size_t from,
                                                  Dir dir);
/// nav_order movement: Down/Right = next, Up/Left = previous, wrapping.
[[nodiscard]] std::optional<size_t> MoveOrdered(std::span<const size_t> order, size_t from,
                                                Dir dir);
/// Where focus starts: the first in nav_order, else the top-most (then left-most) candidate.
[[nodiscard]] std::optional<size_t> FirstFocus(std::span<const Candidate> candidates,
                                               std::span<const size_t> order, bool ordered);
/// The candidate of `allowed` whose centre is nearest (cx, cy).
[[nodiscard]] std::optional<size_t> Nearest(std::span<const Candidate> candidates,
                                            std::span<const size_t> allowed, s32 cx, s32 cy);
/// The candidate showing element `element` of page widget `source`.
[[nodiscard]] std::optional<size_t> Find(std::span<const Candidate> candidates, size_t source,
                                         s64 element);

/// The runtime's per-session navigation state (ModRuntime::nav_state).
struct RuntimeState {
    Controller controller;
    size_t page{static_cast<size_t>(-1)};
    bool has_focus{false};
    size_t source{0};
    s64 element{-1};
    std::array<s32, 4> last_shown{};
    bool gate_on{false};
};

} // namespace Core::Mods::Nav
