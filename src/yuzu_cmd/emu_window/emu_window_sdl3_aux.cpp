// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <span>
#include <tuple>

#include "common/logging.h"
#include "core/core.h"
#include "common/frontend/framebuffer_layout.h"
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
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.windowID == window_id;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        return event.tfinger.windowID == window_id;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    case SDL_EVENT_PINCH_BEGIN:
    case SDL_EVENT_PINCH_UPDATE:
    case SDL_EVENT_PINCH_END:
        return event.pinch.windowID == window_id;
#endif
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
        return ResetInput();
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return ResetInput();
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = true;
        return OnResize();
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        // Closing the aux window just hides it; the emulator keeps running.
        SDL_HideWindow(window);
        is_shown = false;
        return ResetInput();
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.which == SDL_TOUCH_MOUSEID || !fingers.Empty()) {
            return;
        }
        mouse_x = event.motion.x;
        mouse_y = event.motion.y;
        if (mouse_down) {
            PublishTouch(true, false);
        }
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT ||
            !fingers.Empty()) {
            return;
        }
        mouse_x = event.button.x;
        mouse_y = event.button.y;
        mouse_down = true;
        PublishTouch(true, true);
        return;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.which == SDL_TOUCH_MOUSEID || event.button.button != SDL_BUTTON_LEFT ||
            !fingers.Empty()) {
            return;
        }
        mouse_down = false;
        PublishTouch(false, false);
        return;
    case SDL_EVENT_MOUSE_WHEEL: {
        // Own this event even when no companion pane accepts it: an aux wheel must never
        // reach the main game's mouse bindings. Real touch/left-button gestures take priority.
        if (mouse_down || !fingers.Empty() || event.wheel.which == SDL_TOUCH_MOUSEID) {
            return;
        }
        float dx = event.wheel.x;
        float dy = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            dx = -dx;
            dy = -dy;
        }
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0 && dx == 0.0f) {
            dx = -dy;
            dy = 0.0f;
        }
        const auto [px, py] = ToPixels(event.wheel.mouse_x, event.wheel.mouse_y);
        system.GPU().DSModAux().PushWheel({px, py, dx, dy});
        return;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION: {
        int w = 1, h = 1;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (fingers.Empty() && mouse_down) {
            mouse_down = false;
            PublishTouch(false, false);
        }
        fingers.Update(event.tfinger.touchID, event.tfinger.fingerID,
                       static_cast<u32>(std::clamp(event.tfinger.x, 0.0f, 1.0f) * std::max(w - 1, 0)),
                       static_cast<u32>(std::clamp(event.tfinger.y, 0.0f, 1.0f) * std::max(h - 1, 0)));
        system.GPU().DSModAux().SetTouch(fingers.Points());
        fingers.ClearStarts();
        return;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        fingers.Release(event.tfinger.touchID, event.tfinger.fingerID);
        system.GPU().DSModAux().SetTouch(fingers.Points());
        fingers.ClearStarts();
        return;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    case SDL_EVENT_PINCH_BEGIN: {
        if (mouse_down || !fingers.Empty()) return;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        std::tie(native_pinch_x, native_pinch_y) = ToPixels(mouse_x, mouse_y);
        native_pinch_active = true;
        system.GPU().DSModAux().PushPinch({VideoCore::DSMod::AuxPinchEvent::Phase::Begin,
                                          native_pinch_x, native_pinch_y, 1.0f});
        return;
    }
    case SDL_EVENT_PINCH_UPDATE:
        if (native_pinch_active && !mouse_down && fingers.Empty()) {
            SDL_GetMouseState(&mouse_x, &mouse_y);
            std::tie(native_pinch_x, native_pinch_y) = ToPixels(mouse_x, mouse_y);
            system.GPU().DSModAux().PushPinch({VideoCore::DSMod::AuxPinchEvent::Phase::Update,
                                              native_pinch_x, native_pinch_y, event.pinch.scale});
        }
        return;
    case SDL_EVENT_PINCH_END:
        if (native_pinch_active) {
            system.GPU().DSModAux().PushPinch({VideoCore::DSMod::AuxPinchEvent::Phase::End,
                                              native_pinch_x, native_pinch_y, 1.0f});
        }
        native_pinch_active = false;
        return;
#endif
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
    // The UI render paths use this resized layout directly. Publish the same panel
    // dimensions for touch mapping; guest-layer rendering may never run for a module UI.
    auto& aux = system.GPU().DSModAux();
    aux.width.store(layout.width);
    aux.height.store(layout.height);
}

void AuxWindow_SDL3::PublishTouch(bool down, bool started) {
    auto& aux = system.GPU().DSModAux();
    if (!down) {
        aux.SetTouch({});
        return;
    }
    const auto [x, y] = ToPixels(mouse_x, mouse_y);
    const VideoCore::DSMod::AuxTouchPoint point{
        .finger_id = 0,
        .x = x,
        .y = y,
        .attributes = started ? 1u : 0u,
        .delta_ns = 0,
    };
    aux.SetTouch(std::span{&point, 1});
}

void AuxWindow_SDL3::ResetInput() {
    auto& aux = system.GPU().DSModAux();
    if (native_pinch_active) {
        aux.PushPinch(
            {VideoCore::DSMod::AuxPinchEvent::Phase::End, native_pinch_x, native_pinch_y, 1.0f});
        native_pinch_active = false;
    }
    if (mouse_down || !fingers.Empty()) {
        fingers.Clear();
        mouse_down = false;
        aux.SetTouch({});
    }
}

std::pair<u32, u32> AuxWindow_SDL3::ToPixels(float x, float y) const {
    int ww = 1, wh = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(window, &ww, &wh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    const float sx = static_cast<float>(pw) / static_cast<float>(std::max(ww, 1));
    const float sy = static_cast<float>(ph) / static_cast<float>(std::max(wh, 1));
    return {static_cast<u32>(std::max(0.0f, x * sx)), static_cast<u32>(std::max(0.0f, y * sy))};
}
