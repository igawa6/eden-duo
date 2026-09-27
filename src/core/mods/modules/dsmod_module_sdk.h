// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Header-only helpers shared by the native title modules (MK8D, Metroid Dread, P5R). Start here
// when writing a new module: the title file then holds only the game-specific part.
//
//   int_types        u8 .. s64 aliases
//   byte order       Le16/Le32/Le64, Be16/Be32/Be64 over a raw byte pointer (no bounds check)
//   FNV-1a 64        Fnv1a64 / Fnv1a64Step (code fingerprints, key hashes)
//   ranges           InMain: is [at, at + n) inside an image [base, base + size)?
//   guest reads      ReadGuest<MissingIsMapped> and RangeMapped over EdenDsmodHostApi,
//                    Get(read, at, out) over a module's own read(at, dst, size) callable
//   host checks      HostAbiMatches: the ABI prologue every create() starts with
//   exports          DSMOD_SDK_EXPORT_* define the extern "C" table getters
//
// The helpers reproduce the modules' previous private copies exactly; where the copies
// disagreed (the is_mapped policy, zero-size reads, the lowest accepted address) the difference
// is a parameter, never silently unified. Depends only on the C ABI headers and the standard
// library.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace dsmod_sdk {

// ---- integer aliases -------------------------------------------------------------------------
// A module brings these into its own namespace with `using namespace dsmod_sdk::int_types;`.
inline namespace int_types {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
} // namespace int_types

// ---- byte order ------------------------------------------------------------------------------
// Explicit little/big-endian loads from a raw pointer; the caller has checked the bounds.
// (Native-order memcpy loads are a different thing and stay where they are.)
constexpr u16 Le16(const u8* p) {
    return u16(p[0] | p[1] << 8);
}
constexpr u32 Le32(const u8* p) {
    return u32(Le16(p)) | u32(Le16(p + 2)) << 16;
}
constexpr u64 Le64(const u8* p) {
    return u64(Le32(p)) | u64(Le32(p + 4)) << 32;
}
constexpr u16 Be16(const u8* p) {
    return u16(p[0] << 8 | p[1]);
}
constexpr u32 Be32(const u8* p) {
    return u32(Be16(p)) << 16 | Be16(p + 2);
}
constexpr u64 Be64(const u8* p) {
    return u64(Be32(p)) << 32 | Be32(p + 4);
}

// ---- FNV-1a 64 -------------------------------------------------------------------------------
// The one FNV variant the modules use: 64-bit, standard offset basis and prime, one unsigned
// byte per step. Pass the previous result as `h` to hash a range in chunks.
inline constexpr u64 Fnv1a64Basis = 0xcbf29ce484222325ULL;
inline constexpr u64 Fnv1a64Prime = 0x100000001b3ULL;
constexpr u64 Fnv1a64Step(u64 h, u8 byte) {
    return (h ^ byte) * Fnv1a64Prime;
}
constexpr u64 Fnv1a64(const u8* p, std::size_t n, u64 h = Fnv1a64Basis) {
    for (std::size_t i = 0; i < n; ++i)
        h = Fnv1a64Step(h, p[i]);
    return h;
}
constexpr u64 Fnv1a64(std::span<const u8> bytes, u64 h = Fnv1a64Basis) {
    return Fnv1a64(bytes.data(), bytes.size(), h);
}

// ---- ranges ----------------------------------------------------------------------------------
// [at, at + n) lies inside [base, base + size). Written so that nothing can wrap.
constexpr bool InMain(u64 base, u64 size, u64 at, u64 n) {
    return at >= base && size >= n && at - base <= size - n;
}

// ---- guest reads -----------------------------------------------------------------------------
// What a guest read does when the host left EdenDsmodHostApi::is_mapped null. The modules
// disagree, and each keeps its behaviour:
//   Reject        MK8D and Dread. They check every pointer before following it and treat an
//                 unverifiable range as unreadable (MK8D's create() even refuses such a host).
//                 Zero-size ranges fail. ReadGuest (MK8D) also fails a null address;
//                 RangeMapped (Dread) leaves address 0 to is_mapped.
//   AssumeMapped  P5R. It relies on read_memory failing for an unmapped range; is_mapped,
//                 when present, is only an early out. A null address still fails, a zero-size
//                 read is passed on to read_memory.
enum class MissingIsMapped { Reject, AssumeMapped };

template <MissingIsMapped Policy>
inline bool ReadGuest(const EdenDsmodHostApi& host, u64 at, void* out, std::size_t size) {
    if constexpr (Policy == MissingIsMapped::Reject) {
        return at && size && at <= std::numeric_limits<u64>::max() - size && host.is_mapped &&
               host.read_memory && host.is_mapped(host.userdata, at, size) &&
               host.read_memory(host.userdata, at, out, size);
    } else {
        if (!at || at > std::numeric_limits<u64>::max() - size || !host.read_memory ||
            (host.is_mapped && !host.is_mapped(host.userdata, at, size)))
            return false;
        return host.read_memory(host.userdata, at, out, size) != 0;
    }
}

// Dread's pointer check before a read: a non-empty, non-wrapping range the host reports as
// mapped (MissingIsMapped::Reject). Address 0 is left to is_mapped.
inline bool RangeMapped(const EdenDsmodHostApi& host, u64 at, u64 size) {
    return size != 0 && at <= UINT64_MAX - size && host.is_mapped &&
           host.is_mapped(host.userdata, at, size) != 0;
}

// One T through a module's own read(at, dst, size) callable (the P5R decoders take one so that
// tests can feed them fixtures). Addresses below `min_address` fail: 1 rejects only null.
template <class Read, class T>
bool Get(Read& read, u64 at, T& out, u64 min_address = 1) {
    return at >= min_address && at <= std::numeric_limits<u64>::max() - sizeof(T) &&
           read(at, &out, sizeof(T));
}

// ---- host checks -----------------------------------------------------------------------------
// The host speaks this module ABI (version, hash) and its table is at least as large as ours.
// Title id, build id and required callbacks remain each module's own checks.
inline bool HostAbiMatches(const EdenDsmodHostApi* host) {
    return host && host->abi_version == EDEN_DSMOD_MODULE_ABI_VERSION &&
           host->abi_hash == EDEN_DSMOD_MODULE_ABI_HASH &&
           host->struct_size >= sizeof(EdenDsmodHostApi);
}

} // namespace dsmod_sdk

// ---- exported table getters ------------------------------------------------------------------
// Each defines one extern "C" getter that returns &table when the host asks for exactly this
// version and hash, else null. Use at global scope, once per table the module offers. Which
// getters are visible in the .so is decided by the module's linker export list, not here.
#define DSMOD_SDK_DEFINE_GETTER(symbol, Table, table_version, table_hash, table)                   \
    extern "C" __attribute__((visibility("default"))) const Table* symbol(std::uint32_t version,   \
                                                                          std::uint64_t hash) {    \
        return version == (table_version) && hash == (table_hash) ? &(table) : nullptr;            \
    }
#define DSMOD_SDK_EXPORT_MODULE(table)                                                             \
    DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_module, EdenDsmodModuleApi,                             \
                            EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH, table)
#define DSMOD_SDK_EXPORT_EXTENSIONS(table)                                                         \
    DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_extensions, EdenDsmodModuleExtensions,                  \
                            EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH, table)
#define DSMOD_SDK_EXPORT_FONT_EXTENSIONS(table)                                                    \
    DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_font_extensions, EdenDsmodFontExtensions,               \
                            EDEN_DSMOD_FONT_EXT_VERSION, EDEN_DSMOD_FONT_EXT_HASH, table)
#define DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(table)                                                   \
    DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_write_extensions, EdenDsmodModuleWriteExtensions,       \
                            EDEN_DSMOD_WRITE_EXT_VERSION, EDEN_DSMOD_WRITE_EXT_HASH, table)
#define DSMOD_SDK_EXPORT_DATA_EXTENSIONS(table)                                                    \
    DSMOD_SDK_DEFINE_GETTER(eden_dsmod_get_data_extensions, EdenDsmodModuleDataExtensions,         \
                            EDEN_DSMOD_DATA_EXT_VERSION, EDEN_DSMOD_DATA_EXT_HASH, table)
