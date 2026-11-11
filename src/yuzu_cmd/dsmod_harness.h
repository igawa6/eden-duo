// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo: the eden-cli test harness. Everything Duo adds to the SDL frontend lives here so that
// yuzu.cpp keeps upstream's shape and only calls the hooks below:
//
//   * the command line (Duo keeps its own option table, see .upstream/DIVERGENCES.md) and the
//     one-shot utility modes --dump-il2cpp, --dump-romfs and --install;
//   * the second ("aux") screen: --aux-window, --aux-virtual, the desktop haptic sink;
//   * Stage 1 Simulated (single-surface) dual screen: --aux-inline;
//   * headless control: screenshots (<prefix>.shot, SIGUSR1) and scripted input (<prefix>.btn);
//   * EDEN_VSYNC / EDEN_NVDEC overrides and a periodic perf line.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>

#include <SDL3/SDL.h>

class AuxWindow_SDL3;

namespace Core {
class System;
struct LaunchParams;
} // namespace Core

namespace InputCommon {
class InputSubsystem;
}

namespace DSModHarness {

/// Duo state that lives as long as the eden-cli session (owned by the frontend's SdlState).
struct Session {
    Session();
    ~Session();

    std::unique_ptr<AuxWindow_SDL3> aux_window;
    bool aux_window_requested = false;
    bool aux_virtual_requested = false;
    /// --aux-inline <x>,<y>,<w>,<h>: the companion's rect, fractions of the main window (0..1).
    /// The game takes whichever outer edge the companion rect does not touch (its complement on
    /// that one axis); set only when the flag parsed. See ApplyAuxInline in dsmod_harness.cpp.
    std::optional<std::array<float, 4>> aux_inline_companion;
    /// `-u <name>`: resolved once the config is loaded, see ResolveSelectedUser.
    std::optional<std::string> selected_username;
};

/// Parses eden-cli's command line into `lp` and runs the utility modes (--dump-il2cpp,
/// --dump-romfs, --install), which exit the process. Returns a value when SDL_AppInit must
/// return it straight away (--help, --version, a malformed --multiplayer).
std::optional<SDL_AppResult> ParseCommandLine(Session& session, int argc, char** argv,
                                              wchar_t** argv_w, Core::LaunchParams& lp);

/// After the config is loaded: resolves `-u <name>` to a profile index and checks the selected
/// user exists. Returns false when the name matches no profile.
bool ResolveSelectedUser(Session& session, Core::System& system, Core::LaunchParams& lp);

/// After the game is loaded, before the GPU starts: environment overrides, aux screen, haptic
/// sink and signal handlers.
void OnGameLoaded(Session& session, Core::System& system);

/// Once per SDL iteration: perf line, pending screenshots and scripted input.
void Iterate(Core::System& system, InputCommon::InputSubsystem& input_subsystem);

/// Routes an SDL event to the aux window. Returns true when the aux window owned (and handled)
/// it; otherwise the caller hands it to the main window.
bool HandleEvent(Session& session, SDL_Event& event);

} // namespace DSModHarness
