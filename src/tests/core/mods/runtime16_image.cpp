// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 16 image transforms and fills (Image "rotate" / "rotate_bind" / "pivot" / "scale_bind",
// "tint" / "tint_bind" / "tint_colors" on Image and Pips, "fill" stretch / tile / slice, Bar
// "fill_dir" and "image") and the runtime 17 Chart widget (ChartSampler, mod_chart.h).
#include <array>
#include <memory>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_chart.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
constexpr u32 Black = 0xFF000000u;
constexpr u32 Red = 0xFFFF0000u;
constexpr u32 Green = 0xFF00FF00u;
constexpr u32 Blue = 0xFF0000FFu;
constexpr u32 White = 0xFFFFFFFFu;

u32 PixelAt(const Canvas& canvas, s32 x, s32 y) {
    return canvas.Pixels()[static_cast<size_t>(y) * canvas.Width() + static_cast<size_t>(x)];
}

std::shared_ptr<const Image> Solid(u32 w, u32 h, u32 argb) {
    auto image = std::make_shared<Image>();
    image->w = w;
    image->h = h;
    image->pixels.assign(static_cast<size_t>(w) * h, argb);
    return image;
}

/// 20 x 10: the left half green, the right half red (shows which way it was turned).
std::shared_ptr<const Image> HalfAndHalf() {
    auto image = std::make_shared<Image>();
    image->w = 20;
    image->h = 10;
    image->pixels.resize(200);
    for (u32 y = 0; y < 10; ++y) {
        for (u32 x = 0; x < 20; ++x) {
            image->pixels[y * 20 + x] = x < 10 ? Green : Red;
        }
    }
    return image;
}

ImageProvider Provide(std::shared_ptr<const Image> image) {
    return [image](const std::string&) { return image; };
}

Widget ImageWidget(std::array<s32, 4> rect) {
    Widget w;
    w.type = WidgetType::Image;
    w.rect = rect;
    w.src = "file:pic.png";
    w.color = White;
    return w;
}

/// Renders one widget on a black 40 x 40 page.
Canvas Draw(const Widget& widget, const ImageProvider& images, const StateSnapshot& snapshot = {},
            const RenderExtras* extras = nullptr) {
    Canvas canvas;
    canvas.Resize(40, 40);
    Manifest manifest;
    manifest.background = Black;
    Page page;
    page.widgets.push_back(widget);
    RenderPage(canvas, manifest, page, snapshot, images, {}, nullptr, {}, {}, nullptr, nullptr,
               {}, nullptr, extras);
    return canvas;
}
} // namespace

TEST_CASE("DSMod runtime 16: image style keys parse, absent keys keep the defaults",
          "[dsmod][runtime16][image]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"image","src":"file:a_{i}.png","rect":[0,0,10,10],"repeat":2,"rotate":450,
         "rotate_bind":"ang_{i}","scale_bind":"sc_{i}","pivot":[2,3],"tint":"#80FF0000",
         "tint_bind":"t_{i}","tint_colors":["#FF00FF00","0000FF",4278190335],"fill":"slice",
         "slice":[1,2,3,4]},
        {"type":"bar","rect":[0,0,10,10],"fill_dir":"up","image":"file:bar_{i}.png","fill":"tile"},
        {"type":"image","src":"file:b.png"},
        {"type":"chart","bind":"hp","samples":5000,"interval_ms":250,"style":"bar","min":-1},
        {"type":"chart","id":"named","bind":"hp"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& ws = m.pages[0].widgets;
    REQUIRE(ws.size() == 5);
    const Widget& img = ws[0];
    REQUIRE(img.rotate == 90.0f);
    REQUIRE(img.rotate_bind == "ang_{i}");
    REQUIRE(img.scale_bind == "sc_{i}");
    REQUIRE(img.has_pivot);
    REQUIRE(img.pivot == std::array<float, 2>{2.0f, 3.0f});
    REQUIRE(img.has_tint);
    REQUIRE(img.tint == 0x80FF0000u);
    REQUIRE(img.tint_colors == std::vector<u32>{Green, Blue, Blue});
    REQUIRE(img.fill == ImageFill::Slice);
    REQUIRE(img.slice == std::array<s32, 4>{1, 2, 3, 4});
    REQUIRE(img.fill_image.empty()); // "image" is a Bar key
    const Widget& bar = ws[1];
    REQUIRE(bar.fill_dir == BarFillDir::Up);
    REQUIRE(bar.fill_image == "file:bar_{i}.png");
    REQUIRE(bar.fill == ImageFill::Tile);
    // Nothing set: exactly a default widget's runtime 16 fields.
    const Widget& plain = ws[2];
    const Widget d;
    REQUIRE(plain.rotate == d.rotate);
    REQUIRE_FALSE(plain.has_pivot);
    REQUIRE_FALSE(plain.has_tint);
    REQUIRE(plain.tint_colors.empty());
    REQUIRE(plain.fill == ImageFill::Stretch);
    REQUIRE(plain.fill_dir == BarFillDir::Right);
    REQUIRE(plain.chart == nullptr);
    REQUIRE_FALSE(WidgetImageTransform(plain, 10, 10, nullptr).active);
    // Chart: clamped samples, its ring buffer named by page and index, or by id.
    REQUIRE(ws[3].type == WidgetType::Chart);
    REQUIRE(ws[3].chart != nullptr);
    REQUIRE(ws[3].chart->key == "p#3");
    REQUIRE(ws[3].chart->samples == 1024);
    REQUIRE(ws[3].chart->interval_ms == 250);
    REQUIRE(ws[3].chart->style == ChartSpec::Style::Bar);
    REQUIRE(ws[3].chart->has_min);
    REQUIRE(ws[3].chart->min == -1.0);
    REQUIRE_FALSE(ws[3].chart->has_max);
    REQUIRE(ws[4].chart->key == "named");
    REQUIRE(ws[4].chart->samples == 60);
    REQUIRE(ws[4].chart->interval_ms == 1000);
    REQUIRE(ws[4].chart->style == ChartSpec::Style::Line);
    // "{i}" in the new binds.
    const auto expanded = ExpandWidgets(m.pages[0], StateSnapshot{});
    REQUIRE(expanded[1].rotate_bind == "ang_1");
    REQUIRE(expanded[1].scale_bind == "sc_1");
    REQUIRE(expanded[1].tint_bind == "t_1");
}

TEST_CASE("DSMod runtime 16: rotate turns clockwise about the centre or the pivot",
          "[dsmod][runtime16][image]") {
    const auto images = Provide(HalfAndHalf());
    Widget w = ImageWidget({10, 15, 20, 10}); // centre (20, 20)
    // Unturned: the classic blit.
    Canvas plain = Draw(w, images);
    REQUIRE(PixelAt(plain, 11, 20) == Green);
    REQUIRE(PixelAt(plain, 28, 20) == Red);
    REQUIRE(PixelAt(plain, 20, 12) == Black);
    // 90 degrees clockwise: the left (green) half ends on top, the box is 10 x 20.
    w.rotate = 90.0f;
    Canvas turned = Draw(w, images);
    REQUIRE(PixelAt(turned, 20, 12) == Green);
    REQUIRE(PixelAt(turned, 20, 28) == Red);
    REQUIRE(PixelAt(turned, 11, 20) == Black);
    REQUIRE(PixelAt(turned, 28, 20) == Black);
    // rotate_bind adds a published angle: 0 + 90 draws the same picture.
    w.rotate = 0.0f;
    w.rotate_bind = "ang";
    StateSnapshot s;
    s.ints["ang"] = 90;
    REQUIRE(Draw(w, images, s).Pixels() == turned.Pixels());
    // A bound 0 (or a missing value) is the plain picture again.
    s.ints["ang"] = 0;
    REQUIRE(Draw(w, images, s).Pixels() == plain.Pixels());
    REQUIRE(Draw(w, images, StateSnapshot{}).Pixels() == plain.Pixels());
    // Pivot at the rect's top-left corner (20, 20): a quarter turn swings the picture to the left
    // of that corner, x 10..20, y 20..40.
    Widget p = ImageWidget({20, 20, 20, 10});
    p.rotate = 90.0f;
    p.pivot = {0.0f, 0.0f};
    p.has_pivot = true;
    Canvas pivoted = Draw(p, images);
    REQUIRE(PixelAt(pivoted, 15, 25) == Green);
    REQUIRE(PixelAt(pivoted, 15, 35) == Red);
    REQUIRE(PixelAt(pivoted, 25, 25) == Black);
}

TEST_CASE("DSMod runtime 16: scale_bind scales about the pivot", "[dsmod][runtime16][image]") {
    const auto images = Provide(Solid(4, 4, Blue));
    Widget w = ImageWidget({15, 15, 10, 10});
    w.scale_bind = "sc";
    StateSnapshot s;
    s.ints["sc"] = 2000; // 2x about the centre: 10..30
    Canvas big = Draw(w, images, s);
    REQUIRE(PixelAt(big, 10, 10) == Blue);
    REQUIRE(PixelAt(big, 29, 29) == Blue);
    REQUIRE(PixelAt(big, 9, 9) == Black);
    REQUIRE(PixelAt(big, 30, 30) == Black);
    s.ints["sc"] = 500; // half: 17.5..22.5 -> 18..23 (rounded)
    Canvas small = Draw(w, images, s);
    REQUIRE(PixelAt(small, 16, 16) == Black);
    REQUIRE(PixelAt(small, 20, 20) == Blue);
    // Missing: 1x.
    Canvas one = Draw(w, images, StateSnapshot{});
    REQUIRE(PixelAt(one, 15, 15) == Blue);
    REQUIRE(PixelAt(one, 14, 14) == Black);
    // About the top-left corner: 15..35.
    w.has_pivot = true;
    w.pivot = {0.0f, 0.0f};
    s.ints["sc"] = 2000;
    Canvas corner = Draw(w, images, s);
    REQUIRE(PixelAt(corner, 14, 14) == Black);
    REQUIRE(PixelAt(corner, 34, 34) == Blue);
}

TEST_CASE("DSMod runtime 16: a turned picture widens its dirty box and survives a partial redraw",
          "[dsmod][runtime16][image]") {
    Widget w = ImageWidget({10, 15, 20, 10});
    REQUIRE(WidgetDrawOverhang(w, 20, 10) == 0);
    w.rotate = 90.0f;
    // Turned upright: 5 px above and below the rect, plus the margin.
    const s32 pad = WidgetDrawOverhang(w, 20, 10);
    REQUIRE(pad >= 5);
    REQUIRE(pad <= 8);
    // Its whole box lies inside the rect widened by the pad.
    const auto box = TransformedImageBounds(10, 15, 20, 10, 90.0f, 1.0f, 10.0f, 5.0f);
    REQUIRE(box[0] >= 10 - pad);
    REQUIRE(box[1] >= 15 - pad);
    REQUIRE(box[2] <= 30 + pad);
    REQUIRE(box[3] <= 25 + pad);
    // A bound angle: the box for this snapshot; without a snapshot any angle.
    Widget b = ImageWidget({10, 15, 20, 10});
    b.rotate_bind = "ang";
    StateSnapshot s;
    s.ints["ang"] = 0;
    REQUIRE(WidgetDrawOverhang(b, 20, 10, &s) <= 1);
    s.ints["ang"] = 90;
    REQUIRE(WidgetDrawOverhang(b, 20, 10, &s) >= 5);
    REQUIRE(WidgetDrawOverhang(b, 20, 10, nullptr) >= 9); // the 11.2 px circle about (10, 5)
    // Scaled up: the scaled rect.
    Widget sc = ImageWidget({10, 10, 10, 10});
    sc.scale_bind = "sc";
    s.ints["sc"] = 3000;
    REQUIRE(WidgetDrawOverhang(sc, 10, 10, &s) >= 10);
    // A partial redraw whose dirty rect lies only above the rect (inside the turned picture) still
    // draws the widget there.
    RenderExtras extras;
    extras.clip = {15, 10, 10, 4};
    Canvas partial = Draw(w, Provide(HalfAndHalf()), StateSnapshot{}, &extras);
    REQUIRE(PixelAt(partial, 20, 12) == Green);
}

TEST_CASE("DSMod runtime 16: tint, tint_bind and tint_colors on Image and Pips",
          "[dsmod][runtime16][image]") {
    const auto images = Provide(Solid(2, 2, White));
    Widget w = ImageWidget({0, 0, 10, 10});
    REQUIRE(PixelAt(Draw(w, images), 5, 5) == White);
    w.color = 0xFF808080u; // the classic tint stays the default
    REQUIRE(PixelAt(Draw(w, images), 5, 5) == 0xFF808080u);
    w.tint = Green;
    w.has_tint = true;
    REQUIRE(PixelAt(Draw(w, images), 5, 5) == Green);
    w.tint_bind = "state";
    w.tint_colors = {Red, Blue};
    StateSnapshot s;
    s.ints["state"] = 1;
    REQUIRE(PixelAt(Draw(w, images, s), 5, 5) == Blue);
    s.ints["state"] = 0;
    REQUIRE(PixelAt(Draw(w, images, s), 5, 5) == Red);
    s.ints["state"] = 7; // out of range: `tint`
    REQUIRE(PixelAt(Draw(w, images, s), 5, 5) == Green);
    w.has_tint = false; // ... else `color`
    REQUIRE(PixelAt(Draw(w, images, s), 5, 5) == 0xFF808080u);
    REQUIRE(WidgetTint(w, s, 0x12345678u) == 0x12345678u);

    // Pips: the tint replaces `color` for the lit pips only.
    Widget pips;
    pips.type = WidgetType::Pips;
    pips.rect = {0, 0, 0, 6};
    pips.bind = "n";
    pips.max_const = 3;
    pips.color = White;
    pips.bg = Black;
    pips.gap = 2;
    pips.tint_bind = "low";
    pips.tint_colors = {White, Red};
    StateSnapshot ps;
    ps.ints["n"] = 1;
    ps.ints["low"] = 1;
    Canvas drawn = Draw(pips, {}, ps);
    REQUIRE(PixelAt(drawn, 3, 3) == Red);    // lit pip, tinted
    REQUIRE(PixelAt(drawn, 11, 3) == Black); // unlit pip: bg
    // Sprite pips: the sprite multiplied by the lit tint.
    pips.src = "file:pip.png";
    pips.rect = {0, 0, 6, 6};
    Canvas sprites = Draw(pips, images, ps);
    REQUIRE(PixelAt(sprites, 3, 3) == Red);
}

TEST_CASE("DSMod runtime 16: tile and 9-slice fills", "[dsmod][runtime16][image]") {
    // 2 x 2 checker: A B / C D.
    auto checker = std::make_shared<Image>();
    checker->w = 2;
    checker->h = 2;
    checker->pixels = {Red, Green, Blue, White};
    Widget w = ImageWidget({0, 0, 5, 5});
    w.fill = ImageFill::Tile;
    Canvas tiled = Draw(w, Provide(checker));
    REQUIRE(PixelAt(tiled, 0, 0) == Red);
    REQUIRE(PixelAt(tiled, 1, 1) == White);
    REQUIRE(PixelAt(tiled, 2, 0) == Red);
    REQUIRE(PixelAt(tiled, 3, 0) == Green);
    REQUIRE(PixelAt(tiled, 4, 4) == Red);
    REQUIRE(PixelAt(tiled, 5, 5) == Black);
    // Stretch (the default) scales the 2 x 2 over the rect instead.
    w.fill = ImageFill::Stretch;
    REQUIRE(PixelAt(Draw(w, Provide(checker)), 3, 0) == Green);
    REQUIRE(PixelAt(Draw(w, Provide(checker)), 2, 0) == Red);

    // 6 x 6 frame: a 2 px red border around a blue centre, 9-sliced to 20 x 10.
    auto frame = std::make_shared<Image>();
    frame->w = 6;
    frame->h = 6;
    frame->pixels.resize(36);
    for (u32 y = 0; y < 6; ++y) {
        for (u32 x = 0; x < 6; ++x) {
            frame->pixels[y * 6 + x] = (x < 2 || x >= 4 || y < 2 || y >= 4) ? Red : Blue;
        }
    }
    Widget s = ImageWidget({0, 0, 20, 10});
    s.fill = ImageFill::Slice;
    s.slice = {2, 2, 2, 2};
    Canvas sliced = Draw(s, Provide(frame));
    REQUIRE(PixelAt(sliced, 0, 0) == Red);
    REQUIRE(PixelAt(sliced, 1, 1) == Red);
    REQUIRE(PixelAt(sliced, 19, 9) == Red);
    REQUIRE(PixelAt(sliced, 10, 1) == Red); // the top edge, stretched along x only
    REQUIRE(PixelAt(sliced, 1, 5) == Red);  // the left edge
    REQUIRE(PixelAt(sliced, 2, 2) == Blue); // the centre starts right after the 2 px corner
    REQUIRE(PixelAt(sliced, 17, 7) == Blue);
    REQUIRE(PixelAt(sliced, 18, 7) == Red);
    // Plain stretch of the same picture: the border scales up (20 / 6 -> 6 px wide).
    s.fill = ImageFill::Stretch;
    REQUIRE(PixelAt(Draw(s, Provide(frame)), 5, 5) == Red);
    // A rect smaller than two corners: the corners share it.
    Image out;
    ComposeImageFill(out, 3, 3, *frame, 0, 0, 6, 6, ImageFill::Slice, {2, 2, 2, 2});
    REQUIRE(out.w == 3);
    REQUIRE(out.h == 3);
    REQUIRE(out.pixels[0] == Red);
    REQUIRE(out.pixels[8] == Red);
    // Tile and slice work under a rotation too (the filled picture turns as a whole).
    Widget t = ImageWidget({10, 15, 20, 10});
    t.fill = ImageFill::Tile;
    t.rotate = 90.0f;
    Canvas turned = Draw(t, Provide(checker));
    REQUIRE(PixelAt(turned, 11, 20) == Black);
    REQUIRE(PixelAt(turned, 20, 12) != Black);
}

TEST_CASE("DSMod runtime 16: bar fill_dir and a textured fill", "[dsmod][runtime16][image]") {
    Widget bar;
    bar.type = WidgetType::Bar;
    bar.rect = {0, 0, 10, 10};
    bar.bind = "v";
    bar.max_const = 10;
    bar.color = White;
    bar.bg = Black;
    bar.frame = 0;
    StateSnapshot s;
    s.ints["v"] = 5;
    const auto at = [&](BarFillDir dir, s32 x, s32 y) {
        Widget b = bar;
        b.fill_dir = dir;
        return PixelAt(Draw(b, {}, s), x, y);
    };
    REQUIRE(at(BarFillDir::Right, 2, 5) == White);
    REQUIRE(at(BarFillDir::Right, 7, 5) == Black);
    REQUIRE(at(BarFillDir::Left, 2, 5) == Black);
    REQUIRE(at(BarFillDir::Left, 7, 5) == White);
    REQUIRE(at(BarFillDir::Up, 5, 2) == Black);
    REQUIRE(at(BarFillDir::Up, 5, 7) == White);
    REQUIRE(at(BarFillDir::Down, 5, 2) == White);
    REQUIRE(at(BarFillDir::Down, 5, 7) == Black);
    // The classic bar is unchanged: right, then the frame in `color`.
    bar.frame = 1;
    Canvas classic = Draw(bar, {}, s);
    REQUIRE(PixelAt(classic, 0, 5) == White);
    REQUIRE(PixelAt(classic, 9, 5) == White); // frame
    REQUIRE(PixelAt(classic, 7, 5) == Black);
    bar.frame = 0;
    // Textured: the picture spans the bar and the fill reveals it; tint multiplies it.
    auto halves = std::make_shared<Image>();
    halves->w = 2;
    halves->h = 1;
    halves->pixels = {Green, Blue};
    bar.fill_image = "file:bar.png";
    Canvas textured = Draw(bar, Provide(halves), s);
    REQUIRE(PixelAt(textured, 2, 5) == Green);
    REQUIRE(PixelAt(textured, 7, 5) == Black); // unfilled: bg, not the blue half
    bar.fill_dir = BarFillDir::Left;
    Canvas from_right = Draw(bar, Provide(halves), s);
    REQUIRE(PixelAt(from_right, 7, 5) == Blue);
    REQUIRE(PixelAt(from_right, 2, 5) == Black);
    bar.tint = 0xFF00FF00u;
    bar.has_tint = true;
    REQUIRE(PixelAt(Draw(bar, Provide(halves), s), 7, 5) == Black); // blue x green = black
    // A missing picture falls back to the flat colour.
    bar.has_tint = false;
    REQUIRE(PixelAt(Draw(bar, {}, s), 7, 5) == White);
}

TEST_CASE("DSMod runtime 17: chart sampling keeps a ring buffer per widget",
          "[dsmod][runtime17][chart]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"chart","id":"c","bind":"v","samples":3,"interval_ms":100}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    ChartSampler sampler;
    const auto tick = [&](u64 now, std::optional<s64> v) {
        StateSnapshot s;
        if (v) {
            s.ints["v"] = *v;
        }
        sampler.Update(m, now, s);
        return s;
    };
    const auto values = [](const StateSnapshot& s) {
        REQUIRE(s.charts != nullptr);
        return s.charts->at("c").values;
    };
    StateSnapshot s = tick(1000, 1);
    REQUIRE(values(s) == std::vector<f32>{1.0f});
    REQUIRE(s.ints.at("@chart:c") == 1);
    s = tick(1050, 2); // inside the interval: no sample
    REQUIRE(values(s) == std::vector<f32>{1.0f});
    REQUIRE(s.ints.at("@chart:c") == 1);
    s = tick(1100, 2);
    REQUIRE(values(s) == std::vector<f32>{1.0f, 2.0f});
    s = tick(1210, 3); // late by 10 ms: the cadence stays at 1200, 1300, ...
    s = tick(1300, 4);
    REQUIRE(values(s) == std::vector<f32>{2.0f, 3.0f, 4.0f}); // 3 samples: the oldest dropped
    REQUIRE(s.ints.at("@chart:c") == 4);
    s = tick(1400, std::nullopt); // unresolved: no sample, the interval restarts
    REQUIRE(values(s) == std::vector<f32>{2.0f, 3.0f, 4.0f});
    REQUIRE(s.ints.at("@chart:c") == 4);
    s = tick(5000, 9); // after a pause: one sample, not a burst
    REQUIRE(values(s) == std::vector<f32>{3.0f, 4.0f, 9.0f});
    s = tick(5050, 10);
    REQUIRE(values(s) == std::vector<f32>{3.0f, 4.0f, 9.0f});
    // Floats sample too.
    StateSnapshot f;
    f.floats["v"] = 2.5;
    sampler.Update(m, 5100, f);
    REQUIRE(values(f).back() == 2.5f);
    sampler.Reset();
    REQUIRE(tick(6000, std::nullopt).charts->at("c").values.empty());
}

TEST_CASE("DSMod runtime 16/17: mistyped keys are ignored, not a failed package",
          "[dsmod][runtime17][chart]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"chart","id":"c","bind":"v","style":5},
        {"type":"image","rect":[0,0,4,4],"src":"a","rotate_bind":1,"scale_bind":[],"tint_bind":{}}]}],
        "derived":{"left":{"countdown":"deadline","now":7}}})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.pages[0].widgets[0].chart->style == ChartSpec::Style::Line);
    REQUIRE(m.pages[0].widgets[1].rotate_bind.empty());
    REQUIRE(m.pages[0].widgets[1].scale_bind.empty());
    REQUIRE(m.pages[0].widgets[1].tint_bind.empty());
}

TEST_CASE("DSMod runtime 17: a package without charts publishes none",
          "[dsmod][runtime17][chart]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"label","rect":[0,0,4,4],"bind":"v"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    ChartSampler sampler;
    StateSnapshot s;
    s.ints["v"] = 1;
    sampler.Update(m, 1000, s);
    REQUIRE(s.charts == nullptr);
    REQUIRE(s.ints.size() == 1);
}

TEST_CASE("DSMod runtime 17: chart draws bars and a line, newest on the right",
          "[dsmod][runtime17][chart]") {
    Widget chart;
    chart.type = WidgetType::Chart;
    chart.rect = {0, 0, 8, 10};
    chart.color = White;
    chart.bg = Blue;
    auto spec = std::make_shared<ChartSpec>();
    spec->key = "c";
    spec->samples = 4;
    spec->style = ChartSpec::Style::Bar;
    spec->has_min = spec->has_max = true;
    spec->min = 0.0;
    spec->max = 10.0;
    chart.chart = spec;
    auto series = std::make_shared<ChartSeriesMap>();
    (*series)["c"].values = {10.0f, 5.0f};
    StateSnapshot s;
    s.charts = series;
    Canvas bars = Draw(chart, {}, s);
    REQUIRE(PixelAt(bars, 0, 9) == Blue);  // two empty slots on the left
    REQUIRE(PixelAt(bars, 4, 0) == White); // full bar (slot 2)
    REQUIRE(PixelAt(bars, 6, 4) == Blue);  // half bar (slot 3): top half empty
    REQUIRE(PixelAt(bars, 6, 6) == White);
    REQUIRE(PixelAt(bars, 8, 9) == Black); // nothing outside the rect
    // Line: 0 at the bottom left to 10 at the top right.
    chart.rect = {0, 0, 10, 10};
    spec->style = ChartSpec::Style::Line;
    spec->samples = 2;
    (*series)["c"].values = {0.0f, 10.0f};
    Canvas line = Draw(chart, {}, s);
    REQUIRE(PixelAt(line, 0, 9) == White);
    REQUIRE(PixelAt(line, 9, 0) == White);
    REQUIRE(PixelAt(line, 5, 4) == White);
    REQUIRE(PixelAt(line, 0, 0) == Blue);
    REQUIRE(PixelAt(line, 10, 0) == Black); // clipped to the rect
    // Auto range: a flat series sits mid-height.
    spec->has_min = spec->has_max = false;
    (*series)["c"].values = {3.0f, 3.0f};
    Canvas flat = Draw(chart, {}, s);
    REQUIRE(PixelAt(flat, 5, 4) == White);
    REQUIRE(PixelAt(flat, 5, 0) == Blue);
    // No samples yet: just the background.
    REQUIRE(PixelAt(Draw(chart, {}, StateSnapshot{}), 5, 5) == Blue);
}
