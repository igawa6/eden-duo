// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_load_plan.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {
namespace {

std::vector<u8> Words(std::initializer_list<u32> words) {
    std::vector<u8> bytes(words.size() * sizeof(u32));
    std::memcpy(bytes.data(), words.begin(), bytes.size());
    return bytes;
}

} // namespace

TEST_CASE("DSMod load plan applies mailbox relocations", "[dsmod][load-plan]") {
    constexpr VAddr LoadBase = 0x100000;
    constexpr VAddr Mailbox = 0x23456000;
    constexpr u64 PayloadOffset = 0x100;
    std::vector<u8> image(0x200);
    const auto instructions = Words({0x90000005, 0x910000A5}); // ADRP x5; ADD x5, x5, #0

    ModLoadPlan::Write payload{
        .offset = PayloadOffset,
        .expected = std::vector<u8>(instructions.size()),
        .replacement = instructions,
        .relocations = {{0, ModLoadPlan::RelocationKind::AArch64Adrp, 0},
                        {4, ModLoadPlan::RelocationKind::AArch64AddLo12, 0}},
    };
    const ModLoadPlan plan{0x1000, {std::move(payload)}};

    REQUIRE(plan.Apply(image, LoadBase, Mailbox));
    u32 adrp{}, add{};
    std::memcpy(&adrp, image.data() + PayloadOffset, sizeof(adrp));
    std::memcpy(&add, image.data() + PayloadOffset + 4, sizeof(add));
    const u64 encoded = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7FFFF) << 2);
    const s64 pages = static_cast<s64>(encoded << 43) >> 43;
    const VAddr decoded_page = ((LoadBase + PayloadOffset) & ~0xFFFULL) + pages * 0x1000;
    REQUIRE(decoded_page == (Mailbox & ~0xFFFULL));
    REQUIRE(((add >> 10) & 0xFFF) == (Mailbox & 0xFFF));
}

TEST_CASE("DSMod load plan validation is transactional", "[dsmod][load-plan]") {
    std::vector<u8> image{1, 2, 3, 4};
    const auto original = image;
    ModLoadPlan plan{
        0x1000,
        {{.offset = 0, .expected = {1}, .replacement = {9}},
         {.offset = 3, .expected = {8}, .replacement = {7}}},
    };

    REQUIRE_FALSE(plan.Apply(image, 0x1000, 0x4000));
    REQUIRE(image == original);
}

TEST_CASE("DSMod load plan rejects malformed relocation and write bounds", "[dsmod][load-plan]") {
    std::vector<u8> image(16);
    auto replacement = Words({0x910000A5});
    ModLoadPlan::Write write{
        .offset = 0, .expected = std::vector<u8>(4), .replacement = replacement};
    SECTION("replacement cannot exceed checked region") {
        write.replacement.resize(20);
    }
    SECTION("relocation must stay within payload") {
        write.relocations = {{4, ModLoadPlan::RelocationKind::AArch64AddLo12, 0}};
    }
    SECTION("relocation cannot address outside mailbox") {
        write.relocations = {{0, ModLoadPlan::RelocationKind::AArch64AddLo12, 0x1000}};
    }
    SECTION("ADD relocation must not use shifted immediate") {
        write.replacement = Words({0x914000A5});
        write.relocations = {{0, ModLoadPlan::RelocationKind::AArch64AddLo12, 0}};
    }
    const auto original = image;
    REQUIRE_FALSE(ModLoadPlan{0x1000, {write}}.Apply(image, 0x10000, 0x20000));
    REQUIRE(image == original);
}

TEST_CASE("DSMod loader and runtime share manifest usability rules", "[dsmod][load-plan]") {
    REQUIRE_FALSE(IsUsableDualScreenManifest(nlohmann::json::parse(R"({"pages":[]})")));
    // clang-format off: Catch2 stringifies the expression, keep its original layout
    REQUIRE_FALSE(IsUsableDualScreenManifest(nlohmann::json::parse(
        R"({"pages":[{"widgets":[{"src":17}]}]})")));
    REQUIRE(IsUsableDualScreenManifest(nlohmann::json::parse(
        R"({"pages":[{"id":"race","widgets":[]}]})")));
    // clang-format on
    REQUIRE(IsUsableDualScreenManifest(nlohmann::json::parse(R"({"debug_page":true})")));
}

} // namespace Core::Mods
