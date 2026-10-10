// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Unity IL2CPP lookups (Hollow Knight): finding a class and a method by the names IL2CPP keeps
// for them, because their addresses move between builds.
//   - FindClassSlotByName: the main-module slot that points at the class carrying a name. The
//     name is found in the module or near manifest.metadata_anchor; results are cached, and
//     once a class has been missed the module walk is sliced across ticks.
//   - ResolveMethodByName, DetectMethodsOffset, MethodTableLooksReal: a method's entry point.
//     The Il2CppClass method-table offset and the MethodInfo name offset move between Unity
//     versions, so candidates are validated and the layout a hit proves is kept.
// Used by ResolveSymbol (mod_guest_bridge.cpp) and ResolvePoint (mod_state.cpp).
// Why it lives here: it is specific to one engine, so it stays out of the generic guest-call
// bridge. Threads: the tick thread.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {

// IL2CPP metadata layout, taken from Il2CppDumper's generated il2cpp.h for this Unity version
// (Il2CppClass_1 is 20 pointers, which is why static_fields lands at 0xA0 -- the offset the
// existing PlayerData chain already relies on).
namespace {
constexpr s64 Il2CppClassMethods = 0x80;     ///< MethodInfo** methods
constexpr s64 Il2CppClassMethodCount = 0xF4; ///< uint16 method_count
constexpr s64 Il2CppClassName = 0x10;        ///< const char* name
constexpr s64 MethodInfoPointer = 0x00;      ///< the callable address
constexpr s64 MethodInfoName = 0x10;
/// Newer IL2CPP puts a virtualMethodPointer ahead of the name.
constexpr s64 MethodInfoNameNewer = 0x18; ///< const char* name
constexpr u32 MaxMethodsScanned = 4096;
} // namespace

bool ModRuntime::MethodTableLooksReal(VAddr table, s64 name_offset) const {
    auto& memory = OwnerMemory();
    if (!AddressIsSane(table, sizeof(u64))) {
        return false;
    }
    const VAddr first = memory.Read64(table);
    if (!AddressIsSane(first, name_offset + sizeof(u64))) {
        return false;
    }
    const VAddr name_ptr = memory.Read64(first + name_offset);
    if (!AddressIsSane(name_ptr, 1)) {
        return false;
    }
    const std::string probe = memory.ReadCString(name_ptr, 64);
    return !probe.empty() &&
           std::ranges::all_of(probe, [](unsigned char c) { return c >= 0x20 && c < 0x7F; });
}

/// Finds where the method table sits inside Il2CppClass by validating candidates rather than
/// trusting a constant: the offset moves between Unity versions (0x80 on metadata v23, 0x98 on
/// v31) and a wrong guess reads plausible-looking garbage instead of failing.
std::optional<s64> ModRuntime::DetectMethodsOffset(VAddr klass) const {
    auto& memory = OwnerMemory();
    const auto usable = [&](s64 offset) {
        if (!AddressIsSane(klass + offset, sizeof(u64))) {
            return false;
        }
        const VAddr table = memory.Read64(klass + offset);
        const s64 name_offset =
            detected_method_name_offset != 0 ? detected_method_name_offset : MethodInfoName;
        return MethodTableLooksReal(table, name_offset) ||
               MethodTableLooksReal(table, MethodInfoNameNewer);
    };
    if (detected_methods_offset != 0 && usable(detected_methods_offset)) {
        return detected_methods_offset;
    }
    if (usable(manifest.il2cpp.methods)) {
        detected_methods_offset = manifest.il2cpp.methods;
        return detected_methods_offset;
    }
    for (s64 offset = 0x60; offset <= 0xC8; offset += 8) {
        if (usable(offset)) {
            detected_methods_offset = offset;
            LOG_INFO(Core, "DSMod: Il2CppClass method table detected at +{:X}", offset);
            return offset;
        }
    }
    return std::nullopt;
}

std::optional<s64> ModRuntime::ResolveMethodByName(s64 class_slot, const std::string& method,
                                                   VAddr& method_info_out) const {
    auto& memory = OwnerMemory();
    const VAddr slot = main_region_begin + static_cast<VAddr>(class_slot);
    if (!AddressIsSane(slot, sizeof(u64))) {
        return std::nullopt;
    }
    const VAddr klass = memory.Read64(slot);
    if (!AddressIsSane(klass, manifest.il2cpp.method_count + sizeof(u16))) {
        // The class is created lazily; before it is used this is simply not there yet.
        return std::nullopt;
    }

    // Read the class name back: proves the slot really is this class rather than junk.
    std::string class_name;
    if (const VAddr name_ptr = memory.Read64(klass + manifest.il2cpp.name);
        AddressIsSane(name_ptr, 1)) {
        class_name = memory.ReadCString(name_ptr, 64);
    }

    // The method table's offset inside Il2CppClass moves between Unity versions, and a wrong
    // guess reads a count of 65535 rather than failing outright. So validate the offset instead
    // of trusting it: a real table's first entry is a MethodInfo whose name is a C string.
    // Try each plausible table offset and let a real hit prove the layout: finding the requested
    // name inside a candidate is proof, where "the first entry looks like a string" is not.
    std::vector<s64> candidates;
    if (detected_methods_offset != 0) {
        candidates.push_back(detected_methods_offset);
    }
    if (manifest.il2cpp.methods != detected_methods_offset) {
        candidates.push_back(manifest.il2cpp.methods);
    }
    for (s64 offset = 0x60; offset <= 0xC8; offset += 8) {
        candidates.push_back(offset);
    }

    // MethodInfo moves as well: newer IL2CPP gained a virtualMethodPointer ahead of the name,
    // so the name sits at 0x18 there and 0x10 before it. Probe both and let a hit decide.
    std::vector<s64> name_offsets{MethodInfoName, MethodInfoNameNewer};
    if (detected_method_name_offset != 0) {
        name_offsets.insert(name_offsets.begin(), detected_method_name_offset);
    }

    for (const s64 table_offset : candidates) {
        if (!AddressIsSane(klass + table_offset, sizeof(u64))) {
            continue;
        }
        const VAddr methods = memory.Read64(klass + table_offset);
        for (const s64 name_offset : name_offsets) {
            if (!MethodTableLooksReal(methods, name_offset)) {
                continue;
            }
            for (u32 i = 0; i < MaxMethodsScanned; ++i) {
                const VAddr entry = methods + static_cast<VAddr>(i) * sizeof(u64);
                if (!AddressIsSane(entry, sizeof(u64))) {
                    break;
                }
                const VAddr info = memory.Read64(entry);
                if (!AddressIsSane(info, MethodInfoName + sizeof(u64))) {
                    continue;
                }
                const VAddr name_ptr = memory.Read64(info + name_offset);
                if (!AddressIsSane(name_ptr, 1) || memory.ReadCString(name_ptr, 128) != method) {
                    continue;
                }
                const VAddr fn = memory.Read64(info + MethodInfoPointer);
                if (!AddressIsSane(fn, 4)) {
                    continue;
                }
                if (detected_methods_offset != table_offset ||
                    detected_method_name_offset != name_offset) {
                    detected_methods_offset = table_offset;
                    detected_method_name_offset = name_offset;
                    LOG_INFO(
                        Core,
                        "DSMod: IL2CPP layout proven -- method table at class+{:X}, MethodInfo "
                        "name at +{:X} (via {}::{})",
                        table_offset, name_offset, class_name, method);
                }
                method_info_out = info;
                return static_cast<s64>(fn - main_region_begin);
            }
        }
    }

    LOG_WARNING(Core, "DSMod: class '{}' has no method '{}' in any candidate method table",
                class_name.empty() ? "?" : class_name, method);
    return std::nullopt;
}

/// Finds a class' TypeInfo slot by the name IL2CPP stores for it. Addresses move between builds
/// and, as Hollow Knight's 1.3 and 1.4 builds show, they do not even move by a constant -- but the
/// name never changes, so looking it up beats writing an address down.
std::optional<s64> ModRuntime::FindClassSlotByName(const std::string& class_name) const {
    if (const auto cached = class_slot_cache.find(class_name); cached != class_slot_cache.end()) {
        return cached->second;
    }
    if (main_region_begin == 0 || main_region_size == 0) {
        return std::nullopt;
    }
    // A class that is not there yet is retried once a second, not on every point read.
    constexpr u64 ClassSlotRetryTicks = 60;
    if (const auto miss = class_slot_miss_tick.find(class_name);
        miss != class_slot_miss_tick.end() && tick_count - miss->second < ClassSlotRetryTicks) {
        return std::nullopt;
    }
    const auto missed = [&]() -> std::optional<s64> {
        class_slot_miss_tick[class_name] = tick_count;
        return std::nullopt;
    };
    auto& memory = OwnerMemory();

    // 1. the name itself, as a C string inside the module
    PatternFind needle;
    for (const char c : class_name) {
        needle.bytes.push_back(static_cast<u8>(c));
        needle.mask.push_back(1);
    }
    needle.bytes.push_back(0);
    needle.mask.push_back(1);
    VAddr name_address{};
    if (const auto known = class_name_address_cache.find(class_name);
        known != class_name_address_cache.end()) {
        name_address = known->second;
    } else if (const auto in_module = ScanPattern(needle); in_module) {
        name_address = main_region_begin + static_cast<VAddr>(*in_module);
    } else if (manifest.metadata_anchor != 0) {
        // Class names live in IL2CPP's metadata, which is mapped on the heap rather than inside
        // the module. A class we already know points into that same string region, so search
        // around its name rather than hunting the whole address space.
        const VAddr anchor_slot = main_region_begin + static_cast<VAddr>(manifest.metadata_anchor);
        if (!AddressIsSane(anchor_slot, sizeof(u64))) {
            return missed();
        }
        const VAddr anchor_class = memory.Read64(anchor_slot);
        if (!AddressIsSane(anchor_class, manifest.il2cpp.name + sizeof(u64))) {
            return missed();
        }
        const VAddr anchor_name = memory.Read64(anchor_class + manifest.il2cpp.name);
        constexpr u64 Window = 16ULL * 1024 * 1024;
        const VAddr window_base = anchor_name > Window / 2 ? anchor_name - Window / 2 : 0;
        const auto found = ScanRange(window_base, Window, needle);
        if (!found) {
            LOG_WARNING(Core, "DSMod: class name '{}' not found near the metadata", class_name);
            return missed();
        }
        name_address = window_base + static_cast<VAddr>(*found);
    } else {
        LOG_WARNING(Core, "DSMod: class name '{}' is not in the module and no metadata anchor",
                    class_name);
        return missed();
    }
    class_name_address_cache[class_name] = name_address;

    // 2. the slot whose class carries that name. Class objects live on the IL2CPP heap; the module
    //    holds the pointers to them, so the module is what gets walked.
    //    The first lap runs whole, as it always did, so a class that already exists resolves in
    //    the same tick. Once a class has been missed (created lazily -- HeroController at the
    //    title), later laps walk one 256 KiB chunk per tick (~1.5 ms) instead of the whole
    //    module per point read (60-90 ms a tick on Hollow Knight's title screen).
    constexpr u64 ChunkSize = 256 * 1024;
    constexpr u64 ChunksPerTick = 1;
    const bool first_lap = !class_slot_miss_tick.contains(class_name);
    u64& cursor = class_slot_cursor[class_name];
    if (!first_lap) {
        u64& sliced_at = class_slot_slice_tick[class_name];
        if (cursor != 0 && sliced_at == tick_count) {
            return std::nullopt; // this tick's slice already ran (a point resolves twice a tick)
        }
        sliced_at = tick_count;
    }
    const u64 stop = first_lap
                         ? main_region_size
                         : std::min<u64>(main_region_size, cursor + ChunksPerTick * ChunkSize);
    std::vector<u8> chunk(ChunkSize + sizeof(u64));
    u64 base = cursor;
    for (; base + sizeof(u64) <= main_region_size && base < stop; base += ChunkSize) {
        const u64 span = std::min<u64>(ChunkSize + sizeof(u64), main_region_size - base);
        if (!AddressIsSane(main_region_begin + base, span)) {
            continue;
        }
        memory.ReadBlock(main_region_begin + base, chunk.data(), span);
        for (u64 i = 0; i + sizeof(u64) <= span; i += sizeof(u64)) {
            u64 candidate{};
            std::memcpy(&candidate, chunk.data() + i, sizeof(candidate));
            if (candidate <= main_region_begin || !AddressIsSane(candidate, 0x20)) {
                continue;
            }
            if (memory.Read64(candidate + manifest.il2cpp.name) != name_address) {
                continue;
            }
            const s64 slot = static_cast<s64>(base + i);
            class_slot_cache[class_name] = slot;
            class_slot_cursor.erase(class_name);
            LOG_INFO(Core, "DSMod: class '{}' found at main+{:X}", class_name, slot);
            return slot;
        }
    }
    if (base + sizeof(u64) <= main_region_size) {
        cursor = base; // lap in progress: resume here next tick
        return std::nullopt;
    }
    cursor = 0;
    LOG_WARNING(Core, "DSMod: no slot for class '{}'", class_name);
    return missed();
}

} // namespace Core::Mods
