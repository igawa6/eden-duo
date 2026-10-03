// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 16: the @clock.* / @game.seconds points (mod_clock.h) with an injected clock, and the
// derived "countdown" form.
#include <optional>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_clock.h"
#include "core/mods/mod_runtime.h"

using namespace Core::Mods;

namespace {
ExprLookup FromSnapshot(const StateSnapshot& s) {
    return [&s](const std::string& key) -> std::optional<f64> {
        if (const auto it = s.ints.find(key); it != s.ints.end()) {
            return static_cast<f64>(it->second);
        }
        return std::nullopt;
    };
}
} // namespace

TEST_CASE("DSMod clock: UTC breakdown of known instants", "[dsmod][runtime16][clock]") {
    const ClockReading epoch0 = UtcClockReading(0);
    REQUIRE(epoch0.year == 1970);
    REQUIRE(epoch0.month == 1);
    REQUIRE(epoch0.day == 1);
    REQUIRE(epoch0.wday == 4); // Thursday

    // 2026-10-02 13:45:07 UTC, a Friday.
    const ClockReading r = UtcClockReading(1790948707);
    REQUIRE(r.epoch == 1790948707);
    REQUIRE(r.year == 2026);
    REQUIRE(r.month == 10);
    REQUIRE(r.day == 2);
    REQUIRE(r.hour == 13);
    REQUIRE(r.minute == 45);
    REQUIRE(r.second == 7);
    REQUIRE(r.wday == 5);
}

TEST_CASE("DSMod clock: points published from an injected source", "[dsmod][runtime16][clock]") {
    s64 now = 1790948707;
    s64 game = 42;
    ClockPublisher clock;
    clock.SetSource({.epoch_now = [&] { return now; },
                     .breakdown = UtcClockReading,
                     .game_seconds = [&] { return game; }});
    StateSnapshot s;
    clock.Publish(s);
    REQUIRE(s.ints.at("@clock.hour") == 13);
    REQUIRE(s.ints.at("@clock.minute") == 45);
    REQUIRE(s.ints.at("@clock.second") == 7);
    REQUIRE(s.ints.at("@clock.day") == 2);
    REQUIRE(s.ints.at("@clock.month") == 10);
    REQUIRE(s.ints.at("@clock.year") == 2026);
    REQUIRE(s.ints.at("@clock.wday") == 5);
    REQUIRE(s.ints.at("@clock.epoch") == 1790948707);
    REQUIRE(s.ints.at("@game.seconds") == 42);
    REQUIRE(clock.Breakdowns() == 1);

    // Ticks within the same second reuse the breakdown and publish identical values (the redraw
    // signature only moves when a second passes).
    for (int i = 0; i < 59; ++i) {
        StateSnapshot again;
        clock.Publish(again);
        for (const auto& [key, value] : s.ints) {
            REQUIRE(again.ints.at(key) == value);
        }
    }
    REQUIRE(clock.Breakdowns() == 1);

    // A second later: one more breakdown, rolled over across midnight of a month end.
    now = 1793491199; // 2026-10-31 23:59:59 UTC
    clock.Publish(s);
    now += 1;
    game = 43;
    clock.Publish(s);
    REQUIRE(clock.Breakdowns() == 3);
    REQUIRE(s.ints.at("@clock.month") == 11);
    REQUIRE(s.ints.at("@clock.day") == 1);
    REQUIRE(s.ints.at("@clock.hour") == 0);
    REQUIRE(s.ints.at("@clock.wday") == 0); // Sunday
    REQUIRE(s.ints.at("@game.seconds") == 43);
}

TEST_CASE("DSMod clock: default source and key classification", "[dsmod][runtime16][clock]") {
    ClockPublisher clock; // system clock, device local time, game time 0
    StateSnapshot s;
    clock.Publish(s);
    REQUIRE(s.ints.at("@clock.epoch") > 1700000000);
    REQUIRE(s.ints.at("@clock.month") >= 1);
    REQUIRE(s.ints.at("@clock.month") <= 12);
    REQUIRE(s.ints.at("@game.seconds") == 0);
    REQUIRE(IsClockKey("@clock.hour"));
    REQUIRE(IsClockKey("@game.seconds"));
    REQUIRE_FALSE(IsClockKey("@flag:clock"));
    REQUIRE_FALSE(IsClockKey("clock.hour"));
}

TEST_CASE("DSMod clock: only a package that names the keys gets them",
          "[dsmod][runtime16][clock]") {
    const auto page = nlohmann::json::parse(
        R"({"pages":[{"id":"p","widgets":[{"type":"label","rect":[0,0,10,10],"bind":"hp"}]}]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(page, m));
    REQUIRE_FALSE(m.uses_clock_keys); // no once-a-second churn for a package that ignores them
    REQUIRE_FALSE(JsonReferencesClockKeys(page));
    auto bound = page;
    bound["pages"][0]["widgets"][0]["bind"] = "@clock.hour";
    REQUIRE(JsonReferencesClockKeys(bound));
    Manifest m2;
    REQUIRE(ParseDualScreenManifest(bound, m2));
    REQUIRE(m2.uses_clock_keys);
    REQUIRE(JsonReferencesClockKeys(nlohmann::json::parse(
        R"({"derived":{"t":{"expr":"@game.seconds * 2"}}})")));
    REQUIRE(JsonReferencesClockKeys(
        nlohmann::json::parse(R"({"derived":{"left":{"countdown":"deadline"}}})")));
    REQUIRE(JsonReferencesClockKeys(nlohmann::json::parse(R"({"gates":{"@clock.hour":1}})")));
    REQUIRE_FALSE(JsonReferencesClockKeys(nlohmann::json::parse(R"({"a":["@flag:clock", 3]})")));
}

TEST_CASE("DSMod countdown: evaluation", "[dsmod][runtime16][countdown]") {
    StateSnapshot s;
    s.ints["@clock.epoch"] = 1000;
    s.ints["@game.seconds"] = 30;
    s.ints["deadline"] = 1090;
    const auto lookup = FromSnapshot(s);

    DerivedPoint d;
    d.countdown_target.name = "deadline";
    d.countdown_now = "@clock.epoch";
    REQUIRE(EvaluateCountdown(d, lookup) == 90.0);

    s.ints["deadline"] = 900; // in the past: clamped at 0
    REQUIRE(EvaluateCountdown(d, lookup) == 0.0);

    DerivedPoint game;
    game.countdown_target = {.is_const = true, .const_value = 300.0};
    game.countdown_now = "@game.seconds";
    REQUIRE(EvaluateCountdown(game, lookup) == 270.0);

    DerivedPoint missing;
    missing.countdown_target.name = "nope";
    missing.countdown_now = "@clock.epoch";
    REQUIRE_FALSE(EvaluateCountdown(missing, lookup).has_value());
}

TEST_CASE("DSMod countdown: manifest keys", "[dsmod][runtime16][countdown]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"name":"Pkg",
        "pages":[{"id":"p","widgets":[]}],
        "derived":[
          {"name":"left","countdown":"deadline"},
          {"name":"boot","countdown":300,"now":"@game.seconds"},
          {"name":"bad","countdown":true}
        ]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.derived.size() == 3);
    REQUIRE(m.derived[0].countdown_target.name == "deadline");
    REQUIRE(m.derived[0].countdown_now == "@clock.epoch");
    REQUIRE(m.derived[1].countdown_target.is_const);
    REQUIRE(m.derived[1].countdown_target.const_value == 300.0);
    REQUIRE(m.derived[1].countdown_now == "@game.seconds");
    REQUIRE(m.derived[2].countdown_now.empty()); // not a countdown: ignored
}
