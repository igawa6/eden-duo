// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Mercury Engine (Metroid Dread) code in core, used only by the Metroid Dread package.
//   - DecodeBctex (static): a .bctex texture, "MTXT" + gzip (InflateGzip) around an NVN XTX
//     container, from GetImage (mod_map.cpp).
//   - ParseMfnt (static): the MFNT font-metrics container. LoadFont (mod_assets.cpp) tries it
//     after the module's own font extension; the released Dread modules export none, so this
//     fallback has to stay in core.
//   - FindPlayerNode: a manifest "player" point (a scene-graph node class and the offset of its
//     position) resolved by scoring every node of that class against the stick. Reached from
//     ResolvePoint (mod_state.cpp) during sampling, on the tick thread.
// Why it lives here: it is specific to one engine and one title, so it stays out of the
// generic runtime files. BlockLinearOffset comes from engine_formats.h.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>

#include <zlib.h>

#include "bc_decoder.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"
#include "core/mods/engine_formats.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {

namespace {

/// Inflate a gzip stream. Mercury wraps its textures in one; zlib is already linked into core.
std::vector<u8> InflateGzip(std::span<const u8> in) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        return {};
    }
    std::vector<u8> out(1 << 20);
    size_t used = 0;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    for (int status = Z_OK; status != Z_STREAM_END;) {
        if (used == out.size()) {
            out.resize(out.size() * 2);
        }
        zs.next_out = out.data() + used;
        zs.avail_out = static_cast<uInt>(out.size() - used);
        status = inflate(&zs, Z_NO_FLUSH);
        used = out.size() - zs.avail_out;
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&zs);
            return {};
        }
    }
    inflateEnd(&zs);
    out.resize(used);
    return out;
}
} // namespace

/// Decode a Mercury Engine texture: "MTXT" + gzip around an NVN image in an XTX container.
///
/// The pixels live in the XTX *DATA* block, not at the texture_offset in the outer header -- that
/// offset lands one GOB row early and shreds the picture in a way that still looks nearly right.
/// Block height comes from the texture block's layout field as a power of two.
///
/// Decoding here rather than shipping extracted PNGs is the point: a package can then name the
/// game's own texture and a rect within it, and carry no art at all.
bool ModRuntime::DecodeBctex(std::span<const u8> file, Image& out) {
    static constexpr u32 FormatRgba8 = 0x25, FormatBgra8 = 0x6D;
    static constexpr u32 BlockTexture = 2, BlockData = 3;
    if (file.size() < 8 || std::memcmp(file.data(), "MTXT", 4) != 0) {
        return false;
    }
    static constexpr std::array<u8, 3> GzipMagic{0x1F, 0x8B, 0x08};
    const auto gz = std::search(file.begin(), file.end(), GzipMagic.begin(), GzipMagic.end());
    if (gz == file.end()) {
        return false;
    }
    const std::vector<u8> raw = InflateGzip(file.subspan(static_cast<size_t>(gz - file.begin())));
    if (raw.size() < 0x100) {
        LOG_WARNING(Core, "DSMod: bctex gzip stream inflated to only {} bytes", raw.size());
        return false;
    }
    const auto read32 = [&raw](size_t at) {
        u32 v{};
        std::memcpy(&v, raw.data() + at, sizeof(v));
        return v;
    };
    const auto read64 = [&raw](size_t at) {
        u64 v{};
        std::memcpy(&v, raw.data() + at, sizeof(v));
        return v;
    };
    static constexpr std::array<u8, 4> XtxMagic{'D', 'F', 'v', 'N'};
    const auto xtx = std::search(raw.begin(), raw.end(), XtxMagic.begin(), XtxMagic.end());
    if (xtx == raw.end() || raw.size() < 32) {
        return false;
    }
    const size_t xtx_at = static_cast<size_t>(xtx - raw.begin());
    size_t at = xtx_at + read32(xtx_at + 4); // past the XTX header
    u32 width{}, height{}, format{}, block_height = 16;
    std::span<const u8> pixels;
    while (at + 36 <= raw.size() && std::memcmp(raw.data() + at, "HBvN", 4) == 0) {
        const u64 data_size = read64(at + 8);
        const s64 data_offset = static_cast<s64>(read64(at + 16));
        const u32 type = read32(at + 24);
        const size_t data_at = static_cast<size_t>(static_cast<s64>(at) + data_offset);
        if (data_at + data_size > raw.size()) {
            break;
        }
        if (type == BlockTexture && data_size >= 0x70) {
            width = read32(data_at + 12);
            height = read32(data_at + 16);
            format = read32(data_at + 28);
            block_height = 1u << read32(data_at + 40 + 17 * 4);
        } else if (type == BlockData) {
            pixels = std::span<const u8>(raw.data() + data_at, static_cast<size_t>(data_size));
        }
        at = data_at + static_cast<size_t>(data_size);
    }
    if (width == 0 || height == 0 || pixels.empty()) {
        LOG_WARNING(Core, "DSMod: bctex walk found {}x{} and {} pixel bytes", width, height,
                    pixels.size());
        return false;
    }
    static constexpr u32 FormatBc1 = 0x42, FormatBc3 = 0x44;
    if (format == FormatBc1 || format == FormatBc3) {
        // Block-compressed. The tiling works on 4x4 blocks rather than pixels, so the deswizzle
        // runs over a grid a quarter the size with the block as its unit. Dread's fonts are BC3,
        // and their coverage is in alpha.
        const u32 block_bytes = format == FormatBc1 ? 8u : 16u;
        const u32 wide = (width + 3) / 4, tall = (height + 3) / 4;
        const u32 gobs = (wide * block_bytes + 63) / 64;
        std::vector<u8> rgba(static_cast<size_t>(width) * height * 4, 0);
        for (u32 by = 0; by < tall; ++by) {
            for (u32 bx = 0; bx < wide; ++bx) {
                const u64 at_src = BlockLinearOffset(bx, by, block_bytes, block_height, gobs);
                if (at_src + block_bytes > pixels.size()) {
                    continue;
                }
                const u32 px = bx * 4, py = by * 4;
                // The decoder writes at channel + i*bpp + j*pitch and uses x/y only to clip, so
                // the destination has to point at this block's own corner already. Handing it the
                // buffer base puts every block on the first four rows.
                u8* const dst = rgba.data() + (static_cast<size_t>(py) * width + px) * 4;
                if (format == FormatBc1) {
                    bcn::DecodeBc1(pixels.data() + at_src, dst, px, py, width, height);
                } else {
                    bcn::DecodeBc3(pixels.data() + at_src, dst, px, py, width, height);
                }
            }
        }
        out.w = width;
        out.h = height;
        out.pixels.resize(static_cast<size_t>(width) * height);
        for (size_t i = 0; i < out.pixels.size(); ++i) {
            const u8 r = rgba[i * 4 + 0], g = rgba[i * 4 + 1];
            const u8 b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
            out.pixels[i] = (static_cast<u32>(a) << 24) | (static_cast<u32>(r) << 16) |
                            (static_cast<u32>(g) << 8) | b;
        }
        return true;
    }
    if (format != FormatRgba8 && format != FormatBgra8) {
        LOG_WARNING(Core, "DSMod: texture format {:#x} is not one this decodes", format);
        return false;
    }
    out.w = width;
    out.h = height;
    out.pixels.assign(static_cast<size_t>(width) * height, 0);
    const u32 gobs_per_row = (width * 4 + 63) / 64;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u64 at_src = BlockLinearOffset(x, y, 4, block_height, gobs_per_row);
            if (at_src + 4 > pixels.size()) {
                continue;
            }
            u32 texel{};
            std::memcpy(&texel, pixels.data() + at_src, sizeof(texel));
            // The canvas stores ARGB. A little-endian R8G8B8A8 texel reads back as ABGR, so it
            // is the RGBA case that needs red and blue exchanged, not the BGRA one.
            if (format == FormatRgba8) {
                texel = (texel & 0xFF00FF00u) | ((texel & 0xFFu) << 16) | ((texel >> 16) & 0xFFu);
            }
            out.pixels[static_cast<size_t>(y) * width + x] = texel;
        }
    }
    return true;
}

bool ModRuntime::ParseMfnt(std::span<const u8> param, FontMetrics& out) {
    // Metroid Dread's own MFNT font-metrics container: a fixed header (line height at 0x1C, glyph
    // count at 0x20, first-glyph-record offset at 0x28) followed by fourteen-byte glyph records
    // running in codepoint order from the space (glyph 0 is notdef, glyph 1 is the space ->
    // first_codepoint 0x20). In-core fallback for when no module is loaded or the loaded module
    // declares no font extension -- see ModuleDecodeFont, which is tried first and wins when it
    // succeeds. Field-for-field the same algorithm the module's own DecodeFont (Dread's DSMod
    // module, eden-duo-companions/native/modules/010093801237C000.cpp) carries across the ABI boundary, so the two agree on
    // any well-formed asset.
    if (param.size() < 0x60 || std::memcmp(param.data(), "MFNT", 4) != 0) {
        return false;
    }
    const auto read32 = [&](size_t at) {
        u32 v{};
        std::memcpy(&v, param.data() + at, sizeof(v));
        return v;
    };
    const u32 line_height = read32(0x1C);
    const u32 count = read32(0x20);
    const u32 first = read32(0x28);
    // The count comes from the file, so bound it against the file rather than trusting it.
    if (line_height == 0 || count == 0 || first + static_cast<u64>(count) * 14 > param.size()) {
        return false;
    }
    out.first_codepoint = 0x20;
    out.glyphs.clear();
    out.glyphs.reserve(count - 1);
    for (u32 i = 1; i < count; ++i) {
        const size_t at = first + static_cast<size_t>(i) * 14;
        FontGlyph g{};
        std::memcpy(&g.x, param.data() + at + 0, 2);
        std::memcpy(&g.y, param.data() + at + 2, 2);
        std::memcpy(&g.w, param.data() + at + 4, 2);
        std::memcpy(&g.h, param.data() + at + 6, 2);
        std::memcpy(&g.bearing_x, param.data() + at + 8, 2);
        std::memcpy(&g.bearing_y, param.data() + at + 10, 2);
        std::memcpy(&g.advance, param.data() + at + 12, 2);
        out.glyphs.push_back(g);
    }
    out.line_height = line_height;
    return !out.glyphs.empty(); // matches FontMetrics::Valid()'s "no glyphs" rejection
}

std::optional<s64> ModRuntime::FindPlayerNode(const PlayerFind& spec) const {
    if (!spec.Valid() || main_region_begin == 0) {
        return std::nullopt;
    }
    if (player_found != 0) {
        // Cheap sanity check: a room change can destroy the node, and a stale address would
        // quietly report the player standing where the last room left them.
        const VAddr px = player_found + static_cast<VAddr>(spec.pos_offset);
        if (AddressIsSane(player_found, 8) &&
            system.ApplicationMemory().Read64(player_found) ==
                main_region_begin + static_cast<u64>(spec.vtable) +
                    static_cast<u64>(nce_vtable_delta) &&
            AddressIsSane(px, 4)) {
            // Shape alone is not enough to stay confident: this class has thousands of
            // instances, so a freed node reused by another actor still passes the vtable test
            // while reporting somebody else's position.
            if (player_sampled_tick != tick_count) {
                player_sampled_tick = tick_count;
                auto& mem = system.ApplicationMemory();
                f32 x{}, y{};
                const u32 rx = mem.Read32(px);
                const u32 ry = mem.Read32(px + 4);
                std::memcpy(&x, &rx, sizeof(x));
                std::memcpy(&y, &ry, sizeof(y));
                const bool pushed = PadIsPushed();
                const bool moved =
                    std::fabs(x - player_check_x) > 0.5f || std::fabs(y - player_check_y) > 0.5f;
                player_check_x = x;
                player_check_y = y;
                player_bad = (pushed && !moved) ? player_bad + 1 : 0;
                if (player_bad > 240) { // four seconds of being pushed and going nowhere
                    LOG_WARNING(Core,
                                "DSMod player: {:016X} stopped following the stick -- "
                                "looking again",
                                player_found);
                    player_found = 0;
                    player_at.clear();
                    player_last_x.clear();
                    player_last_y.clear();
                    player_score.clear();
                    player_collected = false;
                    player_scan_cursor = 0;
                    player_samples = 0;
                    player_moving = 0;
                    player_still = 0;
                    player_bad = 0;
                    return std::nullopt;
                }
            }
            return static_cast<s64>(player_found);
        }
        player_found = 0;
        player_at.clear();
        player_last_x.clear();
        player_last_y.clear();
        player_score.clear();
        player_hit_move.clear();
        player_hit_still.clear();
        player_collected = false;
        player_scan_cursor = 0;
        player_samples = 0;
        player_moving = 0;
        player_still = 0;
        player_bad = 0;
        LOG_INFO(Core, "DSMod player: node went away -- looking again");
    }
    if (!InGameplay()) {
        return std::nullopt;
    }
    auto& memory = system.ApplicationMemory();
    const u64 vtable =
        main_region_begin + static_cast<u64>(spec.vtable) + static_cast<u64>(nce_vtable_delta);

    // Phase 1: collect nodes of the class whose coordinates are world-scale. Sliced, and reading
    // host pages directly -- a full-heap pass through the page table blocks the render thread.
    if (!player_collected) {
        constexpr u64 PagesPerTick = 2048;
        const VAddr HeapBegin = HeapLow();
        const VAddr HeapEnd = HeapHigh();
        if (player_scan_cursor < HeapBegin || player_scan_cursor >= HeapEnd) {
            player_scan_cursor = HeapBegin;
        }
        const VAddr slice_end =
            std::min<VAddr>(HeapEnd, player_scan_cursor + PagesPerTick * 0x1000);
        for (VAddr page = player_scan_cursor; page < slice_end; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
                u64 word{};
                std::memcpy(&word, host + at, sizeof(word));
                if (word != vtable) {
                    continue;
                }
                const u32 pos = at + static_cast<u32>(spec.pos_offset);
                if (pos + 8 > 0x1000) {
                    continue; // coordinates spill into the next page; skip rather than fault
                }
                f32 x{}, y{};
                std::memcpy(&x, host + pos, sizeof(x));
                std::memcpy(&y, host + pos + 4, sizeof(y));
                if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) < spec.min_abs ||
                    std::fabs(x) > 100000.0f || std::fabs(y) < spec.min_abs ||
                    std::fabs(y) > 100000.0f) {
                    continue;
                }
                player_at.push_back(page + at);
                player_last_x.push_back(x);
                player_last_y.push_back(y);
                player_score.push_back(0);
                player_hit_move.push_back(0);
                player_hit_still.push_back(0);
            }
        }
        player_scan_cursor = slice_end;
        if (player_scan_cursor < HeapEnd) {
            return std::nullopt;
        }
        player_collected = true;
        LOG_INFO(Core, "DSMod player: {} node(s) of class main+{:X} hold world coordinates",
                 player_at.size(), spec.vtable);
        return std::nullopt;
    }
    if (player_at.empty()) {
        player_collected = false; // nothing yet; sweep again rather than give up for good
        return std::nullopt;
    }

    // Phase 2: one sample per frame, scored against the stick. Only the player is still when the
    // stick is still: enemies keep their own schedule, and lifts and platforms move regardless.
    if (player_sampled_tick == tick_count) {
        return std::nullopt;
    }
    player_sampled_tick = tick_count;
    // Watch the d-pad too, not just the analog stick. On a keyboard the stick reads zero all
    // session, so every frame counts as "standing still" -- which is how a window came back with
    // 45 moving samples against 1268 still ones, and a rock won for sitting there.
    const bool deflected = PadIsPushed();
    if (deflected) {
        ++player_moving;
    } else {
        ++player_still;
    }
    // Dread holds ~16.7k candidates here until one follows the pad, and this runs on the tick
    // (core-timing) thread every frame: 1 ms a tick on a desktop, 3-5 ms on a handheld, which
    // delays the vsync event sharing that thread. The candidates are in heap order, so
    // neighbours share guest pages: resolve each page's host pointer once (the phase-1 sweep's
    // own access path), then read the positions with the next ones prefetched. A position that
    // spills over a page edge, or a page without a host pointer, keeps the checked reads below.
    // Same values, same order, same bookkeeping as reading each one through Read32.
    const size_t candidates = player_at.size();
    player_host.resize(candidates);
    {
        VAddr cached_page = ~VAddr{0};
        const u8* cached_host = nullptr;
        for (size_t i = 0; i < candidates; ++i) {
            const VAddr px = player_at[i] + static_cast<VAddr>(spec.pos_offset);
            const u64 in_page = px & 0xFFF;
            if (in_page + 8 > 0x1000) {
                player_host[i] = nullptr;
                continue;
            }
            const VAddr page = px - in_page;
            if (page != cached_page) {
                cached_page = page;
                cached_host = memory.GetPointerSilent(page);
            }
            player_host[i] = cached_host != nullptr ? cached_host + in_page : nullptr;
        }
    }
    constexpr size_t PrefetchAhead = 16;
    for (size_t i = 0; i < candidates; ++i) {
#if defined(__GNUC__) || defined(__clang__)
        if (i + PrefetchAhead < candidates && player_host[i + PrefetchAhead] != nullptr) {
            __builtin_prefetch(player_host[i + PrefetchAhead]);
        }
#endif
        f32 x{}, y{};
        if (const u8* const host = player_host[i]; host != nullptr) {
            std::memcpy(&x, host, sizeof(x));
            std::memcpy(&y, host + 4, sizeof(y));
        } else {
            const VAddr px = player_at[i] + static_cast<VAddr>(spec.pos_offset);
            if (!AddressIsSane(px, 8)) {
                continue;
            }
            const u32 rx = memory.Read32(px);
            const u32 ry = memory.Read32(px + 4);
            std::memcpy(&x, &rx, sizeof(x));
            std::memcpy(&y, &ry, sizeof(y));
        }
        if (!std::isfinite(x) || !std::isfinite(y)) {
            continue;
        }
        // Half a unit, not a twentieth. A node can twitch slightly while its actor stands
        // idle, and counting that as movement fails the "held still" half of the test for the
        // very object we are looking for.
        const bool moved =
            std::fabs(x - player_last_x[i]) > 0.5f || std::fabs(y - player_last_y[i]) > 0.5f;
        player_last_x[i] = x;
        player_last_y[i] = y;
        if (deflected) {
            player_hit_move[i] += moved ? 1 : 0;
        } else {
            player_hit_still[i] += moved ? 0 : 1;
        }
    }
    // The test has no power without contrast. Ninety frames of continuous walking is ninety
    // frames of "the stick is pushed", and every moving object in the room agrees with that
    // perfectly -- which is exactly how a local child transform and then a drifting prop each
    // won with a flawless score. Insist on having seen the player both moving and standing.
    constexpr u64 NeedEach = 45;
    if (++player_samples < static_cast<u64>(spec.min_samples) || player_moving < NeedEach ||
        player_still < NeedEach) {
        return std::nullopt;
    }
    // Judge the two states separately and take the weaker of the two. Summing them lets a
    // lopsided window decide the answer: with movement rare, a static object banks a point on
    // every idle frame and wins without ever having followed the pad once.
    double best_rate = 0.0;
    size_t idx = player_at.size();
    for (size_t i = 0; i < player_at.size(); ++i) {
        const double moves =
            static_cast<double>(player_hit_move[i]) / static_cast<double>(player_moving);
        const double stays =
            static_cast<double>(player_hit_still[i]) / static_cast<double>(player_still);
        const double rate = std::min(moves, stays);
        if (rate > best_rate) {
            best_rate = rate;
            idx = i;
        }
    }
    if (idx >= player_at.size() || best_rate < 0.75) {
        // Collect again if nothing has emerged for a long while. The candidate list is built
        // once, and gameplay starts before the room is populated -- a sweep that runs during
        // the load sees a few hundred nodes instead of thousands, and the player is simply not
        // among them. No amount of further sampling can rescue that.
        if (player_samples > 900) {
            LOG_INFO(Core,
                     "DSMod player: nothing followed the pad among {} node(s) -- sweeping "
                     "again",
                     player_at.size());
            player_at.clear();
            player_last_x.clear();
            player_last_y.clear();
            player_score.clear();
            player_hit_move.clear();
            player_hit_still.clear();
            player_collected = false;
            player_scan_cursor = 0;
            player_samples = 0;
            player_moving = 0;
            player_still = 0;
        }
        return std::nullopt; // nothing follows the pad closely enough yet
    }
    player_found = player_at[idx];
    LOG_INFO(Core,
             "DSMod player: node {:016X} follows the pad -- moved {}/{} pushed frames, held "
             "still {}/{} released frames",
             player_found, player_hit_move[idx], player_moving, player_hit_still[idx],
             player_still);
    return static_cast<s64>(player_found);
}

} // namespace Core::Mods
