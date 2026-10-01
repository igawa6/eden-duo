// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Drag-to-scroll lists (Page::scrolls, Widget::scroll): boot-free checks of the pieces the
// runtime and the renderer share -- offset clamp, visible-row materialisation, hit-test index
// mapping through the clip, the "{i}" keep range, and clipped drawing.
#include <catch2/catch_test_macros.hpp>
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {

constexpr s32 RowPitch = 66;

/// An ITEM-page-like list: a 10-row viewport over up to 160 rows (tap cell + name + highlight).
Page ItemPage(bool scrolled = true) {
    Page page;
    ScrollRegion region;
    region.id = "items";
    region.rect = {24, 200, 1192, 660};
    region.count_bind = "item.count";
    region.row_h = RowPitch;
    region.bar_color = 0xFFFF0000u;
    page.scrolls.push_back(region);

    Widget header; // not part of the list: never clipped or shifted
    header.type = WidgetType::Rect;
    header.rect = {0, 0, 1240, 100};
    header.on_tap = "header";
    page.widgets.push_back(header);

    Widget cell;
    cell.type = WidgetType::Rect;
    cell.rect = {24, 200, 1192, 58};
    cell.repeat = 160;
    cell.repeat_bind = "item.count";
    cell.repeat_dy = RowPitch;
    cell.on_tap = "item.select";
    cell.payload = "{i}";
    cell.scroll = scrolled ? "items" : "";
    page.widgets.push_back(cell);

    Widget name = cell;
    name.type = WidgetType::Label;
    name.on_tap.clear();
    name.payload.clear();
    name.bind_text = "item.{i}.name";
    page.widgets.push_back(name);

    Widget lit = cell;
    lit.on_tap.clear();
    lit.payload.clear();
    lit.hide_bind = "item.sel.key";
    lit.keep_min = 0;
    lit.keep_max = 0;
    lit.keep_min_i = true;
    lit.keep_max_i = true;
    lit.id = "lit.{i}";
    page.widgets.push_back(lit);
    return page;
}

StateSnapshot State(s64 count, s64 offset) {
    StateSnapshot s;
    s.ints["item.count"] = count;
    s.ints[ScrollOffsetKey("items")] = offset;
    s.ints["item.sel.key"] = -1;
    return s;
}

/// The tap cells of an expansion (copies: the expansion is usually a temporary).
std::vector<Widget> CellsOf(const std::vector<Widget>& expanded) {
    std::vector<Widget> out;
    for (const auto& w : expanded) {
        if (w.on_tap == "item.select") {
            out.push_back(w);
        }
    }
    return out;
}

s64 TapPayload(const Page& page, const StateSnapshot& s, s32 x, s32 y) {
    const auto expanded = ExpandWidgets(page, s);
    const s64 hit =
        HitTestIndex(expanded, s, x, y, [](const Widget& w) { return !w.on_tap.empty(); });
    if (hit < 0) {
        return -1000;
    }
    const auto& w = expanded[static_cast<size_t>(hit)];
    if (w.on_tap != "item.select") {
        return -2000;
    }
    return WidgetPayload(w, s).value_or(-3000);
}

} // namespace

TEST_CASE("DSMod scroll: content size and offset clamp", "[dsmod][ui][scroll]") {
    const Page page = ItemPage();
    const ScrollRegion& r = page.scrolls[0];
    auto m = MeasureScroll(page, r, State(160, 0));
    REQUIRE(m.count == 160);
    REQUIRE(m.row_h == RowPitch);
    REQUIRE(m.content_h == 160 * RowPitch);
    REQUIRE(m.max_offset == 160 * RowPitch - 660);
    // A list that fits has nothing to scroll.
    REQUIRE(MeasureScroll(page, r, State(10, 0)).max_offset == 0);
    REQUIRE(MeasureScroll(page, r, State(0, 0)).max_offset == 0);
    // Negative offsets clamp to 0; oversized ones to the end (ExpandWidgets applies max_offset).
    REQUIRE(ScrollOffset(r, State(160, -50)) == 0);
    const auto end = ExpandWidgets(page, State(160, 999999));
    const auto cells = CellsOf(end);
    REQUIRE(!cells.empty());
    REQUIRE(cells.back().payload == "159");
    // The last row's slot ends flush with the viewport bottom.
    REQUIRE(cells.back().rect[1] == 200 + 159 * RowPitch - m.max_offset);
    REQUIRE(cells.back().rect[1] + RowPitch == r.rect[1] + r.rect[3]);
    // Count fallback: no count_bind -> the templates' own live repeat count.
    Page fallback = page;
    fallback.scrolls[0].count_bind.clear();
    REQUIRE(MeasureScroll(fallback, fallback.scrolls[0], State(37, 0)).count == 37);
    fallback.scrolls[0].row_h = 0; // and the pitch from the first template
    REQUIRE(MeasureScroll(fallback, fallback.scrolls[0], State(37, 0)).row_h == RowPitch);
}

TEST_CASE("DSMod scroll: only visible rows are materialised", "[dsmod][ui][scroll]") {
    const Page page = ItemPage();
    // Offset 0: rows 0..9 exactly fill 200..860 (row 10 starts at 860 = the bottom edge).
    auto cells = CellsOf(ExpandWidgets(page, State(160, 0)));
    REQUIRE(cells.size() == 10);
    REQUIRE(cells.front().payload == "0");
    REQUIRE(cells.back().payload == "9");
    // Half a row down: row 0 is still partly visible at the top, row 10 partly at the bottom.
    cells = CellsOf(ExpandWidgets(page, State(160, 33)));
    REQUIRE(cells.size() == 11);
    REQUIRE(cells.front().payload == "0");
    REQUIRE(cells.front().rect[1] == 200 - 33);
    REQUIRE(cells.back().payload == "10");
    // Deep in the list: a 160-row list costs like ~11 rows, and {i} is the real index.
    const auto all = ExpandWidgets(page, State(160, 100 * RowPitch + 10));
    cells = CellsOf(all);
    REQUIRE(cells.size() == 11);
    REQUIRE(cells.front().payload == "100");
    REQUIRE(cells.back().payload == "110");
    REQUIRE(all.size() == 1 + 3 * 11); // header + three templates x visible rows
    for (const auto& w : all) {
        if (w.bind_text.starts_with("item.")) {
            REQUIRE(w.scroll_clip == page.scrolls[0].rect);
        }
    }
    REQUIRE(all.front().scroll_clip[2] == 0); // the header is not a list row
    // Without "scroll" the template expands every element, unclipped, as before.
    const auto plain = CellsOf(ExpandWidgets(ItemPage(false), State(160, 5000)));
    REQUIRE(plain.size() == 160);
    REQUIRE(plain[3].rect[1] == 200 + 3 * RowPitch);
    REQUIRE(plain[3].scroll_clip[2] == 0);
}

TEST_CASE("DSMod scroll: VisibleElementRange grid and edge cases", "[dsmod][ui][scroll]") {
    ScrollRegion r;
    r.rect = {0, 100, 300, 200};
    Widget w;
    w.rect = {0, 100, 90, 40};
    w.repeat = 50;
    w.repeat_cols = 3;
    w.repeat_dx = 100;
    w.repeat_row_dy = 50;
    // Rows 0..3 touch [100,300) at offset 0 (row 4 starts at 300) -> cells 0..11.
    REQUIRE(VisibleElementRange(w, r, 100, 0, 50) == std::pair<s64, s64>{0, 12});
    // Offset 45: row 0 spans 55..95 (gone), row 4 spans 255..295 (in).
    REQUIRE(VisibleElementRange(w, r, 100, 45, 50) == std::pair<s64, s64>{3, 15});
    // Clamped to the element count.
    REQUIRE(VisibleElementRange(w, r, 100, 10000, 50) == std::pair<s64, s64>{50, 50});
    REQUIRE(VisibleElementRange(w, r, 100, 0, 0) == std::pair<s64, s64>{0, 0});
    // Rows that do not advance cannot be culled by position.
    Widget flat = w;
    flat.repeat_cols = 0;
    flat.repeat_dy = 0;
    REQUIRE(VisibleElementRange(flat, r, 100, 0, 7) == std::pair<s64, s64>{0, 7});
}

TEST_CASE("DSMod scroll: taps map to the real element through the clip", "[dsmod][ui][scroll]") {
    const Page page = ItemPage();
    // Top of the list.
    REQUIRE(TapPayload(page, State(160, 0), 600, 210) == 0);
    REQUIRE(TapPayload(page, State(160, 0), 600, 200 + 4 * RowPitch + 5) == 4);
    // Scrolled by ten rows: the same spot is element 10, one row lower is 11.
    REQUIRE(TapPayload(page, State(160, 10 * RowPitch), 600, 210) == 10);
    REQUIRE(TapPayload(page, State(160, 10 * RowPitch), 600, 210 + RowPitch) == 11);
    // The gap between rows (58..66) is not a row.
    REQUIRE(TapPayload(page, State(160, 0), 600, 200 + 60) == -1000);
    // Offset 33: row 0 is drawn 167..225, but only its part inside the viewport is tappable.
    REQUIRE(TapPayload(page, State(160, 33), 600, 190) == -1000);
    REQUIRE(TapPayload(page, State(160, 33), 600, 205) == 0);
    // The header above the viewport still gets its own taps.
    REQUIRE(TapPayload(page, State(160, 33), 600, 50) == -2000);
    // Near the end of a shorter list.
    const auto s = State(25, 999999); // max = 25*66-660 = 990
    REQUIRE(TapPayload(page, s, 600, 200 + 9 * RowPitch + 5) == 24);
    REQUIRE(TapPayload(page, s, 600, 210) == 15);
    // HitTest (the plain path) agrees: nothing under the viewport's top edge from the list.
    REQUIRE(HitTest(page, State(160, 33), 600, 190).empty());
    REQUIRE(HitTest(page, State(160, 33), 600, 205) == "item.select");
}

TEST_CASE("DSMod scroll: keep range follows the element index", "[dsmod][ui][scroll]") {
    const Page page = ItemPage();
    auto s = State(160, 10 * RowPitch);
    s.ints["item.sel.key"] = 12;
    const auto expanded = ExpandWidgets(page, s);
    s64 lit = -1;
    int shown = 0;
    for (const auto& w : expanded) {
        if (w.id.starts_with("lit.") && !WidgetHidden(w, s)) {
            ++shown;
            lit = std::stoll(w.id.substr(4));
        }
    }
    REQUIRE(shown == 1);
    REQUIRE(lit == 12);
    // "{i}+1": lit when the key is one past the row.
    Page shifted = page;
    shifted.widgets[3].keep_min = 1;
    shifted.widgets[3].keep_max = 1;
    shown = 0;
    for (const auto& w : ExpandWidgets(shifted, s)) {
        if (w.id.starts_with("lit.") && !WidgetHidden(w, s)) {
            ++shown;
            REQUIRE(w.id == "lit.11");
        }
    }
    REQUIRE(shown == 1);
}

TEST_CASE("DSMod scroll: packed lists scroll by placed cell", "[dsmod][ui][scroll]") {
    Page page = ItemPage();
    Widget& cell = page.widgets[1];
    cell.pack = true;
    cell.hide_bind = "item.{i}.keep";
    cell.keep_min = 1;
    page.widgets.resize(2);
    StateSnapshot s = State(160, RowPitch * 2); // two placed rows scrolled away
    for (int i = 0; i < 160; ++i) {
        s.ints["item." + std::to_string(i) + ".keep"] = i % 2; // odd elements kept
    }
    const auto cells = CellsOf(ExpandWidgets(page, s));
    REQUIRE(cells.size() == 10);
    REQUIRE(cells.front().payload == "5"); // placed cell 2 = element 5
    REQUIRE(cells.front().rect[1] == 200);
    REQUIRE(cells.back().payload == "23");
}

TEST_CASE("DSMod scroll: rows draw clipped to the viewport, bar follows", "[dsmod][ui][scroll]") {
    constexpr u32 Bg = 0xFF000000u;
    constexpr u32 Row = 0xFF00FF00u;
    constexpr u32 Bar = 0xFFFF0000u;
    Manifest manifest;
    manifest.background = Bg;
    Page page;
    ScrollRegion region;
    region.id = "l";
    region.rect = {0, 20, 100, 40};
    region.bar_color = Bar;
    region.bar_w = 4;
    page.scrolls.push_back(region);
    Widget row;
    row.type = WidgetType::Rect;
    row.rect = {0, 20, 90, 10};
    row.bg = Row;
    row.color = 0;
    row.repeat = 10;
    row.repeat_dy = 10;
    row.scroll = "l";
    page.widgets.push_back(row);
    StateSnapshot s;
    s.ints[ScrollOffsetKey("l")] = 5;
    s.ints["@scroll_on:l"] = 1;
    Canvas canvas;
    canvas.Resize(100, 100);
    REQUIRE(RenderPage(canvas, manifest, page, s));
    const auto px = [&](s32 x, s32 y) { return canvas.Pixels()[static_cast<size_t>(y) * 100 + x]; };
    REQUIRE(px(50, 17) == Bg); // row 0 is at 15..25: its part above the viewport is clipped
    REQUIRE(px(50, 21) == Row);
    REQUIRE(px(50, 59) == Row); // row 4 at 55..65, visible up to the viewport bottom
    REQUIRE(px(50, 62) == Bg);  // ...and clipped below it
    // Content 100, viewport 40 -> max 60; thumb max(16, 24) = 24 px at 20 + (40-24)*5/60 = 21.
    REQUIRE(px(97, 19) != Bar);
    REQUIRE(px(97, 21) == Bar);
    REQUIRE(px(97, 44) == Bar);
    REQUIRE(px(97, 46) != Bar);
    REQUIRE(px(97, 23) == Bar);
    REQUIRE(px(97, 37) == Bar);
    REQUIRE(px(97, 50) != Bar);
    // No bar while the runtime reports the region idle.
    s.ints["@scroll_on:l"] = 0;
    REQUIRE(RenderPage(canvas, manifest, page, s));
    REQUIRE(px(97, 23) != Bar);
}

TEST_CASE("DSMod repeat: {i+N} / {i-N} index expressions", "[dsmod][ui][scroll]") {
    Page page = ItemPage();
    page.widgets[1].payload = "{i+1000}";
    page.widgets[1].on_tap = "item.select";
    page.widgets[2].bind_text = "tab.{i-1}.name.{i}.{x}";
    const auto s = State(160, 10 * RowPitch);
    REQUIRE(TapPayload(page, s, 600, 210) == 1010);
    for (const auto& w : ExpandWidgets(page, s)) {
        if (w.type == WidgetType::Label) {
            REQUIRE(w.bind_text.starts_with("tab."));
            const s64 i = std::stoll(w.bind_text.substr(w.bind_text.find(".name.") + 6));
            REQUIRE(w.bind_text ==
                    "tab." + std::to_string(i - 1) + ".name." + std::to_string(i) + ".{x}");
        }
    }
}

TEST_CASE("DSMod scroll: a label half above the viewport draws its visible glyphs, clipped",
          "[dsmod][ui][scroll]") {
    // The label's rect has no height (the usual "rect":[x,y,0,0]); its text is 10 px tall. With
    // the list scrolled 5 px, row 0's text spans 15..25 across the viewport top at 20: culling by
    // its rect alone dropped it while an image row beside it (with a real height) still drew.
    constexpr u32 Bg = 0xFF000000u;
    constexpr u32 Txt = 0xFFFFFFFFu;
    Manifest manifest;
    manifest.background = Bg;
    Page page;
    ScrollRegion region;
    region.id = "l";
    region.rect = {0, 20, 100, 40};
    page.scrolls.push_back(region);
    Widget label;
    label.type = WidgetType::Label;
    label.rect = {2, 20, 0, 0};
    label.text = "HHHH";
    label.text_scale = 2;
    label.color = Txt;
    label.repeat = 4;
    label.repeat_dy = 20;
    label.scroll = "l";
    page.widgets.push_back(label);
    StateSnapshot s;
    s.ints[ScrollOffsetKey("l")] = 5;

    // Row 0 is materialised although its rect's top is above the viewport.
    const auto expanded = ExpandWidgets(page, s);
    REQUIRE(!expanded.empty());
    REQUIRE(expanded.front().rect[1] == 15);
    // The span a label paints, relative to its rect's top; nothing extra for other widgets.
    const auto span = RowPaintSpan(label, s, 40);
    REQUIRE(span[0] <= 0);
    REQUIRE(span[1] >= 10);
    Widget rect_w;
    rect_w.type = WidgetType::Rect;
    REQUIRE(RowPaintSpan(rect_w, s, 40) == std::array<s32, 2>{0, 0});

    Canvas canvas;
    canvas.Resize(100, 100);
    REQUIRE(RenderPage(canvas, manifest, page, s));
    const auto px = [&](s32 x, s32 y) { return canvas.Pixels()[static_cast<size_t>(y) * 100 + x]; };
    const auto any_text = [&](s32 y0, s32 y1) {
        for (s32 y = y0; y < y1; ++y) {
            for (s32 x = 0; x < 100; ++x) {
                if (px(x, y) == Txt) {
                    return true;
                }
            }
        }
        return false;
    };
    REQUIRE(any_text(20, 25));        // row 0's lower half shows inside the viewport
    REQUIRE_FALSE(any_text(0, 20));   // nothing above the viewport
    REQUIRE_FALSE(any_text(60, 100)); // nothing below it
    REQUIRE(any_text(35, 45));        // row 1 (35..45) whole
    // Scrolled so row 0 is far above the viewport: still culled (fully outside costs nothing).
    s.ints[ScrollOffsetKey("l")] = 40; // row 0 at -20, row 1 at 0 (its text reaches 20..)
    const auto later = ExpandWidgets(page, s);
    REQUIRE(!later.empty());
    REQUIRE(later.front().rect[1] == 0);
}
