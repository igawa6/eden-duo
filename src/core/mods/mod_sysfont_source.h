// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The "sysfont:" asset source (runtime 23): the console's shared system fonts (pl:u), decrypted
// to plain TrueType bytes, for a module that must draw text in a face its game takes from the
// firmware rather than its own romfs (Pokémon Sword's Korean and Chinese).
//   - Names are the shared font file names without ".bfttf" (".bfttf" is accepted too), e.g.
//     "sysfont:nintendo_udsg-r_ko_003" (SysfontNames()). Anything else reads as missing.
//   - Only the real firmware archive is read. When it is missing or cannot be decrypted (no
//     firmware, keys too old) the read is EMPTY -- never the open-source font Eden synthesizes
//     for the guest, so a module cannot present a substitute face as the game's. A module offers
//     the language only when the bytes are non-empty.
//   - get_i64("__source:sysfont") = 1 means only that this host knows the prefix; whether a font
//     is really there is the read itself.
//   - Capability bit EDEN_DSMOD_CAP_SOURCE_SYSFONT. Read-only; each font is decrypted once per
//     session and kept.
// Kept free of ModRuntime so it can be unit tested (src/tests/core/mods/runtime23_sysfont.cpp).

#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/mods/mod_sources.h"

namespace Core::Mods {

struct SysfontEntry {
    u64 archive_id;        ///< The firmware data archive holding the font.
    std::string_view file; ///< The file inside its romfs ("nintendo_udsg-r_ko_003.bfttf").
};

/// The firmware archive and file for a "sysfont:" name (the part after the prefix); nullopt for
/// anything that is not one of the shared fonts.
std::optional<SysfontEntry> FindSysfont(std::string_view name);

/// Every accepted name (without ".bfttf"), for docs and tests.
std::vector<std::string_view> SysfontNames();

/// A shared font file as stored in the firmware (big-endian words, XOR-encrypted, 8-byte header)
/// decrypted to TrueType bytes; empty when it is not one.
std::vector<u8> DecryptSysfont(std::span<const u8> stored);

/// The "sysfont:" byte source. `open_real_archive(id)` returns the extracted romfs of the REAL
/// firmware data archive `id`, or null -- it must never fall back to a synthesized archive.
AssetSource MakeSysfontSource(std::function<FileSys::VirtualDir(u64 archive_id)> open_real_archive);

} // namespace Core::Mods
