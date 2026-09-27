// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// mod_console.cpp -- DriveCmd(), the EDEN_DSMOD_CMD live dev-console command dispatch, kept out
// of mod_runtime.cpp for the same reason as mod_re_tools.cpp (see that file's own banner).
// DriveCmdImpl() below is reached from the same-named ModRuntime::DriveCmd() shell in
// mod_runtime.cpp, which is empty unless EDEN_DSMOD_BUILD_DEV_TOOLS.

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

void ModRuntime::DriveCmdImpl() {
    // A live memory console for a running instance. EDEN_DSMOD_CMD names a file; write one line:
    //   readf <hexaddr> <n>   log n floats from addr
    //   readi <hexaddr> <n>   log n int32 from addr
    //   findi <val>           heap search (aligned) for an int32 value, log matches
    //   findf <val>           heap search for a float value
    //   watch <hexaddr>       log this addr's float+int every tick until "watch 0"
    static const char* const path = std::getenv("EDEN_DSMOD_CMD");
    if (path == nullptr) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    // Mirror console results to "<EDEN_DSMOD_CMD>.out" so a headless/streamed instance whose log we
    // cannot read still hands results back through a file.
    const auto emit = [](const std::string& line) {
        LOG_INFO(Core, "{}", line);
        if (const char* const p = std::getenv("EDEN_DSMOD_CMD")) {
            std::ofstream f(std::string(p) + ".out", std::ios::app);
            if (f) {
                f << line << '\n';
            }
        }
    };
    if (cmd_watch != 0 && AddressIsSane(cmd_watch, 8) && (tick_count % 8) == 0) {
        const u32 r = memory.Read32(cmd_watch);
        f32 fv{};
        std::memcpy(&fv, &r, sizeof(fv));
        LOG_INFO(Core, "DSMod watch {:012X}: i={} f={:g}", cmd_watch, static_cast<s32>(r), fv);
    }
    if (cmd_find_active) {
        constexpr u64 PagesPerTick = 8192;
        const VAddr fend = std::min<VAddr>(HeapHigh(), cmd_find_cursor + PagesPerTick * 0x1000);
        for (VAddr page = cmd_find_cursor; page < fend && cmd_find_hits.size() < 60;
             page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr)
                continue;
            for (u32 at = 0; at + 4 <= 0x1000; at += 4) {
                u32 w{};
                std::memcpy(&w, host + at, 4);
                if (w == cmd_find_want) {
                    cmd_find_hits.push_back(page + at);
                    if (cmd_find_hits.size() >= 60)
                        break;
                }
            }
        }
        cmd_find_cursor = fend;
        if (cmd_find_cursor >= HeapHigh() || cmd_find_hits.size() >= 60) {
            std::string out;
            for (VAddr a : cmd_find_hits)
                out += fmt::format(" {:012X}", a);
            emit(fmt::format("DSMod find {}: {} hit(s){}", cmd_find_label, cmd_find_hits.size(),
                             out));
            cmd_find_active = false;
        }
    }
    if (cmd_mgr_active) {
        // Joe-style durable-anchor finder: the save manager is a singleton stored in the module's
        // BSS. Scan global slots (module offset window where all observed globals live), reading
        // each 8-byte pointer once. Test two shapes against the on-screen stamina value: the slot
        // points straight at the player, or at a manager whose +0xB0 is the player. Signature = >=2
        // of the four u32 at player+0x494..+0x4a0 equal the target. Bounded one-shot reads ->
        // cannot wedge. Scan the WHOLE module data/BSS region -- the save-manager global can live
        // anywhere in it (observed globals span ~main+0x62xxxxx .. main+0x60Cxxxxx across titles),
        // so a narrow window misses it. Heavily sliced so it never wedges even at low fps.
        constexpr u64 WinBeg = 0x01000000ULL;
        const u64 WinEnd =
            std::min<u64>(main_region_size != 0 ? main_region_size : 0x70000000ULL, 0x70000000ULL);
        constexpr u64 PagesPerTick = 2048;
        const VAddr abs_beg = main_region_begin + WinBeg, abs_end = main_region_begin + WinEnd;
        if (cmd_mgr_cursor < abs_beg)
            cmd_mgr_cursor = abs_beg;
        const VAddr slice_end = std::min<VAddr>(abs_end, cmd_mgr_cursor + PagesPerTick * 0x1000);
        const auto sig = [&](VAddr obj) -> bool {
            const u8* h494 = memory.GetPointerSilent(obj + 0x494);
            if (h494 == nullptr)
                return false;
            s32 v[4];
            std::memcpy(v, h494, 16);
            // Player stamina block +0x494..+0x4a0. +0x49c=current, +0x4a0=max (equal when full).
            // Match target OR 2*target (the HUD halves the internal value), current==max, and sane.
            const s32 T = cmd_mgr_target;
            return (v[2] == T || v[2] == 2 * T) && v[3] == v[2] && v[2] > 0 && v[2] <= 2000;
        };
        for (VAddr page = cmd_mgr_cursor; page < slice_end && cmd_mgr_hits.size() < 40;
             page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr)
                continue;
            for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
                u64 g{};
                std::memcpy(&g, host + at, 8);
                if (g < 0x1000000ULL)
                    continue;
                const VAddr slot_off = (page + at) - main_region_begin;
                if (sig(static_cast<VAddr>(g))) {
                    cmd_mgr_hits.push_back(fmt::format("P@main+{:X}->{:012X}", slot_off, g));
                } else {
                    const u8* hb0 = memory.GetPointerSilent(static_cast<VAddr>(g) + 0xB0);
                    if (hb0 != nullptr) {
                        u64 pl{};
                        std::memcpy(&pl, hb0, 8);
                        if (pl >= 0x1000000ULL && sig(static_cast<VAddr>(pl)))
                            cmd_mgr_hits.push_back(
                                fmt::format("M@main+{:X}->{:012X}+B0->{:012X}", slot_off, g, pl));
                    }
                }
                if (cmd_mgr_hits.size() >= 40)
                    break;
            }
        }
        cmd_mgr_cursor = slice_end;
        if (cmd_mgr_cursor >= abs_end || cmd_mgr_hits.size() >= 40) {
            std::string out;
            for (const auto& hh : cmd_mgr_hits)
                out += " " + hh;
            emit(fmt::format("DSMod mgrfind={}: {} hit(s){}", cmd_mgr_target, cmd_mgr_hits.size(),
                             out));
            cmd_mgr_active = false;
        }
    }
    if (cmd_vfind_active) {
        // Tight vtable bound: real vtables point into the module's code/rodata, ~first 16 MB.
        // main_region_size is the whole ~1 GB image reservation (bss/heap) -> useless as a bound.
        constexpr u64 VtableSpan = 0x1000000; // 16 MB
        const u64 vt_lo = main_region_begin + 0x1000;
        const u64 vt_hi = main_region_begin + VtableSpan;
        constexpr u64 PagesPerTick = 256; // light: won't wedge the emu thread
        const VAddr vend = std::min<VAddr>(HeapHigh(), cmd_vfind_cursor + PagesPerTick * 0x1000);
        for (VAddr page = cmd_vfind_cursor; page < vend && cmd_vfind_hits.size() < 16;
             page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr)
                continue;
            for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
                u64 vt{};
                std::memcpy(&vt, host + at, 8);
                if (vt < vt_lo || vt >= vt_hi)
                    continue; // needs a REAL vtable
                const VAddr obj = page + at;
                const u8* const p2 = host + at; // same page, fast local reads
                for (u32 o = 0; o <= 0x600 && at + o + 4 <= 0x1000; o += 4) {
                    u32 w{};
                    std::memcpy(&w, p2 + o, 4);
                    if (w == cmd_vfind_want) {
                        cmd_vfind_hits.push_back(fmt::format("{:012X}(vt=main+{:X},@+{:#x})", obj,
                                                             vt - main_region_begin, o));
                        break;
                    }
                }
                if (cmd_vfind_hits.size() >= 16)
                    break;
            }
        }
        cmd_vfind_cursor = vend;
        if (cmd_vfind_cursor >= HeapHigh() || cmd_vfind_hits.size() >= 16) {
            std::string out;
            for (auto& h : cmd_vfind_hits)
                out += " " + h;
            LOG_INFO(Core, "DSMod vfind {}: {} obj(s){}", cmd_vfind_want, cmd_vfind_hits.size(),
                     out.empty() ? " -" : out);
            cmd_vfind_active = false;
        }
    }
    if (cmd_ptr_active) {
        // Phase 0: scan the module's DATA section (~first 16 MB, NOT the 1 GB reservation) for
        // statics that point at the target; then the heap in slices. cursor 0 = start module scan.
        constexpr u64 PagesPerTick = 2048;    // light
        constexpr u64 ModuleScan = 0x1000000; // 16 MB of module data, not main_region_size
        VAddr start, stop;
        bool in_module;
        if (cmd_ptr_cursor == 0) {
            start = main_region_begin;
            stop = main_region_begin + ModuleScan;
            in_module = true;
        } else if (cmd_ptr_cursor < HeapLow()) {
            // still in the module phase, sliced
            start = cmd_ptr_cursor;
            stop = std::min<VAddr>(main_region_begin + ModuleScan,
                                   cmd_ptr_cursor + PagesPerTick * 0x1000);
            in_module = true;
        } else {
            start = cmd_ptr_cursor;
            stop = std::min<VAddr>(HeapHigh(), cmd_ptr_cursor + PagesPerTick * 0x1000);
            in_module = false;
        }
        for (VAddr page = start & ~0xFFFULL; page < stop && cmd_ptr_hits.size() < 30;
             page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr)
                continue;
            for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
                u64 v{};
                std::memcpy(&v, host + at, 8);
                if (v == cmd_ptr_want) {
                    const VAddr loc = page + at;
                    if (in_module)
                        cmd_ptr_hits.push_back(fmt::format("main+{:X}", loc - main_region_begin));
                    else
                        cmd_ptr_hits.push_back(fmt::format("{:012X}", loc));
                    if (cmd_ptr_hits.size() >= 30)
                        break;
                }
            }
        }
        if (in_module) {
            cmd_ptr_cursor = (stop >= main_region_begin + ModuleScan) ? HeapLow() : stop;
        } else {
            cmd_ptr_cursor = stop;
        }
        if (cmd_ptr_cursor >= HeapHigh() || cmd_ptr_hits.size() >= 30) {
            std::string out;
            for (auto& h : cmd_ptr_hits)
                out += " " + h;
            emit(fmt::format("DSMod ptrto {:012X}: {} holder(s){}", cmd_ptr_want,
                             cmd_ptr_hits.size(), out.empty() ? std::string(" -") : out));
            cmd_ptr_active = false;
        }
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return;
    }
    std::ifstream f(path);
    std::string op;
    f >> op;
    if (op.empty()) {
        return;
    }
    {
        std::ofstream trunc(path, std::ios::trunc);
    }
    if (op == "mapdump") {
        // Write every cached area image whose key starts with the given prefix ("prefog:s010_cave",
        // "mapbase:s010_cave", "map:...") as a binary PPM next to <path>: what the map looks like
        // before / after each stage, for pixel-level comparison against the game's own map.
        std::string prefix, path;
        f >> prefix >> path;
        u32 written = 0;
        // Debug console reader of image_cache (tick thread), racing GetImage on the redraw
        // worker. Snapshot the matching (key, shared_ptr) pairs under a short
        // lock -- each shared_ptr keeps its Image alive regardless of a concurrent eviction sweep
        // -- then do the (potentially slow, multi-file, per-pixel) disk I/O below with the lock
        // released.
        std::vector<std::pair<std::string, std::shared_ptr<const Image>>> matches;
        {
            std::scoped_lock lk{asset_cache_mutex};
            for (const auto& [key, img] : image_cache) {
                if (key.starts_with(prefix) && img && img->w != 0 && img->h != 0) {
                    matches.emplace_back(key, img);
                }
            }
        }
        for (const auto& [key, img] : matches) {
            std::string safe = key;
            for (auto& ch : safe)
                if (ch == '/' || ch == ':' || ch == '@' || ch == '#' || ch == ' ')
                    ch = '_';
            std::ofstream o(path + "." + safe + ".ppm", std::ios::binary);
            if (!o)
                continue;
            o << "P6\n" << img->w << " " << img->h << "\n255\n";
            std::string row;
            row.resize(static_cast<size_t>(img->w) * 3);
            for (u32 y = 0; y < img->h; ++y) {
                for (u32 x = 0; x < img->w; ++x) {
                    const u32 px = img->pixels[static_cast<size_t>(y) * img->w + x];
                    // alpha 0 -> magenta so transparent reads apart from black
                    const bool clear = (px >> 24) == 0;
                    row[x * 3 + 0] = static_cast<char>(clear ? 255 : (px >> 16) & 0xFF);
                    row[x * 3 + 1] = static_cast<char>(clear ? 0 : (px >> 8) & 0xFF);
                    row[x * 3 + 2] = static_cast<char>(clear ? 255 : px & 0xFF);
                }
                o.write(row.data(), static_cast<std::streamsize>(row.size()));
            }
            ++written;
            emit(fmt::format("DSMod mapdump: wrote {} ({}x{}) for '{}'", path + "." + safe + ".ppm",
                             img->w, img->h, key));
        }
        emit(fmt::format("DSMod mapdump '{}': {} image(s)", prefix, written));
    } else if (op == "walls") {
        // Debug console reader, tick thread; map_walls is also written from AcceptModuleMap (tick
        // thread, so no hazard there) and read from GetImage (redraw worker) -- locked for
        // consistency with every other map_state_mutex member.
        std::scoped_lock lk{map_state_mutex};
        for (const auto& [area, tiles] : map_walls) {
            emit(fmt::format("map walls '{}': {} tiles", area, tiles.size()));
        }
    } else if (op == "action") {
        // Fire a manifest action once (a sequence, a page, a flag) from the console -- the way a
        // test drives a one-shot guest call such as a scenario warp without a button for it.
        std::string name, when;
        f >> name >> when;
        cmd_action = name;
        cmd_action_ingame = when == "ingame";
        emit(fmt::format("DSMod action '{}' queued{}", name,
                         cmd_action_ingame ? " (once in play)" : ""));
    } else if (op == "readf" || op == "readi") {
        std::string addr;
        u32 n = 4;
        f >> addr >> n;
        const VAddr base = std::strtoull(addr.c_str(), nullptr, 16);
        std::string out;
        for (u32 i = 0; i < n && i < 8192; ++i) { // the .out file takes a long line
            const VAddr a = base + i * 4;
            if (!AddressIsSane(a, 4)) {
                out += " ??";
                continue;
            }
            const u32 r = memory.Read32(a);
            if (op == "readf") {
                f32 v{};
                std::memcpy(&v, &r, sizeof(v));
                out += fmt::format(" +{}:{:g}", i * 4, v);
            } else
                out += fmt::format(" +{}:{}", i * 4, static_cast<s32>(r));
        }
        emit(fmt::format("DSMod read {:012X}:{}", base, out));
    } else if (op == "page") {
        // Switch the bottom-screen page by id or index (headless verification helper).
        std::string which;
        f >> which;
        size_t target = manifest.pages.size();
        for (size_t i = 0; i < manifest.pages.size(); ++i) {
            if (manifest.pages[i].id == which) {
                target = i;
                break;
            }
        }
        if (target == manifest.pages.size()) {
            const long n = std::strtol(which.c_str(), nullptr, 10);
            if (n >= 0 && static_cast<size_t>(n) < manifest.pages.size())
                target = n;
        }
        if (target < manifest.pages.size()) {
            current_page = target;
            emit(fmt::format("DSMod page -> {} '{}'", target, manifest.pages[target].id));
        } else {
            emit(fmt::format("DSMod page: no page '{}'", which));
        }
    } else if (op == "findi" || op == "findf") {
        std::string val;
        f >> val;
        if (op == "findi") {
            const s32 iv = std::strtol(val.c_str(), nullptr, 0);
            std::memcpy(&cmd_find_want, &iv, 4);
        } else {
            const f32 fv = std::strtof(val.c_str(), nullptr);
            std::memcpy(&cmd_find_want, &fv, 4);
        }
        cmd_find_active = true;
        cmd_find_cursor = HeapLow();
        cmd_find_hits.clear();
        cmd_find_label = op + " = " + val;
    } else if (op == "reload") {
        ReloadManifest();
    } else if (op == "mgrfind") {
        std::string val;
        f >> val;
        cmd_mgr_target = static_cast<s32>(std::strtol(val.c_str(), nullptr, 0));
        cmd_mgr_active = true;
        cmd_mgr_cursor = 0;
        cmd_mgr_hits.clear();
        LOG_INFO(Core, "DSMod mgrfind: scanning BSS globals for stamina={}", cmd_mgr_target);
    } else if (op == "snap") {
        cmd_snap_addr.clear();
        cmd_snap_val.clear();
        for (VAddr page = HeapLow(); page < HeapHigh(); page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr)
                continue;
            for (u32 at = 0; at + 4 <= 0x1000; at += 4) {
                f32 v{};
                std::memcpy(&v, host + at, 4);
                if (std::isfinite(v) && std::fabs(v) > 1.0f && std::fabs(v) < 100000.0f &&
                    v != std::floor(v)) {
                    cmd_snap_addr.push_back(page + at);
                    cmd_snap_val.push_back(v);
                }
            }
        }
        LOG_INFO(Core, "DSMod snap: {} coordinate-shaped floats recorded", cmd_snap_addr.size());
    } else if (op == "diff") {
        // Report snapped addresses that changed by a walking-sized amount (0.3..800).
        std::string out;
        int shown = 0;
        for (size_t i = 0; i < cmd_snap_addr.size() && shown < 24; ++i) {
            if (!AddressIsSane(cmd_snap_addr[i], 4))
                continue;
            const u32 r = memory.Read32(cmd_snap_addr[i]);
            f32 now{};
            std::memcpy(&now, &r, 4);
            const f32 d = now - cmd_snap_val[i];
            if (std::isfinite(now) && std::fabs(d) > 0.3f && std::fabs(d) < 800.0f) {
                out += fmt::format(" {:012X}:{:g}->{:g}", cmd_snap_addr[i], cmd_snap_val[i], now);
                ++shown;
            }
        }
        emit(fmt::format("DSMod diff:{}",
                         out.empty() ? std::string(" (no walking-sized changes)") : out));
    } else if (op == "pairfind") {
        // "pairfind <hexbase> <nints> <v1> <v2> <maxdelta>" -- where do two values sit near each
        // other? An inventory slot array shows up as a small, repeating delta; a definition table
        // shows up as one huge stride. Uncapped, unlike findi.
        std::string addr;
        u32 n = 0;
        s64 v1 = 0, v2 = 0;
        u32 maxd = 0x200;
        f >> addr >> n >> v1 >> v2 >> maxd;
        const VAddr base = std::strtoull(addr.c_str(), nullptr, 16);
        std::vector<VAddr> a_hits, b_hits;
        constexpr size_t Cap = 400000;
        for (u32 i = 0; i < n; ++i) {
            const VAddr at = base + i * 4;
            if (!AddressIsSane(at, 4))
                continue;
            const s32 v = static_cast<s32>(memory.Read32(at));
            if (v == v1 && a_hits.size() < Cap)
                a_hits.push_back(at);
            if (v == v2 && b_hits.size() < Cap)
                b_hits.push_back(at);
        }
        std::string out;
        u32 shown = 0;
        size_t j = 0;
        for (const VAddr a : a_hits) {
            while (j < b_hits.size() && b_hits[j] + maxd < a)
                ++j;
            for (size_t k = j; k < b_hits.size() && b_hits[k] <= a + maxd; ++k) {
                out += fmt::format(" {:012X}{:+#x}", a,
                                   static_cast<s64>(b_hits[k]) - static_cast<s64>(a));
                if (++shown >= 40)
                    break;
            }
            if (shown >= 40) {
                out += " ...";
                break;
            }
        }
        emit(fmt::format("DSMod pairfind {}/{}: {} & {} hits, {} pair(s){}", v1, v2, a_hits.size(),
                         b_hits.size(), shown, out.empty() ? " -" : out));
    } else if (op == "sfind") {
        // Find an inline UTF-32 ASCII string: "sfind <hexbase> <nints> <text>". The game stores
        // some UI labels one codepoint per word, so a plain byte search will not see them.
        std::string addr;
        u32 n = 1024;
        std::string want;
        f >> addr >> n >> want;
        const VAddr base = std::strtoull(addr.c_str(), nullptr, 16);
        std::string out;
        u32 shown = 0;
        if (!want.empty()) {
            const u32 first = static_cast<u32>(want[0]);
            for (u32 i = 0; i + want.size() < n; ++i) {
                const VAddr at = base + i * 4;
                if (!AddressIsSane(at, 4) || memory.Read32(at) != first)
                    continue;
                bool all = true;
                for (size_t k = 1; k < want.size(); ++k) {
                    const VAddr c = at + static_cast<u32>(k) * 4;
                    if (!AddressIsSane(c, 4) || memory.Read32(c) != static_cast<u32>(want[k])) {
                        all = false;
                        break;
                    }
                }
                if (!all)
                    continue;
                out += fmt::format(" {:012X}", at);
                if (++shown >= 30) {
                    out += " ...";
                    break;
                }
            }
        }
        emit(fmt::format("DSMod sfind '{}': {} hit(s){}", want, shown, out.empty() ? " -" : out));
    } else if (op == "cscan") {
        // Stateful differential scanner (Cheat Engine style). cscan <hexaddr> <n> resets a
        // candidate set = every int in the region; then cnarrow same/diff/inc/dec/in keeps the ones
        // matching a rule vs the last snapshot, updating the snapshot each pass. The equipped field
        // is the one value stable while idle yet different after each cycle -- survives where
        // one-shot diffs fail.
        std::string addr;
        u32 n = 1024;
        f >> addr >> n;
        cmd_scan_base = std::strtoull(addr.c_str(), nullptr, 16);
        cmd_scan_snap.assign(n, 0u);
        cmd_scan_cand.clear();
        cmd_scan_cand.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            const VAddr a = cmd_scan_base + i * 4;
            cmd_scan_snap[i] = AddressIsSane(a, 4) ? memory.Read32(a) : 0u;
            cmd_scan_cand.push_back(i);
        }
        emit(
            fmt::format("DSMod cscan {:012X}: {} candidates", cmd_scan_base, cmd_scan_cand.size()));
    } else if (op == "cnarrow") {
        std::string mode;
        f >> mode;
        std::vector<u32> vset; // for "in <v1> <v2> ..."
        if (mode == "in") {
            std::string t;
            while (f >> t)
                vset.push_back(static_cast<u32>(std::strtol(t.c_str(), nullptr, 0)));
        }
        std::vector<u32> keep;
        keep.reserve(cmd_scan_cand.size());
        for (const u32 i : cmd_scan_cand) {
            const VAddr a = cmd_scan_base + i * 4;
            if (!AddressIsSane(a, 4))
                continue;
            const u32 now = memory.Read32(a);
            const u32 old = cmd_scan_snap[i];
            bool k = false;
            if (mode == "same")
                k = (now == old);
            else if (mode == "diff")
                k = (now != old);
            else if (mode == "inc")
                k = (now == old + 1);
            else if (mode == "dec")
                k = (now + 1 == old);
            else if (mode == "in") {
                for (u32 v : vset)
                    if (now == v) {
                        k = true;
                        break;
                    }
            }
            if (k)
                keep.push_back(i);
            cmd_scan_snap[i] = now; // refresh baseline for the next pass
        }
        cmd_scan_cand.swap(keep);
        emit(fmt::format("DSMod cnarrow {}: {} left", mode, cmd_scan_cand.size()));
    } else if (op == "clist") {
        std::string out;
        u32 shown = 0;
        for (const u32 i : cmd_scan_cand) {
            const VAddr a = cmd_scan_base + i * 4;
            out += fmt::format(" {:012X}:{}", a,
                               static_cast<s32>(AddressIsSane(a, 4) ? memory.Read32(a) : 0));
            if (++shown >= 60) {
                out += " ...";
                break;
            }
        }
        emit(fmt::format("DSMod clist ({}){}", cmd_scan_cand.size(), out.empty() ? " -" : out));
    } else if (op == "msbt") {
        // msbt [<alias>#<label>]: the language decision, or one decoded text (icons as {U+XXXX})
        std::string rest;
        std::getline(f, rest);
        MsbtConsole(rest);
    } else if (op == "imgdump") {
        // imgdump <src> <out.png>: decode any image source (romfs:/composite:/...) to a PNG
        std::string rest;
        std::getline(f, rest);
        NxImageDump(rest);
    } else if (op == "writeb") {
        // writeb <abs hex addr> <value>: poke one guest byte (headless verification of reveal
        // flags and other bit-packed state; prints the byte before and after).
        std::string addr, val;
        f >> addr >> val;
        const VAddr a = std::strtoull(addr.c_str(), nullptr, 16);
        if (!AddressIsSane(a, 1) || val.empty()) {
            emit(fmt::format("DSMod writeb {:012X}: refused", a));
        } else {
            const u8 before = memory.Read8(a);
            memory.Write8(a, static_cast<u8>(std::strtoul(val.c_str(), nullptr, 0)));
            emit(fmt::format("DSMod writeb {:012X}: {:#04x} -> {:#04x}", a, before,
                             memory.Read8(a)));
        }
    } else if (op == "tap") {
        std::string sx, sy;
        f >> sx >> sy;
        const s32 tx = static_cast<s32>(std::strtol(sx.c_str(), nullptr, 0));
        const s32 ty = static_cast<s32>(std::strtol(sy.c_str(), nullptr, 0));
        {
            std::scoped_lock lk{tap_mutex};
            pending_taps.push_back(PendingTap{tx, ty});
        }
        emit(fmt::format("DSMod tap queued ({},{})", tx, ty));
    } else if (op == "drag") {
        // "drag x0 y0 x1 y1 [ms] [fling]": a synthetic finger through the aux touch path. Values
        // all in 0..1 are normalised to the aux panel (like the .btn driver); otherwise canvas
        // pixels. "fling" lifts the finger while it is still moving (no rest over the end point).
        std::array<float, 4> v{};
        u64 ms = 400;
        bool ok = true;
        bool fling = false;
        for (auto& c : v) {
            std::string tok;
            ok = ok && static_cast<bool>(f >> tok);
            c = ok ? std::strtof(tok.c_str(), nullptr) : 0.0f;
        }
        for (std::string tok; f >> tok;) {
            if (tok == "fling") {
                fling = true;
            } else {
                ms = std::strtoull(tok.c_str(), nullptr, 0);
            }
        }
        if (!ok) {
            emit("DSMod drag: usage drag <x0> <y0> <x1> <y1> [ms] [fling]");
        } else {
            cmd_drag.normalised =
                std::ranges::all_of(v, [](float c) { return c >= 0.0f && c <= 1.0f; });
            cmd_drag.x0 = v[0];
            cmd_drag.y0 = v[1];
            cmd_drag.x1 = v[2];
            cmd_drag.y1 = v[3];
            cmd_drag.move_ticks = MillisecondsToModTicks(ms);
            cmd_drag.end_dwell = fling ? 0 : 4;
            cmd_drag.start = tick_count;
            cmd_drag.active = true;
            emit(fmt::format("DSMod drag queued ({},{}) -> ({},{}) {} over {} ticks", v[0], v[1],
                             v[2], v[3], cmd_drag.normalised ? "normalised" : "canvas px",
                             cmd_drag.move_ticks));
        }
    } else if (op == "isnap") {
        // Snapshot up to `n` int32 from an absolute address (loops internally, no 64-word cap).
        std::string addr;
        u32 n = 1024;
        f >> addr >> n;
        cmd_isnap_base = std::strtoull(addr.c_str(), nullptr, 16);
        cmd_isnap_vals.clear();
        cmd_isnap_vals.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            const VAddr a = cmd_isnap_base + i * 4;
            cmd_isnap_vals.push_back(AddressIsSane(a, 4) ? memory.Read32(a) : 0u);
        }
        emit(fmt::format("DSMod isnap {:012X}: {} ints", cmd_isnap_base, cmd_isnap_vals.size()));
    } else if (op == "idiff") {
        // Report every int that changed since isnap (offset : old -> new). Optional filter: "idiff
        // small" keeps only |old|<=255 and |new|<=255 (indices / tool ids), cutting live-game
        // noise.
        std::string mode;
        f >> mode;
        const bool small = (mode == "small");
        std::string out;
        u32 shown = 0;
        for (size_t i = 0; i < cmd_isnap_vals.size(); ++i) {
            const VAddr a = cmd_isnap_base + i * 4;
            if (!AddressIsSane(a, 4))
                continue;
            const u32 now = memory.Read32(a);
            if (now == cmd_isnap_vals[i])
                continue;
            const s32 ov = static_cast<s32>(cmd_isnap_vals[i]);
            const s32 nv = static_cast<s32>(now);
            if (small && (std::abs(ov) > 255 || std::abs(nv) > 255))
                continue;
            out += fmt::format(" +{:#x}:{}->{}", i * 4, ov, nv);
            if (++shown >= 40) {
                out += " ...";
                break;
            }
        }
        emit(fmt::format("DSMod idiff{}", out.empty() ? " (none)" : out));
    } else if (op == "chain") {
        // Follow a main-relative pointer chain (Joe-style, NO scan). "chain <mainoff> <off1>
        // <off2>..." addr = main+mainoff; ptr = [addr]; then for each off: addr = ptr+off, ptr =
        // [addr]. Reports each hop's pointer + the final u32/floats. Instant, cannot freeze.
        std::string moff;
        f >> moff;
        VAddr addr = main_region_begin + std::strtoull(moff.c_str(), nullptr, 16);
        std::string out = fmt::format("main+{}: ", moff);
        u64 ptr = AddressIsSane(addr, 8) ? memory.Read64(addr) : 0;
        out += fmt::format("[{:012X}]={:012X}", addr, ptr);
        std::string off;
        while (f >> off) {
            const s64 o = std::strtoll(off.c_str(), nullptr, 0);
            addr = ptr + o;
            if (!AddressIsSane(addr, 8)) {
                out += fmt::format(" +{:#x}->BAD", o);
                break;
            }
            ptr = memory.Read64(addr);
            out += fmt::format(" +{:#x}->{:012X}", o, ptr);
        }
        // also show what's at the final pointer (as ints, first few words)
        if (AddressIsSane(addr, 16)) {
            out += " | @dst:";
            for (u32 k = 0; k < 4; ++k)
                out += fmt::format(" {}", static_cast<s32>(memory.Read32(addr + k * 4)));
        }
        emit(fmt::format("DSMod chain {}", out));
    } else if (op == "ptrto") {
        // Find 8-aligned locations (heap + main statics) holding a pointer to <hexaddr>. Sliced.
        // Static hits (main+X) are the durable route anchors. Also scans the main module region.
        std::string addr;
        f >> addr;
        cmd_ptr_want = std::strtoull(addr.c_str(), nullptr, 16);
        cmd_ptr_active = true;
        cmd_ptr_cursor = 0;
        cmd_ptr_hits.clear();
    } else if (op == "vfind") {
        // Find heap objects with a vtable (main-region ptr at +0) that contain <value> somewhere in
        // their first 0x600 bytes; report object base, the vtable, and the offset. Sliced.
        std::string val;
        f >> val;
        u32 iv{};
        const s32 v = std::strtol(val.c_str(), nullptr, 0);
        std::memcpy(&iv, &v, 4);
        cmd_vfind_want = iv;
        cmd_vfind_active = true;
        cmd_vfind_cursor = HeapLow();
        cmd_vfind_hits.clear();
    } else if (op == "objfind") {
        // Find heap objects (vtable = a main-region pointer at +0) that have <value> at +<hexoff>.
        // This locates the player-data object by its shape (e.g. stamina at +0x4AE per the cheat).
        std::string offs, val;
        f >> offs >> val;
        const s64 off = std::strtoll(offs.c_str(), nullptr, 0);
        u32 want{};
        const s32 iv = std::strtol(val.c_str(), nullptr, 0);
        std::memcpy(&want, &iv, 4);
        const u64 vt_lo = main_region_begin + 0x1000, vt_hi = main_region_begin + 0x1000000;
        std::string out;
        int hits = 0;
        for (VAddr page = HeapLow(); page < HeapHigh() && hits < 20; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr)
                continue;
            for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
                u64 vt{};
                std::memcpy(&vt, host + at, 8);
                if (vt < vt_lo || vt >= vt_hi)
                    continue; // needs a REAL vtable (~first 16MB)
                const VAddr fld = page + at + off;
                if (!AddressIsSane(fld, 4))
                    continue;
                if (memory.Read32(fld) == want) {
                    out += fmt::format(" {:012X}(vt=main+{:X})", page + at, vt - main_region_begin);
                    if (++hits >= 20)
                        break;
                }
            }
        }
        LOG_INFO(Core, "DSMod objfind [+{:#x}]=={}: {} obj(s){}", off, val, hits,
                 out.empty() ? " -" : out);
    } else if (op == "cluster") {
        // Find where 2-4 known u32 values co-occur within a 0x600 window -> the save/player struct.
        // e.g. "cluster 2500 300 3" finds gold+stamina+day together regardless of exact offsets.
        std::vector<u32> want;
        std::string tok2;
        while (f >> tok2 && want.size() < 4)
            want.push_back(static_cast<u32>(std::strtoul(tok2.c_str(), nullptr, 0)));
        if (want.empty()) {
            LOG_INFO(Core, "DSMod cluster: need values");
            return;
        }
        // First collect all addresses of want[0].
        std::vector<VAddr> anchors;
        for (VAddr page = HeapLow(); page < HeapHigh() && anchors.size() < 2000; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr)
                continue;
            for (u32 at = 0; at + 4 <= 0x1000; at += 4) {
                u32 w{};
                std::memcpy(&w, host + at, 4);
                if (w == want[0])
                    anchors.push_back(page + at);
            }
        }
        std::string out;
        int shown = 0;
        for (VAddr a : anchors) {
            bool all = true;
            std::string offs;
            for (size_t k = 1; k < want.size(); ++k) {
                bool found = false;
                for (s32 off = -0x600; off <= 0x600 && !found; off += 4) {
                    const VAddr b = a + off;
                    if (!AddressIsSane(b, 4))
                        continue;
                    if (memory.Read32(b) == want[k]) {
                        found = true;
                        offs += fmt::format(" v{}@{:+#x}", k, off);
                    }
                }
                if (!found) {
                    all = false;
                    break;
                }
            }
            if (all) {
                out += fmt::format(" [{:012X}{}]", a, offs);
                if (++shown >= 12)
                    break;
            }
        }
        LOG_INFO(Core, "DSMod cluster: {} match(es) (anchor {} at {} sites){}", shown, want[0],
                 anchors.size(), out.empty() ? " -" : out);
    } else if (op == "hexdump") {
        VAddr base = 0;
        u64 len = 0x80;
        u64 deref_len = 0x40;
        std::string address, length;
        f >> address >> length;
        base = std::strtoull(address.c_str(), nullptr, 16);
        if (!length.empty()) {
            len = std::strtoull(length.c_str(), nullptr, 0);
        }
        len = std::min<u64>(len, 0x400);
        deref_len = std::min<u64>(deref_len, 0x400);
        const auto words = [&](VAddr at, u64 n, const char* ind) {
            for (u64 off = 0; off < n; off += 32) {
                std::string line = fmt::format("{}+{:04X}:", ind, off);
                for (u64 k = 0; k < 32 && off + k < n; k += 8) {
                    line += AddressIsSane(at + off + k, 8)
                                ? fmt::format(" {:016X}", memory.Read64(at + off + k))
                                : std::string{" ????????????????"};
                }
                emit(line);
            }
        };
        emit(fmt::format("{} base={:012X} len={:X}", op, base, len));
        if (base != 0 && AddressIsSane(base, 8)) {
            words(base, len, "  ");
            for (u64 off = 0; off + 8 <= len; off += 8) {
                const u64 v = memory.Read64(base + off);
                if (v >= HeapLow() && v < HeapHigh() && AddressIsSane(v, deref_len)) {
                    emit(fmt::format("  [+{:03X}] -> {:012X}:", off, v));
                    words(v, deref_len, "      ");
                }
            }
        }
    } else if (op == "gridpgm") {
        std::string path;
        f >> path;
        // Debug console reader, tick thread, racing MarkVisitedAt on the redraw worker. Locked for
        // the whole block including the file write -- a rare, developer-only command, not a hot
        // path.
        std::scoped_lock lk{map_state_mutex};
        const auto grid = map_visited.find(live_scenario);
        if (grid == map_visited.end()) {
            emit("gridpgm: no published visibility grid");
        } else {
            std::ofstream out(path, std::ios::binary);
            out << "P5\n" << VisitedGrid::Cols << " " << VisitedGrid::Rows << "\n255\n";
            for (u8 cell : grid->second.cells) {
                out.put(static_cast<char>(cell * 127));
            }
            emit("gridpgm: wrote published module visibility");
        }
    } else if (op == "items") {
        if (const auto area = manifest.map_areas.find(live_scenario);
            area != manifest.map_areas.end()) {
            for (const auto& marker : area->second.markers) {
                emit(fmt::format("{} hidden={} opened={} collected={} unveiled={} veiled={}",
                                 marker.name, marker.hidden, marker.opened, marker.collected,
                                 marker.unveiled, marker.veiled));
            }
        }
    } else if (op == "value") {
        // value <name>...: published values as of the last tick (points, derived, @ state).
        std::string line = "DSMod value";
        std::string name;
        while (f >> name) {
            if (const auto fl = tick_snapshot.floats.find(name); fl != tick_snapshot.floats.end()) {
                line += fmt::format(" {}={:.4f}f", name, fl->second);
            } else if (const auto in = tick_snapshot.ints.find(name);
                       in != tick_snapshot.ints.end()) {
                line += fmt::format(" {}={}", name, in->second);
            } else if (const auto tx = tick_snapshot.texts.find(name);
                       tx != tick_snapshot.texts.end()) {
                line += fmt::format(" {}=\"{}\"", name, tx->second.substr(0, 200));
            } else {
                line += fmt::format(" {}=missing", name);
            }
        }
        emit(line);
    } else if (op == "animinfo") {
        // animinfo: page transition + widget-group animation state, haptics raised so far.
        std::string line =
            fmt::format("DSMod animinfo page {} transition {} groups:", current_page,
                        page_anim.active ? fmt::format("active {}->{} frame {}", page_anim.req.from,
                                                       page_anim.req.to, page_anim.frames)
                                         : std::string{"idle"});
        for (const auto& def : group_defs) {
            const auto g = group_anims.find(def.anim->key);
            line += fmt::format(" ['{}' box {},{},{},{} {} pos {:.2f}{}]", def.anim->key,
                                def.box[0], def.box[1], def.box[2], def.box[3],
                                g != group_anims.end() && g->second.open ? "open" : "closed",
                                g != group_anims.end() ? g->second.pos : -1.0f,
                                g != group_anims.end() && g->second.moving ? " moving" : "");
        }
        line += fmt::format(" haptics raised {}", system.GPU().DSModAux().haptic_raised.load());
        emit(line);
    } else if (op == "mapinfo") {
        // mapinfo [wx wy]: each Map widget's last drawn transform and on-screen dynamic markers;
        // with a world position, also the canvas px it is drawn at (for aiming test taps).
        std::string sx, sy;
        f >> sx >> sy;
        // Debug console reader, locked for the whole block including the string building -- rare,
        // developer-only, not a hot path.
        std::scoped_lock rlk{map_records_mutex};
        std::string line =
            fmt::format("DSMod mapinfo page {} (records for page {}):", current_page,
                        map_records_page == ~size_t{0} ? -1 : static_cast<s64>(map_records_page));
        for (const auto& r : map_draw_records_published) {
            line += fmt::format(" [widget {} area '{}' rect {},{},{},{} centre ({:.3f},{:.3f}) "
                                "ppw {:.5f}",
                                r.widget_index, r.area, r.rect[0], r.rect[1], r.rect[2], r.rect[3],
                                r.cx, r.cy, r.ppw);
            if (!sx.empty() && !sy.empty()) {
                const float wx = std::strtof(sx.c_str(), nullptr);
                const float wy = std::strtof(sy.c_str(), nullptr);
                line += fmt::format(" world ({:.3f},{:.3f}) at canvas ({:.1f},{:.1f})", wx, wy,
                                    r.rect[0] + r.rect[2] * 0.5f + (wx - r.cx) * r.ppw,
                                    r.rect[1] + r.rect[3] * 0.5f - (wy - r.cy) * r.ppw);
            }
            line += fmt::format(" hits {}", r.hits.size());
            for (const auto& h : r.hits) {
                line += fmt::format(" {}#{}@({:.0f},{:.0f})", h.group, h.index, h.sx, h.sy);
            }
            line += "]";
        }
        for (const auto& [g, v] : map_selections) {
            line += fmt::format(" sel {}={}", g, v);
        }
        emit(line);
    } else if (op == "markers" || op == "mlog") {
        emit("Title-specific diagnostics are supplied by the installed dual-screen module.");
    } else if (op == "watch") {
        std::string addr;
        f >> addr;
        cmd_watch = std::strtoull(addr.c_str(), nullptr, 16);
        LOG_INFO(Core, "DSMod watch set to {:012X}", cmd_watch);
    } else if (op == "bbwalk") {
        // Find a crc64 blackboard key in the LIVE heap and dump the value + one pointer level, so
        // the CGameBlackboard node layout can be read where the offline dump had unmapped zeros.
        std::string h;
        f >> h;
        const u64 want = std::strtoull(h.c_str(), nullptr, 16);
        int found = 0;
        for (VAddr page = HeapLow(); page < HeapHigh() && found < 8; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                continue;
            }
            for (u32 at = 0; at + 8 <= 0x1000 && found < 8; at += 8) {
                u64 v{};
                std::memcpy(&v, host + at, 8);
                if (v != want) {
                    continue;
                }
                const VAddr node = page + at;
                ++found;
                std::string dump;
                for (int k = 0; k < 6 && AddressIsSane(node + k * 8, 8); ++k) {
                    dump += fmt::format(" {:016X}", memory.Read64(node + k * 8));
                }
                emit(fmt::format("DSMod bb @{:012X}:{}", node, dump));
                // Recursively dump a few pointer levels, and flag any run of bytes that reads like
                // an array (the decoded FOW cells we are hunting for).
                const std::function<void(VAddr, int)> descend = [&](VAddr p, int depth) {
                    if (depth > 3 || !AddressIsSane(p, 32)) {
                        return;
                    }
                    std::string d;
                    for (int k = 0; k < 8 && AddressIsSane(p + k * 8, 8); ++k) {
                        d += fmt::format(" {:016X}", memory.Read64(p + k * 8));
                    }
                    // count how many of the next 256 bytes are small (0..3) -- a cell-state array.
                    int small = 0, nonzero = 0;
                    for (int k = 0; k < 256 && AddressIsSane(p + k, 1); ++k) {
                        const u8 b = static_cast<u8>(memory.Read32(p + k) & 0xFF);
                        if (b <= 3)
                            ++small;
                        if (b != 0)
                            ++nonzero;
                    }
                    emit(fmt::format("  {}L{} @{:012X} small={}/256 nz={}:{}",
                                     std::string(depth * 2, ' '), depth, p, small, nonzero, d));
                    for (int voff = 0; voff < 24;
                         voff += 8) { // follow the first few child pointers
                        if (!AddressIsSane(p + voff, 8)) {
                            continue;
                        }
                        const VAddr c = memory.Read64(p + voff);
                        if (AddressIsSane(c, 32) && c != p) {
                            descend(c, depth + 1);
                        }
                    }
                };
                for (int voff : {8, 24, 40}) {
                    if (!AddressIsSane(node + voff, 8)) {
                        continue;
                    }
                    descend(memory.Read64(node + voff), 1);
                }
            }
        }
        emit(fmt::format("DSMod bbwalk {:016X}: {} hit(s)", want, found));
    } else if (op == "bbget" || op == "bbhex") {
        emit("Blackboard diagnostics belong to the installed title module.");
    }
}

#endif // EDEN_DSMOD_BUILD_DEV_TOOLS

} // namespace Core::Mods
