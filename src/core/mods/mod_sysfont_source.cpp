// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/mods/mod_sysfont_source.h"

#include <algorithm>
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
    using Bytes = std::shared_ptr<const std::vector<u8>>;
    struct Cache {
        std::function<FileSys::VirtualDir(u64)> open;
        std::mutex mutex;
        std::map<std::string, Bytes> fonts; // by file name; empty vector = tried, missing
        Bytes Get(const std::string& src) {
            static const Bytes none = std::make_shared<const std::vector<u8>>();
            const auto entry = FindSysfont(AssetSources::PathOf(src));
            if (!entry || !open) {
                return none;
            }
            std::scoped_lock lock{mutex};
            const std::string key{entry->file};
            if (const auto it = fonts.find(key); it != fonts.end()) {
                return it->second;
            }
            std::vector<u8> ttf;
            if (const auto root = open(entry->archive_id)) {
                if (const auto file = root->GetFile(key)) {
                    ttf = DecryptSysfont(file->ReadAllBytes());
                }
            }
            return fonts.emplace(key, std::make_shared<const std::vector<u8>>(std::move(ttf)))
                .first->second;
        }
    };
    auto cache = std::make_shared<Cache>();
    cache->open = std::move(open_real_archive);
    return {
        .prefix = "sysfont",
        .read_bytes = [cache](const std::string& src) { return *cache->Get(src); },
        .capability = EDEN_DSMOD_CAP_SOURCE_SYSFONT,
        .module_readable = true,
        // A module reads a 20 MiB font in chunks: copy only the requested slice of the cached
        // font, never the whole vector per call.
        .read_range = [cache](const std::string& src, u64 offset, void* out,
                              size_t size) -> size_t {
            const auto bytes = cache->Get(src);
            if (out == nullptr) {
                return bytes->size();
            }
            if (offset >= bytes->size()) {
                return 0;
            }
            const size_t count = std::min<size_t>(size, bytes->size() - offset);
            std::memcpy(out, bytes->data() + offset, count);
            return count;
        },
    };
}

} // namespace Core::Mods
