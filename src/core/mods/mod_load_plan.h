// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Mods {

struct LoadPlanSegment {
    u32 location{};
    u32 size{};
};

struct LoadPlanLayout {
    std::array<LoadPlanSegment, 3> segments{};
    u32 bss_size{};
};

class ModLoadPlan {
public:
    enum class RelocationKind {
        AArch64Adrp,
        AArch64AddLo12,
    };

    struct Relocation {
        u32 offset{};
        RelocationKind kind{};
        s64 addend{};
    };

    struct Write {
        u64 offset{};
        std::vector<u8> expected;
        std::vector<u8> replacement;
        std::vector<Relocation> relocations;
    };

    ModLoadPlan() = default;
    ModLoadPlan(u64 mailbox_size_, std::vector<Write> writes_)
        : mailbox_size{mailbox_size_}, writes{std::move(writes_)} {}

    [[nodiscard]] u64 MailboxSize() const {
        return mailbox_size;
    }

    /// Validates and applies all writes atomically. Offsets are relative to the NSO image, not
    /// an NCE pre-text area. mailbox_address is the final guest virtual address of the mailbox.
    [[nodiscard]] bool Apply(std::span<u8> module_image, VAddr load_base,
                             VAddr mailbox_address) const;

private:
    u64 mailbox_size{};
    std::vector<Write> writes;
};

struct LoadPlanDiscovery {
    enum class Status {
        None,
        Found,
        Invalid,
    };

    Status status{Status::None};
    std::optional<ModLoadPlan> plan;
    std::string error;
};

/// Selects the same first enabled, lexically sorted dual-screen package as ModRuntime::Discover.
/// A manifest without load_plan is treated as no plan. A selected malformed or mismatched plan is
/// Invalid rather than silently skipped, preventing a hook for the wrong executable from loading.
[[nodiscard]] LoadPlanDiscovery DiscoverModLoadPlan(System& system, u64 title_id,
                                                    const std::array<u8, 0x20>& build_id,
                                                    std::string_view module_name,
                                                    const LoadPlanLayout& layout);

} // namespace Core::Mods
