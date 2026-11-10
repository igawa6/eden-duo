// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo runtime 24: module texture readback on the Vulkan rasterizer
// (video_core/dsmod/texture_readback.h). A request runs on the GPU thread: it picks the resident
// image, records one image-to-buffer copy into a deferred staging buffer and returns. Nothing
// waits for the GPU: the copy lands with the frame's own submission, and TickFrame hands the
// pixels over once the scheduler reports that tick as finished. The image is only read; the
// guest's memory and the texture cache's state are left as they were.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/texture_cache/texture_cache_base.h"

namespace Vulkan {
namespace {
using VideoCore::DSMod::ReadbackCandidate;
using VideoCore::DSMod::ReadbackFormat;
using VideoCore::DSMod::ReadbackResult;
using VideoCore::DSMod::ReadbackStatus;

enum class HostOrder { Unknown, Red, Rgba, Bgra };

/// How the host image stores its texels, from the Vulkan format the texture cache created it
/// with. Only layouts whose bytes are the plain 8-bit channels qualify.
HostOrder HostByteOrder(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return HostOrder::Red;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return HostOrder::Rgba;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return HostOrder::Bgra;
    default:
        return HostOrder::Unknown;
    }
}
/// EDEN_DSMOD_TEXTURE_READBACK_LOG=1 (dev-tools builds): GPU-thread time spent per readback,
/// logged every 10 s -- picking + recording the copy, and handing the landed pixels over.
struct ReadbackTiming {
    bool enabled = false;
    u64 requests = 0, completions = 0;
    double record_us = 0, record_max_us = 0, complete_us = 0, complete_max_us = 0;
    std::chrono::steady_clock::time_point next_log{};
    ReadbackTiming() {
        if constexpr (Common::DSMod::DevToolsEnabled) {
            const char* env = Common::DSMod::DevEnvironment("EDEN_DSMOD_TEXTURE_READBACK_LOG");
            enabled = env != nullptr && std::strcmp(env, "0") != 0;
        }
    }
    void Add(bool record, std::chrono::steady_clock::time_point start) {
        if (!enabled) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(now - start).count();
        if (record) {
            ++requests;
            record_us += us;
            record_max_us = std::max(record_max_us, us);
        } else {
            ++completions;
            complete_us += us;
            complete_max_us = std::max(complete_max_us, us);
        }
        if (now >= next_log) {
            next_log = now + std::chrono::seconds{10};
            LOG_INFO(Render_Vulkan,
                     "DSMod texture readback GPU-thread cost: {} requests avg {:.1f} us max {:.1f} "
                     "us, {} completions avg {:.1f} us max {:.1f} us",
                     requests, requests ? record_us / requests : 0.0, record_max_us, completions,
                     completions ? complete_us / completions : 0.0, complete_max_us);
            *this = ReadbackTiming{};
            next_log = now + std::chrono::seconds{10};
        }
    }
};
ReadbackTiming& Timing() {
    static ReadbackTiming timing;
    return timing;
}
} // namespace

bool RasterizerVulkan::DSModReadbackTexture(const VideoCore::DSMod::ReadbackRequest& request,
                                            VideoCore::DSMod::ReadbackCallback& callback) {
    DSModCompleteTextureReadbacks();
    const auto started = std::chrono::steady_clock::now();
    ReadbackResult result;
    result.width = request.width;
    result.height = request.height;
    const auto fail = [&](ReadbackStatus status) {
        result.status = status;
        auto done = std::move(callback);
        done(std::move(result));
        return true;
    };
    if (!VideoCore::DSMod::ReadbackRequestInBounds(request.width, request.height,
                                                   request.format)) {
        return fail(ReadbackStatus::Mismatch);
    }

    std::scoped_lock lock{texture_cache.mutex};
    std::vector<ReadbackCandidate> candidates;
    std::vector<VideoCommon::ImageId> ids;
    texture_cache.DSModForEachImage([&](VideoCommon::ImageId id, const Image& image) {
        const bool at_address = request.gpu_addr != 0 && image.gpu_addr == request.gpu_addr;
        const bool same_size = image.info.size.width == request.width &&
                               image.info.size.height == request.height;
        if (request.gpu_addr != 0 ? !at_address : !same_size) {
            return; // cheap pre-filter; PickReadbackImage applies the full rule
        }
        using VideoCommon::ImageFlagBits;
        const auto type = image.info.type;
        const bool plain_2d =
            (type == VideoCommon::ImageType::e2D || type == VideoCommon::ImageType::Linear) &&
            image.info.num_samples == 1 && image.info.resources.layers == 1 &&
            image.info.resources.levels >= 1 && image.info.size.depth <= 1 &&
            True(image.flags & ImageFlagBits::Registered) &&
            False(image.flags & (ImageFlagBits::Converted | ImageFlagBits::Sparse |
                                 ImageFlagBits::IsDecoding));
        candidates.push_back({
            .gpu_addr = image.gpu_addr,
            .width = image.info.size.width,
            .height = image.info.size.height,
            .format = image.info.format,
            .plain_2d = plain_2d,
            .current = False(image.flags & ImageFlagBits::CpuModified),
            .generation = image.modification_tick,
        });
        ids.push_back(id);
    });
    const auto pick = VideoCore::DSMod::PickReadbackImage(request, candidates);
    result.candidates = pick.candidates;
    if (pick.status != ReadbackStatus::Ready) {
        DSModLogReadbackMiss(request, static_cast<int>(pick.status));
        return fail(pick.status);
    }
    Image& image = texture_cache.GetImage(ids[pick.index]);
    const HostOrder order = HostByteOrder(
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, image.info.format).format);
    const bool r8 = request.format == ReadbackFormat::R8;
    if (order == HostOrder::Unknown || (order == HostOrder::Red) != r8) {
        return fail(ReadbackStatus::Unsupported);
    }
    const u32 w = request.width;
    const u32 h = request.height;
    const size_t row_bytes = size_t{w} * VideoCore::DSMod::BytesPerPixel(request.format);
    const size_t bytes = row_bytes * h;
    // Deferred: the pool never hands this buffer to anyone else until FreeDeferred.
    StagingBufferRef staging = staging_pool.Request(bytes, MemoryUsage::Download, true);
    const VideoCommon::BufferImageCopy copy{
        .buffer_offset = 0,
        .buffer_size = bytes,
        .buffer_row_length = w,
        .buffer_image_height = h,
        .image_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
        .image_offset = {0, 0, 0},
        .image_extent = {w, h, 1},
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    image.DownloadMemory(staging, std::span{&copy, 1});

    result.status = ReadbackStatus::Ready;
    result.gpu_addr = image.gpu_addr;
    result.generation = image.modification_tick;
    dsmod_readbacks.push_back(DSModPendingReadback{
        .staging = staging,
        .tick = scheduler.CurrentTick(),
        .row_bytes = row_bytes,
        .guest_format = image.info.format,
        .format = request.format,
        .result = std::move(result),
        .callback = std::move(callback),
    });
    Timing().Add(true, started);
    return true;
}

// Developer aid (EDEN_DSMOD_TEXTURE_READBACK_LOG=1, dev-tools builds only): when a readback
// finds nothing, list the resident images that share a side or the R8 format with the request,
// at most every 5 s, so a module author can see what the game really allocated.
void RasterizerVulkan::DSModLogReadbackMiss(const VideoCore::DSMod::ReadbackRequest& request,
                                            int status) {
    if constexpr (!Common::DSMod::DevToolsEnabled) {
        return;
    }
    static const char* const env = Common::DSMod::DevEnvironment("EDEN_DSMOD_TEXTURE_READBACK_LOG");
    if (env == nullptr || std::strcmp(env, "0") == 0) {
        return;
    }
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::seconds{5};
    std::string list;
    size_t shown = 0, total = 0;
    texture_cache.DSModForEachImage([&](VideoCommon::ImageId, const Image& image) {
        ++total;
        const auto& info = image.info;
        const bool related = info.size.width == request.width || info.size.height == request.height ||
                             info.format == VideoCore::Surface::PixelFormat::R8_UNORM ||
                             (request.gpu_addr != 0 && image.gpu_addr == request.gpu_addr);
        if (!related || shown >= 32) {
            return;
        }
        ++shown;
        list += std::format(" [{:#x} {}x{}x{} fmt={} type={} lv={} ly={} s={} flags={:#x} mod={}]",
                            image.gpu_addr, info.size.width, info.size.height, info.size.depth,
                            static_cast<u32>(info.format), static_cast<u32>(info.type),
                            info.resources.levels, info.resources.layers, info.num_samples,
                            static_cast<u32>(image.flags), image.modification_tick);
    });
    LOG_INFO(Render_Vulkan, "DSMod texture readback miss ({}) for {:#x} {}x{} fmt {}: {} related of {} images:{}",
             status, request.gpu_addr, request.width, request.height,
             static_cast<u32>(request.format), shown, total, list);
}

void RasterizerVulkan::DSModCompleteTextureReadbacks() {
    for (size_t i = 0; i < dsmod_readbacks.size();) {
        auto& pending = dsmod_readbacks[i];
        if (!scheduler.IsFree(pending.tick)) {
            ++i;
            continue;
        }
        const auto started = std::chrono::steady_clock::now();
        ReadbackResult result = std::move(pending.result);
        const bool bgra = HostByteOrder(MaxwellToVK::SurfaceFormat(device, FormatType::Optimal,
                                                                   true, pending.guest_format)
                                            .format) == HostOrder::Bgra;
        if (!VideoCore::DSMod::PackReadbackPixels(pending.format, bgra, result.width,
                                                  result.height, pending.staging.mapped_span,
                                                  pending.row_bytes, result.pixels)) {
            result.status = ReadbackStatus::Failed;
            result.pixels.clear();
        }
        staging_pool.FreeDeferred(pending.staging);
        auto callback = std::move(pending.callback);
        dsmod_readbacks.erase(dsmod_readbacks.begin() + static_cast<std::ptrdiff_t>(i));
        Timing().Add(false, started);
        try {
            callback(std::move(result));
        } catch (...) {
            LOG_ERROR(Render_Vulkan, "DSMod texture readback: completion callback threw");
        }
    }
}

} // namespace Vulkan
