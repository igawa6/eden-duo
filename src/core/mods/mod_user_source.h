// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The "user:" asset source (runtime 17): files the player supplies themselves, read from
//     <EdenDir>/dualscreen/user/<TITLEID>/<path>
// (EdenDir = the emulator's data dir on desktop; on Android the app's external files dir,
// Android/data/<package>/files). A package can then use data it may not ship -- a mod's text
// the player downloaded, their own art -- through every reader the registry serves (images,
// "data" files, a module's read_romfs). Read-only.
//   - The folder is created on first use, so the player can find where files go.
//   - Paths are relative to that folder: "..", "." and empty components, '\\', ':' and NUL are
//     refused, and a path that resolves (through links) outside the folder is refused too.
//   - A file larger than UserSourceMaxFileSize opens as missing.
//   - Capability bit EDEN_DSMOD_CAP_SOURCE_USER; get_i64("__source:user") = 1 once the folder
//     exists (or could be created), 0 otherwise.
// Kept free of ModRuntime so it can be unit tested (src/tests/core/mods/runtime17_user.cpp).

#include <filesystem>
#include <string_view>

#include "common/common_types.h"
#include "core/mods/mod_sources.h"

namespace Core::Mods {

inline constexpr u64 UserSourceMaxFileSize = u64{32} << 20; // 32 MiB

/// <EdenDir>/dualscreen/user.
std::filesystem::path UserSourceRoot();

/// <root>/<TITLEID> (16 upper-case hex digits).
std::filesystem::path UserSourceDir(const std::filesystem::path& root, u64 title_id);

/// Whether `path` (the part after "user:", leading '/' already removed) is a plain relative
/// path: non-empty, no "..", "." or empty components, no '\\', ':' or NUL.
bool IsSafeUserSourcePath(std::string_view path);

/// The "user:" directory source over `dir` (created on first open).
AssetSource MakeUserSource(std::filesystem::path dir);

} // namespace Core::Mods
