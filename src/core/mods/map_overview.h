// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <string>
#include <unordered_map>

#include "core/mods/mod_ui.h"

namespace Core::Mods {

namespace MapOverviewDetail {

inline u32 ScaleRgb(u32 argb, float scale) {
    u32 result = argb & 0xFF000000u;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int value =
            static_cast<int>(std::lround(static_cast<float>((argb >> shift) & 0xFFu) * scale));
        result |= static_cast<u32>(std::clamp(value, 0, 255)) << shift;
    }
    return result;
}

/// `class_colors` is a caller-resolved "region kind -> final ARGB colour" lookup
/// (built once per rebuild by ModRuntime::GetImage from MapArea.room_categories, the same
/// colour every category resolves to in the detailed raster), replacing this function's own old
/// hardcoded station_color/transport_color/zone_active_color/zone_inactive_color/zone_class_scale
/// reads -- those MapStyle fields no longer exist. Keeps this header free of any room_categories/
/// ModRuntime coupling: an unmatched kind (including the fixed "room" kind, which is never a
/// package-declared category) falls back to the plain room class colour, unchanged.
inline u32 ClassColor(const MapArea::OverviewRegion& region, const MapStyle& style,
                      const std::unordered_map<std::string, u32>& class_colors) {
    if (const auto it = class_colors.find(region.kind); it != class_colors.end()) {
        return it->second;
    }
    return style.room_class_color.value_or(style.room_fill);
}

} // namespace MapOverviewDetail

/// Rasterize the game's authored coarse map-room model. Each OverviewRegion is one native RGB
/// identity, so adjacent regions retain their shared border even when both use the same class.
inline Image RasterizeMapOverview(const MapArea& area, const MapStyle& style, u32 width, u32 height,
                                  const std::unordered_map<std::string, u32>& class_colors) {
    Image result{width, height, std::vector<u32>(static_cast<size_t>(width) * height, 0)};
    if (width == 0 || height == 0 || !area.overview_regions || area.max_x <= area.min_x ||
        area.max_y <= area.min_y) {
        return result;
    }

    Canvas labels;
    labels.Resize(width, height);
    labels.Clear(0);
    const float span_x = area.max_x - area.min_x;
    const float span_y = area.max_y - area.min_y;
    const auto projected_x = [&](float x) {
        return static_cast<double>(x - area.min_x) / span_x * (width - 1);
    };
    const auto projected_y = [&](float y) {
        return static_cast<double>(area.max_y - y) / span_y * (height - 1);
    };
    for (size_t i = 0; i < area.overview_regions->size(); ++i) {
        // Opaque labels make overlapping source triangles idempotent. Region count is naturally
        // bounded by the manifest parser/package size; reserve zero for outside coverage.
        const u32 id = 0xFF000000u | static_cast<u32>((i + 1) & 0x00FFFFFFu);
        for (const auto& tri : (*area.overview_regions)[i].tris) {
            if (!std::ranges::all_of(tri, [](float value) { return std::isfinite(value); }))
                continue;
            const std::array<double, 6> projected{projected_x(tri[0]), projected_y(tri[1]),
                                                  projected_x(tri[2]), projected_y(tri[3]),
                                                  projected_x(tri[4]), projected_y(tri[5])};
            // Keep ordinary off-map authored vertices unclamped (Canvas clips the triangle), but
            // reject corrupt coordinates before converting them to integers used by edge maths.
            if (!std::ranges::all_of(projected, [](double value) {
                    return std::isfinite(value) && std::abs(value) <= 1'000'000.0;
                }))
                continue;
            labels.FillTriangle(static_cast<s32>(std::lround(projected[0])),
                                static_cast<s32>(std::lround(projected[1])),
                                static_cast<s32>(std::lround(projected[2])),
                                static_cast<s32>(std::lround(projected[3])),
                                static_cast<s32>(std::lround(projected[4])),
                                static_cast<s32>(std::lround(projected[5])), id);
        }
    }

    const auto& ids = labels.Pixels();
    const auto id_at = [&](s32 x, s32 y) -> u32 {
        if (x < 0 || y < 0 || x >= static_cast<s32>(width) || y >= static_cast<s32>(height))
            return 0;
        return ids[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
    };
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const size_t at = static_cast<size_t>(y) * width + x;
            const u32 id = ids[at];
            if (id == 0)
                continue;
            const size_t region_index = (id & 0x00FFFFFFu) - 1;
            if (region_index >= area.overview_regions->size())
                continue;
            const bool border = id_at(static_cast<s32>(x) - 1, y) != id ||
                                id_at(static_cast<s32>(x) + 1, y) != id ||
                                id_at(x, static_cast<s32>(y) - 1) != id ||
                                id_at(x, static_cast<s32>(y) + 1) != id;
            const u32 cls = MapOverviewDetail::ClassColor((*area.overview_regions)[region_index],
                                                          style, class_colors);
            const u32 rgb = border ? cls : MapOverviewDetail::ScaleRgb(cls, 0.3f);
            result.pixels[at] = 0x99000000u | (rgb & 0x00FFFFFFu);
        }
    }
    return result;
}

/// Put the detailed map over the coarse downloaded overview. Transparent holes in detail reveal
/// coarse rooms, translucent detail is composited normally, and opaque detail remains bit-exact.
inline void CompositeMapOverview(Image& detail, const Image& overview) {
    if (detail.w != overview.w || detail.h != overview.h ||
        detail.pixels.size() != overview.pixels.size())
        return;
    for (size_t i = 0; i < detail.pixels.size(); ++i) {
        const u32 src = detail.pixels[i];
        const u32 dst = overview.pixels[i];
        const u32 sa = src >> 24;
        if (sa == 255)
            continue;
        if (sa == 0) {
            detail.pixels[i] = dst;
            continue;
        }
        const u32 da = dst >> 24;
        const u32 inv = 255 - sa;
        const u32 alpha_num = sa * 255 + da * inv;
        if (alpha_num == 0) {
            detail.pixels[i] = 0;
            continue;
        }
        const u32 oa = (alpha_num + 127) / 255;
        u32 out = oa << 24;
        for (int shift = 0; shift <= 16; shift += 8) {
            const u32 sc = (src >> shift) & 0xFFu;
            const u32 dc = (dst >> shift) & 0xFFu;
            const u32 color_num = sc * sa * 255 + dc * da * inv;
            out |= std::min<u32>(255, (color_num + alpha_num / 2) / alpha_num) << shift;
        }
        detail.pixels[i] = out;
    }
}

} // namespace Core::Mods
