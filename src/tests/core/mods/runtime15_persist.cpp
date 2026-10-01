// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Unreleased runtime 15 addition: manifest "persist_flags" (runtime flags saved on change and
// restored at load, mod_persist.h).
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_persist.h"
#include "core/mods/mod_runtime.h"

using namespace Core::Mods;

namespace {
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("dsmod_persist_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string Slurp(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void Put(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream{p, std::ios::binary} << text;
}
} // namespace

TEST_CASE("DSMod persist_flags: manifest parse", "[dsmod][runtime15][persist]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"min_runtime":15,"name":"Pkg",
        "pages":[{"id":"p","widgets":[]}],
        "flags":{"lp":0,"sc":true},
        "persist_flags":["lp","sc","lp",7,""]})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.persist_flags == std::vector<std::string>{"lp", "sc"}); // dedup, non-names dropped
    REQUIRE(m.flag_defaults.at("sc") == 1);

    Manifest none;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(
        R"({"format":1,"pages":[{"id":"p","widgets":[]}],"persist_flags":"lp"})"), none));
    REQUIRE(none.persist_flags.empty()); // not an array: ignored with a warning
}

TEST_CASE("DSMod persist_flags: file path", "[dsmod][runtime15][persist]") {
    const std::filesystem::path root{"root"};
    REQUIRE(PersistFlagsPath(root, 0x010015100B514000ULL, "Super Mario Wonder DS") ==
            root / "010015100B514000" / "Super_Mario_Wonder_DS.json");
    REQUIRE(PersistFlagsPath(root, 1, "../x") == root / "0000000000000001" / "_.._x.json");
    REQUIRE(PersistFlagsPath(root, 1, "") == root / "0000000000000001" / "_.json");
    REQUIRE(PersistFlagsRoot().filename() == "persist");
}

TEST_CASE("DSMod persist_flags: save / restore round trip", "[dsmod][runtime15][persist]") {
    TempDir dir;
    const auto file = PersistFlagsPath(dir.path, 0x0100ABCD00000000ULL, "pkg");

    // session 1: defaults, nothing stored -> no write until a listed flag changes
    std::unordered_map<std::string, s64> flags{{"lp", 0}, {"sc", 0}, {"other", 5}};
    FlagPersistence p1{file, {"lp", "sc", "never_set"}};
    REQUIRE(p1.Enabled());
    REQUIRE(p1.Restore(flags) == 0);
    REQUIRE(flags.at("lp") == 0);
    REQUIRE_FALSE(p1.SaveIfChanged(flags));
    REQUIRE_FALSE(std::filesystem::exists(file));
    flags["other"] = 9; // not listed: no write
    REQUIRE_FALSE(p1.SaveIfChanged(flags));
    flags["lp"] = 1;
    REQUIRE(p1.SaveIfChanged(flags));
    REQUIRE(std::filesystem::exists(file));
    REQUIRE_FALSE(std::filesystem::exists(file.string() + ".tmp"));
    REQUIRE_FALSE(p1.SaveIfChanged(flags)); // unchanged
    flags["sc"] = 3;
    REQUIRE(p1.SaveIfChanged(flags));
    const auto stored = nlohmann::json::parse(Slurp(file));
    REQUIRE(stored.at("version") == PersistFlagsFileVersion);
    REQUIRE(stored.at("flags") == nlohmann::json{{"lp", 1}, {"sc", 3}}); // only listed flags

    // session 2: manifest defaults again, the stored values win
    std::unordered_map<std::string, s64> again{{"lp", 0}, {"sc", 0}, {"other", 5}};
    FlagPersistence p2{file, {"lp", "sc"}};
    REQUIRE(p2.Restore(again) == 2);
    REQUIRE(again.at("lp") == 1);
    REQUIRE(again.at("sc") == 3);
    REQUIRE(again.at("other") == 5);
    REQUIRE_FALSE(p2.SaveIfChanged(again)); // restored state is what is on disk

    // a flag dropped from persist_flags is no longer restored
    std::unordered_map<std::string, s64> fewer{{"lp", 0}, {"sc", 0}};
    FlagPersistence p3{file, {"sc"}};
    REQUIRE(p3.Restore(fewer) == 1);
    REQUIRE(fewer.at("lp") == 0);
    REQUIRE(fewer.at("sc") == 3);
}

TEST_CASE("DSMod persist_flags: bad files are ignored", "[dsmod][runtime15][persist]") {
    TempDir dir;
    const auto file = dir.path / "t" / "pkg.json";
    std::unordered_map<std::string, s64> flags{{"lp", 0}};

    Put(file, "{not json");
    FlagPersistence a{file, {"lp"}};
    REQUIRE(a.Restore(flags) == 0);
    REQUIRE(flags.at("lp") == 0);
    flags["lp"] = 1; // the next change rewrites the broken file
    REQUIRE(a.SaveIfChanged(flags));
    REQUIRE(nlohmann::json::parse(Slurp(file)).at("flags").at("lp") == 1);

    flags["lp"] = 0;
    Put(file, R"({"version":2,"flags":{"lp":4}})"); // unknown version
    FlagPersistence b{file, {"lp"}};
    REQUIRE(b.Restore(flags) == 0);
    REQUIRE(flags.at("lp") == 0);

    Put(file, R"({"flags":{"lp":4}})"); // no version
    FlagPersistence c{file, {"lp"}};
    REQUIRE(c.Restore(flags) == 0);

    Put(file, R"({"version":1,"flags":{"lp":true,"x":"s"}})"); // bool value, unlisted junk
    FlagPersistence d{file, {"lp", "x"}};
    REQUIRE(d.Restore(flags) == 1);
    REQUIRE(flags.at("lp") == 1);
    REQUIRE(flags.count("x") == 0);

    FlagPersistence off{file, {}};
    REQUIRE_FALSE(off.Enabled());
    REQUIRE_FALSE(off.SaveIfChanged(flags));
}
