// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/mods/mod_nx_assets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <span>

#include "bc_decoder.h"
#include "video_core/textures/astc.h"

namespace Core::Mods::NxAssets {
namespace {

constexpr u64 MaxMetadataBytes = 64ull * 1024 * 1024;
constexpr u32 MaxDimension = 16384;

u16 Rd16(std::span<const u8> s, u64 at) {
    u16 v{};
    if (at + sizeof(v) <= s.size()) {
        std::memcpy(&v, s.data() + at, sizeof(v));
    }
    return v;
}
u32 Rd32(std::span<const u8> s, u64 at) {
    u32 v{};
    if (at + sizeof(v) <= s.size()) {
        std::memcpy(&v, s.data() + at, sizeof(v));
    }
    return v;
}
u64 Rd64(std::span<const u8> s, u64 at) {
    u64 v{};
    if (at + sizeof(v) <= s.size()) {
        std::memcpy(&v, s.data() + at, sizeof(v));
    }
    return v;
}
u16 Rd16E(std::span<const u8> s, u64 at, bool big) {
    const u16 v = Rd16(s, at);
    return big ? static_cast<u16>((v >> 8) | (v << 8)) : v;
}
u32 Rd32E(std::span<const u8> s, u64 at, bool big) {
    const u32 v = Rd32(s, at);
    return big ? ((v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24)) : v;
}

u32 SarcHash(std::string_view name, u32 key) {
    u32 h = 0;
    for (const char c : name) {
        h = h * key + static_cast<u32>(static_cast<s32>(static_cast<signed char>(c)));
    }
    return h;
}

/// Where texel (x, y) of a block-linear (Tegra) image sits. Switch textures are tiled this way
/// rather than row by row; the same math as the runtime's BlockLinearOffset.
u64 BlockLinearOffset(u32 x, u32 y, u32 bpp, u32 block_height, u64 gobs_per_row) {
    const u64 gob = (static_cast<u64>(y / (8 * block_height)) * 512 * block_height * gobs_per_row) +
                    (static_cast<u64>(x) * bpp / 64 * 512 * block_height) +
                    (static_cast<u64>(y % (8 * block_height) / 8) * 512);
    const u64 xb = static_cast<u64>(x) * bpp;
    return gob + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 +
           (y % 2) * 16 + (xb % 16);
}

enum class Kind : u8 { Unsupported, Raw, Bc, Astc };

struct FormatInfo {
    Kind kind{Kind::Unsupported};
    u32 block_w{1};
    u32 block_h{1};
    u32 bytes{0}; ///< per texel (raw) or per block (compressed)
    const char* name{"?"};
};

FormatInfo Describe(u32 format) {
    const u32 type = format >> 8;
    switch (type) {
    case 0x02:
        return {Kind::Raw, 1, 1, 1, "R8"};
    case 0x07:
        return {Kind::Raw, 1, 1, 2, "R5G6B5"};
    case 0x09:
        return {Kind::Raw, 1, 1, 2, "R8G8"};
    case 0x0B:
        return {Kind::Raw, 1, 1, 4, "R8G8B8A8"};
    case 0x0C:
        return {Kind::Raw, 1, 1, 4, "B8G8R8A8"};
    case 0x0F:
        return {Kind::Raw, 1, 1, 2, "B5G6R5"};
    case 0x1A:
        return {Kind::Bc, 4, 4, 8, "BC1"};
    case 0x1B:
        return {Kind::Bc, 4, 4, 16, "BC2"};
    case 0x1C:
        return {Kind::Bc, 4, 4, 16, "BC3"};
    case 0x1D:
        return {Kind::Bc, 4, 4, 8, "BC4"};
    case 0x1E:
        return {Kind::Bc, 4, 4, 16, "BC5"};
    case 0x20:
        return {Kind::Bc, 4, 4, 16, "BC7"};
    default:
        break;
    }
    struct AstcBlock {
        u32 type, w, h;
        const char* name;
    };
    static constexpr std::array<AstcBlock, 14> astc{{
        {0x2D, 4, 4, "ASTC4x4"},
        {0x2E, 5, 4, "ASTC5x4"},
        {0x2F, 5, 5, "ASTC5x5"},
        {0x30, 6, 5, "ASTC6x5"},
        {0x31, 6, 6, "ASTC6x6"},
        {0x32, 8, 5, "ASTC8x5"},
        {0x33, 8, 6, "ASTC8x6"},
        {0x34, 8, 8, "ASTC8x8"},
        {0x35, 10, 5, "ASTC10x5"},
        {0x36, 10, 6, "ASTC10x6"},
        {0x37, 10, 8, "ASTC10x8"},
        {0x38, 10, 10, "ASTC10x10"},
        {0x39, 12, 10, "ASTC12x10"},
        {0x3A, 12, 12, "ASTC12x12"},
    }};
    for (const auto& a : astc) {
        if (a.type == type) {
            return {Kind::Astc, a.w, a.h, 16, a.name};
        }
    }
    return {};
}

bool IsSigned(u32 format) {
    return (format & 0xFF) == 2;
}

} // namespace

ReadAt SpanReader(std::span<const u8> bytes) {
    return [bytes](u64 offset, std::span<u8> out) {
        if (offset > bytes.size() || out.size() > bytes.size() - offset) {
            return false;
        }
        std::memcpy(out.data(), bytes.data() + offset, out.size());
        return true;
    };
}

bool HasMagic(std::span<const u8> head, std::string_view magic) {
    return head.size() >= magic.size() && std::memcmp(head.data(), magic.data(), magic.size()) == 0;
}

// ---------------------------------------------------------------------------------------------
// SARC

const SarcMember* SarcIndex::Find(std::string_view name) const {
    if (const auto it = by_name.find(std::string{name}); it != by_name.end()) {
        return &it->second;
    }
    // A member stored without a name is still addressable: SFAT is keyed by the name's hash.
    if (const auto it = by_hash.find(SarcHash(name, hash_key)); it != by_hash.end()) {
        return &it->second;
    }
    return nullptr;
}

bool ParseSarc(const ReadAt& read, u64 size, SarcIndex& out, std::string& error) {
    out = {};
    std::array<u8, 0x14> header{};
    if (size < header.size() || !read(0, header)) {
        error = "short SARC header";
        return false;
    }
    if (!HasMagic(header, "SARC")) {
        error = "not a SARC";
        return false;
    }
    const bool big = header[6] == 0xFE && header[7] == 0xFF;
    const u16 header_size = Rd16E(header, 4, big);
    const u32 data_offset = Rd32E(header, 0xC, big);
    if (header_size < 0x14 || data_offset <= header_size || data_offset > size ||
        data_offset > MaxMetadataBytes) {
        error = "bad SARC header";
        return false;
    }
    std::vector<u8> meta(data_offset - header_size);
    if (!read(header_size, meta)) {
        error = "short SARC file table";
        return false;
    }
    if (!HasMagic(meta, "SFAT")) {
        error = "no SFAT";
        return false;
    }
    const u16 sfat_size = Rd16E(meta, 4, big);
    const u16 count = Rd16E(meta, 6, big);
    out.hash_key = Rd32E(meta, 8, big);
    const u64 nodes = sfat_size;
    const u64 sfnt = nodes + u64{count} * 16;
    if (sfnt + 8 > meta.size() || !HasMagic(std::span(meta).subspan(sfnt), "SFNT")) {
        error = "no SFNT";
        return false;
    }
    const u64 names = sfnt + Rd16E(meta, sfnt + 4, big);
    for (u32 i = 0; i < count; ++i) {
        const u64 node = nodes + u64{i} * 16;
        const u32 hash = Rd32E(meta, node, big);
        const u32 attr = Rd32E(meta, node + 4, big);
        const u32 start = Rd32E(meta, node + 8, big);
        const u32 end = Rd32E(meta, node + 12, big);
        if (end < start || u64{data_offset} + end > size) {
            continue;
        }
        const SarcMember member{u64{data_offset} + start, u64{end} - start};
        out.by_hash.emplace(hash, member);
        if ((attr & 0x01000000u) == 0) {
            continue;
        }
        const u64 at = names + u64{attr & 0x00FFFFFFu} * 4;
        if (at >= meta.size()) {
            continue;
        }
        const auto* const first = reinterpret_cast<const char*>(meta.data() + at);
        const size_t length = strnlen(first, meta.size() - at);
        out.by_name.emplace(std::string(first, length), member);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// BNTX

const BntxTexture* BntxIndex::Find(std::string_view name) const {
    if (textures.empty()) {
        return nullptr;
    }
    if (name.empty()) {
        return &textures.front();
    }
    const auto it = by_name.find(std::string{name});
    return it == by_name.end() ? nullptr : &textures[it->second];
}

bool ParseBntx(const ReadAt& read, u64 size, BntxIndex& out, std::string& error) {
    out = {};
    std::array<u8, 0x40> header{};
    if (size < header.size() || !read(0, header)) {
        error = "short BNTX header";
        return false;
    }
    if (!HasMagic(header, "BNTX") || std::memcmp(header.data() + 0x20, "NX  ", 4) != 0) {
        error = "not a Switch BNTX";
        return false;
    }
    const u32 count = Rd32(header, 0x24);
    const u64 table = Rd64(header, 0x28);
    u64 meta_end = Rd64(header, 0x30); // BRTD: every descriptor, name and table precedes it
    if (meta_end <= table || meta_end > size) {
        meta_end = size;
    }
    if (count == 0 || count > 65536 || meta_end > MaxMetadataBytes) {
        error = "implausible BNTX texture table";
        return false;
    }
    std::vector<u8> meta(meta_end);
    if (!read(0, meta)) {
        error = "short BNTX metadata";
        return false;
    }
    const std::span<const u8> m{meta};
    out.textures.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const u64 info = Rd64(m, table + u64{i} * 8);
        if (info + 0x78 > m.size() || std::memcmp(m.data() + info, "BRTI", 4) != 0) {
            continue;
        }
        BntxTexture t;
        t.mip_count = Rd16(m, info + 0x16);
        t.format = Rd32(m, info + 0x1C);
        t.width = Rd32(m, info + 0x24);
        t.height = Rd32(m, info + 0x28);
        t.depth = Rd32(m, info + 0x2C);
        t.array_count = Rd32(m, info + 0x30);
        t.block_height_log2 = Rd32(m, info + 0x34) & 7;
        t.image_size = Rd32(m, info + 0x50);
        t.alignment = Rd32(m, info + 0x54);
        std::memcpy(t.channels.data(), m.data() + info + 0x58, 4);
        const u64 name_at = Rd64(m, info + 0x60);
        const u64 mips_at = Rd64(m, info + 0x70);
        if (name_at + 2 <= m.size()) {
            const u16 length = Rd16(m, name_at);
            if (name_at + 2 + length <= m.size()) {
                t.name.assign(reinterpret_cast<const char*>(m.data() + name_at + 2), length);
            }
        }
        if (t.width == 0 || t.height == 0 || t.width > MaxDimension || t.height > MaxDimension ||
            t.mip_count == 0 || t.mip_count > 32 || mips_at + u64{t.mip_count} * 8 > m.size()) {
            continue;
        }
        for (u32 mip = 0; mip < t.mip_count; ++mip) {
            t.mip_offsets.push_back(Rd64(m, mips_at + u64{mip} * 8));
        }
        out.by_name.emplace(t.name, static_cast<u32>(out.textures.size()));
        out.textures.push_back(std::move(t));
    }
    if (out.textures.empty()) {
        error = "BNTX holds no readable texture";
        return false;
    }
    return true;
}

std::string FormatName(u32 format) {
    const FormatInfo info = Describe(format);
    const char* dtype = "?";
    switch (format & 0xFF) {
    case 1:
        dtype = "UNORM";
        break;
    case 2:
        dtype = "SNORM";
        break;
    case 3:
        dtype = "UINT";
        break;
    case 4:
        dtype = "SINT";
        break;
    case 5:
        dtype = "FLOAT";
        break;
    case 6:
        dtype = "SRGB";
        break;
    default:
        break;
    }
    if (info.kind == Kind::Unsupported) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "T%02X_%s", format >> 8, dtype);
        return buffer;
    }
    return std::string(info.name) + "_" + dtype;
}

bool FormatSupported(u32 format) {
    return Describe(format).kind != Kind::Unsupported;
}

namespace {

/// Decodes into `allocate(pixel count)`, which returns w*h*4 writable bytes (zeroed).
bool DecodeTextureInto(const ReadAt& read, u64 bntx_size, const BntxTexture& t, u32 layer,
                       const std::function<std::span<u8>(u64)>& allocate, std::string& error) {
    const FormatInfo f = Describe(t.format);
    if (f.kind == Kind::Unsupported) {
        error = "unsupported format " + FormatName(t.format);
        return false;
    }
    if (t.mip_offsets.empty()) {
        error = "texture has no data";
        return false;
    }
    const u32 layers = std::max<u32>(1, t.array_count);
    if (layer >= layers) {
        error = "array layer out of range";
        return false;
    }
    const u32 w = t.width;
    const u32 h = t.height;
    const u32 grid_w = (w + f.block_w - 1) / f.block_w;
    const u32 grid_h = (h + f.block_h - 1) / f.block_h;
    const u32 bpp = f.bytes;
    const u32 block_height = 1u << t.block_height_log2;
    const u64 gobs_per_row = (u64{grid_w} * bpp + 63) / 64;
    const u64 group_rows = (grid_h + 8u * block_height - 1) / (8u * block_height);
    const u64 layer_bytes = group_rows * 512 * block_height * gobs_per_row;
    // Array layers (font sheets) follow each other, each padded to the texture's alignment.
    u64 slice = 0;
    if (layers > 1) {
        const u64 align = std::max<u32>(1, t.alignment);
        slice = (u64{t.image_size} / layers + align - 1) / align * align;
    }
    const u64 offset = t.mip_offsets[0] + slice * layer;
    if (offset >= bntx_size) {
        error = "texture data outside the file";
        return false;
    }
    const u64 available = std::min(layer_bytes, bntx_size - offset);
    std::vector<u8> tiled(available);
    if (!read(offset, tiled)) {
        error = "short texture data";
        return false;
    }

    // Block-linear -> row order, on the texel/block grid.
    std::vector<u8> linear(u64{grid_w} * grid_h * bpp);
    for (u32 y = 0; y < grid_h; ++y) {
        u8* const row = linear.data() + u64{y} * grid_w * bpp;
        for (u32 x = 0; x < grid_w; ++x) {
            const u64 at = BlockLinearOffset(x, y, bpp, block_height, gobs_per_row);
            if (at + bpp <= tiled.size()) {
                std::memcpy(row + u64{x} * bpp, tiled.data() + at, bpp);
            }
        }
    }
    tiled = {};

    const u64 pixels = u64{w} * h;
    const std::span<u8> rgba = allocate(pixels);
    if (rgba.size() != pixels * 4) {
        error = "out of memory";
        return false;
    }
    switch (f.kind) {
    case Kind::Astc:
        Tegra::Texture::ASTC::Decompress(linear, w, h, 1, f.block_w, f.block_h, rgba);
        break;
    case Kind::Bc: {
        const u32 type = t.format >> 8;
        const bool is_signed = IsSigned(t.format);
        // BC4 decodes to R8 and BC5 to R8G8; widen those to RGBA afterwards.
        const u32 out_bpp = type == 0x1D ? 1 : type == 0x1E ? 2 : 4;
        std::vector<u8> narrow;
        std::span<u8> target = rgba;
        if (out_bpp != 4) {
            narrow.assign(pixels * out_bpp, 0);
            target = narrow;
        }
        for (u32 by = 0; by < grid_h; ++by) {
            for (u32 bx = 0; bx < grid_w; ++bx) {
                const u8* const src = linear.data() + (u64{by} * grid_w + bx) * bpp;
                const u32 px = bx * 4;
                const u32 py = by * 4;
                u8* const dst = target.data() + (u64{py} * w + px) * out_bpp;
                switch (type) {
                case 0x1A:
                    bcn::DecodeBc1(src, dst, px, py, w, h);
                    break;
                case 0x1B:
                    bcn::DecodeBc2(src, dst, px, py, w, h);
                    break;
                case 0x1C:
                    bcn::DecodeBc3(src, dst, px, py, w, h);
                    break;
                case 0x1D:
                    bcn::DecodeBc4(src, dst, px, py, w, h, is_signed);
                    break;
                case 0x1E:
                    bcn::DecodeBc5(src, dst, px, py, w, h, is_signed);
                    break;
                case 0x20:
                    bcn::DecodeBc7(src, dst, px, py, w, h);
                    break;
                default:
                    break;
                }
            }
        }
        if (out_bpp != 4) {
            // What a GPU samples from a one/two-channel texture: (R, 0, 0, 1) / (R, G, 0, 1).
            for (u64 i = 0; i < pixels; ++i) {
                rgba[i * 4] = narrow[i * out_bpp];
                rgba[i * 4 + 1] = out_bpp == 2 ? narrow[i * 2 + 1] : 0;
                rgba[i * 4 + 2] = 0;
                rgba[i * 4 + 3] = 255;
            }
        }
        break;
    }
    case Kind::Raw: {
        const u32 type = t.format >> 8;
        for (u64 i = 0; i < pixels; ++i) {
            const u8* const s = linear.data() + i * bpp;
            u8* const d = rgba.data() + i * 4;
            switch (type) {
            case 0x02: // R8
                d[0] = s[0];
                d[3] = 255;
                break;
            case 0x09: // R8G8
                d[0] = s[0];
                d[1] = s[1];
                d[3] = 255;
                break;
            case 0x0B: // R8G8B8A8
                std::memcpy(d, s, 4);
                break;
            case 0x0C: // B8G8R8A8
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = s[3];
                break;
            case 0x07:   // R5G6B5: red in the LOW bits (checked against LA's BC1 twin texture)
            case 0x0F: { // B5G6R5: red in the high bits
                const u32 v = u32{s[0]} | (u32{s[1]} << 8);
                const u32 lo = v & 31;
                const u32 mid = (v >> 5) & 63;
                const u32 hi = (v >> 11) & 31;
                const u32 r = type == 0x07 ? lo : hi;
                const u32 b = type == 0x07 ? hi : lo;
                d[0] = static_cast<u8>(r * 255 / 31);
                d[1] = static_cast<u8>(mid * 255 / 63);
                d[2] = static_cast<u8>(b * 255 / 31);
                d[3] = 255;
                break;
            }
            default:
                break;
            }
        }
        break;
    }
    case Kind::Unsupported:
        break;
    }

    // The per-texture channel map: grey+alpha masks (RRRG), alpha-only masks (111R), ...
    static constexpr std::array<u8, 4> Identity{2, 3, 4, 5};
    if (t.channels != Identity) {
        for (u64 i = 0; i < pixels; ++i) {
            u8* const p = rgba.data() + i * 4;
            const std::array<u8, 4> src{p[0], p[1], p[2], p[3]};
            for (u32 c = 0; c < 4; ++c) {
                const u8 sel = t.channels[c];
                p[c] = sel == 0 ? u8{0} : sel == 1 ? u8{255} : sel <= 5 ? src[sel - 2] : src[c];
            }
        }
    }
    return true;
}

} // namespace

bool DecodeTexture(const ReadAt& read, u64 bntx_size, const BntxTexture& texture, u32 layer,
                   std::vector<u8>& rgba, std::string& error) {
    return DecodeTextureInto(
        read, bntx_size, texture, layer,
        [&rgba](u64 pixels) {
            rgba.assign(pixels * 4, 0);
            return std::span<u8>{rgba};
        },
        error);
}

bool DecodeTextureImage(const ReadAt& read, u64 bntx_size, const BntxTexture& texture, u32 layer,
                        Image& out, std::string& error) {
    // Decode straight into the image's own storage (no second full-size buffer), then turn the
    // RGBA bytes into ARGB words in place.
    const bool ok = DecodeTextureInto(
        read, bntx_size, texture, layer,
        [&out](u64 pixels) {
            out.pixels.assign(pixels, 0);
            return std::span<u8>{reinterpret_cast<u8*>(out.pixels.data()), pixels * 4};
        },
        error);
    if (!ok) {
        out = {};
        return false;
    }
    out.w = texture.width;
    out.h = texture.height;
    for (u32& p : out.pixels) {
        const u8* const b = reinterpret_cast<const u8*>(&p);
        p = (u32{b[3]} << 24) | (u32{b[0]} << 16) | (u32{b[1]} << 8) | b[2];
    }
    return true;
}

void RgbaToImage(std::span<const u8> rgba, u32 width, u32 height, Image& out, bool flip_y) {
    out.w = width;
    out.h = height;
    out.pixels.resize(u64{width} * height);
    for (u32 y = 0; y < height; ++y) {
        const u32 sy = flip_y ? height - 1 - y : y;
        const u8* src = rgba.data() + u64{sy} * width * 4;
        u32* dst = out.pixels.data() + u64{y} * width;
        for (u32 x = 0; x < width; ++x, src += 4) {
            dst[x] = (u32{src[3]} << 24) | (u32{src[0]} << 16) | (u32{src[1]} << 8) | src[2];
        }
    }
}

std::vector<u8> ImageToRgba(const Image& image) {
    std::vector<u8> out(image.pixels.size() * 4);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const u32 p = image.pixels[i];
        out[i * 4] = static_cast<u8>(p >> 16);
        out[i * 4 + 1] = static_cast<u8>(p >> 8);
        out[i * 4 + 2] = static_cast<u8>(p);
        out[i * 4 + 3] = static_cast<u8>(p >> 24);
    }
    return out;
}

bool DecodeBntxImage(std::span<const u8> file, std::string_view name, Image& out,
                     std::string& error) {
    const ReadAt read = SpanReader(file);
    BntxIndex index;
    if (!ParseBntx(read, file.size(), index, error)) {
        return false;
    }
    const BntxTexture* const texture = index.Find(name);
    if (texture == nullptr) {
        error = "no texture named '" + std::string{name} + "'";
        return false;
    }
    return DecodeTextureImage(read, file.size(), *texture, 0, out, error);
}

// ---------------------------------------------------------------------------------------------
// BFFNT

bool ParseBffnt(std::span<const u8> d, BffntInfo& out, std::string& error) {
    out = {};
    if (d.size() < 0x14 || !HasMagic(d, "FFNT")) {
        error = "not a BFFNT";
        return false;
    }
    const u64 finf = Rd16(d, 6);
    if (finf + 0x20 > d.size() || !HasMagic(d.subspan(finf), "FINF")) {
        error = "no FINF";
        return false;
    }
    out.font_type = d[finf + 8];
    out.height = d[finf + 9];
    out.width = d[finf + 10];
    out.ascent = d[finf + 11];
    out.line_feed = Rd16(d, finf + 12);
    out.alter_char_index = Rd16(d, finf + 14);
    out.default_width = {static_cast<s8>(d[finf + 16]), d[finf + 17], d[finf + 18]};
    const u64 tglp_ptr = Rd32(d, finf + 20);
    const u64 cwdh_ptr = Rd32(d, finf + 24);
    const u64 cmap_ptr = Rd32(d, finf + 28);
    // The block pointers point 8 bytes past each block's magic.
    if (tglp_ptr < 8 || tglp_ptr - 8 + 0x20 > d.size() ||
        !HasMagic(d.subspan(tglp_ptr - 8), "TGLP")) {
        error = "no TGLP";
        return false;
    }
    const u64 t = tglp_ptr - 8;
    out.cell_w = d[t + 8];
    out.cell_h = d[t + 9];
    out.sheet_count = d[t + 10];
    out.baseline = Rd16(d, t + 16);
    out.sheet_format = Rd16(d, t + 18);
    out.cols = Rd16(d, t + 20);
    out.rows = Rd16(d, t + 22);
    out.sheet_w = Rd16(d, t + 24);
    out.sheet_h = Rd16(d, t + 26);
    out.sheet_data_offset = Rd32(d, t + 28);
    if (out.cols == 0 || out.rows == 0 || out.sheet_data_offset >= d.size()) {
        error = "bad TGLP";
        return false;
    }
    u64 block = cwdh_ptr;
    for (u32 guard = 0; block >= 8 && guard < 4096; ++guard) {
        const u64 at = block - 8;
        if (at + 16 > d.size() || !HasMagic(d.subspan(at), "CWDH")) {
            break;
        }
        const u16 first = Rd16(d, at + 8);
        const u16 last = Rd16(d, at + 10);
        for (u32 g = first, q = 0; g <= last; ++g, ++q) {
            const u64 e = at + 16 + u64{q} * 3;
            if (e + 3 > d.size()) {
                break;
            }
            out.widths[static_cast<u16>(g)] = {static_cast<s8>(d[e]), d[e + 1], d[e + 2]};
        }
        const u64 next = Rd32(d, at + 12);
        block = next > block ? next : 0;
    }
    block = cmap_ptr;
    for (u32 guard = 0; block >= 8 && guard < 4096; ++guard) {
        const u64 at = block - 8;
        if (at + 24 > d.size() || !HasMagic(d.subspan(at), "CMAP")) {
            break;
        }
        const u32 begin = Rd32(d, at + 8);
        const u32 end = Rd32(d, at + 12);
        const u16 method = Rd16(d, at + 16);
        u64 q = at + 24;
        if (end >= begin && end - begin < 0x110000) {
            if (method == 0) {
                const u16 base = Rd16(d, q);
                for (u32 c = begin; c <= end; ++c) {
                    out.cmap[c] = static_cast<u16>(base + (c - begin));
                }
            } else if (method == 1) {
                for (u32 c = begin; c <= end && q + 2 <= d.size(); ++c, q += 2) {
                    if (const u16 index = Rd16(d, q); index != 0xFFFF) {
                        out.cmap[c] = index;
                    }
                }
            } else if (method == 2) {
                const u16 n = Rd16(d, q);
                q += 4;
                for (u32 k = 0; k < n && q + 8 <= d.size(); ++k, q += 8) {
                    if (const u16 index = Rd16(d, q + 4); index != 0xFFFF) {
                        out.cmap[Rd32(d, q)] = index;
                    }
                }
            }
        }
        const u64 next = Rd32(d, at + 20);
        block = next > block ? next : 0;
    }
    if (out.cmap.empty()) {
        error = "font has no character map";
        return false;
    }
    return true;
}

bool BuildBffntFont(std::span<const u8> file, FontMetrics& metrics, Image& atlas,
                    std::string& error, u32 first, u32 last) {
    BffntInfo info;
    if (!ParseBffnt(file, info, error)) {
        return false;
    }
    const auto sheets_file = file.subspan(info.sheet_data_offset);
    const ReadAt read = SpanReader(sheets_file);
    BntxIndex index;
    if (!ParseBntx(read, sheets_file.size(), index, error)) {
        error = "glyph sheets: " + error;
        return false;
    }
    const BntxTexture& sheet = index.textures.front();
    u32 sheet_count = std::max<u32>(1, sheet.array_count);
    if (info.sheet_count != 0) {
        sheet_count = std::min<u32>(sheet_count, info.sheet_count);
    }
    const u32 sw = sheet.width;
    const u32 sh = sheet.height;
    atlas.w = sw;
    atlas.h = sh * sheet_count;
    atlas.pixels.assign(u64{atlas.w} * atlas.h, 0);
    std::vector<u8> rgba;
    Image upright;
    for (u32 s = 0; s < sheet_count; ++s) {
        if (!DecodeTexture(read, sheets_file.size(), sheet, s, rgba, error)) {
            error = "glyph sheet " + std::to_string(s) + ": " + error;
            return false;
        }
        // Font sheets are stored bottom-up.
        RgbaToImage(rgba, sw, sh, upright, true);
        std::copy(upright.pixels.begin(), upright.pixels.end(),
                  atlas.pixels.begin() + static_cast<std::ptrdiff_t>(u64{s} * sw * sh));
    }

    const u32 per_sheet = u32{info.cols} * info.rows;
    const auto glyph_for = [&](u16 index) {
        FontGlyph g{};
        const u32 s = index / per_sheet;
        if (s >= sheet_count) {
            return g;
        }
        const u32 cell = index % per_sheet;
        const u32 row = cell / info.cols;
        const u32 col = cell % info.cols;
        const auto w = info.widths.find(index);
        const BffntGlyphWidth width = w == info.widths.end() ? info.default_width : w->second;
        g.x = static_cast<u16>(col * (u32{info.cell_w} + 1) + 1);
        g.y = static_cast<u16>(row * (u32{info.cell_h} + 1) + 1 + s * sh);
        g.w = width.glyph_width;
        g.h = info.cell_h;
        g.bearing_x = width.left;
        g.bearing_y = static_cast<s16>(info.baseline); // cell top sits `baseline` above it
        g.advance = width.char_width;
        return g;
    };
    // FINF's alternate character is a glyph index: what the game draws for a missing character.
    const u32 glyph_total = per_sheet * sheet_count;
    const bool has_alter = info.alter_char_index < glyph_total;
    metrics.first_codepoint = first;
    metrics.glyphs.clear();
    metrics.glyphs.reserve(last - first + 1);
    for (u32 c = first; c <= last; ++c) {
        const auto it = info.cmap.find(c);
        if (it != info.cmap.end()) {
            metrics.glyphs.push_back(glyph_for(it->second));
        } else if (has_alter && c >= 0x21) {
            metrics.glyphs.push_back(glyph_for(info.alter_char_index));
        } else {
            FontGlyph blank{};
            blank.advance = info.default_width.char_width;
            metrics.glyphs.push_back(blank);
        }
    }
    // Every other character the font has (Latin Extended, Cyrillic, arrows, button pictograms
    // ...), looked up sparsely: the game's own text needs more than Latin-1.
    metrics.extra.clear();
    for (const auto& [code, index] : info.cmap) {
        if ((code < first || code > last) && index / per_sheet < sheet_count) {
            metrics.extra.emplace(static_cast<char32_t>(code), glyph_for(index));
        }
    }
    metrics.ascent = info.baseline;
    // The canvas scales text so that `line_height` design pixels span the requested size above
    // the baseline, the way its built-in capitals do -- so use the font's capital height.
    u32 cap = 0;
    for (const char32_t probe : {U'H', U'E', U'A'}) {
        const auto it = info.cmap.find(probe);
        if (it == info.cmap.end()) {
            continue;
        }
        const FontGlyph g = glyph_for(it->second);
        for (u32 row = 0; row < g.h && cap == 0; ++row) {
            for (u32 col = 0; col < g.w; ++col) {
                const u64 at = u64{g.y + row} * atlas.w + g.x + col;
                if (at < atlas.pixels.size() && (atlas.pixels[at] >> 24) >= 128) {
                    cap = row < info.baseline ? info.baseline - row : 0;
                    break;
                }
            }
        }
        if (cap != 0) {
            break;
        }
    }
    metrics.line_height = cap != 0 ? cap : std::max<u32>(1, info.ascent);
    if (!metrics.Valid()) {
        error = "font produced no glyphs";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Resampling

namespace {

struct Tap {
    u32 index;
    float weight;
};

/// For every destination cell [d, d+1) in [first, first+count): the source cells its footprint
/// covers and their share, plus how much of the cell the destination interval covers at all.
void BuildTaps(double s0, double s1, double d0, double d1, s32 first, u32 count, u32 src_limit,
               std::vector<std::vector<Tap>>& taps, std::vector<float>& coverage) {
    taps.assign(count, {});
    coverage.assign(count, 0.0f);
    const double scale = (s1 - s0) / (d1 - d0);
    for (u32 i = 0; i < count; ++i) {
        const double a = std::max(d0, static_cast<double>(first + static_cast<s32>(i)));
        const double b = std::min(d1, static_cast<double>(first + static_cast<s32>(i)) + 1.0);
        if (b <= a) {
            continue;
        }
        coverage[i] = static_cast<float>(b - a);
        double u0 = s0 + (a - d0) * scale;
        double u1 = s0 + (b - d0) * scale;
        u0 = std::clamp(u0, 0.0, static_cast<double>(src_limit));
        u1 = std::clamp(u1, 0.0, static_cast<double>(src_limit));
        if (u1 - u0 < 1e-9) {
            // Degenerate footprint: nearest source cell.
            const u32 k = std::min(src_limit - 1, static_cast<u32>(u0));
            taps[i].push_back({k, 1.0f});
            continue;
        }
        const u32 k0 = static_cast<u32>(std::floor(u0));
        const u32 k1 = std::min(src_limit, static_cast<u32>(std::ceil(u1)));
        const double span = u1 - u0;
        for (u32 k = k0; k < k1; ++k) {
            const double overlap = std::min(u1, k + 1.0) - std::max(u0, static_cast<double>(k));
            if (overlap > 0.0) {
                taps[i].push_back({k, static_cast<float>(overlap / span)});
            }
        }
    }
}

} // namespace

PlacedRaster ResampleArea(const Image& source, double sx0, double sy0, double sx1, double sy1,
                          double dx0, double dy0, double dx1, double dy1, u32 canvas_w,
                          u32 canvas_h, bool edge_coverage) {
    PlacedRaster out;
    if (!source.Valid() || dx1 <= dx0 || dy1 <= dy0 || sx1 <= sx0 || sy1 <= sy0) {
        return out;
    }
    const s32 x0 = std::max(0, static_cast<s32>(std::floor(dx0)));
    const s32 y0 = std::max(0, static_cast<s32>(std::floor(dy0)));
    const s32 x1 = std::min(static_cast<s32>(canvas_w), static_cast<s32>(std::ceil(dx1)));
    const s32 y1 = std::min(static_cast<s32>(canvas_h), static_cast<s32>(std::ceil(dy1)));
    if (x1 <= x0 || y1 <= y0) {
        return out;
    }
    out.x = x0;
    out.y = y0;
    out.w = static_cast<u32>(x1 - x0);
    out.h = static_cast<u32>(y1 - y0);

    std::vector<std::vector<Tap>> col_taps, row_taps;
    std::vector<float> col_cov, row_cov;
    BuildTaps(sx0, sx1, dx0, dx1, x0, out.w, source.w, col_taps, col_cov);
    BuildTaps(sy0, sy1, dy0, dy1, y0, out.h, source.h, row_taps, row_cov);

    // Separable box filter. The horizontal pass runs per source row on demand and only the rows
    // the current output row needs are kept (tap rows only ever move forward), so a 2000-row
    // source needs a handful of float rows rather than a full-height intermediate.
    std::deque<std::pair<u32, std::vector<float>>> rows;
    std::vector<float> premul_row(u64{source.w} * 4);
    u32 col_first = source.w;
    u32 col_last = 0;
    for (const auto& taps : col_taps) {
        for (const Tap& tap : taps) {
            col_first = std::min(col_first, tap.index);
            col_last = std::max(col_last, tap.index + 1);
        }
    }
    const auto horizontal = [&](u32 k) -> const float* {
        if (!rows.empty() && k >= rows.front().first && k <= rows.back().first) {
            return rows[k - rows.front().first].second.data();
        }
        std::vector<float> line(u64{out.w} * 4, 0.0f);
        const u32* const src = source.pixels.data() + u64{k} * source.w;
        for (u32 x = col_first; x < col_last && x < source.w; ++x) {
            const u32 p = src[x];
            const float a = static_cast<float>(p >> 24) / 255.0f;
            premul_row[u64{x} * 4] = static_cast<float>((p >> 16) & 0xFF) * a;
            premul_row[u64{x} * 4 + 1] = static_cast<float>((p >> 8) & 0xFF) * a;
            premul_row[u64{x} * 4 + 2] = static_cast<float>(p & 0xFF) * a;
            premul_row[u64{x} * 4 + 3] = static_cast<float>(p >> 24);
        }
        for (u32 i = 0; i < out.w; ++i) {
            float acc[4]{};
            for (const Tap& tap : col_taps[i]) {
                const float* const t = premul_row.data() + u64{tap.index} * 4;
                acc[0] += t[0] * tap.weight;
                acc[1] += t[1] * tap.weight;
                acc[2] += t[2] * tap.weight;
                acc[3] += t[3] * tap.weight;
            }
            std::memcpy(line.data() + u64{i} * 4, acc, sizeof(acc));
        }
        if (!rows.empty() && k != rows.back().first + 1) {
            rows.clear();
        }
        rows.emplace_back(k, std::move(line));
        return rows.back().second.data();
    };
    out.premul.assign(u64{out.w} * out.h * 4, 0);
    std::vector<const float*> tap_rows;
    for (u32 j = 0; j < out.h; ++j) {
        const auto& taps = row_taps[j];
        if (taps.empty()) {
            continue;
        }
        while (!rows.empty() && rows.front().first < taps.front().index) {
            rows.pop_front();
        }
        tap_rows.clear();
        for (const Tap& tap : taps) {
            tap_rows.push_back(horizontal(tap.index));
        }
        u8* const dst = out.premul.data() + u64{j} * out.w * 4;
        for (u32 i = 0; i < out.w; ++i) {
            float acc[4]{};
            for (size_t t = 0; t < taps.size(); ++t) {
                const float* const v = tap_rows[t] + u64{i} * 4;
                acc[0] += v[0] * taps[t].weight;
                acc[1] += v[1] * taps[t].weight;
                acc[2] += v[2] * taps[t].weight;
                acc[3] += v[3] * taps[t].weight;
            }
            const float coverage = edge_coverage ? col_cov[i] * row_cov[j] : 1.0f;
            const float alpha = std::clamp(acc[3] * coverage, 0.0f, 255.0f);
            u8* const p = dst + u64{i} * 4;
            p[3] = static_cast<u8>(alpha + 0.5f);
            for (u32 c = 0; c < 3; ++c) {
                p[c] = static_cast<u8>(std::min(
                    static_cast<float>(p[3]), std::clamp(acc[c] * coverage, 0.0f, 255.0f) + 0.5f));
            }
        }
    }
    return out;
}

void ApplyMask(PlacedRaster& layer, const PlacedRaster& mask) {
    if (layer.Empty() || layer.x != mask.x || layer.y != mask.y || layer.w != mask.w ||
        layer.h != mask.h || mask.premul.size() != layer.premul.size()) {
        return;
    }
    for (size_t i = 0; i < layer.premul.size(); i += 4) {
        const u32 m = mask.premul[i + 3];
        if (m == 255) {
            continue;
        }
        for (size_t c = 0; c < 4; ++c) {
            layer.premul[i + c] = static_cast<u8>((u32{layer.premul[i + c]} * m + 127) / 255);
        }
    }
}

void TrimTransparent(PlacedRaster& layer) {
    if (layer.Empty()) {
        return;
    }
    u32 x0 = layer.w, y0 = layer.h, x1 = 0, y1 = 0;
    for (u32 j = 0; j < layer.h; ++j) {
        const u8* const row = layer.premul.data() + u64{j} * layer.w * 4;
        for (u32 i = 0; i < layer.w; ++i) {
            if (row[u64{i} * 4 + 3] != 0) {
                x0 = std::min(x0, i);
                x1 = std::max(x1, i + 1);
                y0 = std::min(y0, j);
                y1 = std::max(y1, j + 1);
            }
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        layer = {};
        return;
    }
    if (x0 == 0 && y0 == 0 && x1 == layer.w && y1 == layer.h) {
        return;
    }
    const u32 w = x1 - x0;
    const u32 h = y1 - y0;
    std::vector<u8> trimmed(u64{w} * h * 4);
    for (u32 j = 0; j < h; ++j) {
        std::memcpy(trimmed.data() + u64{j} * w * 4,
                    layer.premul.data() + (u64{y0 + j} * layer.w + x0) * 4, u64{w} * 4);
    }
    layer.x += static_cast<s32>(x0);
    layer.y += static_cast<s32>(y0);
    layer.w = w;
    layer.h = h;
    layer.premul = std::move(trimmed);
}

void BlendPremul(std::vector<u8>& canvas, u32 canvas_w, u32 canvas_h, const PlacedRaster& layer,
                 float opacity) {
    const u32 op = static_cast<u32>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
    if (op == 0 || layer.Empty()) {
        return;
    }
    // x / 255 rounded, exact for x <= 255 * 255.
    const auto div255 = [](u32 x) { return (x + 128 + ((x + 128) >> 8)) >> 8; };
    const s64 i0 = std::max<s64>(0, -s64{layer.x});
    const s64 i1 = std::min<s64>(layer.w, s64{canvas_w} - layer.x);
    if (i1 <= i0) {
        return;
    }
    for (u32 j = 0; j < layer.h; ++j) {
        const s64 cy = s64{layer.y} + j;
        if (cy < 0 || cy >= s64{canvas_h}) {
            continue;
        }
        const u8* src = layer.premul.data() + (u64{j} * layer.w + static_cast<u64>(i0)) * 4;
        u8* dst = canvas.data() +
                  (static_cast<u64>(cy) * canvas_w + static_cast<u64>(s64{layer.x} + i0)) * 4;
        for (s64 i = i0; i < i1; ++i, src += 4, dst += 4) {
            const u32 a = src[3];
            if (a == 0) {
                continue;
            }
            if (op == 255) {
                if (a == 255) {
                    std::memcpy(dst, src, 4);
                    continue;
                }
                const u32 inv = 255 - a;
                for (u32 c = 0; c < 4; ++c) {
                    dst[c] = static_cast<u8>(src[c] + div255(u32{dst[c]} * inv));
                }
                continue;
            }
            const u32 sa = div255(a * op);
            const u32 inv = 255 - sa;
            for (u32 c = 0; c < 3; ++c) {
                dst[c] = static_cast<u8>(
                    std::min<u32>(255, div255(u32{src[c]} * op) + div255(u32{dst[c]} * inv)));
            }
            dst[3] = static_cast<u8>(std::min<u32>(255, sa + div255(u32{dst[3]} * inv)));
        }
    }
}

void PremulToImage(std::span<const u8> premul, u32 width, u32 height, Image& out) {
    out.w = width;
    out.h = height;
    out.pixels.resize(u64{width} * height);
    for (u64 i = 0; i < out.pixels.size(); ++i) {
        const u8* const p = premul.data() + i * 4;
        const u32 a = p[3];
        if (a == 0) {
            out.pixels[i] = 0;
            continue;
        }
        const auto un = [a](u32 v) { return std::min<u32>(255, (v * 255 + a / 2) / a); };
        out.pixels[i] = (a << 24) | (un(p[0]) << 16) | (un(p[1]) << 8) | un(p[2]);
    }
}

} // namespace Core::Mods::NxAssets
