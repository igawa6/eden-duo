// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <thread>

#include "common/logging.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/renderer_base.h"

namespace VideoCore {

namespace {
// Serializes screenshot request handoffs between the requesting threads (guest capture
// service, frontend trigger) and the render thread that services them.
std::mutex g_screenshot_request_mutex;
} // namespace

RendererBase::RendererBase(Core::Frontend::EmuWindow& window_,
                           std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : render_window{window_}, context{std::move(context_)} {
    RefreshBaseSettings();
}

RendererBase::~RendererBase() = default;

void RendererBase::RefreshBaseSettings() {
    UpdateCurrentFramebufferLayout();
}

void RendererBase::UpdateCurrentFramebufferLayout() {
    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();

    render_window.UpdateCurrentFramebufferLayout(layout.width, layout.height);
}

bool RendererBase::IsScreenshotPending() const {
    return renderer_settings.screenshot_requested;
}

void RendererBase::RequestScreenshot(void* data, std::function<void(bool)> callback,
                                     const Layout::FramebufferLayout& layout,
                                     Service::Nvnflinger::LayerStackId layer_stack) {
    auto async_callback{[callback_ = std::move(callback)](bool invert_y) {
        std::thread t{callback_, invert_y};
        t.detach();
    }};
    std::function<void(bool)> stale;
    {
        std::scoped_lock lock{g_screenshot_request_mutex};
        if (renderer_settings.screenshot_requested) {
            // A request made between frames is only serviced once the next frame renders, and a
            // requester (the guest's capture service above all) may itself be waiting for the
            // callback before it renders that frame -- dropping the request loses the callback
            // and deadlocks the guest. Never drop: complete the stale request now. Its buffer
            // still holds the previous capture's pixels, so the image is stale-but-real.
            LOG_ERROR(Render, "A screenshot request was still pending; completing it early so "
                              "the new request can proceed");
            stale = std::move(renderer_settings.screenshot_complete_callback);
            renderer_settings.screenshot_requested = false;
            renderer_settings.screenshot_bits = nullptr;
        }
        renderer_settings.screenshot_bits = data;
        renderer_settings.screenshot_complete_callback = async_callback;
        renderer_settings.screenshot_framebuffer_layout = layout;
        renderer_settings.screenshot_layer_stack = layer_stack;
        renderer_settings.screenshot_requested = true;
    }
    if (stale) {
        stale(false);
    }
}

bool RendererBase::TakePendingScreenshot(void*& data, std::function<void(bool)>& callback,
                                         Layout::FramebufferLayout& layout,
                                         Service::Nvnflinger::LayerStackId& layer_stack) {
    std::scoped_lock lock{g_screenshot_request_mutex};
    if (!renderer_settings.screenshot_requested) {
        return false;
    }
    data = renderer_settings.screenshot_bits;
    callback = std::move(renderer_settings.screenshot_complete_callback);
    layout = renderer_settings.screenshot_framebuffer_layout;
    layer_stack = renderer_settings.screenshot_layer_stack;
    renderer_settings.screenshot_bits = nullptr;
    renderer_settings.screenshot_requested = false;
    return true;
}

} // namespace VideoCore
