// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// MapViewHostValueFrom / PanZoomViewHostValueFrom (mod_input.cpp): the pure matching logic behind
// ModRuntime::MapViewHostValue / ModRuntime::PanZoomViewHostValue, which the get_f64/get_i64
// fall-throughs in mod_module_host.cpp call so a native module's sample() can read
// "@map_view_<field>:<area>" and "@view_zoom:<id>" / "@view_pan_x:<id>" / "@view_pan_y:<id>" from
// the last-drawn records rather than the per-tick snapshot (which is cleared before those are
// published into it). A real ModRuntime needs a Core::System, which nothing in this test binary
// builds (see the "pulls in the whole core" note in min_runtime.cpp), so these two helpers are
// kept free of ModRuntime (mod_runtime.h) and driven directly here with hand-built records/view
// state, the same way ClampPanAxis / DefaultView are unit tested in pan_bounds.cpp.
#include <cmath>
#include <optional>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/mods/mod_runtime.h"
#include "core/mods/mod_view_default.h"

using namespace Core::Mods;

namespace {
/// One draw record of area `area`, every field a distinct value so a wrong field reads wrong.
MapDrawRecord Record(std::string area) {
    MapDrawRecord rec;
    rec.area = std::move(area);
    rec.cx = 11.0f;
    rec.cy = 22.0f;
    rec.ppw = 3.5f;
    rec.rect = {40, 50, 60, 70};
    rec.rotation = 90.0f;
    return rec;
}
} // namespace

TEST_CASE("DSMod host view values: MapViewHostValueFrom answers nullopt for anything not its key",
          "[dsmod][host_view_values]") {
    const MapDrawRecords records{Record("overworld")};
    for (const char* key : {"", "cx", "@flag:foo", "@map_view", "@view_zoom:overworld"}) {
        REQUIRE_FALSE(MapViewHostValueFrom(key, records, 0, 0).has_value());
    }
    // The prefix with no field/area split at all (no colon, or nothing after it).
    REQUIRE_FALSE(MapViewHostValueFrom("@map_view_cx", records, 0, 0).has_value());
    REQUIRE_FALSE(MapViewHostValueFrom("@map_view_cx:", records, 0, 0).has_value());
}

TEST_CASE("DSMod host view values: MapViewHostValueFrom answers each published field",
          "[dsmod][host_view_values]") {
    // Two areas, so picking the right one also proves the loop does not just answer the first
    // record.
    const MapDrawRecords records{Record("dungeon"), Record("overworld")};
    REQUIRE(MapViewHostValueFrom("@map_view_cx:overworld", records, 0, 0) == Catch::Approx(11.0));
    REQUIRE(MapViewHostValueFrom("@map_view_cy:overworld", records, 0, 0) == Catch::Approx(22.0));
    REQUIRE(MapViewHostValueFrom("@map_view_ppw:overworld", records, 0, 0) == Catch::Approx(3.5));
    REQUIRE(MapViewHostValueFrom("@map_view_x:overworld", records, 0, 0) == Catch::Approx(40.0));
    REQUIRE(MapViewHostValueFrom("@map_view_y:overworld", records, 0, 0) == Catch::Approx(50.0));
    REQUIRE(MapViewHostValueFrom("@map_view_w:overworld", records, 0, 0) == Catch::Approx(60.0));
    REQUIRE(MapViewHostValueFrom("@map_view_h:overworld", records, 0, 0) == Catch::Approx(70.0));
    REQUIRE(MapViewHostValueFrom("@map_view_rot:overworld", records, 0, 0) == Catch::Approx(90.0));
    // An unknown field of a known area: nullopt (the loop finds the area, then the field).
    REQUIRE_FALSE(MapViewHostValueFrom("@map_view_bogus:overworld", records, 0, 0).has_value());
    // A matching current/records page need not be page 0.
    REQUIRE(MapViewHostValueFrom("@map_view_cx:overworld", records, 7, 7) == Catch::Approx(11.0));
}

TEST_CASE("DSMod host view values: MapViewHostValueFrom answers nullopt for an unknown area",
          "[dsmod][host_view_values]") {
    const MapDrawRecords records{Record("overworld")};
    REQUIRE_FALSE(MapViewHostValueFrom("@map_view_cx:dungeon", records, 0, 0).has_value());
    const MapDrawRecords two{Record("a"), Record("b")};
    REQUIRE_FALSE(MapViewHostValueFrom("@map_view_cx:c", two, 0, 0).has_value());
}

TEST_CASE("DSMod host view values: MapViewHostValueFrom answers nullopt for another page's "
          "records",
          "[dsmod][host_view_values]") {
    const MapDrawRecords records{Record("overworld")};
    // Same records, same field/area: only the page numbers differ (ModRuntime's own
    // map_records_page vs current_page -- see MapViewHostValue, mod_input.cpp).
    REQUIRE_FALSE(MapViewHostValueFrom("@map_view_cx:overworld", records, 0, 1).has_value());
    REQUIRE_FALSE(MapViewHostValueFrom("@map_view_cx:overworld", records, 2, 0).has_value());
    REQUIRE(MapViewHostValueFrom("@map_view_cx:overworld", records, 3, 3).has_value());
}

TEST_CASE("DSMod host view values: PanZoomViewHostValueFrom answers nullopt for anything not its "
          "key",
          "[dsmod][host_view_values]") {
    ViewState states;
    states["m"] = ViewTransform{};
    for (const char* key : {"", "zoom", "@flag:foo", "@map_view_cx:m", "@view_something:m"}) {
        REQUIRE_FALSE(PanZoomViewHostValueFrom(key, states).has_value());
    }
    // A bare prefix with nothing after the colon.
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_zoom:", states).has_value());
}

TEST_CASE("DSMod host view values: PanZoomViewHostValueFrom answers a tracked view's id",
          "[dsmod][host_view_values]") {
    ViewState states;
    ViewTransform v;
    v.zoom = 2.5f;
    v.pan_x = 10.0f;
    v.pan_y = -5.0f;
    states["m"] = v;
    REQUIRE(PanZoomViewHostValueFrom("@view_zoom:m", states) == Catch::Approx(2.5));
    REQUIRE(PanZoomViewHostValueFrom("@view_pan_x:m", states) == Catch::Approx(10.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_pan_y:m", states) == Catch::Approx(-5.0));
    // A different, untracked id: nullopt, even though "m" is tracked.
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_zoom:other", states).has_value());
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_pan_x:other", states).has_value());
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_pan_y:other", states).has_value());
}

TEST_CASE("DSMod host view values: PanZoomViewHostValueFrom answers the pan as drawn, after a "
          "ClampViewToBounds",
          "[dsmod][host_view_values][pan_bounds]") {
    // Same fixture as pan_bounds.cpp's own ClampViewToBounds case: a 1000x800 pan_zoom widget
    // bounded to u 0.25..1.0 / v 0.5..1.0, zoom 2 (a 500x400 window) -- known to clamp (0, 0) to
    // (250, 400).
    Widget bounded;
    bounded.type = WidgetType::Image;
    bounded.id = "pic";
    bounded.rect = {0, 0, 1000, 800};
    bounded.pan_bounds = PanBounds{0.25f, 0.5f, 1.0f, 1.0f};
    ViewTransform v;
    v.zoom = 2.0f;
    v.pan_x = 0.0f;
    v.pan_y = 0.0f;
    ClampViewToBounds(bounded, v);
    REQUIRE(v.pan_x == Catch::Approx(250.0f));
    REQUIRE(v.pan_y == Catch::Approx(400.0f));
    ViewState states;
    states["pic"] = v;
    REQUIRE(PanZoomViewHostValueFrom("@view_zoom:pic", states) == Catch::Approx(2.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_pan_x:pic", states) == Catch::Approx(250.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_pan_y:pic", states) == Catch::Approx(400.0));
}

TEST_CASE("DSMod host view values: the get_i64 fall-through rounds a map view value to the "
          "nearest int",
          "[dsmod][host_view_values]") {
    MapDrawRecord rec = Record("a");
    rec.cx = 12.7f;
    rec.cy = -12.7f;
    const MapDrawRecords records{rec};
    const auto cx = MapViewHostValueFrom("@map_view_cx:a", records, 0, 0);
    const auto cy = MapViewHostValueFrom("@map_view_cy:a", records, 0, 0);
    REQUIRE(cx.has_value());
    REQUIRE(cy.has_value());
    // mod_module_host.cpp's get_i64 fall-through: static_cast<s64>(std::llround(*view)).
    REQUIRE(static_cast<s64>(std::llround(*cx)) == 13);
    REQUIRE(static_cast<s64>(std::llround(*cy)) == -13);
}

TEST_CASE("DSMod host view values: the get_f64 fall-through's two checks never both answer the "
          "same key",
          "[dsmod][host_view_values]") {
    const MapDrawRecords records{Record("a")};
    ViewState states;
    states["a"] = ViewTransform{};
    // mod_module_host.cpp's get_f64 tries MapViewHostValue first, then PanZoomViewHostValue: a
    // "@map_view_*" key is never a pan_zoom key and vice versa, so which runs first never matters.
    for (const char* key : {"@map_view_cx:a", "@map_view_rot:a"}) {
        REQUIRE(MapViewHostValueFrom(key, records, 0, 0).has_value());
        REQUIRE_FALSE(PanZoomViewHostValueFrom(key, states).has_value());
    }
    for (const char* key : {"@view_zoom:a", "@view_pan_x:a", "@view_pan_y:a"}) {
        REQUIRE_FALSE(MapViewHostValueFrom(key, records, 0, 0).has_value());
        REQUIRE(PanZoomViewHostValueFrom(key, states).has_value());
    }
}

TEST_CASE("MapViewHostValueFrom takes the last record of an area, like PublishMapState",
          "[dsmod][host_view_values]") {
    MapDrawRecords records;
    MapDrawRecord first;
    first.area = "twice";
    first.cx = 1.0f;
    first.rotation = 10.0f;
    MapDrawRecord second;
    second.area = "twice";
    second.cx = 2.0f;
    second.rotation = 20.0f;
    records.push_back(first);
    records.push_back(second);
    REQUIRE(MapViewHostValueFrom("@map_view_cx:twice", records, 0, 0) == 2.0);
    REQUIRE(MapViewHostValueFrom("@map_view_rot:twice", records, 0, 0) == 20.0);
}

TEST_CASE("DSMod host flag values: FlagHostValueFrom answers a settings flag's live value",
          "[dsmod][flags]") {
    // The module's get_i64("@flag:<name>"): PublishFlags runs after the module each tick, so
    // the answer comes from the live flag table, not the tick's snapshot.
    std::unordered_map<std::string, s64> flags{{"smo.hide_top_hud", 1},
                                               {"smo.show_uncollected_moons", 0}};
    REQUIRE(FlagHostValueFrom("@flag:smo.hide_top_hud", flags) == 1);
    REQUIRE(FlagHostValueFrom("@flag:smo.show_uncollected_moons", flags) == 0);
    flags["smo.show_uncollected_moons"] = 1; // the settings toggle (mod_actions.cpp)
    REQUIRE(FlagHostValueFrom("@flag:smo.show_uncollected_moons", flags) == 1);
}

TEST_CASE("DSMod host flag values: FlagHostValueFrom answers nullopt for anything else",
          "[dsmod][flags]") {
    const std::unordered_map<std::string, s64> flags{{"lp_anim", 1}};
    REQUIRE_FALSE(FlagHostValueFrom("@flag:missing", flags).has_value());
    REQUIRE_FALSE(FlagHostValueFrom("@flag:", flags).has_value());
    REQUIRE_FALSE(FlagHostValueFrom("lp_anim", flags).has_value());
    REQUIRE_FALSE(FlagHostValueFrom("@view_zoom:lp_anim", flags).has_value());
}
