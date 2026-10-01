// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/vfs/vfs_vector.h"
#include "core/mods/mod_sources.h"

namespace Core::Mods {
namespace {
FileSys::VirtualDir MakeDir(const std::string& name, const std::string& content) {
    std::vector<u8> bytes(content.begin(), content.end());
    auto file = std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), "a.bin");
    auto sub = std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{file}, std::vector<FileSys::VirtualDir>{}, "dir");
    return std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{sub}, name);
}
} // namespace

TEST_CASE("Asset source prefixes", "[dsmod][sources]") {
    REQUIRE(AssetSources::PrefixOf("romfs:/a/b") == "romfs");
    REQUIRE(AssetSources::PrefixOf("aoc:x") == "aoc");
    REQUIRE(AssetSources::PrefixOf("packs/a.pkg") == "");
    REQUIRE(AssetSources::PrefixOf("packs/a:b") == ""); // a bare path holding a colon
    REQUIRE(AssetSources::PrefixOf("C:x") == "");
    REQUIRE(AssetSources::PathOf("romfs://a/b") == "a/b");
}

TEST_CASE("Asset sources open once and fail cleanly", "[dsmod][sources]") {
    AssetSources sources;
    std::atomic<int> opens{0};
    sources.Register({.prefix = "romfs", .open_dir = [&] {
                          ++opens;
                          return MakeDir("root", "hello");
                      }});
    sources.Register({.prefix = "aoc", .open_dir = [] { return FileSys::VirtualDir{}; },
                      .capability = u64{1} << 9});
    sources.Register({.prefix = "module", .read_bytes = [](const std::string& src) {
                          return std::vector<u8>(src.begin(), src.end());
                      }});

    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] { REQUIRE(sources.ReadAll("romfs:/dir/a.bin").size() == 5); });
    }
    for (auto& t : readers) {
        t.join();
    }
    REQUIRE(opens == 1);

    char buf[8]{};
    REQUIRE(sources.ReadRange("romfs:dir/a.bin", 0, nullptr, 0) == 5);
    REQUIRE(sources.ReadRange("romfs:dir/a.bin", 1, buf, 3) == 3);
    REQUIRE(std::string(buf, 3) == "ell");
    REQUIRE(sources.ReadRange("romfs:dir/a.bin", 9, buf, 3) == 0);

    REQUIRE(sources.Known("aoc:x"));
    REQUIRE(sources.Known("aoc"));
    REQUIRE_FALSE(sources.Available("aoc"));
    REQUIRE(sources.ReadAll("aoc:dir/a.bin").empty());
    REQUIRE_FALSE(sources.Known("base:x"));
    REQUIRE(sources.ReadAll("base:dir/a.bin").empty());
    REQUIRE(sources.Open("romfs:missing") == nullptr);
    REQUIRE(sources.IsDirectorySource("romfs:x"));
    REQUIRE_FALSE(sources.IsDirectorySource("module:x"));
    REQUIRE(sources.ReadAll("module:k").size() == 8);
    REQUIRE(sources.Available("module"));
    REQUIRE(sources.Capabilities() == (u64{1} << 9));
}

} // namespace Core::Mods
