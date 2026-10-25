// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <set>

namespace Tegra {

// Protected by GPU::Impl::composite_mutex. Displays can finish in a different order.
class CompositeCompletion {
public:
    bool IsDone(std::uint64_t id) const {
        return id <= completed || later.contains(id);
    }

    void MarkDone(std::uint64_t id) {
        if (id <= completed) {
            return;
        }
        if (id != completed + 1) {
            later.insert(id);
            return;
        }
        completed = id;
        while (later.erase(completed + 1) != 0) {
            ++completed;
        }
    }

private:
    std::uint64_t completed{};
    std::set<std::uint64_t> later;
};

} // namespace Tegra
