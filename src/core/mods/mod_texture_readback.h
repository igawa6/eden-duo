// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 24: the module side of a guest texture readback (dsmod_module_extensions.h section 6).
// TextureReadbackBroker owns one module's tickets and enforces the extension's limits (formats,
// sizes, in-flight count, rate, timeout); a TextureReadbackBackend does the GPU work
// (MakeGpuTextureReadbackBackend: the renderer, video_core/dsmod/texture_readback.h). The split
// keeps every limit and fail-closed path testable with a fake backend.

#pragma once

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "common/common_types.h"
#include "core/mods/dsmod_module_extensions.h"
#include "video_core/dsmod/texture_readback.h"

namespace Tegra {
class GPU;
}

namespace Core::Mods {

class TextureReadbackBackend {
public:
    virtual ~TextureReadbackBackend() = default;
    /// Whether Submit can work at all on this host right now.
    [[nodiscard]] virtual bool Available() const = 0;
    /// Starts one readback without blocking. True: `callback` runs exactly once later (any
    /// thread), or never if the renderer goes away first. False: not started, never called.
    virtual bool Submit(const VideoCore::DSMod::ReadbackRequest& request,
                        VideoCore::DSMod::ReadbackCallback callback) = 0;
};

/// The renderer behind `gpu` (asynchronous GPU + a renderer with DSModSupportsTextureReadback).
[[nodiscard]] std::unique_ptr<TextureReadbackBackend> MakeGpuTextureReadbackBackend(
    Tegra::GPU& gpu);

class TextureReadbackBroker {
public:
    using Clock = std::chrono::steady_clock;

    explicit TextureReadbackBroker(std::unique_ptr<TextureReadbackBackend> backend);
    /// Late completions after this are dropped (callbacks hold only a weak reference).
    ~TextureReadbackBroker();

    TextureReadbackBroker(const TextureReadbackBroker&) = delete;
    TextureReadbackBroker& operator=(const TextureReadbackBroker&) = delete;

    /// EdenDsmodHostTextureApi::request.
    u64 Request(const EdenDsmodTextureRequest* request, s32* status, Clock::time_point now);
    /// EdenDsmodHostTextureApi::poll.
    s32 Poll(u64 ticket, void* output, size_t output_size, EdenDsmodTextureInfo* info,
             Clock::time_point now);
    /// EdenDsmodHostTextureApi::cancel.
    void Cancel(u64 ticket);

    /// Open tickets (pending or finished but not yet polled).
    [[nodiscard]] size_t Open() const;

private:
    struct Ticket {
        Clock::time_point submitted{};
        u32 format{};
        bool done{};
        VideoCore::DSMod::ReadbackResult result;
    };
    struct State {
        mutable std::mutex mutex;
        std::unordered_map<u64, Ticket> tickets;
    };

    std::shared_ptr<State> state;
    std::unique_ptr<TextureReadbackBackend> backend;
    std::deque<Clock::time_point> accepted; ///< accept times inside the last second
    u64 next_ticket{1};
    std::mutex request_mutex; ///< serialises Request (rate window, ticket numbers)
};

} // namespace Core::Mods
