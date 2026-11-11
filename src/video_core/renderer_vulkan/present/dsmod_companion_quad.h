// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo Stage 1 ("Simulated"/inline dual screen, single-surface composition): the companion
// quad WindowAdaptPass::Draw samples as one more quad of the main window's own frame, drawn after
// the game layers but before the overlay panels (controller hints stay on top). Everything about
// it -- the descriptor/sampler it owns and the two draw steps (pane clear, then quad) -- lives
// here so the upstream file only carries a member, a Prepare() call and a Record() call.

#pragma once

#include <cstdint>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/present/present_push_constants.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {

class Device;
class Scheduler;

// The presenter that owns the companion image (DSModAuxPresenter) fills one of these each frame;
// `view == VK_NULL_HANDLE` (the default) means nothing to draw. A top-level struct (not nested in
// WindowAdaptPass or CompanionQuadRenderer) so a caller that only passes a pointer through
// (BlitScreen) can forward-declare it instead of pulling in this header.
struct CompanionQuad {
    VkImageView view{};
    VkExtent2D extent{};
    /// Bumps whenever the image's content changed. Informational only -- Draw() samples the
    /// image's current GPU memory regardless, and a content-only change needs no descriptor
    /// rebind (the producer writes new pixels into the same `view`); only `view` itself changing
    /// (the producer resized and recreated its image) does.
    std::uint64_t version{};
    // Window pixels. `pane` is the whole area the companion owns (cleared to `background` first);
    // `fit` is the aspect-correct sub-rect of it the image is actually drawn into (bars outside
    // it, inside `pane`, are the clear colour).
    VkRect2D pane{};
    VkRect2D fit{};
    bool fit_is_pane{true}; ///< `fit` already covers the whole `pane`: skip the background clear
    u32 background{0xFF000000u}; ///< 0xAARRGGBB, matches AuxRouting::ui_bg
};

/// Owns the companion quad's sampled-image descriptor and knows how to draw it; WindowAdaptPass
/// owns one instance alongside its own overlay-panel resources.
class CompanionQuadRenderer {
public:
    explicit CompanionQuadRenderer(const Device& device);

    /// This frame's prepared draw, built by Prepare() and consumed by Record(). Both empty
    /// (the default) when `companion` was null or empty -- Record() then draws nothing.
    struct Draw {
        bool pane = false;
        bool quad = false;
        PresentPushConstants push{};
        VkDescriptorSet set{};
        VkRect2D pane_rect{};
        u32 background = 0xFF000000u;
    };

    /// Synchronous prep (called from WindowAdaptPass::Draw before its deferred scheduler.Record):
    /// rebinds the sampled-image descriptor if the producer's view changed -- waiting out any
    /// command buffer still in flight with the old binding first, since Vulkan forbids updating a
    /// descriptor set referenced by a pending submission without UPDATE_AFTER_BIND -- and builds
    /// this frame's push constants and clear rect. A content-only change (same view, new pixels)
    /// needs no rebind: sampling always sees the image's current GPU memory.
    Draw Prepare(const Device& device, Scheduler& scheduler, VkDescriptorSetLayout descriptor_set_layout,
                 const Layout::FramebufferLayout& layout, const CompanionQuad* companion);

    /// Records `draw`'s pane clear (if any) and quad draw (if any) into `cmdbuf` -- called from
    /// inside WindowAdaptPass::Draw's deferred scheduler.Record lambda, between the game layers
    /// and the overlay panels, so the draw order (game -> companion -> overlay) matches exactly.
    /// `pipeline`/`pipeline_layout`: WindowAdaptPass's own overlay pipeline (plain present shader,
    /// coverage blending), reused verbatim -- no separate pipeline for the companion quad.
    void Record(vk::CommandBuffer cmdbuf, VkPipelineLayout pipeline_layout, VkPipeline pipeline,
                const Draw& draw) const;

private:
    vk::Sampler sampler;
    vk::DescriptorPool pool;
    vk::DescriptorSets sets;
    /// The view `sets[0]` currently points at; rebind only when this changes (see Prepare()).
    VkImageView bound_view{};
    /// Last tick whose recorded draw referenced `sets[0]`; waited on before rewriting the
    /// binding, so a still-in-flight command buffer is never updated out from under it.
    std::uint64_t used_tick{};
};

} // namespace Vulkan
