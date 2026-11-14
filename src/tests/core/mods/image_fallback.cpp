// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Image widget draw continuity (ImageFallbackState, mod_ui.h): a src_bind resolving to a source
// key the image provider has not decoded yet (GetModuleImage/CacheFindImage's own "pending"
// nullptr) draws the last picture this widget instance actually drew instead of nothing, and an
// Image widget with "src_fade_ms" set cross-fades into a newly landed picture instead of
// swapping instantly. Covers: the fallback draw itself, that it never fires when the binding
// disappeared (not merely a cache miss), the fade/no-fade decision against "src_fade_bind", and
// manifest parsing (including "{i}" template expansion) of the two new keys.
#include <chrono>
#include <string>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
constexpr u32 Black = 0xFF000000u;
constexpr u32 Red = 0xFFFF0000u;
constexpr u32 Green = 0xFF00FF00u;
constexpr u32 Blue = 0xFF0000FFu;

u32 PixelAt(const Canvas& canvas, s32 x, s32 y) {
    return canvas.Pixels()[static_cast<size_t>(y) * canvas.Width() + static_cast<size_t>(x)];
}

std::shared_ptr<const Image> Solid(u32 argb) {
    auto image = std::make_shared<Image>();
    image->w = 4;
    image->h = 4;
    image->pixels.assign(16, argb);
    return image;
}

/// Resolves exactly the keys in `known`; every other key (the "not decoded yet" case
/// GetModuleImage returns for a cache miss) answers nullptr.
ImageProvider Provide(std::unordered_map<std::string, std::shared_ptr<const Image>> known) {
    return [known = std::move(known)](const std::string& key) {
        const auto it = known.find(key);
        return it == known.end() ? nullptr : it->second;
    };
}

Widget ImageWidget() {
    Widget w;
    w.type = WidgetType::Image;
    w.rect = {0, 0, 10, 10};
    w.color = 0xFFFFFFFFu; // opaque white: tint is a no-op, the picture draws at its own colour
    w.src_bind = "pic";
    return w;
}

/// Renders one widget on a black 40 x 40 page, against a caller-owned (so it survives across
/// calls, like the real ModRuntime::image_fallback_state does) ImageFallbackState.
Canvas Draw(const Widget& widget, const ImageProvider& images, const StateSnapshot& snapshot,
            ImageFallbackState& fallback, bool* animating = nullptr) {
    Canvas canvas;
    canvas.Resize(40, 40);
    Manifest manifest;
    manifest.background = Black;
    Page page;
    page.id = "p";
    page.widgets.push_back(widget);
    RenderPage(canvas, manifest, page, snapshot, images, {}, nullptr, {}, {}, nullptr, animating,
               {}, nullptr, nullptr, nullptr, nullptr, &fallback);
    return canvas;
}
} // namespace

TEST_CASE("DSMod image fallback: a pending decode draws the last picture instead of nothing",
          "[dsmod][image_fallback]") {
    const Widget w = ImageWidget();
    ImageFallbackState fallback;
    StateSnapshot s;
    s.texts["pic"] = "module:farming_simulator:frame/1";
    // "frame/1" lands: a normal draw, and the fallback store now remembers it.
    Canvas first = Draw(w, Provide({{"module:farming_simulator:frame/1", Solid(Blue)}}), s,
                        fallback);
    REQUIRE(PixelAt(first, 5, 5) == Blue);
    // "frame/2" is minted (the module's next live frame); the provider has not decoded it yet
    // (returns nullptr for that key, exactly like a real cache miss) -- without the fallback this
    // draws nothing onto the cleared canvas (background). With it, "frame/1" draws again.
    s.texts["pic"] = "module:farming_simulator:frame/2";
    Canvas second = Draw(w, Provide({{"module:farming_simulator:frame/1", Solid(Blue)}}), s,
                         fallback);
    REQUIRE(PixelAt(second, 5, 5) == Blue);
    // Once "frame/2" actually lands, it replaces the fallback.
    Canvas third = Draw(w,
                        Provide({{"module:farming_simulator:frame/1", Solid(Blue)},
                                 {"module:farming_simulator:frame/2", Solid(Red)}}),
                        s, fallback);
    REQUIRE(PixelAt(third, 5, 5) == Red);
    // A widget drawn with no image_fallback store at all (every caller that predates this
    // feature, and the debug page) keeps exactly the old behaviour: a miss draws nothing, even
    // though a wholly separate call with a store just had something to fall back to.
    s.texts["pic"] = "module:farming_simulator:frame/3";
    Canvas no_store = [&] {
        Canvas canvas;
        canvas.Resize(40, 40);
        Manifest manifest;
        manifest.background = Black;
        Page page;
        page.id = "p";
        page.widgets.push_back(w);
        RenderPage(canvas, manifest, page, s,
                   Provide({{"module:farming_simulator:frame/1", Solid(Blue)}}));
        return canvas;
    }();
    REQUIRE(PixelAt(no_store, 5, 5) == Black);
}

TEST_CASE("DSMod image fallback: never fires once the binding itself disappears",
          "[dsmod][image_fallback]") {
    const Widget w = ImageWidget();
    ImageFallbackState fallback;
    StateSnapshot bound;
    bound.texts["pic"] = "module:g:a";
    Canvas drawn = Draw(w, Provide({{"module:g:a", Solid(Green)}}), bound, fallback);
    REQUIRE(PixelAt(drawn, 5, 5) == Green);
    // The bind itself is gone (not merely unresolved to a decoded image): draws nothing, exactly
    // as a plain Image widget with a missing src_bind always has -- the stale picture must not
    // leak through just because the fallback store still holds it from a moment ago.
    StateSnapshot unbound;
    Canvas gone = Draw(w, Provide({{"module:g:a", Solid(Green)}}), unbound, fallback);
    REQUIRE(PixelAt(gone, 5, 5) == Black);
}

TEST_CASE("DSMod image fallback: src_fade_ms cross-fades only when src_fade_bind changed",
          "[dsmod][image_fallback]") {
    Widget w = ImageWidget();
    w.src_fade_ms = 60;
    w.src_fade_bind = "scene";
    ImageFallbackState fallback;
    bool animating = false;
    StateSnapshot s;
    s.texts["pic"] = "module:g:a";
    s.ints["scene"] = 1;
    // First ever draw for this widget instance: nothing to fade from, so instant (and reports no
    // animation in flight).
    Canvas first = Draw(w, Provide({{"module:g:a", Solid(Green)}}), s, fallback, &animating);
    REQUIRE(PixelAt(first, 5, 5) == Green);
    REQUIRE_FALSE(animating);
    // A new source key, but "scene" did not change: an instant swap, not a fade.
    s.texts["pic"] = "module:g:b";
    Canvas instant = Draw(w, Provide({{"module:g:b", Solid(Blue)}}), s, fallback, &animating);
    REQUIRE(PixelAt(instant, 5, 5) == Blue);
    REQUIRE_FALSE(animating);
    // A new source key AND "scene" changed: a cross-fade starts (from Blue, the image just shown)
    // -- reports animation in flight, and immediately after starting is still (at worst, very
    // nearly) the outgoing picture, never the incoming one outright.
    s.texts["pic"] = "module:g:c";
    s.ints["scene"] = 2;
    Canvas starting = Draw(w, Provide({{"module:g:c", Solid(Red)}}), s, fallback, &animating);
    REQUIRE(animating);
    REQUIRE(PixelAt(starting, 5, 5) != Red);
    // ... and flags it for PublishUi, which owes the follow-up repaints (the redraw worker's own
    // `animating` only re-arms Map widgets).
    REQUIRE(fallback.fade_in_flight.exchange(false));
    // Once src_fade_ms has fully elapsed, the target shows exactly (no more blending) and
    // animating goes false again.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Canvas landed = Draw(w, Provide({{"module:g:c", Solid(Red)}}), s, fallback, &animating);
    REQUIRE(PixelAt(landed, 5, 5) == Red);
    REQUIRE_FALSE(animating);
    REQUIRE_FALSE(fallback.fade_in_flight.load());
}

TEST_CASE("DSMod image fallback: src_fade_ms/src_fade_bind parse, clamp and expand with \"{i}\"",
          "[dsmod][image_fallback]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"image","rect":[0,0,10,10],"src":"a_{i}","src_fade_ms":250,
         "src_fade_bind":"scene_{i}","repeat":2},
        {"type":"image","rect":[0,0,10,10],"src":"b","src_fade_ms":5000},
        {"type":"image","rect":[0,0,10,10],"src":"c","src_fade_ms":-5},
        {"type":"image","rect":[0,0,10,10],"src":"d"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& ws = m.pages[0].widgets;
    REQUIRE(ws.size() == 4);
    REQUIRE(ws[0].src_fade_ms == 250);
    REQUIRE(ws[0].src_fade_bind == "scene_{i}");
    REQUIRE(ws[1].src_fade_ms == 1000); // clamped to the documented 0..1000 range
    REQUIRE(ws[2].src_fade_ms == 0);    // clamped, not negative
    // Nothing set: the same defaults a brand new widget has.
    const Widget d;
    REQUIRE(ws[3].src_fade_ms == d.src_fade_ms);
    REQUIRE(ws[3].src_fade_bind == d.src_fade_bind);
    // "{i}" in src_fade_bind expands per repeated instance, like every other per-element bind.
    const auto expanded = ExpandWidgets(m.pages[0], StateSnapshot{});
    REQUIRE(expanded[0].src_fade_bind == "scene_0");
    REQUIRE(expanded[1].src_fade_bind == "scene_1");
}

TEST_CASE("DSMod image fallback: src_hold false draws nothing for a pending or failed key",
          "[dsmod][image_fallback]") {
    // An identity-changing widget (a list row bound to a different item each time): holding the
    // previous picture at the new item's slot would show the wrong item, so it opts out.
    Widget w = ImageWidget();
    w.src_hold = false;
    w.src_fade_ms = 200; // ignored with src_hold false: no fade state is kept either
    ImageFallbackState fallback;
    StateSnapshot s;
    s.texts["pic"] = "module:g:item/1";
    Canvas first = Draw(w, Provide({{"module:g:item/1", Solid(Blue)}}), s, fallback);
    REQUIRE(PixelAt(first, 5, 5) == Blue);
    // The row now shows item 2, not decoded yet (or failed): blank, never item 1's picture.
    s.texts["pic"] = "module:g:item/2";
    Canvas pending = Draw(w, Provide({{"module:g:item/1", Solid(Blue)}}), s, fallback);
    REQUIRE(PixelAt(pending, 5, 5) == Black);
    // Item 2 lands: drawn at once, with no cross-fade from item 1.
    bool animating = false;
    Canvas landed = Draw(w, Provide({{"module:g:item/1", Solid(Blue)}, {"module:g:item/2", Solid(Red)}}),
                         s, fallback, &animating);
    REQUIRE(PixelAt(landed, 5, 5) == Red);
    REQUIRE_FALSE(fallback.fade_in_flight.load());
    // The same sequence with the default (src_hold true) still holds, unchanged.
    Widget held = ImageWidget();
    ImageFallbackState fallback2;
    StateSnapshot s2;
    s2.texts["pic"] = "module:g:item/1";
    (void)Draw(held, Provide({{"module:g:item/1", Solid(Blue)}}), s2, fallback2);
    s2.texts["pic"] = "module:g:item/2";
    Canvas held_pending = Draw(held, Provide({{"module:g:item/1", Solid(Blue)}}), s2, fallback2);
    REQUIRE(PixelAt(held_pending, 5, 5) == Blue);
}

TEST_CASE("DSMod image fallback: src_hold parses (default true) and survives \"{i}\" expansion",
          "[dsmod][image_fallback]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"image","rect":[0,0,10,10],"src_bind":"row_{i}","src_hold":false,"repeat":2},
        {"type":"image","rect":[0,0,10,10],"src":"b","src_hold":true},
        {"type":"image","rect":[0,0,10,10],"src":"c","src_hold":"no"},
        {"type":"image","rect":[0,0,10,10],"src":"d"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& ws = m.pages[0].widgets;
    REQUIRE(ws.size() == 4);
    REQUIRE_FALSE(ws[0].src_hold);
    REQUIRE(ws[1].src_hold);
    REQUIRE(ws[2].src_hold); // not a boolean: ignored, keeps the default
    REQUIRE(ws[3].src_hold);
    const auto expanded = ExpandWidgets(m.pages[0], StateSnapshot{});
    REQUIRE_FALSE(expanded[0].src_hold);
    REQUIRE_FALSE(expanded[1].src_hold);
}
