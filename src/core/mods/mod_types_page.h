// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 4: pages and widgets. Widget and Page, their scroll regions and view
// state, the Map widget's extras, widget group animations, page transitions and page binds,
// easing curves and haptic feedback settings. Expanded in mod_ui_expand.cpp and drawn by
// RenderPage (mod_ui.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/mods/mod_types_points.h"

namespace Core::Mods {

/// Haptic feedback (manifest "haptics", widget / action "haptic"). The order is the strength: when
/// several events land in one tick the largest value is played.
enum class HapticStrength : u8 { Off = 0, Light, Click, Confirm, Heavy, Reject };
/// What raised a haptic; indexes HapticsConfig::strength.
enum class HapticKind : u8 { Tap, Write, Select, Drag, Drop, Marker, Refused, Hold, Swipe, Count };
struct HapticsConfig {
    bool enabled{false};
    /// The frontend honours the system's own touch-feedback setting.
    bool respect_system{true};
    std::array<HapticStrength, static_cast<size_t>(HapticKind::Count)> strength{
        HapticStrength::Light,   // tap
        HapticStrength::Confirm, // write
        HapticStrength::Light,   // select
        HapticStrength::Light,   // drag
        HapticStrength::Confirm, // drop
        HapticStrength::Light,   // marker
        HapticStrength::Off,     // refused
        HapticStrength::Heavy,   // hold (runtime 13: a fired press-and-hold)
        HapticStrength::Light,   // swipe (runtime 14: a fired horizontal swipe)
    };
};
/// A per-widget / per-action override: -1 = none, else a HapticStrength value.
using HapticOverride = s8;

/// Animation timing curves (cubic).
enum class Easing : u8 { Linear, EaseIn, EaseOut, EaseInOut };
[[nodiscard]] inline float ApplyEasing(Easing easing, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (easing) {
    case Easing::EaseIn:
        return t * t * t;
    case Easing::EaseOut: {
        const float u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case Easing::EaseInOut:
        return t < 0.5f
                   ? 4.0f * t * t * t
                   : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) / 2.0f;
    case Easing::Linear:
    default:
        return t;
    }
}

/// Grow/Shrink scale a page between the rect of the widget that opened it and the full canvas,
/// instead of sliding a straight edge across it. Fade cross-fades the two whole-page snapshots
/// that already exist (a page transition renders each page ONCE) by a plain alpha blend. No
/// geometry at all, so `origin` and `shadow` are both ignored for Fade.
enum class PageTransition : u8 { None, SlideUp, SlideDown, Grow, Shrink, Fade };

/// Widget "anim": the widgets sharing a key slide in / out of their group box (or fade), or grow
/// out of / shrink into a named widget's rect, when the gate opens / closes. The gate is also a
/// visibility gate of the widget.
struct WidgetAnim {
    enum class From : u8 { Right, Left, Top, Bottom, Fade, Widget };
    PointGate gate;
    std::string key; ///< "group", else the bind text as written
    From from{From::Right};
    /// From::Widget only ("widget:<id>"): the id of the widget whose rect is the group's closed
    /// size/position (looked up on the same page, expanded widgets, at animation start).
    std::string origin_widget;
    u32 ms{180};
    Easing easing{Easing::EaseOut};
    bool has_box{false};
    std::array<s32, 4> box{};
};

/// One side of a "page_binds" rule -- what to switch to. Same shape and defaults as a
/// tapped `{"kind": "page", ...}` action's transition keys, so it reuses that exact machinery
/// (RunAction's ActionKind::Page) instead of a parallel one.
struct PageBindTarget {
    std::string page; ///< empty = this branch does nothing (the bind is one-directional)
    PageTransition transition{PageTransition::None};
    u32 duration_ms{220};
    Easing easing{Easing::EaseOut};
    float shadow{0.3f};
    /// Grow/Shrink only. Empty = canvas centre (there is no tapped widget to default to).
    std::string origin;
};

/// Manifest top level "page_binds": binds the active page to a live value, edge-triggered (fires
/// only when the value crosses `equals`, not while it merely holds a value on either side). See
/// DrivePageBinds (mod_pages.cpp) for the full semantics (arming, deferral, no_auto_leave).
struct PageBind {
    std::string point; ///< a published name: a data point, a derived value, or "@flag:<name>"
    f64 equals{};      ///< the reference value; point is compared to it with a small epsilon
    /// Optional extra arm gate (a second published name, "!" negates -- same text form as
    /// "enabled_bind" etc.): the bind stays fully unarmed (no edge tracking, no firing) while this
    /// reads zero/missing, on top of `point` itself needing to resolve. Recommended whenever
    /// `point`'s own pre-save-load value could coincide with a non-sentinel state.
    PointGate ready;
    PageBindTarget when_equal;     ///< fired when point crosses INTO equalling `equals`
    PageBindTarget when_not_equal; ///< fired when point crosses OUT OF equalling `equals`
};

/// Where a pannable widget is currently looking. `pan` is in content units -- canvas pixels at
/// zoom 1 -- and names the content point drawn at the widget's top-left corner, so zooming in
/// about a fingertip is a pan adjustment rather than a special case.
struct ViewTransform {
    float zoom{1.0f};
    float pan_x{0.0f};
    float pan_y{0.0f};
    /// A reset in flight: the view eases towards these each tick until it lands.
    bool gliding{false};
    float goal_zoom{1.0f};
    float goal_pan_x{0.0f};
    float goal_pan_y{0.0f};
    /// Unreleased runtime 15 addition (mod_view_default.h): the view's home -- the bound default
    /// view last applied (ViewDefaultBinds), else zoom 1 / no pan -- and the "view_reset_bind"
    /// value it was applied for.
    float home_zoom{1.0f};
    float home_pan_x{0.0f};
    float home_pan_y{0.0f};
    bool has_reset_value{false};
    double reset_value{0.0};
};

/// Unreleased runtime 15 addition: the published values that place a pan_zoom widget's DEFAULT
/// view (the view it starts in, RESET / view_reset and view_idle_ms glide back to, and
/// "view_custom:<id>" compares against). Widget keys "view_zoom_bind" (zoom, the widget's own
/// zoom scale: 1 = the whole picture), "view_cx_bind" / "view_cy_bind" (the point of the
/// picture to centre, 0..1 across its width / height; pan is clamped to the picture), and
/// optional "view_reset_bind" (a value whose change snaps even a user-moved view back to the
/// default, e.g. a new floor's map). Missing / non-finite values = zoom 1, no pan.
struct ViewDefaultBinds {
    std::string zoom_bind;
    std::string cx_bind;
    std::string cy_bind;
    std::string reset_bind;
};

/// Per-widget view state, keyed by widget id.
using ViewState = std::map<std::string, ViewTransform>;

enum class WidgetType {
    Rect,   ///< filled rectangle (background/panel)
    Label,  ///< static text
    Value,  ///< text + bound number
    Bar,    ///< horizontal progress bar (bind / max)
    Button, ///< tappable rectangle that fires an action
    Pips,   ///< N small squares, `value` of them filled (hearts, masks, ...)
    Image,  ///< a picture loaded from the package or the game's own romfs
    Map,    ///< the game's own map: rooms of one area, with the current one lit
    Chart,  ///< runtime 17: a bound value's recent history, sampled into a ring buffer
};

/// Runtime 16: how an Image (or a Bar's "image") fills its rect. Stretch is the classic scaled
/// blit; Tile repeats the source region at its own pixel size from the rect's top-left; Slice is a
/// 9-slice: Widget::slice insets (source px) keep the corners at their own size, stretch the edges
/// along one axis and the centre along both.
enum class ImageFill : u8 { Stretch, Tile, Slice };
/// Runtime 16: the direction a Bar fills from (Right = today's left-to-right).
enum class BarFillDir : u8 { Right, Left, Up, Down };

/// Runtime 17: a Chart widget's sampling and scale. The samples themselves live in the runtime's
/// ring buffers (mod_chart.h), published per tick as StateSnapshot::charts under `key`.
struct ChartSpec {
    /// The ring buffer's name: the widget's id, else "<page id>#<widget index on the page>".
    std::string key;
    u32 samples{60};      ///< ring buffer length (2..1024)
    u32 interval_ms{1000}; ///< time between samples (>= 16)
    enum class Style : u8 { Line, Bar } style{Style::Line};
    bool has_min{false}; ///< false = the smallest sample shown
    bool has_max{false}; ///< false = the largest sample shown
    f64 min{0.0};
    f64 max{0.0};
};

/// Runtime 17: one chart's samples as of a tick, oldest first (at most ChartSpec::samples).
struct ChartSeries {
    std::vector<f32> values;
    u64 count{0}; ///< samples taken so far (changes whenever `values` does)
};
using ChartSeriesMap = std::unordered_map<std::string, ChartSeries>;

/// Map widget: marker groups, label style and tap handling (shared: widgets are copied per frame).
struct MapWidgetExtras {
    struct Group {
        PointGate show;
        PointGate hide;
        std::vector<s64> hide_in; ///< non-empty: hide while `hide`'s value is one of these
        float opacity{1.0f};
        float min_zoom{0.0f}; ///< hidden while the widget's view zoom is below this (0 = never)
    };
    std::map<std::string, Group> groups;
    struct LabelStyle {
        s32 text_scale{6};
        u32 color{0xFFFFFFFFu};
        u32 outline_color{0xFF000000u};
        s32 outline{2};
        float opacity{1.0f};
    } label_style;
    std::string on_map_tap;
    std::string on_marker_tap;
    bool empty_tap_deselects{true};
    PointGate tap_enabled;
    s32 marker_hit_px{32};
    std::vector<std::string> marker_tap_groups; ///< empty = every dynamic group
    /// Runtime 14: the bound default view ("view_rect_x0_bind", "view_rect_y0_bind",
    /// "view_rect_x1_bind", "view_rect_y1_bind": published values in world units, the area's
    /// min/max space) and its padding ("view_rect_pad", world units). When all four resolve, the
    /// base view (zoom 1, pan 0) fits that rect instead of the whole area (mod_map_view_rect.h).
    std::array<std::string, 4> view_rect_binds;
    float view_rect_pad{0.0f};
    /// Runtime 14: a text point naming the area's base picture ("image_bind"), drawn over the
    /// area's world box like the area's own `image` and overriding it; "" / missing = the area's.
    std::string image_bind;
    /// Runtime 14: pictures placed in world space ("overlays"), drawn over the base picture and
    /// under the markers, panning / zooming with the map. The picture's top edge is at the larger
    /// y (world y grows upward, as for the area).
    struct Overlay {
        std::string src;      ///< a fixed image key
        std::string src_bind; ///< a text point naming the image key (wins when non-empty)
        float x0{}, y0{}, x1{}, y1{};
        PointGate show;
        float opacity{1.0f};
    };
    std::vector<Overlay> overlays;
    [[nodiscard]] bool Tappable() const {
        return !on_map_tap.empty() || !on_marker_tap.empty();
    }
    [[nodiscard]] bool HasViewRect() const {
        return std::ranges::all_of(view_rect_binds,
                                   [](const std::string& b) { return !b.empty(); });
    }
};

struct Widget {
    WidgetType type{WidgetType::Label};
    std::array<s32, 4> rect{}; ///< x, y, w, h in canvas pixels
    /// Map: groups / labels style / taps (null when the widget uses none of them).
    std::shared_ptr<const MapWidgetExtras> map_extras;
    /// Any: a visible widget that swallows taps, so they reach neither a map nor a widget under it.
    bool tap_block{false};
    /// Any: a visible widget that owns every gesture starting on it (taps, pans, pinches, drags)
    /// and hides drop targets below it. "tap_block" sets it too.
    bool input_block{false};
    /// Any: haptic strength for interactions on this widget (-1 = the manifest's default).
    HapticOverride haptic{-1};
    /// Any: group appear / disappear animation (null = none).
    std::shared_ptr<const WidgetAnim> anim;
    std::string text;
    // Optional data-driven position, added to rect.x/y after repetition. Package owns the
    // coordinate transform; scale converts normalized positions into canvas pixels.
    std::string x_bind;
    std::string y_bind;
    float x_scale{1.0f};
    float y_scale{1.0f};
    /// Value: if set, show names[value] instead of the number (season/weather/day-of-week/AM-PM).
    std::vector<std::string> names;
    s32 pad{0};       ///< Value: zero-pad the number to this width (minutes -> 00).
    s32 div{1};       ///< Value: divide the raw number by this before showing (stamina stored x2).
    s32 mul{1};       ///< Value: multiply raw by this (max stamina = berries*15 + 150).
    s32 add{0};       ///< Value: add this after mul/div (the +150 base).
    std::string bind; ///< data point name for the value
    std::string max_bind; ///< data point name for the maximum
    std::string
        max_sep; ///< Value: when set with max_bind, print "<value><max_sep><max>" as one string
    s64 max_const{0};
    u32 color{0xFFE6ECF2};
    u32 bg{0x00000000};
    s32 text_scale{3};
    /// Text drawn with the game font: an outline ring of `text_outline_px` pixels in
    /// `text_outline` (0 = none), and a per-glyph rise of `text_rise` pixels (glyph i is drawn
    /// i * rise higher; only digits count -- the game's HUD numbers step upwards). Older runtimes ignore both keys.
    u32 text_outline{0};
    s32 text_outline_px{0};
    float text_rise{0.0f};
    s32 align{0};       ///< text anchor: 0 left (x is the left edge), 1 centre, 2 right
    bool flip_x{false}; ///< image: mirror horizontally (a diagonal skin piece the other way)
    bool flip_y{false}; ///< image: mirror vertically (a corner piece for the lower corners)
    float spin{
        0.0f}; ///< image: degrees per second of rotation about the rect centre (+ = clockwise)
    float shake{
        0.0f}; ///< image: tremble amplitude in canvas px (a warning piece that will not settle)
    std::string
        need_bind; ///< Any: hidden unless this int is non-zero (a second gate beside hide_bind)
    std::string suffix;                    ///< Value: appended after the number ("%")
    bool area_label{true};                 ///< Map: draw the area name in the widget's corner
    std::vector<std::string> hidden_icons; ///< Map: icon ids omitted by this presentation
    bool pill{false};   ///< Button: rounded (capsule) shape instead of a boxed frame
    s32 frame{2};       ///< Rect / Bar: outline thickness in px (Rect: when `color` is set)
    s32 border{3};      ///< Button: outline thickness in px (box or pill)
    s32 text_inset{12}; ///< Button: a left-aligned caption's inset from the top-left, px
    s32 gap{-1}; ///< Pips: px between pips (-1: 8 for sprite pips, a third of a drawn pip)
    /// Map: the area name's offset from the widget's bottom-left corner {right, up}, px.
    std::array<s32, 2> label_offset{12, 24};
    bool pulse{false};  ///< Rect: the outline's alpha breathes (a warning frame)
    std::string on_tap; ///< action name
    /// Runtime 13: action run once when a single finger rests on this widget for `hold_ms`
    /// (press-and-hold). The lift that ends a fired hold is not a tap (mod_input_hold.h).
    std::string on_hold;
    s32 hold_ms{0}; ///< hold time in ms; <= 0 = DefaultHoldMs (600)
    /// Runtime 14: actions run once when a single finger that landed on this widget moves
    /// sideways at least `swipe_px` (horizontal dominance) and lifts -- left for a finger moving
    /// left, right for one moving right (mod_input_swipe.h). The lift is not a tap.
    std::string on_swipe_left;
    std::string on_swipe_right;
    /// Unreleased runtime 15 addition: the same on the vertical axis -- up for a finger moving
    /// up, down for one moving down (|dy| > 2 |dx|, `swipe_px` travel). Never armed over a scroll
    /// region that can scroll (UpdateGestures).
    std::string on_swipe_up;
    std::string on_swipe_down;
    s32 swipe_px{0}; ///< minimum travel along the swipe's axis in canvas px; <= 0 = 60
    /// Names this widget so a gesture can be remembered against it across frames. Optional:
    /// a widget without one falls back to its index on the page.
    std::string id;
    /// Draw this widget once per element of an array point. Every string on it -- text, binds,
    /// image source -- has "{i}" replaced by the element number, so one definition becomes a
    /// list. `dx`/`dy` step the rectangle between elements.
    s64 repeat{0};
    std::string repeat_bind; ///< element count read from memory, capped by `repeat`
    s32 repeat_div{1};       ///< divide that count first (150 stamina -> 10 berries)
    s32 repeat_dx{0};
    s32 repeat_dy{0};
    s32 repeat_cols{0};   ///< >0: wrap the repeat into a grid this many columns wide.
    s32 repeat_row_dy{0}; ///< grid row pitch when repeat_cols>0.
    bool pack{false};     ///< skip elements failing the keep-range gate and close the gaps.
    /// Repeat: this template belongs to the page's scroll region of this id (Page::scrolls). Its
    /// rows move with the region's drag offset, are clipped to the region's rect, and only the
    /// rows inside that rect are materialised ("{i}" stays the real element index).
    std::string scroll;
    /// Set by ExpandWidgets on a scrolled row (never parsed): {x, y, w, h} the row is drawn and
    /// hit-tested inside. w/h <= 0 = no clip.
    std::array<s32, 4> scroll_clip{0, 0, 0, 0};
    /// Repeat: "keep_min"/"keep_max" given as "{i}" / "{i}+N" / "{i}-N" -- the keep range follows
    /// the element index (a highlight on the row whose index a point names).
    bool keep_min_i{false};
    bool keep_max_i{false};
    /// Opt in to drag-to-pan and pinch-to-zoom. A map you cannot move is a picture of a map.
    bool pan_zoom{false};
    float min_zoom{1.0f};
    float max_zoom{8.0f};
    /// Unreleased runtime 15 addition: a bound default view for a non-map pan_zoom widget
    /// (mod_view_default.h); null = the default view is zoom 1, no pan.
    std::shared_ptr<const ViewDefaultBinds> view_default;
    /// Image: where the picture comes from. "file:<name>" reads it out of the mod package;
    /// "romfs:/<path>" reads it out of the running game's own filesystem, so a package can use
    /// the game's art without shipping any of it.
    std::string src;
    /// Label: show this string point's value. Image: pick the picture by looking this point's
    /// value up in the package's sprite map -- the game chooses, the page follows.
    std::string bind_text;
    std::string src_bind;
    /// Image: pick the source by the numeric `bind` value: src_names[value] (season/weather icons).
    std::vector<std::string> src_names;
    std::vector<std::pair<s64, std::string>> src_thresholds; ///< Image: value<=le -> src (ordered).
    std::string table;      ///< Value: render manifest.tables[table][value] (shared name list).
    std::string src_format; ///< Image: printf(src_format, value) -> source path (icon by id).
    std::string
        hide_bind; ///< Any: gate this widget on the bound value (empty slots / category split).
    s64 keep_min{std::numeric_limits<s64>::min()}; ///< hide unless hide_bind value >= keep_min.
    s64 keep_max{std::numeric_limits<s64>::max()}; ///< hide unless hide_bind value <= keep_max.
    s64 hide_eq{0}; ///< with hide_eq_on: hide when the value equals this (empty sentinel).
    bool hide_eq_on{false};
    std::string area_bind; ///< Map: point holding the current map-zone value
    std::string room_bind; ///< Map: string point holding the current room name
    std::string area;      ///< Map: fixed area, used when neither bind resolves
    /// Map: float points holding the player's live world position. When both resolve, a marker
    /// is drawn at that spot using the same world->panel transform as the area's own icons.
    std::string marker_x_bind;
    std::string marker_y_bind;
    float marker_scale{1.0f}; ///< world units per bound unit (an actor may keep metres)
    std::string marker_icon;  ///< atlas icon to draw there; a plain dot when empty
    std::string actor_x_bind; ///< a second live actor (e.g. a roaming enemy): snapshot floats
    std::string actor_y_bind;
    std::string actor_icon; ///< its atlas icon; nothing is drawn while either bind is absent
    bool actor_reveal_required{false}; ///< draw the actor only after its own map cell is revealed
    float follow_window{0.0f};     ///< world units across the widget when the marker resolves:
                                   ///< the view zooms in and tracks the player like the game's own
                                   ///< minimap. 0 keeps the whole area in frame.
    /// Map: the live marker as a picture (any image source) instead of an atlas cell, drawn at
    /// marker_size px with marker_anchor (fractions of that size) on the player's position and
    /// clipped to the widget.
    std::string marker_src;
    std::array<float, 2> marker_size{0.0f, 0.0f};
    std::array<float, 2> marker_anchor{0.5f, 0.5f};
    /// Pannable widget: glide home by itself after this many ms without a finger on it (0 = never).
    u32 view_idle_ms{0};
    /// Image: draw only the lower fraction of the sprite, from this point's value against the
    /// widget's maximum. Gives a soul orb that fills, using the game's own art.
    std::string fill_bind;
    /// Image: drawn underneath at this tint when a fill is partial (the "empty" state).
    std::string empty_src;
    /// Image: the part of the source to draw, as {u0, v0, u1, v1} in 0..1. This is what lets a
    /// package point at the game's own atlas and name one sprite inside it, instead of shipping a
    /// cut-out copy of that sprite.
    std::array<float, 4> src_rect{0.0f, 0.0f, 1.0f, 1.0f};
    /// Image: the rect for `empty_src`, when the "empty" sprite lives elsewhere in the same
    /// atlas -- an energy tank's blank and full frames, for instance.
    std::array<float, 4> empty_rect{0.0f, 0.0f, 1.0f, 1.0f};
    // --- tap-select / drag-and-drop ---------------------------------------------------------
    /// The value this widget carries ("{i}" expanded): an integer (dec or 0x hex), or "$point"
    /// to carry a published int read at the moment it is used.
    std::string payload;
    /// A tap selects this widget's payload in the group (tap again clears); the selection is
    /// published as snapshot int "@sel:<group>" (-1 when none).
    std::string select_group;
    /// Pressing and moving beyond the tap slop drags `payload`; a ghost follows the finger.
    bool draggable{false};
    float drag_scale{1.15f};
    /// Action run when a drag is released over this widget, or when it is tapped while
    /// `accept_group` holds a selection (which is then cleared). `$payload` = the carried value.
    std::string drop_action;
    std::string accept_group;
    /// Drawn over the widget while it is its group's selection or a drag hovers it: an image, or
    /// `highlight_color` as a `frame`-px outline (frame <= 0 fills the rect instead).
    std::string highlight_src;
    u32 highlight_color{0};
    /// Drawn under the drag ghost (both scaled about the grab point): an image, placed by
    /// `drag_under_rect` {dx, dy, w, h} relative to this widget's rect (w/h 0 = the widget's),
    /// multiplied by `drag_under_tint`, or -- when `drag_under_color` is non-zero -- its alpha
    /// shape filled with that solid colour.
    std::string drag_under_src;
    std::array<float, 4> drag_under_src_rect{0.0f, 0.0f, 1.0f, 1.0f};
    std::array<s32, 4> drag_under_rect{0, 0, 0, 0};
    u32 drag_under_tint{0xFFFFFFFFu};
    u32 drag_under_color{0};
    // --- text by key, multi-line labels --------------------------------------------------------
    /// Label: "msbt:<alias>#<label>" -- the running game's own message text.
    std::string text_src;
    /// Label: pick the text by this int point's value through `text_map` (value -> "msbt:..." or a
    /// literal); a missing point, -1 or an unmapped value draws nothing. Shared: widgets are copied
    /// every frame.
    std::string text_bind;
    std::shared_ptr<const std::unordered_map<s64, std::string>> text_map;
    s32 wrap_width{0}; ///< Label: wrap on spaces at this width in px (0 = no wrap)
    bool fit_text{false}; ///< Shrink a one-line label within wrap_width before ellipsis.
    s32 text_min_scale{1};
    s32 text_center_h{}; ///< Center the laid-out cap block inside this height.
    s32 max_lines{0};  ///< Label: keep at most this many lines, the last ending in an ellipsis
    s32 line_gap{-1};  ///< Label: px between lines beyond the cap height (-1 = text_scale * 3)
    bool icon_silhouette{false}; ///< Label: inline icons drawn as a mask in the label colour
    /// Label/Value: "{c:#AARRGGBB}..{/c}" colour spans in the text.
    bool color_markup{false};
    /// Label/Value: this widget is an outline / shadow copy of another label ("outline_copy"):
    /// with color_markup it lays out the tags like the main copy but draws in its own colour only.
    bool outline_copy{false};
    // --- runtime 17: text-sized boxes, grouped numbers ------------------------------------------
    /// Label/Button ("auto_w"): the widget's width follows its text plus `pad` px on each side
    /// (a left-aligned button caption: text_inset + text + pad). The declared w is the minimum.
    /// Button: align 1 keeps the declared rect's centre, else its left edge. Label: the box sits
    /// around the text at its anchor and its `bg` (a capsule with `pill`) is drawn behind it.
    bool auto_w{false};
    /// Set by ExpandWidgets on an auto_w widget whose rect is now its measured box (never parsed).
    bool auto_box{false};
    /// Value ("group"): thousands separators in the number(s) shown ("12,345"); `group_sep` is
    /// the separator (default ",").
    bool group{false};
    std::string group_sep{","};
    // --- runtime 16: image transforms and fills ------------------------------------------------
    /// Image: a fixed rotation in degrees (+ = clockwise), about `pivot`.
    float rotate{0.0f};
    /// Image: degrees read from this published value, added to `rotate`.
    std::string rotate_bind;
    /// Image: scale x 1000 read from this published value (1000 = 1x), about `pivot`; a missing
    /// value draws at 1x.
    std::string scale_bind;
    /// Image: the rotate / scale centre, px from the rect's top-left; unset = the rect's centre.
    std::array<float, 2> pivot{0.0f, 0.0f};
    bool has_pivot{false};
    /// Image / Pips / Bar image: multiplies the texels (Pips: the lit pips' colour), replacing
    /// `color` in that role. `tint_bind` picks tint_colors[value]; out of range = `tint` / `color`.
    u32 tint{0xFFFFFFFFu};
    bool has_tint{false};
    std::string tint_bind;
    std::vector<u32> tint_colors;
    /// Image / Bar image: how the picture fills the rect, and the 9-slice insets {l, t, r, b} in
    /// source px.
    ImageFill fill{ImageFill::Stretch};
    std::array<s32, 4> slice{0, 0, 0, 0};
    /// Bar: the direction the bar fills from, and an optional picture for the filled part
    /// (manifest key "image").
    BarFillDir fill_dir{BarFillDir::Right};
    std::string fill_image;
    // --- runtime 17 ------------------------------------------------------------------------------
    /// Chart: sampling and scale (null on every other type).
    std::shared_ptr<const ChartSpec> chart;
};

/// A drag-to-scroll list region on a page (page "scrolls", or a widget's inline "scroll" object).
/// Repeat templates naming it are shifted up by the region's pixel offset, clipped to `rect`, and
/// only their visible rows are expanded.
struct ScrollRegion {
    std::string id;
    std::array<s32, 4> rect{0, 0, 0, 0}; ///< viewport, canvas px
    std::string count_bind;              ///< element count; empty = the templates' own repeat count
    s32 row_h{0};   ///< content row pitch, px (0 = the first template's repeat pitch)
    s32 cols{1};    ///< elements per row (a grid)
    s32 pad{0};     ///< extra content height below the last row, px
    PointGate show; ///< region takes gestures / draws its bar only while open (empty = always)
    std::string reset_bind; ///< offset returns to 0 whenever this value changes (a tab switch)
    bool fling{true};       ///< keep gliding after a flick, slowing by `friction`
    float friction{0.135f}; ///< velocity kept per second of fling (0..1)
    std::string bar_src;    ///< Optional decoded game-art scrollbar thumb.
    std::string bar_track_src;
    u32 bar_color{0};       ///< scrollbar thumb colour (0 = no bar)
    u32 bar_track{0};       ///< scrollbar track colour (0 = none)
    s32 bar_w{6};           ///< bar width, px, drawn inside the rect's right edge
};

struct Page {
    std::string id;
    std::string title;
    std::vector<Widget> widgets;
    std::vector<ScrollRegion> scrolls;
    /// When set, the page shows a crop of the game's own frame instead of drawn widgets:
    /// {x, y, w, h}, normalised to the source. The game's art, live and animated, with nothing
    /// extracted or shipped.
    bool mirror{false};
    std::array<float, 4> mirror_rect{0.0f, 0.0f, 1.0f, 1.0f};
    /// While this page is current, a "page_binds" edge never navigates away from it (the edge is
    /// dropped, not queued). Manual taps are unaffected.
    bool no_auto_leave{false};
    /// Runtime 17: controller focus order, widget ids (a repeat template's own "x_{i}" id stands
    /// for all its elements). Empty = geometric navigation over every tappable widget.
    std::vector<std::string> nav_order;
};

} // namespace Core::Mods
