// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// AssetSources (mod_sources.h): the prefix -> source table behind every asset read. The
// runtime's own sources (romfs, file, module) are registered in ModRuntime's constructor
// (RegisterAssetSources, mod_assets.cpp).

#include "core/mods/mod_sources.h"

#include <algorithm>
#include <cstring>

#include "core/file_sys/vfs/vfs.h"

namespace Core::Mods {

void AssetSources::Register(AssetSource source) {
    if (Slot* const existing = Find(source.prefix)) {
        existing->def = std::move(source);
        return;
    }
    auto slot = std::make_unique<Slot>();
    slot->def = std::move(source);
    slots.push_back(std::move(slot));
}

std::string_view AssetSources::PrefixOf(std::string_view src) {
    const size_t colon = src.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return {};
    }
    const std::string_view prefix = src.substr(0, colon);
    const bool valid = std::ranges::all_of(prefix, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
    return valid ? prefix : std::string_view{};
}

std::string_view AssetSources::PathOf(std::string_view src) {
    const std::string_view prefix = PrefixOf(src);
    std::string_view path = prefix.empty() ? src : src.substr(prefix.size() + 1);
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    return path;
}

AssetSources::Slot* AssetSources::Find(std::string_view prefix) const {
    if (prefix.empty()) {
        return nullptr;
    }
    for (const auto& slot : slots) {
        if (slot->def.prefix == prefix) {
            return slot.get();
        }
    }
    return nullptr;
}

FileSys::VirtualDir AssetSources::RootOf(Slot& slot) {
    if (!slot.def.open_dir || slot.def.read_bytes) {
        return nullptr;
    }
    std::call_once(slot.once, [&slot] { slot.root = slot.def.open_dir(); });
    return slot.root;
}

bool AssetSources::Known(std::string_view src) const {
    const std::string_view prefix = src.find(':') == std::string_view::npos ? src : PrefixOf(src);
    return Find(prefix) != nullptr;
}

bool AssetSources::IsDirectorySource(std::string_view src) const {
    const Slot* const slot = Find(PrefixOf(src));
    return slot != nullptr && !slot->def.read_bytes && slot->def.open_dir;
}

bool AssetSources::Available(std::string_view src) {
    const std::string_view prefix = src.find(':') == std::string_view::npos ? src : PrefixOf(src);
    Slot* const slot = Find(prefix);
    if (slot == nullptr) {
        return false;
    }
    return slot->def.read_bytes ? true : RootOf(*slot) != nullptr;
}

FileSys::VirtualDir AssetSources::Root(std::string_view prefix) {
    Slot* const slot = Find(prefix);
    return slot != nullptr ? RootOf(*slot) : nullptr;
}

FileSys::VirtualFile AssetSources::Open(std::string_view src) {
    Slot* const slot = Find(PrefixOf(src));
    if (slot == nullptr) {
        return nullptr;
    }
    const auto root = RootOf(*slot);
    const std::string_view path = PathOf(src);
    if (!root || path.empty()) {
        return nullptr;
    }
    return root->GetFileRelative(path);
}

std::vector<u8> AssetSources::ReadAll(const std::string& src) {
    Slot* const slot = Find(PrefixOf(src));
    if (slot == nullptr) {
        return {};
    }
    if (slot->def.read_bytes) {
        return slot->def.read_bytes(src);
    }
    const auto file = Open(src);
    return file ? file->ReadAllBytes() : std::vector<u8>{};
}

size_t AssetSources::ReadRange(std::string_view src, u64 offset, void* out, size_t size) {
    const auto file = Open(src);
    if (!file) {
        return 0;
    }
    const size_t file_size = file->GetSize();
    if (out == nullptr) {
        return file_size;
    }
    if (offset >= file_size) {
        return 0;
    }
    return file->Read(static_cast<u8*>(out), std::min<size_t>(size, file_size - offset),
                      static_cast<size_t>(offset));
}

u64 AssetSources::Capabilities() const {
    u64 caps = 0;
    for (const auto& slot : slots) {
        caps |= slot->def.capability;
    }
    return caps;
}

std::vector<std::string> AssetSources::Prefixes() const {
    std::vector<std::string> out;
    out.reserve(slots.size());
    for (const auto& slot : slots) {
        out.push_back(slot->def.prefix);
    }
    return out;
}

} // namespace Core::Mods
