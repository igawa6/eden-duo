// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {

/// Tests own their images; the provider hands out non-owning references to them.
std::shared_ptr<const Image> Borrow(const Image* image) {
    return {std::shared_ptr<const Image>{}, image};
}

Manifest ImageMapManifest(u32 background) {
    Manifest manifest;
    manifest.background = background;
    manifest.icon_atlas = "atlas";
    manifest.icon_cell = 1;
    manifest.icon_cells.emplace("PropWarlotus", std::pair{0, 0});
    manifest.icon_cells.emplace("SaveStation", std::pair{0, 1});
    manifest.map_style.item_icon = 2;

    MapArea area;
    area.image = "map-image";
    area.min_x = 0.0f;
    area.min_y = 0.0f;
    area.max_x = 10.0f;
    area.max_y = 10.0f;
    manifest.map_areas.emplace("test", std::move(area));
    return manifest;
}

Widget ImageMapWidget(u32 background) {
    Widget map;
    map.type = WidgetType::Map;
    map.rect = {2, 3, 6, 4};
    map.area = "test";
    map.bg = background;
    return map;
}

} // namespace

TEST_CASE("DSMod moving markers draw and hit-test at the same position", "[dsmod][ui]") {
    Page page;
    Widget marker;
    marker.rect = {10, 20, 30, 30};
    marker.x_bind = "racer.{i}.x";
    marker.y_bind = "racer.{i}.y";
    marker.x_scale = 200;
    marker.y_scale = 100;
    marker.repeat = 2;
    marker.need_bind = "racer.{i}.active";
    marker.on_tap = "select.{i}";
    page.widgets.push_back(marker);
    StateSnapshot state;
    state.floats["racer.0.x"] = 0.5f;
    state.floats["racer.0.y"] = 0.25f;
    state.ints["racer.0.active"] = 1;
    state.ints["racer.1.active"] = 0;
    const auto widgets = ExpandWidgets(page, state);
    REQUIRE(widgets.size() == 2);
    REQUIRE(widgets[0].rect[0] == 110);
    REQUIRE(widgets[0].rect[1] == 45);
    REQUIRE(HitTest(page, state, 115, 50) == "select.0");
    REQUIRE(HitTest(page, state, 15, 25).empty());
}

TEST_CASE("DSMod position bindings tolerate invalid and extreme coordinates", "[dsmod][ui]") {
    Page page;
    Widget marker;
    marker.rect = {10, 20, 30, 30};
    marker.x_bind = "x";
    marker.y_bind = "y";
    page.widgets.push_back(marker);
    StateSnapshot state;
    state.floats["x"] = std::numeric_limits<float>::quiet_NaN();
    state.floats["y"] = 1.0e30f;
    const auto widgets = ExpandWidgets(page, state);
    REQUIRE(widgets[0].rect[0] == 10);
    REQUIRE(widgets[0].rect[1] == 32768);
}

TEST_CASE("DSMod image bindings resolve module keys without an extracted sprite table",
          "[dsmod][ui]") {
    Canvas canvas;
    canvas.Resize(8, 8);
    Page page;
    Widget widget;
    widget.type = WidgetType::Image;
    widget.rect = {0, 0, 8, 8};
    widget.src_bind = "map";
    widget.color = 0xFFFFFFFF;
    page.widgets.push_back(widget);
    StateSnapshot state;
    state.texts["map"] = "module:map/test";
    Image image{1, 1, {0xFFFFFFFF}};
    std::string requested;
    REQUIRE(RenderPage(canvas, Manifest{}, page, state, [&](const std::string& key) {
        requested = key;
        return Borrow(&image);
    }));
    REQUIRE(requested == "module:map/test");
    REQUIRE(canvas.Pixels()[0] == 0xFFFFFFFF);
}

TEST_CASE("DSMod map background is bounded to its widget on CPU and GPU", "[dsmod][ui]") {
    constexpr u32 PageBackground = 0xFF24180Cu;
    constexpr u32 MapBackground = 0xFF123A68u;
    Manifest manifest = ImageMapManifest(PageBackground);
    Page page;
    page.widgets.push_back(ImageMapWidget(MapBackground));
    StateSnapshot state;
    const Image transparent_map{1, 1, {0x00000000u}};
    const Image atlas{2, 1, {0xFFFFFFFFu, 0xFFFFFFFFu}};
    const auto images = [&](const std::string& key) -> std::shared_ptr<const Image> {
        return Borrow(key == "map-image" ? &transparent_map : key == "atlas" ? &atlas : nullptr);
    };

    Canvas cpu;
    cpu.Resize(12, 10);
    REQUIRE(RenderPage(cpu, manifest, page, state, images));
    REQUIRE(cpu.Pixels()[0] == PageBackground);
    REQUIRE(cpu.Pixels()[static_cast<size_t>(3) * 12 + 2] == MapBackground);
    REQUIRE(cpu.Pixels()[static_cast<size_t>(6) * 12 + 7] == MapBackground);
    REQUIRE(cpu.Pixels()[static_cast<size_t>(3) * 12 + 8] == PageBackground);

    Canvas gpu;
    gpu.Resize(12, 10);
    AuxDrawList draw_list;
    REQUIRE(RenderPage(gpu, manifest, page, state, images, {}, nullptr, {}, {}, &draw_list));
    REQUIRE(draw_list.bg == PageBackground);
    const auto solid = std::ranges::find_if(draw_list.quads, [](const auto& q) { return q.solid; });
    REQUIRE(solid != draw_list.quads.end());
    REQUIRE(solid->x == 2.0f);
    REQUIRE(solid->y == 3.0f);
    REQUIRE(solid->w == 6.0f);
    REQUIRE(solid->h == 4.0f);
    REQUIRE(solid->color == MapBackground);
}

TEST_CASE("DSMod map opacity applies to the bounded CPU and GPU composition", "[dsmod][ui]") {
    constexpr u32 PageBackground = 0xFF201008u;
    constexpr u32 MapBackground = 0xFF604020u;
    Manifest manifest = ImageMapManifest(PageBackground);
    manifest.map_style.opacity = 0.5f;
    manifest.map_areas.at("test").markers.push_back(
        {.kind = "Props", .icon = "SaveStation", .x = 5.0f, .y = 5.0f});
    Page page;
    Widget map = ImageMapWidget(MapBackground);
    map.rect = {1, 1, 10, 10};
    page.widgets.push_back(map);
    StateSnapshot state;
    const Image map_image{1, 1, {0xFF804020u}};
    const Image atlas{2, 1, {0xFFFFFFFFu, 0xFF00FF00u}};
    const auto images = [&](const std::string& key) -> std::shared_ptr<const Image> {
        return Borrow(key == "map-image" ? &map_image : key == "atlas" ? &atlas : nullptr);
    };
    const auto draw_half = [](u32 below, u32 above) {
        u32 result = 0xFF000000u;
        for (u32 shift = 0; shift < 24; shift += 8) {
            result |= ((((below >> shift) & 0xFF) * 127 + ((above >> shift) & 0xFF) * 128) / 255)
                      << shift;
        }
        return result;
    };

    Canvas cpu;
    cpu.Resize(12, 12);
    REQUIRE(RenderPage(cpu, manifest, page, state, images));
    REQUIRE(cpu.Pixels()[0] == PageBackground);
    REQUIRE(cpu.Pixels()[static_cast<size_t>(2) * 12 + 2] ==
            draw_half(draw_half(PageBackground, MapBackground), 0xFF804020u));
    // The icon overlaps a nontransparent map texel. Both layers retain their own opacity, matching
    // the GPU's ordered background -> map -> atlas quad composition.
    REQUIRE(
        cpu.Pixels()[static_cast<size_t>(5) * 12 + 5] ==
        draw_half(draw_half(draw_half(PageBackground, MapBackground), 0xFF804020u), 0xFF00FF00u));

    Canvas gpu;
    gpu.Resize(12, 12);
    AuxDrawList draw_list;
    REQUIRE(RenderPage(gpu, manifest, page, state, images, {}, nullptr, {}, {}, &draw_list));
    REQUIRE(draw_list.bg == PageBackground);
    const auto solid = std::ranges::find_if(draw_list.quads, [](const auto& q) { return q.solid; });
    REQUIRE(solid != draw_list.quads.end());
    REQUIRE(solid->color == draw_half(PageBackground, MapBackground));
    const auto map_quad = std::ranges::find_if(
        draw_list.quads, [](const auto& q) { return !q.solid && q.slot == 0; });
    REQUIRE(map_quad != draw_list.quads.end());
    REQUIRE(map_quad->a == 0.5f);
    const auto icon_quad = std::ranges::find_if(
        draw_list.quads, [](const auto& q) { return !q.solid && q.slot == 1; });
    REQUIRE(icon_quad != draw_list.quads.end());
    REQUIRE(icon_quad->a == 0.5f);
    REQUIRE(cpu.Pixels()[static_cast<size_t>(5) * 12 + 5] ==
            draw_half(draw_half(solid->color, map_image.pixels[0]), atlas.pixels[1]));
}

TEST_CASE("DSMod zero map opacity does not affect following HUD widgets", "[dsmod][ui]") {
    constexpr u32 PageBackground = 0xFF201008u;
    constexpr u32 HudColor = 0xFFFF0080u;
    Manifest manifest = ImageMapManifest(PageBackground);
    manifest.map_style.opacity = 0.0f;
    Page page;
    Widget map = ImageMapWidget(0xFF604020u);
    map.rect = {1, 1, 10, 10};
    page.widgets.push_back(map);
    Widget hud;
    hud.type = WidgetType::Rect;
    hud.rect = {0, 0, 1, 1};
    hud.color = HudColor;
    page.widgets.push_back(hud);
    StateSnapshot state;
    const Image map_image{1, 1, {0xFF804020u}};
    const auto images = [&](const std::string& key) -> std::shared_ptr<const Image> {
        return Borrow(key == "map-image" ? &map_image : nullptr);
    };

    Canvas cpu;
    cpu.Resize(12, 12);
    REQUIRE(RenderPage(cpu, manifest, page, state, images));
    REQUIRE(cpu.Pixels()[0] == HudColor);
    REQUIRE(cpu.Pixels()[static_cast<size_t>(5) * 12 + 5] == PageBackground);

    Canvas gpu;
    gpu.Resize(12, 12);
    AuxDrawList draw_list;
    REQUIRE(RenderPage(gpu, manifest, page, state, images, {}, nullptr, {}, {}, &draw_list));
    REQUIRE(gpu.Pixels()[0] == HudColor);
    REQUIRE(gpu.Pixels()[static_cast<size_t>(5) * 12 + 5] == 0x00000000u);
    REQUIRE(
        std::ranges::all_of(draw_list.quads, [](const auto& q) { return q.solid || q.a == 0.0f; }));
}

TEST_CASE("DSMod hidden map icons are omitted without mutating marker data", "[dsmod][ui]") {
    Manifest manifest = ImageMapManifest(0xFF101010u);
    auto& markers = manifest.map_areas.at("test").markers;
    markers.push_back({.kind = "Props", .icon = "PropWarlotus", .x = 3.0f, .y = 5.0f});
    markers.push_back({.kind = "Props", .icon = "SaveStation", .x = 7.0f, .y = 5.0f});
    const auto original = markers;

    Page page;
    Widget map = ImageMapWidget(0xFF202020u);
    map.rect = {0, 0, 20, 10};
    map.hidden_icons.push_back("PropWarlotus");
    page.widgets.push_back(map);
    StateSnapshot state;
    const Image map_image{1, 1, {0xFF303030u}};
    const Image atlas{2, 1, {0xFFFF0000u, 0xFF00FF00u}};
    const auto images = [&](const std::string& key) -> std::shared_ptr<const Image> {
        return Borrow(key == "map-image" ? &map_image : key == "atlas" ? &atlas : nullptr);
    };

    Canvas cpu;
    cpu.Resize(20, 10);
    REQUIRE(RenderPage(cpu, manifest, page, state, images));
    REQUIRE(std::ranges::find(cpu.Pixels(), 0xFFFF0000u) == cpu.Pixels().end());
    REQUIRE(std::ranges::find(cpu.Pixels(), 0xFF00FF00u) != cpu.Pixels().end());

    Canvas gpu;
    gpu.Resize(20, 10);
    AuxDrawList draw_list;
    REQUIRE(RenderPage(gpu, manifest, page, state, images, {}, nullptr, {}, {}, &draw_list));
    const auto atlas_quads = std::ranges::count_if(
        draw_list.quads, [](const auto& q) { return !q.solid && q.slot == 1; });
    REQUIRE(atlas_quads == 1);
    const auto visible_icon = std::ranges::find_if(
        draw_list.quads, [](const auto& q) { return !q.solid && q.slot == 1; });
    REQUIRE(visible_icon != draw_list.quads.end());
    REQUIRE(visible_icon->u0 == 0.5f);
    REQUIRE(visible_icon->u1 == 1.0f);

    REQUIRE(markers.size() == original.size());
    for (size_t i = 0; i < markers.size(); ++i) {
        REQUIRE(markers[i].kind == original[i].kind);
        REQUIRE(markers[i].icon == original[i].icon);
        REQUIRE(markers[i].x == original[i].x);
        REQUIRE(markers[i].y == original[i].y);
        REQUIRE(markers[i].hidden == original[i].hidden);
    }
}

TEST_CASE("DSMod live actor marker requires its own revealed cell on CPU and GPU", "[dsmod][ui]") {
    Manifest manifest = ImageMapManifest(0xFF101010u);
    manifest.icon_cells.emplace("Samus", std::pair{0, 0});
    manifest.icon_cells.emplace("Emmy", std::pair{0, 1});
    manifest.map_style.marker_icon = 2;

    Page page;
    Widget map = ImageMapWidget(0xFF202020u);
    map.rect = {0, 0, 100, 100};
    map.marker_x_bind = "player_x";
    map.marker_y_bind = "player_y";
    map.marker_icon = "Samus";
    map.actor_x_bind = "actor_x";
    map.actor_y_bind = "actor_y";
    map.actor_icon = "Emmy";
    map.actor_reveal_required = true;
    map.marker_scale = 0.5f;
    page.widgets.push_back(map);

    StateSnapshot state;
    state.tick = 24; // peak of the Samus marker's 48-tick alpha pulse
    state.floats["player_x"] = 4.0f;
    state.floats["player_y"] = 4.0f;
    state.floats["actor_x"] = 16.0f;
    state.floats["actor_y"] = 16.0f;
    const Image map_image{1, 1, {0xFF303030u}};
    const Image atlas{2, 1, {0xFFFF0000u, 0xFF00FF00u}};
    const auto images = [&](const std::string& key) -> std::shared_ptr<const Image> {
        return Borrow(key == "map-image" ? &map_image : key == "atlas" ? &atlas : nullptr);
    };

    // Opt-in visibility fails closed without a query, while the player marker remains visible.
    Canvas no_query;
    no_query.Resize(100, 100);
    REQUIRE(RenderPage(no_query, manifest, page, state, images));
    REQUIRE(std::ranges::find(no_query.Pixels(), 0xFFFF0000u) != no_query.Pixels().end());
    REQUIRE(std::ranges::find(no_query.Pixels(), 0xFF00FF00u) == no_query.Pixels().end());

    const auto unexplored = [](const std::string& area, float x, float y) {
        REQUIRE(area == "test");
        REQUIRE(x == 8.0f);
        REQUIRE(y == 8.0f);
        return false;
    };
    Canvas cpu_hidden;
    cpu_hidden.Resize(100, 100);
    REQUIRE(RenderPage(cpu_hidden, manifest, page, state, images, {}, nullptr, {}, unexplored));
    REQUIRE(std::ranges::find(cpu_hidden.Pixels(), 0xFFFF0000u) != cpu_hidden.Pixels().end());
    REQUIRE(std::ranges::find(cpu_hidden.Pixels(), 0xFF00FF00u) == cpu_hidden.Pixels().end());

    const auto revealed = [](const std::string&, float x, float y) {
        return x == 8.0f && y == 8.0f;
    };
    Canvas cpu_visible;
    cpu_visible.Resize(100, 100);
    REQUIRE(RenderPage(cpu_visible, manifest, page, state, images, {}, nullptr, {}, revealed));
    REQUIRE(std::ranges::find(cpu_visible.Pixels(), 0xFF00FF00u) != cpu_visible.Pixels().end());

    Canvas gpu;
    gpu.Resize(100, 100);
    AuxDrawList hidden_list;
    REQUIRE(
        RenderPage(gpu, manifest, page, state, images, {}, nullptr, {}, unexplored, &hidden_list));
    REQUIRE(std::ranges::count_if(hidden_list.quads,
                                  [](const auto& q) { return !q.solid && q.slot == 1; }) == 1);
    AuxDrawList visible_list;
    REQUIRE(
        RenderPage(gpu, manifest, page, state, images, {}, nullptr, {}, revealed, &visible_list));
    REQUIRE(std::ranges::count_if(visible_list.quads,
                                  [](const auto& q) { return !q.solid && q.slot == 1; }) == 2);
}

TEST_CASE("DSMod follow map retains its transform through missing player samples", "[dsmod][ui]") {
    constexpr u32 Background = 0xFF101010u;
    Manifest manifest = ImageMapManifest(Background);
    manifest.map_areas.at("test").no_pin = true;
    Page page;
    page.id = "follow-page";
    Widget map = ImageMapWidget(0xFF202020u);
    map.id = "follow-map";
    map.rect = {0, 0, 100, 100};
    map.marker_x_bind = "player_x";
    map.marker_y_bind = "player_y";
    map.follow_window = 4.0f;
    map.pan_zoom = true;
    map.max_zoom = 4.0f;
    page.widgets.push_back(map);

    std::vector<u32> patterned(100);
    for (size_t i = 0; i < patterned.size(); ++i) {
        patterned[i] = 0xFF000000u | static_cast<u32>(i + 1);
    }
    const Image map_image{10, 10, patterned};
    const auto images = [&](const std::string& key) -> std::shared_ptr<const Image> {
        return Borrow(key == "map-image" ? &map_image : nullptr);
    };
    StateSnapshot valid;
    valid.floats["player_x"] = 2.0;
    valid.floats["player_y"] = 3.0;
    StateSnapshot missing;
    StateSnapshot nonfinite;
    nonfinite.floats["player_x"] = std::numeric_limits<double>::quiet_NaN();
    nonfinite.floats["player_y"] = 3.0;
    ViewState views;
    views["follow-map"].pan_x = 7.0f;
    views["follow-map"].pan_y = -4.0f;
    views["follow-map"].zoom = 1.5f;

    MapFollowState cpu_follow;
    Canvas cpu_valid;
    cpu_valid.Resize(100, 100);
    REQUIRE(RenderPage(cpu_valid, manifest, page, valid, images, views, &cpu_follow));
    Canvas cpu_missing;
    cpu_missing.Resize(100, 100);
    REQUIRE(RenderPage(cpu_missing, manifest, page, missing, images, views, &cpu_follow));
    Canvas cpu_nonfinite;
    cpu_nonfinite.Resize(100, 100);
    REQUIRE(RenderPage(cpu_nonfinite, manifest, page, nonfinite, images, views, &cpu_follow));
    REQUIRE(cpu_missing.Pixels() == cpu_valid.Pixels());
    REQUIRE(cpu_nonfinite.Pixels() == cpu_valid.Pixels());

    const auto map_quad = [](const AuxDrawList& list) {
        return std::ranges::find_if(list.quads,
                                    [](const auto& q) { return !q.solid && q.slot == 0; });
    };
    MapFollowState gpu_follow;
    Canvas gpu;
    gpu.Resize(100, 100);
    AuxDrawList valid_list;
    REQUIRE(
        RenderPage(gpu, manifest, page, valid, images, views, &gpu_follow, {}, {}, &valid_list));
    AuxDrawList missing_list;
    REQUIRE(RenderPage(gpu, manifest, page, missing, images, views, &gpu_follow, {}, {},
                       &missing_list));
    const auto valid_quad = map_quad(valid_list);
    const auto missing_quad = map_quad(missing_list);
    REQUIRE(valid_quad != valid_list.quads.end());
    REQUIRE(missing_quad != missing_list.quads.end());
    REQUIRE(missing_quad->u0 == valid_quad->u0);
    REQUIRE(missing_quad->v0 == valid_quad->v0);
    REQUIRE(missing_quad->u1 == valid_quad->u1);
    REQUIRE(missing_quad->v1 == valid_quad->v1);
    REQUIRE(missing_quad->x == valid_quad->x);
    REQUIRE(missing_quad->y == valid_quad->y);
    REQUIRE(missing_quad->w == valid_quad->w);
    REQUIRE(missing_quad->h == valid_quad->h);

    // A recovered sample resumes following from the retained centre once the user's view is home.
    AuxDrawList home_missing_list;
    REQUIRE(RenderPage(gpu, manifest, page, missing, images, {}, &gpu_follow, {}, {},
                       &home_missing_list));
    const auto home_missing_quad = map_quad(home_missing_list);
    REQUIRE(home_missing_quad != home_missing_list.quads.end());
    StateSnapshot recovered;
    recovered.floats["player_x"] = 8.0;
    recovered.floats["player_y"] = 8.0;
    AuxDrawList recovered_list;
    REQUIRE(RenderPage(gpu, manifest, page, recovered, images, {}, &gpu_follow, {}, {},
                       &recovered_list));
    const auto recovered_quad = map_quad(recovered_list);
    REQUIRE(recovered_quad != recovered_list.quads.end());
    REQUIRE((recovered_quad->u0 != home_missing_quad->u0 ||
             recovered_quad->v0 != home_missing_quad->v0));
}

TEST_CASE("DSMod follow geometry map waits for a position in each area", "[dsmod][ui]") {
    // A geometry map (no picture): before the first valid position in an area, a follow view
    // draws only the widget background, and another area's cached centre never leaks across.
    constexpr u32 PageBackground = 0xFF101010u;
    constexpr u32 MapBackground = 0xFF203040u;
    Manifest manifest = ImageMapManifest(PageBackground);
    manifest.map_areas.at("test").image.clear();
    MapArea second = manifest.map_areas.at("test");
    manifest.map_areas.emplace("test-2", std::move(second));
    Page page;
    page.id = "area-page";
    Widget map = ImageMapWidget(MapBackground);
    map.id = "area-map";
    map.rect = {0, 0, 20, 20};
    map.marker_x_bind = "player_x";
    map.marker_y_bind = "player_y";
    map.follow_window = 4.0f;
    page.widgets.push_back(map);
    const auto images = [](const std::string&) -> std::shared_ptr<const Image> { return {}; };
    const auto has_map_quad = [](const AuxDrawList& list) {
        return std::ranges::any_of(list.quads,
                                   [](const auto& q) { return !q.solid && q.slot == 0; });
    };

    StateSnapshot missing;
    MapFollowState follow_state;
    Canvas cpu;
    cpu.Resize(20, 20);
    REQUIRE(RenderPage(cpu, manifest, page, missing, images, {}, &follow_state));
    REQUIRE(std::ranges::all_of(cpu.Pixels(), [](u32 pixel) { return pixel == MapBackground; }));
    Canvas gpu;
    gpu.Resize(20, 20);
    AuxDrawList initial_list;
    REQUIRE(
        RenderPage(gpu, manifest, page, missing, images, {}, &follow_state, {}, {}, &initial_list));
    REQUIRE(initial_list.active);
    REQUIRE_FALSE(has_map_quad(initial_list));
    REQUIRE(std::ranges::count_if(initial_list.quads, [](const auto& q) { return q.solid; }) == 1);

    StateSnapshot valid;
    valid.floats["player_x"] = 2.0;
    valid.floats["player_y"] = 3.0;
    AuxDrawList valid_list;
    REQUIRE(RenderPage(gpu, manifest, page, valid, images, {}, &follow_state, {}, {}, &valid_list));

    // The first area now has a cached centre; the second has none and must wait again.
    page.widgets[0].area = "test-2";
    Canvas other;
    other.Resize(20, 20);
    REQUIRE(RenderPage(other, manifest, page, missing, images, {}, &follow_state));
    REQUIRE(std::ranges::all_of(other.Pixels(), [](u32 pixel) { return pixel == MapBackground; }));
    AuxDrawList other_area_list;
    REQUIRE(RenderPage(gpu, manifest, page, missing, images, {}, &follow_state, {}, {},
                       &other_area_list));
    REQUIRE_FALSE(has_map_quad(other_area_list));
}

TEST_CASE("DSMod follow picture map shows its centre before the first position", "[dsmod][ui]") {
    // A picture map (the game's own map art) has no blank waiting view: before the first
    // position it shows the area's centre at the follow zoom, in every area, and a map without
    // follow mode keeps the whole-area fit.
    constexpr u32 PageBackground = 0xFF101010u;
    constexpr u32 MapBackground = 0xFF203040u;
    constexpr u32 MapColour = 0xFF8090A0u;
    Manifest manifest = ImageMapManifest(PageBackground);
    MapArea second = manifest.map_areas.at("test");
    second.image = "map-image-2";
    manifest.map_areas.emplace("test-2", std::move(second));
    Page page;
    page.id = "area-page";
    Widget map = ImageMapWidget(MapBackground);
    map.id = "area-map";
    map.rect = {0, 0, 20, 20};
    map.marker_x_bind = "player_x";
    map.marker_y_bind = "player_y";
    map.follow_window = 4.0f;
    page.widgets.push_back(map);
    const Image map_image{1, 1, {MapColour}};
    const auto images = [&](const std::string& key) -> std::shared_ptr<const Image> {
        return Borrow(key == "map-image" || key == "map-image-2" ? &map_image : nullptr);
    };
    const auto has_map_quad = [](const AuxDrawList& list) {
        return std::ranges::any_of(list.quads,
                                   [](const auto& q) { return !q.solid && q.slot == 0; });
    };

    StateSnapshot missing;
    MapFollowState follow_state;
    Canvas cpu;
    cpu.Resize(20, 20);
    REQUIRE(RenderPage(cpu, manifest, page, missing, images, {}, &follow_state));
    REQUIRE(std::ranges::all_of(cpu.Pixels(), [](u32 pixel) { return pixel == MapColour; }));
    Canvas gpu;
    gpu.Resize(20, 20);
    AuxDrawList initial_list;
    REQUIRE(
        RenderPage(gpu, manifest, page, missing, images, {}, &follow_state, {}, {}, &initial_list));
    REQUIRE(has_map_quad(initial_list));

    StateSnapshot valid;
    valid.floats["player_x"] = 2.0;
    valid.floats["player_y"] = 3.0;
    AuxDrawList valid_list;
    REQUIRE(RenderPage(gpu, manifest, page, valid, images, {}, &follow_state, {}, {}, &valid_list));
    REQUIRE(has_map_quad(valid_list));

    page.widgets[0].area = "test-2";
    AuxDrawList other_area_list;
    REQUIRE(RenderPage(gpu, manifest, page, missing, images, {}, &follow_state, {}, {},
                       &other_area_list));
    REQUIRE(has_map_quad(other_area_list));

    page.widgets[0].follow_window = 0.0f;
    AuxDrawList fit_list;
    REQUIRE(RenderPage(gpu, manifest, page, missing, images, {}, &follow_state, {}, {}, &fit_list));
    REQUIRE(has_map_quad(fit_list));
}
