// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace VideoCore { class RendererBase; }

namespace Core::GameSettings {

enum class OS {
    Windows,
    Linux,
    MacOS,
    IOS,
    Android,
    FireOS,
    HarmonyOS,
    FreeBSD,
    DragonFlyBSD,
    NetBSD,
    OpenBSD,
    HaikuOS,
    AIX,
    Managarm,
    RedoxOS,
    Solaris,
    Unknown,
};

enum class GPUVendor {
    Nvidia,
    AMD,
    Intel,
    Apple,
    Qualcomm,
    ARM,
    Imagination,
    Microsoft,
    Unknown,
};

enum class TitleID : std::uint64_t {
    NinjaGaidenRagebound = 0x0100781020710000ULL,
    DoubleDragonRevive = 0x01006AE02236C000ULL,
    FireEmblemEngage = 0x0100A6301214E000ULL,
    DragonQuestVIIReimagined = 0x0100A9D01C446000ULL,
    FinalFantasyResonance = 0x0100606026F12000ULL,
};

struct EnvironmentInfo {
    OS os{OS::Unknown};
    GPUVendor vendor{GPUVendor::Unknown};
    std::string vendor_string; // raw string from driver
};

EnvironmentInfo DetectEnvironment(const VideoCore::RendererBase& renderer);

void LoadOverrides(std::uint64_t program_id, const VideoCore::RendererBase& renderer);

/// Per-title defaults for settings read while the GPU is created, so they run before it. A value
/// the user set for the game is kept; RestoreGlobalState undoes these when the game stops.
void LoadBootOverrides(std::uint64_t program_id);

/// True when the title has tuned settings (the ones LoadBootOverrides defaults).
bool HasOptimalSettings(std::uint64_t program_id);

/// Writes the title's tuned settings into the loaded config as the game's own values, replacing
/// what the user set for them. Returns false when the title has none.
bool ApplyOptimalSettings(std::uint64_t program_id);

} // namespace Core::GameSettings
