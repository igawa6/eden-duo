// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Dual screen mode Off (Stage 3): SetCompanionsDisabledForLaunch/CompanionsDisabledForLaunch is a
// single process-wide flag the frontend sets once at boot, read by SelectLoadPlanPackage
// (mod_load_plan.cpp) and System::Load (core.cpp) to skip companion load-plan selection and
// runtime discovery/initialisation for that one launch. Those two call sites need a live
// Core::System with a mounted mod load root to exercise end to end (not built here, see
// src/tests/core/mods/manifest_digest.cpp and map_pan_zoom_host_value.cpp for the same boundary);
// this test covers the flag itself, which both call sites trust verbatim.

#include <catch2/catch_test_macros.hpp>

#include "core/mods/mod_launch_gate.h"

namespace Core::Mods {
namespace {

// Restores the flag to its previous value on scope exit, so this test never leaks state into
// whichever other test case the shared Catch2 binary runs next.
class ScopedCompanionsDisabled {
public:
    ScopedCompanionsDisabled() : previous{CompanionsDisabledForLaunch()} {}
    ~ScopedCompanionsDisabled() {
        SetCompanionsDisabledForLaunch(previous);
    }

private:
    bool previous;
};

} // namespace

TEST_CASE("DSMod launch gate defaults to companions enabled and round-trips",
          "[dsmod][launch-gate]") {
    const ScopedCompanionsDisabled restore;
    // Nothing else in this test binary calls SetCompanionsDisabledForLaunch; the production
    // default (companions enabled, i.e. dual screen mode is not Off) is std::atomic<bool>'s
    // zero-initialisation in mod_launch_gate.cpp.
    REQUIRE_FALSE(CompanionsDisabledForLaunch());

    SetCompanionsDisabledForLaunch(true);
    REQUIRE(CompanionsDisabledForLaunch());

    SetCompanionsDisabledForLaunch(false);
    REQUIRE_FALSE(CompanionsDisabledForLaunch());

    SetCompanionsDisabledForLaunch(true);
    REQUIRE(CompanionsDisabledForLaunch());
}

} // namespace Core::Mods
