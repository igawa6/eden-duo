// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <span>
namespace Core::Mods {
// BFRES 3.5 little-endian external-file table. Offsets are file-relative.
// Only return a bounded, declared BNTX member; never search arbitrary file bytes.
inline std::span<const uint8_t> BfresBntx(std::span<const uint8_t> b) {
    auto u16 = [&](size_t p) -> uint16_t { return uint16_t(b[p]) | uint16_t(b[p+1]) << 8; };
    auto u32 = [&](size_t p) { uint32_t v{}; for (unsigned i=0;i<4;++i) v |= uint32_t(b[p+i]) << (i*8); return v; };
    auto u64 = [&](size_t p) { uint64_t v{}; for (unsigned i=0;i<8;++i) v |= uint64_t(b[p+i]) << (i*8); return v; };
    if (b.size() < 0xd0 || std::memcmp(b.data(), "FRES", 4) ||
        u32(8) != 0x00050003 || u16(0xc) != 0xfeff || u32(0x1c) != b.size())
        return {};
    const auto table = u64(0x98);
    const auto count = u16(0xc8);
    if (!count || count > 64 || table > b.size() || uint64_t(count)*16 > b.size()-table)
        return {};
    for (unsigned i=0;i<count;++i) {
        const auto p = u64(table+i*16), n = u64(table+i*16+8);
        if (p > b.size() || n > b.size()-p || n < 4) return {};
        if (!std::memcmp(b.data()+p, "BNTX", 4)) return b.subspan(p,n);
    }
    return {};
}
} // namespace Core::Mods
