// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "video_core/dsmod/aux_routing.h"

namespace {
using VideoCore::DSMod::AuxRouting;
using VideoCore::DSMod::AuxTouchPoint;
} // namespace

TEST_CASE("Aux touch preserves a start across moves and a short release", "[dsmod]") {
    AuxRouting aux;
    std::array<AuxTouchPoint, 1> points{{{7, 10, 20, 1, 0}}};
    aux.SetTouch(points);
    points[0] = {7, 12, 23, 0, 0};
    aux.SetTouch(points);
    aux.SetTouch({});
    std::array<AuxTouchPoint, AuxRouting::MaxTouch> out{};
    REQUIRE(aux.GetTouch(out) == 1);
    REQUIRE(out[0].finger_id == 7);
    REQUIRE(out[0].x == 12);
    REQUIRE(out[0].attributes == 1);
    REQUIRE(aux.GetTouch(out) == 1);
    REQUIRE(out[0].attributes == 2);
    REQUIRE(aux.GetTouch(out) == 0);
}

TEST_CASE("Aux touch releases one pointer without releasing another", "[dsmod]") {
    AuxRouting aux;
    std::array<AuxTouchPoint, 2> points{{{1, 10, 20, 1, 0}, {2, 30, 40, 1, 0}}};
    aux.SetTouch(points);
    std::array<AuxTouchPoint, AuxRouting::MaxTouch> out{};
    REQUIRE(aux.GetTouch(out) == 2);
    points[1].attributes = 0;
    aux.SetTouch(std::span{points}.subspan(1));
    REQUIRE(aux.GetTouch(out) == 2);
    REQUIRE(out[0].finger_id == 2);
    REQUIRE(out[0].attributes == 0);
    REQUIRE(out[1].finger_id == 1);
    REQUIRE(out[1].attributes == 2);
    REQUIRE(aux.GetTouch(out) == 1);
    REQUIRE(out[0].finger_id == 2);
}

TEST_CASE("Aux touch consumes only returned edges", "[dsmod]") {
    AuxRouting aux;
    std::array<AuxTouchPoint, 2> points{{{1, 10, 20, 1, 0}, {2, 30, 40, 1, 0}}};
    aux.SetTouch(points);
    REQUIRE(aux.GetTouch({}) == 0);
    std::array<AuxTouchPoint, 1> small{};
    REQUIRE(aux.GetTouch(small) == 1);
    REQUIRE(small[0].attributes == 1);
    std::array<AuxTouchPoint, 2> out{};
    REQUIRE(aux.GetTouch(out) == 2);
    REQUIRE(out[0].attributes == 0);
    REQUIRE(out[1].attributes == 1);
}

TEST_CASE("Aux touch explicit short end is delivered after its start", "[dsmod]") {
    AuxRouting aux;
    std::array<AuxTouchPoint, 1> points{{{3, 10, 20, 3, 0}}};
    aux.SetTouch(points);
    REQUIRE(aux.GetTouch(points) == 1);
    REQUIRE(points[0].attributes == 1);
    REQUIRE(aux.GetTouch(points) == 1);
    REQUIRE(points[0].attributes == 2);
    REQUIRE(aux.GetTouch(points) == 0);
}

TEST_CASE("Aux touch retains a short removed pointer beside a live pointer", "[dsmod]") {
    AuxRouting aux;
    std::array<AuxTouchPoint, 2> points{{{1, 10, 20, 1, 0}, {2, 30, 40, 1, 0}}};
    aux.SetTouch(points);
    points[1].attributes = 0;
    aux.SetTouch(std::span{points}.subspan(1));
    std::array<AuxTouchPoint, 2> out{};
    REQUIRE(aux.GetTouch(out) == 2);
    REQUIRE(out[0].finger_id == 2);
    REQUIRE(out[0].attributes == 1);
    REQUIRE(out[1].finger_id == 1);
    REQUIRE(out[1].attributes == 1);
    // Another frontend move must preserve the deferred release.
    aux.SetTouch(std::span{points}.subspan(1));
    REQUIRE(aux.GetTouch(out) == 2);
    REQUIRE(out[0].attributes == 0);
    REQUIRE(out[1].attributes == 2);
    REQUIRE(aux.GetTouch(out) == 1);
}

TEST_CASE("Aux touch snapshots stay bounded", "[dsmod]") {
    AuxRouting aux;
    std::array<AuxTouchPoint, AuxRouting::MaxTouch + 1> points{};
    for (size_t i = 0; i < points.size(); ++i) {
        points[i] = {static_cast<u32>(i), 0, 0, 1, 0};
    }
    aux.SetTouch(points);
    REQUIRE(aux.GetTouch(points) == AuxRouting::MaxTouch);
    aux.SetTouch({});
    REQUIRE(aux.GetTouch(points) == AuxRouting::MaxTouch);
    REQUIRE(aux.GetTouch(points) == 0);
}

TEST_CASE("Aux map fade bundle publishes matching endpoints and weights", "[dsmod]") {
    AuxRouting aux;
    const std::array<u32, 2> current{0xFF102030, 0xFF405060};
    const std::array<u32, 2> previous{0x00000000, 0xFF203040};
    const std::array<u32, 1> weights{0xFF800000};
    aux.PublishMapFadeTextures(2, 1, current, previous, 1, 1, weights);

    const auto take = [&](u32 slot, std::span<const u32> expected, u32 expected_w, u32 expected_h) {
        u64 serial = 0;
        bool visited = false;
        REQUIRE(aux.WithAuxTexture(slot, serial, [&](std::span<const u32> pixels, u32 w, u32 h) {
            visited = true;
            REQUIRE(w == expected_w);
            REQUIRE(h == expected_h);
            REQUIRE(std::ranges::equal(pixels, expected));
        }));
        REQUIRE(visited);
        REQUIRE(serial != 0);
    };
    take(AuxRouting::MapCurrentSlot, current, 2, 1);
    take(AuxRouting::MapPreviousSlot, previous, 2, 1);
    take(AuxRouting::MapFadeSlot, weights, 1, 1);

    u64 current_serial = 0, previous_serial = 0, fade_serial = 0;
    std::array<bool, AuxRouting::NumAuxTex> visited{};
    REQUIRE(aux.WithMapFadeTextures(
        current_serial, previous_serial, fade_serial,
        [&](u32 slot, std::span<const u32>, u32, u32, const VideoCore::DSMod::TileMask&) {
            visited[slot] = true;
        }));
    REQUIRE(visited[AuxRouting::MapCurrentSlot]);
    REQUIRE(visited[AuxRouting::MapPreviousSlot]);
    REQUIRE(visited[AuxRouting::MapFadeSlot]);

    visited.fill(false);
    const std::array<u32, 1> next_weights{0xFFFF0000};
    aux.PublishAuxTexture(AuxRouting::MapFadeSlot, 1, 1, next_weights);
    REQUIRE(aux.WithMapFadeTextures(
        current_serial, previous_serial, fade_serial,
        [&](u32 slot, std::span<const u32>, u32, u32, const VideoCore::DSMod::TileMask&) {
            visited[slot] = true;
        }));
    REQUIRE_FALSE(visited[AuxRouting::MapCurrentSlot]);
    REQUIRE_FALSE(visited[AuxRouting::MapPreviousSlot]);
    REQUIRE(visited[AuxRouting::MapFadeSlot]);
}

// ---------------------------------------------------------------------------------------------
// Dirty-tile diffing: producer-side diffing + dirty-tile hand-off.
namespace {
using VideoCore::DSMod::TileMask;

// What the renderer does with a take: copy the marked tiles (or everything) into its own image.
// Like the renderer, copy only inside the changed bounding box as well (when one is given).
void ApplyTake(std::vector<u32>& gpu, std::span<const u32> px, u32 w, u32 h, const TileMask& m,
               std::array<s32, 4> bbox = {0, 0, 0, 0}) {
    gpu.resize(px.size());
    if (m.all) {
        std::ranges::copy(px, gpu.begin());
        return;
    }
    for (u32 r = 0; r < m.rows; ++r) {
        for (u32 c = 0; c < m.cols; ++c) {
            if (m.bits[static_cast<size_t>(r) * m.cols + c] == 0) {
                continue;
            }
            for (u32 y = r * TileMask::Size; y < std::min(h, (r + 1) * TileMask::Size); ++y) {
                for (u32 x = c * TileMask::Size; x < std::min(w, (c + 1) * TileMask::Size); ++x) {
                    if (bbox[2] > 0 && (static_cast<s32>(x) < bbox[0] ||
                                        static_cast<s32>(x) >= bbox[0] + bbox[2] ||
                                        static_cast<s32>(y) < bbox[1] ||
                                        static_cast<s32>(y) >= bbox[1] + bbox[3])) {
                        continue;
                    }
                    gpu[static_cast<size_t>(y) * w + x] = px[static_cast<size_t>(y) * w + x];
                }
            }
        }
    }
}

size_t MarkedTiles(const TileMask& m) {
    return static_cast<size_t>(std::ranges::count_if(m.bits, [](u8 b) { return b != 0; }));
}
} // namespace

TEST_CASE("Aux UI diff skips identical publishes and marks only changed tiles", "[dsmod]") {
    AuxRouting aux;
    constexpr u32 W = 300, H = 200; // 5x4 tiles, ragged right/bottom edge
    std::vector<u32> canvas(W * H, 0xFF101010u);
    u64 serial = 0;
    aux.PublishUi(W, H, canvas);
    bool all = false;
    REQUIRE(aux.WithUiTiles(serial, [&](std::span<const u32>, u32, u32, const TileMask& m,
                                        std::array<s32, 4>) { all = m.all; }));
    REQUIRE(all); // first publish replaces everything

    aux.PublishUi(W, H, canvas); // identical frame
    REQUIRE_FALSE(aux.WithUiTiles(serial, [](auto&&...) {}));

    canvas[70 * W + 130] = 0xFFFF0000u;  // tile (2, 1)
    canvas[199 * W + 299] = 0xFF00FF00u; // last, ragged tile (4, 3)
    aux.PublishUi(W, H, canvas);
    REQUIRE(aux.WithUiTiles(serial, [&](std::span<const u32> px, u32 w, u32 h, const TileMask& m,
                                        std::array<s32, 4> bbox) {
        REQUIRE(bbox == std::array<s32, 4>{130, 70, 170, 130}); // union of both changed pixels
        REQUIRE(w == W);
        REQUIRE(h == H);
        REQUIRE_FALSE(m.all);
        REQUIRE(m.cols == 5);
        REQUIRE(m.rows == 4);
        REQUIRE(MarkedTiles(m) == 2);
        REQUIRE(m.bits[1 * 5 + 2] == 1);
        REQUIRE(m.bits[3 * 5 + 4] == 1);
        REQUIRE(std::ranges::equal(px, canvas));
    }));
}

TEST_CASE("Aux UI partial publish diffs inside the rect and resyncs after a composite run",
          "[dsmod]") {
    AuxRouting aux;
    constexpr u32 W = 256, H = 128;
    std::vector<u32> canvas(W * H, 0xFF000000u);
    u64 serial = 0;
    aux.PublishUi(W, H, canvas);
    REQUIRE(aux.WithUiTiles(serial, [](auto&&...) {}));

    // Redrawn but pixel-identical dirty rect: nothing to publish.
    aux.PublishUiPartial(W, H, canvas, {10, 10, 50, 50});
    REQUIRE_FALSE(aux.WithUiTiles(serial, [](auto&&...) {}));

    // A quad-path tick leaves ui_pixels stale for a change outside the next partial rect.
    aux.PublishComposite(W, H, 0xFF000000u, {});
    canvas[100 * W + 200] = 0xFFABCDEFu; // tile (3, 1), outside the rect below
    canvas[5 * W + 5] = 0xFF123456u;     // tile (0, 0), inside it
    aux.PublishUiPartial(W, H, canvas, {0, 0, 16, 16});
    // clang-format off: Catch2 stringifies the expression, keep its original layout
    REQUIRE(aux.WithUiTiles(serial, [&](std::span<const u32> px, u32, u32, const TileMask& m,
                                        std::array<s32, 4>) {
        REQUIRE_FALSE(m.all);
        REQUIRE(MarkedTiles(m) == 2);
        REQUIRE(m.bits[0] == 1);
        REQUIRE(m.bits[1 * 4 + 3] == 1);
        REQUIRE(std::ranges::equal(px, canvas));
    }));
    // clang-format on
}

TEST_CASE("Aux UI tiles accumulate across publishes and keep a consumer copy exact", "[dsmod]") {
    AuxRouting aux;
    constexpr u32 W = 333, H = 257;
    std::vector<u32> canvas(W * H, 0xFF202020u);
    std::vector<u32> gpu;
    u64 serial = 0;
    u32 rng = 12345;
    const auto next = [&] {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    const auto take = [&] {
        aux.WithUiTiles(serial,
                        [&](std::span<const u32> px, u32 w, u32 h, const TileMask& m,
                            std::array<s32, 4> bbox) { ApplyTake(gpu, px, w, h, m, bbox); });
    };
    aux.PublishUi(W, H, canvas);
    take();
    for (int i = 0; i < 300; ++i) {
        const s32 x = static_cast<s32>(next() % W), y = static_cast<s32>(next() % H);
        const s32 w = static_cast<s32>(next() % 90) + 1, h = static_cast<s32>(next() % 90) + 1;
        const u32 color = 0xFF000000u | (next() & 0xFFFFFFu);
        for (s32 yy = y; yy < std::min<s32>(H, y + h); ++yy) {
            for (s32 xx = x; xx < std::min<s32>(W, x + w); ++xx) {
                if ((next() & 3) != 0) { // leave some pixels identical inside the dirty rect
                    canvas[static_cast<size_t>(yy) * W + xx] = color;
                }
            }
        }
        switch (next() % 4) {
        case 0:
            aux.PublishUi(W, H, canvas);
            break;
        case 1:
            aux.PublishComposite(W, H, 0, {});
            [[fallthrough]];
        default:
            aux.PublishUiPartial(W, H, canvas, {x, y, w, h});
            break;
        }
        if ((next() % 3) == 0) { // the renderer polls slower than the producer publishes
            take();
            REQUIRE(gpu == canvas);
        }
    }
    take();
    REQUIRE(gpu == canvas);
}

TEST_CASE("Aux texture publish diffs same-size republishes", "[dsmod]") {
    AuxRouting aux;
    constexpr u32 W = 130, H = 70;
    std::vector<u32> hud(W * H, 0u);
    u64 serial = 0;
    aux.PublishAuxTexture(2, W, H, hud);
    bool all = false;
    // clang-format off: Catch2 stringifies the expression, keep its original layout
    REQUIRE(aux.WithAuxTextureTiles(2, serial,
                                    [&](std::span<const u32>, u32, u32, const TileMask& m) {
                                        all = m.all;
                                    }));
    // clang-format on
    REQUIRE(all);
    aux.PublishAuxTexture(2, W, H, hud);
    REQUIRE_FALSE(aux.WithAuxTextureTiles(2, serial, [](auto&&...) {}));
    hud[69 * W + 129] = 0x80FFFFFFu;
    aux.PublishAuxTexture(2, W, H, hud);
    REQUIRE(aux.WithAuxTextureTiles(2, serial,
                                    [&](std::span<const u32> px, u32, u32, const TileMask& m) {
                                        REQUIRE_FALSE(m.all);
                                        REQUIRE(MarkedTiles(m) == 1);
                                        REQUIRE(m.bits[1 * 3 + 2] == 1);
                                        REQUIRE(std::ranges::equal(px, hud));
                                    }));
}

TEST_CASE("Aux texture rect publish diffs only the dirty rect", "[dsmod]") {
    AuxRouting aux;
    constexpr u32 W = 130, H = 70;
    std::vector<u32> hud(W * H, 0u);
    u64 serial = 0;
    // No texture there yet: the whole picture, whatever the rect.
    const std::array<std::array<s32, 4>, 1> rect{{{0, 0, 10, 10}}};
    aux.PublishAuxTextureRects(2, W, H, hud, rect);
    REQUIRE(aux.WithAuxTexture(2, serial, [](auto&&...) {}));
    // A change inside the rect is taken, the rect clipped to the texture.
    hud[5 * W + 5] = 0xFF00FF00u;
    hud[69 * W + 129] = 0xFF0000FFu; // outside the rect: the producer says it did not change
    const std::array<std::array<s32, 4>, 1> dirty{{{-4, -4, 12, 12}}};
    aux.PublishAuxTextureRects(2, W, H, hud, dirty);
    REQUIRE(aux.WithAuxTextureTiles(2, serial,
                                    [&](std::span<const u32> px, u32, u32, const TileMask& m) {
                                        REQUIRE(MarkedTiles(m) == 1);
                                        REQUIRE(px[5 * W + 5] == 0xFF00FF00u);
                                        REQUIRE(px[69 * W + 129] == 0u);
                                    }));
    // Nothing changed inside the rect: no new serial.
    aux.PublishAuxTextureRects(2, W, H, hud, dirty);
    REQUIRE_FALSE(aux.WithAuxTexture(2, serial, [](auto&&...) {}));
    // A size change takes the whole picture again.
    std::vector<u32> bigger((W + 1) * H, 0x11111111u);
    aux.PublishAuxTextureRects(2, W + 1, H, bigger, dirty);
    REQUIRE(aux.WithAuxTexture(2, serial, [&](std::span<const u32> px, u32 w, u32) {
        REQUIRE(w == W + 1);
        REQUIRE(std::ranges::equal(px, bigger));
    }));
}

TEST_CASE("Aux map bundle tile publish keeps the consumer copy exact and patches only changes",
          "[dsmod]") {
    AuxRouting aux;
    constexpr u32 W = 300, H = 200; // 5x4 tiles, ragged right/bottom edge
    constexpr u32 FW = 3, FH = 2;
    std::vector<u32> current(static_cast<size_t>(W) * H), previous(current.size());
    for (size_t i = 0; i < current.size(); ++i) {
        current[i] = 0xFF000000u | static_cast<u32>(i * 2654435761u >> 8);
        previous[i] = 0xFF000000u | static_cast<u32>(i * 40503u);
    }
    std::vector<u32> weights(FW * FH, 0xFFFF0000u);
    std::vector<u32> gpu_current, gpu_previous, gpu_fade;
    u64 cs = 0, ps = 0, fs = 0;
    std::array<size_t, AuxRouting::NumAuxTex> uploaded_tiles{};
    const auto take = [&] {
        uploaded_tiles.fill(0);
        return aux.WithMapFadeTextures(
            cs, ps, fs, [&](u32 slot, std::span<const u32> px, u32 w, u32 h, const TileMask& m) {
                auto& gpu = slot == AuxRouting::MapCurrentSlot    ? gpu_current
                            : slot == AuxRouting::MapPreviousSlot ? gpu_previous
                                                                  : gpu_fade;
                if (m.all || gpu.size() != px.size()) {
                    gpu.assign(px.begin(), px.end()); // what a (re)created image uploads
                    uploaded_tiles[slot] = ~size_t{0};
                } else {
                    ApplyTake(gpu, px, w, h, m);
                    uploaded_tiles[slot] = MarkedTiles(m);
                }
            });
    };
    const auto diff_publish = [&](u64 base, const std::vector<u32>& old_cur,
                                  const std::vector<u32>& old_prev) {
        TileMask dc, dp;
        dc.Reset(W, H);
        dc.Clear();
        dp.Reset(W, H);
        dp.Clear();
        VideoCore::DSMod::DiffTiles(current, old_cur, W, H, dc);
        VideoCore::DSMod::DiffTiles(previous, old_prev, W, H, dp);
        return aux.PublishMapFadeTexturesTiles(W, H, current, previous, dc, dp, FW, FH, weights,
                                               base);
    };

    u64 epoch = aux.PublishMapFadeTextures(W, H, current, previous, FW, FH, weights);
    REQUIRE(epoch != 0);
    REQUIRE(take());
    REQUIRE(gpu_current == current);
    REQUIRE(gpu_previous == previous);

    // A reveal: a few pixels in one tile of the current endpoint, one in another tile of the
    // previous endpoint, one on the ragged edge.
    std::vector<u32> old_cur = current, old_prev = previous;
    current[70 * W + 70] ^= 0x00FFFFFFu;
    current[71 * W + 72] ^= 0x00FF00FFu;
    current[(H - 1) * W + (W - 1)] ^= 0x0000FF00u;
    previous[5 * W + 250] ^= 0x00000F0Fu;
    weights[1] = 0xFF7F0000u;
    const u64 next = diff_publish(epoch, old_cur, old_prev);
    REQUIRE(next != 0);
    REQUIRE(next != epoch);
    REQUIRE(take());
    REQUIRE(uploaded_tiles[AuxRouting::MapCurrentSlot] == 2);
    REQUIRE(uploaded_tiles[AuxRouting::MapPreviousSlot] == 1);
    REQUIRE(gpu_current == current);
    REQUIRE(gpu_previous == previous);
    REQUIRE(gpu_fade == weights);
    epoch = next;

    // Two diff publishes before the consumer takes: the tiles of both accumulate.
    old_cur = current;
    old_prev = previous;
    current[10 * W + 10] ^= 1u;
    epoch = diff_publish(epoch, old_cur, old_prev);
    REQUIRE(epoch != 0);
    old_cur = current;
    current[150 * W + 290] ^= 1u;
    epoch = diff_publish(epoch, old_cur, old_prev);
    REQUIRE(epoch != 0);
    REQUIRE(take());
    REQUIRE(uploaded_tiles[AuxRouting::MapCurrentSlot] == 2);
    REQUIRE(uploaded_tiles[AuxRouting::MapPreviousSlot] == 0); // unchanged: serial not bumped
    REQUIRE(gpu_current == current);
    REQUIRE(gpu_previous == previous);

    // Nothing changed at all: only the weight grid is re-sent.
    old_cur = current;
    epoch = diff_publish(epoch, old_cur, old_prev);
    REQUIRE(epoch != 0);
    REQUIRE(take());
    REQUIRE(uploaded_tiles[AuxRouting::MapCurrentSlot] == 0);
    REQUIRE(uploaded_tiles[AuxRouting::MapPreviousSlot] == 0);
    REQUIRE(uploaded_tiles[AuxRouting::MapFadeSlot] == ~size_t{0});

    // A stale base (another full publish came between) is refused and leaves the buffer alone.
    const u64 other = aux.PublishMapFadeTextures(W, H, current, previous, FW, FH, weights);
    REQUIRE(other != epoch);
    old_cur = current;
    current[0] ^= 1u;
    REQUIRE(diff_publish(epoch, old_cur, old_prev) == 0);
    REQUIRE(take());
    REQUIRE(gpu_current == old_cur); // the full publish's content, untouched by the refusal
    // A write of an endpoint slot through the generic publish also invalidates the base.
    epoch = aux.PublishMapFadeTextures(W, H, current, previous, FW, FH, weights);
    aux.PublishAuxTexture(AuxRouting::MapPreviousSlot, W, H, previous);
    old_cur = current;
    current[1] ^= 1u;
    REQUIRE(diff_publish(epoch, old_cur, old_prev) == 0);
    // A size change is refused.
    TileMask small;
    small.Reset(W / 2, H);
    small.Clear();
    REQUIRE(aux.PublishMapFadeTexturesTiles(W / 2, H, std::span{current}.first(W / 2 * H),
                                            std::span{previous}.first(W / 2 * H), small, small,
                                            FW, FH, weights, aux.PublishMapFadeTextures(
                                                W, H, current, previous, FW, FH, weights)) ==
            0);
}

TEST_CASE("Aux DiffTiles marks exactly the tiles that differ", "[dsmod]") {
    constexpr u32 W = 130, H = 70; // 3x2 tiles
    std::vector<u32> a(static_cast<size_t>(W) * H, 0xFF000000u), b = a;
    TileMask m;
    m.Reset(W, H);
    m.Clear();
    VideoCore::DSMod::DiffTiles(a, b, W, H, m);
    REQUIRE(MarkedTiles(m) == 0);
    b[0] = 1;                    // tile (0,0)
    b[65 * W + 129] = 1;         // tile (1,2), ragged corner
    b[63 * W + 64] = 1;          // tile (0,1), last row of the first band
    VideoCore::DSMod::DiffTiles(a, b, W, H, m);
    REQUIRE(MarkedTiles(m) == 3);
    REQUIRE(m.bits[0] == 1);
    REQUIRE(m.bits[1] == 1);
    REQUIRE(m.bits[1 * m.cols + 2] == 1);
}
