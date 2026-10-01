// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <limits>

#include <nlohmann/json.hpp>

#include "common/atomic_ops.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/file_sys/fs_save_data_types.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/memory.h"
#include "core/mods/mod_runtime.h"
#include "video_core/textures/astc.h"

namespace Core::Mods {
namespace {
template <typename T>
T* MailboxPointer(const EdenDsmodHostExtensions& extension, const EdenDsmodHostApi& host,
                  u32 offset) {
    if (extension.mailbox_address == 0 || offset % alignof(T) != 0 ||
        offset > extension.mailbox_size || sizeof(T) > extension.mailbox_size - offset ||
        extension.mailbox_address > std::numeric_limits<u64>::max() - offset ||
        !host.get_read_pointer)
        return nullptr;
    const auto* bytes =
        host.get_read_pointer(host.userdata, extension.mailbox_address + offset, sizeof(T));
    if (!bytes || reinterpret_cast<uintptr_t>(bytes) % alignof(T) != 0)
        return nullptr;
    return reinterpret_cast<T*>(const_cast<u8*>(bytes));
}
constexpr size_t ImageBudget = 64 * 1024 * 1024;
/// EDEN_DSMOD_IMAGE_TIMING=1: log when each module image lands (queue -> decoded -> drained).
bool ImageTimingEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("EDEN_DSMOD_IMAGE_TIMING");
        return v != nullptr && std::strcmp(v, "0") != 0;
    }();
    return on;
}
constexpr size_t MaxImageBytes = 16 * 1024 * 1024;

template <typename T>
T AtomicMailboxLoad(T* pointer) {
    // atomic_ref is absent from the Android NDK libc++ used by Eden. The project's CAS helpers
    // are sequentially consistent on supported targets, which is stronger than this ABI's
    // acquire/release contract and operates directly on the aligned guest backing storage.
    T actual{};
    (void)Common::AtomicCompareAndSwap(pointer, T{}, T{}, actual);
    return actual;
}

template <typename T>
void AtomicMailboxStore(T* pointer, T value) {
    T expected = AtomicMailboxLoad(pointer);
    while (!Common::AtomicCompareAndSwap(pointer, value, expected, expected)) {
    }
}

constexpr size_t MaxSaveRead = 64 * 1024 * 1024;
constexpr size_t MaxSavePath = 256;

// Rejects anything that could escape the resolved save directory: absolute paths, empty
// components (implies a leading/trailing/doubled '/'), and "." or ".." components. Deliberately
// stricter than Common::FS::SplitPathComponents needs to be -- this is the only thing standing
// between a module and the rest of the host filesystem.
bool SafeRelativeSavePath(const char* path) {
    if (!path)
        return false;
    size_t length = 0;
    while (path[length] != '\0') {
        if (++length > MaxSavePath)
            return false;
    }
    if (length == 0 || path[0] == '/')
        return false;
    std::string_view view{path, length};
    size_t start = 0;
    while (start <= view.size()) {
        const auto next = view.find('/', start);
        const auto component = view.substr(
            start, next == std::string_view::npos ? std::string_view::npos : next - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (next == std::string_view::npos)
            break;
        start = next + 1;
    }
    return true;
}

// Resolves the exact same on-disk location the running title's own save-data HLE calls resolve
// for its primary (Account-type, User-space, current profile) save -- read-only, never created.
FileSys::VirtualDir ResolveTitleSaveDirectory(Core::System& system, u64 title_id) {
    if (title_id == 0)
        return nullptr;
    try {
        const auto save_controller = system.GetFileSystemController().OpenSaveDataController();
        if (!save_controller)
            return nullptr;
        const auto user_id = system.GetProfileManager().GetLastOpenedUser().AsU128();
        const auto attribute = FileSys::SaveDataAttribute::Make(
            title_id, FileSys::SaveDataType::Account, user_id, FileSys::InvalidSystemSaveDataId);
        FileSys::VirtualDir dir;
        const auto result =
            save_controller->OpenSaveData(&dir, FileSys::SaveDataSpaceId::User, attribute);
        // Debug-only: the account UUID folder component is derived from Eden's *global* account
        // profile store (Service::Account::ProfileManager), not from this title's own isolated
        // nand_directory -- account identity is OS-level on real hardware, and this fork keeps
        // that scope here too, so the save path can differ from what a per-title profile alone
        // would suggest.
        LOG_DEBUG(Core, "DSMod save-read: title={:016X} user={:016X}{:016X} result={:08X} dir={}",
                  title_id, user_id[1], user_id[0], result.raw, dir ? "present" : "null");
        if (result.IsError() || !dir)
            return nullptr;
        return dir;
    } catch (...) {
        return nullptr;
    }
}
} // namespace

void ModRuntime::InitializeModuleExtensions() {
    module_extensions = {};
    if (!game_module || !game_module->Extensions())
        return;
    module_extensions.version = EDEN_DSMOD_EXT_VERSION;
    module_extensions.struct_size = sizeof(module_extensions);
    module_extensions.abi_hash = EDEN_DSMOD_EXT_HASH;
    module_extensions.userdata = this;
    const auto [address, size] = system.GetDualScreenGuestMailbox();
    module_extensions.mailbox_address = address;
    module_extensions.mailbox_size = size;
    module_extensions.load_u32 = [](void* p, u32 offset, u32* value) -> EdenDsmodBool {
        auto& rt = *static_cast<ModRuntime*>(p);
        auto* ptr = MailboxPointer<u32>(rt.module_extensions, rt.module_host, offset);
        if (!ptr || !value)
            return false;
        *value = AtomicMailboxLoad(ptr);
        return true;
    };
    module_extensions.load_u64 = [](void* p, u32 offset, u64* value) -> EdenDsmodBool {
        auto& rt = *static_cast<ModRuntime*>(p);
        auto* ptr = MailboxPointer<u64>(rt.module_extensions, rt.module_host, offset);
        if (!ptr || !value)
            return false;
        *value = AtomicMailboxLoad(ptr);
        return true;
    };
    module_extensions.store_u32 = [](void* p, u32 offset, u32 value) -> EdenDsmodBool {
        auto& rt = *static_cast<ModRuntime*>(p);
        auto* ptr = MailboxPointer<u32>(rt.module_extensions, rt.module_host, offset);
        if (!ptr)
            return false;
        AtomicMailboxStore(ptr, value);
        return true;
    };
    module_extensions.store_u64 = [](void* p, u32 offset, u64 value) -> EdenDsmodBool {
        auto& rt = *static_cast<ModRuntime*>(p);
        auto* ptr = MailboxPointer<u64>(rt.module_extensions, rt.module_host, offset);
        if (!ptr)
            return false;
        AtomicMailboxStore(ptr, value);
        return true;
    };
    module_extensions.decode_astc = [](void*, u32 width, u32 height, u32 bw, u32 bh,
                                       const u8* input, size_t input_size, u8* output,
                                       size_t output_size) -> EdenDsmodBool {
        constexpr std::array<std::pair<u32, u32>, 14> blocks{{{4, 4},
                                                              {5, 4},
                                                              {5, 5},
                                                              {6, 5},
                                                              {6, 6},
                                                              {8, 5},
                                                              {8, 6},
                                                              {8, 8},
                                                              {10, 5},
                                                              {10, 6},
                                                              {10, 8},
                                                              {10, 10},
                                                              {12, 10},
                                                              {12, 12}}};
        if (!input || !output || !width || !height || width > 2048 || height > 2048 ||
            std::find(blocks.begin(), blocks.end(), std::pair{bw, bh}) == blocks.end())
            return false;
        if (input_size != u64{(width + bw - 1) / bw} * ((height + bh - 1) / bh) * 16 ||
            output_size != u64{width} * height * 4)
            return false;
        Tegra::Texture::ASTC::Decompress({input, input_size}, width, height, 1, bw, bh,
                                         {output, output_size});
        return true;
    };
    game_module->Extensions()->configure(game_module_instance, &module_extensions);
}

void ModRuntime::InitializeModuleSaveExtensions() {
    module_save_api = {};
    if (!game_module || !game_module->SaveExtensions())
        return;
    {
        std::scoped_lock lock{module_save_mutex};
        module_save_dir = nullptr; // re-resolved lazily, on this title's first read
    }
    module_save_api.version = EDEN_DSMOD_SAVE_EXT_VERSION;
    module_save_api.struct_size = sizeof(module_save_api);
    module_save_api.abi_hash = EDEN_DSMOD_SAVE_EXT_HASH;
    module_save_api.userdata = this;
    module_save_api.read_save_file = [](void* p, const char* relative_path, u64 offset,
                                        void* output, size_t size) -> size_t {
        if (!SafeRelativeSavePath(relative_path) || size > MaxSaveRead)
            return 0;
        auto& rt = *static_cast<ModRuntime*>(p);
        std::scoped_lock lock{rt.module_save_mutex};
        if (!rt.module_save_dir) {
            rt.module_save_dir = ResolveTitleSaveDirectory(rt.system, rt.manifest.title_id);
            if (!rt.module_save_dir)
                return 0;
        }
        const auto file = rt.module_save_dir->GetFileRelative(relative_path);
        if (!file)
            return 0;
        const auto file_size = file->GetSize();
        if (!output)
            return file_size;
        if (offset >= file_size)
            return 0;
        const size_t count = std::min(size, file_size - static_cast<size_t>(offset));
        return file->Read(static_cast<u8*>(output), count, offset);
    };
    game_module->SaveExtensions()->configure(game_module_instance, &module_save_api);
}

void ModRuntime::InitializeModuleWriteExtensions() {
    module_write_api = {};
    if (!game_module || !game_module->WriteExtensions())
        return;
    module_write_api.version = EDEN_DSMOD_WRITE_EXT_VERSION;
    module_write_api.struct_size = sizeof(module_write_api);
    module_write_api.abi_hash = EDEN_DSMOD_WRITE_EXT_HASH;
    module_write_api.userdata = this;
    // One unit of guest writes: validated in full first, then checked against `expect` and stored
    // while no guest thread runs. Called from sample/tick (a CoreTiming callback): multi-core
    // suspends the application's threads without pausing core timing (which this very thread
    // drives); single-core already runs callbacks between guest time slices.
    module_write_api.write_batch = [](void* p, const EdenDsmodWriteOp* ops,
                                      u32 count) -> EdenDsmodBool {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (!ops || count == 0 || count > EDEN_DSMOD_WRITE_BATCH_MAX_OPS)
            return EDEN_DSMOD_FALSE;
        for (u32 i = 0; i < count; ++i) {
            const auto& op = ops[i];
            if (!op.value || op.reserved != 0 || op.size == 0 ||
                op.size > EDEN_DSMOD_WRITE_BATCH_MAX_BYTES ||
                !rt.module_host.is_mapped(p, op.address, op.size)) {
                return EDEN_DSMOD_FALSE;
            }
        }
        auto& memory = rt.system.ApplicationMemory();
        bool applied = false;
        const auto apply = [&] {
            std::array<u8, EDEN_DSMOD_WRITE_BATCH_MAX_BYTES> now{};
            for (u32 i = 0; i < count; ++i) {
                const auto& op = ops[i];
                if (op.expect && (!memory.ReadBlock(op.address, now.data(), op.size) ||
                                  std::memcmp(now.data(), op.expect, op.size) != 0)) {
                    return; // the guest changed underneath: write nothing
                }
            }
            for (u32 i = 0; i < count; ++i) {
                if (!memory.WriteBlock(ops[i].address, ops[i].value, ops[i].size))
                    return;
            }
            applied = true;
        };
        if (!rt.RunWithGuestStopped(apply)) {
            return EDEN_DSMOD_FALSE; // pause/resume in progress: the module retries later
        }
        return applied ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
    };
    game_module->WriteExtensions()->configure(game_module_instance, &module_write_api);
}

void ModRuntime::RunModuleAction(const std::string& name, s64 argument) {
    if (!game_module || !game_module_instance || !game_module->Extensions() ||
        !game_module->Extensions()->on_action || name.empty() || name.size() > 256)
        return;
    try {
        if (!game_module->Extensions()->on_action(game_module_instance, name.c_str(), argument)) {
            LOG_INFO(Core, "DSMod: module declined action '{}'", name);
        }
    } catch (...) {
        LOG_ERROR(Core, "DSMod: module action threw");
    }
}

void ModRuntime::StartModuleAssetWorker() {
    if (!game_module || !game_module->Extensions() || !game_module->Extensions()->load_image)
        return;
    const auto loader = game_module->Extensions()->load_image;
    const auto host = module_worker_host; // off the tick thread: see InitializeGameModule
    module_asset_worker = std::jthread([this, loader, host](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::string key;
            {
                std::unique_lock lock{module_asset_mutex};
                module_asset_cv.wait(lock, stop, [&] {
                    return !module_asset_queue.empty() && module_asset_completed.size() < 4;
                });
                if (stop.stop_requested())
                    break;
                key = std::move(module_asset_queue.front());
                module_asset_queue.pop_front();
            }
            Image result;
            try {
                std::scoped_lock loader_lock{module_loader_mutex};
                loader(game_module_instance, &host, key.c_str(), &result,
                       [](void* receiver, u32 width, u32 height, const u8* rgba, size_t size) {
                           const u64 bytes = u64{width} * height * 4;
                           if (!rgba || width == 0 || height == 0 || width > 4096 ||
                               height > 4096 || bytes > MaxImageBytes || size != bytes)
                               return;
                           auto& image = *static_cast<Image*>(receiver);
                           image.w = width;
                           image.h = height;
                           image.pixels.resize(size / 4);
                           for (size_t i = 0; i < image.pixels.size(); ++i) {
                               const auto* p = rgba + i * 4;
                               image.pixels[i] =
                                   (u32{p[3]} << 24) | (u32{p[0]} << 16) | (u32{p[1]} << 8) | p[2];
                           }
                       });
            } catch (...) {
                LOG_ERROR(Core, "DSMod: image decoder failed for '{}'", key);
            }
            std::scoped_lock lock{module_asset_mutex};
            if (result.Valid()) {
                if (ImageTimingEnabled()) {
                    module_asset_times[key].second = std::chrono::steady_clock::now();
                }
                module_asset_completed.insert_or_assign(key, std::move(result));
                module_asset_failed.erase(key);
            } else {
                module_asset_pending.erase(key);
                auto& failure = module_asset_failed[key];
                failure.at = std::chrono::steady_clock::now();
                ++failure.attempts;
                module_asset_times.erase(key); // no "landed" line for a failed image
            }
        }
    });
}

void ModRuntime::DrainModuleImages() {
    std::unordered_map<std::string, Image> ready;
    {
        std::scoped_lock lock{module_asset_mutex};
        ready.swap(module_asset_completed);
        for (const auto& [key, image] : ready)
            module_asset_pending.erase(key);
        if (ImageTimingEnabled()) {
            const auto now = std::chrono::steady_clock::now();
            const auto ms = [](auto d) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
            };
            for (const auto& [key, image] : ready) {
                if (const auto t = module_asset_times.find(key); t != module_asset_times.end()) {
                    LOG_INFO(Core,
                             "DSMod: module image '{}' landed: queue->decoded {} ms, "
                             "decoded->drained {} ms (queue {} left)",
                             key, ms(t->second.second - t->second.first),
                             ms(now - t->second.second), module_asset_queue.size());
                    module_asset_times.erase(t);
                }
            }
        }
    }
    if (ready.empty())
        return;
    // image_cache writer -- see CacheFindImage/CachePutImage's own comments.
    // Locked for this whole loop (an LRU sweep plus insert per landed asset -- cheap, uncontended
    // today, matches every other image_cache mutation's own grain).
    {
        std::scoped_lock lock{asset_cache_mutex};
        for (auto& [key, image] : ready) {
            const size_t bytes = image.pixels.size() * sizeof(u32);
            while (module_asset_bytes + bytes > ImageBudget && !module_asset_used.empty()) {
                const auto oldest = std::min_element(
                    module_asset_used.begin(), module_asset_used.end(),
                    [](const auto& a, const auto& b) { return a.second < b.second; });
                if (const auto cached = image_cache.find(oldest->first);
                    cached != image_cache.end()) {
                    module_asset_bytes -=
                        (cached->second ? cached->second->pixels.size() : 0) * sizeof(u32);
                    image_cache.erase(cached);
                }
                module_asset_used.erase(oldest);
            }
            module_asset_bytes += bytes;
            module_asset_used[key] = tick_count;
            image_cache.insert_or_assign(key, std::make_shared<const Image>(std::move(image)));
        }
    }
    ui_signature_valid = false;
    ++asset_epoch; // as PumpNxAssets: a page transition in flight repaints once the image lands
    module_images_landed = true;
    module_asset_cv.notify_all();
}

bool ModRuntime::LoadModuleImageSync(const std::string& key, Image& out) {
    if (key.size() > 256)
        return false;
    std::scoped_lock loader_lock{module_loader_mutex};
    if (!game_module || !game_module_instance || !game_module->Extensions() ||
        !game_module->Extensions()->load_image)
        return false;
    const auto host = module_worker_host; // off the tick thread: see InitializeGameModule
    Image result;
    try {
        game_module->Extensions()->load_image(
            game_module_instance, &host, key.c_str(), &result,
            [](void* receiver, u32 width, u32 height, const u8* rgba, size_t size) {
                const u64 bytes = u64{width} * height * 4;
                if (!rgba || width == 0 || height == 0 || width > 4096 || height > 4096 ||
                    bytes > MaxImageBytes || size != bytes)
                    return;
                auto& image = *static_cast<Image*>(receiver);
                image.w = width;
                image.h = height;
                image.pixels.resize(size / 4);
                for (size_t i = 0; i < image.pixels.size(); ++i) {
                    const auto* p = rgba + i * 4;
                    image.pixels[i] =
                        (u32{p[3]} << 24) | (u32{p[0]} << 16) | (u32{p[1]} << 8) | p[2];
                }
            });
    } catch (...) {
        LOG_ERROR(Core, "DSMod: module image '{}' threw", key);
        return false;
    }
    if (!result.Valid())
        return false;
    out = std::move(result);
    return true;
}

std::shared_ptr<const Image> ModRuntime::GetModuleImage(const std::string& key) {
    if (key.size() > 256)
        return nullptr;
    if (const auto found = CacheFindImage(key)) {
        // GetImage runs on the redraw worker too, while DrainModuleImages (tick thread) inserts and
        // evicts module_asset_used under asset_cache_mutex: an unguarded operator[] here raced that
        // (UB; an insert rehashing under the worker's lookup). Touch only an entry that exists, so
        // a key the eviction sweep just dropped is not re-added as a stale, image-less entry.
        std::scoped_lock lock{asset_cache_mutex};
        if (const auto used = module_asset_used.find(key); used != module_asset_used.end()) {
            used->second = tick_count;
        }
        return found;
    }
    if (!module_asset_worker.joinable())
        return nullptr;
    std::scoped_lock lock{module_asset_mutex};
    if (const auto failed = module_asset_failed.find(key); failed != module_asset_failed.end()) {
        // Retry a failed key after 2 s, 4 s, 6 s, 8 s; give up after 5 attempts.
        constexpr u32 MaxAttempts = 5;
        const auto wait = std::chrono::seconds{2 * failed->second.attempts};
        if (failed->second.attempts >= MaxAttempts ||
            std::chrono::steady_clock::now() - failed->second.at < wait)
            return nullptr;
    }
    if (module_asset_pending.size() >= 128 || module_asset_failed.size() >= 2048 ||
        !module_asset_pending.insert(key).second)
        return nullptr;
    module_asset_queue.push_back(key);
    if (ImageTimingEnabled()) {
        module_asset_times[key].first = std::chrono::steady_clock::now();
    }
    module_asset_cv.notify_one();
    return nullptr;
}

std::vector<u8> ModRuntime::LoadModuleData(const std::string& key) {
    // Runtime 12 "module:" byte source. Serialized with module shutdown (module_data_mutex); the
    // module may block here while it generates the data.
    constexpr size_t MaxModuleData = size_t{64} << 20;
    if (key.size() > 256) {
        return {};
    }
    std::scoped_lock data_lock{module_data_mutex};
    if (!game_module || !game_module_instance || !game_module->DataExtensions() ||
        !game_module->DataExtensions()->load_data) {
        return {};
    }
    const auto host = module_worker_host; // off the tick thread: see InitializeGameModule
    std::vector<u8> result;
    try {
        const bool ok = game_module->DataExtensions()->load_data(
            game_module_instance, &host, key.c_str(), &result,
            [](void* receiver, const u8* bytes, size_t size) {
                if ((bytes == nullptr && size != 0) || size > MaxModuleData) {
                    return;
                }
                auto& out = *static_cast<std::vector<u8>*>(receiver);
                out.assign(bytes, bytes + size);
            });
        if (!ok) {
            result.clear();
        }
    } catch (...) {
        LOG_ERROR(Core, "DSMod: module data '{}' threw", key);
        result.clear();
    }
    return result;
}

void ModRuntime::StartModuleAreas() {
    module_areas_ready.store(false, std::memory_order_release);
    if (manifest.map_areas_src.empty()) {
        return;
    }
    if (!game_module || !game_module->DataExtensions()) {
        LOG_ERROR(Core, "DSMod: map.areas_src '{}' needs a module data extension; the map keeps "
                        "its template areas",
                  manifest.map_areas_src);
        return;
    }
    const std::string src = manifest.map_areas_src;
    module_areas_thread = std::jthread([this, src](std::stop_token stop) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<u8> bytes = LoadModuleData(src);
        const auto t1 = std::chrono::steady_clock::now();
        if (stop.stop_requested()) {
            return;
        }
        std::unordered_map<std::string, MapArea> areas;
        bool ok = !bytes.empty();
        if (ok) {
            const auto json = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            ok = !json.is_discarded() && ParseMapAreasJson(json, areas) && !areas.empty();
        }
        const auto t2 = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        if (!ok) {
            LOG_ERROR(Core,
                      "DSMod: module map areas '{}' unavailable ({} bytes); the map keeps its "
                      "template areas",
                      src, bytes.size());
            return;
        }
        LOG_INFO(Core,
                 "DSMod: module map areas '{}': {} areas, {} bytes (module {:.1f} ms, parse {:.1f} "
                 "ms, worker thread)",
                 src, areas.size(), bytes.size(), ms(t0, t1), ms(t1, t2));
        std::scoped_lock lock{module_areas_mutex};
        module_areas_result = std::move(areas);
        module_areas_ready.store(true, std::memory_order_release);
    });
}

void ModRuntime::InstallModuleAreas() {
    // Tick thread, once. The redraw worker is the only other reader of manifest.map_areas (map
    // rasters, map widget draws); it is joined here and restarts lazily on the next dispatch, so
    // the swap below has no concurrent reader. The Nx and module image workers never touch areas.
    // Only the tick thread dispatches redraw jobs, so a worker found with no job running or queued
    // stays idle until this returns: joining it then costs nothing. While it is busy the install
    // waits for a later tick rather than for the job (bounded: after 60 busy ticks it joins).
    {
        std::scoped_lock lock{redraw_job_mutex};
        if ((redraw_job_running || redraw_pending_job) && ++module_areas_busy_ticks < 60) {
            return;
        }
    }
    std::optional<std::unordered_map<std::string, MapArea>> areas;
    {
        std::scoped_lock lock{module_areas_mutex};
        areas.swap(module_areas_result);
        module_areas_ready.store(false, std::memory_order_release);
    }
    if (!areas) {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    StopRedrawWorker();
    {
        std::scoped_lock mlk{map_state_mutex};
        manifest.map_areas = std::move(*areas);
        // Geometry-derived caches only. What the module published (visibility, walls, doors,
        // items, water, zone flags) is keyed by area name and does not depend on the geometry.
        // The publication generations are left alone: every image built from the template is
        // dropped below, and a generation of 0 keeps meaning "nothing published yet".
        map_geometry.clear();
        map_category.clear();
        map_border.clear();
    }
    map_water_solid.clear();
    map_water_pixels.clear();
    {
        std::scoped_lock ilk{asset_cache_mutex};
        std::erase_if(image_cache, [](const auto& entry) {
            const std::string& key = entry.first;
            for (const char* prefix : {"map:", "mapbase:", "prefog:", "watermask:", "magnet:",
                                       "pulse:", "overview:"}) {
                if (key.starts_with(prefix)) {
                    return true;
                }
            }
            return false;
        });
    }
    image_failed.clear();
    {
        std::scoped_lock composite_lock{gpu_composite_mutex};
        map_pub_current.reset();
        map_pub_previous.reset();
        map_pub_epoch = 0;
        last_map_key.clear();
        last_pulse_key.clear();
        last_pulse_present = false;
        last_map_fade_epoch = std::numeric_limits<u64>::max();
    }
    ui_signature_valid = false;
    widget_sig_page = ~size_t{0};
    interact_groups_ready = false;
    map_groups_ready = false;
    map_draw_records.clear();
    {
        std::scoped_lock rlk{map_records_mutex};
        map_draw_records_published.clear();
        map_records_page = ~size_t{0};
    }
    LOG_INFO(Core, "DSMod: installed {} module map areas in {:.2f} ms (tick thread, after {} busy "
                   "tick(s))",
             manifest.map_areas.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                 .count(),
             module_areas_busy_ticks);
}

} // namespace Core::Mods
