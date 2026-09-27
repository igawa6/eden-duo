// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mods/modules/mk8d_assets.h"

#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
void Put32(Bytes& b, size_t o, std::uint32_t v) {
    std::memcpy(b.data() + o, &v, 4);
}
void Put64(Bytes& b, size_t o, std::uint64_t v) {
    std::memcpy(b.data() + o, &v, 8);
}

Bytes TinyLinearBntx() {
    Bytes b(0x180);
    std::memcpy(b.data(), "BNTX", 4);
    Put32(b, 0x1c, b.size());
    Put32(b, 0x24, 1);
    Put64(b, 0x28, 0x40);
    Put64(b, 0x40, 0x60);
    std::memcpy(b.data() + 0x60, "BRTI", 4);
    b[0x72] = 1; // pitch-linear tile mode
    Put32(b, 0x7c, 11u << 8);
    Put32(b, 0x84, 1);
    Put32(b, 0x88, 1);
    Put32(b, 0x90, 1);
    Put32(b, 0xb0, 4);
    Put32(b, 0xb8, 0x05040302);
    Put64(b, 0xc0, 0x130);
    Put64(b, 0xd0, 0x120);
    Put64(b, 0x120, 0x140);
    b[0x130] = 1;
    b[0x132] = 'x';
    b[0x140] = 10;
    b[0x141] = 20;
    b[0x142] = 30;
    b[0x143] = 40;
    return b;
}
Bytes TinyBlockLinearBc1() {
    Bytes b(0x400);
    std::memcpy(b.data(), "BNTX", 4);
    Put32(b, 0x1c, b.size());
    Put32(b, 0x24, 1);
    Put64(b, 0x28, 0x40);
    Put64(b, 0x40, 0x60);
    std::memcpy(b.data() + 0x60, "BRTI", 4);
    // tile_mode=0, block_height=0, one 4x4 BC1 block in a 64x8-byte GOB.
    Put32(b, 0x7c, 26u << 8);
    Put32(b, 0x84, 4);
    Put32(b, 0x88, 4);
    Put32(b, 0x90, 1);
    Put32(b, 0x94, 0);
    Put32(b, 0xb0, 512);
    Put32(b, 0xb8, 0x05040302);
    Put64(b, 0xc0, 0x130);
    Put64(b, 0xd0, 0x120);
    Put64(b, 0x120, 0x180);
    b[0x130] = 1;
    b[0x132] = 'x';
    b[0x180] = 0x00;
    b[0x181] = 0xf8; // RGB565 red endpoint; all indices select it.
    return b;
}
struct Fake {
    std::unordered_map<std::string, Bytes> files;
};
size_t Read(void* p, const char* path, std::uint64_t offset, void* out, size_t size) {
    auto& files = static_cast<Fake*>(p)->files;
    auto it = files.find(path);
    if (it == files.end() || offset > it->second.size())
        return 0;
    if (!out && !size)
        return it->second.size();
    const auto count = std::min(size, it->second.size() - static_cast<size_t>(offset));
    std::memcpy(out, it->second.data() + offset, count);
    return count;
}
} // namespace

int main() {
    Fake fake;
    fake.files["/Course/Test/course_maptexture.bntx"] = TinyLinearBntx();
    EdenDsmodHostApi host{};
    host.userdata = &fake;
    host.read_romfs = Read;
    Mk8dAssets::Decoder decoder;
    auto image = decoder.LoadImage(host, "mk8d/maps/Test.png");
    assert(image && image->width == 1 && image->height == 1);
    assert((image->rgba == Bytes{10, 20, 30, 40}));
    fake.files["/Course/Block/course_maptexture.bntx"] = TinyBlockLinearBc1();
    auto block = decoder.LoadImage(host, "mk8d/maps/Block.png");
    assert(block && block->width == 4 && block->height == 4 && block->rgba.size() == 64);
    assert(block->rgba[0] == 255 && block->rgba[1] == 0 && block->rgba[2] == 0 &&
           block->rgba[3] == 255);
    assert(!decoder.LoadImage(host, "mk8d/maps/../Test.png"));
    assert(!decoder.LoadImage(host, "mk8d/items/item_1a.png"));
    fake.files["/Course/Bad/course_maptexture.bntx"] = Bytes{'B', 'N', 'T', 'X'};
    assert(!decoder.LoadImage(host, "mk8d/maps/Bad.png"));
    fake.files["/Course/Test/course_mapcamera.bin"] = Bytes(43);
    assert(!decoder.LoadMapCamera(host, "Test"));
    assert(Mk8dAssets::Decoder::DriverImageKey(4, 99) == "mk8d/characters/tc_MapChara_Yoshi08.png");
}
