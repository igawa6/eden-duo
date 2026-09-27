// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Private to the mod_ui*.cpp files. The helpers more than one of them needs -- MaxPips, Blend (the
// per-pixel "over"), BlinkAlpha -- sit in an anonymous namespace and are inline, so every file
// keeps its own internal copy as before the split. WidgetDrawContext is the argument of the
// per-widget draw functions RenderPage dispatches to (mod_ui.cpp, and DrawMap in
// mod_ui_map_widget.cpp).

#pragma once

#include <cstddef>
#include <mutex>

#include "common/common_types.h"
#include "core/mods/mod_ui.h"

namespace Core::Mods {

namespace {
/// A run of pips is drawn one at a time; cap it so a garbage maximum cannot stall a frame.
constexpr s64 MaxPips = 64;

inline u32 Blend(u32 dst, u32 src) {
    const u32 a = (src >> 24) & 0xFF;
    if (a == 0xFF) {
        return src;
    }
    if (a == 0) {
        return dst;
    }
    const u32 da = (dst >> 24) & 0xFF;
    if (da == 0) {
        // Nothing underneath yet (the transparent HUD overlay of the GPU path): keep the source's
        // own alpha, so a translucent panel or an anti-aliased glyph edge composites over the map
        // on the GPU instead of turning into an opaque dark pixel.
        return src;
    }
    const u32 ia = 255 - a;
    const u32 r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    const u32 g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
    const u32 b = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;
    const u32 oa = a + da * ia / 255; // straight-alpha "over": opaque stays opaque
    return (oa << 24) | (r << 16) | (g << 8) | b;
}

/// Ping-pong blink shared by every pulsing marker: a scalar triangle wave (no sine, like the game's
/// minimap::CAnimatedValue) between `lo` and 1.0 over `period` ticks.
inline float BlinkAlpha(u64 tick, u64 period, float lo) {
    if (period < 2) {
        return 1.0f;
    }
    const u64 half = period / 2;
    const u64 t = tick % period;
    const float f = static_cast<float>(t < half ? t : period - t) / static_cast<float>(half);
    return lo + (1.0f - lo) * f;
}
} // namespace

/// Everything one widget's draw function reads (DrawImage ... DrawButton in mod_ui.cpp, DrawMap in
/// mod_ui_map_widget.cpp): RenderPage's arguments, and what the dispatcher resolved for this
/// widget. Made per widget; its references are RenderPage's own variables, so a draw function
/// reads and writes (`x`/`y`, `settling`) exactly what the switch case did.
struct WidgetDrawContext {
    // RenderPage's arguments.
    Canvas& canvas;
    const Manifest& manifest;
    const Page& page;
    const StateSnapshot& snapshot;
    const ImageProvider& images;
    const TextProvider& texts;
    AuxDrawList* draw_list;
    MapFollowState* follow_state;
    const VisitReporter& report_visit;
    const VisitedQuery& is_visited;
    MapDrawRecords* map_records;
    std::mutex* follow_state_mutex;
    std::recursive_mutex* manifest_markers_mutex;
    /// Set while a follow-view glide is still moving (RenderPage reports it as `animating`).
    bool& settling;
    // This widget.
    const Widget& widget;
    size_t widget_index;       ///< index in the expanded widget list
    const ViewTransform& view; ///< the user's pan / zoom of it (the identity when none)
    s32& x; ///< the resolved rect (an unsized Rect / Image spans its default box)
    s32& y;
    s32 rw;
    s32 rh;
    s64 value;   ///< the `bind` value (0 when unbound)
    s64 maximum; ///< the `max_bind` value, else `max_const`
};

/// WidgetType::Map (mod_ui_map_widget.cpp).
void DrawMap(const WidgetDrawContext& ctx);

} // namespace Core::Mods
