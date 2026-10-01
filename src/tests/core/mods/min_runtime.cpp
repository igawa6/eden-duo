// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Package compatibility gating ("min_runtime"): the raw-JSON requirement read, and the built-in
// "update Eden" page shown instead of a package that needs a newer dual-screen runtime.
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {
u32 Need(const char* manifest, const char* package = nullptr) {
    const auto m = nlohmann::json::parse(manifest);
    if (package == nullptr) {
        return PackageMinRuntime(&m, nullptr);
    }
    const auto p = nlohmann::json::parse(package);
    return PackageMinRuntime(&m, &p);
}
constexpr u32 Max = std::numeric_limits<u32>::max();
} // namespace

TEST_CASE("DSMod min_runtime: absent, integer, string, both files", "[dsmod][min-runtime]") {
    REQUIRE(DualScreenRuntimeVersion >= 12);
    REQUIRE(Need(R"({"format":1})") == 0);
    REQUIRE(Need(R"({"min_runtime":null})") == 0);
    REQUIRE(Need(R"({"min_runtime":11})") == 11);
    REQUIRE(Need(R"({"min_runtime":"12"})") == 12);
    // The larger of manifest.json and package.json wins.
    REQUIRE(Need(R"({"min_runtime":11})", R"({"min_runtime":13})") == 13);
    REQUIRE(Need(R"({})", R"({"min_runtime":12})") == 12);
    REQUIRE(Need(R"({"min_runtime":14})", R"({"format":1})") == 14);
    REQUIRE(PackageMinRuntime(nullptr, nullptr) == 0);
    const auto array = nlohmann::json::parse("[1,2]");
    REQUIRE(PackageMinRuntime(&array, nullptr) == 0); // not an object: no requirement read
}

TEST_CASE("DSMod min_runtime: malformed values gate the package", "[dsmod][min-runtime]") {
    REQUIRE(Need(R"({"min_runtime":-1})") == Max);
    REQUIRE(Need(R"({"min_runtime":11.5})") == Max);
    REQUIRE(Need(R"({"min_runtime":"11a"})") == Max);
    REQUIRE(Need(R"({"min_runtime":""})") == Max);
    REQUIRE(Need(R"({"min_runtime":[11]})") == Max);
    REQUIRE(Need(R"({"min_runtime":{"v":11}})") == Max);
    REQUIRE(Need(R"({"min_runtime":true})") == Max);
    REQUIRE(Need(R"({"min_runtime":99999999999})") == Max);
}

TEST_CASE("DSMod min_runtime: read before the full parser, on JSON it cannot parse",
          "[dsmod][min-runtime]") {
    // A future package: keys whose types this runtime's parser does not accept.
    const char* future = R"({"format":1,"min_runtime":999,"frame_hook":7,
        "pages":[{"id":"p","widgets":[{"type":"hologram","rect":"full"}]}]})";
    const auto json = nlohmann::json::parse(future);
    REQUIRE(PackageMinRuntime(&json, nullptr) == 999);
    REQUIRE(PackageMinRuntime(&json, nullptr) > DualScreenRuntimeVersion);
    REQUIRE_FALSE(IsUsableDualScreenManifest(json)); // the parser would have dropped it silently
    // A current package that declares this runtime passes the gate and parses as before.
    const auto ok =
        nlohmann::json::parse(R"({"format":1,"min_runtime":11,"pages":[{"id":"p","widgets":[]}]})");
    REQUIRE(PackageMinRuntime(&ok, nullptr) <= DualScreenRuntimeVersion);
    REQUIRE(IsUsableDualScreenManifest(ok));
}

TEST_CASE("DSMod min_runtime: the built-in update page", "[dsmod][min-runtime]") {
    const Manifest m = ModRuntime::UpdateRequiredManifest(0x01005CA01580E000ull, 12, "P5R");
    REQUIRE(m.valid);
    REQUIRE(m.asset_dir == nullptr); // nothing of the package: no module, font or images
    REQUIRE(m.pages.size() == 1);
    REQUIRE(m.pages[0].id == "update_required");
    std::string all;
    for (const auto& w : m.pages[0].widgets) {
        all += w.text + "|";
    }
    REQUIRE(all.find("This package needs a newer Eden Duo") != std::string::npos);
    REQUIRE(all.find("runtime 12, have " + std::to_string(DualScreenRuntimeVersion)) !=
            std::string::npos);
    REQUIRE(all.find("Update Eden Duo to use it.") != std::string::npos);
    REQUIRE(all.find("P5R") != std::string::npos);
    const Manifest unknown = ModRuntime::UpdateRequiredManifest(1, Max, "x");
    std::string u;
    for (const auto& w : unknown.pages[0].widgets) {
        u += w.text + "|";
    }
    REQUIRE(u.find("runtime ?, have") != std::string::npos);

    // It draws with the built-in font (no package font): ink appears on the canvas.
    Canvas canvas;
    canvas.Resize(m.canvas_w, m.canvas_h);
    StateSnapshot s;
    REQUIRE(RenderPage(canvas, m, m.pages[0], s));
    size_t ink = 0;
    for (const u32 px : canvas.Pixels()) {
        ink += px == 0xFFFFFFFFu || px == 0xFFE6ECF2u || px == 0xFFFFC040u;
    }
    REQUIRE(ink > 1000);
}

// ModRuntime pulls in the whole core (like eden-cli): the VMA implementation lives in the
// frontend, so this test provides it (as tests/common/bit_field.cpp does for `tests`).
#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
