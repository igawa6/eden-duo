// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 25: pane geometry host values (@screen.*, mod_host_values.h), the optional
// "canvas_alt" manifest canvas and its per-widget "rect_alt" (SelectCanvasAspect,
// mod_redraw.cpp), a page scroll region's own "rect_alt" (ScrollRegion::rect_alt, swapped
// alongside the widgets' by the same ApplyCanvasVariant), the L3/R3 virtual button tokens
// (ParseButton, mod_actions.cpp), the "view_zoom_step" action (RunAction's
// ActionKind::ViewZoomStep, mod_actions.cpp; its anchor math, GlideViewZoomStep, in
// mod_view_default.h): a generic +/- zoom step on a map or image pan_zoom widget, about the
// view's own current centre, clamped to its zoom limits and (for an image) its pan_bounds; and a
// widget's own "wrap_width_alt" (Widget::wrap_width_alt, mod_types_page.h), swapped alongside
// rect_alt by the same ApplyCanvasVariant so fit_text / wrap / ellipsis / DrawTextBlock
// (mod_ui.cpp) wrap against whichever canvas is active; and (rt25 addition) a pan_zoom gesture's
// live finger position -- "@view_touch_x/y:<id>" / "@view_touch_down:<id>" (UpdateViewTouch,
// PanZoomViewHostValueFrom, mod_input.cpp), read-only, for a module to draw an effect under the
// finger during a stroke.

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/mod_host_values.h"
#include "core/mods/mod_map_view_rect.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_view_default.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "video_core/dsmod/aux_routing.h"

using Catch::Approx;

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

// --- "view_zoom_step": a generic map/image pan_zoom +/- step, anchored on the view's own centre
// (ActionKind::ViewZoomStep / RunAction, mod_actions.cpp; GlideViewZoomStep, mod_view_default.h)
// -----------------------------------------------------------------------------------------------

namespace {
/// A 1000 x 800 pan_zoom image widget, `bounds` as its pan_bounds (mirrors pan_bounds.cpp's own
/// PanImage -- a separate copy, since that one has file-local linkage).
Widget ZoomImage(std::optional<PanBounds> bounds = std::nullopt) {
    Widget w;
    w.type = WidgetType::Image;
    w.id = "pic";
    w.rect = {0, 0, 1000, 800};
    w.src = "file:pic.png";
    w.pan_zoom = true;
    w.min_zoom = 1.0f;
    w.max_zoom = 8.0f;
    w.pan_bounds = bounds;
    return w;
}
} // namespace

TEST_CASE("DSMod manifest: view_zoom_step action parses view/factor; a missing or non-positive "
          "factor leaves it refusable",
          "[dsmod][runtime25][view_zoom_step]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(R"({
        "pages":[{"id":"p","widgets":[]}],
        "actions":{
            "in":{"kind":"view_zoom_step","view":"map","factor":1.25},
            "out":{"kind":"view_zoom_step","view":"map","factor":0.8}
        }
    })"),
                                    m));
    REQUIRE(m.actions.at("in").kind == ActionKind::ViewZoomStep);
    REQUIRE(m.actions.at("in").view == "map");
    REQUIRE(m.actions.at("in").zoom_factor == Approx(1.25));
    REQUIRE(m.actions.at("out").zoom_factor == Approx(0.8));

    // Absent, zero, negative or non-numeric "factor" all parse to the default 0.0 --
    // ModRuntime::RunAction's own "!(action.zoom_factor > 0.0)" guard (mod_actions.cpp,
    // ActionKind::ViewZoomStep) then refuses the action and logs it, exactly like any other
    // invalid action (module declined, a write with no usable point, ...).
    for (const char* bad : {
             R"({"kind":"view_zoom_step","view":"map"})",
             R"({"kind":"view_zoom_step","view":"map","factor":0})",
             R"({"kind":"view_zoom_step","view":"map","factor":-1.5})",
             R"({"kind":"view_zoom_step","view":"map","factor":"1.5"})",
         }) {
        Manifest bm;
        const auto j = nlohmann::json::parse(
            std::string(R"({"pages":[{"id":"p","widgets":[]}],"actions":{"a":)") + bad + "}}");
        REQUIRE(ParseDualScreenManifest(j, bm));
        REQUIRE(bm.actions.at("a").kind == ActionKind::ViewZoomStep);
        REQUIRE_FALSE(bm.actions.at("a").zoom_factor > 0.0);
    }
}

TEST_CASE("DSMod view_zoom_step: RunAction's other guard -- an unresolved or non-pan_zoom view "
          "-- refuses the action (mod_actions.cpp ActionKind::ViewZoomStep)",
          "[dsmod][runtime25][view_zoom_step]") {
    // RunAction itself needs a live owning kernel/System to call (IsOwnerContext) -- the same
    // boundary every other RunAction case sits outside of in this suite (ViewReset has no
    // RunAction-level test either). What is tested here is the lookup its guard runs before
    // touching a view: FindWidgetByKey's own by-id match on the current page (mod_input.cpp),
    // then "w == nullptr || !w->pan_zoom".
    Page page;
    page.id = "p";
    Widget map_widget = ZoomImage();
    map_widget.id = "map";
    page.widgets.push_back(map_widget);
    Widget label; // present, but not a pan_zoom widget
    label.type = WidgetType::Value;
    label.id = "label";
    label.rect = {0, 0, 10, 10};
    page.widgets.push_back(label);

    const auto find_by_key = [&](const std::string& key) -> const Widget* {
        for (const auto& w : page.widgets) {
            if (w.id == key) {
                return &w;
            }
        }
        return nullptr;
    };
    REQUIRE(find_by_key("map") != nullptr);
    REQUIRE(find_by_key("map")->pan_zoom); // resolves to a real pan_zoom view: RunAction proceeds
    REQUIRE(find_by_key("label") != nullptr);
    REQUIRE_FALSE(find_by_key("label")->pan_zoom); // found, but not pan_zoom: refused
    REQUIRE(find_by_key("nope") == nullptr);       // unknown id: refused
}

TEST_CASE("DSMod view_zoom_step: zooming in or out keeps the view's own current centre fixed",
          "[dsmod][runtime25][view_zoom_step]") {
    const Widget w = ZoomImage(); // 1000x800, min/max zoom 1..8, no pan_bounds
    ViewTransform v;
    v.zoom = 2.0f;
    v.pan_x = 150.0f; // an arbitrary pan left by an earlier drag
    v.pan_y = 80.0f;
    const float ox = static_cast<float>(w.rect[2]) * 0.5f;
    const float oy = static_cast<float>(w.rect[3]) * 0.5f;
    const float cx_before = v.pan_x + ox / v.zoom; // the world point under the widget's centre
    const float cy_before = v.pan_y + oy / v.zoom;

    GlideViewZoomStep(v, w, 1.25f, w.min_zoom, w.max_zoom); // zoom in
    REQUIRE(v.gliding);
    REQUIRE(v.goal_zoom == Approx(2.5f));
    REQUIRE(v.goal_pan_x + ox / v.goal_zoom == Approx(cx_before));
    REQUIRE(v.goal_pan_y + oy / v.goal_zoom == Approx(cy_before));

    // Land the glide (as GlideViews, mod_input.cpp, would over several ticks) and zoom back out:
    // the same centre survives a second step too.
    v.zoom = v.goal_zoom;
    v.pan_x = v.goal_pan_x;
    v.pan_y = v.goal_pan_y;
    v.gliding = false;
    GlideViewZoomStep(v, w, 0.8f, w.min_zoom, w.max_zoom); // zoom out
    REQUIRE(v.goal_zoom == Approx(2.0f));
    REQUIRE(v.goal_pan_x + ox / v.goal_zoom == Approx(cx_before));
    REQUIRE(v.goal_pan_y + oy / v.goal_zoom == Approx(cy_before));
}

TEST_CASE("DSMod view_zoom_step: clamps to the widget's own zoom limits, same as a pinch",
          "[dsmod][runtime25][view_zoom_step]") {
    Widget w = ZoomImage();
    w.min_zoom = 1.0f;
    w.max_zoom = 4.0f;
    ViewTransform v;
    v.zoom = 3.5f;
    v.pan_x = 50.0f;
    v.pan_y = 20.0f;
    GlideViewZoomStep(v, w, 2.0f, w.min_zoom, w.max_zoom); // 7.0 asked for: clamps to 4.0
    REQUIRE(v.goal_zoom == Approx(4.0f));
    // The pan is the anchor math's own answer for the CLAMPED zoom, not the unclamped target.
    const float ox = static_cast<float>(w.rect[2]) * 0.5f;
    const float oy = static_cast<float>(w.rect[3]) * 0.5f;
    const float anchor_x = v.pan_x + ox / 3.5f;
    const float anchor_y = v.pan_y + oy / 3.5f;
    REQUIRE(v.goal_pan_x == Approx(anchor_x - ox / 4.0f));
    REQUIRE(v.goal_pan_y == Approx(anchor_y - oy / 4.0f));

    // Zooming out past min_zoom clamps there too.
    v.zoom = 1.2f;
    v.pan_x = 0.0f;
    v.pan_y = 0.0f;
    v.gliding = false;
    GlideViewZoomStep(v, w, 0.1f, w.min_zoom, w.max_zoom); // 0.12 asked for: clamps to 1.0
    REQUIRE(v.goal_zoom == Approx(1.0f));
}

TEST_CASE("DSMod view_zoom_step: a map step becomes a custom view, holding StepBaseGlide's "
          "follow still (mod_map_view_rect.h), and never zooms out past the whole-area floor",
          "[dsmod][runtime25][view_zoom_step][view_rect]") {
    Widget map_w;
    map_w.type = WidgetType::Map;
    map_w.id = "m";
    map_w.rect = {0, 0, 400, 300};
    map_w.pan_zoom = true;
    map_w.min_zoom = 1.0f;
    map_w.max_zoom = 8.0f;

    ViewTransform v; // at rest, at home: zoom 1, pan 0 (the rect fit)
    REQUIRE_FALSE(MapViewCustom(v.zoom, v.pan_x, v.pan_y, v.gliding));

    // The rect's own area floor: the pinch (and this step) may zoom out to it, no further
    // (RectMinZoom -- the same floor ZoomLimits publishes as "<id>#zmin", mod_input.cpp).
    const float zoom_min = RectMinZoom(/*area_ppw=*/0.4f, /*base_ppw=*/2.0f, map_w.min_zoom);
    REQUIRE(zoom_min == Approx(0.2f));

    GlideViewZoomStep(v, map_w, 1.25f, zoom_min, map_w.max_zoom); // a zoom-in button press
    REQUIRE(v.goal_zoom == Approx(1.25f));
    v.zoom = v.goal_zoom; // land it, as GlideViews would
    v.pan_x = v.goal_pan_x;
    v.pan_y = v.goal_pan_y;
    v.gliding = false;
    // The view is now away from its rect-fit home: StepBaseGlide's caller
    // (mod_ui_map_widget.cpp) reads exactly this to hold the shown base rect still instead of
    // gliding it under the player while the user's own step is in effect.
    REQUIRE(MapViewCustom(v.zoom, v.pan_x, v.pan_y, v.gliding));
    MapBaseView shown{0.0f, 0.0f, 2.0f};
    const MapBaseView target{100.0f, 50.0f, 3.0f}; // the player entered another room
    REQUIRE_FALSE(StepBaseGlide(shown, target, /*hold=*/true));
    REQUIRE(shown.ppw == Approx(2.0f)); // held exactly where the zoom step left it: no drift

    // Stepping out repeatedly never shows less than the whole area.
    for (int i = 0; i < 5; ++i) {
        GlideViewZoomStep(v, map_w, 0.5f, zoom_min, map_w.max_zoom);
        v.zoom = v.goal_zoom;
        v.pan_x = v.goal_pan_x;
        v.pan_y = v.goal_pan_y;
        v.gliding = false;
    }
    REQUIRE(v.zoom == Approx(zoom_min));
}

TEST_CASE("DSMod view_zoom_step: an image's pan_bounds clamps the stepped pan, same as a pinch",
          "[dsmod][runtime25][view_zoom_step][pan_bounds]") {
    // 1000x800 image, bounds u 0.5..1.0 (the right half) -- the same shape as pan_bounds.cpp's
    // "DrawImage keeps the window inside the bounds" fixture.
    const Widget w = ZoomImage(PanBounds{0.5f, 0.0f, 1.0f, 1.0f});
    ViewTransform v;
    v.zoom = 2.0f; // a 500 px window: exactly the bounded half, origin pinned at 500
    v.pan_x = 500.0f;
    v.pan_y = 0.0f;
    GlideViewZoomStep(v, w, 0.5f, w.min_zoom, w.max_zoom); // zoom out to 1: the whole picture
    REQUIRE(v.goal_zoom == Approx(1.0f));
    // A window as wide as the whole picture has one place to be, bounds or not.
    REQUIRE(v.goal_pan_x == Approx(0.0f));
    v.zoom = v.goal_zoom;
    v.pan_x = v.goal_pan_x;
    v.pan_y = v.goal_pan_y;
    v.gliding = false;

    GlideViewZoomStep(v, w, 4.0f, w.min_zoom, w.max_zoom); // zoom back in, past the bounded window
    REQUIRE(v.goal_zoom == Approx(4.0f));
    // A 250 px window at zoom 4, kept inside the bounded half [500, 750] rather than wherever the
    // raw anchor math alone would have put it.
    REQUIRE(v.goal_pan_x >= 500.0f);
    REQUIRE(v.goal_pan_x <= 750.0f);
}

// --- "wrap_width_alt": a label's wrap_width swapped alongside rect_alt on a canvas_alt switch
// (Widget::wrap_width_alt, mod_types_page.h; ModRuntime::ApplyCanvasVariant, mod_redraw.cpp)
// -----------------------------------------------------------------------------------------------

TEST_CASE("DSMod manifest: widget wrap_width_alt parses; absent or malformed leaves it unset",
          "[dsmod][runtime25][wrap_width_alt]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(R"({"pages":[{"id":"p","widgets":[
        {"type":"label","rect":[0,0,100,50],"wrap_width":48,"wrap_width_alt":320},
        {"type":"label","rect":[0,0,100,50],"wrap_width":48}
    ]}]})"),
                                    m));
    REQUIRE(m.pages[0].widgets.size() == 2);
    REQUIRE(m.pages[0].widgets[0].wrap_width == 48);
    REQUIRE(m.pages[0].widgets[0].wrap_width_alt.has_value());
    REQUIRE(*m.pages[0].widgets[0].wrap_width_alt == 320);
    REQUIRE_FALSE(m.pages[0].widgets[1].wrap_width_alt.has_value()); // absent: unset, as before

    for (const char* bad : {
             R"({"type":"label","rect":[0,0,10,10],"wrap_width_alt":"320"})", // a string
             R"({"type":"label","rect":[0,0,10,10],"wrap_width_alt":null})",
             R"({"type":"label","rect":[0,0,10,10],"wrap_width_alt":[1,2]})",
             R"({"type":"label","rect":[0,0,10,10]})",
         }) {
        Manifest bm;
        REQUIRE(ParseDualScreenManifest(
            nlohmann::json::parse(std::string(R"({"pages":[{"id":"p","widgets":[)") + bad + "]}]}"),
            bm));
        REQUIRE(bm.pages[0].widgets.size() == 1);
        REQUIRE_FALSE(bm.pages[0].widgets[0].wrap_width_alt.has_value());
    }
}

TEST_CASE("DSMod canvas_alt: wrap_width_alt swap is self-inverse, alongside rect_alt -- the exact "
          "per-widget op ApplyCanvasVariant performs (mod_redraw.cpp)",
          "[dsmod][runtime25][wrap_width_alt]") {
    Widget w;
    w.rect = {0, 0, 1240, 1080};
    w.rect_alt = std::array<s32, 4>{40, 0, 400, 1080};
    w.wrap_width = 1100;
    w.wrap_width_alt = 320;
    const auto default_rect = w.rect;
    const auto alt_rect = *w.rect_alt;
    const auto default_wrap = w.wrap_width;
    const auto alt_wrap = *w.wrap_width_alt;

    std::swap(w.rect, *w.rect_alt); // switch to canvas_alt
    std::swap(w.wrap_width, *w.wrap_width_alt);
    REQUIRE(w.rect == alt_rect);
    REQUIRE(w.wrap_width == alt_wrap);
    REQUIRE(*w.rect_alt == default_rect);
    REQUIRE(*w.wrap_width_alt == default_wrap);

    std::swap(w.rect, *w.rect_alt); // switch back
    std::swap(w.wrap_width, *w.wrap_width_alt);
    REQUIRE(w.rect == default_rect);
    REQUIRE(w.wrap_width == default_wrap);
    REQUIRE(*w.rect_alt == alt_rect);
    REQUIRE(*w.wrap_width_alt == alt_wrap);

    // A widget with rect_alt but no wrap_width_alt: only the rect swaps, exactly as runtime <= 24
    // -- its wrap_width is never touched on either canvas.
    Widget plain;
    plain.rect = {1, 2, 3, 4};
    plain.rect_alt = std::array<s32, 4>{5, 6, 7, 8};
    plain.wrap_width = 500;
    REQUIRE_FALSE(plain.wrap_width_alt.has_value());
    std::swap(plain.rect, *plain.rect_alt);
    REQUIRE(plain.wrap_width == 500);
}

TEST_CASE("DSMod wrap_width_alt: fit_text / wrap / DrawTextBlock use whichever width the swap "
          "left active -- no extra plumbing past Widget::wrap_width itself",
          "[dsmod][runtime25][wrap_width_alt]") {
    constexpr u32 White = 0xFFFFFFFFu;
    const auto render = [](const Widget& widget) {
        Page page;
        page.id = "p";
        page.widgets.push_back(widget);
        Canvas canvas;
        canvas.Resize(200, 60);
        REQUIRE(RenderPage(canvas, Manifest{}, page, {}));
        return canvas.Pixels();
    };

    Widget w;
    w.type = WidgetType::Label;
    w.text = "ABCDEF";
    w.color = White;
    w.text_scale = 8;
    w.text_min_scale = 2; // the same floor runtime18's own fit_text fixture uses
    w.max_lines = 1;
    w.fit_text = true;
    w.wrap_width = 48;       // landscape: narrow -- "ABCDEF" cannot fit above the floor, so
    w.wrap_width_alt = 4000; // fit_text shrinks it all the way down (the validated runtime18
                             // outcome for this exact text/width/floor, whatever the declared
                             // scale). portrait: generous -- no shrink is ever needed.

    const auto narrow = render(w);
    Canvas narrow_expected;
    narrow_expected.Resize(200, 60);
    narrow_expected.Clear(Manifest{}.background);
    narrow_expected.DrawTextBlock(0, 0, w.text, 2, White, 0, 48, 1, -1);
    REQUIRE(std::equal(narrow.begin(), narrow.end(), narrow_expected.Pixels().begin()));

    // The exact per-widget op ApplyCanvasVariant performs on a canvas switch: swap wrap_width <->
    // wrap_width_alt (alongside rect_alt, not exercised here). RenderPage itself needs no
    // canvas_alt awareness -- it only ever reads the live widget.wrap_width.
    Widget alt = w;
    std::swap(alt.wrap_width, *alt.wrap_width_alt);
    const auto wide = render(alt);
    Canvas wide_expected;
    wide_expected.Resize(200, 60);
    wide_expected.Clear(Manifest{}.background);
    // 4000 px comfortably fits "ABCDEF" at the declared scale 8: fit_text's shrink loop never
    // triggers, so it draws at the widget's own text_scale, unshrunk.
    wide_expected.DrawTextBlock(0, 0, w.text, 8, White, 0, 4000, 1, -1);
    REQUIRE(std::equal(wide.begin(), wide.end(), wide_expected.Pixels().begin()));
    REQUIRE_FALSE(std::equal(narrow.begin(), narrow.end(), wide.begin())); // visibly different
}

// rt25 addition: @view_touch_x/y:<id> + @view_touch_down:<id> -- the live finger position of a
// pan_zoom gesture. UpdateViewTouch (mod_input.cpp) is the pure write side: it stores/clears the
// resolved sample UpdateGestures hands it each tick (`now.front()`'s canvas coordinate when a
// finger is down, matching the single-finger pan path's own `now[0]`/`now.front()` convention a
// few lines above it in UpdateGestures, already relied on there and by SwipeTracker/HoldTracker
// even with extra fingers present) into the ViewState map; PanZoomViewHostValueFrom is the read
// side, extended above to answer the three new keys. Kept free of ModRuntime, like
// map_pan_zoom_host_value.cpp's own tests, for the same Core::System boundary (see that file's
// header comment and mod_launch_gate.cpp's).

TEST_CASE("DSMod view touch: PanZoomViewHostValueFrom answers nullopt for anything not a "
          "view-touch key",
          "[dsmod][runtime25][view_touch]") {
    ViewState states;
    states["pad"] = ViewTransform{};
    for (const char* key : {"", "touch_x", "@flag:foo", "@view_touch:pad", "@view_touch_x",
                            "@view_touch_xx:pad"}) {
        REQUIRE_FALSE(PanZoomViewHostValueFrom(key, states).has_value());
    }
    // A bare prefix with nothing after the colon, for each of the three new keys.
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_touch_x:", states).has_value());
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_touch_y:", states).has_value());
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_touch_down:", states).has_value());
}

TEST_CASE("DSMod view touch: an untracked id is absent for touch_x/y/down too, even when another "
          "id is tracked and touched",
          "[dsmod][runtime25][view_touch]") {
    ViewState states;
    states["pad"] = ViewTransform{};
    UpdateViewTouch(states, "pad", true, 5, 6);
    for (const char* key : {"@view_touch_x:other", "@view_touch_y:other", "@view_touch_down:other"}) {
        REQUIRE_FALSE(PanZoomViewHostValueFrom(key, states).has_value());
    }
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:pad", states) == 1.0); // "pad" unaffected
}

TEST_CASE("DSMod view touch: a finger down records the primary pointer's canvas position; lift "
          "or cancel clears it",
          "[dsmod][runtime25][view_touch]") {
    ViewState states;
    states["pad"] = ViewTransform{}; // already tracked, as if a prior gesture panned/zoomed it
    // No finger yet: @view_touch_x/y absent, @view_touch_down answers 0 (not absent -- it must
    // tell "no finger" from "finger at 0,0").
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_touch_x:pad", states).has_value());
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_touch_y:pad", states).has_value());
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:pad", states) == 0.0);

    // A finger lands at canvas (123, 456) -- UpdateGestures' own call, with `now.front()`'s
    // already-letterbox-corrected canvas coordinate (see the Fit-letterboxed-panel case below).
    UpdateViewTouch(states, "pad", true, 123, 456);
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_x:pad", states) == Catch::Approx(123.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_y:pad", states) == Catch::Approx(456.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:pad", states) == 1.0);
    // get_i64's fall-through (mod_module_host.cpp): static_cast<s64>(std::llround(*view)).
    REQUIRE(static_cast<s64>(std::llround(*PanZoomViewHostValueFrom("@view_touch_x:pad", states))) ==
            123);

    // Moves: the position tracks the latest tick, not the first.
    UpdateViewTouch(states, "pad", true, 700, 800);
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_x:pad", states) == Catch::Approx(700.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_y:pad", states) == Catch::Approx(800.0));

    // Lift (UpdateGestures clears it before gesture_target.clear(), see mod_input.cpp): absent
    // again, even though "pad" itself is still tracked (its zoom/pan untouched by any of this).
    UpdateViewTouch(states, "pad", false, 0, 0);
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_touch_x:pad", states).has_value());
    REQUIRE_FALSE(PanZoomViewHostValueFrom("@view_touch_y:pad", states).has_value());
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:pad", states) == 0.0);
    REQUIRE(states.at("pad").zoom == Catch::Approx(1.0f));

    // Cancel (a page transition, input_block, etc.): the same call UpdateGestures makes for a
    // lift, so it behaves identically -- recorded again, then cleared.
    UpdateViewTouch(states, "pad", true, 50, 60);
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:pad", states) == 1.0);
    UpdateViewTouch(states, "pad", false, 0, 0);
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:pad", states) == 0.0);
}

TEST_CASE("DSMod view touch: no gesture target (nothing pannable under the finger) touches "
          "nothing",
          "[dsmod][runtime25][view_touch]") {
    ViewState states;
    states["pad"] = ViewTransform{};
    UpdateViewTouch(states, "", true, 10, 20); // UpdateGestures' own call when gesture_target
                                                // is empty: a drag, hold, scroll or blocked
                                                // gesture, or nothing pannable at all
    REQUIRE(states.size() == 1); // no "" entry created
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:pad", states) == 0.0); // "pad" untouched
}

TEST_CASE("DSMod view touch: a second finger (pinch) does not retarget which pointer drives the "
          "position",
          "[dsmod][runtime25][view_touch]") {
    // UpdateGestures calls UpdateViewTouch with `now.front()` every tick regardless of finger
    // count, the same primary-pointer choice the single-finger pan path above it already makes
    // for `now[0]` (and SwipeTracker::Move / HoldTracker::Update already make, passing
    // now.front() through even with a second finger down). This simulates one tick each of a
    // 1-finger stroke, a 2-finger pinch (the primary pointer moves a little, a second finger is
    // now also down but plays no part here), and back to 1 finger: the recorded position always
    // follows the primary pointer's own canvas coordinate, uninterrupted by the finger count.
    ViewState states;
    states["map"] = ViewTransform{};
    UpdateViewTouch(states, "map", true, 100, 100); // finger 1 lands
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_x:map", states) == Catch::Approx(100.0));
    UpdateViewTouch(states, "map", true, 110, 105); // finger 1 moves; finger 2 now also down
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_x:map", states) == Catch::Approx(110.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_y:map", states) == Catch::Approx(105.0));
    UpdateViewTouch(states, "map", true, 120, 108); // finger 2 lifts; finger 1 keeps driving it
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_x:map", states) == Catch::Approx(120.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_down:map", states) == 1.0);
}

TEST_CASE("DSMod view touch: records the canvas-space coordinate a Fit-letterboxed panel touch "
          "converts to",
          "[dsmod][runtime25][view_touch]") {
    // The same conversion UpdateGestures applies before building `now` (mod_input.cpp's
    // to_canvas_x/y lambdas, via TouchRect): panel px -> canvas px through the Fit rect
    // (FitCompanion/PanelToCanvas, proven by aux_routing.cpp's "Companion touch mapping follows
    // the Fit rect"). A 1920x1080 panel showing a 1240x1080 canvas Fit: 340 px bars either side.
    using VideoCore::DSMod::CompanionRatio;
    using VideoCore::DSMod::FitCompanion;
    using VideoCore::DSMod::PanelToCanvas;
    const auto fit = FitCompanion(1920, 1080, 1240, 1080, CompanionRatio::Fit);
    REQUIRE_FALSE(fit.full);
    const s32 canvas_x = PanelToCanvas(960, fit.x, fit.w, 1240); // panel centre
    const s32 canvas_y = PanelToCanvas(540, fit.y, fit.h, 1080);
    REQUIRE(canvas_x == 620);
    REQUIRE(canvas_y == 540);

    ViewState states;
    states["stickpad_0"] = ViewTransform{};
    UpdateViewTouch(states, "stickpad_0", true, canvas_x, canvas_y);
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_x:stickpad_0", states) == Catch::Approx(620.0));
    REQUIRE(PanZoomViewHostValueFrom("@view_touch_y:stickpad_0", states) == Catch::Approx(540.0));
}

// --- Bugfix (the Sandrock portrait pilot, item30b): a canvas_alt switch while a page stays open
// must not leave its pre-switch geometry on screen (ModRuntime::ApplyCanvasVariant, mod_redraw.cpp
// -- bug 1) and a bound Map's base view / pan must track the ACTIVE rect, not the one it was last
// fitted to (GeometryMapDraw::FrameView, mod_ui_map_widget.cpp; ReanchorBasePan,
// mod_map_view_rect.h; ReanchorViewPan, mod_view_default.h -- bug 2).
// -----------------------------------------------------------------------------------------------

TEST_CASE("DSMod canvas_alt bugfix: a page switched mid-session to its alternate canvas computes "
          "the exact same widget/scroll/hit-test geometry as the same page authored for that "
          "canvas from the start -- nothing left over from the canvas shape it was drawn in before",
          "[dsmod][runtime25][canvas_alt][bugfix]") {
    // ModRuntime::ApplyCanvasVariant now re-baselines the same private per-tick caches a page
    // re-entry already does (widget_sig_page, canvas_page, dispatch_page, group_defs_page,
    // ui_signature_valid), so the open page re-lays out on its very next publish instead of only
    // once it is left and reopened -- that part needs a live ModRuntime (a System) to exercise and
    // sits outside this suite's boundary, same as mod_launch_gate.cpp's and
    // map_pan_zoom_host_value.cpp's own header comments. What IS provable here, free of
    // ModRuntime: the data layer those caches get rebuilt FROM (ExpandWidgets, HitTest/
    // HitTestIndex, MeasureScroll, ScrollBarThumb) reads only the LIVE widget/region rect every
    // time, never anything left over from a prior canvas shape -- so once the caches are told to
    // rebuild (the fix), there is nothing in the data layer left to rebuild wrong.
    constexpr s32 RowPitch = 24;
    const auto build = [](bool author_for_portrait) {
        Page page;
        page.id = "map_page";
        ScrollRegion region;
        region.id = "legend";
        if (author_for_portrait) {
            region.rect = {0, 1400, 1080, 96};
        } else {
            region.rect = {0, 1000, 1240, 80};
            region.rect_alt = std::array<s32, 4>{0, 1400, 1080, 96};
        }
        page.scrolls.push_back(region);

        Widget row;
        row.type = WidgetType::Rect;
        row.scroll = "legend";
        row.repeat = 6;
        row.repeat_dy = RowPitch;
        row.on_tap = "legend.tap";
        row.payload = "{i}";
        if (author_for_portrait) {
            row.rect = {10, 1400, 1060, RowPitch - 2};
        } else {
            row.rect = {10, 1000, 1220, RowPitch - 2};
            row.rect_alt = std::array<s32, 4>{10, 1400, 1060, RowPitch - 2};
        }
        page.widgets.push_back(row);

        if (!author_for_portrait) {
            // The exact op ApplyCanvasVariant performs per widget/scroll region (mod_redraw.cpp):
            // commit a mid-session switch to canvas_alt.
            for (Widget& w : page.widgets) {
                if (w.rect_alt) {
                    std::swap(w.rect, *w.rect_alt);
                }
            }
            for (ScrollRegion& r : page.scrolls) {
                if (r.rect_alt) {
                    std::swap(r.rect, *r.rect_alt);
                }
            }
        }
        return page;
    };

    const Page fresh = build(true);
    const Page switched = build(false);
    StateSnapshot snapshot;
    snapshot.ints[ScrollOffsetKey("legend")] = 0;

    const auto fresh_expanded = ExpandWidgets(fresh, snapshot);
    const auto switched_expanded = ExpandWidgets(switched, snapshot);
    REQUIRE(fresh_expanded.size() == switched_expanded.size());
    for (size_t i = 0; i < fresh_expanded.size(); ++i) {
        REQUIRE(fresh_expanded[i].rect == switched_expanded[i].rect);
        REQUIRE(fresh_expanded[i].scroll_clip == switched_expanded[i].scroll_clip);
    }

    // A tap on the (now-portrait) third legend row hits the same expanded element either way.
    const auto& target = switched_expanded[2];
    const s32 tap_x = target.rect[0] + target.rect[2] / 2;
    const s32 tap_y = target.rect[1] + target.rect[3] / 2;
    REQUIRE(HitTest(fresh, snapshot, tap_x, tap_y) == "legend.tap");
    REQUIRE(HitTest(switched, snapshot, tap_x, tap_y) == "legend.tap");
    // Outside the portrait row's narrower width: the landscape row's old 1220 px width (which
    // would have reached this point) is gone, not merely overdrawn.
    REQUIRE(HitTest(switched, snapshot, 1200, tap_y).empty());

    const auto measure_fresh = MeasureScroll(fresh, fresh.scrolls[0], snapshot);
    const auto measure_switched = MeasureScroll(switched, switched.scrolls[0], snapshot);
    REQUIRE(measure_fresh.row_h == measure_switched.row_h);
    REQUIRE(measure_fresh.max_offset == measure_switched.max_offset);
    REQUIRE(ScrollBarThumb(fresh.scrolls[0], measure_fresh, 0) ==
            ScrollBarThumb(switched.scrolls[0], measure_switched, 0));
}

TEST_CASE("DSMod canvas_alt bugfix: the redraw worker's cached Page copy (shared_page / "
          "shared_page_index, mod_runtime.h) must be invalidated on a same-page canvas switch, or "
          "it keeps handing the worker the pre-switch rects (item30c live re-verify)",
          "[dsmod][runtime25][canvas_alt][bugfix]") {
    // The live re-verify on CLI 25-c11739ad1abb (= commit 1f1746fe00) found bug 1 only half
    // fixed: a same-page switch updated scroll limits/selection (the widget_sig_page/canvas_page/
    // dispatch_page/group_defs_page/ui_signature_valid reset from the first bugfix commit) but
    // the DRAWN nav bar and rows stayed at their landscape rect -- fixed only by leaving and
    // reopening the page. Root cause: ModRuntime::PublishUi's dispatch block hands the worker
    // `*job.page_copy`, which is `shared_page` -- an IMMUTABLE COPY of the page, rebuilt only
    // when `shared_page_index != current_page` (mod_redraw.cpp's own "the page never changes
    // until a reload" comment). A canvas_alt switch mutates the LIVE page's widgets in place
    // without current_page changing, so that premise silently broke: the worker kept drawing the
    // stale pre-switch copy indefinitely. None of the four sentinel resets above it touch this
    // cache; only `shared_page_index = ~size_t{0}` (this test's own fix) does.
    //
    // ModRuntime cannot be constructed here (needs a System -- this file's other canvas_alt
    // bugfix test and mod_launch_gate.cpp's own boundary comment), so this reproduces the EXACT
    // caching rule from mod_redraw.cpp's dispatch block against a plain Page, which is all the
    // defect actually needed; ExpandWidgets (free, exported) then stands in for "what the worker
    // draws" at the point job.page_copy's widgets would be expanded.
    struct PageCache {
        std::shared_ptr<const Page> copy;
        size_t index{~size_t{0}};
        const Page& Get(const Page& live, size_t page_index) {
            if (!copy || index != page_index) {
                copy = std::make_shared<const Page>(live);
                index = page_index;
            }
            return *copy;
        }
    };

    Page page;
    page.id = "social";
    Widget nav;
    nav.id = "nav_bar";
    nav.rect = {0, 956, 1240, 124};                        // landscape
    nav.rect_alt = std::array<s32, 4>{0, 1496, 1080, 124}; // portrait
    Widget row;
    row.type = WidgetType::Rect;
    row.id = "row";
    row.repeat = 3;
    row.repeat_dy = 40;
    row.rect = {0, 100, 1200, 36};
    row.rect_alt = std::array<s32, 4>{0, 1500, 1040, 36};
    page.widgets = {nav, row};

    PageCache cache;
    constexpr size_t PageIndex = 2; // "Social page 2" -- the companion's own item30c evidence
    const StateSnapshot snapshot;

    // First ask: builds the cache from the (still landscape) live page -- same as a page's first
    // entry.
    REQUIRE(cache.Get(page, PageIndex).widgets[0].rect == std::array<s32, 4>{0, 956, 1240, 124});

    // The exact per-widget op ApplyCanvasVariant performs on a same-page canvas switch
    // (current_page/PageIndex itself never changes): swap rect <-> rect_alt on the LIVE page.
    for (Widget& w : page.widgets) {
        if (w.rect_alt) {
            std::swap(w.rect, *w.rect_alt);
        }
    }
    REQUIRE(page.widgets[0].rect == std::array<s32, 4>{0, 1496, 1080, 124}); // the live page is
                                                                              // already correct

    // The bug (1f1746fe00): asking for the SAME page index hands back the stale pre-switch copy
    // -- the nav bar's drawn rect, and a repeated row's, are still the landscape ones.
    const auto& still_cached = cache.Get(page, PageIndex);
    REQUIRE(still_cached.widgets[0].rect == std::array<s32, 4>{0, 956, 1240, 124}); // stale
    const auto stale_expanded = ExpandWidgets(still_cached, snapshot);
    REQUIRE(stale_expanded.size() == 1 + 3); // nav + 3 repeated rows
    REQUIRE(stale_expanded[0].rect == std::array<s32, 4>{0, 956, 1240, 124}); // nav bar, stale
    REQUIRE(stale_expanded[1].rect[1] == 100);                               // row 0, stale

    // The fix: ApplyCanvasVariant also invalidates the cache's own index, the same `~size_t{0}`
    // sentinel widget_sig_page/canvas_page/dispatch_page/group_defs_page already use -- so the
    // very next ask for this SAME page index rebuilds the copy from the live, now-swapped page.
    cache.index = ~size_t{0};
    const auto& refreshed = cache.Get(page, PageIndex);
    REQUIRE(refreshed.widgets[0].rect == std::array<s32, 4>{0, 1496, 1080, 124}); // nav, correct
    const auto fixed_expanded = ExpandWidgets(refreshed, snapshot);
    REQUIRE(fixed_expanded[0].rect == std::array<s32, 4>{0, 1496, 1080, 124}); // nav bar, correct
    REQUIRE(fixed_expanded[1].rect[1] == 1500);                               // row 0, correct
}

TEST_CASE("DSMod map view_rect bugfix: a bound rect's fit on the ACTIVE canvas is never the stale "
          "axis's ppw rescaled by the other rect's own height ratio (the Sandrock evidence's "
          "0.7545 = 1.1807 x 956/1496)",
          "[dsmod][runtime25][view_rect][bugfix]") {
    // The companion's own rect/rect_alt (RUNTIME-CHANGE-REQUEST-item30b): landscape 1240x956,
    // portrait 1080x1496. A bound world rect sized so FitWorldRect's landscape fit is exactly the
    // evidence's own 1.1807 ppw (aspect-matched to the landscape rect, so both axes tie).
    constexpr float LandscapePpw = 1.1807f;
    const std::array<float, 4> bound{0.0f, 0.0f, 1240.0f / LandscapePpw, 956.0f / LandscapePpw};
    const MapBaseView landscape = FitWorldRect(bound, 1240, 956);
    REQUIRE(landscape.ppw == Approx(LandscapePpw).margin(0.0005f));

    // The ACTIVE rect's own fit -- what the fix's `shown = target;` always uses on a resize.
    const MapBaseView portrait = FitWorldRect(bound, 1080, 1496);
    const float expected_portrait_ppw =
        std::min(1080.0f / (bound[2] - bound[0]), 1496.0f / (bound[3] - bound[1]));
    REQUIRE(portrait.ppw == Approx(expected_portrait_ppw));

    // The host's pre-fix bug: a frozen landscape ppw rescaled by the portrait/landscape HEIGHT
    // ratio alone, with no fresh FitWorldRect against the active rect at all.
    const float buggy_ppw = LandscapePpw * 956.0f / 1496.0f;
    REQUIRE(buggy_ppw == Approx(0.7545f).margin(0.001f)); // matches the evidence exactly
    REQUIRE(portrait.ppw != Approx(buggy_ppw).margin(0.01f));
}

TEST_CASE("DSMod map view_rect bugfix: ReanchorBasePan keeps the same world point centred when "
          "the base view's own fit changes under it, and switching back restores the original pan",
          "[dsmod][runtime25][view_rect][bugfix]") {
    // A bound rect 2000x1200 world units: landscape 1240x956 is width-constrained, portrait
    // 1080x1496 is height-constrained -- a real axis flip, not just a magnitude change.
    const std::array<float, 4> bound{0.0f, 0.0f, 2000.0f, 1200.0f};
    const MapBaseView landscape_home = FitWorldRect(bound, 1240, 956);
    const MapBaseView portrait_home = FitWorldRect(bound, 1080, 1496);
    REQUIRE(landscape_home.ppw != Approx(portrait_home.ppw)); // the whole point of this fixture
    REQUIRE(landscape_home.cx == Approx(portrait_home.cx));   // FitWorldRect's centre never
    REQUIRE(landscape_home.cy == Approx(portrait_home.cy));   // depends on rw/rh -- only ppw does

    // The user panned/zoomed in landscape: 2x zoom, panned off the bound rect's own centre.
    const float zoom = 2.0f;
    const float pan_x = 180.0f;
    const float pan_y = -60.0f;
    // The effective world point this reproduces under the OLD (landscape) geometry -- the same
    // formula GeometryMapDraw::FrameView applies (mod_ui_map_widget.cpp), replicated here since it
    // is private to that render pass.
    const float old_halfw = 1240.0f * 0.5f, old_halfh = 956.0f * 0.5f;
    const float ecx =
        landscape_home.cx + (pan_x + old_halfw / zoom - old_halfw) / landscape_home.ppw;
    const float ecy =
        landscape_home.cy - (pan_y + old_halfh / zoom - old_halfh) / landscape_home.ppw;

    const auto reanchor = ReanchorBasePan(landscape_home, 1240, 956, pan_x, pan_y, zoom,
                                         portrait_home, 1080, 1496);
    // Plugging the reanchored pan back into the SAME formula, now under the portrait geometry,
    // reproduces the identical world point -- a zoom step right after the switch is centred where
    // the player actually was, not wherever the stale base/pan pairing would have put it.
    const float new_halfw = 1080.0f * 0.5f, new_halfh = 1496.0f * 0.5f;
    const float new_ecx =
        portrait_home.cx + (reanchor.pan_x + new_halfw / zoom - new_halfw) / portrait_home.ppw;
    const float new_ecy =
        portrait_home.cy - (reanchor.pan_y + new_halfh / zoom - new_halfh) / portrait_home.ppw;
    REQUIRE(new_ecx == Approx(ecx));
    REQUIRE(new_ecy == Approx(ecy));

    // Switching back (portrait -> landscape), reanchoring again from the just-reanchored pan,
    // restores the ORIGINAL landscape pan exactly -- "landscape return restores" the evidence's
    // own phrase for it.
    const auto back = ReanchorBasePan(portrait_home, 1080, 1496, reanchor.pan_x, reanchor.pan_y,
                                     zoom, landscape_home, 1240, 956);
    REQUIRE(back.pan_x == Approx(pan_x).margin(0.01));
    REQUIRE(back.pan_y == Approx(pan_y).margin(0.01));
}

TEST_CASE("DSMod view_default bugfix: ReanchorViewPan rescales a pan_zoom Image widget's stored "
          "pan so the same fractional window stays shown after its own rect resizes (canvas_alt)",
          "[dsmod][runtime25][view_touch][bugfix]") {
    ViewTransform v;
    v.zoom = 2.0f;
    v.pan_x = 150.0f; // 150 / 500 = 30% into a 500 px wide landscape rect at zoom 1
    v.pan_y = 80.0f;  // 80 / 400 = 20% into a 400 px tall one
    v.home_pan_x = 10.0f;
    v.goal_pan_x = 150.0f;
    ReanchorViewPan(v, /*old_w=*/500, /*old_h=*/400, /*new_w=*/300, /*new_h=*/600);
    // The identical fraction of the NEW rect's own size: DrawImage (mod_ui.cpp) divides pan by
    // the widget's rect span every frame, with no cross-frame caching of its own -- so keeping
    // the fraction is exactly what keeps the same part of the picture shown.
    REQUIRE(v.pan_x == Approx(90.0f));  // 150 * 300/500
    REQUIRE(v.pan_y == Approx(120.0f)); // 80 * 600/400
    REQUIRE(v.goal_pan_x == Approx(90.0f));
    REQUIRE(v.home_pan_x == Approx(6.0f)); // 10 * 300/500
    REQUIRE(v.zoom == Approx(2.0f));       // dimensionless (a fraction of the picture): untouched

    // No previous rect yet (first size ever seen), or an unchanged size: left alone.
    ViewTransform untouched = v;
    ReanchorViewPan(untouched, 0, 0, 300, 600);
    REQUIRE(untouched.pan_x == Approx(v.pan_x));
    ReanchorViewPan(untouched, 300, 600, 300, 600);
    REQUIRE(untouched.pan_x == Approx(v.pan_x));

    // Switching back restores the original pan exactly.
    ReanchorViewPan(v, 300, 600, 500, 400);
    REQUIRE(v.pan_x == Approx(150.0f));
    REQUIRE(v.pan_y == Approx(80.0f));
}

TEST_CASE("DSMod runtime 25: version and ABI unaffected", "[dsmod][runtime25]") {
    REQUIRE(DualScreenRuntimeVersion >= 25);
    // No module ABI change: ABI stays 1 (rt24 extension 6's hash is a fixed macro this stage
    // never touches, dsmod_module_extensions.h / dsmod_module_abi.h).
    REQUIRE(EDEN_DSMOD_MODULE_ABI_VERSION == 1u);
}
