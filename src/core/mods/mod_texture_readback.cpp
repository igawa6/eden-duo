// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <utility>

#include "common/logging.h"
#include "core/mods/mod_texture_readback.h"
#include "video_core/gpu.h"

namespace Core::Mods {
namespace {
using VideoCore::DSMod::ReadbackFormat;
using VideoCore::DSMod::ReadbackStatus;

// The renderer's statuses are the ABI's numbers, so they pass straight through.
static_assert(static_cast<s32>(ReadbackStatus::Ready) == EDEN_DSMOD_TEXTURE_READY);
static_assert(static_cast<s32>(ReadbackStatus::NotFound) == EDEN_DSMOD_TEXTURE_NOT_FOUND);
static_assert(static_cast<s32>(ReadbackStatus::Mismatch) == EDEN_DSMOD_TEXTURE_MISMATCH);
static_assert(static_cast<s32>(ReadbackStatus::Unsupported) == EDEN_DSMOD_TEXTURE_UNSUPPORTED);
static_assert(static_cast<s32>(ReadbackStatus::Failed) == EDEN_DSMOD_TEXTURE_FAILED);
static_assert(EDEN_DSMOD_TEXTURE_MAX_SIDE == VideoCore::DSMod::ReadbackMaxSide);
static_assert(EDEN_DSMOD_TEXTURE_MAX_BYTES == VideoCore::DSMod::ReadbackMaxBytes);
static_assert(EDEN_DSMOD_TEXTURE_R8 == static_cast<u32>(ReadbackFormat::R8));
static_assert(EDEN_DSMOD_TEXTURE_RGBA8 == static_cast<u32>(ReadbackFormat::RGBA8));
static_assert(EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET ==
              VideoCore::DSMod::ReadbackRequireRenderTarget);
static_assert(EDEN_DSMOD_TEXTURE_REQUIRE_SRGB == VideoCore::DSMod::ReadbackRequireSrgb);
static_assert(EDEN_DSMOD_TEXTURE_REQUIRE_UNORM == VideoCore::DSMod::ReadbackRequireUnorm);
static_assert(EDEN_DSMOD_TEXTURE_REQUIRE_REDRAWN == VideoCore::DSMod::ReadbackRequireRedrawn);

constexpr auto Timeout = std::chrono::milliseconds{EDEN_DSMOD_TEXTURE_TIMEOUT_MS};
constexpr auto RateWindow = std::chrono::seconds{1};

class GpuBackend final : public TextureReadbackBackend {
public:
    explicit GpuBackend(Tegra::GPU& gpu_) : gpu{gpu_} {}
    bool Available() const override {
        return gpu.DSModTextureReadbackAvailable();
    }
    bool Submit(const VideoCore::DSMod::ReadbackRequest& request,
                VideoCore::DSMod::ReadbackCallback callback) override {
        return gpu.DSModRequestTextureReadback(request, std::move(callback));
    }

private:
    Tegra::GPU& gpu;
};
} // namespace

std::unique_ptr<TextureReadbackBackend> MakeGpuTextureReadbackBackend(Tegra::GPU& gpu) {
    return std::make_unique<GpuBackend>(gpu);
}

TextureReadbackBroker::TextureReadbackBroker(std::unique_ptr<TextureReadbackBackend> backend_)
    : state{std::make_shared<State>()}, backend{std::move(backend_)} {}

TextureReadbackBroker::~TextureReadbackBroker() = default;

u64 TextureReadbackBroker::Request(const EdenDsmodTextureRequest* request, s32* status,
                                   Clock::time_point now) {
    s32 ignored{};
    s32& out = status ? *status : ignored;
    if (!request || request->struct_size != sizeof(EdenDsmodTextureRequest) ||
        !VideoCore::DSMod::ValidReadbackFormat(request->format) ||
        !VideoCore::DSMod::ValidReadbackFlags(request->flags,
                                              static_cast<ReadbackFormat>(request->format)) ||
        request->reserved != 0 ||
        !VideoCore::DSMod::ReadbackRequestInBounds(request->width, request->height,
                                                   static_cast<ReadbackFormat>(request->format))) {
        out = EDEN_DSMOD_TEXTURE_INVALID;
        return 0;
    }
    if (!backend || !backend->Available()) {
        out = EDEN_DSMOD_TEXTURE_UNSUPPORTED;
        return 0;
    }
    std::scoped_lock request_lock{request_mutex};
    {
        std::scoped_lock lock{state->mutex};
        if (state->tickets.size() >= EDEN_DSMOD_TEXTURE_MAX_IN_FLIGHT) {
            out = EDEN_DSMOD_TEXTURE_BUSY;
            return 0;
        }
    }
    while (!accepted.empty() && now - accepted.front() >= RateWindow) {
        accepted.pop_front();
    }
    if (accepted.size() >= EDEN_DSMOD_TEXTURE_MAX_PER_SECOND) {
        out = EDEN_DSMOD_TEXTURE_RATE_LIMITED;
        return 0;
    }
    const u64 ticket = next_ticket++;
    {
        std::scoped_lock lock{state->mutex};
        state->tickets.emplace(ticket, Ticket{.submitted = now, .format = request->format});
    }
    const VideoCore::DSMod::ReadbackRequest gpu_request{
        .gpu_addr = request->gpu_address,
        .width = request->width,
        .height = request->height,
        .format = static_cast<ReadbackFormat>(request->format),
        .flags = request->flags,
    };
    // The completion may run on the GPU thread long after this broker is gone (or, for a
    // synchronous backend, before Submit returns): it only ever touches the shared state.
    std::weak_ptr<State> weak = state;
    bool submitted = false;
    try {
        submitted =
            backend->Submit(gpu_request, [weak, ticket](VideoCore::DSMod::ReadbackResult&& result) {
                const auto shared = weak.lock();
                if (!shared) {
                    return;
                }
                std::scoped_lock lock{shared->mutex};
                const auto it = shared->tickets.find(ticket);
                if (it == shared->tickets.end()) {
                    return; // cancelled or timed out meanwhile
                }
                it->second.result = std::move(result);
                it->second.done = true;
            });
    } catch (...) {
        LOG_ERROR(Core, "DSMod texture readback: the renderer threw while queueing");
    }
    if (!submitted) {
        std::scoped_lock lock{state->mutex};
        state->tickets.erase(ticket);
        out = EDEN_DSMOD_TEXTURE_UNSUPPORTED;
        return 0;
    }
    accepted.push_back(now);
    out = EDEN_DSMOD_TEXTURE_PENDING;
    return ticket;
}

s32 TextureReadbackBroker::Poll(u64 ticket, void* output, size_t output_size,
                                EdenDsmodTextureInfo* info, Clock::time_point now) {
    if (info && info->struct_size != sizeof(EdenDsmodTextureInfo)) {
        return EDEN_DSMOD_TEXTURE_INVALID;
    }
    std::scoped_lock lock{state->mutex};
    const auto it = state->tickets.find(ticket);
    if (it == state->tickets.end()) {
        if (info) {
            *info = {.struct_size = sizeof(EdenDsmodTextureInfo),
                     .status = EDEN_DSMOD_TEXTURE_UNKNOWN_TICKET};
        }
        return EDEN_DSMOD_TEXTURE_UNKNOWN_TICKET;
    }
    Ticket& entry = it->second;
    const auto fill = [&](s32 status) {
        if (info) {
            const auto& r = entry.result;
            *info = {
                .struct_size = sizeof(EdenDsmodTextureInfo),
                .status = status,
                .gpu_address = r.gpu_addr,
                .width = r.width,
                .height = r.height,
                .format = entry.format,
                .candidates = r.candidates,
                .generation = r.generation,
                .size = status == EDEN_DSMOD_TEXTURE_READY || status == EDEN_DSMOD_TEXTURE_INVALID
                            ? r.pixels.size()
                            : 0,
            };
        }
        return status;
    };
    if (!entry.done) {
        if (now - entry.submitted < Timeout) {
            return fill(EDEN_DSMOD_TEXTURE_PENDING);
        }
        state->tickets.erase(it); // a late completion finds no ticket and is dropped
        if (info) {
            *info = {.struct_size = sizeof(EdenDsmodTextureInfo),
                     .status = EDEN_DSMOD_TEXTURE_FAILED};
        }
        return EDEN_DSMOD_TEXTURE_FAILED;
    }
    const auto status = static_cast<s32>(entry.result.status);
    if (status != EDEN_DSMOD_TEXTURE_READY) {
        const s32 result = fill(status);
        state->tickets.erase(it);
        return result;
    }
    const auto& pixels = entry.result.pixels;
    if (!output || output_size < pixels.size()) {
        return fill(EDEN_DSMOD_TEXTURE_INVALID); // the ticket stays: poll again with room
    }
    std::memcpy(output, pixels.data(), pixels.size());
    const s32 result = fill(EDEN_DSMOD_TEXTURE_READY);
    state->tickets.erase(it);
    return result;
}

void TextureReadbackBroker::Cancel(u64 ticket) {
    std::scoped_lock lock{state->mutex};
    state->tickets.erase(ticket);
}

size_t TextureReadbackBroker::Open() const {
    std::scoped_lock lock{state->mutex};
    return state->tickets.size();
}

} // namespace Core::Mods
