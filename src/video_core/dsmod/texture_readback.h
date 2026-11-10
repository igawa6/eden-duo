// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 24: a bounded, read-only CPU copy of one guest GPU texture, for a companion module
// (the "texture readback" module extension, core/mods/dsmod_module_extensions.h). This header is
// the contract between the module host (core/mods/mod_texture_readback.cpp) and the renderer
// (renderer_vulkan/vk_dsmod_texture_readback.cpp): plain request/result types plus the
// backend-independent rules (which guest formats a request may name, how to pick an image, how
// downloaded rows become the module's tightly packed pixels). Everything here is pure so it can
// be unit-tested without a GPU.

#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/surface.h"

namespace VideoCore::DSMod {

/// The pixel layouts a module may ask for. Each names a family of guest formats with the same
/// bytes per pixel; the result is always tightly packed in this layout.
enum class ReadbackFormat : u32 {
    R8 = 1,    ///< one byte per pixel (R8_UNORM)
    RGBA8 = 2, ///< four bytes per pixel, R, G, B, A in memory order (A8B8G8R8 / B8G8R8A8, any)
};

enum class ReadbackStatus : s32 {
    Ready = 1,
    NotFound = -4,    ///< no resident texture at that address (or none of that size/format)
    Mismatch = -5,    ///< a texture is there, but not with the requested size/format/kind
    Unsupported = -6, ///< this renderer cannot read textures back
    Failed = -7,      ///< the renderer gave up (device lost, shutdown, timeout)
};

inline constexpr u32 ReadbackMaxSide = 2048;
inline constexpr size_t ReadbackMaxBytes = size_t{16} << 20;

/// Optional selector flags (EdenDsmodTextureRequest::flags). Each narrows which images match, in
/// both modes; 0 = any plain current image of the size and format family.
inline constexpr u32 ReadbackRequireRenderTarget = 1u << 0; ///< drawn into as a colour target
inline constexpr u32 ReadbackRequireSrgb = 1u << 1;         ///< an sRGB format (RGBA8 only)
inline constexpr u32 ReadbackRequireUnorm = 1u << 2;        ///< a UNORM (linear) format
inline constexpr u32 ReadbackRequireRedrawn = 1u << 3; ///< drawn into as a colour target in 2+ frames
inline constexpr u32 ReadbackKnownFlags = ReadbackRequireRenderTarget | ReadbackRequireSrgb |
                                          ReadbackRequireUnorm | ReadbackRequireRedrawn;

struct ReadbackRequest {
    /// The texture's base address in the title's GPU address space; 0 = the single newest
    /// resident texture with this size and format (the result reports which one it took).
    GPUVAddr gpu_addr{};
    u32 width{};
    u32 height{};
    ReadbackFormat format{ReadbackFormat::R8};
    u32 flags{}; ///< Readback* selector flags (validated: ValidReadbackFlags)
};

struct ReadbackResult {
    ReadbackStatus status{ReadbackStatus::Failed};
    GPUVAddr gpu_addr{};
    u32 width{};
    u32 height{};
    /// Bumps whenever the GPU writes the texture (the texture cache's modification tick), so a
    /// module can skip work when the contents did not change.
    u64 generation{};
    /// Resident textures that matched (address mode: 1; size/format mode: how many to pick from).
    u32 candidates{};
    std::vector<u8> pixels; ///< width * height * BytesPerPixel(format), rows top to bottom
};

/// Called exactly once per accepted request, on the GPU thread (or never, when the renderer is
/// torn down first; the host times such a request out).
using ReadbackCallback = std::function<void(ReadbackResult&&)>;

[[nodiscard]] constexpr u32 BytesPerPixel(ReadbackFormat format) {
    return format == ReadbackFormat::RGBA8 ? 4 : 1;
}

[[nodiscard]] constexpr bool ValidReadbackFormat(u32 raw) {
    return raw == static_cast<u32>(ReadbackFormat::R8) ||
           raw == static_cast<u32>(ReadbackFormat::RGBA8);
}

/// The request's own bounds, before anything touches the GPU.
[[nodiscard]] constexpr bool ReadbackRequestInBounds(u32 width, u32 height, ReadbackFormat format) {
    return width != 0 && height != 0 && width <= ReadbackMaxSide && height <= ReadbackMaxSide &&
           size_t{width} * height * BytesPerPixel(format) <= ReadbackMaxBytes;
}

/// Selector flags a request may carry: only known bits, not sRGB and UNORM together, and no sRGB
/// requirement for R8 (there is no sRGB R8 format).
[[nodiscard]] constexpr bool ValidReadbackFlags(u32 flags, ReadbackFormat format) {
    if ((flags & ~ReadbackKnownFlags) != 0) {
        return false;
    }
    if ((flags & ReadbackRequireSrgb) != 0 &&
        ((flags & ReadbackRequireUnorm) != 0 || format == ReadbackFormat::R8)) {
        return false;
    }
    return true;
}

[[nodiscard]] constexpr bool IsSrgbReadbackFormat(Surface::PixelFormat guest) {
    return guest == Surface::PixelFormat::A8B8G8R8_SRGB ||
           guest == Surface::PixelFormat::B8G8R8A8_SRGB;
}

/// Whether a guest surface format belongs to the requested family. Only plain 8-bit unorm/srgb
/// colour formats qualify: compressed, depth, float or packed formats are never converted.
[[nodiscard]] constexpr bool ReadbackFormatMatches(Surface::PixelFormat guest,
                                                   ReadbackFormat want) {
    using Surface::PixelFormat;
    switch (want) {
    case ReadbackFormat::R8:
        return guest == PixelFormat::R8_UNORM;
    case ReadbackFormat::RGBA8:
        return guest == PixelFormat::A8B8G8R8_UNORM || guest == PixelFormat::A8B8G8R8_SRGB ||
               guest == PixelFormat::B8G8R8A8_UNORM || guest == PixelFormat::B8G8R8A8_SRGB;
    }
    return false;
}

/// One resident image as the picker sees it.
struct ReadbackCandidate {
    GPUVAddr gpu_addr{};
    u32 width{};
    u32 height{};
    Surface::PixelFormat format{};
    bool plain_2d{};  ///< one 2D (or pitch-linear) level-0 single-sample colour surface
    bool current{};   ///< the host copy is up to date (not modified by the CPU since)
    u64 generation{}; ///< modification tick
    bool render_target{}; ///< the GPU has drawn into it as a colour target
    bool redrawn{};       ///< ... in at least two different frames
};

/// Whether an image satisfies a request's selector flags (format family checked separately).
[[nodiscard]] constexpr bool ReadbackFlagsMatch(const ReadbackCandidate& image, u32 flags) {
    if ((flags & ReadbackRequireRenderTarget) != 0 && !image.render_target) {
        return false;
    }
    if ((flags & ReadbackRequireRedrawn) != 0 && !(image.render_target && image.redrawn)) {
        return false;
    }
    if ((flags & ReadbackRequireSrgb) != 0 && !IsSrgbReadbackFormat(image.format)) {
        return false;
    }
    if ((flags & ReadbackRequireUnorm) != 0 && IsSrgbReadbackFormat(image.format)) {
        return false;
    }
    return true;
}

struct ReadbackPick {
    ReadbackStatus status{ReadbackStatus::NotFound};
    size_t index{}; ///< into the candidate list, valid when status == Ready
    u32 candidates{};
};

/// Chooses the image a request reads. Address mode: the image based exactly at gpu_addr must
/// have the requested size, a format of the requested family, be a plain current 2D surface and
/// satisfy the selector flags, else Mismatch (NotFound when nothing is based there). Size/format
/// mode (gpu_addr 0): the newest (highest generation) such image of that size and family;
/// `candidates` counts every image that matched (size, family and flags).
template <typename Range>
[[nodiscard]] ReadbackPick PickReadbackImage(const ReadbackRequest& request, const Range& images) {
    ReadbackPick pick;
    bool seen_at_address = false;
    u64 best_generation = 0;
    size_t i = 0;
    for (const ReadbackCandidate& image : images) {
        const size_t index = i++;
        const bool fits = image.plain_2d && image.current && image.width == request.width &&
                          image.height == request.height &&
                          ReadbackFormatMatches(image.format, request.format) &&
                          ReadbackFlagsMatch(image, request.flags);
        if (request.gpu_addr != 0) {
            if (image.gpu_addr != request.gpu_addr) {
                continue;
            }
            seen_at_address = true;
            if (fits && (pick.status != ReadbackStatus::Ready ||
                         image.generation > best_generation)) {
                pick.status = ReadbackStatus::Ready;
                pick.index = index;
                pick.candidates = 1;
                best_generation = image.generation;
            }
            continue;
        }
        if (!fits) {
            continue;
        }
        ++pick.candidates;
        if (pick.status != ReadbackStatus::Ready || image.generation > best_generation) {
            pick.status = ReadbackStatus::Ready;
            pick.index = index;
            best_generation = image.generation;
        }
    }
    if (pick.status != ReadbackStatus::Ready && seen_at_address) {
        pick.status = ReadbackStatus::Mismatch;
    }
    return pick;
}

/// Turns downloaded rows (`row_bytes` apart) into the module's tightly packed pixels;
/// `swap_red_blue` when the host copy is stored B, G, R, A. False when the source is too small.
[[nodiscard]] inline bool PackReadbackPixels(ReadbackFormat want, bool swap_red_blue, u32 width,
                                             u32 height, std::span<const u8> source,
                                             size_t row_bytes, std::vector<u8>& out) {
    const size_t bpp = BytesPerPixel(want);
    const size_t tight = size_t{width} * bpp;
    if (width == 0 || height == 0 || row_bytes < tight ||
        source.size() < row_bytes * (height - 1) + tight) {
        return false;
    }
    out.resize(tight * height);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(out.data() + y * tight, source.data() + y * row_bytes, tight);
    }
    if (swap_red_blue && bpp == 4) {
        for (size_t p = 0; p + 3 < out.size(); p += 4) {
            std::swap(out[p], out[p + 2]);
        }
    }
    return true;
}

} // namespace VideoCore::DSMod
