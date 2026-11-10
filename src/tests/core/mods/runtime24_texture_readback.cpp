// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 24: guest texture readback -- the request rules, the image picker, pixel packing and
// the broker's limits and fail-closed paths (mod_texture_readback.h,
// video_core/dsmod/texture_readback.h), against a fake renderer.
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_texture_readback.h"
#include "core/mods/mod_ui.h"
#include "video_core/dsmod/texture_readback.h"

using namespace Core::Mods;
using VideoCore::DSMod::ReadbackCallback;
using VideoCore::DSMod::ReadbackCandidate;
using VideoCore::DSMod::ReadbackFormat;
using VideoCore::DSMod::ReadbackRequest;
using VideoCore::DSMod::ReadbackResult;
using VideoCore::DSMod::ReadbackStatus;
using VideoCore::Surface::PixelFormat;
using Clock = TextureReadbackBroker::Clock;

namespace {

/// A renderer that records each submission and completes it when the test says so.
struct FakeRenderer {
    bool available = true;
    bool accept = true;
    bool synchronous = false; ///< complete inside Submit
    std::vector<ReadbackRequest> requests;
    std::vector<ReadbackCallback> callbacks;
    ReadbackResult sync_result;
};

class FakeBackend final : public TextureReadbackBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeRenderer> r) : renderer{std::move(r)} {}
    bool Available() const override {
        return renderer->available;
    }
    bool Submit(const ReadbackRequest& request, ReadbackCallback callback) override {
        if (!renderer->accept) {
            return false;
        }
        renderer->requests.push_back(request);
        if (renderer->synchronous) {
            auto copy = renderer->sync_result;
            callback(std::move(copy));
        } else {
            renderer->callbacks.push_back(std::move(callback));
        }
        return true;
    }

private:
    std::shared_ptr<FakeRenderer> renderer;
};

EdenDsmodTextureRequest Req(u32 w, u32 h, u32 format = EDEN_DSMOD_TEXTURE_R8, u64 gpu = 0x1000) {
    return {.struct_size = sizeof(EdenDsmodTextureRequest),
            .format = format,
            .gpu_address = gpu,
            .width = w,
            .height = h,
            .flags = 0,
            .reserved = 0};
}

ReadbackResult Ready(u32 w, u32 h, u32 bpp, u64 gpu = 0x1000, u64 generation = 7) {
    ReadbackResult r;
    r.status = ReadbackStatus::Ready;
    r.gpu_addr = gpu;
    r.width = w;
    r.height = h;
    r.generation = generation;
    r.candidates = 1;
    r.pixels.resize(size_t{w} * h * bpp);
    for (size_t i = 0; i < r.pixels.size(); ++i) {
        r.pixels[i] = static_cast<u8>(i * 7);
    }
    return r;
}

EdenDsmodTextureInfo Info() {
    EdenDsmodTextureInfo info{};
    info.struct_size = sizeof(info);
    return info;
}

struct Fixture {
    std::shared_ptr<FakeRenderer> renderer = std::make_shared<FakeRenderer>();
    std::optional<TextureReadbackBroker> broker;
    Clock::time_point t0 = Clock::now();
    Fixture() {
        broker.emplace(std::make_unique<FakeBackend>(renderer));
    }
};

ReadbackCandidate Candidate(u64 gpu, u32 w, u32 h, PixelFormat format, u64 generation = 1) {
    return {.gpu_addr = gpu,
            .width = w,
            .height = h,
            .format = format,
            .plain_2d = true,
            .current = true,
            .generation = generation};
}

} // namespace

TEST_CASE("DSMod texture readback: runtime 24 and the ABI constants", "[dsmod][runtime24]") {
    REQUIRE(DualScreenRuntimeVersion >= 24);
    // A host-only capability bit that collides with no other.
    constexpr u64 others = EDEN_DSMOD_CAP_WRITE_MEMORY | EDEN_DSMOD_CAP_GUEST_CALL |
                           EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_MAP_OUTPUT |
                           EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN |
                           EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_SOURCE_PREFIXES |
                           EDEN_DSMOD_CAP_SOURCE_BASE | EDEN_DSMOD_CAP_SOURCE_AOC |
                           EDEN_DSMOD_CAP_SOURCE_USER | EDEN_DSMOD_CAP_FONT_EPOCH |
                           EDEN_DSMOD_CAP_SOURCE_SYSFONT;
    REQUIRE((others & EDEN_DSMOD_CAP_TEXTURE_READBACK) == 0);
    REQUIRE(EDEN_DSMOD_TEXTURE_MAX_SIDE == 2048);
    REQUIRE(EDEN_DSMOD_TEXTURE_MAX_BYTES == 16u * 1024 * 1024);
}

TEST_CASE("DSMod texture readback: request bounds and formats", "[dsmod][runtime24]") {
    using namespace VideoCore::DSMod;
    REQUIRE(ValidReadbackFormat(1));
    REQUIRE(ValidReadbackFormat(2));
    REQUIRE_FALSE(ValidReadbackFormat(0));
    REQUIRE_FALSE(ValidReadbackFormat(3));
    REQUIRE(ReadbackRequestInBounds(256, 256, ReadbackFormat::R8));
    REQUIRE(ReadbackRequestInBounds(2048, 2048, ReadbackFormat::RGBA8)); // exactly 16 MiB
    REQUIRE_FALSE(ReadbackRequestInBounds(0, 256, ReadbackFormat::R8));
    REQUIRE_FALSE(ReadbackRequestInBounds(256, 0, ReadbackFormat::R8));
    REQUIRE_FALSE(ReadbackRequestInBounds(2049, 1, ReadbackFormat::R8));
    REQUIRE_FALSE(ReadbackRequestInBounds(1, 2049, ReadbackFormat::R8));

    REQUIRE(ReadbackFormatMatches(PixelFormat::R8_UNORM, ReadbackFormat::R8));
    REQUIRE_FALSE(ReadbackFormatMatches(PixelFormat::R8_SNORM, ReadbackFormat::R8));
    REQUIRE_FALSE(ReadbackFormatMatches(PixelFormat::R8_UNORM, ReadbackFormat::RGBA8));
    for (const auto f : {PixelFormat::A8B8G8R8_UNORM, PixelFormat::A8B8G8R8_SRGB,
                         PixelFormat::B8G8R8A8_UNORM, PixelFormat::B8G8R8A8_SRGB}) {
        REQUIRE(ReadbackFormatMatches(f, ReadbackFormat::RGBA8));
        REQUIRE_FALSE(ReadbackFormatMatches(f, ReadbackFormat::R8));
    }
    for (const auto f : {PixelFormat::BC1_RGBA_UNORM, PixelFormat::D32_FLOAT,
                         PixelFormat::R16G16B16A16_FLOAT, PixelFormat::A2B10G10R10_UNORM,
                         PixelFormat::ASTC_2D_4X4_UNORM}) {
        REQUIRE_FALSE(ReadbackFormatMatches(f, ReadbackFormat::RGBA8));
        REQUIRE_FALSE(ReadbackFormatMatches(f, ReadbackFormat::R8));
    }
}

TEST_CASE("DSMod texture readback: picking the image", "[dsmod][runtime24]") {
    using VideoCore::DSMod::PickReadbackImage;
    const ReadbackRequest by_address{.gpu_addr = 0x5000, .width = 256, .height = 256};
    const ReadbackRequest by_size{.gpu_addr = 0, .width = 256, .height = 256};

    SECTION("address: exact match") {
        const std::array images{Candidate(0x4000, 256, 256, PixelFormat::R8_UNORM),
                                Candidate(0x5000, 256, 256, PixelFormat::R8_UNORM)};
        const auto pick = PickReadbackImage(by_address, images);
        REQUIRE(pick.status == ReadbackStatus::Ready);
        REQUIRE(pick.index == 1);
        REQUIRE(pick.candidates == 1);
    }
    SECTION("address: nothing based there") {
        const std::array images{Candidate(0x4000, 256, 256, PixelFormat::R8_UNORM)};
        REQUIRE(PickReadbackImage(by_address, images).status == ReadbackStatus::NotFound);
        REQUIRE(PickReadbackImage(by_address, std::vector<ReadbackCandidate>{}).status ==
                ReadbackStatus::NotFound);
    }
    SECTION("address: wrong size, format, kind or stale") {
        auto wrong_size = Candidate(0x5000, 512, 256, PixelFormat::R8_UNORM);
        auto wrong_format = Candidate(0x5000, 256, 256, PixelFormat::A8B8G8R8_UNORM);
        auto array = Candidate(0x5000, 256, 256, PixelFormat::R8_UNORM);
        array.plain_2d = false;
        auto stale = Candidate(0x5000, 256, 256, PixelFormat::R8_UNORM);
        stale.current = false;
        for (const auto& image : {wrong_size, wrong_format, array, stale}) {
            const std::array images{image};
            REQUIRE(PickReadbackImage(by_address, images).status == ReadbackStatus::Mismatch);
        }
    }
    SECTION("address: two views of the same address, the newest fitting one wins") {
        const std::array images{Candidate(0x5000, 256, 256, PixelFormat::R8_UNORM, 3),
                                Candidate(0x5000, 128, 128, PixelFormat::R8_UNORM, 9),
                                Candidate(0x5000, 256, 256, PixelFormat::R8_UNORM, 5)};
        const auto pick = PickReadbackImage(by_address, images);
        REQUIRE(pick.status == ReadbackStatus::Ready);
        REQUIRE(pick.index == 2);
    }
    SECTION("size: the newest fitting one, with the candidate count") {
        auto stale = Candidate(0x9000, 256, 256, PixelFormat::R8_UNORM, 99);
        stale.current = false;
        const std::array images{Candidate(0x1000, 256, 256, PixelFormat::R8_UNORM, 4),
                                Candidate(0x2000, 256, 256, PixelFormat::R8_UNORM, 8),
                                Candidate(0x3000, 256, 256, PixelFormat::A8B8G8R8_UNORM, 50),
                                Candidate(0x4000, 512, 512, PixelFormat::R8_UNORM, 60), stale};
        const auto pick = PickReadbackImage(by_size, images);
        REQUIRE(pick.status == ReadbackStatus::Ready);
        REQUIRE(pick.index == 1);
        REQUIRE(pick.candidates == 2);
    }
    SECTION("size: none") {
        const std::array images{Candidate(0x1000, 128, 256, PixelFormat::R8_UNORM)};
        const auto pick = PickReadbackImage(by_size, images);
        REQUIRE(pick.status == ReadbackStatus::NotFound);
        REQUIRE(pick.candidates == 0);
    }
}

TEST_CASE("DSMod texture readback: selector flags", "[dsmod][runtime24]") {
    using namespace VideoCore::DSMod;
    REQUIRE(EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET == 1u);
    REQUIRE(EDEN_DSMOD_TEXTURE_REQUIRE_SRGB == 2u);
    REQUIRE(EDEN_DSMOD_TEXTURE_REQUIRE_UNORM == 4u);
    REQUIRE(EDEN_DSMOD_TEXTURE_REQUIRE_REDRAWN == 8u);
    // 0 is today's behaviour; every known combination is valid except SRGB+UNORM and SRGB on R8.
    for (u32 flags = 0; flags <= ReadbackKnownFlags; ++flags) {
        const bool both = (flags & ReadbackRequireSrgb) && (flags & ReadbackRequireUnorm);
        REQUIRE(ValidReadbackFlags(flags, ReadbackFormat::RGBA8) == !both);
        REQUIRE(ValidReadbackFlags(flags, ReadbackFormat::R8) ==
                ((flags & ReadbackRequireSrgb) == 0));
    }
    for (u32 bit = 4; bit < 32; ++bit) {
        REQUIRE_FALSE(ValidReadbackFlags(1u << bit, ReadbackFormat::RGBA8));
        REQUIRE_FALSE(ValidReadbackFlags(1u << bit, ReadbackFormat::R8));
    }

    auto image = Candidate(0x1000, 256, 256, PixelFormat::A8B8G8R8_SRGB);
    REQUIRE(ReadbackFlagsMatch(image, 0));
    REQUIRE_FALSE(ReadbackFlagsMatch(image, ReadbackRequireRenderTarget));
    REQUIRE_FALSE(ReadbackFlagsMatch(image, ReadbackRequireRedrawn));
    REQUIRE(ReadbackFlagsMatch(image, ReadbackRequireSrgb));
    REQUIRE_FALSE(ReadbackFlagsMatch(image, ReadbackRequireUnorm));
    image.redrawn = true; // inconsistent input (redrawn without render target) never matches
    REQUIRE_FALSE(ReadbackFlagsMatch(image, ReadbackRequireRedrawn));
    image.render_target = true;
    REQUIRE(ReadbackFlagsMatch(image, ReadbackRequireRedrawn | ReadbackRequireRenderTarget |
                                          ReadbackRequireSrgb));
    for (const auto f : {PixelFormat::A8B8G8R8_UNORM, PixelFormat::B8G8R8A8_UNORM,
                         PixelFormat::R8_UNORM}) {
        const auto unorm = Candidate(0x1000, 256, 256, f);
        REQUIRE(ReadbackFlagsMatch(unorm, ReadbackRequireUnorm));
        REQUIRE_FALSE(ReadbackFlagsMatch(unorm, ReadbackRequireSrgb));
    }
    REQUIRE(ReadbackFlagsMatch(Candidate(0x1000, 256, 256, PixelFormat::B8G8R8A8_SRGB),
                               ReadbackRequireSrgb));
}

// The resident 256x256 RGBA8 images of a real title in gameplay (My Time at Sandrock, indoors,
// desktop 2026-11-10, evidence rt24-fog-selector/investigation/inv4): one fog-of-war mask the game
// keeps painting (sRGB, drawn into in many frames, *not* the newest write), six one-off sRGB
// render targets drawn once at scene load and then only sampled, one UNORM one-off render target
// at the same address as one of them, two UNORM copy/blit targets, one stale mipmapped texture.
TEST_CASE("DSMod texture readback: selector flags pick one of many", "[dsmod][runtime24]") {
    using namespace VideoCore::DSMod;
    const auto rt = [](u64 gpu, PixelFormat format, u64 generation, bool redrawn) {
        auto c = Candidate(gpu, 256, 256, format, generation);
        c.render_target = true;
        c.redrawn = redrawn;
        return c;
    };
    auto stale = Candidate(0x50bc30000, 256, 256, PixelFormat::A8B8G8R8_SRGB, 0);
    stale.current = false;
    constexpr u64 fog = 0x5108c0000;
    const std::vector<ReadbackCandidate> images{
        rt(0x510110000, PixelFormat::A8B8G8R8_UNORM, 38593, false),
        rt(fog, PixelFormat::A8B8G8R8_SRGB, 1056116, true),
        rt(0x510670000, PixelFormat::A8B8G8R8_SRGB, 1063082, false),
        rt(0x510630000, PixelFormat::A8B8G8R8_SRGB, 1063068, false),
        Candidate(0x5104c0000, 256, 256, PixelFormat::A8B8G8R8_UNORM, 1063081),
        rt(0x510110000, PixelFormat::A8B8G8R8_SRGB, 1063030, false),
        stale,
        rt(0x510420000, PixelFormat::A8B8G8R8_SRGB, 1092282, false),
        rt(0x5103e0000, PixelFormat::A8B8G8R8_SRGB, 1092268, false),
        Candidate(0x510250000, 256, 256, PixelFormat::A8B8G8R8_UNORM, 1092281),
        rt(0x510210000, PixelFormat::A8B8G8R8_SRGB, 1092230, false),
    };
    const auto size = [&](u32 flags) {
        return PickReadbackImage(ReadbackRequest{.gpu_addr = 0,
                                                 .width = 256,
                                                 .height = 256,
                                                 .format = ReadbackFormat::RGBA8,
                                                 .flags = flags},
                                 images);
    };
    // flags 0: today's behaviour -- ten candidates, the newest write (a one-off target) wins.
    auto pick = size(0);
    REQUIRE(pick.status == ReadbackStatus::Ready);
    REQUIRE(pick.candidates == 10);
    REQUIRE(images[pick.index].gpu_addr == 0x510420000);
    REQUIRE(size(ReadbackRequireRenderTarget).candidates == 8);
    REQUIRE(size(ReadbackRequireSrgb).candidates == 7);
    REQUIRE(size(ReadbackRequireRenderTarget | ReadbackRequireSrgb).candidates == 7);
    REQUIRE(size(ReadbackRequireUnorm).candidates == 3);
    REQUIRE(size(ReadbackRequireRenderTarget | ReadbackRequireUnorm).candidates == 1);
    // The redrawn render target is the only one, although it is not the newest write.
    for (const u32 flags :
         {ReadbackRequireRedrawn, ReadbackRequireRenderTarget | ReadbackRequireSrgb |
                                      ReadbackRequireRedrawn}) {
        pick = size(flags);
        REQUIRE(pick.status == ReadbackStatus::Ready);
        REQUIRE(pick.candidates == 1);
        REQUIRE(images[pick.index].gpu_addr == fog);
    }
    REQUIRE(size(ReadbackRequireUnorm | ReadbackRequireRedrawn).status ==
            ReadbackStatus::NotFound);
    REQUIRE(size(ReadbackRequireUnorm | ReadbackRequireRedrawn).candidates == 0);

    // Address mode applies the same flags: the fog still matches, a one-off target is a mismatch.
    const auto at = [&](u64 gpu, u32 flags) {
        return PickReadbackImage(ReadbackRequest{.gpu_addr = gpu,
                                                 .width = 256,
                                                 .height = 256,
                                                 .format = ReadbackFormat::RGBA8,
                                                 .flags = flags},
                                 images);
    };
    constexpr u32 fog_flags =
        ReadbackRequireRenderTarget | ReadbackRequireSrgb | ReadbackRequireRedrawn;
    pick = at(fog, fog_flags);
    REQUIRE(pick.status == ReadbackStatus::Ready);
    REQUIRE(pick.candidates == 1);
    REQUIRE(images[pick.index].gpu_addr == fog);
    REQUIRE(at(0x510420000, fog_flags).status == ReadbackStatus::Mismatch);
    REQUIRE(at(0x510420000, 0).status == ReadbackStatus::Ready);
    // Two images based at one address: the flags choose between them.
    pick = at(0x510110000, ReadbackRequireSrgb);
    REQUIRE(pick.status == ReadbackStatus::Ready);
    REQUIRE(images[pick.index].format == PixelFormat::A8B8G8R8_SRGB);
    pick = at(0x510110000, ReadbackRequireUnorm);
    REQUIRE(pick.status == ReadbackStatus::Ready);
    REQUIRE(images[pick.index].format == PixelFormat::A8B8G8R8_UNORM);
    REQUIRE(at(0x510110000, ReadbackRequireRedrawn).status == ReadbackStatus::Mismatch);
}

TEST_CASE("DSMod texture readback: packing rows", "[dsmod][runtime24]") {
    using VideoCore::DSMod::PackReadbackPixels;
    std::vector<u8> out;
    // R8, 3x2 with 4-byte rows: the padding byte is dropped.
    const std::vector<u8> r8{1, 2, 3, 0xEE, 4, 5, 6};
    REQUIRE(PackReadbackPixels(ReadbackFormat::R8, false, 3, 2, r8, 4, out));
    REQUIRE(out == std::vector<u8>{1, 2, 3, 4, 5, 6});
    // BGRA host storage becomes RGBA.
    const std::vector<u8> bgra{10, 20, 30, 40, 11, 21, 31, 41};
    REQUIRE(PackReadbackPixels(ReadbackFormat::RGBA8, true, 2, 1, bgra, 8, out));
    REQUIRE(out == std::vector<u8>{30, 20, 10, 40, 31, 21, 11, 41});
    REQUIRE(PackReadbackPixels(ReadbackFormat::RGBA8, false, 2, 1, bgra, 8, out));
    REQUIRE(out == bgra);
    // Too little source, rows narrower than a line, empty: refused.
    REQUIRE_FALSE(PackReadbackPixels(ReadbackFormat::R8, false, 3, 2, r8, 5, out));
    REQUIRE_FALSE(PackReadbackPixels(ReadbackFormat::R8, false, 3, 2, r8, 2, out));
    REQUIRE_FALSE(PackReadbackPixels(ReadbackFormat::R8, false, 0, 2, r8, 4, out));
}

TEST_CASE("DSMod texture readback broker: invalid requests never reach the renderer",
          "[dsmod][runtime24]") {
    Fixture f;
    s32 status = 99;
    REQUIRE(f.broker->Request(nullptr, &status, f.t0) == 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_INVALID);
    auto bad_size = Req(256, 256);
    bad_size.struct_size = sizeof(EdenDsmodTextureRequest) - 4;
    auto bad_format = Req(256, 256, 3);
    auto zero_format = Req(256, 256, 0);
    auto flags = Req(256, 256);
    flags.flags = 1u << 4; // unknown selector bit
    auto high_flags = Req(256, 256, EDEN_DSMOD_TEXTURE_RGBA8);
    high_flags.flags = 0x80000000u | EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET;
    auto srgb_and_unorm = Req(256, 256, EDEN_DSMOD_TEXTURE_RGBA8);
    srgb_and_unorm.flags = EDEN_DSMOD_TEXTURE_REQUIRE_SRGB | EDEN_DSMOD_TEXTURE_REQUIRE_UNORM;
    auto srgb_r8 = Req(256, 256, EDEN_DSMOD_TEXTURE_R8);
    srgb_r8.flags = EDEN_DSMOD_TEXTURE_REQUIRE_SRGB;
    auto reserved = Req(256, 256);
    reserved.reserved = 1;
    for (const auto& request :
         {bad_size, bad_format, zero_format, flags, high_flags, srgb_and_unorm, srgb_r8, reserved,
          Req(0, 256), Req(256, 0), Req(2049, 16), Req(16, 4096)}) {
        status = 99;
        REQUIRE(f.broker->Request(&request, &status, f.t0) == 0);
        REQUIRE(status == EDEN_DSMOD_TEXTURE_INVALID);
    }
    REQUIRE(f.renderer->requests.empty());
    REQUIRE(f.broker->Open() == 0);
    // A null status pointer is fine.
    const auto good = Req(256, 256);
    REQUIRE(f.broker->Request(&good, nullptr, f.t0) != 0);
}

TEST_CASE("DSMod texture readback broker: selector flags reach the renderer",
          "[dsmod][runtime24]") {
    Fixture f;
    auto request = Req(256, 256, EDEN_DSMOD_TEXTURE_RGBA8, 0);
    request.flags = EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET | EDEN_DSMOD_TEXTURE_REQUIRE_SRGB |
                    EDEN_DSMOD_TEXTURE_REQUIRE_REDRAWN;
    s32 status{};
    REQUIRE(f.broker->Request(&request, &status, f.t0) != 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_PENDING);
    auto unorm_r8 = Req(256, 256, EDEN_DSMOD_TEXTURE_R8, 0x2000);
    unorm_r8.flags = EDEN_DSMOD_TEXTURE_REQUIRE_UNORM | EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET;
    REQUIRE(f.broker->Request(&unorm_r8, &status, f.t0) != 0);
    REQUIRE(f.renderer->requests.size() == 2);
    REQUIRE(f.renderer->requests[0].flags == 0xBu);
    REQUIRE(f.renderer->requests[0].gpu_addr == 0);
    REQUIRE(f.renderer->requests[1].flags == 0x5u);
    REQUIRE(f.renderer->requests[1].format == ReadbackFormat::R8);
}

TEST_CASE("DSMod texture readback broker: unavailable renderer", "[dsmod][runtime24]") {
    Fixture f;
    const auto request = Req(256, 256);
    s32 status{};
    f.renderer->available = false;
    REQUIRE(f.broker->Request(&request, &status, f.t0) == 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_UNSUPPORTED);
    f.renderer->available = true;
    f.renderer->accept = false; // e.g. the GPU is shutting down
    REQUIRE(f.broker->Request(&request, &status, f.t0) == 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_UNSUPPORTED);
    REQUIRE(f.broker->Open() == 0);
    // Refusals spend no rate budget.
    f.renderer->accept = true;
    REQUIRE(f.broker->Request(&request, &status, f.t0) != 0);
    REQUIRE(f.broker->Request(&request, &status, f.t0) != 0);
    // And a broker without a backend at all fails closed.
    TextureReadbackBroker none{nullptr};
    REQUIRE(none.Request(&request, &status, f.t0) == 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_UNSUPPORTED);
}

TEST_CASE("DSMod texture readback broker: a readback lands", "[dsmod][runtime24]") {
    Fixture f;
    const auto request = Req(4, 2, EDEN_DSMOD_TEXTURE_RGBA8, 0xABC000);
    s32 status{};
    const u64 ticket = f.broker->Request(&request, &status, f.t0);
    REQUIRE(ticket != 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_PENDING);
    REQUIRE(f.renderer->requests.size() == 1);
    REQUIRE(f.renderer->requests[0].gpu_addr == 0xABC000);
    REQUIRE(f.renderer->requests[0].format == ReadbackFormat::RGBA8);
    REQUIRE(f.renderer->requests[0].flags == 0);

    std::vector<u8> out(32, 0);
    auto info = Info();
    REQUIRE(f.broker->Poll(ticket, out.data(), out.size(), &info, f.t0) ==
            EDEN_DSMOD_TEXTURE_PENDING);
    REQUIRE(info.status == EDEN_DSMOD_TEXTURE_PENDING);

    const auto expected = Ready(4, 2, 4, 0xABC000, 42);
    f.renderer->callbacks[0](Ready(4, 2, 4, 0xABC000, 42));
    // Too small an output: INVALID, the ticket stays, the size is reported.
    info = Info();
    REQUIRE(f.broker->Poll(ticket, out.data(), 31, &info, f.t0) == EDEN_DSMOD_TEXTURE_INVALID);
    REQUIRE(info.size == 32);
    REQUIRE(f.broker->Poll(ticket, nullptr, 0, nullptr, f.t0) == EDEN_DSMOD_TEXTURE_INVALID);
    REQUIRE(f.broker->Open() == 1);
    info = Info();
    REQUIRE(f.broker->Poll(ticket, out.data(), out.size(), &info, f.t0) ==
            EDEN_DSMOD_TEXTURE_READY);
    REQUIRE(out == expected.pixels);
    REQUIRE(info.status == EDEN_DSMOD_TEXTURE_READY);
    REQUIRE(info.gpu_address == 0xABC000);
    REQUIRE(info.width == 4);
    REQUIRE(info.height == 2);
    REQUIRE(info.format == EDEN_DSMOD_TEXTURE_RGBA8);
    REQUIRE(info.generation == 42);
    REQUIRE(info.candidates == 1);
    REQUIRE(info.size == 32);
    // Closed.
    REQUIRE(f.broker->Open() == 0);
    REQUIRE(f.broker->Poll(ticket, out.data(), out.size(), &info, f.t0) ==
            EDEN_DSMOD_TEXTURE_UNKNOWN_TICKET);
    REQUIRE(f.broker->Poll(12345, out.data(), out.size(), nullptr, f.t0) ==
            EDEN_DSMOD_TEXTURE_UNKNOWN_TICKET);
}

TEST_CASE("DSMod texture readback broker: renderer errors close the ticket",
          "[dsmod][runtime24]") {
    for (const auto error : {ReadbackStatus::NotFound, ReadbackStatus::Mismatch,
                             ReadbackStatus::Unsupported, ReadbackStatus::Failed}) {
        Fixture f;
        const auto request = Req(256, 256);
        const u64 ticket = f.broker->Request(&request, nullptr, f.t0);
        ReadbackResult result;
        result.status = error;
        result.candidates = 0;
        f.renderer->callbacks[0](std::move(result));
        auto info = Info();
        std::vector<u8> out(256 * 256);
        REQUIRE(f.broker->Poll(ticket, out.data(), out.size(), &info, f.t0) ==
                static_cast<s32>(error));
        REQUIRE(info.status == static_cast<s32>(error));
        REQUIRE(info.size == 0);
        REQUIRE(f.broker->Open() == 0);
    }
}

TEST_CASE("DSMod texture readback broker: in-flight and rate limits", "[dsmod][runtime24]") {
    Fixture f;
    const auto request = Req(256, 256);
    s32 status{};
    const u64 a = f.broker->Request(&request, &status, f.t0);
    const u64 b = f.broker->Request(&request, &status, f.t0);
    REQUIRE(a != 0);
    REQUIRE(b != 0);
    REQUIRE(a != b);
    // Two open tickets: busy, whatever the clock says.
    REQUIRE(f.broker->Request(&request, &status, f.t0 + std::chrono::seconds{5}) == 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_BUSY);
    f.broker->Cancel(a);
    f.broker->Cancel(b);
    // Slots free again, but two were accepted in this second already.
    REQUIRE(f.broker->Request(&request, &status, f.t0 + std::chrono::milliseconds{999}) == 0);
    REQUIRE(status == EDEN_DSMOD_TEXTURE_RATE_LIMITED);
    REQUIRE(f.renderer->requests.size() == 2);
    // A second after the first: one more fits, then the window is full again.
    REQUIRE(f.broker->Request(&request, &status, f.t0 + std::chrono::milliseconds{1000}) != 0);
    REQUIRE(f.broker->Request(&request, &status, f.t0 + std::chrono::milliseconds{1000}) != 0);
    REQUIRE(f.renderer->requests.size() == 4);
}

TEST_CASE("DSMod texture readback broker: timeout, cancel and late completions",
          "[dsmod][runtime24]") {
    Fixture f;
    const auto request = Req(2, 2);
    const u64 slow = f.broker->Request(&request, nullptr, f.t0);
    const u64 dropped = f.broker->Request(&request, nullptr, f.t0);
    auto info = Info();
    std::array<u8, 4> out{};
    REQUIRE(f.broker->Poll(slow, out.data(), out.size(), &info,
                           f.t0 + std::chrono::milliseconds{EDEN_DSMOD_TEXTURE_TIMEOUT_MS - 1}) ==
            EDEN_DSMOD_TEXTURE_PENDING);
    REQUIRE(f.broker->Poll(slow, out.data(), out.size(), &info,
                           f.t0 + std::chrono::milliseconds{EDEN_DSMOD_TEXTURE_TIMEOUT_MS}) ==
            EDEN_DSMOD_TEXTURE_FAILED);
    REQUIRE(info.status == EDEN_DSMOD_TEXTURE_FAILED);
    f.broker->Cancel(dropped);
    f.broker->Cancel(dropped); // twice is fine
    f.broker->Cancel(777);     // unknown is fine
    REQUIRE(f.broker->Open() == 0);
    // The renderer finishes both later: nothing reopens.
    f.renderer->callbacks[0](Ready(2, 2, 1));
    f.renderer->callbacks[1](Ready(2, 2, 1));
    REQUIRE(f.broker->Open() == 0);
    REQUIRE(f.broker->Poll(slow, out.data(), out.size(), nullptr, f.t0) ==
            EDEN_DSMOD_TEXTURE_UNKNOWN_TICKET);
    // A completion after the broker is gone is dropped safely.
    const auto later = Req(2, 2);
    const u64 orphan =
        f.broker->Request(&later, nullptr, f.t0 + std::chrono::seconds{3});
    REQUIRE(orphan != 0);
    f.broker.reset();
    f.renderer->callbacks.back()(Ready(2, 2, 1));
}

TEST_CASE("DSMod texture readback broker: synchronous completion and info checks",
          "[dsmod][runtime24]") {
    Fixture f;
    f.renderer->synchronous = true;
    f.renderer->sync_result = Ready(2, 2, 1, 0x1000, 3);
    const auto request = Req(2, 2);
    const u64 ticket = f.broker->Request(&request, nullptr, f.t0);
    REQUIRE(ticket != 0);
    std::array<u8, 4> out{};
    EdenDsmodTextureInfo wrong{};
    wrong.struct_size = sizeof(wrong) - 8;
    REQUIRE(f.broker->Poll(ticket, out.data(), out.size(), &wrong, f.t0) ==
            EDEN_DSMOD_TEXTURE_INVALID);
    REQUIRE(f.broker->Open() == 1); // a bad info struct does not consume the result
    auto info = Info();
    REQUIRE(f.broker->Poll(ticket, out.data(), out.size(), &info, f.t0) ==
            EDEN_DSMOD_TEXTURE_READY);
    REQUIRE(info.generation == 3);
}

// --- Map overlay "mask_bind" -----------------------------------------------------------------

namespace {
constexpr u32 Blue = 0xFF0000FFu, Green = 0xFF00FF00u;

std::shared_ptr<const Core::Mods::Image> Pic(std::vector<u32> pixels, u32 w, u32 h) {
    auto img = std::make_shared<Core::Mods::Image>();
    img->w = w;
    img->h = h;
    img->pixels = std::move(pixels);
    return img;
}

/// Area "f": world (0,0)-(100,100), blue base; widget 100x100 at the origin; one green overlay
/// over the whole area, masked by the picture the text point "fog.mask" names.
struct FogMap {
    Manifest manifest;
    Page page;
    std::map<std::string, std::shared_ptr<const Core::Mods::Image>> pics{
        {"img.blue", Pic(std::vector<u32>(64, Blue), 8, 8)},
        {"img.green", Pic(std::vector<u32>(64, Green), 8, 8)},
        // Left texel kept (alpha 255), right texel cut (alpha 0); colour is ignored.
        {"module:fog#1", Pic({0xFF123456u, 0x00FFFFFFu}, 2, 1)},
    };
    FogMap() {
        manifest.background = 0xFF000000u;
        MapArea area;
        area.min_x = 0.0f;
        area.max_x = 100.0f;
        area.min_y = 0.0f;
        area.max_y = 100.0f;
        area.image = "img.blue";
        manifest.map_areas.emplace("f", area);
        Widget w;
        w.type = WidgetType::Map;
        w.id = "m";
        w.area = "f";
        w.rect = {0, 0, 100, 100};
        w.pan_zoom = true;
        w.area_label = false;
        auto extras = std::make_shared<MapWidgetExtras>();
        MapWidgetExtras::Overlay ov;
        ov.src = "img.green";
        ov.mask_bind = "fog.mask";
        ov.x0 = 0.0f;
        ov.y0 = 0.0f;
        ov.x1 = 100.0f;
        ov.y1 = 100.0f;
        extras->overlays.push_back(ov);
        w.map_extras = extras;
        page.id = "p";
        page.widgets.push_back(w);
    }
    Canvas Draw(const StateSnapshot& s) const {
        Canvas canvas;
        canvas.Resize(100, 100);
        ViewState views;
        RenderPage(canvas, manifest, page, s,
                   [this](const std::string& key) -> std::shared_ptr<const Core::Mods::Image> {
                       const auto it = pics.find(key);
                       return it != pics.end() ? it->second : nullptr;
                   },
                   views);
        return canvas;
    }
};
u32 Px(const Canvas& c, s32 x, s32 y) {
    return c.Pixels()[static_cast<size_t>(y) * c.Width() + static_cast<size_t>(x)];
}
} // namespace

TEST_CASE("DSMod map overlay mask_bind: a bound alpha mask cuts the overlay",
          "[dsmod][runtime24][mask_bind]") {
    const FogMap map;
    StateSnapshot s;
    s.texts["fog.mask"] = "module:fog#1";
    auto c = map.Draw(s);
    REQUIRE(Px(c, 5, 50) == Green); // the kept half
    REQUIRE(Px(c, 95, 50) == Blue); // the cut half shows the base
    // Bilinear in between: a blend of the two, more green left of centre than right of it.
    const u32 left = Px(c, 40, 50), right = Px(c, 60, 50);
    REQUIRE(((left >> 8) & 0xFF) > ((right >> 8) & 0xFF));
    REQUIRE((left & 0xFF) < (right & 0xFF));
    REQUIRE(left != Green);
    REQUIRE(right != Blue);
    // An empty or missing point: no mask, the overlay is drawn whole.
    s.texts["fog.mask"] = "";
    c = map.Draw(s);
    REQUIRE(Px(c, 95, 50) == Green);
    s.texts.erase("fog.mask");
    REQUIRE(Px(map.Draw(s), 95, 50) == Green);
    // A named mask that is not loaded (yet): the overlay is not drawn at all.
    s.texts["fog.mask"] = "module:fog#2";
    c = map.Draw(s);
    REQUIRE(Px(c, 5, 50) == Blue);
    REQUIRE(Px(c, 95, 50) == Blue);
}

TEST_CASE("DSMod map overlay mask_bind: manifest key", "[dsmod][runtime24][mask_bind]") {
    const auto json = nlohmann::json::parse(R"({"format":1,"pages":[{"id":"p","widgets":[
        {"type":"map","rect":[0,0,10,10],"area":"f",
         "overlays":[{"src":"fog.png","mask_bind":"fog.mask","x0":0,"y0":0,"x1":5,"y1":5},
                     {"src":"plain.png","x0":0,"y0":0,"x1":5,"y1":5}]}]}],
        "map":{"areas":{"f":{"min":[0,0],"max":[100,100]}}}})");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    const auto& mx = m.pages[0].widgets[0].map_extras;
    REQUIRE(mx != nullptr);
    REQUIRE(mx->overlays.size() == 2);
    REQUIRE(mx->overlays[0].mask_bind == "fog.mask");
    REQUIRE(mx->overlays[1].mask_bind.empty());
}
