// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 12: map.areas_src. The per-area parser is shared by the inline manifest path and the
// module-provided areas; both must give the same MapArea contents for the same JSON.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "core/mods/mod_runtime.h"

using namespace Core::Mods;

namespace {

std::string Bits(float v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    return fmt::format("{:08x}", b);
}

/// Every parsed field the map renderer and module use, in a canonical order.
std::string Dump(const MapArea& a) {
    std::string s = fmt::format("geo={} image={} nopin={} pad={} clamp={} box={},{},{},{}\n", a.geo,
                                a.image, a.no_pin, Bits(a.follow_pad), a.clamp_view, Bits(a.min_x),
                                Bits(a.min_y), Bits(a.max_x), Bits(a.max_y));
    for (const auto& m : a.markers) {
        s += fmt::format("M {} {} {} {} {} {} {} op={} sz={} g={} b{}={},{},{},{} p{}={},{},{},{} "
                         "h{}={},{},{},{} {} {} s{} c{}\n",
                         m.kind, m.icon, Bits(m.x), Bits(m.y), m.name, m.vignette, m.hidden,
                         Bits(m.opacity), m.size, m.group, m.has_box, Bits(m.bx0), Bits(m.by0),
                         Bits(m.bx1), Bits(m.by1), m.has_pulse_box, Bits(m.px0), Bits(m.py0),
                         Bits(m.px1), Bits(m.py1), m.has_hint_box, Bits(m.hx0), Bits(m.hy0),
                         Bits(m.hx1), Bits(m.hy1), m.open_icon, m.collected_icon,
                         m.structural ? int(*m.structural) : -1,
                         m.collectible ? int(*m.collectible) : -1);
    }
    for (const auto& l : a.layers) {
        s += fmt::format("L {} {} {:08x} {} {} {} {} {}\n", l.geo, l.kind, l.color, l.color_bind,
                         l.category, l.live_clip, l.post_fog, l.color_map ? l.color_map->size() : 0);
    }
    for (const auto& c : a.room_categories) {
        s += fmt::format("C {} {:08x} {} {} {} {} {} {} {}\n", c.id, c.color, c.color_bind,
                         Bits(c.class_gain), c.reveal_before_unlock, c.bake.mode,
                         Bits(c.bake.gain), c.visited_amount, c.polys.size());
        for (const auto& p : c.polys) {
            for (float v : p) {
                s += Bits(v);
            }
            s += '\n';
        }
    }
    const auto tris = [&s](const char* tag, const auto& list) {
        for (const auto& o : list) {
            s += fmt::format("{} {} ", tag, o.name);
            for (const auto& t : o.tris) {
                for (float v : t) {
                    s += Bits(v);
                }
            }
            s += '\n';
        }
    };
    tris("O", a.occluders);
    tris("V", a.vignettes);
    for (const auto& r : a.camera_rects) {
        s += fmt::format("R {}{}{}{}\n", Bits(r[0]), Bits(r[1]), Bits(r[2]), Bits(r[3]));
    }
    if (a.overview_regions) {
        for (const auto& r : *a.overview_regions) {
            s += "W " + r.kind + " ";
            for (const auto& t : r.tris) {
                for (float v : t) {
                    s += Bits(v);
                }
            }
            s += '\n';
        }
    }
    return s;
}

std::string DumpAll(const std::unordered_map<std::string, MapArea>& areas) {
    std::map<std::string, std::string> sorted;
    for (const auto& [name, area] : areas) {
        sorted[name] = Dump(area);
    }
    std::string s;
    for (const auto& [name, text] : sorted) {
        s += "== " + name + "\n" + text;
    }
    return s;
}

/// Path of a Metroid Dread dual-screen manifest.json with inline map areas (optional).
const char* DreadManifest() {
    const char* path = std::getenv("DSMOD_DREAD_MANIFEST");
    return path != nullptr ? path : "";
}

} // namespace

TEST_CASE("DSMod map.areas_src: shared area parser == inline manifest path", "[dsmod][areas-src]") {
    std::ifstream in{DreadManifest()};
    if (!in) {
        SKIP("Dread manifest not present (set DSMOD_DREAD_MANIFEST)");
    }
    const auto json = nlohmann::json::parse(in);
    Manifest inline_manifest;
    REQUIRE(ParseDualScreenManifest(json, inline_manifest));
    REQUIRE(inline_manifest.map_areas.size() == 9);
    REQUIRE(inline_manifest.map_areas_src.empty());

    std::unordered_map<std::string, MapArea> shared;
    REQUIRE(ParseMapAreasJson(json.at("map").at("areas"), shared));
    const std::string a = DumpAll(inline_manifest.map_areas);
    const std::string b = DumpAll(shared);
    REQUIRE(a.size() > 300000);
    REQUIRE(a == b);

    // A round trip through text (what a module hands over) changes nothing either.
    std::unordered_map<std::string, MapArea> reparsed;
    REQUIRE(ParseMapAreasJson(nlohmann::json::parse(json.at("map").at("areas").dump()), reparsed));
    REQUIRE(DumpAll(reparsed) == a);
}

TEST_CASE("DSMod map.areas_src: key parsed, template kept, bad input refused",
          "[dsmod][areas-src]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"min_runtime":12,
        "pages":[{"id":"p","widgets":[]}],
        "map":{"areas_src":"module:x:areas","areas":{"a":{"geo":"module:x:map/a.geo"}}}})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.map_areas_src == "module:x:areas");
    REQUIRE(m.map_areas.size() == 1);
    REQUIRE(m.map_areas.at("a").geo == "module:x:map/a.geo");

    // areas_src without inline areas still parses (empty template).
    Manifest bare;
    REQUIRE(ParseDualScreenManifest(
        nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[]}],
            "map":{"areas_src":"module:x:areas"}})"),
        bare));
    REQUIRE(bare.map_areas_src == "module:x:areas");
    REQUIRE(bare.map_areas.empty());

    std::unordered_map<std::string, MapArea> out;
    REQUIRE_FALSE(ParseMapAreasJson(nlohmann::json::parse("[1,2]"), out));
    REQUIRE_FALSE(ParseMapAreasJson(nlohmann::json::parse(R"({"a":{"geo":5}})"), out));
    REQUIRE(DualScreenRuntimeVersion >= 12);
}
