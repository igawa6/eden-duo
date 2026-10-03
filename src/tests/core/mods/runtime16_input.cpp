// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 16 input and module fixes: hold and drag on one widget (HoldArmsOverDrag), module action
// refusals (CallModuleAction), the "@drag*" ints gates see before the taps (WriteDragInts), the
// module image key limit, and the romfs source retrying a failed open (AssetSources retry_ms).
#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/vfs/vfs_vector.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/mod_input_drag.h"
#include "core/mods/mod_input_hold.h"
#include "core/mods/mod_module.h"
#include "core/mods/mod_sources.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
Widget Rect(std::array<s32, 4> rect) {
    Widget w;
    w.type = WidgetType::Rect;
    w.rect = rect;
    return w;
}

// The first-finger choice UpdateGestures makes: the drag candidate (topmost draggable), then the
// hold (topmost on_hold / input_block) and whether it arms over that candidate.
struct FirstFinger {
    s64 drag{-1};
    s64 hold{-1};
    bool hold_arms{false};
};
FirstFinger Pick(const std::vector<Widget>& widgets, s32 x, s32 y) {
    const StateSnapshot state;
    FirstFinger f;
    f.drag = HitTestIndex(widgets, state, x, y, [](const Widget& w) { return w.draggable; });
    f.hold = HitTestIndex(widgets, state, x, y,
                          [](const Widget& w) { return !w.on_hold.empty() || w.input_block; });
    f.hold_arms = f.hold >= 0 && !widgets[static_cast<size_t>(f.hold)].on_hold.empty() &&
                  HoldArmsOverDrag(f.hold, f.drag);
    return f;
}

// Fake on_action implementations for CallModuleAction.
std::string g_last_action;
s64 g_last_argument{};
EdenDsmodBool Accepts(void*, const char* action, int64_t argument) {
    g_last_action = action;
    g_last_argument = argument;
    return EDEN_DSMOD_TRUE;
}
EdenDsmodBool Declines(void*, const char* action, int64_t argument) {
    g_last_action = action;
    g_last_argument = argument;
    return EDEN_DSMOD_FALSE;
}
EdenDsmodBool Throws(void*, const char*, int64_t) {
    throw 1;
}

FileSys::VirtualDir MakeDir(const std::string& content) {
    std::vector<u8> bytes(content.begin(), content.end());
    auto file = std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "a.bin");
    return std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{file}, std::vector<FileSys::VirtualDir>{}, "root");
}
} // namespace

TEST_CASE("DSMod hold+drag: a hold arms on the draggable widget or above it",
          "[dsmod][runtime16][hold]") {
    // Before runtime 16 any drag candidate kept the hold disarmed.
    REQUIRE(HoldArmsOverDrag(3, -1));
    REQUIRE(HoldArmsOverDrag(3, 3));       // the draggable widget's own on_hold
    REQUIRE(HoldArmsOverDrag(4, 3));       // a hold zone drawn above it
    REQUIRE_FALSE(HoldArmsOverDrag(2, 3)); // one beneath it: the drag owns the touch
    REQUIRE_FALSE(HoldArmsOverDrag(-1, 3));
    REQUIRE_FALSE(HoldArmsOverDrag(-1, -1));

    std::vector<Widget> page;
    Widget panel = Rect({0, 0, 400, 400}); // a page-wide hold beneath the items
    panel.on_hold = "panel_hold";
    page.push_back(panel);
    Widget item = Rect({10, 10, 80, 80}); // an item that is dragged and held
    item.draggable = true;
    item.payload = "5";
    item.on_hold = "equip";
    page.push_back(item);
    Widget other = Rect({100, 10, 80, 80}); // dragged only; a hold overlay above it
    other.draggable = true;
    other.payload = "6";
    page.push_back(other);
    Widget overlay = Rect({100, 10, 40, 40});
    overlay.on_hold = "zone_hold";
    page.push_back(overlay);

    auto f = Pick(page, 50, 50);
    REQUIRE(f.drag == 1);
    REQUIRE(f.hold == 1);
    REQUIRE(f.hold_arms);
    f = Pick(page, 120, 20); // the overlay's hold arms over the item beneath it
    REQUIRE(f.drag == 2);
    REQUIRE(f.hold == 3);
    REQUIRE(f.hold_arms);
    f = Pick(page, 160, 80); // only the panel's hold, beneath the draggable item: no hold
    REQUIRE(f.drag == 2);
    REQUIRE(f.hold == 0);
    REQUIRE_FALSE(f.hold_arms);
    f = Pick(page, 300, 300); // no drag: the panel's hold arms as before
    REQUIRE(f.drag == -1);
    REQUIRE(f.hold_arms);
}

TEST_CASE("DSMod hold+drag: moving first drags, holding first holds", "[dsmod][runtime16][hold]") {
    // The tracker side of the arbitration (UpdateGestures): leaving the tap slop is the cancel
    // signal and the drag threshold at once, so a drag that starts always cancels the hold first.
    HoldTracker h;
    h.Down(0, true, 600);
    REQUIRE_FALSE(h.Update(200, false));
    REQUIRE_FALSE(h.Update(250, true)); // moved past the slop at 250 ms: the drag starts
    REQUIRE_FALSE(h.Update(900, false));
    REQUIRE_FALSE(h.Fired());
    REQUIRE_FALSE(h.Up()); // the drop / cancel ends it, never a hold
    // Still until hold_ms: the hold fires; UpdateGestures then drops the drag candidate, so the
    // finger moving afterwards drags nothing and the lift is no tap.
    h.Down(0, true, 600);
    REQUIRE(h.Update(600, false));
    REQUIRE(h.Fired());
    REQUIRE_FALSE(h.Update(700, true));
    REQUIRE(h.Up());
}

TEST_CASE("DSMod module action: accepted, declined, threw, not run", "[dsmod][runtime16][module]") {
    EdenDsmodModuleExtensions ext{};
    int instance = 0;
    ext.on_action = &Accepts;
    REQUIRE(CallModuleAction(&ext, &instance, "equip", 7) == ModuleActionOutcome::Accepted);
    REQUIRE(g_last_action == "equip");
    REQUIRE(g_last_argument == 7);
    ext.on_action = &Declines;
    REQUIRE(CallModuleAction(&ext, &instance, "equip", -1) == ModuleActionOutcome::Declined);
    REQUIRE(g_last_argument == -1);
    ext.on_action = &Throws;
    REQUIRE(CallModuleAction(&ext, &instance, "equip", 0) == ModuleActionOutcome::Declined);
    // Not run: no module, no on_action, no instance, empty or over-long name.
    ext.on_action = &Accepts;
    REQUIRE(CallModuleAction(nullptr, &instance, "equip", 0) == ModuleActionOutcome::NotRun);
    REQUIRE(CallModuleAction(&ext, nullptr, "equip", 0) == ModuleActionOutcome::NotRun);
    REQUIRE(CallModuleAction(&ext, &instance, "", 0) == ModuleActionOutcome::NotRun);
    REQUIRE(CallModuleAction(&ext, &instance, std::string(MaxModuleActionName + 1, 'a'), 0) ==
            ModuleActionOutcome::NotRun);
    REQUIRE(CallModuleAction(&ext, &instance, std::string(MaxModuleActionName, 'a'), 0) ==
            ModuleActionOutcome::Accepted);
    EdenDsmodModuleExtensions none{};
    REQUIRE(CallModuleAction(&none, &instance, "equip", 0) == ModuleActionOutcome::NotRun);
}

TEST_CASE("DSMod drag ints: published for a drag, removed without one",
          "[dsmod][runtime16][drag]") {
    StateSnapshot s;
    // The pass before the taps on a drop tick: the dropped drag, so a gate can read its payload.
    WriteDragInts(s, DragInts{6, 620, 841, 12});
    REQUIRE(s.GetInt("@drag") == 1);
    REQUIRE(s.GetInt("@drag_payload") == 6);
    REQUIRE(s.GetInt("@drag_x") == 620);
    REQUIRE(s.GetInt("@drag_y") == 841);
    REQUIRE(s.GetInt("@drag_hover") == 12);
    // The pass after the taps: no drag, and nothing of the early pass left behind (the page draws
    // exactly what it drew before runtime 16).
    WriteDragInts(s, std::nullopt);
    REQUIRE(s.GetInt("@drag", -5) == 0);
    for (const char* key : {"@drag_payload", "@drag_x", "@drag_y", "@drag_hover"}) {
        REQUIRE_FALSE(s.ints.contains(key));
    }
}

TEST_CASE("DSMod module image keys: up to 4096 characters", "[dsmod][runtime16][module]") {
    REQUIRE(MaxModuleImageKey == 4096);
    // A module map key that encodes its layout: a header plus 12 hex digits per room (22 rooms).
    const std::string map_key =
        "module:demo:map/" + std::string(16, 'a') + std::string(22 * 12, 'b');
    REQUIRE(map_key.size() > 256);
    REQUIRE(ModuleImageKeyOk(map_key));
    REQUIRE(ModuleImageKeyOk(std::string(4096, 'k')));
    REQUIRE_FALSE(ModuleImageKeyOk(std::string(4097, 'k')));
}

TEST_CASE("DSMod sources: a romfs that opened too early is opened again",
          "[dsmod][runtime16][sources]") {
    std::atomic<int> opens{0};
    std::atomic<bool> ready{false};
    const auto open = [&] {
        ++opens;
        return ready ? MakeDir("hello") : FileSys::VirtualDir{};
    };

    // Without retry_ms (every source but romfs): the null root is kept for the session.
    AssetSources latched;
    latched.Register({.prefix = "aoc", .open_dir = open});
    REQUIRE_FALSE(latched.Available("aoc"));
    ready = true;
    REQUIRE(latched.ReadAll("aoc:a.bin").empty());
    REQUIRE(opens == 1);

    // retry_ms 0: the next read opens again and the root then stays.
    opens = 0;
    ready = false;
    AssetSources retried;
    retried.Register({.prefix = "romfs", .open_dir = open, .retry_ms = 0});
    REQUIRE(retried.ReadRange("romfs:a.bin", 0, nullptr, 0) == 0); // a module's create(), too early
    REQUIRE(retried.ReadAll("romfs:a.bin").empty());
    REQUIRE(opens == 2);
    ready = true;
    REQUIRE(retried.ReadAll("romfs:a.bin").size() == 5);
    REQUIRE(retried.ReadRange("romfs:a.bin", 0, nullptr, 0) == 5);
    REQUIRE(opens == 3); // opened: never asked again
    ready = false;
    REQUIRE(retried.Available("romfs"));
    REQUIRE(opens == 3);

    // A long retry_ms: no reopen inside the window.
    opens = 0;
    AssetSources throttled;
    throttled.Register({.prefix = "romfs", .open_dir = open, .retry_ms = 60000});
    REQUIRE_FALSE(throttled.Available("romfs"));
    ready = true;
    REQUIRE_FALSE(throttled.Available("romfs"));
    REQUIRE(opens == 1);
}
