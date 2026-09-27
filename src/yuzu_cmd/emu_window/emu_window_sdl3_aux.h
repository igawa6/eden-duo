// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Second ("aux") SDL window: the desktop stand-in for a dual-screen handheld's bottom panel.
// It is a bare Core::Frontend::EmuWindow (no input subsystem, no shared context) whose
// surface the renderer presents the DSMod aux output to, and whose mouse/touch events are
// published to VideoCore::DSMod::AuxRouting for the dsm:u service.

#pragma once

#include <memory>

#include <SDL3/SDL.h>

#include "common/common_types.h"
#include "core/frontend/emu_window.h"

namespace Core {
class System;
}

class AuxWindow_SDL3 final : public Core::Frontend::EmuWindow {
public:
    explicit AuxWindow_SDL3(Core::System& system_, u32 width, u32 height);
    ~AuxWindow_SDL3() override;

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override;

    bool IsShown() const override {
        return is_shown && window != nullptr;
    }

    bool IsValid() const {
        return window != nullptr && window_info.render_surface != nullptr;
    }

    /// True when the SDL event targets this window (by window id).
    bool OwnsEvent(const SDL_Event& event) const;

    void OnEvent(SDL_Event& event);

private:
    void OnResize();
    void PublishTouch(bool down, bool started);

    Core::System& system;
    SDL_Window* window{};
    SDL_WindowID window_id{};
    bool is_shown = true;
    bool mouse_down = false;
    float mouse_x{};
    float mouse_y{};
};
