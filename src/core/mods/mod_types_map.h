// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 2: the geometry map. Rooms, areas, markers, overlay layers, room
// categories, labels, the renderer's style block (manifest "map.style") and the fog-of-war
// grid. Drawn by the Map widget (mod_ui_map_widget.cpp, mod_map.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "core/mods/mod_types_points.h"

namespace Core::Mods {

/// One room on a game's map, in the game's own coordinates. Generated from the game's own data,
/// so nothing here is authored by hand.
struct MapRoom {
    std::string area;
    std::string sprite; ///< asset reference for the room's own drawn shape
    float x{};
    float y{};
    float w{}; ///< sprite size in pixels; divided by pixels-per-unit when drawn
    float h{};
};

/// A marker on a geometry map, in the game's own world coordinates.
struct MapMarker {
    std::string kind;
    std::string icon;
    float x{};
    float y{};
    PointGate show;    ///< drawn only while this is open (empty = always)
    PointGate hide;    ///< hidden while this is open
    std::string group; ///< visibility group (Map widget "groups")
    float opacity{1.0f};
    s32 size{0};           ///< icon px; 0 = map.style.item_icon
    std::string name;      ///< scenario actor instance (blockage shields); empty for plain markers
    std::string vignette;  ///< hidden-room vignette that suppresses this icon until dispelled
    bool hidden{false};    ///< set live from death props / vignette state
    bool opened{false};    ///< set live from "<actor>:DOOR:Opened" (informational)
    bool collected{false}; ///< Items: set live from "<actor>:PICKABLE:PickedUp"
    bool unveiled{
        false}; ///< Items: "<actor>:PICKABLE:Unveiled" == 1 (a hidden item found, not yet taken)
    bool veiled{
        false}; ///< Items: "<actor>:PICKABLE:Unveiled" == 0 (hidden in a block, still unknown)
    // Doors/blockages carry their world-space extent [minx,miny,maxx,maxy]: the renderer stretches
    // the glyph to fill it so the door spans the connector, instead of a fixed-pixel square.
    bool has_box{false};
    float bx0{}, by0{}, bx1{}, by1{};
    /// Items: the item's own map box (the package's grid cells): the region the
    /// minimap pulses for a hidden item, instead of the whole room.
    bool has_pulse_box{false};
    float px0{}, py0{}, px1{}, py1{};
    /// Items hidden in blocks: an optional hint rectangle; room pixels inside it pulse.
    bool has_hint_box{false};
    float hx0{}, hy0{}, hx1{}, hy1{};
    /// Explicit per-icon behaviour ("icons[]" entries "structural"/"open_icon"/"collected_icon"),
    /// replacing the old Door/Blockage-prefix and Adquired-suffix string conventions (a package
    /// that sets these explicitly need not follow those name prefixes). Left unset, each
    /// reproduces the exact old behaviour by falling back to the same convention, computed from
    /// `icon` at draw time, so an existing package's JSON needs no change.
    std::optional<bool>
        structural; ///< size at map.style.door_icon; unset = icon starts_with("Door"/"Blockage")
    std::string open_icon; ///< atlas cell shown once `opened`; empty = the old Door suffix rule
    std::string collected_icon; ///< atlas cell shown once `collected`; empty = "<icon>Adquired"
    /// Hide while `veiled` and not yet `collected`, and blink while uncollected -- the way Dread's
    /// own inventory pins behave. Unset = `kind == "Items"` (the old hardcoded check).
    std::optional<bool> collectible;
};

/// One coloured overlay drawn on top of the base map shape. `kind` is a free-form label kept only
/// for logging/identification -- the renderer never compares it to a string literal;
/// `category`/`live_clip`/`post_fog` (below) declare the behaviour instead.
struct MapLayer {
    std::string geo; ///< asset reference for this layer's packed vertex/index blob
    std::string
        kind; ///< free-form label (water, magnet, zone, hazard, ...); not matched by the runtime
    u32 color{0}; ///< ARGB tint; overridden per-pixel by color_bind/color_map when bound
    /// Generic colour-by-state, same shape as Widget's text_bind/text_map. When
    /// `color_bind` names a published value (a point, a derived value, a module output) that
    /// resolves to an integer and `color_map` has an entry for it, that colour is used instead of
    /// `color`. Resolution happens once per rebuild (never per pixel) -- see ModRuntime::
    /// ResolveBoundColor. Unset `color_bind` (the default) always uses `color`.
    std::string color_bind;
    std::shared_ptr<const std::unordered_map<s64, u32>> color_map;
    /// This layer is the bake.mode:"layer" geometry source for the room_categories
    /// entry with this id (replaces the old `kind == "zone"` match). Empty = not a category layer.
    std::string category;
    /// Not baked into the static cached base; each rebuild its drawn pixels are
    /// tested against live rectangles a native module publishes and kept only where contained
    /// (replaces the old `kind == "water"` match).
    bool live_clip{false};
    /// Composited AFTER the fog/reveal mask, over already-revealed cells only
    /// (replaces the old `kind == "magnet"` match). Every post_fog layer composites, in
    /// declaration order, not just one hardcoded layer.
    bool post_fog{false};
    /// Per-layer colour pre-multiply for the generic fallback path (no category/
    /// live_clip/post_fog set) -- heat, freeze, and any future kind. Unset = map.style.hazard_gain
    /// (today's exact behaviour). live_clip/post_fog layers are governed by map.style's own
    /// water_gain / (no gain, unchanged) and do not read this field.
    std::optional<float> gain;
};

/// Markers drawn from array points ("dynamic_markers"): slot i reads "<x>", "<y>", "<kind>" with
/// "{i}" replaced, world = value * scale + offset (the game's own custom pins).
struct DynamicMarkerDef {
    std::string group;
    s64 count{0};
    std::string x, y, kind;
    bool has_hide_kind{false};
    s64 hide_when_kind{0};
    std::unordered_map<s64, std::string> icon_by_kind;
    std::string icon_default;
    std::string icon; ///< every slot's icon when there is no kind point
    float scale_x{1.0f}, scale_y{1.0f};
    float offset_x{0.0f}, offset_y{0.0f};
    s32 size{0};          ///< px; 0 = map.style.item_icon
    s32 selected_size{0}; ///< px of the selected slot; 0 = size
    std::string selected_icon;
    s32 selected_icon_size{0}; ///< 0 = 1.5 x selected size
    float opacity{1.0f};
    float anchor_x{0.5f}, anchor_y{0.5f};
    PointGate show, hide;
    // Runtime 14, per slot ("{i}" replaced like x / y / kind):
    /// Text point naming an image key: when non-empty, the slot draws that picture instead of
    /// its atlas icon (a unit portrait). An image still loading draws nothing yet.
    std::string icon_src_bind;
    /// Marker size in world units (scales with the map's zoom); 0 = the pixel `size`.
    float size_world{0.0f};
    float size_max{0.0f}; ///< Optional screen-pixel cap on a world-sized marker.
    /// A bar under the icon: value / max (bar_max_bind, else bar_max), filled with bar_color
    /// over bar_bg; bar_h px tall (0 = an eighth of the icon, at least 3).
    std::string bar_bind, bar_max_bind;
    s64 bar_max{100};
    u32 bar_color{0xFF40D060u};
    u32 bar_bg{0xC0000000u};
    s32 bar_h{0};
    /// Gate: the slot draws dimmed (an acted unit).
    std::string dim_bind;
    /// Int point, ARGB: a frame around the icon (an army's colour); 0 or missing = none.
    std::string frame_color_bind;
    s32 frame_px{0}; ///< frame thickness in px; 0 = a sixteenth of the icon, at least 2
    /// Int point, ARGB: multiplies the icon's colour (alpha included); 0 or missing = none.
    std::string tint_bind;
};

/// A text label at a world position ("labels"): a region name shown once visited.
struct MapLabel {
    std::string text;
    std::string text_src; ///< "msbt:<alias>#<label>", wins over text
    float x{}, y{};
    PointGate show, hide;
    std::string group;
    float opacity{1.0f};
    // Per-label style; unset values come from the Map widget's label_style.
    s32 text_scale{0};
    std::optional<u32> color, outline_color;
    s32 outline{-1};
};

/// One named, ordered room category (replaces the old hardcoded gold_polys/transport_polys/
/// emmy_polys fields and their three near-identical accessors). Array order in
/// MapArea::room_categories is precedence, highest first -- the same ladder the old `em ? tp ? sp
/// ?` ternary hardcoded.
struct MapRoomCategory {
    std::string id; ///< stable id, referenced by a MapLayer's `category` field
    /// Per-room polygons in world coords, one poly per room, identical shape to the old
    /// gold_polys/transport_polys/emmy_polys: [x0,y0,x1,y1,x2,y2,...]. Scanline-filled into a
    /// cached fog-grid-resolution mask by the pre-existing, already-generic BuildRoomMask.
    std::vector<std::vector<float>> polys;
    u32 color{0};           ///< static ARGB; used when color_bind is empty/unresolved/unmapped
    std::string color_bind; ///< generic colour-by-state, same mechanism as MapLayer::color_bind
    std::shared_ptr<const std::unordered_map<s64, u32>> color_map;
    /// Pre-scale applied to this category's resolved colour before the shared "dim while locked"
    /// factor (map.style.category_dim_before_unlock). Default 1.0; Dread's EMMI category uses 0.5
    /// (the old zone_class_scale, generalized from an EMMI-only global to a per-category field).
    float class_gain{1.0f};
    /// Show this category's dim colour as soon as its cell is at least seen, even before the
    /// area-wide "map station downloaded" unlock (the old `(sp || tp) && cat != Unexplored`
    /// special case). Default false; Dread's station/transport categories set this true.
    bool reveal_before_unlock{false};
    /// How this category's solid/walked appearance bakes into the cached base raster.
    struct Bake {
        /// "polys" (default): paint `color` directly onto this category's own polys mask (the old
        /// paint_rooms lambda). "layer": don't paint from polys; whichever MapLayer declares
        /// `"category": "<this id>"` supplies the geometry instead (the old `kind == "zone"`
        /// block) -- for a category whose solid-fill extent is authored separately from the
        /// room-camera polygons used to classify it (Dread's EMMI zone).
        std::string mode{"polys"};
        float gain{1.0f}; ///< pre-multiply for the "layer" bake mode (the old zone_color_gain)
    } bake;
    /// Per-category counterpart of MapStyle's own visited_gain/visited_tint/visited_amount (see
    /// that struct's doc comment for the shared mechanism). MapStyle's fields apply only to an
    /// UNCATEGORIZED cell (matched == false in `appear()`, mod_map.cpp); without these, a category
    /// cell's own baked colour (station/transport's flat `color`, or an EMMI cell's layer-baked,
    /// `bake.gain`-scaled colour) passes through untouched and reads too dark on device. These
    /// three fields let each category opt into the SAME gain/tint/amount correction
    /// independently, with the same defaults (256/anything/0, a true no-op) so a category that
    /// does not set them renders byte-for-byte as before.
    s32 visited_gain{256};
    u32 visited_tint{0xFF5A82B4};
    int visited_amount{0};
    /// One gain/tint/amount triple, as resolved for a cell.
    struct VisitedCorrection {
        s32 gain{256};
        u32 tint{0xFF5A82B4};
        int amount{0};
    };
    /// Per-state overrides of the triple above, keyed by the value `color_bind` resolves to (the
    /// same keys as `color_map`). A correction is solved against ONE state's colour, so a
    /// category whose colour follows state needs one per state: the triple above applies to any
    /// state without an entry here. Null (the default) = the triple above for every state.
    std::shared_ptr<const std::unordered_map<s64, VisitedCorrection>> visited_map;

    /// The correction for a cell of this category while `color_bind` resolves to `state`
    /// (nullopt = unbound/unresolved, which takes the category-level triple).
    [[nodiscard]] VisitedCorrection VisitedFor(std::optional<s64> state) const {
        if (state && visited_map) {
            if (const auto it = visited_map->find(*state); it != visited_map->end()) {
                return it->second;
            }
        }
        return {visited_gain, visited_tint, visited_amount};
    }
};

/// The walked/seen colour correction `appear()` (mod_map.cpp) runs on a zone pixel: each channel
/// is scaled by gain/256, then moved toward `tint` by amount/256 (clamped to a byte). gain 256 with
/// amount 0 returns `rgb` unchanged. Byte-order agnostic; the alpha byte is dropped.
[[nodiscard]] inline u32 ApplyVisitedCorrection(u32 rgb, s32 gain, u32 tint, int amount) {
    const auto ch = [&](int sh) {
        const int base_c = static_cast<int>((rgb >> sh) & 0xFF);
        const int gained = std::clamp(base_c * gain / 256, 0, 255);
        const int t = static_cast<int>((tint >> sh) & 0xFF);
        return static_cast<u32>(std::clamp(gained + (t - gained) * amount / 256, 0, 255)) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

/// An area whose map is geometry rather than room sprites: an indexed triangle list in world
/// units, plus the bounds needed to fit it to the panel.
struct MapArea {
    std::string geo; ///< asset reference for the packed vertex/index blob (base shape)
    /// Authored coarse sections shown by a downloaded map, independently of detailed geometry.
    /// Each record is one section identity; adjacent sections retain their own boundaries even
    /// when they use the same palette class. A present empty list deliberately reveals nothing.
    struct OverviewRegion {
        std::string kind;
        std::vector<std::array<float, 6>> tris;
    };
    std::optional<std::vector<OverviewRegion>> overview_regions;
    /// A prerendered picture to draw instead of rasterising geometry: the game's own map art,
    /// fitted to the same world bounds (min/max). When set, no fog grid is kept for the area --
    /// the whole image is visible -- and markers still land by world position on top of it.
    std::string image;
    /// Image-mode areas carry no fog: pin/room logic that only makes sense over the reveal grid
    /// is suppressed. no_pin additionally hides the live player marker (an interior map whose
    /// coordinate space is not yet calibrated, for instance).
    bool no_pin{false};
    /// Follow view: world units of context allowed past the followed room / area edge.
    float follow_pad{400.0f};
    /// Keep the drawn window inside [min, max] on each axis (centred where the view is larger)
    /// instead of only clamping the view centre -- a picture map never shows empty margins.
    bool clamp_view{false};
    float min_x{}, min_y{}, max_x{}, max_y{};
    std::vector<MapMarker> markers;
    std::vector<DynamicMarkerDef> dynamic_markers;
    std::vector<MapLabel> labels;
    std::vector<MapLayer> layers; ///< coloured overlays drawn over the base, in order
    /// N named, ordered room categories (replaces the old gold_polys/transport_polys/
    /// emmy_polys fields). Array order = precedence, highest first. An area with no categorised
    /// rooms at all simply omits this (empty = every pixel falls through to room_fill/
    /// room_class_color, unchanged).
    std::vector<MapRoomCategory> room_categories;
    // Occluder polygons: per destructible actor, the near-black concealment triangles the real
    // minimap draws over the room while the actor lives (dropped once its death prop is set).
    struct MapOccluder {
        std::string name;                       ///< scenario actor instance
        std::vector<std::array<float, 6>> tris; ///< world-space triangles x0,y0,x1,y1,x2,y2
    };
    std::vector<MapOccluder> occluders;
    std::vector<MapOccluder> vignettes; ///< whole hidden rooms, blacked out until dispelled
    // Every camera rect [minx,miny,maxx,maxy]. The map clamps its view centre inside the active
    // camera and can snap between rooms.
    std::vector<std::array<float, 4>> camera_rects;
};

/// How the geometry-map renderer colours and sizes things. Declared once per package under
/// `map.style` so the shared renderer carries no package-specific palette in code. Defaults keep
/// older declarative packages rendering until they opt into explicit style values.
struct MapStyle {
    float opacity{1.0f};                 ///< opacity of the complete map widget composition
    u32 room_fill{0xFF001330};           ///< base room colour
    std::optional<u32> room_class_color; ///< unexplored-room class colour; defaults to room_fill
    u32 border{0xFFB8A938};              ///< room-shape outline
    int revealed_dim{102};               ///< /256 brightness of seen-but-not-walked cells
    /// Applied in `appear()`'s uncategorized-cell zone colour (mod_map.cpp) to correct a measured
    /// gap: an uncategorized room's real walked/seen fill (the game's own separately-baked "zone
    /// colour" raster, pp_minimap_nx.bshdat's ps_texture3) is not bit-identical to the flat
    /// room_fill/room_class_color tunable. `visited_amount` defaults to 0 (a true no-op: gain
    /// 256/256 = 1.0x, then a 0/256 blend changes nothing) so every package that does not opt in
    /// -- i.e. everything except Dread's own manifest, which sets a measured, calibrated
    /// amount/tint -- renders byte-for-byte as before. Do not change this default without
    /// re-verifying every installed package's map output.
    /// These three are the fallback for uncategorized cells only. A room_category cell uses ITS OWN
    /// visited_gain/tint/amount (MapRoomCategory, above) -- see that struct's doc comment.
    s32 visited_gain{256}; ///< walked/seen zone colour: colour x gain/256, applied before the tint
    u32 visited_tint{0xFF5A82B4}; ///< walked/seen zone colour is then lifted toward this colour...
    int visited_amount{0};        ///< ...by this /256 (0 = disabled; see the block comment above)
    int hazard_tint{150};         ///< hazard-layer blend strength /256
    int water_tint{165};          ///< water blend strength /256
    u32 fade_ticks{24};           ///< reveal / colour-change fade duration, in ticks
    s32 item_icon{66};            ///< item / save / station icon size, px
    s32 door_icon{128};           ///< door / blockage icon size, px
    s32 marker_icon{88};          ///< live player marker size, px
    s32 raster_px{1536};          ///< longest side of the cached area raster (finer = sharper)
    bool reveal_required{false};  ///< icons stay hidden until the game's reveal grid is read
    u32 wall_color{0xFF9A6B2E};   ///< fill for an intact breakable-block cell
    /// Shared "dim while locked" factor for every room category's notVisited colour (the old
    /// hardcoded 0.3f literal). Replaces station_color/transport_color/zone_active_color/
    /// zone_inactive_color/zone_class_scale/zone_color_gain, all of which moved to per-category
    /// fields on MapArea.room_categories[].
    float category_dim_before_unlock{0.3f};
    float hazard_gain{6.0f};        ///< gain applied to static hazard layers
    float water_gain{6.0f};         ///< gain applied to water layers
    bool water_clip_live{true};     ///< clip water geometry to live rectangles supplied by a module
    bool water_full_visible{false}; ///< alpha-1 water stays full RGB in every visible cell
    u32 occluder_color{0xFF000103}; ///< concealment colour for occluders and vignettes
    float grid_tile_world_size{100.0f}; ///< world units per grid tile
    std::array<u32, 5> marker_colors{
        {0xFFFF0000u, 0xFF00FF00u, 0xFFFFFF00u, 0xFFFF00FFu, 0xFF00FFFFu}};
    u32 marker_back_color{0xFF101418};
    u32 marker_glyph_color{0xFFFFFFFF};
    u32 marker_pulse_color{0xFFFFAE00}; ///< item marker pulse colour
    /// When positive, item pulse pixels brighten the map beneath them by this gain instead of
    /// using marker_pulse_color. Zero preserves the legacy fixed-colour pulse (blend toward
    /// marker_pulse_color -- see marker_pulse_peak below).
    float marker_pulse_gain{0.0f};
    /// The item-room pulse's own peak blend alpha and ping-pong period, formerly two literals
    /// hardcoded at the one composite call site (`pa = BlinkAlpha(tick, 90, 0.0f) * 0.7f`, now
    /// GeometryMapDraw::DrawBaseLayer in mod_ui_map_widget.cpp). Pulled out so a package can tune
    /// how strongly/how fast its highlight reads without an app rebuild. Defaults (0.7, 90 ticks =
    /// 1.5s @ 60Hz) reproduce those exact old literals byte-for-byte, so every package that does
    /// not set them keeps today's behaviour. Dread's own manifest raises marker_pulse_peak to 1.0
    /// (a CHOSEN, not measured, value) so the pulse's crest fully reaches marker_pulse_color
    /// instead of only a fraction of it.
    float marker_pulse_peak{0.7f};
    u32 marker_pulse_period{90};
    /// Atlas cell names for the player's own map markers (the game's custom pins),
    /// package-declared instead of the fixed literals "CustomMarker"/"CustomMarkerBack" the
    /// renderer used to look up unconditionally -- a package using different atlas names for its
    /// own equivalent icons now draws them instead of silently drawing nothing. Defaults keep
    /// every existing package's JSON working unchanged.
    std::string custom_marker_icon{"CustomMarker"};
    std::string custom_marker_back_icon{"CustomMarkerBack"};
    // --- Atlas naming of the area markers, and the marker layer's blink / pin geometry. The
    // defaults are the conventions of Metroid Dread's minimap atlas the renderer was written
    // against, so a package that sets none of them draws as before; a marker's own open_icon /
    // collected_icon / collectible / structural still win over these. ---
    /// A marker whose icon starts with this is a door: its opened cell is named by the rules
    /// below, and a boxless "...L" / "...R" pair is nudged apart to abut.
    std::string door_prefix{"Door"};
    /// Icons starting with any of these are structural (drawn at door_icon size).
    std::vector<std::string> structural_prefixes{"Door", "Blockage"};
    /// An opened door "<base><door_closed_suffix>" shows "<base><door_open_suffix>"; one ending
    /// in 'L' / 'R' shows door_opened_left / door_opened_right.
    std::string door_closed_suffix{"Closed"};
    std::string door_open_suffix{"Open"};
    std::string door_opened_left{"DoorOpenedL"};
    std::string door_opened_right{"DoorOpenedR"};
    /// A collected item shows "<icon><collected_suffix>", else collected_fallback.
    std::string collected_suffix{"Adquired"};
    std::string collected_fallback{"ItemAdquired"};
    /// Markers of this kind are collectibles (they blink until collected, wait to be unveiled).
    std::string collectible_kind{"Items"};
    /// The uncollected-item blink and the atlas player marker's pulse (BlinkAlpha: ping-pong
    /// period in ticks, lowest alpha).
    u32 item_blink_period{72};
    float item_blink_low{0.45f};
    u32 player_blink_period{48};
    float player_blink_low{0.55f};
    /// The fallback player pin (no atlas cell): two diamonds of these radii and a square core
    /// of this side, px.
    s32 pin_outer{22};
    s32 pin_inner{18};
    s32 pin_core{8};
    /// Radius of every pixel the fallback pin paints.
    [[nodiscard]] s32 PinReach() const {
        return std::max({pin_outer, pin_inner, (pin_core + 1) / 2, 0});
    }
};

/// Fog of war: which cells of an area the player has visited. Stored in image orientation
/// (row 0 = top = max_y), a coarse grid over the area's bbox. A bumped generation invalidates the
/// rasterised, masked map image so it re-renders with the newly revealed cells.
struct VisitedGrid {
    // Canonical logical visibility grid. Packages map their world-space reveal data onto this
    // fixed resolution so fog behavior is stable across render sizes.
    static constexpr int Cols = 650;
    static constexpr int Rows = 300;
    // Cell values: 0 = unexplored (masked out), 1 = revealed (seen, drawn dim), 2 = visited
    // (walked, drawn bright).
    static constexpr u8 Unexplored = 0;
    static constexpr u8 Revealed = 1;
    static constexpr u8 Visited = 2;
    std::vector<u8> cells = std::vector<u8>(static_cast<size_t>(Cols) * Rows, 0);
    // For the fade: `change_tick` is when each cell last changed category, and `prev` is what it
    // was before that change. The renderer blends from the previous appearance to the current one
    // over a short window -- so a cell fades in from nothing (prev 0) and a seen->walked upgrade
    // fades its colour (prev Revealed -> Visited) instead of popping. Both parallel to `cells`.
    std::vector<u32> change_tick = std::vector<u32>(static_cast<size_t>(Cols) * Rows, 0);
    std::vector<u8> prev = std::vector<u8>(static_cast<size_t>(Cols) * Rows, 0);
    u64 generation{0};
    u64 last_reveal_tick{0}; ///< most recent tick any cell changed (drives the per-frame re-raster)
};

} // namespace Core::Mods
