// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include "common/swap.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys::Tests {

inline void Put32(std::vector<u8>& bytes, std::size_t offset, u32 value) {
    const u32_le little = value;
    std::memcpy(bytes.data() + offset, &little, sizeof(little));
}

inline void Put64(std::vector<u8>& bytes, std::size_t offset, u64 value) {
    const u64_le little = value;
    std::memcpy(bytes.data() + offset, &little, sizeof(little));
}

inline std::vector<u8> MakeNpdm(bool ounce) {
    const u32 body = ounce ? 0x300U : 0x200U;
    const u32 fac = body + 0x40;
    const u32 acid_size = body + 0x74;
    const u32 aci = ounce ? 0x400U : 0x300U;
    std::vector<u8> bytes(aci + 0x60);
    std::memcpy(bytes.data(), "META", 4);
    bytes[0xC] = 0x27;
    bytes[0xE] = 0x2C;
    Put32(bytes, 0x10, ounce ? 0x3000000U : 0U);
    Put32(bytes, 0x70, aci);
    Put32(bytes, 0x74, 0x60);
    Put32(bytes, 0x78, 0x80);
    Put32(bytes, 0x7C, acid_size);
    std::memcpy(bytes.data() + 0x80 + body, "ACID", 4);
    Put32(bytes, 0x80 + body + 0x20, fac);
    Put32(bytes, 0x80 + body + 0x24, 0x2C);
    Put32(bytes, 0x80 + body + 0x30, body + 0x70);
    Put32(bytes, 0x80 + body + 0x34, 4);
    bytes[0x80 + fac] = 1;
    Put64(bytes, 0x80 + fac + 4, 0x0123456789ABCDEFULL);
    std::memcpy(bytes.data() + aci, "ACI0", 4);
    Put64(bytes, aci + 0x10, 0x0400000000010000ULL);
    Put32(bytes, aci + 0x20, 0x40);
    Put32(bytes, aci + 0x24, 0x1C);
    Put32(bytes, aci + 0x30, 0x5C);
    Put32(bytes, aci + 0x34, 4);
    bytes[aci + 0x40] = 1;
    Put64(bytes, aci + 0x44, 0x4000000000000000ULL);
    Put32(bytes, aci + 0x5C, 0x30043F7);
    return bytes;
}

class MetadataFile final : public VfsFile {
public:
    explicit MetadataFile(std::vector<u8> bytes_) : bytes(std::move(bytes_)) {}
    std::string GetName() const override { return "metadata"; }
    std::size_t GetSize() const override { return bytes.size(); }
    bool Resize(std::size_t) override { return false; }
    VirtualDir GetContainingDirectory() const override { return nullptr; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::size_t Read(u8* out, std::size_t length, std::size_t offset) const override {
        if (offset >= bytes.size()) {
            return 0;
        }
        const auto size = (std::min)(length, bytes.size() - offset);
        if (size == 0) {
            return 0;
        }
        std::memcpy(out, bytes.data() + offset, size);
        return size;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override { return 0; }
    bool Rename(std::string_view) override { return false; }

private:
    std::vector<u8> bytes;
};

} // namespace FileSys::Tests
