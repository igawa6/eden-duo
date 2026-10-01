// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The Map widget (WidgetType::Map): one area of the game's map, framed on the player, with its
// markers.
//   - DrawMap: the widget background (a canvas fill, or a solid quad on the GPU path) and the area
//     (SelectMapArea: the room / area string point, the zone + season number point, else the fixed
//     area), then GeometryMapDraw for an area with map data (manifest.map_areas), an "unknown
//     area" note, or DrawRoomBoxes (the room-sprite map).
//   - GeometryMapDraw, in draw order: FrameView (the area's extent, the live player position, the
//     follow / fit / pan-zoom framing and the follow glide), DrawBaseLayer (the rasterised
//     "map:<area>@WxH" image or the area's own picture, its cross-fade and the item-room pulse),
//     BeginRecord (what a tap hit-tests against), DrawAtlasLayer (area markers, region labels,
//     array-point pins, the player's custom markers, the player, a second actor),
//     DrawMarkerPicture (a marker_src picture), DrawPinFallback (the diamond pin, visit reports).
//     Runtime 14: the base picture may come from a text point (map "image_bind"), DrawOverlays
//     places bound pictures in world space over it (map "overlays"), and array-point pins may
//     draw per-slot pictures sized in world units with a bar, a dim state and a frame / tint.
//     On the GPU path (draw_list) the map, pulse and atlas glyphs are emitted as quads; glyphs that
//     must sit above the labels still go into the canvas.
//   - The map label bitmaps: a region name rendered once with its outline, then blitted.
// Not here: rasterising the area and the fog of war (GetImage, MarkVisitedAt / IsVisited,
// mod_map.cpp), compositing the quads (PublishGpuComposite, mod_redraw.cpp), map taps
// (mod_input.cpp).
// Threads: see mod_ui.cpp. follow_state_mutex guards every *follow_state access;
// manifest_markers_mutex is held while DrawAtlasLayer reads the markers' live flags.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fmt/format.h>

#include "common/logging.h"
#include "core/mods/mod_map_view_rect.h"
#include "core/mods/mod_ui.h"
#include "core/mods/mod_ui_internal.h"

namespace Core::Mods {

namespace {
/// A map label rendered once (text + outline) into its own picture; `ox`/`oy` = where the text's
/// origin (left edge, cap top) sits inside it.
struct LabelBitmap {
    Image image;
    s32 ox{};
    s32 oy{};
    s32 text_w{};
};

// MapLabelBitmap's cache is one instance for the whole process, shared by every call regardless of
// thread (RenderPage runs on the redraw worker and on the tick thread). A bare static returning a
// raw pointer into itself would let a second thread's concurrent `cache.size() >= 512` eviction
// free the entry the first thread is still using. Same hazard as image_cache and the msbt text
// pointer, handled the same way: a mutex-guarded map of shared_ptr, so a caller's returned handle
// survives whatever the cache does meanwhile.
std::mutex& MapLabelBitmapMutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<const LabelBitmap> MapLabelBitmap(const Canvas& target, const std::string& text,
                                                  s32 scale, u32 color, u32 outline_color,
                                                  s32 outline) {
    static std::unordered_map<std::string, std::shared_ptr<const LabelBitmap>> cache;
    std::scoped_lock lock{MapLabelBitmapMutex()};
    const FontMetrics* const fm = target.HasFont() ? target.font_metrics : nullptr;
    const std::string key = fmt::format(
        "{}\x1f{}\x1f{:08X}\x1f{:08X}\x1f{}\x1f{}\x1f{}\x1f{}", text, scale, color, outline_color,
        outline, static_cast<const void*>(target.HasFont() ? target.font_atlas : nullptr),
        static_cast<const void*>(fm), fm != nullptr ? fm->line_height : 0);
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    if (cache.size() >= 512) {
        cache.clear();
    }
    Canvas tmp;
    if (target.HasFont()) {
        tmp.SetFont(target.font_atlas, target.font_metrics);
    }
    const s32 o = std::clamp(outline, 0, 16);
    const s32 text_w = tmp.MeasureText(text, scale);
    const s32 margin = std::max(2, scale);
    const s32 above = scale * 2; // accents over the capitals
    const s32 below = scale * 3; // descenders
    const s32 w = std::max(1, text_w + 2 * (o + margin));
    const s32 h = std::max(1, scale * 5 + above + below + 2 * o);
    tmp.Resize(static_cast<u32>(w), static_cast<u32>(h));
    tmp.Clear(0);
    const s32 ox = o + margin;
    const s32 oy = o + above;
    if (o > 0 && (outline_color >> 24) != 0) {
        for (s32 dy = -1; dy <= 1; ++dy) {
            for (s32 dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0) {
                    tmp.DrawText(ox + dx * o, oy + dy * o, text, scale, outline_color);
                }
            }
        }
    }
    tmp.DrawText(ox, oy, text, scale, color);
    // Keep only the inked box: the margins for accents / descenders are mostly empty, and every
    // redraw blits the whole picture.
    const auto& px = tmp.Pixels();
    s32 x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (s32 yy = 0; yy < h; ++yy) {
        for (s32 xx = 0; xx < w; ++xx) {
            if ((px[static_cast<size_t>(yy) * w + xx] >> 24) != 0) {
                x0 = std::min(x0, xx);
                x1 = std::max(x1, xx);
                y0 = std::min(y0, yy);
                y1 = std::max(y1, yy);
            }
        }
    }
    LabelBitmap bmp;
    bmp.text_w = text_w;
    if (x1 < x0) {
        return cache.emplace(key, std::make_shared<const LabelBitmap>(std::move(bmp)))
            .first->second; // nothing inked
    }
    bmp.image.w = static_cast<u32>(x1 - x0 + 1);
    bmp.image.h = static_cast<u32>(y1 - y0 + 1);
    bmp.image.pixels.resize(u64{bmp.image.w} * bmp.image.h);
    for (s32 yy = y0; yy <= y1; ++yy) {
        std::memcpy(bmp.image.pixels.data() + static_cast<size_t>(yy - y0) * bmp.image.w,
                    px.data() + static_cast<size_t>(yy) * w + x0, bmp.image.w * sizeof(u32));
    }
    bmp.ox = ox - x0;
    bmp.oy = oy - y0;
    return cache.emplace(key, std::make_shared<const LabelBitmap>(std::move(bmp))).first->second;
}

/// The area a Map widget shows this draw: named by the room (or area) string point, by the
/// zone (and season) number point, else the widget's fixed area. `current_room` is the
/// room name as read, for the room-box fallback's highlight.
void SelectMapArea(const WidgetDrawContext& ctx, std::string& area, std::string& current_room) {
    const Manifest& manifest = ctx.manifest;
    const StateSnapshot& snapshot = ctx.snapshot;
    const Widget& widget = ctx.widget;
    if (!widget.room_bind.empty()) {
        if (const auto it = snapshot.texts.find(widget.room_bind); it != snapshot.texts.end()) {
            current_room = it->second;
            if (const auto room = manifest.map_rooms.find(current_room);
                room != manifest.map_rooms.end()) {
                area = room->second.area;
            } else if (manifest.map_areas.contains(current_room)) {
                // Some games name the area directly rather than the room inside it --
                // Dread's scenario id ("s010_cave") is already the key we draw by.
                area = current_room;
            }
        }
    }
    if (area.empty() && !widget.area_bind.empty()) {
        const auto zone = snapshot.ints.find(widget.area_bind);
        if (zone != snapshot.ints.end()) {
            if (const auto mapped = manifest.zone_area.find(zone->second);
                mapped != manifest.zone_area.end()) {
                area = mapped->second;
            }
        }
    }
    if (area.empty()) {
        area = widget.area;
    }
}

using MapAreaIterator = decltype(Manifest::map_areas)::const_iterator;

/// One Map widget draw of an area that has map data (manifest.map_areas): the framing, the
/// base picture, then the overlays. A method object: the draw's shared values are members,
/// so each phase reads like the code it came from. The lower_case methods are the draw's
/// helpers (to_x/to_y map world to canvas, emit_atlas* place an atlas glyph); the
/// CamelCase ones are its phases, called in order by Draw().
class GeometryMapDraw {
public:
    GeometryMapDraw(const WidgetDrawContext& ctx, const std::string& area_, MapAreaIterator geo_)
        : canvas{ctx.canvas}, manifest{ctx.manifest}, page{ctx.page}, snapshot{ctx.snapshot},
          images{ctx.images}, texts{ctx.texts}, draw_list{ctx.draw_list},
          follow_state{ctx.follow_state}, report_visit{ctx.report_visit},
          is_visited{ctx.is_visited}, map_records{ctx.map_records},
          follow_state_mutex{ctx.follow_state_mutex},
          manifest_markers_mutex{ctx.manifest_markers_mutex}, settling{ctx.settling},
          widget{ctx.widget}, widget_index{ctx.widget_index}, view{ctx.view}, x{ctx.x}, y{ctx.y},
          rw{ctx.rw}, rh{ctx.rh}, area{area_}, geo{geo_} {}

    void Draw();
    /// FrameView alone (ProbeMapView): the view this draw would use, into the record fields.
    bool ProbeView(MapDrawRecord& out);

private:
    // Phases, in draw order.
    bool FrameView();     ///< false: nothing more to draw (no extent, no first sample)
    bool DrawBaseLayer(); ///< false: the picture underlay is missing
    void BeginRecord();
    void DrawAtlasLayer(); ///< area markers, labels, pins, custom markers, player, actor
    void DrawAreaMarkers();
    void DrawDynamicMarkers();
    void DrawCustomMarkers();
    void DrawLivePlayer();
    void DrawSecondActor();
    void DrawMarkerPicture();
    void DrawPinFallback();
    void DrawFallbackPin(s32 cxm, s32 cym);
    void DrawOverlays(); ///< runtime 14: map "overlays", over the base picture, under markers

    // Helpers.
    [[nodiscard]] float to_x(float wx) const;
    [[nodiscard]] float to_y(float wy) const;
    [[nodiscard]] bool visible(float px, float py) const;
    /// A picture into the canvas at (qx, qy, qw, qh), clipped to the widget, tinted (ARGB).
    void blit_clipped(const Image& img, float qx, float qy, float qw, float qh, u32 tint);
    /// A solid rect into the canvas, clipped to the widget.
    void fill_clipped(float l, float t, float w, float h, u32 argb);
    bool group_visible(const std::string& group, float& alpha) const;
    void draw_labels();
    void emit_atlas_tinted(float ux, float uy, float uw, float uh, float qx, float qy, float qw,
                           float qh, float alpha, float cr, float cg, float cb,
                           bool to_canvas = false);
    void emit_atlas(float ux, float uy, float uw, float uh, float qx, float qy, float qw, float qh,
                    float alpha);
    void cell_uv(const std::pair<s32, s32>& cell, float& ux, float& uy) const;
    void emit_pin(float ux, float uy, float uw, float uh, float qx, float qy, float qw, float qh,
                  float alpha);

    // RenderPage's arguments and this widget (see WidgetDrawContext).
    Canvas& canvas;
    const Manifest& manifest;
    const Page& page;
    const StateSnapshot& snapshot;
    const ImageProvider& images;
    const TextProvider& texts;
    AuxDrawList* const draw_list;
    MapFollowState* const follow_state;
    const VisitReporter& report_visit;
    const VisitedQuery& is_visited;
    MapDrawRecords* const map_records;
    std::mutex* const follow_state_mutex;
    std::recursive_mutex* const manifest_markers_mutex;
    bool& settling;
    const Widget& widget;
    const size_t widget_index;
    const ViewTransform& view;
    s32& x;
    s32& y;
    const s32 rw;
    const s32 rh;
    const std::string& area;
    const MapAreaIterator geo;

    // FrameView: the area's extent, the live player position, and the view (centre `cx`/`cy`,
    // `ppw` canvas px per world unit, `cxpix`/`cypix` the widget centre on the canvas).
    bool image_mode{};
    /// The base picture: the area's `image`, or the key a map "image_bind" text point names.
    std::string base_image;
    float min_x{}, max_x{}, min_y{}, max_y{};
    float span_x{}, span_y{};
    bool have_player = false;
    float wx_player = 0.0f, wy_player = 0.0f;
    float ppw{}, cx{}, cy{};
    float cxpix{}, cypix{};
    /// The pinch's lower zoom limit this frame: the widget's min_zoom, or with a bound view rect
    /// the whole-area fit relative to the rect fit (runtime 14).
    float zoom_min{1.0f};
    // DrawBaseLayer: the raster size and the whole area's on-screen rectangle.
    s32 iw{}, ih{};
    float img_l{}, img_t{};
    float dest_w{}, dest_h{};
    // Overlays.
    const MapWidgetExtras* map_extras{};
    MapDrawRecord* record = nullptr;
    bool labels_drawn = false;
    std::shared_ptr<const Image> atlas;
    float aw{}, ah{}, cp{};
    s32 ItemIconSize{}, DoorIconSize{};
    float item_blink{};
    s32 PlayerIcon{};
    // For the record (MapDrawRecord's change-tracking fields): where the live marker painted,
    // whether anything else was drawn from the clock, and which pictures were used.
    std::array<s32, 4> marker_px{};
    bool uses_clock = false;
    u64 content = 0;
    void add_marker_px(s32 bx, s32 by, s32 bw, s32 bh);
    void add_content(const void* picture);
};

void GeometryMapDraw::Draw() {
    if (!FrameView()) {
        return;
    }
    if (!DrawBaseLayer()) {
        return;
    }
    map_extras = widget.map_extras.get();
    BeginRecord();
    DrawOverlays();
    atlas = (images && !manifest.icon_atlas.empty()) ? images(manifest.icon_atlas) : nullptr;
    if (atlas != nullptr && manifest.icon_cell > 0) {
        DrawAtlasLayer();
    } else {
        // No icon atlas: array-point pins that draw their own pictures still show.
        ItemIconSize = manifest.map_style.item_icon;
        DrawDynamicMarkers();
    }
    if (!labels_drawn) {
        draw_labels(); // no icon atlas (yet): the names still show
    }
    DrawMarkerPicture();
    DrawPinFallback();
    if (!image_mode && widget.area_label) {
        canvas.DrawText(x + widget.label_offset[0], y + rh - widget.label_offset[1], area,
                        widget.text_scale, widget.color);
    }
    if (record != nullptr) {
        add_content(atlas.get());
        record->marker_box = marker_px;
        record->clock_dependent = uses_clock;
        record->content_id = content;
    }
}

bool GeometryMapDraw::ProbeView(MapDrawRecord& out) {
    if (!FrameView()) {
        return false;
    }
    out.page_id = page.id;
    out.widget_index = widget_index;
    out.area = area;
    out.rect = {x, y, rw, rh};
    out.cx = cx;
    out.cy = cy;
    out.ppw = ppw;
    out.min_x = min_x;
    out.min_y = min_y;
    out.max_x = max_x;
    out.max_y = max_y;
    return true;
}

void GeometryMapDraw::add_marker_px(s32 bx, s32 by, s32 bw, s32 bh) {
    if (bw <= 0 || bh <= 0) {
        return;
    }
    if (marker_px[2] <= 0 || marker_px[3] <= 0) {
        marker_px = {bx, by, bw, bh};
        return;
    }
    const s32 x0 = std::min(marker_px[0], bx), y0 = std::min(marker_px[1], by);
    const s32 x1 = std::max(marker_px[0] + marker_px[2], bx + bw);
    const s32 y1 = std::max(marker_px[1] + marker_px[3], by + bh);
    marker_px = {x0, y0, x1 - x0, y1 - y0};
}

void GeometryMapDraw::add_content(const void* picture) {
    content = (content ^ static_cast<u64>(reinterpret_cast<uintptr_t>(picture))) *
                  0x100000001B3ULL +
              0x9E3779B97F4A7C15ULL;
}

bool GeometryMapDraw::FrameView() {
    // An area with a prerendered image (the game's own map art) draws that picture
    // instead of rasterising .geo triangles; the world->panel transform, markers and
    // the live pin all behave as before. Fog of war is suppressed: the picture is
    // whole, there is no reveal grid, and MarkVisitedAt would build one that blanks
    // the map.
    base_image = geo->second.image;
    if (const MapWidgetExtras* mx = widget.map_extras.get();
        mx != nullptr && !mx->image_bind.empty()) {
        if (const auto t = snapshot.texts.find(mx->image_bind);
            t != snapshot.texts.end() && !t->second.empty()) {
            base_image = t->second; // runtime 14: the bound picture overrides the area's
        }
    }
    image_mode = !base_image.empty();
    // One uniform pixels-per-world scale drives the base image AND every marker, so the
    // map keeps the world's real proportions instead of being stretched to fill the
    // widget (the area is ~2.9:1 but the panel is ~1.25:1, which squashed it tall).
    min_x = geo->second.min_x;
    max_x = geo->second.max_x;
    min_y = geo->second.min_y;
    max_y = geo->second.max_y;
    span_x = max_x - min_x;
    span_y = max_y - min_y;
    if (span_x <= 0.0f || span_y <= 0.0f) {
        return false;
    }
    if (!widget.marker_x_bind.empty() && !widget.marker_y_bind.empty()) {
        const auto fx = snapshot.floats.find(widget.marker_x_bind);
        const auto fy = snapshot.floats.find(widget.marker_y_bind);
        if (fx != snapshot.floats.end() && fy != snapshot.floats.end()) {
            wx_player = static_cast<float>(fx->second) * widget.marker_scale;
            wy_player = static_cast<float>(fy->second) * widget.marker_scale;
            have_player = std::isfinite(wx_player) && std::isfinite(wy_player);
        }
    }
    // Follow mode: follow_window world units span the widget WIDTH, centred on Samus --
    // the game's own minimap, one screen down. A transiently missing position retains
    // this area's last centre and fixed zoom instead of flashing the whole-area fit.
    // Before the first valid sample (including after an area change), draw only the
    // widget background; another area's cached centre must never leak across.
    const bool follow = widget.follow_window > 0.0f;
    const std::string vkey =
        widget.id.empty() ? page.id + "#" + std::to_string(widget_index) : widget.id;
    const std::string fkey = vkey + "@" + area;
    zoom_min = widget.min_zoom;
    // Bound default view (runtime 14, map "view_rect_*_bind"): when the four values resolve, the
    // base view (zoom 1, pan 0) is that rect fitted into the widget, and a new rect glides the
    // base there unless the user's own view is in effect (then it holds, like the follow
    // centre). A transiently missing rect keeps the last one shown in this area; with none yet,
    // the view falls back to the area fit / follow framing below. The shown base is kept per
    // widget AND area ("<id>@<area>#rect_c" / "#rect_z", restored with the follow state by a
    // narrowed marker redraw); "<id>#zmin" tells the pinch (ZoomLimits) how far out it may go.
    bool rect_base = false;
    if (const MapWidgetExtras* mx = widget.map_extras.get(); mx != nullptr && mx->HasViewRect()) {
        const auto& b = mx->view_rect_binds;
        const auto r = NormaliseViewRect(
            SnapshotNumber(snapshot, b[0]), SnapshotNumber(snapshot, b[1]),
            SnapshotNumber(snapshot, b[2]), SnapshotNumber(snapshot, b[3]), mx->view_rect_pad);
        const bool user_view =
            widget.pan_zoom && MapViewCustom(view.zoom, view.pan_x, view.pan_y, view.gliding);
        std::optional<MapBaseView> shown;
        std::unique_lock<std::mutex> rect_lk;
        if (follow_state != nullptr && follow_state_mutex != nullptr) {
            rect_lk = std::unique_lock<std::mutex>{*follow_state_mutex};
        }
        if (follow_state != nullptr) {
            const auto c = follow_state->find(fkey + "#rect_c");
            const auto z = follow_state->find(fkey + "#rect_z");
            if (c != follow_state->end() && z != follow_state->end()) {
                shown = MapBaseView{c->second[0], c->second[1], z->second[0]};
            }
        }
        if (r.has_value()) {
            const MapBaseView target = FitWorldRect(*r, rw, rh);
            if (!shown.has_value()) {
                shown = target;
            } else if (StepBaseGlide(*shown, target, user_view)) {
                settling = true;
            }
        }
        if (shown.has_value()) {
            rect_base = true;
            cx = shown->cx;
            cy = shown->cy;
            ppw = shown->ppw;
            zoom_min = RectMinZoom(FitWorldRect({min_x, min_y, max_x, max_y}, rw, rh).ppw, ppw,
                                   widget.min_zoom);
        }
        if (follow_state != nullptr) {
            if (shown.has_value()) {
                (*follow_state)[fkey + "#rect_c"] = {shown->cx, shown->cy};
                (*follow_state)[fkey + "#rect_z"] = {shown->ppw, 0.0f};
            }
            (*follow_state)[vkey + "#zmin"] = {zoom_min, 0.0f};
        }
    }
    // Locked whenever follow_state_mutex is given (see RenderPage's declaration in mod_ui.h).
    // Held only around the lookup, not the draw work that follows.
    bool have_cached_follow = false;
    std::array<float, 2> cached_follow_value{};
    if (follow_state != nullptr) {
        std::unique_lock<std::mutex> flk;
        if (follow_state_mutex != nullptr) {
            flk = std::unique_lock<std::mutex>{*follow_state_mutex};
        }
        const auto cached_follow = follow_state->find(fkey);
        have_cached_follow = cached_follow != follow_state->end();
        if (have_cached_follow) {
            cached_follow_value = cached_follow->second;
        }
    }
    if (!rect_base && follow && !have_player && !have_cached_follow && !image_mode) {
        if (draw_list != nullptr) {
            // The bounded solid background is the complete initial follow view. Mark
            // the composite active so the GPU publishes it while waiting for the
            // first sample instead of falling back to the transparent HUD canvas.
            draw_list->active = true;
        }
        return false;
    }
    if (rect_base) {
        // cx / cy / ppw: the bound view rect's fit, set above.
    } else if (follow) {
        ppw = static_cast<float>(rw) / widget.follow_window;
        if (!have_player && !have_cached_follow) {
            // A picture map with no position yet: its centre, at the follow zoom.
            cx = (min_x + max_x) * 0.5f;
            cy = (min_y + max_y) * 0.5f;
        } else if (!have_player) {
            cx = cached_follow_value[0];
            cy = cached_follow_value[1];
        } else {
            // Room-based framing, like Dread's minimap: find the collision camera Samus
            // is in (the SMALLEST rect that contains her, so nested sub-cameras win
            // over the big parent), then clamp the fixed-zoom view centre inside that
            // camera's padded rect. The view holds on the current room and snaps to the
            // next at a doorway, instead of jitter-following the player. camera_rects
            // come from the .bmscc CAMERA_RED boxes.
            // world units of context shown past the room edge (package: follow_pad)
            const float Pad = geo->second.follow_pad;
            float cminx = min_x, cminy = min_y, cmaxx = max_x, cmaxy = max_y;
            float best_area = 3.4e38f;
            for (const auto& r : geo->second.camera_rects) {
                if (wx_player >= r[0] && wx_player <= r[2] && wy_player >= r[1] &&
                    wy_player <= r[3]) {
                    const float ar = (r[2] - r[0]) * (r[3] - r[1]);
                    if (ar < best_area) {
                        best_area = ar;
                        cminx = r[0];
                        cminy = r[1];
                        cmaxx = r[2];
                        cmaxy = r[3];
                    }
                }
            }
            const float halfW = static_cast<float>(rw) / ppw * 0.5f;
            const float halfH = static_cast<float>(rh) / ppw * 0.5f;
            // Clamp the centre so the view stays within [room - Pad, room + Pad]; if
            // the room is smaller than the view on an axis, centre that axis on the
            // room.
            const float tx = ((cmaxx - cminx) + 2.0f * Pad >= 2.0f * halfW)
                                 ? std::clamp(wx_player, cminx - Pad + halfW, cmaxx + Pad - halfW)
                                 : (cminx + cmaxx) * 0.5f;
            const float ty = ((cmaxy - cminy) + 2.0f * Pad >= 2.0f * halfH)
                                 ? std::clamp(wy_player, cminy - Pad + halfH, cmaxy + Pad - halfH)
                                 : (cminy + cmaxy) * 0.5f;
            // Glide to the clamped target: stable within a room (target is the room
            // centre), and a short smooth slide -- not a jump -- when the target snaps
            // to a new room. Keyed by widget AND area, so an elevator to another area
            // re-seeds instead of gliding in from the old area's world coordinates;
            // `settling` asks the caller to keep redrawing until the glide lands, since
            // a standing player otherwise leaves the signature (and thus the view)
            // frozen mid-slide.
            std::array<float, 2> local_follow{tx, ty};
            auto* fs = &local_follow;
            // Held for the whole glide-easing block below, through the `cx = (*fs)[0]` reads
            // that end it: once `fs` points into *follow_state, every access through it must
            // stay inside the same critical section as the try_emplace that produced it.
            std::unique_lock<std::mutex> follow_lk;
            if (follow_state != nullptr) {
                if (follow_state_mutex != nullptr) {
                    follow_lk = std::unique_lock<std::mutex>{*follow_state_mutex};
                }
                auto fit = follow_state->try_emplace(fkey, std::array<float, 2>{tx, ty}).first;
                fs = &fit->second;
            }
            // While the user's own view is in effect (dragged, pinched, or a reset
            // still gliding) the follow centre holds where it was: the map stays
            // exactly as they left it and only resumes tracking Samus once the view is
            // home again.
            const bool user_view =
                widget.pan_zoom && (view.gliding || std::fabs(view.zoom - 1.0f) > 0.001f ||
                                    std::fabs(view.pan_x) > 0.5f || std::fabs(view.pan_y) > 0.5f);
            if (!user_view) {
                (*fs)[0] += (tx - (*fs)[0]) * 0.25f;
                (*fs)[1] += (ty - (*fs)[1]) * 0.25f;
                if (std::fabs(tx - (*fs)[0]) + std::fabs(ty - (*fs)[1]) > 0.5f / ppw) {
                    settling = true;
                } else {
                    (*fs)[0] = tx;
                    (*fs)[1] = ty;
                }
            }
            cx = (*fs)[0];
            cy = (*fs)[1];
        }
    } else {
        ppw = std::min(static_cast<float>(rw) / span_x, static_cast<float>(rh) / span_y);
        cx = (min_x + max_x) * 0.5f;
        cy = (min_y + max_y) * 0.5f;
    }
    // The user's drag / pinch on top of the follow (or fitted) view. Pan is kept in
    // canvas pixels at zoom 1 with the widget's top-left as origin -- the same
    // convention the gesture code uses for pictures -- so a drag moves the rooms
    // with the finger and a pinch zooms about the point between the fingers.
    if (widget.pan_zoom) {
        const float z = std::clamp(view.zoom, std::min(zoom_min, widget.max_zoom), widget.max_zoom);
        const float halfw = static_cast<float>(rw) * 0.5f;
        const float halfh = static_cast<float>(rh) * 0.5f;
        cx += (view.pan_x + halfw / z - halfw) / ppw;
        cy -= (view.pan_y + halfh / z - halfh) / ppw;
        cx = std::clamp(cx, min_x, max_x);
        cy = std::clamp(cy, min_y, max_y);
        if (geo->second.clamp_view) {
            // Keep the window on the picture: centred on an axis the view outgrows,
            // else no further than the edge. The pan that lands exactly there is
            // handed back so a drag past the edge does not leave a dead zone.
            const float ppz = ppw * z;
            const float hw = halfw / ppz, hh = halfh / ppz;
            const float ccx = span_x <= 2.0f * hw ? (min_x + max_x) * 0.5f
                                                  : std::clamp(cx, min_x + hw, max_x - hw);
            const float ccy = span_y <= 2.0f * hh ? (min_y + max_y) * 0.5f
                                                  : std::clamp(cy, min_y + hh, max_y - hh);
            const bool custom_view = std::fabs(view.zoom - 1.0f) > 0.001f ||
                                     std::fabs(view.pan_x) > 0.5f || std::fabs(view.pan_y) > 0.5f;
            if (follow_state != nullptr && custom_view && !view.gliding &&
                (std::fabs(ccx - cx) * ppw > 0.5f || std::fabs(ccy - cy) * ppw > 0.5f)) {
                const std::string vkey =
                    widget.id.empty() ? page.id + "#" + std::to_string(widget_index) : widget.id;
                // The pan-correction write: ApplyViewCorrections (mod_input.cpp) consumes it
                // under the same lock.
                std::unique_lock<std::mutex> pan_lk;
                if (follow_state_mutex != nullptr) {
                    pan_lk = std::unique_lock<std::mutex>{*follow_state_mutex};
                }
                (*follow_state)[vkey + "#pan"] = {view.pan_x + (ccx - cx) * ppw,
                                                  view.pan_y - (ccy - cy) * ppw};
            }
            cx = ccx;
            cy = ccy;
        }
        ppw *= z;
        // A debug-log rate limiter shared by every thread that renders. Approximate is
        // fine (under a race the worst case is one duplicate or one skipped line), so an
        // atomic with relaxed ordering is enough to keep it free of data races.
        static std::atomic<u64> last_view_log{0};
        const u64 last_view_log_at = last_view_log.load(std::memory_order_relaxed);
        if (snapshot.tick - last_view_log_at >= 60 && z != 1.0f) {
            last_view_log.store(snapshot.tick, std::memory_order_relaxed);
            LOG_DEBUG(Core, "DSMod view: centre ({:.0f},{:.0f}) ppw {:.5f} widget [{},{} {}x{}]",
                      cx, cy, ppw, x, y, rw, rh);
        }
    }
    cxpix = static_cast<float>(x) + static_cast<float>(rw) * 0.5f;
    cypix = static_cast<float>(y) + static_cast<float>(rh) * 0.5f;
    return true;
}

float GeometryMapDraw::to_x(float wx) const {
    return cxpix + (wx - cx) * ppw;
}

float GeometryMapDraw::to_y(float wy) const { // world Y is up; screen down
    return cypix - (wy - cy) * ppw;
}

bool GeometryMapDraw::visible(float px, float py) const {
    return px >= x && px < x + rw && py >= y && py < y + rh;
}

void GeometryMapDraw::blit_clipped(const Image& img, float qx, float qy, float qw, float qh,
                                   u32 tint) {
    if (!img.Valid() || qw <= 0.0f || qh <= 0.0f) {
        return;
    }
    const float l = std::max(qx, static_cast<float>(x));
    const float t = std::max(qy, static_cast<float>(y));
    const float r = std::min(qx + qw, static_cast<float>(x + rw));
    const float b = std::min(qy + qh, static_cast<float>(y + rh));
    if (r <= l || b <= t) {
        return;
    }
    const s32 dl = static_cast<s32>(std::lround(l)), dt = static_cast<s32>(std::lround(t));
    const s32 dw = static_cast<s32>(std::lround(r)) - dl,
              dh = static_cast<s32>(std::lround(b)) - dt;
    if (dw <= 0 || dh <= 0) {
        return;
    }
    const float fw = static_cast<float>(img.w), fh = static_cast<float>(img.h);
    const float u0 = (l - qx) / qw, v0 = (t - qy) / qh;
    const float u1 = (r - qx) / qw, v1 = (b - qy) / qh;
    canvas.DrawImageRegion(dl, dt, dw, dh, img, static_cast<s32>(u0 * fw),
                           static_cast<s32>(v0 * fh),
                           std::max(1, static_cast<s32>(std::lround((u1 - u0) * fw))),
                           std::max(1, static_cast<s32>(std::lround((v1 - v0) * fh))), tint);
}

void GeometryMapDraw::fill_clipped(float l, float t, float w, float h, u32 argb) {
    const s32 x0 = std::max(x, static_cast<s32>(std::lround(l)));
    const s32 y0 = std::max(y, static_cast<s32>(std::lround(t)));
    const s32 x1 = std::min(x + rw, static_cast<s32>(std::lround(l + w)));
    const s32 y1 = std::min(y + rh, static_cast<s32>(std::lround(t + h)));
    if (x1 > x0 && y1 > y0) {
        canvas.FillRect(x0, y0, x1 - x0, y1 - y0, argb);
    }
}

// Runtime 14: map "overlays" -- pictures placed in world space over the base picture and under
// the markers (into the canvas, which the GPU path composites over the map quad), panning and
// zooming with the map. An image still loading draws nothing yet.
void GeometryMapDraw::DrawOverlays() {
    if (map_extras == nullptr || !images) {
        return;
    }
    for (const auto& ov : map_extras->overlays) {
        if (!ov.show.Empty() && !GateOpen(ov.show, snapshot)) {
            continue;
        }
        const std::string* key = &ov.src;
        if (!ov.src_bind.empty()) {
            if (const auto t = snapshot.texts.find(ov.src_bind);
                t != snapshot.texts.end() && !t->second.empty()) {
                key = &t->second;
            }
        }
        if (key->empty() || ov.opacity <= 0.0f) {
            continue;
        }
        const std::shared_ptr<const Image> pic = images(*key);
        add_content(pic.get());
        if (pic == nullptr || !pic->Valid()) {
            continue;
        }
        const float l = to_x(std::min(ov.x0, ov.x1));
        const float r = to_x(std::max(ov.x0, ov.x1));
        const float t = to_y(std::max(ov.y0, ov.y1)); // world y grows upward
        const float b = to_y(std::min(ov.y0, ov.y1));
        const u32 a = static_cast<u32>(std::lround(ov.opacity * 255.0f));
        blit_clipped(*pic, l, t, r - l, b - t, (a << 24) | 0x00FFFFFFu);
    }
}

bool GeometryMapDraw::DrawBaseLayer() {
    // Aspect-correct raster: the .geo is normalised 0..65535 on each axis, so an image
    // whose width:height equals span_x:span_y un-stretches it. Longest side capped to
    // keep the cache small; the visible crop is upscaled a little at high zoom.
    // A prerendered underlay ignores the cap and draws at its native size (a 2048x2048
    // game map stays 2048x2048); the same cache entry serves the blit below.
    if (image_mode) {
        const std::shared_ptr<const Image> underlay = images ? images(base_image) : nullptr;
        if (underlay == nullptr || underlay->w == 0 || underlay->h == 0) {
            return false;
        }
        iw = static_cast<s32>(underlay->w);
        ih = static_cast<s32>(underlay->h);
    } else {
        // Longest side from the package (map.style.raster_px): the follow view
        // upscales the raster ~8x at 1536, so a package that wants crisper diagonals
        // asks for a finer raster instead of a blended edge.
        const s32 longest = manifest.map_style.raster_px;
        const float lf = static_cast<float>(longest);
        if (span_x >= span_y) {
            iw = longest;
            ih = std::max(16, static_cast<s32>(lf * span_y / span_x));
        } else {
            ih = longest;
            iw = std::max(16, static_cast<s32>(lf * span_x / span_y));
        }
    }
    const std::string key = image_mode ? base_image : fmt::format("map:{}@{}x{}", area, iw, ih);
    // The whole area's on-screen rectangle; the visible slice of the (possibly huge,
    // when zoomed) image is what gets blitted, and door boxes snap to its raster grid.
    img_l = to_x(min_x);
    img_t = to_y(max_y);
    dest_w = span_x * ppw;
    dest_h = span_y * ppw;
    // The GPU compositor builds/caches its map endpoints in PublishGpuComposite.
    // Calling the ordinary image provider here would still build the animated CPU
    // map before taking the GPU branch, defeating the purpose of the fade texture.
    const std::shared_ptr<const Image> drawn =
        draw_list == nullptr && images ? images(key) : nullptr;
    add_content(drawn.get());
    // A composite with extra levels is one packed picture: sample the level nearest the
    // on-screen scale (the smallest one still >= 0.8 source px per screen px).
    CompositeLevel level{0, 0, 0, 0, 1.0f};
    u32 packed_w = 0, packed_h = 0;
    bool packed = false;
    const std::string composite_name =
        image_mode && key.starts_with("composite:") ? key.substr(10) : std::string{};
    if (!composite_name.empty()) {
        if (const auto c = manifest.composites.find(composite_name);
            c != manifest.composites.end() && c->second && !c->second->levels.empty() &&
            dest_w > 0.0f) {
            const auto levels = CompositeLevels(*c->second, &packed_w, &packed_h);
            const float full_ratio = static_cast<float>(c->second->w) / dest_w;
            level = levels[0];
            for (const auto& lv : levels) {
                if (full_ratio * lv.scale >= 0.8f && lv.scale < level.scale) {
                    level = lv;
                }
            }
            packed = true;
        }
    }
    if (!packed) {
        const u32 w0 = drawn != nullptr ? drawn->w : static_cast<u32>(iw);
        const u32 h0 = drawn != nullptr ? drawn->h : static_cast<u32>(ih);
        level = {0, 0, w0, h0, 1.0f};
        packed_w = w0;
        packed_h = h0;
    }
    if (packed && (static_cast<u32>(iw) != packed_w || static_cast<u32>(ih) != packed_h)) {
        // not (yet) the packed picture the definition describes: draw it whole
        level = {0, 0, static_cast<u32>(iw), static_cast<u32>(ih), 1.0f};
        packed_w = static_cast<u32>(iw);
        packed_h = static_cast<u32>(ih);
    }
    if (draw_list != nullptr || (drawn != nullptr && drawn->Valid())) {
        const float vl = std::max(static_cast<float>(x), img_l);
        const float vt = std::max(static_cast<float>(y), img_t);
        const float vr = std::min(static_cast<float>(x + rw), img_l + dest_w);
        const float vb = std::min(static_cast<float>(y + rh), img_t + dest_h);
        if (vr > vl && vb > vt && dest_w > 0.0f && dest_h > 0.0f) {
            // EXACT fractional source window. Truncating to integer texels here shifted
            // the whole map up to one map-pixel row/col down-right (and stretched it a
            // hair), so world-anchored markers (doors/Samus) sat visibly high relative
            // to the drawn rooms once the room-clamped view stopped moving every frame.
            const float lx = static_cast<float>(level.x);
            const float ly = static_cast<float>(level.y);
            const float lw = static_cast<float>(level.w);
            const float lh = static_cast<float>(level.h);
            const float pw = static_cast<float>(std::max<u32>(1, packed_w));
            const float ph = static_cast<float>(std::max<u32>(1, packed_h));
            if (draw_list != nullptr) {
                draw_list->map_key = key;
                draw_list->active = true;
                // The canvas is composited over the map quads: clear what earlier
                // widgets painted here (a page background, a frame) so the map shows
                // at its place in the stack.
                canvas.ClearRect(x, y, rw, rh);
                draw_list->quads.push_back({0u, (lx + (vl - img_l) / dest_w * lw) / pw,
                                            (ly + (vt - img_t) / dest_h * lh) / ph,
                                            (lx + (vr - img_l) / dest_w * lw) / pw,
                                            (ly + (vb - img_t) / dest_h * lh) / ph, vl, vt, vr - vl,
                                            vb - vt, 1.0f, 1.0f, 1.0f, 1.0f, true});
            } else {
                const s32 su = static_cast<s32>(level.x) +
                               static_cast<s32>(std::lround((vl - img_l) / dest_w * lw));
                const s32 sv = static_cast<s32>(level.y) +
                               static_cast<s32>(std::lround((vt - img_t) / dest_h * lh));
                const s32 sw =
                    std::max<s32>(1, static_cast<s32>(std::lround((vr - vl) / dest_w * lw)));
                const s32 sh =
                    std::max<s32>(1, static_cast<s32>(std::lround((vb - vt) / dest_h * lh)));
                // A flat composite cross-fading after a reveal: the previous picture
                // underneath, the new one over it at the fade's alpha.
                u32 tint = 0xFFFFFFFFu;
                if (!composite_name.empty()) {
                    if (const auto f = snapshot.ints.find("@fade:" + composite_name);
                        f != snapshot.ints.end() && f->second < 1000) {
                        const std::shared_ptr<const Image> prev = images(key + "#prev");
                        add_content(prev.get());
                        if (prev != nullptr && prev->w == drawn->w && prev->h == drawn->h) {
                            canvas.DrawImageRegion(static_cast<s32>(vl), static_cast<s32>(vt),
                                                   static_cast<s32>(vr - vl),
                                                   static_cast<s32>(vb - vt), *prev, su, sv, sw, sh,
                                                   0xFFFFFFFFu);
                            const u32 a =
                                static_cast<u32>(std::clamp<s64>(f->second, 0, 1000) * 255 / 1000);
                            tint = (a << 24) | 0x00FFFFFFu;
                        }
                    }
                }
                canvas.DrawImageRegion(static_cast<s32>(vl), static_cast<s32>(vt),
                                       static_cast<s32>(vr - vl), static_cast<s32>(vb - vt), *drawn,
                                       su, sv, sw, sh, tint);
            }
            // Rooms still holding an uncollected item pulse slowly over the map, as
            // on the game's minimap: the composite's companion highlight rides the
            // same window at a blink alpha, using the package's pulse style.
            if (!image_mode) {
                const std::string pulse_key = "pulse:" + key.substr(4);
                // On the GPU path the runtime obtains the pulse beside the settled
                // current endpoint. It removes this quad when no pulse texture exists.
                const std::shared_ptr<const Image> pulse =
                    draw_list == nullptr && images ? images(pulse_key) : nullptr;
                if (draw_list != nullptr || (pulse != nullptr && pulse->Valid())) {
                    uses_clock = true; // BlinkAlpha(snapshot.tick) below
                    // Period and peak come from the package (map.style.marker_pulse_period
                    // / _peak, default 90 ticks and 0.7); see MapStyle.
                    const float pa =
                        BlinkAlpha(snapshot.tick, manifest.map_style.marker_pulse_period, 0.0f) *
                        manifest.map_style.marker_pulse_peak;
                    if (draw_list != nullptr) {
                        draw_list->pulse_key = pulse_key;
                        draw_list->quads.push_back({3u, (vl - img_l) / dest_w,
                                                    (vt - img_t) / dest_h, (vr - img_l) / dest_w,
                                                    (vb - img_t) / dest_h, vl, vt, vr - vl, vb - vt,
                                                    pa});
                    } else {
                        const s32 su = static_cast<s32>(
                            std::lround((vl - img_l) / dest_w * static_cast<float>(pulse->w)));
                        const s32 sv = static_cast<s32>(
                            std::lround((vt - img_t) / dest_h * static_cast<float>(pulse->h)));
                        const s32 sw = std::max<s32>(
                            1, static_cast<s32>(
                                   std::lround((vr - vl) / dest_w * static_cast<float>(pulse->w))));
                        const s32 sh = std::max<s32>(
                            1, static_cast<s32>(
                                   std::lround((vb - vt) / dest_h * static_cast<float>(pulse->h))));
                        const u32 a = static_cast<u32>(std::clamp(pa, 0.0f, 1.0f) * 255.0f);
                        canvas.DrawImageRegion(static_cast<s32>(vl), static_cast<s32>(vt),
                                               static_cast<s32>(vr - vl), static_cast<s32>(vb - vt),
                                               *pulse, su, sv, sw, sh, (a << 24) | 0x00FFFFFFu);
                    }
                }
            }
        }
    }
    return true;
}

// Map groups (the page's visibility options): a gated group is not drawn and its
// markers cannot be tapped; its opacity multiplies the member's own.
bool GeometryMapDraw::group_visible(const std::string& group, float& alpha) const {
    if (group.empty() || map_extras == nullptr) {
        return true;
    }
    const auto g = map_extras->groups.find(group);
    if (g == map_extras->groups.end()) {
        return true;
    }
    if (!g->second.show.Empty() && !GateOpen(g->second.show, snapshot)) {
        return false;
    }
    if (g->second.min_zoom > 0.0f) {
        // Live view zoom (1 = the default window): names that would overlap at
        // the whole-island view wait for a pinch.
        const float zoom =
            widget.pan_zoom
                ? std::clamp(view.zoom, std::min(zoom_min, widget.max_zoom), widget.max_zoom)
                : 1.0f;
        if (zoom + 1e-4f < g->second.min_zoom) {
            return false;
        }
    }
    if (!g->second.hide.Empty()) {
        if (g->second.hide_in.empty()) {
            if (GateOpen(g->second.hide, snapshot)) {
                return false;
            }
        } else {
            const auto v = SnapshotNumber(snapshot, g->second.hide.point);
            if (v.has_value() && std::ranges::find(g->second.hide_in, static_cast<s64>(*v)) !=
                                     g->second.hide_in.end()) {
                return false;
            }
        }
    }
    alpha *= g->second.opacity;
    return true;
}

void GeometryMapDraw::BeginRecord() {
    if (map_records != nullptr && ppw > 0.0f) {
        // RenderPage clears the records only on a whole-canvas draw (see its top), so a
        // partial redraw that reaches this widget (it was inside the dirty rect) must
        // replace the widget's previous entry, not append a second one: the tap hit-test
        // would take the last match anyway, but the duplicates would pile up, one per
        // redraw, while the page is open. On a full redraw the entry is already gone and
        // this finds nothing.
        std::erase_if(*map_records,
                      [&](const MapDrawRecord& r) { return r.widget_index == widget_index; });
        MapDrawRecord& r = map_records->emplace_back();
        r.page_id = page.id;
        r.widget_index = widget_index;
        r.area = area;
        r.rect = {x, y, rw, rh};
        r.cx = cx;
        r.cy = cy;
        r.ppw = ppw;
        r.min_x = min_x;
        r.min_y = min_y;
        r.max_x = max_x;
        r.max_y = max_y;
        r.render_serial = CurrentRenderSerial();
        record = &r;
    }
}

// Region names: the page font with an outline, fixed size, centred on the spot,
// each rendered once into a cached picture and blitted (clipped to the widget).
void GeometryMapDraw::draw_labels() {
    labels_drawn = true;
    if (geo->second.labels.empty()) {
        return;
    }
    const MapWidgetExtras::LabelStyle style =
        map_extras != nullptr ? map_extras->label_style : MapWidgetExtras::LabelStyle{};
    for (const auto& label : geo->second.labels) {
        if ((!label.show.Empty() && !GateOpen(label.show, snapshot)) ||
            (!label.hide.Empty() && GateOpen(label.hide, snapshot))) {
            continue;
        }
        float alpha = label.opacity * style.opacity;
        if (!group_visible(label.group, alpha) || alpha <= 0.0f) {
            continue;
        }
        const std::string* text = &label.text;
        // texts() returns a shared_ptr (see TextProvider, mod_ui.h); text_owner keeps the
        // string alive while the raw `text` pointer below is in use.
        std::shared_ptr<const std::string> text_owner;
        if (!label.text_src.empty()) {
            text_owner = texts ? texts(label.text_src) : nullptr;
            text = text_owner.get();
        }
        if (text == nullptr || text->empty()) {
            continue;
        }
        const s32 scale = label.text_scale > 0 ? label.text_scale : style.text_scale;
        const std::shared_ptr<const LabelBitmap> bmp =
            MapLabelBitmap(canvas, *text, scale, label.color.value_or(style.color),
                           label.outline_color.value_or(style.outline_color),
                           label.outline >= 0 ? label.outline : style.outline);
        if (bmp == nullptr || !bmp->image.Valid()) {
            continue;
        }
        const s32 qx = static_cast<s32>(std::lround(to_x(label.x))) - bmp->ox - bmp->text_w / 2;
        const s32 qy = static_cast<s32>(std::lround(to_y(label.y))) - bmp->oy - (scale * 5) / 2;
        const s32 bw = static_cast<s32>(bmp->image.w);
        const s32 bh = static_cast<s32>(bmp->image.h);
        const s32 l = std::max(qx, x), t = std::max(qy, y);
        const s32 r = std::min(qx + bw, x + rw), b = std::min(qy + bh, y + rh);
        if (r <= l || b <= t) {
            continue;
        }
        const u32 a = static_cast<u32>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        canvas.DrawImageRegion(l, t, r - l, b - t, bmp->image, l - qx, t - qy, r - l, b - t,
                               (a << 24) | 0x00FFFFFFu);
    }
}

void GeometryMapDraw::DrawAtlasLayer() {
    if (draw_list != nullptr) {
        draw_list->atlas_key = manifest.icon_atlas;
    }
    aw = static_cast<float>(atlas->w);
    ah = static_cast<float>(atlas->h);
    cp = static_cast<float>(manifest.icon_cell);
    // Station/item icons are drawn large so they read at a glance; doors and beam
    // blockages are structural and numerous, so they get their own size (closer
    // to the in-game minimap) and a box-stretched door spans its connector.
    ItemIconSize = manifest.map_style.item_icon;
    DoorIconSize = manifest.map_style.door_icon;
    // Uncollected items blink white-ish on the game's minimap (a scalar ping-pong,
    // ~1 Hz); the same pulse dims their icon here.
    item_blink = BlinkAlpha(snapshot.tick, std::max<u32>(2, manifest.map_style.item_blink_period),
                            manifest.map_style.item_blink_low);
    // The markers' live flags (hidden / opened / collected / unveiled / veiled) are read
    // straight off the Manifest, not through the snapshot, while
    // ModRuntime::UpdateHiddenMarkers writes them holding map_state_mutex for its whole body.
    // The caller passes that same mutex as manifest_markers_mutex; it is held from here to the
    // end of this layer (coarse, like the other map_state_mutex users: nothing here is hot
    // enough to need finer locking).
    std::unique_lock<std::recursive_mutex> markers_lk;
    if (manifest_markers_mutex != nullptr) {
        markers_lk = std::unique_lock<std::recursive_mutex>{*manifest_markers_mutex};
    }
    DrawAreaMarkers();
    draw_labels();
    DrawDynamicMarkers();
    DrawCustomMarkers();
    // The player, live -- the game's own Samus icon from its atlas, drawn last and
    // larger than the area's item icons so it reads at a glance, pulsing like the
    // minimap's SAMUS_GLOW. (With the aux surface B8G8R8A8, the atlas colours come
    // through correctly, so this shows red as in the game.) A red diamond is the
    // fallback if the atlas or its cell is missing.
    PlayerIcon = manifest.map_style.marker_icon;
    DrawLivePlayer();
    DrawSecondActor();
}

// One path for every atlas glyph (markers, Samus, EMMI): clip the destination
// rect to the widget -- rescaling the source window by the same fractions --
// then emit a quad or blit. Without the clip, icons at the map edge overdrew
// the HUD panel above and the margins around the map.
// `to_canvas`: paint into the canvas even on the GPU path (the HUD canvas is
// composited over every quad, so a glyph that must sit above the map labels --
// which live in the canvas -- has to be painted there too).
void GeometryMapDraw::emit_atlas_tinted(float ux, float uy, float uw, float uh, float qx, float qy,
                                        float qw, float qh, float alpha, float cr, float cg,
                                        float cb, bool to_canvas) {
    const float l = std::max(qx, static_cast<float>(x));
    const float t = std::max(qy, static_cast<float>(y));
    const float r = std::min(qx + qw, static_cast<float>(x + rw));
    const float b = std::min(qy + qh, static_cast<float>(y + rh));
    if (r <= l || b <= t || qw <= 0.0f || qh <= 0.0f) {
        return;
    }
    const float cux = ux + uw * (l - qx) / qw;
    const float cuy = uy + uh * (t - qy) / qh;
    const float cuw = uw * (r - l) / qw;
    const float cuh = uh * (b - t) / qh;
    if (draw_list != nullptr && !to_canvas) {
        draw_list->quads.push_back({1u, cux / aw, cuy / ah, (cux + cuw) / aw, (cuy + cuh) / ah, l,
                                    t, r - l, b - t, alpha, cr, cg, cb});
    } else {
        const auto ch = [](float v) {
            return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        const u32 tint = (ch(alpha) << 24) | (ch(cr) << 16) | (ch(cg) << 8) | ch(cb);
        canvas.DrawImageRegion(static_cast<s32>(l), static_cast<s32>(t), static_cast<s32>(r - l),
                               static_cast<s32>(b - t), *atlas, static_cast<s32>(cux),
                               static_cast<s32>(cuy), std::max(1, static_cast<s32>(cuw)),
                               std::max(1, static_cast<s32>(cuh)), tint);
    }
}

void GeometryMapDraw::emit_atlas(float ux, float uy, float uw, float uh, float qx, float qy,
                                 float qw, float qh, float alpha) {
    emit_atlas_tinted(ux, uy, uw, uh, qx, qy, qw, qh, alpha, 1.0f, 1.0f, 1.0f);
}

void GeometryMapDraw::cell_uv(const std::pair<s32, s32>& cell, float& ux, float& uy) const {
    ux = static_cast<float>(cell.second) * cp;
    uy = static_cast<float>(cell.first) * cp;
}

void GeometryMapDraw::DrawAreaMarkers() {
    for (const auto& marker : geo->second.markers) {
        if (std::ranges::find(widget.hidden_icons, marker.icon) != widget.hidden_icons.end()) {
            continue; // retained in area data, omitted by this map presentation
        }
        if (marker.hidden) {
            continue; // destroyed shield/destructible: its icon leaves the map
        }
        if ((!marker.show.Empty() && !GateOpen(marker.show, snapshot)) ||
            (!marker.hide.Empty() && GateOpen(marker.hide, snapshot))) {
            continue; // the game's own show rule (a story flag) says not yet
        }
        float marker_alpha = marker.opacity;
        if (!group_visible(marker.group, marker_alpha) || marker_alpha <= 0.0f) {
            continue;
        }
        // An opened door shows an "opened" atlas cell, and a collected item shows its
        // "acquired" glyph and stops pulsing. The package names those cells (marker
        // .open_icon / .collected_icon); left unset, each falls back to map.style's door /
        // collected naming, whose defaults are Dread's own minimap atlas (the halves
        // DoorOpenedL/R, DoorEmmyOpen, DoorThermalOpen and DoorThermalTrapOpen for exactly
        // that state, which <actor>:DOOR:Opened reports -- written by the door component,
        // main+0x8DA688 -- so a Dread package need not declare them).
        const MapStyle& style = manifest.map_style;
        const bool is_door =
            !style.door_prefix.empty() && marker.icon.starts_with(style.door_prefix);
        std::string shown = marker.icon;
        if (marker.opened) {
            std::string open_name = marker.open_icon;
            if (open_name.empty() && is_door) {
                if (!style.door_closed_suffix.empty() &&
                    marker.icon.ends_with(style.door_closed_suffix)) {
                    const size_t base = marker.icon.size() - style.door_closed_suffix.size();
                    open_name = marker.icon.substr(0, base) + style.door_open_suffix;
                } else if (marker.icon.ends_with("L")) {
                    open_name = style.door_opened_left;
                } else if (marker.icon.ends_with("R")) {
                    open_name = style.door_opened_right;
                }
            }
            if (!open_name.empty() && manifest.icon_cells.contains(open_name)) {
                shown = open_name;
            }
        }
        if (marker.collected) {
            const std::string want = !marker.collected_icon.empty()
                                         ? marker.collected_icon
                                         : marker.icon + style.collected_suffix;
            if (manifest.icon_cells.contains(want)) {
                shown = want;
            } else if (manifest.icon_cells.contains(style.collected_fallback)) {
                shown = style.collected_fallback;
            }
        }
        const auto cell = manifest.icon_cells.find(shown);
        if (cell == manifest.icon_cells.end()) {
            continue;
        }
        const std::string* const icon_name = &shown;
        // Fog of war: an icon stays hidden until its cell is revealed, so items and
        // doors appear as the map does rather than all at once. Image-mode areas
        // have no grid, so their markers are always visible. A "collectible" icon
        // (marker.collectible, default kind == map.style collectible_kind) also waits for the
        // game to unveil it (seen, or its hiding block found); until then its room
        // pulses instead, as on the minimap.
        if (!image_mode && is_visited && !is_visited(area, marker.x, marker.y)) {
            continue;
        }
        const bool collectible = marker.collectible.value_or(marker.kind == style.collectible_kind);
        if (collectible && marker.veiled && !marker.collected) {
            continue; // hidden in a block the player has not found: no icon
        }
        const float mx = to_x(marker.x);
        const float my = to_y(marker.y);
        const bool structural = marker.structural.value_or(
            is_door || std::ranges::any_of(style.structural_prefixes, [&](const std::string& p) {
                return !p.empty() && marker.icon.starts_with(p);
            }));
        const s32 sz = structural ? DoorIconSize : marker.size > 0 ? marker.size : ItemIconSize;
        if (!marker.has_box && !visible(mx, my)) {
            continue;
        }
        // Screen rect for the glyph. A door/blockage that carries a world box is
        // stretched to fill that box (via the map's own to_x/to_y), so it spans the
        // connector at the current zoom -- like the in-game minimap -- instead of a
        // fixed square. A small floor keeps a thin door visible when zoomed out.
        float qx, qy, qw, qh;
        if (marker.has_box) {
            // Snap the box to the RASTER's block grid, replicating the geo bake
            // (floor to u16) + rasteriser (integer to_x/to_y, inclusive fill)
            // exactly. A continuous mapping left up to half a block of drawn pipe
            // uncovered below/beside the door; snapping covers precisely the map
            // pixels the box rasterises to.
            const auto qv = [](float w, float mn, float span) {
                const float f = std::clamp((w - mn) / span, 0.0f, 1.0f);
                return static_cast<s64>(f * 65535.0f);
            };
            const s64 c0 = qv(marker.bx0, min_x, span_x) * (iw - 1) / 65535;
            const s64 c1 = qv(marker.bx1, min_x, span_x) * (iw - 1) / 65535;
            const s64 r0 = (ih - 1) - qv(marker.by1, min_y, span_y) * (ih - 1) / 65535;
            const s64 r1 = (ih - 1) - qv(marker.by0, min_y, span_y) * (ih - 1) / 65535;
            qx = img_l + static_cast<float>(c0) / static_cast<float>(iw) * dest_w;
            qy = img_t + static_cast<float>(r0) / static_cast<float>(ih) * dest_h;
            qw = std::max(static_cast<float>(c1 - c0 + 1) / static_cast<float>(iw) * dest_w, 4.0f);
            qh = std::max(static_cast<float>(r1 - r0 + 1) / static_cast<float>(ih) * dest_h, 4.0f);
        } else {
            // Point icon (item/station) or a boxless door: fixed pixel square. A
            // door's L "[" and R "]" halves share a world position, so nudge them a
            // quarter apart to abut as "[]", and up a little onto the wall line.
            float dx = 0.0f, dy = 0.0f;
            if (is_door) {
                dy = -static_cast<float>(sz) / 6.0f;
                if (marker.icon.back() == 'L')
                    dx = -static_cast<float>(sz) / 6.0f;
                else if (marker.icon.back() == 'R')
                    dx = static_cast<float>(sz) / 6.0f;
            }
            qx = mx - static_cast<float>(sz) * 0.5f + dx;
            qy = my - static_cast<float>(sz) * 0.5f + dy;
            qw = static_cast<float>(sz);
            qh = static_cast<float>(sz);
        }
        // For a box-stretched glyph, sample only the glyph's INK bbox (baked from
        // the atlas, exclusive max): the cell's transparent margins would otherwise
        // shrink the visible door to ~a third of its box, leaving the pipe
        // uncovered.
        float ux, uy;
        cell_uv(cell->second, ux, uy);
        float uw = cp, uh = cp;
        if (marker.has_box) {
            if (const auto ink = manifest.icon_ink.find(*icon_name);
                ink != manifest.icon_ink.end()) {
                ux += static_cast<float>(ink->second[0]);
                uy += static_cast<float>(ink->second[1]);
                uw = static_cast<float>(std::max(1, ink->second[2] - ink->second[0]));
                uh = static_cast<float>(std::max(1, ink->second[3] - ink->second[1]));
            }
        }
        const float alpha = collectible && !marker.collected ? item_blink : 1.0f;
        if (collectible && !marker.collected) {
            uses_clock = true;
        }
        emit_atlas(ux, uy, uw, uh, qx, qy, qw, qh, alpha * marker_alpha);
    }
}

// Markers read from array points (the game's custom pins): fixed size, the
// selected slot larger (over its ring) and drawn last; every on-screen one is
// recorded for the tap hit-test. They are painted above the labels on both
// paths (the canvas on the GPU path).
void GeometryMapDraw::emit_pin(float ux, float uy, float uw, float uh, float qx, float qy, float qw,
                               float qh, float alpha) {
    emit_atlas_tinted(ux, uy, uw, uh, qx, qy, qw, qh, alpha, 1.0f, 1.0f, 1.0f, true);
}

void GeometryMapDraw::DrawDynamicMarkers() {
    for (const auto& dm : geo->second.dynamic_markers) {
        if ((!dm.show.Empty() && !GateOpen(dm.show, snapshot)) ||
            (!dm.hide.Empty() && GateOpen(dm.hide, snapshot))) {
            continue;
        }
        float dm_alpha = dm.opacity;
        if (!group_visible(dm.group, dm_alpha) || dm_alpha <= 0.0f) {
            continue;
        }
        const s64 selected = snapshot.GetInt("@map_sel:" + dm.group, -1);
        const s32 base_sz = dm.size > 0 ? dm.size : ItemIconSize;
        const s32 sel_sz = dm.selected_size > 0 ? dm.selected_size : base_sz;
        const auto slot_key = [](const std::string& pattern, s64 i) {
            std::string out = pattern;
            if (const size_t at = out.find("{i}"); at != std::string::npos) {
                out.replace(at, 3, std::to_string(i));
            }
            return out;
        };
        // Runtime 14: a slot's look beyond its icon.
        struct Look {
            bool dim{false};
            u32 tint{0};  ///< 0 = none
            u32 frame{0}; ///< 0 = none
            std::optional<float> bar;
        };
        struct Deferred {
            float qx, qy, sz;
            std::pair<s32, s32> cell;
            std::shared_ptr<const Image> pic;
            Look look;
        };
        const bool have_atlas = atlas != nullptr && manifest.icon_cell > 0 && cp > 0.0f;
        const auto slot_int = [&](const std::string& pattern, s64 i) -> std::optional<s64> {
            if (pattern.empty()) {
                return std::nullopt;
            }
            const auto v = SnapshotNumber(snapshot, slot_key(pattern, i));
            return v ? std::optional<s64>{static_cast<s64>(*v)} : std::nullopt;
        };
        const auto look_of = [&](s64 i) {
            Look look;
            if (const auto d = slot_int(dm.dim_bind, i)) {
                look.dim = *d != 0;
            }
            look.tint = static_cast<u32>(slot_int(dm.tint_bind, i).value_or(0));
            look.frame = static_cast<u32>(slot_int(dm.frame_color_bind, i).value_or(0));
            if (const auto v = slot_int(dm.bar_bind, i)) {
                const s64 mx =
                    dm.bar_max_bind.empty() ? dm.bar_max : slot_int(dm.bar_max_bind, i).value_or(0);
                if (mx > 0) {
                    look.bar =
                        std::clamp(static_cast<float>(*v) / static_cast<float>(mx), 0.0f, 1.0f);
                }
            }
            return look;
        };
        // One slot: the bar beneath the icon, the icon (picture or atlas cell) tinted / dimmed,
        // then its frame.
        const auto draw_slot = [&](float qx, float qy, float sz, const std::pair<s32, s32>* cell,
                                   const Image* pic, const Look& look) {
            float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = dm_alpha;
            if (look.tint != 0) {
                cr = static_cast<float>((look.tint >> 16) & 0xFF) / 255.0f;
                cg = static_cast<float>((look.tint >> 8) & 0xFF) / 255.0f;
                cb = static_cast<float>(look.tint & 0xFF) / 255.0f;
                ca *= static_cast<float>(look.tint >> 24) / 255.0f;
            }
            if (look.dim) {
                cr *= 0.45f;
                cg *= 0.45f;
                cb *= 0.45f;
            }
            if (look.bar.has_value()) {
                const float bh = dm.bar_h > 0 ? static_cast<float>(dm.bar_h)
                                              : std::max(3.0f, std::round(sz / 8.0f));
                fill_clipped(qx, qy + sz + 1.0f, sz, bh, dm.bar_bg);
                fill_clipped(qx, qy + sz + 1.0f, std::round(sz * *look.bar), bh, dm.bar_color);
            }
            if (pic != nullptr) {
                const auto ch = [](float v) {
                    return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                };
                blit_clipped(*pic, qx, qy, sz, sz,
                             (ch(ca) << 24) | (ch(cr) << 16) | (ch(cg) << 8) | ch(cb));
            } else if (cell != nullptr) {
                float ux, uy;
                cell_uv(*cell, ux, uy);
                emit_atlas_tinted(ux, uy, cp, cp, qx, qy, sz, sz, ca, cr, cg, cb, true);
            }
            if (look.frame != 0) {
                const float f = dm.frame_px > 0 ? static_cast<float>(dm.frame_px)
                                                : std::max(2.0f, std::round(sz / 16.0f));
                fill_clipped(qx, qy, sz, f, look.frame);
                fill_clipped(qx, qy + sz - f, sz, f, look.frame);
                fill_clipped(qx, qy + f, f, sz - 2.0f * f, look.frame);
                fill_clipped(qx + sz - f, qy + f, f, sz - 2.0f * f, look.frame);
            }
        };
        std::optional<Deferred> deferred;
        // Positions: the float an f32 point publishes, not its truncated int.
        const auto position = [&](const std::string& key) -> std::optional<f64> {
            if (const auto f = snapshot.floats.find(key); f != snapshot.floats.end()) {
                return f->second;
            }
            return SnapshotNumber(snapshot, key);
        };
        for (s64 i = 0; i < dm.count && i < 4096; ++i) {
            const auto vx = position(slot_key(dm.x, i));
            const auto vy = position(slot_key(dm.y, i));
            if (!vx || !vy || !std::isfinite(*vx) || !std::isfinite(*vy)) {
                continue;
            }
            const std::string* icon = &dm.icon;
            if (!dm.kind.empty()) {
                const auto kv = snapshot.ints.find(slot_key(dm.kind, i));
                if (kv == snapshot.ints.end() ||
                    (dm.has_hide_kind && kv->second == dm.hide_when_kind)) {
                    continue;
                }
                const auto mapped = dm.icon_by_kind.find(kv->second);
                icon = mapped != dm.icon_by_kind.end() ? &mapped->second : &dm.icon_default;
            }
            // Runtime 14: a per-slot picture (a text point naming an image key) wins over the
            // atlas icon; one still loading draws nothing yet.
            std::shared_ptr<const Image> pic;
            if (!dm.icon_src_bind.empty()) {
                if (const auto t = snapshot.texts.find(slot_key(dm.icon_src_bind, i));
                    t != snapshot.texts.end() && !t->second.empty()) {
                    pic = images ? images(t->second) : nullptr;
                    add_content(pic.get());
                    if (pic == nullptr || !pic->Valid()) {
                        continue;
                    }
                }
            }
            const std::pair<s32, s32>* cellp = nullptr;
            if (pic == nullptr) {
                if (icon->empty() || !have_atlas) {
                    continue;
                }
                const auto cell = manifest.icon_cells.find(*icon);
                if (cell == manifest.icon_cells.end()) {
                    continue;
                }
                cellp = &cell->second;
            }
            const float wx = static_cast<float>(*vx) * dm.scale_x + dm.offset_x;
            const float wy = static_cast<float>(*vy) * dm.scale_y + dm.offset_y;
            const bool is_sel = selected == i;
            // A world-sized marker scales with the zoom (ppw includes it).
            const float sz = dm.size_world > 0.0f ? dm.size_world * ppw
                                                  : static_cast<float>(is_sel ? sel_sz : base_sz);
            const float qx = to_x(wx) - dm.anchor_x * sz;
            const float qy = to_y(wy) - dm.anchor_y * sz;
            const float hx = qx + sz * 0.5f, hy = qy + sz * 0.5f;
            if (record != nullptr && visible(hx, hy)) {
                record->hits.push_back({dm.group, static_cast<s32>(i), hx, hy, wx, wy,
                                        static_cast<float>(*vx), static_cast<float>(*vy)});
            }
            const Look look = look_of(i);
            if (is_sel) {
                deferred = Deferred{qx,  qy,  sz, cellp != nullptr ? *cellp : std::pair<s32, s32>{},
                                    pic, look};
                continue;
            }
            draw_slot(qx, qy, sz, cellp, pic.get(), look);
        }
        if (deferred) {
            const float hx = deferred->qx + deferred->sz * 0.5f;
            const float hy = deferred->qy + deferred->sz * 0.5f;
            if (!dm.selected_icon.empty() && have_atlas) {
                if (const auto ring = manifest.icon_cells.find(dm.selected_icon);
                    ring != manifest.icon_cells.end()) {
                    const float rs = dm.selected_icon_size > 0
                                         ? static_cast<float>(dm.selected_icon_size)
                                         : deferred->sz * 1.5f;
                    float ux, uy;
                    cell_uv(ring->second, ux, uy);
                    emit_pin(ux, uy, cp, cp, hx - rs * 0.5f, hy - rs * 0.5f, rs, rs, dm_alpha);
                }
            }
            draw_slot(deferred->qx, deferred->qy, deferred->sz,
                      deferred->pic != nullptr ? nullptr : &deferred->cell, deferred->pic.get(),
                      deferred->look);
        }
    }
}

// The player's own map markers (the game's custom markers): the game's back
// plate in the marker's colour with the white glyph over it, a touch larger
// than an item icon so they read as the player's own notes.
void GeometryMapDraw::DrawCustomMarkers() {
    // The atlas cell names are package-declared (map.style.custom_marker_back_icon /
    // custom_marker_icon).
    const auto back = manifest.icon_cells.find(manifest.map_style.custom_marker_back_icon);
    const auto glyph = manifest.icon_cells.find(manifest.map_style.custom_marker_icon);
    const float ms = static_cast<float>(ItemIconSize) * 1.25f;
    // A debug-log dedup shared by every thread that renders, like FrameView's
    // last_view_log: an atomic exchange makes the compare-and-update one read-modify-write.
    // Approximate under a race is fine (at worst one duplicate or missed line).
    static std::atomic<size_t> logged_markers{~size_t{0}};
    if (const size_t want = snapshot.custom_markers.size();
        logged_markers.exchange(want, std::memory_order_relaxed) != want) {
        for (const auto& cm : snapshot.custom_markers) {
            LOG_INFO(Core,
                     "DSMod marker draw: world ({:.0f},{:.0f}) -> canvas "
                     "({:.0f},{:.0f}) widget [{},{} {}x{}] back={} glyph={}",
                     cm.x, cm.y, to_x(cm.x), to_y(cm.y), x, y, rw, rh,
                     back != manifest.icon_cells.end(), glyph != manifest.icon_cells.end());
        }
    }
    for (const auto& cm : snapshot.custom_markers) {
        const float mx = to_x(cm.x), my = to_y(cm.y);
        if (!visible(mx, my)) {
            continue;
        }
        const u32 marker_color = manifest.map_style.marker_colors[static_cast<size_t>(std::clamp(
            cm.color, 0, static_cast<s32>(manifest.map_style.marker_colors.size()) - 1))];
        // The atlas' "back" is a dark-grey copy of the chevron a hair larger
        // (an outline), and the marker colour lives on the chevron itself in
        // the game's dialog -- so the back stays as drawn and the white glyph
        // takes the slot colour.
        float ux, uy;
        if (back != manifest.icon_cells.end()) {
            cell_uv(back->second, ux, uy);
            emit_atlas(ux, uy, cp, cp, mx - ms * 0.5f, my - ms * 0.5f, ms, ms, 1.0f);
        }
        if (glyph != manifest.icon_cells.end()) {
            cell_uv(glyph->second, ux, uy);
            emit_atlas_tinted(ux, uy, cp, cp, mx - ms * 0.5f, my - ms * 0.5f, ms, ms, 1.0f,
                              ((marker_color >> 16) & 0xFF) / 255.0f,
                              ((marker_color >> 8) & 0xFF) / 255.0f,
                              (marker_color & 0xFF) / 255.0f);
        }
    }
}

void GeometryMapDraw::DrawLivePlayer() {
    if (have_player && !geo->second.no_pin) {
        // Fog of war: record that the player is here, in exactly the area being
        // drawn, so the map reveals as they explore. The world coords are the
        // marker's, so the reveal lines up with where the marker sits. Image-mode
        // areas keep no grid, so the visit is not recorded.
        if (report_visit && !image_mode) {
            report_visit(area, wx_player, wy_player);
        }
        const float mx = to_x(wx_player);
        const float my = to_y(wy_player);
        const auto cell = widget.marker_icon.empty() ? manifest.icon_cells.end()
                                                     : manifest.icon_cells.find(widget.marker_icon);
        if (cell != manifest.icon_cells.end()) {
            float ux, uy;
            cell_uv(cell->second, ux, uy);
            const float qx = mx - static_cast<float>(PlayerIcon) * 0.5f;
            const float qy = my - static_cast<float>(PlayerIcon) * 0.5f;
            const float qs = static_cast<float>(PlayerIcon);
            // emit_atlas_tinted's own clip to the widget; the blit covers [s32(l), s32(r)).
            const float l = std::max(qx, static_cast<float>(x));
            const float t = std::max(qy, static_cast<float>(y));
            const float r = std::min(qx + qs, static_cast<float>(x + rw));
            const float b = std::min(qy + qs, static_cast<float>(y + rh));
            if (r > l && b > t) {
                const s32 bx = static_cast<s32>(std::floor(l)), by = static_cast<s32>(std::floor(t));
                add_marker_px(bx, by, static_cast<s32>(std::ceil(r)) - bx,
                              static_cast<s32>(std::ceil(b)) - by);
            }
            emit_atlas(ux, uy, cp, cp, qx, qy, qs, qs,
                       BlinkAlpha(snapshot.tick,
                                  std::max<u32>(2, manifest.map_style.player_blink_period),
                                  manifest.map_style.player_blink_low));
        } else if (visible(mx, my) && widget.marker_src.empty()) {
            // (a marker_src picture is drawn below instead of the diamond)
            DrawFallbackPin(static_cast<s32>(mx), static_cast<s32>(my));
        }
    }
}

// A second live actor (Dread's roaming EMMI): the package names the snapshot
// floats and the atlas icon, so the renderer carries no game names.
void GeometryMapDraw::DrawSecondActor() {
    if (!widget.actor_x_bind.empty() && !widget.actor_icon.empty()) {
        const auto ex = snapshot.floats.find(widget.actor_x_bind);
        const auto ey = snapshot.floats.find(widget.actor_y_bind);
        const auto ecell = manifest.icon_cells.find(widget.actor_icon);
        if (ex != snapshot.floats.end() && ey != snapshot.floats.end() &&
            ecell != manifest.icon_cells.end()) {
            const float actor_x = static_cast<float>(ex->second) * widget.marker_scale;
            const float actor_y = static_cast<float>(ey->second) * widget.marker_scale;
            const bool actor_visible =
                !widget.actor_reveal_required || (is_visited && is_visited(area, actor_x, actor_y));
            if (actor_visible) {
                const float emx = to_x(actor_x);
                const float emy = to_y(actor_y);
                float ux, uy;
                cell_uv(ecell->second, ux, uy);
                // Larger than Samus's marker and breathing in size rather than
                // blinking, so the threat reads at a glance: 1.3x..1.8x over 40 ticks.
                const float ph = BlinkAlpha(snapshot.tick, 40, 0.0f);
                const float es = static_cast<float>(PlayerIcon) * (1.3f + 0.5f * ph);
                uses_clock = true; // the breathing size
                emit_atlas(ux, uy, cp, cp, emx - es * 0.5f, emy - es * 0.5f, es, es, 1.0f);
            }
        }
    }
}

// The live marker as a picture of its own (marker_src): anchored on the player,
// clipped to the widget. On the GPU path it rides the atlas slot when the package
// has no atlas, else it is painted into the HUD canvas over the map quads.
void GeometryMapDraw::DrawMarkerPicture() {
    if (have_player && !geo->second.no_pin && !widget.marker_src.empty()) {
        const std::shared_ptr<const Image> mk = images ? images(widget.marker_src) : nullptr;
        add_content(mk.get());
        const bool slot_free = manifest.icon_atlas.empty() || manifest.icon_cell <= 0;
        if (mk != nullptr && mk->Valid()) {
            const float mw =
                widget.marker_size[0] > 0.0f ? widget.marker_size[0] : static_cast<float>(mk->w);
            const float mh =
                widget.marker_size[1] > 0.0f ? widget.marker_size[1] : static_cast<float>(mk->h);
            const float qx = to_x(wx_player) - widget.marker_anchor[0] * mw;
            const float qy = to_y(wy_player) - widget.marker_anchor[1] * mh;
            const float l = std::max(qx, static_cast<float>(x));
            const float t = std::max(qy, static_cast<float>(y));
            const float r = std::min(qx + mw, static_cast<float>(x + rw));
            const float b = std::min(qy + mh, static_cast<float>(y + rh));
            if (r > l && b > t) {
                const float u0 = (l - qx) / mw, v0 = (t - qy) / mh;
                const float u1 = (r - qx) / mw, v1 = (b - qy) / mh;
                if (draw_list != nullptr && slot_free) {
                    draw_list->atlas_key = widget.marker_src;
                    draw_list->quads.push_back({1u, u0, v0, u1, v1, l, t, r - l, b - t, 1.0f});
                } else {
                    const float fw = static_cast<float>(mk->w);
                    const float fh = static_cast<float>(mk->h);
                    add_marker_px(
                        static_cast<s32>(std::lround(l)), static_cast<s32>(std::lround(t)),
                        static_cast<s32>(std::lround(r)) - static_cast<s32>(std::lround(l)),
                        static_cast<s32>(std::lround(b)) - static_cast<s32>(std::lround(t)));
                    canvas.DrawImageRegion(
                        static_cast<s32>(std::lround(l)), static_cast<s32>(std::lround(t)),
                        static_cast<s32>(std::lround(r)) - static_cast<s32>(std::lround(l)),
                        static_cast<s32>(std::lround(b)) - static_cast<s32>(std::lround(t)), *mk,
                        static_cast<s32>(u0 * fw), static_cast<s32>(v0 * fh),
                        std::max(1, static_cast<s32>(std::lround((u1 - u0) * fw))),
                        std::max(1, static_cast<s32>(std::lround((v1 - v0) * fh))), 0xFFFFFFFFu);
                }
            }
        }
    }
}

// The player pin without an atlas cell: two diamonds and a square core (map.style pin_*),
// recorded as the live marker's box.
void GeometryMapDraw::DrawFallbackPin(s32 cxm, s32 cym) {
    const MapStyle& style = manifest.map_style;
    const s32 reach = style.PinReach();
    add_marker_px(cxm - reach - 1, cym - reach - 1, 2 * reach + 3, 2 * reach + 3);
    const auto diamond = [&](s32 r, u32 col) {
        canvas.FillTriangle(cxm, cym - r, cxm + r, cym, cxm, cym + r, col);
        canvas.FillTriangle(cxm, cym - r, cxm, cym + r, cxm - r, cym, col);
    };
    diamond(style.pin_outer, style.marker_back_color);
    diamond(style.pin_inner, style.marker_colors[0]);
    canvas.FillRect(cxm - style.pin_core / 2, cym - style.pin_core / 2, style.pin_core,
                    style.pin_core, style.marker_glyph_color);
}

// The atlas paths above only run when an icon atlas exists; an image-mode area
// without one still gets the live pin as the red diamond (no_pin areas, whose
// coordinate space is not yet calibrated, suppress it).
void GeometryMapDraw::DrawPinFallback() {
    if (image_mode && have_player && !geo->second.no_pin && widget.marker_src.empty() &&
        (atlas == nullptr || manifest.icon_cell <= 0)) {
        const float mx = to_x(wx_player);
        const float my = to_y(wy_player);
        if (visible(mx, my)) {
            DrawFallbackPin(static_cast<s32>(mx), static_cast<s32>(my));
        }
    } else if (!image_mode && have_player && report_visit) {
        report_visit(area, wx_player, wy_player);
    }
}

// Fit the area's own extent into the widget, keeping its proportions.
void DrawRoomBoxes(const WidgetDrawContext& ctx, const std::string& area,
                   const std::string& current_room) {
    Canvas& canvas = ctx.canvas;
    const Manifest& manifest = ctx.manifest;
    const ImageProvider& images = ctx.images;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    for (const auto& [name, room] : manifest.map_rooms) {
        if (room.area != area) {
            continue;
        }
        const float half_w = room.w * 0.5f;
        const float half_h = room.h * 0.5f;
        min_x = std::min(min_x, room.x - half_w);
        max_x = std::max(max_x, room.x + half_w);
        min_y = std::min(min_y, room.y - half_h);
        max_y = std::max(max_y, room.y + half_h);
    }
    if (max_x <= min_x || max_y <= min_y) {
        return;
    }
    const float scale = std::min(static_cast<float>(rw) / (max_x - min_x),
                                 static_cast<float>(rh) / (max_y - min_y));
    const float pad_x = (static_cast<float>(rw) - (max_x - min_x) * scale) * 0.5f;
    const float pad_y = (static_cast<float>(rh) - (max_y - min_y) * scale) * 0.5f;
    for (const auto& [name, room] : manifest.map_rooms) {
        if (room.area != area) {
            continue;
        }
        const float box_w = std::max(3.0f, room.w * scale);
        const float box_h = std::max(3.0f, room.h * scale);
        const float cx = (room.x - min_x) * scale + pad_x;
        // Game coordinates run upwards; the canvas runs down.
        const float cy = (max_y - room.y) * scale + pad_y;
        const s32 rx = x + static_cast<s32>(cx - box_w * 0.5f);
        const s32 ry = y + static_cast<s32>(cy - box_h * 0.5f);
        const bool here = !current_room.empty() && name == current_room;
        // The game's own drawn room, when the package carries it; a plain box otherwise.
        std::shared_ptr<const Image> art;
        if (images && !room.sprite.empty()) {
            art = images(room.sprite);
        }
        if (art != nullptr) {
            // Undiscovered-looking rooms stay dim; the one you are in is drawn at full
            // strength so it reads at a glance.
            canvas.DrawImage(rx, ry, static_cast<s32>(box_w), static_cast<s32>(box_h), *art,
                             here ? 0xFFFFFFFFu : 0xB0FFFFFFu);
        } else {
            canvas.FillRect(rx, ry, static_cast<s32>(box_w), static_cast<s32>(box_h),
                            here ? widget.color : ((widget.color & 0x00FFFFFF) | 0x50000000));
        }
        if (here) {
            canvas.FrameRect(rx - 2, ry - 2, static_cast<s32>(box_w) + 4,
                             static_cast<s32>(box_h) + 4, 2, widget.color);
        }
    }
    if (widget.area_label) {
        canvas.DrawText(x + widget.label_offset[0], y + rh - widget.label_offset[1], area,
                        widget.text_scale, widget.color);
    }
}

} // namespace

std::optional<MapDrawRecord> ProbeMapView(const Manifest& manifest, const Page& page,
                                          const StateSnapshot& snapshot, const Widget& widget,
                                          size_t widget_index, const ViewState& views,
                                          const MapFollowState& follow) {
    // The Map case of RenderPage up to the end of FrameView, on copies: the follow state is
    // advanced exactly as a draw would (glide step, pan correction) but in `scratch`, and nothing
    // is drawn or reported.
    if (widget.type != WidgetType::Map || widget.rect[2] <= 0 || widget.rect[3] <= 0) {
        return std::nullopt;
    }
    static thread_local Canvas unused_canvas;
    MapFollowState scratch = follow;
    ViewTransform view{};
    if (widget.pan_zoom) {
        const std::string key =
            widget.id.empty() ? page.id + "#" + std::to_string(widget_index) : widget.id;
        if (const auto found = views.find(key); found != views.end()) {
            view = found->second;
        }
    }
    s32 x = widget.rect[0];
    s32 y = widget.rect[1];
    bool settling = false;
    const ImageProvider no_images;
    const TextProvider no_texts;
    const VisitReporter no_report;
    const VisitedQuery no_query;
    const WidgetDrawContext ctx{.canvas = unused_canvas,
                                .manifest = manifest,
                                .page = page,
                                .snapshot = snapshot,
                                .images = no_images,
                                .texts = no_texts,
                                .draw_list = nullptr,
                                .follow_state = &scratch,
                                .report_visit = no_report,
                                .is_visited = no_query,
                                .map_records = nullptr,
                                .follow_state_mutex = nullptr,
                                .manifest_markers_mutex = nullptr,
                                .settling = settling,
                                .widget = widget,
                                .widget_index = widget_index,
                                .view = view,
                                .x = x,
                                .y = y,
                                .rw = widget.rect[2],
                                .rh = widget.rect[3],
                                .value = 0,
                                .maximum = 0};
    std::string area;
    std::string current_room;
    SelectMapArea(ctx, area, current_room);
    const auto geo = manifest.map_areas.find(area);
    if (geo == manifest.map_areas.end()) {
        return std::nullopt;
    }
    MapDrawRecord out;
    if (!GeometryMapDraw{ctx, area, geo}.ProbeView(out)) {
        return std::nullopt;
    }
    return out;
}

std::array<s32, 4> PredictMapMarkerBox(const Manifest& manifest, const Widget& widget,
                                       const StateSnapshot& snapshot, const MapDrawRecord& view,
                                       const ImageProvider& images) {
    // Mirrors GeometryMapDraw's live-marker draws (DrawLivePlayer, DrawMarkerPicture,
    // DrawPinFallback) for the view the record holds: the same position lookup (FrameView) and
    // the same world -> canvas mapping (to_x / to_y). Each box is widened a little and drawn
    // unconditionally where its draw has a further condition, so this is a superset of the real
    // box; the caller checks the draw's own marker_box against it anyway.
    if (widget.marker_x_bind.empty() || widget.marker_y_bind.empty() || view.ppw <= 0.0f) {
        return {};
    }
    const auto fx = snapshot.floats.find(widget.marker_x_bind);
    const auto fy = snapshot.floats.find(widget.marker_y_bind);
    if (fx == snapshot.floats.end() || fy == snapshot.floats.end()) {
        return {};
    }
    const float wx = static_cast<float>(fx->second) * widget.marker_scale;
    const float wy = static_cast<float>(fy->second) * widget.marker_scale;
    if (!std::isfinite(wx) || !std::isfinite(wy)) {
        return {};
    }
    const auto geo = manifest.map_areas.find(view.area);
    if (geo == manifest.map_areas.end() || geo->second.no_pin) {
        return {};
    }
    const s32 x = view.rect[0], y = view.rect[1], rw = view.rect[2], rh = view.rect[3];
    const float cxpix = static_cast<float>(x) + static_cast<float>(rw) * 0.5f;
    const float cypix = static_cast<float>(y) + static_cast<float>(rh) * 0.5f;
    const float mx = cxpix + (wx - view.cx) * view.ppw;
    const float my = cypix - (wy - view.cy) * view.ppw;
    constexpr s32 Pad = 2;
    std::array<s32, 4> box{};
    const auto add = [&box](float l, float t, float r, float b) {
        const s32 bx0 = static_cast<s32>(std::floor(l)) - Pad;
        const s32 by0 = static_cast<s32>(std::floor(t)) - Pad;
        const s32 bx1 = static_cast<s32>(std::ceil(r)) + Pad;
        const s32 by1 = static_cast<s32>(std::ceil(b)) + Pad;
        if (bx1 <= bx0 || by1 <= by0) {
            return;
        }
        if (box[2] <= 0 || box[3] <= 0) {
            box = {bx0, by0, bx1 - bx0, by1 - by0};
            return;
        }
        const s32 x0 = std::min(box[0], bx0), y0 = std::min(box[1], by0);
        const s32 x1 = std::max(box[0] + box[2], bx1), y1 = std::max(box[1] + box[3], by1);
        box = {x0, y0, x1 - x0, y1 - y0};
    };
    // A glyph or picture is clipped to the widget before it is drawn.
    const auto add_clipped = [&](float qx, float qy, float qw, float qh) {
        const float l = std::max(qx, static_cast<float>(x));
        const float t = std::max(qy, static_cast<float>(y));
        const float r = std::min(qx + qw, static_cast<float>(x + rw));
        const float b = std::min(qy + qh, static_cast<float>(y + rh));
        if (r > l && b > t) {
            add(l, t, r, b);
        }
    };
    if (!widget.marker_icon.empty()) {
        const float p = static_cast<float>(manifest.map_style.marker_icon);
        add_clipped(mx - p * 0.5f, my - p * 0.5f, p, p);
    }
    if (widget.marker_src.empty()) {
        // The fallback pin (either path), not clipped to the widget: DrawFallbackPin's box
        // about the truncated centre.
        const float reach = static_cast<float>(manifest.map_style.PinReach());
        add(std::floor(mx) - reach - 1.0f, std::floor(my) - reach - 1.0f,
            std::floor(mx) + reach + 2.0f, std::floor(my) + reach + 2.0f);
    } else if (images) {
        const std::shared_ptr<const Image> mk = images(widget.marker_src);
        if (mk != nullptr && mk->Valid()) {
            const float mw =
                widget.marker_size[0] > 0.0f ? widget.marker_size[0] : static_cast<float>(mk->w);
            const float mh =
                widget.marker_size[1] > 0.0f ? widget.marker_size[1] : static_cast<float>(mk->h);
            add_clipped(mx - widget.marker_anchor[0] * mw, my - widget.marker_anchor[1] * mh, mw,
                        mh);
        }
    }
    return box;
}

void DrawMap(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Manifest& manifest = ctx.manifest;
    AuxDrawList* const draw_list = ctx.draw_list;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    // Draw one area of the game's own map: every room in its real position, with the room
    // you are standing in lit up. Positions come from the game's data, not from us.
    if (draw_list == nullptr) {
        canvas.FillRect(x, y, rw, rh, widget.bg);
    } else {
        VideoCore::DSMod::AuxRouting::Quad background{};
        background.x = static_cast<float>(x);
        background.y = static_cast<float>(y);
        background.w = static_cast<float>(rw);
        background.h = static_cast<float>(rh);
        background.solid = true;
        background.color = widget.bg;
        draw_list->quads.push_back(background);
    }
    std::string area;
    std::string current_room;
    SelectMapArea(ctx, area, current_room);
    // A game whose map is geometry: blit the rasterised area, then its own markers.
    if (const auto geo = manifest.map_areas.find(area); geo != manifest.map_areas.end()) {
        GeometryMapDraw{ctx, area, geo}.Draw();
        return;
    }
    if (area.empty()) {
        canvas.DrawText(x + 12, y + 12, "MAP: UNKNOWN AREA", widget.text_scale, widget.color);
        return;
    }

    DrawRoomBoxes(ctx, area, current_room);
}

} // namespace Core::Mods
