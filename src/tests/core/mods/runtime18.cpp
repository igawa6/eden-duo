// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 18: a module re-requests its font through "__font_epoch" (mod_font_epoch.h),
// and the runtime 15/17 forms a package uses for a gate over a value that may be unpublished
// ("view_custom:<id>" before the view was ever touched).
#include <map>
#include <optional>
#include <string>
#include <catch2/catch_test_macros.hpp>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/mod_expr.h"
#include "core/mods/mod_font_epoch.h"

using namespace Core::Mods;

TEST_CASE("DSMod font epoch: a module that never publishes it is never asked again",
          "[dsmod][runtime18][font]") {
    FontEpochWatch w;
    for (int tick = 0; tick < 100; ++tick) {
        w.Observe(std::nullopt);
        REQUIRE_FALSE(w.Due(true));
        REQUIRE_FALSE(w.Due(false));
    }
    w.Observe(0); // publishing 0 is the same as not publishing
    REQUIRE_FALSE(w.Due(true));
}

TEST_CASE("DSMod font epoch: a changed epoch re-requests the font once it is settled",
          "[dsmod][runtime18][font]") {
    FontEpochWatch w;
    // first decode (the Latin set: the game language is not known yet)
    w.Observe(0);
    w.Decoding();
    REQUIRE_FALSE(w.Due(true));
    // the language is read: the module counts up
    w.Observe(1);
    REQUIRE_FALSE(w.Due(false)); // a decode still in its bounded retry sees the new state itself
    REQUIRE(w.Due(true));
    w.Decoding(); // the host calls decode_font again
    REQUIRE_FALSE(w.Due(true));
    w.Observe(1);
    REQUIRE_FALSE(w.Due(true)); // the same epoch next tick: nothing more
    // the player changes the language: once more
    w.Observe(2);
    REQUIRE(w.Due(true));
    w.Decoding();
    REQUIRE_FALSE(w.Due(true));
    // a module that stops publishing (reloaded, restarted) reads 0: a change as well
    w.Observe(std::nullopt);
    REQUIRE(w.Due(true));
    w.Decoding();
    REQUIRE_FALSE(w.Due(true));
}

TEST_CASE("DSMod font epoch: a decode that ran mid-change belongs to the epoch it saw",
          "[dsmod][runtime18][font]") {
    FontEpochWatch w;
    w.Observe(3);
    w.Decoding(); // the bounded retry ran after the module published 3
    REQUIRE_FALSE(w.Due(true));
    w.Observe(3);
    REQUIRE_FALSE(w.Due(true));
}

TEST_CASE("DSMod font epoch: the host capability is a new bit", "[dsmod][runtime18][font]") {
    constexpr u64 older = EDEN_DSMOD_CAP_WRITE_MEMORY | EDEN_DSMOD_CAP_GUEST_CALL |
                          EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_MAP_OUTPUT | (u64{1} << 4) |
                          EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN |
                          EDEN_DSMOD_CAP_SOURCE_PREFIXES | EDEN_DSMOD_CAP_SOURCE_BASE |
                          EDEN_DSMOD_CAP_SOURCE_AOC | EDEN_DSMOD_CAP_SOURCE_USER;
    STATIC_REQUIRE((older & EDEN_DSMOD_CAP_FONT_EPOCH) == 0);
    STATIC_REQUIRE(EDEN_DSMOD_CAP_FONT_EPOCH == (u64{1} << 11));
    STATIC_REQUIRE(FontEpochKey == "__font_epoch");
}

namespace {
std::map<std::string, f64> g_vals;
std::optional<f64> Look(const std::string& name) {
    const auto it = g_vals.find(name);
    return it != g_vals.end() ? std::optional<f64>(it->second) : std::nullopt;
}
} // namespace

TEST_CASE("DSMod gate over an unpublished view_custom: expr short-circuit (runtime 17)",
          "[dsmod][runtime18][expr]") {
    // A map gate: the view_custom value of a map widget whose view was never touched is not
    // published (no view state yet), so any_nonzero over it fails closed (missing). An expression
    // that names the always-published sources first answers 1 without reading it.
    const auto p = CompileExpr("'ui.res.has' || 'ui.map.framed' || 'view_custom:demo_map'");
    REQUIRE(p->Ok());
    g_vals = {{"ui.res.has", 1.0}, {"ui.map.framed", 1.0}};
    REQUIRE(EvaluateExpr(*p, Look) == 1.0);
    g_vals = {{"ui.res.has", 0.0}, {"ui.map.framed", 1.0}};
    REQUIRE(EvaluateExpr(*p, Look) == 1.0);
    g_vals = {{"ui.res.has", 0.0}, {"ui.map.framed", 0.0}};
    REQUIRE_FALSE(EvaluateExpr(*p, Look).has_value()); // missing: a hide gate reads 0 (shown)
    g_vals = {{"ui.res.has", 0.0}, {"ui.map.framed", 0.0}, {"view_custom:demo_map", 1.0}};
    REQUIRE(EvaluateExpr(*p, Look) == 1.0);
}

TEST_CASE("DSMod font epoch: atlas invalidation includes numeric pages only",
          "[dsmod][runtime18][font]") {
    REQUIRE(FontAtlasKeyMatches("module:font", "module:font"));
    REQUIRE_FALSE(FontAtlasKeyMatches("module:font", "module:font/0"));
    REQUIRE(FontAtlasKeyMatches("module:font/{p}.rgba", "module:font/12.rgba"));
    REQUIRE_FALSE(FontAtlasKeyMatches("module:font/{p}.rgba", "module:font/.rgba"));
    REQUIRE_FALSE(FontAtlasKeyMatches("module:font/{p}.rgba", "module:font/x.rgba"));
    REQUIRE_FALSE(FontAtlasKeyMatches("module:font/{p}.rgba", "module:other/12.rgba"));
}
