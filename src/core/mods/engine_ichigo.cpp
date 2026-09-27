// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Story of Seasons: Friends of Mineral Town, Marvelous' in-house Ichigo engine (the code calls
// it "SoS"). Static decoders with no runtime state, used only by the Story of Seasons package:
//   - DecodeLzss: the .lzs wrapper around the game's archives (u32 raw size + LZSS).
//   - ExtractArchiveMember: one named member of a decompressed .lzs archive. ReadAssetBytes
//     (mod_assets.cpp) uses both for "<archive.lzs>#<member>" sources that are not a SARC.
//   - DecodeSosXtx: a raw NVN XTX texture (BC1/BC3/BC7/RGBA8/BGRA8), from GetImage
//     (mod_map.cpp).
//   - ParseSosFont: the game's font-metrics table, LoadFont's last in-core fallback
//     (mod_assets.cpp).
// Why it lives here: the game has no native module to carry these decoders, so they stay in
// core, but outside the generic runtime files. BlockLinearOffset comes from engine_formats.h.
// Threads: the tick thread and the redraw worker (through GetImage); no shared state.

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "bc_decoder.h"
#include "core/mods/engine_formats.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {

// Story of Seasons .lzs: u32 raw size + classic Okumura LZSS (ring 4096 @ 0xFEE).
std::vector<u8> ModRuntime::DecodeLzss(std::span<const u8> in) {
    if (in.size() < 4)
        return {};
    u32 size{};
    std::memcpy(&size, in.data(), 4);
    std::vector<u8> out;
    out.reserve(size);
    std::array<u8, 4096> ring{};
    u32 r = 0xFEE;
    size_t i = 4;
    while (out.size() < size && i < in.size()) {
        u8 flags = in[i++];
        for (int b = 0; b < 8 && out.size() < size && i < in.size(); ++b) {
            if ((flags >> b) & 1) {
                const u8 ch = in[i++];
                out.push_back(ch);
                ring[r] = ch;
                r = (r + 1) & 0xFFF;
            } else {
                if (i + 1 >= in.size())
                    break;
                const u8 b1 = in[i], b2 = in[i + 1];
                i += 2;
                const u32 off = static_cast<u32>(b1) | ((static_cast<u32>(b2) & 0xF0) << 4);
                const u32 ln = (b2 & 0x0F) + 3;
                for (u32 k = 0; k < ln; ++k) {
                    const u8 ch = ring[(off + k) & 0xFFF];
                    out.push_back(ch);
                    ring[r] = ch;
                    r = (r + 1) & 0xFFF;
                }
            }
        }
    }
    return out;
}

// One member out of a decompressed .lzs archive: 64-byte entries {meta, off, size, idx, name[48]}.
std::vector<u8> ModRuntime::ExtractArchiveMember(std::span<const u8> raw, const std::string& want) {
    size_t at = 0;
    while (at + 64 <= raw.size()) {
        u32 off{}, size{};
        std::memcpy(&off, raw.data() + at + 4, 4);
        std::memcpy(&size, raw.data() + at + 8, 4);
        const u8* np = raw.data() + at + 16;
        size_t nl = 0;
        while (nl < 48 && np[nl] != 0xFF && np[nl] != 0x00)
            ++nl;
        if (nl == 0 || np[0] < 0x20 || np[0] >= 0x7F)
            break;
        const std::string name(reinterpret_cast<const char*>(np), nl);
        if (name == want) {
            if (static_cast<size_t>(off) + size <= raw.size())
                return std::vector<u8>(raw.begin() + off, raw.begin() + off + size);
            return {};
        }
        at += 64;
    }
    return {};
}

// A raw (already decompressed) XTX blob: DFvN/HBvN walk, block-linear, BC1/BC3/BC7/RGBA8/BGRA8.
bool ModRuntime::DecodeSosXtx(std::span<const u8> raw, Image& out) {
    static constexpr u32 FormatRgba8 = 0x25, FormatBgra8 = 0x6D;
    static constexpr u32 FormatBc1 = 0x42, FormatBc3 = 0x44, FormatBc7 = 0x4D;
    static constexpr u32 BlockTexture = 2, BlockData = 3;
    if (raw.size() < 0x40)
        return false;
    const auto read32 = [&raw](size_t at) {
        u32 v{};
        std::memcpy(&v, raw.data() + at, 4);
        return v;
    };
    const auto read64 = [&raw](size_t at) {
        u64 v{};
        std::memcpy(&v, raw.data() + at, 8);
        return v;
    };
    static constexpr std::array<u8, 4> XtxMagic{'D', 'F', 'v', 'N'};
    const auto xtx = std::search(raw.begin(), raw.end(), XtxMagic.begin(), XtxMagic.end());
    if (xtx == raw.end())
        return false;
    const size_t xtx_at = static_cast<size_t>(xtx - raw.begin());
    size_t at = xtx_at + read32(xtx_at + 4);
    u32 width{}, height{}, format{}, block_height = 16;
    std::span<const u8> pixels;
    while (at + 36 <= raw.size() && std::memcmp(raw.data() + at, "HBvN", 4) == 0) {
        const u64 data_size = read64(at + 8);
        const s64 data_offset = static_cast<s64>(read64(at + 16));
        const u32 type = read32(at + 24);
        const size_t data_at = static_cast<size_t>(static_cast<s64>(at) + data_offset);
        if (data_at + data_size > raw.size())
            break;
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
    if (width == 0 || height == 0 || pixels.empty())
        return false;
    if (format == FormatBc1 || format == FormatBc3 || format == FormatBc7) {
        const u32 block_bytes = format == FormatBc1 ? 8u : 16u;
        const u32 wide = (width + 3) / 4, tall = (height + 3) / 4;
        const u32 gobs = (wide * block_bytes + 63) / 64;
        std::vector<u8> rgba(static_cast<size_t>(width) * height * 4, 0);
        for (u32 by = 0; by < tall; ++by) {
            for (u32 bx = 0; bx < wide; ++bx) {
                const u64 at_src = BlockLinearOffset(bx, by, block_bytes, block_height, gobs);
                if (at_src + block_bytes > pixels.size())
                    continue;
                const u32 px = bx * 4, py = by * 4;
                u8* const dst = rgba.data() + (static_cast<size_t>(py) * width + px) * 4;
                if (format == FormatBc1)
                    bcn::DecodeBc1(pixels.data() + at_src, dst, px, py, width, height);
                else if (format == FormatBc3)
                    bcn::DecodeBc3(pixels.data() + at_src, dst, px, py, width, height);
                else
                    bcn::DecodeBc7(pixels.data() + at_src, dst, px, py, width, height);
            }
        }
        out.w = width;
        out.h = height;
        out.pixels.resize(static_cast<size_t>(width) * height);
        for (size_t k = 0; k < out.pixels.size(); ++k) {
            const u8 rr = rgba[k * 4], gg = rgba[k * 4 + 1], bb = rgba[k * 4 + 2],
                     aa = rgba[k * 4 + 3];
            out.pixels[k] = (u32(aa) << 24) | (u32(rr) << 16) | (u32(gg) << 8) | bb;
        }
        return true;
    }
    if (format != FormatRgba8 && format != FormatBgra8)
        return false;
    out.w = width;
    out.h = height;
    out.pixels.assign(static_cast<size_t>(width) * height, 0);
    const u32 gpr = (width * 4 + 63) / 64;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u64 at_src = BlockLinearOffset(x, y, 4, block_height, gpr);
            if (at_src + 4 > pixels.size())
                continue;
            u32 texel{};
            std::memcpy(&texel, pixels.data() + at_src, 4);
            if (format == FormatRgba8)
                texel = (texel & 0xFF00FF00u) | ((texel & 0xFFu) << 16) | ((texel >> 16) & 0xFFu);
            out.pixels[static_cast<size_t>(y) * width + x] = texel;
        }
    }
    return true;
}

bool ModRuntime::ParseSosFont(std::span<const u8> param, FontMetrics& out) {
    // Story of Seasons font: a u16[codepoint]->glyph-index CMAP (0xFFFF = none) covering 0..0xFFFF,
    // then 12-byte glyph records from 0x20000: u16 cp, u16 atlas_x, u16 atlas_y, u8 w, u8 h,
    // s8 bearing_x, u8 top_pad, u16 advance(12.4 fixed). `top_pad` is the gap from a shared ascent
    // line down to the glyph's top, so baseline-to-top (what the renderer wants) is ascent -
    // top_pad.
    constexpr size_t CmapCodes = 0x10000, MetricsStart = CmapCodes * 2, Rec = 12;
    if (param.size() < MetricsStart)
        return false;
    const auto rd16 = [&](size_t at) {
        u16 v{};
        std::memcpy(&v, param.data() + at, 2);
        return v;
    };
    const auto rec_off = [&](u32 cp) -> size_t {
        const u16 gi = rd16(static_cast<size_t>(cp) * 2);
        if (gi == 0xFFFF)
            return 0;
        const size_t rec = MetricsStart + static_cast<size_t>(gi) * Rec;
        return rec + Rec <= param.size() ? rec : 0;
    };
    // Uppercase and digits never fall below the baseline, so for them top_pad + height is exactly
    // the ascent; take the max to shrug off a pixel of rounding.
    u32 ascent = 0;
    for (u32 cp = 0x20; cp <= 0x7E; ++cp) {
        const bool no_descend = (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
        const size_t r = rec_off(cp);
        if (r && no_descend) {
            ascent =
                std::max(ascent, static_cast<u32>(param[r + 7]) + static_cast<u32>(param[r + 9]));
        }
    }
    if (ascent == 0)
        ascent = 30;
    out.first_codepoint = 0x20;
    out.glyphs.clear();
    for (u32 cp = 0x20; cp <= 0x7E; ++cp) {
        FontGlyph g{};
        const size_t r = rec_off(cp);
        if (r) {
            g.x = rd16(r + 2);
            g.y = rd16(r + 4);
            g.w = param[r + 6];
            g.h = param[r + 7];
            g.bearing_x = static_cast<s8>(param[r + 8]);
            g.bearing_y =
                static_cast<s16>(static_cast<s32>(ascent) - static_cast<s32>(param[r + 9]));
            g.advance = static_cast<u16>(rd16(r + 10) / 16);
            if (g.advance == 0)
                g.advance = static_cast<u16>(g.w + 2);
        }
        out.glyphs.push_back(g);
    }
    out.line_height = ascent;
    return !out.glyphs.empty();
}

} // namespace Core::Mods
