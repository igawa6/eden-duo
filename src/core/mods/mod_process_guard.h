// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

namespace Core::Mods {

/// A companion belongs to one retained process, not merely a title id. Once that process stops
/// being the current application, refuse the old companion even if it later becomes current
/// again: its caches and outstanding operations have not been rebound to the new session.
/// Callers supply an identity comparison made under the kernel's process-owner lock.
class ProcessOwnerGuard {
public:
    [[nodiscard]] bool Allow(bool same_owner) const {
        if (!same_owner) {
            revoked.store(true, std::memory_order_release);
            return false;
        }
        return !revoked.load(std::memory_order_acquire);
    }

private:
    mutable std::atomic<bool> revoked{false};
};

} // namespace Core::Mods
