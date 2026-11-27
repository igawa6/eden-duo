// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo (runtime 17): the controller gate of the second screen's focus mode. While the
// companion's controller navigation is on (Core::Mods, mod_nav.cpp), NPad::RequestPadStateUpdate
// hands the game a neutral pad: no buttons, centred sticks, released analog triggers. Only what
// the companion itself presses for the game (a tapped button action, `pass`) still goes through.
// When the mode ends, every button still physically held is latched and stays hidden from the
// game until it is released, so the chord or the B that closed the mode never reaches the game.
// The latch is kept per npad slot: each controller clears only its own copy as it releases the
// button, so a second pad (Handheld + Player1, or a P2 pad) that does not hold it cannot unlatch
// it for the one that does.
//
// The gate sits at the HID service, after the EmulatedController: every frontend (Android's
// NativeInput and on-screen overlay, SDL on desktop and eden-cli) feeds the same controllers,
// and the runtime keeps reading the real buttons from them to drive the focus. One gate per
// process, which is one emulated system.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "common/common_types.h"
#include "hid_core/hid_types.h"

namespace Core::HID::DSModPadGate {

namespace Detail {
inline std::atomic<bool> active{false};
inline std::atomic<bool> context_blocked{false};
inline std::atomic<u64> pass{0};        ///< NpadButton bits let through while active
inline std::atomic<u8> pass_sticks{0};  ///< bit 0 left stick, bit 1 right stick
/// Hidden until released (set when the mode ends), one copy per npad slot (NpadIdTypeToIndex:
/// Player1..8, Handheld, Other).
inline constexpr size_t Slots = 10;
inline std::array<std::atomic<u64>, Slots> latched{};
} // namespace Detail

/// The kernel sets this before changing the application process. The timing thread clears it
/// only after retiring the old companion's virtual inputs. Navigation cannot override it.
inline void BlockForContextChange(bool blocked) {
    Detail::context_blocked.store(blocked, std::memory_order_release);
}

/// Turns the gate on or off. `pass`: buttons the companion holds for the game itself.
inline void Set(bool active, u64 pass = 0, bool pass_left_stick = false,
                bool pass_right_stick = false) {
    Detail::pass.store(pass, std::memory_order_relaxed);
    Detail::pass_sticks.store(static_cast<u8>((pass_left_stick ? 1 : 0) | (pass_right_stick ? 2 : 0)),
                              std::memory_order_relaxed);
    Detail::active.store(active, std::memory_order_release);
}

/// Hides these buttons of npad slot `slot` (NpadIdTypeToIndex) from the game until each is
/// released on that slot.
inline void Latch(size_t slot, u64 held) {
    if (held != 0 && slot < Detail::Slots) {
        Detail::latched[slot].fetch_or(held, std::memory_order_acq_rel);
    }
}

[[nodiscard]] inline bool Active() {
    return Detail::active.load(std::memory_order_acquire);
}

/// What slot `slot` still hides (0 for an out-of-range slot).
[[nodiscard]] inline u64 Latched(size_t slot) {
    return slot < Detail::Slots ? Detail::latched[slot].load(std::memory_order_acquire) : 0;
}

/// What any slot still hides.
[[nodiscard]] inline u64 Latched() {
    u64 out = 0;
    for (const auto& slot : Detail::latched) {
        out |= slot.load(std::memory_order_acquire);
    }
    return out;
}

/// Off, nothing latched (tests, runtime teardown).
inline void Reset() {
    Set(false);
    for (auto& slot : Detail::latched) {
        slot.store(0, std::memory_order_release);
    }
}

/// Filters one pad sample of npad slot `slot` (NpadIdTypeToIndex) in place. Returns true when the
/// gate is suppressing (the caller also releases analog triggers then).
inline bool Apply(size_t slot, u64& buttons, AnalogStickState& left, AnalogStickState& right) {
    if (Detail::context_blocked.load(std::memory_order_acquire)) {
        buttons = 0;
        left = {};
        right = {};
        return true;
    }
    // Released latched buttons stop being latched for this slot; the rest stay hidden.
    if (slot < Detail::Slots && Detail::latched[slot].load(std::memory_order_relaxed) != 0) {
        const u64 held = buttons;
        const u64 was = Detail::latched[slot].fetch_and(held, std::memory_order_acq_rel);
        buttons &= ~(was & held);
    }
    if (!Detail::active.load(std::memory_order_acquire)) {
        return false;
    }
    buttons &= Detail::pass.load(std::memory_order_relaxed);
    const u8 sticks = Detail::pass_sticks.load(std::memory_order_relaxed);
    if ((sticks & 1) == 0) {
        left = {};
    }
    if ((sticks & 2) == 0) {
        right = {};
    }
    return true;
}

} // namespace Core::HID::DSModPadGate
