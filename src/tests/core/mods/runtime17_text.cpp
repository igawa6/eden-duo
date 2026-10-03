// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 17 text features: "auto_w" (Label/Button boxes sized by their text + "pad"), "group" /
// "group_sep" (thousands separators on Value numbers), "{i}" in the remaining repeat fields,
// max_lines + ellipsis, and paged font atlases ("font_page_h", FontPages). The built-in font is
// 4 px per glyph at scale 1.
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_font_pages.h"
#include "core/mods/mod_msbt.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
constexpr u32 White = 0xFFFFFFFFu;

Image Solid(u32 w, u32 h, u32 argb) {
    Image image;
    image.w = w;
    image.h = h;
    image.pixels.assign(static_cast<size_t>(w) * h, argb);
    return image;
}

u32 PixelAt(const Canvas& canvas, u32 x, u32 y) {
    return canvas.Pixels()[y * canvas.Width() + x];
}

Manifest Parse(const char* text) {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(text), m));
    return m;
}

Page OnePage(const Widget& w) {
    Page page;
    page.id = "p";
    page.widgets.push_back(w);
    return page;
}

std::vector<u32> Render(const Page& page, const StateSnapshot& s = {}, u32 w = 120,
                        u32 h = 40) {
    Canvas canvas;
    canvas.Resize(w, h);
    REQUIRE(RenderPage(canvas, Manifest{}, page, s));
    return canvas.Pixels();
}
} // namespace

// --- group / group_sep --------------------------------------------------------------------------

TEST_CASE("DSMod r17 group: thousands separators", "[dsmod][r17][group]") {
    REQUIRE(FormatGroupedNumber(0, 0, ",") == "0");
    REQUIRE(FormatGroupedNumber(7, 0, ",") == "7");
    REQUIRE(FormatGroupedNumber(999, 0, ",") == "999");
    REQUIRE(FormatGroupedNumber(1000, 0, ",") == "1,000");
    REQUIRE(FormatGroupedNumber(99999, 0, ",") == "99,999");
    REQUIRE(FormatGroupedNumber(1234567, 0, ",") == "1,234,567");
    // negative numbers: the sign never takes a separator
    REQUIRE(FormatGroupedNumber(-5, 0, ",") == "-5");
    REQUIRE(FormatGroupedNumber(-999, 0, ",") == "-999");
    REQUIRE(FormatGroupedNumber(-1000, 0, ",") == "-1,000");
    REQUIRE(FormatGroupedNumber(-123456, 0, ",") == "-123,456");
    REQUIRE(FormatGroupedNumber(std::numeric_limits<s64>::min(), 0, ",") ==
            "-9,223,372,036,854,775,808");
    // other separators (a period, a thin space, none)
    REQUIRE(FormatGroupedNumber(12345, 0, ".") == "12.345");
    REQUIRE(FormatGroupedNumber(12345, 0, "\xE2\x80\x89") == "12\xE2\x80\x89" "345");
    REQUIRE(FormatGroupedNumber(12345, 0, "") == "12345");
    // zero padding first, like %0*lld (the sign counts toward the width)
    REQUIRE(FormatGroupedNumber(5, 3, ",") == "005");
    REQUIRE(FormatGroupedNumber(1234, 6, ",") == "001,234");
    REQUIRE(FormatGroupedNumber(-5, 3, ",") == "-05");
}

TEST_CASE("DSMod r17 group: manifest keys and the drawn value", "[dsmod][r17][group]") {
    const Manifest m = Parse(R"({"format":1,"min_runtime":17,"pages":[{"id":"p","widgets":[
        {"type":"value","rect":[2,2,0,0],"bind":"n","text_scale":1,"group":true},
        {"type":"value","rect":[2,2,0,0],"bind":"n","text_scale":1,"group":true,"group_sep":"."},
        {"type":"value","rect":[2,2,0,0],"bind":"n","text_scale":1,"group":"yes"},
        {"type":"value","rect":[2,2,0,0],"bind":"n","text_scale":1,"max_bind":"m",
         "max_sep":"/","group":true}]}]})");
    const auto& w = m.pages[0].widgets;
    REQUIRE(w[0].group);
    REQUIRE(w[0].group_sep == ",");
    REQUIRE(w[1].group_sep == ".");
    REQUIRE_FALSE(w[2].group); // not a boolean: ignored
    StateSnapshot s;
    s.ints["n"] = -12345;
    s.ints["m"] = 99000;
    const auto as_label = [&](const std::string& text) {
        Widget label;
        label.type = WidgetType::Label;
        label.rect = {2, 2, 0, 0};
        label.text_scale = 1;
        label.text = text;
        return Render(OnePage(label));
    };
    REQUIRE(Render(OnePage(w[0]), s) == as_label("-12,345"));
    REQUIRE(Render(OnePage(w[1]), s) == as_label("-12.345"));
    REQUIRE(Render(OnePage(w[2]), s) == as_label("-12345"));
    REQUIRE(Render(OnePage(w[3]), s) == as_label("-12,345/99,000"));
    // a small number takes no separator
    s.ints["n"] = 42;
    REQUIRE(Render(OnePage(w[0]), s) == as_label("42"));
}

// --- auto_w ---------------------------------------------------------------------------------------

TEST_CASE("DSMod r17 auto_w: button and label boxes", "[dsmod][r17][auto_w]") {
    const Manifest m = Parse(R"({"format":1,"min_runtime":17,"pages":[{"id":"p","widgets":[
        {"type":"button","rect":[100,10,20,30],"text":"ABC","text_scale":1,"align":"center",
         "auto_w":true,"pad":5,"on_tap":"t"},
        {"type":"button","rect":[100,10,20,30],"text":"ABC","text_scale":1,"auto_w":true,"pad":5},
        {"type":"button","rect":[100,10,40,30],"text":"ABC","text_scale":1,"align":"center",
         "auto_w":true,"pad":5},
        {"type":"label","rect":[50,20,0,0],"text":"AB","text_scale":2,"auto_w":true,"pad":3},
        {"type":"label","rect":[50,20,0,0],"text":"AB","text_scale":2,"auto_w":true,"pad":3,
         "align":"center"},
        {"type":"label","rect":[50,20,0,0],"text":"AB","text_scale":2,"auto_w":true,"pad":3,
         "align":"right"},
        {"type":"button","rect":[0,0,20,30],"text":"ABC","text_scale":1}]}],
        "actions":{"t":{"kind":"flag","flag":"x"}}})");
    const StateSnapshot s;
    // Without a measuring scope the declared rects stand.
    {
        const auto plain = ExpandWidgets(m.pages[0], s);
        REQUIRE(plain[0].rect == std::array<s32, 4>{100, 10, 20, 30});
        REQUIRE_FALSE(plain[0].auto_box);
    }
    Canvas canvas;
    const TextMeasureScope scope{canvas, nullptr};
    const auto e = ExpandWidgets(m.pages[0], s);
    // centred caption: 12 px of text + 2 * 5 pad = 22, about the declared centre (110)
    REQUIRE(e[0].rect == std::array<s32, 4>{99, 10, 22, 30});
    REQUIRE(e[0].auto_box);
    // left caption: inset 12 + text 12 + pad 5, from the left edge
    REQUIRE(e[1].rect == std::array<s32, 4>{100, 10, 29, 30});
    // the declared w is the minimum
    REQUIRE(e[2].rect == std::array<s32, 4>{100, 10, 40, 30});
    // label: 2 glyphs * 8 px + 2 * 3 pad, around the text at its anchor; cap 10 + 2 * 3 tall
    REQUIRE(e[3].rect == std::array<s32, 4>{47, 17, 22, 16});
    REQUIRE(e[4].rect == std::array<s32, 4>{39, 17, 22, 16});
    REQUIRE(e[5].rect == std::array<s32, 4>{31, 17, 22, 16});
    for (size_t i = 3; i < 6; ++i) {
        REQUIRE(AutoBoxTextAnchor(e[i], e[i].rect[0], e[i].rect[1], e[i].rect[2]) ==
                std::array<s32, 2>{50, 20});
    }
    // without auto_w nothing changes
    REQUIRE(e[6].rect == std::array<s32, 4>{0, 0, 20, 30});
    // the measured box is what a tap hits
    REQUIRE(HitTest(m.pages[0], s, 99, 12) == "t");
    REQUIRE(HitTest(m.pages[0], s, 98, 12).empty());
    REQUIRE(HitTest(m.pages[0], s, 120, 12) == "t");
    REQUIRE(HitTest(m.pages[0], s, 121, 12).empty());
}

TEST_CASE("DSMod r17 auto_w: label text follows its bind, wraps, draws its bg",
          "[dsmod][r17][auto_w]") {
    Widget label;
    label.type = WidgetType::Label;
    label.rect = {10, 10, 0, 0};
    label.text_scale = 1;
    label.bind_text = "name";
    label.auto_w = true;
    label.pad = 2;
    label.on_tap = "t";
    Page page = OnePage(label);
    StateSnapshot s;
    s.texts["name"] = "ABCD";
    Canvas canvas;
    {
        const TextMeasureScope scope{canvas, nullptr};
        REQUIRE(ExpandWidgets(page, s)[0].rect == std::array<s32, 4>{8, 8, 20, 9});
        s.texts["name"] = "AB";
        REQUIRE(ExpandWidgets(page, s)[0].rect == std::array<s32, 4>{8, 8, 12, 9});
        // a repeat row re-measures on every expansion (its slot is reused)
        Widget row = label;
        row.repeat = 2;
        row.repeat_dy = 20;
        row.bind_text = "row{i}";
        Page rows = OnePage(row);
        s.texts["row0"] = "A";
        s.texts["row1"] = "ABC";
        std::vector<Widget> slots;
        std::vector<s64> index;
        REQUIRE(ExpandRepeatTemplateInto(rows, row, s, slots, &index) == 2);
        REQUIRE(slots[0].rect[2] == 8);
        REQUIRE(slots[1].rect[2] == 16);
        s.texts["row1"] = "ABCDE";
        REQUIRE(ExpandRepeatTemplateInto(rows, row, s, slots, &index) == 2);
        REQUIRE(slots[1].rect == std::array<s32, 4>{8, 28, 24, 9});
        // wrapped: the widest line, every line tall (pitch 8)
        Widget wrapped = label;
        wrapped.bind_text.clear();
        wrapped.text = "AAAA BB CCC";
        wrapped.wrap_width = 18;
        REQUIRE(ExpandWidgets(OnePage(wrapped), s)[0].rect ==
                std::array<s32, 4>{8, 8, 16 + 4, 8 * 2 + 5 + 4});
    }
    // Drawn: the text where the same label without auto_w draws it; a bg goes behind it.
    s.texts["name"] = "AB";
    Widget plain = label;
    plain.auto_w = false;
    REQUIRE(Render(page, s) == Render(OnePage(plain), s));
    page.widgets[0].bg = 0xFF102030u;
    canvas.Resize(120, 40);
    REQUIRE(RenderPage(canvas, Manifest{}, page, s));
    const u32 page_bg = Manifest{}.background;
    REQUIRE(PixelAt(canvas, 8, 8) == 0xFF102030u);
    REQUIRE(PixelAt(canvas, 19, 16) == 0xFF102030u);
    REQUIRE(PixelAt(canvas, 20, 16) == page_bg);
    REQUIRE(PixelAt(canvas, 7, 8) == page_bg);
    REQUIRE(PixelAt(canvas, 11, 10) == label.color); // 'A' row 0 = .X. at the anchor (10, 10)
    REQUIRE(PixelAt(canvas, 10, 10) == 0xFF102030u);
}

// --- {i} --------------------------------------------------------------------------------------

TEST_CASE("DSMod r17 repeat substitutes {i} in every remaining string field",
          "[dsmod][r17][repeat]") {
    Widget row;
    row.type = WidgetType::Map;
    row.repeat = 2;
    row.names = {"n{i}", "plain"};
    row.hidden_icons = {"h{i}"};
    row.src_thresholds = {{5, "s{i}"}, {9, "t"}};
    row.src_format = "f{i}_%d";
    row.group_sep = "{i}";
    row.area_bind = "a{i}";
    row.room_bind = "r{i}";
    row.area = "area{i+1}";
    row.marker_x_bind = "mx{i}";
    row.marker_y_bind = "my{i}";
    row.marker_icon = "mi{i}";
    row.marker_src = "ms{i}";
    row.actor_x_bind = "ax{i}";
    row.actor_y_bind = "ay{i}";
    row.actor_icon = "ai{i}";
    auto vd = std::make_shared<ViewDefaultBinds>();
    vd->zoom_bind = "z{i}";
    vd->cx_bind = "cx";
    row.view_default = vd;
    row.scroll = "{i}"; // places the template: never substituted
    const auto e = ExpandWidgets(OnePage(row), StateSnapshot{});
    REQUIRE(e.size() == 2);
    const Widget& w = e[1];
    REQUIRE(w.names == std::vector<std::string>{"n1", "plain"});
    REQUIRE(w.hidden_icons == std::vector<std::string>{"h1"});
    REQUIRE(w.src_thresholds[0].second == "s1");
    REQUIRE(w.src_thresholds[1].second == "t");
    REQUIRE(w.src_format == "f1_%d");
    REQUIRE(w.group_sep == "1");
    REQUIRE(w.area_bind == "a1");
    REQUIRE(w.room_bind == "r1");
    REQUIRE(w.area == "area2");
    REQUIRE(w.marker_x_bind == "mx1");
    REQUIRE(w.marker_y_bind == "my1");
    REQUIRE(w.marker_icon == "mi1");
    REQUIRE(w.marker_src == "ms1");
    REQUIRE(w.actor_x_bind == "ax1");
    REQUIRE(w.actor_y_bind == "ay1");
    REQUIRE(w.actor_icon == "ai1");
    REQUIRE(w.view_default->zoom_bind == "z1");
    REQUIRE(w.view_default->cx_bind == "cx");
    REQUIRE(e[0].view_default->zoom_bind == "z0");
    REQUIRE(row.view_default->zoom_bind == "z{i}"); // the template's own copy is untouched
    REQUIRE(w.scroll == "{i}");
}

// --- max_lines + ellipsis -------------------------------------------------------------------------

TEST_CASE("DSMod r17 max_lines ends the last kept line in an ellipsis", "[dsmod][r17][wrap]") {
    Canvas canvas;
    // 4 px per glyph: "one two" is 28 px, a 30 px line.
    auto lines = canvas.LayoutText("one two three four five", 1, 30, 2);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] == "one two");
    REQUIRE(lines[1].ends_with("..."));
    REQUIRE(canvas.MeasureText(lines[1], 1) <= 30);
    // everything fits: no ellipsis
    lines = canvas.LayoutText("one two", 1, 30, 2);
    REQUIRE(lines == std::vector<std::string>{"one two"});
    // hard line breaks only (no wrap width)
    lines = canvas.LayoutText("a\nb\nc", 1, 0, 2);
    REQUIRE(lines == std::vector<std::string>{"a", "b..."});
    // a font with U+2026 uses that glyph
    FontMetrics font;
    font.line_height = 5;
    font.glyphs.assign(96, FontGlyph{0, 0, 1, 1, 0, 1, 4});
    font.extra[0x2026] = FontGlyph{0, 0, 1, 1, 0, 1, 4};
    const Image atlas = Solid(4, 4, White);
    canvas.SetFont(&atlas, &font);
    lines = canvas.LayoutText("one two three four five", 1, 30, 2);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[1].ends_with("\xE2\x80\xA6"));
    REQUIRE(canvas.MeasureText(lines[1], 1) <= 30);
    // an auto_w label measures the ellipsised lines it draws
    canvas.SetFont(nullptr, nullptr);
    Widget label;
    label.type = WidgetType::Label;
    label.rect = {0, 0, 0, 0};
    label.text_scale = 1;
    label.text = "one two three four five";
    label.wrap_width = 30;
    label.max_lines = 2;
    label.auto_w = true;
    const TextMeasureScope scope{canvas, nullptr};
    const auto e = ExpandWidgets(OnePage(label), StateSnapshot{});
    REQUIRE(e[0].rect[2] <= 30);
    REQUIRE(e[0].rect[3] == 8 + 5);
}

// --- font pages -----------------------------------------------------------------------------------

namespace {
/// A loader of 64x64 pages, page p filled with colour 0xFF000000 | p.
FontPages::Loader CountingLoader(int& calls) {
    return [&calls](u32 page, Image& out) {
        ++calls;
        out = Solid(64, 64, 0xFF000000u | page);
        return true;
    };
}
constexpr size_t PageBytes = 64 * 64 * 4;
} // namespace

TEST_CASE("DSMod r17 font pages: LRU within the budget", "[dsmod][r17][font_pages]") {
    int calls = 0;
    FontPages pages{CountingLoader(calls), 3 * PageBytes, true, 0};
    for (u32 p = 0; p < 10; ++p) {
        const auto img = pages.Get(p);
        REQUIRE(img != nullptr);
        REQUIRE(img->pixels[0] == (0xFF000000u | p));
        REQUIRE(pages.ResidentBytes() <= 3 * PageBytes);
    }
    REQUIRE(calls == 10);
    REQUIRE(pages.Loads() == 10);
    REQUIRE(pages.Evictions() == 7);
    REQUIRE(pages.ResidentPages() == 3);
    REQUIRE(pages.Resident(9));
    REQUIRE_FALSE(pages.Resident(0));
    // a hit is not a load; using page 7 keeps it over page 8 at the next eviction
    REQUIRE(pages.Get(7) != nullptr);
    REQUIRE(calls == 10);
    REQUIRE(pages.Get(0) != nullptr);
    REQUIRE(pages.Resident(7));
    REQUIRE_FALSE(pages.Resident(8));
    REQUIRE(pages.TakeLanded());
    REQUIRE_FALSE(pages.TakeLanded());
    // a page past the page limit is never asked for
    REQUIRE(pages.Get(FontPages::MaxPages) == nullptr);
    REQUIRE(calls == 11);
}

TEST_CASE("DSMod r17 font pages: pages in use are never evicted, failures back off",
          "[dsmod][r17][font_pages]") {
    int calls = 0;
    // every page protected for a long time: a third page does not fit
    FontPages pages{CountingLoader(calls), 2 * PageBytes, true, 60000};
    REQUIRE(pages.Get(0) != nullptr);
    REQUIRE(pages.Get(1) != nullptr);
    REQUIRE(pages.Get(2) == nullptr);
    REQUIRE(pages.Refusals() == 1);
    REQUIRE(pages.ResidentPages() == 2);
    REQUIRE(pages.Get(2) == nullptr); // backing off: not even asked again yet
    REQUIRE(calls == 3);
    // a page larger than the whole budget
    FontPages small{CountingLoader(calls), PageBytes / 2, true, 0};
    REQUIRE(small.Get(0) == nullptr);
    REQUIRE(small.Refusals() == 1);
    // a loader that fails
    int failed = 0;
    FontPages broken{[&failed](u32, Image&) {
                         ++failed;
                         return false;
                     },
                     FontPages::DefaultBudget, true, 0};
    REQUIRE(broken.Get(0) == nullptr);
    REQUIRE(broken.Get(0) == nullptr);
    REQUIRE(failed == 1);
}

TEST_CASE("DSMod r17 font pages: asynchronous load and shutdown", "[dsmod][r17][font_pages]") {
    int calls = 0;
    FontPages pages{CountingLoader(calls)};
    REQUIRE(pages.Budget() == FontPages::DefaultBudget);
    REQUIRE(pages.Get(3) == nullptr); // queued
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!pages.Resident(3) && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    REQUIRE(pages.Resident(3));
    REQUIRE(pages.TakeLanded());
    REQUIRE(pages.Get(3) != nullptr);
    pages.Shutdown();
    REQUIRE(pages.Get(3) != nullptr); // resident pages stay usable
    REQUIRE(pages.Get(4) == nullptr); // nothing new loads
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    REQUIRE_FALSE(pages.Resident(4));
}

TEST_CASE("DSMod r17 font pages: glyphs drawn from their pages, many glyphs, eviction",
          "[dsmod][r17][font_pages]") {
    // 200 glyphs, one 8-row band each, 10 bands per page: 20 pages of 8 x 80 px. Page p's image
    // is solid colour 0xFF0000xx with xx = p + 1.
    FontMetrics font;
    font.line_height = 5;
    font.first_codepoint = 0x4E00;
    font.page_h = 80;
    for (u32 i = 0; i < 200; ++i) {
        font.glyphs.push_back(
            FontGlyph{0, static_cast<u16>(i * 8), 4, 5, 0, 5, 5}); // page i / 10, row (i % 10) * 8
    }
    int calls = 0;
    FontPages pages{[&calls](u32 page, Image& out) {
                        ++calls;
                        out = Solid(8, 80, 0xFF000000u | (page + 1));
                        return true;
                    },
                    4 * 8 * 80 * 4, true, 0};
    Canvas canvas;
    canvas.Resize(1200, 10);
    canvas.SetFont(nullptr, &font);
    REQUIRE_FALSE(canvas.HasFont()); // a paged font needs its pages
    canvas.SetFontPages(&pages);
    REQUIRE(canvas.HasFont());
    std::string text;
    for (u32 i = 0; i < 200; ++i) {
        Msbt::AppendUtf8(text, 0x4E00 + i);
    }
    REQUIRE(canvas.MeasureText(text, 1) == 200 * 5);
    canvas.DrawText(0, 0, text, 1, White);
    REQUIRE_FALSE(canvas.TakeGlyphsPending());
    for (u32 i = 0; i < 200; i += 17) {
        REQUIRE(PixelAt(canvas, i * 5 + 1, 2) == (0xFF000000u | (i / 10 + 1)));
    }
    REQUIRE(calls == 20); // each page loaded once for the run (glyphs are in page order)
    REQUIRE(pages.ResidentPages() <= 4);
    REQUIRE(pages.Evictions() == 16);
    // Out of order: every glyph of page 0 and 19 alternating -- two pages, both kept.
    std::string mixed;
    for (u32 k = 0; k < 10; ++k) {
        Msbt::AppendUtf8(mixed, 0x4E00 + k);
        Msbt::AppendUtf8(mixed, 0x4E00 + 190 + k);
    }
    canvas.Clear(0);
    canvas.DrawText(0, 0, mixed, 1, White);
    REQUIRE(PixelAt(canvas, 1, 2) == 0xFF000001u);
    REQUIRE(PixelAt(canvas, 6, 2) == 0xFF000014u);
    REQUIRE(pages.ResidentBytes() <= pages.Budget());
    // A page still loading: its glyphs draw blank and the canvas says so.
    FontPages slow{[](u32, Image& out) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        out = Solid(8, 80, White);
        return true;
    }};
    canvas.SetFontPages(&slow);
    canvas.Clear(0);
    canvas.DrawText(0, 0, mixed, 1, White);
    REQUIRE(canvas.TakeGlyphsPending());
    REQUIRE(PixelAt(canvas, 1, 2) == 0u);
    canvas.SetFontPages(nullptr);
    slow.Shutdown();
}

TEST_CASE("DSMod r17 font_page_h manifest key", "[dsmod][r17][font_pages]") {
    const Manifest m = Parse(R"({"format":1,"min_runtime":17,"font":"module:metrics",
        "font_atlas":"module:font_{p}","font_page_h":1024,"pages":[{"id":"p","widgets":[]}]})");
    REQUIRE(m.font_page_h == 1024);
    REQUIRE(Parse(R"({"format":1,"font_page_h":-4,"pages":[{"id":"p","widgets":[]}]})")
                .font_page_h == 0);
    REQUIRE(Parse(R"({"format":1,"pages":[{"id":"p","widgets":[]}]})").font_page_h == 0);
}

TEST_CASE("DSMod runtime18 parses null text and decimal coordinates", "[dsmod][runtime18]") {
    const auto m = Parse(R"({"pages":[{"id":"p","widgets":[
        {"type":"label","text":null,"rect":[1.5,-2.5,10,10]},
        {"type":"rect","rect":[0,0,5,5],"repeat":2,"repeat_dx":12.5,"repeat_dy":-4.5}
    ]}]})");
    REQUIRE(m.pages[0].widgets[0].text.empty());
    REQUIRE(m.pages[0].widgets[0].rect[0] == 2);
    REQUIRE(m.pages[0].widgets[0].rect[1] == -3);
    auto expanded = ExpandWidgets(m.pages[0], StateSnapshot{});
    REQUIRE(expanded[2].rect[0] == 13);
    REQUIRE(expanded[2].rect[1] == -5);
}

TEST_CASE("DSMod runtime18 negated repeat gates control paint and taps", "[dsmod][runtime18]") {
    const auto m = Parse(R"({"pages":[{"id":"p","widgets":[
        {"type":"rect","rect":[0,0,8,8],"bg":"#FFFFFFFF","repeat":2,"repeat_dx":10,
         "need_bind":"!row{i}.on","on_tap":"tap{i}"}
    ]}]})");
    StateSnapshot s;
    s.ints["row0.on"] = 0;
    s.ints["row1.on"] = 1;
    REQUIRE(HitTest(m.pages[0], s, 1, 1) == "tap0");
    REQUIRE(HitTest(m.pages[0], s, 11, 1).empty());
    Canvas c;
    c.Resize(30, 10);
    REQUIRE(RenderPage(c, m, m.pages[0], s));
    REQUIRE(PixelAt(c, 4, 4) == White);
    REQUIRE(PixelAt(c, 14, 4) == m.background);
    s.ints["row0.on"] = 1;
    s.ints["row1.on"] = 0;
    REQUIRE(HitTest(m.pages[0], s, 1, 1).empty());
    REQUIRE(HitTest(m.pages[0], s, 11, 1) == "tap1");
}

TEST_CASE("DSMod runtime18 centers actual wrapped text height", "[dsmod][runtime18]") {
    Widget w;
    w.type = WidgetType::Label;
    w.text = "ABC\nDEF";
    w.text_scale = 1;
    w.color = White;
    w.line_gap = 1;
    w.text_center_h = 21;
    w.rect = {3, 0, 0, 21};
    const auto actual = Render(OnePage(w), {}, 40, 30);
    Canvas expected;
    expected.Resize(40, 30);
    expected.Clear(Manifest{}.background);
    expected.DrawTextBlock(3, 5, w.text, 1, White, 0, 0, 0, 1);
    REQUIRE(std::equal(actual.begin(), actual.end(), expected.Pixels().begin()));
    w.text = "ABC";
    const auto single = Render(OnePage(w), {}, 40, 30);
    expected.Clear(Manifest{}.background);
    expected.DrawText(3, 8, "ABC", 1, White);
    REQUIRE(std::equal(single.begin(), single.end(), expected.Pixels().begin()));
}

TEST_CASE("DSMod runtime18 shrinks names before bounded ellipsis", "[dsmod][runtime18]") {
    Widget w;
    w.type = WidgetType::Label;
    w.text = "ABCDEF";
    w.text_scale = GENERATE(3, 20, 128);
    w.color = White;
    w.wrap_width = 48;
    w.max_lines = 1;
    w.fit_text = true;
    w.text_min_scale = 2;
    const auto actual = Render(OnePage(w), {}, 70, 20);
    Canvas expected;
    expected.Resize(70, 20);
    expected.Clear(Manifest{}.background);
    expected.DrawTextBlock(0, 0, w.text, 2, White, 0, 48, 1, -1);
    REQUIRE(std::equal(actual.begin(), actual.end(), expected.Pixels().begin()));
}

TEST_CASE("DSMod runtime18 image format works without placeholder src", "[dsmod][runtime18]") {
    Widget w;
    w.type = WidgetType::Image;
    w.rect = {0, 0, 4, 4};
    w.src_format = "module:icon/%d";
    w.bind = "which";
    w.color = White;
    StateSnapshot s;
    s.ints["which"] = 7;
    Canvas c;
    c.Resize(4, 4);
    const auto icon = std::make_shared<Image>(Solid(4, 4, White));
    REQUIRE(RenderPage(c, Manifest{}, OnePage(w), s, [&](const std::string& key) {
        REQUIRE(key == "module:icon/7");
        return icon;
    }));
    REQUIRE(PixelAt(c, 0, 0) == White);
}

TEST_CASE("DSMod runtime18 draws game-art scrollbar tracks", "[dsmod][runtime18]") {
    Page p;
    p.id = "p";
    ScrollRegion r;
    r.id = "list";
    r.rect = {0, 0, 30, 30};
    r.row_h = 10;
    r.count_bind = "count";
    r.bar_w = 6;
    r.bar_color = White;
    r.bar_track_src = "track";
    p.scrolls.push_back(r);
    StateSnapshot s;
    s.ints["count"] = 10;
    s.ints["@scroll_on:list"] = 1;
    Canvas c;
    c.Resize(30, 30);
    const auto track = std::make_shared<Image>(Solid(2, 2, 0xFF123456u));
    REQUIRE(RenderPage(c, Manifest{}, p, s, [&](const std::string&) { return track; }));
    REQUIRE(PixelAt(c, 26, 28) == 0xFF123456u);
    REQUIRE(PixelAt(c, 26, 2) == White);
}

TEST_CASE("DSMod runtime18 malformed image formats do not reach printf", "[dsmod][runtime18]") {
    Widget w;
    w.type = WidgetType::Image;
    w.rect = {0, 0, 4, 4};
    w.src_format = GENERATE("module:icon/%d%", "module:icon/%999999999d", "module:icon/%d%0%");
    w.bind = "which";
    w.color = White;
    StateSnapshot s;
    s.ints["which"] = 7;
    Canvas c;
    c.Resize(4, 4);
    REQUIRE(RenderPage(c, Manifest{}, OnePage(w), s, [&](const std::string& key) {
        FAIL("Malformed image format requested an asset: " << key);
        return std::shared_ptr<const Image>{};
    }));
}

TEST_CASE("DSMod runtime18 centered overflowing text has conservative dirty bounds",
          "[dsmod][runtime18]") {
    Widget w;
    w.type = WidgetType::Label;
    w.text = "A\nB\nC";
    w.rect = {4, 20, 0, 0};
    w.text_scale = 2;
    w.line_gap = 0;
    w.max_lines = 3;
    w.text_center_h = 10;
    const auto bounds =
        WidgetTextBounds(w, 4, 20, 0, 0, StateSnapshot{}, Manifest{}, nullptr, 60, 60);
    REQUIRE(bounds[1] <= 10);
    REQUIRE(bounds[1] + bounds[3] >= 40);
}

TEST_CASE("DSMod runtime18 image selectors and unsigned formats need no placeholder source",
          "[dsmod][runtime18]") {
    Widget w;
    w.type = WidgetType::Image;
    w.rect = {0, 0, 4, 4};
    w.bind = "which";
    w.color = White;
    StateSnapshot s;
    s.ints["which"] = 1;
    const auto selector = GENERATE(0, 1, 2);
    std::string expected;
    if (selector == 0) {
        w.src_names = {"module:a", "module:b"};
        expected = "module:b";
    }
    if (selector == 1) {
        w.src_thresholds = {{2, "module:small"}, {9, "module:large"}};
        expected = "module:small";
    }
    if (selector == 2) {
        w.src_format = "module:%08X";
        s.ints["which"] = 0xF1234567;
        expected = "module:F1234567";
    }
    Canvas c;
    c.Resize(4, 4);
    const auto icon = std::make_shared<Image>(Solid(4, 4, White));
    REQUIRE(RenderPage(c, Manifest{}, OnePage(w), s, [&](const std::string& key) {
        REQUIRE(key == expected);
        return icon;
    }));
    REQUIRE(PixelAt(c, 0, 0) == White);
}

TEST_CASE("DSMod runtime18 negated gates retain held-group and missing-value semantics",
          "[dsmod][runtime18]") {
    Widget w;
    w.need_bind = "!gate";
    StateSnapshot s;
    REQUIRE_FALSE(WidgetHidden(w, s));
    s.ints["gate"] = 1;
    REQUIRE(WidgetHidden(w, s));
    REQUIRE_FALSE(WidgetHiddenHolding(w, s, "gate"));
    REQUIRE(WidgetHiddenHolding(w, s, "other"));
    w.need_bind = "gate";
    s.ints["gate"] = 0;
    REQUIRE(WidgetHidden(w, s));
    REQUIRE_FALSE(WidgetHiddenHolding(w, s, "gate"));
}
