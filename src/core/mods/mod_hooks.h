// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// One global flag so the CPU backend can turn `brk` into a halt without enabling the gdbstub
// (which would also force the expensive memory-access checks on every load and store).

#pragma once

#include <atomic>

namespace Core::Mods {

inline std::atomic<bool> g_guest_hooks_enabled{false};

[[nodiscard]] inline bool GuestHooksEnabled() {
    return g_guest_hooks_enabled.load(std::memory_order_relaxed);
}

} // namespace Core::Mods
