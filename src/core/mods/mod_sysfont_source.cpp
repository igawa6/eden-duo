// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/mods/mod_sysfont_source.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "common/swap.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "core/mods/dsmod_module_abi.h"

namespace Core::Mods {
namespace {
constexpr std::string_view Extension = ".bfttf";
constexpr u32 EncryptedMagic = 0x36f81a1e; // first big-endian word of every stored shared font
} // namespace

std::optional<SysfontEntry> FindSysfont(std::string_view name) {
    if (name.ends_with(Extension)) {
        name.remove_suffix(Extension.size());
    }
    for (const auto& [archive, file] : Service::NS::SHARED_FONTS) {
        std::string_view stem{file};
        stem.remove_suffix(Extension.size());
        if (!name.empty() && name == stem) {
            return SysfontEntry{static_cast<u64>(archive), file};
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> SysfontNames() {
    std::vector<std::string_view> names;
    for (const auto& entry : Service::NS::SHARED_FONTS) {
        std::string_view stem{entry.second};
        stem.remove_suffix(Extension.size());
        names.push_back(stem);
    }
    return names;
}

std::vector<u8> DecryptSysfont(std::span<const u8> stored) {
    // At least the 8-byte header and one word, whole words, and the shared-font magic: the
    // decryptor asserts on anything else.
    if (stored.size() < 12 || stored.size() % sizeof(u32) != 0) {
        return {};
    }
    std::vector<u32> words(stored.size() / sizeof(u32));
    std::memcpy(words.data(), stored.data(), stored.size());
    for (auto& word : words) {
        word = Common::swap32(word);
    }
    if (words[0] != EncryptedMagic) {
        return {};
    }
    std::vector<u8> ttf(stored.size() - 8);
    Service::NS::DecryptSharedFontToTTF(words, ttf);
    return ttf;
}

AssetSource MakeSysfontSource(std::function<FileSys::VirtualDir(u64)> open_real_archive) {
    struct Cache {
        std::mutex mutex;
        std::map<std::string, std::vector<u8>> fonts; // by file name; empty = tried, missing
    };
    auto cache = std::make_shared<Cache>();
    return {
        .prefix = "sysfont",
        .read_bytes = [open = std::move(open_real_archive),
                       cache](const std::string& src) -> std::vector<u8> {
            const auto entry = FindSysfont(AssetSources::PathOf(src));
            if (!entry || !open) {
                return {};
            }
            std::scoped_lock lock{cache->mutex};
            const std::string key{entry->file};
            if (const auto it = cache->fonts.find(key); it != cache->fonts.end()) {
                return it->second;
            }
            std::vector<u8> ttf;
            if (const auto root = open(entry->archive_id)) {
                if (const auto file = root->GetFile(std::string{entry->file})) {
                    const auto stored = file->ReadAllBytes();
                    ttf = DecryptSysfont(stored);
                }
            }
            cache->fonts.emplace(key, ttf);
            return ttf;
        },
        .capability = EDEN_DSMOD_CAP_SOURCE_SYSFONT,
        .module_readable = true,
    };
}

} // namespace Core::Mods
