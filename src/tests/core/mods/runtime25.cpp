// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 25: pane geometry host values (@screen.*, mod_host_values.h), the optional
// "canvas_alt" manifest canvas and its per-widget "rect_alt" (SelectCanvasAspect,
// mod_redraw.cpp), a page scroll region's own "rect_alt" (ScrollRegion::rect_alt, swapped
// alongside the widgets' by the same ApplyCanvasVariant), and the L3/R3 virtual button tokens
// (ParseButton, mod_actions.cpp).

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/mod_host_values.h"
#include "core/mods/mod_runtime.h"
#include "input_common/drivers/virtual_gamepad.h"

namespace Core::Mods {
// ParseButton has external linkage (defined in mod_actions.cpp, declared for internal callers in
// the private mod_runtime_shared.h, which this file -- outside src/core/mods -- does not
// include); forward-declared here instead, same as any other externally linked function a test
// calls without its defining module's private header.
InputCommon::VirtualGamepad::VirtualButton ParseButton(const std::string& name, bool& ok);
} // namespace Core::Mods

using namespace Core::Mods;

namespace {
/// A controllable screen-pane source (mirrors runtime21.cpp's FakeHost for @device./@perf.).
struct FakeScreen {
    ScreenPane pane{};
    HostValueSource Source() {
        return {.screen = [this] { return pane; }};
    }
};
} // namespace

TEST_CASE("DSMod host values: @screen.pane_w/pane_h/orientation/mode", "[dsmod][runtime25][host_values]") {
    FakeScreen host;
    host.pane = {.width = 480, .height = 1080, .simulated = true};
    HostValues values;
    values.SetSource(host.Source());
    REQUIRE_FALSE(values.HasScreen()); // nothing before the first Update()
    values.Update();
    REQUIRE(values.HasScreen());
    StateSnapshot s;
    values.Publish(s);
    REQUIRE(s.ints.at("@screen.pane_w") == 480);
    REQUIRE(s.ints.at("@screen.pane_h") == 1080);
    REQUIRE(s.texts.at("@screen.orientation") == "portrait"); // h > w
    REQUIRE(s.texts.at("@screen.mode") == "simulated");
    REQUIRE(values.GetI64("@screen.pane_w") == 480);
    REQUIRE(values.GetI64("@screen.pane_h") == 1080);
    REQUIRE(values.GetF64("@screen.pane_w") == 480.0); // numeric keys read as floats too
    REQUIRE(values.GetText("@screen.orientation") == "portrait");
    REQUIRE(values.GetText("@screen.mode") == "simulated");
    REQUIRE_FALSE(values.GetI64("@screen.orientation").has_value()); // text-only key
    REQUIRE_FALSE(values.GetF64("@screen.orientation").has_value());

    // A square or wider-than-tall pane reads landscape (the w >= h boundary case included); not
    // simulated reads physical.
    host.pane = {.width = 1240, .height = 1080, .simulated = false};
    values.Update();
    values.Publish(s);
    REQUIRE(s.texts.at("@screen.orientation") == "landscape");
    REQUIRE(s.texts.at("@screen.mode") == "physical");
    host.pane = {.width = 1000, .height = 1000, .simulated = false};
    values.Update();
    REQUIRE(values.GetText("@screen.orientation") == "landscape"); // w == h -> landscape
}

TEST_CASE("DSMod host values: no screen source means no @screen.* at all",
          "[dsmod][runtime25][host_values]") {
    HostValues values;
    values.SetSource({}); // @device./@perf./@screen. all empty: battery/perf default, no screen
    values.Update();
    REQUIRE_FALSE(values.HasScreen());
    StateSnapshot s;
    values.Publish(s);
    REQUIRE_FALSE(s.ints.contains("@screen.pane_w"));
    REQUIRE_FALSE(s.texts.contains("@screen.orientation"));
    REQUIRE_FALSE(values.GetText("@screen.mode").has_value());
}

TEST_CASE("DSMod host values: @screen. is a host key like @device./@perf.",
          "[dsmod][runtime25][host_values]") {
    REQUIRE(IsHostValueKey("@screen.pane_w"));
    REQUIRE(IsHostValueKey("@screen.orientation"));
    REQUIRE_FALSE(IsHostValueKey("screen.pane_w"));
    REQUIRE_FALSE(IsHostValueKey("@sel:list"));
    REQUIRE(JsonReferencesHostKeys(
        nlohmann::json::parse(R"({"derived":{"tall":{"expr":"@screen.orientation"}}})")));
    REQUIRE(JsonReferencesHostKeys(nlohmann::json::parse(R"({"gates":{"@screen.mode":1}})")));
    REQUIRE_FALSE(JsonReferencesHostKeys(nlohmann::json::parse(R"({"a":"screen.pane_w"})")));
}

TEST_CASE("DSMod host values: only a package that names @screen. gets uses_host_keys",
          "[dsmod][runtime25][host_values]") {
    const auto page = nlohmann::json::parse(
        R"({"pages":[{"id":"p","widgets":[{"type":"value","rect":[0,0,10,10],"bind":"hp"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(page, m));
    REQUIRE_FALSE(m.uses_host_keys);
    auto bound = page;
    bound["pages"][0]["widgets"][0]["bind_text"] = "@screen.orientation";
    Manifest m2;
    REQUIRE(ParseDualScreenManifest(bound, m2));
    REQUIRE(m2.uses_host_keys);
}

TEST_CASE("DSMod manifest: canvas_alt parses w/h/when, rejects malformed",
          "[dsmod][runtime25][canvas_alt]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(
        nlohmann::json::parse(
            R"({"canvas_w":1240,"canvas_h":1080,"canvas_alt":{"w":720,"h":1280},"pages":[]})"),
        m));
    REQUIRE(m.HasCanvasAlt());
    REQUIRE(m.canvas_alt_w == 720);
    REQUIRE(m.canvas_alt_h == 1280);
    REQUIRE(m.canvas_alt_when == CanvasAltWhen::Portrait); // default

    Manifest narrow;
    REQUIRE(ParseDualScreenManifest(
        nlohmann::json::parse(R"({"canvas_alt":{"w":1600,"h":900,"when":"narrow"},"pages":[]})"),
        narrow));
    REQUIRE(narrow.canvas_alt_when == CanvasAltWhen::Narrow);

    // Malformed / zero / absent: no alternate, exactly as a runtime <= 24 package.
    for (const char* bad : {R"({"canvas_alt":{"w":0,"h":1280},"pages":[]})",
                            R"({"canvas_alt":{"w":720,"h":0},"pages":[]})",
                            R"({"canvas_alt":{"w":720},"pages":[]})",
                            R"({"canvas_alt":[1,2],"pages":[]})", R"({"pages":[]})"}) {
        Manifest none;
        REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(bad), none));
        REQUIRE_FALSE(none.HasCanvasAlt());
        REQUIRE(none.canvas_alt_w == 0);
        REQUIRE(none.canvas_alt_h == 0);
    }
}

TEST_CASE("DSMod manifest: widget rect_alt parses, defaults to unset",
          "[dsmod][runtime25][canvas_alt]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(
        nlohmann::json::parse(
            R"({"pages":[{"id":"p","widgets":[
                {"type":"rect","rect":[0,0,100,50],"rect_alt":[5,10,80,30]},
                {"type":"rect","rect":[0,0,100,50]}
            ]}]})"),
        m));
    REQUIRE(m.pages.size() == 1);
    REQUIRE(m.pages[0].widgets.size() == 2);
    REQUIRE(m.pages[0].widgets[0].rect_alt.has_value());
    REQUIRE(*m.pages[0].widgets[0].rect_alt == std::array<s32, 4>{5, 10, 80, 30});
    REQUIRE(m.pages[0].widgets[0].rect == std::array<s32, 4>{0, 0, 100, 50});
    REQUIRE_FALSE(m.pages[0].widgets[1].rect_alt.has_value()); // no rect_alt key: unset
}

TEST_CASE("DSMod canvas_alt: ApplyCanvasVariant's swap is self-inverse on the widget data model",
          "[dsmod][runtime25][canvas_alt]") {
    // The exact operation ModRuntime::ApplyCanvasVariant performs per widget (mod_redraw.cpp):
    // swapping rect <-> *rect_alt and back must restore the original values exactly.
    Widget w;
    w.rect = {0, 0, 1240, 1080};
    w.rect_alt = std::array<s32, 4>{40, 0, 400, 1080};
    const auto default_rect = w.rect;
    const auto alt_rect = *w.rect_alt;
    std::swap(w.rect, *w.rect_alt); // switch to canvas_alt
    REQUIRE(w.rect == alt_rect);
    REQUIRE(*w.rect_alt == default_rect);
    std::swap(w.rect, *w.rect_alt); // switch back
    REQUIRE(w.rect == default_rect);
    REQUIRE(*w.rect_alt == alt_rect);

    // A widget without rect_alt is never touched (ApplyCanvasVariant's `if (widget.rect_alt)`):
    // nothing to swap, so it keeps `rect` on every canvas.
    Widget plain;
    plain.rect = {1, 2, 3, 4};
    REQUIRE_FALSE(plain.rect_alt.has_value());
}

TEST_CASE("DSMod manifest: scroll region rect_alt parses, defaults to unset",
          "[dsmod][runtime25][canvas_alt]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(
        nlohmann::json::parse(
            R"({"pages":[{"id":"p","scrolls":[
                {"id":"a","rect":[0,20,200,40],"rect_alt":[0,0,40,200]},
                {"id":"b","rect":[0,0,10,10]}
            ],"widgets":[]}]})"),
        m));
    REQUIRE(m.pages.size() == 1);
    REQUIRE(m.pages[0].scrolls.size() == 2);
    const ScrollRegion* const a = FindScrollRegion(m.pages[0], "a");
    const ScrollRegion* const b = FindScrollRegion(m.pages[0], "b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(a->rect_alt.has_value());
    REQUIRE(*a->rect_alt == std::array<s32, 4>{0, 0, 40, 200});
    REQUIRE(a->rect == std::array<s32, 4>{0, 20, 200, 40}); // the base rect is unaffected
    REQUIRE_FALSE(b->rect_alt.has_value()); // no rect_alt key: unset, same as a runtime <= 24 region
}

TEST_CASE("DSMod manifest: scroll region rect_alt malformed is ignored",
          "[dsmod][runtime25][canvas_alt]") {
    // Same malformed handling as a widget's "rect_alt": anything but a 4-number array leaves the
    // field unset rather than rejecting the whole region.
    for (const char* bad : {
             R"({"id":"a","rect":[0,0,10,10],"rect_alt":[1,2,3]})",     // 3 numbers
             R"({"id":"a","rect":[0,0,10,10],"rect_alt":[1,2,3,4,5]})", // 5 numbers
             R"({"id":"a","rect":[0,0,10,10],"rect_alt":"nope"})",      // not an array
             R"({"id":"a","rect":[0,0,10,10],"rect_alt":42})",         // not an array
             R"({"id":"a","rect":[0,0,10,10],"rect_alt":null})",      // not an array
             R"({"id":"a","rect":[0,0,10,10]})",                      // absent
         }) {
        Manifest m;
        REQUIRE(ParseDualScreenManifest(
            nlohmann::json::parse(std::string(R"({"pages":[{"id":"p","scrolls":[)") + bad +
                                  R"(],"widgets":[]}]})"),
            m));
        REQUIRE(m.pages.size() == 1);
        REQUIRE(m.pages[0].scrolls.size() == 1); // the malformed key never drops the region
        REQUIRE_FALSE(m.pages[0].scrolls[0].rect_alt.has_value());
        REQUIRE(m.pages[0].scrolls[0].rect == std::array<s32, 4>{0, 0, 10, 10}); // rect still parses
    }
}

TEST_CASE("DSMod canvas_alt: ApplyCanvasVariant's swap is self-inverse on the scroll region data "
          "model",
          "[dsmod][runtime25][canvas_alt]") {
    // The exact operation ModRuntime::ApplyCanvasVariant performs per scroll region
    // (mod_redraw.cpp), mirroring what it already does per widget: swapping rect <-> *rect_alt
    // and back must restore the original values exactly.
    ScrollRegion r;
    r.id = "l";
    r.rect = {0, 20, 1200, 40};
    r.rect_alt = std::array<s32, 4>{0, 0, 200, 1080};
    const auto default_rect = r.rect;
    const auto alt_rect = *r.rect_alt;
    std::swap(r.rect, *r.rect_alt); // switch to canvas_alt
    REQUIRE(r.rect == alt_rect);
    REQUIRE(*r.rect_alt == default_rect);
    std::swap(r.rect, *r.rect_alt); // switch back
    REQUIRE(r.rect == default_rect);
    REQUIRE(*r.rect_alt == alt_rect);

    // A region without rect_alt is never touched: nothing to swap, so it keeps `rect` on every
    // canvas, same as runtime <= 24.
    ScrollRegion plain;
    plain.rect = {1, 2, 3, 4};
    REQUIRE_FALSE(plain.rect_alt.has_value());
}

TEST_CASE("DSMod canvas_alt: a page's scroll regions swap and restore together, like its widgets",
          "[dsmod][runtime25][canvas_alt]") {
    // Mirrors the exact per-page loop ModRuntime::ApplyCanvasVariant runs over page.scrolls
    // (mod_redraw.cpp), alongside its existing per-widget loop -- a committed switch moves both,
    // and a second commit (switching back) restores both.
    Page page;
    ScrollRegion list;
    list.id = "list";
    list.rect = {24, 200, 1192, 660};
    list.rect_alt = std::array<s32, 4>{24, 120, 392, 900};
    page.scrolls.push_back(list);
    ScrollRegion fixed;
    fixed.id = "fixed"; // no rect_alt: stays put on every canvas
    fixed.rect = {0, 0, 100, 50};
    page.scrolls.push_back(fixed);

    const auto apply = [](Page& p) {
        for (auto& region : p.scrolls) {
            if (region.rect_alt) {
                std::swap(region.rect, *region.rect_alt);
            }
        }
    };
    const auto default_list_rect = page.scrolls[0].rect;
    const auto alt_list_rect = *page.scrolls[0].rect_alt;
    const auto fixed_rect = page.scrolls[1].rect;

    apply(page); // canvas_alt on
    REQUIRE(page.scrolls[0].rect == alt_list_rect);
    REQUIRE(*page.scrolls[0].rect_alt == default_list_rect);
    REQUIRE(page.scrolls[1].rect == fixed_rect); // untouched: no rect_alt

    apply(page); // canvas_alt off
    REQUIRE(page.scrolls[0].rect == default_list_rect);
    REQUIRE(*page.scrolls[0].rect_alt == alt_list_rect);
    REQUIRE(page.scrolls[1].rect == fixed_rect);
}

TEST_CASE("DSMod scroll rect_alt: clip, scrollbar track, content/max offset and hit test "
          "recompute after the swap",
          "[dsmod][runtime25][canvas_alt]") {
    constexpr s32 RowPitch = 20;
    Page page;
    ScrollRegion region;
    region.id = "l";
    region.rect = {0, 20, 200, 100};                     // landscape: wide, short viewport
    region.rect_alt = std::array<s32, 4>{0, 0, 60, 300}; // portrait: narrow, tall viewport
    region.bar_color = 0xFFFF0000u;
    region.bar_w = 4;
    page.scrolls.push_back(region);

    Widget row;
    row.type = WidgetType::Rect;
    row.rect = {0, 20, 190, 18}; // same y as the region's own rect, like a real manifest would
    row.repeat = 30;             // 30 * 20 = 600 px of content: taller than either viewport
    row.repeat_dy = RowPitch;
    row.scroll = "l";
    row.on_tap = "row.tap";
    row.payload = "{i}";
    page.widgets.push_back(row);

    const auto state = [](s64 offset) {
        StateSnapshot s;
        s.ints[ScrollOffsetKey("l")] = offset;
        return s;
    };
    const ScrollRegion& active = page.scrolls[0]; // tracks the live (possibly swapped) rect

    // --- Landscape: the manifest's default canvas, nothing has swapped yet -------------------
    const auto m_landscape = MeasureScroll(page, active, state(0));
    REQUIRE(m_landscape.content_h == 30 * RowPitch);
    REQUIRE(m_landscape.max_offset == 30 * RowPitch - 100); // 500: the short viewport's own range

    for (const auto& w : ExpandWidgets(page, state(0))) {
        REQUIRE(w.scroll_clip == active.rect); // clip follows the active (landscape) rect
    }
    // A tap inside the landscape viewport hits row 0; the same point sits outside a 60-px-wide
    // portrait viewport.
    REQUIRE(HitTest(page, state(0), 150, 25) == "row.tap");

    const auto thumb_landscape = ScrollBarThumb(active, m_landscape, 0);
    REQUIRE(thumb_landscape == std::array<s32, 4>{196, 20, 4, 24});

    // --- Commit the canvas_alt switch: the exact op ApplyCanvasVariant performs --------------
    std::swap(page.scrolls[0].rect, *page.scrolls[0].rect_alt);

    const auto m_portrait = MeasureScroll(page, active, state(0));
    REQUIRE(m_portrait.content_h == 30 * RowPitch);         // the content itself is unchanged...
    REQUIRE(m_portrait.max_offset == 30 * RowPitch - 300);  // 300: the taller viewport's own range
    REQUIRE(m_portrait.max_offset != m_landscape.max_offset);

    for (const auto& w : ExpandWidgets(page, state(0))) {
        REQUIRE(w.scroll_clip == active.rect); // clip now follows the swapped-in portrait rect
    }
    REQUIRE(HitTest(page, state(0), 150, 25).empty());      // outside the narrower viewport now
    REQUIRE(HitTest(page, state(0), 30, 25) == "row.tap");  // inside it, still hits row 0

    const auto thumb_portrait = ScrollBarThumb(active, m_portrait, 0);
    REQUIRE(thumb_portrait == std::array<s32, 4>{56, 0, 4, 150});
    REQUIRE(thumb_portrait != thumb_landscape); // the track moved and resized with the viewport

    // --- Switch back: every derived value returns to the landscape geometry exactly ----------
    std::swap(page.scrolls[0].rect, *page.scrolls[0].rect_alt);
    REQUIRE(MeasureScroll(page, active, state(0)).max_offset == m_landscape.max_offset);
    REQUIRE(ScrollBarThumb(active, m_landscape, 0) == thumb_landscape);
}

TEST_CASE("DSMod scroll rect_alt: two-axis regions and scroll_follow also recompute after the "
          "swap",
          "[dsmod][runtime25][canvas_alt]") {
    // A table-like region (scroll_2d.cpp's TablePage shape): a frozen header follows x only, so
    // its clip stays unbounded on y regardless of which rect is active.
    Page page;
    ScrollRegion region;
    region.id = "tbl";
    region.rect = {100, 200, 600, 400};
    region.rect_alt = std::array<s32, 4>{20, 40, 300, 700};
    region.axes = ScrollRegion::AxisX | ScrollRegion::AxisY;
    region.content_w = 1800;
    region.content_h = 1200;
    page.scrolls.push_back(region);

    Widget pane; // follows both axes (Widget::scroll_follow defaults to AxisX | AxisY)
    pane.type = WidgetType::Rect;
    pane.rect = {100, 200, 1800, 1200};
    pane.repeat = 1;
    pane.scroll = "tbl";
    page.widgets.push_back(pane);

    Widget header; // a frozen row: follows x only
    header.type = WidgetType::Rect;
    header.rect = {100, 160, 1800, 40};
    header.repeat = 1;
    header.scroll = "tbl";
    header.scroll_follow = ScrollRegion::AxisX;
    page.widgets.push_back(header);

    const ScrollRegion& active = page.scrolls[0];
    StateSnapshot s;
    s.ints[ScrollOffsetXKey("tbl")] = 100;
    s.ints[ScrollOffsetKey("tbl")] = 100;

    const auto m = MeasureScroll(page, active, s);
    REQUIRE(m.max_offset_x == 1800 - 600); // 1200
    REQUIRE(m.max_offset == 1200 - 400);   // 800
    auto expanded = ExpandWidgets(page, s);
    REQUIRE(expanded[0].scroll_clip == active.rect);       // the pane clips to the full rect
    REQUIRE(expanded[1].scroll_clip[0] == active.rect[0]); // the header: bounded on x...
    REQUIRE(expanded[1].scroll_clip[2] == active.rect[2]);
    REQUIRE(expanded[1].scroll_clip[1] < 0); // ...unbounded on y (it follows x only)

    std::swap(page.scrolls[0].rect, *page.scrolls[0].rect_alt); // commit the canvas switch

    const auto m2 = MeasureScroll(page, active, s);
    REQUIRE(m2.max_offset_x == 1800 - 300); // 1500: the new, narrower viewport's own x range
    REQUIRE(m2.max_offset == 1200 - 700);   // 500: the new, taller viewport's own y range
    REQUIRE(m2.max_offset_x != m.max_offset_x);
    REQUIRE(m2.max_offset != m.max_offset);
    expanded = ExpandWidgets(page, s);
    REQUIRE(expanded[0].scroll_clip == active.rect); // the clip follows the swapped-in rect
    REQUIRE(expanded[1].scroll_clip[0] == active.rect[0]);
    REQUIRE(expanded[1].scroll_clip[2] == active.rect[2]);
    REQUIRE(expanded[1].scroll_clip[1] < 0); // scroll_follow's unbounded axis is unaffected
}

TEST_CASE("DSMod scroll rect_alt: the stored offset is preserved across the swap and clamped to "
          "whichever rect is active",
          "[dsmod][runtime25][canvas_alt]") {
    Page page;
    ScrollRegion region;
    region.id = "l";
    region.rect = {0, 0, 200, 400};                      // landscape: generous scroll range
    region.rect_alt = std::array<s32, 4>{0, 0, 200, 950}; // portrait: far less room to scroll
    page.scrolls.push_back(region);
    Widget row;
    row.type = WidgetType::Rect;
    row.rect = {0, 0, 190, 20}; // same y as the region's own rect, both before and after the swap
    row.repeat = 50;            // content_h = 50 * 20 = 1000
    row.repeat_dy = 20;
    row.scroll = "l";
    page.widgets.push_back(row);

    const ScrollRegion& active = page.scrolls[0];
    StateSnapshot s;
    constexpr s64 StoredOffset = 580; // valid in landscape (max 600), oversized in portrait (max 50)
    s.ints[ScrollOffsetKey("l")] = StoredOffset;

    const auto m = MeasureScroll(page, active, s);
    REQUIRE(m.max_offset == 1000 - 400); // 600
    REQUIRE(ScrollOffset(active, s) == StoredOffset); // the raw stored value, unclamped by itself
    // Every real use clamps it to this canvas' own range (ExpandWidgets / TapPayload's own
    // std::min(ScrollOffset(...), m.max_offset)); still in range here, so it is used as-is.
    REQUIRE(std::min(ScrollOffset(active, s), m.max_offset) == StoredOffset);

    std::swap(page.scrolls[0].rect, *page.scrolls[0].rect_alt); // commit the canvas switch

    const auto m2 = MeasureScroll(page, active, s);
    REQUIRE(m2.max_offset == 1000 - 950); // 50: far less room in the alternate (taller) viewport
    // The snapshot is untouched by the swap: the raw value survives exactly.
    REQUIRE(ScrollOffset(active, s) == StoredOffset);
    // But its effective, used value now clamps to the new, smaller range.
    REQUIRE(std::min(ScrollOffset(active, s), m2.max_offset) == m2.max_offset);
    // ExpandWidgets applies that same clamp (its own std::min(ScrollOffset, max_offset)): the list
    // lands pinned to its new end, not left scrolled past it or snapped back to 0.
    const auto expanded = ExpandWidgets(page, s);
    REQUIRE(!expanded.empty());
    REQUIRE(expanded.back().rect[1] + 20 == active.rect[1] + active.rect[3]);

    // Switching back restores the original, generous range -- the stored value was never touched.
    std::swap(page.scrolls[0].rect, *page.scrolls[0].rect_alt);
    REQUIRE(MeasureScroll(page, active, s).max_offset == m.max_offset);
    REQUIRE(ScrollOffset(active, s) == StoredOffset);
}

TEST_CASE("DSMod canvas selection: no canvas_alt is byte-identical to runtime <= 24",
          "[dsmod][runtime25][canvas_alt]") {
    // No alt declared (alt_w/alt_h 0): always the old ternary (manifest canvas, else the panel).
    auto choice = SelectCanvasAspect(480, 1080, 1240, 1080, 0, 0, CanvasAltWhen::Portrait);
    REQUIRE(choice.w == 1240);
    REQUIRE(choice.h == 1080);
    REQUIRE_FALSE(choice.alt);
    choice = SelectCanvasAspect(480, 1080, 0, 0, 0, 0, CanvasAltWhen::Portrait);
    REQUIRE(choice.w == 480); // canvas_w/h 0 falls back to the panel, as before
    REQUIRE(choice.h == 1080);
    REQUIRE_FALSE(choice.alt);
}

TEST_CASE("DSMod canvas selection: \"portrait\" picks the alternate only in a taller pane",
          "[dsmod][runtime25][canvas_alt]") {
    // A side-by-side Simulated column: tall and narrow (RESEARCH.md S4.5 C2).
    auto tall = SelectCanvasAspect(480, 1080, 1240, 1080, 720, 1280, CanvasAltWhen::Portrait);
    REQUIRE(tall.alt);
    REQUIRE(tall.w == 720);
    REQUIRE(tall.h == 1280);
    // A normal landscape pane: the default canvas, unchanged.
    auto wide = SelectCanvasAspect(1240, 1080, 1240, 1080, 720, 1280, CanvasAltWhen::Portrait);
    REQUIRE_FALSE(wide.alt);
    REQUIRE(wide.w == 1240);
    REQUIRE(wide.h == 1080);
    // Exactly square: not strictly taller than wide, so still the default.
    auto square = SelectCanvasAspect(1000, 1000, 1240, 1080, 720, 1280, CanvasAltWhen::Portrait);
    REQUIRE_FALSE(square.alt);
}

TEST_CASE("DSMod canvas selection: \"narrow\" also catches a landscape pane narrower than the "
          "default canvas",
          "[dsmod][runtime25][canvas_alt]") {
    // Still wider than tall (landscape by the simple w>h test) but noticeably narrower in aspect
    // than the 1240x1080 default canvas -- "narrow" should prefer the alternate; "portrait"
    // (strict h>w) should not.
    constexpr u32 PanelW = 900, PanelH = 820; // aspect ~1.098, narrower than 1240/1080 ~1.148
    auto narrow = SelectCanvasAspect(PanelW, PanelH, 1240, 1080, 720, 900, CanvasAltWhen::Narrow);
    REQUIRE(narrow.alt);
    auto portrait =
        SelectCanvasAspect(PanelW, PanelH, 1240, 1080, 720, 900, CanvasAltWhen::Portrait);
    REQUIRE_FALSE(portrait.alt); // still landscape (w > h): Portrait never fires
    // A pane at least as wide, relative to its height, as the default canvas: neither fires.
    auto wide = SelectCanvasAspect(1300, 1080, 1240, 1080, 720, 900, CanvasAltWhen::Narrow);
    REQUIRE_FALSE(wide.alt);
}

TEST_CASE("DSMod canvas selection: zero panel or alt dims never crash, pick the default",
          "[dsmod][runtime25][canvas_alt]") {
    REQUIRE_FALSE(SelectCanvasAspect(0, 0, 1240, 1080, 720, 1280, CanvasAltWhen::Portrait).alt);
    REQUIRE_FALSE(SelectCanvasAspect(480, 1080, 1240, 1080, 720, 0, CanvasAltWhen::Portrait).alt);
    REQUIRE_FALSE(SelectCanvasAspect(480, 1080, 1240, 1080, 0, 1280, CanvasAltWhen::Portrait).alt);
}

TEST_CASE("DSMod ParseButton: L3/R3 and the StickL/StickR aliases reach the virtual stick clicks",
          "[dsmod][runtime25][button]") {
    using VB = InputCommon::VirtualGamepad::VirtualButton;
    bool ok = false;
    REQUIRE(ParseButton("L3", ok) == VB::StickL);
    REQUIRE(ok);
    REQUIRE(ParseButton("R3", ok) == VB::StickR);
    REQUIRE(ok);
    REQUIRE(ParseButton("StickL", ok) == VB::StickL);
    REQUIRE(ok);
    REQUIRE(ParseButton("StickR", ok) == VB::StickR);
    REQUIRE(ok);
    // Existing tokens are unaffected.
    REQUIRE(ParseButton("A", ok) == VB::ButtonA);
    REQUIRE(ok);
    REQUIRE(ParseButton("L", ok) == VB::TriggerL);
    REQUIRE(ok);
    ParseButton("L4", ok);
    REQUIRE_FALSE(ok); // still an unknown token
}

TEST_CASE("DSMod runtime 25: version and ABI unaffected", "[dsmod][runtime25]") {
    REQUIRE(DualScreenRuntimeVersion >= 25);
    // No module ABI change: ABI stays 1 (rt24 extension 6's hash is a fixed macro this stage
    // never touches, dsmod_module_extensions.h / dsmod_module_abi.h).
    REQUIRE(EDEN_DSMOD_MODULE_ABI_VERSION == 1u);
}
