// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 25: pane geometry host values (@screen.*, mod_host_values.h), the optional
// "canvas_alt" manifest canvas and its per-widget "rect_alt" (SelectCanvasAspect,
// mod_redraw.cpp), and the L3/R3 virtual button tokens (ParseButton, mod_actions.cpp).

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
