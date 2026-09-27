// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 3: text. The game's own font (glyph metrics), message-file (MSBT)
// configuration, and the private-use code points that carry inline icons through decoded
// text.

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods {

/// One glyph in the game's own font, as its metrics file describes it.
struct FontGlyph {
    u16 x{}, y{}, w{}, h{};       ///< where it sits in the atlas
    s16 bearing_x{}, bearing_y{}; ///< where to put it relative to the pen and the baseline
    u16 advance{};                ///< how far the pen moves afterwards
};

/// A font read out of the running game rather than drawn by us. Glyphs run in codepoint order
/// from `first_codepoint`, so ASCII indexes straight in.
struct FontMetrics {
    u32 line_height{};
    u32 first_codepoint{0x20};
    std::vector<FontGlyph> glyphs;
    /// Glyphs outside the dense run (a BFFNT's Latin Ext-A, Cyrillic, arrows ...).
    std::unordered_map<char32_t, FontGlyph> extra;
    /// Design pixels from a glyph cell's top to the baseline (0 = unknown): inline icons from a
    /// second font are sized so the two ascents match.
    u32 ascent{0};
    [[nodiscard]] bool Valid() const {
        return !glyphs.empty() && line_height > 0;
    }
    [[nodiscard]] const FontGlyph* Find(char32_t c) const {
        if (c >= first_codepoint && c - first_codepoint < glyphs.size()) {
            return &glyphs[c - first_codepoint];
        }
        if (extra.empty()) {
            return nullptr;
        }
        const auto it = extra.find(c);
        return it == extra.end() ? nullptr : &it->second;
    }
};

/// Text a package names by key ("msbt:<alias>#<label>") instead of shipping it: message files of
/// the running game, in the language the game itself uses (manifest "msbt*" keys).
struct MsbtConfig {
    std::map<std::string, std::string> files; ///< alias -> path with {REGION} / {LANG}
    /// language key (desired-language name, settings index or settings name) -> {REGION, LANG}
    std::map<std::string, std::pair<std::string, std::string>> langs;
    std::pair<std::string, std::string> fallback;
    bool has_fallback{false};
    std::string icon_font; ///< BFFNT drawn inline for the icon tag
    u16 icon_group{0xFFFF};
    u16 icon_type{0xFFFF};
    std::unordered_map<u32, char32_t> icon_glyphs; ///< tag parameter byte -> glyph code point
    [[nodiscard]] bool Enabled() const {
        return !files.empty();
    }
};

/// Inline icons travel inside decoded text as code points in this plane-15 private-use block:
/// IconBase + the icon font's own code point.
constexpr char32_t TextIconBase = 0xF0000;
constexpr char32_t TextIconLast = 0xFFFFF;

} // namespace Core::Mods
