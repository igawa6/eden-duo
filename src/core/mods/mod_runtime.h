// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime for declarative dual-screen mod packages. Modelled on Core::Memory::CheatEngine: a
// core-timing event samples guest RAM at a fixed rate, but at 60 Hz rather than the cheat VM's
// 12 Hz, and the result drives a UI that the renderer presents on the second screen.
//
// Declares ModRuntime and the manifest-parser entry points (types: mod_types.h, drawing:
// mod_ui.h). Per tick: sample -> derived -> input/actions -> page binds -> redraw/publish.
// Each member function is tagged with the file that defines it: a "// --- ... (mod_x.cpp) ---"
// line over a contiguous run, else "// mod_x.cpp" on or above it ("shell -> f" = a dev-tools shell
// whose FooImpl is in f). Member order is load-bearing, so members are annotated, never moved.
//   mod_runtime.cpp       lifecycle, Tick, dev-tools shells
//   mod_manifest.cpp      Discover, manifest parsing, ReloadManifest
//   mod_state.cpp         guest reads, SampleState, EvaluateDerived, ApplyEnforceRules
//   mod_actions.cpp       RunAction, virtual-pad presses, headless input drivers
//   mod_input.cpp         gestures, taps, scroll, haptics, interaction state
//   mod_pages.cpp         page binds, page transitions, group animations
//   mod_assets.cpp        asset bytes, BNTX/DDS decoders, fonts, image cache
//   mod_map.cpp           fog of war, map masks, GetImage and the "map:" rasteriser
//   mod_redraw.cpp        UiSignature, dirty rects, PublishUi, redraw worker, GPU composite
//   mod_guest_bridge.cpp  guest calls, spies, patches, symbol resolution
//   mod_module_host.cpp, mod_module_services.cpp  native title module
//   mod_nx_runtime.cpp    Nintendo assets, composites, msbt
//   mod_re_tools.cpp, mod_console.cpp  dev-tools FooImpl bodies
//   engine_ichigo.cpp, engine_mercury.cpp, engine_il2cpp.cpp  engine-specific decoders, fonts,
//                         Dread's player finder, IL2CPP class and method lookups
// Threads: Tick on the CoreTiming timing thread, RunRedrawJob on "DSModRedraw", OnGuestBreakpoint
// on guest CPU threads, decodes on asset workers; shared mutexes are documented where declared.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "common/common_types.h"
#include "core/hle/kernel/svc_types.h"
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/mod_input_hold.h"
#include "core/mods/mod_input_swipe.h"
#include "core/mods/mod_module.h"
#include "core/mods/mod_persist.h"
#include "core/mods/mod_sources.h"
#include "core/mods/mod_types.h"
#include "core/mods/mod_ui.h"
#include "video_core/dsmod/aux_routing.h"

namespace Core {
class System;
class ArmInterface;
} // namespace Core

namespace Kernel {
class KThread;
}

namespace Core::Timing {
class CoreTiming;
struct EventType;
} // namespace Core::Timing

namespace Core::Mods {

/// Uses the runtime's full manifest parser to decide whether package discovery would accept a
/// manifest as having a renderable page. Invalid field types and malformed widgets return false.
[[nodiscard]] bool IsUsableDualScreenManifest(const nlohmann::json& json) noexcept;
/// The runtime's manifest parser for tools (offline checks, benchmarks); false on a parse error.
[[nodiscard]] bool ParseDualScreenManifest(const nlohmann::json& json, Manifest& out) noexcept;
/// The manifest's per-area map parser on its own: one "map.areas" JSON object -> MapArea entries
/// (added with emplace). Used for inline areas and for a module's "map.areas_src" data. False when
/// `areas` is not an object or a value has the wrong type.
[[nodiscard]] bool ParseMapAreasJson(const nlohmann::json& areas,
                                     std::unordered_map<std::string, MapArea>& out) noexcept;

/// The dual-screen runtime contract version this build implements. A package declares the oldest
/// runtime it works with as "min_runtime" (an integer, in dualscreen/manifest.json and/or the
/// package's package.json); a package that needs a newer runtime is not parsed or loaded at all --
/// the second screen shows a built-in "update Eden" page instead (UpdateRequiredManifest).
/// Runtimes before 11 had no version and ignore the key. History:
///   <= 10  unversioned (grow/shrink transitions, page_binds)
///   11     scroll regions (page "scrolls"), text dirty-rect fix, "module:" composite layers +
///          full repaint when module images land (asset-free packages), min_runtime gating
///   12     module data extension: "module:" byte sources (ReadAssetBytes, e.g. map geometry)
///          + map.areas_src (the module generates the map areas from the game's romfs)
///   13     press-and-hold (widget "on_hold" / "hold_ms", DrainTaps "hold"), hold haptic
///          (HapticKind::Hold, manifest haptics "hold"); redraw worker: a job superseded by a
///          newer dispatch still publishes (redraw_authority_generation) + UnpublishedRegions,
///          an anim group's settle frame cleared on the dispatch path; EDEN_DSMOD_IMAGE_TIMING
///   14     horizontal swipe (widget "on_swipe_left" / "on_swipe_right" / "swipe_px", DrainTaps
///          "swipe"), swipe haptic (HapticKind::Swipe, manifest haptics "swipe"); map widget
///          image_bind / overlays / per-slot dynamic-marker images, bars, dim, frame and tint;
///          map view_rect binds; @map_tap_x/_y/_seq readable by modules; label "color_markup"
///          ({c:#AARRGGBB}...{/c}); module image and font retry
///   15     named asset sources (AssetSources): "base:" (the program romfs without update or
///          LayeredFS) and "aoc:" (the add-on content data romfs), module read_romfs through
///          them, EDEN_DSMOD_CAP_SOURCE_* bits + get_i64("__source:<prefix>"), unknown prefixes
///          fail instead of reading romfs; "module_tick_hidden" + EDEN_DSMOD_CAP_(NO_)TICK_WHEN_HIDDEN;
///          label/value "outline_copy"; button "border" / "text_inset", pips "gap", bar "frame",
///          map "label_offset" and the map.style door / collectible / blink / pin keys; "{i}" in
///          src_names, empty_src, suffix, max_sep, table and text_map; vertical swipe (widget
///          "on_swipe_up" / "on_swipe_down", mod_input_swipe.h) and the bound default view of a
///          non-map pan_zoom widget ("view_zoom_bind" / "view_cx_bind" / "view_cy_bind" /
///          "view_reset_bind", mod_view_default.h); manifest "persist_flags" (runtime flags saved
///          on change and restored at load, mod_persist.h)
inline constexpr u32 DualScreenRuntimeVersion = 15;

/// Regions a redraw-worker job painted into its canvas but did not publish because it went stale
/// (runtime 13). Before runtime 13 a job already running when the next was dispatched finished
/// stale, and the newer partial job repainted and published only its own clip -- so the stale
/// job's region stayed unpublished (rows moved by y_bind stuck on screen: MK8D rank table during
/// overtakes). Since redraw_authority_generation, only a tick-thread takeover (page transition,
/// synchronous carve-out) makes a job stale, and those sites also force the next dispatch to be a
/// full redraw, so this is a safety net: RunRedrawJob notes a stale job here and the next
/// published job publishes the noted regions too (or publishes full).
struct UnpublishedRegions {
    /// More noted rects than this and the next publish is simply full.
    static constexpr size_t MaxRects = 64;
    std::vector<std::array<s32, 4>> rects;
    bool full{false}; ///< a skipped full (non-partial) redraw: the next publish must be full
    /// A stale job: remember what it painted.
    void Note(const RenderExtras& extras, bool partial);
    /// The next job to publish: merge the notes into its (partial) extras and clear them. Returns
    /// false when that publish must be full instead (a skipped full redraw); true otherwise.
    bool Apply(RenderExtras& extras, s32 canvas_w, s32 canvas_h);
    void Clear() {
        rects.clear();
        full = false;
    }
    bool Empty() const {
        return rects.empty() && !full;
    }
};

/// The runtime a package asks for: the larger "min_runtime" of `manifest` and `package` (either
/// may be null / not an object). 0 when neither declares one. Reads only that key, so it works on
/// any JSON a newer package format might bring. A present but malformed value (not a
/// non-negative integer or a decimal string) returns UINT32_MAX: the package is gated, never
/// half-loaded.
[[nodiscard]] u32 PackageMinRuntime(const nlohmann::json* manifest,
                                    const nlohmann::json* package) noexcept;

/// Nintendo asset references and composite images (mod_nx_runtime.cpp): caches, the decode
/// worker and its queues. Kept out of this header; always allocated with the runtime.
struct NxAssetState;
struct MsbtResult;
struct NxAssetStateDeleter {
    void operator()(NxAssetState* state) const;
};
class ModRuntime;
/// `rt` serves "module:" composite layers (LoadModuleImageSync); may be null (tools).
[[nodiscard]] NxAssetState* MakeNxAssetState(ModRuntime* rt = nullptr);

/// Snapshot keys built from a name, made once and reused every tick instead of concatenated per
/// publish ("@flag:" + name, element keys "<point><i>"). A key is a pure function of its inputs, so
/// a cache never goes stale (a manifest reload only adds entries). Tick thread only.
class PrefixedKeys {
public:
    explicit PrefixedKeys(std::string_view prefix_) : prefix{prefix_} {}
    const std::string& operator()(const std::string& name) {
        auto it = keys.find(name);
        if (it == keys.end()) {
            std::string key;
            key.reserve(prefix.size() + name.size());
            key.append(prefix).append(name);
            it = keys.emplace(name, std::move(key)).first;
        }
        return it->second;
    }

private:
    std::string_view prefix;
    std::unordered_map<std::string, std::string> keys;
};

/// "<base><index>" keys (a count point's elements), per base, grown on demand. Tick thread only.
class IndexedKeys {
public:
    const std::string& operator()(const std::string& base, size_t index) {
        auto& list = keys[base];
        while (list.size() <= index) {
            list.push_back(base + std::to_string(list.size()));
        }
        return list[index];
    }

private:
    std::unordered_map<std::string, std::vector<std::string>> keys;
};

/// A tap that arrived from the second screen, in canvas pixels.
struct PendingTap {
    s32 x{};
    s32 y{};
    /// Runtime 13: not a tap but a fired press-and-hold; runs this action (the on_hold widget's).
    std::string hold_action;
    /// Runtime 14: not a tap but a fired swipe; runs this action (the swipe widget's
    /// on_swipe_left / on_swipe_right, or on_swipe_up / on_swipe_down).
    std::string swipe_action;
};

class ModRuntime {
public:
    // --- lifecycle (mod_runtime.cpp) ---------------------------------------------------------
    explicit ModRuntime(System& system_, Manifest manifest_);
    ~ModRuntime();

    /// Guest extents the package's "main+..." offsets are relative to.
    void SetMainMemoryParameters(VAddr main_region_begin, u64 main_region_size);

    /// Starts the sampling event. Safe to call once emulation is running.
    void Initialize();

    // mod_guest_bridge.cpp
    /// Called from the kernel when a guest breakpoint fires. Returns true when this was one of
    /// ours and the thread may resume immediately.
    bool OnGuestBreakpoint(Kernel::KThread& thread, Core::ArmInterface& arm_interface);

    // --- package discovery (mod_manifest.cpp) ------------------------------------------------
    /// The page shown for a title without a package: its icon, dimmed, centred on black.
    static std::optional<Manifest> IdleManifest(u64 title_id);
    /// The page shown instead of a package that needs a newer runtime (`required` > this build's
    /// DualScreenRuntimeVersion): built in, nothing of the package is parsed or loaded.
    static Manifest UpdateRequiredManifest(u64 title_id, u32 required, const std::string& mod_dir);
    /// Discovers a package for the running title. Returns nullopt when the title has none.
    static std::optional<Manifest> Discover(System& system, u64 title_id,
                                            const std::array<u8, 0x20>& build_id);

private:
    void Tick(); // mod_runtime.cpp
    /// EDEN_DSMOD_AUTO_MGRFIND tail of Tick(), split into mod_re_tools.cpp.
    void TickAutoMgrFindImpl();                            // mod_re_tools.cpp
    void ApplyEnforceRules(const StateSnapshot& snapshot); // mod_state.cpp
    /// The guest-bridge upkeep that does not depend on the page: the stuck-call watchdog and the
    /// package's code patches. Runs every tick, second screen shown or not (mod_state.cpp).
    void MaintainGuestBridge();
    /// Dev-tools-only tail of ApplyEnforceRules(), split into mod_re_tools.cpp.
    void ApplyEnforceRulesDevToolsImpl(const StateSnapshot& snapshot); // mod_re_tools.cpp
    void ArmSpies();                                                   // mod_guest_bridge.cpp
    void ApplyPatches();                                               // mod_guest_bridge.cpp
    void DumpWatchedMemory();                       // mod_runtime.cpp shell -> mod_re_tools.cpp
    void DumpWatchedMemoryImpl();                   // mod_re_tools.cpp
    void DumpOneWatch(const std::string& spec);     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void DumpOneWatchImpl(const std::string& spec); // mod_re_tools.cpp
    void ScanForMovingFloats();                     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void ScanForMovingFloatsImpl();                 // mod_re_tools.cpp
    void TraceChainTo(VAddr target);                // mod_runtime.cpp shell -> mod_re_tools.cpp
    void TraceChainToImpl(VAddr target);            // mod_re_tools.cpp
    std::set<u64> seen_states;
    void DumpLuaRegistry();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void DumpLuaRegistryImpl(); // mod_re_tools.cpp
    void DescribeStringUses(const std::string& text); // mod_runtime.cpp shell -> mod_re_tools.cpp
    void DescribeStringUsesImpl(const std::string& text); // mod_re_tools.cpp
    // --- fire/rest differential search ------------------------------------------------------
    void DiffScan();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void DiffScanImpl(); // mod_re_tools.cpp
    /// True once the game reports a live player. Every search tool needs this: the module, the
    /// string literals and even the anchor objects all exist long before there is anything to
    /// find, and a tool that runs at boot reports a confident, meaningless answer.
    bool InGameplay() const; // mod_state.cpp
    /// Last positive value read from the manifest's declared `gameplay_point` (default "energy"),
    /// cached for InGameplayHonest() -- see SampleState.
    mutable f32 last_read_gameplay_signal{0.0f};
    void HeapFind();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void HeapFindImpl(); // mod_re_tools.cpp
    void FieldProbe();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void FieldProbeImpl(); // mod_re_tools.cpp
    void RangeWatch();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void RangeWatchImpl(); // mod_re_tools.cpp
    void ClassDump();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void ClassDumpImpl(); // mod_re_tools.cpp
    void ArrayDump();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void ArrayDumpImpl(); // mod_re_tools.cpp
    // mod_runtime.cpp shell -> mod_re_tools.cpp
    /// Who refers to this object, allowing for it being embedded in something larger? A trace
    /// that only asks about the exact address finds nothing whenever the holder points at the
    /// parent allocation instead, which is the usual case for a component inside an actor.
    void TraceWithSlack(VAddr object, s64 slack) const;
    void TraceWithSlackImpl(VAddr object, s64 slack) const; // mod_re_tools.cpp
    // mod_runtime.cpp shell -> mod_re_tools.cpp
    /// Walk backwards from an address through several levels of "who points at this", allowing
    /// each hop to land a little before the target, and report anything that turns out to live
    /// in the module. A static is the only root worth having: it is the same address in every
    /// run, which no heap object is.
    void TraceToStatic(VAddr target, int depth, s64 slack) const;
    void TraceToStaticImpl(VAddr target, int depth, s64 slack) const; // mod_re_tools.cpp
    // mod_runtime.cpp shell -> mod_re_tools.cpp
    /// Walk forward from every module static, following pointers, looking for one that reaches
    /// an address. Cheaper than searching backwards -- there are far fewer roots than holders --
    /// and it is the direction the game itself takes to find the player.
    void PathFromStatics(VAddr target, int depth, s64 slack) const;
    void PathFromStaticsImpl(VAddr target, int depth, s64 slack) const; // mod_re_tools.cpp
    /// A route that reached the target once, and how often it still does. A slot the game reuses
    /// resolves correctly for an instant and then points elsewhere -- only a route that keeps
    /// resolving is a binding.
    struct PathRoute {
        s64 root;              ///< module offset the walk starts from
        std::vector<s64> hops; ///< offset added before each dereference
        s64 delta;             ///< from the final pointer to the value
        int good{0};
    };
    void RecheckRoutes();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void RecheckRoutesImpl(); // mod_re_tools.cpp
    void MotionScan();        // mod_runtime.cpp shell -> mod_re_tools.cpp
    void MotionScanImpl();    // mod_re_tools.cpp
    /// Locate an array by its shape and remember it. The heap moves between runs, so this is
    /// resolved once per session rather than written into a package as an address.
    std::optional<s64> FindEntryArray(const ArrayFind& spec, s64 value_off) const; // mod_state.cpp
    // mod_state.cpp
    std::optional<s64> SelectLiveInventory(const ArrayFind& spec, s64 value_off) const;
    std::optional<s64> FindPlayerNode(const PlayerFind& spec) const; // engine_mercury.cpp
    // mod_state.cpp
    VAddr HeapLow() const; ///< heap region begin, from the live page table (cached)
    // mod_state.cpp
    VAddr HeapHigh() const;                      ///< heap region end
    [[nodiscard]] bool PadIsPushed() const;      // mod_state.cpp
    [[nodiscard]] bool InGameplayHonest() const; // mod_state.cpp
    void DriveInputScript();                     // mod_actions.cpp
    void DriveLiveInput();                       // mod_actions.cpp
    void DriveCmd();                             // mod_runtime.cpp shell -> mod_console.cpp
    void DriveCmdImpl();                         // mod_console.cpp
    void DriveAutoChain();                       // mod_runtime.cpp shell -> mod_re_tools.cpp
    void DriveAutoChainImpl();                   // mod_re_tools.cpp
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    void ReloadManifest(); // mod_manifest.cpp; the console "reload"
#endif
    mutable std::map<std::pair<s64, s64>, s64> entry_array_cache;
    // Player-node identification: candidates of the right class holding world-scale coordinates,
    // scored by whether they move when the stick does.
    mutable std::vector<VAddr> player_at;
    mutable std::vector<float> player_last_x, player_last_y;
    mutable std::vector<s32> player_score;
    mutable std::vector<s32> player_hit_move;  // moved while the pad was pushed
    mutable std::vector<s32> player_hit_still; // stayed put while the pad was released
    /// Scratch for the per-tick candidate sample: each candidate's host pointer (the position's
    /// guest page resolved once per page), reused across ticks so the sample never allocates.
    mutable std::vector<const u8*> player_host;
    mutable VAddr player_found{0};
    mutable VAddr player_scan_cursor{0};
    mutable bool player_collected{false};
    mutable u64 player_samples{0};
    mutable u64 player_sampled_tick{0};
    /// Discovered heap region, cached on the first successful ask (any thread: HeapLow).
    mutable std::atomic<VAddr> heap_low{0};
    mutable std::atomic<VAddr> heap_high{0};
    mutable std::mutex heap_bounds_mutex;
    mutable u64 entry_array_diag_tick{0}; ///< throttle for the "array not found yet" NCE log
    mutable s64 nce_vtable_delta{0};      ///< data-segment shift of vtables under NCE, learned once
    mutable std::map<VAddr, u64> entry_inv_hash; ///< per-copy value-column hash, last scan
    mutable VAddr entry_inv_sel{0};              ///< currently selected inventory array
    mutable bool entry_inv_confirmed{false};     ///< selection proven live (its values changed)
    mutable VAddr entry_inv_cursor{0};           ///< sliced-sweep cursor across the heap
    mutable u64 entry_inv_swept_tick{0};   ///< last tick the sliced sweep advanced (per-tick guard)
    mutable u64 entry_inv_confirm_tick{0}; ///< last liveness-watchdog check of the confirmed copy
    mutable u64 entry_inv_confirm_hash{0}; ///< the confirmed copy's value hash at the last check
    mutable int entry_inv_frozen_checks{0}; ///< consecutive watchdog checks with no value change
    mutable std::map<VAddr, u64> entry_inv_scan_now; ///< candidate hashes accumulated this lap
    mutable VAddr entry_inv_first{0}; ///< first candidate seen this lap (provisional)
    mutable u64 player_moving{0};     // samples taken with the stick pushed
    mutable u64 player_still{0};      // samples taken with the stick released
    mutable u64 player_bad{0};        // consecutive frames the winner failed to follow the stick
    mutable float player_check_x{0.0f}, player_check_y{0.0f};
    // Where the incremental sweep for that array has got to, per (vtable, stride).
    mutable std::map<std::pair<s64, s64>, u64> entry_array_cursor;
    mutable std::map<std::pair<s64, s64>, u64> entry_array_swept_tick;
    u32 literal_slot{
        0}; ///< rolling slot for $"literal" call arguments staged below the borrowed thread's SP
    mutable std::unordered_map<const DataPoint*, VAddr> text_scan_cache;
    mutable std::unordered_map<const DataPoint*, u64> text_scan_attempt;
    [[nodiscard]] VAddr ScanForU32Text(const TextScan& spec) const; // mod_state.cpp
    // mgrfind: scan module BSS global slots for the save-data manager (or player) by stamina
    // signature.

    void FindValueCluster();      // mod_runtime.cpp shell -> mod_re_tools.cpp
    void FindValueClusterImpl();  // mod_re_tools.cpp
    void NarrowByAgreement();     // mod_runtime.cpp shell -> mod_re_tools.cpp
    void NarrowByAgreementImpl(); // mod_re_tools.cpp
    void DumpHeapSnapshot();      // mod_runtime.cpp shell -> mod_re_tools.cpp
    void DumpHeapSnapshotImpl();  // mod_re_tools.cpp
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    /// Reverse-engineering and search state (mod_re_tools.cpp, mod_console.cpp) and the headless
    /// input drivers' state (EDEN_DSMOD_AUTOSTART / _INPUT / _INPUT_LIVE, mod_actions.cpp). Only
    /// a dev-tools build has it: a release build carries none of this, and the code that reads
    /// it is compiled out with it.
    struct DevTools {
        /// EDEN_DSMOD_SCAN="<address-name>:<megabytes>" -- hunt a world position by behaviour rather
        /// than by structure. Anything that holds a coordinate swings while the character walks and
        /// settles when they stop, so collect every plausible float near a known object and watch
        /// which ones move.
        std::string scan_spec;
        std::vector<VAddr> scan_addresses;
        std::vector<const u8*> scan_host;
        std::vector<float> scan_low, scan_high;
        u64 scan_started{};
        float scan_min_magnitude{1.0f};
        VAddr scan_cursor{};
        VAddr scan_end{};
        std::string heapdump_path;
        bool heapdump_done{false};
        std::string scan_best;
        /// EDEN_DSMOD_FIND="a,b,c" -- look for a place in memory where those 32-bit values sit close
        /// together. Reading a game's numbers through its scripting bridge costs a guest call, which
        /// needs breakpoints, which need the JIT; finding the same numbers in memory costs nothing to
        /// read, updates the instant the game changes them, and works on a backend that runs guest
        /// code natively.
        /// EDEN_DSMOD_FIND="<value>" -- collect every address holding that 32-bit value, then keep
        /// only those that stop holding it once the game changes it. One pass leaves thousands of
        /// coincidences; the second leaves the one the player is looking at.
        std::string find_spec;
        std::vector<VAddr> find_candidates;
        std::vector<const u8*> find_host;
        s32 find_value{};
        bool find_float_only{false};
        std::string find_track;
        std::vector<std::string> find_others;
        s64 find_reach{0x40};
        std::string find_anchor;
        /// A published object address to work out a durable route to, named by the manifest. This is
        /// the step that turns "the player is at this address today" into something a package can
        /// ship: a static in the module plus a hop or two.
        std::string trace_target;
        bool trace_done{false};
        /// Recover the game's Lua registration table from the *running* module. The pointers that
        /// pair a name with its C function are relocated at load time, so they exist in memory and
        /// not in the file on disk -- which is why reading the NSO finds nothing.
        bool registry_done{false};
        bool describe_done{false};
        u64 find_window{0x400000};
        std::vector<s32> find_last;
        std::vector<int> find_changes;
        u64 find_started{};
        int find_round{};
        /// Locate a counter by spending it, without asking the game what it holds. The scripting
        /// bridge is stale between updates -- it can sit on six missiles while the screen shows none
        /// -- so it cannot say what the value is at the moment we look. What it can never be wrong
        /// about is what *we* just did: a counter we are spending goes down while we fire and holds
        /// while we rest, and nothing else in memory does both on cue.
        std::string diff_spec;
        std::string diff_anchor;
        u64 diff_window{0x10000};
        std::vector<VAddr> diff_candidates;
        std::vector<s32> diff_last;
        int diff_phase{0}; ///< 0 = not started, 1 = firing, 2 = resting
        u64 diff_started{};
        int diff_cycles{};
        /// Search the heap for literal bytes and report what sits around each hit. The module is a
        /// known quantity; the heap is where the game keeps what it is actually doing, and a table
        /// keyed by a hash gives itself away by holding that hash next to the number it maps to.
        std::string heapfind_spec;
        bool heapfind_done{false};
        /// "<value>@<offset>": find a float in the heap, step back by `offset` to the object that
        /// would contain it, and keep only the ones something actually points at. Aimed at a chain
        /// whose shape is known but whose root is not -- being pointed at is what separates a field
        /// of a live object from a number that happens to be lying there.
        std::string field_spec;
        bool field_done{false};
        /// Watch every heap float that could be a small whole-numbered counter, and report the ones
        /// whose value actually travels. Unlike the fire/rest test this never drops a candidate for
        /// failing a single phase -- a counter you can spend to zero stops moving once it is empty,
        /// and that is exactly when the strict test throws the answer away.
        std::string range_spec;
        int range_phase{0};
        u64 range_started{};
        std::vector<VAddr> range_at;
        std::vector<f32> range_min;
        std::vector<f32> range_max;
        /// Which whole values each candidate has been seen holding, as a bit per value 0..31. A
        /// counter descends through every step; memory the game cleared goes straight to nought.
        /// Counting the steps separates the two without assuming anything about the layout.
        std::vector<u32> range_seen;
        /// When set, only watch a value whose neighbour one word along holds this exact number.
        /// A counter and its capacity sit together, so "something, then fifteen" is a far stronger
        /// precondition than "something in a range" -- and it is a fact the HUD hands us.
        f32 range_pair_max{0.0f};
        /// Addresses already traced. Tracing once and latching meant the first shortlist -- three
        /// discarded allocations -- consumed the only chance, and the counter that appeared in a
        /// later report was never followed up.
        std::set<VAddr> range_traced;
        /// Dump every heap object whose vtable is a given module offset. Dread keeps each item's
        /// amount in an instance of one class -- the missile count and a stack of 99s were all found
        /// at +0x10 of main+0x1D4C700 -- so seeing what else those objects hold is what turns one
        /// address into a way of asking for any item by name.
        std::string classdump_spec;
        bool classdump_done{false};
        /// Find the inventory: a run of {vtable, tag, float} entries packed at a 0x18 stride. Each
        /// item's amount is one entry, which is why no two of them were ever neighbours -- energy
        /// sits exactly one entry before missiles, and aeion two before that.
        std::string arraydump_spec;
        bool arraydump_done{false};
        /// Find the player's position by walking. A coordinate rises while you hold right and falls
        /// while you hold left; nothing else in the heap agrees with the stick that consistently.
        /// Candidates are scored rather than dropped -- the fire/rest search failed because one bad
        /// phase discarded the answer permanently.
        std::string motion_spec;
        int motion_phase{0};
        u64 motion_started{};
        std::vector<VAddr> motion_at;
        std::vector<f32> motion_last;
        std::vector<s32> motion_score;
        std::vector<s8> motion_dir;    // sign of the last change, for counting reversals
        std::vector<s32> motion_turns; // how many times it changed direction
        VAddr motion_sweep_at{0};      ///< how far the collection pass has got
        bool motion_traced{false};
        mutable std::vector<PathRoute> path_routes;
        int path_checks{0};
        std::size_t motion_cursor{0};
        bool auto_start{false};
        std::vector<std::string> input_script;
        bool input_script_loaded{false};
        std::size_t input_step{0};
        u64 input_step_tick{0};
        u64 live_hold_left{0};
        int live_held_button{-1};
        bool live_held_stick{false};
        bool live_stick_right{false};
        float live_held_x{0.0f};
        float live_held_y{0.0f};
        VAddr cmd_watch{0};
        std::string
            cmd_action; ///< console "action <name>": a manifest action to run once on the next tick
        bool cmd_action_ingame{
            false}; ///< ... but not before the game is in play ("action <name> ingame")
        VAddr cmd_isnap_base{0};
        std::vector<u32> cmd_isnap_vals;
        VAddr cmd_scan_base{0};
        std::vector<u32> cmd_scan_snap;
        std::vector<u32> cmd_scan_cand;
        std::vector<VAddr> cmd_snap_addr;
        std::vector<float> cmd_snap_val;
        bool cmd_mgr_active{false};
        VAddr cmd_mgr_cursor{0};
        s32 cmd_mgr_target{0};
        std::vector<std::string> cmd_mgr_hits;
        bool cmd_find_active{false};
        u32 cmd_find_want{0};
        VAddr cmd_find_cursor{0};
        std::vector<VAddr> cmd_find_hits;
        std::string cmd_find_label;
        bool cmd_vfind_active{false};
        u32 cmd_vfind_want{0};
        VAddr cmd_vfind_cursor{0};
        std::vector<std::string> cmd_vfind_hits;
        bool cmd_ptr_active{false};
        u64 cmd_ptr_want{0};
        VAddr cmd_ptr_cursor{0};
        std::vector<std::string> cmd_ptr_hits;
        /// The ground truth as of the last sample, and how many times we have watched a candidate
        /// follow it from one value to another. A transition is the only evidence that separates a
        /// real counter from an address that merely happens to hold the same number.
        s32 find_last_truth{};
        bool find_truth_seen{false};
        int find_transitions{};
        /// Log every guest thread PC every 3s; set from EDEN_DSMOD_THREADS=1.
        bool log_guest_threads{false};
        /// EDEN_DSMOD_DUMP="<address-name>:<offset>:<bytes>" -- periodically log a block of guest
        /// memory hanging off an address a call sequence published. This is how a field nobody
        /// documented gets found: sample, move, sample again, and see which words changed.
        std::string dump_spec;
    };
    DevTools dev;
#endif
    // --- guest-call sequences (mod_guest_bridge.cpp) -----------------------------------------
    void StartSequence(const std::string& name, const CallSequence& sequence,
                       const StateSnapshot& snapshot);
    bool PrepareCallStep();
    [[nodiscard]] u64 ResolveCallArg(const std::string& arg);
    void RunPolledSequences(const StateSnapshot& snapshot);

    // mod_guest_bridge.cpp
    std::optional<u64> ScanRange(VAddr base, u64 size, const PatternFind& find) const;
    std::optional<s64> ScanPattern(const PatternFind& find) const; // mod_guest_bridge.cpp
    // engine_il2cpp.cpp
    std::optional<s64> FindClassSlotByName(const std::string& class_name) const;
    std::optional<s64> ReadIndexPoint(const std::string& name) const; // mod_state.cpp
    // mod_state.cpp
    std::optional<std::string> ReadPointText(const DataPoint& point, s64 array_index = 0) const;
    bool MethodTableLooksReal(VAddr table, s64 name_offset) const; // engine_il2cpp.cpp
    std::optional<s64> DetectMethodsOffset(VAddr klass) const;     // engine_il2cpp.cpp
    enum class MapFadeEndpoint : u8 { Animated, Previous, Current };
    /// Decodes and caches a widget's "src". Assets come either from the package itself or from
    /// the running game's romfs, so a package never has to ship the game's own art.
    std::shared_ptr<const Image> GetImage(
        const std::string& src,
        MapFadeEndpoint endpoint = MapFadeEndpoint::Animated); // mod_map.cpp
    std::vector<u8> ReadAssetBytes(const std::string& src);    // mod_assets.cpp

    // --- fog of war and map masks (mod_map.cpp) ----------------------------------------------
    /// Fog of war. MarkVisitedAt reveals the cells around a world position (and bumps the area's
    /// generation if anything new was revealed); VisitedGen feeds that generation into the map
    /// image cache key; MaskUnvisited blanks the not-yet-visited pixels of a rasterised area.
    void MarkVisitedAt(const std::string& area, float wx, float wy);
    u64 VisitedGen(const std::string& area) const;
    bool IsVisited(const std::string& area, float wx, float wy) const;
    /// `border_weight` (per pixel, 0..255, may be null) is the grey outline's share of the pixel,
    /// lerped in after the dim so the anti-aliased border never dims and fills stay flat.
    void MaskUnvisited(std::vector<u32>& pixels, const std::string& area, int width, int height,
                       const std::vector<u8>* border_weight, const std::vector<u8>* solid_color,
                       MapFadeEndpoint endpoint = MapFadeEndpoint::Animated) const;
    const std::vector<u8>& GeometryMask(const std::string& area); ///< which grid cells have map
    /// Cached mask of the cells belonging to one room_categories entry, keyed
    /// (area, category_id). Replaces the old SpecialRoomMask/TransportRoomMask/EmmyRoomMask (three
    /// near-identical accessors, one cache map each) with one generic accessor.
    const std::vector<u8>& RoomCategoryMask(const std::string& area,
                                            const std::string& category_id) const;
    // mod_redraw.cpp
    /// Generic colour-by-state resolution shared by MapLayer and MapRoomCategory (the
    /// color_bind/color_map primitive). Looks `color_bind` up in `tick_snapshot`
    /// (published points/derived values/module output, exactly like Widget's text_bind), and
    /// `color_map` by that resolved integer; falls back to `static_color` when `color_bind` is
    /// empty, unresolved, or has no matching `color_map` entry. Never called from a per-pixel loop.
    u32 ResolveBoundColor(
        u32 static_color, const std::string& color_bind,
        const std::shared_ptr<const std::unordered_map<s64, u32>>& color_map) const;
    /// The raw published value color_bind names (before the color_map lookup), or nullopt if the
    /// name is empty or unresolved. Also used to key the base-raster cache per live category state
    /// (one `id:value` per category with a non-empty color_bind, generalizing the old single
    /// hardcoded emmy_state cache-key int).
    std::optional<s64> LookupBoundInt(const std::string& name) const; // mod_redraw.cpp
    mutable std::map<std::string, std::vector<u8>> map_geometry;      ///< cached geometry masks
    mutable std::map<std::string, std::vector<u8>> map_category; ///< cached room-category masks
    /// Anti-aliased room silhouette per area@size (shared by the EMMI base variants): `coverage`
    /// is the geometry's area share of each pixel (the base image alpha), `weight` the grey
    /// border's share of that covered part (see BuildBorderCoverage). Keyed "<area>@<w>x<h>".
    struct BorderLayer {
        std::vector<u8> coverage;
        std::vector<u8> weight;
    };
    std::unordered_map<std::string, BorderLayer> map_border;
    mutable std::map<std::string, VisitedGrid> map_visited;

    /// Guards `map_visited`, `map_geometry`, and the whole
    /// module-supplied dynamic-map-state cluster below (`map_walls`, `map_occ_dead`,
    /// `map_vig_dispelled`, `map_door_open`, `map_item_picked/unveiled/veiled`, `wall_gen`,
    /// `water_gen`, `zone_gen`, `marker_gen`, `water_boxes`, `map_unlocked`, `map_zone_inactive`,
    /// `map_zone_alert`). One mutex, not several: every member here is read from the same
    /// `GetImage` map-rasterisation span and written from the same one or two module-callback
    /// functions (`AcceptModuleMap`/`AcceptModuleMapState`, mod_module_host.cpp) or
    /// `MarkVisitedAt`/ `UpdateHiddenMarkers` -- splitting these across several locks would add
    /// lock-ordering risk for no isolation benefit. `std::recursive_mutex`, deliberately, not
    /// `std::mutex`: `MarkVisitedAt` calls `GeometryMask` and `AcceptModuleMapState` calls
    /// `UpdateHiddenMarkers` while already holding it -- rather than hand-verifying every call path
    /// never re-enters (the easy class of mistake to make), same-thread re-entry is made safe by
    /// construction. With `GetImage`/`RenderPageTo` on the redraw worker,
    /// `AcceptModuleMap`/`AcceptModuleMapState`/ `UpdateHiddenMarkers` (tick thread, via
    /// `RunGameModule` during sampling) are genuine concurrent writers against the worker's reads.
    /// `MarkVisitedAt`/`GeometryMask`/`IsVisited`/ `VisitedGen`/`StampFor`/ `UpdateHiddenMarkers`
    /// each take only their own short critical section. `GetImage`'s "map:" composite-building span
    /// is the one deliberate exception: it holds this lock for the WHOLE span
    /// (base/prefog/magnet/pulse build, ~2-3ms of real rasterisation work), not a series of short
    /// per-touch-point copies: a single, provably-sufficient lock is safer than several
    /// hand-derived short critical sections whose correctness depends on nothing downstream of
    /// each copy re-reading the live map. Known perf cost: a module map update landing
    /// mid-rasterisation stalls the tick thread for that span's cost.
    mutable std::recursive_mutex map_state_mutex;

    // Generic dynamic map state supplied by an optional installed title module.
    struct WallTile {
        float x{}, y{};
        u32 type{}, color{};
        bool operator==(const WallTile&) const = default;
    };
    mutable std::map<std::string, std::vector<WallTile>> map_walls;
    mutable std::map<std::string, std::set<std::string>> map_occ_dead;
    mutable std::map<std::string, std::set<std::string>> map_vig_dispelled;
    mutable std::map<std::string, std::set<std::string>> map_door_open;
    mutable std::map<std::string, std::set<std::string>> map_item_picked;
    mutable std::map<std::string, std::set<std::string>> map_item_unveiled;
    mutable std::map<std::string, std::set<std::string>> map_item_veiled;
    mutable u64 wall_gen{}, marker_gen{}, water_gen{};
    u64 zone_gen{};
    bool map_unlocked{}, map_zone_inactive{}, map_zone_alert{};
    mutable std::set<std::string> game_vis_areas;
    mutable std::vector<std::array<float, 4>> water_boxes;
    std::string live_scenario;
    s64 last_in_game{-1};
    void UpdateHiddenMarkers(const std::string& area); // mod_map.cpp

    /// Nonzero while GetImage's "map:" raster holds map_state_mutex (set before the lock, cleared
    /// after it). That span runs 25-250 ms on a desktop per reveal (both fade endpoints), several
    /// times that on a handheld, on the redraw worker. The tick thread runs on the core-timing
    /// thread that also fires the guest's vsync, so it must never sit behind it: its map_state
    /// users defer instead (LockMapStateUnlessRaster).
    std::atomic<int> map_raster_busy{0};
    using MapStateLock = std::unique_lock<std::remove_cvref_t<decltype(map_state_mutex)>>;
    /// Takes `lock` (deferred, on map_state_mutex) unless a raster holds the mutex: waits out
    /// short holds (spinning with a yield, at most 250 us), returns false without the lock while
    /// a raster is in progress or the holder outlasts that.
    bool LockMapStateUnlessRaster(MapStateLock& lock) const;
    /// One module map publication (AcceptModuleMap / AcceptModuleMapState), copied out of the
    /// module's buffers so it can be applied after the call returns. Tick thread only.
    struct PendingMapUpdate {
        bool is_state{};
        std::string area;
        u64 tick{}; ///< tick_count on arrival: the reveal/change stamps it would have had
        bool has_visibility{};
        std::vector<u8> visibility;
        std::vector<u8> visibility_previous;     ///< empty: none supplied
        std::vector<u32> visibility_change_ticks; ///< empty: none supplied
        bool has_water{};
        std::vector<std::array<float, 4>> water;
        bool has_walls{};
        std::vector<WallTile> walls;
        bool has_zone{};
        bool unlocked{}, inactive{}, alert{};
        std::array<std::set<std::string>, 6> state_sets; ///< is_state: the six actor sets
    };
    /// Publications not yet applied because a raster held map_state_mutex, in arrival order.
    std::deque<PendingMapUpdate> pending_map_updates;
    void ApplyPendingMapUpdates();                           // mod_module_host.cpp
    void ApplyMapUpdateLocked(const PendingMapUpdate& update); // mod_module_host.cpp

    std::unique_ptr<GameModule> game_module;
    void* game_module_instance{};
    EdenDsmodHostApi module_host{};
    /// module_host for callbacks off the tick thread (load_image, load_data): no publishing, no
    /// snapshot reads (InitializeGameModule, mod_module_host.cpp).
    EdenDsmodHostApi module_worker_host{};
    /// nce_vtable_delta as of the last module call, for module_worker_host's "__relocation_delta".
    std::atomic<s64> worker_relocation_delta{0};
    EdenDsmodHostExtensions module_extensions{};
    EdenDsmodHostSaveApi module_save_api{};
    EdenDsmodHostWriteApi module_write_api{};
    std::mutex module_save_mutex;
    FileSys::VirtualDir module_save_dir; // lazily resolved, guarded by module_save_mutex
    std::jthread module_asset_worker;
    std::mutex module_asset_mutex;
    std::condition_variable_any module_asset_cv;
    std::deque<std::string> module_asset_queue;
    std::unordered_set<std::string> module_asset_pending;
    /// Module image keys whose decode failed: when and how often. A module may read the running
    /// game to build an image (sprite tables resolved from main), which can fail before
    /// the game is up, so a failed key is retried with a growing delay, a bounded number of times.
    struct ModuleAssetFailure {
        std::chrono::steady_clock::time_point at;
        u32 attempts{};
    };
    std::unordered_map<std::string, ModuleAssetFailure> module_asset_failed;
    std::unordered_map<std::string, Image> module_asset_completed;
    /// EDEN_DSMOD_IMAGE_TIMING=1: when each module image was queued / decoded (steady clock), for
    /// the per-image "landed" log line in DrainModuleImages. Guarded by module_asset_mutex.
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point,
                                              std::chrono::steady_clock::time_point>>
        module_asset_times;
    std::unordered_map<std::string, u64> module_asset_used;
    size_t module_asset_bytes{};
    std::mutex module_romfs_mutex;
    /// Serializes every call into the module's load_image (the module asset worker and the Nx
    /// worker's "module:" composite layers) with module shutdown.
    std::mutex module_loader_mutex;
    /// Set when module images land: the next UI draw repaints the whole page. A static image
    /// widget's dependency hash does not change when its asynchronously built picture arrives, so
    /// a partial redraw (a spinning widget elsewhere keeps one going every tick) would never paint
    /// it.
    std::atomic<bool> module_images_landed{false};
    /// Runtime 12 module data ("module:" byte sources): every load_data call, serialized, and
    /// held by module shutdown before the instance goes (separate from module_loader_mutex so a
    /// long generation never blocks module images).
    std::mutex module_data_mutex;
    /// map.areas_src: fetched and parsed off the tick thread (module_areas_thread), installed once
    /// by the tick thread (InstallModuleAreas).
    std::jthread module_areas_thread;
    std::mutex module_areas_mutex;
    std::optional<std::unordered_map<std::string, MapArea>> module_areas_result; ///< areas_mutex
    std::atomic<bool> module_areas_ready{false};
    u32 module_areas_busy_ticks{}; ///< ticks InstallModuleAreas found the redraw worker busy
    StateSnapshot* module_snapshot{}; // borrowed only during a synchronous module callback
    std::string module_error;
    void InitializeGameModule();                                         // mod_module_host.cpp
    void ShutdownGameModule();                                           // mod_module_host.cpp
    void RunGameModule(StateSnapshot& snapshot, bool tick);              // mod_module_host.cpp
    /// Whether the module is ticked while the second screen is hidden: manifest
    /// "module_tick_hidden", else the module's TICK_WHEN_HIDDEN / NO_TICK_WHEN_HIDDEN flag, else
    /// "exports on_action" (the behaviour before the explicit switches existed).
    [[nodiscard]] bool ModuleTicksWhileHidden() const; // mod_module_host.cpp
    /// A module's get_i64("__source:<prefix>") (either host copy); nullopt: not answered.
    static std::optional<s64> SourceQuery(ModRuntime& rt, const char* name); // mod_module_host.cpp
    void InitializeModuleExtensions();                                   // mod_module_services.cpp
    void InitializeModuleSaveExtensions();                               // mod_module_services.cpp
    void InitializeModuleWriteExtensions();                              // mod_module_services.cpp
    void StartModuleAssetWorker();                                       // mod_module_services.cpp
    void DrainModuleImages();                                            // mod_module_services.cpp
    void StartModuleAreas();                                             // mod_module_services.cpp
    void InstallModuleAreas();                                           // mod_module_services.cpp
public:
    /// A "module:" byte source through the module's data extension (any thread; serialized).
    /// Empty when no module serves it.
    std::vector<u8> LoadModuleData(const std::string& key); // mod_module_services.cpp
private:
    std::shared_ptr<const Image> GetModuleImage(const std::string& key); // mod_module_services.cpp
public:
    /// Synchronous module image (any thread; serialized): composite layers with a "module:"
    /// source (asset-free packages build every layer in the module). False when no module serves
    /// it.
    bool LoadModuleImageSync(const std::string& key, Image& out); // mod_module_services.cpp
private:
    void RunModuleAction(const std::string& name, s64 argument); // mod_module_services.cpp
    void AcceptModuleMap(const EdenDsmodMapFrame& frame);        // mod_module_host.cpp
    void AcceptModuleMapState(const char* json);                 // mod_module_host.cpp


    /// Named runtime flags (ints: 0/1, or a small multi-state value) a mod sets from a button;
    /// enforce rules read them as non-zero, pages as "@flag:<name>".
    std::unordered_map<std::string, s64> flags;
    /// The manifest's "persist_flags": restored in Initialize, saved by flag actions.
    FlagPersistence flag_persistence;
    std::unordered_map<std::string, VAddr> method_info_cache;
    /// Values are
    /// `shared_ptr<const Image>`, not `Image` by value, so a caller that has already read a pointer
    /// out of this cache keeps a live, valid object even if a concurrent eviction sweep
    /// (`GetImage`'s own several `erase()` loops) or `insert`/`insert_or_assign` rehashes the map
    /// out from under it. Modeled directly on `NxAssetState::fonts`/`bntxs` (mod_nx_runtime.cpp),
    /// the existing precedent for "a worker thread and a tick-thread caller sharing a cache of
    /// decoded assets". Every read/insert/erase of this map (and of `module_asset_completed`, which
    /// also feeds `GetModuleImage`) must hold `asset_cache_mutex` -- see
    /// `CacheFindImage`/`CachePutImage`/ `CacheEraseImagesIf` below, the only sanctioned way to
    /// touch it.
    std::unordered_map<std::string, std::shared_ptr<const Image>> image_cache;
    /// Guards `image_cache` (every find/insert/erase, including the eviction sweeps inside
    /// `GetImage`) and `module_asset_completed` (the other map `GetModuleImage` reads/writes, same
    /// hazard shape -- a worker thread's read racing `DrainModuleImages`'s tick-thread insert).
    /// Contended once the redraw worker calls GetImage while the tick thread runs
    /// PumpNxAssets/DrainModuleImages.
    mutable std::mutex asset_cache_mutex;
    // --- image cache accessors (mod_assets.cpp) ----------------------------------------------
    /// The only sanctioned way to read `image_cache`: returns a live, ref-counted copy (or
    /// nullptr), never a raw pointer into the map. Safe to hold across any amount of caller-side
    /// computation.
    std::shared_ptr<const Image> CacheFindImage(const std::string& key) const;
    /// The only sanctioned way to insert into `image_cache`. Builds the `shared_ptr` OUTSIDE the
    /// lock (the `Image` move/construction itself needs no synchronization), then locks only for
    /// the map mutation -- keeps the critical section to "one map insert", not "however long
    /// building this Image took". `insert_or_assign` (not `emplace`): every call site in `GetImage`
    /// already only reaches an insert after confirming the key absent, so the two are behaviourally
    /// identical here, but `insert_or_assign` doesn't silently keep a stale entry if that invariant
    /// ever slips.
    std::shared_ptr<const Image> CachePutImage(const std::string& key, Image&& image);
    /// The only sanctioned way to run one of `GetImage`'s cache-eviction sweeps. `should_erase` is
    /// called only with each entry's KEY (every existing eviction predicate in `GetImage` is
    /// key-prefix-based; none inspects the cached `Image` itself) -- held under the lock for the
    /// whole sweep, same as today's unlocked loops, so this changes nothing about eviction *cost*,
    /// only makes it safe to run concurrently with a reader/inserter on another thread.
    void CacheEraseImagesIf(const std::function<bool(const std::string& key)>& should_erase);
    /// Alpha-1 water pixels keyed exactly like their prefog image. Kept separately because the
    /// room silhouette alpha cannot encode the shader's solid-colour classification.
    std::unordered_map<std::string, std::vector<u8>> map_water_solid;
    /// Static water geometry keyed by area and raster size. A drain changes only the live clip
    /// boxes; retaining the geometry pixels avoids rebuilding a full scratch canvas and scanning
    /// every map pixel on the render thread for each water generation.
    struct WaterPixel {
        u32 index{};
        u32 color{};
        float x{}, y{};
    };
    std::unordered_map<std::string, std::vector<WaterPixel>> map_water_pixels;
    std::unordered_set<std::string> image_failed;
    /// Every "<prefix>:" asset source (mod_sources.h): registered once in the constructor
    /// (RegisterAssetSources, mod_assets.cpp), then read-only and safe from any thread. Shared
    /// with the Nx asset worker's jobs, which may outlive a reload of the Nx state.
    std::shared_ptr<AssetSources> asset_sources{std::make_shared<AssetSources>()};
    /// The runtime's own sources: "file:" (the package folder), "romfs:" (the running game's
    /// patched romfs, opened on first use through a private storage chain) and "module:" (the
    /// module data extension). Further sources register here too.
    void RegisterAssetSources(); // mod_assets.cpp
    // --- Nintendo assets, composites and msbt (mod_nx_runtime.cpp) ---------------------------
    // Nintendo asset references ("romfs:/x.arc#member#texture", .bntx, .bffnt) and
    // "composite:<name>" images, decoded off the render path (mod_nx_runtime.cpp).
    std::unique_ptr<NxAssetState, NxAssetStateDeleter> nx_assets{MakeNxAssetState(this)};
    [[nodiscard]] bool IsNxAssetSource(const std::string& src) const;
    std::shared_ptr<const Image> GetNxImage(const std::string& src);
    std::shared_ptr<const Image> GetCompositeImage(const std::string& name);
    /// Sync: the bytes of a SARC member ("<file>#<member>[#...]"); false when `src` does not
    /// start with a SARC (the caller then tries the other archive kinds).
    bool ReadNxMember(const std::string& src, std::vector<u8>& out);
    /// Once per tick before drawing: land finished decodes, drive composites and their fades.
    void PumpNxAssets(const StateSnapshot& snapshot);
    /// Publishes "@fade:<name>" (0..1000) for every flat composite cross-fading right now.
    void PublishNxFades(StateSnapshot& snapshot);
    /// Bumped whenever a composite picture lands (the GPU path re-uploads its map then).
    /// Written on the tick thread (`PumpNxAssets`, every tick) but also read from the redraw
    /// worker (`PublishGpuComposite`, once GPU_COMPOSITE pages dispatch), so a plain `u64` would be
    /// an unguarded cross-thread race. A monotonic counter compared only for inequality needs no
    /// more than this.
    std::atomic<u64> composite_epoch{0};
    u64 last_map_composite_epoch{0};
    /// Bumped whenever ANY asset lands (image, composite, font, msbt) -- PumpNxAssets already
    /// knew this as its local "landed" flag; this just keeps a running count of it so a page
    /// transition's one-time target-page snapshot (DrivePageTransition) can tell "something a
    /// widget on this page asked for has since arrived" apart from "nothing changed", which its
    /// existing sig-based redraw trigger cannot see (UiSignature does not depend on asset state).
    /// Fixes the first-open snapshot race: a page's composite requested by its own first render
    /// (e.g. Gear's cell_disc discs) decodes a few ms later, off the render path; without this the
    /// transition's cached snapshot never learns it landed and every transition frame -- and the
    /// settled frame right after -- draws without it.
    u64 asset_epoch{0};
    u32 last_composite_weight{255};
    /// A flat composite's cross-fade progress, 0..1 (1 = settled; also ends a finished fade).
    float CompositeFade(const std::string& name);
    void ResetNxAssets();
    bool RequestNxFont();
    /// Console "imgdump <src> <out.png>". Defined only in a dev-tools build.
    void NxImageDump(const std::string& args);
    [[nodiscard]] bool NxFallback(const std::string& src) const;
    // "msbt:<alias>#<label>" text (mod_nx_runtime.cpp): the running game's own message files in
    // the language the game uses, decoded on the asset worker.
    /// The decoded text, or nullptr until it has landed (then the page redraws by itself).
    std::shared_ptr<const std::string> GetMsbtText(const std::string& ref);
    /// Once per session: settings language -> the language the game is given -> msbt_lang.
    void ResolveMsbtLanguage();
    void RequestMsbt(const std::string& alias);
    /// A worker result: installs the texts, or switches the session to the fallback language.
    bool AcceptMsbt(const MsbtResult& result);
    /// The inline-icon font once loaded (nullptr before); `pending` while it is loading.
    // `metrics_copy`, when non-null, is filled with an owned copy of the resolved
    // metrics (see mod_nx_runtime.cpp's own comment -- NxAssetState is an incomplete type outside
    // that file, so this is the sanctioned way a caller elsewhere gets a race-free copy instead of
    // the raw pointer this function still also returns for same-tick synchronous use).
    const FontMetrics* NxIconFont(std::shared_ptr<const Image>& atlas, bool& pending,
                                  FontMetrics* metrics_copy = nullptr);
    /// Console "msbt [<alias>#<label>]": the language decision, or one decoded text. Defined
    /// only in a dev-tools build.
    void MsbtConsole(const std::string& args);
    /// "@last:<group>": the latest selected / dragged payload per group (reset on page change).
    std::unordered_map<std::string, s64> last_selection;
    mutable s64 detected_methods_offset{0};
    mutable int index_depth{0};
    std::unordered_map<std::string, u64> spy_values;
    std::unordered_map<std::string, VAddr> spy_armed;
    std::unordered_map<VAddr, u32> spy_original;
    /// What each patched address really held, keyed by address. A spy and a call hook can want
    /// the same function -- lua_pcall is both the best place to watch and the best place to
    /// borrow a thread -- and without this the second patch records the first one's brk as the
    /// "original", then writes that brk back into the game's code for good.
    std::unordered_map<VAddr, u32> patched_original;
    bool patches_applied{false};
    std::vector<bool> patch_done; ///< per manifest.patches entry: written (or skipped) already
    mutable std::unordered_map<std::string, s64> pattern_cache;
    mutable std::unordered_map<std::string, s64> class_slot_cache;
    /// Where a class' name string was found (the IL2CPP metadata does not move), and
    /// the tick of the last failed slot lookup -- a class is created lazily, and retrying the
    /// whole-module walk on every point read cost Hollow Knight 60-90 ms a tick until it existed.
    mutable std::unordered_map<std::string, VAddr> class_name_address_cache;
    mutable std::unordered_map<std::string, u64> class_slot_miss_tick;
    mutable std::unordered_map<std::string, u64> class_slot_cursor;     ///< sliced retry lap
    mutable std::unordered_map<std::string, u64> class_slot_slice_tick; ///< one slice per tick
    mutable s64 detected_method_name_offset{0};
    // --- guest-memory sampling (mod_state.cpp) -----------------------------------------------
    void SampleState(StateSnapshot& out);
    bool ResolvePoint(const DataPoint& point, VAddr& address_out, s64 array_index = 0) const;
    bool ReadPoint(const DataPoint& point, s64& value_out, s64 array_index = 0) const;
    /// Follow an intrusive circular list to element `index`, yielding the object
    /// around the node rather than the node itself.
    bool WalkList(VAddr list, const ChainHop& hop, s64 index, VAddr& out) const;
    void DrainTaps(const StateSnapshot& snapshot); // mod_input.cpp
    /// What an action did: its work (Done), nothing because its enabled_bind gate or a full slot
    /// refused it (Refused), or nothing because a value / point / write was missing (Skipped).
    enum class ActionResult : u8 { Done, Refused, Skipped };
    /// `payload`: the dragged or selected widget's value, for actions that write "$payload".
    /// `tap_origin`: the rect (canvas px) of the widget a tap/drag/marker ran this action from,
    /// when there was one -- a page action's Grow/Shrink transition defaults to it.
    ActionResult RunAction(const Action& action, const StateSnapshot& snapshot,
                           std::optional<s64> payload = std::nullopt,
                           const std::array<s32, 4>* tap_origin = nullptr); // mod_actions.cpp
    // mod_actions.cpp
    /// The rect of the widget named `id` on `page_index`, expanded (so {i}/x_bind resolved),
    /// or nullopt when the page/id doesn't resolve to one. Used to resolve a page action's
    /// explicit "origin" (checked on the destination page first, then the source page) and a
    /// group anim's "from": "widget:<id>".
    std::optional<std::array<s32, 4>> FindWidgetRect(size_t page_index, const std::string& id,
                                                     const StateSnapshot& snapshot) const;

    // --- haptic feedback (mod_input.cpp) -----------------------------------------------------
    /// The strongest haptic asked for this tick; played once by FlushHaptic.
    struct PendingHaptic {
        HapticStrength strength{HapticStrength::Off};
        HapticKind kind{HapticKind::Tap};
        std::string source;
    };
    PendingHaptic pending_haptic;
    /// Asks for a haptic of `kind` (manifest default, overridden by the action, then the widget).
    void QueueHaptic(HapticKind kind, HapticOverride widget_haptic, HapticOverride action_haptic,
                     std::string_view source);
    /// Hands this tick's haptic to the frontend (AuxRouting's notifier thread) and logs it.
    void FlushHaptic();
    /// The haptic kind of a tap-run action that did its work: a game-memory write or a plain tap.
    [[nodiscard]] static HapticKind TapHapticKind(const Action& action);

    // --- animations (grow/shrink + first-open snapshot fix) ----------------------------------
    /// A page switch asked to animate (RunAction), started by the next publish.
    struct PageAnimRequest {
        size_t from{};
        size_t to{};
        PageTransition kind{PageTransition::None};
        u32 duration_ms{};
        Easing easing{Easing::EaseOut};
        float shadow{};
        /// Grow/Shrink only: canvas-pixel rect of the small end of the scale ({0,0,0,0} =
        /// unresolved -- DrivePageTransition falls back to the canvas centre).
        std::array<s32, 4> origin{};
    };
    std::optional<PageAnimRequest> page_anim_request;
    struct PageAnim {
        bool active{false};
        PageAnimRequest req;
        std::chrono::steady_clock::time_point start;
        u64 to_sig{};
        /// asset_epoch as of the last render of the target page: a pending image/composite
        /// landing mid-transition bumps asset_epoch without changing to_sig, so this is checked
        /// alongside it to catch the case sig alone misses (the fix for the first-open snapshot
        /// race -- see DrivePageTransition).
        u64 to_assets_epoch{};
        u32 frames{};
        double compose_ms{};
        double compose_max_ms{};
        double start_ms{}; ///< rendering both pages at the start
        u32 redraws{};     ///< target page re-rendered mid-transition
    };
    PageAnim page_anim;
    std::vector<u32> anim_from_px; ///< the page being left, frozen
    /// One animated widget group of the current page.
    struct GroupDef {
        std::shared_ptr<const WidgetAnim> anim;
        std::array<s32, 4> box{};
        /// From::Widget only: the origin widget's rect at page-load time ({0,0,0,0} = not found,
        /// falls back to the box itself -- no visible grow/shrink, just an instant appear).
        std::array<s32, 4> origin_box{};
    };
    std::vector<GroupDef> group_defs;
    size_t group_defs_page{~size_t{0}};
    struct GroupAnim {
        bool open{false};
        float pos{0.0f}; ///< 0 = hidden, 1 = shown (linear progress; eased when drawn)
        float start_pos{0.0f};
        bool moving{false};
        bool settle_pending{false}; ///< reached its end: one more frame of its box is due
        std::chrono::steady_clock::time_point start;
        u32 frames{};
        double frame_ms{};
        double frame_max_ms{};
    };
    std::map<std::string, GroupAnim> group_anims;
    bool group_moving{false}; ///< any group of the current page is mid-animation
    RenderExtras render_extras;
    /// What the canvas holds: the last full frame of this page (npos = nothing reusable) and
    /// whether it is the GPU path's HUD-only overlay.
    size_t canvas_page{~size_t{0}};
    bool canvas_hud{false};
    u32 anim_dump_seq{0};
    /// Advances every group of the current page (snapping them on a page change).
    void UpdateGroupAnims(const StateSnapshot& snapshot); // mod_pages.cpp
    /// Fills render_extras for this frame; returns the union of the boxes that need a redraw.
    /// `widgets_may_be_dirty`: true only on a tick where UiSignature already changed -- gates the
    /// per-widget scan; the group-box union always runs. `target_w`/`target_h`: canvas size, for
    /// the area-cutover check and the asset-epoch full-redraw rect.
    std::array<s32, 4> BuildRenderExtras(const StateSnapshot& snapshot, bool widgets_may_be_dirty,
                                         u32 target_w, u32 target_h); // mod_redraw.cpp
    // mod_pages.cpp
    /// Starts / advances a page transition; true when it published this tick's frame.
    bool DrivePageTransition(const StateSnapshot& snapshot, u64 sig, u32 target_w, u32 target_h);
    // mod_redraw.cpp
    /// Renders one page into the canvas with the runtime's providers (CPU unless `dl`).
    bool RenderPageTo(const Page& page, const StateSnapshot& snapshot, AuxDrawList* dl,
                      bool* animating, MapDrawRecords* records, const RenderExtras* extras);
    // mod_redraw.cpp
    /// RenderPageTo once per disjoint dirty rect of `extras` (RenderExtras::clips), else once.
    bool RenderPageRects(const Page& page, const StateSnapshot& snapshot, AuxDrawList* dl,
                         bool* animating, MapDrawRecords* records, const RenderExtras* extras);
    /// PublishUiPartial of `from` for each dirty rect of `extras` (or its one clip).
    static void PublishPartial(VideoCore::DSMod::AuxRouting& aux, const Canvas& from,
                               const RenderExtras& extras); // mod_redraw.cpp
    /// Drops every animation (mirror / capture pages, reload).
    void CancelAnimations(const char* why); // mod_pages.cpp
    // mod_pages.cpp
    /// Test hook EDEN_DSMOD_ANIM_DUMP=<dir>: writes a published animation frame as a PNG.
    void DumpAnimFrame(std::span<const u32> pixels, u32 w, u32 h, const std::string& label);
    /// Test hook EDEN_DSMOD_ANIM_SCALE=<f>: every animation lasts f times longer.
    [[nodiscard]] static float AnimTimeScale(); // mod_pages.cpp

    // --- automatic page switching ("page_binds"; mod_pages.cpp) ------------------------------
    /// Per-bind tracking, parallel to manifest.page_binds (resized/reset lazily so a manifest
    /// reload with a different bind list can't read a stale slot).
    struct PageBindState {
        /// False until `point` (and `ready`, when given) has read a valid value at least once.
        /// Any tick where either is unreadable drops back to false: this is what keeps a bind
        /// from ever firing on boot/menu garbage, and makes a
        /// mid-session save reload (quit to title, load a different file) re-baseline cleanly
        /// instead of comparing against the old file's last value.
        bool armed{false};
        bool last_equal{false}; ///< the branch as of the last time tracking was updated
        bool pending{false};    ///< an edge fired and is waiting out a deferral (see below)
        bool pending_equal{false};
    };
    std::vector<PageBindState> page_bind_state;
    /// Evaluates every manifest.page_binds entry once this tick's snapshot (points, derived,
    /// flags, interaction state) is complete: arms, edge-detects, and fires the matching branch's
    /// page switch (as RunAction would for a tapped `page` action, with no tap origin) -- deferred
    /// while a finger is down on the second screen, dropped (not queued) while the current page is
    /// `no_auto_leave`.
    void DrivePageBinds(const StateSnapshot& snapshot);

    // --- input blocking -----------------------------------------------------------------------
    bool gesture_blocked{false}; ///< the gesture started on an input_block widget
    bool gesture_ignored{false}; ///< the gesture started during a page transition
    /// The topmost visible input_block widget at a point that is above every draggable and
    /// pannable widget there (expanded index), or -1.
    s64 InputBlockAt(const std::vector<Widget>& expanded, const StateSnapshot& snapshot, s32 x,
                     s32 y) const; // mod_input.cpp
    /// Writes a point honouring its shift/mask modifiers (only those bits change). `array_index`
    /// = element of a `count` point; `as_float` = write IEEE bits into an f32 point.
    bool WritePointValue(const DataPoint& point, s64 value, s64 array_index = 0,
                         std::optional<f64> as_float = std::nullopt); // mod_state.cpp
    /// One guest store: `bits` (already shifted into place) into the `field` bits of the `width`
    /// byte word at `address` (field all-ones = a whole-word store, else read-modify-write).
    struct GuestStore {
        VAddr address{};
        u32 width{};
        u64 bits{};
        u64 field{~u64{0}};
    };
    /// A check a batch makes before storing: the word's `field` bits still equal `bits`.
    struct GuestExpect {
        VAddr address{};
        u32 width{};
        u64 bits{};
        u64 field{~u64{0}};
    };
    // --- guest write batches (mod_state.cpp) --------------------------------------------------
    /// The store WritePointValue would make, resolved now; nullopt when the point cannot be
    /// written (unresolved, unmapped, read-only type).
    std::optional<GuestStore> PlanPointWrite(const DataPoint& point, s64 value,
                                             s64 array_index = 0,
                                             std::optional<f64> as_float = std::nullopt) const;
    /// "This point's word must still hold what it holds now."
    std::optional<GuestExpect> ExpectUnchanged(const DataPoint& point, s64 array_index = 0) const;
    /// Runs `fn` with no guest thread running (single core: directly). False when the application
    /// could not be stalled right now (pause/resume in progress); `fn` did not run then.
    bool RunWithGuestStopped(const std::function<void()>& fn);
    /// Checks every expect, then applies every store, as one unit with the guest stopped (a lone
    /// whole-word store is written directly). False when an expect failed: nothing was written.
    bool ApplyGuestStores(std::span<const GuestExpect> expects, std::span<const GuestStore> stores);
    /// A value an action names: an int, and the float it came from when the source is a float.
    struct ActionScalar {
        s64 i{};
        std::optional<f64> f;
    };
    // mod_actions.cpp
    /// "$name" sources of actions: payload, the map-tap context, then live runtime state
    /// (@flag:, @map_sel:, @sel:, @last:), then the snapshot.
    std::optional<ActionScalar> ResolveActionRef(const std::string& ref,
                                                 const StateSnapshot& snapshot,
                                                 std::optional<s64> payload) const;
    // mod_actions.cpp
    /// Replaces every "{$name}" in a point name by that value (nullopt when one is missing or
    /// negative).
    std::optional<std::string> ExpandActionName(const std::string& text,
                                                const StateSnapshot& snapshot,
                                                std::optional<s64> payload) const;
    // mod_actions.cpp
    /// "<array><i>" -> element i of the `count` point <array> (an exact point name wins).
    const DataPoint* ResolveElementPoint(const std::string& name, s64& index) const;
    /// What a map tap hands its action ($map_x, $map_y, $world_x, $world_y, $marker_index).
    struct MapTapContext {
        bool active{false};
        f64 map_x{}, map_y{}, world_x{}, world_y{};
        s64 marker_index{-1};
        std::string marker_group;
    };
    MapTapContext map_tap_ctx;
    /// What every Map widget of the current page showed on its LATEST draw call -- the buffer
    /// RenderPage/RenderPageTo write into directly (cleared, then repopulated, every call). Owned
    /// by whichever thread calls RenderPageTo (today: the tick thread; a later step: the redraw
    /// worker) -- never read from anywhere else. Not guarded by map_records_mutex; see
    /// map_draw_records_published below for the guarded, cross-phase-safe copy every reader outside
    /// RenderPageTo's own call must use instead.
    MapDrawRecords map_draw_records;
    /// The published copy `DrainTaps` (this tick's own
    /// tap hit-testing, reading what the *previous* successful RenderPageTo call drew -- a designed
    /// one-tick-delayed cross-phase hand-off, not a bug to remove) and the console `mapinfo`
    /// command actually read, alongside `map_records_page` (which page it's for) -- both guarded
    /// together by `map_records_mutex` since they are only ever meaningful as a pair. Updated by a
    /// short copy-out (map records are dozens of hits, cheap to copy wholesale) immediately after
    /// each of RenderPageTo's three call sites succeeds, still on whichever thread made that call
    /// -- a genuine cross-thread copy when RenderPageTo runs on the redraw worker while
    /// DrainTaps/mapinfo stay on the tick thread.
    MapDrawRecords map_draw_records_published;
    mutable std::mutex map_records_mutex;
    size_t map_records_page{~size_t{0}};
    /// Selected dynamic marker per group ("@map_sel:<group>").
    std::unordered_map<std::string, s64> map_selections;
    /// Tick each selection was made: the snapshot of that tick predates a write that made the slot.
    std::unordered_map<std::string, u64> map_selection_tick;
    std::set<std::string> map_groups; ///< every dynamic marker group of the package
    bool map_groups_ready{false};
    std::optional<std::pair<f64, f64>> last_map_tap; ///< world position of the latest map tap
    s64 map_tap_seq{0}; ///< bumped on every map tap, so readers can tell a new tap from the last
    /// Writes @map_tap_x/_y (float and int) and @map_tap_seq for the latest map tap.
    void PublishMapTap(StateSnapshot& snapshot) const;
    /// Publishes @map_sel*, @map_tap_*, @slot_used/@slot_full (and @flag:* when `with_flags`);
    /// drops stale selections.
    void PublishMapState(StateSnapshot& snapshot, bool with_flags = true); // mod_input.cpp
    /// Derived points: evaluated after sampling, published into ints and floats.
    /// `volatile_only`: the second, post-interaction evaluation of a tick. Only derived values that
    /// (transitively) read a key written between the two passes ("@..." interaction/flag state,
    /// "view_custom:...") are recomputed; every other one would republish its first-pass value.
    void EvaluateDerived(StateSnapshot& snapshot, bool volatile_only = false); // mod_state.cpp
    std::unordered_map<std::string, f64> derived_held; ///< hold_last_nonzero memory
    std::unordered_map<std::string, size_t> derived_index;
    std::vector<u8> derived_done;
    /// Redraw-job inputs shared instead of copied per dispatch (tick thread): one immutable copy of
    /// the current page (reset by ReloadManifest), the last font/icon metrics handed to the worker,
    /// and a snapshot the worker hands back so the next job's copy reuses its map nodes
    /// (spare_snapshot: guarded by redraw_job_mutex).
    std::shared_ptr<const Page> shared_page;
    size_t shared_page_index{~size_t{0}};
    std::shared_ptr<const FontMetrics> shared_font_metrics;
    std::shared_ptr<const FontMetrics> shared_icon_metrics;
    std::optional<StateSnapshot> spare_snapshot;
    /// EvaluateDerived's replayed evaluation order, and the list it was computed for (a reload
    /// resets derived_order_list).
    std::vector<u32> derived_order;
    const DerivedPoint* derived_order_list{nullptr};
    size_t derived_order_size{0};
    /// Whether any derived entry is volatile (derived_volatile): the post-tap pass is due.
    bool derived_reads_interaction{false};
    /// Per derived value: whether it reads this tick's interaction state ("@..", "view_custom:..")
    /// directly or through another derived value; the post-tap pass recomputes only those.
    std::vector<u8> derived_volatile;
    void UpdateHeldButtons();                      // mod_actions.cpp
    void PublishUi(const StateSnapshot& snapshot); // mod_redraw.cpp
    /// GPU-composite publish: hand the renderer a quad display list plus up to three source
    /// textures (map area, icon atlas, HUD overlay), each re-uploaded only when it changes.
    /// Appends the HUD quad to `dl`. `hud_canvas` is whichever `Canvas` actually holds
    /// this tick's HUD-only raster -- `ModRuntime::canvas` from the synchronous/kill-switch carve-
    /// out, `worker_canvas` from the redraw worker -- mirroring the same substitution
    /// `RunRedrawJob` already makes for the ordinary canvas path
    /// (`dispatch_w`/`dispatch_h`/`worker_canvas` standing in for `canvas`). Reads only
    /// `hud_canvas`'s pixels/dimensions; every other touched member
    /// (`last_map_key`/`last_hud_hash`/etc.) is guarded by `gpu_composite_mutex` -- see that
    /// field's own comment for why a single-caller-per-run invariant is not enough by itself.
    /// `hud_dirty` (optional): `hud_canvas` changed only inside this rect since the last publish
    /// of it, so only the rect of the HUD slot is diffed (used only when that last HUD publish was
    /// of this same canvas; the full-canvas hash + diff otherwise).
    void PublishGpuComposite(AuxDrawList& dl, Canvas& hud_canvas,
                             const std::array<s32, 4>* hud_dirty = nullptr); // mod_redraw.cpp
    /// Everything a rasterised area image depends on; one definition shared by the image cache
    /// key, the GPU re-upload gate and the UI signature.
    struct MapStamp {
        u64 vis{}, water{}, fade{}, wall{}, emmy{};
        bool operator==(const MapStamp&) const = default;
    };
    [[nodiscard]] MapStamp StampFor(const std::string& area) const; // mod_map.cpp
    [[nodiscard]] static std::string StampKey(const MapStamp& s);   // mod_map.cpp
    /// pixel -> reveal-grid cell lookup tables for one image size (see CellTables).
    struct PixelCellTables {
        int width{-1}, height{-1};
        std::vector<int> col_of_px;
        std::vector<int> row_of_py;
    };
    const PixelCellTables& CellTables(int width, int height) const; // mod_map.cpp
    mutable PixelCellTables cell_tables;
    AuxDrawList draw_list; ///< reused per publish (quads cleared, no per-tick allocation)
    // mod_redraw.cpp
    /// Cheap change-signature over everything the aux page draws from; when unchanged we
    /// skip RenderPage + the aux copy + GPU re-upload (the page is idle).
    u64 UiSignature(const StateSnapshot& snapshot, u32 target_w, u32 target_h) const;
    /// Hashes of the page-draw inputs the per-widget dependency hashes do not cover
    /// ({hard, soft}; see its definition in mod_redraw.cpp).
    std::pair<u64, u64> UncoveredSignature(const StateSnapshot& snapshot) const;
    // mod_map.cpp
    /// A Map widget whose resolved area is a fixed, prerendered picture (no geometry redraw, no
    /// self-animation) -- shared by UiSignature's `animating` flag and WidgetDependencyHash so both
    /// treat the same widget the same way. See UiSignature's own comment for the area_bind caveat.
    [[nodiscard]] bool IsPictureMapWidget(const Widget& widget, const StateSnapshot& s) const;
    // mod_map.cpp
    /// The map area a Map widget draws for this snapshot (room_bind, area_bind + season, area).
    [[nodiscard]] std::string ResolveMapArea(const Widget& w, const StateSnapshot& s) const;
    u64 last_ui_signature{0};
    bool ui_signature_valid{false};
    /// UncoveredSignature at the last redraw decision (PublishUi; tick thread).
    u64 last_uncovered_hard{0};
    u64 last_uncovered_soft{0};
    bool uncovered_valid{false};
    // Dirty-region redraw: a second, cheaper level under
    // UiSignature. UiSignature stays the unchanged top-level "did anything change at all" gate;
    // these hold, per SOURCE widget of the current page (manifest.pages[current_page].widgets --
    // pre-ExpandWidgets, so a repeat template gets exactly one slot, at the definition, not one per
    // instance), the dependency hash and rect last published, so a tick
    // where only some widgets actually changed can redraw just their union rect instead of the
    // whole page. Computed only on ticks where UiSignature already says "changed" (see
    // WidgetDependencyHash / BuildRenderExtras) -- zero extra cost on the common idle tick.
    std::vector<u64> widget_sig;
    std::vector<std::array<s32, 4>> widget_last_rect;
    size_t widget_sig_page{~size_t{0}}; ///< page these two arrays belong to; ~0 = rebuild/full next
    /// asset_epoch baseline as of the last widget_sig rebuild. A landed image/composite/font/msbt
    /// doesn't correspond to any bind value, so no per-widget hash would ever notice it moved --
    /// same shape as the first-open snapshot race, at ordinary-redraw granularity instead of a
    /// transition's. When this has moved, BuildRenderExtras forces one full-page redraw and
    /// rebaselines every slot, instead of leaving a just-landed asset's widget silently stale.
    u64 widget_sig_asset_epoch{0};
    // mod_redraw.cpp
    /// Per-(source-widget, snapshot) dependency hash: changes iff something THIS widget's draw
    /// case actually reads changed. Re-derived from the per-type draw functions RenderPage
    /// dispatches to (mod_ui.cpp, mod_ui_map_widget.cpp), not summarised (a missed input would
    /// leave that widget stale). `picture_map`: whether the resolved area of a Map widget draws a
    /// prerendered picture (no per-tick self-animation) or geometry (redraws continuously, same
    /// test UiSignature's own `animating` flag already uses -- kept as one shared predicate,
    /// WidgetSelfAnimates, so the two can never disagree).
    [[nodiscard]] u64 WidgetDependencyHash(const Widget& w, const StateSnapshot& s,
                                           const std::string& page_id, size_t source_index) const;
    /// WidgetDependencyHash for a repeat template: the template plus every element ExpandWidgets
    /// builds from it this tick (substituted binds, rects, scroll state).
    [[nodiscard]] u64 RepeatTemplateDependencyHash(const Page& page, size_t index,
                                                   const StateSnapshot& s) const; // mod_redraw.cpp
    /// Reused element buffer of RepeatTemplateDependencyHash (tick thread only).
    mutable std::vector<Widget> repeat_hash_scratch;
    /// Scroll metrics of the page being scanned, per region (reset per scan; tick thread).
    mutable ScrollMemo scan_scroll_memo;
    /// Per-template element slots of the dirty scan (index = the template's widget index on the
    /// scanned page) and the element each slot holds; reset whenever widget_sig is re-baselined.
    mutable std::vector<std::vector<Widget>> repeat_hash_elements;
    mutable std::vector<std::vector<s64>> repeat_hash_indices;
    /// The elements of the template RepeatTemplateDependencyHash expanded last (tick thread), their
    /// element indices (empty when not cached per template), per-element hashes, and the hash of
    /// the list-level inputs alone.
    mutable std::span<const Widget> repeat_hash_last;
    mutable std::span<const s64> repeat_hash_last_idx;
    mutable std::vector<u64> repeat_hash_elem;
    mutable u64 repeat_hash_level{};
    /// Per repeat template (by widget index): the elements as of its last dirty-scan baseline.
    struct RepeatState {
        bool valid{false};
        u64 level{};
        std::vector<s64> idx;
        std::vector<u64> hash;
        std::vector<std::array<s32, 4>> rect;
    };
    std::vector<RepeatState> repeat_state;
    // mod_redraw.cpp
    void RebuildRepeatState(size_t index, const StateSnapshot& s, u32 cw, u32 ch);
    // mod_redraw.cpp
    /// Where those elements paint this tick: the union of their dirty boxes.
    [[nodiscard]] std::array<s32, 4> RepeatElementsRect(const StateSnapshot& s, u32 cw,
                                                        u32 ch) const;
    // GPU-composite path (EDEN_DSMOD_GPU_COMPOSITE): emit quads + source textures for the renderer
    // to composite, instead of one flattened ~5 MB canvas upload per change.
    bool gpu_composite_mode{false};
    /// `PublishGpuComposite` runs off the tick thread whenever this page dispatches (the ordinary
    /// case -- see `will_dispatch`), so the fields below are not implicitly tick-thread-owned.
    /// Exactly one of {tick thread, worker thread} calls `PublishGpuComposite` for a given
    /// process run (`will_dispatch` depends only on `sync_redraw`/`page.id`, both fixed per run --
    /// see `PublishGpuComposite`'s own comment), so these are NOT concurrently written by both --
    /// but the console "reload" reset (`ModRuntime::Reload`-ish path, the block right after this
    /// struct's own definition that clears `last_atlas_key`/`last_map_key`/etc.) is a TICK-THREAD
    /// writer outside that single-caller discipline, and can race a worker mid-composite. Same
    /// class of hazard as `image_cache`/`map_visited` (a console-triggered reset outside the normal
    /// call chain), guarded the same way: one mutex, taken by `PublishGpuComposite` and by the
    /// reload reset.
    mutable std::mutex gpu_composite_mutex;
    std::string last_map_key;   ///< map-area image key uploaded to slot 0 ...
    MapStamp last_map_stamp;    ///< ... and its content stamp; re-upload when either moves
    std::string last_pulse_key; ///< stamped key of the pulse companion last uploaded (slot 3)
    bool last_pulse_present{false};
    std::vector<u32> map_fade_weights; ///< 650x300 RGBA weights, reused across fade publishes
    u64 last_map_fade_epoch{std::numeric_limits<u64>::max()};
    /// The icon atlas slot 1 holds: its GetImage key and the Image uploaded (a different key, or
    /// the same key decoded again, uploads again).
    std::string last_atlas_key;
    std::shared_ptr<const Image> last_atlas_image;
    u64 last_hud_hash{0}; ///< HUD overlay content hash (slot 2), re-upload on change
    bool last_hud_hash_valid{false}; ///< last_hud_hash describes what slot 2 holds
    const Canvas* hud_slot_canvas{nullptr}; ///< the canvas slot 2 was last published from
    /// The map endpoints (slots 0/4) the routing buffer holds, and the bundle epoch that publish
    /// returned: the base a reveal's tile-diff publish is computed against.
    std::shared_ptr<const Image> map_pub_current, map_pub_previous;
    u64 map_pub_epoch{0};
    [[nodiscard]] bool AddressIsSane(VAddr address, u64 size) const; // mod_state.cpp
    /// True when any reveal-grid cell inside the world rect has been explored.
    [[nodiscard]] bool RoomExplored(const std::string& area, const std::array<float, 4>& rect,
                                    bool visited_only = false) const; // mod_map.cpp
    [[nodiscard]] std::string DescribeGuestValue(u64 value) const;    // mod_state.cpp
    // --- asset decoders and fonts (mod_assets.cpp; engine formats in engine_*.cpp) -----------
    static bool DecodeBctex(std::span<const u8> file, Image& out); // engine_mercury.cpp
    static bool DecodeBntx(std::span<const u8> file, Image& out);
    static bool DecodeSosXtx(std::span<const u8> raw, Image& out); // engine_ichigo.cpp
    static bool DecodeDds(std::span<const u8> file, Image& out);
    /// Metroid Dread's own MFNT font-metrics container -- the in-core fallback used when no
    /// module is loaded, the module declares no font extension, or it rejects `param` as
    /// unrecognised. A module's own decode_font still takes precedence when present (see
    /// ModuleDecodeFont below); this exists so a Dread package whose module predates
    /// eden_dsmod_get_font_extensions still gets a working font instead of none at all.
    static bool ParseMfnt(std::span<const u8> param, FontMetrics& out);    // engine_mercury.cpp
    static bool ParseSosFont(std::span<const u8> param, FontMetrics& out); // engine_ichigo.cpp
    static std::vector<u8> DecodeLzss(std::span<const u8> in);             // engine_ichigo.cpp
    // engine_ichigo.cpp
    static std::vector<u8> ExtractArchiveMember(std::span<const u8> raw, const std::string& want);
    std::vector<u8> ReadAssetBytesRaw(const std::string& src);
    /// The "romfs:" source's opener (RegisterAssetSources): the running game's patched romfs.
    FileSys::VirtualDir OpenGameRomFS(); // mod_assets.cpp

    System& system;
    Core::Timing::CoreTiming& core_timing;
    Manifest manifest;
    std::shared_ptr<Core::Timing::EventType> event;

    VAddr main_region_begin{};
    u64 main_region_size{};

    Canvas canvas;
    /// The atlases `canvas` borrows raw pointers to (SetFont / SetIconFont): held here so a
    /// re-landed module image cannot free them while the canvas still points at them.
    std::shared_ptr<const Image> canvas_font_ref;
    std::shared_ptr<const Image> canvas_icon_ref;
    size_t current_page{0};
    u64 tick_count{0};

    // Virtual-gamepad presses are held for a few ticks so the guest's 60 Hz sampling sees them.
    /// A press queued for a future tick, so one tap can walk several steps.
    struct ScheduledPress {
        std::string button;
        u64 at_tick{};
        u32 frames{4};
    };
    /// A stick pushed for a while, released on its own. Sticks are not buttons, so they need
    /// their own hold list.
    struct HeldStick {
        bool right{};
        u32 ticks_left{};
    };
    std::vector<HeldStick> held_sticks;
    /// Press a button or shove a stick, whichever the token names. Returns false for nonsense.
    bool PressToken(const std::string& name, u32 frames); // mod_actions.cpp
    /// Release every virtual control owned by this runtime. Used during teardown so a disappearing
    /// second screen cannot leave a guest button or stick held indefinitely.
    void ReleaseHeldInputs(); // mod_actions.cpp
    std::vector<ScheduledPress> press_queue;
    std::unordered_map<std::string, s64> counters;

    struct HeldButton {
        std::string name;
        u32 ticks_left{};
    };
    std::vector<HeldButton> held_buttons;

    // --- guest calls (mod_guest_bridge.cpp) --------------------------------------------------
    // A call borrows the game's own thread: arm a breakpoint at a per-frame function, and when it
    // fires redirect pc to the target with lr pointing back at the hook, so the return traps and
    // the original context can be restored. No foreign thread, no scratch memory.
    enum class CallState { Idle, Armed, InCall };
    /// The timing thread arms calls while a CPU thread completes them at a guest breakpoint.
    /// Keep the small bridge state transitions serialized; expensive sampling/raster work never
    /// holds this lock. A recursive mutex lets bridge helpers install/remove their breakpoints.
    mutable std::recursive_mutex guest_bridge_mutex;
    /// BRK-based thread borrowing is handled by Dynarmic. NCE executes BRK on the host CPU and
    /// therefore must never have one inserted into guest code.
    bool guest_bridge_supported{true};
    /// Looks a method up by name in an IL2CPP class's method table. Addresses move every game
    /// update; names do not.
    std::optional<s64> ResolveSymbol(const std::string& name);
    // engine_il2cpp.cpp
    std::optional<s64> ResolveMethodByName(s64 class_slot, const std::string& method,
                                           VAddr& method_info_out) const;

    bool InstallBreakpoint(VAddr address);
    void RemoveBreakpoint(VAddr address);
    void RequestCall(const Action& action, const StateSnapshot& snapshot);

    std::atomic<CallState> call_state{CallState::Idle};
    VAddr call_target{};
    VAddr resolved_hook{};
    VAddr return_trampoline{};
    u64 call_started_tick{};
    static constexpr u64 StuckCallTicks = 300; ///< ~5 s at the 60 Hz tick
    std::array<u64, 8> call_args{};
    Kernel::Svc::ThreadContext saved_context{};
    u32 original_instruction{};
    /// The instruction each of our two patch sites replaced. Keeping them separately matters:
    /// a spy firing mid-sequence overwrites the shared scratch copy, and restoring the wrong
    /// word would leave a brk in the game's code.
    u32 hook_original{};
    u32 trampoline_original{};
    s64 last_call_result{};
    f64 last_call_float{};
    /// The sequence in flight, where it is, and the values its steps have saved. `call_slots`
    /// also holds "L": the lua_State the hook was carrying when we borrowed the thread.
    const CallSequence* call_seq{nullptr};
    std::string call_seq_name;
    size_t call_seq_step{0};
    /// The armed sequence's step functions and "$@symbol" arguments, resolved by StartSequence on
    /// the tick thread (guest_bridge_mutex); the breakpoint handler only indexes them.
    std::vector<std::optional<s64>> call_seq_fn;
    std::unordered_map<std::string, std::optional<s64>> call_seq_symbols;
    std::unordered_map<std::string, u64> call_slots;
    StateSnapshot call_snapshot;
    StateSnapshot
        tick_snapshot; ///< reused each tick; clear() keeps capacity, avoids per-frame alloc
    // Per-tick snapshot keys, built once (see PrefixedKeys). Tick thread only.
    PrefixedKeys flag_keys{"@flag:"};
    PrefixedKeys view_custom_keys{"view_custom:"};
    PrefixedKeys counter_keys{"@"};
    PrefixedKeys map_sel_keys{"@map_sel:"};
    PrefixedKeys map_sel_sx_keys{"@map_sel_sx:"};
    PrefixedKeys map_sel_sy_keys{"@map_sel_sy:"};
    PrefixedKeys slot_used_keys{"@slot_used:"};
    PrefixedKeys slot_full_keys{"@slot_full:"};
    PrefixedKeys sel_keys{"@sel:"};
    PrefixedKeys last_keys{"@last:"};
    IndexedKeys element_keys;
    /// Publishes every runtime flag as "@flag:<name>".
    void PublishFlags(StateSnapshot& snapshot); // mod_input.cpp
    std::unordered_map<std::string, f64> sequence_values;
    std::unordered_map<std::string, u64> sequence_addresses;
    std::unordered_map<std::string, std::string> sequence_texts;
    /// Breakpoint-thread results wait here until Tick moves them into the timing-owned maps above.
    std::unordered_map<std::string, f64> pending_sequence_values;
    std::unordered_map<std::string, u64> pending_sequence_addresses;
    std::unordered_map<std::string, std::string> pending_sequence_texts;
    std::unordered_map<std::string, u64> pending_spy_values;
    std::atomic<u64> bridge_tick_count{0};
    void DrainGuestBridgeResults();
    std::unordered_map<std::string, u64> sequence_last_run;
    std::unordered_map<std::string, s64> symbol_cache;

    mutable std::mutex tap_mutex;
    std::vector<PendingTap> pending_taps;
    bool tap_was_down{false};

    // --- second-screen gestures (mod_input.cpp) ----------------------------------------------
    /// One finger as of the previous frame. The panel reports a set of live points rather than
    /// events, and once the guest has consumed a start edge no end edge is ever delivered, so a
    /// release is inferred from a finger that stopped being reported.
    struct TrackedFinger {
        u32 id{};
        s32 x{};
        s32 y{};
    };
    std::vector<TrackedFinger> live_fingers;
    std::string gesture_target; ///< id of the widget this gesture pans, empty if none
    s32 gesture_down_x{};
    s32 gesture_down_y{};
    bool gesture_moved{false}; ///< travelled far enough to stop being a tap
    /// Press-and-hold of this gesture (runtime 13, "on_hold"): the tracker, and the action the
    /// on_hold widget under the first finger names.
    HoldTracker hold_tracker;
    std::string hold_action;
    /// Swipe of this gesture (runtime 14, "on_swipe_left" / "on_swipe_right"; unreleased 15
    /// addition "on_swipe_up" / "on_swipe_down"): the tracker, and the actions of the swipe
    /// widget under the first finger.
    SwipeTracker swipe_tracker;
    std::string swipe_left_action;
    std::string swipe_right_action;
    std::string swipe_up_action;
    std::string swipe_down_action;
    float gesture_span{0.0f};  ///< finger separation last frame, for pinch
    mutable std::mutex view_mutex;
    ViewState view_state;
    MapFollowState map_follow_state;
    /// Bumped (under view_mutex) whenever map_follow_state is reset or its pan corrections are
    /// consumed; the redraw worker's marker-only redraw restores a saved follow state only while
    /// this is unchanged.
    u64 follow_state_epoch{0};
    /// When a finger last rested on each pannable widget (view_idle_ms return home).
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> view_touched;
    /// Glides home every view whose widget asks for it (view_idle_ms) once left alone that long.
    void ReturnIdleViews();
    /// Takes the "<key>#pan" corrections a clamped map left in map_follow_state into view_state.
    void ApplyViewCorrections();
    /// Bound default views (unreleased runtime 15 addition, mod_view_default.h): each pan_zoom
    /// widget of the current page with "view_zoom_bind" / "view_cx_bind" / "view_cy_bind" gets
    /// its home from this tick's values; a view at its old home follows. Takes view_mutex.
    void ApplyViewDefaults(const StateSnapshot& snapshot);

    // --- tap-select and drag-and-drop (mod_input.cpp) ----------------------------------------
    /// A finger that landed on a draggable widget. It becomes a drag once it travels beyond the
    /// tap slop; until then it may still be a tap. A drag never pans what lies beneath.
    struct DragState {
        bool candidate{false};
        bool active{false};
        u32 finger{};
        size_t page{};
        Widget widget;  ///< expanded copy of the widget picked up
        s64 source{-1}; ///< its expanded index on the page
        s64 payload{};
        s32 grab_dx{};
        s32 grab_dy{};
        s32 x{}; ///< latest finger position, canvas px
        s32 y{};
        s64 hover{-1}; ///< drop target under the finger (expanded index), -1 none
    };
    DragState drag_state;

    // --- drag-to-scroll lists (Page::scrolls; mod_input.cpp) ---------------------------------
    struct ScrollState {
        float offset{0.0f};   ///< px, content scrolled up out of the rect
        float velocity{0.0f}; ///< px/s while flinging (+ = content moving up)
        bool flinging{false};
        bool has_reset{false};
        s64 reset_value{0}; ///< last reset_bind value seen
    };
    std::unordered_map<std::string, ScrollState> scroll_state; ///< region id -> state
    size_t scroll_page{~size_t{0}};
    std::chrono::steady_clock::time_point scroll_clock{};
    std::string scroll_target;   ///< region the current gesture scrolls, empty if none
    bool scroll_dragging{false}; ///< past the tap slop: the finger now moves the list
    bool scroll_caught{false};   ///< the finger landed on a gliding list: stopping it is not a tap
    float scroll_anchor_offset{0.0f};
    s32 scroll_anchor_y{0};
    struct ScrollSample {
        std::chrono::steady_clock::time_point t;
        s32 y;
    };
    std::vector<ScrollSample> scroll_samples; ///< recent finger positions, for the fling speed
    /// The topmost live scroll region under a canvas point on the current page, or "".
    std::string ScrollRegionAt(const StateSnapshot& snapshot, s32 x, s32 y) const;
    /// Advances flings (when `advance`), applies resets / clamps and publishes "@scroll:<id>",
    /// "@scroll_max:<id>", "@scroll_on:<id>", "@scroll_first:<id>" for the current page.
    void PublishScroll(StateSnapshot& snapshot, bool advance);
    /// Fold one frame of the gesture into the scroll target (called from UpdateGestures).
    void UpdateScrollGesture(const StateSnapshot& snapshot, size_t fingers, s32 y, bool moved,
                             bool released);

    struct PendingDrop {
        std::string action;
        s64 payload{};
        HapticOverride haptic{-1}; ///< the drop target's haptic override
        std::string group;         ///< the dragged widget's select_group (cleared after the drop)
        s32 x{};
        s32 y{};
    };
    std::vector<PendingDrop> pending_drops;
    std::unordered_map<std::string, s64> selections; ///< select_group -> selected payload
    std::set<std::string> interact_groups;           ///< every group named by the package
    bool interact_groups_ready{false};
    size_t interact_page{0};
    /// Drop target (expanded index) under a canvas point for the current drag, or -1.
    s64 DropTargetAt(const StateSnapshot& snapshot, s32 x, s32 y) const;
    void ResetInteraction(const char* why);
    /// Publishes "@sel:<group>", "@drag*" and the drag overlay into the snapshot.
    void PublishInteraction(StateSnapshot& snapshot);
    /// Console "drag x0 y0 x1 y1 ms": a synthetic finger fed through the aux touch path.
    struct CmdDrag {
        bool active{false};
        bool normalised{true};
        u64 start{};
        u64 move_ticks{1};
        u64 end_dwell{4}; ///< ticks resting over the end point before release (0 = a flick)
        float x0{}, y0{}, x1{}, y1{};
    };
    CmdDrag cmd_drag;
    void DriveCmdDrag(u32 panel_w, u32 panel_h, u32 canvas_w, u32 canvas_h);

    /// Which pannable widget, if any, sits under this canvas point.
    std::string PannableAt(const StateSnapshot& snapshot, s32 x, s32 y) const;
    const Widget* FindWidgetByKey(const std::string& key) const;
    std::pair<float, float> ZoomLimits(const std::string& key) const;
    std::array<s32, 4> WidgetRect(const std::string& key) const;

    /// Fold this frame's touch points into pan/zoom, and emit a tap on release when the finger
    /// never really moved.
    void UpdateGestures(const StateSnapshot& snapshot,
                        std::span<const VideoCore::DSMod::AuxTouchPoint> points, size_t count,
                        u32 panel_w, u32 panel_h, u32 canvas_w, u32 canvas_h);

public:
    /// Snapshot of every pannable widget's position, for the renderer.
    ViewState GetViewState() const;
    /// Advances every view a reset put in flight; true while any is still easing home.
    bool GlideViews();

private:
    /// A module's own font extension (dsmod_module_extensions.h) decodes proprietary metrics
    /// formats when it is present -- takes precedence over any in-core parser. False when no
    /// module is loaded, the module declares no font decoder, or it rejected `bytes` as
    /// malformed/unrecognised; LoadFont() then falls back to ParseMfnt/ParseSosFont in core.
    bool ModuleDecodeFont(std::span<const u8> bytes, FontMetrics& out); // mod_assets.cpp
    FontMetrics font_metrics;
    bool font_ready{false};
    /// A module font decoder may read the running game (metric tables read from main),
    /// which can fail before the game is up: LoadFont then retries on a slow schedule, bounded.
    u32 font_module_attempts{0};
    u64 font_retry_tick{0};
    void LoadFont(); // mod_assets.cpp

    // --- Off-thread redraw worker: renders ordinary page redraws and publishes its output -------
    // The unit of work handed to the worker. Every field is an OWNED value or shared_ptr -- nothing
    // here is a pointer into a ModRuntime member a later tick could still mutate. The font atlas
    // and `font_metrics`/`icon_metrics` are held by value/shared_ptr for that reason: the live ones
    // are plain, unlocked ModRuntime/NxAssetState members mutated by `PumpNxAssets` on the tick
    // thread, so a pointer to them could dangle.
    //
    // `report_visit`/`follow_state`/`map_records` are handed to the worker LIVE (the real
    // `MarkVisitedAt`, `&map_follow_state` under `&view_mutex`, a job-local scratch buffer
    // published into `map_draw_records_published` on success): this job's render IS the published
    // frame, so it must drive the real side effects, under the same locks the synchronous call
    // uses.
    struct RedrawJob {
        u64 generation{};
        u64 authority{}; ///< redraw_authority_generation at dispatch (runtime 13)
        /// Shared, immutable copy of the page (made once per page, not per job).
        std::shared_ptr<const Page> page_copy;
        StateSnapshot snapshot;
        RenderExtras extras;
        bool has_extras{false}; ///< mirrors PublishUi's own "groups_dirty || !groups.empty()" gate
        /// The actual partial-vs-full publish decision (PublishUi's own `partial`, at
        /// dispatch time) -- NOT the same thing as `has_extras` above (that only gates whether
        /// `extras` is passed to RenderPage at all for widget-group clipping; a page can have
        /// has_extras=true with partial=false, e.g. group animating but the page just changed).
        /// RunRedrawJob uses this, not has_extras, to choose PublishUiPartial vs PublishUi.
        bool partial{false};
        ViewState views;
        std::shared_ptr<const Image> font_atlas;
        /// Shared, immutable metrics (a new copy only when the font actually changes).
        std::shared_ptr<const FontMetrics> font_metrics_copy;
        std::shared_ptr<const Image> icon_atlas;
        std::shared_ptr<const FontMetrics> icon_metrics_copy;
        bool icon_pending{false};
        u32 target_w{}, target_h{};
        /// `current_page` AT DISPATCH TIME -- captured here rather than re-read from
        /// `ModRuntime::current_page` inside RunRedrawJob, because that member can change on the
        /// tick thread before the worker gets around to this job (backlog), and reading it
        /// unguarded from the worker would be a race on a plain size_t. Used to publish
        /// map_records_page correctly for THIS job's page, not whatever page is current by the
        /// time the worker finishes.
        size_t page_index{~size_t{0}};
        /// True when this tick's page is GPU_COMPOSITE-eligible (mirrors PublishUi's own
        /// `dl != nullptr` at dispatch time). When set, RunRedrawJob renders through `draw_list`
        /// below (an owned, by-value AuxDrawList -- the same treatment `extras` gets, since
        /// `ModRuntime::draw_list` is a live member the tick thread mutates next tick) instead of
        /// publishing the plain canvas.
        bool gpu_composite{false};
        /// Job-owned draw list: RenderPage fills this DURING the worker's own render call (never
        /// touches `ModRuntime::draw_list`, which stays exclusively tick-thread-owned for the
        /// synchronous/kill-switch carve-out). Default-constructed (empty quads, `active=false`) --
        /// RenderPage sets `canvas_w`/`canvas_h`/`bg` itself off the worker's own target canvas,
        /// same as the synchronous path does off `canvas`.
        AuxDrawList draw_list;
    };

    /// Single-slot coalescing mailbox, modelled directly on this codebase's own
    /// `AuxRouting::haptic_thread`/`NxAssetState::worker` lazy-start jthread pattern. "Coalescing,
    /// not queueing": if the worker hasn't picked up the previous job yet,
    /// this tick's job REPLACES it (after folding the stale job's dirty rect into this one, so no
    /// region a skipped tick would have painted is lost) instead of piling up -- bounds worker
    /// backlog to at most one job's latency, ever. The tick thread never blocks on the worker:
    /// DispatchRedraw is an O(1) lock + assign + notify whether the worker is idle or mid-render.
    mutable std::mutex redraw_job_mutex;
    std::condition_variable_any redraw_job_cv;
    std::optional<RedrawJob> redraw_pending_job; ///< guarded by redraw_job_mutex
    /// The worker is inside RunRedrawJob (guarded by redraw_job_mutex). InstallModuleAreas waits
    /// for a moment with no job running or queued instead of joining the worker mid-job.
    bool redraw_job_running{false};
    /// Bumped by DispatchRedraw for every new job (tick thread only). ALSO bumped by
    /// any tick-thread code path that starts directly publishing to `aux` outside this mailbox --
    /// DrivePageTransition (both its fresh-transition-start and its abnormal
    /// switched-again/resized branches) and PublishUi's own synchronous carve-out
    /// (GPU_COMPOSITE/debug page/sync_redraw). RunRedrawJob compares a job's own captured
    /// `generation` against this before publishing: if something else has since taken authority
    /// over the screen, this job's content is stale and must not overwrite it. The race this
    /// closes: a worker job already
    /// in flight when a page transition begins has nothing to coalesce against -- the mailbox is
    /// only checked at DISPATCH time -- and would otherwise publish its now-superseded pixels
    /// whenever it happens to finish, silently overwriting whatever the transition already
    /// correctly put on screen).
    std::atomic<u64> redraw_dispatch_generation{0};
    /// Runtime 13: bumped ONLY where the tick thread takes authority over the screen (a page
    /// transition, the synchronous carve-out) -- the sites that also bump
    /// redraw_dispatch_generation, minus DispatchRedraw itself. RunRedrawJob's staleness check uses
    /// this: a job that was merely superseded by a newer DISPATCH is not stale (the single worker
    /// finishes jobs in order, so publishing its region first is correct). Before, every dispatch
    /// made the running job stale: with a slow page (an anim group repainting a big box every
    /// tick) every job went stale and nothing was published until the page settled -- the MK8D
    /// row-format anim never showed.
    std::atomic<u64> redraw_authority_generation{0};
    std::atomic<u64> redraw_completed_generation{0}; ///< bumped by the worker after each job;
                                                     ///< observability only (how far behind the
                                                     ///< worker is), nothing gates on it
    /// The worker's own persistent canvas (partial redraw is stateful --
    /// "everything outside the clip keeps last frame's pixels" -- so this must be ONE long-lived
    /// canvas the worker alone ever touches, never a ping-ponged double buffer, or every job would
    /// be forced full). This canvas's pixels are what's actually published to the
    /// screen for the ordinary ticks that dispatch to it -- see RunRedrawJob and PublishUi's
    /// `will_dispatch` branch. `ModRuntime::canvas` (the tick thread's own) is the synchronous
    /// carve-out's canvas (debug page, page transitions, GPU_COMPOSITE).
    Canvas worker_canvas;
    /// Worker-thread only: see UnpublishedRegions (RunRedrawJob).
    UnpublishedRegions worker_unpublished;
    /// Worker-thread only: the job before this one ended in a GPU-composite publish of
    /// worker_canvas (so composite slot 2 equals worker_canvas outside the next job's dirty rect).
    bool worker_hud_synced{false};
    /// The worker's own persistent `MapDrawRecords` scratch buffer, mirroring
    /// `worker_canvas`'s own reasoning immediately above -- a partial redraw only repaints (and
    /// only re-records) whatever is inside this tick's dirty rect, so whatever `RenderPage` is
    /// asked to accumulate INTO must be the same long-lived container across jobs, never a fresh
    /// one per job, or every widget this job's own dirty rect doesn't happen to cover (the Map
    /// widget, on the overwhelming majority of ordinary HUD-only partial redraws) silently loses
    /// its tap-hit-testing record the moment `RunRedrawJob` copies this out into
    /// `map_draw_records_published` below (`RenderPage`'s conditional clear, mod_ui.cpp, relies on
    /// the container surviving across calls; the synchronous carve-out uses the persistent,
    /// tick-thread-owned `map_draw_records` member for the same reason). Worker-thread-exclusive,
    /// same as `worker_canvas` itself: no mutex needed here, only `RunRedrawJob`'s own copy-out
    /// into `map_draw_records_published` (already `map_records_mutex`-guarded) crosses threads.
    MapDrawRecords worker_map_draw_records;
    /// What `worker_canvas` currently holds -- the dispatch-path analogue of
    /// `canvas_page`/`canvas_hud` above, tracked SEPARATELY because those two must keep accurately
    /// describing `ModRuntime::canvas`'s own actual content (DrivePageTransition's `reuse` check
    /// depends on that staying true) rather than being overwritten by a page this job only
    /// DISPATCHED, not yet drawn. Tick-thread-owned and tick-thread-only (no mutex needed): updated
    /// optimistically right after DispatchRedraw, exactly mirroring how canvas_page is updated
    /// optimistically right after a successful synchronous RenderPageTo -- correct because
    /// DispatchRedraw's own coalescing/clip-union logic already guarantees no region a "skipped"
    /// generation would have painted is ever lost, so the tick thread never needs to wait for the
    /// worker's actual completion to reason about continuity. canvas_hud has no equivalent here:
    /// dispatch eligibility already requires dl == nullptr, so the dispatch path is never HUD-only.
    size_t dispatch_page{~size_t{0}};
    u32 dispatch_w{0};
    u32 dispatch_h{0};
    /// The worker's own "a view glide is still in flight" signal (RenderPage's own
    /// `animating` out-param), latched here because -- unlike the synchronous path, where
    /// `animating` is known same-tick -- the dispatch path only learns it once the worker actually
    /// finishes, generations later. PublishUi consumes-and-clears this once per tick (before any
    /// early return) and forces `ui_signature_valid = false` when it was set, matching exactly what
    /// the synchronous path already did inline.
    std::atomic<bool> dispatch_animating{false};
    // --- redraw worker (mod_redraw.cpp) ------------------------------------------------------
    void EnsureRedrawWorker();
    void StopRedrawWorker();
    void RedrawWorkerMain(std::stop_token stop);
    void RunRedrawJob(RedrawJob& job);
    /// Builds a RedrawJob from this tick's already-resolved dispatch state and hands it to the
    /// worker (starting it on first use). Never blocks.
    void DispatchRedraw(RedrawJob&& job);
    std::jthread redraw_thread; ///< LAST member: stopped/joined (StopRedrawWorker) before anything
                                ///< above it goes away; see ~ModRuntime.

public:
};

} // namespace Core::Mods
