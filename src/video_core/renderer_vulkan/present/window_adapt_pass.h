// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <list>

#include "common/math_util.h"
#include "common/overlay_panels.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Tegra {
struct FramebufferConfig;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
struct Frame;
class Layer;
class Scheduler;
class RasterizerVulkan;

// Eden Duo Stage 1 ("Simulated"/inline dual screen, single-surface composition): the companion
// image sampled as one more quad of the main window's own frame, drawn after the game layers but
// before the overlay panels (controller hints stay on top). The presenter that owns the
// companion image (DSModAuxPresenter) fills this each frame; `view == VK_NULL_HANDLE` (the
// default) means nothing to draw. A top-level struct (not nested in WindowAdaptPass) so a caller
// that only passes a pointer through (BlitScreen) can forward-declare it instead of pulling in
// this whole header.
struct CompanionQuad {
    VkImageView view{};
    VkExtent2D extent{};
    /// Bumps whenever the image's content changed; together with `view` tells Draw() when a
    /// cached descriptor binding is stale. Sampling itself always sees the image's current GPU
    /// memory, version or not -- this is only for the "does the binding need updating" decision.
    std::uint64_t version{};
    // Window pixels. `pane` is the whole area the companion owns (cleared to `background` first);
    // `fit` is the aspect-correct sub-rect of it the image is actually drawn into (bars outside
    // it, inside `pane`, are the clear colour).
    VkRect2D pane{};
    VkRect2D fit{};
    bool fit_is_pane{true}; ///< `fit` already covers the whole `pane`: skip the background clear
    u32 background{0xFF000000u}; ///< 0xAARRGGBB, matches AuxRouting::ui_bg
};

class WindowAdaptPass final {
public:
    explicit WindowAdaptPass(const Device& device, MemoryAllocator& memory_allocator,
                             VkFormat frame_format, vk::Sampler&& sampler,
                             vk::ShaderModule&& fragment_shader);
    ~WindowAdaptPass();

    /// (With the overlay panels on top only where asked: the screen's, not a capture)
    /// `companion`: Eden Duo Stage 1, see CompanionQuad above. Null (the default) or an empty
    /// (zero-extent `pane`) quad draws nothing extra -- zero cost when inline dual screen is off.
    void Draw(const Device& device, RasterizerVulkan& rasterizer, Scheduler& scheduler, size_t image_index,
              std::list<Layer>& layers, std::span<const Tegra::FramebufferConfig> configs,
              const Layout::FramebufferLayout& layout, Frame* dst, bool overlay_panels = false,
              const CompanionQuad* companion = nullptr);

    VkDescriptorSetLayout GetDescriptorSetLayout();
    VkRenderPass GetRenderPass();

private:
    void CreateDescriptorSetLayout(const Device& device);
    void CreatePipelineLayout(const Device& device);
    void CreateVertexShader(const Device& device);
    void CreateRenderPass(const Device& device, VkFormat frame_format);
    void CreatePipelines(const Device& device);

    MemoryAllocator& memory_allocator;

    vk::DescriptorSetLayout descriptor_set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Sampler sampler;
    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;
    vk::RenderPass render_pass;
    vk::Pipeline opaque_pipeline;
    vk::Pipeline premultiplied_pipeline;
    vk::Pipeline coverage_pipeline;

    // The overlay panels' own pipeline: the plain present shader, pixel for pixel (the scaling
    // filter's shader would blur their text)
    vk::ShaderModule overlay_shader;
    vk::Sampler overlay_sampler;
    vk::Pipeline overlay_pipeline;

    // The overlay panels (common/overlay_panels.h): a straight-alpha quad each, composited on
    // top of the layers, for both the accelerated and the unaccelerated present paths.
    struct OverlayQuad {
        vk::Image image;
        vk::ImageView view;
        vk::DescriptorPool pool;
        vk::DescriptorSets sets;
        VkExtent2D extent{};
        std::uint64_t version = 0;
        bool ready = false;
        /// Staging buffers, used in turn: each is written again only once the GPU is past its
        /// last copy (normally no wait)
        std::array<vk::Buffer, 2> staging;
        std::array<std::size_t, 2> staging_size{};
        std::array<std::uint64_t, 2> staging_tick{};
        std::size_t next_staging = 0;
        /// The GPU tick that last drew the image: replaced only once the GPU is past it
        std::uint64_t used_tick = 0;
    };
    std::array<OverlayQuad, Common::OverlayPanelCount> overlays;

    // Eden Duo Stage 1: the companion quad's own descriptor (see CompanionQuad above). Unlike an
    // OverlayQuad it owns no image and does no CPU upload -- the presenter's image already holds
    // current content; only the descriptor binding needs to track which image/version it last
    // pointed at.
    vk::Sampler companion_sampler;
    vk::DescriptorPool companion_pool;
    vk::DescriptorSets companion_sets;
    VkImageView companion_bound_view{};
    std::uint64_t companion_bound_version{~std::uint64_t{0}};
};

} // namespace Vulkan
