// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Unreleased runtime 15 additions: vertical swipe ("on_swipe_up" / "on_swipe_down",
// mod_input_swipe.h) and the bound default view of a non-map pan_zoom widget ("view_zoom_bind" /
// "view_cx_bind" / "view_cy_bind" / "view_reset_bind", mod_view_default.h).
#include <cmath>
#include <optional>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_input_swipe.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"
#include "core/mods/mod_view_default.h"

using namespace Core::Mods;

namespace {
/// One finger from (x0, y0) to (x1, y1) in `steps` 16 ms frames, then lifts; the widget has the
/// given swipe actions.
SwipeDir Stroke(SwipeTracker& s, int x0, int y0, int x1, int y1, bool left, bool right, bool up,
                bool down, int steps = 6, int frame_ms = 16) {
    s.Down(0, x0, y0, left, right, 0, 0, up, down);
    for (int i = 1; i <= steps; ++i) {
        s.Move(static_cast<std::int64_t>(i) * frame_ms, x0 + (x1 - x0) * i / steps,
               y0 + (y1 - y0) * i / steps, false);
    }
    return s.Up();
}

bool Near(float a, float b) {
    return std::fabs(a - b) < 1e-3f;
}
} // namespace

TEST_CASE("DSMod swipe: up and down mirror left and right", "[dsmod][runtime15][swipe]") {
    SwipeTracker s;
    REQUIRE(Stroke(s, 300, 500, 310, 380, false, false, true, true) == SwipeDir::Up);
    REQUIRE(Stroke(s, 300, 500, 290, 620, false, false, true, true) == SwipeDir::Down);
    // only the direction the widget has an action for fires
    REQUIRE(Stroke(s, 300, 500, 300, 380, false, false, false, true) == SwipeDir::None);
    REQUIRE(Stroke(s, 300, 500, 300, 620, false, false, false, true) == SwipeDir::Down);
    // same threshold as the horizontal swipe (DefaultSwipePx), same dominance (|dy| > 2|dx|)
    REQUIRE(Stroke(s, 300, 500, 300, 445, false, false, true, true) == SwipeDir::None);
    REQUIRE(Stroke(s, 300, 500, 300, 440, false, false, true, true) == SwipeDir::Up);
    REQUIRE(Stroke(s, 300, 500, 250, 400, false, false, true, true) == SwipeDir::None);
    // too slow: a drag
    REQUIRE(Stroke(s, 300, 500, 300, 380, false, false, true, true, 60, 40) == SwipeDir::None);
}

TEST_CASE("DSMod swipe: a vertical-only widget leaves horizontal drags alone and vice versa",
          "[dsmod][runtime15][swipe]") {
    SwipeTracker s;
    // horizontal finger on an up/down widget: not armed after the lock, does not own the gesture
    s.Down(0, 300, 500, false, false, 0, 0, true, true);
    s.Move(16, 330, 502, false);
    REQUIRE_FALSE(s.OwnsGesture());
    REQUIRE_FALSE(s.Armed());
    REQUIRE(s.Up() == SwipeDir::None);
    // vertical finger on a left/right widget: as before (a scroll or nothing)
    s.Down(0, 300, 500, true, true, 0, 0, false, false);
    s.Move(16, 300, 530, false);
    REQUIRE_FALSE(s.OwnsGesture());
    REQUIRE(s.Up() == SwipeDir::None);
    // all four: the first leg of the move picks the axis and owns the gesture
    s.Down(0, 300, 500, true, true, 0, 0, true, true);
    s.Move(16, 300, 480, false);
    REQUIRE(s.OwnsGesture());
    s.Move(32, 302, 420, false);
    REQUIRE(s.Up() == SwipeDir::Up);
    s.Down(0, 300, 500, true, true, 0, 0, true, true);
    s.Move(16, 330, 500, false);
    s.Move(32, 380, 504, false);
    REQUIRE(s.Up() == SwipeDir::Right);
    // a second finger cancels
    s.Down(0, 300, 500, false, false, 0, 0, true, true);
    s.Move(16, 300, 470, false);
    s.Move(32, 300, 400, true);
    REQUIRE(s.Up() == SwipeDir::None);
}

TEST_CASE("DSMod swipe: up/down widgets arm the swipe hit test, manifest keys, {i}",
          "[dsmod][runtime15][swipe]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"min_runtime":15,"pages":[{"id":"p",
        "widgets":[{"type":"rect","rect":[0,0,100,100],"on_swipe_up":"u{i}","on_swipe_down":"d",
                    "repeat":2,"repeat_dx":100}]}],
        "actions":{"d":{"kind":"flag","flag":"y"}}})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& w = m.pages[0].widgets[0];
    REQUIRE(w.on_swipe_up == "u{i}");
    REQUIRE(w.on_swipe_down == "d");
    REQUIRE(SwipeArms(w));
    REQUIRE(SwipeHitFilter(w));
    const StateSnapshot state;
    const auto expanded = ExpandWidgets(m.pages[0], state);
    REQUIRE(expanded.size() == 2);
    REQUIRE(expanded[1].on_swipe_up == "u1");
    Widget pz = w;
    pz.pan_zoom = true;
    REQUIRE_FALSE(SwipeArms(pz)); // never on a pannable widget
}

TEST_CASE("DSMod view default: the window is centred on the bound point and clamped",
          "[dsmod][runtime15][view_default]") {
    // zoom 2 on a 1000x800 widget: the window is half the picture
    auto h = DefaultView(1000, 800, 2.0, 0.5, 0.5, 1.0f, 4.0f);
    REQUIRE(h);
    REQUIRE(Near(h->zoom, 2.0f));
    REQUIRE(Near(h->pan_x, 250.0f)); // window u 0.25..0.75
    REQUIRE(Near(h->pan_y, 200.0f));
    // near an edge: clamped so the window stays on the picture
    h = DefaultView(1000, 800, 2.0, 0.05, 0.98, 1.0f, 4.0f);
    REQUIRE(Near(h->pan_x, 0.0f));
    REQUIRE(Near(h->pan_y, 400.0f));
    // zoom outside the widget's limits is clamped; zoom 1 = the whole picture, no pan
    h = DefaultView(1000, 800, 0.5, 0.3, 0.3, 1.0f, 4.0f);
    REQUIRE(Near(h->zoom, 1.0f));
    REQUIRE(Near(h->pan_x, 0.0f));
    REQUIRE(Near(h->pan_y, 0.0f));
    REQUIRE(Near(DefaultView(1000, 800, 9.0, 0.5, 0.5, 1.0f, 4.0f)->zoom, 4.0f));
    // missing / bad values: no default
    REQUIRE_FALSE(DefaultView(1000, 800, std::nullopt, 0.5, 0.5, 1.0f, 4.0f));
    REQUIRE_FALSE(DefaultView(1000, 800, 2.0, NAN, 0.5, 1.0f, 4.0f));
    REQUIRE_FALSE(DefaultView(0, 800, 2.0, 0.5, 0.5, 1.0f, 4.0f));
}

TEST_CASE("DSMod view default: home, follow, user views and the reset value",
          "[dsmod][runtime15][view_default]") {
    ViewTransform v;
    REQUIRE_FALSE(ViewAwayFromHome(v)); // no default: zoom 1 / no pan is home, as before
    const ViewHome a{2.0f, 250.0f, 200.0f};
    // a fresh view starts at the default
    REQUIRE(ApplyViewDefault(v, a, 7.0, false));
    REQUIRE(Near(v.zoom, 2.0f));
    REQUIRE(Near(v.pan_x, 250.0f));
    REQUIRE_FALSE(ViewAwayFromHome(v));
    REQUIRE_FALSE(ApplyViewDefault(v, a, 7.0, false)); // unchanged
    // a view at its home follows a new default (the current room moved)
    const ViewHome b{2.0f, 300.0f, 180.0f};
    REQUIRE(ApplyViewDefault(v, b, 7.0, false));
    REQUIRE(Near(v.pan_x, 300.0f));
    REQUIRE(Near(v.pan_y, 180.0f));
    // the user zooms out: custom; a new default only moves the home
    v.zoom = 1.0f;
    v.pan_x = v.pan_y = 0.0f;
    REQUIRE(ViewAwayFromHome(v));
    REQUIRE(ApplyViewDefault(v, a, 7.0, false));
    REQUIRE(Near(v.zoom, 1.0f));
    REQUIRE(Near(v.home_pan_x, 250.0f));
    // RESET glides to the home
    GlideViewHome(v);
    REQUIRE(v.gliding);
    REQUIRE(Near(v.goal_zoom, 2.0f));
    REQUIRE(Near(v.goal_pan_x, 250.0f));
    REQUIRE(Near(v.goal_pan_y, 200.0f));
    v.gliding = false;
    // a changed reset value (a new floor) snaps even a user-moved view home
    v.zoom = 3.0f;
    REQUIRE(ApplyViewDefault(v, b, 8.0, false));
    REQUIRE(Near(v.zoom, 2.0f));
    REQUIRE(Near(v.pan_x, 300.0f));
    REQUIRE_FALSE(ViewAwayFromHome(v));
    // a finger on the view: the home moves, the view stays under the finger
    REQUIRE(ApplyViewDefault(v, a, 8.0, true));
    REQUIRE(Near(v.pan_x, 300.0f));
    REQUIRE(ViewAwayFromHome(v));
    // the default disappears: home is zoom 1 / no pan again
    ViewTransform w;
    REQUIRE(ApplyViewDefault(w, a, std::nullopt, false));
    REQUIRE(ApplyViewDefault(w, std::nullopt, std::nullopt, false));
    REQUIRE(Near(w.zoom, 1.0f));
    REQUIRE(Near(w.pan_x, 0.0f));
}

TEST_CASE("DSMod view default: manifest keys", "[dsmod][runtime15][view_default]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"min_runtime":15,"pages":[{"id":"p",
        "widgets":[{"type":"image","id":"m","rect":[0,0,100,100],"pan_zoom":true,
                    "view_zoom_bind":"z","view_cx_bind":"x","view_cy_bind":"y",
                    "view_reset_bind":"r"},
                   {"type":"image","rect":[0,0,100,100],"pan_zoom":true,"view_zoom_bind":"z"},
                   {"type":"image","rect":[0,0,100,100],"view_zoom_bind":"z",
                    "view_cx_bind":"x","view_cy_bind":"y"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& w = m.pages[0].widgets;
    REQUIRE(w[0].view_default);
    REQUIRE(w[0].view_default->zoom_bind == "z");
    REQUIRE(w[0].view_default->cx_bind == "x");
    REQUIRE(w[0].view_default->cy_bind == "y");
    REQUIRE(w[0].view_default->reset_bind == "r");
    REQUIRE_FALSE(w[1].view_default); // all three are needed
    REQUIRE_FALSE(w[2].view_default); // only on a pan_zoom widget
}
