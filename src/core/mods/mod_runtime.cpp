// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// ModRuntime lifecycle and the per-frame tick. The runtime is split by area into mod_*.cpp files;
// this one keeps what ties them together:
//   - ModRuntime / ~ModRuntime (the destructor stops the redraw worker before anything else goes
//     away), SetMainMemoryParameters, and Initialize: loads the native module, reads the env and
//     manifest switches, and schedules the looping "DSMod::Tick" CoreTiming event at ModTickHz.
//   - Tick, the per-frame driver: module images and guest-bridge results are drained, headless
//     input drivers run, then SampleState (mod_state.cpp) -> touch capture + module tick ->
//     EvaluateDerived -> PublishMapState / PublishScroll / UpdateGestures / DrainTaps
//     (mod_input.cpp; taps run RunAction, mod_actions.cpp) -> EvaluateDerived (volatile pass) ->
//     ApplyEnforceRules (mod_state.cpp) -> DrivePageBinds (mod_pages.cpp) -> PublishUi
//     (mod_redraw.cpp).
//   - The dev-tools shells (DumpWatchedMemory, DiffScan, DriveCmd, ...): empty unless
//     EDEN_DSMOD_BUILD_DEV_TOOLS, then each calls its FooImpl in mod_re_tools.cpp or
//     mod_console.cpp.
// Not here: manifest parsing (mod_manifest.cpp), guest reads (mod_state.cpp), drawing and publish
// (mod_redraw.cpp), native title modules (mod_module_host.cpp, mod_module_services.cpp).
// Threads: the constructor and Initialize run on the thread that loads the title (System load).
// Tick is a CoreTiming callback (the host timing thread in multi-core mode), so time spent in it
// delays other timed events. This file takes view_mutex directly (the view_custom: values);
// every other lock is taken inside the callees.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <cstdlib>
#include <fstream>
#include "common/fs/fs_util.h"
#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/mods/mod_hooks.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "core/mods/mod_view_default.h"
#include "hid_core/resources/npad/dsmod_pad_gate.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/gpu.h"

namespace Core::Mods {

namespace {
constexpr auto ModTickNs = std::chrono::nanoseconds{1000000000 / ModTickHz};
} // namespace

ModRuntime::ModRuntime(System& system_, Manifest manifest_)
    : system{system_}, core_timing{system_.CoreTiming()}, manifest{std::move(manifest_)} {
    // Before anything can read an asset: the registry is read without a lock from here on.
    RegisterAssetSources();
    // Runtime 16 @game.seconds: emulated-system time since boot (the wall clock is the default).
    clock_points.SetSource(ClockSource{.game_seconds = [this] {
        return static_cast<s64>(core_timing.GetGlobalTimeNs().count() / 1000000000);
    }});
}

ModRuntime::~ModRuntime() {
    // Stop and join the redraw worker FIRST, before anything below tears down
    // state it might still be mid-render against (manifest, image_cache, the nx-asset state, ...).
    // std::jthread's own destructor would eventually do this too (it is the last-declared member,
    // so member teardown order alone would stop it before asset_cache_mutex/map_state_mutex/etc.
    // are destroyed) -- but that happens only AFTER this function's own body finishes, which is too
    // late relative to ShutdownGameModule() below. Mirrors NxAssetState::Stop()'s same reasoning.
    StopRedrawWorker();
    DropFontPages(); // its loader reads through this runtime
    if (event) {
        core_timing.UnscheduleEvent(event);
    }
    ShutdownGameModule();
    ReleaseHeldInputs();
    Core::HID::DSModPadGate::Reset(); // runtime 17: never leave the game's pad gated
    system.GPU().DSModAux().ClearUi();
    // The brk gate stays open for the whole session (see OnGuestBreakpoint), but it belongs to
    // this game: the runtime is destroyed only after the cores are shut down, and the next title
    // in the same process gets fresh JITs. Leaving it set made every later game treat any JIT
    // exception as a debug breakpoint (suspend) instead of upstream's log-and-continue.
    g_guest_hooks_enabled.store(false, std::memory_order_relaxed);
}

void ModRuntime::SetMainMemoryParameters(VAddr main_region_begin_, u64 main_region_size_) {
    main_region_begin = main_region_begin_;
    main_region_size = main_region_size_;
}

void ModRuntime::Initialize() {
    if (event) {
        return;
    }
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    dev.log_guest_threads = Common::DSMod::DevEnvironment("EDEN_DSMOD_THREADS") != nullptr;
#endif
    // EDEN_DSMOD_NO_GUEST_BRIDGE=1 reproduces the handheld's NCE conditions on a Dynarmic desktop:
    // no frame hook, sequences or spies, so only memory reads and the title module feed the page.
    guest_bridge_supported =
        !Settings::IsNceEnabled() && Common::DSMod::DevEnvironment("EDEN_DSMOD_NO_GUEST_BRIDGE") == nullptr;
    if (!guest_bridge_supported &&
        (manifest.frame_hook != 0 || !manifest.frame_hook_symbol.empty() ||
         !manifest.spies.empty() || !manifest.sequences.empty())) {
        LOG_WARNING(Core,
                    "DSMod: guest calls, sequences, and spies are disabled under NCE because "
                    "the BRK bridge is only handled by Dynarmic; memory readers remain active");
    }
    InitializeGameModule();

#if EDEN_DSMOD_BUILD_DEV_TOOLS
    // EDEN_DSMOD_DUMP_ROMFS="<path in romfs>:<file to write>" saves one file out of the running
    // game's *patched* filesystem. An update ships only the files it changes, so this is the only
    // way to get the metadata for a base+update pairing without owning a merged dump.
    if (const char* const spec = Common::DSMod::DevEnvironment("EDEN_DSMOD_DUMP_ROMFS"); spec != nullptr) {
        const std::string text{spec};
        if (const auto split = text.rfind(':'); split != std::string::npos) {
            // Any registered source may be named ("aoc:x:/tmp/x"); a bare path is a romfs path.
            const std::string what = text.substr(0, split);
            const auto bytes =
                ReadAssetBytes(AssetSources::PrefixOf(what).empty() ? "romfs:" + what : what);
            const auto out = text.substr(split + 1);
            if (bytes.empty()) {
                LOG_ERROR(Core, "DSMod: '{}' not found in the game's romfs", text.substr(0, split));
            } else if (std::ofstream file{out, std::ios::binary}; file) {
                file.write(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
                LOG_INFO(Core, "DSMod: wrote {} ({} bytes) from the game's romfs", out,
                         bytes.size());
            }
        }
    }
#endif
    flags = manifest.flag_defaults;
    if (!manifest.persist_flags.empty()) {
        const std::string package = manifest.name.empty() ? manifest.mod_dir_name : manifest.name;
        flag_persistence = FlagPersistence{
            PersistFlagsPath(PersistFlagsRoot(), manifest.title_id, package), manifest.persist_flags};
        const size_t restored = flag_persistence.Restore(flags);
        LOG_INFO(Core, "DSMod: {} persisted flag(s), {} restored from {}",
                 manifest.persist_flags.size(), restored,
                 Common::FS::PathToUTF8String(flag_persistence.File()));
    }
    if (manifest.poll_hz != ModTickHz) {
        LOG_DEBUG(Core,
                  "DSMod: manifest poll_hz={} is legacy metadata; runtime scheduling is fixed "
                    "at {} Hz and millisecond intervals use elapsed tick time",
                    manifest.poll_hz, ModTickHz);
    }
    event = Core::Timing::CreateEvent(
        "DSMod::Tick",
        [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            Tick();
            return std::nullopt;
        });
    core_timing.ScheduleLoopingEvent(ModTickNs, ModTickNs, event);
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    // Search tools and headless input drivers (dev-tools builds only).
    if (const char* const heap = Common::DSMod::DevEnvironment("EDEN_DSMOD_HEAPDUMP"); heap != nullptr) {
        dev.heapdump_path = heap;
        LOG_INFO(Core, "DSMod: will snapshot the heap to '{}'", dev.heapdump_path);
    }
    // The manifest can carry these too. An Android app inherits no environment, so a search that
    // can only be configured by an environment variable can only ever run on a desktop -- and the
    // device is where the interesting differences live.
    if (!manifest.find_spec.empty()) {
        dev.find_spec = manifest.find_spec;
    }
    dev.trace_target = manifest.trace_target;
    if (!dev.trace_target.empty()) {
        LOG_INFO(Core, "DSMod: will trace a static route to '{}'", dev.trace_target);
    }
    if (const char* const find = Common::DSMod::DevEnvironment("EDEN_DSMOD_FIND"); find != nullptr) {
        dev.find_spec = find;
    }
    // Dread stores its inventory in floats. Matching integers as well multiplies the
    // coincidences without adding one real candidate: 99 is among the commonest words in any
    // heap, and 99.0f is not.
    if (const char* const only = Common::DSMod::DevEnvironment("EDEN_DSMOD_FINDFLOAT");
        only != nullptr && only[0] == '1') {
        dev.find_float_only = true;
        LOG_INFO(Core, "DSMod: the value search will match float encodings only");
    }
    if (!dev.find_spec.empty()) {
        LOG_INFO(Core, "DSMod: looking for the value cluster '{}'", dev.find_spec);
    }
    dev.auto_start = Common::DSMod::DevEnvironment("EDEN_DSMOD_AUTOSTART") != nullptr;
    if (const char* const path = Common::DSMod::DevEnvironment("EDEN_DSMOD_INPUT"); path != nullptr) {
        std::ifstream f(path);
        std::string token;
        while (f >> token) {
            dev.input_script.push_back(token);
        }
        dev.input_script_loaded = !dev.input_script.empty();
        LOG_INFO(Core, "DSMod: loaded {} input step(s) from {}", dev.input_script.size(), path);
    }
    if (const char* const ms = Common::DSMod::DevEnvironment("EDEN_DSMOD_MOTION"); ms != nullptr) {
        dev.motion_spec = ms;
        LOG_INFO(Core, "DSMod: walking to find a coordinate");
    }
    if (const char* const ad = Common::DSMod::DevEnvironment("EDEN_DSMOD_ARRAY"); ad != nullptr) {
        dev.arraydump_spec = ad;
        LOG_INFO(Core, "DSMod: will map value arrays of class '{}'", dev.arraydump_spec);
    }
    if (const char* const cd = Common::DSMod::DevEnvironment("EDEN_DSMOD_CLASSDUMP"); cd != nullptr) {
        dev.classdump_spec = cd;
        LOG_INFO(Core, "DSMod: will dump objects of class '{}'", dev.classdump_spec);
    }
    if (const char* const pm = Common::DSMod::DevEnvironment("EDEN_DSMOD_PAIRMAX"); pm != nullptr) {
        dev.range_pair_max = std::strtof(pm, nullptr);
        LOG_INFO(Core, "DSMod: a watched counter must be followed by {:g}", dev.range_pair_max);
    }
    if (const char* const rw = Common::DSMod::DevEnvironment("EDEN_DSMOD_RANGE"); rw != nullptr) {
        dev.range_spec = rw;
        LOG_INFO(Core, "DSMod: watching whole-numbered floats in '{}'", dev.range_spec);
    }
    if (const char* const fp = Common::DSMod::DevEnvironment("EDEN_DSMOD_FIELD"); fp != nullptr) {
        dev.field_spec = fp;
        LOG_INFO(Core, "DSMod: field probe '{}'", dev.field_spec);
    }
    if (const char* const hf = Common::DSMod::DevEnvironment("EDEN_DSMOD_HEAPFIND"); hf != nullptr) {
        dev.heapfind_spec = hf;
        LOG_INFO(Core, "DSMod: heap search for '{}'", dev.heapfind_spec);
    }
    if (const char* const diff = Common::DSMod::DevEnvironment("EDEN_DSMOD_DIFF"); diff != nullptr) {
        dev.diff_spec = diff;
        LOG_INFO(Core, "DSMod: fire/rest differential search around '{}'", dev.diff_spec);
    }
    if (const char* const scan = Common::DSMod::DevEnvironment("EDEN_DSMOD_SCAN"); scan != nullptr) {
        dev.scan_spec = scan;
        LOG_INFO(Core, "DSMod: scanning for moving floats '{}'", dev.scan_spec);
    }
    if (const char* const spec = Common::DSMod::DevEnvironment("EDEN_DSMOD_DUMP"); spec != nullptr) {
        dev.dump_spec = spec;
        LOG_INFO(Core, "DSMod: watching memory '{}'", dev.dump_spec);
    }
#endif
    {
        // Enable the GPU composite path via the env var (desktop testing) OR a manifest "flags"
        // entry "gpu_composite": true (the reliable switch on Android, where the app gets no shell
        // env -- push a manifest with the flag on to turn it on for a device).
        const char* const gc = Common::DSMod::DevEnvironment("EDEN_DSMOD_GPU_COMPOSITE");
        const bool env_gc = gc != nullptr && gc[0] == '1';
        const bool env_off = gc != nullptr && gc[0] == '0'; // explicit "0" overrides the manifest
        const auto gcf = manifest.flag_defaults.find("gpu_composite");
        const bool manifest_gc = gcf != manifest.flag_defaults.end() && gcf->second;
        if (!env_off && (env_gc || manifest_gc)) {
            gpu_composite_mode = true;
            system.GPU().DSModAux().gpu_composite.store(true);
            LOG_INFO(Core, "DSMod: GPU composite path enabled (via {})",
                     env_gc ? "env" : "manifest flag");
        }
    }
    LOG_INFO(Core, "DSMod: runtime started for '{}'", manifest.name);
}

void ModRuntime::DumpWatchedMemory() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DumpWatchedMemoryImpl();
#endif
}

void ModRuntime::DumpOneWatch([[maybe_unused]] const std::string& spec) {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DumpOneWatchImpl(spec);
#endif
}

void ModRuntime::ScanForMovingFloats() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    ScanForMovingFloatsImpl();
#endif
}

void ModRuntime::DumpHeapSnapshot() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DumpHeapSnapshotImpl();
#endif
}

void ModRuntime::TraceChainTo([[maybe_unused]] VAddr target) {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    TraceChainToImpl(target);
#endif
}

void ModRuntime::RecheckRoutes() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    RecheckRoutesImpl();
#endif
}

void ModRuntime::PathFromStatics([[maybe_unused]] VAddr target, [[maybe_unused]] int depth,
                                 [[maybe_unused]] s64 slack) const {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    PathFromStaticsImpl(target, depth, slack);
#endif
}

void ModRuntime::TraceToStatic([[maybe_unused]] VAddr target, [[maybe_unused]] int depth,
                               [[maybe_unused]] s64 slack) const {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    TraceToStaticImpl(target, depth, slack);
#endif
}

void ModRuntime::TraceWithSlack([[maybe_unused]] VAddr object, [[maybe_unused]] s64 slack) const {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    TraceWithSlackImpl(object, slack);
#endif
}

void ModRuntime::MotionScan() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    MotionScanImpl();
#endif
}

void ModRuntime::ArrayDump() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    ArrayDumpImpl();
#endif
}

void ModRuntime::ClassDump() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    ClassDumpImpl();
#endif
}

void ModRuntime::RangeWatch() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    RangeWatchImpl();
#endif
}

void ModRuntime::FieldProbe() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    FieldProbeImpl();
#endif
}

void ModRuntime::HeapFind() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    HeapFindImpl();
#endif
}

void ModRuntime::DiffScan() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DiffScanImpl();
#endif
}

void ModRuntime::FindValueCluster() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    FindValueClusterImpl();
#endif
}

void ModRuntime::NarrowByAgreement() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    NarrowByAgreementImpl();
#endif
}

void ModRuntime::DumpLuaRegistry() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DumpLuaRegistryImpl();
#endif
}

void ModRuntime::DescribeStringUses([[maybe_unused]] const std::string& text) {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DescribeStringUsesImpl(text);
#endif
}

void ModRuntime::DriveAutoChain() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DriveAutoChainImpl();
#endif
}

void ModRuntime::DriveCmd() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    DriveCmdImpl();
#endif
}

void ModRuntime::Tick() {
    static thread_local RuntimeStageStats stats;
    const RuntimeStageTimer timer{stats, "tick"};
    ++tick_count;
    if (module_areas_ready.load(std::memory_order_acquire)) {
        InstallModuleAreas();
    }
    DrainModuleImages();
    bridge_tick_count.store(tick_count, std::memory_order_release);
    DrainGuestBridgeResults();
    // Module map publications a map raster held back (AcceptModuleMap): applied as soon as the
    // redraw worker lets go of map_state_mutex, before this tick samples anything.
    ApplyPendingMapUpdates();

    // No explored-map persistence: the live minimap grid is the source of truth and reflects the
    // loaded save within a couple of seconds, so a persisted explored.bin only risks showing stale
    // cells from a prior session/save that the current save has not explored (the bottom map then
    // reveals more than the game's own minimap). The self-tracked MarkVisitedAt fallback still runs
    // in-session when the grid can't be read; it just no longer survives a restart.

#if EDEN_DSMOD_BUILD_DEV_TOOLS
    // Headless entry into gameplay: pulse the buttons that get a save loaded and a cutscene
    // skipped, until the game reports a player. Only for unattended runs -- it stops the moment
    // gameplay starts, so it cannot fight a person at the controls.
    // The press loop must judge gameplay honestly: with ASSUME_GAMEPLAY set (which unparks the
    // search tools for a game with no points yet) InGameplay() is always true, and the very
    // override that enables exploration silently disabled the presses that make it possible.
    static const u64 autostart_ticks = [] {
        const char* const v = Common::DSMod::DevEnvironment("EDEN_DSMOD_AUTOSTART_TICKS");
        return v != nullptr ? std::strtoull(v, nullptr, 0) : u64{60 * 60 * 30};
    }();
    if (dev.auto_start && !InGameplayHonest() && tick_count < autostart_ticks) {
        if (auto* const pad = system.GetInputSubsystem() != nullptr
                                  ? system.GetInputSubsystem()->GetVirtualGamepad()
                                  : nullptr) {
            constexpr u64 Period = 45; // three quarters of a second per press
            // The pattern is the env value itself when it names buttons ("A,A,Plus"): games
            // differ in what their menus need, and B in the wrong place backs out of them.
            static const std::vector<std::string> keys = [] {
                std::vector<std::string> parsed;
                const char* const spec = Common::DSMod::DevEnvironment("EDEN_DSMOD_AUTOSTART");
                if (spec != nullptr && std::strchr(spec, ',') != nullptr) {
                    std::string token;
                    for (const char* c = spec;; ++c) {
                        if (*c == ',' || *c == '\0') {
                            if (!token.empty())
                                parsed.push_back(token);
                            token.clear();
                            if (*c == '\0')
                                break;
                        } else {
                            token.push_back(*c);
                        }
                    }
                }
                if (parsed.empty())
                    parsed = {"A", "Plus", "A", "B"};
                return parsed;
            }();
            const auto* const name = keys[(tick_count / Period) % keys.size()].c_str();
            bool ok{};
            const auto button = ParseButton(name, ok);
            if (ok) {
                const bool held = (tick_count % Period) < 6;
                pad->SetButtonState(0, button, held);
                pad->SetButtonState(8, button, held);
            }
        }
    }

    // A one-shot input script: EDEN_DSMOD_INPUT names a file of whitespace-separated tokens,
    // each "BUTTON" or "BUTTON:ticks" (default 6 held / then Gap released). Unlike AUTOSTART's
    // loop this plays once, so it can complete a form -- pick a field, choose a value, confirm --
    // which a repeating pattern never can. Stick directions: LX+/LX-/LY+/LY- (held as a deflect).
    if (dev.input_script_loaded) {
        DriveInputScript();
    }
    DriveLiveInput();
#endif
    DriveCmd();
    DriveAutoChain();
    // Input ownership must age independently of the aux surface. A display can disappear while a
    // virtual button is held; returning before this update used to leave the guest input stuck.
    UpdateHeldButtons();

#if EDEN_DSMOD_BUILD_DEV_TOOLS
    TickAutoMgrFindImpl();
#endif

#if EDEN_DSMOD_BUILD_DEV_TOOLS
    // Diagnostic: where are the guest threads? Useful when a title hangs before it ever presents.
    if (dev.log_guest_threads && (tick_count % 180) == 0) {
        if (auto* const process = system.ApplicationProcess(); process != nullptr) {
            std::string summary;
            int listed = 0;
            for (const auto& thread : process->GetThreadList()) {
                const u64 pc = thread.GetContext().pc;
                const s64 rel = static_cast<s64>(pc) - static_cast<s64>(main_region_begin);
                summary += fmt::format(" [{}:{:X}{}]", thread.GetThreadId(), pc,
                                       (rel >= 0 && rel < static_cast<s64>(main_region_size))
                                           ? fmt::format(" main+{:X}", rel)
                                           : std::string{});
                if (++listed >= 12) {
                    break;
                }
            }
            LOG_INFO(Core, "DSMod guest threads:{}", summary);
        }
    }
#endif

    auto& aux = system.GPU().DSModAux();
    if (!aux.present.load()) {
        // Action-capable modules may have an asynchronous guest mailbox to retire even while the
        // auxiliary surface is hidden. Keep that cheap extension tick alive without running the
        // declarative sampler or legacy modules such as Dread, whose tick performs full scans.
        if (ModuleTicksWhileHidden()) {
            StateSnapshot& snapshot = tick_snapshot;
            snapshot.ints.clear();
            snapshot.texts.clear();
            snapshot.floats.clear();
            snapshot.addresses.clear();
            RunGameModule(snapshot, true);
        }
        // What the package does to the game itself must not depend on whether a second display
        // exists (lid closed at boot, one-display setups): code patches, spies, and the watchdog
        // for a call that went in just before the surface vanished. With the surface shown these
        // run from ApplyEnforceRules / after the page binds, exactly as before.
        MaintainGuestBridge();
        ArmSpies();
        NavOff("second screen hidden"); // runtime 17: the game gets its controller back
        return;
    }

    StateSnapshot& snapshot = tick_snapshot; // reused: clear() keeps capacity, no per-tick alloc
    snapshot.ints.clear();
    snapshot.texts.clear();
    snapshot.floats.clear();
    snapshot.addresses.clear();
    SampleState(snapshot);

    // Capture the second screen's touch points now, then interpret them after game-specific
    // sampling has populated every visibility gate used by drawing and tap hit-testing.
    std::array<VideoCore::DSMod::AuxTouchPoint, VideoCore::DSMod::AuxRouting::MaxTouch> points{};
    const u32 panel_w = std::max(1u, aux.width.load());
    const u32 panel_h = std::max(1u, aux.height.load());
    const u32 cw = canvas.Width() != 0 ? canvas.Width() : panel_w;
    const u32 ch = canvas.Height() != 0 ? canvas.Height() : panel_h;
    aux.SetCanvasRect(TouchRect(panel_w, panel_h, cw, ch)); // for frontends' scripted touches
    DriveCmdDrag(panel_w, panel_h, cw, ch); // console "drag": feeds the touch read just below
    const size_t count = aux.GetTouch(points);

    RunGameModule(snapshot, true);
    WatchFontEpoch(snapshot);  // runtime 18: the module may ask for its font again
    PublishFlags(snapshot);    // runtime flags are sources for derived values too
    if (manifest.uses_clock_keys) {
        // Runtime 16 @clock.* / @game.seconds, also derived sources. Only for a package that
        // names them: they change every second and would otherwise churn UiSignature.
        clock_points.Publish(snapshot);
    }
    EvaluateDerived(snapshot); // after every source (points, sequences, module) is published
    // View gestures are a shared host feature, available to every package. Bound default views
    // first (unreleased runtime 15 addition): "custom" is measured against each view's home
    // (zoom 1 / no pan without one).
    // Runtime 17 auto_w: from here on (hit tests, the dirty scan, the synchronous draw) text-sized
    // boxes are measured on the tick canvas, with its fonts, as the page draws them.
    const TextProvider measure_texts = [this](const std::string& ref) { return GetMsbtText(ref); };
    const TextMeasureScope text_measure{canvas, &measure_texts};
    ApplyViewDefaults(snapshot);
    {
        std::scoped_lock lock{view_mutex};
        for (const auto& [key, view] : view_state) {
            snapshot.ints[view_custom_keys(key)] = ViewAwayFromHome(view) ? 1 : 0;
        }
    }

    // Map selections etc. before hit-testing. The flags were published above and nothing since
    // has changed them, so this pass skips republishing them.
    PublishMapState(snapshot, false);
    PublishScroll(snapshot, true); // list offsets as drawn: taps hit-test the rows on screen
    UpdateGestures(snapshot, points, count, panel_w, panel_h, cw, ch);
    ReturnIdleViews();
    UpdateNav(snapshot); // runtime 17: controller focus mode; its A press is a tap drained below
    if (HasPendingInput()) {
        // Runtime 16: the taps' gates (enabled_bind and the derived values they read) see this
        // tick's "@sel:" / "@drag*", the drag being dropped included.
        PublishInteraction(snapshot, true);
        if (!manifest.derived.empty() && derived_order_list == manifest.derived.data() &&
            derived_order_size == manifest.derived.size() && derived_reads_interaction) {
            EvaluateDerived(snapshot, true);
        }
    }
    DrainTaps(snapshot);
    FlushHaptic();                // at most one per tick, handed to the frontend's notifier thread
    PublishInteraction(snapshot); // selection / drag state as of this tick, for the page
    PublishMapState(snapshot);    // ...and the flags / map selections the taps just changed
    PublishScroll(snapshot, false); // ...and the list offsets this tick's gesture moved
    // Derived points that read "@sel:", "@last:" or "@drag*" see this tick's interaction state.
    // (Which ones those are is fixed per list: EvaluateDerived, already run this tick, keeps it.)
    if (!manifest.derived.empty() && derived_order_list == manifest.derived.data() &&
        derived_order_size == manifest.derived.size() && derived_reads_interaction) {
        EvaluateDerived(snapshot, true);
    }
    ApplyEnforceRules(snapshot);
    {
        // Edge-triggered automatic page switches. Own stage so its true per-tick
        // cost is visible (EDEN_DSMOD_PROFILE) instead of hiding inside the whole-tick average.
        static thread_local RuntimeStageStats page_bind_stats;
        const RuntimeStageTimer page_bind_timer{page_bind_stats, "page-binds"};
        DrivePageBinds(snapshot);
    }
    ArmSpies();
    DiffScan();
    HeapFind();
    FieldProbe();
    RangeWatch();
    ClassDump();
    ArrayDump();
    MotionScan();

    // Periodic snapshot line: on a real game the second screen may be on hardware we cannot
    // screenshot, and this makes the reads verifiable from the log alone.
    if ((tick_count % 180) == 0 && !manifest.points.empty()) {
        // Report whatever this package actually declares. The names used to be hard-coded to
        // one game's, so every other title logged a row of FAIL that said nothing about itself.
        std::string summary;
        int shown = 0;
        for (const auto& [name, point] : manifest.points) {
            if (shown++ >= 24) {
                break;
            }
            // A value may come from a reader other than the point's pointer chain (the map
            // marker's player_x/y come from the minimap-grid tracker), so report the value
            // whenever one exists and say "unresolved" only when there is neither.
            if (const auto number = snapshot.floats.find(name); number != snapshot.floats.end()) {
                summary += fmt::format("{}={:g} ", name, number->second);
            } else if (const auto whole = snapshot.ints.find(name); whole != snapshot.ints.end()) {
                summary += fmt::format("{}={} ", name, whole->second);
            } else if (const auto address = snapshot.addresses.find(name);
                       address == snapshot.addresses.end() || address->second == 0) {
                summary += fmt::format("{}=unresolved ", name);
            } else {
                summary += fmt::format("{}=? ", name);
            }
        }
        LOG_INFO(Core, "DSMod snapshot: {}", summary);
    }

    // Redraw every tick (~60 Hz) so the marker tracks smoothly. This is affordable now that the
    // aux screen presents on its own thread (force_present_thread) instead of blocking the game's
    // render thread once per frame -- that per-frame present, not this redraw, was the fps cost.
    // Runtime 17: Chart widgets sample their bound values (after every source and derived value).
    chart_sampler.Update(manifest,
                         static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count()),
                         snapshot);
    PublishNxFades(snapshot);
    PublishUi(snapshot);
}

} // namespace Core::Mods
