// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo developer probes on the Vulkan rasterizer: route one render target to the second
// screen (EDEN_DSMOD_RT_PICK) and log the render-target pass sequence (EDEN_DSMOD_RTLOG).
// Called once per draw from RasterizerVulkan::PrepareDraw.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "video_core/gpu.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/texture_cache_base.h"

namespace Vulkan {

void RasterizerVulkan::DSModCaptureRenderTarget(VideoCommon::ImageViewId view_id) {
    if constexpr (!Common::DSMod::DevToolsEnabled) {
        return;
    }
    if (!view_id) {
        return;
    }
    auto& view = texture_cache.GetImageView(view_id);
    const u32 w = view.size.width;
    const u32 h = view.size.height;
    if (w == 0 || h == 0 || VideoCore::Surface::BytesPerBlock(view.format) != 4) {
        static bool once = false;
        if (!once) {
            once = true;
            LOG_WARNING(Render_Vulkan, "DSMod rt pick: {}x{} format {} is not a 32-bit colour "
                                       "target; nothing to show",
                        w, h, static_cast<int>(view.format));
        }
        return;
    }
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    const auto staging = staging_pool.Request(bytes, MemoryUsage::Download);
    const VideoCommon::BufferImageCopy copy{
        .buffer_offset = staging.offset,
        .buffer_size = bytes,
        .buffer_row_length = w,
        .buffer_image_height = h,
        .image_subresource = {.base_level = 0, .base_layer = 0, .num_layers = 1},
        .image_offset = {0, 0, 0},
        .image_extent = {w, h, 1},
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    texture_cache.GetImage(view.image_id).DownloadMemory(staging, std::span{&copy, 1});
    // The readback is only useful once the GPU has actually written it, and this runs a few
    // times a second rather than every frame, so paying for a full wait here is acceptable.
    scheduler.Finish();
    std::vector<u32> pixels(static_cast<size_t>(w) * h);
    std::memcpy(pixels.data(), staging.mapped_span.data(), bytes);
    gpu.DSModAux().PublishUi(w, h, pixels, /*require_context=*/true);
    static bool first = true;
    if (first) {
        first = false;
        LOG_INFO(Render_Vulkan, "DSMod rt pick: captured {}x{} from the chosen pass", w, h);
    }
}

// Per-call-site probe state. PrepareDraw is a template instantiated once per draw kind (direct,
// indirect), and these used to be function-local statics inside it, so each draw kind keeps its
// own copy -- PrepareDraw hands in its own (static) pointer, allocated here on first use.
struct RasterizerVulkan::DSModProbeState {
    u32 want_w = 0, want_h = 0, want_k = 0;
    bool parsed = false;
    VkFramebuffer last_fb = VK_NULL_HANDLE;
    u32 matches = 0;
    bool armed = true;
    std::chrono::steady_clock::time_point next_arm{};
    struct Segment {
        VkFramebuffer handle;
        u32 w, h;
        u64 draws;
    };
    std::vector<Segment> rt_seq;
    u64 rt_draws = 0;
};

void RasterizerVulkan::DSModProbeRenderTargets(DSModProbeState*& state) {
    if (state == nullptr) {
        state = new DSModProbeState{}; // lives as long as the static it replaces
    }
    auto& [want_w, want_h, want_k, parsed, last_fb, matches, armed, next_arm, rt_seq, rt_draws] =
        *state;
    // Route one render target to the second screen. The compositor only ever sees a single
    // presented surface, but the frame underneath is built from a dozen targets -- and a game
    // that draws its UI into its own target can have that target intercepted here, which is the
    // one place it exists separately from the scene.
    // EDEN_DSMOD_RT_PICK=<w>x<h>[:k] -- the k-th pass at that size.
    static const char* const pick_env = Common::DSMod::DevEnvironment("EDEN_DSMOD_RT_PICK");
    if (pick_env != nullptr &&
        gpu.DSModAux().companion_context_active.load(std::memory_order_acquire)) {
        if (!parsed) {
            parsed = true;
            if (std::sscanf(pick_env, "%ux%u:%u", &want_w, &want_h, &want_k) < 2) {
                LOG_WARNING(Render_Vulkan, "DSMod rt pick: cannot read '{}'", pick_env);
            }
            LOG_INFO(Render_Vulkan, "DSMod rt pick: {}x{} pass #{}", want_w, want_h, want_k);
            gpu.DSModAux().rt_capture.store(true);
        }
        if (const Framebuffer* const fb = texture_cache.GetFramebuffer();
            fb != nullptr && want_w != 0) {
            if (fb->Handle() != last_fb) {
                last_fb = fb->Handle();
                const auto area = fb->RenderArea();
                if (rt_pick_pending) {
                    // The selected pass has just ended, so its target now holds the finished
                    // picture. Read it back before anything else draws into it.
                    DSModCaptureRenderTarget(rt_pick_view);
                    rt_pick_pending = false;
                    armed = false;
                    matches = 0;
                    next_arm = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                } else if (armed && area.width == want_w && area.height == want_h) {
                    if (matches++ == want_k) {
                        rt_pick_view = texture_cache.GetColorBufferId(0);
                        rt_pick_pending = rt_pick_view != VideoCommon::ImageViewId{};
                    }
                } else if (!armed && std::chrono::steady_clock::now() >= next_arm) {
                    armed = true;  // capture a few times a second, not every frame
                    matches = 0;
                }
            }
        }
    }

    // Which render targets does this game actually draw into, and how many draws each?
    // A single presented layer says nothing about the pass structure underneath it: if the UI
    // goes into its own target, it can be intercepted there even though the compositor only ever
    // sees one surface. EDEN_DSMOD_RTLOG=1 to find out.
    static const bool rt_log = Common::DSMod::DevEnvironment("EDEN_DSMOD_RTLOG") != nullptr;
    if (rt_log) {
        // Order matters more than totals: UI is drawn last, into whichever target is presented.
        // Record the passes as a sequence, collapsing consecutive draws into the same target.
        if (const Framebuffer* const fb = texture_cache.GetFramebuffer(); fb != nullptr) {
            const auto area = fb->RenderArea();
            if (rt_seq.empty() || rt_seq.back().handle != fb->Handle()) {
                rt_seq.push_back({fb->Handle(), area.width, area.height, 1});
            } else {
                ++rt_seq.back().draws;
            }
            ++rt_draws;
        }
        if (rt_draws >= 3000) {
            std::string report;
            for (size_t i = 0; i < rt_seq.size() && i < 40; ++i) {
                report += std::format(" {}:{}x{}/{}", static_cast<void*>(rt_seq[i].handle),
                                      rt_seq[i].w, rt_seq[i].h, rt_seq[i].draws);
            }
            LOG_INFO(Render_Vulkan, "DSMod rt seq ({} passes, {} draws):{}", rt_seq.size(),
                     rt_draws, report);
            rt_seq.clear();
            rt_draws = 0;
        }
    }
}

} // namespace Vulkan
