// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"

namespace Mk8dAssets {

struct AstcDecoder {
    void* userdata{};
    EdenDsmodAstcDecoder decode{};
};

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

struct MapCamera {
    float position[3]{};
    float look_at[3]{};
    float source_up[3]{};
    float right[3]{};
    float up[3]{};
    float width{};
    float height{};

    /// Project a game-world point to normalized image coordinates (top-left origin).
    bool Project(float x, float y, float z, float& u, float& v) const;
};

/// Per-module-instance, thread-safe lazy RomFS decoder. No decoded bytes are persisted or shipped.
class Decoder {
public:
    explicit Decoder(std::size_t cache_budget = 32 * 1024 * 1024);
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// Logical keys: mk8d/characters/tc_MapChara_*.png, mk8d/items/item_N.png,
    /// mk8d/maps/<course>.png, and mk8d/ui/lap_flag.png.
    std::shared_ptr<const Image> LoadImage(const EdenDsmodHostApi& host,
                                           std::string_view logical_key);
    std::shared_ptr<const MapCamera> LoadMapCamera(const EdenDsmodHostApi& host,
                                                   std::string_view course);
    void SetAstcDecoder(AstcDecoder decoder);
    void Clear();

    /// Convert a driver id/variant to the RomFS texture alias used by LoadImage.
    static std::string DriverImageKey(std::uint32_t driver_id, std::uint32_t variant);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Mk8dAssets
