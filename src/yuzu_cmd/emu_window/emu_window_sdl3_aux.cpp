// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <span>

#include "common/logging.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/gpu.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_aux.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_vk.h"

AuxWindow_SDL3::AuxWindow_SDL3(Core::System& system_, u32 width, u32 height) : system{system_} {
    window = SDL_CreateWindow("Eden - Screen 2 (DSMod aux)", static_cast<int>(width),
                              static_cast<int>(height),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to create aux SDL window: {}", SDL_GetError());
        return;
    }
    window_id = SDL_GetWindowID(window);
    if (!FillWindowSystemInfoFromSDL(window, window_info)) {
        LOG_CRITICAL(Frontend, "Unable to determine native window backend for aux window");
        SDL_DestroyWindow(window);
        window = nullptr;
        return;
    }
    OnResize();
    LOG_INFO(Frontend, "DSMod aux window created ({}x{}, id {})", width, height, window_id);
}

AuxWindow_SDL3::~AuxWindow_SDL3() {
    if (window != nullptr) {
        SDL_DestroyWindow(window);
    }
}

std::unique_ptr<Core::Frontend::GraphicsContext> AuxWindow_SDL3::CreateSharedContext() const {
    return std::make_unique<DummyContext>();
}

bool AuxWindow_SDL3::OwnsEvent(const SDL_Event& event) const {
    if (window == nullptr) {
        return false;
    }
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID == window_id;
    }
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.windowID == window_id;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.windowID == window_id;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
        return event.tfinger.windowID == window_id;
    default:
        return false;
    }
}

void AuxWindow_SDL3::OnEvent(SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        return OnResize();
    case SDL_EVENT_WINDOW_MINIMIZED:
        is_shown = false;
        return;
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = true;
        return OnResize();
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        // Closing the aux window just hides it; the emulator keeps running.
        SDL_HideWindow(window);
        is_shown = false;
        return;
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.which == SDL_TOUCH_MOUSEID) {
            return;
        }
        mouse_x = event.motion.x;
        mouse_y = event.motion.y;
        if (mouse_down) {
            PublishTouch(true, false);
        }
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT) {
            return;
        }
        mouse_x = event.button.x;
        mouse_y = event.button.y;
        mouse_down = true;
        PublishTouch(true, true);
        return;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT) {
            return;
        }
        mouse_down = false;
        PublishTouch(false, false);
        return;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION: {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        mouse_x = event.tfinger.x * static_cast<float>(w);
        mouse_y = event.tfinger.y * static_cast<float>(h);
        const bool started = event.type == SDL_EVENT_FINGER_DOWN;
        mouse_down = true;
        PublishTouch(true, started);
        return;
    }
    case SDL_EVENT_FINGER_UP:
        mouse_down = false;
        PublishTouch(false, false);
        return;
    default:
        return;
    }
}

void AuxWindow_SDL3::OnResize() {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    // The aux output always fills the whole window (no 16:9 letterboxing): Tier B modules
    // query the real aux size through dsm:u and lay themselves out accordingly.
    Layout::FramebufferLayout layout{};
    layout.width = static_cast<u32>(width);
    layout.height = static_cast<u32>(height);
    layout.screen = Common::Rectangle<u32>{0, 0, static_cast<u32>(width), static_cast<u32>(height)};
    layout.is_srgb = false;
    NotifyFramebufferLayoutChanged(layout);
}

void AuxWindow_SDL3::PublishTouch(bool down, bool started) {
    auto& aux = system.GPU().DSModAux();
    if (!down) {
        aux.SetTouch({});
        return;
    }
    int ww = 1, wh = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    const float sx = static_cast<float>(pw) / static_cast<float>(std::max(ww, 1));
    const float sy = static_cast<float>(ph) / static_cast<float>(std::max(wh, 1));
    const VideoCore::DSMod::AuxTouchPoint point{
        .finger_id = 0,
        .x = static_cast<u32>(std::max(0.0f, mouse_x * sx)),
        .y = static_cast<u32>(std::max(0.0f, mouse_y * sy)),
        .attributes = started ? 1u : 0u,
        .delta_ns = 0,
    };
    aux.SetTouch(std::span{&point, 1});
}
