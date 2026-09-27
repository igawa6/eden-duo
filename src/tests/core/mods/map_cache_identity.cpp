// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/mods/map_cache_identity.h"

namespace Core::Mods {
namespace {
struct Shape {
    std::string name;
    std::vector<int> tris;
};
} // namespace

TEST_CASE("Map silhouette ignores actor records that do not remove polygons",
          "[dsmod][map-cache]") {
    const std::vector<Shape> holes{{"wall", {1}}, {"empty", {}}};
    const std::vector<Shape> rooms{{"secret", {1}}};
    const std::set<std::string> none;
    const auto original = ActiveHoleIdentity(holes, rooms, none, none);
    REQUIRE(original == "1.1");
    REQUIRE(ActiveHoleIdentity(holes, rooms, std::set<std::string>{"door", "item", "empty"},
                               none) == original);
    REQUIRE(ActiveHoleIdentity(holes, rooms, none, std::set<std::string>{"unrelated"}) == original);
    REQUIRE(ActiveHoleIdentity(holes, rooms, std::set<std::string>{"wall"}, none) == "0.1");
    REQUIRE(ActiveHoleIdentity(holes, rooms, none, std::set<std::string>{"secret"}) == "1.0");
    REQUIRE(ActiveHoleIdentity(holes, rooms, std::set<std::string>{"wall"},
                               std::set<std::string>{"secret"}) == "0.0");
    // Reloading an earlier save restores the exact prior identity.
    REQUIRE(ActiveHoleIdentity(holes, rooms, none, none) == original);
}

TEST_CASE("Map silhouette identities distinguish hole positions and groups", "[dsmod][map-cache]") {
    const std::vector<Shape> holes{{"left", {1}}, {"right", {1}}};
    const std::vector<Shape> none;
    const std::set<std::string> empty;
    REQUIRE(ActiveHoleIdentity(holes, none, std::set<std::string>{"left"}, empty) !=
            ActiveHoleIdentity(holes, none, std::set<std::string>{"right"}, empty));
    REQUIRE(ActiveHoleIdentity(holes, none, empty, empty) !=
            ActiveHoleIdentity(none, holes, empty, empty));
    REQUIRE(ActiveHoleIdentity(none, none, empty, empty) == ".");
}
} // namespace Core::Mods
