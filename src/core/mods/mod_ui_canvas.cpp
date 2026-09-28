// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Canvas basics and shape primitives: the pixel buffer (Resize), the clip (SetClip), Clear,
// FillRect, Pill, FrameRect, ClearRect, BlitRaw (the chrome cache's raw copy) and FillTriangle.
// Every call clips to [clip_x0, clip_x1) x [clip_y0, clip_y1). Clear, ClearRect and BlitRaw store
// pixels as given; the others blend with Blend (mod_ui_internal.h) at the canvas opacity
// (ApplyDrawOpacity, mod_ui_image.cpp).
// Not here: images (mod_ui_image.cpp), text (mod_ui_text.cpp).
// Threads: a Canvas has no lock; each thread draws into its own.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/mods/mod_ui.h"
#include "core/mods/mod_ui_internal.h"

namespace Core::Mods {

void Canvas::Resize(u32 width, u32 height) {
    if (width == w && height == h) {
        return;
    }
    w = width;
    h = height;
    pixels.assign(static_cast<size_t>(w) * h, 0xFF000000u);
    ResetClip();
}

void Canvas::SetClip(s32 x, s32 y, s32 cw, s32 ch) {
    if (cw <= 0 || ch <= 0) {
        ResetClip();
        return;
    }
    clip_x0 = std::clamp(x, 0, static_cast<s32>(w));
    clip_y0 = std::clamp(y, 0, static_cast<s32>(h));
    clip_x1 = std::clamp(x + cw, clip_x0, static_cast<s32>(w));
    clip_y1 = std::clamp(y + ch, clip_y0, static_cast<s32>(h));
}

void Canvas::Clear(u32 argb) {
    if (clip_x0 == 0 && clip_y0 == 0 && clip_x1 == static_cast<s32>(w) &&
        clip_y1 == static_cast<s32>(h)) {
        std::ranges::fill(pixels, argb);
        return;
    }
    ClearRect(clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0, argb);
}

void Canvas::FillRect(s32 x, s32 y, s32 rw, s32 rh, u32 argb) {
    if (rw <= 0 || rh <= 0) {
        return;
    }
    const s32 x0 = std::max(clip_x0, x);
    const s32 y0 = std::max(clip_y0, y);
    const s32 x1 = std::min<s32>(clip_x1, x + rw);
    const s32 y1 = std::min<s32>(clip_y1, y + rh);
    const u32 color = ApplyDrawOpacity(argb);
    if (x1 <= x0) {
        return;
    }
    if ((color >> 24) == 0xFF) {
        // Blend() returns an opaque source unchanged: a plain fill, without reading the row.
        for (s32 py = y0; py < y1; ++py) {
            std::fill_n(pixels.data() + static_cast<size_t>(py) * w + static_cast<size_t>(x0),
                        static_cast<size_t>(x1 - x0), color);
        }
        return;
    }
    for (s32 py = y0; py < y1; ++py) {
        u32* row = pixels.data() + static_cast<size_t>(py) * w;
        for (s32 px = x0; px < x1; ++px) {
            row[px] = Blend(row[px], color);
        }
    }
}

void Canvas::Pill(s32 x, s32 y, s32 rw, s32 rh, s32 t, u32 fill, u32 frame) {
    if (rw <= 0 || rh <= 0) {
        return;
    }
    // Signed distance to the capsule: the segment between the two cap centres, minus the radius.
    const float r = static_cast<float>(std::min(rw, rh)) * 0.5f;
    const float ax = static_cast<float>(x) + r, bx = static_cast<float>(x + rw) - r;
    const float ay = static_cast<float>(y) + r, by = static_cast<float>(y + rh) - r;
    const s32 x0 = std::max(clip_x0, x), y0 = std::max(clip_y0, y);
    const s32 x1 = std::min<s32>(clip_x1, x + rw);
    const s32 y1 = std::min<s32>(clip_y1, y + rh);
    const u32 faded_fill = ApplyDrawOpacity(fill);
    const u32 faded_frame = ApplyDrawOpacity(frame);
    for (s32 py = y0; py < y1; ++py) {
        u32* row = pixels.data() + static_cast<size_t>(py) * w;
        const float fy = static_cast<float>(py) + 0.5f;
        for (s32 px = x0; px < x1; ++px) {
            const float fx = static_cast<float>(px) + 0.5f;
            const float qx = std::clamp(fx, ax, bx), qy = std::clamp(fy, ay, by);
            const float d = std::sqrt((fx - qx) * (fx - qx) + (fy - qy) * (fy - qy)) - r;
            if (d >= 0.0f) {
                continue;
            }
            row[px] = Blend(row[px], d > -static_cast<float>(t) ? faded_frame : faded_fill);
        }
    }
}

void Canvas::FrameRect(s32 x, s32 y, s32 rw, s32 rh, s32 t, u32 argb) {
    FillRect(x, y, rw, t, argb);
    FillRect(x, y + rh - t, rw, t, argb);
    FillRect(x, y, t, rh, argb);
    FillRect(x + rw - t, y, t, rh, argb);
}

void Canvas::ClearRect(s32 x, s32 y, s32 rw, s32 rh, u32 argb) {
    const s32 x0 = std::max(clip_x0, x);
    const s32 y0 = std::max(clip_y0, y);
    const s32 x1 = std::min(clip_x1, x + rw);
    const s32 y1 = std::min(clip_y1, y + rh);
    if (x1 <= x0) {
        return;
    }
    for (s32 py = y0; py < y1; ++py) {
        std::fill_n(pixels.data() + static_cast<size_t>(py) * w + static_cast<size_t>(x0),
                    static_cast<size_t>(x1 - x0), argb);
    }
}

void Canvas::BlitRaw(s32 x, s32 y, s32 rw, s32 rh, const u32* src, s32 src_stride) {
    // Same clip-intersect shape as ClearRect just above -- a raw row copy, no Blend()/tint/opacity
    // math, because the caller (RenderPage's chrome cache) only ever hands this already-final,
    // already-blended pixels captured from a real draw at CurrentOpacity()==1, so re-blending here
    // would be redundant work, not a correctness requirement.
    if (rw <= 0 || rh <= 0 || src == nullptr || src_stride <= 0) {
        return;
    }
    const s32 x0 = std::max(clip_x0, x);
    const s32 y0 = std::max(clip_y0, y);
    const s32 x1 = std::min(clip_x1, x + rw);
    const s32 y1 = std::min(clip_y1, y + rh);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    for (s32 py = y0; py < y1; ++py) {
        const u32* src_row =
            src + static_cast<size_t>(py - y) * static_cast<size_t>(src_stride) + (x0 - x);
        u32* dst_row = pixels.data() + static_cast<size_t>(py) * w + static_cast<size_t>(x0);
        std::memcpy(dst_row, src_row, static_cast<size_t>(x1 - x0) * sizeof(u32));
    }
}

void Canvas::FillTriangle(s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2, u32 argb) {
    if (w == 0 || h == 0) {
        return;
    }
    const s32 min_y = std::max<s32>(clip_y0, std::min({y0, y1, y2}));
    const s32 max_y = std::min<s32>(clip_y1 - 1, std::max({y0, y1, y2}));
    const s32 min_x = std::max<s32>(clip_x0, std::min({x0, x1, x2}));
    const s32 max_x = std::min<s32>(clip_x1 - 1, std::max({x0, x1, x2}));
    if (min_y > max_y || min_x > max_x) {
        return;
    }
    // Edge functions; the sign convention is normalised so winding order does not matter.
    const s64 area = static_cast<s64>(x1 - x0) * (y2 - y0) - static_cast<s64>(y1 - y0) * (x2 - x0);
    if (area == 0) {
        return;
    }
    const s64 sign = area > 0 ? 1 : -1;
    const u32 color = ApplyDrawOpacity(argb);
    for (s32 py = min_y; py <= max_y; ++py) {
        u32* const row = pixels.data() + static_cast<size_t>(py) * w;
        for (s32 px = min_x; px <= max_x; ++px) {
            const s64 e0 =
                (static_cast<s64>(x1 - x0) * (py - y0) - static_cast<s64>(y1 - y0) * (px - x0)) *
                sign;
            const s64 e1 =
                (static_cast<s64>(x2 - x1) * (py - y1) - static_cast<s64>(y2 - y1) * (px - x1)) *
                sign;
            const s64 e2 =
                (static_cast<s64>(x0 - x2) * (py - y2) - static_cast<s64>(y0 - y2) * (px - x2)) *
                sign;
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                row[px] = Blend(row[px], color);
            }
        }
    }
}

} // namespace Core::Mods
