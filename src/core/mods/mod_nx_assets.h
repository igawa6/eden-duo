// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Nintendo first-party asset containers, read straight out of the running game's romfs so a
// package can show the game's own art without shipping any of it:
//   SARC  -- plain (uncompressed) archive, e.g. Link's Awakening's ui/*.arc
//   BNTX  -- Switch texture container (multi-texture, block-linear, ASTC / BCn / raw formats)
//   BFFNT -- Switch binary font (v4: glyph sheets are one embedded BNTX array texture)
// plus the area-averaging resampler the composite images use.
//
// Everything here is pure format code: no runtime, no logging, no filesystem. Callers hand in a
// ranged reader so a 75 MB archive is never read whole just to pull one 144 px icon out of it.

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "common/common_types.h"
#include "core/mods/mod_types.h"
#include "core/mods/mod_ui.h"

namespace Core::Mods::NxAssets {

/// Reads exactly `out.size()` bytes at `offset` of some byte source; false on a short read.
using ReadAt = std::function<bool(u64 offset, std::span<u8> out)>;

/// A ReadAt over bytes already in memory.
[[nodiscard]] ReadAt SpanReader(std::span<const u8> bytes);

[[nodiscard]] bool HasMagic(std::span<const u8> head, std::string_view magic);

// ---------------------------------------------------------------------------------------------
// SARC

struct SarcMember {
    u64 offset{}; ///< absolute, within the archive
    u64 size{};
};

struct SarcIndex {
    std::unordered_map<std::string, SarcMember> by_name;
    std::unordered_map<u32, SarcMember> by_hash;
    u32 hash_key{0x65};
    [[nodiscard]] const SarcMember* Find(std::string_view name) const;
};

/// Parses the SARC header + file table of an archive of `size` bytes.
bool ParseSarc(const ReadAt& read, u64 size, SarcIndex& out, std::string& error);

// ---------------------------------------------------------------------------------------------
// BNTX

struct BntxTexture {
    std::string name;
    u32 format{}; ///< (type << 8) | data type
    u32 width{};
    u32 height{};
    u32 depth{};
    u32 array_count{};
    u32 mip_count{};
    u32 block_height_log2{}; ///< BRTI +0x34 low bits: Tegra block-linear block height in GOBs
    u32 image_size{};        ///< all mips, all layers
    u32 alignment{};
    std::array<u8, 4> channels{2, 3, 4, 5}; ///< BRTI +0x58: 0 zero, 1 one, 2 R, 3 G, 4 B, 5 A
    std::vector<u64> mip_offsets;           ///< relative to the start of the BNTX
};

struct BntxIndex {
    std::vector<BntxTexture> textures;
    std::unordered_map<std::string, u32> by_name;
    /// A texture by exact name; an empty name means texture 0.
    [[nodiscard]] const BntxTexture* Find(std::string_view name) const;
};

/// Parses the texture table of a BNTX of `size` bytes (only its metadata part is read).
bool ParseBntx(const ReadAt& read, u64 size, BntxIndex& out, std::string& error);

/// "ASTC6x6_SRGB", "BC5_UNORM", ... for logs.
[[nodiscard]] std::string FormatName(u32 format);

/// Whether the decoder handles this BNTX format.
[[nodiscard]] bool FormatSupported(u32 format);

/// Reads and decodes mip 0 of one array layer to straight-alpha RGBA8 (bytes as stored: sRGB
/// textures are not linearised, which is what the game's own UI shader expects to see), with the
/// texture's channel map applied. `read` addresses the BNTX itself.
bool DecodeTexture(const ReadAt& read, u64 bntx_size, const BntxTexture& texture, u32 layer,
                   std::vector<u8>& rgba, std::string& error);

/// DecodeTexture straight into a canvas ARGB image (one full-size buffer, no RGBA copy).
bool DecodeTextureImage(const ReadAt& read, u64 bntx_size, const BntxTexture& texture, u32 layer,
                        Image& out, std::string& error);

/// RGBA8 bytes -> the canvas' ARGB image, optionally flipped vertically.
void RgbaToImage(std::span<const u8> rgba, u32 width, u32 height, Image& out, bool flip_y = false);
/// The canvas' ARGB image -> RGBA8 bytes (for PNG dumps).
std::vector<u8> ImageToRgba(const Image& image);

/// Decodes a whole BNTX held in memory: the named texture (empty = texture 0) into `out`.
bool DecodeBntxImage(std::span<const u8> file, std::string_view name, Image& out,
                     std::string& error);

// ---------------------------------------------------------------------------------------------
// BFFNT

struct BffntGlyphWidth {
    s8 left{};
    u8 glyph_width{};
    u8 char_width{};
};

struct BffntInfo {
    u8 font_type{};
    u8 height{};
    u8 width{};
    u8 ascent{};
    u16 line_feed{};
    u16 alter_char_index{};
    BffntGlyphWidth default_width{};
    u8 cell_w{};
    u8 cell_h{};
    u8 sheet_count{};
    u16 baseline{};
    u16 sheet_format{};
    u16 cols{};
    u16 rows{};
    u16 sheet_w{};
    u16 sheet_h{};
    u64 sheet_data_offset{};
    std::unordered_map<u32, u16> cmap;               ///< code point -> glyph index
    std::unordered_map<u16, BffntGlyphWidth> widths; ///< glyph index -> widths
};

bool ParseBffnt(std::span<const u8> file, BffntInfo& out, std::string& error);

/// Builds a canvas font from a BFFNT: glyph metrics for code points [first, last] (dense, missing
/// ones = the font's alternate character) plus every other mapped code point (sparse `extra`), and
/// one atlas holding every sheet, upright, stacked top to bottom (sheet i starts at y = i *
/// sheet_h). Glyph cells keep the font's own design size; the canvas scales them.
bool BuildBffntFont(std::span<const u8> file, FontMetrics& metrics, Image& atlas,
                    std::string& error, u32 first = 0x20, u32 last = 0xFF);

// ---------------------------------------------------------------------------------------------
// Area-averaged resampling for composites

/// A layer resampled into composite space: premultiplied RGBA8 covering [x, x+w) x [y, y+h).
struct PlacedRaster {
    s32 x{};
    s32 y{};
    u32 w{};
    u32 h{};
    std::vector<u8> premul; ///< RGBA, premultiplied by alpha (edge coverage included)
    [[nodiscard]] bool Empty() const {
        return w == 0 || h == 0;
    }
};

/// Box-filters the source sub-rectangle [sx0, sx1) x [sy0, sy1) (source pixels) onto the
/// destination rectangle [dx0, dx1) x [dy0, dy1) (composite pixels, fractional allowed), clipped
/// to a `canvas_w` x `canvas_h` composite. Every destination pixel is the exact area average of
/// the source it covers; partially covered edge pixels get partial alpha. Premultiplied alpha,
/// so transparent texels never darken the edges.
/// `edge_coverage` = false leaves partially covered edge pixels at full weight (used for masks,
/// whose placement already matches the layer they cut).
PlacedRaster ResampleArea(const Image& source, double sx0, double sy0, double sx1, double sy1,
                          double dx0, double dy0, double dx1, double dy1, u32 canvas_w,
                          u32 canvas_h, bool edge_coverage = true);

/// Multiplies a layer by a mask's alpha. Both must come from ResampleArea with the same
/// destination rectangle and canvas (identical placement); otherwise the layer is left alone.
void ApplyMask(PlacedRaster& layer, const PlacedRaster& mask);

/// Shrinks a raster to the bounding box of its non-transparent pixels (masked map pieces are
/// mostly empty rectangles); an all-transparent raster becomes empty.
void TrimTransparent(PlacedRaster& layer);

/// Source-over of a placed raster onto a premultiplied RGBA8 canvas with an extra opacity.
void BlendPremul(std::vector<u8>& canvas_premul, u32 canvas_w, u32 canvas_h,
                 const PlacedRaster& layer, float opacity);

/// Premultiplied RGBA8 canvas -> straight-alpha ARGB image.
void PremulToImage(std::span<const u8> premul, u32 width, u32 height, Image& out);

// ---------------------------------------------------------------------------------------------
// Manifest (implemented in mod_nx_runtime.cpp)

/// Reads the top-level "composites" object and the long font key spellings
/// ("font_metrics_src" / "font_atlas_src") into the manifest.
void ParseManifestExtras(const nlohmann::json& json, Manifest& manifest);

} // namespace Core::Mods::NxAssets
