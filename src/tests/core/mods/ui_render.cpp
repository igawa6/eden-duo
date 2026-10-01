// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Canvas and RenderPage edge cases: image blits at the edge of their source, and the renderer's
// widget skips (dirty-rect pre-reject, occlusion under an opaque map).
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
u32 PixelAt(const Canvas& canvas, u32 x, u32 y) {
    return canvas.Pixels()[y * canvas.Width() + x];
}

Image Solid(u32 w, u32 h, u32 argb) {
    Image image;
    image.w = w;
    image.h = h;
    image.pixels.assign(static_cast<size_t>(w) * h, argb);
    return image;
}
} // namespace

namespace {
constexpr u32 MapBg = 0xFF102030u;
constexpr u32 Ink = 0xFFFFFFFFu;

/// A page whose last widget is an opaque Map (occludes what lies under it) at {0,0,40,20}.
Widget OpaqueMap() {
    Widget map;
    map.type = WidgetType::Map;
    map.rect = {0, 0, 40, 20};
    map.bg = MapBg;
    map.color = MapBg; // its "unknown area" note draws invisibly
    map.text_scale = 1;
    return map;
}
} // namespace

TEST_CASE("DSMod occlusion skip keeps text painting past the map", "[dsmod][ui]") {
    Canvas canvas;
    canvas.Resize(100, 20);
    Page page;
    Widget label;
    label.type = WidgetType::Label;
    label.rect = {30, 2, 0, 0}; // unsized: anchored inside the map, its text runs past it
    label.text = "88888";      // 5 glyphs x 4 px at scale 1: x 30..49
    label.text_scale = 1;
    label.color = Ink;
    page.widgets.push_back(label);
    page.widgets.push_back(OpaqueMap());
    Manifest manifest;
    manifest.background = 0xFF000000u;
    REQUIRE(RenderPage(canvas, manifest, page, StateSnapshot{}));
    // Under the map: the map's own fill. Past it: the label's glyphs ('8' row 0 = XXX).
    REQUIRE(PixelAt(canvas, 30, 2) == MapBg);
    REQUIRE(PixelAt(canvas, 40, 2) == Ink);
    REQUIRE(PixelAt(canvas, 42, 2) == Ink);
}

TEST_CASE("DSMod occlusion skip keeps a selected widget's highlight", "[dsmod][ui]") {
    Canvas canvas;
    canvas.Resize(100, 20);
    Page page;
    Widget cell;
    cell.type = WidgetType::Rect;
    cell.rect = {4, 4, 10, 10};
    cell.bg = 0xFF00FF00u;
    cell.color = 0;
    cell.select_group = "g";
    cell.payload = "7";
    cell.highlight_color = 0xFFFF0000u;
    cell.frame = 0; // fill
    page.widgets.push_back(cell);
    page.widgets.push_back(OpaqueMap());
    StateSnapshot snapshot;
    snapshot.ints["@sel:g"] = 7;
    REQUIRE(RenderPage(canvas, Manifest{}, page, snapshot));
    // The cell itself is under the opaque map; its highlight is painted after every widget.
    REQUIRE(PixelAt(canvas, 8, 8) == 0xFFFF0000u);
    // Not selected: the map covers the cell.
    snapshot.ints["@sel:g"] = 3;
    REQUIRE(RenderPage(canvas, Manifest{}, page, snapshot));
    REQUIRE(PixelAt(canvas, 8, 8) == MapBg);
}

TEST_CASE("DSMod text layout cache follows the font's content", "[dsmod][ui]") {
    FontMetrics font;
    font.line_height = 10;
    font.glyphs.assign(96, FontGlyph{0, 0, 1, 1, 0, 1, 10});
    const Image atlas = Solid(4, 4, 0xFFFFFFFFu);
    Canvas canvas;
    canvas.SetFont(&atlas, &font);
    // Scale 2 = 10 px cap = the font's line height: advances are px. 30 + 10 + 30 fits 70.
    REQUIRE(canvas.LayoutText("AAA BBB", 2, 70, 0).size() == 1);
    // The same FontMetrics object reassigned in place (as the runtime's own font is), wider.
    for (auto& g : font.glyphs) {
        g.advance = 20;
    }
    canvas.SetFont(&atlas, &font);
    REQUIRE(canvas.LayoutText("AAA BBB", 2, 70, 0).size() == 2);
    // Many labels cycling: the cache keeps them all (it used to hold 64, first in first out).
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 200; ++i) {
            REQUIRE(canvas.LayoutText("L" + std::to_string(i) + " X", 2, 400, 0).size() == 1);
        }
    }
    // A copied canvas lays out on its own.
    const Canvas copy = canvas;
    REQUIRE(copy.LayoutText("AAA BBB", 2, 70, 0).size() == 2);
}

TEST_CASE("DSMod repeat substitutes {i} in lists, suffix, table and text_map", "[dsmod][ui]") {
    Page page;
    Widget row;
    row.type = WidgetType::Value;
    row.repeat = 2;
    row.src_names = {"pic_{i}", "plain"};
    row.empty_src = "empty_{i+1}";
    row.suffix = "/{i}";
    row.max_sep = "{i}";
    row.table = "names_{i}";
    row.text_map = std::make_shared<const std::unordered_map<s64, std::string>>(
        std::unordered_map<s64, std::string>{{0, "msbt:a#row_{i}"}, {1, "literal"}});
    page.widgets.push_back(row);
    const auto widgets = ExpandWidgets(page, StateSnapshot{});
    REQUIRE(widgets.size() == 2);
    REQUIRE(widgets[1].src_names == std::vector<std::string>{"pic_1", "plain"});
    REQUIRE(widgets[1].empty_src == "empty_2");
    REQUIRE(widgets[1].suffix == "/1");
    REQUIRE(widgets[1].max_sep == "1");
    REQUIRE(widgets[1].table == "names_1");
    REQUIRE(widgets[0].text_map->at(0) == "msbt:a#row_0");
    REQUIRE(widgets[1].text_map->at(0) == "msbt:a#row_1");
    REQUIRE(widgets[1].text_map->at(1) == "literal");
    // A text_map without "{i}" stays shared with the template.
    page.widgets[0].text_map = std::make_shared<const std::unordered_map<s64, std::string>>(
        std::unordered_map<s64, std::string>{{0, "same"}});
    const auto shared = ExpandWidgets(page, StateSnapshot{});
    REQUIRE(shared[1].text_map == page.widgets[0].text_map);
}

TEST_CASE("DSMod DrawImageFilled samples nothing outside its image", "[dsmod][ui]") {
    Canvas canvas;
    canvas.Resize(16, 16);
    canvas.Clear(0xFF000000u);
    const Image image = Solid(4, 4, 0xFFFF0000u);
    // A source rect reaching past the image (a src_rect beyond 0..1): the part inside is drawn,
    // the rest is left alone instead of read out of bounds.
    canvas.DrawImageFilled(0, 0, 8, 8, image, 0xFFFFFFFFu, 1.0f, 2, 2, 4, 4);
    REQUIRE(PixelAt(canvas, 0, 0) == 0xFFFF0000u);
    REQUIRE(PixelAt(canvas, 3, 3) == 0xFFFF0000u);
    REQUIRE(PixelAt(canvas, 4, 4) == 0xFF000000u);
    REQUIRE(PixelAt(canvas, 7, 0) == 0xFF000000u);
    // A negative origin skips the rows / columns before the image.
    canvas.Clear(0xFF000000u);
    canvas.DrawImageFilled(0, 0, 8, 8, image, 0xFFFFFFFFu, 1.0f, -4, -4, 8, 8);
    REQUIRE(PixelAt(canvas, 0, 0) == 0xFF000000u);
    REQUIRE(PixelAt(canvas, 4, 4) == 0xFFFF0000u);
    // Inside the image nothing changes: the bottom half of a 4x4 sprite scaled to 8x8.
    canvas.Clear(0xFF000000u);
    canvas.DrawImageFilled(0, 0, 8, 8, image, 0xFFFFFFFFu, 0.5f);
    REQUIRE(PixelAt(canvas, 0, 3) == 0xFF000000u);
    REQUIRE(PixelAt(canvas, 0, 4) == 0xFFFF0000u);
    REQUIRE(PixelAt(canvas, 7, 7) == 0xFFFF0000u);
}

TEST_CASE("DSMod grow transition shadows only the band around the rect", "[dsmod][ui]") {
    constexpr u32 W = 200, H = 120;
    const std::vector<u32> background(W * H, 0xFF808080u);
    const std::vector<u32> moving(W * H, 0xFF102030u);
    std::vector<u32> out(W * H);
    const std::array<s32, 4> dest{50, 40, 60, 30};
    ComposePageGrow(out, background, moving, W, H, dest, dest, 1.0f, 0.5f);
    const auto expect = [](s32 d) {
        const float t = 1.0f - static_cast<float>(d) / 28.0f;
        const u32 keep = static_cast<u32>(std::lround(255.0f * (1.0f - 0.5f * t * t)));
        const u32 c = (0x80u * keep) / 255;
        return 0xFF000000u | (c << 16) | (c << 8) | c;
    };
    const auto at = [&](s32 x, s32 y) { return out[static_cast<size_t>(y) * W + x]; };
    REQUIRE(at(60, 50) == 0xFF102030u);            // inside: the moving page
    REQUIRE(at(49, 50) == expect(1));              // left of the rect, 1 px out
    REQUIRE(at(110, 69) == expect(1));             // right edge
    REQUIRE(at(60, 30) == expect(10));             // above, 10 px out
    REQUIRE(at(45, 76) == expect(7));              // a corner: the larger distance
    REQUIRE(at(10, 10) == 0xFF808080u);            // beyond the band: untouched
}

TEST_CASE("DSMod widget spacing props default to the fixed values", "[dsmod][ui]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p",
        "widgets":[{"type":"button","rect":[0,0,40,30],"text":"8"},
                   {"type":"button","rect":[0,0,40,30],"text":"8","border":1,"text_inset":4},
                   {"type":"pips","rect":[0,0,4,4],"gap":2},
                   {"type":"map","rect":[0,0,4,4],"label_offset":[3,9]}]}]})"),
                                    m));
    const auto& w = m.pages[0].widgets;
    REQUIRE(w[0].border == 3);
    REQUIRE(w[0].text_inset == 12);
    REQUIRE(w[0].gap == -1);
    REQUIRE(w[0].label_offset == std::array<s32, 2>{12, 24});
    REQUIRE(w[1].border == 1);
    REQUIRE(w[1].text_inset == 4);
    REQUIRE(w[2].gap == 2);
    REQUIRE(w[3].label_offset == std::array<s32, 2>{3, 9});

    Canvas canvas;
    canvas.Resize(40, 30);
    Page page;
    page.widgets = {w[0]};
    page.widgets[0].bg = 0xFF000000u;
    page.widgets[0].color = Ink;
    page.widgets[0].text_scale = 1;
    REQUIRE(RenderPage(canvas, Manifest{}, page, StateSnapshot{}));
    REQUIRE(PixelAt(canvas, 2, 15) == Ink);            // a 3 px frame by default
    REQUIRE(PixelAt(canvas, 3, 15) == 0xFF000000u);
    REQUIRE(PixelAt(canvas, 12, 12) == Ink);           // '8' top-left at the 12 px inset
    page.widgets = {w[1]};
    page.widgets[0].bg = 0xFF000000u;
    page.widgets[0].color = Ink;
    page.widgets[0].text_scale = 1;
    REQUIRE(RenderPage(canvas, Manifest{}, page, StateSnapshot{}));
    REQUIRE(PixelAt(canvas, 0, 15) == Ink);            // border 1
    REQUIRE(PixelAt(canvas, 1, 15) == 0xFF000000u);
    REQUIRE(PixelAt(canvas, 4, 4) == Ink);             // inset 4
    REQUIRE(PixelAt(canvas, 12, 12) == 0xFF000000u);
}

TEST_CASE("DSMod map.style marker conventions default to Dread's", "[dsmod][ui]") {
    Manifest defaults;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(R"({"format":1,"pages":[],
        "map":{"areas":{},"style":{}}})"),
                                    defaults));
    const MapStyle& d = defaults.map_style;
    REQUIRE(d.door_prefix == "Door");
    REQUIRE(d.structural_prefixes == std::vector<std::string>{"Door", "Blockage"});
    REQUIRE(d.collected_suffix == "Adquired");
    REQUIRE(d.collected_fallback == "ItemAdquired");
    REQUIRE(d.collectible_kind == "Items");
    REQUIRE(d.item_blink_period == 72);
    REQUIRE(d.player_blink_period == 48);
    REQUIRE(d.PinReach() == 22);

    Manifest custom;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(R"({"format":1,"pages":[],
        "map":{"areas":{},"style":{"door_prefix":"Gate","structural_prefixes":["Gate","Wall"],
        "door_closed_suffix":"Shut","door_open_suffix":"Ajar","collected_suffix":"_got",
        "collected_fallback":"Got","collectible_kind":"Loot","item_blink_period":30,
        "item_blink_low":0.2,"player_blink_period":1,"pin_outer":10,"pin_inner":6,
        "pin_core":30}}})"),
                                    custom));
    const MapStyle& c = custom.map_style;
    REQUIRE(c.door_prefix == "Gate");
    REQUIRE(c.structural_prefixes == std::vector<std::string>{"Gate", "Wall"});
    REQUIRE(c.door_closed_suffix == "Shut");
    REQUIRE(c.door_open_suffix == "Ajar");
    REQUIRE(c.collected_suffix == "_got");
    REQUIRE(c.collected_fallback == "Got");
    REQUIRE(c.collectible_kind == "Loot");
    REQUIRE(c.item_blink_period == 30);
    REQUIRE(c.player_blink_period == 2); // clamped: a ping-pong needs two ticks
    REQUIRE(c.PinReach() == 15);         // the core square reaches past both diamonds
}
