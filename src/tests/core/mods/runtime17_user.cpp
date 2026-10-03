// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 17: the "user:" asset source (mod_user_source.h).
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/mod_sources.h"
#include "core/mods/mod_user_source.h"

using namespace Core::Mods;

namespace {
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("dsmod_user_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void Put(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream{p, std::ios::binary} << text;
}
} // namespace

TEST_CASE("DSMod user source: path validation", "[dsmod][runtime17][user]") {
    REQUIRE(IsSafeUserSourcePath("eid/en_us.lua"));
    REQUIRE(IsSafeUserSourcePath("a.png"));
    REQUIRE(IsSafeUserSourcePath("dir/.hidden"));
    REQUIRE(IsSafeUserSourcePath("a..b/c"));
    REQUIRE_FALSE(IsSafeUserSourcePath(""));
    REQUIRE_FALSE(IsSafeUserSourcePath("/etc/passwd"));
    REQUIRE_FALSE(IsSafeUserSourcePath(".."));
    REQUIRE_FALSE(IsSafeUserSourcePath("../x"));
    REQUIRE_FALSE(IsSafeUserSourcePath("a/../../x"));
    REQUIRE_FALSE(IsSafeUserSourcePath("a/.."));
    REQUIRE_FALSE(IsSafeUserSourcePath("./a"));
    REQUIRE_FALSE(IsSafeUserSourcePath("a//b"));
    REQUIRE_FALSE(IsSafeUserSourcePath("a/"));
    REQUIRE_FALSE(IsSafeUserSourcePath("a\\..\\b"));
    REQUIRE_FALSE(IsSafeUserSourcePath("C:/x"));
    REQUIRE_FALSE(IsSafeUserSourcePath(std::string_view{"a\0b", 3}));
}

TEST_CASE("DSMod user source: folder layout", "[dsmod][runtime17][user]") {
    REQUIRE(UserSourceDir("root", 0x01001F5010DFA000ULL) ==
            std::filesystem::path{"root"} / "01001F5010DFA000");
    REQUIRE(UserSourceRoot().filename() == "user");
    REQUIRE(UserSourceRoot().parent_path().filename() == "dualscreen");
}

TEST_CASE("DSMod user source: reads, refusals and capability", "[dsmod][runtime17][user]") {
    TempDir tmp;
    const auto dir = tmp.path / "user" / "0100000000000001"; // created on first use
    AssetSources sources;
    sources.Register({.prefix = "romfs", .open_dir = [] { return FileSys::VirtualDir{}; }});
    sources.Register(MakeUserSource(dir));

    REQUIRE((sources.Capabilities() & EDEN_DSMOD_CAP_SOURCE_USER) != 0);
    REQUIRE(EDEN_DSMOD_CAP_SOURCE_USER == (UINT64_C(1) << 10));
    REQUIRE(sources.Known("user"));
    REQUIRE(sources.IsDirectorySource("user:x"));
    REQUIRE_FALSE(std::filesystem::exists(dir));
    REQUIRE(sources.Available("user")); // what get_i64("__source:user") answers: 1
    REQUIRE(std::filesystem::is_directory(dir));

    Put(dir / "eid" / "en.lua", "return {}");
    Put(tmp.path / "user" / "secret.txt", "secret");
    Put(tmp.path / "outside.txt", "outside");

    const auto all = sources.ReadAll("user:eid/en.lua");
    REQUIRE(std::string(all.begin(), all.end()) == "return {}");
    REQUIRE(sources.ReadAll("user:/eid/en.lua").size() == 9); // leading '/' = folder root
    char buf[4]{};
    REQUIRE(sources.ReadRange("user:eid/en.lua", 0, nullptr, 0) == 9);
    REQUIRE(sources.ReadRange("user:eid/en.lua", 7, buf, 4) == 2);
    REQUIRE(std::string(buf, 2) == "{}");

    REQUIRE(sources.ReadAll("user:missing.bin").empty());
    REQUIRE(sources.ReadAll("user:../secret.txt").empty());
    REQUIRE(sources.ReadAll("user:eid/../../secret.txt").empty());
    REQUIRE(sources.ReadAll("user:../../outside.txt").empty());
    REQUIRE(sources.ReadRange("user:../secret.txt", 0, nullptr, 0) == 0);
    REQUIRE(sources.Open("user:eid") == nullptr); // a folder is not a file

    // A link inside the folder that leads out of it is refused; one that stays inside is not.
    std::error_code ec;
    std::filesystem::create_symlink(tmp.path / "outside.txt", dir / "link_out.txt", ec);
    if (!ec) {
        REQUIRE(sources.ReadAll("user:link_out.txt").empty());
        std::filesystem::create_symlink(dir / "eid" / "en.lua", dir / "link_in.lua", ec);
        REQUIRE_FALSE(ec);
        REQUIRE(sources.ReadAll("user:link_in.lua").size() == 9);
    }
}

TEST_CASE("DSMod user source: size cap", "[dsmod][runtime17][user]") {
    TempDir tmp;
    AssetSources sources;
    auto source = MakeUserSource(tmp.path / "t");
    REQUIRE(source.max_file_size == UserSourceMaxFileSize);
    REQUIRE(UserSourceMaxFileSize == 32ull * 1024 * 1024);
    source.max_file_size = 8; // the cap is per source; shrink it to test without 32 MiB files
    sources.Register(std::move(source));
    REQUIRE(sources.Available("user"));
    Put(tmp.path / "t" / "small.bin", "12345678");
    Put(tmp.path / "t" / "big.bin", "123456789");
    REQUIRE(sources.ReadAll("user:small.bin").size() == 8);
    REQUIRE(sources.ReadAll("user:big.bin").empty());
    REQUIRE(sources.ReadRange("user:big.bin", 0, nullptr, 0) == 0);
}

TEST_CASE("DSMod user source: unavailable folder", "[dsmod][runtime17][user]") {
    TempDir tmp;
    Put(tmp.path / "file", "x"); // a file where the folder's parent should be
    AssetSources sources;
    sources.Register(MakeUserSource(tmp.path / "file" / "0100000000000001"));
    REQUIRE(sources.Known("user"));
    REQUIRE_FALSE(sources.Available("user")); // get_i64("__source:user") = 0
    REQUIRE(sources.ReadAll("user:a").empty());
}
