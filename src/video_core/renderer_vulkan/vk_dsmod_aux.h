// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo: the Vulkan presenter for the aux ("second screen") window. RendererVulkan owns one
// instance and calls into it from Composite (Route) and SetAuxWindow; everything else about the
// second screen -- its surface, swapchain, present manager, the mod UI upload/blit path and the
// GPU-composite quad pipeline -- lives here so the upstream renderer only carries hook calls.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/present/dsmod_companion_quad.h" // Eden Duo Stage 1: CompanionQuad
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class EmuWindow;
}

namespace Layout {
struct FramebufferLayout;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
class RasterizerVulkan;
class Scheduler;

class DSModAuxPresenter {
public:
    explicit DSModAuxPresenter(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& device_memory,
                               const vk::Instance& instance, const Device& device,
                               MemoryAllocator& memory_allocator, Scheduler& scheduler,
                               RasterizerVulkan& rasterizer);
    ~DSModAuxPresenter();

    DSModAuxPresenter(const DSModAuxPresenter&) = delete;
    DSModAuxPresenter& operator=(const DSModAuxPresenter&) = delete;

    /// Attach (or detach with nullptr) the aux window. See RendererBase::SetAuxWindow.
    void SetAuxWindow(Core::Frontend::EmuWindow* aux_window_);

    /// Presents this frame's aux output (mod UI, GPU composite, bound layer or mirror) and returns
    /// the layers that stay on the primary screen. The returned span is valid until the next call.
    /// `main_window_layout`: Eden Duo Stage 1 (Simulated/inline dual screen) -- the main window's
    /// current size, needed to turn AuxRouting's inline fractions into pixels. Ignored while a
    /// real aux window is attached (Physical always wins; see GetInlineCompanionQuad).
    std::span<const Tegra::FramebufferConfig> Route(
        std::span<const Tegra::FramebufferConfig> framebuffers,
        const Layout::FramebufferLayout& main_window_layout);

    /// EDEN_NO_PRESENT (headless verification): never hand a primary frame to the window.
    static bool PrimaryPresentDisabled();

    /// Eden Duo Stage 1: this frame's companion quad for WindowAdaptPass to sample, filled by the
    /// Route() call that just returned (nothing else touches it meanwhile: both run on the GPU
    /// thread, sequentially, within RendererVulkan::Composite). Empty (`pane` zero-extent) when
    /// inline is not in effect -- a real aux window attached, the switch off, or the pane fraction
    /// resolving to nothing.
    [[nodiscard]] const CompanionQuad& GetInlineCompanionQuad() const { return inline_companion; }

    /// Eden Duo Stage 1: `tick` is the scheduler tick whose recording just sampled this frame's
    /// companion quad (RendererVulkan::Composite, right after WindowAdaptPass recorded it).
    /// Recreating the image behind it (a canvas or pane size change) waits for that tick first:
    /// the draw that wrote the image is not the last reader, the companion quad is.
    void NoteInlineSampled(u64 tick) {
        if (inline_companion.view != VK_NULL_HANDLE) {
            inline_sample_tick = tick;
        }
    }

    /// Eden Duo Stage 1: true while the game is drawing into its own rect of the window (not the
    /// whole thing) rather than upstream's normal full-window layout -- RendererVulkan::Composite
    /// skips LSFG frame generation while this holds (a generated frame would warp the companion
    /// quad WindowAdaptPass drew alongside the game, and frame_gen has no notion of it).
    [[nodiscard]] bool IsInlineActive() const { return inline_game_rect.has_value(); }

    /// Eden Duo Stage 1: `window_layout`'s screen rect, fitted into this frame's game rect the same
    /// way upstream's own DefaultFrameLayout fits it into the whole window (honouring
    /// Settings::values.aspect_ratio, including Stretch) and placed at the game rect's own
    /// position -- or `window_layout` unchanged when inline is not in effect.
    [[nodiscard]] Layout::FramebufferLayout FitInlineGameLayout(
        const Layout::FramebufferLayout& window_layout) const;

private:
    void SyncAuxWindow();
    void RenderAuxWindow(std::span<const Tegra::FramebufferConfig> layers);
    /// Shared by the physical (RenderAuxModUi) and Simulated/inline (RenderInlineModUi) paths:
    /// keeps aux_ui_image/aux_ui_view holding the mod runtime's latest canvas, uploading only
    /// what changed (or creating/resizing it on a shape change). Returns whether anything did.
    bool UpdateAuxUiImage();
    /// Eden Duo Stage 1: inline dispatch, called from Route() instead of the aux_window branch
    /// below when inline is active and no real aux window is attached. Returns the game's rect
    /// (main-window pixels) when the pane has a non-empty size, else nullopt.
    std::optional<VideoCore::DSMod::CompanionRect> RenderInline(
        const Layout::FramebufferLayout& main_window_layout);
    /// Fills inline_companion from the mod runtime's CPU canvas (aux_ui_image/aux_ui_view),
    /// Fit-ted inside `pane`. Returns false (and clears inline_companion) when there is no canvas
    /// yet.
    bool RenderInlineModUi(const VideoCore::DSMod::CompanionRect& pane);
    /// Fills inline_companion from the GPU-composite quad list, rendered into an offscreen image
    /// this presenter owns (aux_inline_image) instead of an aux swapchain frame. Returns false
    /// (and clears inline_companion) when there is nothing to composite yet.
    bool RenderInlineModUiGpu(const VideoCore::DSMod::CompanionRect& pane);
    void EnsureInlineCompositeImage(u32 w, u32 h);
    /// Presents the mod runtime's UI image on the aux window. Returns false when there is none.
    bool RenderAuxModUi();

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    const vk::Instance& instance;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    RasterizerVulkan& rasterizer;

    /// Storage for the primary layers when a bound layer is routed away (Route's result).
    boost::container::small_vector<Tegra::FramebufferConfig, 4> primary_layers;

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
    u64 aux_ui_blit_tick{}; ///< last blit that read aux_ui_image; retire before replacing it
    vk::ImageView aux_ui_view; ///< Eden Duo Stage 1: lets the inline quad sample aux_ui_image
    u32 aux_ui_w{};
    u32 aux_ui_h{};
    u64 aux_ui_serial{};
    bool aux_ui_presented{}; ///< the current UI has been blitted+presented at least once
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
    /// `color_format`: the render pass/pipelines are rebuilt for it when it differs from the one
    /// they were last built with (physical mode passes the aux swapchain's format; Simulated/
    /// inline mode, with no swapchain at all, passes a fixed one -- see RenderInlineModUiGpu).
    /// Mutual exclusion between the two modes means this never thrashes in practice.
    void EnsureAuxCompositeResources(VkFormat color_format);
    bool RenderAuxModUiGpu();
    /// Shared by RenderAuxModUiGpu (physical) and RenderInlineModUiGpu (Simulated/inline): takes
    /// the latest quad list + textures from the routing buffer and uploads whatever changed into
    /// aux_c_img/aux_c_quads. Returns false when there is nothing to composite yet (no canvas
    /// size published).
    bool UpdateAuxComposite();
    /// Records the quad list (aux_c_quads) into `dst_fb`'s render pass at `render_area`, Fit-ting
    /// into `fit` (relative to {0,0}, i.e. already local to the destination image) when
    /// `fit_full` is false. Shared by both GPU-composite call sites; ends with a barrier making
    /// the write visible to a later shader read of `dst_image` in the same frame (matches the
    /// FXAA/SMAA passes' own convention for a same-frame write-then-sample).
    void RecordCompositeDraws(VkFramebuffer dst_fb, VkImage dst_image, VkExtent2D render_area,
                              VideoCore::DSMod::CompanionRect fit, bool fit_full);
    bool aux_c_init{false};
    VkFormat aux_c_format{}; ///< the format aux_c_render_pass/pipelines were last built for
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
    u64 aux_c_draw_tick{}; ///< last recorded quad draw; retire before replacing a sampled image
    // Last composite taken from the mod: kept so the panel can be re-presented (resize, reattach)
    // without a new publish, and held untouched when nothing changed.
    std::vector<VideoCore::DSMod::AuxRouting::Quad> aux_c_quads;
    u32 aux_c_cw{};
    u32 aux_c_ch{};
    u32 aux_c_bg{};
    u64 aux_c_comp_serial{};
    bool aux_c_dirty{false}; ///< quads or a texture changed since the last present
    bool aux_c_presented{false};
    u32 aux_c_last_w{}; ///< panel size at the last present
    u32 aux_c_last_h{};
    bool aux_c_last_full{true}; ///< Eden Duo: the last present filled the frame (no Fit bars)
    u64 aux_c_present_count{};
    std::array<u64, NumAuxTex> aux_c_upload_count{};
    vk::Buffer aux_c_readback_buf;
    u32 aux_c_readback_w{};
    u32 aux_c_readback_h{};

    // ------------- Eden Duo Stage 1: Simulated (single-surface) dual screen -------------
    // The offscreen image RenderInlineModUiGpu renders the composite into, 1:1 at the canvas's
    // own size (no letterboxing here -- that happens once, in WindowAdaptPass, against the
    // companion pane). Recreated on a canvas size change, like aux_c_img's slots.
    vk::Image aux_inline_image;
    vk::ImageView aux_inline_view;
    vk::Framebuffer aux_inline_framebuffer;
    u32 aux_inline_w{};
    u32 aux_inline_h{};

    /// This frame's companion quad (GetInlineCompanionQuad) and game rect (FitInlineGameLayout),
    /// written by RenderInline/RenderInlineModUi(Gpu) during Route(), read right after by
    /// RendererVulkan::Composite on the same (GPU) thread.
    CompanionQuad inline_companion;
    std::optional<VideoCore::DSMod::CompanionRect> inline_game_rect;
    u64 inline_sample_tick{}; ///< last frame that sampled inline_companion (NoteInlineSampled)
    bool inline_was_active{}; ///< last Route() ran RenderInline (it published the pane as aux)
};

} // namespace Vulkan
