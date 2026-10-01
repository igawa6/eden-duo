// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 14: horizontal swipe (SwipeTracker, the swipe hit test) and its manifest keys; the map
// widget's bound default view (view_rect_*_bind, mod_map_view_rect.h).
#include <cmath>
#include <map>
#include <memory>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_input_swipe.h"
#include "core/mods/mod_map_view_rect.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
/// One finger: lands at (x0, y0) at t=0, moves in `steps` equal frames of `frame_ms` to (x1, y1),
/// then lifts. Returns what the lift fired.
SwipeDir Stroke(SwipeTracker& s, int x0, int y0, int x1, int y1, bool left = true,
                bool right = true, int swipe_px = 0, int steps = 6, int frame_ms = 16) {
    s.Down(0, x0, y0, left, right, swipe_px);
    for (int i = 1; i <= steps; ++i) {
        s.Move(static_cast<std::int64_t>(i) * frame_ms, x0 + (x1 - x0) * i / steps,
               y0 + (y1 - y0) * i / steps, false);
    }
    return s.Up();
}

Widget SwipeCard(std::array<s32, 4> rect) {
    Widget w;
    w.type = WidgetType::Rect;
    w.rect = rect;
    w.on_swipe_left = "next";
    w.on_swipe_right = "prev";
    return w;
}
} // namespace

TEST_CASE("DSMod runtime 14: version", "[dsmod][runtime14]") {
    REQUIRE(DualScreenRuntimeVersion >= 14);
}

TEST_CASE("DSMod swipe: left and right fire once at the lift", "[dsmod][runtime14][swipe]") {
    SwipeTracker s;
    REQUIRE(Stroke(s, 500, 300, 380, 310) == SwipeDir::Left);
    REQUIRE(Stroke(s, 500, 300, 620, 290) == SwipeDir::Right);
    // Nothing fires before the lift; the lift fires once.
    s.Down(0, 100, 100, true, true, 0);
    s.Move(16, 200, 100, false);
    REQUIRE(s.Armed());
    REQUIRE(s.Up() == SwipeDir::Right);
    REQUIRE(s.Up() == SwipeDir::None);
    // Only a direction the widget has an action for.
    REQUIRE(Stroke(s, 500, 300, 380, 300, false, true) == SwipeDir::None);
    REQUIRE(Stroke(s, 500, 300, 620, 300, false, true) == SwipeDir::Right);
    // No swipe widget under the finger: never armed.
    REQUIRE(Stroke(s, 500, 300, 380, 300, false, false) == SwipeDir::None);
    // A custom swipe_px.
    REQUIRE(Stroke(s, 500, 300, 400, 300, true, true, 120) == SwipeDir::None);
    REQUIRE(Stroke(s, 500, 300, 370, 300, true, true, 120) == SwipeDir::Left);
}

TEST_CASE("DSMod swipe: below the threshold is no swipe (a still finger stays a tap)",
          "[dsmod][runtime14][swipe]") {
    SwipeTracker s;
    // Inside the 12 px tap slop: no swipe; UpdateGestures sees an unmoved finger -> a tap.
    REQUIRE(Stroke(s, 500, 300, 508, 300) == SwipeDir::None);
    // Past the slop but short of swipe_px (60): no swipe (and no tap: it moved).
    REQUIRE(Stroke(s, 500, 300, 459, 300) == SwipeDir::None);
    REQUIRE(Stroke(s, 500, 300, 440, 300) == SwipeDir::Left); // exactly swipe_px
    // A swipe_px under the slop still needs the finger to leave the slop, so a fired swipe is
    // never a still finger: the lift that ends it can never also be a tap.
    REQUIRE(Stroke(s, 500, 300, 510, 300, true, true, 5) == SwipeDir::None);
    REQUIRE(Stroke(s, 500, 300, 514, 300, true, true, 5) == SwipeDir::Right);
    REQUIRE(SwipeLockSlop == 12);
    // Reached, then carried back before the lift: the lift point must still qualify.
    s.Down(0, 500, 300, true, true, 0);
    s.Move(16, 420, 300, false);
    s.Move(32, 480, 300, false);
    REQUIRE(s.Up() == SwipeDir::None);
}

TEST_CASE("DSMod swipe: vertical drags stay scrolls, a horizontal start owns the gesture",
          "[dsmod][runtime14][swipe]") {
    SwipeTracker s;
    // Straight down (a list scroll): never a swipe, never owns the gesture.
    s.Down(0, 500, 300, true, true, 0);
    s.Move(16, 500, 340, false);
    REQUIRE_FALSE(s.OwnsGesture());
    REQUIRE_FALSE(s.Armed());
    s.Move(32, 400, 360, false); // turning sideways later does not re-arm it
    REQUIRE(s.Up() == SwipeDir::None);
    // Diagonal (|dx| <= 2|dy|) is not horizontal dominance.
    REQUIRE(Stroke(s, 500, 300, 400, 250) == SwipeDir::None);
    // A horizontal start owns the gesture (the scroll region does not start dragging).
    s.Down(0, 500, 300, true, true, 0);
    s.Move(16, 480, 302, false);
    REQUIRE(s.OwnsGesture());
    s.Move(32, 420, 305, false);
    REQUIRE(s.Up() == SwipeDir::Left);
    // Horizontal start that then drifts vertical: dominance fails at the lift, nothing fires.
    s.Down(0, 500, 300, true, true, 0);
    s.Move(16, 420, 305, false);
    s.Move(32, 400, 380, false);
    REQUIRE(s.Up() == SwipeDir::None);
}

TEST_CASE("DSMod swipe: slow drags, second fingers and page changes cancel",
          "[dsmod][runtime14][swipe]") {
    SwipeTracker s;
    // Reaching swipe_px more than SwipeMaxMs after leaving the slop: a drag, not a swipe.
    REQUIRE(Stroke(s, 500, 300, 380, 300, true, true, 0, 60, 40) == SwipeDir::None);
    // A rest before moving does not count against it (the clock starts at the slop crossing).
    s.Down(0, 500, 300, true, true, 0);
    s.Move(900, 500, 300, false);
    s.Move(1000, 470, 300, false);
    s.Move(1100, 400, 300, false);
    REQUIRE(s.Up() == SwipeDir::Left);
    // Cancel (second finger, drag, page transition, fired hold) is final.
    s.Down(0, 500, 300, true, true, 0);
    s.Move(16, 450, 300, true);
    s.Move(32, 380, 300, false);
    REQUIRE(s.Up() == SwipeDir::None);
    // Page switched under the finger.
    s.Down(0, 500, 300, true, true, 0, 2);
    s.Move(16, 420, 300, false, 3);
    s.Move(32, 380, 300, false, 2);
    REQUIRE(s.Up() == SwipeDir::None);
}

TEST_CASE("DSMod swipe: hit test, input_block above it and maps", "[dsmod][runtime14][swipe]") {
    const StateSnapshot state;
    Page page;
    page.widgets.push_back(SwipeCard({0, 0, 400, 300}));
    Widget button; // a small tap-only button above the card keeps its tap, the card its swipe
    button.type = WidgetType::Button;
    button.rect = {10, 10, 50, 50};
    button.on_tap = "btn";
    page.widgets.push_back(button);
    Widget block; // an input_block over the right half
    block.type = WidgetType::Rect;
    block.rect = {200, 0, 200, 300};
    block.input_block = true;
    page.widgets.push_back(block);
    Widget map_w;
    map_w.type = WidgetType::Map;
    map_w.rect = {0, 400, 400, 200};
    map_w.on_swipe_left = "x";
    page.widgets.push_back(map_w);
    Widget pz = SwipeCard({500, 0, 100, 100});
    pz.pan_zoom = true;
    page.widgets.push_back(pz);
    const auto widgets = ExpandWidgets(page, state);
    const auto arms = [&](s32 x, s32 y) {
        const s64 hit = HitTestIndex(widgets, state, x, y, SwipeHitFilter);
        return hit >= 0 && SwipeArms(widgets[static_cast<size_t>(hit)]);
    };
    REQUIRE(arms(100, 100));
    REQUIRE(arms(20, 20));         // under the tap-only button: the swipe is still the card's
    REQUIRE_FALSE(arms(300, 100)); // the input_block above blocks it
    REQUIRE_FALSE(arms(100, 500)); // map widgets never arm (they pan)
    REQUIRE_FALSE(arms(550, 50));  // nor pan_zoom widgets
    REQUIRE_FALSE(arms(450, 350)); // nothing there
}

TEST_CASE("DSMod swipe: a repeat template substitutes {i} in on_swipe_*",
          "[dsmod][runtime14][swipe]") {
    Page page;
    Widget card;
    card.type = WidgetType::Rect;
    card.rect = {0, 0, 100, 40};
    card.repeat_dy = 50;
    card.repeat = 3;
    card.on_swipe_left = "card.{i}.next";
    card.on_swipe_right = "card.{i}.prev";
    card.swipe_px = 80;
    page.widgets.push_back(card);
    const StateSnapshot state;
    const auto widgets = ExpandWidgets(page, state);
    REQUIRE(widgets.size() == 3);
    REQUIRE(widgets[1].on_swipe_left == "card.1.next");
    REQUIRE(widgets[2].on_swipe_right == "card.2.prev");
    REQUIRE(widgets[2].swipe_px == 80);
    const s64 hit = HitTestIndex(widgets, state, 50, 120, SwipeHitFilter);
    REQUIRE(hit == 2);
}

TEST_CASE("DSMod swipe: manifest keys on_swipe_left / on_swipe_right / swipe_px, haptic",
          "[dsmod][runtime14][swipe]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"min_runtime":13,"pages":[{"id":"p",
        "widgets":[{"type":"rect","rect":[0,0,10,10],"on_tap":"a","on_swipe_left":"b",
                    "on_swipe_right":"c","swipe_px":90},
                   {"type":"rect","rect":[0,0,10,10],"on_swipe_right":"c"}]}],
        "haptics":{"enabled":true,"swipe":"click"},
        "actions":{"a":{"kind":"flag","flag":"x"},"b":{"kind":"flag","flag":"y"},
                   "c":{"kind":"flag","flag":"z"}}})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.pages.size() == 1);
    const auto& w = m.pages[0].widgets;
    REQUIRE(w.size() == 2);
    REQUIRE(w[0].on_tap == "a");
    REQUIRE(w[0].on_swipe_left == "b");
    REQUIRE(w[0].on_swipe_right == "c");
    REQUIRE(w[0].swipe_px == 90);
    REQUIRE(w[1].on_swipe_left.empty());
    REQUIRE(w[1].on_swipe_right == "c");
    REQUIRE(w[1].swipe_px == 0);
    REQUIRE(m.haptics.strength[static_cast<size_t>(HapticKind::Swipe)] == HapticStrength::Click);
    // Default strength of the swipe kind.
    REQUIRE(HapticsConfig{}.strength[static_cast<size_t>(HapticKind::Swipe)] ==
            HapticStrength::Light);
}

// --- Map widget bound default view (view_rect_*_bind) --------------------------------------------

namespace {
/// Area "a" spans world (0,0)-(1000,500); widget "m" is 400x300 canvas px, so the whole-area fit
/// is 0.4 px per world unit. The bound rect is published as rx0/ry0/rx1/ry1.
struct RectMap {
    Manifest manifest;
    Page page;
    explicit RectMap(bool pan_zoom = true) {
        MapArea area;
        area.min_x = 0.0f;
        area.max_x = 1000.0f;
        area.min_y = 0.0f;
        area.max_y = 500.0f;
        manifest.map_areas.emplace("a", area);
        Widget w;
        w.type = WidgetType::Map;
        w.id = "m";
        w.area = "a";
        w.rect = {0, 0, 400, 300};
        w.pan_zoom = pan_zoom;
        w.min_zoom = 1.0f;
        w.max_zoom = 8.0f;
        auto extras = std::make_shared<MapWidgetExtras>();
        extras->view_rect_binds = {"rx0", "ry0", "rx1", "ry1"};
        w.map_extras = extras;
        page.id = "p";
        page.widgets.push_back(w);
    }
    const Widget& widget() const {
        return page.widgets[0];
    }
};
StateSnapshot RectState(f64 x0, f64 y0, f64 x1, f64 y1) {
    StateSnapshot s;
    s.floats["rx0"] = x0;
    s.ints["ry0"] = static_cast<s64>(y0); // ints resolve too
    s.floats["rx1"] = x1;
    s.floats["ry1"] = y1;
    return s;
}
bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}
} // namespace

TEST_CASE("DSMod view rect: fit math, normalising, zoom-out limit, glide step",
          "[dsmod][runtime14][view_rect]") {
    // Uniform scale min(rw/w, rh/h), centred.
    const MapBaseView f = FitWorldRect({100.0f, 100.0f, 300.0f, 200.0f}, 400, 300);
    REQUIRE(Near(f.cx, 200.0f));
    REQUIRE(Near(f.cy, 150.0f));
    REQUIRE(Near(f.ppw, 2.0f)); // min(400/200, 300/100)
    REQUIRE(Near(FitWorldRect({0.0f, 0.0f, 100.0f, 300.0f}, 400, 300).ppw, 1.0f)); // tall rect
    // Values in any order per axis, padded on every side.
    const auto r = NormaliseViewRect(300.0, 200.0, 100.0, 100.0, 10.0f);
    REQUIRE(r.has_value());
    REQUIRE(*r == std::array<float, 4>{90.0f, 90.0f, 310.0f, 210.0f});
    // A missing, non-finite or empty rect does not resolve.
    REQUIRE_FALSE(NormaliseViewRect(std::nullopt, 0.0, 1.0, 1.0, 0.0f).has_value());
    REQUIRE_FALSE(NormaliseViewRect(0.0, 0.0, 1.0, std::nan(""), 0.0f).has_value());
    REQUIRE_FALSE(NormaliseViewRect(5.0, 0.0, 5.0, 1.0, 0.0f).has_value());
    REQUIRE(NormaliseViewRect(5.0, 0.0, 5.0, 1.0, 1.0f).has_value()); // a line + pad has area
    // Zoom out down to exactly the whole-area fit, never past it.
    REQUIRE(Near(RectMinZoom(0.4f, 2.0f, 1.0f), 0.2f));
    REQUIRE(Near(RectMinZoom(0.4f, 2.0f, 0.05f), 0.2f)); // a lower widget min_zoom is clamped
    REQUIRE(Near(RectMinZoom(0.4f, 0.2f, 1.0f), 1.0f));  // the rect already shows the whole area
    // Glide: a quarter of the way per step, lands exactly, holds while the user's view is in
    // effect.
    MapBaseView shown{200.0f, 150.0f, 2.0f};
    const MapBaseView target{700.0f, 350.0f, 4.0f};
    REQUIRE_FALSE(StepBaseGlide(shown, target, true));
    REQUIRE(Near(shown.cx, 200.0f));
    REQUIRE(StepBaseGlide(shown, target, false));
    REQUIRE(Near(shown.cx, 325.0f));
    REQUIRE(Near(shown.ppw, 2.5f));
    int steps = 1;
    while (StepBaseGlide(shown, target, false)) {
        ++steps;
        REQUIRE(steps < 100);
    }
    REQUIRE(shown.cx == target.cx);
    REQUIRE(shown.ppw == target.ppw);
    REQUIRE(MapViewCustom(1.0f, 0.0f, 0.0f, true));
    REQUIRE(MapViewCustom(0.5f, 0.0f, 0.0f, false));
    REQUIRE_FALSE(MapViewCustom(1.0f, 0.2f, 0.0f, false));
}

TEST_CASE("DSMod view rect: base view is the rect fit, falls back to the area fit",
          "[dsmod][runtime14][view_rect]") {
    RectMap map;
    const ViewState views;
    // All four resolve: the rect fit at zoom 1 / pan 0.
    const auto fit = ProbeMapView(map.manifest, map.page, RectState(100, 100, 300, 200),
                                  map.widget(), 0, views, {});
    REQUIRE(fit.has_value());
    REQUIRE(Near(fit->cx, 200.0f));
    REQUIRE(Near(fit->cy, 150.0f));
    REQUIRE(Near(fit->ppw, 2.0f));
    // With padding.
    {
        RectMap padded;
        auto extras = std::make_shared<MapWidgetExtras>(*padded.widget().map_extras);
        extras->view_rect_pad = 50.0f; // 300x200 -> min(400/300, 300/200)
        padded.page.widgets[0].map_extras = extras;
        const auto p = ProbeMapView(padded.manifest, padded.page, RectState(100, 100, 300, 200),
                                    padded.widget(), 0, views, {});
        REQUIRE(p.has_value());
        REQUIRE(Near(p->ppw, 400.0f / 300.0f));
        REQUIRE(Near(p->cx, 200.0f));
    }
    // One value missing and nothing shown yet: today's whole-area fit.
    StateSnapshot missing = RectState(100, 100, 300, 200);
    missing.floats.erase("ry1");
    const auto area = ProbeMapView(map.manifest, map.page, missing, map.widget(), 0, views, {});
    REQUIRE(area.has_value());
    REQUIRE(Near(area->cx, 500.0f));
    REQUIRE(Near(area->cy, 250.0f));
    REQUIRE(Near(area->ppw, 0.4f));
    // A transient miss after a rect was shown keeps that rect (no flash to the whole area).
    MapFollowState follow;
    follow["m@a#rect_c"] = {200.0f, 150.0f};
    follow["m@a#rect_z"] = {2.0f, 0.0f};
    const auto held = ProbeMapView(map.manifest, map.page, missing, map.widget(), 0, views, follow);
    REQUIRE(held.has_value());
    REQUIRE(Near(held->cx, 200.0f));
    REQUIRE(Near(held->ppw, 2.0f));
    // No view_rect keys at all: unchanged behaviour (area fit).
    RectMap plain;
    plain.page.widgets[0].map_extras = nullptr;
    const auto p = ProbeMapView(plain.manifest, plain.page, RectState(100, 100, 300, 200),
                                plain.widget(), 0, views, {});
    REQUIRE(p.has_value());
    REQUIRE(Near(p->ppw, 0.4f));
}

TEST_CASE("DSMod view rect: pinch out stops at the whole-area fit; reset is the rect fit",
          "[dsmod][runtime14][view_rect]") {
    RectMap map;
    const StateSnapshot s = RectState(100, 100, 300, 200);
    ViewState views;
    views["m"].zoom = 0.5f; // half the rect fit
    auto v = ProbeMapView(map.manifest, map.page, s, map.widget(), 0, views, {});
    REQUIRE(v.has_value());
    REQUIRE(Near(v->ppw, 1.0f));
    views["m"].zoom = 0.01f; // far past the area: clamped to the whole-area fit
    v = ProbeMapView(map.manifest, map.page, s, map.widget(), 0, views, {});
    REQUIRE(Near(v->ppw, 0.4f));
    views["m"].zoom = 3.0f; // zoom in on top of the rect fit as before
    v = ProbeMapView(map.manifest, map.page, s, map.widget(), 0, views, {});
    REQUIRE(Near(v->ppw, 6.0f));
    // view_reset glides the view to zoom 1 / pan 0: that is the rect fit again.
    views["m"] = ViewTransform{};
    v = ProbeMapView(map.manifest, map.page, s, map.widget(), 0, views, {});
    REQUIRE(Near(v->ppw, 2.0f));
    REQUIRE(Near(v->cx, 200.0f));
    // A draw publishes the pinch limit ("<id>#zmin", read by ZoomLimits) and the shown base.
    Canvas canvas;
    canvas.Resize(400, 300);
    MapFollowState follow;
    RenderPage(canvas, map.manifest, map.page, s, {}, ViewState{}, &follow);
    REQUIRE(follow.contains("m#zmin"));
    REQUIRE(Near(follow["m#zmin"][0], 0.2f));
    REQUIRE(Near(follow["m@a#rect_c"][0], 200.0f));
    REQUIRE(Near(follow["m@a#rect_z"][0], 2.0f));
}

TEST_CASE("DSMod view rect: a new rect glides the base view, never over a custom view",
          "[dsmod][runtime14][view_rect]") {
    RectMap map;
    MapFollowState follow; // the old room was shown
    follow["m@a#rect_c"] = {200.0f, 150.0f};
    follow["m@a#rect_z"] = {2.0f, 0.0f};
    const StateSnapshot next = RectState(600, 300, 800, 400); // the player entered another room
    ViewState views;
    auto v = ProbeMapView(map.manifest, map.page, next, map.widget(), 0, views, follow);
    REQUIRE(v.has_value());
    REQUIRE(Near(v->cx, 325.0f)); // a quarter of the way to 700: gliding, not jumping
    REQUIRE(Near(v->cy, 200.0f));
    // Driven to the end through a real follow state it lands on the new fit.
    Canvas canvas;
    canvas.Resize(400, 300);
    for (int i = 0; i < 100; ++i) {
        RenderPage(canvas, map.manifest, map.page, next, {}, views, &follow);
    }
    REQUIRE(Near(follow["m@a#rect_c"][0], 700.0f));
    REQUIRE(Near(follow["m@a#rect_c"][1], 350.0f));
    // The user moved the view (custom, or a reset still gliding): the base holds.
    follow["m@a#rect_c"] = {200.0f, 150.0f};
    views["m"].gliding = true;
    v = ProbeMapView(map.manifest, map.page, next, map.widget(), 0, views, follow);
    REQUIRE(Near(v->cx, 200.0f));
    views["m"] = ViewTransform{};
    views["m"].zoom = 2.0f;
    RenderPage(canvas, map.manifest, map.page, next, {}, views, &follow);
    REQUIRE(Near(follow["m@a#rect_c"][0], 200.0f));
    // A widget without pan_zoom has no user view: always glides.
    RectMap fixed(false);
    ViewState any;
    any["m"].gliding = true;
    v = ProbeMapView(fixed.manifest, fixed.page, next, fixed.widget(), 0, any, follow);
    REQUIRE(Near(v->cx, 325.0f));
}

TEST_CASE("DSMod view rect: manifest keys", "[dsmod][runtime14][view_rect]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"map","rect":[0,0,10,10],"view_rect_x0_bind":"a","view_rect_y0_bind":"b",
         "view_rect_x1_bind":"c","view_rect_y1_bind":"d","view_rect_pad":12.5},
        {"type":"map","rect":[0,0,10,10],"view_rect_x0_bind":"a"},
        {"type":"map","rect":[0,0,10,10]}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& w = m.pages[0].widgets;
    REQUIRE(w.size() == 3);
    REQUIRE(w[0].map_extras != nullptr);
    REQUIRE(w[0].map_extras->HasViewRect());
    REQUIRE(w[0].map_extras->view_rect_binds == std::array<std::string, 4>{"a", "b", "c", "d"});
    REQUIRE(w[0].map_extras->view_rect_pad == 12.5f);
    REQUIRE(w[1].map_extras != nullptr);
    REQUIRE_FALSE(w[1].map_extras->HasViewRect()); // needs all four
    REQUIRE(w[2].map_extras == nullptr);
}

// --- Map widget battle-map additions: image_bind, overlays, per-slot marker pictures -------------

namespace {
constexpr u32 Red = 0xFFFF0000u, Blue = 0xFF0000FFu, Green = 0xFF00FF00u, White = 0xFFFFFFFFu;
constexpr u32 Yellow = 0xFFFFFF00u, Magenta = 0xFFFF00FFu;
std::shared_ptr<const Image> Solid(u32 argb, u32 w = 8, u32 h = 8) {
    auto img = std::make_shared<Image>();
    img->w = w;
    img->h = h;
    img->pixels.assign(static_cast<size_t>(w) * h, argb);
    return img;
}
/// Area "b": world (0,0)-(100,100) with a static picture; widget "m" 100x100 at the origin, so at
/// zoom 1 canvas x = world x and canvas y = 100 - world y.
struct BattleMap {
    Manifest manifest;
    Page page;
    std::map<std::string, std::shared_ptr<const Image>> pics{{"img.red", Solid(Red)},
                                                             {"img.blue", Solid(Blue)},
                                                             {"img.green", Solid(Green)},
                                                             {"img.white", Solid(White)},
                                                             {"img.white2", Solid(White)}};
    BattleMap() {
        manifest.background = 0xFF000000u;
        MapArea area;
        area.min_x = 0.0f;
        area.max_x = 100.0f;
        area.min_y = 0.0f;
        area.max_y = 100.0f;
        area.image = "img.blue";
        DynamicMarkerDef dm;
        dm.group = "units";
        dm.count = 3;
        dm.x = "u{i}.x";
        dm.y = "u{i}.y";
        dm.kind = "u{i}.k";
        dm.icon_by_kind[1] = "AtlasPin"; // no atlas here: only the pictures can draw
        dm.icon_src_bind = "u{i}.img";
        dm.size_world = 10.0f;
        dm.bar_bind = "u{i}.hp";
        dm.bar_max_bind = "u{i}.hpmax";
        dm.bar_color = Yellow;
        dm.dim_bind = "u{i}.done";
        dm.frame_color_bind = "u{i}.army";
        area.dynamic_markers.push_back(dm);
        manifest.map_areas.emplace("b", area);
        Widget w;
        w.type = WidgetType::Map;
        w.id = "m";
        w.area = "b";
        w.rect = {0, 0, 100, 100};
        w.pan_zoom = true;
        w.max_zoom = 8.0f;
        w.area_label = false;
        auto extras = std::make_shared<MapWidgetExtras>();
        extras->image_bind = "bt.map";
        MapWidgetExtras::Overlay ov;
        ov.src_bind = "bt.ov";
        ov.x0 = 30.0f;
        ov.y0 = 60.0f;
        ov.x1 = 50.0f;
        ov.y1 = 80.0f;
        extras->overlays.push_back(ov);
        w.map_extras = extras;
        page.id = "p";
        page.widgets.push_back(w);
    }
    ImageProvider Images() const {
        return [this](const std::string& key) -> std::shared_ptr<const Image> {
            const auto it = pics.find(key);
            return it != pics.end() ? it->second : nullptr;
        };
    }
    Canvas Draw(const StateSnapshot& s, float zoom = 1.0f) const {
        Canvas canvas;
        canvas.Resize(100, 100);
        ViewState views;
        views["m"].zoom = zoom;
        RenderPage(canvas, manifest, page, s, Images(), views);
        return canvas;
    }
};
u32 Px(const Canvas& c, s32 x, s32 y) {
    return c.Pixels()[static_cast<size_t>(y) * c.Width() + static_cast<size_t>(x)];
}
StateSnapshot Units() {
    StateSnapshot s;
    s.texts["bt.map"] = "img.red";
    s.texts["bt.ov"] = "img.green";
    // Unit 0 at world (20,20): a portrait, half its HP, an army frame.
    s.ints["u0.x"] = 20;
    s.ints["u0.y"] = 20;
    s.ints["u0.k"] = 1;
    s.texts["u0.img"] = "img.white";
    s.ints["u0.hp"] = 5;
    s.ints["u0.hpmax"] = 10;
    s.ints["u0.army"] = static_cast<s64>(Magenta);
    // Unit 1 at (70,20): acted (dimmed).
    s.ints["u1.x"] = 70;
    s.ints["u1.y"] = 20;
    s.ints["u1.k"] = 1;
    s.texts["u1.img"] = "img.white2";
    s.ints["u1.done"] = 1;
    // Unit 2 at (30,70): no picture -> its atlas icon, and there is no atlas: not drawn.
    s.ints["u2.x"] = 30;
    s.ints["u2.y"] = 70;
    s.ints["u2.k"] = 1;
    return s;
}
} // namespace

TEST_CASE("DSMod battle map: image_bind overrides the area picture, falls back to it",
          "[dsmod][runtime14][battle_map]") {
    const BattleMap map;
    StateSnapshot s;
    s.texts["bt.map"] = "img.red";
    REQUIRE(Px(map.Draw(s), 5, 5) == Red);
    s.texts["bt.map"] = ""; // empty: the area's own picture
    REQUIRE(Px(map.Draw(s), 5, 5) == Blue);
    s.texts.erase("bt.map"); // missing: likewise
    REQUIRE(Px(map.Draw(s), 5, 5) == Blue);
}

TEST_CASE("DSMod battle map: overlays sit in world space and pan / zoom with the map",
          "[dsmod][runtime14][battle_map]") {
    const BattleMap map;
    StateSnapshot s;
    s.texts["bt.map"] = "img.red";
    s.texts["bt.ov"] = "img.green";
    // World (30,60)-(50,80) -> canvas x 30..50, y 20..40 at zoom 1.
    auto c = map.Draw(s);
    REQUIRE(Px(c, 40, 30) == Green);
    REQUIRE(Px(c, 31, 21) == Green);
    REQUIRE(Px(c, 25, 30) == Red);
    REQUIRE(Px(c, 40, 45) == Red);
    // Zoom 2 about the top-left (pan 0): canvas = 2 * (world x), 2 * (100 - world y) -> x 60..100,
    // y 40..80.
    c = map.Draw(s, 2.0f);
    REQUIRE(Px(c, 70, 60) == Green);
    REQUIRE(Px(c, 55, 60) == Red);
    REQUIRE(Px(c, 70, 35) == Red);
    // Hidden by its gate; no picture named -> nothing.
    BattleMap gated;
    auto extras = std::make_shared<MapWidgetExtras>(*gated.page.widgets[0].map_extras);
    extras->overlays[0].show = PointGate{"bt.show", false};
    gated.page.widgets[0].map_extras = extras;
    REQUIRE(Px(gated.Draw(s), 40, 30) == Red);
    s.ints["bt.show"] = 1;
    REQUIRE(Px(gated.Draw(s), 40, 30) == Green);
    s.texts["bt.ov"] = "";
    REQUIRE(Px(gated.Draw(s), 40, 30) == Red);
}

TEST_CASE("DSMod battle map: per-slot pictures, world size, bar, dim and frame",
          "[dsmod][runtime14][battle_map]") {
    const BattleMap map;
    const StateSnapshot s = Units();
    auto c = map.Draw(s);
    // Unit 0: 10 world units = 10 px at zoom 1, centred on (20,80): x 15..25, y 75..85. The
    // picture overrides the kind's atlas icon.
    REQUIRE(Px(c, 20, 80) == White);
    REQUIRE(Px(c, 15, 80) == Magenta); // its 2 px army frame
    REQUIRE(Px(c, 24, 80) == Magenta);
    REQUIRE(Px(c, 20, 76) == Magenta);
    REQUIRE(Px(c, 27, 80) == Red); // outside the marker
    // The bar under it: 5 / 10 filled in the bar colour, the rest the dark background.
    REQUIRE(Px(c, 16, 87) == Yellow);
    REQUIRE(Px(c, 23, 87) != Yellow);
    REQUIRE(Px(c, 23, 87) != Red);
    // Unit 1 (acted): dimmed picture, no bar, no frame.
    const u32 dim = Px(c, 70, 80);
    REQUIRE((dim & 0xFF000000u) == 0xFF000000u);
    REQUIRE(((dim >> 8) & 0xFF) > 90);
    REQUIRE(((dim >> 8) & 0xFF) < 140);
    REQUIRE(((dim >> 16) & 0xFF) == ((dim >> 8) & 0xFF));
    REQUIRE(Px(c, 70, 87) == Red);
    // Unit 2: no picture and no icon atlas -> nothing (the existing atlas path, unchanged).
    REQUIRE(Px(c, 30, 30) == Green); // the overlay under where it would be
    // Zoom 2: world-sized markers grow with the map. Unit 0 now at canvas (40,160): off the
    // widget; move it to world (30,70) -> canvas (60,60), 20 px: x 50..70.
    StateSnapshot z = s;
    z.ints["u0.x"] = 30;
    z.ints["u0.y"] = 70;
    c = map.Draw(z, 2.0f);
    REQUIRE(Px(c, 53, 60) == White);
    REQUIRE(Px(c, 66, 60) == White);
    REQUIRE(Px(c, 60, 53) == White);
    REQUIRE(Px(c, 47, 60) == Red); // just outside the 20 px box, on the base picture
    // Without size_world the pixel size stays as before (map.style.item_icon, zoom-independent).
    BattleMap px_map;
    px_map.manifest.map_areas.at("b").dynamic_markers[0].size_world = 0.0f;
    px_map.manifest.map_areas.at("b").dynamic_markers[0].size = 6;
    c = px_map.Draw(z, 2.0f);
    REQUIRE(Px(c, 60, 60) == White);
    REQUIRE(Px(c, 53, 60) != White); // 6 px: x 57..63
}

TEST_CASE("DSMod battle map: manifest keys", "[dsmod][runtime14][battle_map]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"map","rect":[0,0,10,10],"area":"b","image_bind":"bt.map",
         "overlays":[{"src_bind":"bt.ov","x0":1,"y0":2,"x1":3,"y1":4,"show_bind":"!bt.hide",
                      "opacity":0.5},
                     {"src":"fixed.png","x0":0,"y0":0,"x1":5,"y1":5},
                     {"x0":0,"y0":0,"x1":5,"y1":5}]}]}],
        "map":{"areas":{"b":{"min":[0,0],"max":[100,100],"dynamic_markers":[
          {"group":"units","count":4,"x":"u{i}.x","y":"u{i}.y","icon":"Pin",
           "icon_src_bind":"bt.u{i}.dport_s","size_world":12.5,"bar_bind":"u{i}.hp",
           "bar_max_bind":"u{i}.hpmax","bar_color":"#FF00FF00","bar_h":4,"dim_bind":"u{i}.done",
           "frame_color_bind":"u{i}.army","frame_px":3,"tint_bind":"u{i}.tint"},
          {"count":1,"x":"a","y":"b","icon":"Pin"}]}}}})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& mx = m.pages[0].widgets[0].map_extras;
    REQUIRE(mx != nullptr);
    REQUIRE(mx->image_bind == "bt.map");
    REQUIRE(mx->overlays.size() == 2); // the one without a picture is dropped
    REQUIRE(mx->overlays[0].src_bind == "bt.ov");
    REQUIRE(mx->overlays[0].x1 == 3.0f);
    REQUIRE(mx->overlays[0].y1 == 4.0f);
    REQUIRE(mx->overlays[0].show.point == "bt.hide");
    REQUIRE(mx->overlays[0].show.negate);
    REQUIRE(mx->overlays[0].opacity == 0.5f);
    REQUIRE(mx->overlays[1].src == "fixed.png");
    const auto& dms = m.map_areas.at("b").dynamic_markers;
    REQUIRE(dms.size() == 2);
    REQUIRE(dms[0].icon_src_bind == "bt.u{i}.dport_s");
    REQUIRE(dms[0].size_world == 12.5f);
    REQUIRE(dms[0].bar_bind == "u{i}.hp");
    REQUIRE(dms[0].bar_max_bind == "u{i}.hpmax");
    REQUIRE(dms[0].bar_color == 0xFF00FF00u);
    REQUIRE(dms[0].bar_h == 4);
    REQUIRE(dms[0].dim_bind == "u{i}.done");
    REQUIRE(dms[0].frame_color_bind == "u{i}.army");
    REQUIRE(dms[0].frame_px == 3);
    REQUIRE(dms[0].tint_bind == "u{i}.tint");
    // Absent keys: today's behaviour.
    REQUIRE(dms[1].icon_src_bind.empty());
    REQUIRE(dms[1].size_world == 0.0f);
    REQUIRE(dms[1].bar_bind.empty());
}
