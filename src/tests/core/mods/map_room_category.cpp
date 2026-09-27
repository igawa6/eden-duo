// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <unordered_map>

#include <catch2/catch_test_macros.hpp>

#include "core/mods/mod_types_map.h"

namespace Core::Mods {
namespace {

// Dread's EMMI zone category as the package declares it: grey vEmmyColor (#1E1E1E) while the
// unit lives, green vEmmyDeadColor (#17461B) once defeated, both baked x3 (bake.gain).
constexpr u32 AliveZone = 0x5A5A5A; // #1E1E1E x3
constexpr u32 DeadZone = 0x45D251;  // #17461B x3

MapRoomCategory EmmiCategory() {
    MapRoomCategory cat;
    cat.id = "emmi";
    cat.color_bind = "emmi_state";
    // The dead-state correction the package was tuned with (gain 420, tint #5BFF5D, 112/256).
    cat.visited_gain = 420;
    cat.visited_tint = 0xFF5BFF5D;
    cat.visited_amount = 112;
    return cat;
}

u32 Channel(u32 rgb, int sh) {
    return (rgb >> sh) & 0xFF;
}

} // namespace

TEST_CASE("Visited correction is a no-op by default", "[dsmod][map-category]") {
    const MapRoomCategory cat;
    const auto vc = cat.VisitedFor(std::nullopt);
    CHECK(ApplyVisitedCorrection(AliveZone, vc.gain, vc.tint, vc.amount) == AliveZone);
}

TEST_CASE("One correction for every state turns a live EMMI zone green", "[dsmod][map-category]") {
    // The regression: a single triple solved against the dead colour also runs on the alive grey.
    const MapRoomCategory cat = EmmiCategory();
    const auto vc = cat.VisitedFor(0);
    const u32 alive = ApplyVisitedCorrection(AliveZone, vc.gain, vc.tint, vc.amount);
    CHECK(alive == 0x7BC27C); // (123,194,124): reads as green on the panel
    CHECK(Channel(alive, 8) > Channel(alive, 16) + 50);
}

TEST_CASE("Per-state visited correction keeps a live EMMI zone grey", "[dsmod][map-category]") {
    MapRoomCategory cat = EmmiCategory();
    // Package fix: the tinted triple applies to the defeated state only; the alive state keeps
    // the shared brightness gain and no hue shift.
    auto vm = std::make_shared<std::unordered_map<s64, MapRoomCategory::VisitedCorrection>>();
    vm->emplace(1, MapRoomCategory::VisitedCorrection{420, 0xFF5BFF5D, 112});
    cat.visited_map = vm;
    cat.visited_amount = 0;

    const auto alive_vc = cat.VisitedFor(0);
    const u32 alive =
        ApplyVisitedCorrection(AliveZone, alive_vc.gain, alive_vc.tint, alive_vc.amount);
    CHECK(alive == 0x939393); // 0x5A * 420 / 256 = 147 in every channel: grey

    const auto dead_vc = cat.VisitedFor(1);
    const u32 dead = ApplyVisitedCorrection(DeadZone, dead_vc.gain, dead_vc.tint, dead_vc.amount);
    // Unchanged from the tuned look: (104,255,115).
    CHECK(dead == 0x68FF73);

    // An unresolved bind takes the category-level triple (here: no tint).
    const auto none = cat.VisitedFor(std::nullopt);
    CHECK(none.amount == 0);
}

} // namespace Core::Mods
