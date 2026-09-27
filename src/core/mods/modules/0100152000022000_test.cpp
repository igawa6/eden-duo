// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone synthetic-memory test. Build manually with 0100152000022000.cpp; it deliberately
// has no Eden/Core or test-framework dependency and is not part of the emulator target.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
struct Host {
    std::unordered_map<u64, u8> memory;
    std::unordered_map<std::string, std::int64_t> ints;
    std::unordered_map<std::string, double> floats;
    u64 tick{}, reads{};
    u64 mailbox_request{}, mailbox_completed{};
    u32 mailbox_state{};
    std::vector<std::string> mailbox_ops;
    template <typename T>
    void Put(u64 at, const T& value) {
        const auto* p = reinterpret_cast<const u8*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i)
            memory[at + i] = p[i];
    }
    template <typename T>
    void PutArray(u64 at, const T& value) {
        Put(at, value);
    }
};
EdenDsmodBool Mapped(void* p, u64 at, u64 size) {
    auto& h = *static_cast<Host*>(p);
    for (u64 i = 0; i < size; ++i)
        if (!h.memory.contains(at + i))
            return 0;
    return 1;
}
EdenDsmodBool Read(void* p, u64 at, void* out, size_t size) {
    auto& h = *static_cast<Host*>(p);
    ++h.reads;
    if (!Mapped(p, at, size))
        return 0;
    auto* bytes = static_cast<u8*>(out);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = h.memory.at(at + i);
    return 1;
}
u64 Tick(void* p) {
    return static_cast<Host*>(p)->tick;
}
void I64(void* p, const char* n, std::int64_t v) {
    static_cast<Host*>(p)->ints[n] = v;
}
void F64(void* p, const char* n, double v) {
    static_cast<Host*>(p)->floats[n] = v;
}
void Noop(void*) {}
size_t Romfs(void*, const char* path, u64 offset, void* output, size_t size) {
    if (std::string{path} != "/Course/Gu_MarioCircuit/course_mapcamera.bin" || offset != 0)
        return 0;
    const std::array<float, 11> camera{0, 10, 0, 0, 0, 0, 0, 0, 1, 100, 100};
    if (!output)
        return sizeof(camera);
    if (size < sizeof(camera))
        return 0;
    std::memcpy(output, camera.data(), sizeof(camera));
    return sizeof(camera);
}
EdenDsmodBool Load32(void* p, u32 offset, u32* value) {
    auto& h = *static_cast<Host*>(p);
    if (!value || offset != 16)
        return 0;
    h.mailbox_ops.emplace_back("load_state");
    *value = h.mailbox_state;
    return 1;
}
EdenDsmodBool Load64(void* p, u32 offset, u64* value) {
    auto& h = *static_cast<Host*>(p);
    if (!value || (offset != 0 && offset != 8))
        return 0;
    h.mailbox_ops.emplace_back(offset == 0 ? "load_request" : "load_completed");
    *value = offset == 0 ? h.mailbox_request : h.mailbox_completed;
    return 1;
}
EdenDsmodBool Store32(void* p, u32 offset, u32 value) {
    auto& h = *static_cast<Host*>(p);
    if (offset != 16)
        return 0;
    h.mailbox_ops.emplace_back("store_state");
    h.mailbox_state = value;
    return 1;
}
EdenDsmodBool Store64(void* p, u32 offset, u64 value) {
    auto& h = *static_cast<Host*>(p);
    if (offset != 0 && offset != 8)
        return 0;
    h.mailbox_ops.emplace_back(offset == 0 ? "store_request" : "store_completed");
    (offset == 0 ? h.mailbox_request : h.mailbox_completed) = value;
    return 1;
}

EdenDsmodHostApi Api(Host& h) {
    EdenDsmodHostApi api{};
    api.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    api.struct_size = sizeof(api);
    api.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    api.userdata = &h;
    api.title_id = UINT64_C(0x0100152000022000);
    api.main_base = 0x10000000;
    api.main_size = 0x02000000;
    api.get_tick = Tick;
    api.is_mapped = Mapped;
    api.read_memory = Read;
    api.begin_output = Noop;
    api.publish_i64 = I64;
    api.publish_f64 = F64;
    api.end_output = Noop;
    api.read_romfs = Romfs;
    return api;
}

void BuildRace(Host& h) {
    constexpr u64 base = 0x10000000, cell = 0x20000000, root = 0x20000100, sh = 0x20000200;
    constexpr u64 scene = 0x20000300, engine = 0x20000500, rh = 0x20000800, re = 0x20000900;
    constexpr u64 info = 0x20000c00, rd = 0x20001000, checks = 0x20001200, kd = 0x20001400;
    constexpr u64 holders = 0x20001600, holder0 = 0x20001800, vehicle0 = 0x20001a00;
    constexpr u64 move = 0x20001c00, pos = 0x20001e00, check0 = 0x20002000, lap = 0x20002200;
    constexpr u64 itemd = 0x20002400, owners = 0x20002600, owner = 0x20002800;
    constexpr u64 slot0 = 0x20002a00, slot1 = 0x20002c00;
    h.Put(base + 0x012F6388, cell);
    h.Put(cell, root);
    h.Put(root + 0x20, sh);
    h.Put(sh + 8, scene);
    h.Put(scene + 0x180, u32{4});
    h.Put(scene + 0x1b0, engine);
    h.Put(root + 0x28, rh);
    h.Put(rh + 0x240, re);
    h.Put(re + 0x10, info);
    h.Put(info + 0x180, u32{1});
    h.Put(info + 8, u32{0});
    h.Put(info + 0x1a0, u32{0x10});
    h.Put(info + 0x30 + 0x0c, u32{9});
    h.Put(info + 0x30 + 0x10, u8{2});
    h.Put(info + 0x30 + 0x14, u32{1});
    h.Put(engine + 0x218, rd);
    h.Put(rd + 0x60, u32{1});
    h.Put(rd + 0x68, checks);
    h.Put(checks, check0);
    h.Put(rd + 0x58, lap);
    h.Put(lap + 0x64, u8{3});
    std::array<u32, 2> rank_lap{0, 1};
    h.PutArray(check0 + 0x40, rank_lap);
    h.Put(check0 + 0x50, u8{6});
    h.Put(engine + 0x238, kd);
    h.Put(kd + 0xc8, holders);
    h.Put(holders, holder0);
    h.Put(holder0 + 8, vehicle0);
    std::array<u32, 4> identity{0, 0, 0, 9};
    h.PutArray(vehicle0 + 0xa8, identity);
    h.Put(vehicle0 + 0xd0, u8{1});
    h.Put(vehicle0 + 0x28, move);
    h.Put(move + 0x50, pos);
    std::array<float, 3> xyz{1.5f, 2.5f, 3.5f};
    h.PutArray(pos, xyz);
    h.Put(engine + 0x240, itemd);
    h.Put(itemd + 0x38, owners);
    h.Put(owners, owner);
    h.Put(owner + 0x40, u32{0});
    h.Put(owner + 0x60, slot0);
    h.Put(owner + 0x68, slot1);
    h.Put(slot0 + 0x68, u32{0});
    h.Put(slot1 + 0x68, u32{1});
    h.Put(slot0 + 0x70, owner);
    h.Put(slot1 + 0x70, owner);
    for (const u64 slot : {slot0, slot1}) {
        for (u64 at = slot + 0x40; at < slot + 0xcc; ++at)
            if (!h.memory.contains(at))
                h.memory[at] = 0;
        h.Put(slot + 0x41, u8{3});
        h.Put(slot + 0x9c, std::int32_t{4});
        h.Put(slot + 0xa0, u32{1});
    }
    // Fill every requested byte in bulk structures; individual values above remain intact.
    for (u64 at = info + 0x30; at < info + 0x4c; ++at)
        if (!h.memory.contains(at))
            h.memory[at] = 0;
    for (u64 at = checks; at < checks + 8; ++at)
        if (!h.memory.contains(at))
            h.memory[at] = 0;
    for (u64 at = holders; at < holders + 8; ++at)
        if (!h.memory.contains(at))
            h.memory[at] = 0;
    for (u64 at = owners; at < owners + 8; ++at)
        if (!h.memory.contains(at))
            h.memory[at] = 0;
}
} // namespace

int main() {
    const auto* module =
        eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
    assert(module && module->capabilities == EDEN_DSMOD_CAP_EXTENSIONS);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    assert(ext);
    assert(
        module->supports_build("FE941ED5BA14BE5D505698DA1BBF4FE700000000000000000000000000000000"));
    assert(!module->supports_build("FE941ED5BA14BE5D")); // exact full build ID only
    Host h;
    auto api = Api(h);
    void* instance = module->create(&api, nullptr);
    assert(instance);
    module->sample(instance, &api);
    assert(h.ints["race.active"] == 0 && h.ints["player.0.active"] == 0);
    BuildRace(h);
    h.tick = 4;
    module->sample(instance, &api);
    assert(h.ints["race.active"] == 1 && h.ints["race.local_player_id"] == 0);
    assert(h.ints["player.0.driver_id"] == 9 && h.ints["player.0.driver_variant"] == 2);
    assert(h.ints["player.0.rank"] == 1 && h.ints["player.0.lap"] == 2 &&
           h.ints["player.0.coins"] == 6);
    assert(h.ints["display.0.player_id"] == 0 && h.floats["player.0.z"] == 3.5);
    assert(h.ints["player.0.map_valid"] == 1 && h.floats["player.0.map_x"] > 0.48 &&
           h.floats["player.0.map_x"] < 0.50);
    EdenDsmodHostExtensions host_ext{EDEN_DSMOD_EXT_VERSION,
                                     sizeof(EdenDsmodHostExtensions),
                                     EDEN_DSMOD_EXT_HASH,
                                     &h,
                                     0x30000000,
                                     0x20,
                                     Load32,
                                     Load64,
                                     Store32,
                                     Store64,
                                     nullptr};
    ext->configure(instance, &host_ext);
    h.mailbox_ops.clear();
    assert(ext->on_action(instance, "activate_item", 0));
    assert(h.mailbox_request == 1 && h.mailbox_state == 1);
    assert(
        (h.mailbox_ops == std::vector<std::string>{"load_state", "load_request", "store_completed",
                                                   "store_request", "store_state"}));
    assert(!ext->on_action(instance, "activate_item", 1)); // queued mailbox is busy
    h.mailbox_request = 0;
    h.mailbox_state = 3;
    h.mailbox_completed = 0;
    module->tick(instance, &api); // completion polling remains alive without a visible aux surface
    module->sample(instance, &api);
    assert(h.ints["action.busy"] == 0 && h.ints["action.state"] == 3);
    assert(h.ints["local.item.0.id"] == 4 && h.ints["local.driver_id"] == 9);
    host_ext.mailbox_size = 16;
    ext->configure(instance, &host_ext);
    assert(!ext->on_action(instance, "activate_item", 0)); // out-of-range mailbox rejected
    const u64 reads = h.reads;
    h.memory.erase(0x20002000 + 0x40);
    h.tick = 5;
    module->sample(instance, &api);
    assert(h.reads == reads && h.ints["race.active"] == 1); // cached publication between samples
    h.tick = 8;
    module->sample(instance, &api);
    assert(h.ints["race.active"] == 1); // bad racer read is safe
    h.memory.erase(0x20000100 + 0x20);
    h.tick = 12;
    module->sample(instance, &api);
    assert(h.ints["race.active"] == 0 && h.ints["player.0.active"] == 0 &&
           h.ints["race.course_id"] == 0);
    module->destroy(instance);
}
