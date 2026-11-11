// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo: Vulkan aux ("second screen") presenter. See vk_dsmod_aux.h.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>
#include <vector>

#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "video_core/gpu.h"
#include "video_core/host_shaders/vulkan_dsmod_map_fade_frag_spv.h"
#include "video_core/host_shaders/vulkan_dsmod_quad_frag_spv.h"
#include "video_core/host_shaders/vulkan_dsmod_quad_vert_spv.h"
#include "video_core/present.h"
#include "video_core/renderer_vulkan/present/present_push_constants.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_dsmod_aux.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"

namespace Vulkan {
namespace {

bool DsmodProfilingEnabled() {
    static const bool enabled = [] {
        const char* value = Common::DSMod::DevEnvironment("EDEN_DSMOD_PROFILE");
        return Common::DSMod::DevToolsEnabled &&
               (!value || (std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
                           std::strcmp(value, "FALSE") != 0));
    }();
    return enabled;
}

void RecordAuxUploadProfile(size_t bytes, std::chrono::steady_clock::duration cpu,
                            std::chrono::steady_clock::duration wait) {
    struct Counters {
        std::chrono::steady_clock::time_point report_at = std::chrono::steady_clock::now();
        u64 count{};
        u64 bytes{};
        std::chrono::nanoseconds cpu{};
        std::chrono::nanoseconds wait{};
        std::chrono::nanoseconds max_cpu{};
        std::chrono::nanoseconds max_wait{};
    };
    static Counters counters;
    const auto cpu_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(cpu);
    const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wait);
    ++counters.count;
    counters.bytes += bytes;
    counters.cpu += cpu_ns;
    counters.wait += wait_ns;
    counters.max_cpu = std::max(counters.max_cpu, cpu_ns);
    counters.max_wait = std::max(counters.max_wait, wait_ns);
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.report_at < std::chrono::seconds{5}) {
        return;
    }
    const double count = static_cast<double>(counters.count);
    // CPU staging/recording time only. This deliberately adds no GPU timestamp, readback, or wait.
    LOG_INFO(Render_Vulkan,
             "DSMod upload profile (CPU, 5s): count={} bytes={} avg={:.1f}us max={:.1f}us; "
             "scheduler-wait total={:.1f}us avg={:.1f}us max={:.1f}us",
             counters.count, counters.bytes,
             std::chrono::duration<double, std::micro>{counters.cpu}.count() / count,
             std::chrono::duration<double, std::micro>{counters.max_cpu}.count(),
             std::chrono::duration<double, std::micro>{counters.wait}.count(),
             std::chrono::duration<double, std::micro>{counters.wait}.count() / count,
             std::chrono::duration<double, std::micro>{counters.max_wait}.count());
    counters = Counters{.report_at = now};
}

// Push block of the DSMod quad pipeline (vulkan_dsmod_quad.vert): clip = pos * scale + offset,
// plus a per-quad alpha and RGB tint. 96 bytes, inside the 128-byte push-constant minimum.
struct AuxQuadPushConstants {
    std::array<f32, 4> scale_offset;
    std::array<ScreenRectVertex, 4> vertices;
    f32 alpha;
    std::array<f32, 3> tint;
};
static_assert(sizeof(AuxQuadPushConstants) == 96);

// `preserve`: the copy covers only part of the image, so the image must keep its other
// texels -- the pre-copy barrier then transitions from the GENERAL layout every aux image rests in
// between uploads/samples/blits, instead of UNDEFINED. UNDEFINED lets the driver discard the old
// contents (with framebuffer/UBWC compression on Adreno it may do exactly that), which a whole-image
// upload is free to allow but a sub-rect/tile upload must not.
template <typename Fill>
void UploadRegionsAsync(Scheduler& scheduler, MemoryAllocator& allocator, vk::Buffer& staging,
                        size_t& staging_bytes, u64& tick, VkImage image,
                        std::vector<VkBufferImageCopy> regions, size_t total_bytes, bool preserve,
                        Fill&& fill) {
    if (regions.empty() || total_bytes == 0) {
        return;
    }
    const bool profile = DsmodProfilingEnabled();
    const auto cpu_start = profile ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    const auto wait_start = cpu_start;
    scheduler.Wait(tick);
    const auto wait_end = profile ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    if (staging_bytes < total_bytes) {
        staging = CreateWrappedBuffer(allocator, total_bytes, MemoryUsage::Upload);
        staging_bytes = total_bytes;
    }
    fill(staging.Mapped().data());
    staging.Flush();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buf = *staging, image, regions = std::move(regions),
                      preserve](vk::CommandBuffer cmdbuf) {
        constexpr VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const VkImageMemoryBarrier pre{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             (preserve ? VK_ACCESS_TRANSFER_WRITE_BIT : VkAccessFlags{0}),
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = preserve ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, pre);
        cmdbuf.CopyBufferToImage(buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, regions);
        const VkImageMemoryBarrier post{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, post);
    });
    tick = scheduler.CurrentTick();
    if (profile) {
        RecordAuxUploadProfile(total_bytes, std::chrono::steady_clock::now() - cpu_start,
                               wait_end - wait_start);
    }
}

VkBufferImageCopy ColorRegion(VkDeviceSize offset, s32 x, s32 y, u32 w, u32 h) {
    return VkBufferImageCopy{
        .bufferOffset = offset,
        .bufferRowLength = w,
        .bufferImageHeight = h,
        .imageSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                          .mipLevel = 0,
                          .baseArrayLayer = 0,
                          .layerCount = 1},
        .imageOffset{.x = x, .y = y, .z = 0},
        .imageExtent{.width = w, .height = h, .depth = 1},
    };
}

// Async image upload through a persistent staging buffer -- no scheduler.Finish(). `tick` fences
// reuse: the previous copy out of this buffer must have retired before it is overwritten (Wait
// returns at once when it has, which at <=30 Hz republish is nearly always). The barriers name the
// TRANSFER stage explicitly: TransitionImageLayout's graphics-only masks neither order the copy
// after the previous frame's sampling nor make its write visible to the shader, and were only ever
// correct behind the full drain UploadImage() does.
//
// `dst_offset` places `bytes` -- always exactly
// `extent.width * extent.height` pixels, tightly packed, whether that's a full canvas or a compact
// dirty sub-rect the caller already extracted -- at that offset in `image` instead of always {0,0}.
// `preserve` must be true for a sub-rect (see UploadRegionsAsync).
void UploadImageAsync(Scheduler& scheduler, MemoryAllocator& allocator, vk::Buffer& staging,
                      size_t& staging_bytes, u64& tick, VkImage image, VkExtent2D extent,
                      std::span<const u8> bytes, VkOffset2D dst_offset = {0, 0},
                      bool preserve = false) {
    UploadRegionsAsync(scheduler, allocator, staging, staging_bytes, tick, image,
                       {ColorRegion(0, dst_offset.x, dst_offset.y, extent.width, extent.height)},
                       bytes.size(), preserve,
                       [&](u8* dst) { std::memcpy(dst, bytes.data(), bytes.size()); });
}

// Uploads the tiles `mask` marks (or the whole image when `full`/mask.all), straight
// from the routing buffer into staging -- one region per horizontal run of dirty tiles, merged
// downwards while a run repeats, packed back to back. Past ~70% of the image a single full region
// is cheaper to record and no larger to copy. Returns the bytes staged (0 = nothing to do).
size_t UploadTilesAsync(Scheduler& scheduler, MemoryAllocator& allocator, vk::Buffer& staging,
                        size_t& staging_bytes, u64& tick, VkImage image, std::span<const u32> px,
                        u32 w, u32 h, const VideoCore::DSMod::TileMask& mask, bool full,
                        std::array<s32, 4> bbox = {0, 0, 0, 0}) {
    constexpr u32 T = VideoCore::DSMod::TileMask::Size;
    const size_t image_px = static_cast<size_t>(w) * h;
    const bool mask_usable = mask.cols == (w + T - 1) / T && mask.rows == (h + T - 1) / T &&
                             mask.bits.size() == static_cast<size_t>(mask.cols) * mask.rows;
    std::vector<VkBufferImageCopy> regions;
    size_t dirty_px = 0;
    if (!full && !mask.all && mask_usable) {
        for (u32 r = 0; r < mask.rows; ++r) {
            const u8* const row = mask.bits.data() + static_cast<size_t>(r) * mask.cols;
            const s32 y = static_cast<s32>(r * T);
            const u32 rh = std::min(T, h - r * T);
            for (u32 c = 0; c < mask.cols;) {
                if (row[c] == 0) {
                    ++c;
                    continue;
                }
                const u32 start = c;
                while (c < mask.cols && row[c] != 0) {
                    ++c;
                }
                const s32 x = static_cast<s32>(start * T);
                const u32 rw = std::min(c * T, w) - start * T;
                dirty_px += static_cast<size_t>(rw) * rh;
                const auto below = std::ranges::find_if(regions, [&](const VkBufferImageCopy& g) {
                    return g.imageOffset.x == x && g.imageExtent.width == rw &&
                           g.imageOffset.y + static_cast<s32>(g.imageExtent.height) == y;
                });
                if (below != regions.end()) {
                    below->imageExtent.height += rh;
                    below->bufferImageHeight = below->imageExtent.height;
                } else {
                    regions.push_back(ColorRegion(0, x, y, rw, rh));
                }
            }
        }
        // Trim each run to the exact changed bounding box (when the producer supplied one).
        if (bbox[2] > 0 && bbox[3] > 0) {
            const s32 bx1 = bbox[0] + bbox[2], by1 = bbox[1] + bbox[3];
            dirty_px = 0;
            std::erase_if(regions, [&](VkBufferImageCopy& g) {
                const s32 x0 = std::max(g.imageOffset.x, bbox[0]);
                const s32 y0 = std::max(g.imageOffset.y, bbox[1]);
                const s32 x1 =
                    std::min(g.imageOffset.x + static_cast<s32>(g.imageExtent.width), bx1);
                const s32 y1 =
                    std::min(g.imageOffset.y + static_cast<s32>(g.imageExtent.height), by1);
                if (x1 <= x0 || y1 <= y0) {
                    return true;
                }
                g = ColorRegion(0, x0, y0, static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0));
                dirty_px += static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);
                return false;
            });
        }
        if (regions.empty()) {
            return 0;
        }
    }
    if (full || mask.all || !mask_usable || dirty_px * 10 > image_px * 7) {
        UploadImageAsync(scheduler, allocator, staging, staging_bytes, tick, image,
                         VkExtent2D{w, h},
                         std::span<const u8>{reinterpret_cast<const u8*>(px.data()),
                                             px.size_bytes()});
        return px.size_bytes();
    }
    VkDeviceSize offset = 0;
    for (auto& g : regions) {
        g.bufferOffset = offset;
        offset += static_cast<VkDeviceSize>(g.imageExtent.width) * g.imageExtent.height *
                  sizeof(u32);
    }
    const size_t total = static_cast<size_t>(offset);
    const auto copy_regions = regions;
    UploadRegionsAsync(scheduler, allocator, staging, staging_bytes, tick, image,
                       std::move(regions), total, /*preserve=*/true, [&](u8* dst) {
                           for (const auto& g : copy_regions) {
                               u8* out = dst + g.bufferOffset;
                               const size_t row_bytes = g.imageExtent.width * sizeof(u32);
                               for (u32 row = 0; row < g.imageExtent.height; ++row) {
                                   const size_t src = (static_cast<size_t>(g.imageOffset.y) + row) *
                                                          w +
                                                      static_cast<size_t>(g.imageOffset.x);
                                   std::memcpy(out, px.data() + src, row_bytes);
                                   out += row_bytes;
                               }
                           }
                       });
    return total;
}

} // namespace

DSModAuxPresenter::DSModAuxPresenter(Tegra::GPU& gpu_,
                                     Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                     const vk::Instance& instance_, const Device& device_,
                                     MemoryAllocator& memory_allocator_, Scheduler& scheduler_,
                                     RasterizerVulkan& rasterizer_)
    : gpu{gpu_}, device_memory{device_memory_}, instance{instance_}, device{device_},
      memory_allocator{memory_allocator_}, scheduler{scheduler_}, rasterizer{rasterizer_} {}

DSModAuxPresenter::~DSModAuxPresenter() = default;

// Duo-only PresentManager entry point (declared in vk_present_manager.h), kept here so the
// upstream file carries only the declaration.
Frame* PresentManager::TryGetRenderFrame() {
    std::scoped_lock lock{free_mutex};
    const auto ready = std::ranges::find_if(free_queue, [](const Frame* frame) {
        return frame->present_done.GetStatus() == VK_SUCCESS;
    });
    if (ready == free_queue.end()) {
        return nullptr;
    }
    Frame* const frame = *ready;
    free_queue.erase(ready);
    frame->present_done.Reset();
    return frame;
}

// RendererBase hook, defined here so renderer_vulkan.cpp only declares it.
void RendererVulkan::SetAuxWindow(Core::Frontend::EmuWindow* aux_window_) {
    dsmod_aux->SetAuxWindow(aux_window_);
}

bool DSModAuxPresenter::PrimaryPresentDisabled() {
    // Headless verification: a compositor that suspends a hidden window stops consuming
    // swapchain images, and presenting then blocks the GPU thread, which stalls the guest with
    // it. EDEN_NO_PRESENT keeps everything upstream of the present -- including screenshots --
    // running and simply never hands a frame to the window.
    static const bool no_present = Common::DSMod::DevEnvironment("EDEN_NO_PRESENT") != nullptr;
    return no_present;
}

std::span<const Tegra::FramebufferConfig> DSModAuxPresenter::Route(
    std::span<const Tegra::FramebufferConfig> framebuffers,
    const Layout::FramebufferLayout& main_window_layout) {
    // Route the bound VI layer (or mirror everything) to the aux window.
    // Report the layers a game actually presents, once. Routing a layer to the second screen is
    // the only way to *move* part of a game's own output there rather than redraw it, and it is
    // possible exactly when the game composes its interface as its own layer instead of painting
    // it into the same image as the world.
    {
        static std::set<u64> seen_layers;
        for (const auto& fb : framebuffers) {
            if (seen_layers.insert(fb.layer_id).second) {
                LOG_INFO(Render_Vulkan, "DSMod: guest presents layer {} ({}x{}), {} layer(s) now",
                         fb.layer_id, fb.width, fb.height, seen_layers.size());
            }
        }
    }
    primary_layers.clear();
    boost::container::small_vector<Tegra::FramebufferConfig, 4> aux_layers;
    std::span<const Tegra::FramebufferConfig> primary = framebuffers;
    std::scoped_lock aux_lock{aux_mutex};
    SyncAuxWindow();

    // Eden Duo Stage 1 (Simulated/inline dual screen): a real aux window always wins. Only when
    // none is attached do we consider drawing the companion into the main window's own frame.
    inline_game_rect.reset();
    if (aux_window == nullptr && gpu.DSModAux().inline_enabled.load(std::memory_order_acquire)) {
        inline_game_rect = RenderInline(main_window_layout);
    } else {
        inline_companion = CompanionQuad{};
    }

    if (aux_window != nullptr && gpu.DSModAux().gpu_composite.load() &&
        gpu.DSModAux().HasComposite() && aux_window->IsShown()) {
        // GPU-composite mod path: draw map/icons/marker/HUD as quads over cached textures.
        RenderAuxModUiGpu();
    } else if (aux_window != nullptr && gpu.DSModAux().HasUi() && aux_window->IsShown()) {
        // A dual-screen mod package owns the second screen: show its UI instead of guest
        // layers.
        RenderAuxModUi();
    } else if (aux_window != nullptr) {
        const u64 bound = gpu.DSModAux().bound_layer.load(std::memory_order_relaxed);
        if (bound != VideoCore::DSMod::AuxRouting::NoLayer) {
            for (const auto& fb : framebuffers) {
                (fb.layer_id == bound ? aux_layers : primary_layers).push_back(fb);
            }
            primary = primary_layers;
            if (!aux_layers.empty() && aux_window->IsShown()) {
                RenderAuxWindow(aux_layers);
            }
        } else if (aux_window->IsShown() && !framebuffers.empty()) {
            if (gpu.DSModAux().mirror_enabled.load()) {
                // Show only the requested corner of the guest's frame, blown up to the panel.
                // This is the game's real, animated art -- nothing is extracted or shipped.
                boost::container::small_vector<Tegra::FramebufferConfig, 4> cropped{
                    framebuffers.begin(), framebuffers.end()};
                auto& fb = cropped.front();
                const s32 base_x = fb.crop_rect.left;
                const s32 base_y = fb.crop_rect.top;
                const s32 full_w = fb.crop_rect.GetWidth() > 0 ? fb.crop_rect.GetWidth()
                                                               : static_cast<s32>(fb.width);
                const s32 full_h = fb.crop_rect.GetHeight() > 0 ? fb.crop_rect.GetHeight()
                                                                : static_cast<s32>(fb.height);
                fb.crop_rect.left =
                    base_x + static_cast<s32>(gpu.DSModAux().mirror_x.load() * full_w);
                fb.crop_rect.top =
                    base_y + static_cast<s32>(gpu.DSModAux().mirror_y.load() * full_h);
                fb.crop_rect.right =
                    fb.crop_rect.left + static_cast<s32>(gpu.DSModAux().mirror_w.load() * full_w);
                fb.crop_rect.bottom =
                    fb.crop_rect.top + static_cast<s32>(gpu.DSModAux().mirror_h.load() * full_h);
                RenderAuxWindow(cropped);
            } else {
                RenderAuxWindow(framebuffers);
            }
        }
    }
    return primary;
}

void DSModAuxPresenter::SetAuxWindow(Core::Frontend::EmuWindow* aux_window_) {
    pending_aux_window.store(aux_window_);
    aux_dirty.store(true);
    // Publish presence/size right away so guest code that queries dsm:u before the first
    // composite (the GPU-thread attach happens there) already sees the aux display.
    auto& aux = gpu.DSModAux();
    if (aux_window_ != nullptr) {
        const auto& layout = aux_window_->GetFramebufferLayout();
        aux.width.store(layout.width);
        aux.height.store(layout.height);
        aux.present.store(true);
        return;
    }
    aux.present.store(false);

    // Detach must be synchronous: the caller is about to release the native surface.
    std::unique_lock lock{aux_mutex};
    aux_cv.wait_for(lock, std::chrono::milliseconds{500}, [this] { return aux_window == nullptr; });
    if (aux_window != nullptr) {
        // GPU thread is not compositing (emulation paused/stopped); tear down from here.
        LOG_WARNING(Render_Vulkan, "DSMod aux detach: GPU thread idle, destroying from caller");
        aux_dirty.store(false);
        DestroyAuxLocked();
    }
}

void DSModAuxPresenter::DestroyAuxLocked() {
    // Recorded-but-unexecuted aux work references the frames destroyed below: let the scheduler
    // worker drain first, then the GPU. The next attach re-records from the kept composite/canvas.
    scheduler.WaitWorker();
    if (aux_present_manager) {
        aux_present_manager->WaitPresent();
    }
    void(device.GetLogical().WaitIdle());
    aux_c_presented = false;
    aux_ui_presented = false;
    blit_aux.reset();
    aux_present_manager.reset();
    // These objects depend on the surface format. Keep texture uploads and descriptors,
    // but rebuild the render passes and pipeline for the next attached swapchain.
    aux_c_pipeline = {};
    aux_c_map_pipeline = {};
    aux_c_render_pass = {};
    aux_ui_render_pass = {};
    aux_swapchain.reset();
    aux_surface.reset();
    aux_window = nullptr;
    gpu.DSModAux().present.store(false);
    LOG_INFO(Render_Vulkan, "DSMod aux window detached");
    aux_cv.notify_all();
}

void DSModAuxPresenter::SyncAuxWindow() {
    if (!aux_dirty.exchange(false)) {
        return;
    }
    Core::Frontend::EmuWindow* const wanted = pending_aux_window.load();
    if (aux_window == wanted) {
        return;
    }
    auto& aux = gpu.DSModAux();
    if (aux_window != nullptr) {
        DestroyAuxLocked();
    }
    if (wanted == nullptr) {
        return;
    }
    try {
        aux_surface.emplace(CreateSurface(instance, wanted->GetWindowInfo()));
        if (!device.GetPhysical().GetSurfaceSupportKHR(device.GetPresentFamily(), **aux_surface)) {
            LOG_ERROR(Render_Vulkan,
                      "DSMod aux surface is not supported by the present queue family");
            aux_surface.reset();
            return;
        }
        const auto& layout = wanted->GetFramebufferLayout();
        aux_swapchain.emplace(**aux_surface, device, scheduler, layout.width, layout.height,
                              /*dsmod_aux=*/true);
        aux_present_manager.emplace(instance, *wanted, device, memory_allocator, scheduler,
                                    *aux_swapchain, *aux_surface, /*allow_present_thread=*/true,
                                    /*force_present_thread=*/true);
        blit_aux.emplace(device_memory, device, memory_allocator, *aux_present_manager, scheduler,
                         PresentFiltersForDisplay);
        aux_window = wanted;
        aux.width.store(layout.width);
        aux.height.store(layout.height);
        aux.present.store(true);
        LOG_INFO(Render_Vulkan, "DSMod aux window attached ({}x{})", layout.width, layout.height);
    } catch (const vk::Exception& exception) {
        LOG_ERROR(Render_Vulkan, "DSMod aux window setup failed: {}", exception.what());
        blit_aux.reset();
        aux_present_manager.reset();
        aux_swapchain.reset();
        aux_surface.reset();
        aux_window = nullptr;
        aux.present.store(false);
    }
}

// Shared by the physical (RenderAuxModUi) and Simulated/inline (RenderInlineModUi) paths: keeps
// aux_ui_image/aux_ui_view holding the mod runtime's latest canvas, uploading only what changed.
// Take a changed canvas straight into the staging buffer under the routing lock: one copy, no
// per-publish allocation. Recreate on a size change only after its last upload and blit
// have retired. Pixels are u32 0xAARRGGBB, whose little-endian bytes are [B,G,R,A]:
// B8G8R8A8 (labelling it R8G8B8A8 swapped red and blue on the Adreno panel).
//
// `dirty` is AuxRouting's own accumulated
// union since our last successful take (see WithUiPartial's doc comment) -- everything that
// changed in `px` since `aux_ui_image` was last brought up to date, which may span more than
// one core-side publish if this renderer polled less often than they landed. Two situations
// still force a full upload rather than trusting it, mirroring AuxRouting's own
// `identity_changed` fallback one layer up:
//  - the image itself is being created or resized this call -- its prior contents (if any)
//    are the wrong shape or don't exist, so a sub-rect patch would leave the rest undefined;
//  - `dirty` is degenerate or already covers the whole canvas -- nothing to save by extracting
//    a sub-rect, and an empty/malformed rect (should not happen, but is not trusted blindly)
//    must not turn into "upload nothing".
//
// Tile diff (default): the routing buffer tracks which 64x64 tiles changed since our last take;
// upload just those, straight from the routing buffer into staging (no scratch copy). The
// bounding-box path below stays as the EDEN_DSMOD_UI_DIFF=0 fallback.
//
// Returns whether anything changed (recreated or uploaded) since the previous call.
bool DSModAuxPresenter::UpdateAuxUiImage() {
    auto& aux = gpu.DSModAux();
    const auto recreate_if_needed = [&](u32 tw, u32 th) {
        const bool recreated = !aux_ui_image || aux_ui_w != tw || aux_ui_h != th;
        if (recreated) {
            scheduler.Wait(std::max(aux_ui_upload_tick, aux_ui_blit_tick));
            aux_ui_image =
                CreateWrappedImage(memory_allocator, VkExtent2D{tw, th}, VK_FORMAT_B8G8R8A8_UNORM);
            aux_ui_view = CreateWrappedImageView(device, aux_ui_image, VK_FORMAT_B8G8R8A8_UNORM);
            aux_ui_w = tw;
            aux_ui_h = th;
        }
        return recreated;
    };
    // Developer check (EDEN_DSMOD_AUX_VERIFY): read the aux image back after every upload and
    // compare it with the routing buffer the upload came from. Costs a GPU drain per upload.
    static const bool verify_uploads = Common::DSMod::DevEnvironment("EDEN_DSMOD_AUX_VERIFY") != nullptr;
    static std::vector<u32> verify_expect;
    std::array<s32, 4> dirty{0, 0, 0, 0};
    const bool updated =
        VideoCore::DSMod::UiDiffEnabled()
            ? aux.WithUiTiles(aux_ui_serial,
                              [&](std::span<const u32> px, u32 tw, u32 th,
                                  const VideoCore::DSMod::TileMask& mask,
                                  std::array<s32, 4> bbox) {
                                  const bool recreated = recreate_if_needed(tw, th);
                                  if (verify_uploads) {
                                      verify_expect.assign(px.begin(), px.end());
                                  }
                                  UploadTilesAsync(scheduler, memory_allocator, aux_ui_staging,
                                                   aux_ui_staging_bytes, aux_ui_upload_tick,
                                                   *aux_ui_image, px, tw, th, mask, recreated,
                                                   bbox);
                              })
            : aux.WithUiPartial(aux_ui_serial, dirty, [&](std::span<const u32> px, u32 tw, u32 th) {
            const bool recreated = recreate_if_needed(tw, th);
            const s32 x0 = std::clamp(dirty[0], 0, static_cast<s32>(tw));
            const s32 y0 = std::clamp(dirty[1], 0, static_cast<s32>(th));
            const s32 x1 = std::clamp(dirty[0] + std::max(dirty[2], 0), 0, static_cast<s32>(tw));
            const s32 y1 = std::clamp(dirty[1] + std::max(dirty[3], 0), 0, static_cast<s32>(th));
            const u32 dw = static_cast<u32>(std::max(0, x1 - x0));
            const u32 dh = static_cast<u32>(std::max(0, y1 - y0));
            const bool whole_canvas = dw >= tw && dh >= th;
            if (recreated || dw == 0 || dh == 0 || whole_canvas) {
                UploadImageAsync(
                    scheduler, memory_allocator, aux_ui_staging, aux_ui_staging_bytes,
                    aux_ui_upload_tick, *aux_ui_image, VkExtent2D{tw, th},
                    std::span<const u8>{reinterpret_cast<const u8*>(px.data()), px.size_bytes()});
                return;
            }
            // Extract just the dirty rows into a compact scratch buffer -- mirrors
            // AuxRouting::PublishUiPartial's own row-bounded copy, so this upload's own cost is
            // proportional to the dirty area, not the whole ~5 MB canvas.
            aux_ui_partial_scratch.resize(static_cast<size_t>(dw) * dh);
            for (u32 row = 0; row < dh; ++row) {
                const size_t src = (static_cast<size_t>(y0) + row) * tw + static_cast<size_t>(x0);
                std::copy_n(px.data() + src, dw,
                           aux_ui_partial_scratch.data() + static_cast<size_t>(row) * dw);
            }
            UploadImageAsync(scheduler, memory_allocator, aux_ui_staging, aux_ui_staging_bytes,
                             aux_ui_upload_tick, *aux_ui_image, VkExtent2D{dw, dh},
                             std::span<const u8>{
                                 reinterpret_cast<const u8*>(aux_ui_partial_scratch.data()),
                                 aux_ui_partial_scratch.size() * sizeof(u32)},
                             VkOffset2D{x0, y0}, /*preserve=*/true);
        });
    if (!aux_ui_image || aux_ui_w == 0 || aux_ui_h == 0) {
        return updated;
    }
    if (verify_uploads && updated &&
        verify_expect.size() == static_cast<size_t>(aux_ui_w) * aux_ui_h) {
        static u64 verified = 0, bad_frames = 0, bad_px_max = 0;
        const size_t nbytes = verify_expect.size() * sizeof(u32);
        vk::Buffer buf = CreateWrappedBuffer(memory_allocator, nbytes, MemoryUsage::Download);
        const VkExtent3D ext{aux_ui_w, aux_ui_h, 1};
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([img = *aux_ui_image, b = *buf, ext](vk::CommandBuffer cmdbuf) {
            DownloadColorImage(cmdbuf, img, b, ext);
        });
        scheduler.Finish();
        buf.Invalidate();
        const u32* got = reinterpret_cast<const u32*>(buf.Mapped().data());
        u64 bad = 0;
        for (size_t i = 0; i < verify_expect.size(); ++i) {
            bad += got[i] != verify_expect[i];
        }
        ++verified;
        bad_frames += bad != 0;
        bad_px_max = std::max(bad_px_max, bad);
        if (bad != 0 || verified % 50 == 0) {
            LOG_INFO(Render_Vulkan, "DSMod aux verify: uploads={} mismatched={} max_bad_px={}",
                     verified, bad_frames, bad_px_max);
        }
    }
    return updated;
}

bool DSModAuxPresenter::RenderAuxModUi() {
    const auto layout = aux_window->GetFramebufferLayout();
    if (layout.width == 0 || layout.height == 0) {
        return false;
    }
    const bool updated = UpdateAuxUiImage();
    if (!aux_ui_image || aux_ui_w == 0 || aux_ui_h == 0) {
        return false;
    }
    // Eden Duo: Fit keeps the canvas shape inside a panel of another shape (bars in the page
    // background); a full rect is the plain whole-frame blit.
    const auto fit = VideoCore::DSMod::FitCompanion(layout.width, layout.height, aux_ui_w, aux_ui_h,
                                                    VideoCore::DSMod::SecondScreenOptions::Ratio());
    // The UI changes at <=30 Hz but this runs every presented frame; re-blitting an identical
    // image cost a whole extra present per frame (~6 ms on the Adreno panel). The panel holds
    // the last present, so when nothing changed leave it up.
    if (!updated && aux_ui_presented && aux_ui_last_w == layout.width &&
        aux_ui_last_h == layout.height && aux_ui_last_full == fit.full) {
        return true;
    }
    const VkExtent2D ui_extent{aux_ui_w, aux_ui_h};
    if (!aux_ui_render_pass) {
        aux_ui_render_pass = CreateWrappedRenderPass(device, aux_swapchain->GetImageViewFormat());
    }

    Frame* frame = aux_present_manager->TryGetRenderFrame();
    if (frame == nullptr) {
        aux_ui_presented = false;
        return false;
    }
    if (frame->width != layout.width || frame->height != layout.height || !frame->image) {
        aux_present_manager->RecreateFrame(frame, layout.width, layout.height,
                                           aux_swapchain->GetImageViewFormat(),
                                           *aux_ui_render_pass, /*storage=*/false);
    }

    const VkExtent2D dst_extent{frame->width, frame->height};
    scheduler.RequestOutsideRenderPassOperationContext();
    const u32 bar = gpu.DSModAux().ui_bg.load(std::memory_order_relaxed);
    const VkClearColorValue bar_color{.float32 = {static_cast<f32>((bar >> 16) & 0xFF) / 255.0f,
                                                  static_cast<f32>((bar >> 8) & 0xFF) / 255.0f,
                                                  static_cast<f32>(bar & 0xFF) / 255.0f, 1.0f}};
    scheduler.Record([src = *aux_ui_image, dst = *frame->image, ui_extent, dst_extent, fit,
                      bar_color](vk::CommandBuffer cmdbuf) {
        const auto transition = [&](VkImage image, VkImageLayout old_layout,
                                    VkImageLayout new_layout, VkAccessFlags src_access,
                                    VkAccessFlags dst_access) {
            const VkImageMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = src_access,
                .dstAccessMask = dst_access,
                .oldLayout = old_layout,
                .newLayout = new_layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, barrier);
        };
        transition(src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT);
        transition(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT);
        if (!fit.full) {
            // The blit covers only the fitted rect: the bars are the page background.
            cmdbuf.ClearColorImage(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, bar_color,
                                   VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
            transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        const VkImageBlit region{
            .srcSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1},
            .srcOffsets{{0, 0, 0},
                        {static_cast<s32>(ui_extent.width), static_cast<s32>(ui_extent.height), 1}},
            .dstSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1},
            .dstOffsets{
                fit.full ? VkOffset3D{0, 0, 0}
                         : VkOffset3D{static_cast<s32>(fit.x), static_cast<s32>(fit.y), 0},
                fit.full ? VkOffset3D{static_cast<s32>(dst_extent.width),
                                      static_cast<s32>(dst_extent.height), 1}
                         : VkOffset3D{static_cast<s32>(fit.x + fit.w),
                                      static_cast<s32>(fit.y + fit.h), 1}},
        };
        cmdbuf.BlitImage(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region, VK_FILTER_LINEAR);
        transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    });
    aux_ui_blit_tick = scheduler.CurrentTick();
    scheduler.Flush(*frame->render_ready);
    aux_present_manager->Present(frame);
    aux_ui_presented = true;
    aux_ui_last_w = layout.width;
    aux_ui_last_h = layout.height;
    aux_ui_last_full = fit.full;
    return true;
}

// Eden Duo Stage 1 (Simulated/inline dual screen): the CPU-UI canvas path's analogue of
// RenderAuxModUi above, minus the parts that need a real aux window -- no swapchain frame to
// blit into or present; the final pass (WindowAdaptPass) samples aux_ui_image/aux_ui_view
// directly instead. `pane` is the companion's area in main-window pixels (RenderInline already
// checked it is non-empty).
bool DSModAuxPresenter::RenderInlineModUi(const VideoCore::DSMod::CompanionRect& pane) {
    UpdateAuxUiImage();
    if (!aux_ui_image || aux_ui_w == 0 || aux_ui_h == 0) {
        return false;
    }
    // Eden Duo: Fit keeps the canvas shape inside the pane (bars in the page background), same
    // math as the physical blit's own Fit, just against the pane instead of the aux window.
    const auto fit = VideoCore::DSMod::FitCompanion(pane.w, pane.h, aux_ui_w, aux_ui_h,
                                                    VideoCore::DSMod::SecondScreenOptions::Ratio());
    inline_companion.view = *aux_ui_view;
    inline_companion.extent = VkExtent2D{aux_ui_w, aux_ui_h};
    inline_companion.version = aux_ui_serial;
    inline_companion.pane = VkRect2D{{static_cast<s32>(pane.x), static_cast<s32>(pane.y)},
                                     {pane.w, pane.h}};
    inline_companion.fit =
        VkRect2D{{static_cast<s32>(pane.x + fit.x), static_cast<s32>(pane.y + fit.y)},
                 {fit.w, fit.h}};
    inline_companion.fit_is_pane = fit.full;
    inline_companion.background = gpu.DSModAux().ui_bg.load(std::memory_order_relaxed);
    return true;
}

// Eden Duo Stage 1: dispatch between the CPU-UI and GPU-composite inline paths (mirrors Route's
// own physical-mode dispatch just above it), and the geometry/presence bookkeeping common to
// both. Returns the game's rect (main-window pixels) when the companion pane has a non-empty
// size -- the caller (RendererVulkan::Composite) then narrows the game's own layout to it --
// else nullopt (leave the game using the whole window; nothing to show on the companion side).
std::optional<VideoCore::DSMod::CompanionRect> DSModAuxPresenter::RenderInline(
    const Layout::FramebufferLayout& main_window_layout) {
    auto& aux = gpu.DSModAux();
    const auto rects = aux.GetInlineRects(main_window_layout.width, main_window_layout.height);
    if (rects.companion.w == 0 || rects.companion.h == 0) {
        inline_companion = CompanionQuad{};
        aux.present.store(false, std::memory_order_relaxed);
        return std::nullopt;
    }
    // Stage 1 work item 1: while inline is active and the pane is non-empty, present/width/height
    // read as they would with a real second screen (__aux_present, HUD-hide, runtime ticks).
    aux.width.store(rects.companion.w, std::memory_order_relaxed);
    aux.height.store(rects.companion.h, std::memory_order_relaxed);
    aux.present.store(true, std::memory_order_relaxed);

    const bool drew = (aux.gpu_composite.load(std::memory_order_relaxed) && aux.HasComposite())
                          ? RenderInlineModUiGpu(rects.companion)
                          : RenderInlineModUi(rects.companion);
    if (!drew) {
        // Nothing published yet: still report the pane (background-only, no quad) so the window
        // shows the companion's colour instead of the game's own letterbox colour there.
        inline_companion = CompanionQuad{};
        inline_companion.pane = VkRect2D{{static_cast<s32>(rects.companion.x),
                                          static_cast<s32>(rects.companion.y)},
                                         {rects.companion.w, rects.companion.h}};
        inline_companion.fit_is_pane = true;
        inline_companion.background = aux.ui_bg.load(std::memory_order_relaxed);
    }
    return rects.game;
}

void DSModAuxPresenter::EnsureAuxCompositeResources(VkFormat color_format) {
    if (aux_c_init) {
        if (!aux_c_render_pass || aux_c_format != color_format) {
            aux_c_render_pass = CreateWrappedRenderPass(device, color_format,
                                                        VK_IMAGE_LAYOUT_UNDEFINED);
            aux_c_pipeline = CreateWrappedCoverageBlendingPipeline(
                device, aux_c_render_pass, aux_c_pipe_layout, std::tie(aux_c_vert, aux_c_frag));
            aux_c_map_pipeline = CreateWrappedCoverageBlendingPipeline(
                device, aux_c_render_pass, aux_c_map_pipe_layout,
                std::tie(aux_c_vert, aux_c_map_frag));
            aux_c_format = color_format;
        }
        return;
    }
    const VkFormat fmt = color_format;
    aux_c_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    aux_c_map_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    const VkPushConstantRange pc_range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(AuxQuadPushConstants),
    };
    aux_c_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = aux_c_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    });
    aux_c_map_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = aux_c_map_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    });
    aux_c_vert = BuildShader(device, VULKAN_DSMOD_QUAD_VERT_SPV);
    aux_c_frag = BuildShader(device, VULKAN_DSMOD_QUAD_FRAG_SPV);
    aux_c_map_frag = BuildShader(device, VULKAN_DSMOD_MAP_FADE_FRAG_SPV);
    // UNDEFINED initial layout (DONT_CARE) is fine: the pass clears to the background first.
    aux_c_render_pass = CreateWrappedRenderPass(device, fmt, VK_IMAGE_LAYOUT_UNDEFINED);
    // Straight-alpha src-over for colour; the frame's alpha ends up as the last quad's (the
    // transparent HUD), which the swapchain ignores under VK_COMPOSITE_ALPHA_OPAQUE. A compositor
    // that honours frame alpha (the INHERIT fallback) would need an RGB-only write mask here.
    aux_c_pipeline = CreateWrappedCoverageBlendingPipeline(
        device, aux_c_render_pass, aux_c_pipe_layout, std::tie(aux_c_vert, aux_c_frag));
    aux_c_map_pipeline = CreateWrappedCoverageBlendingPipeline(
        device, aux_c_render_pass, aux_c_map_pipe_layout, std::tie(aux_c_vert, aux_c_map_frag));
    aux_c_sampler_nearest = CreateNearestNeighborSampler(device);
    aux_c_pool = CreateWrappedDescriptorPool(device, NumAuxTex, NumAuxTex);
    const std::vector<VkDescriptorSetLayout> layouts(NumAuxTex, *aux_c_set_layout);
    aux_c_sets = CreateWrappedDescriptorSets(aux_c_pool, layouts);
    aux_c_map_pool = CreateWrappedDescriptorPool(device, 3, 1);
    const std::array<VkDescriptorSetLayout, 1> map_layouts{*aux_c_map_set_layout};
    aux_c_map_set = CreateWrappedDescriptorSets(aux_c_map_pool, map_layouts);
    aux_c_format = fmt;
    aux_c_init = true;
}

// Shared by RenderAuxModUiGpu (physical) and RenderInlineModUiGpu (Simulated/inline): takes the
// latest quad list + textures from the routing buffer and uploads whatever changed. Returns false
// when there is nothing to composite yet (no canvas size published).
bool DSModAuxPresenter::UpdateAuxComposite() {
    using Quad = VideoCore::DSMod::AuxRouting::Quad;
    auto& aux = gpu.DSModAux();
    if (!aux.HasComposite()) {
        return false;
    }
    {
        std::vector<Quad> quads;
        u32 cw = 0, ch = 0, bg = 0;
        if (aux.TakeComposite(quads, cw, ch, bg, aux_c_comp_serial)) {
            aux_c_quads = std::move(quads);
            aux_c_cw = cw;
            aux_c_ch = ch;
            aux_c_bg = bg;
            aux_c_dirty = true;
        }
    }
    if (aux_c_cw == 0 || aux_c_ch == 0) {
        return false;
    }

    // Re-upload any changed source texture, async (persistent per-slot staging, no Finish),
    // copying straight out of the routing buffer. A slot whose size changed gets a new image and
    // descriptor -- only after the last draw that sampled the old one has retired.
    // `mask` (per-slot, non-map slots only) limits the upload to changed tiles; the map
    // bundle and any freshly (re)created image upload whole.
    const auto upload_tiles = [&](u32 s, std::span<const u32> px, u32 tw, u32 th,
                                  const VideoCore::DSMod::TileMask* mask) {
        const bool recreated = !aux_c_img[s] || aux_c_w[s] != tw || aux_c_h[s] != th;
        if (recreated) {
            scheduler.Wait(std::max(aux_c_draw_tick, aux_c_upload_tick[s]));
            aux_c_img[s] = CreateWrappedImage(memory_allocator, VkExtent2D{tw, th},
                                              VK_FORMAT_B8G8R8A8_UNORM);
            aux_c_view[s] =
                CreateWrappedImageView(device, aux_c_img[s], VK_FORMAT_B8G8R8A8_UNORM);
            aux_c_w[s] = tw;
            aux_c_h[s] = th;
            std::vector<VkDescriptorImageInfo> infos;
            infos.reserve(1);
            // Nearest for every slot -- matches the CPU canvas' nearest-neighbour blits, so
            // the map stays crisp instead of bilinear-soft.
            const VkWriteDescriptorSet write = CreateWriteDescriptorSet(
                infos, *aux_c_sampler_nearest, *aux_c_view[s], aux_c_sets[s], 0);
            device.GetLogical().UpdateDescriptorSets(std::array{write}, {});
            if (s == VideoCore::DSMod::AuxRouting::MapCurrentSlot ||
                s == VideoCore::DSMod::AuxRouting::MapPreviousSlot ||
                s == VideoCore::DSMod::AuxRouting::MapFadeSlot) {
                aux_c_map_desc_ready = false;
            }
        }
        static const VideoCore::DSMod::TileMask whole{};  // all = true
        UploadTilesAsync(scheduler, memory_allocator, aux_c_staging[s], aux_c_staging_bytes[s],
                         aux_c_upload_tick[s], *aux_c_img[s], px, tw, th,
                         mask != nullptr ? *mask : whole, recreated || mask == nullptr);
        ++aux_c_upload_count[s];
        // Developer check (EDEN_DSMOD_AUX_VERIFY), as for the canvas path: read the slot image
        // back after every upload and compare it with the routing buffer it came from.
        static const bool verify_slots = Common::DSMod::DevEnvironment("EDEN_DSMOD_AUX_VERIFY") != nullptr;
        if (verify_slots) {
            static std::array<u64, NumAuxTex> verified{}, bad_uploads{};
            const size_t nbytes = px.size_bytes();
            vk::Buffer buf = CreateWrappedBuffer(memory_allocator, nbytes, MemoryUsage::Download);
            const VkExtent3D ext{tw, th, 1};
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([img = *aux_c_img[s], b = *buf, ext](vk::CommandBuffer cmdbuf) {
                DownloadColorImage(cmdbuf, img, b, ext);
            });
            scheduler.Finish();
            buf.Invalidate();
            const bool same = std::memcmp(buf.Mapped().data(), px.data(), nbytes) == 0;
            ++verified[s];
            bad_uploads[s] += same ? 0 : 1;
            if (!same || verified[s] % 20 == 0 || (mask != nullptr && !mask->all && !recreated)) {
                LOG_INFO(Render_Vulkan,
                         "DSMod aux slot verify: slot {} {}x{} {} upload -> {}; slot uploads={} "
                         "mismatched={}",
                         s, tw, th,
                         (mask != nullptr && !mask->all && !recreated) ? "tile" : "full",
                         same ? "identical" : "MISMATCH", verified[s], bad_uploads[s]);
            }
        }
    };
    for (u32 s = 0; s < NumAuxTex; ++s) {
        if (s == VideoCore::DSMod::AuxRouting::MapCurrentSlot ||
            s == VideoCore::DSMod::AuxRouting::MapPreviousSlot ||
            s == VideoCore::DSMod::AuxRouting::MapFadeSlot) {
            continue;
        }
        const bool up = aux.WithAuxTextureTiles(
            s, aux_c_serial[s],
            [&](std::span<const u32> px, u32 tw, u32 th, const VideoCore::DSMod::TileMask& mask) {
                upload_tiles(s, px, tw, th, VideoCore::DSMod::UiDiffEnabled() ? &mask : nullptr);
            });
        aux_c_dirty = aux_c_dirty || up;
    }
    // The endpoints arrive with the tiles changed since the last take (a reveal patches a few;
    // a full publish marks all of them), so a reveal uploads its tiles, not two whole maps.
    const bool map_up = aux.WithMapFadeTextures(
        aux_c_serial[VideoCore::DSMod::AuxRouting::MapCurrentSlot],
        aux_c_serial[VideoCore::DSMod::AuxRouting::MapPreviousSlot],
        aux_c_serial[VideoCore::DSMod::AuxRouting::MapFadeSlot],
        [&](u32 s, std::span<const u32> px, u32 tw, u32 th,
            const VideoCore::DSMod::TileMask& mask) {
            upload_tiles(s, px, tw, th, VideoCore::DSMod::UiDiffEnabled() ? &mask : nullptr);
        });
    aux_c_dirty = aux_c_dirty || map_up;
    if (!aux_c_map_desc_ready && aux_c_img[VideoCore::DSMod::AuxRouting::MapCurrentSlot] &&
        aux_c_img[VideoCore::DSMod::AuxRouting::MapPreviousSlot] &&
        aux_c_img[VideoCore::DSMod::AuxRouting::MapFadeSlot]) {
        std::vector<VkDescriptorImageInfo> infos;
        infos.reserve(3);
        std::array<VkWriteDescriptorSet, 3> writes{
            CreateWriteDescriptorSet(infos, *aux_c_sampler_nearest,
                                     *aux_c_view[VideoCore::DSMod::AuxRouting::MapPreviousSlot],
                                     aux_c_map_set[0], 0),
            CreateWriteDescriptorSet(infos, *aux_c_sampler_nearest,
                                     *aux_c_view[VideoCore::DSMod::AuxRouting::MapCurrentSlot],
                                     aux_c_map_set[0], 1),
            CreateWriteDescriptorSet(infos, *aux_c_sampler_nearest,
                                     *aux_c_view[VideoCore::DSMod::AuxRouting::MapFadeSlot],
                                     aux_c_map_set[0], 2),
        };
        device.GetLogical().UpdateDescriptorSets(writes, {});
        aux_c_map_desc_ready = true;
    }
    return true;
}

// See the header doc comment. `fit`/`fit_full` are already resolved by the caller (against the
// aux window's layout for the physical path, against the companion pane for inline).
void DSModAuxPresenter::RecordCompositeDraws(VkFramebuffer dst_fb, VkImage dst_image,
                                             VkExtent2D render_area,
                                             VideoCore::DSMod::CompanionRect fit, bool fit_full) {
    using Quad = VideoCore::DSMod::AuxRouting::Quad;
    const std::array<f32, 4> scale_offset{2.0f / static_cast<f32>(aux_c_cw),
                                          2.0f / static_cast<f32>(aux_c_ch), -1.0f, -1.0f};
    const f32 canvas_to_frame_x =
        static_cast<f32>(fit_full ? render_area.width : fit.w) / aux_c_cw;
    const f32 canvas_to_frame_y =
        static_cast<f32>(fit_full ? render_area.height : fit.h) / aux_c_ch;
    const s32 frame_x0 = fit_full ? 0 : static_cast<s32>(fit.x);
    const s32 frame_y0 = fit_full ? 0 : static_cast<s32>(fit.y);
    const f32 br = static_cast<f32>((aux_c_bg >> 16) & 0xFF) / 255.0f;
    const f32 bgg = static_cast<f32>((aux_c_bg >> 8) & 0xFF) / 255.0f;
    const f32 bb = static_cast<f32>(aux_c_bg & 0xFF) / 255.0f;
    const f32 ba = static_cast<f32>((aux_c_bg >> 24) & 0xFF) / 255.0f;

    // Everything the recorded lambda touches is captured by value: it executes later on the
    // scheduler worker, while this thread may already be replacing a slot's image for the next
    // present, and the frame belongs to a present manager a detach can reset.
    struct DrawQuad {
        Quad q;
        VkDescriptorSet set;
        bool map_fade;
    };
    std::vector<DrawQuad> draws;
    draws.reserve(aux_c_quads.size());
    for (const auto& q : aux_c_quads) {
        if (q.solid) {
            draws.push_back({q, {}, false});
            continue;
        }
        if (q.slot >= NumAuxTex || !aux_c_img[q.slot]) {
            continue;
        }
        if (q.map_fade && !aux_c_map_desc_ready) {
            continue;
        }
        draws.push_back({q, q.map_fade ? aux_c_map_set[0] : aux_c_sets[q.slot], q.map_fade});
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([draws = std::move(draws), fb = dst_fb, img = dst_image,
                      rp = *aux_c_render_pass, pipe = *aux_c_pipeline,
                      map_pipe = *aux_c_map_pipeline, pl = *aux_c_pipe_layout,
                      map_pl = *aux_c_map_pipe_layout, render_area, scale_offset,
                      canvas_to_frame_x, canvas_to_frame_y, frame_x0, frame_y0, fit_full, fit_x = fit.x,
                      fit_y = fit.y, fit_w = fit.w, fit_h = fit.h,
                      br, bgg, bb, ba](vk::CommandBuffer cmdbuf) {
        BeginRenderPass(cmdbuf, rp, fb, render_area);
        const VkClearAttachment clear_att{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0,
            .clearValue = {.color = {.float32 = {br, bgg, bb, ba}}},
        };
        const VkClearRect clear_rect{
            .rect = {.offset = {0, 0}, .extent = render_area},
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        cmdbuf.ClearAttachments({clear_att}, {clear_rect});
        if (!fit_full) {
            // Draw the canvas into the fitted rect only; the clear above made the bars.
            const VkViewport fit_viewport{
                .x = static_cast<f32>(fit_x),
                .y = static_cast<f32>(fit_y),
                .width = static_cast<f32>(fit_w),
                .height = static_cast<f32>(fit_h),
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
            };
            const VkRect2D fit_scissor{
                .offset = {static_cast<s32>(fit_x), static_cast<s32>(fit_y)},
                .extent = {fit_w, fit_h},
            };
            cmdbuf.SetViewport(0, fit_viewport);
            cmdbuf.SetScissor(0, fit_scissor);
        }
        const s32 area_x1 = fit_full ? static_cast<s32>(render_area.width)
                                     : static_cast<s32>(fit_x + fit_w);
        const s32 area_y1 = fit_full ? static_cast<s32>(render_area.height)
                                     : static_cast<s32>(fit_y + fit_h);
        for (const auto& d : draws) {
            const Quad& q = d.q;
            if (q.solid) {
                const f32 sr = static_cast<f32>((q.color >> 16) & 0xFF) / 255.0f;
                const f32 sg = static_cast<f32>((q.color >> 8) & 0xFF) / 255.0f;
                const f32 sb = static_cast<f32>(q.color & 0xFF) / 255.0f;
                const f32 sa = static_cast<f32>((q.color >> 24) & 0xFF) / 255.0f;
                const auto left =
                    frame_x0 + static_cast<s32>(std::floor(q.x * canvas_to_frame_x));
                const auto top =
                    frame_y0 + static_cast<s32>(std::floor(q.y * canvas_to_frame_y));
                const auto right =
                    frame_x0 + static_cast<s32>(std::ceil((q.x + q.w) * canvas_to_frame_x));
                const auto bottom =
                    frame_y0 + static_cast<s32>(std::ceil((q.y + q.h) * canvas_to_frame_y));
                const s32 x0 = std::clamp(left, frame_x0, area_x1);
                const s32 y0 = std::clamp(top, frame_y0, area_y1);
                const s32 x1 = std::clamp(right, x0, area_x1);
                const s32 y1 = std::clamp(bottom, y0, area_y1);
                const VkClearAttachment solid_att{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .colorAttachment = 0,
                    .clearValue = {.color = {.float32 = {sr, sg, sb, sa}}},
                };
                const VkClearRect solid_rect{
                    .rect = {.offset = {x0, y0},
                             .extent = {static_cast<u32>(x1 - x0),
                                        static_cast<u32>(y1 - y0)}},
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                };
                if (x1 > x0 && y1 > y0) {
                    cmdbuf.ClearAttachments({solid_att}, {solid_rect});
                }
                continue;
            }
            const VkPipeline draw_pipe = d.map_fade ? map_pipe : pipe;
            const VkPipelineLayout draw_layout = d.map_fade ? map_pl : pl;
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipe);
            AuxQuadPushConstants pc{};
            pc.scale_offset = scale_offset;
            pc.vertices[0] = ScreenRectVertex(q.x, q.y, q.u0, q.v0);
            pc.vertices[1] = ScreenRectVertex(q.x + q.w, q.y, q.u1, q.v0);
            pc.vertices[2] = ScreenRectVertex(q.x, q.y + q.h, q.u0, q.v1);
            pc.vertices[3] = ScreenRectVertex(q.x + q.w, q.y + q.h, q.u1, q.v1);
            pc.alpha = q.a;
            pc.tint = {q.r, q.g, q.b};
            cmdbuf.PushConstants(draw_layout, VK_SHADER_STAGE_VERTEX_BIT, pc);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, draw_layout, 0, d.set, {});
            cmdbuf.Draw(4, 1, 0, 0);
        }
        cmdbuf.EndRenderPass();
        // Eden Duo Stage 1: makes this write visible to a later shader read of the same image in
        // the same frame (RenderInlineModUiGpu samples it right after, in WindowAdaptPass) --
        // same convention FXAA/SMAA use for a same-frame write-then-sample. A no-op in spirit for
        // the physical path (presentation has its own sync), harmless to include unconditionally.
        TransitionImageLayout(cmdbuf, img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    });
    aux_c_draw_tick = scheduler.CurrentTick();
}

bool DSModAuxPresenter::RenderAuxModUiGpu() {
    if (!UpdateAuxComposite()) {
        return false;
    }
    const auto& layout = aux_window->GetFramebufferLayout();
    if (layout.width == 0 || layout.height == 0) {
        return false;
    }
    EnsureAuxCompositeResources(aux_swapchain->GetImageViewFormat());

    // Eden Duo: Fit keeps the canvas shape (see RenderAuxModUi); the bars are the page
    // background the full-frame clear below already lays down.
    const auto fit = VideoCore::DSMod::FitCompanion(layout.width, layout.height, aux_c_cw, aux_c_ch,
                                                    VideoCore::DSMod::SecondScreenOptions::Ratio());
    // Hold the last present when nothing changed: the panel keeps showing it, so we neither
    // re-record nor re-present (presenting every game frame halved fps). A panel resize or a
    // reattach (DestroyAuxLocked clears aux_c_presented) re-presents the kept composite.
    if (!aux_c_dirty && aux_c_presented && aux_c_last_w == layout.width &&
        aux_c_last_h == layout.height && aux_c_last_full == fit.full) {
        return true;
    }

    Frame* frame = aux_present_manager->TryGetRenderFrame();
    if (frame == nullptr) {
        return false;
    }
    if (frame->width != layout.width || frame->height != layout.height || !frame->framebuffer) {
        aux_present_manager->RecreateFrame(frame, layout.width, layout.height,
                                           aux_swapchain->GetImageViewFormat(), *aux_c_render_pass,
                                           /*storage=*/false);
    }
    RecordCompositeDraws(*frame->framebuffer, *frame->image,
                         VkExtent2D{frame->width, frame->height}, fit, fit.full);
    // Debug: pull the composited frame back and hand it to the screenshot path (screen2.png).
    // Costs a full GPU drain per present, so it is a developer switch, read once.
    static const bool readback = Common::DSMod::DevEnvironment("EDEN_DSMOD_GPU_READBACK") != nullptr;
    if (readback) {
        const size_t nbytes = static_cast<size_t>(frame->width) * frame->height * 4;
        if (!aux_c_readback_buf || aux_c_readback_w != frame->width ||
            aux_c_readback_h != frame->height) {
            aux_c_readback_buf =
                CreateWrappedBuffer(memory_allocator, nbytes, MemoryUsage::Download);
            aux_c_readback_w = frame->width;
            aux_c_readback_h = frame->height;
        }
        const VkExtent3D ext{frame->width, frame->height, 1};
        scheduler.Record(
            [img = *frame->image, buf = *aux_c_readback_buf, ext](vk::CommandBuffer cmdbuf) {
                DownloadColorImage(cmdbuf, img, buf, ext);
            });
    }
    scheduler.Flush(*frame->render_ready);
    aux_present_manager->Present(frame);
    aux_ui_presented = true;
    aux_c_presented = true;
    aux_c_dirty = false;
    aux_c_last_w = layout.width;
    aux_c_last_h = layout.height;
    aux_c_last_full = fit.full;
    if (readback) {
        scheduler.Finish();
        aux_c_readback_buf.Invalidate();
        std::vector<u32> out(static_cast<size_t>(aux_c_readback_w) * aux_c_readback_h);
        std::memcpy(out.data(), aux_c_readback_buf.Mapped().data(), out.size() * sizeof(u32));
        gpu.DSModAux().PublishUi(aux_c_readback_w, aux_c_readback_h, out,
                               /*require_context=*/true);
    }
    if (((++aux_c_present_count) % 600) == 0) {
        LOG_DEBUG(
            Render_Vulkan,
            "DSMod GPU composite: {} presents, uploads map={} prev={} fade={} atlas={} hud={}",
            aux_c_present_count, aux_c_upload_count[0],
            aux_c_upload_count[VideoCore::DSMod::AuxRouting::MapPreviousSlot],
            aux_c_upload_count[VideoCore::DSMod::AuxRouting::MapFadeSlot], aux_c_upload_count[1],
                  aux_c_upload_count[2]);
    }
    return true;
}

// Eden Duo Stage 1: the offscreen image RenderInlineModUiGpu composites into, 1:1 at the canvas's
// own size (letterboxing against the companion pane happens once more, in WindowAdaptPass).
// aux_c_render_pass must already be built for this image's fixed format (EnsureAuxCompositeResources).
void DSModAuxPresenter::EnsureInlineCompositeImage(u32 w, u32 h) {
    if (aux_inline_image && aux_inline_w == w && aux_inline_h == h) {
        return;
    }
    // The previous image may still be read by WindowAdaptPass's companion quad (last frame's
    // sample); the draw that wrote it is the last thing that could still be reading it too.
    scheduler.Wait(aux_c_draw_tick);
    constexpr VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    aux_inline_image = CreateWrappedImage(memory_allocator, VkExtent2D{w, h}, fmt);
    aux_inline_view = CreateWrappedImageView(device, aux_inline_image, fmt);
    const VkImageView view = *aux_inline_view;
    aux_inline_framebuffer = device.GetLogical().CreateFramebuffer(VkFramebufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = *aux_c_render_pass,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = w,
        .height = h,
        .layers = 1,
    });
    aux_inline_w = w;
    aux_inline_h = h;
}

// Eden Duo Stage 1: the GPU-composite path's analogue of RenderAuxModUiGpu above, minus the parts
// that need a real aux window -- the quad list draws into aux_inline_image (1:1, no Fit) instead
// of an aux swapchain frame; the final pass samples it directly. `pane` is the companion's area
// in main-window pixels (RenderInline already checked it is non-empty).
bool DSModAuxPresenter::RenderInlineModUiGpu(const VideoCore::DSMod::CompanionRect& pane) {
    if (!UpdateAuxComposite()) {
        return false;
    }
    EnsureAuxCompositeResources(VK_FORMAT_B8G8R8A8_UNORM);
    EnsureInlineCompositeImage(aux_c_cw, aux_c_ch);
    if (aux_c_dirty) {
        RecordCompositeDraws(*aux_inline_framebuffer, *aux_inline_image,
                             VkExtent2D{aux_c_cw, aux_c_ch},
                             VideoCore::DSMod::CompanionRect{0, 0, aux_c_cw, aux_c_ch, true},
                             /*fit_full=*/true);
        aux_c_dirty = false;
    }
    // Eden Duo: Fit keeps the composited canvas' shape inside the pane (bars in its background);
    // a full rect is the plain whole-pane draw.
    const auto fit = VideoCore::DSMod::FitCompanion(pane.w, pane.h, aux_c_cw, aux_c_ch,
                                                    VideoCore::DSMod::SecondScreenOptions::Ratio());
    inline_companion.view = *aux_inline_view;
    inline_companion.extent = VkExtent2D{aux_c_cw, aux_c_ch};
    inline_companion.version = aux_c_comp_serial;
    inline_companion.pane = VkRect2D{{static_cast<s32>(pane.x), static_cast<s32>(pane.y)},
                                     {pane.w, pane.h}};
    inline_companion.fit =
        VkRect2D{{static_cast<s32>(pane.x + fit.x), static_cast<s32>(pane.y + fit.y)},
                 {fit.w, fit.h}};
    inline_companion.fit_is_pane = fit.full;
    inline_companion.background = aux_c_bg;
    return true;
}

void DSModAuxPresenter::RenderAuxWindow(std::span<const Tegra::FramebufferConfig> layers) {
    const auto& window_layout = aux_window->GetFramebufferLayout();
    if (window_layout.width == 0 || window_layout.height == 0 || layers.empty()) {
        return;
    }
    auto& aux = gpu.DSModAux();
    aux.width.store(window_layout.width);
    aux.height.store(window_layout.height);

    // Fit the guest layer into the panel without distorting it. The panel is not 16:9 (the Thor's
    // second screen is 1240x1080), so neither stretching to fill nor the 16:9 default layout is
    // right: scale by the source's own aspect and centre it.
    Layout::FramebufferLayout layout = window_layout;
    const auto& src = layers.front();
    const u32 src_w =
        src.crop_rect.GetWidth() > 0 ? static_cast<u32>(src.crop_rect.GetWidth()) : src.width;
    const u32 src_h =
        src.crop_rect.GetHeight() > 0 ? static_cast<u32>(src.crop_rect.GetHeight()) : src.height;
    if (src_w > 0 && src_h > 0) {
        const f32 panel_aspect =
            static_cast<f32>(window_layout.width) / static_cast<f32>(window_layout.height);
        const f32 src_aspect = static_cast<f32>(src_w) / static_cast<f32>(src_h);
        u32 draw_w = window_layout.width;
        u32 draw_h = window_layout.height;
        if (src_aspect > panel_aspect) {
            draw_h = static_cast<u32>(static_cast<f32>(window_layout.width) / src_aspect);
        } else {
            draw_w = static_cast<u32>(static_cast<f32>(window_layout.height) * src_aspect);
        }
        const u32 off_x = (window_layout.width - draw_w) / 2;
        const u32 off_y = (window_layout.height - draw_h) / 2;
        layout.screen = Common::Rectangle<u32>{off_x, off_y, off_x + draw_w, off_y + draw_h};
    }

    Frame* frame = aux_present_manager->TryGetRenderFrame();
    if (frame == nullptr) {
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    blit_aux->DrawToFrame(device, rasterizer, frame, layers, layout, aux_swapchain->GetImageCount(),
                          aux_swapchain->GetImageViewFormat());
    scheduler.Flush(*frame->render_ready);
    aux_present_manager->Present(frame);
}

} // namespace Vulkan
