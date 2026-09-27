// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/mods/map_overview.h"

namespace Core::Mods {
namespace {

MapArea BaseArea() {
    MapArea area;
    area.min_x = 0;
    area.min_y = 0;
    area.max_x = 100;
    area.max_y = 100;
    area.overview_regions.emplace();
    return area;
}

MapArea::OverviewRegion Rect(std::string kind, float x0, float y0, float x1, float y1) {
    return {std::move(kind), {{{x0, y0, x1, y0, x1, y1}, {x0, y0, x1, y1, x0, y1}}}};
}

} // namespace

TEST_CASE("Map overview uses authored coarse geometry", "[dsmod][map-overview]") {
    auto area = BaseArea();
    area.overview_regions->push_back(Rect("room", 10, 10, 40, 40));
    // An irregular authored section proves the helper does not infer camera/grid rectangles.
    area.overview_regions->push_back({"station", {{{60, 10, 90, 10, 75, 45}}}});
    MapStyle style;
    style.room_fill = 0xFF6496C8;
    const std::unordered_map<std::string, u32> class_colors{{"station", 0xFFFFA000}};

    const Image image = RasterizeMapOverview(area, style, 101, 101, class_colors);
    REQUIRE(image.Valid());
    CHECK((image.pixels[75 * 101 + 5] >> 24) == 0); // outside authored coverage
    CHECK((image.pixels[75 * 101 + 5] & 0x00FFFFFFu) == 0);
    CHECK((image.pixels[75 * 101 + 75] >> 24) == 0x99); // inside triangle
    CHECK((image.pixels[50 * 101 + 75] >> 24) == 0);    // outside its sloping edge
}

TEST_CASE("Map overview preserves same-class section boundaries", "[dsmod][map-overview]") {
    auto area = BaseArea();
    area.overview_regions->push_back(Rect("room", 10, 20, 50, 80));
    area.overview_regions->push_back(Rect("room", 50, 20, 90, 80));
    MapStyle style;
    style.room_fill = 0xFF6496C8;
    const Image image = RasterizeMapOverview(area, style, 101, 101, {});

    const u32 interior = image.pixels[50 * 101 + 30];
    const u32 boundary = image.pixels[50 * 101 + 50];
    CHECK((interior >> 24) == 0x99);
    CHECK((interior & 0x00FFFFFFu) == 0x001E2D3Cu); // 0.3 * room class
    CHECK((boundary & 0x00FFFFFFu) == 0x006496C8u); // full-color identity boundary
}

TEST_CASE("Map overview composes below detailed pixels", "[dsmod][map-overview]") {
    Image overview{4, 1, {0x99112233, 0x99445566, 0x99FFFFFF, 0x00000000}};
    Image detail{4, 1, {0xFFABCDEF, 0x00000000, 0x01FFFFFF, 0x7F123456}};
    CompositeMapOverview(detail, overview);
    CHECK(detail.pixels[0] == 0xFFABCDEF); // explored opaque detail remains exact
    CHECK(detail.pixels[1] == 0x99445566); // unexplored hole receives only coarse overview
    CHECK(detail.pixels[2] == 0x99FFFFFF); // low-alpha white cannot overflow a channel
    CHECK(detail.pixels[3] == 0x7F123456); // transparent overview preserves detail bit-exactly
}

} // namespace Core::Mods
