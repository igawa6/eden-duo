// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Map and geometry: fog-of-war state and the "map:" rasteriser behind the Map widget. Much of it
// was written for Metroid Dread first and is still shaped by it (rooms, EMMI zones, water).
//   - Fog of war: MarkVisitedAt (self-tracked reveal when the game's own grid is not read),
//     IsVisited, VisitedGen, RoomExplored, MaskUnvisited, CellTables.
//   - Geometry: GeometryMask, RoomCategoryMask, BuildRoomMask, BuildBorderCoverage.
//   - GetImage: resolves any widget "src". module:, composite: and Nintendo sources go to
//     mod_module_services.cpp / mod_nx_runtime.cpp; package and romfs pictures, icons and pulses
//     are decoded and cached here; most of its body is the "map:<area>@<w>x<h>" rasteriser.
//   - IsPictureMapWidget, ResolveMapArea, StampFor / StampKey (the MapStamp shared by the image
//     cache key, the GPU re-upload gate and UiSignature), UpdateHiddenMarkers.
// Not here: drawing the Map widget (mod_ui_map_widget.cpp), module map frames (AcceptModuleMap /
// AcceptModuleMapState, mod_module_host.cpp), the image cache itself (mod_assets.cpp), the
// engine texture decoders (engine_ichigo.cpp, engine_mercury.cpp) and Dread's player finder,
// FindPlayerNode (engine_mercury.cpp).
// Flow: mostly the redraw/publish stage. GetImage and MarkVisitedAt are reached through
// RenderPage's callbacks, so they run on the redraw worker for a dispatched job and on the tick
// thread for page transitions and the synchronous path. UpdateHiddenMarkers runs from a module
// map update, on the tick thread.
// map_state_mutex (recursive) guards the map state; GetImage holds it for its whole "map:" build.
// Image-cache access goes through the asset_cache_mutex helpers.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include "common/stb.h"

#include <cstdlib>
#include "common/logging.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/mods/map_cache_identity.h"
#include "core/mods/map_overview.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "video_core/gpu.h"

namespace Core::Mods {

void ModRuntime::MarkVisitedAt(const std::string& area, float wx, float wy) {
    // Guards map_visited (mutated below) and, via GeometryMask(), map_geometry. Recursive:
    // GeometryMask() is called from inside this same function, on the same thread, while this lock
    // is already held.
    std::scoped_lock lk{map_state_mutex};
    if (game_vis_areas.count(area) != 0) {
        return; // the game's own MINIMAP_VISIBILITY drives this area's reveal; do not self-track
    }
    const auto a = manifest.map_areas.find(area);
    if (a == manifest.map_areas.end()) {
        return;
    }
    const float span_x = a->second.max_x - a->second.min_x;
    const float span_y = a->second.max_y - a->second.min_y;
    if (span_x <= 0.0f || span_y <= 0.0f) {
        return;
    }
    const float u = (wx - a->second.min_x) / span_x; // 0..1 left->right
    const float v = (wy - a->second.min_y) / span_y; // 0..1 bottom->top
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return; // player is not on this area's map (e.g. a transition)
    }
    auto& grid = map_visited[area];
    const int col = std::clamp(static_cast<int>(u * VisitedGrid::Cols), 0, VisitedGrid::Cols - 1);
    const int row = // grid row 0 is the top (max_y), matching the rasterised image
        std::clamp(static_cast<int>((1.0f - v) * VisitedGrid::Rows), 0, VisitedGrid::Rows - 1);
    // Reveal by flooding OUT from the player through cells that hold map geometry, bounded by a
    // step budget. A plain square radius punched through walls -- it lit up the room below and the
    // next room over. Flooding only across connected geometry stops at the empty cells that stand
    // for walls and floors, so the reveal fills the room the player is in and no further.
    constexpr int Cols = VisitedGrid::Cols, Rows = VisitedGrid::Rows;
    // How far the fill spreads from the player, in cells -- scaled to the grid width so a room's
    // worth of cells is revealed regardless of grid resolution (fallback only; the game's own
    // MINIMAP_VISIBILITY drives the reveal when it can be read).
    constexpr int MaxSteps = VisitedGrid::Cols / 48;
    const auto& geom = GeometryMask(area);
    const auto idx = [&](int cx, int cy) { return static_cast<size_t>(cy) * Cols + cx; };
    bool changed = false;
    const auto reveal = [&](int cx, int cy) {
        u8& cell = grid.cells[idx(cx, cy)];
        if (cell == 0) {
            cell = VisitedGrid::Visited; // self-tracked fallback marks where the player has been
            grid.prev[idx(cx, cy)] = 0;  // appeared from nothing: fade in
            grid.change_tick[idx(cx, cy)] = static_cast<u32>(tick_count);
            changed = true;
        }
    };
    std::vector<int> step(static_cast<size_t>(Cols) * Rows, -1);
    std::vector<std::pair<int, int>> queue{{col, row}};
    step[idx(col, row)] = 0;
    reveal(col, row); // the player's own cell, even at a doorway/edge
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const auto [cx, cy] = queue[qi];
        const int d = step[idx(cx, cy)];
        if (d >= MaxSteps) {
            continue;
        }
        static constexpr int DX[] = {1, -1, 0, 0}, DY[] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            const int nx = cx + DX[k], ny = cy + DY[k];
            if (nx < 0 || nx >= Cols || ny < 0 || ny >= Rows || step[idx(nx, ny)] >= 0) {
                continue;
            }
            if (geom.empty() || geom[idx(nx, ny)] == 0) {
                continue; // a wall/gap -- do not spread the reveal across it
            }
            step[idx(nx, ny)] = d + 1;
            reveal(nx, ny);
            queue.emplace_back(nx, ny);
        }
    }
    if (changed) {
        ++grid.generation;
        grid.last_reveal_tick = tick_count;
    }
}

const std::vector<u8>& ModRuntime::GeometryMask(const std::string& area) {
    // The caller must already hold map_state_mutex -- today that is always MarkVisitedAt, the
    // only call site. Not locked here itself (map_state_mutex is recursive, so it would be safe
    // either way, but locking here too would read as "this function is independently safe to call
    // without the lock", which is not true: it returns a reference INTO map_geometry, valid only
    // while the lock the caller took is still held).
    if (const auto it = map_geometry.find(area); it != map_geometry.end()) {
        return it->second;
    }
    std::vector<u8> mask(static_cast<size_t>(VisitedGrid::Cols) * VisitedGrid::Rows, 0);
    if (const auto a = manifest.map_areas.find(area); a != manifest.map_areas.end()) {
        const auto blob = ReadAssetBytes(a->second.geo);
        if (blob.size() >= 8) {
            u32 vc{}, ic{};
            std::memcpy(&vc, blob.data(), 4);
            std::memcpy(&ic, blob.data() + 4, 4);
            const size_t vbytes = static_cast<size_t>(vc) * 4;
            if (blob.size() >= 8 + vbytes + static_cast<size_t>(ic) * 4) {
                const auto* vs = reinterpret_cast<const u16*>(blob.data() + 8);
                const auto* is = reinterpret_cast<const u32*>(blob.data() + 8 + vbytes);
                Canvas c;
                c.Resize(VisitedGrid::Cols, VisitedGrid::Rows);
                c.Clear(0);
                const auto gx = [](u16 t) {
                    return static_cast<s32>(static_cast<s64>(t) * (VisitedGrid::Cols - 1) / 65535);
                };
                const auto gy = [](u16 t) {
                    return static_cast<s32>((VisitedGrid::Rows - 1) -
                                            static_cast<s64>(t) * (VisitedGrid::Rows - 1) / 65535);
                };
                for (u32 i = 0; i + 2 < ic; i += 3) {
                    const u32 x = is[i], y = is[i + 1], z = is[i + 2];
                    if (x >= vc || y >= vc || z >= vc) {
                        continue;
                    }
                    c.FillTriangle(gx(vs[x * 2]), gy(vs[x * 2 + 1]), gx(vs[y * 2]),
                                   gy(vs[y * 2 + 1]), gx(vs[z * 2]), gy(vs[z * 2 + 1]),
                                   0xFFFFFFFFu);
                }
                const auto& px = c.Pixels();
                for (size_t k = 0; k < mask.size() && k < px.size(); ++k) {
                    mask[k] = px[k] != 0 ? 1 : 0;
                }
            }
        }
    }
    return map_geometry.emplace(area, std::move(mask)).first->second;
}

namespace {
// Scanline-fill a world-space polygon into the 650x300 room mask (even-odd rule; handles concave
// collision-camera polygons). World (x up-positive y) -> grid (row 0 = top).
void FillPolyIntoMask(std::vector<u8>& mask, const std::vector<float>& poly, float min_x,
                      float min_y, float span_x, float span_y) {
    constexpr int Cols = VisitedGrid::Cols, Rows = VisitedGrid::Rows;
    const size_t n = poly.size() / 2;
    if (n < 3 || span_x <= 0.0f || span_y <= 0.0f) {
        return;
    }
    std::vector<std::pair<float, float>> gp(n);
    for (size_t k = 0; k < n; ++k) {
        gp[k].first = (poly[k * 2] - min_x) / span_x * Cols;
        gp[k].second = (1.0f - (poly[k * 2 + 1] - min_y) / span_y) * Rows;
    }
    float miny = 1e9f, maxy = -1e9f;
    for (const auto& p : gp) {
        miny = std::min(miny, p.second);
        maxy = std::max(maxy, p.second);
    }
    const int r0 = std::clamp(static_cast<int>(std::floor(miny)), 0, Rows - 1);
    const int r1 = std::clamp(static_cast<int>(std::ceil(maxy)), 0, Rows - 1);
    std::vector<float> xs;
    for (int ry = r0; ry <= r1; ++ry) {
        const float yc = static_cast<float>(ry) + 0.5f;
        xs.clear();
        for (size_t a = 0, b = n - 1; a < n; b = a++) {
            const float ya = gp[a].second, yb = gp[b].second;
            if ((ya > yc) != (yb > yc)) {
                xs.push_back(gp[a].first + (yc - ya) / (yb - ya) * (gp[b].first - gp[a].first));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            if (xs[k + 1] < 0.5f || xs[k] > static_cast<float>(Cols) - 0.5f) {
                continue; // wholly off-grid: both ends would clamp to one edge cell and paint it
            }
            const int cx0 = std::clamp(static_cast<int>(std::ceil(xs[k] - 0.5f)), 0, Cols - 1);
            const int cx1 = std::clamp(static_cast<int>(std::floor(xs[k + 1] - 0.5f)), 0, Cols - 1);
            for (int cx = cx0; cx <= cx1; ++cx) {
                mask[static_cast<size_t>(ry) * Cols + cx] = 1;
            }
        }
    }
}

/// A tunable colour times the factor the game's area-map raster applies (x6 water/heat/cold at
/// main+0xEA2778, x3 the roaming EMMI colour at 0xEA3874), clamped per channel.
u32 ScaleColor(u32 argb, float k) {
    u32 out = argb & 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        const int v = static_cast<int>(std::lround(static_cast<float>((argb >> sh) & 0xFF) * k));
        out |= static_cast<u32>(std::clamp(v, 0, 255)) << sh;
    }
    return out;
}

/// Subsamples per pixel side for the anti-aliased room silhouette (16 coverage levels).
// 1 = no subsampling: the outline is a hard 1-px line (a crisp staircase by design, not a
// blended edge); finer diagonals come from a larger raster (map.style.raster_px) instead.
constexpr int BorderSS = 1;
/// Room-outline thickness in raster pixels (1 px at the 1536-wide raster = ~8 screen px zoomed).
constexpr int BorderPx = 1;

/// Anti-aliased room silhouette + outline, from the same packed geo blob the colour raster draws.
/// The geometry is point-sampled at BorderSS x BorderSS subsamples per pixel at its EXACT
/// sub-pixel edge (the colour fills use the floor-quantised one -- they only supply colour), and a
/// subsample within BorderPx pixels (Euclidean) of the outside -- the image edge counts as outside
/// -- is outline. Per pixel: `coverage` = inside subsamples / total (the image alpha: a diagonal
/// room edge becomes a shaded line against the panel background instead of a 1-px staircase),
/// `weight` = outline subsamples / inside subsamples (the grey's share of the covered part, lerped
/// in by the fog pass so the outline never dims and the fills under it stay flat).
bool BuildBorderCoverage(const std::vector<u8>& blob, int width, int height,
                         std::vector<u8>& coverage, std::vector<u8>& weight,
                         const std::vector<std::array<u32, 6>>& holes) {
    if (blob.size() < 8 || width <= 0 || height <= 0) {
        return false;
    }
    u32 vc{}, ic{};
    std::memcpy(&vc, blob.data(), 4);
    std::memcpy(&ic, blob.data() + 4, 4);
    const size_t vbytes = static_cast<size_t>(vc) * 4;
    if (blob.size() < 8 + vbytes + static_cast<size_t>(ic) * 4) {
        return false;
    }
    const auto* vs = reinterpret_cast<const u16*>(blob.data() + 8);
    const auto* is = reinterpret_cast<const u32*>(blob.data() + 8 + vbytes);
    constexpr int S = BorderSS;
    const int W = width * S, H = height * S;
    // One byte per subsample: 1 = inside after the fill, then rewritten in place to the run
    // distance to the nearest outside subsample along the row (0 = outside), capped at T + 1.
    std::vector<u8> in(static_cast<size_t>(W) * H, 0);
    // Vertices snap to the SAME integer raster pixel as the colour raster (to_x/to_y in GetImage:
    // floor, world Y up / raster down) and the S x S block of pixel p is sampled at p + i / S with
    // the same inclusive edge test, then dilated right/down by S - 1 subsamples: every pixel the
    // colour raster fills is fully covered, so an axis-aligned room edge stays the crisp pixel
    // boundary it always was, and only a diagonal edge gains fractional coverage on its outside.
    const auto to_sx = [&](u32 v) {
        return static_cast<s32>(static_cast<s64>(v) * (width - 1) / 65535) * S;
    };
    const auto to_sy = [&](u32 v) {
        return static_cast<s32>((height - 1) - static_cast<s64>(v) * (height - 1) / 65535) * S;
    };
    const auto fill = [&](s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2, u8 value) {
        const s64 area =
            static_cast<s64>(x1 - x0) * (y2 - y0) - static_cast<s64>(y1 - y0) * (x2 - x0);
        if (area == 0) {
            return;
        }
        if (area < 0) { // normalise the winding so "inside" is all three edge functions >= 0
            std::swap(x1, x2);
            std::swap(y1, y2);
        }
        const s32 min_y = std::max(0, std::min({y0, y1, y2}));
        const s32 max_y = std::min(H - 1, std::max({y0, y1, y2}));
        const s32 min_x = std::max(0, std::min({x0, x1, x2}));
        const s32 max_x = std::min(W - 1, std::max({x0, x1, x2}));
        if (min_y > max_y || min_x > max_x) {
            return;
        }
        // Edge functions e_k = A_k * (y - y_k) - B_k * (x - x_k), stepped by -B_k along a row.
        const s64 A0 = x1 - x0, B0 = y1 - y0, A1 = x2 - x1, B1 = y2 - y1, A2 = x0 - x2,
                  B2 = y0 - y2;
        for (s32 py = min_y; py <= max_y; ++py) {
            s64 e0 = A0 * (py - y0) - B0 * (min_x - x0);
            s64 e1 = A1 * (py - y1) - B1 * (min_x - x1);
            s64 e2 = A2 * (py - y2) - B2 * (min_x - x2);
            u8* const row = in.data() + static_cast<size_t>(py) * W;
            for (s32 px = min_x; px <= max_x; ++px, e0 -= B0, e1 -= B1, e2 -= B2) {
                if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                    row[px] = value;
                }
            }
        }
    };
    for (u32 i = 0; i + 2 < ic; i += 3) {
        const u32 a = is[i], b = is[i + 1], c = is[i + 2];
        if (a >= vc || b >= vc || c >= vc) {
            continue;
        }
        fill(to_sx(vs[a * 2]), to_sy(vs[a * 2 + 1]), to_sx(vs[b * 2]), to_sy(vs[b * 2 + 1]),
             to_sx(vs[c * 2]), to_sy(vs[c * 2 + 1]), 1);
    }
    // Holes: a living destructible's concealment polys / an undispelled vignette are walls as far
    // as the map is concerned -- cut them out of the silhouette so the outline runs around them.
    for (const auto& h : holes) {
        fill(to_sx(h[0]), to_sy(h[1]), to_sx(h[2]), to_sy(h[3]), to_sx(h[4]), to_sy(h[5]), 0);
    }
    // Right/down dilation by S - 1 (see to_sx): a lattice-sampled pixel corner carries its pixel.
    for (int y = 0; y < H; ++y) {
        u8* const row = in.data() + static_cast<size_t>(y) * W;
        int run = 0;
        for (int x = 0; x < W; ++x) {
            if (row[x] != 0) {
                run = S - 1;
            } else if (run > 0) {
                row[x] = 1;
                --run;
            }
        }
    }
    std::vector<u8> carry(static_cast<size_t>(W), 0);
    for (int y = 0; y < H; ++y) {
        u8* const row = in.data() + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            u8& c = carry[static_cast<size_t>(x)];
            if (row[x] != 0) {
                c = static_cast<u8>(S - 1);
            } else if (c > 0) {
                row[x] = 1;
                --c;
            }
        }
    }
    // Outline = inside AND some outside subsample within T (Euclidean). Pass 1: per-row distance
    // to the nearest outside subsample (x = -1 and x = W are outside), min of a left and a right
    // sweep. Pass 2 (below, per pixel row): a subsample is outline when any of the 2T + 1 rows it
    // can reach has that row distance within the circle, or the row is off the image.
    constexpr int T = BorderPx * S;
    constexpr u8 Cap = static_cast<u8>(T + 1);
    for (int y = 0; y < H; ++y) {
        u8* const row = in.data() + static_cast<size_t>(y) * W;
        u8 d = 0;
        for (int x = 0; x < W; ++x) {
            d = row[x] != 0 ? std::min<u8>(Cap, static_cast<u8>(d + 1)) : u8{0};
            row[x] = d;
        }
        d = 0;
        for (int x = W - 1; x >= 0; --x) {
            d = row[x] != 0 ? std::min<u8>(Cap, static_cast<u8>(d + 1)) : u8{0};
            row[x] = std::min(row[x], d);
        }
    }
    std::array<u8, T + 1> reach{}; // reach[dy]: max row distance still inside the circle
    for (int dy = 0; dy <= T; ++dy) {
        reach[static_cast<size_t>(dy)] =
            static_cast<u8>(std::floor(std::sqrt(static_cast<double>(T * T - dy * dy))));
    }
    coverage.assign(static_cast<size_t>(width) * height, 0);
    weight.assign(static_cast<size_t>(width) * height, 0);
    std::vector<u8> outline(static_cast<size_t>(W));
    std::vector<u16> n_in(static_cast<size_t>(width)), n_out(static_cast<size_t>(width));
    for (int py = 0; py < height; ++py) {
        std::fill(n_in.begin(), n_in.end(), u16{0});
        std::fill(n_out.begin(), n_out.end(), u16{0});
        for (int j = 0; j < S; ++j) {
            const int y = py * S + j;
            const u8* const row = in.data() + static_cast<size_t>(y) * W;
            const bool rim = y < T || y >= H - T; // within T of the top/bottom image edge
            for (int x = 0; x < W; ++x) {
                outline[static_cast<size_t>(x)] = row[x] != 0 && (rim || row[x] <= T) ? 1 : 0;
            }
            for (int dy = 1; dy <= T && !rim; ++dy) {
                const u8 th = reach[static_cast<size_t>(dy)];
                for (const int yy : {y - dy, y + dy}) {
                    const u8* const other = in.data() + static_cast<size_t>(yy) * W;
                    for (int x = 0; x < W; ++x) {
                        if (row[x] != 0 && other[x] <= th) {
                            outline[static_cast<size_t>(x)] = 1;
                        }
                    }
                }
            }
            for (int x = 0; x < W; ++x) {
                if (row[x] != 0) {
                    ++n_in[static_cast<size_t>(x / S)];
                    n_out[static_cast<size_t>(x / S)] += outline[static_cast<size_t>(x)];
                }
            }
        }
        const size_t rowbase = static_cast<size_t>(py) * width;
        for (int px = 0; px < width; ++px) {
            const u32 ni = n_in[static_cast<size_t>(px)];
            if (ni == 0) {
                continue;
            }
            coverage[rowbase + px] = static_cast<u8>(ni * 255 / (S * S));
            weight[rowbase + px] = static_cast<u8>(n_out[static_cast<size_t>(px)] * 255 / ni);
        }
    }
    return true;
}
} // namespace

static std::vector<u8> BuildRoomMask(const MapArea& a,
                                     const std::vector<std::vector<float>>& polys) {
    constexpr int Cols = VisitedGrid::Cols, Rows = VisitedGrid::Rows;
    std::vector<u8> mask(static_cast<size_t>(Cols) * Rows, 0);
    const float span_x = a.max_x - a.min_x, span_y = a.max_y - a.min_y;
    for (const auto& poly : polys) {
        FillPolyIntoMask(mask, poly, a.min_x, a.min_y, span_x, span_y);
    }
    return mask;
}

const std::vector<u8>& ModRuntime::RoomCategoryMask(const std::string& area,
                                                    const std::string& category_id) const {
    // One accessor over MapArea.room_categories, keyed (area, category_id) in a single cache map.
    // Baked from a category's own `polys` (pre-baked .bmscc collision-camera / maproom
    // vertex-colour data) and scanline-filled by BuildRoomMask -- the fill ends exactly at each
    // room's own polygon (which extends to the doorway) and, since adjacent room polygons are
    // disjoint, cannot leak into a neighbour. Static per area+category -> cached.
    const std::string key = area + "\x1f" + category_id;
    if (const auto it = map_category.find(key); it != map_category.end()) {
        return it->second;
    }
    std::vector<u8> mask(static_cast<size_t>(VisitedGrid::Cols) * VisitedGrid::Rows, 0);
    if (const auto a = manifest.map_areas.find(area); a != manifest.map_areas.end()) {
        for (const auto& cat : a->second.room_categories) {
            if (cat.id == category_id) {
                mask = BuildRoomMask(a->second, cat.polys);
                break;
            }
        }
    }
    return map_category.emplace(key, std::move(mask)).first->second;
}

u64 ModRuntime::VisitedGen(const std::string& area) const {
    std::scoped_lock lk{map_state_mutex};
    const auto it = map_visited.find(area);
    return it == map_visited.end() ? 0 : it->second.generation;
}

bool ModRuntime::IsVisited(const std::string& area, float wx, float wy) const {
    // Held for the whole body: `grid` below is an iterator into map_visited, valid only while held.
    std::scoped_lock lk{map_state_mutex};
    const auto grid = map_visited.find(area);
    if (grid == map_visited.end()) {
        // Tracking not started. A package whose map is revealed by the game (Dread) shows
        // nothing until the first reveal read lands -- otherwise every icon of the area flashed
        // up before the map did; a package without a reveal source shows everything.
        return !manifest.map_style.reveal_required;
    }
    const auto a = manifest.map_areas.find(area);
    if (a == manifest.map_areas.end()) {
        return true;
    }
    const float span_x = a->second.max_x - a->second.min_x;
    const float span_y = a->second.max_y - a->second.min_y;
    if (span_x <= 0.0f || span_y <= 0.0f) {
        return true;
    }
    const float u = (wx - a->second.min_x) / span_x;
    const float v = (wy - a->second.min_y) / span_y;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return false;
    }
    const int col = std::clamp(static_cast<int>(u * VisitedGrid::Cols), 0, VisitedGrid::Cols - 1);
    const int row =
        std::clamp(static_cast<int>((1.0f - v) * VisitedGrid::Rows), 0, VisitedGrid::Rows - 1);
    return grid->second.cells[static_cast<size_t>(row) * VisitedGrid::Cols + col] != 0;
}

bool ModRuntime::RoomExplored(const std::string& area, const std::array<float, 4>& rect,
                              bool visited_only) const {
    std::scoped_lock lk{map_state_mutex}; // held for the whole body, as in IsVisited
    const auto grid = map_visited.find(area);
    const auto a = manifest.map_areas.find(area);
    if (grid == map_visited.end() || a == manifest.map_areas.end()) {
        return false;
    }
    const float span_x = a->second.max_x - a->second.min_x;
    const float span_y = a->second.max_y - a->second.min_y;
    if (span_x <= 0.0f || span_y <= 0.0f) {
        return false;
    }
    const auto col_of = [&](float wx) {
        return std::clamp(static_cast<int>((wx - a->second.min_x) / span_x * VisitedGrid::Cols), 0,
                          VisitedGrid::Cols - 1);
    };
    const auto row_of = [&](float wy) {
        return std::clamp(
            static_cast<int>((1.0f - (wy - a->second.min_y) / span_y) * VisitedGrid::Rows), 0,
            VisitedGrid::Rows - 1);
    };
    const int c0 = col_of(rect[0]), c1 = col_of(rect[2]);
    const int r0 = row_of(rect[3]), r1 = row_of(rect[1]);
    for (int r = std::min(r0, r1); r <= std::max(r0, r1); ++r) {
        for (int c = std::min(c0, c1); c <= std::max(c0, c1); ++c) {
            const u8 st = grid->second.cells[static_cast<size_t>(r) * VisitedGrid::Cols + c];
            if (visited_only ? st == VisitedGrid::Visited : st != VisitedGrid::Unexplored) {
                return true;
            }
        }
    }
    return false;
}

void ModRuntime::MaskUnvisited(std::vector<u32>& pixels, const std::string& area, int width,
                               int height, const std::vector<u8>* border_weight,
                               const std::vector<u8>* solid_color, MapFadeEndpoint endpoint) const {
    // The caller must already hold map_state_mutex -- today that is always GetImage's "map:"
    // span, which locks for its whole duration (see map_state_mutex's declaration comment for
    // why). Not re-locked here: this is real per-pixel work, and the only call site already holds
    // it.
    const auto it = map_visited.find(area);
    if (it == map_visited.end()) {
        // Reveal not read yet -> show NOTHING, not the whole map. Rendering the full map for the
        // second or two before the live grid resolves is the "it loads showing the full map first"
        // flash; a blank map that fills in as the read lands is the wanted behaviour.
        std::fill(pixels.begin(), pixels.end(), 0x00000000u);
        return;
    }
    const auto& grid = it->second;
    const u64 fade = std::max<u64>(1, manifest.map_style.fade_ticks);
    const u32 border = manifest.map_style.border;
    // The game's compositing shader (system/shd/pp_minimap_nx.bshdat, source embedded):
    //   visited  = 1.0 for a walked cell, 0.4 for a seen one (mesh builder main+0xE95194)
    //   notVisited = classColour * 0.3 when the area map is "unlocked" (downloaded at a map
    //                station: g_uColorCellLevels.z, mgr+0xD64618), else the background
    //   pixel = zone * visited + notVisited * (1 - visited)   (visible cells)
    //   pixel = notVisited                                     (unexplored cells)
    // zone = the base raster (room fill / hazard / EMMI zone), classColour = g_uNormalRoomClr,
    // g_uSpecialRoomClr, g_uTransportRoomClr or the EMMI room colour (dead ? vEmmyDeadColor :
    // vEmmyColor) x 0.5 (manager, main+0xE8FD80), by the maproom model's vertex colour.
    //
    // The package-declared room_categories are resolved ONCE HERE -- never inside the per-pixel
    // loop below, which must stay cheap. `appear` (below) only ever iterates this short vector
    // doing mask[ci] != 0 array reads.
    const auto map_area = manifest.map_areas.find(area);
    struct ResolvedCategory {
        const std::vector<u8>* mask;
        u32 color; ///< resolved (color_bind/color_map) and class_gain-scaled, ready to use as-is
        bool reveal_before_unlock;
        // This category's own visited_gain/tint/amount, copied through unresolved
        // (they are plain literals, nothing to bind/scale) so `appear` below can read them without
        // touching manifest.map_areas again per pixel.
        s32 visited_gain;
        u32 visited_tint;
        int visited_amount;
    };
    std::vector<ResolvedCategory> categories;
    if (map_area != manifest.map_areas.end()) {
        categories.reserve(map_area->second.room_categories.size());
        for (const auto& cat : map_area->second.room_categories) {
            // The walked/seen correction for the state the category's colour follows right now
            // (a correction is solved against one state's colour -- see VisitedFor).
            const auto vc = cat.VisitedFor(cat.color_bind.empty() ? std::nullopt
                                                                  : LookupBoundInt(cat.color_bind));
            categories.push_back(
                {&RoomCategoryMask(area, cat.id),
                 ScaleColor(ResolveBoundColor(cat.color, cat.color_bind, cat.color_map),
                            cat.class_gain),
                 cat.reveal_before_unlock, vc.gain, vc.tint, vc.amount});
        }
    }
    const bool unlocked = map_unlocked;
    const bool separate_overview =
        map_area != manifest.map_areas.end() && map_area->second.overview_regions.has_value();
    // The in-game minimap drives brightness from the cell's visibility state: a cell only SEEN
    // (revealed, e.g. through a door) is drawn dim, a cell Samus WALKED through at full brightness
    // (no tint -- the game distinguishes the two by brightness alone, plus the grey border).
    // `appear` turns a base (fill/layer) pixel into a category's final look; unexplored is
    // transparent. To animate, blend from the cell's PREVIOUS category's look to the current one
    // over the fade window -- so a cell fades in from nothing and a seen->walked upgrade fades its
    // colour instead of popping. All maths are per-channel and byte-order-agnostic.
    // `bw` is the pixel's grey-outline share (0..255, from the anti-aliased silhouette): the
    // room-outline border is a fixed UI colour, not a room fill, so it is lerped in AFTER the
    // dim -- a consistent grey whether a room was only seen or fully walked, and a diagonal
    // outline stays a smooth shaded line rather than a stepped exact-colour one.
    const auto appear = [&](u32 base, u8 cat, u32 bw, size_t ci) -> u32 {
        const u32 a = base & 0xFF000000u;
        // Array order = precedence, highest first. Only cheap mask[ci] != 0 array reads happen
        // here per pixel; colour/gain were resolved once above.
        u32 cls = manifest.map_style.room_class_color.value_or(manifest.map_style.room_fill);
        bool reveal_before_unlock = false;
        // The visited-correction triple to use below -- map.style's own (the uncategorized-room
        // default) unless a matched category below overrides it with its own.
        s32 vg = manifest.map_style.visited_gain;
        u32 vt = manifest.map_style.visited_tint;
        int va = manifest.map_style.visited_amount;
        for (const auto& rc : categories) {
            if (ci < rc.mask->size() && (*rc.mask)[ci] != 0) {
                cls = rc.color;
                reveal_before_unlock = rc.reveal_before_unlock;
                vg = rc.visited_gain;
                vt = rc.visited_tint;
                va = rc.visited_amount;
                break;
            }
        }
        // notVisited: classColour * category_dim_before_unlock once unlocked; a category that
        // opts into reveal_before_unlock shows it as soon as it is visible even when locked (the
        // shader's second condition, e.g. Dread's station/transport rooms).
        const bool nv_on = unlocked || (reveal_before_unlock && cat != VisitedGrid::Unexplored);
        const u32 nv =
            nv_on ? ScaleColor(cls, manifest.map_style.category_dim_before_unlock) : 0xFF000000u;
        if (cat == VisitedGrid::Unexplored) {
            // Downloaded sections have their own authored geometry. Dimming the detailed
            // silhouette here would expose passage and platform shapes before exploration.
            return unlocked && !separate_overview ? (a | (nv & 0x00FFFFFFu)) : 0x00000000u;
        }
        u32 rgb = base & 0x00FFFFFFu;
        // `base` (== room_fill for every uncategorized cell -- draw_blob painted the whole area
        // raster with that one flat colour; see its call site's own comment) stands in for the
        // game's "zone colour" raster (pp_minimap_nx.bshdat's ps_texture3), which the real
        // compositing shader samples AS-IS for an uncategorized room (no gamma, no post-process
        // step touches it after). That raster is not bit-identical to the flat tunable -- measured
        // on two large real-game samples (~55k px each) at (5,33,51) vs. our (0,19,48), a small,
        // G-biased lift that is not gamma-shaped.
        // The correction also runs for category cells (station/transport/emmi). The real shader
        // uses the category's flat colour for those (`vZone.rgb = vRoomColor`), which paint_rooms /
        // the hazard-layer bake above already draw, but on the device they still read too dark
        // without it. It uses `vg`/`vt`/`va`: map.style's fallback triple for an uncategorized
        // cell, or the matched category's OWN visited_gain/tint/amount (resolved above, in the
        // mask-match loop). Both triples default to a true no-op (gain 256/256 = 1.0x, amount
        // 0/256 = no blend), so a category that does not opt in renders unchanged.
        rgb = ApplyVisitedCorrection(rgb, vg, vt, va);
        if (cat == VisitedGrid::Revealed) { // seen: 0.4 * zone + 0.6 * notVisited
            const auto ch = [&](int sh) {
                const int z = static_cast<int>((rgb >> sh) & 0xFF);
                const int n = static_cast<int>((nv >> sh) & 0xFF);
                return static_cast<u32>(std::clamp((z * 4 + n * 6) / 10, 0, 255)) << sh;
            };
            rgb = ch(16) | ch(8) | ch(0);
        }
        if (bw != 0) {
            const auto ch = [&](int sh) {
                const int f = static_cast<int>((rgb >> sh) & 0xFF);
                const int t = static_cast<int>((border >> sh) & 0xFF);
                return static_cast<u32>(f + (t - f) * static_cast<int>(bw) / 255) << sh;
            };
            rgb = ch(16) | ch(8) | ch(0);
        }
        return a | rgb; // walked: the room's colour at 1.0x (the game's builder, main+0xE95194)
    };
    const auto blend = [](u32 from, u32 to, int num, int den) -> u32 {
        const auto ch = [&](int sh) {
            const int f = static_cast<int>((from >> sh) & 0xFF),
                      t = static_cast<int>((to >> sh) & 0xFF);
            return static_cast<u32>(std::clamp(f + (t - f) * num / den, 0, 255)) << sh;
        };
        return (ch(24) | ch(16) | ch(8) | ch(0)); // includes alpha so a fade-in ramps opacity too
    };
    const PixelCellTables& tables = CellTables(width, height);
    const u8* const bwp = (border_weight != nullptr && border_weight->size() == pixels.size())
                              ? border_weight->data()
                              : nullptr;
    const u8* const scp = (solid_color != nullptr && solid_color->size() == pixels.size())
                              ? solid_color->data()
                              : nullptr;
    for (int py = 0; py < height; ++py) {
        const size_t crow =
            static_cast<size_t>(tables.row_of_py[static_cast<size_t>(py)]) * VisitedGrid::Cols;
        const size_t rowbase = static_cast<size_t>(py) * width;
        for (int px = 0; px < width; ++px) {
            const size_t ci = crow + tables.col_of_px[static_cast<size_t>(px)];
            const u8 c = grid.cells[ci];
            u32& pix = pixels[rowbase + px];
            // Alpha is the room silhouette. Active occluders and vignettes cut holes out of it;
            // keep those pixels empty even when an unlocked/visited cell has a class colour.
            if ((pix & 0xFF000000u) == 0) {
                pix = 0;
                continue;
            }
            const u32 bw = bwp != nullptr ? bwp[rowbase + px] : 0u;
            const bool solid = scp != nullptr && scp[rowbase + px] != 0;
            const auto appearance = [&](u8 category) {
                return solid && category != VisitedGrid::Unexplored ? pix
                                                                    : appear(pix, category, bw, ci);
            };
            const u32 to = appearance(c);
            const u32 ct = grid.change_tick[ci];
            const u64 age = ct == 0 ? fade : tick_count - ct;
            if (endpoint == MapFadeEndpoint::Current || age >= fade) {
                pix = to; // settled
            } else if (endpoint == MapFadeEndpoint::Previous) {
                // Endpoint textures are mixed per cell by the GPU. A cell whose transition has
                // already settled uses the current category in both endpoints; an active cell
                // uses the exact category it changed from.
                pix = appearance(grid.prev[ci]);
            } else {
                pix = blend(appearance(grid.prev[ci]), to, static_cast<int>(age) + 1,
                            static_cast<int>(fade));
            }
        }
    }
}

const ModRuntime::PixelCellTables& ModRuntime::CellTables(int width, int height) const {
    // pixel -> reveal-grid cell, the same floor mapping every grid->pixel loop used to recompute
    // with two divisions per pixel; one table per image size, rebuilt only when the size changes.
    if (cell_tables.width != width || cell_tables.height != height) {
        cell_tables.width = width;
        cell_tables.height = height;
        cell_tables.col_of_px.resize(static_cast<size_t>(std::max(0, width)));
        cell_tables.row_of_py.resize(static_cast<size_t>(std::max(0, height)));
        for (int px = 0; px < width; ++px) {
            cell_tables.col_of_px[static_cast<size_t>(px)] =
                std::min(px * VisitedGrid::Cols / width, VisitedGrid::Cols - 1);
        }
        for (int py = 0; py < height; ++py) {
            cell_tables.row_of_py[static_cast<size_t>(py)] =
                std::min(py * VisitedGrid::Rows / height, VisitedGrid::Rows - 1);
        }
    }
    return cell_tables;
}

std::shared_ptr<const Image> ModRuntime::GetImage(const std::string& src_in,
                                                  MapFadeEndpoint endpoint) {
    if (src_in.starts_with("module:")) {
        return GetModuleImage(src_in);
    }
    // Nintendo containers and composites decode on the asset worker (mod_nx_runtime.cpp).
    if (src_in.starts_with("composite:")) {
        return GetCompositeImage(src_in.substr(10));
    }
    if (IsNxAssetSource(src_in) && !NxFallback(src_in)) {
        return GetNxImage(src_in);
    }
    // A "map:<area>@<w>x<h>" request is stamped with the area's fog-of-war generation, so the
    // cache entry is invalidated (re-rasterised) the moment a new cell is revealed. Callers do not
    // need to know the generation; it is appended here.
    std::string src = src_in;
    const auto append_map_stamp = [&](std::string& key, const std::string& area) {
        MapStamp stamp = StampFor(area);
        if (endpoint != MapFadeEndpoint::Animated) {
            stamp.fade = 0; // endpoint images change with content, never with animation time
        }
        key += StampKey(stamp);
        if (endpoint == MapFadeEndpoint::Previous) {
            key += ".prev";
        } else if (endpoint == MapFadeEndpoint::Current) {
            key += ".current";
        }
    };
    if (src == "icon:") {
        // The running title's own icon out of its control data (a JPEG), decoded once.
        if (const auto cached = CacheFindImage(src)) {
            return cached;
        }
        if (image_failed.contains(src)) {
            return nullptr;
        }
        std::vector<u8> jpeg;
        system.GetAppLoader().ReadIcon(jpeg);
        int iw = 0, ih = 0, comps = 0;
        stbi_uc* const rgba =
            jpeg.empty() ? nullptr
                         : stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &iw,
                                                 &ih, &comps, 4);
        if (rgba == nullptr || iw <= 0 || ih <= 0) {
            LOG_WARNING(Core, "DSMod: the title has no decodable icon ({} bytes)", jpeg.size());
            image_failed.insert(src);
            return nullptr;
        }
        Image icon;
        icon.w = static_cast<u32>(iw);
        icon.h = static_cast<u32>(ih);
        icon.pixels.resize(static_cast<size_t>(iw) * static_cast<size_t>(ih));
        for (size_t k = 0; k < icon.pixels.size(); ++k) {
            const u8* const p = rgba + k * 4;
            icon.pixels[k] = (u32(p[3]) << 24) | (u32(p[0]) << 16) | (u32(p[2 - 1]) << 8) | p[2];
        }
        stbi_image_free(rgba);
        LOG_INFO(Core, "DSMod: decoded the title icon ({}x{})", iw, ih);
        return CachePutImage(src, std::move(icon));
    }
    if (src.starts_with("pulse:")) {
        // The item-room pulse companion of a map composite, built beside it: ask for the map
        // (which stamps the key and rasterises if needed), then look the companion up by the
        // same stamped key. Absent when no drawn room holds an item.
        std::string mk = "map:" + src.substr(6);
        if (GetImage(mk, endpoint) == nullptr) {
            return nullptr;
        }
        if (mk.find('#') == std::string::npos) {
            if (const auto at = mk.find('@'); at != std::string::npos) {
                append_map_stamp(mk, mk.substr(4, at - 4));
            }
        }
        return CacheFindImage("pulse:" + mk.substr(4));
    }
    if (src.starts_with("map:") && src.find('#') == std::string::npos) {
        const auto at = src.find('@');
        if (at != std::string::npos) {
            const std::string area = src.substr(4, at - 4);
            // Stamp the reveal generation and the live-water generation so the map re-rasterises
            // when a new cell is revealed or a pool's level changes. While a reveal is still fading
            // in (recent last_reveal_tick), also stamp the tick so it re-rasterises every frame and
            // the fade animates; once the fade window passes it settles back to a stable cached
            // image.
            append_map_stamp(src, area);
        }
    }
    if (const auto cached = CacheFindImage(src)) {
        return cached;
    }
    if (image_failed.contains(src_in)) {
        return nullptr; // a missing asset / geometry is not re-read from disk on every publish
    }
    // "map:<area>@<w>x<h>" is drawn rather than loaded: an area's triangles are rasterised once
    // at the size asked for and then reused, because a map can run to tens of thousands of them.
    if (src.starts_with("map:")) {
        // Held for the ENTIRE map-composite-building span below (base/prefog/magnet/pulse/
        // overview) -- see map_state_mutex's own declaration comment (mod_runtime.h) for why the
        // whole-span grain was chosen over per-touch-point copies. Guards every map_visited/
        // map_occ_dead/map_vig_dispelled/map_walls/water_boxes/map_unlocked/wall_gen/water_gen/
        // zone_gen read below.
        // Announced before the lock is taken and withdrawn after it is released (declaration
        // order): the tick thread's map_state users defer rather than wait while this is set.
        struct RasterBusy {
            std::atomic<int>& count;
            explicit RasterBusy(std::atomic<int>& c) : count{c} {
                count.fetch_add(1, std::memory_order_acq_rel);
            }
            ~RasterBusy() {
                count.fetch_sub(1, std::memory_order_release);
            }
        };
        const RasterBusy raster_busy{map_raster_busy};
        std::scoped_lock state_lk{map_state_mutex};
        const auto at = src.find('@');
        const auto by = src.find('x', at == std::string::npos ? 0 : at);
        if (at == std::string::npos || by == std::string::npos) {
            return nullptr;
        }
        // The key may carry a visited-grid generation: "map:<area>@<w>x<h>#<gen>". It only makes
        // the cache entry unique per reveal state; the actual grid is read live below.
        std::string area_name = src.substr(4, at - 4);
        const auto hash = src.find('#', by);
        const int width = std::atoi(src.substr(at + 1, by - at - 1).c_str());
        const int height = std::atoi(
            src.substr(by + 1, hash == std::string::npos ? std::string::npos : hash - by - 1)
                .c_str());
        const auto area = manifest.map_areas.find(area_name);
        if (area == manifest.map_areas.end() || width <= 0 || height <= 0) {
            return nullptr;
        }
        // An area with a prerendered image never rasterises geometry: the picture is returned
        // as-is (the requested size is ignored), so an image-mode area cannot hit the
        // geo-missing failure path below. Callers that draw such an area fetch the image by its
        // own source key instead of constructing "map:" keys at all.
        if (!area->second.image.empty()) {
            return GetImage(area->second.image);
        }
        Canvas target; // only used to build the base; sized lazily in the base-build branch below
        const auto to_x = [&](u32 v) {
            return static_cast<s32>(static_cast<s64>(v) * (width - 1) / 65535);
        };
        const auto to_y = [&](u32 v) { // world Y runs up, the canvas runs down
            return static_cast<s32>((height - 1) - static_cast<s64>(v) * (height - 1) / 65535);
        };
        // Rasterise one packed geo blob with one opaque colour. Opaque (not 75% alpha) because
        // FillTriangle blends src-over: a translucent fill piled up brighter where triangles
        // overlapped (a box) and along shared edges (grid/diagonal artefacts). Solid re-sets,
        // clean.
        const auto draw_blob = [&](Canvas& tgt, const std::string& ref, u32 color) -> u32 {
            const auto blob = ReadAssetBytes(ref);
            if (blob.size() < 8) {
                return 0;
            }
            u32 vc{}, ic{};
            std::memcpy(&vc, blob.data(), 4);
            std::memcpy(&ic, blob.data() + 4, 4);
            const size_t vbytes = static_cast<size_t>(vc) * 4;
            if (blob.size() < 8 + vbytes + static_cast<size_t>(ic) * 4) {
                return 0;
            }
            const auto* vs = reinterpret_cast<const u16*>(blob.data() + 8);
            const auto* is = reinterpret_cast<const u32*>(blob.data() + 8 + vbytes);
            for (u32 i = 0; i + 2 < ic; i += 3) {
                const u32 a = is[i], b = is[i + 1], c = is[i + 2];
                if (a >= vc || b >= vc || c >= vc) {
                    continue;
                }
                tgt.FillTriangle(to_x(vs[a * 2]), to_y(vs[a * 2 + 1]), to_x(vs[b * 2]),
                                 to_y(vs[b * 2 + 1]), to_x(vs[c * 2]), to_y(vs[c * 2 + 1]), color);
            }
            return ic / 3;
        };
        const float min_x = area->second.min_x, min_y = area->second.min_y;
        const float span_x = area->second.max_x - min_x, span_y = area->second.max_y - min_y;
        // The base and outline depend on actual silhouette holes, not tile/door/item updates.
        // Manifest reload clears both caches, so geometry and style are fixed for this identity.
        const std::string silhouette =
            ActiveHoleIdentity(area->second.occluders, area->second.vignettes,
                               map_occ_dead[area_name], map_vig_dispelled[area_name]);
        const std::string border_key =
            fmt::format("{}@{}x{}#h{}", area_name, width, height, silhouette);
        const std::string base_family = fmt::format("mapbase:{}@{}x{}#", area_name, width, height);
        const std::string active_base_family = base_family + "h" + silhouette + ".e";
        // Key off EVERY category that has a color_bind (not just one live category's state) -- a
        // second live-state category must not silently share a cache entry with the wrong state:
        // id:resolved_state, one per bound category, in declaration order.
        std::string state_suffix;
        if (area != manifest.map_areas.end()) {
            for (const auto& cat : area->second.room_categories) {
                if (!cat.color_bind.empty()) {
                    state_suffix += "." + cat.id + ":" +
                                    std::to_string(LookupBoundInt(cat.color_bind).value_or(-1));
                }
            }
        }
        const std::string base_key = active_base_family + state_suffix;
        // shared_ptr, not a raw pointer into image_cache -- see CacheFindImage's own comment.
        // `base_key`'s entry (found or freshly built) is used well past this point, so it must
        // stay valid even if something erases/rehashes image_cache meanwhile.
        std::shared_ptr<const Image> base = CacheFindImage(base_key);
        if (!base) {
            static thread_local RuntimeStageStats base_stats;
            const RuntimeStageTimer base_timer{base_stats, "map-base-rebuild"};
            // Keep only the current silhouette's active/inactive zone variants for this raster.
            // Unrelated areas and sizes remain cached; old progress states cannot grow forever.
            // (still under asset_cache_mutex via CacheEraseImagesIf's own lock, not map_state_mutex
            // -- the two are independent; this loop only ever touches image_cache.)
            CacheEraseImagesIf([&](const std::string& key) {
                return key.starts_with(base_family) && !key.starts_with(active_base_family);
            });
            target.Resize(static_cast<u32>(width), static_cast<u32>(height));
            target.Clear(0x00000000);
            const u32 base_tris = draw_blob(target, area->second.geo, manifest.map_style.room_fill);
            if (base_tris == 0) {
                LOG_WARNING(Core, "DSMod: map geometry '{}' missing", area->second.geo);
                image_failed.insert(src_in);
                return nullptr;
            }
            Image b;
            b.w = static_cast<u32>(width);
            b.h = static_cast<u32>(height);
            b.pixels = target.Pixels();
            // Station rooms are filled gold (Dread's minimap "Yellow Zone"). Static per area, so it
            // is baked into the cached base; the fog mask still hides unrevealed station rooms.
            const PixelCellTables& tables = CellTables(width, height);
            // Station rooms gold (Dread's "Yellow Zone") and transport rooms purple, from the
            // per-room masks; one shared pixel->cell table lookup per pixel.
            const auto paint_rooms = [&](const std::vector<u8>& mask, u32 colour) {
                for (int py = 0; py < height; ++py) {
                    const size_t crow =
                        static_cast<size_t>(tables.row_of_py[static_cast<size_t>(py)]) *
                        VisitedGrid::Cols;
                    const size_t rowbase = static_cast<size_t>(py) * width;
                    for (int px = 0; px < width; ++px) {
                        if (mask[crow + tables.col_of_px[static_cast<size_t>(px)]] != 0 &&
                            (b.pixels[rowbase + px] & 0xFF000000u) != 0) {
                            b.pixels[rowbase + px] = colour;
                        }
                    }
                }
            };
            // bake.mode == "polys" categories. Painted in REVERSE declaration order so that, on
            // any mask overlap, the HIGHEST-precedence category (earliest in the array, same
            // convention `appear` above uses) ends up on top (Dread: transport over station).
            for (auto it = area->second.room_categories.rbegin();
                 it != area->second.room_categories.rend(); ++it) {
                if (it->bake.mode == "layer" || it->polys.empty()) {
                    continue;
                }
                paint_rooms(RoomCategoryMask(area_name, it->id),
                            ResolveBoundColor(it->color, it->color_bind, it->color_map));
            }
            // Hazard regions (heat / freeze / E.M.M.I.) are drawn as a TRANSLUCENT tint over the
            // navy room, not a solid fill -- the in-game minimap keeps the room shape visible under
            // a red/blue wash. Each layer is drawn opaque into a scratch and alpha-blended in, so
            // triangle overlap inside one layer does not pile up (src-over would over-darken).
            for (const auto& layer : area->second.layers) {
                if (layer.live_clip || layer.post_fog) {
                    continue; // water is dynamic (per-frame); post_fog composites after the fog
                }
                // A category-bake "layer" layer is painted as a SOLID state-coloured fill (Dread's
                // EMMI zone: grey while it roams, green once defeated -- read live from the
                // blackboard via the category's color_bind), like the real minimap, so it reads as
                // a clear colour rather than a muddy translucent wash. Other layers (no `category`
                // set: heat/freeze today) stay translucent tints over the navy room.
                if (!layer.category.empty()) {
                    const MapRoomCategory* cat = nullptr;
                    for (const auto& c : area->second.room_categories) {
                        if (c.id == layer.category) {
                            cat = &c;
                            break;
                        }
                    }
                    if (cat == nullptr) {
                        continue;
                    }
                    // The area-map raster (main+0xEA1D78) fills the zone with vEmmyDeadColor
                    // (#17461B) as-is once the unit is dead, else vEmmyColor (#1E1E1E) x3
                    // (0xEA3874) -> #5A5A5A. A chase (vEmmyZoneClosedColor) is shown by the
                    // page's translucent pulse and frame instead, by design.
                    const u32 ec =
                        ScaleColor(ResolveBoundColor(cat->color, cat->color_bind, cat->color_map),
                                   cat->bake.gain);
                    Canvas escratch;
                    escratch.Resize(static_cast<u32>(width), static_cast<u32>(height));
                    escratch.Clear(0x00000000);
                    draw_blob(escratch, layer.geo, ec);
                    const auto& esp = escratch.Pixels();
                    for (size_t i = 0; i < b.pixels.size() && i < esp.size(); ++i) {
                        if ((esp[i] & 0xFF000000u) != 0 && (b.pixels[i] & 0xFF000000u) != 0) {
                            b.pixels[i] = ec; // replace the room fill with the category's colour
                        }
                    }
                    continue;
                }
                // Heat / cold rooms: the game's raster writes vHeatColor / vColdColor x 6.0
                // (0xEA2778) into its zone target and the compositing shader (pp_minimap_nx)
                // uses that zone colour as the room's fill -- a solid replace, clamped.
                Canvas scratch;
                scratch.Resize(static_cast<u32>(width), static_cast<u32>(height));
                scratch.Clear(0x00000000);
                const u32 layer_color =
                    ResolveBoundColor(layer.color, layer.color_bind, layer.color_map);
                draw_blob(
                    scratch, layer.geo,
                    ScaleColor(layer_color, layer.gain.value_or(manifest.map_style.hazard_gain)));
                const auto& sp = scratch.Pixels();
                for (size_t i = 0; i < b.pixels.size() && i < sp.size(); ++i) {
                    if ((sp[i] & 0xFF000000u) == 0 || (b.pixels[i] & 0xFF000000u) == 0) {
                        continue; // only where the layer covers an actual room pixel
                    }
                    b.pixels[i] = 0xFF000000u | (sp[i] & 0x00FFFFFFu);
                }
            }
            // Room-shape outline + anti-aliased silhouette (BuildBorderCoverage): the geometry's
            // per-pixel coverage becomes the image alpha, so a diagonal room edge is a shaded
            // line rather than a 1-px staircase; the grey outline is NOT baked in but kept as a
            // per-pixel weight the fog pass lerps in (never dimmed, hidden with unrevealed rooms
            // like before). Computed on the geometry, so it follows the rooms and does not shift
            // as the player reveals more; static per area@size, shared by the EMMI variants.
            {
                // Share the outline across zone variants, replacing it only when holes change.
                auto bl = map_border.find(border_key);
                if (bl == map_border.end()) {
                    const std::string bprefix = fmt::format("{}@{}x{}#", area_name, width, height);
                    for (auto it = map_border.begin(); it != map_border.end();) {
                        it = it->first.starts_with(bprefix) ? map_border.erase(it) : std::next(it);
                    }
                    std::vector<std::array<u32, 6>> holes;
                    {
                        const float hsx = std::max(area->second.max_x - area->second.min_x, 1.0f);
                        const float hsy = std::max(area->second.max_y - area->second.min_y, 1.0f);
                        const auto hnx = [&](float wx) {
                            return static_cast<u32>(
                                std::clamp((wx - area->second.min_x) / hsx, 0.0f, 1.0f) * 65535.0f);
                        };
                        const auto hny = [&](float wy) {
                            return static_cast<u32>(
                                std::clamp((wy - area->second.min_y) / hsy, 0.0f, 1.0f) * 65535.0f);
                        };
                        const auto& hdead = map_occ_dead[area_name];
                        const auto& hdisp = map_vig_dispelled[area_name];
                        const auto add = [&](const std::vector<std::array<float, 6>>& tris) {
                            for (const auto& t : tris) {
                                holes.push_back({hnx(t[0]), hny(t[1]), hnx(t[2]), hny(t[3]),
                                                 hnx(t[4]), hny(t[5])});
                            }
                        };
                        for (const auto& oc : area->second.occluders) {
                            if (!hdead.contains(oc.name)) {
                                add(oc.tris);
                            }
                        }
                        for (const auto& vg : area->second.vignettes) {
                            if (!hdisp.contains(vg.name)) {
                                add(vg.tris);
                            }
                        }
                    }
                    BorderLayer layer;
                    if (BuildBorderCoverage(ReadAssetBytes(area->second.geo), width, height,
                                            layer.coverage, layer.weight, holes)) {
                        bl = map_border.emplace(border_key, std::move(layer)).first;
                    }
                }
                if (bl != map_border.end()) {
                    const u32 fill = manifest.map_style.room_fill & 0x00FFFFFFu;
                    const auto& cov = bl->second.coverage;
                    for (size_t i = 0; i < b.pixels.size(); ++i) {
                        const u32 c = cov[i];
                        // A fringe pixel the floor-quantised colour raster missed has no colour
                        // yet; it is (almost) all outline, so the fill under it never shows.
                        const u32 rgb = (c != 0 && (b.pixels[i] & 0xFF000000u) == 0)
                                            ? fill
                                            : (b.pixels[i] & 0x00FFFFFFu);
                        b.pixels[i] = c == 0 ? 0u : ((c << 24) | rgb);
                    }
                }
            }
            LOG_INFO(Core, "DSMod: rasterised base '{}' at {}x{}", area_name, width, height);
            base = CachePutImage(base_key, std::move(b));
        }
        // PRE-FOG (cached per water+wall state): base + live water + intact walls, NO fog. During a
        // reveal only fade_stamp / VisitedGen change, so this image is reused and the hot fade path
        // becomes copy + MaskUnvisited -- no water alloc/raster, no water loop, no wall loop.
        const std::string prefog_key = fmt::format("prefog:{}@{}x{}#{}.{}.e{}", area_name, width,
                                                   height, water_gen, wall_gen, zone_gen);
        std::shared_ptr<const Image> prefog;
        if (const auto pc = CacheFindImage(prefog_key)) {
            prefog = pc;
        } else {
            static thread_local RuntimeStageStats prefog_stats;
            const RuntimeStageTimer prefog_timer{prefog_stats, "map-prefog-rebuild"};
            Image pf;
            pf.w = static_cast<u32>(width);
            pf.h = static_cast<u32>(height);
            pf.pixels = base->pixels; // copy
            std::vector<u8> solid_water;
            std::vector<std::array<float, 4>> boxes;
            if (manifest.map_style.water_clip_live) {
                for (const auto& bx : water_boxes) {
                    // A drained pool collapses to a line; native degenerate triangles cover
                    // no pixels. Reject it before the inclusive per-pixel containment test.
                    if (bx[2] > bx[0] && bx[3] > bx[1] && bx[2] > min_x &&
                        bx[0] < area->second.max_x && bx[3] > min_y && bx[1] < area->second.max_y) {
                        boxes.push_back(bx);
                    }
                }
            }
            // Water geometry is static; only the live component boxes move when a pool drains.
            // Cache the sparse source pixels once, then test just those pixels against the live
            // boxes on a generation change. The previous implementation allocated a full scratch
            // canvas and walked width*height for every water layer on every drain.
            const std::string water_key =
                fmt::format("watermask:{}@{}x{}", area_name, width, height);
            auto water_it = map_water_pixels.find(water_key);
            if (water_it == map_water_pixels.end()) {
                std::vector<WaterPixel> pixels;
                for (const auto& layer : area->second.layers) {
                    if (!layer.live_clip)
                        continue;
                    Canvas scratch;
                    scratch.Resize(static_cast<u32>(width), static_cast<u32>(height));
                    scratch.Clear(0);
                    draw_blob(scratch, layer.geo, layer.color);
                    const auto& sp = scratch.Pixels();
                    for (int py = 0; py < height; ++py) {
                        const float wy =
                            min_y + (1.0f - static_cast<float>(py) / (height - 1)) * span_y;
                        for (int px = 0; px < width; ++px) {
                            const u32 index =
                                static_cast<u32>(static_cast<size_t>(py) * width + px);
                            if ((sp[index] & 0xFF000000u) == 0 ||
                                (base->pixels[index] & 0xFF000000u) == 0) {
                                continue;
                            }
                            pixels.push_back({index, sp[index],
                                              min_x + static_cast<float>(px) / (width - 1) * span_x,
                                              wy});
                        }
                    }
                }
                water_it = map_water_pixels.emplace(water_key, std::move(pixels)).first;
            }
            if (manifest.map_style.water_full_visible && !water_it->second.empty()) {
                solid_water.resize(pf.pixels.size());
            }
            for (const auto& pixel : water_it->second) {
                bool wet = !manifest.map_style.water_clip_live;
                if (!wet) {
                    wet = std::ranges::any_of(boxes, [&](const auto& bx) {
                        return pixel.x >= bx[0] && pixel.x <= bx[2] && pixel.y >= bx[1] &&
                               pixel.y <= bx[3];
                    });
                }
                if (!wet)
                    continue;
                if (!solid_water.empty())
                    solid_water[pixel.index] = 1;
                // vWaterColor x 6.0 (the raster's factor, 0xEA2778) as the zone fill.
                pf.pixels[pixel.index] =
                    (pf.pixels[pixel.index] & 0xFF000000u) |
                    (ScaleColor(pixel.color, manifest.map_style.water_gain) & 0x00FFFFFFu);
            }
            if (const auto wit = map_walls.find(area_name); wit != map_walls.end()) {
                // Dynamic map structure: each INTACT breakable tile is painted as its exact
                // 100-world-unit lattice block, quantized through the SAME chain as the geometry
                // raster (floor to u16 -> integer to_x/to_y -> inclusive fill), so a run of tiles
                // forms a flush wall that seals the passage on the map. When a tile breaks it
                // vanishes from map_walls (wall_gen bumps, this prefog rebuilds) and the room fill
                // shows through -- the passage opens, like the game's own minimap.
                const u32 WallColor = manifest.map_style.wall_color; // destructible-block tan
                const auto qcol = [&](float wx) {
                    const s64 qv = std::clamp<s64>(
                        static_cast<s64>((wx - min_x) / span_x * 65535.0f), 0, 65535);
                    return static_cast<int>(qv * (width - 1) / 65535);
                };
                const auto qrow = [&](float wy) {
                    const s64 qv = std::clamp<s64>(
                        static_cast<s64>((wy - min_y) / span_y * 65535.0f), 0, 65535);
                    return static_cast<int>((height - 1) - qv * (height - 1) / 65535);
                };
                const float tile_size = std::max(1.0f, manifest.map_style.grid_tile_world_size);
                for (const auto& t : wit->second) {
                    // Snap the tile to its manifest-declared lattice cell.
                    const float wx0 = min_x + std::floor((t.x - min_x) / tile_size) * tile_size;
                    const float wy0 = min_y + std::floor((t.y - min_y) / tile_size) * tile_size;
                    const int px0 = qcol(wx0), px1 = qcol(wx0 + tile_size);
                    const int py0 = qrow(wy0 + tile_size), py1 = qrow(wy0);
                    const u32 col = t.color != 0 ? t.color : WallColor;
                    for (int py = std::max(0, py0); py <= std::min(height - 1, py1); ++py) {
                        const size_t rowbase = static_cast<size_t>(py) * width;
                        for (int px = std::max(0, px0); px <= std::min(width - 1, px1); ++px) {
                            if ((pf.pixels[rowbase + px] & 0xFF000000u) != 0) {
                                pf.pixels[rowbase + px] =
                                    (pf.pixels[rowbase + px] & 0xFF000000u) | (col & 0x00FFFFFFu);
                                if (!solid_water.empty()) {
                                    solid_water[rowbase + px] = 0;
                                }
                            }
                        }
                    }
                }
            }
            // Active occluders and hidden-room vignettes are absent from the bottom-screen map.
            // They were already cut out of the cached silhouette so its outline follows the hole;
            // clear them again after water and breakable-tile overlays so no later prefog layer
            // fills the concealed geometry. Once their live state changes, wall_gen rebuilds this
            // image and the room underneath becomes visible.
            if (const auto oa = manifest.map_areas.find(area_name);
                oa != manifest.map_areas.end() &&
                (!oa->second.occluders.empty() || !oa->second.vignettes.empty())) {
                const auto& dead = map_occ_dead[area_name];
                const auto oqc = [&](float wx) {
                    const s64 qv = std::clamp<s64>(
                        static_cast<s64>((wx - min_x) / span_x * 65535.0f), 0, 65535);
                    return static_cast<int>(qv * (width - 1) / 65535);
                };
                const auto oqr = [&](float wy) {
                    const s64 qv = std::clamp<s64>(
                        static_cast<s64>((wy - min_y) / span_y * 65535.0f), 0, 65535);
                    return static_cast<int>((height - 1) - qv * (height - 1) / 65535);
                };
                const auto& dispelled = map_vig_dispelled[area_name];
                const auto paint_tris = [&](const std::vector<std::array<float, 6>>& tris) {
                    for (const auto& tr : tris) {
                        const int x0 = oqc(tr[0]), y0 = oqr(tr[1]);
                        const int x1 = oqc(tr[2]), y1 = oqr(tr[3]);
                        const int x2 = oqc(tr[4]), y2 = oqr(tr[5]);
                        const s64 tarea = static_cast<s64>(x1 - x0) * (y2 - y0) -
                                          static_cast<s64>(y1 - y0) * (x2 - x0);
                        if (tarea == 0) {
                            continue;
                        }
                        const s64 sign = tarea > 0 ? 1 : -1;
                        const int by0 = std::max(0, std::min({y0, y1, y2}));
                        const int by1 = std::min(height - 1, std::max({y0, y1, y2}));
                        const int bx0 = std::max(0, std::min({x0, x1, x2}));
                        const int bx1 = std::min(width - 1, std::max({x0, x1, x2}));
                        for (int py = by0; py <= by1; ++py) {
                            const size_t rowbase = static_cast<size_t>(py) * width;
                            for (int px = bx0; px <= bx1; ++px) {
                                const s64 e0 = (static_cast<s64>(x1 - x0) * (py - y0) -
                                                static_cast<s64>(y1 - y0) * (px - x0)) *
                                               sign;
                                const s64 e1 = (static_cast<s64>(x2 - x1) * (py - y1) -
                                                static_cast<s64>(y2 - y1) * (px - x1)) *
                                               sign;
                                const s64 e2 = (static_cast<s64>(x0 - x2) * (py - y2) -
                                                static_cast<s64>(y0 - y2) * (px - x2)) *
                                               sign;
                                if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                                    pf.pixels[rowbase + px] = 0;
                                }
                            }
                        }
                    }
                };
                for (const auto& oc : oa->second.occluders) {
                    if (!dead.contains(oc.name)) {
                        paint_tris(oc.tris);
                    }
                }
                // OCCLUDER_VIGNETTES[name] flips true when a hidden room is disclosed.
                for (const auto& vg : oa->second.vignettes) {
                    if (!dispelled.contains(vg.name)) {
                        paint_tris(vg.tris);
                    }
                }
            }
            // Keep only the newest pre-fog per area+size (never collides with "map:"/"mapbase:").
            const std::string pf_prefix = fmt::format("prefog:{}@{}x{}#", area_name, width, height);
            CacheEraseImagesIf([&](const std::string& key) {
                return key != prefog_key && key.starts_with(pf_prefix);
            });
            for (auto it = map_water_solid.begin(); it != map_water_solid.end();) {
                it = (it->first != prefog_key && it->first.starts_with(pf_prefix))
                         ? map_water_solid.erase(it)
                         : std::next(it);
            }
            if (!solid_water.empty()) {
                map_water_solid.insert_or_assign(prefog_key, std::move(solid_water));
            }
            prefog = CachePutImage(prefog_key, std::move(pf));
        }
        // FOG (cheap, per reveal/fade state): copy the pre-fog image and mask + fade it.
        Image image;
        image.w = static_cast<u32>(width);
        image.h = static_cast<u32>(height);
        image.pixels = prefog->pixels; // copy before any cache mutation below
        const auto bwl = map_border.find(border_key);
        const auto swl = map_water_solid.find(prefog_key);
        MaskUnvisited(image.pixels, area_name, width, height, // fog mask + reveal/colour fade
                      bwl != map_border.end() ? &bwl->second.weight : nullptr,
                      swl != map_water_solid.end() ? &swl->second : nullptr, endpoint);
        // Breakable tiles are alpha-1 items in the game's colour layer: the compositing shader
        // forces visited = visible for them, so a tile in a merely seen cell keeps its full
        // weapon colour. Re-stamp them over the fogged image wherever the cell is visible.
        if (const auto wit = map_walls.find(area_name);
            wit != map_walls.end() && !wit->second.empty()) {
            if (const auto vit = map_visited.find(area_name); vit != map_visited.end()) {
                const auto& cells = vit->second.cells;
                const PixelCellTables& tables = CellTables(width, height);
                const auto ma2 = manifest.map_areas.find(area_name);
                if (ma2 != manifest.map_areas.end()) {
                    const float tmin_x = ma2->second.min_x, tmin_y = ma2->second.min_y;
                    const float tspan_x = std::max(ma2->second.max_x - tmin_x, 1.0f);
                    const float tspan_y = std::max(ma2->second.max_y - tmin_y, 1.0f);
                    const auto qcol = [&](float wx) {
                        const s64 qv = std::clamp<s64>(
                            static_cast<s64>((wx - tmin_x) / tspan_x * 65535.0f), 0, 65535);
                        return static_cast<int>(qv * (width - 1) / 65535);
                    };
                    const auto qrow = [&](float wy) {
                        const s64 qv = std::clamp<s64>(
                            static_cast<s64>((wy - tmin_y) / tspan_y * 65535.0f), 0, 65535);
                        return static_cast<int>((height - 1) - qv * (height - 1) / 65535);
                    };
                    const float tile_size = std::max(1.0f, manifest.map_style.grid_tile_world_size);
                    for (const auto& t : wit->second) {
                        const float wx0 =
                            tmin_x + std::floor((t.x - tmin_x) / tile_size) * tile_size;
                        const float wy0 =
                            tmin_y + std::floor((t.y - tmin_y) / tile_size) * tile_size;
                        const int px0 = qcol(wx0), px1 = qcol(wx0 + tile_size);
                        const int py0 = qrow(wy0 + tile_size), py1 = qrow(wy0);
                        const u32 col = t.color != 0 ? t.color : manifest.map_style.wall_color;
                        for (int py = std::max(0, py0); py <= std::min(height - 1, py1); ++py) {
                            const int r = tables.row_of_py[static_cast<size_t>(py)];
                            for (int px = std::max(0, px0); px <= std::min(width - 1, px1); ++px) {
                                const size_t i = static_cast<size_t>(py) * width + px;
                                const int c = tables.col_of_px[static_cast<size_t>(px)];
                                if (c < 0 || c >= VisitedGrid::Cols || r < 0 ||
                                    r >= VisitedGrid::Rows ||
                                    cells[static_cast<size_t>(r) * VisitedGrid::Cols + c] ==
                                        VisitedGrid::Unexplored ||
                                    ((image.pixels[i] & 0xFF000000u) == 0 &&
                                     endpoint != MapFadeEndpoint::Previous)) {
                                    continue;
                                }
                                image.pixels[i] =
                                    (image.pixels[i] & 0xFF000000u) | (col & 0x00FFFFFFu);
                            }
                        }
                    }
                }
            }
        }
        // Magnet-surface overlay, POST-fog: magnet walls sit on the room fringe the fog hides, so
        // baking them into the base then masking erased them. Draw them full-blue here, but only
        // where the cell (or a neighbouring cell) is revealed, so a magnet line shows beside
        // explored rooms and never floats in unexplored void. The magnet raster is cached per
        // area@size; the per-composite work is one table lookup per magnet pixel.
        if (const auto ma = manifest.map_areas.find(area_name); ma != manifest.map_areas.end()) {
            const std::string mkey = fmt::format("magnet:{}@{}x{}", area_name, width, height);
            std::shared_ptr<const Image> magnet = CacheFindImage(mkey);
            if (!magnet) {
                Image m;
                m.w = static_cast<u32>(width);
                m.h = static_cast<u32>(height);
                Canvas ms;
                ms.Resize(static_cast<u32>(width), static_cast<u32>(height));
                ms.Clear(0x00000000);
                // Every post_fog layer composites, in declaration order.
                for (const auto& layer : ma->second.layers) {
                    if (layer.post_fog) {
                        draw_blob(
                            ms, layer.geo,
                            ResolveBoundColor(layer.color, layer.color_bind, layer.color_map));
                    }
                }
                m.pixels = ms.Pixels();
                magnet = CachePutImage(mkey, std::move(m));
            }
            if (const auto vit = map_visited.find(area_name);
                vit != map_visited.end() && magnet->Valid()) {
                const auto& cells = vit->second.cells;
                const PixelCellTables& tables = CellTables(width, height);
                constexpr int C = VisitedGrid::Cols, Rw = VisitedGrid::Rows;
                // The game paints magnet surfaces in the same cell pass as the rooms, so a
                // magnet line shows only in a revealed cell and takes that cell's intensity
                // (0.4x seen, 1.0x walked -- main+0xE95194); nothing spills into unexplored cells.
                const auto& mp = magnet->pixels;
                for (int y = 0; y < height; ++y) {
                    const int r = tables.row_of_py[static_cast<size_t>(y)];
                    for (int x = 0; x < width; ++x) {
                        const size_t i = static_cast<size_t>(y) * width + x;
                        if ((mp[i] & 0xFF000000u) == 0) {
                            continue;
                        }
                        const int c = tables.col_of_px[static_cast<size_t>(x)];
                        if (c < 0 || c >= C || r < 0 || r >= Rw) {
                            continue;
                        }
                        const u8 st = cells[static_cast<size_t>(r) * C + c];
                        if (st == VisitedGrid::Unexplored) {
                            continue;
                        }
                        // pp_minimap_nx: an alpha-1 item (magnet surface, tile) is drawn with
                        // visited forced to visible -- full colour once the cell is visible.
                        image.pixels[i] = mp[i];
                    }
                }
            }
        }
        // Item-room pulse companion: the rooms (camera rects) still holding an uncollected,
        // revealed item, as the composite's own pixels in white -- the map widget draws it over
        // the map at a slow blink, like the minimap's pulse for rooms with items left. Cached as
        // "pulse:<same key>" beside the composite and rebuilt with it (a pickup bumps wall_gen).
        if (endpoint != MapFadeEndpoint::Previous) {
            const std::string pprefix = fmt::format("pulse:{}@{}x{}#", area_name, width, height);
            CacheEraseImagesIf([&](const std::string& key) {
                const bool candidate = key.starts_with(pprefix);
                const bool endpoint_key = key.ends_with(".prev") || key.ends_with(".current");
                const bool same_family =
                    endpoint == MapFadeEndpoint::Animated ? !endpoint_key : endpoint_key;
                return candidate && same_family;
            });
            Image pulse;
            pulse.w = image.w;
            pulse.h = image.h;
            pulse.pixels.assign(image.pixels.size(), 0u);
            bool any = false;
            if (const auto ma = manifest.map_areas.find(area_name);
                ma != manifest.map_areas.end()) {
                const MapArea& A = ma->second;
                const float sx = std::max(A.max_x - A.min_x, 1.0f);
                const float sy = std::max(A.max_y - A.min_y, 1.0f);
                const auto nx = [&](float wx) {
                    return static_cast<u32>(std::clamp((wx - A.min_x) / sx, 0.0f, 1.0f) * 65535.0f);
                };
                const auto ny = [&](float wy) {
                    return static_cast<u32>(std::clamp((wy - A.min_y) / sy, 0.0f, 1.0f) * 65535.0f);
                };
                // The minimap's rule (checked against the game on the handheld): a room the
                // player has VISITED pulses while it still holds a hidden item they have not
                // found (Unveiled == 0). Rooms only revealed by a map station do not pulse, and
                // neither do found (Unveiled == 1) or plain-sight items. An item still concealed
                // behind a room vignette counts -- that concealment IS what the pulse hints at
                // (the three green-zone rooms the handheld blinked were all vignette items).
                // What pulses is the item's own map box (the game's oBox, whole map cells --
                // the top screen highlights that box, not the room), gated on the room having
                // been walked. An item without a box falls back to its room.
                // An item's tile is usually inside a wall or a breakable block, i.e. outside
                // the drawn room shape, so the box is painted whole (the game draws its
                // highlight over the tile the same way); a room fallback keeps to room pixels.
                const auto paint = [&](const std::array<float, 4>& box, bool whole) {
                    const int px0 = std::clamp(to_x(nx(box[0])), 0, width - 1);
                    const int px1 = std::clamp(to_x(nx(box[2])), 0, width - 1);
                    const int py0 = std::clamp(to_y(ny(box[3])), 0, height - 1);
                    const int py1 = std::clamp(to_y(ny(box[1])), 0, height - 1);
                    for (int y = py0; y <= py1; ++y) {
                        for (int x = px0; x <= px1; ++x) {
                            const size_t i = static_cast<size_t>(y) * width + x;
                            const u32 p = image.pixels[i];
                            const bool opaque = (p & 0xFF000000u) != 0;
                            // A living occluder / undispelled vignette is painted near-black
                            // over the room (a sealed gate, a hidden room): the pulse must not
                            // lift it to grey, or the wall reads as an open passage.
                            const u32 lum = ((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF);
                            if (opaque && lum < 24) {
                                continue;
                            }
                            if (whole || opaque) {
                                if (manifest.map_style.marker_pulse_gain > 0.0f) {
                                    // Both renderers alpha-composite this pulse over the same map
                                    // image. A gained copy therefore blinks toward a brighter form
                                    // of the underlying colour while retaining its hue.
                                    const auto brighten = [&](u32 shift) {
                                        return static_cast<u32>(std::clamp(
                                            std::lround(static_cast<float>((p >> shift) & 0xFF) *
                                                        manifest.map_style.marker_pulse_gain),
                                            0l, 255l));
                                    };
                                    pulse.pixels[i] = (p & 0xFF000000u) | (brighten(16) << 16) |
                                                      (brighten(8) << 8) | brighten(0);
                                    any |= opaque;
                                } else {
                                    // Fixed highlight colour: the pulse pixel becomes this flat
                                    // colour outright (opaque), so the composite alpha
                                    // (GeometryMapDraw::DrawBaseLayer, mod_ui_map_widget.cpp,
                                    // marker_pulse_peak) blends the room straight toward it rather
                                    // than toward a gained copy of the room's own hue.
                                    // This is Dread's path (marker_pulse_gain left at the struct
                                    // default, 0): marker_pulse_color is the game's own
                                    // g_uHighlightColor/vHighlightColor (#FFFFAE00). A
                                    // hue-preserving x2.0 multiply tops out around (9,57,87) even
                                    // against the corrected base colour, because multiplying a
                                    // dark, desaturated navy can never reach a bright, saturated
                                    // result. Blending toward the game's own fixed highlight
                                    // colour can.
                                    pulse.pixels[i] = manifest.map_style.marker_pulse_color;
                                    any = true;
                                }
                            }
                        }
                    }
                };
                for (const auto& mk : A.markers) {
                    if (mk.kind != "Items" || !mk.veiled || mk.collected) {
                        continue;
                    }
                    // the smallest camera holding the item is its room; it must have been walked
                    const std::array<float, 4>* room = nullptr;
                    float best = 3.4e38f;
                    for (const auto& cr : A.camera_rects) {
                        if (mk.x >= cr[0] && mk.x <= cr[2] && mk.y >= cr[1] && mk.y <= cr[3]) {
                            const float ar = (cr[2] - cr[0]) * (cr[3] - cr[1]);
                            if (ar < best) {
                                best = ar;
                                room = &cr;
                            }
                        }
                    }
                    if (room == nullptr || !RoomExplored(area_name, *room, true)) {
                        continue;
                    }
                    LOG_DEBUG(Core, "DSMod pulse '{}': item {} box={}", area_name, mk.name,
                              mk.has_pulse_box);
                    // The game's own hint rectangle wins: the room pixels inside it pulse (a
                    // box clipped to the room, as on the top screen). Without one, the item's
                    // own tile is painted whole; without either, the room.
                    if (mk.has_hint_box) {
                        paint({mk.hx0, mk.hy0, mk.hx1, mk.hy1}, false);
                    } else if (mk.has_pulse_box) {
                        paint({mk.px0, mk.py0, mk.px1, mk.py1}, true);
                    } else {
                        paint(*room, false);
                    }
                }
            }
            if (any) {
                CachePutImage("pulse:" + src.substr(4), std::move(pulse));
            }
        }
        // Add the downloaded section map underneath the explored detail only after building
        // detail overlays and the item pulse. Neither a hidden collision contour nor the
        // coarse background may create extra item hints or modify the game's reveal grid.
        if (map_unlocked && map_visited.contains(area_name) &&
            area->second.overview_regions.has_value()) {
            // The coarse overview raster colours its "station"/"transport"/"zone" region kinds
            // by room category. Each class colour is resolved here (once per rebuild, same as
            // everywhere else) and RasterizeMapOverview gets a plain lookup, so map_overview.h
            // stays free of any room_categories/ModRuntime coupling. "zone" is matched to
            // whichever category uses bake.mode:"layer" (what actually makes it zone-shaped, not
            // its id string), so a package naming that category anything other than "emmi" still
            // works.
            std::unordered_map<std::string, u32> overview_colors;
            for (const auto& cat : area->second.room_categories) {
                const u32 col = ScaleColor(
                    ResolveBoundColor(cat.color, cat.color_bind, cat.color_map), cat.class_gain);
                if (cat.id == "station" || cat.id == "transport") {
                    overview_colors[cat.id] = col;
                } else if (cat.bake.mode == "layer") {
                    overview_colors["zone"] = col;
                }
            }
            const std::string overview_key =
                fmt::format("overview:{}@{}x{}#{}", area_name, width, height, state_suffix);
            std::shared_ptr<const Image> overview = CacheFindImage(overview_key);
            if (!overview) {
                overview = CachePutImage(overview_key,
                                         RasterizeMapOverview(area->second, manifest.map_style,
                                                              width, height, overview_colors));
            }
            CompositeMapOverview(image, *overview);
        }
        // Keep only the newest composite per area+size (never evicts the base/pre-fog).
        const std::string prefix = fmt::format("map:{}@{}x{}#", area_name, width, height);
        std::string sibling;
        if (endpoint == MapFadeEndpoint::Previous) {
            sibling = src.substr(0, src.size() - std::string_view{".prev"}.size()) + ".current";
        } else if (endpoint == MapFadeEndpoint::Current) {
            sibling = src.substr(0, src.size() - std::string_view{".current"}.size()) + ".prev";
        }
        CacheEraseImagesIf([&](const std::string& key) {
            const bool candidate = key.starts_with(prefix);
            const bool endpoint_key = key.ends_with(".prev") || key.ends_with(".current");
            const bool same_family =
                endpoint == MapFadeEndpoint::Animated ? !endpoint_key : endpoint_key;
            return candidate && same_family && key != src && key != sibling;
        });
        return CachePutImage(src, std::move(image));
    }
    if (image_failed.contains(src)) {
        return nullptr;
    }

    const auto bytes = ReadAssetBytes(src);
    if (bytes.empty()) {
        LOG_WARNING(Core, "DSMod: asset '{}' not found", src);
        image_failed.insert(src);
        return nullptr;
    }
    // The game's own textures, read straight out of its filesystem. A package that names one of
    // these carries a rect and no pixels, so nothing of the game travels with it.
    if (bytes.size() > 4 && std::memcmp(bytes.data(), "BNTX", 4) == 0) {
        Image texture;
        if (!DecodeBntx(bytes, texture)) {
            LOG_WARNING(Core, "DSMod: '{}' is a BNTX this build cannot decode", src);
            image_failed.insert(src);
            return nullptr;
        }
        LOG_INFO(Core, "DSMod: decoded '{}' ({}x{}) from the game's own filesystem", src, texture.w,
                 texture.h);
        return CachePutImage(src, std::move(texture));
    }
    if (bytes.size() > 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
        Image texture;
        if (!DecodeDds(bytes, texture)) {
            LOG_WARNING(Core, "DSMod: '{}' is a DDS this build cannot decode", src);
            image_failed.insert(src);
            return nullptr;
        }
        LOG_INFO(Core, "DSMod: decoded '{}' ({}x{}) from the game's own filesystem", src, texture.w,
                 texture.h);
        return CachePutImage(src, std::move(texture));
    }
    if (bytes.size() > 8 && std::memcmp(bytes.data(), "DFvN", 4) == 0) {
        Image texture;
        if (!DecodeSosXtx(bytes, texture)) {
            LOG_WARNING(Core, "DSMod: '{}' is an XTX this build cannot decode", src);
            image_failed.insert(src);
            return nullptr;
        }
        LOG_INFO(Core, "DSMod: decoded '{}' ({}x{}) from the game's own filesystem", src, texture.w,
                 texture.h);
        return CachePutImage(src, std::move(texture));
    }
    if (bytes.size() > 4 && std::memcmp(bytes.data(), "MTXT", 4) == 0) {
        Image texture;
        if (!DecodeBctex(bytes, texture)) {
            LOG_WARNING(Core, "DSMod: '{}' is a Mercury texture this build cannot decode", src);
            image_failed.insert(src);
            return nullptr;
        }
        LOG_INFO(Core, "DSMod: decoded '{}' ({}x{}) from the game's own filesystem", src, texture.w,
                 texture.h);
        return CachePutImage(src, std::move(texture));
    }
    int width{}, height{}, channels{};
    stbi_uc* const decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                   &width, &height, &channels, 4);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        LOG_WARNING(Core, "DSMod: asset '{}' is {} bytes but does not decode as an image", src,
                    bytes.size());
        image_failed.insert(src);
        stbi_image_free(decoded);
        return nullptr;
    }
    Image image;
    image.w = static_cast<u32>(width);
    image.h = static_cast<u32>(height);
    image.pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const stbi_uc* const px = decoded + i * 4;
        image.pixels[i] = (static_cast<u32>(px[3]) << 24) | (static_cast<u32>(px[0]) << 16) |
                          (static_cast<u32>(px[1]) << 8) | static_cast<u32>(px[2]);
    }
    stbi_image_free(decoded);
    LOG_INFO(Core, "DSMod: loaded asset '{}' ({}x{})", src, image.w, image.h);
    return CachePutImage(src, std::move(image));
}

bool ModRuntime::IsPictureMapWidget(const Widget& widget, const StateSnapshot& s) const {
    // A fixed-area picture map has nothing that moves by itself: its view glides, fades and marker
    // moves already change the signature (or invalidate it).
    if (widget.type != WidgetType::Map) {
        return false;
    }
    if (widget.area_bind.empty() && widget.room_bind.empty()) {
        const auto a = manifest.map_areas.find(widget.area);
        return a != manifest.map_areas.end() && !a->second.image.empty();
    }
    // area_bind/room_bind: the area this tick's snapshot resolves to (exactly as the Map case
    // does). It is a still picture when that area draws the game's image (image mode: no fog, no
    // room pulse) and nothing on it is clocked: no static markers (uncollected items blink), no
    // atlas player icon (it blinks) and no roaming actor (it breathes). Anything else keeps the
    // conservative continuous redraw.
    const auto a = manifest.map_areas.find(ResolveMapArea(widget, s));
    return a != manifest.map_areas.end() && !a->second.image.empty() && a->second.markers.empty() &&
           widget.marker_icon.empty() && (widget.actor_x_bind.empty() || widget.actor_icon.empty());
}

std::string ModRuntime::ResolveMapArea(const Widget& w, const StateSnapshot& s) const {
    // Resolve the area exactly as the Map widget itself does (SelectMapArea, mod_ui_map_widget.cpp)
    // -- a small, deliberately duplicated copy.
    std::string area;
    if (!w.room_bind.empty()) {
        if (const auto it = s.texts.find(w.room_bind); it != s.texts.end()) {
            if (const auto room = manifest.map_rooms.find(it->second);
                room != manifest.map_rooms.end()) {
                area = room->second.area;
            } else if (manifest.map_areas.contains(it->second)) {
                area = it->second;
            }
        }
    }
    if (area.empty() && !w.area_bind.empty()) {
        if (const auto zone = s.ints.find(w.area_bind); zone != s.ints.end()) {
            if (!w.area_season_bind.empty()) {
                if (const auto season = s.ints.find(w.area_season_bind);
                    season != s.ints.end() && season->second >= 0 && season->second < 4) {
                    if (const auto mapped =
                            manifest.zone_area.find(zone->second * 4 + season->second);
                        mapped != manifest.zone_area.end()) {
                        area = mapped->second;
                    }
                }
            }
            if (area.empty()) {
                if (const auto mapped = manifest.zone_area.find(zone->second);
                    mapped != manifest.zone_area.end()) {
                    area = mapped->second;
                }
            }
        }
    }
    if (area.empty()) {
        area = w.area;
    }
    return area;
}

ModRuntime::MapStamp ModRuntime::StampFor(const std::string& area) const {
    // Guards map_visited plus water_gen/wall_gen/zone_gen below. Recursive:
    // VisitedGen() below re-locks on the same thread.
    std::scoped_lock lk{map_state_mutex};
    // Everything a rasterised area image depends on. The fade term steps at ~15 Hz only while a
    // reveal is fading, so the colour transition animates and then the stamp settles.
    u64 fade = 0;
    if (const auto it = map_visited.find(area);
        it != map_visited.end() && tick_count - it->second.last_reveal_tick <
                                       std::max<u64>(1, manifest.map_style.fade_ticks)) {
        fade = tick_count / 4;
    }
    return {VisitedGen(area), water_gen, fade, wall_gen, zone_gen};
}

std::string ModRuntime::StampKey(const MapStamp& s) {
    return fmt::format("#{}.{}.{}.{}.e{}", s.vis, s.water, s.fade, s.wall, s.emmy);
}

void ModRuntime::UpdateHiddenMarkers(const std::string& area) {
    // Guards map_occ_dead/map_vig_dispelled/map_door_open/map_item_picked/map_item_unveiled/
    // map_item_veiled (read below) and marker_gen (written below). Callers (AcceptModuleMapState)
    // may already hold this lock -- recursive_mutex makes that safe.
    // It also covers the write to ma->second.markers[i].hidden/opened/collected/unveiled/veiled
    // just below, whose reader is the Map widget's draw (GeometryMapDraw::DrawAtlasLayer,
    // mod_ui_map_widget.cpp), which reads these live off the Manifest, outside StateSnapshot.
    // RenderPage takes this same mutex as its manifest_markers_mutex parameter (see mod_ui.h's
    // declaration comment; ModRuntime::RenderPageTo and RunRedrawJob both pass &map_state_mutex),
    // so the writer and the reader share one lock.
    std::scoped_lock lk{map_state_mutex};
    // An icon is hidden while its shield actor is dead, or while the vignette hiding its room is
    // still active (not yet dispelled).
    const auto ma = manifest.map_areas.find(area);
    if (ma == manifest.map_areas.end()) {
        return;
    }
    const auto& dead = map_occ_dead[area];
    const auto& dispelled = map_vig_dispelled[area];
    const auto& opened = map_door_open[area];
    const auto& picked = map_item_picked[area];
    const auto& unveiled = map_item_unveiled[area];
    const auto& veiled = map_item_veiled[area];
    for (auto& mk : ma->second.markers) {
        mk.hidden = (!mk.name.empty() && dead.contains(mk.name)) ||
                    (!mk.vignette.empty() && !dispelled.contains(mk.vignette));
        mk.opened = !mk.name.empty() && opened.contains(mk.name);
        mk.collected = !mk.name.empty() && picked.contains(mk.name);
        mk.unveiled = !mk.name.empty() && unveiled.contains(mk.name);
        mk.veiled = !mk.name.empty() && veiled.contains(mk.name);
    }
    ++marker_gen; // markers only: the UI signature redraws, the raster is untouched
}

} // namespace Core::Mods
