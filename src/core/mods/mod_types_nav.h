// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 8 (runtime 17): controller navigation of the second screen ("nav").
// A chord toggles a focus mode in which the D-pad moves a highlight frame among the page's
// tappable widgets, A taps the focused one and B (or the chord) leaves; the game sees no
// controller input meanwhile. Parsed in mod_manifest.cpp, driven by mod_nav.cpp, drawn by
// RenderPage (mod_ui.cpp).

#pragma once

#include <string>

#include "common/common_types.h"

namespace Core::Mods {

/// Manifest top-level "nav" (runtime 17). Absent = enabled with the global default chord for a
/// package declaring min_runtime >= 17, disabled for an older one (NavDefaultOn).
struct NavConfig {
    /// "nav": false or {"enabled": false} opts the package out (no chord, no frame, no
    /// suppression); "nav": true or an object opts any package in. Off by default here (a
    /// Manifest built in code, a package published before runtime 17); ParseNav decides.
    bool enabled{false};
    /// "toggle": the chord as written ("ZL+ZR"); empty = the global default (Nav::DefaultToggle).
    std::string toggle;
    /// The chord as Core::HID::NpadButton bits; 0 = the global default.
    u64 toggle_mask{0};
    /// The focus frame: "color" (#AARRGGBB, default opaque amber), "frame" (outline px; <= 0
    /// fills the rect, like a selection highlight's "frame"), "src" (an image drawn over the rect
    /// instead, like "highlight_src").
    u32 color{0xFFFFC107u};
    s32 frame{3};
    std::string src;
    /// Strength of the haptic played when the focus moves ("haptic" here, or "haptics": {"nav":
    /// ...}); -1 = none. A HapticOverride value.
    s8 haptic{-1};
};

} // namespace Core::Mods
