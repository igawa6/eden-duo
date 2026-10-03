// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Companion settings page (runtime 17; manifest top level "settings").
//   "settings": [{"flag": "map.dark", "label": "Dark map", "type": "toggle", "default": 0},
//                {"flag": "hud.size", "label": "HUD size", "type": "choice",
//                 "choices": ["Small", "Medium", "Large"], "default": 1}]
//   - Each entry is a runtime flag: a toggle is 0/1, a choice is the index into "choices".
//     "default" (int or bool, default 0) seeds the flag unless "flags" already declares it.
//   - Every settings flag is persisted as if listed in "persist_flags" (mod_persist.h).
//   - The runtime appends a built-in page with id "@settings": a title, a BACK button and one
//     big row per entry (tap = next value, wrapping), drawn with the package font when it has
//     one. A package opens it with {"kind": "page", "page": "@settings"}; BACK (the action
//     page "@back") returns to the page that was showing when it opened.
//   - Invalid entries (no flag, a choice without at least two choices, an unknown type) are
//     skipped with a warning; a package without pages gets no settings page.
// Kept free of ModRuntime so it can be unit tested (src/tests/core/mods/runtime17_settings.cpp).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "common/common_types.h"

namespace Core::Mods {

struct Manifest;
struct Page;

inline constexpr std::string_view SettingsPageId = "@settings";
/// The page action target that leaves "@settings" for the page it was opened from.
inline constexpr std::string_view SettingsBackPageId = "@back";
/// Generated action names: "@settings:back" and "@settings:<flag>".
inline constexpr std::string_view SettingsActionPrefix = "@settings:";

struct SettingDef {
    enum class Type : u8 { Toggle, Choice };
    std::string flag;
    std::string label;
    Type type{Type::Toggle};
    std::vector<std::string> choices; ///< Toggle: {"OFF", "ON"}
    s64 default_value{0};
};

/// The valid entries of a "settings" array (a non-array gives none, with a warning).
std::vector<SettingDef> ParseSettings(const nlohmann::json& list);

/// Adds the "@settings" page, its actions, the flag defaults and the implicit persist_flags to a
/// parsed manifest. No-op when `settings` is empty or the manifest has no pages.
void ApplySettings(const std::vector<SettingDef>& settings, Manifest& manifest);

/// Where a page action goes: the index of the page `target` names (or, for "@back", the page
/// remembered in `settings_return`), nullopt when there is none. Opening "@settings" from
/// another page stores that page in `settings_return`.
std::optional<size_t> ResolvePageTarget(const std::vector<Page>& pages, std::string_view target,
                                        size_t current, size_t& settings_return);

} // namespace Core::Mods
