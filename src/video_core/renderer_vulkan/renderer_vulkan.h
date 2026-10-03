// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/dynamic_library.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/frame_gen.h"
#endif
#include <array>
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_turbo_mode.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Memory {
class Memory;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface);

class RendererVulkan final : public VideoCore::RendererBase {
public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                            std::unique_ptr<Core::Frontend::GraphicsContext> context_);
    ~RendererVulkan() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.GetDriverName();
    }

    // Enhanced platform-specific initialization
    void InitializePlatformSpecific();

    void SetAuxWindow(Core::Frontend::EmuWindow* aux_window_) override;

private:
    void InterpolateFrames(Frame* prev_frame, Frame* curr_frame);
    Frame* previous_frame = nullptr;  // Store the previous frame for interpolation
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer command_buffer);
    void Report() const;

    vk::Buffer RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                              const Layout::FramebufferLayout& layout, VkFormat format,
                              VkDeviceSize buffer_size);
    void RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers);
    void RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers);

    // DSMod aux (second screen) output
    void SyncAuxWindow();
    void RenderAuxWindow(std::span<const Tegra::FramebufferConfig> layers);
    /// Presents the mod runtime's UI image on the aux window. Returns false when there is none.
    bool RenderAuxModUi();

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;

    // Keep original handles for compatibility with existing code
    vk::Instance instance;

    vk::DebugUtilsMessenger debug_messenger;

    vk::SurfaceKHR surface;

    Device device;
    MemoryAllocator memory_allocator;
    StateTracker state_tracker;
    Scheduler scheduler;
    Swapchain swapchain;
    PresentManager present_manager;
    BlitScreen blit_swapchain;
    BlitScreen blit_capture;
    BlitScreen blit_applet;
    RasterizerVulkan rasterizer;
#ifdef HAS_LSFG
    FrameGen frame_gen;
#endif
    std::optional<TurboMode> turbo_mode;

    Frame applet_frame;

    // DSMod aux output. Attach/detach requests are applied on the GPU thread (SyncAuxWindow);
    // a detach additionally blocks the caller until the surface is no longer in use, or force-
    // destroys under aux_mutex when the GPU thread is idle (paused), so the frontend may release
    // the native window right after SetAuxWindow(nullptr) returns.
    void DestroyAuxLocked();
    std::atomic<Core::Frontend::EmuWindow*> pending_aux_window{nullptr};
    std::atomic<bool> aux_dirty{false};
    std::mutex aux_mutex;
    std::condition_variable aux_cv;
    Core::Frontend::EmuWindow* aux_window{nullptr};
    std::optional<vk::SurfaceKHR> aux_surface;
    std::optional<Swapchain> aux_swapchain;
    std::optional<PresentManager> aux_present_manager;
    std::optional<BlitScreen> blit_aux;
    vk::RenderPass aux_ui_render_pass;
    vk::Image aux_ui_image;
    // Persistent staging buffer for the aux-UI upload. The old path allocated a throwaway staging
    // buffer per republish and called scheduler.Finish() to keep it alive until the GPU copy ran --
    // a full blocking GPU drain on the shared scheduler, on the render thread, ahead of the guest
    // frame's own present in Composite(). Keeping the buffer alive across frames lets the copy be
    // recorded and left in flight (no Finish); aux_ui_upload_tick fences reuse so we never scribble
    // over a copy the GPU has not consumed yet.
    vk::Buffer aux_ui_staging;
    size_t aux_ui_staging_bytes{};
    u64 aux_ui_upload_tick{};
    u64 aux_ui_blit_tick{};  ///< last blit that read aux_ui_image; retire before replacing it
    u32 aux_ui_w{};
    u32 aux_ui_h{};
    u64 aux_ui_serial{};
    bool aux_ui_presented{};  ///< the current UI has been blitted+presented at least once
    u32 aux_ui_last_w{};
    u32 aux_ui_last_h{};
    bool aux_ui_last_full{true}; ///< Eden Duo: the last present filled the frame (no Fit bars)
    // Reused compact scratch buffer for a partial (dirty-sub-rect) upload: RenderAuxModUi extracts
    // just the dirty rows out of the full canvas into here (mirrors AuxRouting::PublishUiPartial's
    // own row-bounded copy), so UploadImageAsync's staging copy is proportional to the dirty area,
    // not the whole ~5 MB canvas. Grows only, like aux_ui_staging itself.
    std::vector<u32> aux_ui_partial_scratch;

    // ------------- GPU composite path (EDEN_DSMOD_GPU_COMPOSITE) -------------
    // One coverage-blend pipeline that draws a display list of textured quads over up to NumAuxTex
    // cached source textures (0=map area, 1=icon atlas, 2=HUD overlay) into the aux frame's render
    // pass. Textures re-upload async (persistent per-slot staging, no Finish) only when their
    // serial moves; the marker/map-pan is just new quad geometry per frame.
    static constexpr u32 NumAuxTex = VideoCore::DSMod::AuxRouting::NumAuxTex;
    void EnsureAuxCompositeResources();
    bool RenderAuxModUiGpu();
    bool aux_c_init{false};
    vk::DescriptorSetLayout aux_c_set_layout;
    vk::DescriptorSetLayout aux_c_map_set_layout;
    vk::PipelineLayout aux_c_pipe_layout;
    vk::PipelineLayout aux_c_map_pipe_layout;
    vk::ShaderModule aux_c_vert;
    vk::ShaderModule aux_c_frag;
    vk::ShaderModule aux_c_map_frag;
    vk::RenderPass aux_c_render_pass;
    vk::Pipeline aux_c_pipeline;
    vk::Pipeline aux_c_map_pipeline;
    vk::Sampler aux_c_sampler_nearest;
    vk::DescriptorPool aux_c_pool;
    vk::DescriptorPool aux_c_map_pool;
    vk::DescriptorSets aux_c_sets;
    vk::DescriptorSets aux_c_map_set;
    bool aux_c_map_desc_ready{false};
    std::array<vk::Image, NumAuxTex> aux_c_img;
    std::array<vk::ImageView, NumAuxTex> aux_c_view;
    std::array<vk::Buffer, NumAuxTex> aux_c_staging;
    std::array<size_t, NumAuxTex> aux_c_staging_bytes{};
    std::array<u32, NumAuxTex> aux_c_w{};
    std::array<u32, NumAuxTex> aux_c_h{};
    std::array<u64, NumAuxTex> aux_c_serial{};
    std::array<u64, NumAuxTex> aux_c_upload_tick{};
    u64 aux_c_draw_tick{};  ///< last recorded quad draw; retire before replacing a sampled image
    // Last composite taken from the mod: kept so the panel can be re-presented (resize, reattach)
    // without a new publish, and held untouched when nothing changed.
    std::vector<VideoCore::DSMod::AuxRouting::Quad> aux_c_quads;
    u32 aux_c_cw{};
    u32 aux_c_ch{};
    u32 aux_c_bg{};
    u64 aux_c_comp_serial{};
    bool aux_c_dirty{false};      ///< quads or a texture changed since the last present
    bool aux_c_presented{false};
    u32 aux_c_last_w{};           ///< panel size at the last present
    u32 aux_c_last_h{};
    bool aux_c_last_full{true};   ///< Eden Duo: the last present filled the frame (no Fit bars)
    u64 aux_c_present_count{};
    std::array<u64, NumAuxTex> aux_c_upload_count{};
    vk::Buffer aux_c_readback_buf;
    u32 aux_c_readback_w{};
    u32 aux_c_readback_h{};
};

} // namespace Vulkan
