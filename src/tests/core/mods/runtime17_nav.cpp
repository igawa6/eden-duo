// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 17: controller navigation of the second screen (mod_nav.h): the toggle chord, the
// Controller state machine, focus movement (geometry and nav_order), the candidates a page
// offers, the manifest keys, the focus frame and the HID gate that keeps the game's pad neutral.
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_nav.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"
#include "hid_core/resources/npad/dsmod_pad_gate.h"

using namespace Core::Mods;
using Nav::Dir;

TEST_CASE("DSMod process gate cannot be reopened by stale navigation", "[dsmod][process]") {
    namespace Gate = Core::HID::DSModPadGate;
    Gate::Reset();
    Gate::BlockForContextChange(true);
    Gate::Set(true, ~u64{0}, true, true);
    Gate::Reset(); // navigation exits while the old tick finishes after the process switch
    u64 buttons = ~u64{0};
    Core::HID::AnalogStickState left{100, 200};
    Core::HID::AnalogStickState right{300, 400};
    REQUIRE(Gate::Apply(0, buttons, left, right));
    REQUIRE(buttons == 0);
    REQUIRE(left.x == 0);
    REQUIRE(left.y == 0);
    REQUIRE(right.x == 0);
    REQUIRE(right.y == 0);
    Gate::BlockForContextChange(false); // old virtual source has now been neutralized
    buttons = 1;
    REQUIRE_FALSE(Gate::Apply(0, buttons, left, right));
    REQUIRE(buttons == 1);
}

namespace {
constexpr u64 ZL = 1ULL << 8;
constexpr u64 ZR = 1ULL << 9;
constexpr u64 X = 1ULL << 2;

Nav::Candidate Cand(s32 x, s32 y, s32 w = 40, s32 h = 40, std::string id = {}) {
    Nav::Candidate c;
    c.rect = c.shown = {x, y, w, h};
    c.id = c.template_id = std::move(id);
    return c;
}

std::vector<size_t> All(size_t n) {
    std::vector<size_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = i;
    }
    return v;
}

Widget Button(std::array<s32, 4> rect, std::string id, std::string on_tap = "go") {
    Widget w;
    w.type = WidgetType::Rect;
    w.rect = rect;
    w.id = std::move(id);
    w.on_tap = std::move(on_tap);
    return w;
}

u32 PixelAt(const Canvas& canvas, u32 x, u32 y) {
    return canvas.Pixels()[y * canvas.Width() + x];
}

Manifest Parse(const nlohmann::json& j) {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(j, m));
    return m;
}

nlohmann::json MinimalManifest() {
    return nlohmann::json::parse(R"({
        "pages":[{"id":"p","widgets":[{"type":"rect","rect":[0,0,10,10],"on_tap":"a"}]}],
        "actions":{"a":{"kind":"page","page":"p"}}
    })");
}
} // namespace

TEST_CASE("DSMod nav: chord parsing", "[dsmod][runtime17][nav]") {
    REQUIRE(Nav::ParseChord("ZL+ZR") == (ZL | ZR));
    REQUIRE(Nav::ParseChord(" zl + ZR ") == (ZL | ZR));
    REQUIRE(Nav::ParseChord("LS+RS") == Nav::DefaultToggleMask);
    REQUIRE(Nav::ParseChord("L3+R3") == Nav::DefaultToggleMask);
    REQUIRE(Nav::ParseChord("StickL+StickR") == Nav::DefaultToggleMask);
    REQUIRE(Nav::ParseChord("Minus+DDown") == ((1ULL << 11) | Nav::BtnDown));
    REQUIRE(Nav::ParseChord("L+R+X") == ((1ULL << 6) | (1ULL << 7) | X));
    // One button alone, a repeated button, an unknown name, an empty part: no chord.
    REQUIRE_FALSE(Nav::ParseChord("A").has_value());
    REQUIRE_FALSE(Nav::ParseChord("ZL+ZL").has_value());
    REQUIRE_FALSE(Nav::ParseChord("ZL+Foo").has_value());
    REQUIRE_FALSE(Nav::ParseChord("ZL+").has_value());
    REQUIRE_FALSE(Nav::ParseChord("").has_value());
    REQUIRE(Nav::ButtonBit("dup") == Nav::BtnUp);
    REQUIRE(Nav::ButtonBit("nope") == 0);
}

TEST_CASE("DSMod nav: manifest keys", "[dsmod][runtime17][nav]") {
    SECTION("absent: off for a package older than runtime 17") {
        // A published package (no min_runtime, or one below 17) keeps its behaviour: no chord,
        // no frame, the game keeps its pad.
        auto j = MinimalManifest();
        REQUIRE_FALSE(Parse(j).nav.enabled);
        j["min_runtime"] = 16;
        REQUIRE_FALSE(Parse(j).nav.enabled);
        REQUIRE_FALSE(NavDefaultOn(&j, nullptr));
        // min_runtime >= 17 in package.json alone (the loader applies NavDefaultOn).
        const nlohmann::json package = {{"min_runtime", 17}};
        REQUIRE(NavDefaultOn(&j, &package));
        REQUIRE_FALSE(NavDefaultOn(&j, nullptr));
        // Explicit opt-in on an old package.
        j["nav"] = true;
        REQUIRE(Parse(j).nav.enabled);
        j["nav"] = {{"color", "#FF00FF00"}};
        REQUIRE(Parse(j).nav.enabled);
    }
    SECTION("absent with min_runtime 17: on, default chord, amber frame") {
        auto j = MinimalManifest();
        j["min_runtime"] = 17;
        const Manifest m = Parse(j);
        REQUIRE(m.nav.enabled);
        REQUIRE(m.nav.toggle_mask == 0);
        REQUIRE(m.nav.color == 0xFFFFC107u);
        REQUIRE(m.nav.frame == 3);
        REQUIRE(m.nav.haptic == -1);
        REQUIRE(m.pages[0].nav_order.empty());
    }
    SECTION("opt out") {
        auto j = MinimalManifest();
        j["min_runtime"] = 17;
        j["nav"] = false;
        REQUIRE_FALSE(Parse(j).nav.enabled);
        j["nav"] = {{"enabled", false}};
        REQUIRE_FALSE(Parse(j).nav.enabled);
    }
    SECTION("chord, style, haptic, nav_order") {
        auto j = MinimalManifest();
        j["nav"] = {{"toggle", "ZL+ZR"}, {"color", "#FF00FF00"}, {"frame", 0}, {"src", "file:f.png"},
                    {"haptic", "click"}};
        j["pages"][0]["nav_order"] = {"b", "slot_{i}", 5};
        const Manifest m = Parse(j);
        REQUIRE(m.nav.enabled);
        REQUIRE(m.nav.toggle == "ZL+ZR");
        REQUIRE(m.nav.toggle_mask == (ZL | ZR));
        REQUIRE(m.nav.color == 0xFF00FF00u);
        REQUIRE(m.nav.frame == 0);
        REQUIRE(m.nav.src == "file:f.png");
        REQUIRE(m.nav.haptic == static_cast<s8>(HapticStrength::Click));
        REQUIRE(m.pages[0].nav_order == std::vector<std::string>{"b", "slot_{i}"});
    }
    SECTION("a bad chord keeps the default; haptics.nav") {
        auto j = MinimalManifest();
        j["nav"] = {{"toggle", "A"}};
        j["haptics"] = {{"enabled", true}, {"nav", "light"}};
        const Manifest m = Parse(j);
        REQUIRE(m.nav.toggle_mask == 0);
        REQUIRE(m.nav.toggle.empty());
        REQUIRE(m.nav.haptic == static_cast<s8>(HapticStrength::Light));
        // "nav" is not a HapticKind: the haptics table itself is unchanged.
        REQUIRE(m.haptics.strength == HapticsConfig{}.strength);
    }
}

TEST_CASE("DSMod nav: the toggle chord enters and leaves", "[dsmod][runtime17][nav]") {
    Nav::Controller c;
    const u64 chord = ZL | ZR;
    REQUIRE_FALSE(c.Step(ZL, 0, chord).entered);
    auto ev = c.Step(ZL | ZR, 16, chord);
    REQUIRE(ev.entered);
    REQUIRE(c.Active());
    // Still holding it: nothing, and no re-toggle.
    ev = c.Step(ZL | ZR, 32, chord);
    REQUIRE_FALSE(ev.left);
    REQUIRE_FALSE(ev.activate);
    c.Step(0, 48, chord);
    // Pressing it again leaves.
    REQUIRE(c.Step(ZL | ZR, 64, chord).left);
    REQUIRE_FALSE(c.Active());
    // Default chord when the mask is 0.
    c.Step(0, 80, 0);
    REQUIRE(c.Step(Nav::DefaultToggleMask, 96, 0).entered);
    // B leaves; B held while inactive does nothing.
    REQUIRE(c.Step(Nav::BtnB, 112, 0).left);
    REQUIRE_FALSE(c.Step(Nav::BtnB, 128, 0).entered);
    // A off the mode is not an activation.
    REQUIRE_FALSE(c.Step(Nav::BtnA, 144, 0).activate);
    c.ForceOff();
    REQUIRE_FALSE(c.Active());
}

TEST_CASE("DSMod nav: A activates once per press, the D-pad moves with repeat",
          "[dsmod][runtime17][nav]") {
    Nav::Controller c;
    c.Step(0, 0, 0);
    REQUIRE(c.Step(Nav::DefaultToggleMask, 10, 0).entered);
    c.Step(0, 20, 0);
    REQUIRE(c.Step(Nav::BtnA, 30, 0).activate);
    REQUIRE_FALSE(c.Step(Nav::BtnA, 40, 0).activate); // held: once
    c.Step(0, 50, 0);
    REQUIRE(c.Step(Nav::BtnDown, 100, 0).move == Dir::Down);
    REQUIRE(c.Step(Nav::BtnDown, 200, 0).move == Dir::None);
    REQUIRE(c.Step(Nav::BtnDown, 100 + Nav::Controller::RepeatDelayMs, 0).move == Dir::Down);
    REQUIRE(c.Step(Nav::BtnDown, 100 + Nav::Controller::RepeatDelayMs + 10, 0).move == Dir::None);
    REQUIRE(c.Step(Nav::BtnDown,
                   100 + Nav::Controller::RepeatDelayMs + Nav::Controller::RepeatPeriodMs, 0)
                .move == Dir::Down);
    // A new direction moves at once; the left stick's digital directions count too.
    REQUIRE(c.Step(Nav::BtnStickLRight, 1000, 0).move == Dir::Right);
    c.Step(0, 1010, 0);
    REQUIRE(c.Step(Nav::BtnStickLUp, 1020, 0).move == Dir::Up);
    // A direction held while the chord opens the mode is not a move.
    Nav::Controller d;
    d.Step(0, 0, ZL | Nav::BtnDown);
    REQUIRE(d.Step(ZL | Nav::BtnDown, 10, ZL | Nav::BtnDown).entered);
    REQUIRE(d.Step(Nav::BtnDown, 20, ZL | Nav::BtnDown).move == Dir::None);
}

TEST_CASE("DSMod nav: geometric movement", "[dsmod][runtime17][nav]") {
    // A 3x3 grid, 100 px apart, plus a wide bar under it.
    std::vector<Nav::Candidate> c;
    for (s32 row = 0; row < 3; ++row) {
        for (s32 col = 0; col < 3; ++col) {
            c.push_back(Cand(col * 100, row * 100));
        }
    }
    c.push_back(Cand(0, 300, 240, 40)); // 9
    const auto all = All(c.size());
    REQUIRE(Nav::MoveGeometric(c, all, 4, Dir::Up) == 1u);
    REQUIRE(Nav::MoveGeometric(c, all, 4, Dir::Down) == 7u);
    REQUIRE(Nav::MoveGeometric(c, all, 4, Dir::Left) == 3u);
    REQUIRE(Nav::MoveGeometric(c, all, 4, Dir::Right) == 5u);
    // Edges: nothing further, no wrap.
    REQUIRE_FALSE(Nav::MoveGeometric(c, all, 0, Dir::Up).has_value());
    REQUIRE_FALSE(Nav::MoveGeometric(c, all, 2, Dir::Right).has_value());
    // The bar overlaps every column across the axis: Down from any bottom cell lands on it.
    REQUIRE(Nav::MoveGeometric(c, all, 8, Dir::Down) == 9u);
    REQUIRE(Nav::MoveGeometric(c, all, 6, Dir::Down) == 9u);
    // Back up from the bar: the cell straight above its centre wins over the diagonal ones.
    REQUIRE(Nav::MoveGeometric(c, all, 9, Dir::Up) == 7u);
    // Only `allowed` candidates are considered.
    const std::vector<size_t> some{0, 2, 4};
    REQUIRE(Nav::MoveGeometric(c, some, 0, Dir::Right) == 2u);
    // Initial focus: top-most, then left-most.
    REQUIRE(Nav::FirstFocus(c, std::vector<size_t>{5, 4, 1, 2}, false) == 1u);
    REQUIRE(Nav::Nearest(c, all, 210, 110) == 5u);
}

TEST_CASE("DSMod nav: nav_order", "[dsmod][runtime17][nav]") {
    std::vector<Nav::Candidate> c{Cand(0, 0, 40, 40, "a"), Cand(0, 50, 40, 40, "slot_0"),
                                  Cand(0, 100, 40, 40, "slot_1"), Cand(0, 150, 40, 40, "b"),
                                  Cand(0, 200, 40, 40, "loose")};
    c[1].template_id = c[2].template_id = "slot_{i}";
    const auto order = Nav::Order(c, {"b", "slot_{i}", "a", "missing"});
    REQUIRE(order == std::vector<size_t>{3, 1, 2, 0}); // "loose" is not listed: not focusable
    REQUIRE(Nav::FirstFocus(c, order, true) == 3u);
    REQUIRE(Nav::MoveOrdered(order, 3, Dir::Down) == 1u);
    REQUIRE(Nav::MoveOrdered(order, 3, Dir::Right) == 1u);
    REQUIRE(Nav::MoveOrdered(order, 3, Dir::Up) == 0u); // wraps to the end
    REQUIRE(Nav::MoveOrdered(order, 0, Dir::Down) == 3u); // and to the start
    REQUIRE(Nav::MoveOrdered(order, 4, Dir::Down) == 3u); // off the list: the first
    REQUIRE_FALSE(Nav::MoveOrdered(std::vector<size_t>{2}, 2, Dir::Down).has_value());
    REQUIRE(Nav::Order(c, {}) == All(c.size()));
}

TEST_CASE("DSMod nav: the candidates of a page", "[dsmod][runtime17][nav]") {
    Page page;
    page.widgets.push_back(Button({0, 0, 50, 50}, "a"));
    Widget label; // not tappable
    label.type = WidgetType::Label;
    label.rect = {60, 0, 50, 50};
    page.widgets.push_back(label);
    Widget hidden = Button({120, 0, 50, 50}, "hidden");
    hidden.hide_bind = "h";
    hidden.keep_min = 1;
    hidden.keep_max = 1;
    page.widgets.push_back(hidden);
    page.widgets.push_back(Button({180, 0, 50, 50}, "covered"));
    Widget popup; // a blocker drawn over "covered"
    popup.type = WidgetType::Rect;
    popup.rect = {170, 0, 100, 60};
    popup.input_block = true;
    page.widgets.push_back(popup);
    page.widgets.push_back(Button({300, 0, 50, 50}, "offscreen_half"));
    Widget sel; // a selectable cell is focusable too
    sel.type = WidgetType::Rect;
    sel.rect = {0, 100, 40, 40};
    sel.select_group = "g";
    sel.payload = "1";
    page.widgets.push_back(sel);

    StateSnapshot s;
    s.ints["h"] = 0;
    auto c = NavCandidates(page, s, 320, 200);
    REQUIRE(c.size() == 3);
    REQUIRE(c[0].id == "a");
    REQUIRE(c[1].id == "offscreen_half");
    REQUIRE(c[1].shown == std::array<s32, 4>{300, 0, 20, 50}); // clipped to the canvas
    REQUIRE(c[2].source == 6);
    // The gate opens: the hidden button joins, in page order.
    s.ints["h"] = 1;
    c = NavCandidates(page, s, 320, 200);
    REQUIRE(c.size() == 4);
    REQUIRE(c[1].id == "hidden");
    REQUIRE(c[1].source == 2);
    REQUIRE(c[1].element == -1);
}

TEST_CASE("DSMod nav: list rows are candidates with their element", "[dsmod][runtime17][nav]") {
    Page page;
    ScrollRegion region;
    region.id = "list";
    region.rect = {0, 100, 200, 100}; // two 50 px rows visible
    region.row_h = 50;
    page.scrolls.push_back(region);
    Widget row = Button({0, 100, 200, 50}, "row_{i}");
    row.repeat = 6;
    row.repeat_dy = 50;
    row.scroll = "list";
    page.widgets.push_back(row);
    page.widgets.push_back(Button({0, 0, 50, 50}, "top"));
    StateSnapshot s;
    s.ints[ScrollOffsetKey("list")] = 0;
    auto c = NavCandidates(page, s, 400, 400);
    REQUIRE(c.size() == 3);
    REQUIRE(c[0].id == "row_0");
    REQUIRE(c[0].element == 0);
    REQUIRE(c[0].template_id == "row_{i}");
    REQUIRE(c[0].scroll == "list");
    REQUIRE(c[1].element == 1);
    REQUIRE(c[2].id == "top");
    // Scrolled by 75 px: row 1 half shown at the top, rows 2 and 3 below it.
    s.ints[ScrollOffsetKey("list")] = 75;
    c = NavCandidates(page, s, 400, 400);
    REQUIRE(c[0].element == 1);
    REQUIRE(c[0].shown == std::array<s32, 4>{0, 100, 200, 25});
    REQUIRE(c[1].element == 2);
    REQUIRE(Nav::Find(c, 0, 3).has_value());
    REQUIRE_FALSE(Nav::Find(c, 0, 0).has_value());
}

TEST_CASE("DSMod nav: the focus frame", "[dsmod][runtime17][nav]") {
    Canvas canvas;
    canvas.Resize(100, 60);
    Page page;
    Widget cell = Button({10, 10, 30, 30}, "c");
    cell.bg = 0xFF00FF00u;
    cell.color = 0;
    page.widgets.push_back(cell);
    Manifest manifest;
    manifest.background = 0xFF000000u;
    StateSnapshot s;
    REQUIRE(RenderPage(canvas, manifest, page, s));
    REQUIRE(PixelAt(canvas, 10, 10) == 0xFF00FF00u);
    s.ints["@nav.active"] = 1;
    s.ints["@nav.x"] = 10;
    s.ints["@nav.y"] = 10;
    s.ints["@nav.w"] = 30;
    s.ints["@nav.h"] = 30;
    REQUIRE(RenderPage(canvas, manifest, page, s));
    REQUIRE(PixelAt(canvas, 10, 10) == 0xFFFFC107u);  // the frame, 3 px
    REQUIRE(PixelAt(canvas, 12, 25) == 0xFFFFC107u);
    REQUIRE(PixelAt(canvas, 25, 25) == 0xFF00FF00u);  // the inside stays
    manifest.nav.color = 0xFFFF0000u;
    manifest.nav.frame = 0; // fill, like a selection highlight's frame 0
    REQUIRE(RenderPage(canvas, manifest, page, s));
    REQUIRE(PixelAt(canvas, 25, 25) == 0xFFFF0000u);
    // Inactive: no frame, whatever the rect says.
    s.ints["@nav.active"] = 0;
    REQUIRE(RenderPage(canvas, manifest, page, s));
    REQUIRE(PixelAt(canvas, 10, 10) == 0xFF00FF00u);
}

TEST_CASE("DSMod nav: the HID gate suppresses the game's pad", "[dsmod][runtime17][nav]") {
    namespace Gate = Core::HID::DSModPadGate;
    Gate::Reset();
    Core::HID::AnalogStickState l{1000, -2000}, r{3000, 4000};
    u64 buttons = Nav::BtnA | Nav::BtnDown;
    REQUIRE_FALSE(Gate::Apply(0, buttons, l, r)); // off: untouched
    REQUIRE(buttons == (Nav::BtnA | Nav::BtnDown));
    REQUIRE(l.x == 1000);

    Gate::Set(true);
    REQUIRE(Gate::Active());
    REQUIRE(Gate::Apply(0, buttons, l, r));
    REQUIRE(buttons == 0);
    REQUIRE((l.x == 0 && l.y == 0 && r.x == 0 && r.y == 0));

    // What the companion presses for the game itself passes, with its stick.
    Gate::Set(true, X, false, true);
    buttons = X | Nav::BtnA;
    l = {1000, 1000};
    r = {500, 500};
    REQUIRE(Gate::Apply(0, buttons, l, r));
    REQUIRE(buttons == X);
    REQUIRE(l.x == 0);
    REQUIRE(r.x == 500);

    // Leaving: the buttons still held stay hidden until each is released.
    Gate::Latch(0, Nav::BtnB | ZL);
    Gate::Set(false);
    buttons = Nav::BtnB | ZL | Nav::BtnA;
    REQUIRE_FALSE(Gate::Apply(0, buttons, l, r));
    REQUIRE(buttons == Nav::BtnA);
    buttons = ZL; // B released
    Gate::Apply(0, buttons, l, r);
    REQUIRE(buttons == 0);
    REQUIRE(Gate::Latched(0) == ZL);
    buttons = Nav::BtnB | ZL; // B pressed again after its release: the game sees it, not ZL
    Gate::Apply(0, buttons, l, r);
    REQUIRE(buttons == Nav::BtnB);
    REQUIRE(Gate::Latched(0) == ZL);
    buttons = 0; // ZL released
    Gate::Apply(0, buttons, l, r);
    REQUIRE(Gate::Latched() == 0);
    buttons = ZL;
    Gate::Apply(0, buttons, l, r);
    REQUIRE(buttons == ZL);
    Gate::Reset();
    REQUIRE_FALSE(Gate::Active());
}

TEST_CASE("DSMod nav: the latch is per npad slot", "[dsmod][runtime17][nav]") {
    namespace Gate = Core::HID::DSModPadGate;
    Gate::Reset();
    Core::HID::AnalogStickState l{}, r{};
    constexpr size_t P1 = 0, P2 = 1, Handheld = 8;
    // B closed the mode on the handheld; Player1 and a P2 pad are connected too. Only the
    // handheld held it, so only its slot latches it.
    Gate::Latch(Handheld, Nav::BtnB);
    Gate::Latch(P1, 0);
    u64 p1 = 0; // Player1 does not hold B: its sample must not unlatch it for the handheld
    Gate::Apply(P1, p1, l, r);
    u64 p2 = 0;
    Gate::Apply(P2, p2, l, r);
    REQUIRE(Gate::Latched(P1) == 0);
    REQUIRE(Gate::Latched(P2) == 0);
    u64 hh = Nav::BtnB;
    Gate::Apply(Handheld, hh, l, r);
    REQUIRE(hh == 0); // still hidden from the game
    REQUIRE(Gate::Latched(Handheld) == Nav::BtnB);
    REQUIRE(Gate::Latched() == Nav::BtnB);
    p1 = Nav::BtnB; // Player1 pressing B after its own release is a real press
    Gate::Apply(P1, p1, l, r);
    REQUIRE(p1 == Nav::BtnB);
    hh = 0; // released on the handheld
    Gate::Apply(Handheld, hh, l, r);
    REQUIRE(Gate::Latched() == 0);
    hh = Nav::BtnB;
    Gate::Apply(Handheld, hh, l, r);
    REQUIRE(hh == Nav::BtnB);
    REQUIRE(Gate::Latched(99) == 0); // out of range: nothing
    Gate::Reset();
}
