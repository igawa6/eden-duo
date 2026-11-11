// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/frontend/framebuffer_layout.h"
#include "video_core/framebuffer_config.h"
#include "video_core/host_shaders/vulkan_present_frag_spv.h"
#include "video_core/host_shaders/vulkan_present_vert_spv.h"
#include "video_core/renderer_vulkan/present/layer.h"
#include "video_core/renderer_vulkan/present/present_push_constants.h"
#include "video_core/renderer_vulkan/present/util.h"
#include <mutex>

#include "common/overlay_panels.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/present/present_push_constants.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/present/window_adapt_pass.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

WindowAdaptPass::WindowAdaptPass(const Device& device, MemoryAllocator& memory_allocator_,
                             VkFormat frame_format, vk::Sampler&& sampler_,
                             vk::ShaderModule&& fragment_shader_)
    : memory_allocator{memory_allocator_}
    , sampler(std::move(sampler_))
    , fragment_shader(std::move(fragment_shader_))
{
    CreateDescriptorSetLayout(device);
    CreatePipelineLayout(device);
    CreateVertexShader(device);
    CreateRenderPass(device, frame_format);
    CreatePipelines(device);
    overlay_shader = BuildShader(device, VULKAN_PRESENT_FRAG_SPV);
    overlay_sampler = CreateNearestNeighborSampler(device);
    overlay_pipeline = CreateWrappedCoverageBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, overlay_shader));
    // Eden Duo Stage 1: the companion quad samples with the same filtering the physical aux
    // blit used (VK_FILTER_LINEAR in DSModAuxPresenter::RenderAuxModUi), not the overlay panels'
    // pixel-for-pixel nearest (their crisp text would blur, but the companion is a whole page
    // meant to scale like the game does).
    companion_sampler = CreateBilinearSampler(device);
}

WindowAdaptPass::~WindowAdaptPass() = default;

void WindowAdaptPass::Draw(const Device& device, RasterizerVulkan& rasterizer, Scheduler& scheduler, size_t image_index,
                           std::list<Layer>& layers,
                           std::span<const Tegra::FramebufferConfig> configs,
                           const Layout::FramebufferLayout& layout, Frame* dst,
                           bool overlay_panels, const CompanionQuad* companion) {

    const VkFramebuffer host_framebuffer{*dst->framebuffer};
    const VkRenderPass renderpass{*render_pass};
    const VkPipelineLayout graphics_pipeline_layout{*pipeline_layout};
    const VkExtent2D render_area{
        .width = dst->width,
        .height = dst->height,
    };

    const size_t layer_count = configs.size();
    std::vector<PresentPushConstants> push_constants(layer_count);
    std::vector<VkDescriptorSet> descriptor_sets(layer_count);
    std::vector<VkPipeline> graphics_pipelines(layer_count);

    auto layer_it = layers.begin();
    for (size_t i = 0; i < layer_count; i++) {
        switch (configs[i].blending) {
        case Tegra::BlendMode::Opaque:
        default:
            graphics_pipelines[i] = *opaque_pipeline;
            break;
        case Tegra::BlendMode::Premultiplied:
            graphics_pipelines[i] = *premultiplied_pipeline;
            break;
        case Tegra::BlendMode::Coverage:
            graphics_pipelines[i] = *coverage_pipeline;
            break;
        }

        layer_it->ConfigureDraw(device, &push_constants[i], &descriptor_sets[i], rasterizer, *sampler,
                                image_index, configs[i], layout);
        layer_it++;
    }

    // The overlay panels (see common/overlay_panels.h): one more quad each, straight-alpha,
    // drawn last.
    std::array<bool, Common::OverlayPanelCount> overlay_draw{};
    std::array<PresentPushConstants, Common::OverlayPanelCount> overlay_push{};
    std::array<VkDescriptorSet, Common::OverlayPanelCount> overlay_set{};
    auto& panels = Common::GetOverlayPanels();
    for (size_t o = 0; overlay_panels && o < panels.size(); ++o) {
        auto& overlay_frame = panels[o];
        auto& quad = overlays[o];
        std::scoped_lock lock{overlay_frame.mutex};
        if (!overlay_frame.enabled || overlay_frame.pixels.empty() || overlay_frame.width == 0 ||
            overlay_frame.height == 0) {
            continue;
        }
        constexpr VkFormat overlay_format = VK_FORMAT_R8G8B8A8_UNORM;
        const VkExtent2D extent{overlay_frame.width, overlay_frame.height};
        if (!quad.ready || quad.extent.width != extent.width ||
            quad.extent.height != extent.height) {
            // (A frame still on the GPU may be drawing the old image: replaced once it's done)
            scheduler.Wait(quad.used_tick);
            quad.image = CreateWrappedImage(memory_allocator, extent, overlay_format);
            quad.view = CreateWrappedImageView(device, quad.image, overlay_format);
            quad.pool = CreateWrappedDescriptorPool(device, 1, 1);
            const std::vector layouts(1u, *descriptor_set_layout);
            quad.sets = CreateWrappedDescriptorSets(quad.pool, layouts);
            quad.extent = extent;
            quad.ready = true;
            quad.version = 0;
        }
        if (quad.version != overlay_frame.version) {
            // Copied through one of the panel's two staging buffers: only that buffer's last
            // copy is waited for, never the whole GPU
            const std::size_t k = quad.next_staging;
            quad.next_staging ^= 1;
            scheduler.Wait(quad.staging_tick[k]);
            auto& staging = quad.staging[k];
            const std::size_t bytes = overlay_frame.pixels.size();
            if (!staging || quad.staging_size[k] < bytes) {
                const VkBufferCreateInfo staging_ci{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = bytes,
                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                };
                staging = memory_allocator.CreateBuffer(staging_ci, MemoryUsage::Upload);
                quad.staging_size[k] = bytes;
            }
            std::ranges::copy(overlay_frame.pixels, staging.Mapped().begin());
            staging.Flush();
            const VkBuffer source = *staging;
            const VkImage target = *quad.image;
            const VkBufferImageCopy region{
                .bufferOffset = 0,
                .bufferRowLength = extent.width,
                .bufferImageHeight = extent.height,
                .imageSubresource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
                .imageOffset{},
                .imageExtent{.width = extent.width, .height = extent.height, .depth = 1},
            };
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([source, target, region](vk::CommandBuffer cmdbuf) {
                TransitionImageLayout(cmdbuf, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_UNDEFINED);
                cmdbuf.CopyBufferToImage(source, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         std::array{region});
                TransitionImageLayout(cmdbuf, target, VK_IMAGE_LAYOUT_GENERAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            });
            quad.staging_tick[k] = scheduler.CurrentTick();
            quad.version = overlay_frame.version;
            const VkDescriptorImageInfo image_info{
                .sampler = *overlay_sampler,
                .imageView = *quad.view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = quad.sets[0],
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_info,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            };
            device.GetLogical().UpdateDescriptorSets(std::array{write}, {});
        }
        overlay_set[o] = quad.sets[0];

        auto& push = overlay_push[o];
        push.modelview_matrix =
            MakeOrthographicMatrix(f32(layout.width), static_cast<f32>(layout.height));
        const auto& screen = layout.screen;
        const auto place = Common::PlaceOverlayPanel(overlay_frame, f32(screen.left),
                                                     f32(screen.top), f32(screen.right),
                                                     f32(screen.bottom));
        const f32 w = place.width;
        const f32 h = place.height;
        const f32 x = place.x;
        const f32 y = place.y;
        push.vertices[0] = ScreenRectVertex(x, y, 0.f, 0.f);
        push.vertices[1] = ScreenRectVertex(x + w, y, 1.f, 0.f);
        push.vertices[2] = ScreenRectVertex(x, y + h, 0.f, 1.f);
        push.vertices[3] = ScreenRectVertex(x + w, y + h, 1.f, 1.f);
        overlay_draw[o] = true;
        quad.used_tick = scheduler.CurrentTick();

        static u32 logged_quads = 0;
        if (logged_quads < 3) {
            ++logged_quads;
            LOG_INFO(Render_Vulkan, "overlay: quad drawn {}x{} at {},{}", extent.width,
                     extent.height, x, y);
        }
    }

    // Eden Duo Stage 1 (Simulated/inline dual screen): one quad sampling the companion image the
    // presenter already owns -- drawn after the game layers, before the overlay panels, only when
    // inline is actually active this frame (zero cost otherwise). See CompanionQuad's doc comment.
    //
    // The pane is cleared to the companion's background first, unless the quad that follows
    // already covers all of it: with no image yet (first frame, nothing published) the clear
    // alone is the whole picture; with Fit bars it is just the bars, the quad overdrawing the
    // rest; with a full-pane fit the clear is skipped entirely (the quad would overdraw it).
    bool draw_companion_pane = false;
    bool draw_companion_quad = false;
    PresentPushConstants companion_push{};
    VkDescriptorSet companion_set{};
    VkRect2D companion_pane{};
    u32 companion_bg = 0xFF000000u;
    if (companion != nullptr && companion->pane.extent.width > 0 &&
        companion->pane.extent.height > 0) {
        companion_pane = companion->pane;
        companion_bg = companion->background;
        draw_companion_pane = true;
        if (companion->view != VK_NULL_HANDLE) {
            if (!companion_pool) {
                companion_pool = CreateWrappedDescriptorPool(device, 1, 1);
                const std::vector layouts(1u, *descriptor_set_layout);
                companion_sets = CreateWrappedDescriptorSets(companion_pool, layouts);
            }
            // Content (version) changes need no rebind at all: the producer writes new pixels
            // into the SAME image/view, which this binding already points at, and sampling
            // always sees current GPU memory. Only a view-handle change (the producer resized
            // and recreated its image) needs one -- and only after any command buffer that may
            // still be executing with the OLD binding has retired (Vulkan forbids updating a
            // descriptor set referenced by a pending submission without UPDATE_AFTER_BIND).
            if (companion_bound_view != companion->view) {
                scheduler.Wait(companion_used_tick);
                const VkDescriptorImageInfo image_info{
                    .sampler = *companion_sampler,
                    .imageView = companion->view,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                };
                const VkWriteDescriptorSet write{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = companion_sets[0],
                    .dstBinding = 0,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &image_info,
                    .pBufferInfo = nullptr,
                    .pTexelBufferView = nullptr,
                };
                device.GetLogical().UpdateDescriptorSets(std::array{write}, {});
                companion_bound_view = companion->view;
            }
            companion_push.modelview_matrix =
                MakeOrthographicMatrix(f32(layout.width), static_cast<f32>(layout.height));
            const f32 fx = static_cast<f32>(companion->fit.offset.x);
            const f32 fy = static_cast<f32>(companion->fit.offset.y);
            const f32 fw = static_cast<f32>(companion->fit.extent.width);
            const f32 fh = static_cast<f32>(companion->fit.extent.height);
            companion_push.vertices[0] = ScreenRectVertex(fx, fy, 0.f, 0.f);
            companion_push.vertices[1] = ScreenRectVertex(fx + fw, fy, 1.f, 0.f);
            companion_push.vertices[2] = ScreenRectVertex(fx, fy + fh, 0.f, 1.f);
            companion_push.vertices[3] = ScreenRectVertex(fx + fw, fy + fh, 1.f, 1.f);
            companion_set = companion_sets[0];
            draw_companion_quad = true;
            // This tick's recording (below) will reference companion_sets[0]; retired before a
            // future rebind replaces its binding (see the Wait above).
            companion_used_tick = scheduler.CurrentTick();
        }
        // The quad already covers the whole pane: the clear above would be fully overdrawn, so
        // skip it (one fewer ClearAttachments per frame in the common no-bars case).
        if (draw_companion_quad && companion->fit_is_pane) {
            draw_companion_pane = false;
        }
    }

    scheduler.Record([=, this](vk::CommandBuffer cmdbuf) {
        const f32 bg_red = Settings::values.bg_red.GetValue() / 255.0f;
        const f32 bg_green = Settings::values.bg_green.GetValue() / 255.0f;
        const f32 bg_blue = Settings::values.bg_blue.GetValue() / 255.0f;
        const VkClearAttachment clear_attachment{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0,
            .clearValue =
                {
                    .color = {.float32 = {bg_red, bg_green, bg_blue, 1.0f}},
                },
        };
        const VkClearRect clear_rect{
            .rect =
                {
                    .offset = {0, 0},
                    .extent = render_area,
                },
            .baseArrayLayer = 0,
            .layerCount = 1,
        };

        BeginRenderPass(cmdbuf, renderpass, host_framebuffer, render_area);
        cmdbuf.ClearAttachments({clear_attachment}, {clear_rect});

        for (size_t i = 0; i < layer_count; i++) {
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipelines[i]);
            cmdbuf.PushConstants(graphics_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                                 push_constants[i]);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_layout, 0,
                                      descriptor_sets[i], {});
            cmdbuf.Draw(4, 1, 0, 0);
        }

        // Eden Duo Stage 1: companion pane, after the game layers, before the overlay panels.
        if (draw_companion_pane) {
            const f32 cr = static_cast<f32>((companion_bg >> 16) & 0xFF) / 255.0f;
            const f32 cg = static_cast<f32>((companion_bg >> 8) & 0xFF) / 255.0f;
            const f32 cb = static_cast<f32>(companion_bg & 0xFF) / 255.0f;
            const f32 ca = static_cast<f32>((companion_bg >> 24) & 0xFF) / 255.0f;
            const VkClearAttachment pane_clear{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
                .clearValue = {.color = {.float32 = {cr, cg, cb, ca}}},
            };
            const VkClearRect pane_rect{
                .rect = companion_pane,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            cmdbuf.ClearAttachments({pane_clear}, {pane_rect});
        }
        if (draw_companion_quad) {
            // The overlay panels' own pipeline: plain present shader (pixel for pixel, no
            // upscaling-filter ringing/sharpening on UI content) with coverage blending, same
            // reasoning as the controller hints just below.
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *overlay_pipeline);
            cmdbuf.PushConstants(graphics_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                                 companion_push);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_layout, 0,
                                      companion_set, {});
            cmdbuf.Draw(4, 1, 0, 0);
        }

        for (size_t o = 0; o < overlay_draw.size(); ++o) {
            if (!overlay_draw[o]) {
                continue;
            }
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *overlay_pipeline);
            cmdbuf.PushConstants(graphics_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                                 overlay_push[o]);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_layout,
                                      0, overlay_set[o], {});
            cmdbuf.Draw(4, 1, 0, 0);
        }

        cmdbuf.EndRenderPass();
    });
}

VkDescriptorSetLayout WindowAdaptPass::GetDescriptorSetLayout() {
    return *descriptor_set_layout;
}

VkRenderPass WindowAdaptPass::GetRenderPass() {
    return *render_pass;
}

void WindowAdaptPass::CreateDescriptorSetLayout(const Device& device) {
    descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
}

void WindowAdaptPass::CreatePipelineLayout(const Device& device) {
    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PresentPushConstants),
    };

    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
}

void WindowAdaptPass::CreateVertexShader(const Device& device) {
    vertex_shader = BuildShader(device, VULKAN_PRESENT_VERT_SPV);
}

void WindowAdaptPass::CreateRenderPass(const Device& device, VkFormat frame_format) {
    render_pass = CreateWrappedRenderPass(device, frame_format, VK_IMAGE_LAYOUT_UNDEFINED);
}

void WindowAdaptPass::CreatePipelines(const Device& device) {
    opaque_pipeline = CreateWrappedPipeline(device, render_pass, pipeline_layout,
                                            std::tie(vertex_shader, fragment_shader));
    premultiplied_pipeline = CreateWrappedPremultipliedBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, fragment_shader));
    coverage_pipeline = CreateWrappedCoverageBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, fragment_shader));
}

} // namespace Vulkan
