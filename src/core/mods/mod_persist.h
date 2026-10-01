// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Persisted runtime flags (unreleased runtime 15 addition; manifest top level
// "persist_flags": ["<flag>", ...], next to "flags").
//   - The listed runtime flags (ints: 0/1 or a small multi-state value, see ModRuntime::flags) are
//     written whenever a flag action changes one of them, and restored when the package loads,
//     overriding the manifest's "flags" default. Flags not listed are never stored or restored.
//   - Storage: one small JSON file per title + package in Eden's own user data dir,
//       <EdenDir>/dualscreen/persist/<TITLEID>/<package>.json
//     (EdenDir = the emulator's data dir on desktop, the app's user dir on Android; the same
//     Common::FS path API on both). Not in the package folder (the Android installer replaces
//     it on update, and its folder name carries the version) and not in the game's save data.
//     <package> is the manifest "name" (the mod folder name when empty), reduced to
//     [A-Za-z0-9._-].
//   - Format: {"version": 1, "flags": {"<flag>": <int or bool>, ...}}. Written atomically (temp
//     file in the same directory + rename). A missing file or another version is ignored
//     silently; a malformed file is ignored with a warning (the next change rewrites it).
// Kept free of ModRuntime so it can be unit tested (src/tests/core/mods/runtime15_persist.cpp).

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods {

inline constexpr int PersistFlagsFileVersion = 1;

/// <root>/<TITLEID>/<package>.json (root = PersistFlagsRoot() in the runtime).
std::filesystem::path PersistFlagsPath(const std::filesystem::path& root, u64 title_id,
                                       std::string_view package);

/// <EdenDir>/dualscreen/persist.
std::filesystem::path PersistFlagsRoot();

/// Saves and restores the runtime flags a package lists in "persist_flags".
class FlagPersistence {
public:
    FlagPersistence() = default;
    FlagPersistence(std::filesystem::path file, std::vector<std::string> keys);

    bool Enabled() const {
        return !keys.empty() && !file.empty();
    }
    const std::filesystem::path& File() const {
        return file;
    }

    /// Overrides the listed flags with the stored values (file missing/old/malformed: no change).
    /// Returns how many flags were restored.
    size_t Restore(std::unordered_map<std::string, s64>& flags);

    /// Writes the listed flags when one differs from what was last restored or written. Returns
    /// true when the file was written.
    bool SaveIfChanged(const std::unordered_map<std::string, s64>& flags);

private:
    std::unordered_map<std::string, s64> Pick(const std::unordered_map<std::string, s64>& flags) const;

    std::filesystem::path file;
    std::vector<std::string> keys;
    std::unordered_map<std::string, s64> saved;
    bool have_saved{false};
};

} // namespace Core::Mods
