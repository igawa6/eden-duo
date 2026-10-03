// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 17: manifest "settings" and the built-in "@settings" page (mod_settings.h).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_persist.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_settings.h"

using namespace Core::Mods;

namespace {
constexpr const char* Package = R"({"format":1,"min_runtime":17,"name":"Pkg",
    "canvas_w":1240,"canvas_h":1080,
    "pages":[{"id":"main","widgets":[]},{"id":"map","widgets":[]}],
    "flags":{"hud.size":2},
    "persist_flags":["other"],
    "actions":{"open":{"kind":"page","page":"@settings"}},
    "settings":[
        {"flag":"map.dark","label":"Dark map","type":"toggle","default":true},
        {"flag":"hud.size","label":"HUD size","type":"choice","choices":["S","M","L"],"default":0},
        {"flag":"spoilers","label":"Spoilers","choices":["Hide","Show"]},
        {"flag":"bad","type":"choice","choices":["only one"]},
        {"flag":"weird","type":"slider"},
        {"label":"no flag"},
        {"flag":"map.dark","label":"twice"},
        {"flag":"big","type":"choice","choices":["a","b"],"default":9}
    ]})";

const Widget* FindWidget(const Page& page, std::string_view id) {
    const auto it = std::ranges::find_if(page.widgets, [&](const Widget& w) { return w.id == id; });
    return it == page.widgets.end() ? nullptr : &*it;
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("dsmod_settings_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

TEST_CASE("DSMod settings: parsing", "[dsmod][runtime17][settings]") {
    const auto json = nlohmann::json::parse(Package);
    const auto defs = ParseSettings(json.at("settings"));
    REQUIRE(defs.size() == 4);
    REQUIRE(defs[0].flag == "map.dark");
    REQUIRE(defs[0].type == SettingDef::Type::Toggle);
    REQUIRE(defs[0].choices == std::vector<std::string>{"OFF", "ON"});
    REQUIRE(defs[0].default_value == 1);
    REQUIRE(defs[1].type == SettingDef::Type::Choice);
    REQUIRE(defs[1].choices == std::vector<std::string>{"S", "M", "L"});
    REQUIRE(defs[2].type == SettingDef::Type::Toggle); // toggle is the default type
    REQUIRE(defs[2].choices == std::vector<std::string>{"Hide", "Show"}); // two choices rename
    REQUIRE(defs[3].flag == "big");
    REQUIRE(defs[3].default_value == 1); // clamped into the choices
    REQUIRE(defs[3].label == "big");     // label defaults to the flag
    REQUIRE(ParseSettings(nlohmann::json::parse(R"({"flag":"x"})")).empty()); // not an array
}

TEST_CASE("DSMod settings: manifest page, actions and flags", "[dsmod][runtime17][settings]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(Package), m));
    REQUIRE(m.pages.size() == 3);
    const Page& page = m.pages.back();
    REQUIRE(page.id == SettingsPageId);
    REQUIRE(page.no_auto_leave);

    // flags: "flags" wins over the setting default; persisted implicitly, after "persist_flags"
    REQUIRE(m.flag_defaults.at("map.dark") == 1);
    REQUIRE(m.flag_defaults.at("hud.size") == 2);
    REQUIRE(m.flag_defaults.at("spoilers") == 0);
    REQUIRE(m.persist_flags ==
            std::vector<std::string>{"other", "map.dark", "hud.size", "spoilers", "big"});

    // one big row + value per entry, all inside the canvas, tappable, in order
    const Widget* back = FindWidget(page, "@settings.back");
    REQUIRE(back != nullptr);
    REQUIRE(back->type == WidgetType::Button);
    REQUIRE(m.actions.at(back->on_tap).kind == ActionKind::Page);
    REQUIRE(m.actions.at(back->on_tap).page == SettingsBackPageId);
    s32 last_y = 0;
    for (const char* flag : {"map.dark", "hud.size", "spoilers", "big"}) {
        const Widget* row = FindWidget(page, std::string{"@settings.row."} + flag);
        const Widget* value = FindWidget(page, std::string{"@settings.value."} + flag);
        REQUIRE(row != nullptr);
        REQUIRE(value != nullptr);
        REQUIRE(row->type == WidgetType::Button);
        REQUIRE(row->rect[1] > last_y);
        last_y = row->rect[1];
        REQUIRE(row->rect[3] >= 96); // touch friendly
        REQUIRE(row->rect[0] >= 0);
        REQUIRE(row->rect[0] + row->rect[2] <= 1240);
        REQUIRE(row->rect[1] + row->rect[3] <= 1080);
        REQUIRE(value->type == WidgetType::Value);
        REQUIRE(value->bind == std::string{"@flag:"} + flag);
        const Action& act = m.actions.at(row->on_tap);
        REQUIRE(act.kind == ActionKind::Flag);
        REQUIRE(act.flag == flag);
        REQUIRE(act.cycle == static_cast<s64>(value->names.size()));
    }
    REQUIRE(FindWidget(page, "@settings.value.hud.size")->names ==
            std::vector<std::string>{"S", "M", "L"});
    REQUIRE(FindWidget(page, "@settings.row.hud.size")->text == "HUD size");

    // many entries still fit the canvas
    nlohmann::json many = nlohmann::json::parse(Package);
    many["settings"] = nlohmann::json::array();
    for (int i = 0; i < 20; ++i) {
        many["settings"].push_back({{"flag", "f" + std::to_string(i)}});
    }
    Manifest crowded;
    REQUIRE(ParseDualScreenManifest(many, crowded));
    for (const Widget& w : crowded.pages.back().widgets) {
        REQUIRE(w.rect[1] + w.rect[3] <= 1080);
    }
}

TEST_CASE("DSMod settings: nothing generated without settings or pages",
          "[dsmod][runtime17][settings]") {
    Manifest plain;
    REQUIRE(ParseDualScreenManifest(
        nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[]}]})"), plain));
    REQUIRE(plain.pages.size() == 1);
    REQUIRE(plain.persist_flags.empty());

    Manifest no_pages;
    REQUIRE(ParseDualScreenManifest(
        nlohmann::json::parse(R"({"format":1,"settings":[{"flag":"x"}]})"), no_pages));
    REQUIRE(no_pages.pages.empty());

    Manifest own;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(R"({"format":1,
        "pages":[{"id":"p","widgets":[]},{"id":"@settings","widgets":[]}],
        "settings":[{"flag":"x"}]})"), own));
    REQUIRE(own.pages.size() == 2); // the package's own "@settings" page is kept
}

TEST_CASE("DSMod settings: open and back", "[dsmod][runtime17][settings]") {
    Manifest m;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(Package), m));
    size_t ret = 0;
    REQUIRE(ResolvePageTarget(m.pages, "map", 0, ret) == 1u);
    REQUIRE(ret == 0);
    REQUIRE(ResolvePageTarget(m.pages, "@settings", 1, ret) == 2u);
    REQUIRE(ret == 1);
    REQUIRE(ResolvePageTarget(m.pages, "@settings", 2, ret) == 2u); // re-opening keeps "map"
    REQUIRE(ret == 1);
    REQUIRE(ResolvePageTarget(m.pages, "@back", 2, ret) == 1u);
    REQUIRE(ResolvePageTarget(m.pages, "nope", 0, ret) == std::nullopt);
    size_t stale = 99;
    REQUIRE(ResolvePageTarget(m.pages, "@back", 2, stale) == 0u);
    REQUIRE(ResolvePageTarget({}, "@back", 0, stale) == std::nullopt);
}

TEST_CASE("DSMod settings: values persist across sessions", "[dsmod][runtime17][settings]") {
    TempDir dir;
    Manifest m;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(Package), m));
    const auto file = PersistFlagsPath(dir.path, 0x0100ABCD00000000ULL, m.name);

    // session 1: tap "HUD size" twice (2 -> 0 -> 1) and "Dark map" once (1 -> 0)
    auto flags = m.flag_defaults;
    FlagPersistence p1{file, m.persist_flags};
    REQUIRE(p1.Restore(flags) == 0);
    const auto tap = [&](const std::string& flag) {
        const Action& a = m.actions.at(std::string{SettingsActionPrefix} + flag);
        s64& v = flags[a.flag];
        v = v < 0 ? 0 : (v + 1) % a.cycle; // RunAction's ActionKind::Flag with a cycle
        p1.SaveIfChanged(flags);
    };
    tap("hud.size");
    tap("hud.size");
    tap("map.dark");
    REQUIRE(flags.at("hud.size") == 1);
    REQUIRE(flags.at("map.dark") == 0);

    // session 2: the defaults are overridden by what was saved
    auto flags2 = m.flag_defaults;
    FlagPersistence p2{file, m.persist_flags};
    REQUIRE(p2.Restore(flags2) >= 2);
    REQUIRE(flags2.at("hud.size") == 1);
    REQUIRE(flags2.at("map.dark") == 0);
    REQUIRE(flags2.at("spoilers") == 0);
}
