// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 23: the "sysfont:" asset source (mod_sysfont_source.h).
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/swap.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_sources.h"
#include "core/mods/mod_sysfont_source.h"

using namespace Core::Mods;

namespace {
constexpr u32 Magic = 0x36f81a1e;
constexpr u32 Result = 0x7f9a0218;

// A shared font as the firmware stores it: big-endian words, magic, size word, then the TTF
// words XOR-encrypted with the key the decryptor derives from the magic.
std::vector<u8> Store(const std::vector<u8>& ttf) {
    REQUIRE(ttf.size() % 4 == 0);
    const u32 key = Magic ^ Result;
    std::vector<u32> words{Magic, static_cast<u32>(ttf.size()) ^ key};
    for (std::size_t i = 0; i < ttf.size(); i += 4) {
        u32 t;
        std::memcpy(&t, ttf.data() + i, 4);
        words.push_back(Common::swap32(t) ^ key);
    }
    std::vector<u8> stored(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const u32 be = Common::swap32(words[i]);
        std::memcpy(stored.data() + i * 4, &be, 4);
    }
    return stored;
}

const std::vector<u8> FakeTtf{0x00, 0x01, 0x00, 0x00, 'g', 'l', 'y', 'f', 1, 2, 3, 4};

FileSys::VirtualDir ArchiveWith(const std::string& file, std::vector<u8> bytes) {
    auto dir = std::make_shared<FileSys::VectorVfsDirectory>();
    dir->AddFile(std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), file));
    return dir;
}
} // namespace

TEST_CASE("DSMod sysfont source: names", "[dsmod][runtime23][sysfont]") {
    REQUIRE(DualScreenRuntimeVersion >= 23);
    const auto ko = FindSysfont("nintendo_udsg-r_ko_003");
    REQUIRE(ko);
    REQUIRE(ko->archive_id == 0x0100000000000812);
    REQUIRE(ko->file == "nintendo_udsg-r_ko_003.bfttf");
    REQUIRE(FindSysfont("nintendo_udsg-r_ko_003.bfttf"));
    REQUIRE(FindSysfont("nintendo_udsg-r_org_zh-cn_003")->archive_id == 0x0100000000000814);
    REQUIRE(FindSysfont("nintendo_udjxh-db_zh-tw_003")->archive_id == 0x0100000000000813);
    REQUIRE(SysfontNames().size() == 7);
    REQUIRE_FALSE(FindSysfont(""));
    REQUIRE_FALSE(FindSysfont("../nintendo_udsg-r_ko_003"));
    REQUIRE_FALSE(FindSysfont("nintendo_udsg-r_ko"));
    REQUIRE_FALSE(FindSysfont("NINTENDO_UDSG-R_KO_003"));
}

TEST_CASE("DSMod sysfont source: decrypt", "[dsmod][runtime23][sysfont]") {
    REQUIRE(DecryptSysfont(Store(FakeTtf)) == FakeTtf);
    REQUIRE(DecryptSysfont({}).empty());
    const std::vector<u8> short_file{0x36, 0xf8, 0x1a, 0x1e, 0, 0, 0, 0};
    REQUIRE(DecryptSysfont(short_file).empty());
    auto wrong_magic = Store(FakeTtf);
    wrong_magic[0] ^= 0xff;
    REQUIRE(DecryptSysfont(wrong_magic).empty());
    auto odd = Store(FakeTtf);
    odd.push_back(0);
    REQUIRE(DecryptSysfont(odd).empty());
}

TEST_CASE("DSMod sysfont source: real archive only, cached", "[dsmod][runtime23][sysfont]") {
    int opens = 0;
    AssetSources sources;
    sources.Register(MakeSysfontSource([&](u64 id) -> FileSys::VirtualDir {
        ++opens;
        if (id == 0x0100000000000812) {
            return ArchiveWith("nintendo_udsg-r_ko_003.bfttf", Store(FakeTtf));
        }
        return nullptr; // no real archive: must read empty, never a synthesized font
    }));
    REQUIRE(sources.ReadAll("sysfont:nintendo_udsg-r_ko_003") == FakeTtf);
    REQUIRE(sources.ReadAll("sysfont:nintendo_udsg-r_ko_003.bfttf") == FakeTtf);
    REQUIRE(opens == 1);
    REQUIRE(sources.ReadAll("sysfont:nintendo_udjxh-db_zh-tw_003").empty());
    REQUIRE(sources.ReadAll("sysfont:nintendo_udjxh-db_zh-tw_003").empty());
    REQUIRE(opens == 2);
    REQUIRE(sources.ReadAll("sysfont:not_a_font").empty());
    REQUIRE(opens == 2);
    REQUIRE((sources.Capabilities() & EDEN_DSMOD_CAP_SOURCE_SYSFONT) != 0);
    // Ranged reads (what a module's read_romfs uses) slice the cached font, no re-open.
    REQUIRE(sources.ReadRange("sysfont:nintendo_udsg-r_ko_003", 0, nullptr, 0) == FakeTtf.size());
    std::vector<u8> part(5, 0xEE);
    REQUIRE(sources.ReadRange("sysfont:nintendo_udsg-r_ko_003", 4, part.data(), 5) == 5);
    REQUIRE(std::vector<u8>(FakeTtf.begin() + 4, FakeTtf.begin() + 9) == part);
    REQUIRE(sources.ReadRange("sysfont:nintendo_udsg-r_ko_003", 10, part.data(), 5) == 2);
    REQUIRE(sources.ReadRange("sysfont:nintendo_udsg-r_ko_003", 99, part.data(), 5) == 0);
    REQUIRE(sources.ReadRange("sysfont:nintendo_udjxh-db_zh-tw_003", 0, nullptr, 0) == 0);
    REQUIRE(opens == 2);
}

TEST_CASE("DSMod sysfont source: readable from a module's read_romfs", "[dsmod][runtime23][sysfont]") {
    // The module host's read_romfs admits directory sources and byte sources that opt in; the
    // first rt23 build refused "sysfont:" there although it advertised the capability.
    AssetSources sources;
    sources.Register(MakeSysfontSource([](u64) -> FileSys::VirtualDir { return nullptr; }));
    sources.Register({.prefix = "module",
                      .read_bytes = [](const std::string&) { return std::vector<u8>{1}; }});
    REQUIRE(sources.IsModuleReadableByteSource("sysfont:nintendo_udsg-r_ko_003"));
    REQUIRE_FALSE(sources.IsDirectorySource("sysfont:nintendo_udsg-r_ko_003"));
    REQUIRE_FALSE(sources.IsModuleReadableByteSource("module:x:y"));
    REQUIRE_FALSE(sources.IsModuleReadableByteSource("nosuch:x"));
}

TEST_CASE("DSMod sysfont source: archive without the file", "[dsmod][runtime23][sysfont]") {
    AssetSources sources;
    sources.Register(MakeSysfontSource(
        [](u64) { return ArchiveWith("other.bfttf", Store(FakeTtf)); }));
    REQUIRE(sources.ReadAll("sysfont:nintendo_udsg-r_ko_003").empty());
}
