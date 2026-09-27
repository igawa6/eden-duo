// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// mod_re_tools.cpp -- the DSMod RE/heap-analysis developer toolkit (world-position scanning,
// heap/array/class dumping, pointer-chain tracing, Lua registry inspection), kept out of
// mod_runtime.cpp so the boundary between the shipped runtime and this desktop-only tooling is
// physical, not just a scattering of #if EDEN_DSMOD_BUILD_DEV_TOOLS blocks. Each entry point is
// a FooImpl() called from a same-named ModRuntime::Foo() shell in mod_runtime.cpp (empty when
// EDEN_DSMOD_BUILD_DEV_TOOLS=OFF, so a release build's mod_runtime.cpp.o is unaffected by this
// file's existence).

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <span>
#include <sstream>
#include "common/stb.h"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "bc_decoder.h"
#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/stb.h"
#include "common/thread.h"
#include "core/arm/arm_interface.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/mods/map_cache_identity.h"
#include "core/mods/map_overview.h"
#include "core/mods/mod_hooks.h"
#include "core/mods/mod_module.h"
#include "core/mods/mod_nx_assets.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/gpu.h"

namespace Core::Mods {

#if EDEN_DSMOD_BUILD_DEV_TOOLS

/// How far back to look for the vtable that names a value's class. Dread puts energy at +0x65C
/// of its object -- the published infinite-health cheat writes exactly there -- so a 0x400 walk
/// steps straight over the only candidate that matters.
constexpr s64 ObjectWalkBack = 0x2000;

/// Log a window of guest memory around a published address, as words and as floats. Sampling it
/// while the game moves is what turns "the player object is somewhere in here" into an offset.
void ModRuntime::DumpWatchedMemoryImpl() {
    if (dump_spec.empty() || (tick_count % 120) != 0) {
        return;
    }
    // "name:a>b:bytes[,name:c:bytes...]" -- several chains per run. Each run costs minutes of
    // menu navigation before the game is even playable, so following one pointer at a time is
    // the expensive way to find anything.
    for (size_t start = 0; start <= dump_spec.size();) {
        const auto comma = dump_spec.find(',', start);
        DumpOneWatch(dump_spec.substr(start, comma - start));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
}

void ModRuntime::DumpOneWatchImpl([[maybe_unused]] const std::string& dump_spec) {
    const auto colon = dump_spec.find(':');
    const auto colon2 = dump_spec.rfind(':');
    if (colon == std::string::npos || colon2 == colon) {
        return;
    }
    const auto name = dump_spec.substr(0, colon);
    const auto path = dump_spec.substr(colon + 1, colon2 - colon - 1);
    const u64 bytes = std::strtoull(dump_spec.substr(colon2 + 1).c_str(), nullptr, 0);
    const auto found = sequence_addresses.find(name);
    if (found == sequence_addresses.end() || found->second == 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    // "a>b>c": add a and dereference, add b and dereference, then add c. An object the game hands
    // us is rarely the object we want -- it is usually a handle holding a pointer to it.
    VAddr base = static_cast<VAddr>(found->second);
    size_t start = 0;
    while (start <= path.size()) {
        const auto arrow = path.find('>', start);
        base +=
            static_cast<VAddr>(std::strtoll(path.substr(start, arrow - start).c_str(), nullptr, 0));
        if (arrow == std::string::npos) {
            break;
        }
        if (!AddressIsSane(base, sizeof(u64))) {
            return;
        }
        base = memory.Read64(base);
        start = arrow + 1;
    }
    std::string words, floats;
    for (u64 i = 0; i < bytes && i < 0x400; i += sizeof(u32)) {
        if (!AddressIsSane(base + i, sizeof(u32))) {
            break;
        }
        const u32 raw = memory.Read32(base + i);
        float value{};
        std::memcpy(&value, &raw, sizeof(value));
        words += fmt::format(" {:08X}", raw);
        floats += fmt::format(" {:g}", std::isfinite(value) ? value : 0.0f);
    }
    LOG_INFO(Core, "DSMod dump {}:{} @{:016X} words:{}", name, path, base, words);
    LOG_INFO(Core, "DSMod dump {}:{} floats:{}", name, path, floats);
}

/// Look for a world position the way one is actually found: by watching, not by reading structs.
///
/// First pass collects every 4-byte float in a window around an object we can already reach, whose
/// magnitude is plausible for a coordinate. Later passes re-read those addresses and track how far
/// each has swung. Walk the character back and forth and the position falls out as the field with
/// the largest travel -- no knowledge of the game's classes required.
void ModRuntime::ScanForMovingFloatsImpl() {
    if (!InGameplay()) {
        return;
    }
    if (scan_spec.empty() || (tick_count % 120) != 0) {
        return;
    }
    const auto colon = scan_spec.find(':');
    if (colon == std::string::npos) {
        return;
    }
    const auto anchor_name = scan_spec.substr(0, colon);
    const auto found = sequence_addresses.find(anchor_name);
    if (found == sequence_addresses.end() || found->second == 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    if (scan_end == 0) {
        if (tick_count < 9000) { // ~150 s: past the menus, into gameplay
            return;
        }
        // "<anchor>:<megabytes>[:<minimum magnitude>]". A span of hundreds of megabytes is fine:
        // collection is incremental, a slice per invocation, so the emulator never stalls on one
        // tick while the net is cast over the whole heap.
        const auto rest = scan_spec.substr(colon + 1);
        const auto second = rest.find(':');
        const u64 span = std::strtoull(rest.substr(0, second).c_str(), nullptr, 0) << 20;
        scan_min_magnitude = second == std::string::npos
                                 ? 1.0f
                                 : std::strtof(rest.substr(second + 1).c_str(), nullptr);
        scan_cursor =
            static_cast<VAddr>(found->second) > span ? static_cast<VAddr>(found->second) - span : 0;
        scan_end = found->second + (span >> 4); // mostly below the anchor: heaps grow upward
        LOG_INFO(Core, "DSMod scan: sweeping {:016X}..{:016X}", scan_cursor, scan_end);
        return;
    }
    if (scan_cursor < scan_end) {
        constexpr size_t MaxCandidates = 900000;
        constexpr u64 SlicePerTick = 96ULL << 20;
        const VAddr slice_end = std::min<VAddr>(scan_cursor + SlicePerTick, scan_end);
        // Work through host pointers, a page at a time. Reading candidate words one guest access
        // at a time slowed the tick thread so far that a 15-second reporting window took minutes
        // of wall clock -- the scan looked dead while it was merely glacial.
        for (VAddr page = scan_cursor & ~0xFFFULL;
             page < slice_end && scan_addresses.size() < MaxCandidates; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            for (u32 in_page = 0; in_page < 0x1000; in_page += 4) {
                u32 bits{};
                std::memcpy(&bits, host + in_page, sizeof(bits));
                float value{};
                std::memcpy(&value, &bits, sizeof(value));
                if (!std::isfinite(value) || std::fabs(value) <= scan_min_magnitude ||
                    std::fabs(value) >= 1e7f) {
                    continue;
                }
                // Never trust a float that is really half of a pointer: this heap sits at
                // 0x21C1'xxxx'xxxx, so pointer low words read as floats around -13.
                u64 wide{};
                std::memcpy(&wide, host + (in_page & ~7u), sizeof(wide));
                if (wide >= 0x2000000000ULL && wide < 0x8000000000ULL) {
                    continue;
                }
                scan_addresses.push_back(page + in_page);
                scan_host.push_back(host + in_page);
                scan_low.push_back(value);
                scan_high.push_back(value);
                if (scan_addresses.size() >= MaxCandidates) {
                    break;
                }
            }
        }
        scan_cursor = slice_end;
        if (scan_cursor >= scan_end || scan_addresses.size() >= MaxCandidates) {
            scan_cursor = scan_end;
            scan_started = tick_count;
            LOG_INFO(Core, "DSMod scan: watching {} candidate floats around {} ({:016X})",
                     scan_addresses.size(), anchor_name, found->second);
        }
        return;
    }
    for (size_t i = 0; i < scan_host.size(); ++i) {
        float value{};
        std::memcpy(&value, scan_host[i], sizeof(value));
        if (!std::isfinite(value)) {
            continue;
        }
        scan_low[i] = std::min(scan_low[i], value);
        scan_high[i] = std::max(scan_high[i], value);
    }
    if (tick_count - scan_started < 900) { // one reporting window of walking
        return;
    }
    std::vector<size_t> best(scan_addresses.size());
    std::iota(best.begin(), best.end(), size_t{0});
    std::ranges::sort(best, [this](size_t a, size_t b) {
        return (scan_high[a] - scan_low[a]) > (scan_high[b] - scan_low[b]);
    });
    for (size_t rank = 0; rank < std::min<size_t>(12, best.size()); ++rank) {
        const size_t i = best[rank];
        if (rank > 0 && scan_high[i] - scan_low[i] <= 0.01f) {
            break; // nothing else is moving at all
        }
        LOG_INFO(Core, "DSMod scan: {:016X} swung {:.1f} ({:.1f} .. {:.1f})", scan_addresses[i],
                 scan_high[i] - scan_low[i], scan_low[i], scan_high[i]);
        if (rank == 0 && scan_high[i] - scan_low[i] > 1.0f) {
            scan_best = fmt::format("{:016X}", scan_addresses[i]);
            TraceChainTo(scan_addresses[i]);
        }
    }
    // The winner's surroundings, and its owner. A position is a vector, so its neighbours say
    // which component this is; and the nearest preceding vtable pointer both finds the object's
    // start and names its class -- which is the first step of turning a lucky absolute address
    // into a pointer chain a package can actually ship.
    if (!best.empty() && scan_high[best[0]] - scan_low[best[0]] > 1.0f) {
        const VAddr hit = scan_addresses[best[0]];
        std::string hood;
        for (s64 off = -0x20; off <= 0x2C; off += 4) {
            const VAddr at = hit + off;
            if (!AddressIsSane(at, 4)) {
                continue;
            }
            const u32 bits = memory.Read32(at);
            float value{};
            std::memcpy(&value, &bits, sizeof(value));
            hood += fmt::format(" [{:+#x}]={:.2f}/{:08X}", off, value, bits);
        }
        LOG_INFO(Core, "DSMod scan hood:{}", hood);
        for (VAddr back = hit & ~7ULL; back > hit - 0x800 && AddressIsSane(back, 8); back -= 8) {
            const u64 candidate = memory.Read64(back);
            if (candidate >= main_region_begin &&
                candidate < main_region_begin + main_region_size) {
                LOG_INFO(Core, "DSMod scan owner: object? {:016X} vtable main+{:X} field +{:#x}",
                         back, candidate - main_region_begin, hit - back);
                break;
            }
        }
    }
    // Re-baseline every window. Without this a value that rose from zero once during start-up
    // outranks a coordinate that genuinely oscillates, because the range is measured for all time
    // -- which is exactly what the first run reported: round numbers climbing from 0.
    for (size_t i = 0; i < scan_host.size(); ++i) {
        float value{};
        std::memcpy(&value, scan_host[i], sizeof(value));
        scan_low[i] = scan_high[i] = std::isfinite(value) ? value : 0.0f;
    }
    scan_started = tick_count;
}

/// Write every mapped page of the module and heap to one file, through host pointers. This is the
/// raw material for re-anchoring: a scan hit is an absolute address that dies with the boot, and
/// the only way to turn it into something a package can ship is to find, offline, a chain of
/// pointers leading to it from the module's static data. Format: repeated [u64 addr][u32
/// len][bytes].
void ModRuntime::DumpHeapSnapshotImpl() {
    if (heapdump_done || heapdump_path.empty() || scan_best.empty()) {
        return;
    }
    // Wait for the scan to name a target. A snapshot without one is unusable: the address a scan
    // finds dies with the boot, so the two have to come from the same run.
    LOG_INFO(Core, "DSMod: snapshotting for target {}", scan_best);
    heapdump_done = true;
    auto& memory = system.ApplicationMemory();
    FILE* const out = std::fopen(heapdump_path.c_str(), "wb");
    if (out == nullptr) {
        LOG_ERROR(Core, "DSMod: cannot write heap snapshot to {}", heapdump_path);
        return;
    }
    u64 written = 0;
    const auto dump_range = [&](VAddr from, VAddr to) {
        VAddr run_start = 0;
        u64 run_len = 0;
        const auto flush = [&] {
            if (run_len == 0) {
                return;
            }
            const u32 len32 = static_cast<u32>(run_len);
            std::fwrite(&run_start, sizeof(run_start), 1, out);
            std::fwrite(&len32, sizeof(len32), 1, out);
            for (u64 off = 0; off < run_len; off += 0x1000) {
                std::fwrite(memory.GetPointerSilent(run_start + off), 1, 0x1000, out);
            }
            written += run_len;
            run_len = 0;
        };
        for (VAddr page = from & ~0xFFFULL; page < to; page += 0x1000) {
            if (memory.GetPointerSilent(page) == nullptr || run_len >= (64ULL << 20)) {
                flush();
                continue;
            }
            if (run_len == 0) {
                run_start = page;
            } else if (run_start + run_len != page) {
                flush();
                run_start = page;
            }
            run_len += 0x1000;
        }
        flush();
    };
    dump_range(main_region_begin, main_region_begin + main_region_size);
    dump_range(HeapLow(), HeapHigh());
    std::fclose(out);
    LOG_INFO(Core, "DSMod: heap snapshot, {} MB -> {}", written >> 20, heapdump_path);
}

/// Work out how a package could reach this address on a later boot.
///
/// An address found by watching memory belongs to one run and is worthless in the next. What
/// survives is the route: some pointer in the module's own data leads, through a hop or two, to
/// the object. That is the form every working mod of this kind uses -- Eden-DS reaches Mario
/// Kart's race state from a single static at main+0x12F6388, and Breath of the Wild's player from
/// main+0x1D66A78 -- and it is what a package can express as an ordinary pointer chain.
///
/// So search backwards: who points at this address, and who points at them, until the trail
/// reaches the module. Scanning the module at every level matters -- an earlier version only
/// checked it for a direct pointer, which finds nothing whenever the game keeps its objects one
/// container away from the static, as most engines do.
void ModRuntime::TraceChainToImpl([[maybe_unused]] VAddr target) {
    auto& memory = system.ApplicationMemory();
    constexpr u64 ModuleImageMax = 0x4000000;
    const VAddr module_end = main_region_begin + std::min<u64>(main_region_size, ModuleImageMax);

    // Start from the object the value sits in, so the chain ends at a field offset rather than
    // partway through a structure.
    VAddr object = target;
    s64 field = 0;
    for (VAddr back = target & ~7ULL; back + ObjectWalkBack > target && back > 0x1000; back -= 8) {
        if (!AddressIsSane(back, 8)) {
            break;
        }
        const u64 word = memory.Read64(back);
        if (word >= main_region_begin && word < module_end) {
            object = back;
            field = static_cast<s64>(target - back);
            LOG_INFO(Core, "DSMod chain: object {:016X} (vtable main+{:X}), field +{:#x}", object,
                     word - main_region_begin, field);
            break;
        }
    }

    const auto holders_of = [&](const std::vector<VAddr>& wanted, bool in_module) {
        std::vector<std::pair<VAddr, VAddr>> found; // holder, which target it held
        const VAddr from = in_module ? main_region_begin : HeapLow();
        const VAddr to = in_module ? module_end : HeapHigh();
        for (VAddr page = from & ~0xFFFULL; page < to && found.size() < 8; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
                u64 word{};
                std::memcpy(&word, host + at, sizeof(word));
                for (const VAddr candidate : wanted) {
                    if (word == candidate) {
                        found.emplace_back(page + at, candidate);
                        break;
                    }
                }
                if (found.size() >= 8) {
                    break;
                }
            }
        }
        return found;
    };

    std::vector<VAddr> level{object, target};
    std::vector<s64> trail{field};
    for (int depth = 0; depth < 3 && !level.empty(); ++depth) {
        for (const auto& [holder, held] : holders_of(level, true)) {
            LOG_INFO(Core,
                     "DSMod chain: STATIC main+{:X} reaches the value in {} hop(s), field +{:#x}",
                     holder - main_region_begin, depth + 1, field);
        }
        std::vector<VAddr> next;
        for (const auto& [holder, held] : holders_of(level, false)) {
            LOG_INFO(Core, "DSMod chain: {:016X} holds {:016X} (depth {})", holder, held,
                     depth + 1);
            next.push_back(holder);
        }
        if (next.empty()) {
            LOG_INFO(Core, "DSMod chain: nothing points at depth {}", depth + 1);
            break;
        }
        level = next;
    }
}

void ModRuntime::RecheckRoutesImpl() {
    if (path_routes.empty() || (tick_count % 120) != 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    ++path_checks;
    std::string report;
    for (auto& route : path_routes) {
        // Replay the route exactly as it was found. Assuming a fixed number of hops scored
        // every deep route as broken, which said more about the check than the route.
        bool ok = false;
        f32 x{};
        VAddr at = main_region_begin + static_cast<VAddr>(route.root);
        for (size_t i = 0; i <= route.hops.size(); ++i) {
            if (!AddressIsSane(at, 8)) {
                at = 0;
                break;
            }
            at = static_cast<VAddr>(memory.Read64(at));
            if (i < route.hops.size()) {
                at += static_cast<VAddr>(route.hops[i]);
            }
        }
        if (at != 0) {
            const VAddr value_at = at + static_cast<VAddr>(route.delta);
            if (AddressIsSane(value_at, 4)) {
                const u32 raw = memory.Read32(value_at);
                std::memcpy(&x, &raw, sizeof(x));
                // A coordinate here is a few thousand units and never vanishingly small.
                ok = std::isfinite(x) && std::fabs(x) > 1.0f && std::fabs(x) < 100000.0f;
            }
        }
        if (ok) {
            ++route.good;
        }
        report += fmt::format(" main+{:X}[{}]:{}/{}", route.root, route.hops.size(), route.good,
                              path_checks);
    }
    LOG_INFO(Core, "DSMod path: routes still resolving after {} check(s):{}", path_checks, report);
}

void ModRuntime::PathFromStaticsImpl([[maybe_unused]] VAddr target, [[maybe_unused]] int depth,
                                     [[maybe_unused]] s64 slack) const {
    auto& memory = system.ApplicationMemory();
    constexpr u64 ModuleImageMax = 0x4000000;
    const VAddr module_end = main_region_begin + std::min<u64>(main_region_size, ModuleImageMax);
    const VAddr low = target > static_cast<VAddr>(slack) ? target - static_cast<VAddr>(slack) : 0;

    // Every module word that looks like a heap pointer is a possible root. There are far fewer
    // of these than there are holders of an arbitrary heap object, which is what makes going
    // forwards the cheaper direction.
    struct Step {
        VAddr at;
        s64 root;
        std::vector<s64> hops; ///< the offsets walked to get here, so a route can be replayed
    };
    std::vector<Step> frontier;
    for (VAddr at = main_region_begin; at + 8 <= module_end; at += 8) {
        if (!AddressIsSane(at, 8)) {
            continue;
        }
        const u64 word = memory.Read64(at);
        if (word >= HeapLow() && word < HeapHigh()) {
            frontier.push_back(
                {static_cast<VAddr>(word), static_cast<s64>(at - main_region_begin), {}});
        }
    }
    LOG_INFO(Core, "DSMod path: {} module static(s) point into the heap", frontier.size());

    std::set<VAddr> visited;
    for (int level = 1; level <= depth && !frontier.empty(); ++level) {
        std::vector<Step> next;
        for (const auto& step : frontier) {
            // Does this object contain the target, or a pointer near it?
            for (s64 off = 0; off < 0x200; off += 8) {
                const VAddr at = step.at + static_cast<VAddr>(off);
                if (!AddressIsSane(at, 8)) {
                    continue;
                }
                const u64 word = memory.Read64(at);
                if (word >= low && word <= target) {
                    // Reaching the target once is not a binding. A slot the game reuses will
                    // happen to point the right way for an instant; what matters is whether the
                    // same walk still lands there seconds later, so remember the route and
                    // re-check it rather than reporting the first hit.
                    auto hops = step.hops;
                    hops.push_back(off);
                    path_routes.push_back({step.root, std::move(hops),
                                           static_cast<s64>(target) - static_cast<s64>(word)});
                    LOG_INFO(Core,
                             "DSMod path: candidate main+{:X} +{:#x} (target{:+#x}) "
                             "at depth {}",
                             step.root, off, static_cast<s64>(target) - static_cast<s64>(word),
                             level);
                    if (path_routes.size() >= 24) {
                        return;
                    }
                    continue;
                }
                if (next.size() < 20000 && word >= HeapLow() && word < HeapHigh() &&
                    visited.insert(static_cast<VAddr>(word)).second) {
                    auto hops = step.hops;
                    hops.push_back(off);
                    next.push_back({static_cast<VAddr>(word), step.root, std::move(hops)});
                }
            }
        }
        LOG_INFO(Core, "DSMod path: depth {} -- {} object(s) reachable", level, next.size());
        frontier = std::move(next);
    }
    LOG_WARNING(Core, "DSMod path: no static reaches {:016X} within {} level(s)", target, depth);
}

void ModRuntime::TraceToStaticImpl([[maybe_unused]] VAddr target, [[maybe_unused]] int depth,
                                   [[maybe_unused]] s64 slack) const {
    auto& memory = system.ApplicationMemory();
    constexpr u64 ModuleImageMax = 0x4000000;
    const VAddr module_end = main_region_begin + std::min<u64>(main_region_size, ModuleImageMax);

    std::set<VAddr> frontier{target};
    std::set<VAddr> seen{target};
    for (int level = 1; level <= depth; ++level) {
        // Anything a holder might name: the address itself or a little before it, since a
        // component is reached through the object that contains it.
        std::set<VAddr> wanted;
        for (const VAddr t : frontier) {
            for (s64 back = 0; back <= slack; back += 8) {
                wanted.insert(t - static_cast<VAddr>(back));
            }
        }
        std::set<VAddr> next;
        int module_hits = 0;
        // The module first: a hit there ends the search, because that is the root we want.
        for (VAddr at = main_region_begin; at + 8 <= module_end && module_hits < 8; at += 8) {
            if (!AddressIsSane(at, 8)) {
                continue;
            }
            const u64 word = memory.Read64(at);
            if (wanted.contains(static_cast<VAddr>(word))) {
                LOG_INFO(Core, "DSMod static: main+{:X} holds {:016X} at depth {}",
                         at - main_region_begin, word, level);
                ++module_hits;
            }
        }
        if (module_hits > 0) {
            LOG_INFO(Core, "DSMod static: reached the module at depth {}", level);
            return;
        }
        if (level == depth) {
            break;
        }
        for (VAddr page = HeapLow(); page < HeapHigh() && next.size() < 64; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            for (u32 at = 0; at + 8 <= 0x1000 && next.size() < 64; at += 8) {
                u64 word{};
                std::memcpy(&word, host + at, sizeof(word));
                if (wanted.contains(static_cast<VAddr>(word)) && !seen.contains(page + at)) {
                    next.insert(page + at);
                    seen.insert(page + at);
                }
            }
        }
        LOG_INFO(Core, "DSMod static: depth {} -- {} holder(s) in the heap, none in the module",
                 level, next.size());
        if (next.empty()) {
            return;
        }
        frontier = std::move(next);
    }
    LOG_WARNING(Core, "DSMod static: no module route within {} level(s)", depth);
}

void ModRuntime::TraceWithSlackImpl([[maybe_unused]] VAddr object,
                                    [[maybe_unused]] s64 slack) const {
    auto& memory = system.ApplicationMemory();
    constexpr u64 ModuleImageMax = 0x4000000;
    const VAddr module_end = main_region_begin + std::min<u64>(main_region_size, ModuleImageMax);

    // Every address the holder might legitimately be pointing at: the object itself, or any
    // aligned address up to `slack` bytes before it, since a component usually sits inside the
    // actor that owns it and the pointer names the actor.
    std::set<VAddr> wanted;
    for (s64 back = 0; back <= slack; back += 8) {
        wanted.insert(object - static_cast<VAddr>(back));
    }
    int found = 0;
    for (VAddr page = HeapLow(); page < HeapHigh() && found < 12; page += 0x1000) {
        const u8* const host = memory.GetPointerSilent(page);
        if (host == nullptr) {
            continue;
        }
        for (u32 at = 0; at + 8 <= 0x1000 && found < 12; at += 8) {
            u64 word{};
            std::memcpy(&word, host + at, sizeof(word));
            if (!wanted.contains(static_cast<VAddr>(word))) {
                continue;
            }
            const VAddr holder = page + at;
            // What class is the holder? That is the thing a package could name.
            std::string owner = "unknown";
            for (VAddr back = holder & ~7ULL; back + 0x200 > holder && back > 0x1000; back -= 8) {
                if (!AddressIsSane(back, 8)) {
                    break;
                }
                const u64 vt = memory.Read64(back);
                if (vt >= main_region_begin && vt < module_end) {
                    owner = fmt::format("class main+{:X} field +{:#x}", vt - main_region_begin,
                                        holder - back);
                    break;
                }
            }
            LOG_INFO(Core, "DSMod slack: {:016X} holds {:016X} (object{:+#x}) -- holder is {}",
                     holder, word, static_cast<s64>(word) - static_cast<s64>(object), owner);
            ++found;
        }
    }
    LOG_INFO(Core, "DSMod slack: {} holder(s) within {:#x} of {:016X}", found, slack, object);
}

void ModRuntime::MotionScanImpl() {
    if (motion_spec.empty() || !InGameplay() || (tick_count % 5) != 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    constexpr u64 LegTicks = 240; // ~4 s of holding one direction

    if (motion_phase == 0) {
        // "<published name>@<window>" restricts the search to memory around an object the game
        // handed us. The minimap has to know where the player is in order to draw the dot, and
        // its manager is a singleton -- so a coordinate found inside it is reachable from a
        // static, which a coordinate inside one actor among thirty thousand is not.
        VAddr from = HeapLow(), to = HeapHigh();
        // "1" and "free" both mean the whole heap. "free" only ever described the input side
        // (a person drives, not the runtime); falling into the anchor branch made it a name to
        // look up, which never resolved, so phase 0 returned every tick and nothing was watched.
        if (motion_spec != "1" && motion_spec != "free") {
            std::string anchor = motion_spec;
            u64 window = 0x10000;
            if (const auto at = anchor.rfind('@'); at != std::string::npos) {
                window = std::strtoull(anchor.c_str() + at + 1, nullptr, 0);
                anchor = anchor.substr(0, at);
            }
            const auto found = sequence_addresses.find(anchor);
            if (found == sequence_addresses.end() || found->second == 0) {
                return; // wait until the game hands us the object
            }
            const VAddr at_addr = static_cast<VAddr>(found->second);
            from = at_addr > window ? at_addr - window : 0;
            to = at_addr + window;
            LOG_INFO(Core, "DSMod motion: searching {:016X}..{:016X} around {}", from, to, anchor);
        }
        // Collect a slice at a time. Sweeping the whole heap in one call runs on the emulator's
        // own thread and freezes the game for seconds -- unnoticeable when a script is playing,
        // indistinguishable from a hang when a person is.
        constexpr u64 PagesPerTick = 4096;
        if (motion_sweep_at == 0) {
            motion_sweep_at = from & ~0xFFFULL;
        }
        const VAddr sweep_end = std::min<VAddr>(to, motion_sweep_at + PagesPerTick * 0x1000);
        for (VAddr page = motion_sweep_at; page < sweep_end; page += 0x1000) {
            if (motion_at.size() >= (4u << 20)) {
                break;
            }
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            // Only floats that have a coordinate-shaped neighbour. A position is a vector, so
            // its components sit side by side; a lone float that happens to look like a
            // coordinate is almost always something else. This is what keeps the set inside its
            // cap -- collecting every plausible float filled two million slots before the sweep
            // had crossed the heap, so whole regions were never looked at.
            const auto coordish = [](f32 v) {
                return std::isfinite(v) && std::fabs(v) > 1.0f && std::fabs(v) < 100000.0f &&
                       v != std::floor(v);
            };
            for (u32 at = 0; at + 8 <= 0x1000; at += 4) {
                f32 v{}, next{};
                std::memcpy(&v, host + at, sizeof(v));
                std::memcpy(&next, host + at + 4, sizeof(next));
                if (!coordish(v) || !coordish(next)) {
                    continue;
                }
                motion_at.push_back(page + at);
                motion_last.push_back(v);
                motion_score.push_back(0);
                motion_dir.push_back(0);
                motion_turns.push_back(0);
            }
        }
        motion_sweep_at = sweep_end;
        if (motion_sweep_at < to && motion_at.size() < (4u << 20)) {
            return; // more to sweep next tick
        }
        LOG_INFO(Core, "DSMod motion: watching {} float(s) that could be a coordinate",
                 motion_at.size());
        motion_phase = 1;
        motion_started = tick_count;
        return;
    }

    // Hold one direction for a few seconds, then the other. Walking back and forth is what makes
    // a position identify itself: it must follow the stick both ways, and a value that only ever
    // rises is a timer.
    // With a person at the controls, do not touch the stick and do not assume a direction.
    // What identifies a coordinate then is simply that it keeps taking new values: walking a
    // real route through rooms moves it through hundreds, where a counter or a flag has a
    // handful and an idle field has one.
    const bool driven = motion_spec != "free";
    const bool going_right = ((tick_count - motion_started) / LegTicks) % 2 == 0;
    if (driven) {
        if (auto* pad = system.GetInputSubsystem() ? system.GetInputSubsystem()->GetVirtualGamepad()
                                                   : nullptr) {
            for (const std::size_t player : {std::size_t{0}, std::size_t{8}}) {
                pad->SetStickPosition(player, InputCommon::VirtualGamepad::VirtualStick::Left,
                                      going_right ? 1.0f : -1.0f, 0.0f);
            }
        }
    }

    // A slice per tick, not the whole set. Sampling two million addresses with a page-table
    // lookup each took long enough to block presentation outright -- audio kept playing while
    // the picture never updated, which is indistinguishable from a hang. Candidates are stored
    // in ascending address order, so caching one host page pointer covers a long run of them.
    constexpr size_t SlicePerTick = 1u << 18;
    const u8* page_host = nullptr;
    VAddr page_base = 1; // never a real page base
    for (size_t step = 0; step < SlicePerTick && !motion_at.empty(); ++step) {
        if (motion_cursor >= motion_at.size()) {
            motion_cursor = 0;
        }
        const size_t i = motion_cursor++;
        const VAddr addr = motion_at[i];
        if (const VAddr base = addr & ~0xFFFULL; base != page_base) {
            page_base = base;
            page_host = memory.GetPointerSilent(base);
        }
        if (page_host == nullptr) {
            continue;
        }
        f32 now{};
        std::memcpy(&now, page_host + (addr & 0xFFF), sizeof(now));
        if (!std::isfinite(now)) {
            continue;
        }
        const f32 moved = now - motion_last[i];
        motion_last[i] = now;
        if (std::fabs(moved) < 0.01f) {
            continue; // standing still proves nothing either way
        }
        if (!driven) {
            // Free play: what matters is not how much a value moves but whether it ever turns
            // around. Counting movement alone ranks timers first -- they change every single
            // sample and never stop -- which is exactly what the first run of this produced.
            // A coordinate reverses every time the player does; a clock never reverses once.
            if (std::fabs(moved) >= 200.0f) {
                motion_score[i] -= 2; // a teleport, a camera cut, a respawn
                continue;
            }
            const s8 dir = moved > 0.0f ? s8{1} : s8{-1};
            if (motion_dir[i] != 0 && dir != motion_dir[i]) {
                ++motion_turns[i];
                motion_score[i] += 4; // a turn is worth far more than another step
            } else {
                motion_score[i] += 1;
            }
            motion_dir[i] = dir;
            continue;
        }
        // Agreeing with the stick earns a point; disagreeing loses one. Nothing is discarded --
        // a coordinate stops for a wall or a ledge, and a single stall must not be fatal.
        motion_score[i] += ((moved > 0.0f) == going_right) ? 1 : -1;
    }

    // Ranking sorts the whole candidate set, so do it on a timer rather than every sample.
    if (tick_count - motion_started < LegTicks * 4 || (tick_count - motion_started) % 300 != 0) {
        return;
    }
    std::vector<size_t> order(motion_at.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    // Rank deeply enough to look for structure, not just to print a top ten.
    const size_t top = std::min<size_t>(256, order.size());
    std::ranges::partial_sort(
        order.begin(), order.begin() + top, order.end(),
        [&](size_t a, size_t b) { return motion_score[a] > motion_score[b]; });
    std::string report;
    for (size_t k = 0; k < order.size() && k < 10; ++k) {
        const size_t i = order[k];
        if (motion_score[i] <= 0) {
            break;
        }
        report += fmt::format(" {:012X}:{:+d}/{}turns@{:g}", motion_at[i], motion_score[i],
                              motion_turns[i], motion_last[i]);
    }
    LOG_INFO(Core, "DSMod motion: best agreement with the stick:{}",
             report.empty() ? " (nothing yet)" : report);

    // Everything that follows the player reverses when the player does, so a turn count alone
    // cannot separate a position from a camera offset or an animation blend. What can: a
    // position is a vector, so its components sit four bytes apart in one object. A scalar that
    // merely tracks the player has no high-scoring neighbour.
    std::map<VAddr, size_t> top_at;
    for (size_t k = 0; k < top; ++k) {
        top_at[motion_at[order[k]]] = order[k];
    }
    std::string vecs;
    int shown = 0;
    // The pair worth tracing, as opposed to the loudest scalar. Velocities and blend factors
    // out-score a position easily -- they change every frame -- but they are lone floats near
    // zero. A world coordinate comes with a neighbour and is thousands of units from the origin.
    VAddr vec_best = 0;
    s32 vec_best_score = -1;
    for (const auto& [addr, idx] : top_at) {
        if (top_at.contains(addr - 4)) {
            continue; // report the start of a run, not its middle
        }
        const auto ny = top_at.find(addr + 4);
        if (ny == top_at.end()) {
            continue;
        }
        if (std::fabs(motion_last[idx]) > 1000.0f && motion_score[idx] > vec_best_score) {
            vec_best_score = motion_score[idx];
            vec_best = addr;
        }
        const auto nz = top_at.find(addr + 8);
        vecs += fmt::format(
            " {:012X}=({:g},{:g}{})", addr, motion_last[idx], motion_last[ny->second],
            nz == top_at.end() ? std::string{} : fmt::format(",{:g}", motion_last[nz->second]));
        if (++shown >= 10) {
            break;
        }
    }
    LOG_INFO(Core, "DSMod motion: adjacent movers (possible x/y/z):{}",
             vecs.empty() ? " (none)" : vecs);

    // Once one candidate is clearly ahead, show where it lives and what sits beside it. A map
    // needs two coordinates, and the other one is almost always the next float along -- so the
    // neighbourhood is the answer to the second half of the question.
    if (!order.empty() && !motion_traced &&
        (vec_best != 0 ? vec_best_score >= 30 : motion_score[order[0]] >= 30)) {
        motion_traced = true;
        const VAddr best = vec_best != 0 ? vec_best : motion_at[order[0]];
        std::string around;
        for (s64 off = -0x20; off <= 0x20; off += 4) {
            const VAddr at_addr = best + static_cast<VAddr>(off);
            if (!AddressIsSane(at_addr, 4)) {
                continue;
            }
            const u32 raw = memory.Read32(at_addr);
            f32 v{};
            std::memcpy(&v, &raw, sizeof(v));
            around += std::isfinite(v) ? fmt::format(" {:+#x}:{:g}", off, v)
                                       : fmt::format(" {:+#x}:?", off);
        }
        LOG_INFO(Core, "DSMod motion: winner {:016X} score {}, around:{}", best,
                 motion_score[order[0]], around);
        // Wider view: an identifying field is more likely a little further out than right
        // beside the coordinates.
        // Name module pointers relative to the module, in the run that read them. An absolute
        // vtable is meaningless in the next run, and converting one with another run's base
        // produces an offset that matches nothing.
        constexpr u64 ModuleImageMax = 0x4000000;
        const VAddr module_end =
            main_region_begin + std::min<u64>(main_region_size, ModuleImageMax);
        std::string wide;
        for (s64 off = -0x80; off <= 0x80; off += 8) {
            const VAddr at_addr = best + static_cast<VAddr>(off);
            if (!AddressIsSane(at_addr, 8)) {
                continue;
            }
            const u64 word = memory.Read64(at_addr);
            wide += (word >= main_region_begin && word < module_end)
                        ? fmt::format(" {:+#x}:main+{:X}", off, word - main_region_begin)
                        : fmt::format(" {:+#x}:{:016X}", off, word);
        }
        LOG_INFO(Core, "DSMod motion: wide{}", wide);
        TraceChainTo(best);
        TraceWithSlack(best, 0x400);
        TraceToStatic(best, 3, 0x400);
        PathFromStatics(best, 4, 0x400);
    }
    RecheckRoutes();
    if (false) {
    }
    motion_started = tick_count;
}

void ModRuntime::ArrayDumpImpl() {
    if (arraydump_spec.empty() || arraydump_done || !InGameplay()) {
        return;
    }
    arraydump_done = true;
    auto& memory = system.ApplicationMemory();
    constexpr s64 Stride = 0x18;
    constexpr int MinRun = 8;
    const s64 vtable_offset = std::strtoll(
        arraydump_spec.c_str() + (arraydump_spec.starts_with("main+") ? 5 : 0), nullptr, 0);
    const u64 vtable = main_region_begin + static_cast<u64>(vtable_offset);
    LOG_INFO(Core, "DSMod array: runs of >= {} entries at stride {:#x}, vtable main+{:X}", MinRun,
             Stride, vtable_offset);

    int runs = 0;
    for (VAddr page = HeapLow(); page < HeapHigh() && runs < 6; page += 0x1000) {
        if (memory.GetPointerSilent(page) == nullptr) {
            continue;
        }
        for (u32 at = 0; at + 8 <= 0x1000 && runs < 6; at += 8) {
            const VAddr start = page + at;
            if (!AddressIsSane(start, 8) || memory.Read64(start) != vtable) {
                continue;
            }
            // Only report the first entry of a run, so one array is not reported once per item.
            if (AddressIsSane(start - Stride, 8) && memory.Read64(start - Stride) == vtable) {
                continue;
            }
            int length = 0;
            while (AddressIsSane(start + length * Stride, 8) &&
                   memory.Read64(start + length * Stride) == vtable && length < 128) {
                ++length;
            }
            if (length < MinRun) {
                continue;
            }
            std::string values;
            for (int i = 0; i < length && i < 48; ++i) {
                const VAddr slot = start + i * Stride + 0x10;
                if (!AddressIsSane(slot, 4)) {
                    continue;
                }
                const u32 raw = memory.Read32(slot);
                f32 value{};
                std::memcpy(&value, &raw, sizeof(value));
                values += std::isfinite(value) ? fmt::format(" [{}]{:g}", i, value)
                                               : fmt::format(" [{}]?", i);
            }
            LOG_INFO(Core, "DSMod array: {:016X} x{}:{}", start, length, values);
            // What surrounds the run matters as much as the run. Missile capacity is not one of
            // these entries, and a heap-wide search for it fills its result cap from the bottom
            // of the address space long before reaching here -- so read the neighbourhood
            // directly instead of hoping a global scan reaches it.
            if (runs == 0) {
                for (s64 base = -0x300; base < 0x300; base += 0x60) {
                    std::string around;
                    for (s64 off = base; off < base + 0x60; off += 4) {
                        const VAddr at_addr = start + static_cast<VAddr>(off);
                        if (!AddressIsSane(at_addr, 4)) {
                            continue;
                        }
                        const u32 raw = memory.Read32(at_addr);
                        f32 value{};
                        std::memcpy(&value, &raw, sizeof(value));
                        if (std::isfinite(value) && value != 0.0f && std::fabs(value) >= 0.01f &&
                            std::fabs(value) < 1.0e7f) {
                            around += fmt::format(" {:+#x}:{:g}", off, value);
                        } else if (raw != 0 && raw < 0x10000) {
                            around += fmt::format(" {:+#x}:i{}", off, raw);
                        }
                    }
                    if (!around.empty()) {
                        LOG_INFO(Core, "DSMod array:  near{}", around);
                    }
                }
            }
            ++runs;
            at += static_cast<u32>(length * Stride) & 0xFF8;
        }
    }
    LOG_INFO(Core, "DSMod array: {} run(s)", runs);
}

void ModRuntime::ClassDumpImpl() {
    if (classdump_spec.empty() || classdump_done || !InGameplay()) {
        return;
    }
    classdump_done = true;
    auto& memory = system.ApplicationMemory();
    // "main+0x...@15" narrows to instances holding exactly that amount. Missile capacity is not
    // in the array the other five came from, so it has to be looked for by the number itself.
    std::string spec = classdump_spec;
    f32 want_value = 0.0f;
    bool want_set = false;
    if (const auto at_sign = spec.find('@'); at_sign != std::string::npos) {
        want_value = std::strtof(spec.c_str() + at_sign + 1, nullptr);
        want_set = true;
        spec = spec.substr(0, at_sign);
    }
    const s64 vtable_offset =
        std::strtoll(spec.c_str() + (spec.starts_with("main+") ? 5 : 0), nullptr, 0);
    const u64 vtable = main_region_begin + static_cast<u64>(vtable_offset);
    LOG_INFO(Core, "DSMod classdump: objects whose vtable is main+{:X} ({:016X})", vtable_offset,
             vtable);

    int found = 0, total = 0;
    for (VAddr page = HeapLow(); page < HeapHigh(); page += 0x1000) {
        const u8* const host = memory.GetPointerSilent(page);
        if (host == nullptr) {
            continue;
        }
        for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
            u64 word{};
            std::memcpy(&word, host + at, sizeof(word));
            if (word != vtable) {
                continue;
            }
            const VAddr object = page + at;
            // Only instances that look like they hold an amount. This class is generic -- most
            // of its objects are list nodes full of pointers -- and dumping from the bottom of
            // the heap filled the report with those long before reaching the one that matters.
            if (!AddressIsSane(object + 0x10, 4)) {
                continue;
            }
            f32 held{};
            const u32 held_raw = memory.Read32(object + 0x10);
            std::memcpy(&held, &held_raw, sizeof(held));
            // Only insist on an amount-shaped field when a value was asked for. Counting how
            // many objects share a class needs every instance, not the ones that look like they
            // hold a number.
            if (want_set) {
                if (!std::isfinite(held) || held != want_value) {
                    continue;
                }
            }
            // Show the object as words and as 64-bit values: an item's key is a CRC-64, so it
            // only reads as itself when two words are taken together.
            std::string body;
            for (s32 off = 0; off <= 0x40; off += 4) {
                if (!AddressIsSane(object + off, 4)) {
                    continue;
                }
                const u32 raw = memory.Read32(object + off);
                f32 as_float{};
                std::memcpy(&as_float, &raw, sizeof(as_float));
                const bool floatish = std::isfinite(as_float) && as_float != 0.0f &&
                                      std::fabs(as_float) >= 0.01f && std::fabs(as_float) < 1.0e7f;
                body += floatish ? fmt::format(" +{:x}:{:08X}/{:g}", off, raw, as_float)
                                 : fmt::format(" +{:x}:{:08X}", off, raw);
            }
            std::string quads;
            for (s32 off = 0; off <= 0x38; off += 8) {
                if (AddressIsSane(object + off, 8)) {
                    quads += fmt::format(" +{:x}:{:016X}", off, memory.Read64(object + off));
                }
            }
            ++total;
            if (found < 16) {
                LOG_INFO(Core, "DSMod classdump: {:016X} holds {:g}{}", object, held, body);
                ++found;
            }
        }
    }
    LOG_INFO(Core, "DSMod classdump: {} object(s) of this class in the heap", total);
}

void ModRuntime::RangeWatchImpl() {
    if (range_spec.empty() || !InGameplay() || (tick_count % 30) != 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const auto comma = range_spec.find(':');
    const f32 low = std::strtof(range_spec.c_str(), nullptr);
    const f32 high =
        comma == std::string::npos ? low : std::strtof(range_spec.c_str() + comma + 1, nullptr);
    // An optional third field: the capacity the HUD shows, e.g. "2:16:15" for 15 missiles. A
    // counter's maximum is not a guess when the screen is displaying it.
    const auto second = range_spec.find(':', comma == std::string::npos ? 0 : comma + 1);
    const f32 range_ceiling =
        second == std::string::npos ? 0.0f : std::strtof(range_spec.c_str() + second + 1, nullptr);

    if (range_phase == 0) {
        // Every float that looks like it could be a counter: inside the range, and a whole
        // number. Ammunition is counted, never fractional, and that alone discards most of a
        // heap full of positions, timers and interpolation weights.
        // Bound the sweep. Asking for whole numbers from zero upwards collected two hundred and
        // forty three million words -- about five gigabytes of bookkeeping for a search meant to
        // find one counter -- because 0.0 and 1.0 are the commonest values in any heap. A
        // counter is identified as it descends, so it only has to be caught while it still holds
        // something distinctive.
        constexpr size_t MaxWatched = 4u << 20;
        for (VAddr page = HeapLow(); page < HeapHigh(); page += 0x1000) {
            if (range_at.size() >= MaxWatched) {
                LOG_WARNING(Core, "DSMod range: stopped at {} candidates -- narrow the range",
                            range_at.size());
                break;
            }
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            for (u32 at = 0; at + 4 <= 0x1000; at += 4) {
                f32 value{};
                std::memcpy(&value, host + at, sizeof(value));
                if (!std::isfinite(value) || value < low || value > high ||
                    value != std::floor(value)) {
                    continue;
                }
                if (range_pair_max > 0.0f) {
                    // Require the capacity beside it. Alone, "a whole number under sixteen"
                    // describes tens of thousands of words; paired with its own maximum it
                    // describes an ammunition counter.
                    if (at + 8 > 0x1000) {
                        continue;
                    }
                    f32 beside{};
                    std::memcpy(&beside, host + at + 4, sizeof(beside));
                    if (beside != range_pair_max) {
                        continue;
                    }
                }
                {
                    range_at.push_back(page + at);
                    range_min.push_back(value);
                    range_max.push_back(value);
                    range_seen.push_back(
                        value >= 0.0f && value < 32.0f ? (1u << static_cast<int>(value)) : 0u);
                }
            }
        }
        LOG_INFO(Core, "DSMod range: watching {} whole-numbered float(s) in [{:g}, {:g}]",
                 range_at.size(), low, high);
        range_phase = 1;
        range_started = tick_count;
        return;
    }

    // Fire, so the count has a reason to move. Holding R readies a missile and Y launches it;
    // together on one frame they only fire the beam.
    //
    // Walk as well. Standing still, this drained fifteen missiles to twelve and then stopped for
    // the rest of the run -- the opening platform holds Samus in a state where the shot does not
    // come out, and a search that identifies a counter by watching it move cannot see a value
    // that has stopped moving. The playthrough script that does drain the count reliably is the
    // one that walks between shots.
    if (auto* pad = system.GetInputSubsystem() ? system.GetInputSubsystem()->GetVirtualGamepad()
                                               : nullptr) {
        bool ok{};
        const auto r = ParseButton("R", ok);
        const auto y = ParseButton("Y", ok);
        const u64 beat = tick_count / 30;
        // Release the trigger periodically: a missile has to be readied again, not held forever.
        const bool ready = (beat % 4) != 3;
        const bool pulse = (beat % 2) == 0 && ready;
        pad->SetButtonState(0, r, ready);
        pad->SetButtonState(8, r, ready);
        pad->SetButtonState(0, y, pulse);
        pad->SetButtonState(8, y, pulse);
        const float lean = ((beat / 4) % 2) == 0 ? 1.0f : -1.0f;
        for (const std::size_t player : {std::size_t{0}, std::size_t{8}}) {
            pad->SetStickPosition(player, InputCommon::VirtualGamepad::VirtualStick::Left, lean,
                                  0.0f);
        }
    }

    for (size_t i = 0; i < range_at.size(); ++i) {
        if (!AddressIsSane(range_at[i], 4)) {
            continue;
        }
        const u32 raw = memory.Read32(range_at[i]);
        f32 value{};
        std::memcpy(&value, &raw, sizeof(value));
        if (!std::isfinite(value)) {
            continue;
        }
        range_min[i] = std::min(range_min[i], value);
        range_max[i] = std::max(range_max[i], value);
        if (value >= 0.0f && value < 32.0f && value == std::floor(value)) {
            range_seen[i] |= 1u << static_cast<int>(value);
        }
    }

    if (tick_count - range_started < 900) {
        return;
    }
    range_started = tick_count;
    // Report by how far each one travelled. A counter being spent has the largest honest swing;
    // anything that never moved says nothing, and is not evidence against itself either.
    std::vector<size_t> order(range_at.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    // Rank by how many steps it took, not how far it went.
    std::ranges::sort(order, [&](size_t a, size_t b) {
        return std::popcount(range_seen[a]) > std::popcount(range_seen[b]);
    });
    // Ranking by the widest swing was wrong: a missile count that goes fifteen to twelve moves
    // by three, and is buried under anything that happened to sweep zero to a hundred. What
    // identifies a counter is not how far it travelled but that it started at its maximum, only
    // ever fell, and is a whole number the whole way.
    std::string report;
    int shown = 0;
    for (const size_t i : order) {
        if (range_max[i] == range_min[i] || shown >= 16) {
            continue;
        }
        const u32 raw = AddressIsSane(range_at[i], 4) ? memory.Read32(range_at[i]) : 0;
        f32 current{};
        std::memcpy(&current, &raw, sizeof(current));
        if (!std::isfinite(current) || current != std::floor(current) || current < 0.0f) {
            continue;
        }
        // Spent, not merely discarded. Requiring only "it went down" selects every allocation
        // the game cleared, which is most of the heap: those all read N -> 0. A counter still in
        // use is strictly between empty and its maximum, and its maximum is the capacity the HUD
        // shows -- so say so, rather than ranking a field of zeroes.
        // An empty counter is still a counter. Excluding zero was meant to skip memory the game
        // had cleared, and it threw away the answer instead: the missiles drained all the way to
        // nought, which is exactly what being spent looks like. The capacity sitting beside it
        // already rules out freed memory, so when that is being required, zero is allowed.
        if (current < 0.0f || current >= range_max[i] || range_min[i] < 0.0f) {
            continue;
        }
        // The step count is what tells a counter from a discarded allocation, and it needs no
        // assumption about where the capacity is stored. Fifteen missiles fired one at a time
        // pass through fifteen values; memory the game cleared holds two, whatever it was and
        // then nought.
        if (std::popcount(range_seen[i]) < 4) {
            continue;
        }
        if (range_ceiling > 0.0f && range_max[i] != range_ceiling) {
            continue;
        }
        report += fmt::format(" {:012X}:{:g}->{:g}({} steps)", range_at[i], range_max[i], current,
                              std::popcount(range_seen[i]));
        ++shown;
    }
    LOG_INFO(Core, "DSMod range: spent counters among {}:{}", range_at.size(),
             report.empty() ? " (none yet)" : report);

    // Trace a short list straight away, in the run that found it. A heap address means nothing
    // once the process is gone -- what survives is the class it belongs to and the offset of the
    // field within it, and that is the only form a package can use.
    if (shown > 0 && shown <= 4) {
        for (const size_t i : order) {
            if (range_max[i] == range_min[i] || !AddressIsSane(range_at[i], 4)) {
                continue;
            }
            const u32 raw = memory.Read32(range_at[i]);
            f32 current{};
            std::memcpy(&current, &raw, sizeof(current));
            if (!std::isfinite(current) || current >= range_max[i] || current < 0.0f) {
                continue;
            }
            if (range_pair_max > 0.0f && range_max[i] != range_pair_max) {
                continue;
            }
            if (!range_traced.insert(range_at[i]).second) {
                continue; // already followed this one
            }
            LOG_INFO(Core, "DSMod range: tracing {:016X} ({:g} of {:g}, {} steps)", range_at[i],
                     current, range_max[i], std::popcount(range_seen[i]));
            // Show the object this value lives in, here, in the run that identified it. The
            // class turned out to be a generic float wrapper used all over the engine, so what
            // distinguishes the inventory's instance from a tuning parameter's has to be read
            // off the object itself rather than assumed from its type.
            for (s64 base = -0x40; base <= 0x40; base += 0x20) {
                std::string row;
                for (s64 off = base; off < base + 0x20; off += 8) {
                    const VAddr at_addr = range_at[i] + static_cast<VAddr>(off);
                    if (AddressIsSane(at_addr, 8)) {
                        row += fmt::format(" {:+#x}:{:016X}", off, memory.Read64(at_addr));
                    }
                }
                LOG_INFO(Core, "DSMod range:  around{}", row);
            }
            TraceChainTo(range_at[i]);
        }
    }
}

void ModRuntime::FieldProbeImpl() {
    if (field_spec.empty() || field_done || !InGameplay()) {
        return;
    }
    field_done = true;
    auto& memory = system.ApplicationMemory();

    const auto at_sign = field_spec.find('@');
    if (at_sign == std::string::npos) {
        LOG_ERROR(Core, "DSMod field: expected \"<value>@<offset>\"");
        return;
    }
    const f32 wanted = std::strtof(field_spec.substr(0, at_sign).c_str(), nullptr);
    const s64 offset = std::strtoll(field_spec.substr(at_sign + 1).c_str(), nullptr, 0);
    LOG_INFO(Core, "DSMod field: looking for {} at +{:#x} of a referenced object", wanted, offset);

    const VAddr HeapFrom = HeapLow(), HeapTo = HeapHigh();
    // Stage one: every float in the heap that holds the value.
    std::unordered_map<u64, VAddr> bases; // candidate object -> where the value sits
    for (VAddr page = HeapFrom; page < HeapTo && bases.size() < 40000; page += 0x1000) {
        const u8* const host = memory.GetPointerSilent(page);
        if (host == nullptr) {
            continue;
        }
        for (u32 at = 0; at + 4 <= 0x1000; at += 4) {
            f32 value{};
            std::memcpy(&value, host + at, sizeof(value));
            if (std::fabs(value - wanted) < 0.001f) {
                const VAddr hit = page + at;
                if (hit > static_cast<VAddr>(offset)) {
                    bases.emplace(hit - static_cast<VAddr>(offset), hit);
                }
            }
        }
    }
    LOG_INFO(Core, "DSMod field: {} candidate object(s) hold {} at +{:#x}", bases.size(), wanted,
             offset);

    // Stage two: one pass looking for anything that points at one of them. A number lying loose
    // in the heap is common; a number at a fixed offset inside an object someone holds is not.
    int reported = 0;
    for (VAddr page = HeapFrom; page < HeapTo && reported < 40; page += 0x1000) {
        const u8* const host = memory.GetPointerSilent(page);
        if (host == nullptr) {
            continue;
        }
        for (u32 at = 0; at + 8 <= 0x1000 && reported < 40; at += 8) {
            u64 word{};
            std::memcpy(&word, host + at, sizeof(word));
            const auto found = bases.find(word);
            if (found == bases.end()) {
                continue;
            }
            std::string around;
            for (s32 off = -16; off <= 48; off += 4) {
                const VAddr probe_at = found->second + off;
                if (!AddressIsSane(probe_at, 4)) {
                    continue;
                }
                const u32 raw = memory.Read32(probe_at);
                f32 as_float{};
                std::memcpy(&as_float, &raw, sizeof(as_float));
                around += std::isfinite(as_float) && as_float != 0.0f
                              ? fmt::format(" {:+d}:{:g}", off, as_float)
                              : fmt::format(" {:+d}:{}", off, static_cast<s32>(raw));
            }
            LOG_INFO(Core, "DSMod field: object {:016X} held at {:016X}, value at {:016X} --{}",
                     word, page + at, found->second, around);
            ++reported;
        }
    }
    LOG_INFO(Core, "DSMod field: {} referenced candidate(s)", reported);
}

void ModRuntime::HeapFindImpl() {
    if (heapfind_spec.empty() || heapfind_done || !InGameplay()) {
        return;
    }
    heapfind_done = true;
    auto& memory = system.ApplicationMemory();

    // "aabb..,ccdd.." -- one or more literal byte strings, hex, comma separated.
    std::vector<std::vector<u8>> wanted;
    for (size_t at = 0; at <= heapfind_spec.size();) {
        const auto comma = heapfind_spec.find(',', at);
        const auto piece = heapfind_spec.substr(at, comma - at);
        std::vector<u8> bytes;
        for (size_t i = 0; i + 1 < piece.size(); i += 2) {
            bytes.push_back(static_cast<u8>(std::strtoul(piece.substr(i, 2).c_str(), nullptr, 16)));
        }
        if (!bytes.empty()) {
            wanted.push_back(std::move(bytes));
        }
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }
    const bool aligned =
        std::ranges::all_of(wanted, [](const std::vector<u8>& p) { return (p.size() % 4) == 0; });
    LOG_INFO(Core, "DSMod heapfind: looking for {} pattern(s){}", wanted.size(),
             aligned ? ", on word boundaries" : "");

    int reported = 0;
    for (VAddr page = HeapLow(); page < HeapHigh() && reported < 60; page += 0x1000) {
        const u8* const host = memory.GetPointerSilent(page);
        if (host == nullptr) {
            continue;
        }
        // Step by four when every pattern is a whole number of words. A float field is aligned,
        // and a byte-by-byte walk matches the same bytes lying unaligned inside unrelated data --
        // which is all a search for 12.0f returned: sixty hits, not one of them on a boundary.
        const u32 step = aligned ? 4u : 1u;
        for (u32 at = 0; at < 0x1000 && reported < 60; at += step) {
            for (size_t w = 0; w < wanted.size(); ++w) {
                const auto& bytes = wanted[w];
                if (at + bytes.size() > 0x1000) {
                    continue;
                }
                if (std::memcmp(host + at, bytes.data(), bytes.size()) != 0) {
                    continue;
                }
                // Print the neighbourhood as words: whatever this key maps to is one of them,
                // and seeing them all is what says which offset holds the amount.
                std::string around;
                for (s32 off = -32; off <= 128; off += 4) {
                    const VAddr probe_at = page + at + off;
                    if (!AddressIsSane(probe_at, 4)) {
                        continue;
                    }
                    const u32 raw = memory.Read32(probe_at);
                    f32 as_float{};
                    std::memcpy(&as_float, &raw, sizeof(as_float));
                    // Dread's Lua is built with a float LUA_NUMBER, so an amount may well be
                    // stored as one. Show both readings rather than assume: a value of 99 and a
                    // value of 99.0f look nothing alike as words, and only one of them is there.
                    const bool floatish = std::isfinite(as_float) && as_float != 0.0f &&
                                          std::fabs(as_float) >= 0.01f &&
                                          std::fabs(as_float) < 1.0e7f;
                    if (floatish) {
                        around +=
                            fmt::format(" {:+d}:{}/{:g}", off, static_cast<s32>(raw), as_float);
                    } else {
                        around += fmt::format(" {:+d}:{}", off, static_cast<s32>(raw));
                    }
                }
                LOG_INFO(Core, "DSMod heapfind: pattern {} at {:016X} --{}", w, page + at, around);
                ++reported;
            }
        }
    }
    LOG_INFO(Core, "DSMod heapfind: {} hit(s)", reported);
}

void ModRuntime::DiffScanImpl() {
    // Phases are several seconds long, so there is no need to look every frame.
    if (diff_spec.empty() || (tick_count % 30) != 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    constexpr u64 FirePhaseTicks = 420; // ~7 s of firing
    constexpr u64 RestPhaseTicks = 240; // ~4 s of standing still

    const auto sample = [&](VAddr address, s32& out) {
        if (!AddressIsSane(address, 4)) {
            return false;
        }
        const u32 raw = memory.Read32(address);
        std::memcpy(&out, &raw, sizeof(out));
        return true;
    };

    if (diff_phase == 0) {
        if (const auto at = diff_spec.rfind('@'); at != std::string::npos) {
            diff_window = std::strtoull(diff_spec.substr(at + 1).c_str(), nullptr, 0);
            diff_anchor = diff_spec.substr(0, at);
        } else {
            diff_anchor = diff_spec;
        }
        // Do not touch the pad until the game is actually being played. The anchor object exists
        // long before that, and holding the trigger through the intro menus both navigates them
        // wrongly and takes the game down on the next A press -- a crash the null guards do not
        // cover, because it is not a path the game would ever have taken on its own.
        // A reported energy is the same signal the playthrough script waits on: it means a player
        // exists and the run has left the menus.
        if (!InGameplay()) {
            return;
        }
        const auto found = sequence_addresses.find(diff_anchor);
        if (found == sequence_addresses.end() || found->second == 0) {
            return; // wait for the game to hand us the object
        }
        const VAddr anchor_at = static_cast<VAddr>(found->second);
        if (!AddressIsSane(anchor_at, 4)) {
            return;
        }
        // Every aligned word in reach that could plausibly be a small counter. Being generous
        // here costs nothing: the fire/rest test removes coincidences far more sharply than any
        // guess about the range would.
        const VAddr from = anchor_at > diff_window ? anchor_at - diff_window : 0;
        const VAddr to = anchor_at + diff_window;
        for (VAddr at = from & ~3ULL; at < to; at += 4) {
            s32 value{};
            if (sample(at, value) && value >= 0 && value <= 9999) {
                diff_candidates.push_back(at);
                diff_last.push_back(value);
            }
        }
        LOG_INFO(Core, "DSMod diff: watching {} word(s) around {} ({:016X}) -- firing now",
                 diff_candidates.size(), diff_anchor, anchor_at);
        diff_phase = 1;
        diff_started = tick_count;
        return;
    }

    // Drive the trigger ourselves. Dread readies a missile while R is held and fires it on Y;
    // pressed on the same frame it only shoots the beam, which is why an earlier search watched
    // a count that never moved. Doing this from inside the runtime is what makes the phase
    // exactly knowable -- an external script and a sampler have no shared clock.
    if (diff_phase == 1) {
        auto* pad =
            system.GetInputSubsystem() ? system.GetInputSubsystem()->GetVirtualGamepad() : nullptr;
        if (pad != nullptr) {
            bool ok{};
            const auto r = ParseButton("R", ok);
            const auto y = ParseButton("Y", ok);
            const bool pulse = ((tick_count / 30) % 2) == 0;
            pad->SetButtonState(0, r, true);
            pad->SetButtonState(8, r, true);
            pad->SetButtonState(0, y, pulse);
            pad->SetButtonState(8, y, pulse);
        }
    }

    const u64 elapsed = tick_count - diff_started;
    if (diff_phase == 1 && elapsed < FirePhaseTicks) {
        return;
    }
    if (diff_phase == 2 && elapsed < RestPhaseTicks) {
        return;
    }

    std::vector<VAddr> kept;
    std::vector<s32> kept_last;
    const bool was_firing = diff_phase == 1;
    for (size_t i = 0; i < diff_candidates.size(); ++i) {
        s32 now{};
        if (!sample(diff_candidates[i], now)) {
            continue;
        }
        // Firing spends the counter; resting leaves it alone. A coincidence fails one or the
        // other within a cycle or two, and the two tests together are what no unrelated word
        // survives -- a value that drifts constantly fails the rest, and a constant fails the fire.
        const bool ok = was_firing ? now < diff_last[i] : now == diff_last[i];
        if (ok) {
            kept.push_back(diff_candidates[i]);
            kept_last.push_back(now);
        }
    }
    ++diff_cycles;
    LOG_INFO(Core, "DSMod diff: after {} ({}): {} of {} survived", was_firing ? "firing" : "rest",
             diff_cycles, kept.size(), diff_candidates.size());
    if (kept.empty()) {
        // Do not narrow to nothing: an empty set is almost always a missed phase (no ammo left to
        // spend, or a load screen) rather than proof that the counter is not here. Re-baseline
        // and keep going.
        for (size_t i = 0; i < diff_candidates.size(); ++i) {
            s32 now{};
            if (sample(diff_candidates[i], now)) {
                diff_last[i] = now;
            }
        }
        LOG_WARNING(Core, "DSMod diff: nothing survived that phase -- re-baselining, not dropping");
    } else {
        diff_candidates = std::move(kept);
        diff_last = std::move(kept_last);
    }

    if (diff_candidates.size() <= 40) {
        std::string report;
        for (size_t i = 0; i < diff_candidates.size(); ++i) {
            report += fmt::format(" {:012X}={}", diff_candidates[i], diff_last[i]);
        }
        LOG_INFO(Core, "DSMod diff: survivors:{}", report);
    }

    if (diff_phase == 1) {
        // Let go of the trigger before the rest phase, or the counter keeps draining.
        auto* pad =
            system.GetInputSubsystem() ? system.GetInputSubsystem()->GetVirtualGamepad() : nullptr;
        if (pad != nullptr) {
            bool ok{};
            for (const char* name : {"R", "Y"}) {
                const auto button = ParseButton(name, ok);
                pad->SetButtonState(0, button, false);
                pad->SetButtonState(8, button, false);
            }
        }
    }
    diff_phase = diff_phase == 1 ? 2 : 1;
    diff_started = tick_count;
}

namespace {
/// Does this word hold `want`, however the game chose to store it?
///
/// Dread keeps its inventory in floats -- its Lua is built with a float LUA_NUMBER, and the
/// published infinite-health cheat writes 0x42C60000, which is 99.0f rather than 99. A search
/// that only compares integers cannot see any of it, which is why several passes over the right
/// memory came back empty.
bool WordHolds(u32 raw, s64 want, bool float_only) {
    if (!float_only && static_cast<s32>(raw) == static_cast<s32>(want)) {
        return true;
    }
    f32 as_float{};
    std::memcpy(&as_float, &raw, sizeof(as_float));
    return std::isfinite(as_float) && std::fabs(as_float - static_cast<f32>(want)) < 0.001f;
}
} // namespace

/// Find where a number the player can see lives in memory.
///
/// A value read through the scripting bridge costs a guest call, so it can only be sampled when
/// the game happens to run script and only every few seconds. The same number read straight out
/// of memory costs nothing, updates the moment the game writes it, and needs no breakpoints --
/// which is what makes it work on a backend that runs guest code natively.
///
/// Finding it is a process of elimination: thousands of addresses happen to hold "12" at any
/// moment, but almost none of them stop holding it exactly when the player fires a missile.
void ModRuntime::FindValueClusterImpl() {
    if (find_spec.empty() || (tick_count % 60) != 0) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    if (find_round == 0) {
        // Which published reading to track. Take the value to collect from the game itself
        // rather than from the command line: by the time the search starts the player has been
        // playing for a while, and a number typed in beforehand is already stale -- collecting
        // "12" once the count is down to 8 gathers nothing but coincidences.
        // "<anchor>+<neighbour>+<neighbour>": the reading to locate, and the ones that must sit
        // beside it in the same structure.
        find_others.clear();
        // A trailing "@<bytes>" widens how far apart the fields may sit.
        // A leading "<anchor>/" restricts the search to memory near a published object address.
        if (const auto slash = find_spec.find('/'); slash != std::string::npos) {
            find_anchor = find_spec.substr(0, slash);
            find_spec = find_spec.substr(slash + 1);
        }
        if (const auto at_sign = find_spec.rfind('@'); at_sign != std::string::npos) {
            find_reach = std::strtoll(find_spec.substr(at_sign + 1).c_str(), nullptr, 0);
            find_spec = find_spec.substr(0, at_sign);
        }
        for (size_t at = 0; at <= find_spec.size();) {
            const auto plus = find_spec.find('+', at);
            const auto part = find_spec.substr(at, plus - at);
            if (at == 0) {
                find_track = part;
            } else {
                find_others.push_back(part);
            }
            if (plus == std::string::npos) {
                break;
            }
            at = plus + 1;
        }
        const auto truth = sequence_values.find(find_track);
        if (truth == sequence_values.end()) {
            return; // wait until the game has told us what the value is
        }
        find_value = static_cast<s32>(truth->second);
        // Wait for actual play -- but judge that by what the game reports, not by a stopwatch.
        // A fixed delay is wrong on both ends: it wastes a minute on a device that reaches
        // gameplay quickly, and on a slow desktop it can fire while the game is still a load
        // screen, where a hundred structures being filled in look exactly like a counter being
        // spent. A tracked value that reads non-zero means the player exists.
        if (find_value == 0) {
            return; // the game has not reported a player yet
        }
        // Search near an object the game handed us, when one is named. Sweeping the whole heap
        // for "8" returns a hundred thousand coincidences and no amount of filtering afterwards
        // is as effective as not collecting them: the inventory lives near the player, and the
        // scripting bridge already knows where the player is.
        VAddr from = HeapLow(), to = HeapHigh();
        if (!find_anchor.empty()) {
            u64 anchor = 0;
            if (find_anchor.starts_with("main+")) {
                // A static in the module: the one kind of address that means the same thing in
                // every run, and therefore the only kind worth building a package around. Read
                // the cell and follow it to the object it holds.
                const VAddr cell =
                    main_region_begin +
                    static_cast<VAddr>(std::strtoull(find_anchor.substr(5).c_str(), nullptr, 0));
                if (!AddressIsSane(cell, sizeof(u64))) {
                    return;
                }
                anchor = memory.Read64(cell);
                if (anchor != 0 && AddressIsSane(static_cast<VAddr>(anchor), sizeof(u64))) {
                    anchor = memory.Read64(static_cast<VAddr>(anchor));
                }
            } else if (const auto at = sequence_addresses.find(find_anchor);
                       at != sequence_addresses.end()) {
                anchor = at->second;
            }
            if (anchor == 0 || !AddressIsSane(static_cast<VAddr>(anchor), 4)) {
                return; // wait until the game has built whatever this points at
            }
            from = anchor > find_window ? anchor - find_window : 0;
            to = anchor + find_window;
            LOG_INFO(Core, "DSMod find: searching {:016X}..{:016X} around {} ({:016X})", from, to,
                     find_anchor, anchor);
        }
        for (VAddr page = from & ~0xFFFULL; page < to; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            for (u32 at = 0; at + 4 <= 0x1000; at += 4) {
                u32 raw{};
                std::memcpy(&raw, host + at, sizeof(raw));
                if (WordHolds(raw, find_value, find_float_only)) {
                    find_candidates.push_back(page + at);
                    find_host.push_back(host + at);
                }
            }
        }
        find_round = 1;
        find_started = tick_count;
        LOG_INFO(Core, "DSMod find: {} addresses hold {} = {} -- now change it in game",
                 find_candidates.size(), find_track, find_value);
        return;
    }
    if (find_round >= 2) {
        NarrowByAgreement();
        return;
    }
    if (find_candidates.empty() || tick_count - find_started < 180) {
        return;
    }
    // Identify the structure by its neighbours, not by waiting for the value to move.
    //
    // Making a game change a number on cue turns out to be the unreliable part -- firing missiles
    // needs a button combination the emulated pad does not reproduce -- and a value that never
    // changes discriminates nothing. But an inventory keeps its fields together: the address
    // holding the missile count has the maximum and the energy a few bytes away. Requiring all
    // three, at distinct offsets within one structure's reach, is a test almost no coincidence
    // passes, and it needs the player to do nothing at all.
    std::vector<std::pair<std::string, s32>> wanted;
    for (const auto& name : find_others) {
        const auto it = sequence_values.find(name);
        if (it == sequence_values.end()) {
            return; // wait until the game has reported every field
        }
        wanted.emplace_back(name, static_cast<s32>(it->second));
    }
    const s64 Reach = find_reach;
    std::vector<VAddr> still;
    for (const VAddr candidate : find_candidates) {
        std::set<s64> used;
        size_t matched = 0;
        for (const auto& [name, value] : wanted) {
            for (s64 off = -Reach; off <= Reach; off += 4) {
                if (off == 0 || used.contains(off) || !AddressIsSane(candidate + off, 4)) {
                    continue;
                }
                const u32 raw = memory.Read32(candidate + off);
                if (WordHolds(raw, value, find_float_only)) {
                    used.insert(off);
                    ++matched;
                    break;
                }
            }
        }
        if (matched == wanted.size()) {
            still.push_back(candidate);
        }
    }
    LOG_INFO(Core, "DSMod find: {} of {} hold {} with all of {} nearby", still.size(),
             find_candidates.size(), find_track, find_others.size());
    // Group what is left by class. Each survivor sits inside some object, and an object's first
    // word is a pointer to its vtable, which lives in the module and so names the class the same
    // way in every run. Coincidences are scattered across whatever happened to be in memory; the
    // real inventory is one class, and the field sits at the same offset within it every time.
    std::map<std::pair<s64, s64>, std::vector<VAddr>> by_class;
    for (const VAddr hit : still) {
        for (VAddr back = hit & ~7ULL; back + ObjectWalkBack > hit && back > 0x1000; back -= 8) {
            if (!AddressIsSane(back, 8)) {
                break;
            }
            const u64 word = memory.Read64(back);
            // The module image is tens of megabytes; main_region_size covers the heap as well, so
            // without this bound an ordinary heap pointer passes as a vtable and invents a class.
            constexpr u64 ModuleImageMax = 0x4000000;
            if (word >= main_region_begin && word < main_region_begin + ModuleImageMax) {
                by_class[{static_cast<s64>(word - main_region_begin), static_cast<s64>(hit - back)}]
                    .push_back(hit);
                break;
            }
        }
    }
    std::vector<std::pair<std::pair<s64, s64>, size_t>> ranked;
    for (const auto& [key, hits] : by_class) {
        ranked.emplace_back(key, hits.size());
    }
    std::ranges::sort(ranked, [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < ranked.size() && i < 6; ++i) {
        const auto& [key, count] = ranked[i];
        LOG_INFO(Core, "DSMod find:   class main+{:X} field +{:#x} -- {} object(s), first {:016X}",
                 key.first, key.second, count, by_class[key].front());
    }
    // Keep going. The neighbour test is a static filter -- it narrows by layout, and layout
    // cannot tell a real counter from something that merely sits next to similar numbers. What
    // settles it is time: keep only the addresses that still agree with the game once the player
    // spends the value. An earlier version stopped here and never used that at all.
    find_candidates = still;
    find_host.clear();
    for (const VAddr address : find_candidates) {
        find_host.push_back(memory.GetPointerSilent(address & ~0xFFFULL) + (address & 0xFFF));
    }
    find_round = 2;
    find_started = tick_count;
}

/// Report what each surviving candidate currently holds.
///
/// Comparing against the value the scripting bridge reports only works while that reading is
/// fresh, and it is not: the bridge updates when the game happens to run a line of script, so it
/// can sit frozen at a refill while the player has since spent half their ammo. The screen,
/// though, is never stale. So print the candidates and what each one says, and let the number the
/// player can actually see decide which is the real one.
void ModRuntime::NarrowByAgreementImpl() {
    // The one filter that actually discriminates: watch the tracked reading change, and keep only
    // the addresses that changed with it. Layout tests narrow by where a value sits; this narrows
    // by what it does, and an address that merely happens to hold the same number cannot fake it.
    //
    // Only act at the instant the reported value changes. The bridge is stale between updates --
    // it refreshes when the game happens to run script -- so comparing at an arbitrary moment
    // would drop the true address for being ahead of the report. At a transition the report has
    // just caught up, and by then the real counter is already holding the new value.
    if (const auto truth = sequence_values.find(find_track); truth != sequence_values.end()) {
        const auto now = static_cast<s32>(truth->second);
        if (!find_truth_seen) {
            find_last_truth = now;
            find_truth_seen = true;
        } else if (now != find_last_truth) {
            std::vector<VAddr> kept;
            std::vector<const u8*> kept_host;
            for (size_t i = 0; i < find_candidates.size(); ++i) {
                u32 raw{};
                std::memcpy(&raw, find_host[i], sizeof(raw));
                if (WordHolds(raw, now, find_float_only)) {
                    kept.push_back(find_candidates[i]);
                    kept_host.push_back(find_host[i]);
                }
            }
            ++find_transitions;
            LOG_INFO(Core, "DSMod find: {} -> {} (transition {}): {} of {} followed it",
                     find_last_truth, now, find_transitions, kept.size(), find_candidates.size());
            find_last_truth = now;
            if (kept.empty()) {
                // Everything was dropped, which means the sample missed the change rather than
                // that nothing tracks it. Say so instead of continuing with an empty set: a
                // silent zero here reads as "not found" and wastes the whole run.
                LOG_WARNING(Core,
                            "DSMod find: no candidate held {} at the transition -- keeping "
                            "the previous set; widen the poll or slow the change",
                            now);
            } else {
                find_candidates = std::move(kept);
                find_host = std::move(kept_host);
            }
        }
    }
    if (tick_count - find_started < 180) {
        return;
    }
    find_started = tick_count;
    std::string report;
    for (size_t i = 0; i < find_candidates.size() && i < 24; ++i) {
        s32 value{};
        std::memcpy(&value, find_host[i], sizeof(value));
        report += fmt::format(" {:012X}={}", find_candidates[i], value);
    }
    LOG_INFO(Core, "DSMod find: {} candidate(s) after {} transition(s):{}", find_candidates.size(),
             find_transitions, report);
}

/// Log the C functions the game registers into Lua.
///
/// Lua's registration is an array of { const char* name; lua_CFunction fn; }, so in memory the
/// table gives itself away: a pointer to a string followed by a pointer into executable code.
/// Finding it turns "the game exposes Game.GetItemAmount" into "here is the routine that answers
/// it" -- and that routine reads whichever static holds the inventory. That static is what a
/// package needs in order to read the value directly rather than ask for it through a bridge,
/// which is the difference between working only on the JIT and working everywhere.
void ModRuntime::DumpLuaRegistryImpl() {
    if (registry_done || !manifest.dump_registry || tick_count < 600) {
        return;
    }
    registry_done = true;
    auto& memory = system.ApplicationMemory();
    constexpr u64 ModuleImageMax = 0x4000000;
    const VAddr module_end = main_region_begin + std::min<u64>(main_region_size, ModuleImageMax);
    int reported = 0;
    for (VAddr at = main_region_begin; at + 16 < module_end && reported < 600; at += 8) {
        if (!AddressIsSane(at, 16)) {
            continue;
        }
        const u64 name_ptr = memory.Read64(at);
        const u64 fn = memory.Read64(at + 8);
        if (name_ptr < main_region_begin || name_ptr >= module_end) {
            continue;
        }
        if (fn < main_region_begin || fn >= module_end || (fn & 3) != 0) {
            continue;
        }
        // The name must read as an identifier, which is what separates a registration entry from
        // two unrelated pointers that happen to sit together.
        std::string name;
        for (u64 i = 0; i < 48 && AddressIsSane(name_ptr + i, 1); ++i) {
            const u8 ch = memory.Read8(name_ptr + i);
            if (ch == 0) {
                break;
            }
            if (!std::isalnum(ch) && ch != '_') {
                name.clear();
                break;
            }
            name.push_back(static_cast<char>(ch));
        }
        if (name.size() < 4 || std::isdigit(static_cast<unsigned char>(name.front()))) {
            continue;
        }
        LOG_INFO(Core, "DSMod registry: {} -> main+{:X} (entry main+{:X})", name,
                 fn - main_region_begin, at - main_region_begin);
        ++reported;
    }
    LOG_INFO(Core, "DSMod registry: {} entries reported", reported);
}

/// Show what the module keeps beside a named string.
///
/// A game that registers its scripting API through its own reflection system does not leave a
/// plain name/function table to find. What it does leave is a descriptor -- some structure that
/// mentions the name -- and the routine that answers the call is a pointer inside it. So find
/// every place in the module that points at the string, and print the words around each: the
/// layout becomes obvious from three or four examples, and the code pointer with it.
void ModRuntime::DescribeStringUsesImpl([[maybe_unused]] const std::string& text) {
    auto& memory = system.ApplicationMemory();
    constexpr u64 ModuleImageMax = 0x4000000;
    const VAddr module_end = main_region_begin + std::min<u64>(main_region_size, ModuleImageMax);

    // Locate the string itself, then every pointer to it.
    std::vector<VAddr> strings;
    for (VAddr at = main_region_begin; at + text.size() + 1 < module_end && strings.size() < 4;
         ++at) {
        if (!AddressIsSane(at, text.size() + 1)) {
            continue;
        }
        bool same = memory.Read8(at + text.size()) == 0;
        for (size_t i = 0; same && i < text.size(); ++i) {
            same = memory.Read8(at + i) == static_cast<u8>(text[i]);
        }
        if (same) {
            strings.push_back(at);
            LOG_INFO(Core, "DSMod uses: '{}' lives at main+{:X}", text, at - main_region_begin);
        }
    }
    int shown = 0;
    for (VAddr at = main_region_begin; at + 8 < module_end && shown < 6; at += 8) {
        if (!AddressIsSane(at, 8)) {
            continue;
        }
        const u64 word = memory.Read64(at);
        if (std::ranges::find(strings, static_cast<VAddr>(word)) == strings.end()) {
            continue;
        }
        std::string around;
        for (s64 off = -0x20; off <= 0x20; off += 8) {
            if (!AddressIsSane(at + off, 8)) {
                continue;
            }
            const u64 value = memory.Read64(at + off);
            const bool in_module = value >= main_region_begin && value < module_end;
            around += fmt::format(" [{:+d}]={}", off,
                                  in_module ? fmt::format("main+{:X}", value - main_region_begin)
                                            : fmt::format("{:X}", value));
        }
        LOG_INFO(Core, "DSMod uses: descriptor at main+{:X}:{}", at - main_region_begin, around);
        ++shown;
    }
}

/// The dev-tools-only tail of ApplyEnforceRules() (mod_state.cpp): fires the always-on RE
/// tools (Lua registry dump, string-use tracer, pointer-chain trace-on-demand, watched-memory
/// dump, moving-float scan, value-cluster find, heap snapshot) once a tick, gated the same way
/// every other function in this file is. Pure move, no logic change.
void ModRuntime::ApplyEnforceRulesDevToolsImpl(const StateSnapshot& snapshot) {
    DumpLuaRegistry();
    if (!manifest.describe_string.empty() && !describe_done && tick_count > 600) {
        describe_done = true;
        DescribeStringUses(manifest.describe_string);
    }
    if (!trace_done && !trace_target.empty() && InGameplay()) {
        // "name" or "name:a>b>c" -- the published address, optionally walked first. What a
        // scripting VM hands out is often a wrapper that nothing else points at, so the object
        // worth finding a route to is usually one dereference further in.
        const auto colon = trace_target.find(':');
        const auto name = trace_target.substr(0, colon);
        // A published address if a sequence produced one, otherwise a data point. Tracing a point
        // is what lets us ask "who refers to this string literal", which is how an item table
        // gives itself away: it holds the name, and the amount sits beside it.
        u64 seed = 0;
        if (const auto at = sequence_addresses.find(name);
            at != sequence_addresses.end() && at->second != 0) {
            seed = at->second;
        } else if (const auto point = manifest.points.find(name); point != manifest.points.end()) {
            VAddr resolved{};
            if (ResolvePoint(point->second, resolved)) {
                seed = resolved;
            }
        }
        if (seed != 0) {
            trace_done = true;
            VAddr address = static_cast<VAddr>(seed);
            if (colon != std::string::npos) {
                auto& memory = system.ApplicationMemory();
                const auto path = trace_target.substr(colon + 1);
                for (size_t start = 0; start <= path.size();) {
                    const auto arrow = path.find('>', start);
                    address += static_cast<VAddr>(
                        std::strtoll(path.substr(start, arrow - start).c_str(), nullptr, 0));
                    if (arrow == std::string::npos) {
                        break;
                    }
                    if (!AddressIsSane(address, sizeof(u64))) {
                        LOG_WARNING(Core, "DSMod: trace path left mapped memory at {:016X}",
                                    address);
                        address = 0;
                        break;
                    }
                    address = memory.Read64(address);
                    start = arrow + 1;
                }
                if (address == 0) {
                    return;
                }
            }
            LOG_INFO(Core, "DSMod: tracing {} -> {:016X}", trace_target, address);
            TraceChainTo(address);
        }
    }
    DumpWatchedMemory();
    ScanForMovingFloats();
    FindValueCluster();
    DumpHeapSnapshot();
}

/// The dev-tools-only EDEN_DSMOD_AUTO_MGRFIND tail of Tick() (mod_runtime.cpp): periodically
/// arms the mgrfind console command with no console interaction, for a streamed session. Pure
/// move, no logic change.
void ModRuntime::TickAutoMgrFindImpl() {
    // EDEN_DSMOD_AUTO_MGRFIND=<stamina>: periodically fire mgrfind (the BSS-global save-manager
    // finder) so a 60fps streamed session captures the durable route with no console interaction.
    static const s32 auto_mgr_target = [] {
        const char* const v = std::getenv("EDEN_DSMOD_AUTO_MGRFIND");
        return v != nullptr ? static_cast<s32>(std::strtol(v, nullptr, 0)) : 0;
    }();
    // Delay first fire past the load window (~20s) so the BSS scan never perturbs the delicate
    // save-load timing (that perturbation triggers the NoExecuteFault race, esp. at low fps).
    if (auto_mgr_target != 0 && tick_count > 1200 && !cmd_mgr_active && (tick_count % 300) == 20) {
        cmd_mgr_target = auto_mgr_target;
        cmd_mgr_active = true;
        cmd_mgr_cursor = 0;
        cmd_mgr_hits.clear();
    }
}

// EDEN_DSMOD_AUTOCHAIN: passively follow candidate static pointer chains every second and log
// when one lands on a plausible player object (small ints where stamina lives). Joe's method --
// pure pointer following from a fixed module global, NO scanning, so it cannot wedge the emu.
// Format: routes separated by ';', each "mainoffHex:off1/off2/..." (offs decimal or 0xhex).
// Example: EDEN_DSMOD_AUTOCHAIN="60c02388:0xB0;60c02388:0x8/0xF8/0x10/0x70"
void ModRuntime::DriveAutoChainImpl() {
    struct Route {
        VAddr mainoff;
        std::vector<s64> hops;
        std::string text;
    };
    static const std::vector<Route> routes = [] {
        std::vector<Route> out;
        const char* const spec = std::getenv("EDEN_DSMOD_AUTOCHAIN");
        if (spec == nullptr)
            return out;
        std::string all{spec};
        size_t p = 0;
        while (p < all.size()) {
            size_t semi = all.find(';', p);
            std::string one =
                all.substr(p, semi == std::string::npos ? std::string::npos : semi - p);
            p = semi == std::string::npos ? all.size() : semi + 1;
            if (one.empty())
                continue;
            Route r;
            r.text = one;
            const size_t colon = one.find(':');
            r.mainoff = std::strtoull(one.substr(0, colon).c_str(), nullptr, 16);
            if (colon != std::string::npos) {
                std::string rest = one.substr(colon + 1);
                size_t q = 0;
                while (q < rest.size()) {
                    size_t slash = rest.find('/', q);
                    std::string h =
                        rest.substr(q, slash == std::string::npos ? std::string::npos : slash - q);
                    q = slash == std::string::npos ? rest.size() : slash + 1;
                    if (!h.empty())
                        r.hops.push_back(std::strtoll(h.c_str(), nullptr, 0));
                }
            }
            out.push_back(std::move(r));
        }
        return out;
    }();
    if (routes.empty() || (tick_count % 30) != 0)
        return;

    auto& memory = system.ApplicationMemory();

    for (const auto& r : routes) {
        VAddr addr = main_region_begin + r.mainoff;
        if (!AddressIsSane(addr, sizeof(u64)))
            continue;
        u64 ptr = memory.Read64(addr);
        bool ok = ptr != 0;
        for (const s64 off : r.hops) {
            addr = ptr + static_cast<VAddr>(off);
            if (!AddressIsSane(addr, sizeof(u64))) {
                ok = false;
                break;
            }
            ptr = memory.Read64(addr);
            if (ptr == 0) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;
        // 'ptr' is the landing object (after the last hop deref). Inspect stamina slot +0x494.
        const VAddr obj = ptr;
        if (!AddressIsSane(obj + 0x4a4, 4))
            continue;
        const s32 s0 = static_cast<s32>(memory.Read32(obj + 0x494));
        const s32 s1 = static_cast<s32>(memory.Read32(obj + 0x498));
        const s32 s2 = static_cast<s32>(memory.Read32(obj + 0x49c));
        const s32 s3 = static_cast<s32>(memory.Read32(obj + 0x4a0));
        const bool plausible = s2 > 0 && s2 <= 1000 && s3 > 0 && s3 <= 1000;
        LOG_INFO(Core, "DSMod AUTOCHAIN {} -> obj {:012X}{} sta[494..4a0]={} {} {} {}", r.text, obj,
                 plausible ? " *** PLAYER? ***" : "", s0, s1, s2, s3);
    }
}

#endif // EDEN_DSMOD_BUILD_DEV_TOOLS

} // namespace Core::Mods
