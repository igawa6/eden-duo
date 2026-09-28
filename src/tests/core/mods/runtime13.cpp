// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 13: the redraw worker publishes what a superseded (stale) job painted
// (UnpublishedRegions), press-and-hold (HoldTracker), and the manifest keys of both.
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_input_hold.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
bool Covers(const std::array<s32, 4>& outer, const std::array<s32, 4>& inner) {
    return outer[0] <= inner[0] && outer[1] <= inner[1] &&
           outer[0] + outer[2] >= inner[0] + inner[2] &&
           outer[1] + outer[3] >= inner[1] + inner[3];
}
bool AnyCovers(const RenderExtras& e, const std::array<s32, 4>& r) {
    for (const auto& c : e.clips) {
        if (Covers(c, r)) {
            return true;
        }
    }
    return e.clips.empty() && Covers(e.clip, r);
}
} // namespace

TEST_CASE("DSMod runtime 13: version", "[dsmod][runtime13]") {
    REQUIRE(DualScreenRuntimeVersion >= 13);
}

TEST_CASE("DSMod redraw: a stale job's region is published with the next job",
          "[dsmod][runtime13][redraw]") {
    UnpublishedRegions u;
    REQUIRE(u.Empty());
    // The overtake case: job A (rows 2..3 of the rank table) was running when job B (one item
    // bubble) was dispatched; A finished stale, B publishes -- it must publish A's rows too.
    RenderExtras a;
    a.clip = {104, 90, 246, 180};
    u.Note(a, true);
    REQUIRE_FALSE(u.Empty());
    RenderExtras b;
    b.clip = {203, 90, 130, 87};
    REQUIRE(u.Apply(b, 1240, 1080));
    REQUIRE(AnyCovers(b, {104, 90, 246, 180}));
    REQUIRE(AnyCovers(b, {203, 90, 130, 87}));
    REQUIRE(Covers(b.clip, {104, 90, 246, 180}));
    REQUIRE(u.Empty()); // applied once, then forgotten

    // Several stale partial jobs accumulate, multi-rect clips included.
    RenderExtras c;
    c.clip = {0, 0, 100, 1080};
    c.clips = {{0, 0, 100, 90}, {0, 540, 100, 90}};
    u.Note(c, true);
    RenderExtras d;
    d.clip = {900, 900, 50, 50};
    u.Note(d, true);
    RenderExtras e;
    e.clip = {500, 500, 10, 10};
    REQUIRE(u.Apply(e, 1240, 1080));
    REQUIRE(AnyCovers(e, {0, 0, 100, 90}));
    REQUIRE(AnyCovers(e, {0, 540, 100, 90}));
    REQUIRE(AnyCovers(e, {900, 900, 50, 50}));
    REQUIRE(AnyCovers(e, {500, 500, 10, 10}));

    // A stale FULL redraw forces the next publish to be full.
    RenderExtras full;
    u.Note(full, false);
    RenderExtras f;
    f.clip = {10, 10, 10, 10};
    REQUIRE_FALSE(u.Apply(f, 1240, 1080));
    REQUIRE(u.Empty());
    // A partial job without a clip is the whole canvas too.
    RenderExtras noclip;
    u.Note(noclip, true);
    REQUIRE(u.full);
    u.Clear();
    // Nothing noted: the job publishes unchanged.
    RenderExtras g;
    g.clip = {1, 2, 3, 4};
    REQUIRE(u.Apply(g, 1240, 1080));
    REQUIRE(g.clip == std::array<s32, 4>{1, 2, 3, 4});
    REQUIRE(g.clips.empty());

    // Bounded: past MaxRects notes the next publish is full instead of an ever-growing list.
    for (s32 i = 0; i <= static_cast<s32>(UnpublishedRegions::MaxRects); ++i) {
        RenderExtras s;
        s.clip = {i, i, 4, 4};
        u.Note(s, true);
    }
    REQUIRE(u.full);
    REQUIRE(u.rects.empty());
    RenderExtras h;
    h.clip = {1, 1, 1, 1};
    REQUIRE_FALSE(u.Apply(h, 1240, 1080));
    REQUIRE(u.Empty());
}

TEST_CASE("DSMod hold: a repeat template substitutes {i} in on_hold like on_tap",
          "[dsmod][runtime13][hold]") {
    Page page;
    Widget row;
    row.rect = {0, 0, 100, 10};
    row.repeat_dy = 10;
    row.repeat = 3;
    row.on_tap = "tap.{i}";
    row.on_hold = "hold.{i}";
    page.widgets.push_back(row);
    const StateSnapshot state;
    const auto widgets = ExpandWidgets(page, state);
    REQUIRE(widgets.size() == 3);
    REQUIRE(widgets[2].on_tap == "tap.2");
    REQUIRE(widgets[2].on_hold == "hold.2");
    const s64 hit = HitTestIndex(widgets, state, 50, 25,
                                 [](const Widget& w) { return !w.on_hold.empty(); });
    REQUIRE(hit == 2);
}

TEST_CASE("DSMod hold: a page switch under the still finger cancels the hold",
          "[dsmod][runtime13][hold]") {
    HoldTracker h;
    h.Down(0, true, 300, 2);
    REQUIRE_FALSE(h.Update(100, false, 2));
    REQUIRE_FALSE(h.Update(200, false, 3)); // page bind switched the page mid-hold
    REQUIRE_FALSE(h.Update(1000, false, 2)); // cancelled for good, even back on the same page
    REQUIRE_FALSE(h.Up());
    h.Down(0, true, 300, 5);
    REQUIRE(h.Update(300, false, 5));
    REQUIRE(h.Up());
}

TEST_CASE("DSMod hold: fires once after hold_ms, cancels, eats the tap",
          "[dsmod][runtime13][hold]") {
    HoldTracker h;
    // No on_hold widget under the finger: never fires, the lift is a tap.
    h.Down(1000, false, 600);
    REQUIRE_FALSE(h.Update(5000, false));
    REQUIRE_FALSE(h.Up());
    // Default hold time.
    h.Down(0, true, 0);
    REQUIRE_FALSE(h.Update(DefaultHoldMs - 1, false));
    REQUIRE(h.Update(DefaultHoldMs, false));
    REQUIRE_FALSE(h.Update(DefaultHoldMs + 500, false)); // once only
    REQUIRE(h.Fired());
    REQUIRE(h.Up()); // the lift after a hold is not a tap
    REQUIRE_FALSE(h.Fired());
    // Released before hold_ms: a normal tap.
    h.Down(0, true, 800);
    REQUIRE_FALSE(h.Update(400, false));
    REQUIRE_FALSE(h.Up());
    // Moving (or a second finger, a drag, a page transition) cancels the hold for good.
    h.Down(0, true, 300);
    REQUIRE_FALSE(h.Update(100, true));
    REQUIRE_FALSE(h.Update(1000, false));
    REQUIRE_FALSE(h.Up());
}

TEST_CASE("DSMod hold: manifest keys on_hold / hold_ms", "[dsmod][runtime13][hold]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"min_runtime":13,"pages":[{"id":"p",
        "widgets":[{"type":"rect","rect":[0,0,10,10],"on_tap":"a","on_hold":"b","hold_ms":800},
                   {"type":"rect","rect":[0,0,10,10],"on_hold":"c"}]}],
        "actions":{"a":{"kind":"flag","flag":"x"},"b":{"kind":"flag","flag":"y"},
                   "c":{"kind":"flag","flag":"z"}}})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.pages.size() == 1);
    const auto& w = m.pages[0].widgets;
    REQUIRE(w.size() == 2);
    REQUIRE(w[0].on_tap == "a");
    REQUIRE(w[0].on_hold == "b");
    REQUIRE(w[0].hold_ms == 800);
    REQUIRE(w[1].on_hold == "c");
    REQUIRE(w[1].hold_ms == 0);
}
