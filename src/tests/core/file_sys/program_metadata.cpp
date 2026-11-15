// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "core/file_sys/program_metadata.h"
#include "core/loader/loader.h"
#include "tests/core/file_sys/metadata_test_file.h"

namespace FileSys {
namespace {

using Tests::MakeNpdm;
using Tests::Put32;

u16 LoadStatus(ProgramMetadata& metadata, const std::vector<u8>& bytes) {
    return u16(metadata.Load(std::make_shared<Tests::MetadataFile>(bytes)));
}

} // namespace

TEST_CASE("NPDM supports NX and Ounce ACID layouts", "[core][file_sys]") {
    const bool ounce = GENERATE(false, true);
    ProgramMetadata metadata;
    REQUIRE(LoadStatus(metadata, MakeNpdm(ounce)) == u16(Loader::ResultStatus::Success));
    REQUIRE(metadata.GetPlatform() == (ounce ? ContentMetaPlatform::Ounce : ContentMetaPlatform::Nx));
    REQUIRE(metadata.GetTitleID() == 0x0400000000010000ULL);
    REQUIRE(metadata.GetFilesystemPermissions() == 0x4000000000000000ULL);
    REQUIRE(metadata.GetRemoteMemorySize() == (ounce ? 0x3000000U : 0U));
    REQUIRE(metadata.GetKernelCapabilities() == std::vector<u32>{0x30043F7});
}

TEST_CASE("NPDM rejects malformed blocks", "[core][file_sys]") {
    auto bytes = MakeNpdm(true);
    ProgramMetadata metadata;
    SECTION("Bad META magic") {
        bytes[0] = 0;
    }
    SECTION("Truncated ACID extent") {
        Put32(bytes, 0x7C, 0xFFFFFF00);
    }
    SECTION("Unknown ACID layout") {
        bytes[0x380] = 0;
    }
    SECTION("Ambiguous ACID layout") {
        std::memcpy(bytes.data() + 0x280, "ACID", 4);
    }
    SECTION("Invalid ACI0 magic") {
        bytes[0x400] = 0;
    }
    SECTION("Overlapping ACI0 and ACID") {
        Put32(bytes, 0x70, 0x100);
    }
    SECTION("FAC outside ACID") {
        Put32(bytes, 0x3A0, 0xFFFFFF00);
    }
    SECTION("FAH outside ACI0") {
        Put32(bytes, 0x420, 0xFFFFFF00);
    }
    SECTION("Misaligned kernel capability size") {
        Put32(bytes, 0x434, 3);
    }
    SECTION("Kernel capabilities outside ACI0") {
        Put32(bytes, 0x430, 0xFFFFFF00);
    }
    REQUIRE(LoadStatus(metadata, bytes) != u16(Loader::ResultStatus::Success));
}

TEST_CASE("Supplied local NPDM samples parse as Ounce", "[.local_sample][file_sys]") {
    const char* sample_root = std::getenv("NSP2_SAMPLE_DUMP");
    REQUIRE(sample_root != nullptr);
    for (const auto* relative : {"base/exefs/main.npdm", "updates/458752/exefs/main.npdm"}) {
        std::ifstream input(std::filesystem::path(sample_root) / relative, std::ios::binary);
        REQUIRE(input.good());
        const std::vector<u8> bytes{std::istreambuf_iterator<char>(input), {}};
        ProgramMetadata metadata;
        REQUIRE(LoadStatus(metadata, bytes) == u16(Loader::ResultStatus::Success));
        REQUIRE(metadata.GetPlatform() == ContentMetaPlatform::Ounce);
        REQUIRE(metadata.GetTitleID() == 0x040074A01BF12000);
        REQUIRE(metadata.GetRemoteMemorySize() == 0x3000000);
        REQUIRE(metadata.GetFilesystemPermissions() == 0x4000000000000000);
        REQUIRE(metadata.GetKernelCapabilities().size() == 9);
    }
}

} // namespace FileSys
