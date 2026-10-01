// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Label colour tags ("color_markup": "{c:#AARRGGBB}".."{/c}"), with the built-in font: every
// glyph is 4 px wide at scale 1, drawn as FillRect cells.
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
constexpr u32 White = 0xFFFFFFFFu;
constexpr u32 Blue = 0xFF3D6FE0u;

u32 PixelAt(const Canvas& canvas, u32 x, u32 y) {
    return canvas.Pixels()[y * canvas.Width() + x];
}
} // namespace

TEST_CASE("DSMod colour tags take no width", "[dsmod][ui][markup]") {
    Canvas canvas;
    canvas.SetColorMarkup(true);
    REQUIRE(canvas.MeasureText("{c:#FF3D6FE0}AB{/c}C", 1) == canvas.MeasureText("ABC", 1));
    REQUIRE(canvas.MeasureText("{c:#ff3d6fe0}AB{/c}", 2) == 16);
    REQUIRE(TextMarkupBytes("An {c:#FF3D6FE0}item{/c}.") == 17);
    // Wrapping measures without the tags: both words fit a 28 px line.
    const auto lines = canvas.LayoutText("{c:#FF3D6FE0}ABC{/c} DEF", 1, 28, 0);
    REQUIRE(lines.size() == 1);
    // An ellipsis never cuts into a tag.
    const auto cut = canvas.LayoutText("AB {c:#FF3D6FE0}CD{/c} EF", 1, 20, 1);
    REQUIRE(cut.size() == 1);
    REQUIRE(cut[0].find("{c:") == std::string::npos);
    REQUIRE(canvas.MeasureText(cut[0], 1) <= 20);
}

TEST_CASE("DSMod colour span continues on the wrapped line", "[dsmod][ui][markup]") {
    Canvas canvas;
    canvas.Resize(40, 40);
    canvas.SetColorMarkup(true);
    const std::string text = "{c:#FF3D6FE0}AAA BBB{/c} CCC";
    const auto lines = canvas.LayoutText(text, 1, 14, 0);
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[1] == "{c:#FF3D6FE0}BBB{/c}");
    REQUIRE(lines[2] == "CCC");
    // Line pitch 8 (5 + line_gap 3). 'A' row 0 = .X., 'B' and 'C' row 0 start with X.
    canvas.DrawTextBlock(0, 0, text, 1, White, 0, 14, 0, 3);
    REQUIRE(PixelAt(canvas, 1, 0) == Blue);
    REQUIRE(PixelAt(canvas, 0, 8) == Blue);
    REQUIRE(PixelAt(canvas, 0, 16) == White);
    // An outline copy keeps its own colour.
    canvas.Clear(0);
    canvas.SetOutlineCopy(true);
    canvas.DrawTextBlock(0, 0, text, 1, White, 0, 14, 0, 3);
    REQUIRE(PixelAt(canvas, 1, 0) == White);
    REQUIRE(PixelAt(canvas, 0, 8) == White);
    // Silhouette icons are about icons only: the spans still apply.
    canvas.Clear(0);
    canvas.SetOutlineCopy(false);
    canvas.SetIconSilhouette(true);
    canvas.DrawTextBlock(0, 0, text, 1, White, 0, 14, 0, 3);
    REQUIRE(PixelAt(canvas, 1, 0) == Blue);
    REQUIRE(PixelAt(canvas, 0, 8) == Blue);
}

TEST_CASE("DSMod outline_copy label draws its spans in one colour", "[dsmod][ui][markup]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"label","rect":[0,0,10,10],"text":"x","color_markup":true,"outline_copy":true},
        {"type":"label","rect":[0,0,10,10],"text":"x","color_markup":true,
         "icon_style":"silhouette"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.pages[0].widgets[0].outline_copy);
    REQUIRE_FALSE(m.pages[0].widgets[1].outline_copy);
    REQUIRE(m.pages[0].widgets[1].icon_silhouette);

    Canvas canvas;
    canvas.Resize(60, 10);
    Page page;
    Widget widget;
    widget.type = WidgetType::Label;
    widget.rect = {0, 0, 60, 10};
    widget.text = "{c:#FF3D6FE0}A{/c}B";
    widget.text_scale = 1;
    widget.color = White;
    widget.color_markup = true;
    widget.outline_copy = true;
    page.widgets.push_back(widget);
    REQUIRE(RenderPage(canvas, Manifest{}, page, StateSnapshot{}));
    REQUIRE(PixelAt(canvas, 1, 0) == White);
    // A main copy with silhouette icons keeps the span colour.
    page.widgets[0].outline_copy = false;
    page.widgets[0].icon_silhouette = true;
    REQUIRE(RenderPage(canvas, Manifest{}, page, StateSnapshot{}));
    REQUIRE(PixelAt(canvas, 1, 0) == Blue);
}

TEST_CASE("DSMod colour tags at wrap and ellipsis edges", "[dsmod][ui][markup]") {
    Canvas canvas;
    canvas.SetColorMarkup(true);
    // A glyph wider than the line after a tag: no line made of the tag alone.
    const auto narrow = canvas.LayoutText("{c:#FF3D6FE0}AB", 1, 2, 0);
    REQUIRE(narrow == std::vector<std::string>{"{c:#FF3D6FE0}A", "{c:#FF3D6FE0}B"});
    // The space before a line's closing tag is trimmed; the tag stays.
    const auto closed = canvas.LayoutText("{c:#FF3D6FE0}AAA {/c} BBB", 1, 16, 0);
    REQUIRE(closed.size() == 2);
    REQUIRE(closed[0] == "{c:#FF3D6FE0}AAA{/c}");
    REQUIRE(canvas.MeasureText(closed[0], 1) == 12);
    // An ellipsis cut inside a span closes the span first: the ellipsis is in the label colour.
    const auto cut = canvas.LayoutText("{c:#FF3D6FE0}AAAA BBBB", 1, 20, 1);
    REQUIRE(cut == std::vector<std::string>{"{c:#FF3D6FE0}AA{/c}..."});
    // Without markup nothing changes: the same text is literal.
    Canvas plain;
    REQUIRE(plain.LayoutText("AB CD ", 1, 400, 0) == std::vector<std::string>{"AB CD"});
}

TEST_CASE("DSMod malformed colour tags are plain text", "[dsmod][ui][markup]") {
    Canvas canvas;
    canvas.SetColorMarkup(true);
    for (const std::string tag :
         {"{c:#GG3D6FE0}", "{c:#3D6FE0}", "{c:FF3D6FE0 }", "{c:#FF3D6FE0", "{/C}", "{/c"}) {
        INFO(tag);
        const std::string text = tag + "A";
        REQUIRE(canvas.MeasureText(text, 1) == static_cast<s32>(text.size()) * 4);
        REQUIRE(TextMarkupBytes(text) == 0);
    }
}

TEST_CASE("DSMod colour tags are text without color_markup", "[dsmod][ui][markup]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"label","rect":[0,0,10,10],"text":"x","color_markup":true},
        {"type":"label","rect":[0,0,10,10],"text":"x"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.pages[0].widgets[0].color_markup);
    REQUIRE_FALSE(m.pages[0].widgets[1].color_markup);

    Canvas canvas;
    canvas.Resize(120, 20);
    const std::string text = "{c:#FF3D6FE0}A{/c} B";
    REQUIRE(canvas.MeasureText(text, 1) == static_cast<s32>(text.size()) * 4);
    REQUIRE(canvas.LayoutText(text, 1, 400, 0) == std::vector<std::string>{text});
    // No span is carried to the next line either.
    const auto lines = canvas.LayoutText("{c:#FF3D6FE0}A\nB", 1, 400, 0);
    REQUIRE(lines == std::vector<std::string>{"{c:#FF3D6FE0}A", "B"});
    // Drawn through a label widget: the 'A' sits after the tag's 13 literal cells, in white.
    Page page;
    Widget widget;
    widget.type = WidgetType::Label;
    widget.rect = {0, 0, 120, 20};
    widget.text = text;
    widget.text_scale = 1;
    widget.color = White;
    page.widgets.push_back(widget);
    REQUIRE(RenderPage(canvas, Manifest{}, page, StateSnapshot{}));
    REQUIRE(PixelAt(canvas, 13 * 4 + 1, 0) == White);
    REQUIRE(PixelAt(canvas, 1, 0) != Blue);
    // The same widget with color_markup: 'A' first, in the span colour.
    page.widgets[0].color_markup = true;
    canvas.Clear(0);
    REQUIRE(RenderPage(canvas, Manifest{}, page, StateSnapshot{}));
    REQUIRE(PixelAt(canvas, 1, 0) == Blue);
}
