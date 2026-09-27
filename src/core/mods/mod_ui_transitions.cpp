// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Page transition frames, composed from whole-canvas snapshots of the two pages: ComposePageSlide
// (slide up / down with an edge shadow), ComposePageGrow (a rect growing from an origin, with a
// shadow band) and ComposePageFade (a cross-fade). Div255 / BlendOver are the exact integer blend
// they share. Driven by DrivePageTransition (mod_pages.cpp).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/mods/mod_ui.h"

namespace Core::Mods {

void ComposePageSlide(std::span<u32> out, std::span<const u32> from, std::span<const u32> to, u32 w,
                      u32 h, PageTransition kind, float eased, float shadow) {
    const size_t row_px = w;
    if (out.size() < static_cast<size_t>(w) * h || from.size() < out.size() ||
        to.size() < out.size() || w == 0 || h == 0) {
        return;
    }
    eased = std::clamp(eased, 0.0f, 1.0f);
    const bool up = kind == PageTransition::SlideUp;
    // Edge = the moving page's top edge. Above it: the page underneath (rows 0..edge of the old
    // page for slide_up, of the new page for slide_down). Below it: the moving page's own top rows.
    const s32 edge =
        static_cast<s32>(std::lround(static_cast<float>(h) * (up ? 1.0f - eased : eased)));
    const std::span<const u32> under = up ? from : to;
    const std::span<const u32> mover = up ? to : from;
    const size_t top_rows = static_cast<size_t>(std::clamp<s32>(edge, 0, static_cast<s32>(h)));
    if (top_rows > 0) {
        std::memcpy(out.data(), under.data(), top_rows * row_px * sizeof(u32));
    }
    if (top_rows < h) {
        std::memcpy(out.data() + top_rows * row_px, mover.data(),
                    (h - top_rows) * row_px * sizeof(u32));
    }
    // A soft shadow on the page underneath, strongest right at the edge: the moving page reads as
    // lying on top of the other one.
    constexpr s32 ShadowRows = 24;
    if (shadow > 0.0f && top_rows > 0 && top_rows < h) {
        const s32 first = std::max<s32>(0, edge - ShadowRows);
        for (s32 y = first; y < edge; ++y) {
            const float t = static_cast<float>(y - (edge - ShadowRows) + 1) / ShadowRows;
            const u32 keep = static_cast<u32>(
                std::lround(255.0f * (1.0f - std::clamp(shadow, 0.0f, 1.0f) * t * t)));
            u32* const row = out.data() + static_cast<size_t>(y) * row_px;
            for (size_t x = 0; x < row_px; ++x) {
                const u32 p = row[x];
                const u32 r = (((p >> 16) & 0xFF) * keep) / 255;
                const u32 g = (((p >> 8) & 0xFF) * keep) / 255;
                const u32 b = ((p & 0xFF) * keep) / 255;
                row[x] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

namespace {
/// Exact floor(x/255) for x in [0, 65025] (the range every BlendOver channel sum is bounded to:
/// ch*alpha + ch*inv <= 255*alpha + 255*inv == 255*255, since alpha+inv==255 and each channel byte
/// is <=255) without a hardware divide -- a well-known bit trick, not an approximation: two shifts
/// and two adds instead of an integer division. It matters for the Fade transition, which (unlike
/// Grow/Shrink) runs this blend across the WHOLE canvas on EVERY frame rather than only within a
/// shrinking/growing rect during its first/last quarter -- the division was showing up as real cost
/// at that call volume. Grow/Shrink's own fade-in/out blend uses it too, with bit-identical output.
inline u32 Div255(u32 x) {
    return (x + 1 + (x >> 8)) >> 8;
}

/// Blends `src` over `dst` by `alpha` (0..255), keeping dst's alpha channel opaque (both spans are
/// whole opaque canvas frames).
inline u32 BlendOver(u32 dst, u32 src, u32 alpha) {
    if (alpha >= 255) {
        return src;
    }
    if (alpha == 0) {
        return dst;
    }
    const u32 inv = 255 - alpha;
    const u32 r = Div255(((src >> 16) & 0xFF) * alpha + ((dst >> 16) & 0xFF) * inv);
    const u32 g = Div255(((src >> 8) & 0xFF) * alpha + ((dst >> 8) & 0xFF) * inv);
    const u32 b = Div255((src & 0xFF) * alpha + (dst & 0xFF) * inv);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
} // namespace

void ComposePageGrow(std::span<u32> out, std::span<const u32> background,
                     std::span<const u32> moving, u32 w, u32 h, std::array<s32, 4> origin,
                     std::array<s32, 4> dest, float p, float shadow) {
    if (out.size() < static_cast<size_t>(w) * h || background.size() < out.size() ||
        moving.size() < out.size() || w == 0 || h == 0) {
        return;
    }
    p = std::clamp(p, 0.0f, 1.0f);
    const size_t row_px = w;
    // Background everywhere first: always a plain memcpy (a per-pixel dim of the WHOLE canvas,
    // tried first, cost ~6-7 ms of the ~7-9 ms this transition style measured at full canvas size
    // -- by far the dominant cost, and most of it invisible, dimming pixels the moving rect is
    // about to draw over anyway). The shadow below is a bounded band around the rect instead.
    std::memcpy(out.data(), background.data(), out.size() * sizeof(u32));
    const auto lerp = [p](s32 a, s32 b) { return static_cast<s32>(std::lround(a + (b - a) * p)); };
    const std::array<s32, 4> rect = {lerp(origin[0], dest[0]), lerp(origin[1], dest[1]),
                                     lerp(origin[2], dest[2]), lerp(origin[3], dest[3])};
    const s32 iw = static_cast<s32>(w), ih = static_cast<s32>(h);
    const s32 rx0 = std::clamp(rect[0], 0, iw);
    const s32 ry0 = std::clamp(rect[1], 0, ih);
    const s32 rx1 = std::clamp(rect[0] + rect[2], 0, iw);
    const s32 ry1 = std::clamp(rect[1] + rect[3], 0, ih);
    if (rx1 <= rx0 || ry1 <= ry0 || rect[2] <= 0 || rect[3] <= 0 || dest[2] <= 0 || dest[3] <= 0) {
        return; // an unresolved / zero-size origin or dest: background only, nothing to grow
    }
    // A soft shadow in a band just outside the growing/shrinking rect, strongest right at its
    // edge (the same idea as the slide transition's edge shadow, wrapped around a rect instead of
    // one horizontal line): bounded to the rect's perimeter x ShadowPx, not the whole canvas, so
    // it stays cheap even while the rect covers most of the canvas.
    if (shadow > 0.0f) {
        constexpr s32 ShadowPx = 28;
        const s32 sx0 = std::max(0, rx0 - ShadowPx), sy0 = std::max(0, ry0 - ShadowPx);
        const s32 sx1 = std::min(iw, rx1 + ShadowPx), sy1 = std::min(ih, ry1 + ShadowPx);
        const float shadow_c = std::clamp(shadow, 0.0f, 1.0f);
        for (s32 y = sy0; y < sy1; ++y) {
            const bool row_in_rect = y >= ry0 && y < ry1;
            u32* const row = out.data() + static_cast<size_t>(y) * row_px;
            for (s32 x = sx0; x < sx1; ++x) {
                if (row_in_rect && x >= rx0 && x < rx1) {
                    continue; // inside the rect itself: the moving blit below overwrites this
                }
                const s32 dx = x < rx0 ? rx0 - x : (x >= rx1 ? x - rx1 + 1 : 0);
                const s32 dy = y < ry0 ? ry0 - y : (y >= ry1 ? y - ry1 + 1 : 0);
                const s32 d = std::max(dx, dy);
                if (d > ShadowPx) {
                    continue;
                }
                const float t = 1.0f - static_cast<float>(d) / static_cast<float>(ShadowPx);
                const u32 keep = static_cast<u32>(std::lround(255.0f * (1.0f - shadow_c * t * t)));
                const u32 px = row[x];
                const u32 r = (((px >> 16) & 0xFF) * keep) / 255;
                const u32 g = (((px >> 8) & 0xFF) * keep) / 255;
                const u32 b = ((px & 0xFF) * keep) / 255;
                row[x] = (px & 0xFF000000u) | (r << 16) | (g << 8) | b;
            }
        }
    }
    // Nearest-neighbour, with the destination column precomputed once per output column so the
    // inner loop over rows is pure array lookups (no per-pixel divide).
    std::vector<s32> xsrc(static_cast<size_t>(rx1 - rx0));
    for (s32 x = rx0; x < rx1; ++x) {
        const float nx = static_cast<float>(x - rect[0]) / static_cast<float>(rect[2]);
        xsrc[static_cast<size_t>(x - rx0)] =
            std::clamp(dest[0] + static_cast<s32>(nx * static_cast<float>(dest[2])), 0, iw - 1);
    }
    // Fades the moving page in over the first quarter of the grow (out over the last quarter of
    // the shrink, since the caller passes p already flipped for Shrink) so the very first,
    // heavily-downscaled frame reads as a soft materialisation rather than a jarring pop of noise.
    const float alpha_f = std::clamp(p / 0.25f, 0.0f, 1.0f);
    const u32 alpha = static_cast<u32>(std::lround(alpha_f * 255.0f));
    for (s32 y = ry0; y < ry1; ++y) {
        const float ny = static_cast<float>(y - rect[1]) / static_cast<float>(rect[3]);
        const s32 sy =
            std::clamp(dest[1] + static_cast<s32>(ny * static_cast<float>(dest[3])), 0, ih - 1);
        const u32* const src_row = moving.data() + static_cast<size_t>(sy) * row_px;
        u32* const out_row = out.data() + static_cast<size_t>(y) * row_px;
        for (s32 x = rx0; x < rx1; ++x) {
            const u32 src = src_row[static_cast<size_t>(xsrc[static_cast<size_t>(x - rx0)])];
            out_row[x] = alpha >= 255 ? src : BlendOver(out_row[x], src, alpha);
        }
    }
}

void ComposePageFade(std::span<u32> out, std::span<const u32> from, std::span<const u32> to, u32 w,
                     u32 h, float eased) {
    if (out.size() < static_cast<size_t>(w) * h || from.size() < out.size() ||
        to.size() < out.size() || w == 0 || h == 0) {
        return;
    }
    eased = std::clamp(eased, 0.0f, 1.0f);
    const u32 alpha = static_cast<u32>(std::lround(eased * 255.0f));
    // The extremes are a plain memcpy (both pages already exist at full size, nothing to blend
    // yet/anymore) -- cheaper than routing them through BlendOver's per-pixel branch, and it is
    // exactly what the loop below would have produced anyway.
    if (alpha == 0) {
        std::memcpy(out.data(), from.data(), out.size() * sizeof(u32));
        return;
    }
    if (alpha >= 255) {
        std::memcpy(out.data(), to.data(), out.size() * sizeof(u32));
        return;
    }
    // One pass, no LUT, no rect geometry -- both inputs already cover the whole canvas at full
    // size (the one-time page snapshots), so unlike Grow/Shrink there is nothing to sample or
    // scale, only a per-pixel blend. The cheapest transition of the set by construction.
    // `alpha` is the same for every pixel this frame, so `inv` is hoisted out of the loop and the
    // blend is inlined rather than routed through BlendOver's alpha==0/alpha>=255 branch checks
    // (already handled once, above, for the whole frame -- re-testing them per pixel here would
    // be pure overhead since neither can be true inside this loop).
    const u32 inv = 255 - alpha;
    for (size_t i = 0; i < out.size(); ++i) {
        const u32 s = to[i];
        const u32 d = from[i];
        const u32 r = Div255(((s >> 16) & 0xFF) * alpha + ((d >> 16) & 0xFF) * inv);
        const u32 g = Div255(((s >> 8) & 0xFF) * alpha + ((d >> 8) & 0xFF) * inv);
        const u32 b = Div255((s & 0xFF) * alpha + (d & 0xFF) * inv);
        out[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

} // namespace Core::Mods
