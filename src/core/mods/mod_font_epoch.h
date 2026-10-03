// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Runtime 18: a module asks the host to decode its font again ("__font_epoch").
//
// A module's decode_font (dsmod_module_extensions.h) runs once, plus a bounded retry while the
// module declines it (LoadFont, mod_assets.cpp: about once a second, 30 times). A module whose
// glyph set depends on something it learns late (the game language after a delayed
// initialization or a language-change prompt) used to keep the
// first answer for the session. Now the module publishes an integer "__font_epoch" (publish_i64,
// every sample like any other value) and changes it whenever its font would decode differently;
// the host notices a value different from the one in effect when decode_font last ran and calls
// decode_font again (a fresh bounded retry), keeping the current font until the new one is
// decoded, then drops the cached atlas image and repaints the page.
//   - A missing value reads 0, so a module that never publishes it is never asked again (every
//     module before runtime 18), and a module may start at 0 and count up.
//   - Hosts that implement it advertise EDEN_DSMOD_CAP_FONT_EPOCH in EdenDsmodHostApi::capabilities
//     (dsmod_module_abi.h) so a module can tell whether a late re-decode will come; a module must
//     not set the bit in its own capabilities (older hosts would refuse to load it).
// Kept free of ModRuntime so it can be unit tested (src/tests/core/mods/runtime18.cpp).

#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Core::Mods {

/// The module-published value a host watches (publish_i64; missing = 0).
inline constexpr std::string_view FontEpochKey{"__font_epoch"};

/// Match one atlas or a numeric page of a {p} atlas, without evicting unrelated images.
inline bool FontAtlasKeyMatches(std::string_view pattern, std::string_view key) {
    const auto page = pattern.find("{p}");
    if (page == std::string_view::npos)
        return pattern == key;
    const auto prefix = pattern.substr(0, page);
    const auto suffix = pattern.substr(page + 3);
    if (!key.starts_with(prefix) || !key.ends_with(suffix) ||
        key.size() <= prefix.size() + suffix.size())
        return false;
    const auto number = key.substr(prefix.size(), key.size() - prefix.size() - suffix.size());
    return number.find_first_not_of("0123456789") == std::string_view::npos;
}

struct FontEpochWatch {
    s64 decoded{0}; ///< the published epoch when decode_font last ran
    s64 latest{0};  ///< this tick's published epoch

    /// This tick's published value (nullopt = not published = 0).
    void Observe(std::optional<s64> value) {
        latest = value.value_or(0);
    }
    /// decode_font is about to run: what it answers belongs to the current epoch.
    void Decoding() {
        decoded = latest;
    }
    /// A new decode is due: the font is settled (decoded, or the bounded retry ran out -- a decode
    /// still being retried will see the module's new state by itself) and the epoch moved on.
    [[nodiscard]] bool Due(bool settled) const {
        return settled && latest != decoded;
    }
};

} // namespace Core::Mods
