// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Guest-call bridge (Dynarmic only): borrowing a game thread to call guest functions, spies on
// guest calls, instruction patches, and symbol resolution.
//   - Symbols: ResolveSymbol (a manifest "symbols" entry: byte pattern, fixed address, or IL2CPP
//     class + method), with ScanPattern and ScanRange here and the IL2CPP lookups
//     (ResolveMethodByName, FindClassSlotByName, DetectMethodsOffset, MethodTableLooksReal) in
//     engine_il2cpp.cpp. ResolvePoint (mod_state.cpp) uses the class-slot and pattern lookups for
//     points too.
//   - Calls: RequestCall arms a BRK at the per-frame hook; OnGuestBreakpoint redirects pc to the
//     target with lr at a trampoline and restores the saved context when the call traps back.
//     StartSequence / PrepareCallStep / ResolveCallArg / RunPolledSequences chain calls into
//     sequences; ArmSpies arms breakpoints that record a value when the game makes a call.
//   - ApplyPatches (manifest "patches", written once; optional ones only with
//     EDEN_DSMOD_PATCHES=1), InstallBreakpoint / RemoveBreakpoint.
//   - DrainGuestBridgeResults: moves breakpoint-thread results into the tick-owned maps.
// Not here: publishing those results (SampleState) and the stuck-call timeout
// (ApplyEnforceRules), both in mod_state.cpp.
// Flow: DrainGuestBridgeResults runs at the top of Tick; patches and polled sequences run from
// ApplyEnforceRules; RequestCall from RunAction; ArmSpies after the page binds. Threads: the tick
// thread, except OnGuestBreakpoint, which runs on the guest CPU core thread that hit the BRK
// (physical_core.cpp). guest_bridge_mutex (recursive) serialises the bridge state between them;
// call_state is atomic. Under NCE guest_bridge_supported is false and no BRK is ever inserted.

#include <algorithm>
#include <cmath>
#include <cstring>

#include <cstdlib>
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/mods/mod_hooks.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "video_core/gpu.h"

namespace Core::Mods {

void ModRuntime::DrainGuestBridgeResults() {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    for (auto& [name, value] : pending_sequence_values) {
        sequence_values.insert_or_assign(name, value);
    }
    pending_sequence_values.clear();
    for (auto& [name, value] : pending_sequence_addresses) {
        sequence_addresses[name] = value;
    }
    pending_sequence_addresses.clear();
    for (auto& [name, value] : pending_sequence_texts) {
        sequence_texts[name] = std::move(value);
    }
    pending_sequence_texts.clear();
    for (const auto& [name, value] : pending_spy_values) {
        spy_values[name] = value;
    }
    pending_spy_values.clear();
}

/// Searches an arbitrary guest range for a pattern. Returns the offset from `base`.
std::optional<u64> ModRuntime::ScanRange(VAddr base, u64 size, const PatternFind& find) const {
    if (!find.Valid() || size == 0) {
        return std::nullopt;
    }
    auto& memory = system.ApplicationMemory();
    constexpr u64 ChunkSize = 1024 * 1024;
    const size_t needle = find.bytes.size();
    std::vector<u8> chunk(ChunkSize + needle);
    u32 seen = 0;
    for (u64 offset = 0; offset + needle <= size; offset += ChunkSize) {
        const u64 span = std::min<u64>(ChunkSize + needle, size - offset);
        if (!AddressIsSane(base + offset, span)) {
            continue;
        }
        memory.ReadBlock(base + offset, chunk.data(), span);
        for (u64 i = 0; i + needle <= span; ++i) {
            bool hit = true;
            for (size_t j = 0; j < needle; ++j) {
                if (find.mask[j] && chunk[i + j] != find.bytes[j]) {
                    hit = false;
                    break;
                }
            }
            if (hit && seen++ >= find.index) {
                return offset + i;
            }
        }
    }
    return std::nullopt;
}

/// Searches the main module for a pattern. Results are cached: a scan reads the whole module.
std::optional<s64> ModRuntime::ScanPattern(const PatternFind& find) const {
    if (!find.Valid() || main_region_begin == 0 || main_region_size == 0) {
        return std::nullopt;
    }
    const std::string key =
        (find.heap ? "H:" : "M:") +
        std::string{reinterpret_cast<const char*>(find.bytes.data()), find.bytes.size()};
    if (const auto cached = pattern_cache.find(key); cached != pattern_cache.end()) {
        if (cached->second == 0) {
            return std::nullopt; // a remembered miss
        }
        return cached->second;
    }

    auto& memory = system.ApplicationMemory();
    constexpr u64 ChunkSize = 1024 * 1024;
    const size_t needle = find.bytes.size();
    std::vector<u8> chunk(ChunkSize + needle);
    u32 seen = 0;

    // A signature drawn from live data lives in the heap, not the executable. The heap is also
    // sparse, so it has to be walked a page at a time: asking whether a whole megabyte is mapped
    // fails almost everywhere and silently skips the very memory being searched.
    const VAddr HeapBegin = HeapLow();
    const VAddr HeapEnd = HeapHigh();
    if (find.heap) {
        // Not before there is a game to search. A signature made of live data does not exist
        // during boot, and a miss recorded then would be cached and never retried -- the same
        // mistake as scanning a menu, made permanent.
        if (!InGameplay()) {
            return std::nullopt;
        }
        std::vector<u8> run;
        VAddr run_begin = 0;
        const auto search_run = [&]() -> std::optional<s64> {
            for (u64 i = 0; i + needle <= run.size(); ++i) {
                bool hit = true;
                for (size_t j = 0; j < needle; ++j) {
                    if (find.mask[j] && run[i + j] != find.bytes[j]) {
                        hit = false;
                        break;
                    }
                }
                if (hit && seen++ >= find.index) {
                    return static_cast<s64>(run_begin + i) + find.offset;
                }
            }
            return std::nullopt;
        };
        for (VAddr page = HeapBegin; page < HeapEnd; page += 0x1000) {
            const u8* const host = memory.GetPointerSilent(page);
            if (host == nullptr) {
                // A gap breaks the run: bytes either side of it are not neighbours.
                if (const auto found = search_run()) {
                    pattern_cache[key] = *found;
                    LOG_INFO(Core,
                             "DSMod: pattern ({} bytes, index {}) found in the heap at "
                             "{:016X}",
                             needle, find.index, *found);
                    return *found;
                }
                run.clear();
                continue;
            }
            if (run.empty()) {
                run_begin = page;
            }
            run.insert(run.end(), host, host + 0x1000);
            // Keep runs bounded; carry the tail so a match across the seam is still seen.
            if (run.size() >= (16u << 20)) {
                if (const auto found = search_run()) {
                    pattern_cache[key] = *found;
                    LOG_INFO(Core,
                             "DSMod: pattern ({} bytes, index {}) found in the heap at "
                             "{:016X}",
                             needle, find.index, *found);
                    return *found;
                }
                const size_t keep = needle - 1;
                run.erase(run.begin(), run.end() - static_cast<long>(keep));
                run_begin = page + 0x1000 - keep;
            }
        }
        if (const auto found = search_run()) {
            pattern_cache[key] = *found;
            LOG_INFO(Core, "DSMod: pattern ({} bytes, index {}) found in the heap at {:016X}",
                     needle, find.index, *found);
            return *found;
        }
        // Remember the miss. Rescanning a gigabyte every frame for something that is not there
        // costs more than the search it is meant to serve.
        pattern_cache[key] = 0;
        LOG_WARNING(Core, "DSMod: pattern ({} bytes) not found in the heap", needle);
        return std::nullopt;
    }
    const VAddr scan_begin = main_region_begin;
    const u64 scan_size = main_region_size;

    for (u64 base = 0; base + needle <= scan_size; base += ChunkSize) {
        const u64 span = std::min<u64>(ChunkSize + needle, scan_size - base);
        if (!AddressIsSane(scan_begin + base, span)) {
            continue;
        }
        memory.ReadBlock(scan_begin + base, chunk.data(), span);
        for (u64 i = 0; i + needle <= span; ++i) {
            bool hit = true;
            for (size_t j = 0; j < needle; ++j) {
                if (find.mask[j] && chunk[i + j] != find.bytes[j]) {
                    hit = false;
                    break;
                }
            }
            if (!hit) {
                continue;
            }
            if (seen++ < find.index) {
                continue;
            }
            // A module match is reported relative to the module, because that is what stays
            // true between runs. A heap match cannot be: it is wherever the allocator put it
            // this time, so it is reported absolute and the caller must not add a base to it.
            const s64 result = find.heap ? static_cast<s64>(scan_begin + base + i) + find.offset
                                         : static_cast<s64>(base + i) + find.offset;
            pattern_cache[key] = result;
            LOG_INFO(Core, "DSMod: pattern ({} bytes, index {}) found at {}{:X}", needle,
                     find.index, find.heap ? "" : "main+", result);
            return result;
        }
    }
    pattern_cache[key] = 0;
    LOG_WARNING(Core, "DSMod: pattern ({} bytes) not found in the main module", needle);
    return std::nullopt;
}

std::optional<s64> ModRuntime::ResolveSymbol(const std::string& name) {
    if (const auto cached = symbol_cache.find(name); cached != symbol_cache.end()) {
        return cached->second;
    }
    VAddr method_info{};
    const auto it = manifest.symbols.find(name);
    if (it == manifest.symbols.end()) {
        return std::nullopt;
    }
    if (it->second.find.Valid()) {
        const auto scanned = ScanPattern(it->second.find);
        if (scanned) {
            symbol_cache[name] = *scanned;
        }
        return scanned;
    }
    if (it->second.address != 0) {
        return it->second.address;
    }
    s64 class_slot = it->second.class_slot;
    if (!it->second.class_name.empty()) {
        class_slot = FindClassSlotByName(it->second.class_name).value_or(0);
    }
    if (class_slot == 0 || it->second.method.empty()) {
        return std::nullopt;
    }
    const auto resolved = ResolveMethodByName(class_slot, it->second.method, method_info);
    if (resolved) {
        symbol_cache[name] = *resolved;
        // IL2CPP passes the MethodInfo* as the trailing argument. Managed methods usually ignore
        // it, but the wrappers around engine icalls -- GameObject::SetActive among them -- read it,
        // and calling one with a null MethodInfo takes the game down.
        method_info_cache[name] = method_info;
    }
    return resolved;
}

bool ModRuntime::InstallBreakpoint(VAddr address) {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    if (!guest_bridge_supported) {
        return false;
    }
    auto& memory = system.ApplicationMemory();
    if (!AddressIsSane(address, sizeof(u32))) {
        LOG_ERROR(Core, "DSMod: cannot hook unmapped address {:016X}", address);
        return false;
    }
    // Order matters: dynarmic decides whether a block honours breakpoints when it compiles the
    // block, so the gate has to be open before the patch is visible and the cache invalidated.
    g_guest_hooks_enabled.store(true, std::memory_order_relaxed);
    constexpr u32 BrkInstruction = 0xD4200000; // A64: brk #0
    const u32 present = memory.Read32(address);
    if (present == BrkInstruction) {
        // Already ours. Never record this as the original instruction.
        const auto known = patched_original.find(address);
        if (known == patched_original.end()) {
            LOG_ERROR(Core, "DSMod: {:016X} already holds a breakpoint we cannot account for",
                      address);
            return false;
        }
        original_instruction = known->second;
        return true;
    }
    original_instruction = present;
    patched_original[address] = present;
    memory.Write32(address, BrkInstruction);
    Core::InvalidateInstructionCacheRange(system.ApplicationProcess(), address, sizeof(u32));
    return true;
}

void ModRuntime::RemoveBreakpoint(VAddr address) {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    auto& memory = system.ApplicationMemory();
    if (!AddressIsSane(address, sizeof(u32))) {
        return;
    }
    u32 restore = original_instruction;
    if (const auto known = patched_original.find(address); known != patched_original.end()) {
        restore = known->second; // what the address really held, whoever patched it
        patched_original.erase(known);
    }
    memory.Write32(address, restore);
    Core::InvalidateInstructionCacheRange(system.ApplicationProcess(), address, sizeof(u32));
}

void ModRuntime::RequestCall(const Action& action, const StateSnapshot& snapshot) {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    if (!guest_bridge_supported) {
        return;
    }
    if (call_state != CallState::Idle) {
        LOG_INFO(Core, "DSMod: a guest call is already in flight ({} ticks), ignoring",
                 tick_count - call_started_tick);
        return;
    }
    s64 hook_offset = manifest.frame_hook;
    if (!manifest.frame_hook_symbol.empty()) {
        hook_offset = ResolveSymbol(manifest.frame_hook_symbol).value_or(0);
    }
    s64 fn_offset = action.call_fn;
    if (!action.call_fn_symbol.empty()) {
        const auto resolved = ResolveSymbol(action.call_fn_symbol);
        if (!resolved) {
            LOG_ERROR(Core,
                      "DSMod: could not resolve '${}' -- wrong data file for this build, or the "
                      "class is not created yet (load a save first)",
                      action.call_fn_symbol);
            return;
        }
        fn_offset = *resolved;
    }
    if (hook_offset == 0 || fn_offset == 0) {
        LOG_ERROR(Core, "DSMod: call needs both a manifest 'frame_hook' and an action 'fn'");
        return;
    }
    call_target = main_region_begin + static_cast<VAddr>(fn_offset);
    call_args.fill(0);
    for (size_t i = 0; i < action.args.size() && i < call_args.size(); ++i) {
        const auto& arg = action.args[i];
        if (!arg.empty() && arg.front() == '$') {
            const auto name = arg.substr(1);
            // "$point" passes the point's value; "$&point" passes its address.
            if (!name.empty() && name.front() == '&') {
                const auto addr_it = snapshot.addresses.find(name.substr(1));
                call_args[i] = addr_it == snapshot.addresses.end() ? 0 : addr_it->second;
            } else {
                call_args[i] = static_cast<u64>(snapshot.GetInt(name));
            }
        } else {
            call_args[i] = static_cast<u64>(std::strtoll(arg.c_str(), nullptr, 0));
        }
    }
    // Append the MethodInfo* IL2CPP expects as the trailing parameter, in the register after the
    // declared arguments.
    if (!action.call_fn_symbol.empty()) {
        if (const auto info = method_info_cache.find(action.call_fn_symbol);
            info != method_info_cache.end() && info->second != 0) {
            const size_t slot = std::min(action.args.size(), call_args.size() - 1);
            call_args[slot] = info->second;
        }
    }
    resolved_hook = main_region_begin + static_cast<VAddr>(hook_offset);
    if (!InstallBreakpoint(resolved_hook)) {
        return;
    }
    hook_original = original_instruction;
    call_seq = nullptr;
    call_state = CallState::Armed;
    call_started_tick = tick_count;
    LOG_INFO(Core,
             "DSMod: guest call armed, fn={:016X} (main+{:X}) hook={:016X} x0={:X} x1={:X} x2={:X}",
             call_target, call_target - main_region_begin, resolved_hook, call_args[0],
             call_args[1], call_args[2]);
}

/// One argument of one step. The forms exist because a composed VM call needs values from four
/// different places: the state handed to us by the hook, a literal that lives in the module, what
/// the previous step returned, and what an earlier step put aside.
u64 ModRuntime::ResolveCallArg(const std::string& arg) {
    if (arg.empty() || arg.front() != '$') {
        return static_cast<u64>(std::strtoll(arg.c_str(), nullptr, 0));
    }
    const std::string name = arg.substr(1);
    if (name == "L") {
        if (const auto it = call_slots.find("L"); it != call_slots.end()) {
            return it->second;
        }
        if (const auto pending = pending_spy_values.find("lua_state");
            pending != pending_spy_values.end()) {
            return pending->second;
        }
        const auto spy = spy_values.find("lua_state"); // fallback: whatever a spy caught earlier
        return spy == spy_values.end() ? 0 : spy->second;
    }
    if (name == "ret") {
        return static_cast<u64>(last_call_result);
    }
    if (name.starts_with("#")) {
        const auto it = call_slots.find(name.substr(1));
        return it == call_slots.end() ? 0 : it->second;
    }
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        // $"text": a string literal the module does not carry (a start point name, an actor
        // name). It is staged well below the borrowed thread's stack pointer -- memory that
        // thread owns and is not using -- and only has to outlive the one call that copies it
        // (lua_pushstring makes its own TString). Eight rolling 512-byte slots, 32 KB down.
        const std::string text = name.substr(1, name.size() - 2);
        const u32 slot = literal_slot++ % 8u;
        const VAddr addr = saved_context.sp - 0x8000 - static_cast<VAddr>(slot) * 0x200;
        if (saved_context.sp == 0 || text.size() >= 0x200 || !AddressIsSane(addr, 0x200)) {
            LOG_WARNING(Core, "DSMod: cannot stage literal \"{}\" below sp {:016X}", text,
                        saved_context.sp);
            return 0;
        }
        std::string bytes = text;
        bytes.push_back('\0');
        system.ApplicationMemory().WriteBlock(addr, bytes.data(), bytes.size());
        LOG_INFO(Core, "DSMod: literal \"{}\" staged at {:016X} (sp {:016X})", text, addr,
                 saved_context.sp);
        return addr;
    }
    if (name.starts_with("@")) {
        // A pointer to something in the module -- a string literal, most usefully. Resolved when
        // the sequence was armed (StartSequence, tick thread): this runs on the guest CPU thread
        // inside the breakpoint handler, where a symbol lookup may scan the module and touches
        // caches the tick thread owns.
        const auto it = call_seq_symbols.find(name.substr(1));
        return it != call_seq_symbols.end() && it->second
                   ? main_region_begin + static_cast<VAddr>(*it->second)
                   : 0;
    }
    if (name.starts_with("&")) {
        const auto it = call_snapshot.addresses.find(name.substr(1));
        return it == call_snapshot.addresses.end() ? 0 : it->second;
    }
    return static_cast<u64>(call_snapshot.GetInt(name));
}

/// Point the machinery at the current step. Returns false if its symbol will not resolve, which
/// aborts the sequence rather than jumping somewhere arbitrary.
bool ModRuntime::PrepareCallStep() {
    const CallStep& step = call_seq->steps[call_seq_step];
    // Resolved at arm time (StartSequence); never looked up here, on the guest CPU thread.
    const auto off = call_seq_step < call_seq_fn.size() ? call_seq_fn[call_seq_step]
                                                        : std::optional<s64>{};
    if (!off) {
        LOG_ERROR(Core,
                  "DSMod: sequence '{}' step {} names '{}', which this build has no address "
                  "for",
                  call_seq_name, call_seq_step, step.fn);
        return false;
    }
    call_target = main_region_begin + static_cast<VAddr>(*off);
    call_args.fill(0);
    for (size_t i = 0; i < step.args.size() && i < call_args.size(); ++i) {
        call_args[i] = ResolveCallArg(step.args[i]);
    }
    return true;
}

/// Arm a sequence. Like a single call it waits for the hook, but the thread is then kept for the
/// whole chain instead of being handed back after one call.
void ModRuntime::StartSequence(const std::string& name, const CallSequence& sequence,
                               const StateSnapshot& snapshot) {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    if (!guest_bridge_supported) {
        return;
    }
    if (call_state != CallState::Idle || sequence.steps.empty()) {
        return;
    }
    s64 hook_offset = manifest.frame_hook;
    if (!manifest.frame_hook_symbol.empty()) {
        hook_offset = ResolveSymbol(manifest.frame_hook_symbol).value_or(0);
    }
    if (hook_offset == 0) {
        LOG_ERROR(Core, "DSMod: a sequence needs a manifest 'frame_hook' to borrow a thread at");
        return;
    }
    // Every step's function and every "$@symbol" argument is resolved here, on the tick thread,
    // before anything is armed: the steps run on the guest CPU thread inside the breakpoint
    // handler, which must neither scan the module nor write the symbol/pattern caches. A step
    // that does not resolve refuses the whole sequence now instead of abandoning it mid-chain.
    std::vector<std::optional<s64>> step_fns;
    std::unordered_map<std::string, std::optional<s64>> symbols;
    step_fns.reserve(sequence.steps.size());
    for (size_t i = 0; i < sequence.steps.size(); ++i) {
        const CallStep& step = sequence.steps[i];
        auto off = ResolveSymbol(step.fn);
        if (!off) {
            LOG_ERROR(Core,
                      "DSMod: sequence '{}' step {} names '{}', which this build has no address "
                      "for",
                      name, i, step.fn);
            return;
        }
        step_fns.push_back(off);
        for (const auto& arg : step.args) {
            if (arg.size() > 2 && arg.starts_with("$@")) {
                auto key = arg.substr(2);
                if (!symbols.contains(key)) {
                    auto resolved = ResolveSymbol(key);
                    symbols.emplace(std::move(key), resolved);
                }
            }
        }
    }
    resolved_hook = main_region_begin + static_cast<VAddr>(hook_offset);
    if (!InstallBreakpoint(resolved_hook)) {
        return;
    }
    hook_original = original_instruction;
    call_seq_fn = std::move(step_fns);
    call_seq_symbols = std::move(symbols);
    call_seq = &sequence;
    call_seq_name = name;
    call_seq_step = 0;
    call_slots.clear();
    call_snapshot = snapshot;
    call_state = CallState::Armed;
    call_started_tick = tick_count;
    if (sequence.every_ms == 0) {
        LOG_INFO(Core, "DSMod: sequence '{}' armed at {:016X} ({} step(s))", name, resolved_hook,
                 sequence.steps.size());
    }
}

/// Sequences that publish a value re-run on a timer. Reading a scripting VM is far too expensive
/// to do every frame -- each step is a breakpoint trap and a thread borrow.
void ModRuntime::RunPolledSequences(const StateSnapshot& snapshot) {
    if (!guest_bridge_supported) {
        return;
    }
    for (const auto& [name, sequence] : manifest.sequences) {
        if (sequence.every_ms == 0 || call_state != CallState::Idle) {
            continue;
        }
        if (!sequence.flag.empty()) {
            // find, not operator[]: a lookup must not create the flag (it would then be published
            // as "@flag:<name>" = 0 as a side effect).
            const auto flag = flags.find(sequence.flag);
            if (flag == flags.end() || flag->second == 0) {
                continue;
            }
        }
        const u64 due = MillisecondsToModTicks(sequence.every_ms);
        auto& last = sequence_last_run[name];
        if (last != 0 && tick_count - last < std::max<u64>(due, 1)) {
            continue;
        }
        last = tick_count;
        StartSequence(name, sequence, snapshot);
        return; // one at a time: only one thread is ever borrowed
    }
}

/// Write the package's code patches over the game, once. Applied from the tick thread the first
/// time the module reads as mapped, which is late enough that the loader has finished with it.
void ModRuntime::ApplyPatches() {
    if (patches_applied || manifest.patches.empty()) {
        return;
    }
    auto& memory = system.ApplicationMemory();
    const char* const opt_in = std::getenv("EDEN_DSMOD_PATCHES");
    const bool apply_optional = opt_in != nullptr && opt_in[0] == '1';
    // Each patch is written (and logged) once. A patch whose address is not mapped yet waits for
    // a later tick; the ones before it are not rewritten every tick meanwhile.
    patch_done.resize(manifest.patches.size(), false);
    bool waiting = false;
    for (size_t index = 0; index < manifest.patches.size(); ++index) {
        const auto& patch = manifest.patches[index];
        if (patch_done[index]) {
            continue;
        }
        if (patch.optional && !apply_optional) {
            LOG_INFO(Core, "DSMod: skipping optional patch at main+{:X} ({})", patch.at, patch.why);
            patch_done[index] = true;
            continue;
        }
        const VAddr address = main_region_begin + static_cast<VAddr>(patch.at);
        if (!AddressIsSane(address, patch.words.size() * sizeof(u32))) {
            waiting = true; // not mapped yet -- try again on a later tick
            continue;
        }
        for (size_t i = 0; i < patch.words.size(); ++i) {
            memory.Write32(address + i * sizeof(u32), patch.words[i]);
        }
        Core::InvalidateInstructionCacheRange(system.ApplicationProcess(), address,
                                              patch.words.size() * sizeof(u32));
        LOG_INFO(Core, "DSMod: patched main+{:X} with {} instruction(s): {}", patch.at,
                 patch.words.size(), patch.why);
        patch_done[index] = true;
    }
    patches_applied = !waiting;
}

/// Patches each declared spy once. The breakpoint stays until the game happens to make the call.
void ModRuntime::ArmSpies() {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    if (!guest_bridge_supported) {
        return;
    }
    for (const auto& spy : manifest.spies) {
        if (spy_values.contains(spy.name) || pending_spy_values.contains(spy.name) ||
            spy_armed.contains(spy.name)) {
            continue;
        }
        const auto resolved =
            ResolveSymbol(spy.symbol.starts_with("$") ? spy.symbol.substr(1) : spy.symbol);
        if (!resolved) {
            continue;
        }
        const VAddr address = main_region_begin + static_cast<VAddr>(*resolved);
        if (!InstallBreakpoint(address)) {
            continue;
        }
        spy_armed[spy.name] = address;
        spy_original[address] = original_instruction;
        LOG_INFO(Core, "DSMod: watching {} at {:016X} for argument x{}", spy.name, address,
                 spy.reg);
    }
}

bool ModRuntime::OnGuestBreakpoint(Kernel::KThread& thread, Core::ArmInterface& arm_interface) {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    auto& ctx = thread.GetContext();
    if (!spy_armed.empty()) {
        for (const auto& [name, address] : spy_armed) {
            if (ctx.pc != address) {
                continue;
            }
            const auto spy = std::ranges::find_if(
                manifest.spies, [&](const SpyPoint& s) { return s.name == name; });
            const u32 reg = spy == manifest.spies.end() ? 0 : spy->reg;
            pending_spy_values[name] = ctx.r[std::min<u32>(reg, 28)];
            // Put the real instruction back and let the call proceed untouched.
            original_instruction = spy_original[address];
            RemoveBreakpoint(address);
            // Keep the whole argument set, not just one register: identifying an unknown function
            // is a question about its signature, and a spy only ever fires once.
            std::string args;
            for (u32 i = 0; i < 8; ++i) {
                args += fmt::format(" x{}={}", i, DescribeGuestValue(ctx.r[i]));
            }
            // d0 as well: a float argument is itself a signature (lua_pushnumber takes one where
            // the integer registers carry nothing).
            double d0{};
            const u64 d0_bits = ctx.v[0][0]; // low half of v0 is d0
            std::memcpy(&d0, &d0_bits, sizeof(d0));
            LOG_INFO(Core, "DSMod: caught {} at main+{:X} lr={}{} d0={}", name,
                     address - main_region_begin, DescribeGuestValue(ctx.lr), args,
                     std::isfinite(d0) ? fmt::format("{:g}", d0) : std::string{"-"});
            spy_armed.erase(name);
            arm_interface.SetContext(ctx);
            return true;
        }
    }
    // A spy can be hit again after we have retired it: a second thread may already have been
    // stopped at the same brk, or a compiled block can still carry it. Falling through would
    // report our own breakpoint as an unhandled exception and take the game down -- which is
    // exactly what a wide sweep makes likely, since it plants dozens of them in hot code.
    // Only ever a last resort, though: a spy and a hook can share an address, and swallowing the
    // trap here would leave the hook waiting for a call that already happened.
    if (call_state == CallState::Idle && return_trampoline != 0 && ctx.pc == return_trampoline &&
        patched_original.contains(return_trampoline)) {
        // A call the watchdog gave up on has come back after all: hand the thread its saved
        // context (the trampoline is the module entry, which must never run again).
        LOG_WARNING(Core, "DSMod: an abandoned guest call returned late; restoring its thread");
        original_instruction = trampoline_original;
        RemoveBreakpoint(return_trampoline);
        ctx = saved_context;
        arm_interface.SetContext(ctx);
        return true;
    }
    const VAddr awaited = call_state == CallState::InCall  ? return_trampoline
                          : call_state == CallState::Armed ? resolved_hook
                                                           : 0;
    if (ctx.pc != awaited) {
        if (const auto retired = patched_original.find(ctx.pc); retired != patched_original.end()) {
            original_instruction = retired->second;
            RemoveBreakpoint(ctx.pc);
            arm_interface.SetContext(ctx);
            return true;
        }
    }
    if (call_state == CallState::Idle) {
        return false;
    }
    const VAddr hook = resolved_hook;
    const VAddr wanted = call_state == CallState::InCall ? return_trampoline : hook;
    if (ctx.pc != wanted) {
        return false; // not our breakpoint
    }

    if (call_state == CallState::Armed) {
        // Borrow this thread: stash where it was, jump into the target, and return to a
        // trampoline of our own.
        saved_context = ctx;
        // Whatever the hook was carrying in x0 is the most current lua_State there is -- the hook
        // for a scripting game is the VM's own entry point, so its first argument is the state.
        call_slots["L"] = ctx.r[0];
        if (seen_states.insert(ctx.r[0]).second) {
            LOG_INFO(Core, "DSMod: scripting state {:016X} seen at the hook ({} distinct so far)",
                     ctx.r[0], seen_states.size());
        }
        if (call_seq != nullptr && !PrepareCallStep()) {
            LOG_ERROR(Core, "DSMod: sequence '{}' abandoned at step {} (argument or symbol)",
                      call_seq_name, call_seq_step);
            call_state = CallState::Idle;
            call_seq = nullptr;
            original_instruction = hook_original;
            RemoveBreakpoint(hook);
            return true;
        }
        original_instruction = hook_original;
        RemoveBreakpoint(hook); // the hook is done; leave the real instruction in place
        // The return address must be somewhere the game itself never executes. Re-using the hook
        // meant that anything the callee triggered which re-entered the hooked function -- and
        // deactivating a UI object triggers a lot of managed code -- was mistaken for our own
        // return, and the thread was then restored from the middle of a live call stack.
        return_trampoline = main_region_begin; // the module entry: run once by rtld, never again
        if (!InstallBreakpoint(return_trampoline)) {
            // Nothing has been disturbed yet, so hand the thread back untouched.
            call_state = CallState::Idle;
            call_seq = nullptr;
            arm_interface.SetContext(ctx);
            return true;
        }
        trampoline_original = original_instruction;
        for (size_t i = 0; i < call_args.size(); ++i) {
            ctx.r[i] = call_args[i];
        }
        ctx.lr = return_trampoline;
        ctx.pc = call_target;
        arm_interface.SetContext(ctx);
        call_state = CallState::InCall;
        // The stuck-call watchdog counts from HERE, not from arming: a hook that fires five
        // seconds after arming (the title screen runs script only on input) used to trip it
        // the instant the call entered, and the abandoned call then returned into a trampoline
        // nobody claimed -- the guest executed the module entry and died.
        call_started_tick = bridge_tick_count.load(std::memory_order_acquire);
        LOG_INFO(Core, "DSMod: entering guest call {:016X}", call_target);
        return true;
    }

    // The call returned. Read the result the way this step said it comes back: Lua built with
    // float numbers hands lua_tonumber's answer back in s0, where x0 holds nothing of interest.
    if (call_seq != nullptr) {
        const CallStep& done = call_seq->steps[call_seq_step];
        if (done.ret_float) {
            float value{};
            const u32 bits = static_cast<u32>(ctx.v[0][0]);
            std::memcpy(&value, &bits, sizeof(value));
            last_call_float = value;
            last_call_result = SaturatingToS64(value);
        } else {
            last_call_result = static_cast<s64>(ctx.r[0]);
            last_call_float = static_cast<f64>(last_call_result);
        }
        if (!done.save.empty()) {
            call_slots[done.save] = static_cast<u64>(last_call_result);
        }
        // A step says where its own answer goes. One chain can then read a whole inventory,
        // rather than paying for a thread borrow per number.
        if (!done.out.empty()) {
            pending_sequence_values[done.out] = last_call_float;
        }
        if (!done.out_addr.empty()) {
            pending_sequence_addresses[done.out_addr] = static_cast<u64>(last_call_result);
        }
        if (!done.out_text.empty()) {
            // A Lua string handed back as a char*: copy it out while the pointer is still good.
            std::string text;
            const VAddr at = static_cast<VAddr>(last_call_result);
            for (u64 i = 0; i < MaxTextLength && AddressIsSane(at + i, 1); ++i) {
                const u8 ch = system.ApplicationMemory().Read8(at + i);
                if (ch == 0) {
                    break;
                }
                text.push_back(static_cast<char>(ch));
            }
            pending_sequence_texts[done.out_text] = std::move(text);
        }
        // More to do: keep the thread and go straight into the next step. The trampoline is
        // already in place, so the chain costs one borrow rather than one per call.
        size_t next = call_seq_step + 1;
        if (done.has_reject && last_call_result == done.reject && done.else_step >= 0) {
            LOG_DEBUG(Core, "DSMod: sequence '{}' step {} answered {}; wrong state, leaving",
                      call_seq_name, call_seq_step, last_call_result);
            next = static_cast<size_t>(done.else_step);
        } else if (done.has_expect && last_call_result != done.expect && done.else_step >= 0) {
            LOG_INFO(Core, "DSMod: sequence '{}' step {} got {}, expected {}; skipping to {}",
                     call_seq_name, call_seq_step, last_call_result, done.expect, done.else_step);
            next = static_cast<size_t>(done.else_step);
        }
        if (next < call_seq->steps.size()) {
            call_seq_step = next;
            if (PrepareCallStep()) {
                for (size_t i = 0; i < call_args.size(); ++i) {
                    ctx.r[i] = call_args[i];
                }
                ctx.lr = return_trampoline;
                ctx.pc = call_target;
                arm_interface.SetContext(ctx);
                return true;
            }
        }
        if (!call_seq->out.empty()) {
            pending_sequence_values[call_seq->out] = last_call_float;
        }
        std::string published;
        for (const auto& [key, value] : pending_sequence_values) {
            published += fmt::format(" {}={:g}", key, value);
        }
        for (const auto& [key, value] : pending_sequence_addresses) {
            published += fmt::format(" {}={:X}", key, value);
        }
        for (const auto& [key, value] : pending_sequence_texts) {
            published += fmt::format(" {}=\"{}\"", key, value);
        }
        LOG_INFO(Core, "DSMod: sequence '{}' ran {} step(s):{}", call_seq_name, call_seq_step + 1,
                 published.empty() ? std::string{" (no output)"} : published);
        call_seq = nullptr;
    } else {
        last_call_result = static_cast<s64>(ctx.r[0]);
    }
    original_instruction = trampoline_original;
    RemoveBreakpoint(return_trampoline);
    // The gate is deliberately left open: dynarmic decides whether a block honours breakpoints
    // when it compiles the block, so a block compiled while it was shut would later meet one of
    // our brks as an undefined instruction.
    ctx = saved_context;
    arm_interface.SetContext(ctx);
    call_state = CallState::Idle;
    LOG_DEBUG(Core, "DSMod: guest call returned {}", last_call_result);
    return true;
}

} // namespace Core::Mods
