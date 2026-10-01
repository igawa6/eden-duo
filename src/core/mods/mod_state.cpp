// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// State sampling: reading the declarative package's values out of guest memory, and the few paths
// that write it back.
//   - Point resolution and reads: ResolvePoint (pointer chains, "static_fields" hops, IL2CPP class
//     slots from engine_il2cpp.cpp, pattern scans from mod_guest_bridge.cpp, FindPlayerNode from
//     engine_mercury.cpp), ReadPoint, ReadPointText, ReadIndexPoint, WalkList, ScanForU32Text,
//     AddressIsSane, HeapLow / HeapHigh, DescribeGuestValue.
//   - SampleState: fills the tick's StateSnapshot from spy and sequence results, counters, every
//     manifest point, then the native module's sample() (RunGameModule, mod_module_host.cpp).
//   - EvaluateDerived: the manifest "derived" values in a cached dependency order; the second
//     pass after input recomputes only the entries that read interaction state.
//   - Writes: WritePointValue (for RunAction) and ApplyEnforceRules (stuck-call timeout,
//     ApplyPatches, console-fired and enforce-rule actions, polled sequences).
//   - Inventory array finders (FindEntryArray, SelectLiveInventory) and the gameplay gate
//     (InGameplay, InGameplayHonest, PadIsPushed) that the search tools wait on.
// Not here: the guest-call bridge that produces spy and sequence values (mod_guest_bridge.cpp),
// action dispatch (mod_actions.cpp).
// Flow: Tick -> SampleState -> EvaluateDerived -> input/actions -> EvaluateDerived (volatile) ->
// ApplyEnforceRules -> page binds -> publish. Everything here runs on the tick thread; the only
// lock taken is guest_bridge_mutex (ApplyEnforceRules' stuck-call check). NormaliseToType has
// external linkage for mod_actions.cpp and mod_input.cpp (declared in mod_runtime_shared.h).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <cstdlib>
#include <filesystem>
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "hid_core/frontend/emulated_controller.h"

namespace Core::Mods {

namespace {
/// static_fields follows the method table by four pointers in every IL2CPP layout seen so far.
constexpr s64 StaticFieldsAfterMethods = 0x20;
/// A managed string keeps its length here and its UTF-16 characters just after.
constexpr s64 Il2CppStringLength = 0x10;
constexpr s64 Il2CppStringChars = 0x14;
} // namespace

bool ModRuntime::AddressIsSane(VAddr address, u64 size) const {
    if (address == 0 || size == 0) {
        return false;
    }
    return system.ApplicationMemory().IsValidVirtualAddressRange(address, size);
}

/// The heap region as the running process actually laid it out. The old fixed window
/// (0x21'80000000..0x21'D0000000) is where the 39-bit Dynarmic layout puts Dread's heap, and it
/// held on every desktop build -- then NCE placed the guest heap somewhere else entirely and
/// every sweep quietly found nothing. Ask the page table; the constants remain only as the
/// fallback for the moment before a process exists.
VAddr ModRuntime::HeapLow() const {
    // Any thread may ask (the tick thread, module callbacks on asset workers). The bounds are
    // found under heap_bounds_mutex and published high first, then low (release); a caller that
    // sees low != 0 therefore sees high too.
    if (heap_low.load(std::memory_order_acquire) == 0) {
        std::scoped_lock lock{heap_bounds_mutex};
        if (heap_low.load(std::memory_order_relaxed) == 0) {
            if (auto* const process = system.ApplicationProcess(); process != nullptr) {
                const u64 start = GetInteger(process->GetPageTable().GetHeapRegionStart());
                // The *region* is a 128 GB reservation, almost all unmapped. Sweeping it whole
                // means 33M page probes that never finish in time -- which is exactly why energy
                // and the player read `unresolved` under NCE while the old fixed 1.25 GB window
                // worked. Bound the walk to the committed heap plus a margin, so it stays a
                // couple of GB.
                const u64 committed = process->GetPageTable().GetNormalMemorySize();
                const u64 region = process->GetPageTable().GetHeapRegionSize();
                if (start != 0 && committed != 0) {
                    const u64 span =
                        std::min<u64>(region, committed + 0x40000000ULL); // +1 GB slack
                    heap_high.store(start + span, std::memory_order_relaxed);
                    heap_low.store(start, std::memory_order_release);
                    LOG_INFO(Core, "DSMod: heap {:X}..{:X} (committed {} MiB of {} MiB region)",
                             start, start + span, committed >> 20, region >> 20);
                }
            }
        }
    }
    const VAddr low = heap_low.load(std::memory_order_acquire);
    return low != 0 ? low : 0x21'80000000ULL;
}

VAddr ModRuntime::HeapHigh() const {
    HeapLow();
    const VAddr high = heap_high.load(std::memory_order_acquire);
    return high != 0 ? high : 0x21'D0000000ULL;
}

/// Renders one captured register in whatever form makes it identifiable. A wide spy sweep is only
/// as useful as this function: `lua_pushstring(L, "x")` gives itself away by carrying a readable
/// string, where `lua_settop(L, n)` carries a small integer and `lua_getfield(L, i, "k")` carries
/// both. Raw hex would leave all three looking alike.
std::string ModRuntime::DescribeGuestValue(u64 value) const {
    if (value == 0) {
        return "0";
    }
    if (value < 0x10000) {
        return fmt::format("{}", value); // an index, a count, a small enum
    }
    // Negative ints matter more than they look: Lua's pseudo-indices are constants like
    // LUA_GLOBALSINDEX (-10002), so a small negative argument names the API family outright.
    // A 32-bit int arrives with the top half cleared, having been written through a w register.
    const auto small_negative = [](s64 v) { return v < 0 && v > -0x10000; };
    if (small_negative(static_cast<s64>(value))) {
        return fmt::format("{}", static_cast<s64>(value));
    }
    if ((value >> 32) == 0 && small_negative(static_cast<s32>(value))) {
        return fmt::format("{}", static_cast<s32>(value));
    }
    // The "main region" the loader hands us spans the heap as well as the module, so an offset
    // into it is not on its own evidence of anything -- read the bytes first and only fall back
    // to the label.
    const std::string label = (main_region_size != 0 && value >= main_region_begin &&
                               value < main_region_begin + main_region_size)
                                  ? fmt::format("main+{:X}", value - main_region_begin)
                                  : fmt::format("{:X}", value);
    if (!AddressIsSane(value, sizeof(u64))) {
        return label; // not a pointer we can follow
    }
    auto& memory = system.ApplicationMemory();
    // A readable NUL-terminated ASCII run is the strongest signal there is.
    std::string text;
    for (u64 i = 0; i < 40 && AddressIsSane(value + i, 1); ++i) {
        const u8 ch = memory.Read8(value + i);
        if (ch == 0) {
            break;
        }
        if (ch < 0x20 || ch > 0x7E) {
            text.clear();
            break;
        }
        text.push_back(static_cast<char>(ch));
    }
    if (text.size() >= 2) { // "_G" is a real Lua key: two characters is already a strong hit
        return fmt::format("\"{}\"", text);
    }
    return fmt::format("{}->{:X}", label, memory.Read64(value));
}

/// Reads another point purely to use as an array index. Guarded so a data file that points a
/// hop at itself cannot spin.
std::optional<s64> ModRuntime::ReadIndexPoint(const std::string& name) const {
    if (index_depth > 2) {
        return std::nullopt;
    }
    const auto it = manifest.points.find(name);
    if (it == manifest.points.end()) {
        return std::nullopt;
    }
    ++index_depth;
    s64 value{};
    const bool ok = ReadPoint(it->second, value);
    --index_depth;
    return ok ? std::optional<s64>{value} : std::nullopt;
}

bool ModRuntime::WalkList(VAddr list, const ChainHop& hop, s64 index, VAddr& out) const {
    // A guard against a corrupt or half-built list walking us off into the weeds: no real
    // inventory is longer than this, and a circular list with a broken link never terminates.
    constexpr s64 MaxListWalk = 4096;
    if (index < 0 || index >= MaxListWalk) {
        return false;
    }
    auto& memory = system.ApplicationMemory();
    if (!AddressIsSane(list + static_cast<VAddr>(hop.list_next), sizeof(u64))) {
        return false;
    }
    // The distance from a node back to its object is stored in the list when the manifest does
    // not name it outright.
    s64 node_offset = hop.list_node_at;
    if (hop.list_node_at != 0) {
        if (!AddressIsSane(list + static_cast<VAddr>(hop.list_node_at), sizeof(u32))) {
            return false;
        }
        node_offset = static_cast<s32>(memory.Read32(list + static_cast<VAddr>(hop.list_node_at)));
    }
    if (node_offset < 0 || node_offset > 0x1000) {
        return false;
    }
    VAddr node = memory.Read64(list + static_cast<VAddr>(hop.list_next));
    for (s64 k = 0; k <= index; ++k) {
        if (node == list || node == 0 || node < static_cast<VAddr>(node_offset)) {
            return false;
        }
        if (k == index) {
            out = node - static_cast<VAddr>(node_offset);
            return true;
        }
        if (!AddressIsSane(node + static_cast<VAddr>(hop.list_next), sizeof(u64))) {
            return false;
        }
        node = memory.Read64(node + static_cast<VAddr>(hop.list_next));
    }
    return false;
}

VAddr ModRuntime::ScanForU32Text(const TextScan& spec) const {
    // One codepoint per 32-bit word. Walks the game heap once; the caller caches the result.
    auto& memory = system.ApplicationMemory();
    const VAddr Begin = HeapLow(), End = HeapLow() + 0x10000000ULL;
    for (VAddr at = Begin; at < End; at += 4) {
        if (!AddressIsSane(at, 4))
            continue;
        const u32 first = memory.Read32(at);
        if (first < 0x20 || first > 0x7E)
            continue;
        for (const auto& want : spec.candidates) {
            if (want.empty() || static_cast<u32>(want[0]) != first)
                continue;
            bool all = true;
            for (size_t k = 1; k < want.size(); ++k) {
                const VAddr c = at + static_cast<u32>(k) * 4;
                if (!AddressIsSane(c, 4) || memory.Read32(c) != static_cast<u32>(want[k])) {
                    all = false;
                    break;
                }
            }
            if (all) {
                LOG_INFO(Core, "DSMod: text_scan matched '{}' at {:012X}", want, at);
                return at;
            }
        }
    }
    LOG_WARNING(Core, "DSMod: text_scan found none of its candidates");
    return 0;
}

bool ModRuntime::ResolvePoint(const DataPoint& point, VAddr& address_out, s64 array_index) const {
    if (point.chain.empty()) {
        return false;
    }
    VAddr address = point.base == BaseRegion::Main ? main_region_begin : 0;
    if (!point.root_bind.empty()) {
        // The chain hangs off an object the game handed us this frame, not off the module.
        const auto root = sequence_addresses.find(point.root_bind);
        if (root == sequence_addresses.end() || root->second == 0) {
            return false;
        }
        address = static_cast<VAddr>(root->second) + static_cast<VAddr>(point.chain.front());
    } else if (!point.class_name.empty()) {
        const auto slot = FindClassSlotByName(point.class_name);
        if (!slot) {
            return false;
        }
        address += static_cast<VAddr>(*slot);
    } else if (point.text_scan.Valid()) {
        // Found once and remembered: the scan is expensive, and the buffer keeps its address for
        // as long as the game is running.
        const auto cached = text_scan_cache.find(&point);
        VAddr at = cached != text_scan_cache.end() ? cached->second : 0;
        if (at == 0) {
            // The buffer only exists once the game has drawn the label, which is not true at the
            // moment a save finishes loading. Retry on a slow cadence rather than every frame:
            // the scan is expensive and would otherwise stutter the game while it waits.
            constexpr u64 RetryTicks = 240;
            const auto last = text_scan_attempt.find(&point);
            if (last != text_scan_attempt.end() && tick_count - last->second < RetryTicks) {
                return false;
            }
            text_scan_attempt[&point] = tick_count;
            at = ScanForU32Text(point.text_scan);
            if (at == 0) {
                return false;
            }
            text_scan_cache[&point] = at;
        }
        address = at + static_cast<VAddr>(point.text_scan.offset);
    } else if (point.player.Valid()) {
        const auto node = FindPlayerNode(point.player);
        if (!node) {
            return false;
        }
        address = static_cast<VAddr>(*node) + static_cast<VAddr>(point.player.pos_offset);
    } else if (point.array.Valid()) {
        const auto base = FindEntryArray(point.array, point.offset);
        if (!base) {
            return false;
        }
        address =
            static_cast<VAddr>(*base) + static_cast<VAddr>(point.array.index * point.array.stride);
    } else if (point.find.Valid()) {
        const auto scanned = ScanPattern(point.find);
        if (!scanned) {
            return false;
        }
        // A heap match is already absolute; adding the module base to it would land nowhere.
        address =
            point.find.heap ? static_cast<VAddr>(*scanned) : address + static_cast<VAddr>(*scanned);
    } else {
        address += static_cast<VAddr>(point.chain.front());
    }
    if (!point.hops.empty()) {
        for (const auto& hop : point.hops) {
            if (hop.is_list) {
                // An intrusive circular list: the head is a node like any other, and reaching
                // element N means following N+1 links rather than multiplying by a stride. The
                // object we want sits a fixed distance behind its node, and the list itself says
                // how far. No dereference first -- the head is inside the object we are already
                // standing on.
                if (!WalkList(address + static_cast<VAddr>(hop.offset), hop,
                              hop.index_from_array ? array_index : hop.index, address)) {
                    return false;
                }
                continue;
            }
            if (!AddressIsSane(address, sizeof(u64))) {
                return false;
            }
            address = system.ApplicationMemory().Read64(address);
            if (hop.static_fields) {
                const auto methods_offset = DetectMethodsOffset(address);
                if (!methods_offset) {
                    return false;
                }
                address += static_cast<VAddr>(*methods_offset + StaticFieldsAfterMethods);
                continue;
            }
            s64 index = hop.index;
            if (hop.index_from_array) {
                index = array_index;
            } else if (!hop.index_bind.empty()) {
                index = ReadIndexPoint(hop.index_bind).value_or(hop.index);
            }
            address += static_cast<VAddr>(hop.offset + index * hop.stride);
        }
        if (point.stride != 0 && std::ranges::none_of(point.hops, [](const ChainHop& h) {
                return h.index_from_array;
            })) {
            address += static_cast<VAddr>(array_index * point.stride);
        }
        address += static_cast<VAddr>(point.offset);
        address_out = address;
        return true;
    }

    for (size_t i = 1; i < point.chain.size(); ++i) {
        if (!AddressIsSane(address, sizeof(u64))) {
            return false;
        }
        address = system.ApplicationMemory().Read64(address);
        if (point.chain[i] == StaticFieldsToken) {
            // "static_fields": the class' static block. In every IL2CPP layout so far it follows
            // the method table by four pointers, so detecting one gives the other.
            const auto methods_offset = DetectMethodsOffset(address);
            if (!methods_offset) {
                return false;
            }
            address += static_cast<VAddr>(*methods_offset + StaticFieldsAfterMethods);
            continue;
        }
        address += static_cast<VAddr>(point.chain[i]);
    }
    address += static_cast<VAddr>(point.offset + array_index * point.stride);
    address_out = address;
    return true;
}

namespace {
/// Bytes an integer point occupies; 0 for floats and strings.
u32 IntegerWidth(ValueType type) {
    switch (type) {
    case ValueType::U8:
    case ValueType::S8:
    case ValueType::Bool:
        return 1;
    case ValueType::U16:
    case ValueType::S16:
        return 2;
    case ValueType::U32:
    case ValueType::S32:
        return 4;
    case ValueType::U64:
    case ValueType::S64:
        return 8;
    default:
        return 0;
    }
}

/// shift, then mask, then popcount, on the unsigned bit pattern of the read.
s64 ApplyPointModifiers(const DataPoint& point, u64 raw) {
    if (point.shift > 0) {
        raw = point.shift >= 64 ? 0 : raw >> point.shift;
    }
    if (point.has_mask) {
        raw &= point.mask;
    }
    if (point.popcount) {
        return static_cast<s64>(std::popcount(raw));
    }
    return static_cast<s64>(raw);
}
} // namespace

/// A value as the point's own type would hold it, so -1 and 255 compare equal on a u8.
s64 NormaliseToType(ValueType type, s64 value) {
    switch (type) {
    case ValueType::U8:
    case ValueType::Bool:
        return static_cast<u8>(value);
    case ValueType::S8:
        return static_cast<s8>(value);
    case ValueType::U16:
        return static_cast<u16>(value);
    case ValueType::S16:
        return static_cast<s16>(value);
    case ValueType::U32:
        return static_cast<u32>(value);
    case ValueType::S32:
        return static_cast<s32>(value);
    default:
        return value;
    }
}

bool ModRuntime::ReadPoint(const DataPoint& point, s64& value_out, s64 array_index) const {
    VAddr address{};
    if (!ResolvePoint(point, address, array_index)) {
        return false;
    }
    if (point.is_pointer) {
        value_out = static_cast<s64>(address);
        return true;
    }
    auto& memory = system.ApplicationMemory();
    const auto read = [&](u64 size) { return AddressIsSane(address, size); };
    if (const u32 width = IntegerWidth(point.type); width != 0 && point.HasModifiers()) {
        if (!read(width)) {
            return false;
        }
        const u64 raw = width == 1   ? memory.Read8(address)
                        : width == 2 ? memory.Read16(address)
                        : width == 4 ? memory.Read32(address)
                                     : memory.Read64(address);
        value_out = ApplyPointModifiers(point, raw);
        return true;
    }
    switch (point.type) {
    case ValueType::U8:
    case ValueType::Bool:
        if (!read(1))
            return false;
        value_out = memory.Read8(address);
        return true;
    case ValueType::S8:
        if (!read(1))
            return false;
        value_out = static_cast<s8>(memory.Read8(address));
        return true;
    case ValueType::U16:
        if (!read(2))
            return false;
        value_out = memory.Read16(address);
        return true;
    case ValueType::S16:
        if (!read(2))
            return false;
        value_out = static_cast<s16>(memory.Read16(address));
        return true;
    case ValueType::U32:
        if (!read(4))
            return false;
        value_out = memory.Read32(address);
        return true;
    case ValueType::S32:
        if (!read(4))
            return false;
        value_out = static_cast<s32>(memory.Read32(address));
        return true;
    case ValueType::U64:
    case ValueType::S64:
        if (!read(8))
            return false;
        value_out = static_cast<s64>(memory.Read64(address));
        return true;
    case ValueType::Utf16String:
    case ValueType::CString:
    case ValueType::Utf32String:
        return false;
    case ValueType::F32: {
        if (!read(4))
            return false;
        const u32 raw = memory.Read32(address);
        f32 as_float{};
        std::memcpy(&as_float, &raw, sizeof(as_float));
        value_out = SaturatingToS64(as_float);
        return true;
    }
    }
    return false;
}

/// Reads a string the game owns. A managed string keeps its length at +0x10 and UTF-16 characters
/// from +0x14; the chain lands on the field holding the reference, so that is dereferenced first.
std::optional<std::string> ModRuntime::ReadPointText(const DataPoint& point,
                                                     s64 array_index) const {
    VAddr address{};
    if (!ResolvePoint(point, address, array_index) || !AddressIsSane(address, sizeof(u64))) {
        return std::nullopt;
    }
    auto& memory = system.ApplicationMemory();
    if (point.type == ValueType::Utf32String) {
        // The game keeps some UI labels as one codepoint per 32-bit word, inline at the address
        // rather than behind a handle -- read straight through until the terminator.
        std::string text;
        for (u32 i = 0; i < MaxTextLength; ++i) {
            const VAddr at = address + i * 4;
            if (!AddressIsSane(at, 4))
                break;
            const u32 c = memory.Read32(at);
            if (c == 0 || c < 0x20 || c > 0x7E)
                break;
            text.push_back(static_cast<char>(c));
        }
        if (text.empty())
            return std::nullopt;
        return text;
    }
    if (point.type == ValueType::CString) {
        const VAddr target = memory.Read64(address);
        if (!AddressIsSane(target, 1)) {
            return std::nullopt;
        }
        return memory.ReadCString(target, MaxTextLength);
    }

    const VAddr object = memory.Read64(address);
    if (!AddressIsSane(object, Il2CppStringChars)) {
        return std::nullopt;
    }
    const u32 length = memory.Read32(object + Il2CppStringLength);
    if (length == 0 || length > MaxTextLength) {
        return std::nullopt;
    }
    std::string text;
    text.reserve(length);
    for (u32 i = 0; i < length; ++i) {
        const u16 unit = memory.Read16(object + Il2CppStringChars + i * sizeof(u16));
        // Sprite and scene names are ASCII in practice; anything else is shown as '?'.
        text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return text;
}

void ModRuntime::SampleState(StateSnapshot& out) {
    out.tick = tick_count;
    for (const auto& [name, value] : spy_values) {
        out.ints[name] = static_cast<s64>(value);
    }
    // What the guest's own code told us, on the same footing as anything read out of memory --
    // a widget binds to it without caring which of the two it was.
    for (const auto& [name, value] : sequence_values) {
        out.floats[name] = value;
        out.ints[name] = SaturatingToS64(value);
    }
    for (const auto& [name, value] : sequence_addresses) {
        out.addresses[name] = value;
    }
    for (const auto& [name, value] : sequence_texts) {
        out.texts[name] = value;
    }
    // The slots this runtime is steering are state too: publish them as "@name" so a page can
    // mark the selected cell the way the game marks it.
    for (const auto& [cname, cval] : counters) {
        out.ints[counter_keys(cname)] = cval;
    }
    for (const auto& [name, point] : manifest.points) {
        // A plain point is an array of one, so both kinds go through the same loop. How many
        // elements can itself be a reading -- an inventory's length is not known in advance.
        s64 count = std::max<s64>(1, point.count);
        if (!point.count_bind.empty()) {
            if (const auto live = ReadIndexPoint(point.count_bind)) {
                count = std::clamp<s64>(*live, 0, point.count > 0 ? point.count : 1);
            }
        }
        for (s64 i = 0; i < count; ++i) {
            const std::string& key = point.count > 1 || !point.count_bind.empty()
                                          ? element_keys(name, static_cast<size_t>(i))
                                          : name;
            VAddr address{};
            out.addresses[key] = ResolvePoint(point, address, i) ? address : 0;
            if (point.type == ValueType::Utf16String || point.type == ValueType::CString ||
                point.type == ValueType::Utf32String) {
                if (const auto text = ReadPointText(point, i)) {
                    out.texts[key] = *text;
                }
                continue;
            }
            s64 value{};
            if (ReadPoint(point, value, i)) {
                out.ints[key] = value;
                if (key == manifest.gameplay_point && value > 0 && value < 10000) {
                    last_read_gameplay_signal = static_cast<f32>(value);
                }
                // A float point keeps its fraction. Truncating a position held in metres to whole
                // metres quantises a map marker to visible steps.
                if (point.type == ValueType::F32 && AddressIsSane(address, 4)) {
                    const u32 raw = system.ApplicationMemory().Read32(address);
                    f32 exact{};
                    std::memcpy(&exact, &raw, sizeof(exact));
                    out.floats[key] = exact;
                }
            }
        }
    }
    RunGameModule(out, false);
}

void ModRuntime::EvaluateDerived(StateSnapshot& snapshot, bool volatile_only) {
    const auto& list = manifest.derived;
    if (list.empty()) {
        return;
    }
    // Terms may name other derived values in any order: evaluate what is ready, repeat; a cycle (or
    // a self reference) is evaluated on the last pass with whatever is published by then. Which
    // entry is ready when depends only on the list itself, never on values, so the resulting
    // evaluation order is computed once per list and replayed: the same evaluations in the same
    // order as the multi-pass loop, without re-testing every dependency by name on every pass.
    if (derived_order_list != list.data() || derived_order_size != list.size() ||
        derived_volatile.size() != list.size()) {
        derived_index.clear();
        for (size_t i = 0; i < list.size(); ++i) {
            derived_index[list[i].name] = i;
        }
        derived_order.clear();
        derived_order.reserve(list.size());
        derived_done.assign(list.size(), 0);
        for (size_t pass = 0; pass <= list.size(); ++pass) {
            bool left = false;
            for (size_t i = 0; i < list.size(); ++i) {
                if (derived_done[i] != 0) {
                    continue;
                }
                const DerivedPoint& d = list[i];
                const auto waits = [&](const std::string& src) {
                    const auto it = derived_index.find(src);
                    return it != derived_index.end() && it->second != i &&
                           derived_done[it->second] == 0;
                };
                bool wait = false;
                if (!d.select.empty()) {
                    wait = waits(d.select) || (!d.select_then.empty() && waits(d.select_then)) ||
                           (!d.select_else.empty() && waits(d.select_else));
                } else if (!d.hold_last_nonzero.empty()) {
                    wait =
                        waits(d.hold_last_nonzero) || (!d.hold_gate.empty() && waits(d.hold_gate));
                } else if (!d.cmp_op.empty()) {
                    wait = (!d.cmp_a.is_const && waits(d.cmp_a.name)) ||
                           (!d.cmp_b.is_const && waits(d.cmp_b.name));
                } else if (!d.nonzero_sources.empty()) {
                    for (const auto& src : d.nonzero_sources) {
                        wait = wait || waits(src);
                    }
                } else {
                    for (const auto& term : d.terms) {
                        wait = wait || waits(term.first);
                    }
                }
                if (wait && pass < list.size()) {
                    left = true;
                    continue;
                }
                derived_done[i] = 1;
                derived_order.push_back(static_cast<u32>(i));
            }
            if (!left) {
                break;
            }
        }
        derived_order_list = list.data();
        derived_order_size = list.size();
        // Which values the post-tap pass must recompute.
        derived_volatile.assign(list.size(), 0);
        const auto is_volatile_key = [](const std::string& name) {
            return name.starts_with('@') || name.starts_with("view_custom:");
        };
        for (size_t i = 0; i < list.size(); ++i) {
            const DerivedPoint& d = list[i];
            // Volatility is judged on every name the evaluation can read, whichever branch runs.
            bool vol = is_volatile_key(d.name) || is_volatile_key(d.any_eq_array) ||
                       is_volatile_key(d.select) || is_volatile_key(d.select_then) ||
                       is_volatile_key(d.select_else) || is_volatile_key(d.hold_last_nonzero) ||
                       is_volatile_key(d.hold_gate) ||
                       (!d.cmp_a.is_const && is_volatile_key(d.cmp_a.name)) ||
                       (!d.cmp_b.is_const && is_volatile_key(d.cmp_b.name));
            for (const auto& src : d.nonzero_sources)
                vol = vol || is_volatile_key(src);
            for (const auto& term : d.terms)
                vol = vol || is_volatile_key(term.first);
            derived_volatile[i] = vol ? 1 : 0;
        }
        // A value computed from a volatile derived value is volatile itself. Every derived-typed
        // source the evaluation reads must count here, not only the ones the ordering waits on.
        const auto all_derived_sources = [&](size_t i, auto&& visit) {
            const DerivedPoint& d = list[i];
            const auto one = [&](const std::string& name) {
                if (name.empty())
                    return;
                if (const auto it = derived_index.find(name); it != derived_index.end()) {
                    visit(it->second);
                }
            };
            one(d.select);
            one(d.select_then);
            one(d.select_else);
            one(d.hold_last_nonzero);
            one(d.hold_gate);
            if (!d.cmp_a.is_const)
                one(d.cmp_a.name);
            if (!d.cmp_b.is_const)
                one(d.cmp_b.name);
            for (const auto& src : d.nonzero_sources)
                one(src);
            for (const auto& term : d.terms)
                one(term.first);
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < list.size(); ++i) {
                if (derived_volatile[i] != 0)
                    continue;
                bool vol = false;
                all_derived_sources(i, [&](size_t j) { vol = vol || derived_volatile[j] != 0; });
                if (vol) {
                    derived_volatile[i] = 1;
                    changed = true;
                }
            }
        }
        // The post-tap pass runs whenever any value is volatile. This used to be a separate,
        // narrower test (select/hold/terms only) that missed "cmp" and "nonzero" over "@flag:",
        // so a package whose only interaction reads were those (P5R) drew one frame of
        // pre-tap derived values after every tap.
        derived_reads_interaction =
            std::ranges::any_of(derived_volatile, [](u8 v) { return v != 0; });
    }
    const auto lookup = [&snapshot](const std::string& key) -> std::optional<f64> {
        if (const auto f = snapshot.floats.find(key); f != snapshot.floats.end()) {
            return f->second;
        }
        if (const auto i = snapshot.ints.find(key); i != snapshot.ints.end()) {
            return static_cast<f64>(i->second);
        }
        return std::nullopt;
    };
    const auto publish = [&snapshot](const std::string& name, f64 v) {
        if (!std::isfinite(v)) {
            return;
        }
        snapshot.floats[name] = v;
        snapshot.ints[name] = static_cast<s64>(std::clamp(v, -9.2e18, 9.2e18));
    };
    for (const u32 i : derived_order) {
        if (volatile_only && derived_volatile[i] == 0) {
            // Inputs of a non-volatile value have not changed since this tick's first pass, so its
            // published value (or its absence) and hold state already are what a recompute gives.
            continue;
        }
        {
            const DerivedPoint& d = list[i];
            if (!d.any_eq_array.empty()) {
                // How many elements of an array point equal the value (missing element = missing).
                s64 hits = 0;
                bool missing = d.any_eq_count <= 0;
                for (s64 k = 0; k < d.any_eq_count && !missing; ++k) {
                    const auto v =
                        snapshot.ints.find(element_keys(d.any_eq_array, static_cast<size_t>(k)));
                    if (v == snapshot.ints.end()) {
                        missing = true;
                    } else if (v->second == d.any_eq_value) {
                        ++hits;
                    }
                }
                if (!missing) {
                    publish(d.name, static_cast<f64>(hits));
                }
                continue;
            }
            if (!d.select.empty()) {
                if (const auto cond = lookup(d.select)) {
                    const std::string& chosen = *cond != 0.0 ? d.select_then : d.select_else;
                    if (const auto v = chosen.empty() ? std::nullopt : lookup(chosen)) {
                        publish(d.name, *v);
                    }
                }
                continue;
            }
            if (!d.hold_last_nonzero.empty()) {
                const auto src = lookup(d.hold_last_nonzero);
                const auto gate = d.hold_gate.empty() ? src : lookup(d.hold_gate);
                const auto held = derived_held.find(d.name);
                if (src && gate && *gate != 0.0) {
                    derived_held[d.name] = *src;
                    publish(d.name, *src);
                } else if (held != derived_held.end()) {
                    publish(d.name, held->second);
                } else if (src) {
                    publish(d.name, *src);
                }
                continue;
            }
            if (!d.cmp_op.empty()) {
                // Native comparison, in place of hand-derived floor-of-linear-
                // combination tricks (dgn_ge/dgn_eq et al). A small epsilon absorbs float-path
                // noise on an otherwise-integer value without blurring genuine fractional
                // comparisons (world coordinates, timers). Missing when either side is missing.
                const auto resolve = [&](const DerivedPoint::CmpOperand& op) -> std::optional<f64> {
                    return op.is_const ? std::optional<f64>(op.const_value) : lookup(op.name);
                };
                const auto a = resolve(d.cmp_a);
                const auto b = resolve(d.cmp_b);
                if (a && b) {
                    constexpr f64 Eps = 1e-6;
                    bool result = false;
                    if (d.cmp_op == "eq") {
                        result = std::fabs(*a - *b) < Eps;
                    } else if (d.cmp_op == "ne") {
                        result = std::fabs(*a - *b) >= Eps;
                    } else if (d.cmp_op == "ge") {
                        result = *a >= *b - Eps;
                    } else if (d.cmp_op == "gt") {
                        result = *a > *b + Eps;
                    } else if (d.cmp_op == "le") {
                        result = *a <= *b + Eps;
                    } else if (d.cmp_op == "lt") {
                        result = *a < *b - Eps;
                    }
                    publish(d.name, result ? 1.0 : 0.0);
                }
                continue;
            }
            if (!d.nonzero_sources.empty()) {
                // all_nonzero / any_nonzero, in place of hand-derived
                // dgn_and/dgn_or/dgn_andnot chains. Fails closed like any_eq: any unresolved
                // source makes the whole result missing rather than silently treating it as 0.
                bool missing = false;
                bool any_true = false;
                bool all_true = true;
                for (const auto& src : d.nonzero_sources) {
                    const auto v = lookup(src);
                    if (!v) {
                        missing = true;
                        break;
                    }
                    const bool nz = *v != 0.0;
                    any_true = any_true || nz;
                    all_true = all_true && nz;
                }
                if (!missing) {
                    publish(d.name, (d.nonzero_require_all ? all_true : any_true) ? 1.0 : 0.0);
                }
                continue;
            }
            f64 sum = d.add;
            bool missing = false;
            for (const auto& [src, factor] : d.terms) {
                const auto v = lookup(src);
                if (!v) {
                    missing = true;
                    break;
                }
                sum += *v * factor;
            }
            if (missing) {
                continue;
            }
            if (d.floor) {
                sum = std::floor(sum);
            } else if (d.round) {
                sum = std::round(sum);
            }
            publish(d.name, sum);
        }
    }
}

std::optional<ModRuntime::GuestStore> ModRuntime::PlanPointWrite(
    const DataPoint& point, s64 value, s64 array_index, std::optional<f64> as_float) const {
    if (point.popcount || point.is_pointer) {
        LOG_WARNING(Core, "DSMod: refusing to write a {} point",
                    point.popcount ? "popcount" : "pointer");
        return std::nullopt;
    }
    u32 width = IntegerWidth(point.type);
    if (width == 0) {
        if (point.type != ValueType::F32) {
            return std::nullopt; // strings are read-only
        }
        width = 4; // historical behaviour: an integer written into the word
    }
    VAddr address{};
    if (!ResolvePoint(point, address, array_index) || !AddressIsSane(address, width)) {
        return std::nullopt;
    }
    GuestStore store{.address = address, .width = width};
    if (point.type == ValueType::F32 && as_float.has_value()) {
        // A float source (a map position) lands as the float itself, not as an integer word.
        const f32 f = static_cast<f32>(*as_float);
        u32 bits{};
        std::memcpy(&bits, &f, sizeof(bits));
        store.bits = bits;
        return store;
    }
    const u64 raw = static_cast<u64>(value);
    if (point.shift != 0 || point.has_mask) {
        // Only the point's own bits change: a flag point cannot clobber its neighbours.
        const u64 field_mask = point.has_mask ? point.mask : ~u64{0};
        store.field = point.shift >= 64 ? 0 : field_mask << point.shift;
        store.bits = point.shift >= 64 ? 0 : (raw & field_mask) << point.shift;
    } else {
        store.bits = raw;
    }
    return store;
}

std::optional<ModRuntime::GuestExpect> ModRuntime::ExpectUnchanged(const DataPoint& point,
                                                                   s64 array_index) const {
    const u32 width = point.type == ValueType::F32 ? 4 : IntegerWidth(point.type);
    VAddr address{};
    if (width == 0 || !ResolvePoint(point, address, array_index) ||
        !AddressIsSane(address, width)) {
        return std::nullopt;
    }
    auto& memory = system.ApplicationMemory();
    const u64 now = width == 1   ? memory.Read8(address)
                    : width == 2 ? memory.Read16(address)
                    : width == 4 ? memory.Read32(address)
                                 : memory.Read64(address);
    return GuestExpect{.address = address, .width = width, .bits = now};
}

bool ModRuntime::RunWithGuestStopped(const std::function<void()>& fn) {
    // Single core: this CoreTiming callback already runs between guest time slices. Multi-core:
    // suspend the application's threads without pausing core timing (which this thread drives).
    if (!system.IsMulticore()) {
        fn();
        return true;
    }
    return system.RunWithGuestThreadsSuspended(fn);
}

bool ModRuntime::ApplyGuestStores(std::span<const GuestExpect> expects,
                                  std::span<const GuestStore> stores) {
    auto& memory = system.ApplicationMemory();
    const auto width_mask = [](u32 width) {
        return width >= 8 ? ~u64{0} : (u64{1} << (width * 8)) - 1;
    };
    const auto read = [&memory](VAddr address, u32 width) -> u64 {
        return width == 1   ? memory.Read8(address)
               : width == 2 ? memory.Read16(address)
               : width == 4 ? memory.Read32(address)
                            : memory.Read64(address);
    };
    const auto partial = [&](const GuestStore& s) {
        return (s.field & width_mask(s.width)) != width_mask(s.width);
    };
    bool applied = false;
    const auto apply = [&] {
        for (const auto& e : expects) {
            if (((read(e.address, e.width) ^ e.bits) & e.field & width_mask(e.width)) != 0) {
                return; // the guest changed underneath: write nothing
            }
        }
        for (const auto& s : stores) {
            u64 raw = s.bits;
            if (partial(s)) {
                raw = (read(s.address, s.width) & ~s.field) | (s.bits & s.field);
            }
            switch (s.width) {
            case 1:
                memory.Write8(s.address, static_cast<u8>(raw));
                break;
            case 2:
                memory.Write16(s.address, static_cast<u16>(raw));
                break;
            case 8:
                memory.Write64(s.address, raw);
                break;
            default:
                memory.Write32(s.address, static_cast<u32>(raw));
                break;
            }
        }
        applied = true;
    };
    // One whole-word store is already a single store: no reason to stop the guest for it.
    if (expects.empty() && stores.size() == 1 && !partial(stores.front())) {
        apply();
        return applied;
    }
    // Several stores, a read-modify-write or a check: done while no guest thread runs, so the
    // guest never sees half a slot, both halves of a swap holding one item, or a lost update to
    // a neighbouring bit. When the application cannot be stalled right now (a pause/resume in
    // progress) the batch is applied as before this path existed, rather than dropping the tap.
    if (!RunWithGuestStopped(apply)) {
        apply();
    }
    return applied;
}

bool ModRuntime::WritePointValue(const DataPoint& point, s64 value, s64 array_index,
                                 std::optional<f64> as_float) {
    const auto store = PlanPointWrite(point, value, array_index, as_float);
    return store && ApplyGuestStores({}, std::span{&*store, 1});
}

/// True while player 1's left stick is pushed past a drift dead zone, or a direction (d-pad or the
/// digital left-stick directions) is held. FindPlayerNode (engine_mercury.cpp) scores its
/// candidates by whether they move while this is true.
bool ModRuntime::PadIsPushed() const {
    auto* const controller = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    if (controller == nullptr) {
        return false;
    }
    const auto sticks = controller->GetSticks();
    constexpr s32 Dead = 12000; // well outside stick drift
    if (std::abs(sticks.left.x) > Dead || std::abs(sticks.left.y) > Dead) {
        return true;
    }
    // D-pad and the digital left-stick directions a keyboard or a hat produces.
    constexpr u64 DirectionMask = 0xF000ULL | 0xF0000ULL;
    return (static_cast<u64>(controller->GetNpadButtons().raw) & DirectionMask) != 0;
}

std::optional<s64> ModRuntime::FindEntryArray(const ArrayFind& spec, s64 value_off) const {
    if (!spec.Valid() || main_region_begin == 0) {
        return std::nullopt;
    }
    // Under NCE there is more than one array of the inventory class in the heap -- a live gameplay
    // copy and one or more static copies (a freshly-loaded/full template that reads current==max
    // forever). The first in heap order is a static one here (it is the live one under Dynarmic,
    // which is why the plain sweep is right there). Tell them apart the only way that generalises:
    // the live copy's values CHANGE as the game is played; the static ones never do. Runs only once
    // the delta is known (i.e. only under NCE); Dynarmic keeps the original fast path below.
    if (nce_vtable_delta != 0) {
        return SelectLiveInventory(spec, value_off);
    }
    const auto key = std::make_pair(spec.vtable, spec.stride);
    if (const auto cached = entry_array_cache.find(key); cached != entry_array_cache.end()) {
        return cached->second == 0 ? std::nullopt : std::optional<s64>{cached->second};
    }
    // This is called from point resolution, which runs every frame from boot, so the sweep must
    // not be a gigabyte in one go. It used to be, guarded by an InGameplay() gate -- and that
    // gate deadlocked the moment energy stopped coming from the scripting bridge: the gate asks
    // for energy, energy comes from this array, this array waits on the gate. Nothing resolved
    // for a whole session of play.
    // So: no gate, and no permanent miss either. Sweep a slice per tick and wrap around, which
    // bounds the per-frame cost and lets the array be picked up whenever it does appear.
    // Read through the host page directly. Going via the page table for every eight bytes meant
    // two million lookups a tick, which blocks the thread that presents frames -- the game keeps
    // running and making sound behind a picture that never updates.
    constexpr u64 PagesPerTick = 1024;
    const VAddr HeapBegin = HeapLow();
    const VAddr HeapEnd = HeapHigh();
    const auto swept = entry_array_swept_tick.find(key);
    if (swept != entry_array_swept_tick.end() && tick_count - swept->second < 2) {
        return std::nullopt; // at most one slice every other frame, per key
    }
    entry_array_swept_tick[key] = tick_count;
    auto& cursor = entry_array_cursor[key];
    if (cursor < HeapBegin || cursor >= HeapEnd) {
        cursor = HeapBegin;
    }
    const VAddr slice_end = std::min<VAddr>(HeapEnd, cursor + PagesPerTick * 0x1000);
    auto& memory = system.ApplicationMemory();
    // Under NCE the module's data segment (where vtables live) is mapped at a different offset
    // from the text base than under Dynarmic, so the precomputed `main + vtable` is wrong by a
    // constant that is the same for every class. Discovered once from the inventory array's shape
    // (below) and applied to every vtable here and in FindPlayerNode.
    const u64 vtable =
        main_region_begin + static_cast<u64>(spec.vtable) + static_cast<u64>(nce_vtable_delta);
    for (VAddr page = cursor; page < slice_end; page += 0x1000) {
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
            const VAddr start = page + at;
            // Take the first entry of the run, not the middle of one.
            if (AddressIsSane(start - spec.stride, 8) &&
                memory.Read64(start - spec.stride) == vtable) {
                continue;
            }
            int length = 0;
            while (length < 256 && AddressIsSane(start + length * spec.stride, 8) &&
                   memory.Read64(start + length * spec.stride) == vtable) {
                ++length;
            }
            if (length >= spec.min_run) {
                LOG_INFO(Core, "DSMod: entry array of {} at {:016X} (stride {:#x})", length, start,
                         spec.stride);
                entry_array_cache[key] = static_cast<s64>(start);
                return static_cast<s64>(start);
            }
        }
    }
    // Slice done. Advance, and on a full lap start over rather than recording a permanent miss:
    // the inventory does not exist during the intro, and a session that gave up there would stay
    // blank for as long as the game ran.
    cursor = slice_end;
    if (cursor >= HeapEnd) {
        cursor = HeapBegin;
        // Re-log each lap while unresolved (throttled), NOT once: the first lap finishes at the
        // title screen where the inventory objects do not exist yet, so a one-shot log only ever
        // reports the empty title-screen heap. Reporting again in gameplay is the whole point.
        // No exact run this lap. If we have not yet learned the NCE vtable delta, recover it from
        // the inventory array's SHAPE rather than its address: find the longest run of >= min_run
        // entries at this stride whose first 8 bytes are all identical and point into the module
        // region. That word is the real (shifted) vtable; the delta from the precomputed value is
        // constant across all classes, so recording it once fixes every finder. Under Dynarmic the
        // exact path already matched and this never runs.
        if (nce_vtable_delta == 0 && main_region_size != 0) {
            const u64 mod_lo = main_region_begin;
            const u64 mod_hi = main_region_begin + std::max<u64>(main_region_size, 0x8000000ULL);
            const auto in_module = [&](u64 w) { return w >= mod_lo && w < mod_hi; };
            // Identifying the array by "longest run of identical module pointers" is not enough:
            // any UI/system array of the same stride matches, and its class sits at a different
            // module offset, so the delta comes out wrong (learned -0x45E0 off a title-screen
            // array, then energy read 5e-14). The inventory is the one whose entries carry real
            // stat floats -- finite, 0..1e5, and not all near-zero. That signature only appears in
            // gameplay, so this also self-gates: nothing passes at the title screen.
            // Dread's inventory stats are whole numbers stored as floats -- 99, 1000, 12, 15, 0 --
            // so an entry's value column is integer-valued. A coincidental geometry/matrix array
            // (values like 2727.54) is not, which is what slipped through a looser check. Require
            // integer-valued, and require the run to contain at least one large round value
            // (energy_max 99 / aeion_max 1000 are always present), which no small-int or fractional
            // array carries.
            const auto int_stat = [](f32 v) {
                return std::isfinite(v) && v >= 0.0f && v <= 100000.0f && v == std::trunc(v);
            };
            u64 best_word = 0;
            VAddr best_start = 0;
            int best_len = 0;
            constexpr int EnoughLen = 12;
            for (VAddr page = HeapBegin; page < HeapEnd && best_len < EnoughLen; page += 0x1000) {
                const u8* const host = memory.GetPointerSilent(page);
                if (host == nullptr) {
                    continue;
                }
                for (u32 at = 0; at + 8 <= 0x1000 && best_len < EnoughLen; at += 8) {
                    u64 w{};
                    std::memcpy(&w, host + at, sizeof(w));
                    if (!in_module(w)) {
                        continue;
                    }
                    const VAddr start = page + at;
                    if (AddressIsSane(start - spec.stride, 8) &&
                        memory.Read64(start - spec.stride) == w) {
                        continue; // not the first entry of its run
                    }
                    int len = 0;
                    int stats = 0;       // entries whose value field is an integer stat
                    int nonzero = 0;     // ...of which are >= 1
                    int large_round = 0; // ...that are a big round value (energy 99 / aeion 1000)
                    while (len < 256 && AddressIsSane(start + len * spec.stride, 8) &&
                           memory.Read64(start + len * spec.stride) == w) {
                        const VAddr vaddr =
                            start + len * spec.stride + static_cast<VAddr>(value_off);
                        if (AddressIsSane(vaddr, 4)) {
                            f32 fv{};
                            const u32 raw = memory.Read32(vaddr);
                            std::memcpy(&fv, &raw, sizeof(fv));
                            if (int_stat(fv)) {
                                ++stats;
                                if (fv >= 1.0f) {
                                    ++nonzero;
                                }
                                if (fv >= 90.0f && fv <= 2000.0f) {
                                    ++large_round;
                                }
                            }
                        }
                        ++len;
                    }
                    // Integer-valued across the whole run, a few non-zero, and at least one big
                    // round value -- the energy/aeion maxima the inventory always carries.
                    const bool looks_inventory =
                        len >= spec.min_run && stats >= len - 1 && nonzero >= 3 && large_round >= 1;
                    if (looks_inventory && len > best_len) {
                        best_len = len;
                        best_word = w;
                        best_start = start;
                    }
                }
            }
            if (best_len >= spec.min_run && best_word != 0) {
                // Learn the delta ONLY -- do not cache this particular instance. There can be more
                // than one array of the class (a live gameplay copy and a static/saved one); the
                // discovery's "longest run" was a stale copy that never depleted. With the delta
                // known, the normal fast path below re-finds the array in heap order on the next
                // lap, exactly as it does under Dynarmic where it lands on the live one.
                nce_vtable_delta =
                    static_cast<s64>(best_word) - static_cast<s64>(main_region_begin + spec.vtable);
                LOG_INFO(Core,
                         "DSMod: NCE vtable delta learned: real {:016X} vs main+{:X} -> delta "
                         "{:#x} (inventory-shaped run = {} entries at {:016X}, stride {:#x})",
                         best_word, spec.vtable, nce_vtable_delta, best_len, best_start,
                         spec.stride);
            } else if (tick_count - entry_array_diag_tick > 900) {
                entry_array_diag_tick = tick_count;
                LOG_INFO(Core, "DSMod: no inventory-shaped run for vtable {:X} yet", vtable);
            }
        }
    }
    return std::nullopt;
}

/// NCE-only: among every heap array of the inventory class, return the LIVE one -- the copy whose
/// value column changes as the game is played, as opposed to the static full-template copies that
/// read current==max forever. Confirmed once, then re-verified cheaply; a full re-scan only runs
/// while unconfirmed or after the chosen array is freed.
std::optional<s64> ModRuntime::SelectLiveInventory(const ArrayFind& spec, s64 value_off) const {
    auto& memory = system.ApplicationMemory();
    const u64 vtable =
        main_region_begin + static_cast<u64>(spec.vtable) + static_cast<u64>(nce_vtable_delta);
    // Hash a candidate's value column so a change between scans is detectable.
    const auto value_hash = [&](VAddr start, int len) {
        u64 h = 1469598103934665603ULL; // FNV offset
        for (int i = 0; i < len && i < 48; ++i) {
            const VAddr v =
                start + static_cast<VAddr>(i) * spec.stride + static_cast<VAddr>(value_off);
            u32 raw = AddressIsSane(v, 4) ? memory.Read32(v) : 0;
            h = (h ^ raw) * 1099511628211ULL;
        }
        return h;
    };

    // Fast path: a confirmed-live array that still carries its vtable. Cheap, runs every frame.
    if (entry_inv_confirmed && entry_inv_sel != 0 && AddressIsSane(entry_inv_sel, 8) &&
        memory.Read64(entry_inv_sel) == vtable) {
        // Liveness watchdog: the vtable test alone would keep passing if this array were freed and
        // its slot reused by a STATIC copy of the same class, freezing the values forever. Re-hash
        // on a slow cadence; if the values have not moved across a long window of play, treat it as
        // frozen and re-discover. Aeion/energy/ammo move within that window during real play, so a
        // genuinely live copy is never dropped.
        if (tick_count - entry_inv_confirm_tick >= 300) { // ~5 s between checks
            entry_inv_confirm_tick = tick_count;
            const u64 h = value_hash(entry_inv_sel, 48);
            if (h != entry_inv_confirm_hash) {
                entry_inv_confirm_hash = h;
                entry_inv_frozen_checks = 0;
            } else if (++entry_inv_frozen_checks >=
                       24) { // ~2 min unchanged -> suspect a stale copy
                entry_inv_confirmed = false;
                entry_inv_hash.clear(); // clean baseline so re-discovery needs a real change
                entry_inv_frozen_checks = 0;
            }
        }
        if (entry_inv_confirmed) {
            return static_cast<s64>(entry_inv_sel);
        }
    }
    entry_inv_confirmed = false; // freed, frozen, or never found -- (re)discover

    // Advance the sweep at most once per tick. All six inventory points (energy, missile, aeion and
    // their maxima) share one array and call this every tick; without this guard each ran its own
    // slice, so the heap took 6x the pages/frame while unconfirmed -- the stutter this sweep exists
    // to avoid. Between advances, hand back the current provisional pick.
    if (entry_inv_swept_tick == tick_count && entry_inv_sel != 0) {
        return static_cast<s64>(entry_inv_sel);
    }
    entry_inv_swept_tick = tick_count;

    // SLICED candidate sweep. A full-heap scan in one call every N ticks is what made the game
    // stutter while unconfirmed (idle -- no value change to confirm on, so it swept 650k pages
    // over and over); firing a missile changed a value, confirmed the copy, and the sweep stopped,
    // which is exactly why it ran smoothly while firing. Sweeping a bounded slice per frame removes
    // the stall: one lap finishes in a few seconds, and a value that moves within a lap confirms.
    constexpr u64 PagesPerTick = 4096;
    const VAddr HeapBegin = HeapLow();
    const VAddr HeapEnd = HeapHigh();
    if (entry_inv_cursor < HeapBegin || entry_inv_cursor >= HeapEnd) {
        entry_inv_cursor = HeapBegin;
    }
    const VAddr slice_end = std::min<VAddr>(HeapEnd, entry_inv_cursor + PagesPerTick * 0x1000);
    for (VAddr page = entry_inv_cursor; page < slice_end; page += 0x1000) {
        const u8* const host = memory.GetPointerSilent(page);
        if (host == nullptr) {
            continue;
        }
        for (u32 at = 0; at + 8 <= 0x1000; at += 8) {
            u64 w{};
            std::memcpy(&w, host + at, sizeof(w));
            if (w != vtable) {
                continue;
            }
            const VAddr start = page + at;
            if (AddressIsSane(start - spec.stride, 8) &&
                memory.Read64(start - spec.stride) == vtable) {
                continue; // not the first entry of its run
            }
            int len = 0;
            while (len < 256 && AddressIsSane(start + len * spec.stride, 8) &&
                   memory.Read64(start + len * spec.stride) == vtable) {
                ++len;
            }
            if (len < spec.min_run) {
                continue;
            }
            if (entry_inv_first == 0) {
                entry_inv_first = start;
            }
            const u64 h = value_hash(start, len);
            entry_inv_scan_now[start] = h;
            const auto prev = entry_inv_hash.find(start);
            if (prev != entry_inv_hash.end() && prev->second != h) {
                entry_inv_sel = start; // its values moved since last lap -> the live copy
                entry_inv_confirmed = true;
                entry_inv_confirm_tick = tick_count; // seed the liveness watchdog
                entry_inv_confirm_hash = h;
                entry_inv_frozen_checks = 0;
                LOG_INFO(Core, "DSMod: live inventory confirmed at {:016X} (of {} copies seen)",
                         entry_inv_sel, entry_inv_scan_now.size());
            }
        }
    }
    entry_inv_cursor = slice_end;
    if (entry_inv_cursor >= HeapEnd) {
        // Lap done: this lap's hashes become the baseline for the next lap's change detection.
        entry_inv_hash.swap(entry_inv_scan_now);
        entry_inv_scan_now.clear();
        entry_inv_cursor = HeapBegin;
        if (entry_inv_sel == 0) {
            entry_inv_sel = entry_inv_first; // show a copy so the page is not blank
        }
        entry_inv_first = 0;
    }
    if (entry_inv_confirmed) {
        return static_cast<s64>(entry_inv_sel);
    }
    return entry_inv_sel == 0 ? std::nullopt : std::optional<s64>{static_cast<s64>(entry_inv_sel)};
}

bool ModRuntime::InGameplay() const {
    // Exploration override for games whose package has no points yet: with nothing to read,
    // neither source below can ever open the gate, and every search tool stays parked.
    // Never set this together with a tool that drives the pad on its own.
    static const bool assume = std::getenv("EDEN_DSMOD_ASSUME_GAMEPLAY") != nullptr;
    if (assume) {
        return true;
    }
    // A trigger file lets a person mark "I am in-game now" from outside -- the moment it appears,
    // the search tools (which collect their baseline the first frame gameplay is true) begin, on
    // real in-game memory rather than the title screen. Removing it pauses them again.
    static const char* const trigger = std::getenv("EDEN_DSMOD_GAMEPLAY_TRIGGER");
    if (trigger != nullptr) {
        return std::filesystem::exists(trigger);
    }
    return InGameplayHonest();
}

bool ModRuntime::InGameplayHonest() const {
    // Either source will do. This used to ask only the scripting bridge, which was fine while
    // the bridge was the only way to read the gameplay signal -- and became a bug the moment the
    // same value started coming from memory, because then the gate depended on a thing the
    // package no longer needs and every search stayed parked.
    // The point name is manifest.gameplay_point (default "energy", Dread's HP stat
    // and this gate's historical hardcoded literal), not a fixed string -- see Manifest in
    // mod_types.h.
    if (const auto signal = sequence_values.find(manifest.gameplay_point);
        signal != sequence_values.end() && signal->second > 0) {
        return true;
    }
    return last_read_gameplay_signal > 0.0f;
}

/// Re-apply state the game keeps undoing. Rules are cheap: a tick counter comparison, and at most
/// one guest call in flight at a time (RunAction is a no-op for Call while one is armed).
void ModRuntime::ApplyEnforceRules(const StateSnapshot& snapshot) {
    MaintainGuestBridge();
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    ApplyEnforceRulesDevToolsImpl(snapshot);
    if (!dev.cmd_action.empty()) {
        // A console-fired action. A sequence can only be armed while the call machinery is idle
        // (StartSequence silently declines otherwise), so hold it until then and take the turn
        // ahead of the polled sequences.
        const auto it = manifest.actions.find(dev.cmd_action);
        if (it == manifest.actions.end()) {
            LOG_INFO(Core, "DSMod action '{}': no such action", dev.cmd_action);
            dev.cmd_action.clear();
        } else if (call_state == CallState::Idle && (!dev.cmd_action_ingame || last_in_game != 0)) {
            LOG_INFO(Core, "DSMod action '{}': running", dev.cmd_action);
            RunAction(it->second, snapshot);
            dev.cmd_action.clear();
        }
    }
#endif
    RunPolledSequences(snapshot);
    if (manifest.enforce.empty()) {
        return;
    }
    // Nothing is called into the game until its own state reads sane. Before that the values are
    // garbage and the game is still building itself -- calling in then took Hollow Knight down.
    if (!manifest.enforce_gate.empty()) {
        const auto gate = snapshot.ints.find(manifest.enforce_gate);
        if (gate == snapshot.ints.end() || gate->second <= 0 ||
            gate->second > manifest.enforce_gate_max) {
            return;
        }
    }
    for (const auto& rule : manifest.enforce) {
        if (!rule.flag.empty()) {
            const auto it = flags.find(rule.flag);
            const bool value = it != flags.end() && it->second;
            if (value != rule.flag_value) {
                continue;
            }
        }
        const u64 period = MillisecondsToModTicks(rule.every_ms);
        if ((tick_count % period) != 0) {
            continue;
        }
        const auto action = manifest.actions.find(rule.action);
        if (action != manifest.actions.end()) {
            RunAction(action->second, snapshot);
        }
    }
}

void ModRuntime::MaintainGuestBridge() {
    // A call that never traps back would otherwise block every later call for the rest of the
    // session, silently. Give up on it and unpatch, so the mod keeps working.
    // Only a call that went in and never came back is stuck. Waiting for the hook is not: the
    // game calls it when it calls it, and unpatching from this thread while a guest thread is
    // about to trap on it is a race that corrupts the borrow.
    // Only a call that went in and never came back is stuck. *Waiting* for the hook is not a
    // fault: the hook is whatever the game happens to call, and a game standing still in a save
    // room can go a minute without running a line of script. Giving up on the wait and re-arming
    // on the next poll opens a gap, and the one call the game does make lands in it -- which is
    // how a sequence can work perfectly in the menus and never fire again in play.
    {
        std::scoped_lock bridge_lock{guest_bridge_mutex};
        if (call_state == CallState::InCall && tick_count - call_started_tick > StuckCallTicks) {
            // Give up on the attempt, but do not touch guest memory to do it. Unpatching from this
            // thread while a guest thread is about to trap on the same address is a race, and the
            // leftover breakpoint is harmless: whichever thread hits it next finds it in
            // patched_original, puts the instruction back and carries on.
            LOG_WARNING(Core,
                        "DSMod: a call went in and never came back; abandoning it after {} ticks",
                        StuckCallTicks);
            call_state = CallState::Idle;
            call_seq = nullptr;
        }
    }
    ApplyPatches();
}

} // namespace Core::Mods
