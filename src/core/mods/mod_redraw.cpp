// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The redraw decision, the off-thread redraw worker and the publish to the second screen.
//   - Change detection: UiSignature (the page-level "did anything visible change" hash),
//     WidgetDependencyHash / RepeatTemplateDependencyHash / RebuildRepeatState /
//     RepeatElementsRect (per-widget hashes), BuildRenderExtras (at most 4 merged dirty rects,
//     MergeDirtyRects / WidgetEffectiveRect).
//   - PublishUi, the publish stage: PumpNxAssets, the ~30 Hz throttle, the signature gate, page
//     transitions (DrivePageTransition, mod_pages.cpp), then DispatchRedraw -- or a synchronous
//     render into `canvas` for the debug page and EDEN_DSMOD_SYNC_REDRAW.
//   - The worker: DispatchRedraw (single coalescing mailbox), EnsureRedrawWorker /
//     StopRedrawWorker, RedrawWorkerMain ("DSModRedraw"), RunRedrawJob (renders into
//     worker_canvas, drops stale generations, publishes).
//   - RenderPageTo / RenderPageRects / PublishPartial, PublishGpuComposite (quads + textures), and
//     LookupBoundInt / ResolveBoundColor, which read the job's snapshot on the worker
//     (render_snapshot) and tick_snapshot elsewhere.
// Not here: widget drawing (RenderPage, mod_ui.cpp), map rasterisation (GetImage, mod_map.cpp),
// the transport to the renderer (video_core/dsmod/aux_routing.h).
// Threads: PublishUi, the hashes and DispatchRedraw run on the tick thread; RunRedrawJob, and
// PublishGpuComposite for a dispatched job, run on the DSModRedraw worker. Locks:
// redraw_job_mutex / redraw_job_cv (the mailbox), view_mutex, map_state_mutex, map_records_mutex,
// gpu_composite_mutex; AuxRouting takes its own ui_mutex when publishing.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <span>

#include <cstdlib>
#include <fstream>
#include "common/cityhash.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/gpu.h"

namespace Core::Mods {

namespace {

/// murmur3-style finalizer, shared by UiSignature and ModRuntime::WidgetDependencyHash
/// (dirty-region redraw) -- hoisted out of UiSignature's own local lambda so both hashes mix bits
/// the same way.
constexpr u64 MixHash(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

/// The snapshot RenderPage is drawing on this thread (the redraw worker's job), for LookupBoundInt.
/// Set and cleared by RunRedrawJob. Both stay in this file: the variable has internal linkage, so a
/// copy in another translation unit would never be set.
thread_local const StateSnapshot* render_snapshot = nullptr;

/// Set while WidgetDependencyHash computes a Map widget's "view" hash: everything the widget draws
/// from except the live marker's position (marker_x_bind / marker_y_bind), for the redraw worker's
/// marker-only redraw (RenderExtras::maps).
thread_local bool hash_without_marker = false;

/// The redraw worker's marker-only Map redraw (RenderExtras::maps): on unless
/// EDEN_DSMOD_MARKER_REDRAW is 0/false.
bool MarkerRedrawEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("EDEN_DSMOD_MARKER_REDRAW");
        return v == nullptr || *v == '\0' ||
               (std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 &&
                std::strcmp(v, "FALSE") != 0);
    }();
    return enabled;
}

/// The same font twice: glyph tables compared by value (FontGlyph is plain data).
bool SameFontMetrics(const FontMetrics& a, const FontMetrics& b) {
    if (a.line_height != b.line_height || a.first_codepoint != b.first_codepoint ||
        a.ascent != b.ascent || a.glyphs.size() != b.glyphs.size() ||
        a.extra.size() != b.extra.size()) {
        return false;
    }
    if (!a.glyphs.empty() &&
        std::memcmp(a.glyphs.data(), b.glyphs.data(), a.glyphs.size() * sizeof(FontGlyph)) != 0) {
        return false;
    }
    for (const auto& [c, g] : a.extra) {
        const auto it = b.extra.find(c);
        if (it == b.extra.end() || std::memcmp(&it->second, &g, sizeof(FontGlyph)) != 0) {
            return false;
        }
    }
    return true;
}

/// An immutable shared copy of `now` for a redraw job: `cache` again while the font is unchanged,
/// a fresh copy (kept in `cache`) when it changed.
std::shared_ptr<const FontMetrics> SharedFontMetrics(std::shared_ptr<const FontMetrics>& cache,
                                                     const FontMetrics& now) {
    if (!cache || !SameFontMetrics(*cache, now)) {
        cache = std::make_shared<const FontMetrics>(now);
    }
    return cache;
}

/// Whether a widget's own content changes purely with the clock, independent of any bound value --
/// so a per-widget dependency hash (which only reflects bound VALUES) must fold in a tick epoch or
/// it would report "unchanged" forever on a widget that is visibly moving. Exactly the test
/// UiSignature's own per-page `animating` flag already uses for these widgets (historically inline
/// in UiSignature); shared here so the page-level flag and the per-widget hash can never disagree
/// about which widgets animate on their own.
constexpr bool WidgetSelfAnimates(const Widget& widget, bool picture_map) {
    return (widget.type == WidgetType::Map && !picture_map) || widget.pulse ||
           widget.spin != 0.0f || widget.shake != 0.0f;
}
} // namespace

std::optional<s64> ModRuntime::LookupBoundInt(const std::string& name) const {
    // The generic "published value as of the last tick" lookup color_bind (and the
    // base-raster cache-key generalisation, see GetImage) read from -- the exact same store the
    // console `value <name>` command and EvaluateDerived's own name resolution already use.
    if (name.empty()) {
        return std::nullopt;
    }
    // Map rasterisation runs inside RenderPage, i.e. on the redraw worker for a dispatched job:
    // there it must read the job's own snapshot -- tick_snapshot is being cleared and refilled by
    // the tick thread at the same time (a data race that could read freed strings).
    const StateSnapshot& snap = render_snapshot != nullptr ? *render_snapshot : tick_snapshot;
    if (const auto it = snap.ints.find(name); it != snap.ints.end()) {
        return it->second;
    }
    if (const auto it = snap.floats.find(name); it != snap.floats.end()) {
        return static_cast<s64>(std::llround(it->second));
    }
    return std::nullopt;
}

u32 ModRuntime::ResolveBoundColor(
    u32 static_color, const std::string& color_bind,
    const std::shared_ptr<const std::unordered_map<s64, u32>>& color_map) const {
    // The color_bind/color_map primitive. Deliberately NOT called from any
    // per-pixel loop -- every call site resolves once per rebuild and hands the result down
    // through a small pre-resolved vector/local, exactly like the old hardcoded emmy_class did.
    if (const auto value = LookupBoundInt(color_bind); value && color_map) {
        if (const auto it = color_map->find(*value); it != color_map->end()) {
            return it->second;
        }
    }
    return static_color;
}

u64 ModRuntime::UiSignature(const StateSnapshot& s, u32 target_w, u32 target_h) const {
    const auto& mix = MixHash;
    const std::hash<std::string> hs;
    u64 sig = 1469598103934665603ULL;
    sig ^=
        mix((static_cast<u64>(current_page) << 40) ^ (static_cast<u64>(target_w) << 20) ^ target_h);
    for (const auto& [k, v] : s.ints)
        sig ^= mix(hs(k) ^ mix(static_cast<u64>(v)));
    for (const auto& [k, v] : s.texts)
        sig ^= mix(hs(k) ^ hs(v));
    for (const auto& m : s.custom_markers)
        sig ^= mix(mix(static_cast<u64>(std::llround(m.x))) ^
                   (static_cast<u64>(std::llround(m.y)) << 20) ^ (static_cast<u64>(m.color) << 48) ^
                   0x5A5A);
    // Floats (marker x/y, ...) quantised to 4 units so idle jitter is ignored but any real move
    // redraws.
    for (const auto& [k, v] : s.floats)
        sig ^= mix(hs(k) ^ mix(static_cast<u64>(std::llround(v * 0.25))));
    sig ^= mix(water_gen * 0x100000001B3ULL + wall_gen * 0x9E3779B1u +
               zone_gen * 0xA24BAED4963EE407ULL + marker_gen * 0x2545F4914F6CDD1DULL + 0x1234);
    // Only pages with visible time-dependent content receive an animation epoch. The previous
    // unconditional epoch forced even static status/title pages to rasterise and upload at 15 Hz.
    bool animating = false;
    bool visible_map = false;
    if (current_page < manifest.pages.size()) {
        for (const auto& widget : manifest.pages[current_page].widgets) {
            if (WidgetHidden(widget, s)) {
                continue;
            }
            visible_map = visible_map || widget.type == WidgetType::Map;
            animating = animating || WidgetSelfAnimates(widget, IsPictureMapWidget(widget, s));
        }
    }
    if (animating) {
        sig ^= mix((tick_count / 4) * 0x9E3779B97F4A7C15ULL);
    }
    const u64 fade = std::max<u64>(1, manifest.map_style.fade_ticks);
    {
        // UiSignature runs on the tick thread while MarkVisitedAt (called from the Map widget's
        // draw case) runs on the redraw worker -- this read and that write are a genuine
        // cross-thread pair, guarded here.
        std::scoped_lock lk{map_state_mutex};
        for (const auto& [area, grid] : map_visited) {
            sig ^= mix(hs(area) ^ mix(grid.generation));
            if (visible_map && tick_count - grid.last_reveal_tick < fade)
                sig ^= mix((tick_count / 4) * 0xD1B54A32D192ED03ULL); // animate the fade at ~15 Hz
        }
    }
    {
        std::scoped_lock lk{view_mutex};
        for (const auto& [key, vt] : view_state) {
            u32 zb, px, py;
            std::memcpy(&zb, &vt.zoom, 4);
            std::memcpy(&px, &vt.pan_x, 4);
            std::memcpy(&py, &vt.pan_y, 4);
            sig ^= mix(hs(key) ^ (static_cast<u64>(zb) << 32) ^ (static_cast<u64>(px) << 16) ^ py);
        }
    }
    return sig;
}

u64 UncoveredDrawInputsHash(const StateSnapshot& s) {
    const std::hash<std::string> hs;
    u64 hard = MixHash(s.drag.active ? 0xD1A6ULL : 0x0FFULL);
    if (s.drag.active) {
        hard = MixHash(hard ^ (static_cast<u64>(static_cast<u32>(s.drag.x)) << 32) ^
                       static_cast<u32>(s.drag.y));
        hard = MixHash(hard ^ static_cast<u64>(s.drag.hover) ^
                       (static_cast<u64>(s.drag.source) << 32));
    }
    const auto uncovered_key = [](const std::string& k) {
        return k.starts_with("@drag") || k.starts_with("@scroll_on:");
    };
    for (const auto& [k, v] : s.ints) {
        if (uncovered_key(k)) {
            hard ^= MixHash(hs(k) ^ MixHash(static_cast<u64>(v)));
        }
    }
    for (const auto& [k, v] : s.texts) {
        if (uncovered_key(k)) {
            hard ^= MixHash(hs(k) ^ hs(v));
        }
    }
    return hard;
}

std::pair<u64, u64> ModRuntime::UncoveredSignature(const StateSnapshot& s) const {
    // The inputs of a page draw that no per-widget dependency hash (WidgetDependencyHash) is
    // guaranteed to see, split by what a change needs (PublishUi):
    //  hard: painted outside any widget's own rect (UncoveredDrawInputsHash).
    //  soft: view transforms (pan/zoom, glides) and, while a map on this page was last drawn from
    //        the clock (a blinking item, a pulsing marker picture), the clock itself.
    const std::hash<std::string> hs;
    const u64 hard = UncoveredDrawInputsHash(s);
    u64 soft = 0x50F7ULL;
    {
        std::scoped_lock lk{view_mutex};
        for (const auto& [key, vt] : view_state) {
            u32 zb, px, py;
            std::memcpy(&zb, &vt.zoom, 4);
            std::memcpy(&px, &vt.pan_x, 4);
            std::memcpy(&py, &vt.pan_y, 4);
            soft ^= MixHash(hs(key) ^ (static_cast<u64>(zb) << 32) ^ (static_cast<u64>(px) << 16) ^
                            py ^ (vt.gliding ? 0x61ULL : 0));
        }
    }
    bool clock_map = false;
    {
        std::scoped_lock lk{map_records_mutex};
        if (map_records_page == current_page) {
            clock_map = std::ranges::any_of(map_draw_records_published,
                                            [](const MapDrawRecord& r) { return r.clock_dependent; });
        }
    }
    if (clock_map) {
        soft ^= MixHash((tick_count / 4) * 0x9E3779B97F4A7C15ULL);
    }
    return {hard, soft};
}

u64 ModRuntime::WidgetDependencyHash(const Widget& w, const StateSnapshot& s,
                                     const std::string& page_id, size_t source_index) const {
    const std::hash<std::string> hs;
    u64 h = 1469598103934665603ULL;
    // Ints first, then floats, then texts -- same lookup order WidgetHidden/GetInt and the render
    // switch use, so this reflects whatever the widget would actually read. Floats quantised to the
    // same ~4-unit step UiSignature (above) uses, so idle jitter on a bound float doesn't mark a
    // widget dirty when the page-level gate wouldn't have redrawn for it either.
    // Folded in sequence, not XORed: a name read twice (need_bind == bind is common) used to
    // cancel itself out, so that value could change without the widget ever being marked dirty.
    // The names themselves are fixed per widget and the fold is positional, so only what was found
    // (int / float / text / nothing) and its value go in -- no per-name string hash.
    const auto fold = [&h](u64 term) { h = MixHash(h + term + 0x9E3779B97F4A7C15ULL); };
    const auto add = [&](const std::string& name) {
        if (name.empty()) {
            return;
        }
        if (const auto it = s.ints.find(name); it != s.ints.end()) {
            fold(MixHash(static_cast<u64>(it->second)) ^ 1);
            return;
        }
        if (const auto it = s.floats.find(name); it != s.floats.end()) {
            fold(MixHash(static_cast<u64>(std::llround(it->second * 4.0))) ^ 2);
            return;
        }
        if (const auto it = s.texts.find(name); it != s.texts.end()) {
            fold(MixHash(hs(it->second)) ^ 3);
            return;
        }
        fold(4); // missing: a name appearing or vanishing changes the hash too
    };
    // --- Universal: visibility gating every widget type shares (WidgetHidden/WidgetHiddenHolding,
    // mod_ui_widget_state.cpp) plus the select-group highlight (RenderPage's highlight pass,
    // mod_ui.cpp; a drag's own hover highlight is not tracked here -- snapshot.drag.active forces
    // the full-page path instead, enforced explicitly in PublishUi's own `partial` condition). ---
    add(w.need_bind);
    add(w.hide_bind);
    if (w.anim) {
        add(w.anim->gate.point);
    }
    if (!w.select_group.empty()) {
        static thread_local std::string sel_key; // no allocation per widget per scan
        sel_key.assign("@sel:");
        sel_key += w.select_group;
        add(sel_key);
    }
    // --- Value/Label/Bar/Pips/Image share one `value` read in RenderPage's per-widget loop
    // (mod_ui.cpp, widget.bind -> WidgetDrawContext::value) plus these binds used by one or more of
    // those draw functions. Re-derived by reading each per-type draw function RenderPage dispatches
    // to (mod_ui.cpp, mod_ui_map_widget.cpp) one by one, not summarised: a missed bind leaves a
    // widget stale until an unrelated redraw. `text_bind` (Label, DrawLabel in mod_ui.cpp) is its
    // own field, distinct from `bind` -- an easy one to miss, since every OTHER "which value" field
    // on Widget is named close to `bind` but this one picks text out of `text_map` instead of
    // formatting a number. ---
    add(w.bind);
    add(w.max_bind);
    add(w.bind_text);
    add(w.fill_bind);
    add(w.text_bind);
    // x_bind/y_bind-positioned widgets (ExpandWidgetsInto's ResolveBindOffset, mod_ui_expand.cpp)
    // -- not repeat templates; those are hashed at the definition by the caller
    // (BuildRenderExtras), since repeat/{i} strings here would never match a published point name.
    //
    // Only while the widget is actually going to be painted this tick (`!WidgetHidden`, the exact
    // same gate WidgetEffectiveRect below already applies on the RECT side -- reused here
    // unmodified, not a new predicate).
    // A hidden x_bind/y_bind widget's live position is causally irrelevant to what's on screen --
    // nothing is drawn for it either way (RenderPage's own WidgetHidden-gated dispatch) -- so
    // hashing it while hidden only manufactures dirty churn from a bind value the widget does not
    // even use yet. The widget's hide<->show TRANSITION is never missed by this gate: `hide_bind`
    // (above) is hashed unconditionally, so the tick it actually appears/disappears still flips
    // this hash regardless of x_bind/y_bind, and `WidgetEffectiveRect`'s own prev-rect/current-rect
    // union (unchanged) still correctly invalidates a widget's last VISIBLE rect the moment it
    // hides.
    //
    // Measured live on LA's real, unmodified overworld package -- this is not a hypothetical: while
    // simply walking outdoors, LA's map page carries 13 `dgn_marker_<ri>_f<tab>` widgets (the
    // per-dungeon floor markers, merged onto this same page), of which at most 1 is ever visible;
    // the other >=12 are permanently hidden while outdoors, and each one's `x_bind`/`y_bind`
    // derived value (a function of Link's live WORLD position through THAT dungeon's own coordinate
    // formula, evaluated unconditionally regardless of location) sweeps continuously while walking.
    // Without this gate, every one of those 12 hidden widgets is marked dirty on nearly every
    // walking tick, each contributing its small-but-FAR-AWAY static fallback rect
    // `[-28,-28,56,56]` (WidgetEffectiveRect's hidden-widget fallback) into the SAME single-rect
    // accumulator (BuildRenderExtras) as the overworld Map widget's own, separately and
    // legitimately dirty (follow-camera) redraw -- not because the corner rect's own area is large,
    // but because the BOUNDING-BOX union of two disjoint rects pulls in the empty gap between them
    // too. Map's own declared rect alone is 970x828 = 803,160px, 59.98% of LA's 1240x1080 canvas --
    // just under the 60% area-cutover threshold -- but unioned with the far corner it becomes
    // 1024x962 = 985,088px, 73%: enough to trip the cutover and silently degrade every ordinary
    // walking tick from the cheap `anim-frame (partial)` path (~2.4-2.65ms, the accepted floor for
    // a legitimately dirty Map widget) to a full, ~902-widget `page-draw` (measured 8.4-30.5ms,
    // several over the 16.7ms frame budget): an overworld frame-rate drop to about 45 fps while
    // moving. The dungeon page itself is unaffected either way: WidgetEffectiveRect's
    // `!WidgetHidden` rect-side gate already makes the ONE visible dungeon marker resolve correctly
    // and cheaply; this hash-side gate changes nothing about that widget while it IS visible.
    if (!WidgetHidden(w, s)) {
        add(w.x_bind);
        add(w.y_bind);
    }
    // Image: which picture to draw (DrawImage, mod_ui.cpp).
    add(w.src_bind);
    // A widget whose content changes purely with the clock -- a non-picture Map, a breathing Rect
    // outline (`pulse`), a spinning/trembling Image (`spin`/`shake`) -- needs a tick epoch folded
    // in here, or it would report "unchanged" on every tick that doesn't ALSO move a bound value,
    // even though it is visibly animating. Same predicate and epoch constant UiSignature's own
    // page-level `animating` flag uses, so the two can never disagree about which widgets these
    // are.
    const bool picture_map = IsPictureMapWidget(w, s);
    if (WidgetSelfAnimates(w, picture_map)) {
        h ^= MixHash((tick_count / 4) * 0x9E3779B97F4A7C15ULL);
    }
    // Pan/zoom view -- generic per-widget, not Map-only: RenderPage's `if (widget.pan_zoom)`
    // (mod_ui.cpp) resolves `view` the same way for EVERY widget type (consumed by DrawImage's
    // crop, and by several of Map's own uses below). This used to be hashed only inside
    // the Map-specific branch past the early return below, which left a hypothetical
    // `pan_zoom: true` widget of any OTHER type completely untracked by this scan -- a drag/pinch
    // on it would never mark it dirty here (no shipped package has a `pan_zoom: true` widget
    // outside a Map, but nothing prevents one). Moved here, above the Map-only return, so it
    // applies uniformly; Map still gets it exactly as before (this runs before the early return,
    // not instead of the Map-specific block past it). Same key ExpandWidgets/RenderPage resolve it
    // by: widget.id when set, else "<page>#<index>" -- source_index is the caller's pre-expansion
    // index, which can differ from the expanded (post- repeat) index RenderPage actually keys by on
    // a page that mixes repeat/x_bind/y_bind widgets with a plain, unnamed, pannable one;
    // documented, not silently assumed away (same caveat this code carried when it lived in the
    // Map-only branch).
    if (w.pan_zoom) {
        const auto add_view = [&](const std::string& key, const ViewTransform& view) {
            u32 zb, px, py;
            std::memcpy(&zb, &view.zoom, 4);
            std::memcpy(&px, &view.pan_x, 4);
            std::memcpy(&py, &view.pan_y, 4);
            h ^=
                MixHash(hs(key) ^ (static_cast<u64>(zb) << 32) ^ (static_cast<u64>(px) << 16) ^ py);
        };
        std::scoped_lock lk{view_mutex};
        if (!w.id.empty()) {
            if (const auto it = view_state.find(w.id); it != view_state.end()) {
                add_view(it->first, it->second);
            }
        } else {
            // An unnamed widget's view is keyed by its EXPANDED index (RenderPage), which differs
            // from `source_index` after a repeat template: take every unnamed view of the page.
            const std::string prefix = page_id + "#";
            for (const auto& [key, view] : view_state) {
                if (key.starts_with(prefix)) {
                    add_view(key, view);
                }
            }
        }
    }
    if (w.type != WidgetType::Map) {
        return h;
    }
    // --- Map: the widest dependency set, deliberately not narrowed further -- the Map widget's
    // own redraw is the accepted cost floor (~2.4-2.65 ms) whether
    // or not this hash is exact; the win this whole design is chasing is stopping the OTHER ~900
    // widgets on a page like LA's map from redrawing for a change that never touched them, not
    // shrinking the map's own draw. Built by reading the Map widget's draw (DrawMap and
    // GeometryMapDraw, mod_ui_map_widget.cpp) end to end against this list, not from a summary of
    // it (which would only name area/room/marker/actor binds and the four generation counters) --
    // three more live-changing sources were found this way, all real, verified against the shipped
    // LA package (dualscreen/manifest.json): (a) the Map widget's own `groups` (LA sets 3:
    // icons/names/pins, each gated by "@flag:map_vis"), (b) the resolved area's
    // markers/labels/dynamic_markers, each with their own show_bind/hide_bind (LA: 27 icons, 18
    // labels, 1 dynamic_markers def of 30 pin slots) -- none of these are read through a
    // `w.<field>` access so a grep for `widget\.` inside the Map case alone would NOT have found
    // them, and (c) a flat composite's cross-fade progress (GeometryMapDraw::DrawBaseLayer,
    // mod_ui_map_widget.cpp, "@fade:<composite>"), which is not a widget field or a generation
    // counter at all. Missing any of (a)-(c) would not crash or warn -- it would leave a toggled
    // visibility group, a freshly revealed icon, a moved pin, or an in-flight reveal fade silently
    // stuck at its old picture until an unrelated redraw happened to also touch the map. ---
    add(w.area_bind);
    add(w.area_season_bind);
    add(w.room_bind);
    if (!hash_without_marker) {
        add(w.marker_x_bind);
        add(w.marker_y_bind);
    }
    add(w.actor_x_bind);
    add(w.actor_y_bind);
    h ^= MixHash(water_gen * 0x100000001B3ULL + wall_gen * 0x9E3779B1u +
                 zone_gen * 0xA24BAED4963EE407ULL + marker_gen * 0x2545F4914F6CDD1DULL);
    const std::string area = ResolveMapArea(w, s);
    // map_visited[area] generation + fade window -- the same block UiSignature computes over EVERY
    // area it has ever visited, scoped here to just this widget's resolved area (cheap: one area).
    // Same cross-thread pair as UiSignature's own map_visited loop above -- guarded.
    {
        std::scoped_lock lk{map_state_mutex};
        if (const auto it = map_visited.find(area); it != map_visited.end()) {
            h ^= MixHash(hs(area) ^ MixHash(it->second.generation));
            const u64 fade = std::max<u64>(1, manifest.map_style.fade_ticks);
            if (tick_count - it->second.last_reveal_tick < fade) {
                h ^= MixHash((tick_count / 4) * 0xD1B54A32D192ED03ULL);
            }
        }
    }
    // This widget's own pan/zoom view: hashed above, before the Map-only early return, since
    // RenderPage's own `if (widget.pan_zoom)` resolution isn't Map-specific (see that comment) --
    // Map's own case is not special here, just the widest consumer of the resolved `view`.
    // This widget's own `groups` (GeometryMapDraw::group_visible, mod_ui_map_widget.cpp, and every
    // marker/label/ dynamic-marker group membership resolves through these): hash the raw show/hide
    // point values, not GateOpen's boolean outcome -- what matters for dirtiness is "did the input
    // change", and hashing the value (not the gate's negate/hide_in semantics) is correct
    // regardless of them.
    if (w.map_extras) {
        for (const auto& [name, group] : w.map_extras->groups) {
            add(group.show.point);
            add(group.hide.point);
        }
    }
    // The resolved area's own markers/labels/dynamic_markers -- read live by the Map case
    // (GeometryMapDraw's marker and label passes, mod_ui_map_widget.cpp) but never through a
    // `widget.<field>`, so nothing above would have caught a reveal, a toggled icon, or a
    // moved/selected pin.
    if (const auto geo = manifest.map_areas.find(area); geo != manifest.map_areas.end()) {
        for (const auto& marker : geo->second.markers) {
            add(marker.show.point);
            add(marker.hide.point);
        }
        for (const auto& label : geo->second.labels) {
            add(label.show.point);
            add(label.hide.point);
        }
        for (const auto& dm : geo->second.dynamic_markers) {
            add(dm.show.point);
            add(dm.hide.point);
            if (!dm.group.empty()) {
                add("@map_sel:" + dm.group);
            }
            const auto substitute = [](std::string field, s64 i) {
                const std::string index = std::to_string(i);
                for (size_t at = field.find("{i}"); at != std::string::npos;
                     at = field.find("{i}", at)) {
                    field.replace(at, 3, index);
                    at += index.size();
                }
                return field;
            };
            for (s64 i = 0; i < dm.count; ++i) {
                add(substitute(dm.x, i));
                add(substitute(dm.y, i));
                add(substitute(dm.kind, i));
            }
        }
    }
    // A flat composite ("flat": true) cross-fading a newly revealed layer in
    // (GeometryMapDraw::DrawBaseLayer, mod_ui_map_widget.cpp, "@fade:<composite>", 0..1000): not a
    // widget field, not a generation counter, and its composite name is resolved deep inside the
    // Map case's own image-key logic -- duplicating that resolution here is more risk than it is
    // worth for a purely cosmetic ~400 ms in-flight fade (missing it degrades to "pops to the final
    // state instead of cross-fading", not a forever-stale widget). Cheaper and just as correct:
    // fold in every "@fade:" entry currently in flight, whichever composite it belongs to -- s.ints
    // is already being walked in full by UiSignature the same tick this runs on, so this adds no
    // new snapshot growth, only a bounded second pass over it (typically empty; non-empty only for
    // ~400 ms after a reveal).
    for (const auto& [k, v] : s.ints) {
        if (k.size() > 6 && k.compare(0, 6, "@fade:") == 0) {
            h ^= MixHash(hs(k) ^ MixHash(static_cast<u64>(v)));
        }
    }
    // The player's own custom markers (GeometryMapDraw::DrawCustomMarkers): not bound values, so
    // nothing above sees them move. UiSignature hashes them for the page; the page-level
    // signature used to be the only thing that noticed, through the full-page redraw a changed
    // signature with no dirty widget fell back to.
    for (const auto& m : s.custom_markers) {
        u32 fx, fy;
        const float mx = m.x, my = m.y;
        std::memcpy(&fx, &mx, 4);
        std::memcpy(&fy, &my, 4);
        fold(MixHash((static_cast<u64>(fx) << 32) ^ fy) ^ static_cast<u64>(m.color) ^ 5);
    }
    return h;
}

namespace {
std::array<s32, 4> WidgetEffectiveRect(const Widget& w, u32 canvas_w, u32 canvas_h,
                                       const StateSnapshot& snapshot, const Manifest& manifest,
                                       const FontMetrics* font, bool expanded);
void MergeDirtyRects(std::vector<std::array<s32, 4>>& rects, s32 canvas_w, s32 canvas_h);
s64 RectsArea(const std::vector<std::array<s32, 4>>& rects);
std::array<s32, 4> RectsBox(const std::vector<std::array<s32, 4>>& rects);
} // namespace

u64 ModRuntime::RepeatTemplateDependencyHash(const Page& page, size_t index,
                                             const StateSnapshot& s) const {
    const Widget& w = page.widgets[index];
    const std::hash<std::string> hs;
    // The template's own fields (unsubstituted binds) as for any widget -- in the whole-list hash
    // only: every element hashes the same fields substituted (and the same clock epoch when it
    // animates), so they never decide which rows changed...
    const u64 own = WidgetDependencyHash(w, s, page.id, index);
    u64 h = 0x1F0E5ED3A7ULL;
    // ...the list it scrolls in (thumb size/position, the region's "@scroll_on" gate)...
    if (!w.scroll.empty()) {
        if (const ScrollRegion* const region = FindScrollRegion(page, w.scroll)) {
            const ScrollMetrics& m = scan_scroll_memo.Get(page, *region, s);
            h ^= MixHash(0x5C0110000ULL ^ MixHash(static_cast<u64>(m.count)) ^
                         (static_cast<u64>(static_cast<u32>(m.max_offset)) << 20) ^
                         (static_cast<u64>(static_cast<u32>(ScrollOffset(*region, s))) << 40) ^
                         static_cast<u64>(static_cast<u32>(m.row_h)));
            h ^= MixHash(0x5C0111000ULL + static_cast<u64>(s.GetInt("@scroll_on:" + region->id)));
        }
    }
    // ...and every element exactly as ExpandWidgets builds it this tick: which ones exist, where,
    // and what each one reads.
    // One slot vector per template (reset with the page's widget_sig baseline), so a steady list
    // re-expands in place: only the element rects are recomputed.
    const bool cached = index < repeat_hash_elements.size();
    auto& slots = cached ? repeat_hash_elements[index] : repeat_hash_scratch;
    const size_t count = ExpandRepeatTemplateInto(
        page, w, s, slots, cached ? &repeat_hash_indices[index] : nullptr, &scan_scroll_memo);
    const std::span<const Widget> elements{slots.data(), count};
    h ^= MixHash(0x7E9EA7000ULL + elements.size());
    // Everything above decides the list as a whole; the per-element hashes below let the dirty
    // scan repaint just the elements that changed when the list itself did not.
    repeat_hash_level = h;
    h ^= MixHash(own + 0x0DD5EED5ULL);
    repeat_hash_elem.clear();
    repeat_hash_last_idx =
        cached ? std::span<const s64>{repeat_hash_indices[index].data(),
                                      std::min(count, repeat_hash_indices[index].size())}
               : std::span<const s64>{};
    u64 k = 0;
    repeat_hash_last = elements; // RepeatElementsRect reads these if this template is dirty
    for (const Widget& e : elements) {
        u64 eh = WidgetDependencyHash(e, s, page.id, index);
        eh ^= MixHash((static_cast<u64>(static_cast<u32>(e.rect[0])) << 32) ^
                      static_cast<u32>(e.rect[1]));
        eh ^= MixHash((static_cast<u64>(static_cast<u32>(e.rect[2])) << 32) ^
                      static_cast<u32>(e.rect[3]) ^ 0xA5A5ULL);
        eh ^= MixHash((static_cast<u64>(static_cast<u32>(e.scroll_clip[1])) << 32) ^
                      static_cast<u32>(e.scroll_clip[3]) ^ 0x5A5AULL);
        // Which element this is: everything else an element holds (its substituted text, payload,
        // src, id, keep range) is a function of its index alone.
        if (k < repeat_hash_last_idx.size()) {
            eh ^= MixHash(0xE1E3E47ULL + static_cast<u64>(repeat_hash_last_idx[k]));
        } else {
            eh ^= MixHash(hs(e.text) ^ (hs(e.payload) << 1) ^ (hs(e.src) << 2) ^ (hs(e.id) << 3));
            eh ^= MixHash(static_cast<u64>(e.keep_min) ^ (static_cast<u64>(e.keep_max) << 17));
        }
        repeat_hash_elem.push_back(eh);
        h ^= MixHash(eh + (++k) * 0x9E3779B97F4A7C15ULL);
    }
    return h;
}

void ModRuntime::RebuildRepeatState(size_t index, const StateSnapshot& s, u32 cw, u32 ch) {
    // After RepeatTemplateDependencyHash: remember each element (its index, hash and box) so a
    // later scan can tell exactly which rows changed.
    if (index >= repeat_state.size()) {
        return;
    }
    auto& st = repeat_state[index];
    st.valid = repeat_hash_last_idx.size() == repeat_hash_last.size();
    if (!st.valid) {
        return;
    }
    const FontMetrics* const text_font = font_metrics.Valid() ? &font_metrics : nullptr;
    st.level = repeat_hash_level;
    st.idx.assign(repeat_hash_last_idx.begin(), repeat_hash_last_idx.end());
    st.hash = repeat_hash_elem;
    st.rect.resize(repeat_hash_last.size());
    for (size_t k = 0; k < repeat_hash_last.size(); ++k) {
        st.rect[k] = WidgetEffectiveRect(repeat_hash_last[k], cw, ch, s, manifest, text_font, true);
    }
}

std::array<s32, 4> ModRuntime::RepeatElementsRect(const StateSnapshot& s, u32 cw, u32 ch) const {
    // Where the elements RepeatTemplateDependencyHash just expanded paint (only asked for a
    // template that turned out dirty, or when re-baselining).
    const FontMetrics* const text_font = font_metrics.Valid() ? &font_metrics : nullptr;
    s32 rx0 = std::numeric_limits<s32>::max(), ry0 = rx0;
    s32 rx1 = std::numeric_limits<s32>::min(), ry1 = rx1;
    for (const Widget& e : repeat_hash_last) {
        const auto er = WidgetEffectiveRect(e, cw, ch, s, manifest, text_font, true);
        if (er[2] > 0 && er[3] > 0) {
            rx0 = std::min(rx0, er[0]);
            ry0 = std::min(ry0, er[1]);
            rx1 = std::max(rx1, er[0] + er[2]);
            ry1 = std::max(ry1, er[1] + er[3]);
        }
    }
    return rx1 > rx0 && ry1 > ry0 ? std::array<s32, 4>{rx0, ry0, rx1 - rx0, ry1 - ry0}
                                  : std::array<s32, 4>{};
}

void ModRuntime::PublishUi(const StateSnapshot& snapshot) {
    PumpNxAssets(snapshot); // land decoded game art, drive composites (never blocks)
    auto& aux = system.GPU().DSModAux();
    if (aux.rt_capture.load()) {
        ui_signature_valid = false;   // force a full redraw when we next draw widgets
        widget_sig_page = ~size_t{0}; // and re-baseline dirty-rect tracking from scratch then too
        CancelAnimations("render-target capture");
        return; // the renderer owns the screen while it is showing a captured pass
    }
    if (manifest.pages.empty()) {
        return; // a hot-reload could leave no pages; indexing below would be out of bounds
    }
    const auto& active = manifest.pages[std::min(current_page, manifest.pages.size() - 1)];
    if (active.mirror) {
        // The page asks for the game's own pixels rather than drawn widgets: hand the renderer a
        // crop and clear our canvas so it takes the mirror path.
        aux.mirror_x.store(active.mirror_rect[0]);
        aux.mirror_y.store(active.mirror_rect[1]);
        aux.mirror_w.store(active.mirror_rect[2]);
        aux.mirror_h.store(active.mirror_rect[3]);
        aux.mirror_enabled.store(true);
        aux.ClearUi();
        ui_signature_valid = false;   // force a full redraw when the page next draws widgets
        widget_sig_page = ~size_t{0}; // and re-baseline dirty-rect tracking from scratch then too
        CancelAnimations("mirror page");
        return;
    }
    aux.mirror_enabled.store(false);
    // Consume-and-clear the redraw worker's own "still gliding" signal as early as
    // possible (before any of the early returns below could otherwise swallow it silently) --
    // the dispatch path only learns `animating` once the worker finishes a job, generations after
    // this function dispatched it, so it cannot be folded into `ui_signature_valid` inline the way
    // the synchronous path's own `animating` still is, further down.
    if (dispatch_animating.exchange(false, std::memory_order_relaxed)) {
        // A view glide is in flight: its Map widgets redraw on the next publish regardless. Not
        // an invalidation (ui_signature_valid) -- that repaints the whole page, which only the
        // map needs (BuildRenderExtras adds the map rects for glide_owed).
        render_extras.glide_owed = true;
    }
    // Widget groups advance on real time; a group that starts moving on an odd tick is drawn from
    // the next published frame on.
    UpdateGroupAnims(snapshot);
    // Throttle the drawn-widget republish to ~30 Hz: each republish forces a second Vulkan present
    // + a ~5 MB upload, so 60 Hz doubled that GPU cost for a marker that reads fine at 30 Hz. The
    // rt-capture and mirror paths above still run every tick; the dirty-flag below still drops idle
    // frames to 0 Hz. An animation publishes every tick (anim_hz 60): its frames are cheap blits.
    //
    // Restored from a ~20 Hz throttle (tick_count % 3) back to the original ~30 Hz (tick_count
    // % 2): that tighter throttle was a stopgap for exactly the problem the per-widget dependency
    // tracking (BuildRenderExtras/WidgetDependencyHash, below) now fixes at its actual cause.
    // Before: a page with a live-tracked value (the map marker following Link) had its UiSignature
    // change on nearly every attempt while that value kept moving, so the WHOLE page (bg + map +
    // decorations, not just the marker) redrew at whatever rate this gate allowed -- throttling the
    // ATTEMPT rate was the only lever available, and it only traded fps for tracking smoothness, it
    // never reduced what a firing redraw actually cost. Now: a moving marker's redraw is bounded to
    // the union of what actually changed (the Map widget's own box, plus whichever other widgets'
    // dependencies moved), so the same 30 Hz republish rate this package shipped with before the
    // throttle no longer needs trading away for it. This gate still only decides how often a redraw
    // is ATTEMPTED; the dirty-flag/signature check right below still drops idle ticks to 0 Hz
    // regardless of this rate.
    const bool animating_now = page_anim.active || page_anim_request.has_value() || group_moving;
    const bool publish_tick = (tick_count % 2) == 0;
    if (!publish_tick && !(animating_now && manifest.anim_hz >= 60)) {
        return; // ~30 Hz republish (composite too): presenting the aux every frame halves fps.
    }
    const u32 panel_w = aux.width.load();
    const u32 panel_h = aux.height.load();
    const u32 target_w = manifest.canvas_w != 0 ? manifest.canvas_w : panel_w;
    const u32 target_h = manifest.canvas_h != 0 ? manifest.canvas_h : panel_h;
    if (target_w == 0 || target_h == 0 || manifest.pages.empty()) {
        return;
    }
    // Skip the whole redraw + publish when nothing the page draws from changed; TakeUi keeps the
    // previous frame (serial unchanged), so the screen is identical for free.
    if (publish_tick && GlideViews()) { // one easing step per republish-eligible frame
        ui_signature_valid = false;     // a reset is easing the view home: redraw every tick
    }
    // UiSignature and the per-widget scan (BuildRenderExtras -> WidgetDependencyHash) read the
    // map state under map_state_mutex. The redraw worker holds that mutex for a whole map raster
    // (25-250 ms per reveal on a desktop, several times that on a handheld), and this is the
    // core-timing thread the guest's vsync event runs on: while a raster is in progress (or a
    // preempted short holder outlasts LockMapStateUnlessRaster's grace) this publish attempt is
    // skipped and the next publish tick retries (the worker could not start the job before the
    // raster ends anyway). Otherwise the mutex is held across each of the two reads (their own locks
    // re-enter), so neither can end up waiting behind a raster that starts in between. Nothing
    // is written before BuildRenderExtras, so a skip leaves no half-updated baseline.
    MapStateLock map_lock{map_state_mutex, std::defer_lock};
    if (!LockMapStateUnlessRaster(map_lock)) {
        return;
    }
    const u64 sig = UiSignature(snapshot, target_w, target_h);
    map_lock.unlock();
    // Gates WidgetDependencyHash's per-widget scan in BuildRenderExtras: only run it on a tick
    // where the cheap top-level signature already says "something changed" -- zero extra cost on
    // the common idle tick, which UiSignature already reduces to 0 redraws/s on its own. Computed
    // here (not after DrivePageTransition) so a transition tick's own redraw-once bookkeeping is
    // untouched; BuildRenderExtras is only consulted below, after a transition has already had
    // first refusal.
    // Invalidated (an asset landed, a page was shown, the renderer handed the screen back, ...):
    // the whole page is repainted whatever the dirty scan finds -- the scan tracks bound values,
    // not these.
    const bool full_owed = !ui_signature_valid;
    const bool sig_changed = full_owed || sig != last_ui_signature || render_extras.glide_owed;
    if ((page_anim.active || page_anim_request.has_value()) &&
        DrivePageTransition(snapshot, sig, target_w, target_h)) {
        return; // this tick's transition frame is published
    }
    if (!LockMapStateUnlessRaster(map_lock)) {
        return;
    }
    // The dirty scan re-baselines (and reports nothing) on a new page or a reloaded manifest: that
    // tick repaints the whole page.
    const bool rebaselined =
        widget_sig_page != current_page || current_page >= manifest.pages.size() ||
        widget_sig.size() != manifest.pages[current_page].widgets.size();
    const std::array<s32, 4> anim_dirty =
        BuildRenderExtras(snapshot, sig_changed, target_w, target_h);
    map_lock.unlock();
    const bool groups_dirty = anim_dirty[2] > 0 && anim_dirty[3] > 0;
    const bool same = !sig_changed;
    if (same && !groups_dirty) {
        return;
    }
    last_ui_signature = sig;
    ui_signature_valid = true;
    canvas.Resize(target_w, target_h);
    LoadFont();
    // These four live at function scope (not in a nested `{}`) so DispatchRedraw below can hand the
    // worker OWNED copies instead of the raw pointers the synchronous canvas.SetFont/SetIconFont
    // calls still (safely, for their own single-tick use) take .get() of. See mod_runtime.h's
    // RedrawJob comment for why a raw pointer sourced from image_cache/NxAssetState's msbt bucket
    // is not safe to outlive this stack frame once a job carrying it can be read by a different
    // thread later.
    std::shared_ptr<const Image> dispatch_font_atlas;
    static const FontMetrics no_font{};
    const FontMetrics* dispatch_font_metrics = &no_font;
    std::shared_ptr<const Image> dispatch_icon_atlas;
    FontMetrics dispatch_icon_metrics_copy;
    bool dispatch_icon_pending = false;
    if (font_metrics.Valid()) {
        // GetImage() returns shared_ptr<const Image>; .get() is safe here for the SYNCHRONOUS call
        // below regardless of threading -- image_cache's own map entry keeps the Image alive for as
        // long as this function runs, exactly today's lifetime guarantee, unchanged.
        // dispatch_font_atlas (an owned copy of the same shared_ptr) is the separate, job-safe
        // handle DispatchRedraw uses instead.
        dispatch_font_atlas = GetImage(manifest.font_atlas_src);
        dispatch_font_metrics = &font_metrics; // shared below (SharedFontMetrics): an owned,
                                               // immutable copy made only when it changed --
                                               // PumpNxAssets reassigns font_metrics wholesale.
        canvas.SetFont(dispatch_font_atlas.get(), &font_metrics);
    }
    {
        // The FontMetrics* out-param is filled by NxIconFont itself while it still holds
        // nx_assets->state_mutex internally -- this file can't take that lock directly
        // (NxAssetState is an incomplete type here by design, see mod_runtime.h).
        const FontMetrics* const icons =
            manifest.msbt.Enabled() ? NxIconFont(dispatch_icon_atlas, dispatch_icon_pending,
                                                 &dispatch_icon_metrics_copy)
                                    : nullptr;
        canvas.SetIconFont(dispatch_icon_atlas.get(), icons, dispatch_icon_pending);
    }
    const auto& page = manifest.pages[std::min(current_page, manifest.pages.size() - 1)];
    draw_list.quads.clear();
    draw_list.map_key.clear();
    draw_list.atlas_key.clear();
    draw_list.active = false;
    // The GPU compositor only pays off for a page with a map; a page without one takes the plain
    // canvas path (and the debug page always does).
    const bool page_has_map = std::ranges::any_of(
        page.widgets, [](const Widget& w) { return w.type == WidgetType::Map; });
    AuxDrawList* dl =
        gpu_composite_mode && page_has_map && page.id != "__debug" ? &draw_list : nullptr;
    // Kill switch: forces the fully-synchronous redraw regardless of `will_dispatch` below.
    // Computed this early because `will_dispatch` is needed by canvas_same_size/partial's
    // computation just below. The default is the worker path. Two past bugs on it, both fixed and
    // covered by the TSan harness's `[transition-publish]` tag group: DrivePageTransition's normal
    // completion must invalidate dispatch_page (else a post-transition partial redraw could paint
    // onto a stale worker_canvas), and RunRedrawJob must not publish a superseded in-flight job
    // (it could overwrite a transition's already-correct pixels).
    // EDEN_DSMOD_SYNC_REDRAW=1 remains as an escape hatch back to the old fully-synchronous path;
    // unset (or "0"/"false"/"FALSE") runs the worker.
    static const bool sync_redraw = [] {
        const char* off = std::getenv("EDEN_DSMOD_SYNC_REDRAW");
        return off != nullptr && std::strcmp(off, "0") != 0 && std::strcmp(off, "false") != 0 &&
               std::strcmp(off, "FALSE") != 0;
    }();
    // This tick's ordinary canvas-path render goes to the redraw worker instead of running
    // synchronously here, unless the kill switch forces the old behaviour or this page is excluded
    // by design -- only the debug page stays fully synchronous. GPU_COMPOSITE (`dl != nullptr`)
    // dispatches too. Excluded, a page with `gpu_composite` (Dread) would pay every millisecond of
    // its bottom-screen cost (the composite draw-list build AND `PublishGpuComposite`'s
    // HUD-hash/map/fade republish) on the tick thread, competing directly with guest CPU emulation
    // -- worst during an EMMI chase (a full-screen self-animating pulse widget, dirty every tick)
    // or a water-drain level change (a `water_gen`-gated prefog rebuild that fires on essentially
    // every tick for the whole drain). Every mechanism the worker needs is the one the ordinary
    // canvas path already uses on this second thread (`RenderPage` guest-memory-free,
    // `GetImage`/`image_cache` and `AuxRouting`'s publish boundary already called off the tick
    // thread, the "`draw_list` is a live ModRuntime member the tick thread mutates next tick"
    // hazard the exact same shape `RenderExtras`/`render_extras` already got the by-value-copy
    // treatment for) -- ported through, not reinvented: see `RedrawJob::gpu_composite`/
    // `RedrawJob::draw_list` (mod_runtime.h) and `RunRedrawJob`'s own composite branch below.
    const bool will_dispatch = !sync_redraw && page.id != "__debug";
    // Cutover to a full redraw once the dirty union covers most of the canvas anyway: partial's own
    // fixed overhead (the accumulator bookkeeping above, Clear()'s clip check) is pure waste once
    // most of the page is dirty. 60% is a tunable, not load-bearing -- group animations never grew
    // large enough to need this, ordinary per-widget dirtying can (e.g. a full-map
    // reveal-generation bump). GPU_COMPOSITE (dl != nullptr) is exempted: its cost model is
    // quad-count/texture-serial driven, not canvas-pixel-area driven, so there is no basis to cut
    // it over early there.
    constexpr s64 kPartialAreaCutoverPercent = 60;
    const s64 canvas_area = static_cast<s64>(target_w) * static_cast<s64>(target_h);
    // The area that will actually be repainted: the disjoint rects' total (dl == nullptr path, one
    // pass per rect), else the bounding box.
    s64 dirty_area =
        static_cast<s64>(std::max(0, anim_dirty[2])) * static_cast<s64>(std::max(0, anim_dirty[3]));
    if (dl == nullptr && render_extras.clips.size() > 1) {
        dirty_area = 0;
        for (const auto& r : render_extras.clips) {
            dirty_area += static_cast<s64>(r[2]) * r[3];
        }
    }
    // A dirty marker-only candidate map (RenderExtras::maps) usually costs the worker only its
    // marker's boxes, so its rect does not count toward the cutover. LA's map alone is 59.98% of
    // the canvas: with it counted, any second dirty widget turned a marker move into a page draw.
    // When the worker cannot narrow it, the map rect is one more clipped pass -- no more pixels
    // than the page draw it would otherwise have been.
    if (dl == nullptr && will_dispatch && MarkerRedrawEnabled() &&
        std::ranges::any_of(render_extras.maps,
                            [](const RenderMapCandidate& m) { return m.dirty; })) {
        dirty_area = RectsArea(render_extras.other_clips);
    }
    const bool area_cutover = dl == nullptr && canvas_area > 0 &&
                              dirty_area * 100 > canvas_area * kPartialAreaCutoverPercent;
    // Only the dirty rect needs pixels when nothing else forces a full redraw and the canvas still
    // holds this page's last full frame in this mode. Deliberately NOT gated on `same`: the whole
    // point of the per-widget scan above is to handle the sig-DID-change case (an ordinary value
    // moved) with a small clip instead of a full redraw -- requiring `same` here (as the group-box-
    // only mechanism alone used to) would make anim_dirty's new per-widget contribution literally
    // unreachable, since WidgetDependencyHash reads nothing UiSignature doesn't already hash a
    // superset of: whenever `same` is true, every per-widget hash is provably unchanged
    // too (the scan doesn't even run then -- see sig_changed/widgets_may_be_dirty above), so `same`
    // and "a new rect is non-empty" can never both hold at once. The group-box case (same == true,
    // a group animating on wall-clock time, sig untouched) still works exactly as before: it is the
    // only source anim_dirty can have on such a tick, so this condition reduces to the original
    // one. While snapshot.drag.active, the full-page path is taken unconditionally
    // (WidgetDependencyHash's own note: "a drag's own hover highlight is not tracked here"). The
    // drag ghost (RenderPage's synthetic widget appended after page.widgets, mod_ui.cpp) and the
    // drop-target hover highlight (RenderPage's highlight pass, keyed off snapshot.drag.hover) have
    // no index into widget_sig[]/widget_last_rect[], so the per-widget scan can never mark either
    // dirty. Without this rule they would redraw correctly only while nothing ELSE on the same page
    // is independently dirty on the same tick -- an accident of content, not a guarantee. A page
    // mixing a draggable widget with a pulse/spin/shake widget (or any other independently-dirtying
    // widget) would leave the ghost silently stale/lagging behind the finger and the hover
    // highlight silently wrong. Forced explicitly here instead of left to chance.
    //
    // Deliberately NOT forced by making `extras` itself nullptr at the RenderPageTo call below:
    // render_extras.groups must stay populated and passed through, so a widget-group animation that
    // happens to be mid-move on the SAME tick as a drag still gets its own correct group-offset
    // clip (RenderPage's `anim_group != nullptr` handling, RenderAnimGroup's box/dx/dy) -- only the
    // outer per-widget clip needs to widen to "everything" (partial=false => render_extras.clip
    // below becomes the {0,0,0,0} sentinel, which Canvas::SetClip (mod_ui_canvas.cpp) and
    // RenderPage's own bbox pre-reject (mod_ui.cpp) both already treat as "whole canvas"), not the
    // group machinery disabled outright.
    // "Does the canvas that will actually receive this tick's draw already hold a redrawable
    // baseline" branches on WHICH canvas that is -- ModRuntime::canvas (canvas_page/canvas_hud,
    // unchanged) for the synchronous carve-out, or worker_canvas
    // (dispatch_page/dispatch_w/dispatch_h, mod_runtime.h) for the dispatch path. Getting this
    // wrong in either direction is invisible to pixel diffing: reading the wrong canvas's
    // continuity here would either force a full redraw every tick (silent perf regression,
    // pixel-identical to a correct partial) or claim a partial redraw is valid when the target
    // canvas doesn't hold that baseline (visible corruption).
    const bool canvas_same_size = will_dispatch
                                      ? (dispatch_w == target_w && dispatch_h == target_h)
                                      : (canvas.Width() == target_w && canvas.Height() == target_h);
    // Asynchronously built module images (asset-free packages) landed: repaint everything once.
    const bool images_landed = module_images_landed.exchange(false);
    // The target canvas still holds this page's last frame in this mode (the partial redraw's
    // precondition).
    const bool continuity =
        !images_landed && canvas_same_size &&
        (will_dispatch ? dispatch_page == current_page
                       : (canvas_page == current_page && canvas_hud == (dl != nullptr))) &&
        !snapshot.drag.active;
    render_extras.glide_owed = false; // BuildRenderExtras has added the maps for it
    // What the page draws from beyond the per-widget dependency hashes (UncoveredSignature):
    // `hard` (the drag ghost and drop highlight, scrollbar gates) is painted outside any widget's
    // rect, so a change repaints the whole page even when some widget is dirty too; `soft` (view
    // transforms, clock-driven map content) only keeps the no-dirty-widget skip below from
    // applying.
    const auto [uncovered_hard, uncovered_soft] = UncoveredSignature(snapshot);
    const bool hard_changed = !uncovered_valid || uncovered_hard != last_uncovered_hard;
    const bool soft_changed = !uncovered_valid || uncovered_soft != last_uncovered_soft;
    last_uncovered_hard = uncovered_hard;
    last_uncovered_soft = uncovered_soft;
    uncovered_valid = true;
    if (!groups_dirty && continuity && !full_owed && !rebaselined && !hard_changed &&
        !soft_changed) {
        // The signature moved, but nothing any widget draws from did, and none of the inputs the
        // widget hashes do not cover: the frame on screen is already this state's. This used to
        // fall through to a page draw (an LA overworld walk did ~4 of them a second, 15-23 ms
        // each on the Thor, for position-derived values of hidden widgets). A page draw is still
        // what a new page, an invalidation, a resize, a lost canvas or an uncovered change get;
        // a follow glide gets its maps redrawn (glide_owed).
        return;
    }
    const bool partial = continuity && groups_dirty && !area_cutover && !hard_changed;
    render_extras.clip = partial ? anim_dirty : std::array<s32, 4>{};
    if (!partial || dl != nullptr) {
        render_extras.clips.clear(); // one pass: the whole canvas, or the GPU path's one box
    }
    bool animating = false;
    static thread_local RuntimeStageStats draw_stats;
    static thread_local RuntimeStageStats anim_stats;
    const RuntimeStageTimer draw_timer{partial ? anim_stats : draw_stats,
                                       partial ? "anim-frame (partial)" : "page-draw"};
    const auto frame_start = std::chrono::steady_clock::now();
    // The ordinary canvas-path redraw: handed to the worker (`DispatchRedraw`) when
    // `will_dispatch`, else the synchronous `RenderPageTo` call (the debug page, or the
    // EDEN_DSMOD_SYNC_REDRAW kill switch above).
    if (will_dispatch) {
        // The worker does the actual render -- its own RunRedrawJob drives the
        // real side effects this job's draw will have (fog-of-war reveal via the real
        // MarkVisitedAt, camera follow_state glide/pan under the real view_mutex,
        // map_draw_records_published, ApplyViewCorrections) and the aux publish; see RunRedrawJob's
        // own body. The tick thread only builds the job and updates its OWN dispatch_page/w/h
        // bookkeeping optimistically here -- exactly mirroring how canvas_page/canvas_hud are
        // updated optimistically right after a successful synchronous render in the branch below,
        // and just as safe here for the same reason: DispatchRedraw's own coalescing/clip-union
        // logic already guarantees no region a "skipped" generation would have painted is ever
        // lost, so correctness never depends on this function waiting for the worker to actually
        // finish this specific job (RenderPage itself only ever fails on a 0x0 canvas, already
        // excluded by the target_w/h == 0 early-return far above).
        RedrawJob job;
        job.generation = redraw_dispatch_generation.fetch_add(1, std::memory_order_relaxed) + 1;
        job.authority = redraw_authority_generation.load(std::memory_order_relaxed);
        {
            // The page never changes until a reload (which drops it): one shared copy of the page
            // on screen, made when the page changes rather than once per job.
            const size_t page_index = std::min(current_page, manifest.pages.size() - 1);
            if (!shared_page || shared_page_index != page_index) {
                shared_page = std::make_shared<const Page>(page);
                shared_page_index = page_index;
            }
            job.page_copy = shared_page;
        }
        {
            // Copy into a snapshot the worker handed back: its maps' nodes are reused.
            std::scoped_lock lk{redraw_job_mutex};
            if (spare_snapshot) {
                job.snapshot = std::move(*spare_snapshot);
                spare_snapshot.reset();
            }
        }
        job.snapshot = snapshot; // a copy: `snapshot` is a const ref this function does not own
        job.extras = render_extras;
        if (dl != nullptr) {
            job.extras.maps.clear(); // the GPU path is never narrowed
        }
        job.has_extras = groups_dirty || !render_extras.groups.empty();
        job.partial = partial;
        // `dl != nullptr` is a pure function of page identity (`gpu_composite_mode`,
        // whether `page` has a Map widget, and `page.id != "__debug"` -- all constant for a given
        // page index across ticks), so `dispatch_page == current_page` above already guarantees the
        // worker's last dispatch for this page was in the same mode -- no extra
        // "dispatch_hud"-style bookkeeping needed the way the synchronous branch's `canvas_hud`
        // tracks it (that one is needed there because `canvas`/`canvas_page` are also shared with
        // the always-canvas-mode debug page under the sync-redraw kill switch;
        // `worker_canvas`/`dispatch_page` are not). `job.draw_list` stays default-constructed
        // (RenderPage fills it during the worker's own render call, off `worker_canvas`, never
        // touching `ModRuntime::draw_list`).
        job.gpu_composite = dl != nullptr;
        job.views = GetViewState();
        job.font_atlas = dispatch_font_atlas;
        job.font_metrics_copy = SharedFontMetrics(shared_font_metrics, *dispatch_font_metrics);
        job.icon_atlas = dispatch_icon_atlas;
        job.icon_metrics_copy = SharedFontMetrics(shared_icon_metrics, dispatch_icon_metrics_copy);
        job.icon_pending = dispatch_icon_pending;
        job.target_w = target_w;
        job.target_h = target_h;
        job.page_index = current_page; // captured now -- see RedrawJob's own field comment
        DispatchRedraw(std::move(job));
        // A group's one settle frame is now in a dispatched job (published even if superseded,
        // runtime 13): stop adding its box, or a settled group would redraw its box every tick.
        for (const auto& rg : render_extras.groups) {
            if (auto g = group_anims.find(rg.key);
                g != group_anims.end() && !rg.moving && g->second.settle_pending) {
                g->second.settle_pending = false;
            }
        }
        dispatch_page = current_page;
        dispatch_w = target_w;
        dispatch_h = target_h;
    } else {
        if (!RenderPageRects(page, snapshot, dl, &animating, &map_draw_records,
                             groups_dirty || !render_extras.groups.empty() ? &render_extras
                                                                           : nullptr)) {
            // map_records_page is part of the map_records_mutex-guarded pair -- see
            // map_draw_records_published's own declaration comment (mod_runtime.h).
            std::scoped_lock rlk{map_records_mutex};
            map_records_page = ~size_t{0};
            canvas_page = ~size_t{0};
            return;
        }
        {
            std::scoped_lock rlk{map_records_mutex};
            map_draw_records_published = map_draw_records;
            map_records_page = current_page;
        }
        canvas_page = current_page;
        canvas_hud = dl != nullptr;
        // This synchronous carve-out (debug page/sync_redraw; GPU_COMPOSITE only reaches it when
        // EDEN_DSMOD_SYNC_REDRAW=1 forces it, same as any other page) is about to publish directly,
        // outside the dispatch mailbox -- bump the shared generation so a worker job that was
        // already in flight (dispatched before this tick took the synchronous branch, e.g. a
        // manifest whose pages differ in dispatch eligibility, or a mid-run toggle) is recognised
        // as stale by RunRedrawJob's own check below and does not publish its now-superseded pixels
        // afterward. This holds for any manifest shape.
        redraw_dispatch_generation.fetch_add(1, std::memory_order_relaxed);
        redraw_authority_generation.fetch_add(1, std::memory_order_relaxed);
        ApplyViewCorrections();
        if (animating) {
            render_extras.glide_owed = true; // a view glide is in flight: its maps redraw next tick
        }
        if (dl != nullptr && dl->active) {
            PublishGpuComposite(draw_list, canvas);
        } else {
            // The renderer prefers a published composite over the canvas, so a page without a map
            // must retire the last one or the stale map stays on the panel.
            aux.ClearComposite();
            // A partial tick only repainted render_extras.clip's rows -- AuxRouting::
            // PublishUiPartial (video_core/dsmod/aux_routing.h) then copies just that region to the
            // aux buffer instead of the whole ~5 MB canvas.
            if (partial) {
                PublishPartial(aux, canvas, render_extras);
            } else {
                aux.PublishUi(canvas.Width(), canvas.Height(), canvas.Pixels());
            }
        }
    }
    // The group-animation settle-timing/log block below (and DumpAnimFrame) is scoped to
    // the synchronous carve-out only -- it measures `canvas`'s own pixels/wall-clock cost, neither
    // of which is meaningful for a dispatched tick (the actual draw, and its cost, now happen on
    // the worker, generations later; `canvas` was never touched this tick at all). Skipping it here
    // is a narrow, deliberate, debug-instrumentation-only scope decision (settle logging/anim-dump
    // simply do not fire for ordinary dispatch-eligible pages), not a silent gap: the draw_timer
    // above still measures this function's own (near-zero) tick-thread cost on the dispatch path,
    // and RunRedrawJob's own "redraw-worker" stage timer reports the actual render cost
    // separately.
    if (groups_dirty && !will_dispatch) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - frame_start)
                              .count();
        for (const auto& rg : render_extras.groups) {
            auto g = group_anims.find(rg.key);
            if (g == group_anims.end() || !(rg.moving || g->second.settle_pending)) {
                continue;
            }
            ++g->second.frames;
            g->second.frame_ms += ms;
            g->second.frame_max_ms = std::max(g->second.frame_max_ms, ms);
            if (g->second.settle_pending) {
                g->second.settle_pending = false;
                const std::string line =
                    fmt::format("DSMod anim group '{}' {} after {} frame(s): {} redraw avg {:.2f} "
                                "ms max {:.2f} ms "
                                "(box {}x{})",
                                rg.key, g->second.open ? "shown" : "hidden", g->second.frames,
                                partial ? "box" : "page",
                                g->second.frame_ms / std::max<u32>(1, g->second.frames),
                                g->second.frame_max_ms, rg.box[2], rg.box[3]);
                LOG_INFO(Core, "{}", line);
                if (const char* const p = std::getenv("EDEN_DSMOD_CMD")) {
                    std::ofstream f(std::string(p) + ".out", std::ios::app);
                    if (f) {
                        f << line << '\n';
                    }
                }
            }
            if (dl == nullptr) {
                const float e = ApplyEasing(Easing::Linear, g->second.pos);
                DumpAnimFrame(canvas.Pixels(), canvas.Width(), canvas.Height(),
                              fmt::format("group_{}_f{:02}_p{:03}", rg.key, g->second.frames,
                                          static_cast<int>(e * 1000.0f)));
            }
        }
    }
}

bool ModRuntime::RenderPageTo(const Page& page, const StateSnapshot& snapshot, AuxDrawList* dl,
                              bool* animating, MapDrawRecords* records,
                              const RenderExtras* extras) {
    if (page.id == "__debug") {
        canvas.ResetClip();
        RenderDebugPage(canvas, manifest, snapshot);
        return true;
    }
    return RenderPage(
        canvas, manifest, page, snapshot, [this](const std::string& src) { return GetImage(src); },
        GetViewState(), &map_follow_state,
        [this](const std::string& area, float wx, float wy) { MarkVisitedAt(area, wx, wy); },
        [this](const std::string& area, float wx, float wy) { return IsVisited(area, wx, wy); }, dl,
        animating, [this](const std::string& ref) { return GetMsbtText(ref); }, records, extras,
        // Reuse view_mutex, not a new lock -- the same
        // mutex ApplyViewCorrections already takes to consume these corrections right after this
        // call returns. Last argument, deliberately (see RenderPage's own declaration comment).
        &view_mutex,
        // Reuse map_state_mutex -- the same lock UpdateHiddenMarkers already holds for its whole
        // body. This call site (the debug page aside, __debug returns above) also serves the
        // synchronous carve-out (page transitions, GPU_COMPOSITE), which can run concurrently with
        // the redraw worker's own RenderPage call, so this needs to be real, not a formality.
        &map_state_mutex);
}

// -------------------------------------------------------------------------------------------------
// Off-thread redraw worker: renders and publishes the second screen off the tick thread. Modelled
// directly on this codebase's own
// AuxRouting::haptic_thread/HapticLoop (video_core/dsmod/aux_routing.h) and
// NxAssetState::EnsureWorker/WorkerMain (mod_nx_runtime.cpp) -- a lazily-started std::jthread fed
// by a mutex+condition_variable_any, stopped via stop_token.
//
// There is no in-process checksum comparison of the worker's output: RunRedrawJob's own render IS
// the published frame, so pixel-equivalence of redraw-path changes is checked against baseline
// captures instead.

bool ModRuntime::RenderPageRects(const Page& page, const StateSnapshot& snapshot, AuxDrawList* dl,
                                 bool* animating, MapDrawRecords* records,
                                 const RenderExtras* extras) {
    if (extras == nullptr || extras->clips.size() <= 1 || dl != nullptr) {
        return RenderPageTo(page, snapshot, dl, animating, records, extras);
    }
    // Several disjoint dirty rects: one clipped pass each (see RenderExtras::clips).
    RenderExtras pass = *extras;
    bool ok = true;
    for (const auto& rect : extras->clips) {
        pass.clip = rect;
        bool pass_animating = false;
        ok = RenderPageTo(page, snapshot, dl, &pass_animating, records, &pass) && ok;
        if (animating != nullptr) {
            *animating = *animating || pass_animating;
        }
    }
    return ok;
}

void ModRuntime::PublishPartial(VideoCore::DSMod::AuxRouting& aux, const Canvas& from,
                                const RenderExtras& extras) {
    if (extras.clips.size() <= 1) {
        aux.PublishUiPartial(from.Width(), from.Height(), from.Pixels(), extras.clip);
        return;
    }
    // One publish for all of the rects: the renderer must not take a frame between two of them.
    aux.PublishUiPartialRects(from.Width(), from.Height(), from.Pixels(), extras.clips);
}

void ModRuntime::EnsureRedrawWorker() {
    if (!redraw_thread.joinable()) {
        redraw_thread = std::jthread([this](std::stop_token stop) { RedrawWorkerMain(stop); });
    }
}

void ModRuntime::StopRedrawWorker() {
    if (redraw_thread.joinable()) {
        redraw_thread.request_stop();
        redraw_job_cv.notify_all();
        redraw_thread.join();
    }
}

void ModRuntime::DispatchRedraw(RedrawJob&& job) {
    EnsureRedrawWorker();
    std::scoped_lock lock{redraw_job_mutex};
    if (redraw_pending_job) {
        // The worker hasn't picked up the last one yet: this tick's job replaces it (only the
        // LATEST snapshot/extras/page matter -- a skipped intermediate frame is invisible), but it
        // must still repaint everything the skipped job would have.
        const RedrawJob& old = *redraw_pending_job;
        if (!old.partial || !job.partial || old.page_index != job.page_index) {
            // Either one repaints the whole canvas (a page switch, a cutover, a landed asset):
            // so does the merged job. (This used to keep a partial job's clip here -- a full
            // redraw followed by a partial one, or the reverse, then painted only the small clip,
            // leaving the rest of a just-switched page stale until something else redrew it.)
            job.partial = false;
            job.extras.clip = {};
            job.extras.clips.clear();
        } else {
            // Both partial on the same page: repaint both dirty regions.
            const auto& old_clip = old.extras.clip;
            auto& new_clip = job.extras.clip;
            const s32 x0 = std::min(old_clip[0], new_clip[0]);
            const s32 y0 = std::min(old_clip[1], new_clip[1]);
            const s32 x1 = std::max(old_clip[0] + old_clip[2], new_clip[0] + new_clip[2]);
            const s32 y1 = std::max(old_clip[1] + old_clip[3], new_clip[1] + new_clip[3]);
            new_clip = {x0, y0, x1 - x0, y1 - y0};
            if (!job.extras.clips.empty() || !old.extras.clips.empty()) {
                if (job.extras.clips.empty()) {
                    job.extras.clips.push_back(job.extras.clip);
                }
                if (old.extras.clips.empty()) {
                    job.extras.clips.push_back(old.extras.clip);
                } else {
                    job.extras.clips.insert(job.extras.clips.end(), old.extras.clips.begin(),
                                            old.extras.clips.end());
                }
                MergeDirtyRects(job.extras.clips, static_cast<s32>(job.target_w),
                                static_cast<s32>(job.target_h));
            }
            // The marker-only bookkeeping (RenderExtras::maps): the merged job's candidates are
            // the new job's (same page, same list), dirty when either job's was; everything else
            // either job repaints is in other_clips. A candidate only the old job has (not
            // expected) repaints its whole rect.
            if (!job.extras.maps.empty()) {
                auto& other = job.extras.other_clips;
                if (old.extras.maps.empty()) {
                    if (old.extras.clips.empty()) {
                        other.push_back(old_clip);
                    } else {
                        other.insert(other.end(), old.extras.clips.begin(),
                                     old.extras.clips.end());
                    }
                } else {
                    other.insert(other.end(), old.extras.other_clips.begin(),
                                 old.extras.other_clips.end());
                    for (const auto& om : old.extras.maps) {
                        if (!om.dirty) {
                            continue;
                        }
                        const auto it = std::ranges::find_if(
                            job.extras.maps,
                            [&om](const RenderMapCandidate& m) { return m.index == om.index; });
                        if (it != job.extras.maps.end()) {
                            it->dirty = true;
                        } else {
                            other.push_back(om.rect);
                        }
                    }
                }
                MergeDirtyRects(other, static_cast<s32>(job.target_w),
                                static_cast<s32>(job.target_h));
                // Every dirty candidate's rect is in `clips` already: both jobs' clips are merged
                // above (a partial canvas-path job always carries its clips).
            }
        }
    }
    if (redraw_pending_job && !spare_snapshot) {
        spare_snapshot = std::move(redraw_pending_job->snapshot); // superseded: recycle its nodes
    }
    redraw_pending_job = std::move(job);
    redraw_job_cv.notify_one();
}

void ModRuntime::RedrawWorkerMain(std::stop_token stop) {
    // Below-normal OS scheduling priority, so the worker doesn't contend with the guest-CPU/tick
    // thread for CPU time on a real device. Reuses this codebase's OWN existing portable mechanism
    // rather than inventing a new one: Common::SetCurrentThreadPriority(ThreadPriority::Low) is the
    // same call core/hle/service/glue/time/worker.cpp's TimeWorker and common/thread_worker.h's
    // background-placement StatefulThreadWorker already use -- Windows
    // THREAD_PRIORITY_BELOW_NORMAL, Android/Linux a positive nice value, a generic sched_param
    // fallback elsewhere (common/thread.cpp), so this is portable across desktop Linux and Android
    // without a new platform branch. Checked first: neither of this codebase's own two closest
    // precedents for a lazily-started jthread worker feeding off a mutex+condition_variable --
    // AuxRouting::haptic_thread/HapticLoop (video_core/dsmod/aux_routing.h) and
    // NxAssetState::EnsureWorker/WorkerMain (mod_nx_runtime.cpp), the two this file's own comment
    // above models redraw_thread on -- actually sets a priority themselves (grepped both bodies,
    // neither calls SetCurrentThreadPriority or SetCurrentThreadName); the mechanism above is this
    // tree's real precedent for the priority call itself, not those two threads. An escape hatch,
    // EDEN_DSMOD_REDRAW_NORMAL_PRIORITY=1, leaves the worker at the default OS priority so the two
    // configurations can be A/B measured without a rebuild.
    static const bool keep_normal_priority = [] {
        const char* v = std::getenv("EDEN_DSMOD_REDRAW_NORMAL_PRIORITY");
        return v != nullptr && std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 &&
               std::strcmp(v, "FALSE") != 0;
    }();
    Common::SetCurrentThreadName("DSModRedraw");
    if (!keep_normal_priority) {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    }
    std::unique_lock lock{redraw_job_mutex};
    while (!stop.stop_requested()) {
        redraw_job_cv.wait(lock, stop, [this] { return redraw_pending_job.has_value(); });
        if (stop.stop_requested()) {
            return;
        }
        RedrawJob job = std::move(*redraw_pending_job);
        redraw_pending_job.reset();
        redraw_job_running = true;
        lock.unlock();
        RunRedrawJob(job);
        lock.lock();
        redraw_job_running = false;
        spare_snapshot = std::move(job.snapshot); // the next job copies into its nodes
    }
}

namespace {
/// What the redraw worker last painted for one marker-only candidate Map widget
/// (RenderExtras::maps): the dependency hash it was drawn from and the record of that draw.
/// `valid`: worker_canvas holds that whole draw (the view, and the marker at marker_box).
struct DrawnMapState {
    size_t index{};
    bool valid{};
    u64 view_hash{};
    MapDrawRecord rec;
};
/// The redraw worker's DrawnMapStates. One per worker thread, i.e. per runtime; reset when the
/// job's page or canvas size is not the one they describe.
struct WorkerMapMemo {
    std::shared_ptr<const Page> page;
    u32 w{}, h{};
    std::vector<DrawnMapState> maps;
    DrawnMapState* Find(size_t index) {
        for (auto& m : maps) {
            if (m.index == index) {
                return &m;
            }
        }
        return nullptr;
    }
};
thread_local WorkerMapMemo worker_map_memo;

/// The same view (area, rect, centre, scale, extent -- bit for bit), and with `content` the same
/// pictures.
bool SameMapView(const MapDrawRecord& a, const MapDrawRecord& b, bool content = true) {
    const auto same = [](float x, float y) { return std::memcmp(&x, &y, sizeof(float)) == 0; };
    return a.area == b.area && a.rect == b.rect && same(a.cx, b.cx) && same(a.cy, b.cy) &&
           same(a.ppw, b.ppw) && same(a.min_x, b.min_x) && same(a.min_y, b.min_y) &&
           same(a.max_x, b.max_x) && same(a.max_y, b.max_y) &&
           (!content || a.content_id == b.content_id);
}

bool RectInside(const std::array<s32, 4>& inner, const std::array<s32, 4>& outer) {
    if (inner[2] <= 0 || inner[3] <= 0) {
        return true;
    }
    return inner[0] >= outer[0] && inner[1] >= outer[1] &&
           inner[0] + inner[2] <= outer[0] + outer[2] &&
           inner[1] + inner[3] <= outer[1] + outer[3];
}

bool EnvOn(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 &&
           std::strcmp(v, "FALSE") != 0;
}

/// The latest record of expanded widget `index` in `records` (nullptr: none).
const MapDrawRecord* FindRecord(const MapDrawRecords& records, size_t index) {
    const MapDrawRecord* found = nullptr;
    for (const auto& r : records) {
        if (r.widget_index == index) {
            found = &r;
        }
    }
    return found;
}

/// Counters of the worker's marker-only redraw and of EDEN_DSMOD_VERIFY_REDRAW, logged every 5 s.
struct NarrowStats {
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 jobs{}, full_jobs{}, narrowed_maps{}, fallback_maps{}, failed_maps{};
    u64 verified{}, mismatched{};
};
} // namespace

void ModRuntime::RunRedrawJob(RedrawJob& job) {
    // Marker-only redraw of a Map widget (default on; EDEN_DSMOD_MARKER_REDRAW=0 turns it off) and
    // its self-check (EDEN_DSMOD_VERIFY_REDRAW=1: every partial job is compared with a full redraw
    // of the same state, mismatches logged -- a debugging aid, it doubles the worker's cost).
    const bool marker_redraw = MarkerRedrawEnabled();
    static const bool verify_redraw = EnvOn("EDEN_DSMOD_VERIFY_REDRAW", false);
    static thread_local NarrowStats nstats;
    static thread_local RuntimeStageStats worker_stats;
    static thread_local RuntimeStageStats marker_stats;
    static thread_local RuntimeStageStats map_stats;
    const bool touches_map_candidates =
        std::ranges::any_of(job.extras.maps, [](const RenderMapCandidate& m) { return m.dirty; });
    // Timed apart: a whole-map (or page) job and a marker-only one cost an order of magnitude
    // apart. Chosen after the narrowing decision below (the timer starts there).
    std::optional<RuntimeStageTimer> worker_timer;
    if (worker_canvas.Width() != job.target_w || worker_canvas.Height() != job.target_h) {
        worker_canvas.Resize(job.target_w, job.target_h);
    }
    worker_canvas.SetFont(job.font_atlas.get(), job.font_metrics_copy.get());
    worker_canvas.SetIconFont(job.icon_atlas.get(), job.icon_metrics_copy.get(), job.icon_pending);
    const ImageProvider images = [this](const std::string& src) { return GetImage(src); };
    // The memo describes worker_canvas only for this page at this size.
    auto& memo = worker_map_memo;
    if (memo.page != job.page_copy || memo.w != job.target_w || memo.h != job.target_h) {
        memo.page = job.page_copy;
        memo.w = job.target_w;
        memo.h = job.target_h;
        memo.maps.clear();
    }
    const u64 serial_start = CurrentRenderSerial();
    const auto refreshed = [&](const MapDrawRecord* r) {
        return r != nullptr && r->render_serial > serial_start;
    };
    // --- Marker-only redraw. A dirty candidate map whose last draw is fully on worker_canvas,
    // drawn from the same inputs apart from the live marker's position and not from the clock,
    // is repainted only inside the marker's old and new boxes (every widget there, in order, as
    // any clipped pass does) instead of its whole rect. Whether the view really stayed put (the
    // follow camera moves with the marker) is only known once the map has been drawn: the draw's
    // own record is checked afterwards, and a map whose view or marker box came out different is
    // then repainted whole from the follow state it started from -- so the pixels are always
    // those of an ordinary whole-map redraw. ---
    struct Narrowed {
        size_t index;
        std::array<s32, 4> predicted;
        const Widget* widget;
    };
    std::vector<Narrowed> narrowed;
    MapFollowState follow_before;
    u64 follow_epoch_before = 0;
    bool follow_saved = false;
    const auto save_follow = [&] {
        if (!follow_saved) {
            std::scoped_lock lk{view_mutex};
            follow_before = map_follow_state;
            follow_epoch_before = follow_state_epoch;
            follow_saved = true;
        }
    };
    if (marker_redraw && job.partial && !job.gpu_composite && job.has_extras &&
        touches_map_candidates && job.page_copy != nullptr) {
        std::vector<std::array<s32, 4>> clips = job.extras.other_clips;
        const auto& pw = job.page_copy->widgets;
        save_follow();
        for (const auto& cand : job.extras.maps) {
            if (!cand.dirty) {
                continue;
            }
            DrawnMapState* const st = memo.Find(cand.index);
            bool ok = cand.index < pw.size() && st != nullptr && st->valid &&
                      st->view_hash == cand.view_hash && !st->rec.clock_dependent;
            std::array<s32, 4> predicted{};
            if (ok) {
                // Where the camera will be (FrameView on a copy of the follow state): a follow
                // map that moves with the marker is repainted whole straight away.
                const auto v = ProbeMapView(manifest, *job.page_copy, job.snapshot, pw[cand.index],
                                            cand.index, job.views, follow_before);
                ok = v.has_value() && SameMapView(*v, st->rec, false);
            }
            if (ok) {
                predicted =
                    PredictMapMarkerBox(manifest, pw[cand.index], job.snapshot, st->rec, images);
                const auto& old_box = st->rec.marker_box;
                // Nothing to paint for the marker (none drawn before or now): an ordinary redraw.
                ok = (predicted[2] > 0 && predicted[3] > 0) || (old_box[2] > 0 && old_box[3] > 0);
                if (ok) {
                    if (predicted[2] > 0 && predicted[3] > 0) {
                        clips.push_back(predicted);
                    }
                    if (old_box[2] > 0 && old_box[3] > 0) {
                        clips.push_back(old_box);
                    }
                    narrowed.push_back({cand.index, predicted, &pw[cand.index]});
                }
            }
            if (!ok) {
                clips.push_back(cand.rect);
                ++nstats.fallback_maps;
            }
        }
        if (!narrowed.empty()) {
            MergeDirtyRects(clips, static_cast<s32>(job.target_w), static_cast<s32>(job.target_h));
            if (clips.empty()) {
                // Everything clipped away (a marker entirely off the canvas): repaint as dispatched.
                narrowed.clear();
            } else {
                job.extras.clips = std::move(clips);
                job.extras.clip = RectsBox(job.extras.clips);
                nstats.narrowed_maps += narrowed.size();
            }
        }
    }
    worker_timer.emplace(!narrowed.empty() ? marker_stats
                         : touches_map_candidates || !job.partial ? map_stats
                                                                  : worker_stats,
                         !narrowed.empty()                        ? "redraw-worker(marker)"
                         : touches_map_candidates || !job.partial ? "redraw-worker(map/page)"
                                                                  : "redraw-worker");
    if (verify_redraw && job.partial && !job.gpu_composite) {
        save_follow();
    }
    // Published into map_draw_records_published below on success, under map_records_mutex,
    // exactly mirroring what PublishUi's own synchronous branch does with the tick-thread-owned
    // map_draw_records member.
    // The records accumulate in the persistent, worker-thread-exclusive `worker_map_draw_records`
    // member, not a fresh local per call: RenderPage does not clear them on a genuinely partial
    // redraw, and a container that never survives past the call that populated it would have
    // nothing to preserve (mod_runtime.h,
    // declared right next to `worker_canvas`, which already established this exact "one long-lived,
    // worker-owned buffer, never a fresh one per job" precedent for the pixels themselves -- see
    // its own comment for why a fresh buffer per job forces every partial redraw back to full). A
    // widget this job's own dirty rect doesn't cover (the Map widget, on the ordinary HUD-only
    // partial redraw the pin panel's own opening animation triggers) now correctly keeps the entry
    // the LAST job that actually drew it left behind, instead of that entry vanishing the moment
    // any other widget on the page redraws. Bound values the renderer's map rasterisation reads
    // (LookupBoundInt) come from this job.
    render_snapshot = &job.snapshot;
    const auto render_passes = [&](const RenderExtras& extras, bool use_extras, bool& animating) {
        // A partial job with several disjoint dirty rects: one clipped pass per rect.
        const bool multi = use_extras && job.partial && extras.clips.size() > 1;
        RenderExtras pass_extras;
        if (multi) {
            pass_extras = extras;
        }
        bool ok = true;
        for (size_t pass = 0; pass < (multi ? extras.clips.size() : size_t{1}); ++pass) {
            if (multi) {
                pass_extras.clip = extras.clips[pass];
            }
            bool pass_animating = false;
            ok = RenderPage(
                     worker_canvas, manifest, *job.page_copy, job.snapshot, images, job.views,
                     &map_follow_state,
                     // The REAL report_visit/follow_state: this call IS the one and only render for
                     // this job, so it must drive the real fog-of-war reveal and the real camera
                     // glide/pan, under the same locks the synchronous call uses: map_state_mutex
                     // (MarkVisitedAt/GeometryMask, recursive-safe) and view_mutex
                     // (follow_state_mutex, reused, matching RenderPageTo's own choice).
                     [this](const std::string& area, float wx, float wy) {
                         MarkVisitedAt(area, wx, wy);
                     },
                     [this](const std::string& area, float wx, float wy) {
                         return IsVisited(area, wx, wy);
                     },
                     // An owned, by-value AuxDrawList (RedrawJob::draw_list) for a GPU_COMPOSITE
                     // page -- RenderPage fills it during THIS call, off `worker_canvas`, exactly
                     // like the synchronous carve-out's `&draw_list` fills the tick-thread member
                     // off `canvas`. Never touches `ModRuntime::draw_list` (that stays exclusively
                     // owned by the synchronous/kill-switch path).
                     job.gpu_composite ? &job.draw_list : nullptr, &pass_animating,
                     [this](const std::string& ref) { return GetMsbtText(ref); },
                     &worker_map_draw_records,
                     multi        ? &pass_extras
                     : use_extras ? &extras
                                  : nullptr,
                     &view_mutex,
                     // Same reuse as RenderPageTo's own call -- see
                     // UpdateHiddenMarkers's own updated comment.
                     &map_state_mutex) &&
                 ok;
            animating = animating || pass_animating;
        }
        return ok;
    };
    bool animating = false;
    bool ok = render_passes(job.extras, job.has_extras, animating);
    // Check each marker-only map against its own record (see above).
    std::vector<std::array<s32, 4>> repaint;
    for (const auto& n : narrowed) {
        const MapDrawRecord* const r = FindRecord(worker_map_draw_records, n.index);
        const DrawnMapState* const st = memo.Find(n.index);
        const bool good = refreshed(r) && st != nullptr && SameMapView(*r, st->rec) &&
                          !r->clock_dependent && RectInside(r->marker_box, n.predicted);
        if (good) {
            continue;
        }
        ++nstats.failed_maps;
        const auto rect = std::ranges::find_if(job.extras.maps, [&n](const RenderMapCandidate& m) {
                              return m.index == n.index;
                          })->rect;
        // The whole map, and every marker box this job or the last draw may have painted (the
        // diamond is not clipped to the widget, so it can reach outside `rect`).
        repaint.push_back(rect);
        repaint.push_back(n.predicted);
        if (st != nullptr) {
            repaint.push_back(st->rec.marker_box);
        }
        if (r != nullptr) {
            repaint.push_back(r->marker_box);
        }
        // Back to the follow state this job started from, for this widget only (its glide
        // step and pan correction are taken again by the repaint) -- unless another thread reset
        // or consumed follow state meanwhile (a manifest reload, a page transition's
        // ApplyViewCorrections): then the repaint steps on from what is there now.
        const std::string vkey = n.widget->id.empty()
                                     ? job.page_copy->id + "#" + std::to_string(n.index)
                                     : n.widget->id;
        const auto mine = [&vkey](const std::string& key) {
            return key == vkey + "#pan" ||
                   (key.size() > vkey.size() && key.compare(0, vkey.size(), vkey) == 0 &&
                    key[vkey.size()] == '@');
        };
        std::scoped_lock lk{view_mutex};
        if (follow_state_epoch != follow_epoch_before) {
            continue;
        }
        std::erase_if(map_follow_state, [&mine](const auto& kv) { return mine(kv.first); });
        for (const auto& [k, v] : follow_before) {
            if (mine(k)) {
                map_follow_state[k] = v;
            }
        }
    }
    if (!repaint.empty()) {
        MergeDirtyRects(repaint, static_cast<s32>(job.target_w), static_cast<s32>(job.target_h));
        RenderExtras again = job.extras;
        again.clips = repaint;
        again.clip = RectsBox(repaint);
        ok = render_passes(again, true, animating) && ok;
        job.extras.clips.insert(job.extras.clips.end(), repaint.begin(), repaint.end());
        MergeDirtyRects(job.extras.clips, static_cast<s32>(job.target_w),
                        static_cast<s32>(job.target_h));
        job.extras.clip = RectsBox(job.extras.clips);
    }
    render_snapshot = nullptr;
    // What worker_canvas now holds of each candidate map.
    for (const auto& cand : job.extras.maps) {
        const MapDrawRecord* const r = FindRecord(worker_map_draw_records, cand.index);
        DrawnMapState* st = memo.Find(cand.index);
        if (st == nullptr) {
            st = &memo.maps.emplace_back();
            st->index = cand.index;
        }
        // Painted whole this job: a page draw, its rect was dirty (and not narrowed, or repainted
        // after a failed check), or narrowed and checked.
        const bool whole = !job.partial || !ok ||
                           (cand.dirty && std::ranges::none_of(narrowed, [&](const Narrowed& n) {
                                return n.index == cand.index;
                            })) ||
                           std::ranges::any_of(repaint, [&](const auto& rr) {
                               return RectInside(cand.rect, rr);
                           });
        const bool checked_narrow =
            std::ranges::any_of(narrowed, [&](const Narrowed& n) { return n.index == cand.index; });
        if (!ok) {
            st->valid = false;
        } else if (whole || checked_narrow) {
            st->valid = refreshed(r);
            if (st->valid) {
                st->rec = *r;
                st->view_hash = cand.view_hash;
            }
        } else if (refreshed(r)) {
            // Not dirty, but some other rect overlapped it: still whole only if that pass drew it
            // exactly as it was.
            st->valid = st->valid && SameMapView(*r, st->rec) && r->marker_box == st->rec.marker_box;
        }
    }
    if (!job.partial) {
        ++nstats.full_jobs;
    }
    ++nstats.jobs;
    // Not on the GPU-composite path: its canvas is only the HUD overlay over the map quads.
    if (verify_redraw && job.partial && ok && !job.gpu_composite && job.page_copy != nullptr) {
        // The same state drawn whole, from the follow state this job started from.
        static thread_local Canvas full;
        full.Resize(job.target_w, job.target_h);
        full.SetFont(job.font_atlas.get(), job.font_metrics_copy.get());
        full.SetIconFont(job.icon_atlas.get(), job.icon_metrics_copy.get(), job.icon_pending);
        MapFollowState follow = follow_before;
        MapDrawRecords records;
        render_snapshot = &job.snapshot;
        (void)RenderPage(
            full, manifest, *job.page_copy, job.snapshot, images, job.views, &follow,
            [](const std::string&, float, float) {},
            [this](const std::string& area, float wx, float wy) { return IsVisited(area, wx, wy); },
            nullptr, nullptr, [this](const std::string& ref) { return GetMsbtText(ref); }, &records,
            nullptr, nullptr, &map_state_mutex);
        render_snapshot = nullptr;
        ++nstats.verified;
        size_t diff = 0;
        s32 bx0 = std::numeric_limits<s32>::max(), by0 = bx0, bx1 = -1, by1 = -1;
        const auto a = worker_canvas.Pixels();
        const auto b = full.Pixels();
        for (size_t p = 0; p < a.size() && p < b.size(); ++p) {
            if (a[p] != b[p]) {
                ++diff;
                const s32 px = static_cast<s32>(p % job.target_w);
                const s32 py = static_cast<s32>(p / job.target_w);
                bx0 = std::min(bx0, px);
                by0 = std::min(by0, py);
                bx1 = std::max(bx1, px);
                by1 = std::max(by1, py);
            }
        }
        if (diff != 0) {
            ++nstats.mismatched;
            LOG_WARNING(Core,
                        "DSMod verify-redraw: partial job ({} narrowed map(s), {} repainted) "
                        "differs from a full redraw in {} px within [{},{} {}x{}]",
                        narrowed.size(), repaint.size(), diff, bx0, by0, bx1 - bx0 + 1,
                        by1 - by0 + 1);
        }
    }
    if (const auto now = std::chrono::steady_clock::now();
        now - nstats.window >= std::chrono::seconds{5}) {
        if ((RuntimeProfileEnabled() || verify_redraw) &&
            (nstats.narrowed_maps != 0 || nstats.fallback_maps != 0 || nstats.verified != 0)) {
            LOG_INFO(Core,
                     "DSMod redraw worker 5s: {} jobs ({} full), map marker-only {} (checked "
                     "failed {}), whole-map {}; verify {} partial jobs, {} mismatched",
                     nstats.jobs, nstats.full_jobs, nstats.narrowed_maps, nstats.failed_maps,
                     nstats.fallback_maps, nstats.verified, nstats.mismatched);
        }
        nstats = NarrowStats{};
        nstats.window = now;
    }
    redraw_completed_generation.store(job.generation, std::memory_order_release);
    if (!ok) {
        // Matches RenderPageTo's own precedent (mod_runtime.h's map_draw_records_published
        // comment): a failed render leaves whatever was already published alone, rather than
        // publishing nothing/garbage. RenderPage only returns false for a 0x0 canvas, already
        // excluded by PublishUi's own target_w/h == 0 early-return before a job can ever be
        // dispatched -- this path is not expected to be reachable in practice, handled anyway
        // rather than assumed away.
        return;
    }
    // Has the tick thread taken authority
    // over the screen since this job was dispatched (a page transition started, or a synchronous
    // GPU_COMPOSITE/debug-page/sync_redraw carve-out published directly)? Both bump
    // redraw_authority_generation when they do (a newer dispatch alone does not: runtime 13, see
    // its declaration comment). A mismatch here means this job's render -- and its
    // map_draw_records -- are for a page/state the tick thread has already moved past; publishing
    // either would silently overwrite something newer (map_draw_records: wrong-page tap
    // hit-testing; the aux publish: stale pixels replacing a transition's or the synchronous
    // carve-out's already-correct frame). Relaxed load: this is a best-effort staleness check, not
    // a source of any other ordering guarantee -- consistent with the relaxed fetch_add at every
    // bump site and with DispatchRedraw's own pre-existing tolerance for skipping an intermediate
    // frame.
    const bool stale =
        job.authority != redraw_authority_generation.load(std::memory_order_relaxed);
    if (!stale) {
        std::scoped_lock rlk{map_records_mutex};
        // Copies OUT of the persistent worker_map_draw_records accumulator (see its own
        // declaration comment) -- map_draw_records_published still gets a full, independent copy
        // (readers never see the worker's own live buffer), this just changes what `RenderPage` was
        // handed to accumulate INTO in the first place.
        map_draw_records_published = worker_map_draw_records;
        map_records_page = job.page_index;
    }
    // Must run right after THIS draw call finishes, not on a fixed tick-thread schedule -- folded
    // into the worker's own completion.
    // Runs regardless of staleness: it only settles shared camera/view_mutex-guarded state towards
    // this render's own glide step, which is harmless (and still correct) to apply even if this
    // job's PIXELS are no longer going to be shown.
    ApplyViewCorrections();
    if (animating) {
        dispatch_animating.store(true, std::memory_order_relaxed);
    }
    if (stale) {
        // Do not publish superseded pixels now -- but they ARE in worker_canvas, and the job that
        // superseded this one only repaints (and publishes) its own dirty region: remember ours
        // so the next published job publishes it as well (UnpublishedRegions).
        worker_unpublished.Note(job.extras, job.partial);
        return; // see the staleness check above -- do not publish superseded pixels
    }
    // The actual publish, off the tick thread -- safe because AuxRouting's publish boundary is
    // already fully mutex-guarded (verified directly against
    // video_core/dsmod/aux_routing.h's PublishUi/PublishUiPartial/ClearComposite/PublishAuxTexture/
    // PublishMapFadeTextures/PublishComposite bodies, not assumed), so any thread may call it.
    // GPU_COMPOSITE is reached here too -- mirrors PublishUi's own synchronous-branch
    // choice exactly (`dl != nullptr && dl->active` -> PublishGpuComposite; otherwise the plain
    // canvas publish, since a composite-eligible page without an active map quad this tick still
    // needs the ordinary path to retire any stale composite and show its HUD-only canvas).
    auto& aux = system.GPU().DSModAux();
    if (job.gpu_composite && job.draw_list.active) {
        worker_unpublished.Clear(); // the composite publishes the whole HUD canvas
        PublishGpuComposite(job.draw_list, worker_canvas);
        return;
    }
    aux.ClearComposite();
    bool partial = job.partial;
    if (partial && !worker_unpublished.Empty()) {
        partial = worker_unpublished.Apply(job.extras, static_cast<s32>(worker_canvas.Width()),
                                           static_cast<s32>(worker_canvas.Height()));
    }
    if (partial) {
        PublishPartial(aux, worker_canvas, job.extras);
    } else {
        worker_unpublished.Clear();
        aux.PublishUi(worker_canvas.Width(), worker_canvas.Height(), worker_canvas.Pixels());
    }
}

void UnpublishedRegions::Note(const RenderExtras& extras, bool partial) {
    if (!partial || extras.clip[2] <= 0 || extras.clip[3] <= 0) {
        // A full redraw (or a partial job without a clip, i.e. the whole canvas).
        full = true;
        rects.clear();
        return;
    }
    if (full) {
        return;
    }
    if (extras.clips.empty()) {
        rects.push_back(extras.clip);
    } else {
        rects.insert(rects.end(), extras.clips.begin(), extras.clips.end());
    }
    if (rects.size() > MaxRects) {
        // Bounded: many stale jobs in a row without a publish in between is a full repaint anyway.
        full = true;
        rects.clear();
    }
}

bool UnpublishedRegions::Apply(RenderExtras& extras, s32 canvas_w, s32 canvas_h) {
    if (full) {
        Clear();
        return false;
    }
    if (rects.empty()) {
        return true;
    }
    if (extras.clips.empty()) {
        extras.clips.push_back(extras.clip);
    }
    extras.clips.insert(extras.clips.end(), rects.begin(), rects.end());
    MergeDirtyRects(extras.clips, canvas_w, canvas_h);
    s32 x0 = std::numeric_limits<s32>::max(), y0 = x0;
    s32 x1 = std::numeric_limits<s32>::min(), y1 = x1;
    for (const auto& r : extras.clips) {
        if (r[2] <= 0 || r[3] <= 0) {
            continue;
        }
        x0 = std::min(x0, r[0]);
        y0 = std::min(y0, r[1]);
        x1 = std::max(x1, r[0] + r[2]);
        y1 = std::max(y1, r[1] + r[3]);
    }
    if (x1 > x0 && y1 > y0) {
        extras.clip = {x0, y0, x1 - x0, y1 - y0};
    }
    Clear();
    return true;
}

namespace {
/// A source widget's effective rect for dirty-rect purposes, mirroring the "an unsized rect/image
/// spans the canvas" special case RenderPage's own per-widget loop applies at draw time
/// (mod_ui.cpp) -- without this, a currently-hidden full-canvas overlay (rect={0,0,0,0})
/// would union in nothing when it appears or disappears, silently missing the redraw.
///
/// Also resolves a live x_bind/y_bind offset, matching what ExpandWidgetsInto does
/// (mod_ui_expand.cpp, via the shared ResolveBindOffset()) -- `w.rect[0]/[1]` for an x_bind/y_bind
/// widget is only ever a BASE constant the package author wrote (often an anchor like -size/2,
/// nowhere near the canvas), not where the widget is actually drawn; ExpandWidgets adds a live
/// `bind * scale` term to it at every RenderPage call, on a COPY, so `page.widgets[i].rect` (what
/// a verbatim return of it) never changes as the widget moves. Returning it verbatim would make
/// this function's caller (BuildRenderExtras) union in the same static, usually off-screen rect
/// every tick -- the widget's hash correctly goes dirty every tick the bind value changes
/// (WidgetDependencyHash already tracks x_bind/y_bind), but the accompanying rect would never
/// cover the widget's true drawn position, so the redraw would never repaint it (seen live as a
/// dungeon-map Link marker that never visibly moved while walking; the overworld's own marker is
/// immune -- it draws via the Map widget's marker_x_bind/marker_y_bind, INSIDE that widget's own
/// large static rect, which already contains every position the marker can take). The caller
/// still unions in BOTH the previous publish's resolved rect and the new one (widget_last_rect[i]
/// in BuildRenderExtras), so returning the CORRECT rect each time is all that logic needs.
std::array<s32, 4> WidgetEffectiveRect(const Widget& w, u32 canvas_w, u32 canvas_h,
                                       const StateSnapshot& snapshot, const Manifest& manifest,
                                       const FontMetrics* font, bool expanded) {
    if (w.rect[2] == 0 && w.rect[3] == 0 &&
        (w.type == WidgetType::Rect || w.type == WidgetType::Image)) {
        const s32 cw = static_cast<s32>(canvas_w), ch = static_cast<s32>(canvas_h);
        if (w.type == WidgetType::Image) {
            const s32 side = std::max(64, std::min(cw, ch) / 3);
            const s32 pad = WidgetDrawOverhang(w, side, side);
            return {(cw - side) / 2 - pad, (ch - side) / 2 - pad, side + 2 * pad, side + 2 * pad};
        }
        return {0, 0, cw, ch};
    }
    // Only resolve the live position for a widget that is actually going to be PAINTED this tick
    // (mirrors WidgetHidden(), mod_ui_widget_state.cpp -- the exact same gate RenderPage's own
    // per-widget loop checks before dispatching a draw). A hidden x_bind/y_bind widget's bind value
    // can be completely meaningless while hidden -- e.g. a dungeon-floor marker's x_bind/y_bind is
    // a linear function of Link's WORLD position through THAT dungeon's own coordinate formula,
    // continuously evaluated regardless of whether Link is anywhere near that dungeon, so all 12
    // OTHER (hidden) dungeon markers on LA's map page sweep through wide, scattered, frequently
    // off-canvas coordinates every single tick while simply walking the overworld. Resolving all of
    // them live unions in a huge, scattered rect on every walking tick -- a real, measured
    // regression (overworld walk at ~130 page-draw/5s, avg ~11ms, matching the
    // PRE-dirty-region-redraw shape, from the 60% area-cutover firing almost every tick). Since
    // nothing is drawn for a hidden widget regardless of its rect, falling back to the static
    // declared `w.rect` for the hidden case is harmless (a small, usually off-canvas box that never
    // dominates the union) -- and the moment the ACTUAL matching widget becomes visible
    // (WidgetHidden flips false), this function starts resolving ITS live position correctly, and
    // the existing old-rect-union-new-rect logic (BuildRenderExtras, unchanged) still invalidates
    // whatever the widget's last visible rect was, so no stale pixels are left behind either way.
    std::array<s32, 4> r = w.rect;
    // `expanded`: an ExpandWidgets element, whose rect already carries its x_bind/y_bind offset.
    if (!expanded && !(w.x_bind.empty() && w.y_bind.empty()) && !WidgetHidden(w, snapshot)) {
        r = {ResolveBindOffset(w.rect[0], w.x_bind, w.x_scale, snapshot),
             ResolveBindOffset(w.rect[1], w.y_bind, w.y_scale, snapshot), w.rect[2], w.rect[3]};
    }
    // Unsized text (Label/Value with w == 0) is anchored at rect x: left-aligned text grows right,
    // centred text around x, right-aligned text grows LEFT. A zero-width rect never entered the
    // dirty union, so a changed number was repainted only where some other widget's rect happened
    // to overlap it -- e.g. a right-aligned stat value beside its bar lost its leading digits.
    // Give such widgets a conservative box on the side the text actually grows.
    if (r[2] == 0 && (w.type == WidgetType::Label || w.type == WidgetType::Value)) {
        const s32 scale = std::max<s32>(1, w.text_scale);
        const s32 span = scale * 5 * 16; // ~16 glyphs at the font's cap height
        const s32 h = r[3] > 0 ? r[3] : scale * 9;
        const s32 x = w.align == 2 ? r[0] - span : w.align == 1 ? r[0] - span / 2 : r[0];
        r = {x, r[1] - scale, span, h + 2 * scale};
    }
    // A spinning or trembling picture paints outside its own rect (the rotated corners, the shake
    // offset): its dirty box must cover that too, or its edge pixels go stale.
    if (const s32 pad = WidgetDrawOverhang(w, r[2], r[3]); pad > 0) {
        r = {r[0] - pad, r[1] - pad, r[2] + 2 * pad, r[3] + 2 * pad};
    }
    // Text can run past the fixed box above (a longer string than ~16 glyphs, a font taller than
    // its cap height): widen to the text's real bound for this snapshot.
    if (w.type == WidgetType::Label || w.type == WidgetType::Value ||
        w.type == WidgetType::Button || w.type == WidgetType::Pips) {
        const bool moved =
            !expanded && !(w.x_bind.empty() && w.y_bind.empty()) && !WidgetHidden(w, snapshot);
        const s32 tx =
            moved ? ResolveBindOffset(w.rect[0], w.x_bind, w.x_scale, snapshot) : w.rect[0];
        const s32 ty =
            moved ? ResolveBindOffset(w.rect[1], w.y_bind, w.y_scale, snapshot) : w.rect[1];
        const auto t = WidgetPaintBounds(w, tx, ty, w.rect[2], w.rect[3], snapshot, manifest, font,
                                         static_cast<s32>(canvas_w), static_cast<s32>(canvas_h));
        if (t[2] > 0 && t[3] > 0) {
            if (r[2] <= 0 || r[3] <= 0) {
                r = t;
            } else {
                const s32 x0 = std::min(r[0], t[0]), y0 = std::min(r[1], t[1]);
                const s32 x1 = std::max(r[0] + r[2], t[0] + t[2]);
                const s32 y1 = std::max(r[1] + r[3], t[1] + t[3]);
                r = {x0, y0, x1 - x0, y1 - y0};
            }
        }
    }
    return r;
}

/// The union rect of every POTENTIAL instance of a repeat/"{i}" template, over the full declared
/// `repeat` count (not the live repeat_bind-resolved count, so this stays valid while that count is
/// changing) -- same offset math ExpandWidgets itself uses (ExpandTemplateInto, mod_ui_expand.cpp).
/// Repeat templates are not narrowed by WidgetDependencyHash (hashing survives at the definition,
/// but a template's own bind/hide_bind strings still contain literal "{i}", which never matches a
/// published point name) -- the caller always treats a repeat widget as dirty whenever the page's
/// signature changed, and unions this rect in. Among the shipped packages only StoryOfSeasonsFoMT
/// (1 widget) and BreathOfTheWild (6) use `repeat` at all, so this conservative "always dirty"
/// choice costs little in practice.
/// Turns the dirty scan's rects into at most four disjoint ones (clipped to the canvas): rects
/// that overlap or nearly touch become their bounding box, then the pair whose merge adds the least
/// area is merged until four remain. Each can then be redrawn as its own clipped pass without any
/// pixel being painted twice.
void MergeDirtyRects(std::vector<std::array<s32, 4>>& rects, s32 canvas_w, s32 canvas_h) {
    constexpr size_t MaxRects = 4;
    constexpr s32 Gap = 24; // closer than this: one pass is cheaper than two
    using Rect = std::array<s32, 4>;
    std::erase_if(rects, [&](Rect& r) {
        const s32 rx0 = std::max(0, r[0]), ry0 = std::max(0, r[1]);
        const s32 rx1 = std::min(canvas_w, r[0] + r[2]), ry1 = std::min(canvas_h, r[1] + r[3]);
        r = {rx0, ry0, rx1 - rx0, ry1 - ry0};
        return r[2] <= 0 || r[3] <= 0;
    });
    const auto bbox = [](const Rect& a, const Rect& b) {
        const s32 bx0 = std::min(a[0], b[0]), by0 = std::min(a[1], b[1]);
        const s32 bx1 = std::max(a[0] + a[2], b[0] + b[2]);
        const s32 by1 = std::max(a[1] + a[3], b[1] + b[3]);
        return Rect{bx0, by0, bx1 - bx0, by1 - by0};
    };
    const auto area = [](const Rect& r) { return static_cast<s64>(r[2]) * r[3]; };
    const auto near = [](const Rect& a, const Rect& b) {
        return a[0] < b[0] + b[2] + Gap && b[0] < a[0] + a[2] + Gap && a[1] < b[1] + b[3] + Gap &&
               b[1] < a[1] + a[3] + Gap;
    };
    for (;;) {
        bool merged = false;
        for (size_t i = 0; i < rects.size() && !merged; ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                if (near(rects[i], rects[j])) {
                    rects[i] = bbox(rects[i], rects[j]);
                    rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
        if (merged) {
            continue;
        }
        if (rects.size() <= MaxRects) {
            break;
        }
        size_t bi = 0, bj = 1;
        s64 best = std::numeric_limits<s64>::max();
        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                const s64 grow = area(bbox(rects[i], rects[j])) - area(rects[i]) - area(rects[j]);
                if (grow < best) {
                    best = grow;
                    bi = i;
                    bj = j;
                }
            }
        }
        rects[bi] = bbox(rects[bi], rects[bj]);
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(bj));
    }
}

std::array<s32, 4> RepeatTemplateUnionRect(const Widget& w, const Page& page) {
    if (w.repeat <= 0) {
        return w.rect;
    }
    // A scrolled list only ever paints inside its region's viewport (rows and bar are clipped to
    // it), whatever its full content height -- a 160-row list must not dirty 10000 px of canvas.
    if (!w.scroll.empty()) {
        if (const ScrollRegion* const region = FindScrollRegion(page, w.scroll)) {
            return region->rect;
        }
    }
    s32 x0 = w.rect[0], y0 = w.rect[1];
    s32 x1 = w.rect[0] + w.rect[2], y1 = w.rect[1] + w.rect[3];
    for (s64 i = 0; i < w.repeat; ++i) {
        s32 ox, oy;
        if (w.repeat_cols > 0) {
            const s64 col = i % w.repeat_cols, row = i / w.repeat_cols;
            ox = static_cast<s32>(col) * w.repeat_dx;
            oy = static_cast<s32>(row) * w.repeat_row_dy;
        } else {
            ox = static_cast<s32>(i) * w.repeat_dx;
            oy = static_cast<s32>(i) * w.repeat_dy;
        }
        x0 = std::min(x0, w.rect[0] + ox);
        y0 = std::min(y0, w.rect[1] + oy);
        x1 = std::max(x1, w.rect[0] + ox + w.rect[2]);
        y1 = std::max(y1, w.rect[1] + oy + w.rect[3]);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

/// Whether the redraw worker may repaint a Map widget marker-only (RenderExtras::maps): a plain
/// Map with a live marker whose rect never moves (no x_bind/y_bind, no widget group) and whose
/// expanded index is its own (no repeat template before it on the page, so the index RenderPage
/// keys its record and follow state by is `index`).
bool MarkerOnlyCandidate(const Widget& w) {
    return w.type == WidgetType::Map && w.repeat <= 0 && w.x_bind.empty() && w.y_bind.empty() &&
           !w.anim && !w.marker_x_bind.empty() && !w.marker_y_bind.empty() && w.rect[2] > 0 &&
           w.rect[3] > 0;
}

s64 RectsArea(const std::vector<std::array<s32, 4>>& rects) {
    s64 a = 0;
    for (const auto& r : rects) {
        a += static_cast<s64>(std::max(0, r[2])) * std::max(0, r[3]);
    }
    return a;
}

std::array<s32, 4> RectsBox(const std::vector<std::array<s32, 4>>& rects) {
    s32 x0 = std::numeric_limits<s32>::max(), y0 = x0;
    s32 x1 = std::numeric_limits<s32>::min(), y1 = x1;
    for (const auto& r : rects) {
        if (r[2] <= 0 || r[3] <= 0) {
            continue;
        }
        x0 = std::min(x0, r[0]);
        y0 = std::min(y0, r[1]);
        x1 = std::max(x1, r[0] + r[2]);
        y1 = std::max(y1, r[1] + r[3]);
    }
    return x1 > x0 && y1 > y0 ? std::array<s32, 4>{x0, y0, x1 - x0, y1 - y0}
                              : std::array<s32, 4>{};
}
} // namespace

std::array<s32, 4> ModRuntime::BuildRenderExtras(const StateSnapshot& snapshot,
                                                 bool widgets_may_be_dirty, u32 target_w,
                                                 u32 target_h) {
    render_extras.groups.clear();
    render_extras.clips.clear();
    render_extras.other_clips.clear();
    for (auto& m : render_extras.maps) {
        m.dirty = false;
    }
    // Rects of the marker-only candidate Map widgets (RenderExtras::maps) that are dirty: kept
    // apart from `clips` until the end, so `other_clips` can be everything else.
    std::vector<std::array<s32, 4>> map_rects;
    const auto candidate = [this](size_t i) -> RenderMapCandidate* {
        for (auto& m : render_extras.maps) {
            if (m.index == i) {
                return &m;
            }
        }
        return nullptr;
    };
    bool rebased = false;
    s32 x0 = std::numeric_limits<s32>::max(), y0 = x0;
    s32 x1 = std::numeric_limits<s32>::min(), y1 = x1;
    // Every contribution is kept as its own rect too (render_extras.clips, merged below into a
    // few disjoint ones): two small changes far apart no longer redraw everything between them.
    const auto add_rect = [&](s32 rx, s32 ry, s32 rw, s32 rh) {
        x0 = std::min(x0, rx);
        y0 = std::min(y0, ry);
        x1 = std::max(x1, rx + rw);
        y1 = std::max(y1, ry + rh);
        render_extras.clips.push_back({rx, ry, rw, rh});
    };
    // --- The union of every currently-moving widget-group's box (the group-animation partial
    // redraw). ---
    for (const auto& def : group_defs) {
        const auto g = group_anims.find(def.anim->key);
        if (g == group_anims.end()) {
            continue;
        }
        RenderAnimGroup rg;
        rg.key = def.anim->key;
        rg.box = def.box;
        rg.moving = g->second.moving;
        rg.shown = g->second.open || g->second.pos > 0.0f;
        if (rg.moving) {
            const float e = ApplyEasing(def.anim->easing, g->second.pos);
            const float hidden_part = 1.0f - e;
            switch (def.anim->from) {
            case WidgetAnim::From::Right:
                rg.dx = static_cast<s32>(std::lround(static_cast<float>(def.box[2]) * hidden_part));
                break;
            case WidgetAnim::From::Left:
                rg.dx =
                    -static_cast<s32>(std::lround(static_cast<float>(def.box[2]) * hidden_part));
                break;
            case WidgetAnim::From::Top:
                rg.dy =
                    -static_cast<s32>(std::lround(static_cast<float>(def.box[3]) * hidden_part));
                break;
            case WidgetAnim::From::Bottom:
                rg.dy = static_cast<s32>(std::lround(static_cast<float>(def.box[3]) * hidden_part));
                break;
            case WidgetAnim::From::Fade:
                rg.alpha = e;
                break;
            case WidgetAnim::From::Widget: {
                // Grows out of / shrinks into origin_box: the widgets keep their real (final)
                // rect and only the clip box lerps from the origin to the group's own box, so the
                // group appears to expand from the origin widget rather than a straight edge --
                // at zero extra per-frame cost over the box-redraw every other style already pays
                // (no pixel scaling: it is the same "redraw the box, clipped" draw as the other
                // styles, just clipped to a smaller, moving rect while e < 1).
                const auto& o = def.origin_box;
                const auto lerp = [e](s32 a, s32 b) {
                    return static_cast<s32>(
                        std::lround(static_cast<float>(a) + static_cast<float>(b - a) * e));
                };
                rg.box = {lerp(o[0], def.box[0]), lerp(o[1], def.box[1]), lerp(o[2], def.box[2]),
                          lerp(o[3], def.box[3])};
                break;
            }
            }
        }
        if ((rg.moving || g->second.settle_pending) && def.box[2] > 0 && def.box[3] > 0) {
            add_rect(def.box[0], def.box[1], def.box[2], def.box[3]);
        }
        render_extras.groups.push_back(std::move(rg));
    }
    // --- Ordinary per-widget dirty tracking, unioned into the SAME accumulator above -- one merged
    // rect, one RenderPage call, not two passes. Kill switch: EDEN_DSMOD_FORCE_FULL_REDRAW=1 skips
    // this whole block, degrading to exactly the group-box-only behaviour above -- a field problem
    // in this new path can be diagnosed by setting the env var, no rebuild required. ---
    static const bool force_full_redraw = [] {
        const char* v = std::getenv("EDEN_DSMOD_FORCE_FULL_REDRAW");
        return v != nullptr && std::strcmp(v, "0") != 0 && std::strcmp(v, "false") != 0 &&
               std::strcmp(v, "FALSE") != 0;
    }();
    if (force_full_redraw) {
        render_extras.maps.clear();
    }
    if (!force_full_redraw && current_page < manifest.pages.size() && target_w > 0 &&
        target_h > 0) {
        const auto& page = manifest.pages[current_page];
        const auto& widgets = page.widgets;
        // The font the redraw will measure text with (PublishUi sets it from these same metrics).
        const FontMetrics* const text_font = font_metrics.Valid() ? &font_metrics : nullptr;
        scan_scroll_memo.Reset(page.scrolls.size()); // this snapshot's list metrics, once each
        // A template's box: every slot it can occupy, plus where this tick's elements actually
        // paint (x_bind/y_bind moves, long text) -- the elements RepeatTemplateDependencyHash just
        // expanded for this template.
        const auto RepeatTemplateRect = [&](const Widget& w, const Page& pg) {
            auto r = RepeatTemplateUnionRect(w, pg);
            const auto e = RepeatElementsRect(snapshot, target_w, target_h);
            if (e[2] > 0 && e[3] > 0) {
                if (r[2] <= 0 || r[3] <= 0) {
                    r = e;
                } else {
                    const s32 x0 = std::min(r[0], e[0]), y0 = std::min(r[1], e[1]);
                    const s32 x1 = std::max(r[0] + r[2], e[0] + e[2]);
                    const s32 y1 = std::max(r[1] + r[3], e[1] + e[3]);
                    r = {x0, y0, x1 - x0, y1 - y0};
                }
            }
            return r;
        };
        const bool page_changed =
            widget_sig_page != current_page || widget_sig.size() != widgets.size();
        // mod_nx_runtime.cpp bumps asset_epoch whenever ANY asset lands (image/composite/font/msbt)
        // -- that never corresponds to a bound value, so no per-widget hash would notice it on its
        // own (same class of gap as the first-open snapshot race fixed for page transitions, here
        // at ordinary-redraw granularity instead of a transition's). Only matters on a tick that is
        // already redrawing (widgets_may_be_dirty) -- see the comment at the call site in
        // PublishUi.
        const bool assets_landed = widgets_may_be_dirty && widget_sig_asset_epoch != asset_epoch;
        if (page_changed || assets_landed) {
            static thread_local RuntimeStageStats rebaseline_stats;
            const RuntimeStageTimer rebaseline_timer{rebaseline_stats, "widget-dirty-scan"};
            widget_sig.assign(widgets.size(), 0);
            widget_last_rect.assign(widgets.size(), std::array<s32, 4>{});
            repeat_hash_elements.clear();
            repeat_hash_indices.clear();
            repeat_hash_elements.resize(widgets.size());
            repeat_hash_indices.resize(widgets.size());
            repeat_state.clear();
            repeat_state.resize(widgets.size());
            for (size_t i = 0; i < widgets.size(); ++i) {
                widget_sig[i] = widgets[i].repeat > 0
                                    ? RepeatTemplateDependencyHash(page, i, snapshot)
                                    : WidgetDependencyHash(widgets[i], snapshot, page.id, i);
                widget_last_rect[i] =
                    widgets[i].repeat > 0
                        ? RepeatTemplateRect(widgets[i], page)
                        : WidgetEffectiveRect(widgets[i], target_w, target_h, snapshot, manifest,
                                              text_font, false);
                if (widgets[i].repeat > 0) {
                    RebuildRepeatState(i, snapshot, target_w, target_h);
                }
            }
            rebased = true;
            render_extras.maps.clear();
            for (size_t i = 0; i < widgets.size() && widgets[i].repeat <= 0; ++i) {
                if (MarkerOnlyCandidate(widgets[i])) {
                    render_extras.maps.push_back({i, widgets[i].rect, 0, false});
                }
            }
            widget_sig_page = current_page;
            widget_sig_asset_epoch = asset_epoch;
            if (assets_landed && !page_changed) {
                // A page switch already gets a full redraw through the ordinary sig-changed path (a
                // new page's UiSignature, above, differs in its page-index term). Only the
                // "something landed mid-page" case needs an explicit full-canvas rect here, so
                // whatever just landed draws now instead of whenever something else redraws it.
                add_rect(0, 0, static_cast<s32>(target_w), static_cast<s32>(target_h));
            }
        } else if (widgets_may_be_dirty) {
            static thread_local RuntimeStageStats scan_stats;
            const RuntimeStageTimer scan_timer{scan_stats, "widget-dirty-scan"};
            for (size_t i = 0; i < widgets.size(); ++i) {
                const Widget& w = widgets[i];
                const bool repeat_template = w.repeat > 0;
                // A repeat template is hashed over its expanded elements (their substituted binds,
                // positions and the list's scroll state), so it is dirty only when one of them
                // changed -- it used to be dirty on every scan, which put every list on the page
                // into the dirty union whenever anything at all (a spinning icon) changed.
                const u64 h = repeat_template ? RepeatTemplateDependencyHash(page, i, snapshot)
                                              : WidgetDependencyHash(w, snapshot, page.id, i);
                if (h == widget_sig[i]) {
                    continue;
                }
                if (repeat_template && i < repeat_state.size()) {
                    // The same elements as last time, list-level inputs unchanged: repaint only
                    // the rows whose own hash moved (their old and new boxes).
                    auto& st = repeat_state[i];
                    if (st.valid && st.level == repeat_hash_level &&
                        st.hash.size() == repeat_hash_elem.size() &&
                        repeat_hash_last_idx.size() == st.idx.size() &&
                        std::equal(st.idx.begin(), st.idx.end(), repeat_hash_last_idx.begin())) {
                        for (size_t k = 0; k < st.hash.size(); ++k) {
                            if (st.hash[k] == repeat_hash_elem[k]) {
                                continue;
                            }
                            const auto nr =
                                WidgetEffectiveRect(repeat_hash_last[k], target_w, target_h,
                                                    snapshot, manifest, text_font, true);
                            const auto& orr = st.rect[k];
                            if (orr[2] > 0 && orr[3] > 0) {
                                add_rect(orr[0], orr[1], orr[2], orr[3]);
                            }
                            if (nr[2] > 0 && nr[3] > 0) {
                                add_rect(nr[0], nr[1], nr[2], nr[3]);
                                // keep the template's whole-list box covering it (a later
                                // list-level change repaints from that box)
                                auto& full = widget_last_rect[i];
                                const s32 fx0 = std::min(full[0], nr[0]),
                                          fy0 = std::min(full[1], nr[1]);
                                const s32 fx1 = std::max(full[0] + full[2], nr[0] + nr[2]);
                                const s32 fy1 = std::max(full[1] + full[3], nr[1] + nr[3]);
                                full = {fx0, fy0, fx1 - fx0, fy1 - fy0};
                            }
                            st.rect[k] = nr;
                            st.hash[k] = repeat_hash_elem[k];
                        }
                        widget_sig[i] = h;
                        continue;
                    }
                }
                const auto rect = repeat_template
                                      ? RepeatTemplateRect(w, page)
                                      : WidgetEffectiveRect(w, target_w, target_h, snapshot,
                                                            manifest, text_font, false);
                if (RenderMapCandidate* const m = candidate(i);
                    m != nullptr && rect == widget_last_rect[i] && rect == m->rect) {
                    // Its rect is fixed: kept apart (see map_rects), in the union all the same.
                    m->dirty = true;
                    widget_sig[i] = h;
                    x0 = std::min(x0, rect[0]);
                    y0 = std::min(y0, rect[1]);
                    x1 = std::max(x1, rect[0] + rect[2]);
                    y1 = std::max(y1, rect[1] + rect[3]);
                    map_rects.push_back(rect);
                    continue;
                }
                const auto& prev = widget_last_rect[i];
                if (prev[2] > 0 && prev[3] > 0) {
                    add_rect(prev[0], prev[1], prev[2], prev[3]);
                }
                if (rect[2] > 0 && rect[3] > 0) {
                    add_rect(rect[0], rect[1], rect[2], rect[3]);
                }
                widget_sig[i] = h;
                widget_last_rect[i] = rect;
                if (repeat_template) {
                    RebuildRepeatState(i, snapshot, target_w, target_h);
                }
            }
        }
    }
    if (!force_full_redraw && current_page < manifest.pages.size() && target_w > 0 &&
        target_h > 0 && widget_sig_page == current_page) {
        const auto& widgets = manifest.pages[current_page].widgets;
        // A follow-view glide still in flight (RenderExtras::glide_owed): every visible Map widget
        // is redrawn, as nothing bound changed for it.
        if (render_extras.glide_owed) {
            const FontMetrics* const text_font = font_metrics.Valid() ? &font_metrics : nullptr;
            for (size_t i = 0; i < widgets.size(); ++i) {
                const Widget& w = widgets[i];
                if (w.type != WidgetType::Map || WidgetHidden(w, snapshot)) {
                    continue;
                }
                if (RenderMapCandidate* const m = candidate(i); m != nullptr) {
                    if (!m->dirty) {
                        m->dirty = true;
                        x0 = std::min(x0, m->rect[0]);
                        y0 = std::min(y0, m->rect[1]);
                        x1 = std::max(x1, m->rect[0] + m->rect[2]);
                        y1 = std::max(y1, m->rect[1] + m->rect[3]);
                        map_rects.push_back(m->rect);
                    }
                    continue;
                }
                const auto r =
                    WidgetEffectiveRect(w, target_w, target_h, snapshot, manifest, text_font, false);
                if (r[2] > 0 && r[3] > 0) {
                    add_rect(r[0], r[1], r[2], r[3]);
                }
            }
        }
        // Each candidate's view hash, as of this snapshot (unchanged while nothing may be dirty).
        if (widgets_may_be_dirty || rebased) {
            const auto& page = manifest.pages[current_page];
            for (auto& m : render_extras.maps) {
                if (m.index < widgets.size()) {
                    hash_without_marker = true;
                    m.view_hash = WidgetDependencyHash(widgets[m.index], snapshot, page.id, m.index);
                    hash_without_marker = false;
                }
            }
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        render_extras.clips.clear();
        return {0, 0, 0, 0};
    }
    render_extras.other_clips = render_extras.clips;
    MergeDirtyRects(render_extras.other_clips, static_cast<s32>(target_w),
                    static_cast<s32>(target_h));
    render_extras.clips.insert(render_extras.clips.end(), map_rects.begin(), map_rects.end());
    MergeDirtyRects(render_extras.clips, static_cast<s32>(target_w), static_cast<s32>(target_h));
    return {x0, y0, x1 - x0, y1 - y0};
}

void ModRuntime::PublishGpuComposite(AuxDrawList& dl, Canvas& hud_canvas) {
    static thread_local RuntimeStageStats stats;
    const RuntimeStageTimer timer{stats, "map-composite"};
    // Everything from here down touches `last_map_key`/`last_map_stamp`/
    // `last_pulse_key`/`last_pulse_present`/`map_fade_weights`/`last_map_fade_epoch`/
    // `atlas_published`/`last_hud_hash`/`last_composite_weight`/`last_map_composite_epoch` --
    // plain ModRuntime members this function is the ONLY normal-path writer of, but not
    // implicitly single-threaded since the worker can call this too (see `gpu_composite_mutex`'s
    // own comment: the one real cross-thread writer is the console "reload" reset, not concurrent
    // calls to this function itself). One lock for the whole body -- it is already gated to at most
    // once per tick per caller (`dl->active`/`will_dispatch`), so contention is a non-issue; the
    // point is exclusion against that reset, not throughput.
    std::scoped_lock composite_lock{gpu_composite_mutex};
    auto& aux = system.GPU().DSModAux();
    bool map_content_changed = false;
    std::string map_area;
    MapStamp map_stamp{};
    // GPU map fading uses two full-resolution endpoint images plus a 650x300 per-cell blend
    // texture. Endpoint images change only with map content/visibility; advancing the one-second
    // animation uploads just the compact weight grid.
    if (dl.map_key.starts_with("composite:")) {
        // A composite picture (image-mode area): the endpoints are the picture and, while a flat
        // composite cross-fades, its previous picture; the weight is one texel for the whole map.
        const std::shared_ptr<const Image> current = GetImage(dl.map_key);
        std::shared_ptr<const Image> prev = GetImage(dl.map_key + "#prev");
        if (current != nullptr && prev != nullptr &&
            (!prev->Valid() || prev->w != current->w || prev->h != current->h)) {
            prev = nullptr;
        }
        const float fade = CompositeFade(dl.map_key.substr(10));
        const u32 weight =
            prev == nullptr ? 255u
                            : static_cast<u32>(std::lround(std::clamp(fade, 0.0f, 1.0f) * 255.0f));
        if (current != nullptr && current->Valid() &&
            (dl.map_key != last_map_key || composite_epoch != last_map_composite_epoch)) {
            map_fade_weights.assign(1, 0xFF000000u | (weight << 16));
            aux.PublishMapFadeTextures(current->w, current->h, current->pixels,
                                       prev != nullptr ? prev->pixels : current->pixels, 1, 1,
                                       map_fade_weights);
            last_map_key = dl.map_key;
            last_map_stamp = {};
            last_map_composite_epoch = composite_epoch;
            last_composite_weight = weight;
        } else if (weight != last_composite_weight && last_map_key == dl.map_key) {
            map_fade_weights.assign(1, 0xFF000000u | (weight << 16));
            aux.PublishAuxTexture(VideoCore::DSMod::AuxRouting::MapFadeSlot, 1, 1,
                                  map_fade_weights);
            last_composite_weight = weight;
        }
    } else if (!dl.map_key.empty()) {
        if (const auto at = dl.map_key.find('@');
            dl.map_key.starts_with("map:") && at != std::string::npos) {
            map_area = dl.map_key.substr(4, at - 4);
        }
        map_stamp = StampFor(map_area);
        const u64 fade_epoch = map_stamp.fade;
        map_stamp.fade = 0;
        map_content_changed = dl.map_key != last_map_key || !(map_stamp == last_map_stamp);

        if (map_content_changed || fade_epoch != last_map_fade_epoch) {
            const u64 fade_ticks = std::max<u64>(1, manifest.map_style.fade_ticks);
            map_fade_weights.resize(static_cast<size_t>(VisitedGrid::Cols) * VisitedGrid::Rows);
            // MarkVisitedAt can run on the other thread (tick thread or redraw worker) -- same
            // cross-thread pair as UiSignature's, guarded.
            std::scoped_lock lk{map_state_mutex};
            const auto grid = map_visited.find(map_area);
            for (size_t i = 0; i < map_fade_weights.size(); ++i) {
                u64 numerator = fade_ticks;
                if (grid != map_visited.end()) {
                    const u32 changed = grid->second.change_tick[i];
                    const u64 age = changed == 0 ? fade_ticks : tick_count - changed;
                    if (age < fade_ticks) {
                        numerator = age + 1;
                    }
                }
                const u32 weight =
                    static_cast<u32>(std::min<u64>(255, numerator * 255 / fade_ticks));
                // Aux textures use BGRA8 and pixels are 0xAARRGGBB, so this stores weight in R.
                map_fade_weights[i] = 0xFF000000u | (weight << 16);
            }
        }

        if (map_content_changed) {
            const std::shared_ptr<const Image> previous =
                GetImage(dl.map_key, MapFadeEndpoint::Previous);
            const std::shared_ptr<const Image> current =
                GetImage(dl.map_key, MapFadeEndpoint::Current);
            if (previous != nullptr && current != nullptr && previous->Valid() &&
                current->Valid() && previous->w == current->w && previous->h == current->h) {
                // A reveal changes a few cells of the map, yet both endpoints used to be copied
                // whole (~26 MB at 3072x1052) into the routing buffer under the lock the render
                // thread takes every frame, and then whole into staging on the render thread.
                // Against the endpoints last published, only the differing 64x64 tiles are sent:
                // the routing buffer and the uploaded textures end up byte-identical either way.
                u64 epoch = 0;
                if (VideoCore::DSMod::UiDiffEnabled() && map_pub_epoch != 0 &&
                    map_pub_current != nullptr && map_pub_previous != nullptr &&
                    map_pub_current->w == current->w && map_pub_current->h == current->h &&
                    map_pub_previous->w == current->w && map_pub_previous->h == current->h &&
                    map_pub_current->pixels.size() == current->pixels.size() &&
                    map_pub_previous->pixels.size() == current->pixels.size()) {
                    VideoCore::DSMod::TileMask current_dirty, previous_dirty;
                    current_dirty.Reset(current->w, current->h);
                    current_dirty.Clear();
                    previous_dirty.Reset(current->w, current->h);
                    previous_dirty.Clear();
                    VideoCore::DSMod::DiffTiles(current->pixels, map_pub_current->pixels,
                                                current->w, current->h, current_dirty);
                    VideoCore::DSMod::DiffTiles(previous->pixels, map_pub_previous->pixels,
                                                current->w, current->h, previous_dirty);
                    epoch = aux.PublishMapFadeTexturesTiles(
                        current->w, current->h, current->pixels, previous->pixels,
                        current_dirty, previous_dirty, VisitedGrid::Cols, VisitedGrid::Rows,
                        map_fade_weights, map_pub_epoch);
                }
                if (epoch == 0) {
                    epoch = aux.PublishMapFadeTextures(current->w, current->h, current->pixels,
                                                       previous->pixels, VisitedGrid::Cols,
                                                       VisitedGrid::Rows, map_fade_weights);
                }
                map_pub_current = current;
                map_pub_previous = previous;
                map_pub_epoch = epoch;
                last_map_key = dl.map_key;
                last_map_stamp = map_stamp;
                last_map_fade_epoch = fade_epoch;
            }
        } else if (fade_epoch != last_map_fade_epoch) {
            aux.PublishAuxTexture(VideoCore::DSMod::AuxRouting::MapFadeSlot, VisitedGrid::Cols,
                                  VisitedGrid::Rows, map_fade_weights);
            last_map_fade_epoch = fade_epoch;
        }
    }
    // Item-room pulse (slot 3): it depends on the current settled endpoint, not animation time.
    if (!dl.pulse_key.empty()) {
        const std::string stamped = dl.pulse_key + StampKey(map_stamp);
        if (stamped != last_pulse_key) {
            const std::shared_ptr<const Image> p = GetImage(dl.pulse_key, MapFadeEndpoint::Current);
            last_pulse_present = p != nullptr && p->Valid();
            if (last_pulse_present) {
                aux.PublishAuxTexture(3, p->w, p->h, p->pixels);
            }
            last_pulse_key = stamped;
        }
        if (!last_pulse_present) {
            std::erase_if(dl.quads, [](const auto& q) { return q.slot == 3; });
        }
    }
    // Icon atlas (slot 1): a constant sheet, upload once.
    if (!atlas_published && !dl.atlas_key.empty()) {
        if (const std::shared_ptr<const Image> a = GetImage(dl.atlas_key);
            a != nullptr && a->Valid()) {
            aux.PublishAuxTexture(1, a->w, a->h, a->pixels);
            atlas_published = true;
        }
    }
    // HUD overlay (slot 2): the canvas holds only the HUD (transparent elsewhere); re-upload only
    // when its pixels change. Hash EVERY pixel: a strided sample with the stride dividing the
    // width only ever looked at the same few columns, so a digit change elsewhere never uploaded.
    // Reads `hud_canvas`, not the tick-thread's own `canvas` -- see this function's own
    // declaration comment (mod_runtime.h) for why the caller picks which Canvas that is.
    {
        // A full-content hash (every byte), ~4x faster than the per-pixel FNV it replaced.
        const std::vector<u32>& px = hud_canvas.Pixels();
        u64 h =
            Common::CityHash64(reinterpret_cast<const char*>(px.data()), px.size() * sizeof(u32));
        h ^= px.size();
        if (h != last_hud_hash) {
            aux.PublishAuxTexture(2, hud_canvas.Width(), hud_canvas.Height(), px);
            last_hud_hash = h;
        }
    }
    // Append the HUD overlay as a final full-canvas quad (slot 2), drawn last so the HUD strip
    // and area label sit on top of the map/icons/marker; transparent everywhere else.
    dl.quads.push_back({2u, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, static_cast<float>(dl.canvas_w),
                        static_cast<float>(dl.canvas_h), 1.0f});
    aux.PublishComposite(dl.canvas_w, dl.canvas_h, dl.bg, dl.quads);
}

} // namespace Core::Mods
