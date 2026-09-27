// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 7: composite images (manifest "composites"). Layers, the
// definition, and the packed level layout. Built by the asset worker (mod_nx_runtime.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods {

/// One layer of a composite image (manifest "composites", mod_nx_runtime.cpp): a picture
/// area-resampled into a rectangle of the composite, optionally cut by another image's alpha.
struct CompositeLayer {
    std::string src;
    std::array<float, 4> rect{};               ///< x, y, w, h in composite pixels (fractional ok)
    std::array<float, 4> src_rect{0, 0, 1, 1}; ///< normalised u0, v0, u1, v1 of the source
    std::array<float, 4> src_px{};             ///< source pixels x, y, w, h; wins when w, h > 0
    std::string mask_src;                      ///< alpha multiplier, stretched over `rect`
    std::array<float, 4> mask_src_rect{0, 0, 1, 1}; ///< normalised part of the mask used
    std::string show_bind; ///< shown while empty or non-zero ("!x" negates)
    std::string hide_bind; ///< hidden while non-empty and non-zero
    u32 fade_ms{0};        ///< fade-in when the layer becomes visible
    float opacity{1.0f};
};

/// A picture composed at runtime from many layers ("src": "composite:<name>").
struct CompositeDef {
    std::string name;
    u32 w{};
    u32 h{};
    u32 background{0}; ///< ARGB
    std::vector<CompositeLayer> layers;
    /// Extra downscaled copies (scale factors < 1 of w x h), packed beside the full picture in the
    /// same image (CompositeLevels gives their rects), so a zooming map can pick the nearest one.
    std::vector<float> levels;
    /// Flat mode: visible layers are folded into one retained canvas instead of keeping every
    /// layer's raster; a newly shown layer is decoded then and drawn on top, and its fade is a
    /// draw-time cross-fade from the previous picture ("@fade:<name>" 0..1000, "<src>#prev").
    bool flat{false};
};

/// One picture of a composite's packed image: level 0 is the full-size picture at (0, 0).
struct CompositeLevel {
    u32 x{}, y{}, w{}, h{};
    float scale{1.0f};
};

/// Where every level of a composite sits in its packed image (shelves in columns to the right
/// of level 0); the packed image's size comes back through packed_w / packed_h.
inline std::vector<CompositeLevel> CompositeLevels(const CompositeDef& def, u32* packed_w = nullptr,
                                                   u32* packed_h = nullptr) {
    std::vector<CompositeLevel> out{{0, 0, def.w, def.h, 1.0f}};
    u32 col_x = def.w, col_w = 0, shelf_x = def.w, shelf_y = 0, shelf_h = 0;
    u32 max_w = def.w, max_h = def.h;
    for (const float s : def.levels) {
        if (!(s > 0.0f && s < 1.0f)) {
            continue;
        }
        const u32 lw = std::max<u32>(1, static_cast<u32>(def.w * s + 0.5f));
        const u32 lh = std::max<u32>(1, static_cast<u32>(def.h * s + 0.5f));
        if (col_w == 0) {
            col_w = lw;
        }
        if (shelf_x + lw > col_x + col_w) {
            shelf_y += shelf_h;
            shelf_x = col_x;
            shelf_h = 0;
        }
        if (shelf_y + lh > def.h) {
            col_x += col_w;
            col_w = lw;
            shelf_x = col_x;
            shelf_y = 0;
            shelf_h = 0;
        }
        out.push_back({shelf_x, shelf_y, lw, lh, s});
        shelf_x += lw;
        shelf_h = std::max(shelf_h, lh);
        max_w = std::max(max_w, shelf_x);
        max_h = std::max(max_h, shelf_y + lh);
    }
    if (packed_w != nullptr) {
        *packed_w = max_w;
    }
    if (packed_h != nullptr) {
        *packed_h = max_h;
    }
    return out;
}

} // namespace Core::Mods
