// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Canvas image drawing: DrawImage, DrawImageRegion (the hot blit: a column table instead of a
// divide per pixel, tint lookup tables, reuse of an opaque source row), DrawImageMask,
// DrawImageRegionRotated and DrawImageFilled, plus ApplyDrawOpacity, the canvas opacity they apply
// per pixel (the shape primitives call it once per draw). Nearest-neighbour sampling throughout;
// every fast path is bit-exact with the plain Tint + Blend formulas.
// Not here: shapes (mod_ui_canvas.cpp), glyph layout (mod_ui_text.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/mods/mod_ui.h"
#include "core/mods/mod_ui_internal.h"

namespace Core::Mods {

namespace {
/// Multiply a texel by a tint, channel by channel (alpha included): a white skin piece drawn with
/// the panel's line colour comes out in that colour, not just fainter.
u32 Tint(u32 texel, u32 tint) {
    u32 out = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
        const u32 c = (((texel >> shift) & 0xFF) * ((tint >> shift) & 0xFF)) / 255;
        out |= c << shift;
    }
    return out;
}
} // namespace

u32 Canvas::ApplyDrawOpacity(u32 argb) const {
    const float opacity = EffectiveOpacity();
    if (opacity >= 1.0f) {
        return argb;
    }
    const u32 alpha = static_cast<u32>(static_cast<float>(argb >> 24) * opacity + 0.5f);
    return (argb & 0x00FFFFFFu) | (alpha << 24);
}

void Canvas::DrawImage(s32 x, s32 y, s32 rw, s32 rh, const Image& image, u32 tint) {
    if (!image.Valid() || rw <= 0 || rh <= 0 || w == 0 || h == 0) {
        return;
    }
    // The whole image is the region (0,0,w,h): same floor sampling, per-channel tint and blend, so
    // the output is bit-identical -- but the region path looks the source column up in a table
    // instead of a 64-bit divide per pixel, which made a full-page redraw (page background, map
    // sheet, map composite: ~2.6 Mpx) cost 10-30 ms on every marker move.
    DrawImageRegion(x, y, rw, rh, image, 0, 0, static_cast<s32>(image.w), static_cast<s32>(image.h),
                    tint);
}

void Canvas::DrawImageRegion(s32 x, s32 y, s32 rw, s32 rh, const Image& image, s32 sx, s32 sy,
                             s32 sw, s32 sh, u32 tint, bool flip_x, bool flip_y) {
    if (!image.Valid() || rw <= 0 || rh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    const bool plain = tint == 0xFFFFFFFFu;
    // Precompute column -> source-x once instead of a 64-bit divide per pixel (~480x fewer divides
    // for a full-panel map blit). Same floor formula and bounds, so the output is bit-identical;
    // -1 marks a column sampling outside the image / off-canvas, skipped in the row loop.
    static thread_local std::vector<s32> col_src;
    col_src.assign(static_cast<size_t>(rw), -1);
    const s32 iw = static_cast<s32>(image.w);
    for (s32 col = 0; col < rw; ++col) {
        const s32 px = x + col;
        if (px < clip_x0 || px >= clip_x1) {
            continue;
        }
        const s32 off = static_cast<s32>(static_cast<s64>(col) * sw / rw);
        const s32 src_x = flip_x ? sx + (sw - 1) - off : sx + off;
        if (src_x < 0 || src_x >= iw) {
            continue;
        }
        col_src[static_cast<size_t>(col)] = src_x;
    }
    // The drawn columns form one run (clipping and the image edge only cut the ends).
    s32 run0 = rw, run1 = 0;
    for (s32 col = 0; col < rw; ++col) {
        if (col_src[static_cast<size_t>(col)] >= 0) {
            run0 = std::min(run0, col);
            run1 = col + 1;
        }
    }
    // An upscaled blit repeats source rows: a row drawn only with opaque texels does not depend on
    // what was under it, so the next row from the same source row is a copy of it.
    s32 reuse_src_y = -1;
    const u32* reuse_row = nullptr;
    const bool full_opacity = EffectiveOpacity() >= 1.0f; // ApplyDrawOpacity is the identity
    // Tint() per channel as a lookup; only worth building for a large blit (not for a glyph).
    const bool use_lut = !plain && full_opacity && static_cast<s64>(rw) * rh >= 4096;
    // A tint that only scales alpha (a translucent map icon): one small table, same result as Tint.
    const bool alpha_only = !plain && !use_lut && (tint & 0x00FFFFFFu) == 0x00FFFFFFu;
    std::array<u32, 256> alpha_lut{};
    if (alpha_only) {
        const u32 ta = tint >> 24;
        for (u32 v = 0; v < 256; ++v) {
            alpha_lut[v] = ((v * ta) / 255) << 24;
        }
    }
    std::array<std::array<u32, 256>, 4> tint_lut{};
    if (use_lut) {
        for (u32 ch = 0; ch < 4; ++ch) {
            const u32 t = (tint >> (ch * 8)) & 0xFF;
            for (u32 v = 0; v < 256; ++v) {
                tint_lut[ch][v] = ((v * t) / 255) << (ch * 8);
            }
        }
    }
    if (run1 <= run0) {
        return; // nothing of the image falls inside the clip
    }
    const s32 row0 = std::max(0, clip_y0 - y);
    const s32 row1 = std::min(rh, clip_y1 - y);
    for (s32 row = row0; row < row1; ++row) {
        const s32 py = y + row;
        const s32 off_y = static_cast<s32>(static_cast<s64>(row) * sh / rh);
        const s32 src_y = flip_y ? sy + (sh - 1) - off_y : sy + off_y;
        if (src_y < 0 || src_y >= static_cast<s32>(image.h)) {
            continue;
        }
        u32* const dst = pixels.data() + static_cast<size_t>(py) * w;
        const u32* const src = image.pixels.data() + static_cast<size_t>(src_y) * image.w;
        if (!full_opacity) {
            for (s32 col = 0; col < rw; ++col) {
                const s32 src_x = col_src[static_cast<size_t>(col)];
                if (src_x < 0) {
                    continue;
                }
                u32 texel = src[src_x];
                if (!plain) {
                    texel = Tint(texel, tint);
                }
                dst[x + col] = Blend(dst[x + col], ApplyDrawOpacity(texel));
            }
            continue;
        }
        // Full opacity (the common case): the tint comes from the per-channel table and an opaque
        // texel is stored directly -- exactly what Tint + Blend compute, without their per-pixel
        // arithmetic (a tinted full-page background went from ~9 ms to ~1.5 ms).
        if (src_y == reuse_src_y && reuse_row != nullptr && run1 > run0) {
            std::memcpy(dst + x + run0, reuse_row + x + run0,
                        static_cast<size_t>(run1 - run0) * sizeof(u32));
            continue;
        }
        bool opaque_row = true;
        for (s32 col = 0; col < rw; ++col) {
            const s32 src_x = col_src[static_cast<size_t>(col)];
            if (src_x < 0) {
                continue;
            }
            u32 texel = src[src_x];
            if (use_lut) {
                texel = tint_lut[0][texel & 0xFF] | tint_lut[1][(texel >> 8) & 0xFF] |
                        tint_lut[2][(texel >> 16) & 0xFF] | tint_lut[3][texel >> 24];
            } else if (alpha_only) {
                texel = (texel & 0x00FFFFFFu) | alpha_lut[texel >> 24];
            } else if (!plain) {
                texel = Tint(texel, tint);
            }
            if ((texel >> 24) == 0xFF) {
                dst[x + col] = texel;
            } else {
                dst[x + col] = Blend(dst[x + col], texel);
                opaque_row = false;
            }
        }
        reuse_src_y = opaque_row ? src_y : -1;
        reuse_row = opaque_row ? dst : nullptr;
    }
}

void Canvas::DrawImageMask(s32 x, s32 y, s32 rw, s32 rh, const Image& image, s32 sx, s32 sy, s32 sw,
                           s32 sh, u32 argb) {
    if (!image.Valid() || rw <= 0 || rh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    const u32 color_alpha = argb >> 24;
    const u32 rgb = argb & 0x00FFFFFFu;
    const s32 iw = static_cast<s32>(image.w);
    const s32 ih = static_cast<s32>(image.h);
    for (s32 row = 0; row < rh; ++row) {
        const s32 py = y + row;
        const s32 src_y = sy + static_cast<s32>(static_cast<s64>(row) * sh / rh);
        if (py < clip_y0 || py >= clip_y1 || src_y < 0 || src_y >= ih) {
            continue;
        }
        u32* const dst = pixels.data() + static_cast<size_t>(py) * w;
        const u32* const src = image.pixels.data() + static_cast<size_t>(src_y) * image.w;
        for (s32 col = 0; col < rw; ++col) {
            const s32 px = x + col;
            const s32 src_x = sx + static_cast<s32>(static_cast<s64>(col) * sw / rw);
            if (px < clip_x0 || px >= clip_x1 || src_x < 0 || src_x >= iw) {
                continue;
            }
            const u32 a = ((src[src_x] >> 24) * color_alpha) / 255;
            if (a != 0) {
                dst[px] = Blend(dst[px], ApplyDrawOpacity((a << 24) | rgb));
            }
        }
    }
}

void Canvas::DrawImageRegionRotated(s32 x, s32 y, s32 rw, s32 rh, const Image& image, s32 sx,
                                    s32 sy, s32 sw, s32 sh, u32 tint, float angle_rad) {
    if (!image.Valid() || rw <= 0 || rh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    // Inverse mapping: every canvas pixel inside the rotated rect's bounding circle is turned
    // back by the angle and, when it lands inside the unrotated rect, takes the nearest texel.
    // Screen y points down, so a positive angle turns clockwise as seen on the panel.
    const bool plain = tint == 0xFFFFFFFFu;
    const float cx = static_cast<float>(x) + static_cast<float>(rw) * 0.5f;
    const float cy = static_cast<float>(y) + static_cast<float>(rh) * 0.5f;
    const float c = std::cos(angle_rad), s = std::sin(angle_rad);
    const float half_w = static_cast<float>(rw) * 0.5f, half_h = static_cast<float>(rh) * 0.5f;
    const float radius = std::sqrt(half_w * half_w + half_h * half_h) + 1.0f;
    const s32 x0 = std::max(clip_x0, static_cast<s32>(std::floor(cx - radius)));
    const s32 x1 = std::min(clip_x1 - 1, static_cast<s32>(std::ceil(cx + radius)));
    const s32 y0 = std::max(clip_y0, static_cast<s32>(std::floor(cy - radius)));
    const s32 y1 = std::min(clip_y1 - 1, static_cast<s32>(std::ceil(cy + radius)));
    const float su = static_cast<float>(sw) / static_cast<float>(rw);
    const float sv = static_cast<float>(sh) / static_cast<float>(rh);
    const s32 iw = static_cast<s32>(image.w), ih = static_cast<s32>(image.h);
    for (s32 py = y0; py <= y1; ++py) {
        u32* const dst = pixels.data() + static_cast<size_t>(py) * w;
        const float dy = static_cast<float>(py) + 0.5f - cy;
        for (s32 px = x0; px <= x1; ++px) {
            const float dx = static_cast<float>(px) + 0.5f - cx;
            const float ux = dx * c + dy * s;
            const float uy = -dx * s + dy * c;
            if (ux < -half_w || ux >= half_w || uy < -half_h || uy >= half_h) {
                continue;
            }
            const s32 src_x = sx + static_cast<s32>((ux + half_w) * su);
            const s32 src_y = sy + static_cast<s32>((uy + half_h) * sv);
            if (src_x < 0 || src_x >= iw || src_y < 0 || src_y >= ih) {
                continue;
            }
            u32 texel = image.pixels[static_cast<size_t>(src_y) * image.w + src_x];
            if (!plain) {
                texel = Tint(texel, tint);
            }
            dst[px] = Blend(dst[px], ApplyDrawOpacity(texel));
        }
    }
}

void Canvas::DrawImageFilled(s32 x, s32 y, s32 rw, s32 rh, const Image& image, u32 tint,
                             float fraction, s32 sx, s32 sy, s32 sw, s32 sh) {
    if (!image.Valid() || rw <= 0 || rh <= 0) {
        return;
    }
    // Default to the whole picture; a package drawing from the game's own atlas passes the rect
    // of the one sprite it means.
    if (sw <= 0 || sh <= 0) {
        sx = 0;
        sy = 0;
        sw = static_cast<s32>(image.w);
        sh = static_cast<s32>(image.h);
    }
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    const s32 visible = static_cast<s32>(static_cast<float>(rh) * clamped);
    if (visible <= 0) {
        return;
    }
    // Keep the sprite's own scale and simply reveal it from the bottom up.
    const s32 top = y + rh - visible;
    for (s32 row = 0; row < visible; ++row) {
        const s32 py = top + row;
        if (py < clip_y0 || py >= clip_y1) {
            continue;
        }
        const s32 src_row = rh - visible + row;
        const u32 source_y = static_cast<u32>(sy + static_cast<s64>(src_row) * sh / rh);
        u32* const dst_row = pixels.data() + static_cast<size_t>(py) * w;
        const u32* const src = image.pixels.data() + static_cast<size_t>(source_y) * image.w;
        for (s32 col = 0; col < rw; ++col) {
            const s32 px = x + col;
            if (px < clip_x0 || px >= clip_x1) {
                continue;
            }
            const u32 source_x = static_cast<u32>(sx + static_cast<s64>(col) * sw / rw);
            u32 texel = src[source_x];
            if (tint != 0xFFFFFFFFu) {
                const u32 a = (((texel >> 24) & 0xFF) * ((tint >> 24) & 0xFF)) / 255;
                texel = (a << 24) | (texel & 0x00FFFFFF);
            }
            dst_row[px] = Blend(dst_row[px], ApplyDrawOpacity(texel));
        }
    }
}

} // namespace Core::Mods
