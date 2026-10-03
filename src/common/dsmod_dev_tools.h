// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>

namespace Common::DSMod {
// Standalone tools retain development behavior; CMake explicitly selects the release gate.
#if defined(EDEN_DSMOD_BUILD_DEV_TOOLS) && !EDEN_DSMOD_BUILD_DEV_TOOLS
inline constexpr bool DevToolsEnabled = false;
inline constexpr const char* DevEnvironment(const char*) { return nullptr; }
#else
inline constexpr bool DevToolsEnabled = true;
inline const char* DevEnvironment(const char* name) { return std::getenv(name); }
#endif
} // namespace Common::DSMod
