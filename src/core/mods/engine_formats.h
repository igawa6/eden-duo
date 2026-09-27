// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Private header for the engine format files (engine_ichigo.cpp, engine_mercury.cpp): helpers
// that more than one engine's decoder needs. Both engines wrap NVN textures in an XTX
// container, whose pixels are stored block-linear. Anonymous namespace, so each including file
// gets its own copy, exactly as when the helper sat in mod_assets.cpp; every includer uses it.

#pragma once

#include "common/common_types.h"

namespace Core::Mods {

namespace {

/// Where pixel (x, y) sits in a block-linear (Tegra) image. Switch textures are stored in this
/// tiling rather than row by row; read linearly they look like torn stripes.
u64 BlockLinearOffset(u32 x, u32 y, u32 bpp, u32 block_height, u32 gobs_per_row) {
    const u64 gob = (static_cast<u64>(y / (8 * block_height)) * 512 * block_height * gobs_per_row) +
                    (static_cast<u64>(x * bpp / 64) * 512 * block_height) +
                    (static_cast<u64>(y % (8 * block_height) / 8) * 512);
    const u32 xb = x * bpp;
    return gob + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 +
           (y % 2) * 16 + (xb % 16);
}
} // namespace

} // namespace Core::Mods
