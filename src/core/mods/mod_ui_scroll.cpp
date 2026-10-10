// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Drag-to-scroll lists (Page::scrolls): the "@scroll:<id>" offset key, region lookup, a repeat
// template's element count and row pitch, the content size (MeasureScroll, memoised per snapshot by
// ScrollMemo), the clamped offset, the visible element range and the scrollbar thumb. Pure
// functions; the drag and fling are in mod_input.cpp.

#include <algorithm>

#include "core/mods/mod_ui.h"

namespace Core::Mods {

std::string ScrollOffsetKey(const std::string& id) {
    return "@scroll:" + id;
}

const ScrollRegion* FindScrollRegion(const Page& page, const std::string& id) {
    for (const auto& region : page.scrolls) {
        if (region.id == id) {
            return &region;
        }
    }
    return nullptr;
}

s64 RepeatElementCount(const Widget& widget, const StateSnapshot& snapshot) {
    if (widget.repeat <= 0) {
        return 0;
    }
    if (widget.repeat_bind.empty()) {
        return widget.repeat;
    }
    s64 counted = snapshot.GetInt(widget.repeat_bind);
    if (widget.repeat_div > 1) {
        counted /= widget.repeat_div;
    }
    return std::clamp<s64>(counted, 0, widget.repeat);
}

s32 RepeatRowPitch(const Widget& widget) {
    return widget.repeat_cols > 0 ? widget.repeat_row_dy : widget.repeat_dy;
}

ScrollMetrics MeasureScroll(const Page& page, const ScrollRegion& region,
                            const StateSnapshot& snapshot) {
    ScrollMetrics m;
    m.cols = std::max(1, region.cols);
    m.row_h = region.row_h;
    s64 template_count = 0;
    for (const auto& w : page.widgets) {
        if (w.repeat <= 0 || w.scroll != region.id) {
            continue;
        }
        if (m.row_h <= 0) {
            m.row_h = RepeatRowPitch(w);
        }
        template_count = std::max(template_count, RepeatElementCount(w, snapshot));
    }
    m.count = region.count_bind.empty() ? template_count
                                        : std::max<s64>(0, snapshot.GetInt(region.count_bind));
    m.row_h = std::max(0, m.row_h);
    const s64 rows = (m.count + m.cols - 1) / m.cols;
    m.content_h = rows * m.row_h + (rows > 0 ? std::max(0, region.pad) : 0);
    // A single wrapped auto_w label is a document, with its real native-font height.
    // Reuse the same measuring scope as rendering/hit tests; fixed-pitch lists keep
    // their existing metrics. This avoids either truncating text or blank scroll tails.
    if (region.row_h <= 0) {
        for (const auto& w : page.widgets) {
            if (w.scroll != region.id || w.repeat != 1 || !w.auto_w ||
                RepeatElementCount(w, snapshot) != 1) {
                continue;
            }
            Widget measured = w;
            ApplyAutoWidth(measured, snapshot);
            if (measured.auto_box) {
                m.content_h = std::max<s64>(m.content_h,
                    std::max<s64>(0, static_cast<s64>(measured.rect[1]) - region.rect[1]) +
                    measured.rect[3] + std::max(0, region.pad));
            }
        }
    }
    m.max_offset =
        static_cast<s32>(std::clamp<s64>(m.content_h - std::max(0, region.rect[3]), 0, 1'000'000));
    return m;
}

s32 ScrollOffset(const ScrollRegion& region, const StateSnapshot& snapshot) {
    const s64 v = snapshot.GetInt(ScrollOffsetKey(region.id));
    return static_cast<s32>(std::clamp<s64>(v, 0, 1'000'000));
}

std::pair<s64, s64> VisibleElementRange(const Widget& widget, const ScrollRegion& region,
                                        s32 base_y, s32 offset, s64 rows, s32 paint_top,
                                        s32 paint_bottom) {
    const s64 pitch = RepeatRowPitch(widget);
    if (rows <= 0) {
        return {0, 0};
    }
    if (pitch <= 0) {
        return {0, rows}; // rows do not advance vertically: nothing to cull by position
    }
    const s64 cols = widget.repeat_cols > 0 ? widget.repeat_cols : 1;
    // Row r paints [y_r + above, y_r + h) with y_r = base_y + r*pitch - offset: its rect, widened
    // to what its text paints (a label's glyphs run past a zero or short rect height). Keep it
    // when that touches [top, bottom); the draw clips it to the viewport.
    const s64 h = std::max<s64>({widget.rect[3], paint_bottom, 1});
    const s64 above = std::min(0, paint_top);
    const s64 top = region.rect[1];
    const s64 bottom = static_cast<s64>(region.rect[1]) + region.rect[3];
    const auto floor_div = [](s64 a, s64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const s64 first_row = std::max<s64>(0, floor_div(top + offset - base_y - h, pitch) + 1);
    const s64 end_row =
        std::max<s64>(0, floor_div(bottom + offset - base_y - above - 1, pitch) + 1);
    return {std::min(rows, first_row * cols), std::min(rows, end_row * cols)};
}

std::array<s32, 2> RowPaintSpan(const Widget& widget, const StateSnapshot& snapshot, s32 max_h) {
    if (widget.type != WidgetType::Label && widget.type != WidgetType::Value &&
        widget.type != WidgetType::Button) {
        return {0, 0};
    }
    // The text bound with the built-in metrics (widths do not matter here, so no manifest tables
    // and no font); a game font's descender reaches up to about one more em below. The canvas
    // height caps the extent (a wrapped label of unknown length), here the viewport's.
    static const Manifest no_manifest;
    const s32 em = std::max<s32>(1, widget.text_scale) * 5;
    const auto t = WidgetTextBounds(widget, widget.rect[0], 0, widget.rect[2], widget.rect[3],
                                    snapshot, no_manifest, nullptr, 1 << 16, std::max(1, max_h));
    if (t[3] <= 0) {
        return {0, 0};
    }
    return {std::min(0, t[1]), std::min(t[1] + t[3] + em + 2, std::max(1, max_h) + em + 2)};
}

std::array<s32, 4> ScrollBarThumb(const ScrollRegion& region, const ScrollMetrics& m, s32 offset) {
    if (region.bar_color == 0 || m.max_offset <= 0 || m.content_h <= 0 || region.rect[3] <= 0) {
        return {0, 0, 0, 0};
    }
    const s32 track = region.rect[3];
    const s32 thumb = std::clamp(static_cast<s32>(static_cast<s64>(track) * track / m.content_h),
                                 std::min(track, 24), track);
    const s32 y =
        region.rect[1] + static_cast<s32>(static_cast<s64>(track - thumb) *
                                          std::clamp(offset, 0, m.max_offset) / m.max_offset);
    const s32 bw = std::max(1, region.bar_w);
    return {region.rect[0] + region.rect[2] - bw, y, bw, thumb};
}

const ScrollMetrics& ScrollMemo::Get(const Page& page, const ScrollRegion& region,
                                     const StateSnapshot& snapshot) {
    const size_t ri = static_cast<size_t>(&region - page.scrolls.data());
    if (ri >= known.size()) {
        Reset(page.scrolls.size());
    }
    if (known[ri] == 0) {
        metrics[ri] = MeasureScroll(page, region, snapshot);
        known[ri] = 1;
    }
    return metrics[ri];
}

} // namespace Core::Mods
