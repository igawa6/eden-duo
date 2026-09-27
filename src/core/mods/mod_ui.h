// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Second-screen widget renderer. Pages of widgets rasterise into a CPU canvas that is uploaded
// once per change; with an AuxDrawList the map widget instead emits GPU quads (map texture, atlas
// icons, markers) and the canvas carries only the HUD overlay. Either way every mod-facing
// concept (pages, widgets, binds) stays out of the renderer.

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/mods/mod_types.h"
#include "video_core/dsmod/aux_routing.h"

namespace Core::Mods {

/// A decoded picture, in the canvas' own pixel order.
struct Image {
    u32 w{};
    u32 h{};
    std::vector<u32> pixels;
    [[nodiscard]] bool Valid() const {
        return w != 0 && h != 0 && !pixels.empty();
    }
};

/// Resolves a widget's "src" to a decoded image, or nullptr when it cannot be loaded. The runtime
/// owns the cache; the renderer only borrows -- but "borrows" now means holding a ref-counted
/// shared_ptr, not a raw pointer into the runtime's own map (a raw pointer into the cache could
/// dangle if the runtime's own eviction/insert races the caller once the caller is a different
/// thread; a shared_ptr keeps the Image alive for exactly as long as the caller holds this return
/// value, regardless of what the cache does meanwhile).
using ImageProvider = std::function<std::shared_ptr<const Image>(const std::string& src)>;
/// Resolves a text reference ("msbt:<alias>#<label>") to its decoded text, or nullptr while it is
/// not available (yet). The runtime owns the strings; the renderer only borrows -- but "borrows"
/// now means holding a ref-counted shared_ptr (aliased onto the msbt bucket's own label-map
/// shared_ptr), not a raw pointer into it (the same hazard as ImageProvider's: a re-landed alias's
/// shared_ptr reassignment can free a raw pointer into the map out from under the caller once the
/// caller is a different thread -- fixed the same way ImageProvider is).
using TextProvider = std::function<std::shared_ptr<const std::string>(const std::string& ref)>;
using MapFollowState = std::map<std::string, std::array<float, 2>>;
/// Called with the area the map is drawing and the player's world position in it, so the runtime
/// can record fog-of-war reveal against exactly the area shown (not a guessed one).
using VisitReporter = std::function<void(const std::string& area, float wx, float wy)>;
/// Whether a world position in an area has been revealed yet -- gates icon drawing so unexplored
/// features stay hidden with the geometry.
using VisitedQuery = std::function<bool(const std::string& area, float wx, float wy)>;

class Canvas {
public:
    void Resize(u32 width, u32 height);
    void Clear(u32 argb);
    void FillRect(s32 x, s32 y, s32 w, s32 h, u32 argb);
    /// Sets every pixel of the rect to `argb` (no blending): a hole in a composited overlay.
    void ClearRect(s32 x, s32 y, s32 w, s32 h, u32 argb = 0);
    /// A capsule (pill): `fill` inside, a `thickness`-px `frame` ring along the edge.
    void Pill(s32 x, s32 y, s32 w, s32 h, s32 thickness, u32 fill, u32 frame);
    void FrameRect(s32 x, s32 y, s32 w, s32 h, s32 thickness, u32 argb);
    void DrawText(s32 x, s32 y, std::string_view text, s32 scale, u32 argb);
    /// Width DrawText would cover, in the current font (or the built-in one), so a caller can
    /// centre or right-align.
    [[nodiscard]] s32 MeasureText(std::string_view text, s32 scale) const;
    /// DrawText anchored: align 0 = x is the left edge, 1 = x is the centre, 2 = the right edge.
    void DrawTextAligned(s32 x, s32 y, std::string_view text, s32 scale, u32 argb, s32 align);
    /// Splits text into lines: at every '\n', and (wrap_width > 0) greedily on spaces so each line
    /// fits, breaking a word by character only when it cannot fit a line alone. With max_lines > 0
    /// the last kept line ends in an ellipsis, trimmed to fit.
    [[nodiscard]] std::vector<std::string> LayoutText(std::string_view text, s32 scale,
                                                      s32 wrap_width, s32 max_lines) const;
    /// Multi-line DrawTextAligned: line i's cap top is y + i * (scale * 5 + line_gap), each line
    /// aligned on its own (line_gap < 0 = scale * 3).
    void DrawTextBlock(s32 x, s32 y, std::string_view text, s32 scale, u32 argb, s32 align,
                       s32 wrap_width, s32 max_lines, s32 line_gap);
    /// Inline icons (TextIconBase + code point) come from this second font, drawn untinted at the
    /// text's size. `pending`: the font is still loading (icons keep their space, drawn blank);
    /// with no font and not pending an icon is written as "(X)".
    void SetIconFont(const Image* atlas, const FontMetrics* metrics, bool pending) {
        icon_atlas = atlas;
        icon_metrics = metrics;
        icon_pending = pending;
    }
    /// Draw inline icons as a mask in the text colour (a shadow copy of a label).
    void SetIconSilhouette(bool on) {
        icon_silhouette = on;
    }
    /// Draw with the game's own font instead of the built-in one. The atlas holds coverage in
    /// alpha, so the glyph is a mask and the colour comes from the caller -- which is what lets
    /// one font serve every label on the page.
    void SetFont(const Image* atlas, const FontMetrics* metrics) {
        font_atlas = atlas;
        font_metrics = metrics;
    }
    [[nodiscard]] bool HasFont() const {
        return font_atlas != nullptr && font_metrics != nullptr;
    }
    /// The font DrawText uses (nullptr = the built-in one).
    [[nodiscard]] const FontMetrics* ActiveFont() const {
        return HasFont() ? font_metrics : nullptr;
    }
    /// The loaded font's glyph for a codepoint, or for its plain stand-in (’ -> ', é -> e) when
    /// the font lacks it; nullptr when neither exists.
    [[nodiscard]] const FontGlyph* FindGlyph(char32_t c) const;
    /// Scales the image into the rect (nearest neighbour) and alpha-blends it. A tint other than
    /// opaque white multiplies the image, so one asset can be recoloured per widget.
    void DrawImage(s32 x, s32 y, s32 w, s32 h, const Image& image, u32 tint);
    /// Flat-filled triangle. A game whose map is geometry rather than tiles needs this to draw
    /// its own map at all.
    void FillTriangle(s32 x0, s32 y0, s32 x1, s32 y1, s32 x2, s32 y2, u32 argb);
    /// Blits one cell out of a sprite atlas, so icons can come from the game's own sheet.
    void DrawImageRegion(s32 x, s32 y, s32 w, s32 h, const Image& image, s32 sx, s32 sy, s32 sw,
                         s32 sh, u32 tint, bool flip_x = false, bool flip_y = false);
    /// DrawImageRegion that keeps only the image's alpha: the shape is filled with `argb` (its
    /// alpha multiplies the shape's).
    void DrawImageMask(s32 x, s32 y, s32 w, s32 h, const Image& image, s32 sx, s32 sy, s32 sw,
                       s32 sh, u32 argb);
    /// DrawImageRegion turned by `angle_rad` about the rect's centre (a loading wheel); nearest
    /// sampling, clockwise for positive angles on the panel's y-down axes.
    void DrawImageRegionRotated(s32 x, s32 y, s32 w, s32 h, const Image& image, s32 sx, s32 sy,
                                s32 sw, s32 sh, u32 tint, float angle_rad);
    /// Draws only the bottom `fraction` of the image (or of one atlas cell), so a sprite can act
    /// as a gauge. The tint's alpha multiplies the image like DrawImageRegion.
    void DrawImageFilled(s32 x, s32 y, s32 w, s32 h, const Image& image, u32 tint, float fraction,
                         s32 sx = 0, s32 sy = 0, s32 sw = 0, s32 sh = 0);

    [[nodiscard]] u32 Width() const {
        return w;
    }
    const Image* font_atlas{nullptr};
    const FontMetrics* font_metrics{nullptr};
    [[nodiscard]] u32 Height() const {
        return h;
    }
    [[nodiscard]] const std::vector<u32>& Pixels() const {
        return pixels;
    }

    /// Multiply the alpha of subsequent drawing operations. Used while painting one map widget so
    /// the CPU path has the same per-layer opacity as the GPU quad compositor.
    void SetDrawOpacity(float opacity) {
        draw_opacity = std::clamp(opacity, 0.0f, 1.0f);
    }
    /// A second alpha multiplier on top of SetDrawOpacity (a fading widget group), so widgets that
    /// set their own draw opacity still fade with their group.
    void SetLayerOpacity(float opacity) {
        layer_opacity = std::clamp(opacity, 0.0f, 1.0f);
    }
    /// draw_opacity * layer_opacity, exposed so a caller can tell whether "this
    /// exact draw" would be a plain, reusable-later blend (1.0, the common case) or a one-tick-only
    /// fade (a page transition or an animating widget-group's own SetLayerOpacity) whose output
    /// must never be cached and replayed on a later, differently-faded tick.
    [[nodiscard]] float CurrentOpacity() const {
        return EffectiveOpacity();
    }
    /// An unblended, un-tinted copy of `rw`x`rh` pixels from `src` (row-major, stride
    /// `src_stride`) straight into the canvas at (x, y), clipped like every other draw call here.
    /// For replaying a previously-rendered widget's own output verbatim -- the caller is
    /// responsible for only ever doing that when the source pixels are still valid for exactly
    /// this draw (same resolved inputs, same opacity, same canvas size).
    void BlitRaw(s32 x, s32 y, s32 rw, s32 rh, const u32* src, s32 src_stride);

    /// Restricts every drawing operation, Clear() included, to a rectangle (intersected with the
    /// canvas); w or h <= 0 means the whole canvas. An animation redraws only its own box this way.
    void SetClip(s32 x, s32 y, s32 cw, s32 ch);
    void ResetClip() {
        clip_x0 = clip_y0 = 0;
        clip_x1 = static_cast<s32>(w);
        clip_y1 = static_cast<s32>(h);
    }
    /// The current clip as {x, y, w, h}.
    [[nodiscard]] std::array<s32, 4> Clip() const {
        return {clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0};
    }

private:
    [[nodiscard]] u32 ApplyDrawOpacity(u32 argb) const;
    /// Pixel size of the text's ascent (cell top to baseline) at a cap height of `cap` px.
    [[nodiscard]] f32 TextAscentPx(f32 cap) const;
    /// Pen advance of an inline icon (glyph = the icon font's code point) at cap height `cap`.
    [[nodiscard]] f32 IconAdvance(char32_t glyph, f32 cap, s32 scale) const;
    /// Draws an inline icon with its baseline at `baseline`; returns the advance.
    f32 DrawIcon(f32 pen, f32 baseline, char32_t glyph, f32 cap, s32 scale, u32 argb);
    [[nodiscard]] bool IconDrawable(char32_t glyph) const;
    const Image* icon_atlas{nullptr};
    const FontMetrics* icon_metrics{nullptr};
    bool icon_pending{false};
    bool icon_silhouette{false};
    struct LayoutKey {
        std::string text;
        s32 scale, wrap, lines;
        const FontMetrics* font;
        u32 font_cap;
        const FontMetrics* icons;
        bool icons_pending;
        bool operator==(const LayoutKey&) const = default;
    };
    mutable std::vector<std::pair<LayoutKey, std::vector<std::string>>> layout_cache;
    [[nodiscard]] float EffectiveOpacity() const {
        return draw_opacity * layer_opacity;
    }
    std::vector<u32> pixels;
    u32 w{0};
    u32 h{0};
    float draw_opacity{1.0f};
    float layer_opacity{1.0f};
    s32 clip_x0{0}, clip_y0{0}, clip_x1{0}, clip_y1{0}; ///< drawable area, [x0, x1) x [y0, y1)
};

/// GPU-composite output. When RenderPage receives a non-null AuxDrawList, the Map widget appends
/// textured quads here (map endpoints, icon atlas, player marker) instead of blitting them into the
/// canvas, and records the GetImage keys for the two source textures. The canvas then holds only
/// the HUD overlay on a transparent background, and the renderer composites map + icons + marker +
/// HUD on the GPU -- so a panning map / moving marker costs a few quad draws, not a ~5 MB
/// re-upload. Null => the canvas is the whole bottom screen, exactly as before.
struct AuxDrawList {
    std::vector<VideoCore::DSMod::AuxRouting::Quad> quads;
    std::string map_key;   ///< GetImage key for the map-area texture (compositor slot 0)
    std::string atlas_key; ///< GetImage key for the icon atlas (compositor slot 1)
    std::string pulse_key; ///< GetImage key for the item-room pulse companion (slot 3)
    u32 canvas_w{0};
    u32 canvas_h{0};
    u32 bg{0xFF000000u}; ///< page background used for the compositor's full-canvas clear
    bool active{false};  ///< set once a map quad was emitted; the caller must then clear any
                         ///< stale composite itself when a page without a map is drawn
};

/// What a Map widget showed on its last draw, so a tap can be turned into a world position and
/// hit-tested against the markers exactly as they were on screen.
struct MapDrawRecord {
    std::string page_id;
    size_t widget_index{}; ///< index in the expanded widget list
    std::string area;
    std::array<s32, 4> rect{};
    float cx{}, cy{}; ///< world point at the widget centre
    float ppw{};      ///< canvas px per world unit
    float min_x{}, min_y{}, max_x{}, max_y{};
    struct Hit {
        std::string group;
        s32 index{};
        float sx{}, sy{}; ///< canvas px of the marker's position
        float wx{}, wy{}; ///< world position
        float vx{}, vy{}; ///< the slot's own point values
    };
    std::vector<Hit> hits; ///< visible, on-screen dynamic markers of this draw
    [[nodiscard]] float WorldX(float sx) const {
        return cx + (sx - (static_cast<float>(rect[0]) + static_cast<float>(rect[2]) * 0.5f)) / ppw;
    }
    [[nodiscard]] float WorldY(float sy) const {
        return cy - (sy - (static_cast<float>(rect[1]) + static_cast<float>(rect[3]) * 0.5f)) / ppw;
    }
};
using MapDrawRecords = std::vector<MapDrawRecord>;

/// One widget group ("anim") as it is drawn this frame.
struct RenderAnimGroup {
    std::string key;
    std::array<s32, 4> box{}; ///< the group's widgets are clipped to this rect while it moves
    s32 dx{};                 ///< offset of the group's widgets this frame
    s32 dy{};
    float alpha{1.0f};
    bool shown{false};  ///< drawn this frame: open, or still on its way out
    bool moving{false}; ///< mid-animation (clip + offset apply)
};

/// Optional per-frame state of a RenderPage call (animations).
struct RenderExtras {
    /// Draw only inside this rect ({x, y, w, h}; w or h <= 0 = the whole canvas).
    std::array<s32, 4> clip{};
    /// The dirty region as disjoint rects inside `clip` (their bounding box). A caller that redraws
    /// more than one of them runs one RenderPage pass per rect (clip = that rect); RenderPage
    /// itself only reads `clip`.
    std::vector<std::array<s32, 4>> clips;
    std::vector<RenderAnimGroup> groups;
    [[nodiscard]] const RenderAnimGroup* Find(const std::string& key) const {
        for (const auto& g : groups) {
            if (g.key == key) {
                return &g;
            }
        }
        return nullptr;
    }
};

/// The box a widget group animates in: the first member's explicit box, else the union of the
/// members' rects that have a size. {0,0,0,0} when the page has no such group.
std::array<s32, 4> AnimGroupBox(const std::vector<Widget>& widgets, const std::string& key);

/// The rect of the first widget with this id (expanded widgets, so repeats/{i} and x_bind/y_bind
/// already resolved), or nullopt when there is none / the id is empty. Shared by the page-level
/// "origin" key (grow/shrink) and the group-level "from": "widget:<id>".
std::optional<std::array<s32, 4>> WidgetRectById(const std::vector<Widget>& widgets,
                                                 const std::string& id);

/// A page transition frame: `from` / `to` are whole canvases of w x h. slide_up: the target page's
/// top edge is at h * (1 - eased); slide_down: the current page's top edge is at h * eased. A
/// `shadow` (0..1) darkens 24 rows of the lower page right above that edge. Writes all of `out`.
void ComposePageSlide(std::span<u32> out, std::span<const u32> from, std::span<const u32> to, u32 w,
                      u32 h, PageTransition kind, float eased, float shadow);

/// A grow/shrink page transition frame: `background` (a whole w x h canvas) shows
/// everywhere outside the rect that lerps from `origin` (p=0) to `dest` (p=1); inside that rect,
/// `moving` (also a whole w x h canvas -- the page at its natural, fully-open size) is nearest-
/// neighbour scaled to fill it, fading in over the first quarter of `p` so a heavily downscaled
/// first frame doesn't flash as noise. `shadow` (0..1) darkens a ~28 px band of `background` just
/// outside the rect's edge, strongest right at it (the slide transition's edge shadow, wrapped
/// around a rect). Cost is one memcpy of `background` plus the scaled blit and shadow band, both
/// bounded to the rect (the whole canvas only at the rect's largest, i.e. dest itself). Writes all
/// of `out`.
void ComposePageGrow(std::span<u32> out, std::span<const u32> background,
                     std::span<const u32> moving, u32 w, u32 h, std::array<s32, 4> origin,
                     std::array<s32, 4> dest, float p, float shadow);

/// A cross-fade page transition frame: `from` / `to` are the exact same two whole
/// w x h canvases a slide/grow transition already has (the one-time "render each page ONCE"
/// snapshots) -- Fade just alpha-blends `to` over `from` by `eased` (0 = all `from`, 1 = all
/// `to`), pixel for pixel, no scaling and no shadow band. This is deliberately the cheapest
/// transition of the set: no LUT, no rect geometry, no separate background memcpy -- a single
/// pass writing every pixel once (`eased` at the extremes short-circuits to a plain memcpy of
/// the relevant canvas, same as the composited result would be anyway). `origin` and `shadow`
/// from the action are not accepted here at all -- both are meaningless for a plain fade and are
/// documented as ignored rather than silently accepted and dropped.
/// Writes all of `out`.
void ComposePageFade(std::span<u32> out, std::span<const u32> from, std::span<const u32> to, u32 w,
                     u32 h, float eased);

/// Whether a PointGate is open against a snapshot (ints first, then floats; missing = 0).
bool GateOpen(const PointGate& gate, const StateSnapshot& snapshot);
/// A published value by name: ints first, then floats.
std::optional<f64> SnapshotNumber(const StateSnapshot& snapshot, const std::string& key);

/// Draws one page of the manifest using the latest snapshot. Returns false when nothing is
/// drawable. `animating` (optional) reports that a view glide is still in flight, so the caller
/// should redraw next tick even when nothing bound changed. `map_records` (optional) receives what
/// every Map widget drew (cleared first).
/// `follow_state_mutex` (last parameter,
/// deliberately -- see below), when non-null, is locked around every write to `*follow_state` the
/// Map widget's draw case makes (the camera-follow glide easing and pan-correction write). The
/// caller (ModRuntime::RenderPageTo) passes its own `view_mutex` -- the same lock
/// `ApplyViewCorrections` already takes to read/consume those corrections right after this call
/// returns -- so the two sides of that hand-off share one lock instead of inventing a second
/// one. Null is safe (no locking, today's behaviour) for any caller that
/// doesn't pass `follow_state` either (`RenderDebugPage`-style callers, tests). Placed LAST, not
/// next to `follow_state`, so every existing positional call site (this codebase's own tests
/// included) that stops before `extras` keeps compiling and keeps its existing meaning unchanged --
/// inserting it mid-list would silently reinterpret every trailing positional argument at every
/// such call site.
///
/// `manifest_markers_mutex`: when non-null, locked around the Map widget's marker-visibility reads
/// (`manifest.map_areas[area].markers[i].hidden/.opened/.collected/.unveiled/.veiled`) -- the same
/// lock `ModRuntime::UpdateHiddenMarkers` already holds for its whole body (`map_state_mutex`,
/// reused here rather than a new lock, same reasoning as `follow_state_mutex`/`view_mutex`). Added
/// last of all, after `follow_state_mutex`, for the identical positional-compatibility reason.
bool RenderPage(Canvas& canvas, const Manifest& manifest, const Page& page,
                const StateSnapshot& snapshot, const ImageProvider& images = {},
                const ViewState& views = {}, MapFollowState* follow_state = nullptr,
                const VisitReporter& report_visit = {}, const VisitedQuery& is_visited = {},
                AuxDrawList* draw_list = nullptr, bool* animating = nullptr,
                const TextProvider& texts = {}, MapDrawRecords* map_records = nullptr,
                const RenderExtras* extras = nullptr, std::mutex* follow_state_mutex = nullptr,
                std::recursive_mutex* manifest_markers_mutex = nullptr);

/// Draws a generated page listing every data point with its resolved address and value, so a
/// broken pointer chain is obvious on the device instead of silently reading zero.
void RenderDebugPage(Canvas& canvas, const Manifest& manifest, const StateSnapshot& snapshot);

/// Repeating widgets become one widget per element (drawing and hit-testing share the answer).
/// A template in a scroll region (Widget::scroll) expands only its rows visible through the
/// region's rect at the offset published as "@scroll:<id>", shifted up by that offset and tagged
/// with the rect as their scroll_clip.
std::vector<Widget> ExpandWidgets(const Page& page, const StateSnapshot& snapshot);
/// The elements of one repeating widget (`source.repeat > 0`, a member of `page`), appended to
/// `out` exactly as ExpandWidgets builds them (the dirty scan hashes them per element).
void ExpandRepeatTemplate(const Page& page, const Widget& source, const StateSnapshot& snapshot,
                          std::vector<Widget>& out);
struct ScrollMemo;
/// ExpandWidgets into reusable storage: slots [0, returned count) hold the expansion, copy-assigned
/// over whatever Widgets were there (their strings keep their capacity), later slots are leftovers.
size_t ExpandWidgetsInto(const Page& page, const StateSnapshot& snapshot,
                         std::vector<Widget>& slots);
/// ExpandRepeatTemplate into reusable storage (slots [0, returned count)). With `slot_index` (the
/// element index each slot was last built for, kept by the caller alongside `slots` for this one
/// template and cleared whenever the template may have changed), a slot already holding element i
/// only has its rect recomputed.
size_t ExpandRepeatTemplateInto(const Page& page, const Widget& source,
                                const StateSnapshot& snapshot, std::vector<Widget>& slots,
                                std::vector<s64>* slot_index = nullptr, ScrollMemo* memo = nullptr);

/// Drag-to-scroll lists (Page::scrolls). The snapshot key holding a region's pixel offset.
std::string ScrollOffsetKey(const std::string& id);
/// The region of this id on the page, or nullptr.
const ScrollRegion* FindScrollRegion(const Page& page, const std::string& id);
/// Elements a repeat template draws this tick (repeat_bind / repeat_div, capped by repeat).
s64 RepeatElementCount(const Widget& widget, const StateSnapshot& snapshot);
/// A template's own row pitch (repeat_row_dy in a grid, else repeat_dy).
s32 RepeatRowPitch(const Widget& widget);
struct ScrollMetrics {
    s64 count{0}; ///< elements in the list
    s32 row_h{0}; ///< px per content row
    s32 cols{1};
    s64 content_h{0};  ///< px of content (rows * row_h + pad)
    s32 max_offset{0}; ///< largest valid offset (0 = everything fits)
};
/// Content size of a region against the snapshot (count_bind, else the largest template count).
ScrollMetrics MeasureScroll(const Page& page, const ScrollRegion& region,
                            const StateSnapshot& snapshot);
/// MeasureScroll per region, computed at most once while one snapshot is being expanded or
/// scanned (it walks every widget of the page, once per region).
struct ScrollMemo {
    std::vector<ScrollMetrics> metrics;
    std::vector<char> known;
    const ScrollMetrics& Get(const Page& page, const ScrollRegion& region,
                             const StateSnapshot& snapshot);
    void Reset(size_t regions) {
        metrics.assign(regions, ScrollMetrics{});
        known.assign(regions, 0);
    }
};
/// The scroll offset of a region clamped to [0, max_offset] from the snapshot.
s32 ScrollOffset(const ScrollRegion& region, const StateSnapshot& snapshot);
/// Half-open element range [first, last) of a template whose rows touch the region's rect at the
/// given offset (clamped to [0, rows)). Pure; shared by ExpandWidgets and tests.
std::pair<s64, s64> VisibleElementRange(const Widget& widget, const ScrollRegion& region,
                                        s32 base_y, s32 offset, s64 rows);
/// Thumb rect of a region's scrollbar, {0,0,0,0} when the list fits or no bar is configured.
std::array<s32, 4> ScrollBarThumb(const ScrollRegion& region, const ScrollMetrics& m, s32 offset);
/// One axis of an x_bind/y_bind widget's live on-screen offset: `base` when `bind` is empty,
/// otherwise `base + snapshot_value(bind) * scale` (clamped to a finite canvas-ish range) -- the
/// exact formula ExpandWidgets' own `position()` lambda applies to `widget.rect[0]`/`rect[1]` at
/// draw time. Extracted so dirty-rect computation (BuildRenderExtras/WidgetEffectiveRect,
/// mod_redraw.cpp) can resolve a widget's TRUE drawn position instead of only ever seeing its
/// static declared `rect` (that mismatch made a bound marker's dirty rect lag its drawn position --
/// see mod_redraw.cpp's WidgetEffectiveRect for the full explanation).
s32 ResolveBindOffset(s32 base, const std::string& bind, float scale,
                      const StateSnapshot& snapshot);
/// The shared visibility rule used by drawing, taps, and gesture targeting (need / hide gates and
/// the "anim" gate).
bool WidgetHidden(const Widget& widget, const StateSnapshot& snapshot);
/// How far a widget can paint outside its rect of `rw` x `rh`: a spinning picture's rotated
/// corners, a trembling one's shake offset (0 for everything else). Dirty boxes are widened by it.
s32 WidgetDrawOverhang(const Widget& widget, s32 rw, s32 rh);
/// Conservative box of every pixel a Label/Value widget's text can paint for this snapshot, drawn
/// at (x, y) with resolved size rw x rh: the font's real ascent/descent (whichever of `font` and
/// the built-in font is used) and a width bound from the shown text's byte length. {0,0,0,0} for
/// other widget types. Dirty boxes and the partial-redraw pre-reject use it so long text or a tall
/// font never leaves pixels outside the box that gets repainted.
std::array<s32, 4> WidgetTextBounds(const Widget& widget, s32 x, s32 y, s32 rw, s32 rh,
                                    const StateSnapshot& snapshot, const Manifest& manifest,
                                    const FontMetrics* font, s32 canvas_w, s32 canvas_h);
/// Everything a widget can paint outside its own rect for this snapshot: WidgetTextBounds for
/// Label/Value/Button text, the whole run for Pips; {0,0,0,0} when it stays inside its rect.
std::array<s32, 4> WidgetPaintBounds(const Widget& widget, s32 x, s32 y, s32 rw, s32 rh,
                                     const StateSnapshot& snapshot, const Manifest& manifest,
                                     const FontMetrics* font, s32 canvas_w, s32 canvas_h);
/// WidgetHidden with one point held open (a group sliding out keeps its gated widgets drawn).
bool WidgetHiddenHolding(const Widget& widget, const StateSnapshot& snapshot,
                         const std::string& held_point);
/// The plain stand-in the canvas draws for a code point its font lacks (’ -> ', é -> e), 0 = none.
char32_t TextFallbackCodepoint(char32_t c);
/// Hit-tests a tap in canvas pixels; returns the action name of the widget hit, or "".
std::string HitTest(const Page& page, const StateSnapshot& snapshot, s32 x, s32 y);
/// Index of the topmost visible expanded widget under (x, y) that `accept` admits, or -1.
s64 HitTestIndex(const std::vector<Widget>& expanded, const StateSnapshot& snapshot, s32 x, s32 y,
                 const std::function<bool(const Widget&)>& accept);
/// The value a widget carries (`payload`, already "{i}"-expanded): a literal integer, or the
/// published int a "$name" payload names. nullopt when there is none.
std::optional<s64> WidgetPayload(const Widget& widget, const StateSnapshot& snapshot);

} // namespace Core::Mods
