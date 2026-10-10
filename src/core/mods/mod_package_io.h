// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/file_sys/vfs/vfs.h"

namespace Core::Mods {

inline constexpr size_t MaxPackageMetadataBytes = 4 * 1024 * 1024;
inline constexpr size_t MaxLoadPlanBytes = 1024 * 1024;
inline constexpr size_t MaxNativeModuleBytes = 64 * 1024 * 1024;

inline std::optional<std::vector<u8>> ReadPackageBytes(const FileSys::VirtualFile& file,
                                                       size_t limit) {
    if (!file) {
        return std::nullopt;
    }
    const auto size = file->GetSize();
    if (size == 0 || size > limit) {
        return std::nullopt;
    }
    std::vector<u8> bytes(size);
    if (file->Read(bytes.data(), size, 0) != size) {
        return std::nullopt;
    }
    return bytes;
}

inline std::optional<nlohmann::json> ReadPackageJson(const FileSys::VirtualFile& file,
                                                     size_t limit = MaxPackageMetadataBytes) {
    const auto bytes = ReadPackageBytes(file, limit);
    if (!bytes) {
        return std::nullopt;
    }
    auto json = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return json;
}

} // namespace Core::Mods
