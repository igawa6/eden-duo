// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/memory.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {
namespace {
constexpr size_t MaxRead = 64 * 1024 * 1024;
constexpr size_t MaxName = 256;
constexpr size_t MaxText = 1024 * 1024;
constexpr size_t MaxMapObjects = 65536;

bool DsmodProfilingEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_DSMOD_PROFILE");
        return !value || (std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
                          std::strcmp(value, "FALSE") != 0);
    }();
    return enabled;
}

void RecordModuleProfile(bool tick, std::chrono::steady_clock::duration elapsed) {
    struct Counters {
        std::chrono::steady_clock::time_point report_at = std::chrono::steady_clock::now();
        u64 sample_count{};
        u64 tick_count{};
        std::chrono::nanoseconds sample_time{};
        std::chrono::nanoseconds tick_time{};
        std::chrono::nanoseconds max_sample{};
        std::chrono::nanoseconds max_tick{};
    };
    static Counters counters;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    if (tick) {
        ++counters.tick_count;
        counters.tick_time += ns;
        counters.max_tick = std::max(counters.max_tick, ns);
    } else {
        ++counters.sample_count;
        counters.sample_time += ns;
        counters.max_sample = std::max(counters.max_sample, ns);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.report_at < std::chrono::seconds{5}) {
        return;
    }
    const auto average_us = [](std::chrono::nanoseconds total, u64 count) {
        return count ? std::chrono::duration<double, std::micro>{total}.count() /
                           static_cast<double>(count)
                     : 0.0;
    };
    const auto max_us = [](std::chrono::nanoseconds value) {
        return std::chrono::duration<double, std::micro>{value}.count();
    };
    // Callback CPU times only. The frontend reports FPS separately; do not scan the cumulative
    // frame history or reset its performance counters on the timing thread.
    LOG_INFO(Core,
             "DSMod profile (CPU, 5s): sample={} avg={:.1f}us max={:.1f}us; tick={} "
             "avg={:.1f}us max={:.1f}us",
             counters.sample_count, average_us(counters.sample_time, counters.sample_count),
             max_us(counters.max_sample), counters.tick_count,
             average_us(counters.tick_time, counters.tick_count), max_us(counters.max_tick));
    counters = Counters{.report_at = now};
}

bool ValidString(const char* string, size_t limit) {
    if (!string) {
        return false;
    }
    for (size_t i = 0; i <= limit; ++i) {
        if (string[i] == '\0') {
            return true;
        }
    }
    return false;
}
} // namespace

void ModRuntime::ShutdownGameModule() {
    // The areas fetch may be blocked inside the module's load_data: let it finish before the
    // module goes (it holds no lock the rest of shutdown needs).
    if (module_areas_thread.joinable()) {
        module_areas_thread.request_stop();
        module_areas_thread.join();
    }
    {
        std::scoped_lock lock{module_areas_mutex};
        module_areas_result.reset();
        module_areas_ready.store(false, std::memory_order_release);
    }
    if (module_asset_worker.joinable()) {
        module_asset_worker.request_stop();
        module_asset_cv.notify_all();
        module_asset_worker.join();
    }
    {
        std::scoped_lock lock{module_asset_mutex};
        module_asset_queue.clear();
        module_asset_pending.clear();
        module_asset_failed.clear();
        module_asset_completed.clear();
    }
    {
        // Shutdown-path writer of image_cache -- guarded for consistency even
        // though today (module_asset_worker already stopped+joined above) nothing else touches it
        // concurrently here.
        std::scoped_lock lock{asset_cache_mutex};
        for (const auto& [key, used] : module_asset_used) {
            image_cache.erase(key);
        }
    }
    module_asset_used.clear();
    module_asset_bytes = 0;
    module_snapshot = nullptr;
    {
        std::scoped_lock lock{module_save_mutex};
        module_save_dir = nullptr;
    }
    // Any synchronous module image call (Nx worker composite layer) finishes before the instance
    // goes.
    std::scoped_lock loader_lock{module_loader_mutex};
    std::scoped_lock data_lock{module_data_mutex};
    if (game_module_instance && game_module) {
        try {
            game_module->Api()->destroy(game_module_instance);
        } catch (...) {
            LOG_ERROR(Core, "DSMod module: destroy callback threw an exception");
        }
    }
    game_module_instance = nullptr;
    game_module.reset();
}

void ModRuntime::InitializeGameModule() {
    ShutdownGameModule();
    game_module = GameModule::Load(manifest.asset_dir, manifest.title_id, manifest.running_build_id,
                                   module_error);
    if (!game_module) {
        return;
    }
    module_host = {};
    module_host.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    module_host.struct_size = sizeof(module_host);
    module_host.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    module_host.capabilities =
        EDEN_DSMOD_CAP_ROMFS_READ | EDEN_DSMOD_CAP_MAP_OUTPUT | EDEN_DSMOD_CAP_EXTENSIONS;
    module_host.userdata = this;
    module_host.title_id = manifest.title_id;
    module_host.main_base = main_region_begin;
    module_host.main_size = main_region_size;
    for (size_t i = 0;
         i < sizeof(module_host.build_id) && i * 2 + 1 < manifest.running_build_id.size(); ++i) {
        module_host.build_id[i] =
            static_cast<u8>(std::stoul(manifest.running_build_id.substr(i * 2, 2), nullptr, 16));
    }
    module_host.get_tick = [](void* p) { return static_cast<ModRuntime*>(p)->tick_count; };
    module_host.get_heap_begin = [](void* p) { return static_cast<ModRuntime*>(p)->HeapLow(); };
    module_host.get_heap_end = [](void* p) { return static_cast<ModRuntime*>(p)->HeapHigh(); };
    module_host.is_mapped = [](void* p, u64 address, u64 size) -> EdenDsmodBool {
        return size <= MaxRead && address <= std::numeric_limits<u64>::max() - size &&
               static_cast<ModRuntime*>(p)->AddressIsSane(address, size);
    };
    module_host.read_memory = [](void* p, u64 address, void* output, size_t size) -> EdenDsmodBool {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (!output || !rt.module_host.is_mapped(p, address, size)) {
            return EDEN_DSMOD_FALSE;
        }
        return rt.system.ApplicationMemory().ReadBlock(address, output, size);
    };
    module_host.get_read_pointer = [](void* p, u64 address, size_t size) -> const u8* {
        auto& rt = *static_cast<ModRuntime*>(p);
        // A single guest page is contiguous. Never promise a contiguous host pointer across
        // guest page boundaries, even when both ranges individually exist.
        if (size == 0 || size > 4096 - (address & 4095) ||
            !rt.module_host.is_mapped(p, address, size)) {
            return nullptr;
        }
        return rt.system.ApplicationMemory().GetPointerSilent(address);
    };
    // Bounded guest writes: the same ApplicationMemory store path a manifest "write" action
    // (LA's X/Y equip) and the console's writeb use, so it works under NCE and Dynarmic alike.
    // At most MaxWrite bytes per call into mapped guest memory; naturally aligned 1/2/4/8-byte
    // stores are single stores. Modules gate what they write; this only bounds the range.
    module_host.capabilities |= EDEN_DSMOD_CAP_WRITE_MEMORY;
    module_host.write_memory = [](void* p, u64 address, const void* input,
                                  size_t size) -> EdenDsmodBool {
        constexpr size_t MaxWrite = 64;
        auto& rt = *static_cast<ModRuntime*>(p);
        if (!input || size == 0 || size > MaxWrite || !rt.module_host.is_mapped(p, address, size)) {
            return EDEN_DSMOD_FALSE;
        }
        auto& memory = rt.system.ApplicationMemory();
        const bool aligned = (address & (size - 1)) == 0;
        if (aligned && size == 1) {
            memory.Write8(address, *static_cast<const u8*>(input));
        } else if (aligned && size == 2) {
            u16 v;
            std::memcpy(&v, input, 2);
            memory.Write16(address, v);
        } else if (aligned && size == 4) {
            u32 v;
            std::memcpy(&v, input, 4);
            memory.Write32(address, v);
        } else if (aligned && size == 8) {
            u64 v;
            std::memcpy(&v, input, 8);
            memory.Write64(address, v);
        } else if (!memory.WriteBlock(address, input, size)) {
            return EDEN_DSMOD_FALSE;
        }
        return EDEN_DSMOD_TRUE;
    };
    module_host.log = [](void*, u32 level, const char* message) {
        if (!ValidString(message, MaxText)) {
            return;
        }
        switch (level) {
        case EDEN_DSMOD_LOG_DEBUG:
            LOG_DEBUG(Core, "{}", message);
            break;
        case EDEN_DSMOD_LOG_WARNING:
            LOG_WARNING(Core, "{}", message);
            break;
        case EDEN_DSMOD_LOG_ERROR:
            LOG_ERROR(Core, "{}", message);
            break;
        default:
            LOG_INFO(Core, "{}", message);
            break;
        }
    };
    module_host.begin_output = [](void*) {};
    module_host.end_output = [](void*) {};
    module_host.publish_i64 = [](void* p, const char* name, s64 value) {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (rt.module_snapshot && ValidString(name, MaxName)) {
            rt.module_snapshot->ints[name] = value;
        }
    };
    module_host.publish_f64 = [](void* p, const char* name, double value) {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (rt.module_snapshot && ValidString(name, MaxName) && std::isfinite(value)) {
            rt.module_snapshot->floats[name] = value;
        }
    };
    module_host.publish_text = [](void* p, const char* name, const char* value) {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (!rt.module_snapshot || !ValidString(name, MaxName) || !ValidString(value, MaxText)) {
            return;
        }
        if (std::strcmp(name, "__map_state") == 0) {
            rt.AcceptModuleMapState(value);
        } else {
            rt.module_snapshot->texts[name] = value;
        }
    };
    module_host.publish_address = [](void* p, const char* name, u64 value) {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (rt.module_snapshot && ValidString(name, MaxName)) {
            rt.module_snapshot->addresses[name] = value;
        }
    };
    module_host.publish_map = [](void* p, const EdenDsmodMapFrame* frame) {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (rt.module_snapshot && frame && frame->struct_size == sizeof(*frame)) {
            rt.AcceptModuleMap(*frame);
        }
    };
    module_host.get_i64 = [](void* p, const char* name, s64 fallback) -> s64 {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (!ValidString(name, MaxName)) {
            return fallback;
        }
        if (std::strcmp(name, "__relocation_delta") == 0) {
            return rt.nce_vtable_delta;
        }
        if (rt.module_snapshot) {
            const auto value = rt.module_snapshot->ints.find(name);
            if (value != rt.module_snapshot->ints.end()) {
                return value->second;
            }
        }
        return fallback;
    };
    module_host.get_f64 = [](void* p, const char* name, double fallback) -> double {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (rt.module_snapshot && ValidString(name, MaxName)) {
            const auto value = rt.module_snapshot->floats.find(name);
            if (value != rt.module_snapshot->floats.end()) {
                return value->second;
            }
        }
        return fallback;
    };
    module_host.get_text = [](void* p, const char* name) -> const char* {
        auto& rt = *static_cast<ModRuntime*>(p);
        if (!ValidString(name, MaxName)) {
            return nullptr;
        }
        constexpr std::string_view SequencePrefix{"__sequence:"};
        if (std::string_view{name}.starts_with(SequencePrefix)) {
            const auto value = rt.sequence_texts.find(name + SequencePrefix.size());
            return value == rt.sequence_texts.end() ? nullptr : value->second.c_str();
        }
        if (rt.module_snapshot) {
            const auto value = rt.module_snapshot->texts.find(name);
            if (value != rt.module_snapshot->texts.end()) {
                return value->second.c_str();
            }
        }
        return nullptr;
    };
    module_host.read_romfs = [](void* p, const char* path, u64 offset, void* output,
                                size_t size) -> size_t {
        if (!ValidString(path, 4096) || size > MaxRead) {
            return 0;
        }
        auto& rt = *static_cast<ModRuntime*>(p);
        std::scoped_lock asset_lock{rt.module_romfs_mutex};
        std::string source{path};
        if (!source.starts_with("romfs:") && !source.starts_with("file:")) {
            source.insert(0, "romfs:");
        }
        // Range reads keep large ROMFS archives out of the 60 Hz reader path and avoid
        // allocating/rereading the entire file for every size/chunk request.
        if (source.find('#') == std::string::npos) {
            FileSys::VirtualFile file;
            if (source.starts_with("file:")) {
                file = rt.manifest.asset_dir
                           ? rt.manifest.asset_dir->GetFileRelative(source.substr(5))
                           : nullptr;
            } else {
                if (!rt.romfs_tried) {
                    // Initialize the existing session-owned ROMFS root with an empty-path probe.
                    rt.ReadAssetBytesRaw("romfs:");
                }
                auto relative = source.substr(6);
                while (!relative.empty() && relative.front() == '/')
                    relative.erase(0, 1);
                file = rt.romfs_root ? rt.romfs_root->GetFileRelative(relative) : nullptr;
            }
            if (!file)
                return 0;
            if (!output)
                return file->GetSize();
            if (offset >= file->GetSize())
                return 0;
            return file->Read(static_cast<u8*>(output),
                              std::min(size, file->GetSize() - static_cast<size_t>(offset)),
                              offset);
        }
        const auto bytes = rt.ReadAssetBytes(source);
        if (!output) {
            return bytes.size();
        }
        if (offset >= bytes.size()) {
            return 0;
        }
        const size_t count = std::min(size, bytes.size() - static_cast<size_t>(offset));
        std::memcpy(output, bytes.data() + offset, count);
        return count;
    };
    if ((game_module->Api()->capabilities & ~module_host.capabilities) != 0) {
        module_error = "This dual-screen module requires unsupported host capabilities.";
        ShutdownGameModule();
        return;
    }
    try {
        // A data-generating module (runtime 12) starts reading romfs from inside create() on its
        // own thread: resolve the session filesystem here, on its owner thread, first.
        if (game_module->DataExtensions() || !manifest.map_areas_src.empty()) {
            ReadAssetBytesRaw("romfs:");
        }
        const auto file = manifest.asset_dir->GetFile("manifest.json");
        auto config = nlohmann::json::parse(file->ReadAllBytes());
        config["_build_match"] = !manifest.build_id_file.empty();
        config["_build_id"] = manifest.running_build_id;
        game_module_instance = game_module->Api()->create(&module_host, config.dump().c_str());
        if (game_module_instance) {
            InitializeModuleExtensions();
            InitializeModuleSaveExtensions();
            InitializeModuleWriteExtensions();
            StartModuleAssetWorker();
            StartModuleAreas();
        }
    } catch (const std::exception& error) {
        module_error = error.what();
    } catch (...) {
        module_error = "The dual-screen module threw during initialization.";
    }
    if (!game_module_instance) {
        if (module_error.empty()) {
            module_error = "The dual-screen module could not initialize.";
        }
        ShutdownGameModule();
        LOG_ERROR(Core, "DSMod module: {}", module_error);
    }
}

void ModRuntime::RunGameModule(StateSnapshot& snapshot, bool tick) {
    snapshot.ints["module_ready"] = game_module_instance ? 1 : 0;
    snapshot.ints["module_error"] = module_error.empty() ? 0 : 1;
    if (!module_error.empty()) {
        snapshot.texts["module_error_message"] = module_error;
        snapshot.ints["build_match"] = 0;
    }
    if (!game_module_instance) {
        return;
    }
    module_snapshot = &snapshot;
    module_host.main_base = main_region_begin;
    module_host.main_size = main_region_size;
    const bool profile = DsmodProfilingEnabled();
    const auto profile_start =
        profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    try {
        if (tick) {
            game_module->Api()->tick(game_module_instance, &module_host);
        } else {
            game_module->Api()->sample(game_module_instance, &module_host);
        }
    } catch (...) {
        if (profile) {
            RecordModuleProfile(tick, std::chrono::steady_clock::now() - profile_start);
        }
        module_error = "The dual-screen module threw during execution.";
        LOG_ERROR(Core, "DSMod module: {}", module_error);
        ShutdownGameModule();
        snapshot.ints["module_ready"] = 0;
        snapshot.ints["module_error"] = 1;
        snapshot.ints["build_match"] = 0;
        snapshot.texts["module_error_message"] = module_error;
    }
    if (profile && game_module_instance) {
        RecordModuleProfile(tick, std::chrono::steady_clock::now() - profile_start);
    }
    module_snapshot = nullptr;
    if (const auto scenario = snapshot.texts.find("scenario"); scenario != snapshot.texts.end()) {
        live_scenario = scenario->second;
    }
    if (const auto ready = snapshot.ints.find("map_ready"); ready != snapshot.ints.end()) {
        last_in_game = ready->second;
    }
}

bool ModRuntime::LockMapStateUnlessRaster(MapStateLock& lock) const {
    // Short holders (the worker's marker pass, a fade-weight pass) are waited out, but only
    // briefly: the redraw worker runs at a low OS priority and can be preempted while it holds
    // the mutex, and a tick that waits for it then inherits the stall (measured: 26 ms on a
    // loaded desktop). Past the grace period the caller defers exactly as it does for a raster.
    constexpr auto Grace = std::chrono::microseconds{250};
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (lock.try_lock()) {
            return true;
        }
        if (map_raster_busy.load(std::memory_order_acquire) != 0 ||
            std::chrono::steady_clock::now() - start > Grace) {
            return false;
        }
        std::this_thread::yield();
    }
}

void ModRuntime::AcceptModuleMap(const EdenDsmodMapFrame& frame) {
    if (!ValidString(frame.area, MaxName) || !manifest.map_areas.contains(frame.area)) {
        return;
    }
    // Tick thread (RunGameModule, during sampling). The publication is validated and copied here
    // and applied under map_state_mutex by ApplyMapUpdateLocked -- right away unless the redraw
    // worker is rasterising a map, which holds that mutex for 25-250 ms (several times that on
    // a handheld). Waiting it out here stalled the core-timing thread, and with it the guest's
    // vsync, for the whole raster; now the update waits instead (ApplyPendingMapUpdates, next
    // tick). Sections are validated in the original order and a malformed one still ends the
    // publication there, with the earlier sections applied. Custom markers go to this tick's
    // snapshot, which needs no lock.
    PendingMapUpdate update;
    update.area = frame.area;
    update.tick = tick_count;
    const auto queue = [&] {
        if (update.has_visibility || update.has_water || update.has_walls || update.has_zone) {
            pending_map_updates.push_back(std::move(update));
        }
        ApplyPendingMapUpdates();
    };
    if ((frame.flags & EDEN_DSMOD_MAP_UPDATE_VISIBILITY) != 0) {
        const size_t count = static_cast<size_t>(VisitedGrid::Cols) * VisitedGrid::Rows;
        if (frame.visibility_columns != VisitedGrid::Cols ||
            frame.visibility_rows != VisitedGrid::Rows || frame.visibility_count != count ||
            !frame.visibility ||
            !std::all_of(frame.visibility, frame.visibility + count,
                         [](u8 cell) { return cell <= 2; })) {
            return;
        }
        update.has_visibility = true;
        update.visibility.assign(frame.visibility, frame.visibility + count);
        if (frame.visibility_previous) {
            update.visibility_previous.assign(frame.visibility_previous,
                                              frame.visibility_previous + count);
        }
        if (frame.visibility_change_ticks) {
            update.visibility_change_ticks.assign(frame.visibility_change_ticks,
                                                  frame.visibility_change_ticks + count);
        }
    }
    if ((frame.flags & EDEN_DSMOD_MAP_UPDATE_WATER) != 0 && frame.water_count <= MaxMapObjects &&
        (frame.water || frame.water_count == 0)) {
        std::vector<std::array<float, 4>> next;
        next.reserve(frame.water_count);
        for (size_t i = 0; i < frame.water_count; ++i) {
            const auto& r = frame.water[i];
            if (!std::isfinite(r.min_x) || !std::isfinite(r.min_y) || !std::isfinite(r.max_x) ||
                !std::isfinite(r.max_y)) {
                queue();
                return;
            }
            next.push_back({r.min_x, r.min_y, r.max_x, r.max_y});
        }
        update.has_water = true;
        update.water = std::move(next);
    }
    if ((frame.flags & EDEN_DSMOD_MAP_UPDATE_WALLS) != 0 && frame.wall_count <= MaxMapObjects &&
        (frame.walls || frame.wall_count == 0)) {
        std::vector<WallTile> next;
        next.reserve(frame.wall_count);
        for (size_t i = 0; i < frame.wall_count; ++i) {
            const auto& tile = frame.walls[i];
            if (!std::isfinite(tile.x) || !std::isfinite(tile.y)) {
                queue();
                return;
            }
            next.push_back({tile.x, tile.y, tile.type, tile.color});
        }
        update.has_walls = true;
        update.walls = std::move(next);
    }
    if ((frame.flags & EDEN_DSMOD_MAP_UPDATE_POINTS) != 0 && frame.point_count <= MaxMapObjects &&
        (frame.points || frame.point_count == 0)) {
        module_snapshot->custom_markers.clear();
        for (size_t i = 0; i < frame.point_count; ++i) {
            const auto& point = frame.points[i];
            if ((point.flags & EDEN_DSMOD_POINT_CUSTOM) != 0 && std::isfinite(point.x) &&
                std::isfinite(point.y)) {
                module_snapshot->custom_markers.push_back(
                    {point.x, point.y, static_cast<s32>(point.color)});
            }
        }
    }
    if ((frame.flags & EDEN_DSMOD_MAP_UPDATE_ZONE) != 0) {
        update.has_zone = true;
        update.unlocked = (frame.flags & EDEN_DSMOD_MAP_UNLOCKED) != 0;
        update.inactive = (frame.flags & EDEN_DSMOD_MAP_ZONE_INACTIVE) != 0;
        update.alert = (frame.flags & EDEN_DSMOD_MAP_ZONE_ALERT) != 0;
    }
    queue();
}

void ModRuntime::AcceptModuleMapState(const char* json) {
    PendingMapUpdate update;
    try {
        const auto state = nlohmann::json::parse(json);
        const auto area = state.at("area").get<std::string>();
        if (!manifest.map_areas.contains(area)) {
            return;
        }
        constexpr std::array keys{"dead_actors",  "dispelled_regions", "open_doors",
                                  "picked_items", "unveiled_items",    "veiled_items"};
        static_assert(keys.size() == std::tuple_size_v<decltype(update.state_sets)>);
        // Validate the whole publication before replacing any live actor set.
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto& value = state.at(keys[i]);
            if (!value.is_array() || value.size() > MaxMapObjects) {
                return;
            }
            update.state_sets[i] = value.get<std::set<std::string>>();
        }
        update.is_state = true;
        update.area = area;
        update.tick = tick_count;
    } catch (const nlohmann::json::exception& error) {
        LOG_ERROR(Core, "DSMod module: invalid map state: {}", error.what());
        return;
    }
    // Applied like AcceptModuleMap's publications, in the same queue (arrival order).
    pending_map_updates.push_back(std::move(update));
    ApplyPendingMapUpdates();
}

void ModRuntime::ApplyPendingMapUpdates() {
    if (pending_map_updates.empty()) {
        return;
    }
    MapStateLock lock{map_state_mutex, std::defer_lock};
    if (!LockMapStateUnlessRaster(lock)) {
        return; // a raster holds the map state: retried next tick
    }
    while (!pending_map_updates.empty()) {
        ApplyMapUpdateLocked(pending_map_updates.front());
        pending_map_updates.pop_front();
    }
}

void ModRuntime::ApplyMapUpdateLocked(const PendingMapUpdate& update) {
    // Writes map_visited, map_walls, water_boxes, the generations and the zone flags, and (a
    // state publication) the six actor sets -- all read by GetImage's map raster on the redraw
    // worker; the caller holds map_state_mutex. A manifest reload in between may have dropped the
    // area: skip it then, exactly as its arrival check would have.
    const std::string& area = update.area;
    if (!manifest.map_areas.contains(area)) {
        return;
    }
    if (update.is_state) {
        bool changed = false;
        const auto apply = [&](auto& values, size_t index) {
            if (values[area] != update.state_sets[index]) {
                values[area] = update.state_sets[index];
                changed = true;
            }
        };
        apply(map_occ_dead, 0);
        apply(map_vig_dispelled, 1);
        apply(map_door_open, 2);
        apply(map_item_picked, 3);
        apply(map_item_unveiled, 4);
        apply(map_item_veiled, 5);
        if (changed) {
            ++wall_gen;
            UpdateHiddenMarkers(area); // re-locks map_state_mutex: recursive
        }
        return;
    }
    if (update.has_visibility) {
        const size_t count = update.visibility.size();
        auto& grid = map_visited[area];
        bool changed = false;
        for (size_t i = 0; i < count; ++i) {
            if (grid.cells[i] != update.visibility[i]) {
                grid.prev[i] = !update.visibility_previous.empty()
                                   ? update.visibility_previous[i]
                                   : grid.cells[i];
                grid.cells[i] = update.visibility[i];
                grid.change_tick[i] = !update.visibility_change_ticks.empty()
                                          ? update.visibility_change_ticks[i]
                                          : static_cast<u32>(update.tick);
                changed = true;
            }
        }
        if (changed) {
            ++grid.generation;
            grid.last_reveal_tick = update.tick;
        }
        game_vis_areas.insert(area);
    }
    if (update.has_water && water_boxes != update.water) {
        water_boxes = update.water;
        ++water_gen;
    }
    if (update.has_walls && map_walls[area] != update.walls) {
        map_walls[area] = update.walls;
        ++wall_gen;
    }
    if (update.has_zone) {
        if (update.unlocked != map_unlocked || update.inactive != map_zone_inactive) {
            ++zone_gen;
        }
        map_unlocked = update.unlocked;
        map_zone_inactive = update.inactive;
        map_zone_alert = update.alert;
    }
}

} // namespace Core::Mods
