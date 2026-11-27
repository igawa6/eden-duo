// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include "common/frontend/framebuffer_layout.h"
#include "video_core/renderer_vulkan/present/dsmod_companion_quad.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

CompanionQuadRenderer::CompanionQuadRenderer(const Device& device) {
    // Samples with the same filtering the physical aux blit used (VK_FILTER_LINEAR in
    // DSModAuxPresenter::RenderAuxModUi), not the overlay panels' pixel-for-pixel nearest (their
    // crisp text would blur, but the companion is a whole page meant to scale like the game does).
    sampler = CreateBilinearSampler(device);
}

CompanionQuadRenderer::Draw CompanionQuadRenderer::Prepare(
    const Device& device, Scheduler& scheduler, VkDescriptorSetLayout descriptor_set_layout,
    const Layout::FramebufferLayout& layout, const CompanionQuad* companion) {
    // The pane is cleared to the companion's background first, unless the quad that follows
    // already covers all of it: with no image yet (first frame, nothing published) the clear
    // alone is the whole picture; with Fit bars it is just the bars, the quad overdrawing the
    // rest; with a full-pane fit the clear is skipped entirely (the quad would overdraw it).
    Draw draw{};
    if (companion == nullptr || companion->pane.extent.width == 0 ||
        companion->pane.extent.height == 0) {
        return draw;
    }
    draw.pane = true;
    draw.pane_rect = companion->pane;
    draw.background = companion->background;
    if (companion->view != VK_NULL_HANDLE) {
        if (!pool) {
            pool = CreateWrappedDescriptorPool(device, 1, 1);
            const std::vector layouts(1u, descriptor_set_layout);
            sets = CreateWrappedDescriptorSets(pool, layouts);
        }
        // Content (version) changes need no rebind at all: the producer writes new pixels into
        // the SAME image/view, which this binding already points at, and sampling always sees
        // current GPU memory. Only a view-handle change (the producer resized and recreated its
        // image) needs one -- and only after any command buffer that may still be executing with
        // the OLD binding has retired (Vulkan forbids updating a descriptor set referenced by a
        // pending submission without UPDATE_AFTER_BIND).
        if (bound_view != companion->view) {
            scheduler.Wait(used_tick);
            const VkDescriptorImageInfo image_info{
                .sampler = *sampler,
                .imageView = companion->view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = sets[0],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_info,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            };
            device.GetLogical().UpdateDescriptorSets(std::array{write}, {});
            bound_view = companion->view;
        }
        draw.push.modelview_matrix =
            MakeOrthographicMatrix(f32(layout.width), static_cast<f32>(layout.height));
        const f32 fx = static_cast<f32>(companion->fit.offset.x);
        const f32 fy = static_cast<f32>(companion->fit.offset.y);
        const f32 fw = static_cast<f32>(companion->fit.extent.width);
        const f32 fh = static_cast<f32>(companion->fit.extent.height);
        draw.push.vertices[0] = ScreenRectVertex(fx, fy, 0.f, 0.f);
        draw.push.vertices[1] = ScreenRectVertex(fx + fw, fy, 1.f, 0.f);
        draw.push.vertices[2] = ScreenRectVertex(fx, fy + fh, 0.f, 1.f);
        draw.push.vertices[3] = ScreenRectVertex(fx + fw, fy + fh, 1.f, 1.f);
        draw.set = sets[0];
        draw.quad = true;
        // This tick's recording (Record(), right after) will reference sets[0]; retired before a
        // future rebind replaces its binding (see the Wait above).
        used_tick = scheduler.CurrentTick();
    }
    // The quad already covers the whole pane: the clear above would be fully overdrawn, so skip
    // it (one fewer ClearAttachments per frame in the common no-bars case).
    if (draw.quad && companion->fit_is_pane) {
        draw.pane = false;
    }
    return draw;
}

void CompanionQuadRenderer::Record(vk::CommandBuffer cmdbuf, VkPipelineLayout pipeline_layout,
                                   VkPipeline pipeline, const Draw& draw) const {
    if (draw.pane) {
        const f32 cr = static_cast<f32>((draw.background >> 16) & 0xFF) / 255.0f;
        const f32 cg = static_cast<f32>((draw.background >> 8) & 0xFF) / 255.0f;
        const f32 cb = static_cast<f32>(draw.background & 0xFF) / 255.0f;
        const f32 ca = static_cast<f32>((draw.background >> 24) & 0xFF) / 255.0f;
        const VkClearAttachment pane_clear{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0,
            .clearValue = {.color = {.float32 = {cr, cg, cb, ca}}},
        };
        const VkClearRect pane_clear_rect{
            .rect = draw.pane_rect,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        cmdbuf.ClearAttachments({pane_clear}, {pane_clear_rect});
    }
    if (draw.quad) {
        // The overlay panels' own pipeline: plain present shader (pixel for pixel, no
        // upscaling-filter ringing/sharpening on UI content) with coverage blending, same
        // reasoning as the controller hints drawn right after.
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.PushConstants(pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, draw.push);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, draw.set, {});
        cmdbuf.Draw(4, 1, 0, 0);
    }
}

} // namespace Vulkan
