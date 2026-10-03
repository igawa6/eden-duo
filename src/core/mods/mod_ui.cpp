// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The second-screen page renderer: RenderPage and the widget draw functions it dispatches to.
//   - RenderPage: clears the canvas (fully transparent on the GPU path), expands the page's
//     widgets and the drag ghost (ExpandWidgetsInto, mod_ui_expand.cpp), then per widget: the
//     dirty-rect pre-reject on a partial redraw, the occlusion skip under a later opaque Map
//     widget, the visibility gates, the anim-group / scrolled-row clip and opacity, and the draw.
//     Selection highlights and scroll bars are painted after every widget, the ghost last.
//   - The draw functions, one per widget type, each taking a WidgetDrawContext
//     (mod_ui_internal.h): DrawImage, DrawRect, DrawLabel,
//     DrawValue, DrawBar, DrawPips, DrawButton. DrawMap is in mod_ui_map_widget.cpp.
//   - RenderDebugPage: the generated "__debug" page (every data point, its address and value).
// Not here: the Canvas calls (mod_ui_canvas.cpp, mod_ui_image.cpp, mod_ui_text.cpp), visibility
// and paint bounds (mod_ui_widget_state.cpp), deciding what to redraw and when (mod_redraw.cpp).
// Threads: RenderPage runs on the DSModRedraw worker and, for page transitions, the debug page and
// EDEN_DSMOD_SYNC_REDRAW, on the tick thread, at the same time. The process-wide state here (the
// widget profile) and in the map widget (label bitmaps) each has its own mutex;
// follow_state_mutex and manifest_markers_mutex come from the caller.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>

#include "common/logging.h"
#include "core/mods/mod_ui.h"
#include "core/mods/mod_ui_internal.h"

namespace Core::Mods {

namespace {
/// WidgetType::Image: a picture (by src, bound name, table, threshold or format), whole,
/// cropped, mirrored, turning, trembling or filled as a gauge.
void DrawImage(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Manifest& manifest = ctx.manifest;
    const StateSnapshot& snapshot = ctx.snapshot;
    const ImageProvider& images = ctx.images;
    const Widget& widget = ctx.widget;
    const ViewTransform& view = ctx.view;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    if (!images) {
        return;
    }
    // Images get the same linear transform as values, so an icon can be addressed as
    // "base id + upgrade level" straight out of two numbers the game keeps apart.
    s64 img_value = value;
    if (widget.mul != 1)
        img_value *= widget.mul;
    if (widget.div > 1)
        img_value /= widget.div;
    img_value += widget.add;
    if (!widget.empty_src.empty()) {
        if (const std::shared_ptr<const Image> under = images(widget.empty_src); under != nullptr) {
            const auto& r = widget.empty_rect;
            canvas.DrawImageRegion(
                x, y, rw, rh, *under, static_cast<s32>(r[0] * static_cast<float>(under->w)),
                static_cast<s32>(r[1] * static_cast<float>(under->h)),
                std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(under->w))),
                std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(under->h))),
                widget.color);
        }
    }
    // Which picture, by reference: copying the key per draw cost an allocation per Image widget.
    const std::string* source = &widget.src;
    std::string formatted; // src_format's result
    if (!widget.src_names.empty() && img_value >= 0 &&
        img_value < static_cast<s64>(widget.src_names.size())) {
        source = &widget.src_names[static_cast<size_t>(img_value)];
    }
    if (!widget.src_thresholds.empty()) {
        // Fatigue and similar bands: the first entry whose ceiling the value fits under
        // wins, so an irregular 0 / 1-49 / 50-79 / 80-100 split maps straight to four
        // faces.
        source = &widget.src_thresholds.back().second;
        for (const auto& [ceiling, path] : widget.src_thresholds) {
            if (value <= ceiling) {
                source = &path;
                break;
            }
        }
    }
    if (!widget.src_format.empty()) {
        // Build an icon path from a numeric id, e.g. "...#item_%03d_00.xtx". Only a single
        // integer conversion is allowed, so a stray %s/%n in the manifest cannot be abused.
        bool safe = true;
        int convs = 0;
        bool unsigned_conversion = false;
        for (size_t i = 0; i < widget.src_format.size(); ++i) {
            if (widget.src_format[i] != '%')
                continue;
            size_t j = i + 1;
            u32 numeric_field = 0;
            while (j < widget.src_format.size() &&
                   (std::isdigit(static_cast<unsigned char>(widget.src_format[j])) ||
                    widget.src_format[j] == '0' || widget.src_format[j] == '-' ||
                    widget.src_format[j] == '.')) {
                const char digit = widget.src_format[j];
                if (digit >= '0' && digit <= '9') {
                    numeric_field = numeric_field * 10 + static_cast<u32>(digit - '0');
                    if (numeric_field > 512) {
                        safe = false;
                        break;
                    }
                } else {
                    numeric_field = 0;
                }
                ++j;
            }
            if (!safe)
                break;
            const char c = j < widget.src_format.size() ? widget.src_format[j] : '\0';
            if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
                ++convs;
                unsigned_conversion = c == 'u' || c == 'x' || c == 'X';
            } else if (c == '%' && j == i + 1) {
                // literal %% is fine
            } else {
                safe = false;
                break;
            }
            i = j;
        }
        if (safe && convs == 1) {
            char sbuf[512];
            const int count = unsigned_conversion
                                  ? std::snprintf(sbuf, sizeof(sbuf), widget.src_format.c_str(),
                                                  static_cast<unsigned int>(img_value))
                                  : std::snprintf(sbuf, sizeof(sbuf), widget.src_format.c_str(),
                                                  static_cast<int>(img_value));
            if (count >= 0 && static_cast<size_t>(count) < sizeof(sbuf)) {
                formatted.assign(sbuf, static_cast<size_t>(count));
                source = &formatted;
            }
        }
        if (formatted.empty() && source->empty()) {
            return; // invalid/truncated format without a usable fallback source
        }
    }
    if (!widget.src_bind.empty()) {
        // The game names what it is showing; the package says which picture that is.
        const auto text = snapshot.texts.find(widget.src_bind);
        if (text == snapshot.texts.end()) {
            return;
        }
        const auto mapped = manifest.sprite_map.find(text->second);
        if (mapped == manifest.sprite_map.end()) {
            // Module-owned images use logical keys, validated by that module's decoder.
            if (!text->second.starts_with("module:")) {
                return;
            }
            source = &text->second;
        } else {
            source = &mapped->second;
        }
    }
    const std::shared_ptr<const Image> image = images(*source);
    // Pan and zoom on a picture is a narrower window onto it: at 2x we sample half the
    // source across the same rectangle. Panning is clamped so the content cannot be
    // dragged off its own frame and leave the user staring at nothing.
    std::array<float, 4> eff = widget.src_rect;
    if (widget.pan_zoom && widget.rect[2] > 0 && widget.rect[3] > 0) {
        const float zoom = std::max(view.zoom, 0.01f);
        const float span_u = (eff[2] - eff[0]) / zoom;
        const float span_v = (eff[3] - eff[1]) / zoom;
        const float max_pan_x = static_cast<float>(widget.rect[2]) * (1.0f - 1.0f / zoom);
        const float max_pan_y = static_cast<float>(widget.rect[3]) * (1.0f - 1.0f / zoom);
        const float px = std::clamp(view.pan_x, 0.0f, std::max(0.0f, max_pan_x));
        const float py = std::clamp(view.pan_y, 0.0f, std::max(0.0f, max_pan_y));
        const float u0 = eff[0] + (px / static_cast<float>(widget.rect[2])) * (eff[2] - eff[0]);
        const float v0 = eff[1] + (py / static_cast<float>(widget.rect[3])) * (eff[3] - eff[1]);
        eff = {u0, v0, u0 + span_u, v0 + span_v};
    }
    const auto region = [&](const Image& img, s32 tint) {
        const auto& r = eff;
        const s32 sx = static_cast<s32>(r[0] * static_cast<float>(img.w));
        const s32 sy = static_cast<s32>(r[1] * static_cast<float>(img.h));
        const s32 sw = std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(img.w)));
        const s32 sh = std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(img.h)));
        canvas.DrawImageRegion(x, y, rw, rh, img, sx, sy, sw, sh, static_cast<u32>(tint),
                               widget.flip_x, widget.flip_y);
    };
    if (image == nullptr) {
        return;
    }
    const bool whole = eff[0] == 0.0f && eff[1] == 0.0f && eff[2] == 1.0f && eff[3] == 1.0f;
    if (widget.shake != 0.0f) {
        // A trembling piece (the version warning's X): a fresh pseudo-random offset within
        // +-shake px every 3 ticks (20 Hz), hashed off the tick so it never settles into
        // a visible loop. The page's 15 Hz signature keeps it redrawn while visible.
        const u32 step = static_cast<u32>((snapshot.tick / 3) * 2654435761u);
        const s32 span = std::max(1, static_cast<s32>(std::lround(widget.shake)) * 2 + 1);
        const s32 amp = span / 2;
        x += static_cast<s32>((step >> 8) % static_cast<u32>(span)) - amp;
        y += static_cast<s32>((step >> 20) % static_cast<u32>(span)) - amp;
    }
    // Runtime 16: tint / tint_bind replace `color` as the picture's tint (the same value when
    // neither is set).
    const u32 tint = WidgetTint(widget, snapshot, widget.color);
    const ImageTransform xf = WidgetImageTransform(widget, rw, rh, &snapshot);
    const float spin_deg =
        widget.spin != 0.0f
            ? std::fmod(widget.spin * static_cast<float>(snapshot.tick % 216000) / 60.0f, 360.0f)
            : 0.0f;
    if (widget.fill_bind.empty() && (xf.active || widget.fill != ImageFill::Stretch)) {
        // Runtime 16: tile / 9-slice into a picture of the rect's size first, then rotate /
        // scale that picture as a whole (with any spin), or blit it.
        const auto& r = eff;
        const Image* pic = image.get();
        s32 sx = static_cast<s32>(r[0] * static_cast<float>(image->w));
        s32 sy = static_cast<s32>(r[1] * static_cast<float>(image->h));
        s32 sw = std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(image->w)));
        s32 sh = std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(image->h)));
        static thread_local Image composed;
        if (widget.fill != ImageFill::Stretch) {
            ComposeImageFill(composed, rw, rh, *image, sx, sy, sw, sh, widget.fill, widget.slice);
            pic = &composed;
            sx = sy = 0;
            sw = static_cast<s32>(composed.w);
            sh = static_cast<s32>(composed.h);
        }
        if (xf.active || widget.spin != 0.0f) {
            const float pivot_x = xf.active ? xf.pivot_x : static_cast<float>(rw) * 0.5f;
            const float pivot_y = xf.active ? xf.pivot_y : static_cast<float>(rh) * 0.5f;
            canvas.DrawImageTransformed(x, y, rw, rh, *pic, sx, sy, sw, sh, tint,
                                        xf.degrees + spin_deg, xf.scale, pivot_x, pivot_y,
                                        widget.flip_x, widget.flip_y);
        } else {
            canvas.DrawImageRegion(x, y, rw, rh, *pic, sx, sy, sw, sh, tint, widget.flip_x,
                                   widget.flip_y);
        }
    } else if (widget.fill_bind.empty()) {
        if (widget.spin != 0.0f) {
            // A turning piece (loading wheel): the angle advances with the tick, the
            // page's 15 Hz signature keeps it redrawn while the widget is visible.
            const float deg = spin_deg;
            const auto& r = eff;
            canvas.DrawImageRegionRotated(
                x, y, rw, rh, *image, static_cast<s32>(r[0] * static_cast<float>(image->w)),
                static_cast<s32>(r[1] * static_cast<float>(image->h)),
                std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(image->w))),
                std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(image->h))),
                tint, deg * 3.14159265f / 180.0f);
        } else if (whole && !widget.flip_x && !widget.flip_y) {
            canvas.DrawImage(x, y, rw, rh, *image, tint);
        } else {
            // A mirrored whole image takes the region path too (same sampling as
            // DrawImage), so flip_x/flip_y work with or without src_rect.
            region(*image, static_cast<s32>(tint));
        }
    } else {
        const s64 fill_value = snapshot.GetInt(widget.fill_bind);
        const s64 span = maximum > 0 ? maximum : 1;
        const auto& sr = widget.src_rect;
        canvas.DrawImageFilled(x, y, rw, rh, *image, tint,
                               static_cast<float>(fill_value) / static_cast<float>(span),
                               static_cast<s32>(sr[0] * static_cast<float>(image->w)),
                               static_cast<s32>(sr[1] * static_cast<float>(image->h)),
                               static_cast<s32>((sr[2] - sr[0]) * static_cast<float>(image->w)),
                               static_cast<s32>((sr[3] - sr[1]) * static_cast<float>(image->h)));
    }
}

/// WidgetType::Rect: a filled box with an optional (breathing) frame.
void DrawRect(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const StateSnapshot& snapshot = ctx.snapshot;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    u32 bg = widget.bg;
    u32 col = widget.color;
    if (widget.pulse) {
        // breathing: the fill's and the outline's alpha swing 20..100 % of their set
        // value over ~0.7 s (the page's 15 Hz signature keeps it redrawn while visible)
        const float a = BlinkAlpha(snapshot.tick, 40, 0.2f);
        const auto breathe = [&](u32 c) {
            const u32 amax = (c >> 24) & 0xFF;
            return (c & 0x00FFFFFFu) |
                   (static_cast<u32>(std::clamp(a * static_cast<float>(amax), 0.0f, 255.0f)) << 24);
        };
        bg = breathe(bg);
        col = breathe(col);
    }
    canvas.FillRect(x, y, rw, rh, bg);
    if (widget.color != 0) {
        canvas.FrameRect(x, y, rw, rh, std::max(1, widget.frame), col);
    }
}

/// WidgetType::Label: text picked by value, by key, from a string point or literal.
void DrawLabel(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const StateSnapshot& snapshot = ctx.snapshot;
    const TextProvider& texts = ctx.texts;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    // The text (LabelShownText). A key the runtime cannot answer yet draws nothing; the page is
    // redrawn when the text lands. msbt_owner keeps a resolved msbt string alive for as long as
    // `shown` (below) is still in use.
    std::shared_ptr<const std::string> msbt_owner;
    const std::string* const shown = LabelShownText(widget, snapshot, texts, msbt_owner);
    if (shown == nullptr || shown->empty()) {
        return;
    }
    s32 tx = x;
    s32 ty = y;
    if (widget.auto_box) {
        // Runtime 17 auto_w: the rect is the box around the text; its bg (a capsule with
        // `pill`) goes behind the text.
        if ((widget.bg >> 24) != 0) {
            if (widget.pill) {
                canvas.Pill(x, y, ctx.rw, ctx.rh, 0, widget.bg, widget.bg);
            } else {
                canvas.FillRect(x, y, ctx.rw, ctx.rh, widget.bg);
            }
        }
        const auto anchor = AutoBoxTextAnchor(widget, x, y, ctx.rw);
        tx = anchor[0];
        ty = anchor[1];
    }
    // An outline copy skips the colour tags but draws in its own colour only; icon_style only
    // decides how inline icons draw.
    canvas.SetIconSilhouette(widget.icon_silhouette);
    canvas.SetColorMarkup(widget.color_markup);
    canvas.SetOutlineCopy(widget.outline_copy);
    s32 scale = widget.text_scale;
    if (widget.fit_text && widget.wrap_width > 0 && shown->find('\n') == std::string::npos) {
        const s32 floor = std::clamp(widget.text_min_scale, 1, std::max(1, scale));
        if (scale > floor && canvas.MeasureText(*shown, scale) > widget.wrap_width) {
            // Width increases with integer scale. Find the largest fitting size with
            // logarithmic measurements instead of rescanning the string at every size.
            s32 low = floor, high = scale - 1;
            while (low < high) {
                const s32 mid = low + (high - low + 1) / 2;
                if (canvas.MeasureText(*shown, mid) <= widget.wrap_width)
                    low = mid;
                else
                    high = mid - 1;
            }
            scale = low;
        }
    }
    if (widget.text_center_h > 0) {
        const auto& lines = canvas.LayoutLines(*shown, scale, widget.wrap_width, widget.max_lines);
        const s32 gap = widget.line_gap >= 0 ? widget.line_gap : scale * 3;
        const s32 height =
            lines.empty() ? 0
                          : scale * 5 + (static_cast<s32>(lines.size()) - 1) * (scale * 5 + gap);
        ty += (widget.text_center_h - height) / 2;
    }
    if (widget.wrap_width > 0 || shown->find('\n') != std::string::npos) {
        canvas.DrawTextBlock(tx, ty, *shown, scale, widget.color, widget.align, widget.wrap_width,
                             widget.max_lines, widget.line_gap);
    } else {
        canvas.DrawTextAligned(tx, ty, *shown, scale, widget.color, widget.align);
    }
    canvas.SetOutlineCopy(false);
    canvas.SetColorMarkup(false);
    canvas.SetIconSilhouette(false);
}

/// WidgetType::Value: a bound number, transformed, named or padded, as text.
void DrawValue(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Manifest& manifest = ctx.manifest;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    char buf[128];
    // Linear transform on the raw read: max stamina = berries*15 + 150; stamina/fatigue
    // are stored at twice the shown value (div folds that out).
    s64 shown = value;
    if (widget.mul != 1)
        shown *= widget.mul;
    if (widget.div > 1)
        shown /= widget.div;
    shown += widget.add;
    if (!widget.table.empty()) {
        // Shared value->name list declared once in the manifest (item names, character
        // names).
        const auto tbl = manifest.tables.find(widget.table);
        const char* label = "";
        if (tbl != manifest.tables.end() && shown >= 0 &&
            shown < static_cast<s64>(tbl->second.size())) {
            label = tbl->second[static_cast<size_t>(shown)].c_str();
        }
        std::snprintf(buf, sizeof(buf), "%s%s", widget.text.c_str(), label);
    } else if (!widget.names.empty() && shown >= 0 &&
               shown < static_cast<s64>(widget.names.size())) {
        std::snprintf(buf, sizeof(buf), "%s%s", widget.text.c_str(),
                      widget.names[static_cast<size_t>(shown)].c_str());
    } else if (widget.group) {
        // Runtime 17: thousands separators ("12,345", "-1,234"), same precedence as below: a
        // zero-padded number, else "<value><max_sep><max>", else the value alone.
        std::string grouped = widget.text;
        grouped += FormatGroupedNumber(shown, widget.pad, widget.group_sep);
        if (widget.pad <= 0 && !widget.max_sep.empty() && !widget.max_bind.empty()) {
            grouped += widget.max_sep;
            grouped += FormatGroupedNumber(maximum, 0, widget.group_sep);
        }
        std::snprintf(buf, sizeof(buf), "%s", grouped.c_str());
    } else if (widget.pad > 0) {
        std::snprintf(buf, sizeof(buf), "%s%0*lld", widget.text.c_str(), widget.pad,
                      static_cast<long long>(shown));
    } else if (!widget.max_sep.empty() && !widget.max_bind.empty()) {
        // "184/199" as one run of text, so the slash hugs the digits whatever their count
        std::snprintf(buf, sizeof(buf), "%s%lld%s%lld", widget.text.c_str(),
                      static_cast<long long>(shown), widget.max_sep.c_str(),
                      static_cast<long long>(maximum));
    } else {
        std::snprintf(buf, sizeof(buf), "%s%lld", widget.text.c_str(),
                      static_cast<long long>(shown));
    }
    // Colour tags as on a label. A value's icon_style applies only with color_markup, as before
    // markup existed (a plain value never read it).
    canvas.SetColorMarkup(widget.color_markup);
    canvas.SetOutlineCopy(widget.outline_copy);
    canvas.SetIconSilhouette(widget.color_markup && widget.icon_silhouette);
    if (widget.suffix.empty()) {
        canvas.DrawTextAligned(x, y, buf, widget.text_scale, widget.color, widget.align);
    } else {
        canvas.DrawTextAligned(x, y, std::string{buf} + widget.suffix, widget.text_scale,
                               widget.color, widget.align);
    }
    canvas.SetIconSilhouette(false);
    canvas.SetOutlineCopy(false);
    canvas.SetColorMarkup(false);
}

/// WidgetType::Bar: a gauge of value / maximum, filling rightwards (or, runtime 16, from
/// `fill_dir`, and with a picture as the filled part).
void DrawBar(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    canvas.FillRect(x, y, rw, rh, widget.bg);
    const s64 span = maximum > 0 ? maximum : 1;
    const s64 clamped = std::clamp<s64>(value, 0, span);
    if (widget.fill_dir == BarFillDir::Right && widget.fill_image.empty()) {
        const s32 filled = static_cast<s32>(static_cast<s64>(rw) * clamped / span);
        canvas.FillRect(x, y, filled, rh, widget.color);
        canvas.FrameRect(x, y, rw, rh, widget.frame, widget.color);
        return;
    }
    // Runtime 16: the filled part, from the side `fill_dir` names.
    const bool vertical = widget.fill_dir == BarFillDir::Up || widget.fill_dir == BarFillDir::Down;
    const s32 filled =
        static_cast<s32>(static_cast<s64>(vertical ? rh : rw) * clamped / span);
    std::array<s32, 4> part{x, y, filled, rh};
    switch (widget.fill_dir) {
    case BarFillDir::Left:
        part = {x + rw - filled, y, filled, rh};
        break;
    case BarFillDir::Up:
        part = {x, y + rh - filled, rw, filled};
        break;
    case BarFillDir::Down:
        part = {x, y, rw, filled};
        break;
    case BarFillDir::Right:
    default:
        break;
    }
    std::shared_ptr<const Image> image;
    if (!widget.fill_image.empty() && ctx.images) {
        image = ctx.images(widget.fill_image);
    }
    if (image == nullptr || !image->Valid()) {
        canvas.FillRect(part[0], part[1], part[2], part[3], widget.color);
    } else if (part[2] > 0 && part[3] > 0) {
        // The picture spans the whole bar and the fill reveals it (stretch / tile); a 9-slice is
        // laid out on the filled part itself, so its end caps travel with the value.
        const auto& r = widget.src_rect;
        const s32 sx = static_cast<s32>(r[0] * static_cast<float>(image->w));
        const s32 sy = static_cast<s32>(r[1] * static_cast<float>(image->h));
        const s32 sw = std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(image->w)));
        const s32 sh = std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(image->h)));
        const std::array<s32, 4> box =
            widget.fill == ImageFill::Slice ? part : std::array<s32, 4>{x, y, rw, rh};
        const auto clip = canvas.Clip(); // {x, y, w, h}
        const s32 cx0 = std::max(clip[0], part[0]), cy0 = std::max(clip[1], part[1]);
        const s32 cx1 = std::min(clip[0] + clip[2], part[0] + part[2]);
        const s32 cy1 = std::min(clip[1] + clip[3], part[1] + part[3]);
        if (cx1 > cx0 && cy1 > cy0) {
            canvas.SetClip(cx0, cy0, cx1 - cx0, cy1 - cy0);
            const u32 tint = WidgetTint(widget, ctx.snapshot, 0xFFFFFFFFu);
            if (widget.fill == ImageFill::Stretch) {
                canvas.DrawImageRegion(box[0], box[1], box[2], box[3], *image, sx, sy, sw, sh,
                                       tint, widget.flip_x, widget.flip_y);
            } else {
                static thread_local Image composed;
                ComposeImageFill(composed, box[2], box[3], *image, sx, sy, sw, sh, widget.fill,
                                 widget.slice);
                canvas.DrawImageRegion(box[0], box[1], box[2], box[3], composed, 0, 0,
                                       static_cast<s32>(composed.w),
                                       static_cast<s32>(composed.h), tint, widget.flip_x,
                                       widget.flip_y);
            }
            canvas.SetClip(clip[0], clip[1], clip[2], clip[3]);
        }
    }
    canvas.FrameRect(x, y, rw, rh, widget.frame, widget.color);
}

/// WidgetType::Pips: a run of sprites or squares, `value` of them lit.
void DrawPips(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const ImageProvider& images = ctx.images;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rh = ctx.rh;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    // Runtime 16: tint / tint_bind replace `color` as the lit pips' colour.
    const u32 lit = WidgetTint(widget, ctx.snapshot, widget.color);
    if (!widget.src.empty() && images) {
        if (const std::shared_ptr<const Image> pip = images(widget.src); pip != nullptr) {
            // Values are garbage until the game's own state exists, and a run of pips is
            // drawn one sprite at a time: without a bound, a nonsense maximum on the
            // first frames asks for millions of draws and takes the app down.
            const s64 total = std::clamp<s64>(maximum > 0 ? maximum : value, 0, MaxPips);
            const s32 step = widget.rect[2] + (widget.gap >= 0 ? widget.gap : 8);
            for (s64 i = 0; i < total; ++i) {
                const s32 px = x + static_cast<s32>(i) * step;
                const u32 tint = i < value ? lit : widget.bg;
                canvas.DrawImage(px, y, widget.rect[2], widget.rect[3], *pip, tint);
            }
            return;
        }
    }
    const s64 total = maximum > 0 ? maximum : value;
    const s32 count = static_cast<s32>(std::clamp<s64>(total, 0, MaxPips));
    const s32 pip = std::max(4, rh);
    for (s32 i = 0; i < count; ++i) {
        const s32 px = x + i * (pip + (widget.gap >= 0 ? widget.gap : pip / 3));
        const bool on = i < value;
        canvas.FillRect(px, y, pip, pip, on ? lit : widget.bg);
        canvas.FrameRect(px, y, pip, pip, 1, widget.color);
    }
}

/// WidgetType::Chart (runtime 17): the bound value's recent samples (StateSnapshot::charts) as a
/// line or bars over `bg`, newest at the right edge, scaled to min / max (each the samples' own
/// extreme when not given). Drawn inside the widget's rect only.
void DrawChart(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Widget& widget = ctx.widget;
    const s32 x = ctx.x, y = ctx.y, rw = ctx.rw, rh = ctx.rh;
    if (rw <= 0 || rh <= 0) {
        return;
    }
    canvas.FillRect(x, y, rw, rh, widget.bg);
    if (!widget.chart || !ctx.snapshot.charts) {
        return;
    }
    const ChartSpec& spec = *widget.chart;
    const auto found = ctx.snapshot.charts->find(spec.key);
    if (found == ctx.snapshot.charts->end() || found->second.values.empty()) {
        return;
    }
    const std::vector<f32>& values = found->second.values;
    const s64 capacity = std::max<s64>(2, spec.samples);
    const s64 n = std::min<s64>(static_cast<s64>(values.size()), capacity);
    const size_t first = values.size() - static_cast<size_t>(n);
    f64 lo = spec.min, hi = spec.max;
    if (!spec.has_min || !spec.has_max) {
        const auto [mn, mx] = std::minmax_element(values.begin() + static_cast<std::ptrdiff_t>(first),
                                                  values.end());
        if (!spec.has_min) {
            lo = *mn;
        }
        if (!spec.has_max) {
            hi = *mx;
        }
    }
    if (!(hi > lo)) {
        // A flat series (or min >= max): centre it in a range of 2.
        const f64 mid = spec.has_min && !spec.has_max ? lo + 1.0
                        : !spec.has_min && spec.has_max ? hi - 1.0
                                                        : (lo + hi) * 0.5;
        lo = mid - 1.0;
        hi = mid + 1.0;
    }
    const auto level = [&](f32 v) { // 0..1 of the range
        return std::clamp((static_cast<f64>(v) - lo) / (hi - lo), 0.0, 1.0);
    };
    const auto clip = canvas.Clip();
    const s32 cx0 = std::max(clip[0], x), cy0 = std::max(clip[1], y);
    const s32 cx1 = std::min(clip[0] + clip[2], x + rw), cy1 = std::min(clip[1] + clip[3], y + rh);
    if (cx1 <= cx0 || cy1 <= cy0) {
        return;
    }
    canvas.SetClip(cx0, cy0, cx1 - cx0, cy1 - cy0);
    const s64 slot0 = capacity - n; // the oldest shown sample's slot; the newest is capacity - 1
    if (spec.style == ChartSpec::Style::Bar) {
        for (s64 i = 0; i < n; ++i) {
            const s64 slot = slot0 + i;
            const s32 bx0 = x + static_cast<s32>(slot * rw / capacity);
            const s32 bx1 = x + static_cast<s32>((slot + 1) * rw / capacity);
            const s32 bw = bx1 - bx0 >= 3 ? bx1 - bx0 - 1 : bx1 - bx0; // a 1 px gap when room
            const s32 bh = static_cast<s32>(
                std::lround(level(values[first + static_cast<size_t>(i)]) * static_cast<f64>(rh)));
            canvas.FillRect(bx0, y + rh - bh, std::max(1, bw), bh, widget.color);
        }
    } else {
        // 2 px line through the samples' points (Bresenham steps of 2x2 dots).
        const auto point = [&](s64 i) {
            const s64 slot = slot0 + i;
            const s32 px = x + static_cast<s32>(slot * (rw - 1) / (capacity - 1));
            const s32 py = y + (rh - 1) -
                           static_cast<s32>(std::lround(
                               level(values[first + static_cast<size_t>(i)]) * (rh - 1)));
            return std::pair{px, py};
        };
        auto [px, py] = point(0);
        canvas.FillRect(px, py, 2, 2, widget.color);
        for (s64 i = 1; i < n; ++i) {
            const auto [qx, qy] = point(i);
            const s32 dx = std::abs(qx - px), dy = -std::abs(qy - py);
            const s32 step_x = px < qx ? 1 : -1, step_y = py < qy ? 1 : -1;
            s32 err = dx + dy;
            while (true) {
                canvas.FillRect(px, py, 2, 2, widget.color);
                if (px == qx && py == qy) {
                    break;
                }
                const s32 e2 = 2 * err;
                if (e2 >= dy) {
                    err += dy;
                    px += step_x;
                }
                if (e2 <= dx) {
                    err += dx;
                    py += step_y;
                }
            }
        }
    }
    canvas.SetClip(clip[0], clip[1], clip[2], clip[3]);
}

/// WidgetType::Button: a boxed or pill-shaped caption.
void DrawButton(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    const s32 border = widget.border;
    if (widget.pill) {
        canvas.Pill(x, y, rw, rh, border, widget.bg, widget.color);
    } else {
        canvas.FillRect(x, y, rw, rh, widget.bg);
        canvas.FrameRect(x, y, rw, rh, border, widget.color);
    }
    if (widget.align == 1) {
        canvas.DrawTextAligned(x + rw / 2, y + (rh - widget.text_scale * 5) / 2, widget.text,
                               widget.text_scale, widget.color, 1);
    } else {
        canvas.DrawText(x + widget.text_inset, y + widget.text_inset, widget.text,
                        widget.text_scale, widget.color);
    }
}

thread_local u64 render_serial_now = 0;
std::atomic<u64> render_serial_next{0};

} // namespace

u64 CurrentRenderSerial() {
    return render_serial_now;
}

bool RenderPage(Canvas& canvas, const Manifest& manifest, const Page& page,
                const StateSnapshot& snapshot, const ImageProvider& images, const ViewState& views,
                MapFollowState* follow_state, const VisitReporter& report_visit,
                const VisitedQuery& is_visited, AuxDrawList* draw_list, bool* animating,
                const TextProvider& texts, MapDrawRecords* map_records, const RenderExtras* extras,
                std::mutex* follow_state_mutex, std::recursive_mutex* manifest_markers_mutex) {
    // A genuinely narrowed partial redraw (extras != nullptr with a real, non-whole-canvas clip --
    // the same test the per-widget bbox pre-reject below uses) must NOT blow away every Map
    // widget's tap-hit-testing record just because this pass didn't happen to touch the map. A
    // partial pass triggered by something with no spatial overlap with the map (a panel sliding in,
    // a HUD icon, anything outside the Map widget's own rect) `continue`s past the Map widget at
    // the bbox pre-reject a few hundred lines down, without ever reaching the `map_records->
    // emplace_back()` call that would re-add its entry. Clearing unconditionally would leave
    // `*map_records` (and, one publish later, `map_draw_records_published`) with ZERO records for a
    // map that is still fully visible and unchanged on screen, so the map could not be tapped. This
    // holds on both the redraw worker and EDEN_DSMOD_SYNC_REDRAW=1; it is not a threading matter
    // but the interaction between the bbox pre-reject and a clear-every-call policy. A full redraw
    // (extras == nullptr, or its clip is the {0,0,0,0} "whole canvas" sentinel -- Canvas::SetClip's
    // own contract, same test the bbox pre-reject uses) still clears and rebuilds from scratch
    // exactly as before: every widget is visited that pass, so nothing is lost by starting empty.
    // On a real partial pass, entries simply survive unless something below explicitly touches them
    // -- see the matching `erase_if` in the Map widget's BeginRecord, mod_ui_map_widget.cpp (a
    // widget that DOES redraw this
    // pass refreshes its own entry, never leaving a stale duplicate) and at the widget-hidden
    // `continue` just below (a Map widget that just became hidden -- reachable here on a partial
    // pass only when its own dependency hash changed, i.e. it IS inside this pass's dirty rect --
    // must not leave a now-invalid record a tap could still hit).
    render_serial_now = render_serial_next.fetch_add(1, std::memory_order_relaxed) + 1;
    // Runtime 17 auto_w: the expansions below size text boxes on this canvas, as drawn.
    const TextMeasureScope measure{canvas, &texts};
    const bool whole_canvas_draw =
        extras == nullptr || extras->clip[2] <= 0 || extras->clip[3] <= 0;
    if (map_records != nullptr && whole_canvas_draw) {
        map_records->clear();
    }
    if (canvas.Width() == 0 || canvas.Height() == 0) {
        return false;
    }
    // An animation frame redraws only its dirty box; everything else keeps last frame's pixels.
    if (extras != nullptr) {
        canvas.SetClip(extras->clip[0], extras->clip[1], extras->clip[2], extras->clip[3]);
    } else {
        canvas.ResetClip();
    }
    canvas.SetLayerOpacity(1.0f);
    const std::array<s32, 4> frame_clip = canvas.Clip();
    bool settling = false; // a follow-view glide is still in flight: caller must redraw again
    bool clear_pending = false;
    if (draw_list != nullptr) {
        // GPU composite: the canvas is only the HUD overlay (map/icons/marker are emitted as
        // quads). Clear it fully transparent each frame so the composited map shows through where
        // no HUD sits; the map widget emits its own bounded background fill before its textures.
        draw_list->bg = manifest.background;
        draw_list->canvas_w = canvas.Width();
        draw_list->canvas_h = canvas.Height();
        canvas.Clear(0x00000000u);
    } else {
        // Widgets repaint only their own pixels (text draws glyph strokes, not boxes), and a
        // widget that hides between redraws leaves its old pixels behind, so every redraw starts
        // from the background. A full-canvas fill is ~0.3 ms; the stale-pixel bugs it prevents
        // (old digits under new ones, a hidden picture peeking out around the panels) are not
        // worth saving it.
        // Deferred to just before the widget loop, where it can leave out what an opaque Map
        // widget repaints anyway (see static_occluders).
        clear_pending = true;
    }

    bool any_repeat = false;
    for (const auto& rw_ : page.widgets) {
        if (rw_.repeat > 0 || !rw_.x_bind.empty() || !rw_.y_bind.empty() || rw_.auto_w) {
            any_repeat = true;
            break;
        }
    }
    // The expanded widgets live in this thread's reusable slots (copy-assigned each call, so their
    // strings keep their storage): expanding a large page no longer allocates per widget.
    static thread_local std::vector<Widget> expanded_storage;
    size_t expanded_count = 0;
    // A drag in flight appends its ghost -- a scaled copy of the dragged widget pinned under the
    // finger -- so it is drawn last, over everything, by the same code as the widget itself.
    const Widget* const ghost_source =
        snapshot.drag.active && snapshot.drag.widget.type != WidgetType::Map &&
                snapshot.drag.widget.rect[2] > 0 && snapshot.drag.widget.rect[3] > 0
            ? &snapshot.drag.widget
            : nullptr;
    size_t ghost_index = std::numeric_limits<size_t>::max();
    if (ghost_source != nullptr) {
        if (any_repeat) {
            expanded_count = ExpandWidgetsInto(page, snapshot, expanded_storage);
        } else {
            if (expanded_storage.size() < page.widgets.size()) {
                expanded_storage.resize(page.widgets.size());
            }
            std::copy(page.widgets.begin(), page.widgets.end(), expanded_storage.begin());
            expanded_count = page.widgets.size();
        }
        Widget ghost = *ghost_source;
        const float s = std::clamp(ghost.drag_scale, 0.25f, 4.0f);
        const auto scaled = [s](s32 v) {
            return static_cast<s32>(std::lround(static_cast<float>(v) * s));
        };
        ghost.rect = {snapshot.drag.x - scaled(snapshot.drag.grab_dx),
                      snapshot.drag.y - scaled(snapshot.drag.grab_dy), scaled(ghost.rect[2]),
                      scaled(ghost.rect[3])};
        ghost.text_scale = std::max(1, scaled(ghost.text_scale));
        ghost.hide_bind.clear();
        ghost.need_bind.clear();
        ghost.anim.reset();
        ghost.pan_zoom = false;
        ghost.repeat = 0;
        ghost_index = expanded_count;
        if (expanded_count < expanded_storage.size()) {
            expanded_storage[expanded_count] = std::move(ghost);
        } else {
            expanded_storage.push_back(std::move(ghost));
        }
        ++expanded_count;
    }
    // The widgets to draw, by reference: a page widget itself where the expansion would only copy
    // it (no repeat, no x_bind/y_bind -- most of them), else its expanded copy. Copying every
    // Widget of a ~900-widget page per pass because a few of them move cost ~0.35 ms.
    static thread_local std::vector<const Widget*> expanded_refs;
    expanded_refs.clear();
    if (ghost_source != nullptr) {
        for (size_t i = 0; i < expanded_count; ++i) {
            expanded_refs.push_back(&expanded_storage[i]);
        }
    } else if (any_repeat) {
        ExpandWidgetRefsInto(page, snapshot, expanded_storage, expanded_refs);
    } else {
        for (const Widget& w : page.widgets) {
            expanded_refs.push_back(&w);
        }
    }
    const std::span<const Widget* const> expanded{expanded_refs};

    // Selection / drop-hover highlights are painted after every widget (so an icon drawn over a
    // cell cannot hide its cell's frame) and before the ghost.
    struct HighlightAt {
        std::string src;
        u32 color;
        s32 frame;
        s32 x, y, w, h;
        std::array<s32, 4> clip; ///< the widget's clip (an animated group's box)
        float alpha;
    };
    std::vector<HighlightAt> highlights;
    // Runtime 17: the controller focus frame ("@nav.*", ModRuntime::UpdateNav), painted like a
    // selection highlight with the manifest's "nav" style.
    if (snapshot.GetInt("@nav.active") != 0 && snapshot.GetInt("@nav.w") > 0 &&
        snapshot.GetInt("@nav.h") > 0) {
        highlights.push_back({manifest.nav.src,
                              manifest.nav.color,
                              manifest.nav.frame, static_cast<s32>(snapshot.GetInt("@nav.x")),
                              static_cast<s32>(snapshot.GetInt("@nav.y")),
                              static_cast<s32>(snapshot.GetInt("@nav.w")),
                              static_cast<s32>(snapshot.GetInt("@nav.h")), frame_clip, 1.0f});
    }
    const auto draw_highlights = [&] {
        for (const auto& hl : highlights) {
            canvas.SetClip(hl.clip[0], hl.clip[1], hl.clip[2], hl.clip[3]);
            canvas.SetLayerOpacity(hl.alpha);
            bool drawn = false;
            if (!hl.src.empty() && images) {
                if (const std::shared_ptr<const Image> img = images(hl.src); img != nullptr) {
                    canvas.DrawImage(hl.x, hl.y, hl.w, hl.h, *img, 0xFFFFFFFFu);
                    drawn = true;
                }
            }
            if (!drawn && hl.color != 0) {
                if (hl.frame <= 0) {
                    canvas.FillRect(hl.x, hl.y, hl.w, hl.h, hl.color);
                } else {
                    canvas.FrameRect(hl.x, hl.y, hl.w, hl.h, hl.frame, hl.color);
                }
            }
        }
        canvas.SetClip(frame_clip[0], frame_clip[1], frame_clip[2], frame_clip[3]);
        canvas.SetLayerOpacity(1.0f);
        highlights.clear();
    };

    // Scrollbars of the page's drag-to-scroll regions: over the rows, under highlights and the
    // drag ghost. Drawn only while the runtime says the region is live ("@scroll_on:<id>").
    bool scroll_bars_drawn = page.scrolls.empty();
    const auto draw_scroll_bars = [&] {
        if (scroll_bars_drawn) {
            return;
        }
        scroll_bars_drawn = true;
        canvas.SetClip(frame_clip[0], frame_clip[1], frame_clip[2], frame_clip[3]);
        canvas.SetLayerOpacity(1.0f);
        canvas.SetDrawOpacity(1.0f);
        for (const auto& region : page.scrolls) {
            if ((region.bar_color == 0 && region.bar_src.empty() && region.bar_track_src.empty()) ||
                snapshot.GetInt("@scroll_on:" + region.id) == 0) {
                continue;
            }
            const ScrollMetrics m = MeasureScroll(page, region, snapshot);
            const auto thumb = ScrollBarThumb(region, m, ScrollOffset(region, snapshot));
            if (thumb[2] <= 0 || thumb[3] <= 0) {
                continue;
            }
            if (region.bar_track != 0) {
                canvas.FillRect(thumb[0], region.rect[1], thumb[2], region.rect[3],
                                region.bar_track);
            }
            if (!region.bar_track_src.empty()) {
                const auto track = images ? images(region.bar_track_src) : nullptr;
                if (track && track->Valid())
                    canvas.DrawImage(thumb[0], region.rect[1], thumb[2], region.rect[3], *track,
                                     0xFFFFFFFFu);
            }
            const auto art = !region.bar_src.empty() && images ? images(region.bar_src) : nullptr;
            if (art && art->Valid())
                canvas.DrawImage(thumb[0], thumb[1], thumb[2], thumb[3], *art, 0xFFFFFFFFu);
            else
                canvas.FillRect(thumb[0], thumb[1], thumb[2], thumb[3], region.bar_color);
        }
    };

    // EDEN_DSMOD_PROFILE_WIDGETS: per-widget draw time, the slowest ones logged every 5 s.
    static const bool profile_widgets =
        VideoCore::DSMod::DsmodEnvFlag("EDEN_DSMOD_PROFILE_WIDGETS", false);
    struct WidgetCost {
        double ms{};
        u64 calls{};
        std::string what;
    };
    // widget_costs/profile_window are opt-in (EDEN_DSMOD_PROFILE_WIDGETS), and RenderPage runs on
    // two threads (the redraw worker and the tick thread's synchronous carve-out) concurrently --
    // unsynchronised, that would be an unordered_map mutation race (real corruption risk, not just
    // a stale read) plus a plain-scalar race on profile_window. One mutex for both: they are only
    // ever touched together (every read/write site below takes it for its whole critical section),
    // and this is debug instrumentation, not the hot path -- correctness over fine-grained cost
    // here.
    static std::unordered_map<std::string, WidgetCost> widget_costs;
    static auto profile_window = std::chrono::steady_clock::now();
    static std::mutex profile_mutex;
    // Occlusion skip: skip widgets fully repainted by a later, unconditionally-opaque Map widget.
    // Measured (EDEN_DSMOD_PROFILE_WIDGETS=1, real outdoor overworld walk): LA's map page's "bg"
    // (full-canvas Image) and "map_sheet" (paper Image) widgets -- both drawn BEFORE the Map widget
    // in z-order, both with no bind of their own -- cost ~0.85-1.0ms EACH, ~35% of the ~5.0-5.9ms
    // partial-redraw average, on every ordinary walking tick. They are redrawn only because their
    // static rects intersect the tick's dirty union (the dirty-region redraw's "everything
    // intersecting participates" correctness rule) -- which is almost exactly the Map widget's own
    // rect while walking -- even though the Map widget's draw (DrawMap, mod_ui_map_widget.cpp)
    // unconditionally does `canvas.FillRect(x, y, rw, rh, widget.bg)` as the very FIRST thing it
    // does, before anything else, regardless of area/follow/zoom state. When widget.bg is fully
    // opaque (alpha byte == 0xFF) that FillRect is a complete, unconditional overwrite of every
    // pixel in its declared rect -- Blend() (mod_ui_internal.h): `if (a == 0xFF) return src;`,
    // no dependency whatsoever on what was drawn there before. So any earlier (lower z-order)
    // widget whose own rect is entirely inside an eligible Map widget's rect can never be visible
    // this tick and does not need to draw.
    //
    // Eligibility (all checked below, static_occluders built once per RenderPage call): the Map
    // widget itself must not be hidden this tick (WidgetHidden(), the same pure predicate the
    // dirty-region scan already relies on); its widget.bg alpha must be exactly 0xFF;
    // manifest.map_style.opacity must be exactly 1.0 (else the Map's own draw is itself only
    // partially opaque -- see "translucent_map" below); and it must not
    // carry a widget.anim (a moving/fading group member), which is excluded outright so this never
    // has to reason about anim_clip/widget_alpha for the occluder side -- LA's map_overworld is not
    // one, so this excludes nothing in practice. A Map widget's own declared rect is never
    // x_bind/y_bind-repositioned (the map marker moves via marker_x_bind/marker_y_bind, consumed
    // entirely inside DrawMap, never the widget's own rect), so using widget.rect
    // verbatim here is correct without replicating ExpandWidgets.
    //
    // Scope: draw_list != nullptr (EDEN_DSMOD_GPU_COMPOSITE) is excluded entirely -- LA's shipped
    // package never sets that flag and the Map widget's draw-list branch behaves differently there
    // (queues quads instead of a canvas fill); this change does not enter that, unverified,
    // territory. Applies to both a clipped (partial) and an unclipped (full) RenderPage call alike
    // -- frame_clip already equals the whole canvas on a full draw (Canvas::SetClip's own contract,
    // matching the existing bbox pre-reject below's use of the same property) -- so no
    // extras/partial-specific branch is needed; the correctness argument above does not depend on
    // why RenderPage is being called.
    std::vector<std::pair<size_t, std::array<s32, 4>>> static_occluders;
    if (draw_list == nullptr) {
        for (size_t oi = 0; oi < expanded.size(); ++oi) {
            const Widget& ow = *expanded[oi];
            if (ow.type != WidgetType::Map || ow.anim || manifest.map_style.opacity < 1.0f ||
                ((ow.bg >> 24) & 0xFFu) != 0xFFu) {
                continue;
            }
            const s32 ox0 = std::max(frame_clip[0], ow.rect[0]);
            const s32 oy0 = std::max(frame_clip[1], ow.rect[1]);
            const s32 ox1 = std::min(frame_clip[0] + frame_clip[2], ow.rect[0] + ow.rect[2]);
            const s32 oy1 = std::min(frame_clip[1] + frame_clip[3], ow.rect[1] + ow.rect[3]);
            if (ox1 <= ox0 || oy1 <= oy0 || WidgetHidden(ow, snapshot)) {
                continue; // not actually painting anything (this tick) that could occlude
            }
            static_occluders.emplace_back(oi, std::array<s32, 4>{ox0, oy0, ox1 - ox0, oy1 - oy0});
        }
    }
    if (clear_pending) {
        // The background fill under the first occluder is overwritten, opaque, by that Map
        // widget's own background before anything reads it (every canvas operation reads only the
        // pixel it writes, and a widget skipped or blended there is repainted the same way), so
        // only the rest of the clip is cleared.
        const auto& occ = static_occluders.empty() ? std::array<s32, 4>{} : static_occluders.front().second;
        if (static_occluders.empty() ||
            expanded[static_occluders.front().first]->scroll_clip[2] > 0) {
            canvas.Clear(manifest.background);
        } else {
            const s32 cx0 = frame_clip[0], cy0 = frame_clip[1];
            const s32 cx1 = frame_clip[0] + frame_clip[2], cy1 = frame_clip[1] + frame_clip[3];
            const s32 ox0 = occ[0], oy0 = occ[1], ox1 = occ[0] + occ[2], oy1 = occ[1] + occ[3];
            canvas.ClearRect(cx0, cy0, cx1 - cx0, oy0 - cy0, manifest.background); // above
            canvas.ClearRect(cx0, oy1, cx1 - cx0, cy1 - oy1, manifest.background); // below
            canvas.ClearRect(cx0, oy0, ox0 - cx0, oy1 - oy0, manifest.background); // left
            canvas.ClearRect(ox1, oy0, cx1 - ox1, oy1 - oy0, manifest.background); // right
        }
    }
    for (size_t widget_index = 0; widget_index < expanded.size(); ++widget_index) {
        const Widget& widget = *expanded[widget_index];
        const RenderAnimGroup* anim_group = nullptr;
        // A widget of a sliding group is drawn at this frame's offset: added to its resolved
        // position below (the draw functions position everything from ctx.x / ctx.y), not by
        // copying the Widget.
        s32 move_dx = 0, move_dy = 0;
        if (extras != nullptr && widget.anim) {
            anim_group = extras->Find(widget.anim->key);
            if (anim_group != nullptr && anim_group->moving) {
                move_dx = anim_group->dx;
                move_dy = anim_group->dy;
            }
        }
        const bool is_ghost = widget_index == ghost_index;
        if (is_ghost) {
            draw_scroll_bars();
            draw_highlights();
            if (!widget.drag_under_src.empty() && images) {
                // The underlay (a cell's disc) travels with the ghost: placed relative to the
                // widget's own rect, scaled about the grab point like the ghost itself.
                if (const std::shared_ptr<const Image> under = images(widget.drag_under_src);
                    under != nullptr) {
                    const float s = std::clamp(ghost_source->drag_scale, 0.25f, 4.0f);
                    const auto scaled = [s](s32 v) {
                        return static_cast<s32>(std::lround(static_cast<float>(v) * s));
                    };
                    const auto& ur = widget.drag_under_rect;
                    const s32 uw = ur[2] > 0 ? ur[2] : ghost_source->rect[2];
                    const s32 uh = ur[3] > 0 ? ur[3] : ghost_source->rect[3];
                    const auto& sr = widget.drag_under_src_rect;
                    const s32 sx = static_cast<s32>(sr[0] * static_cast<float>(under->w));
                    const s32 sy = static_cast<s32>(sr[1] * static_cast<float>(under->h));
                    const s32 sw = std::max(
                        1, static_cast<s32>((sr[2] - sr[0]) * static_cast<float>(under->w)));
                    const s32 sh = std::max(
                        1, static_cast<s32>((sr[3] - sr[1]) * static_cast<float>(under->h)));
                    canvas.SetDrawOpacity(0.8f);
                    if (widget.drag_under_color != 0) {
                        canvas.DrawImageMask(widget.rect[0] + scaled(ur[0]),
                                             widget.rect[1] + scaled(ur[1]), scaled(uw), scaled(uh),
                                             *under, sx, sy, sw, sh, widget.drag_under_color);
                    } else {
                        canvas.DrawImageRegion(
                            widget.rect[0] + scaled(ur[0]), widget.rect[1] + scaled(ur[1]),
                            scaled(uw), scaled(uh), *under, sx, sy, sw, sh, widget.drag_under_tint);
                    }
                    canvas.SetDrawOpacity(1.0f);
                }
            }
        }
        // Hidden is decided first: a hidden widget draws nothing, so the pre-reject's text
        // measuring and the occlusion test below are wasted on it (LA's map page hides ~500 of its
        // ~900 widgets while outdoors). Only a Map widget has something to do when hidden (its
        // record, at the `hidden` check further down) -- it still goes through the rejects first,
        // exactly as before.
        const bool hidden = anim_group != nullptr
                                ? !anim_group->shown ||
                                      WidgetHiddenHolding(widget, snapshot, widget.anim->gate.point)
                                : WidgetHidden(widget, snapshot);
        if (hidden && widget.type != WidgetType::Map) {
            continue;
        }
        // Where this widget is currently looking, if the user has dragged or pinched it.
        ViewTransform view{};
        if (widget.pan_zoom) {
            const std::string key =
                widget.id.empty() ? page.id + "#" + std::to_string(widget_index) : widget.id;
            if (const auto found = views.find(key); found != views.end()) {
                view = found->second;
            }
        }
        s32 x = widget.rect[0] + move_dx;
        s32 y = widget.rect[1] + move_dy;
        s32 rw = widget.rect[2];
        s32 rh = widget.rect[3];
        if (rw == 0 && rh == 0 &&
            (widget.type == WidgetType::Rect || widget.type == WidgetType::Image)) {
            // An unsized rect spans the canvas (a background); an unsized image sits centred
            // as a square a third of the shorter side (the idle page's title icon). Text
            // widgets are unsized by design and keep their anchor.
            const s32 cw = static_cast<s32>(canvas.Width()), ch = static_cast<s32>(canvas.Height());
            if (widget.type == WidgetType::Image) {
                const s32 side = std::max(64, std::min(cw, ch) / 3);
                x = (cw - side) / 2;
                y = (ch - side) / 2;
                rw = rh = side;
            } else {
                x = 0;
                y = 0;
                rw = cw;
                rh = ch;
            }
        }
        // Dirty-region redraw: on a clipped (partial) frame, a widget whose resolved rect doesn't
        // even touch the clip pays nothing for its pixel writes already
        // (Canvas::FillRect/DrawImage/etc. clamp their own loops to the clip) -- but it still paid
        // a WidgetHidden() gate check plus a full switch-dispatch, for every one of a page's
        // widgets, every clipped frame, before this. Skipped here using the SAME x/y/rw/rh already
        // resolved just above (including the unsized-widget expansion right above this line --
        // using the raw, pre-expansion widget.rect here would wrongly reject an unsized full-canvas
        // background, since its own rect is {0,0,0,0}). Conservatively skipped for a widget
        // belonging to a currently-moving anim group (anim_group != nullptr): that widget's
        // effective draw rect is the group's own (already unioned-in) box offset by dx/dy, not its
        // own resolved rect, and it is few enough in practice that leaving it on the existing,
        // already-proven anim_clip path untouched is not worth the risk of this rewriting that
        // interaction (the reject would inherit the anim_clip path's own risk under a different
        // trigger). extras->clip with w or h <= 0 means "whole canvas" (the Canvas::SetClip
        // contract), so nothing is ever wrongly rejected on a full-page draw.
        // Unsized text is anchored at rect x and grows right (left align), both ways (centre) or
        // LEFT (right align). Rejects use the same conservative extent the dirty-region scan uses
        // (WidgetEffectiveRect, mod_redraw.cpp); testing the zero-width anchor alone skipped a
        // right-aligned number whenever the dirty rect ended left of its anchor, after the
        // background under its leading digits had been repainted.
        const auto fixed_box = [&] {
            s32 bx = x, bw = rw, by = y, bh = rh;
            if (rw == 0 && (widget.type == WidgetType::Label || widget.type == WidgetType::Value)) {
                const s32 scale = std::max<s32>(1, widget.text_scale);
                const s32 span = scale * 5 * 16;
                bx = widget.align == 2 ? x - span : widget.align == 1 ? x - span / 2 : x;
                bw = span;
                by = y - scale;
                bh = (rh > 0 ? rh : scale * 9) + 2 * scale;
            }
            if (const s32 pad = WidgetDrawOverhang(widget, rw, rh, &snapshot); pad > 0) {
                bx -= pad;
                by -= pad;
                bw += 2 * pad;
                bh += 2 * pad;
            }
            return std::array<s32, 4>{bx, by, bw, bh};
        };
        // The fixed box widened by everything the widget can paint past it (long text, a tall
        // font, a run of pips).
        const auto paint_box = [&](std::array<s32, 4> b) {
            if (const auto t = WidgetPaintBounds(
                    widget, x, y, rw, rh, snapshot, manifest, canvas.ActiveFont(),
                    static_cast<s32>(canvas.Width()), static_cast<s32>(canvas.Height()));
                t[2] > 0 && t[3] > 0) {
                const s32 ux0 = std::min(b[0], t[0]), uy0 = std::min(b[1], t[1]);
                const s32 ux1 = std::max(b[0] + b[2], t[0] + t[2]),
                          uy1 = std::max(b[1] + b[3], t[1] + t[3]);
                b = {ux0, uy0, ux1 - ux0, uy1 - uy0};
            }
            return b;
        };
        if (extras != nullptr && anim_group == nullptr && extras->clip[2] > 0 &&
            extras->clip[3] > 0) {
            const s32 cx0 = extras->clip[0], cy0 = extras->clip[1];
            const s32 cx1 = cx0 + extras->clip[2], cy1 = cy0 + extras->clip[3];
            const auto misses = [&](const std::array<s32, 4>& b) {
                return b[0] + b[2] <= cx0 || b[0] >= cx1 || b[1] + b[3] <= cy0 || b[1] >= cy1;
            };
            // The paint bounds only widen the box: measure them only when the box alone misses.
            if (const auto b = fixed_box(); misses(b) && misses(paint_box(b))) {
                continue; // entirely outside this frame's dirty rect
            }
        }
        // Occlusion skip -- see static_occluders' own build-site comment above for the full safety
        // argument. widget_index >= occ_index is excluded (only a strictly LATER, higher-z-order
        // occluder can repaint over this widget; one drawn before or at the same index cannot).
        // x/y here already include a moving group's offset (move_dx/move_dy), so this is correct
        // for a moving group member too -- it is simply the widget actually being drawn this
        // tick, wherever that is. Frame_clip is
        // intersected in, exactly like the bbox pre-reject just above, so a widget only partly
        // inside the occluder (never fully covered) is correctly NOT skipped.
        // Everything the widget paints counts, not just its rect (a zero-width label's text, a
        // spinning picture's corners, a run of pips), and a widget with a highlight is never
        // skipped: highlights are painted after every widget, over the map.
        if (!static_occluders.empty() && widget.highlight_color == 0 &&
            widget.highlight_src.empty()) {
            const auto occluded_box = [&](const std::array<s32, 4>& b) {
                const s32 wx0 = std::max(frame_clip[0], b[0]);
                const s32 wy0 = std::max(frame_clip[1], b[1]);
                const s32 wx1 = std::min(frame_clip[0] + frame_clip[2], b[0] + b[2]);
                const s32 wy1 = std::min(frame_clip[1] + frame_clip[3], b[1] + b[3]);
                for (const auto& [occ_index, occ_rect] : static_occluders) {
                    if (widget_index >= occ_index) {
                        continue;
                    }
                    if (wx0 >= occ_rect[0] && wy0 >= occ_rect[1] &&
                        wx1 <= occ_rect[0] + occ_rect[2] && wy1 <= occ_rect[1] + occ_rect[3]) {
                        return true;
                    }
                }
                return false;
            };
            // The rect alone first (the cheap test): only a rect under an occluder needs the
            // widened box.
            if (occluded_box({x, y, rw, rh})) {
                if (const auto b = fixed_box(); occluded_box(b) && occluded_box(paint_box(b))) {
                    continue; // fully repainted by a later, opaque Map widget this tick
                }
            }
        }
        const s64 value = widget.bind.empty() ? 0 : snapshot.GetInt(widget.bind);
        if (hidden) {
            // A Map widget reaches this `continue` on a partial pass only when its OWN dependency
            // hash changed (otherwise the bbox pre-reject above already skipped it without ever
            // getting this far) -- i.e. it just became hidden this very tick. Its old `map_records`
            // entry (surviving the conditional clear at the top of RenderPage) is now stale and
            // must not stay tap-hittable. Cheap: `map_records` holds at most a handful of entries
            // per page even in the worst case, and this is a `size_t` equality scan, not a redraw.
            if (widget.type == WidgetType::Map && map_records != nullptr) {
                std::erase_if(*map_records, [&](const MapDrawRecord& r) {
                    return r.widget_index == widget_index;
                });
            }
            continue; // empty slot / out of this panel's category
        }
        // A moving group draws inside its box only, at its own opacity.
        std::array<s32, 4> widget_clip = frame_clip;
        float widget_alpha = 1.0f;
        const bool anim_clip = anim_group != nullptr && anim_group->moving;
        if (anim_clip) {
            const auto& b = anim_group->box;
            const s32 cx0 = std::max(frame_clip[0], b[0]);
            const s32 cy0 = std::max(frame_clip[1], b[1]);
            const s32 cx1 = std::min(frame_clip[0] + frame_clip[2], b[0] + b[2]);
            const s32 cy1 = std::min(frame_clip[1] + frame_clip[3], b[1] + b[3]);
            if (cx1 <= cx0 || cy1 <= cy0 || anim_group->alpha <= 0.0f) {
                continue; // entirely outside this frame's dirty area, or fully faded
            }
            widget_clip = {cx0, cy0, cx1 - cx0, cy1 - cy0};
            widget_alpha = anim_group->alpha;
        }
        // A scrolled list row draws only inside its region's viewport.
        const bool scroll_clip = widget.scroll_clip[2] > 0 && widget.scroll_clip[3] > 0;
        if (scroll_clip) {
            const auto& b = widget.scroll_clip;
            const s32 cx0 = std::max(widget_clip[0], b[0]);
            const s32 cy0 = std::max(widget_clip[1], b[1]);
            const s32 cx1 = std::min(widget_clip[0] + widget_clip[2], b[0] + b[2]);
            const s32 cy1 = std::min(widget_clip[1] + widget_clip[3], b[1] + b[3]);
            if (cx1 <= cx0 || cy1 <= cy0) {
                continue; // scrolled out of view, or outside this frame's dirty area
            }
            widget_clip = {cx0, cy0, cx1 - cx0, cy1 - cy0};
        }
        const bool own_clip = anim_clip || scroll_clip;
        if (!is_ghost && (widget.highlight_color != 0 || !widget.highlight_src.empty())) {
            bool lit = snapshot.drag.active && snapshot.drag.hover >= 0 &&
                       static_cast<size_t>(snapshot.drag.hover) == widget_index;
            if (!lit && !widget.select_group.empty()) {
                const auto sel = snapshot.ints.find("@sel:" + widget.select_group);
                if (sel != snapshot.ints.end() && sel->second != -1) {
                    const auto carried = WidgetPayload(widget, snapshot);
                    lit = carried.has_value() && *carried == sel->second;
                }
            }
            if (lit) {
                highlights.push_back({widget.highlight_src, widget.highlight_color, widget.frame, x,
                                      y, rw, rh, widget_clip, widget_alpha});
            }
        }
        const s64 maximum = widget.max_bind.empty()
                                ? widget.max_const
                                : snapshot.GetInt(widget.max_bind, widget.max_const);
        // Establish a clean state for every widget as well as resetting below after a map. This
        // keeps a future early-exit path from leaking map opacity into later HUD widgets.
        canvas.SetDrawOpacity(is_ghost ? 0.8f : 1.0f);
        const bool translucent_map =
            widget.type == WidgetType::Map && manifest.map_style.opacity < 1.0f;
        const size_t map_quad_begin = draw_list != nullptr ? draw_list->quads.size() : 0;
        if (translucent_map) {
            canvas.SetDrawOpacity(manifest.map_style.opacity);
        }
        const auto widget_started = profile_widgets ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
        if (own_clip) {
            canvas.SetClip(widget_clip[0], widget_clip[1], widget_clip[2], widget_clip[3]);
            canvas.SetLayerOpacity(widget_alpha);
        }
        const WidgetDrawContext ctx{.canvas = canvas,
                                    .manifest = manifest,
                                    .page = page,
                                    .snapshot = snapshot,
                                    .images = images,
                                    .texts = texts,
                                    .draw_list = draw_list,
                                    .follow_state = follow_state,
                                    .report_visit = report_visit,
                                    .is_visited = is_visited,
                                    .map_records = map_records,
                                    .follow_state_mutex = follow_state_mutex,
                                    .manifest_markers_mutex = manifest_markers_mutex,
                                    .settling = settling,
                                    .widget = widget,
                                    .widget_index = widget_index,
                                    .view = view,
                                    .x = x,
                                    .y = y,
                                    .rw = rw,
                                    .rh = rh,
                                    .value = value,
                                    .maximum = maximum};
        canvas.SetTextEffects(widget.text_outline, widget.text_outline_px, widget.text_rise);
        switch (widget.type) {
        case WidgetType::Image:
            DrawImage(ctx);
            break;
        case WidgetType::Map:
            DrawMap(ctx);
            break;
        case WidgetType::Rect:
            DrawRect(ctx);
            break;
        case WidgetType::Label:
            DrawLabel(ctx);
            break;
        case WidgetType::Value:
            DrawValue(ctx);
            break;
        case WidgetType::Bar:
            DrawBar(ctx);
            break;
        case WidgetType::Pips:
            DrawPips(ctx);
            break;
        case WidgetType::Button:
            DrawButton(ctx);
            break;
        case WidgetType::Chart:
            DrawChart(ctx);
            break;
        }
        canvas.SetTextEffects(0, 0, 0.0f);
        if (own_clip) {
            canvas.SetClip(frame_clip[0], frame_clip[1], frame_clip[2], frame_clip[3]);
            canvas.SetLayerOpacity(1.0f);
        }
        if (translucent_map) {
            canvas.SetDrawOpacity(1.0f);
            if (draw_list != nullptr) {
                const u32 amount = static_cast<u32>(manifest.map_style.opacity * 255.0f + 0.5f);
                for (size_t i = map_quad_begin; i < draw_list->quads.size(); ++i) {
                    auto& quad = draw_list->quads[i];
                    if (quad.solid) {
                        // Solid rectangles use a bounded attachment clear, so resolve their
                        // opacity over the compositor background before submission. The GPU path
                        // always draws the HUD canvas last, so it does not model a non-map widget
                        // underneath this background; packages should use the page background for
                        // a translucent map when widgets overlap.
                        u32 mixed = 0;
                        for (u32 shift = 0; shift < 32; shift += 8) {
                            const u32 below = (draw_list->bg >> shift) & 0xFF;
                            const u32 above = (quad.color >> shift) & 0xFF;
                            mixed |= ((below * (255 - amount) + above * amount) / 255) << shift;
                        }
                        quad.color = mixed;
                    } else {
                        quad.a *= manifest.map_style.opacity;
                    }
                }
            }
        }
        if (profile_widgets) {
            std::scoped_lock plk{profile_mutex};
            auto& cost = widget_costs[page.id + "#" + std::to_string(widget_index)];
            cost.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 widget_started)
                           .count();
            ++cost.calls;
            if (cost.what.empty()) {
                const std::string& src = widget.src;
                cost.what = fmt::format("type {} id '{}' rect {}x{} src '{}'",
                                        static_cast<int>(widget.type), widget.id, rw, rh,
                                        src.substr(src.find_last_of('#') == std::string::npos
                                                       ? 0
                                                       : src.find_last_of('#') + 1));
            }
        }
    }
    if (profile_widgets) {
        // Snapshot-and-clear under the lock, sort/log outside it -- keeps the critical section
        // short without letting two threads race the "is it time yet" check or the clear/reset.
        std::vector<std::pair<std::string, WidgetCost>> sorted;
        bool should_log = false;
        {
            std::scoped_lock plk{profile_mutex};
            if (std::chrono::steady_clock::now() - profile_window >= std::chrono::seconds{5}) {
                should_log = true;
                sorted.assign(widget_costs.begin(), widget_costs.end());
                widget_costs.clear();
                profile_window = std::chrono::steady_clock::now();
            }
        }
        if (should_log) {
            std::ranges::sort(
                sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
            double total = 0;
            for (const auto& [k, c] : sorted) {
                total += c.ms;
            }
            LOG_INFO(Core, "DSMod widget cost: {} widget(s), {:.1f} ms total in 5 s", sorted.size(),
                     total);
            for (size_t i = 0; i < sorted.size() && i < 14; ++i) {
                const auto& [k, c] = sorted[i];
                LOG_INFO(Core, "DSMod widget cost: {} {:.3f} ms/draw x{} ({})", k, c.ms / c.calls,
                         c.calls, c.what);
            }
        }
    }
    draw_scroll_bars(); // no ghost this frame: bars and highlights still need painting
    draw_highlights();
    canvas.SetDrawOpacity(1.0f);
    canvas.SetLayerOpacity(1.0f);
    canvas.ResetClip();
    if (animating != nullptr) {
        *animating = settling;
    }
    return true;
}

void RenderDebugPage(Canvas& canvas, const Manifest& manifest, const StateSnapshot& snapshot) {
    canvas.Clear(manifest.background);
    const u32 head = 0xFFF2E7A8u; // colours here are already in upload byte order
    const u32 ok = 0xFF87C29Cu;
    const u32 bad = 0xFF7070EFu;
    const u32 dim = 0xFFA6988Au;

    canvas.DrawText(32, 28, "DSMOD OFFSET DEBUG", 6, head);
    char line[192];
    std::snprintf(line, sizeof(line), "%s   DATA %s", manifest.name.c_str(),
                  manifest.build_id_file.empty() ? "none" : manifest.build_id_file.c_str());
    canvas.DrawText(32, 82, line, 3, dim);

    // Stable order so the list does not jump around between frames.
    std::vector<std::string> names;
    names.reserve(manifest.points.size());
    for (const auto& [name, point] : manifest.points) {
        names.push_back(name);
    }
    std::ranges::sort(names);

    s32 y = 130;
    const s32 row = 34;
    for (const auto& name : names) {
        const auto addr_it = snapshot.addresses.find(name);
        const auto val_it = snapshot.ints.find(name);
        const bool resolved = addr_it != snapshot.addresses.end() && addr_it->second != 0;
        const bool readable = val_it != snapshot.ints.end();
        if (readable) {
            std::snprintf(line, sizeof(line), "%-14s %010llX  %lld", name.c_str(),
                          static_cast<unsigned long long>(resolved ? addr_it->second : 0),
                          static_cast<long long>(val_it->second));
        } else {
            std::snprintf(line, sizeof(line), "%-14s %010llX  FAIL", name.c_str(),
                          static_cast<unsigned long long>(resolved ? addr_it->second : 0));
        }
        canvas.DrawText(32, y, line, 3, readable ? ok : bad);
        y += row;
        if (y > static_cast<s32>(canvas.Height()) - row) {
            break;
        }
    }
}

} // namespace Core::Mods
