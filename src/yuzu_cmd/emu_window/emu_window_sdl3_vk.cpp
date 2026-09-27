// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstdint>
#include <memory>
#include <string>

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_vk.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

bool FillWindowSystemInfoFromSDL(SDL_Window* window,
                                 Core::Frontend::EmuWindow::WindowSystemInfo& info) {
    const SDL_PropertiesID window_props = SDL_GetWindowProperties(window);
    if (void* hwnd =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        info.type = Core::Frontend::WindowSystemType::Windows;
        info.render_surface = hwnd;
    } else if (void* wl_display =
                   SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER,
                                          nullptr);
               wl_display != nullptr) {
        void* wl_surface =
            SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        if (wl_surface == nullptr) {
            LOG_CRITICAL(Frontend, "Wayland surface is unavailable");
            return false;
        }
        info.type = Core::Frontend::WindowSystemType::Wayland;
        info.display_connection = wl_display;
        info.render_surface = wl_surface;
    } else if (void* x11_display =
                   SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER,
                                          nullptr);
               x11_display != nullptr) {
        const auto x11_window =
            SDL_GetNumberProperty(window_props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (x11_window == 0) {
            LOG_CRITICAL(Frontend, "X11 window handle is unavailable");
            return false;
        }
        info.type = Core::Frontend::WindowSystemType::X11;
        info.display_connection = x11_display;
        info.render_surface = reinterpret_cast<void*>(static_cast<uintptr_t>(x11_window));
    } else if (SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                      nullptr) != nullptr) {
        info.type = Core::Frontend::WindowSystemType::Cocoa;
        info.render_surface = SDL_Metal_CreateView(window);
    } else if (void* android_window =
                   SDL_GetPointerProperty(window_props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                          nullptr);
               android_window != nullptr) {
        info.type = Core::Frontend::WindowSystemType::Android;
        info.render_surface = android_window;
    } else {
        LOG_CRITICAL(Frontend, "Unable to determine native window backend from SDL properties");
        return false;
    }

    return true;
}

EmuWindow_SDL3_VK::EmuWindow_SDL3_VK(InputCommon::InputSubsystem* input_subsystem_,
                                     Core::System& system_, bool fullscreen)
    : EmuWindow_SDL3{input_subsystem_, system_} {
    const std::string window_title = fmt::format("Eden {} | {}-{} (Vulkan)",
                                                 Common::g_build_name,
                                                 Common::g_scm_branch,
                                                 Common::g_scm_desc);
    render_window =
        SDL_CreateWindow(window_title.c_str(), Layout::ScreenUndocked::Width,
                         Layout::ScreenUndocked::Height,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);


    SetWindowIcon();

    if (fullscreen) {
        Fullscreen();
        ShowCursor(false);
    }

    if (!FillWindowSystemInfoFromSDL(render_window, window_info)) {
        std::exit(EXIT_FAILURE);
    }

    OnResize();
    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);
    SDL_PumpEvents();
    LOG_INFO(Frontend, "Eden Version: {} | {}-{} (Vulkan)", Common::g_build_name,
             Common::g_scm_branch, Common::g_scm_desc);
}

EmuWindow_SDL3_VK::~EmuWindow_SDL3_VK() = default;

std::unique_ptr<Core::Frontend::GraphicsContext> EmuWindow_SDL3_VK::CreateSharedContext() const {
    return std::make_unique<DummyContext>();
}
