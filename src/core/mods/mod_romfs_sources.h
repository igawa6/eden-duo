// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Extra romfs roots of the running title, beside the patched program romfs ("romfs:", opened in
// mod_assets.cpp):
//   - OpenBaseRomfs: the program romfs as the base NCA ships it, without the update or any
//     LayeredFS patch ("base:"). Files the update dropped are still there.
//   - OpenAocRomfs: the add-on content data romfs the game mounts (nn::fs::MountAddOnContent ->
//     FSP OpenDataStorageByDataId): the lowest AOC id of this base title that the content
//     providers list, unless the user disabled the title's DLC ("aoc:").
// Both are private storage chains (fresh NCA objects, never the game's cipher or table layers)
// behind a serialising file, opened on first use and cached until another title or boot runs.
// Thread-safe; the first call blocks while the chain is built. A null root means unavailable.

#pragma once

#include "core/file_sys/vfs/vfs_types.h"
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Mods {

/// Root of the running title's unpatched program romfs, or null.
FileSys::VirtualDir OpenBaseRomfs(Core::System& system, u64 program_id);

/// Root of the running title's add-on content data romfs, or null (no DLC, DLC disabled).
FileSys::VirtualDir OpenAocRomfs(Core::System& system, u64 program_id);

/// Drops both cached roots (and their open files); the next call reopens them.
void ReleaseRomfsSources();

} // namespace Core::Mods
