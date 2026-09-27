// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// MSBT (LMS "MsgStdBn") message files, read out of the running game's romfs so a package can show
// the game's own words by key without shipping any of them. Pure format code: no runtime, no
// logging, no filesystem.
//
// A message becomes one UTF-8 string: plain text as is, '\n' kept, the icon tag turned into a
// private-use code point (TextIconBase + the icon font's code point), ruby dropped (its base text
// follows the tag as ordinary text), every other tag stripped and reported.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods::Msbt {

struct Options {
    u16 icon_group{0xFFFF};
    u16 icon_type{0xFFFF};
    /// First parameter byte of the icon tag -> the icon font's code point.
    const std::unordered_map<u32, char32_t>* icon_glyphs{nullptr};
};

struct File {
    std::unordered_map<std::string, std::string> texts; ///< label -> decoded UTF-8
    u8 encoding{};                                      ///< 0 UTF-8, 1 UTF-16, 2 UTF-32
    /// Tags that were stripped (group << 16 | type), each once, in order of first sight.
    std::vector<u32> stripped;
    /// Icon tag parameters without a glyph, each once.
    std::vector<u32> unmapped_icons;
};

/// Parses LBL1 + TXT2 of a whole message file.
bool Parse(std::span<const u8> bytes, const Options& options, File& out, std::string& error);

/// Appends one code point as UTF-8.
void AppendUtf8(std::string& out, char32_t cp);

/// Next code point of a UTF-8 string (a malformed byte is taken on its own), advancing `i`.
char32_t NextUtf8(std::string_view text, size_t& i);

} // namespace Core::Mods::Msbt
