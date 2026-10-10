// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The "base:" and "aoc:" romfs roots (see mod_romfs_sources.h). Each opener mirrors the service
// the game itself goes through, but on objects of its own:
//   - base: the loader over the booted game file, ReadRomFS (the base NCA's romfs, no
//     PatchRomFS); the Program NCA from the content providers when the loader has none.
//   - aoc: Service::AOC's add-on list (AccumulateAOCTitleIDs + ListAddOnContent: AOC Data
//     entries of this base title that parse, none when "DLC" is in disabled_addons), lowest
//     AOC id first, then FSP_SRV::OpenDataStorageByDataId: the Data NCA's romfs through
//     PatchManager::PatchRomFS(Data), so AOC updates and LayeredFS mods apply as for the game.
// The cache key is the program id plus the booted game file object (held, so its address
// cannot be reused by a later boot): a new boot reopens, so a changed DLC setting takes effect.

#include <algorithm>
#include <chrono>
#include <mutex>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/mods/mod_romfs_sources.h"

namespace Core::Mods {

namespace {

/// Serialises reads of one private storage chain: its cipher and table layers keep state across
/// a read, and asset workers, the tick thread and module threads all read it. Long reads go in
/// chunks so no reader waits for more than one. (Same approach as mod_assets.cpp's romfs.)
class SerialRomfsFile final : public FileSys::VfsFile {
public:
    explicit SerialRomfsFile(FileSys::VirtualFile base_) : base{std::move(base_)} {}

    std::string GetName() const override {
        return base->GetName();
    }
    std::size_t GetSize() const override {
        return base->GetSize();
    }
    bool Resize(std::size_t) override {
        return false;
    }
    FileSys::VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        constexpr std::size_t Chunk = 1 << 20;
        std::size_t done = 0;
        while (done < length) {
            const std::size_t want = std::min(Chunk, length - done);
            std::size_t got = 0;
            {
                std::scoped_lock lock{mutex};
                got = base->Read(data + done, want, offset + done);
            }
            done += got;
            if (got != want) {
                break;
            }
        }
        return done;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

private:
    FileSys::VirtualFile base;
    mutable std::mutex mutex;
};

struct CachedRoot {
    u64 program_id = 0;
    FileSys::VirtualFile game_file; // identity of the boot the root belongs to
    bool tried = false;
    FileSys::VirtualDir root;
};

// Never destroyed: a chain's host files refer back to the System's filesystem, which is gone by
// the time static destructors run at exit.
struct Cache {
    std::mutex mutex;
    CachedRoot base;
    CachedRoot aoc;
};
Cache& GetCache() {
    static Cache* const cache = new Cache;
    return *cache;
}

FileSys::VirtualDir Extract(FileSys::VirtualFile raw) {
    return raw ? FileSys::ExtractRomFS(std::make_shared<SerialRomfsFile>(std::move(raw))) : nullptr;
}

double MsSince(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
        .count();
}

FileSys::VirtualDir BuildBase(Core::System& system, u64 program_id) {
    const auto started = std::chrono::steady_clock::now();
    FileSys::VirtualFile raw;
    const char* from = "loader";
    if (const auto loader =
            Loader::GetLoader(system, system.GetAppLoader().GetFile(), program_id, 0)) {
        if (loader->ReadRomFS(raw) != Loader::ResultStatus::Success) {
            raw = nullptr;
        }
    }
    if (!raw) {
        from = "content provider";
        if (const auto nca = system.GetContentProvider().GetEntry(
                program_id, FileSys::ContentRecordType::Program)) {
            raw = nca->GetRomFS();
        }
    }
    auto root = Extract(std::move(raw));
    LOG_INFO(Core, "DSMod: base romfs of {:016X} {} ({}, {:.1f} ms)", program_id,
             root ? "opened" : "unavailable", from, MsSince(started));
    return root;
}

FileSys::VirtualDir BuildAoc(Core::System& system, u64 program_id) {
    const auto started = std::chrono::steady_clock::now();
    const u64 base_title = FileSys::GetBaseTitleID(program_id);
    const auto disabled = Settings::values.disabled_addons.find(base_title);
    if (disabled != Settings::values.disabled_addons.end() &&
        std::find(disabled->second.begin(), disabled->second.end(), "DLC") !=
            disabled->second.end()) {
        LOG_INFO(Core, "DSMod: aoc romfs of {:016X} unavailable (DLC disabled)", program_id);
        return nullptr;
    }
    const auto& provider = system.GetContentProvider();
    std::vector<u64> aoc_ids;
    for (const auto& entry :
         provider.ListEntriesFilter(FileSys::TitleType::AOC, FileSys::ContentRecordType::Data)) {
        if (FileSys::GetBaseTitleID(entry.title_id) != base_title) {
            continue;
        }
        const auto nca = provider.GetEntry(entry.title_id, FileSys::ContentRecordType::Data);
        if (nca != nullptr && nca->GetStatus() == Loader::ResultStatus::Success) {
            aoc_ids.push_back(entry.title_id);
        }
    }
    if (aoc_ids.empty()) {
        LOG_INFO(Core, "DSMod: aoc romfs of {:016X} unavailable (no add-on content)", program_id);
        return nullptr;
    }
    const u64 tid = *std::min_element(aoc_ids.begin(), aoc_ids.end(), [](u64 a, u64 b) {
        return FileSys::GetAOCID(a) < FileSys::GetAOCID(b);
    });
    std::shared_ptr<FileSys::NCA> nca = provider.GetEntry(tid, FileSys::ContentRecordType::Data);
    FileSys::VirtualFile raw = nca ? nca->GetRomFS() : nullptr;
    if (raw) {
        const FileSys::PatchManager patch_manager{tid, system.GetFileSystemController(), provider};
        raw = patch_manager.PatchRomFS(nca.get(), std::move(raw), FileSys::ContentRecordType::Data);
    }
    auto root = Extract(std::move(raw));
    LOG_INFO(Core, "DSMod: aoc romfs {:016X} (of {} add-on{}) {} ({:.1f} ms)", tid, aoc_ids.size(),
             aoc_ids.size() == 1 ? "" : "s", root ? "opened" : "unavailable", MsSince(started));
    return root;
}

FileSys::VirtualDir Open(Core::System& system, u64 program_id, CachedRoot Cache::* which,
                         FileSys::VirtualDir (*build)(Core::System&, u64)) {
    if (program_id == 0) {
        return nullptr;
    }
    FileSys::VirtualFile game_file = system.GetAppLoader().GetFile();
    Cache& all = GetCache();
    std::scoped_lock lock{all.mutex};
    CachedRoot& cache = all.*which;
    if (cache.tried && cache.program_id == program_id && cache.game_file == game_file) {
        return cache.root;
    }
    cache = CachedRoot{program_id, std::move(game_file), true, nullptr};
    cache.root = build(system, program_id);
    return cache.root;
}

} // namespace

FileSys::VirtualDir OpenBaseRomfs(Core::System& system, u64 program_id) {
    return Open(system, program_id, &Cache::base, BuildBase);
}

FileSys::VirtualDir OpenAocRomfs(Core::System& system, u64 program_id) {
    return Open(system, program_id, &Cache::aoc, BuildAoc);
}

void ReleaseRomfsSources() {
    Cache& all = GetCache();
    std::scoped_lock lock{all.mutex};
    all.base = {};
    all.aoc = {};
}

} // namespace Core::Mods
