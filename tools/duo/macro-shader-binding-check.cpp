// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

#include <array>
#include <cstdio>
#include <span>
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"

int main() {
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    using Engine = Tegra::Engines::Maxwell3D;
    using Mode = Engine::Regs::ShadowRamControl;
    // Captured Z-A slot 12: method 0xe18 -> code position 0x81,
    // hash eb29b2a09aa06d38 (HLE_BindShader), including its exit delay slots.
    constexpr std::array<u32, 28> code{
        0x03470a15, 0x00000401, 0x0004a210, 0x00055007,
        0x00000301, 0x02004211, 0x21005212, 0x61801022,
        0x00001841, 0x03470a11, 0x61801022, 0x00002041,
        0x03488a11, 0x61801022, 0x00001a31, 0x06380151,
        0x40000041, 0x02304042, 0x46004042, 0x19c08112,
        0x02410911, 0x618008a2, 0x00044041, 0x00000011,
        0x00000281, 0x00000101, 0x00000091, 0x00000011,
    };
    Settings::values.disable_macro_jit = true;
    Settings::values.dump_macros = false;
    const auto dispatch = [&](Engine& engine, const auto& params, bool interpreted) {
        Settings::values.disable_macro_hle = interpreted;
        // Execute only after the final parameter has entered ProcessMacro.
        for (size_t i = 0; i < params.size(); ++i) {
            engine.CallMethod(system, i == 0 ? 0xe18 : 0xe19,
                              params[i], i + 1 == params.size());
        }
    };
    for (const u32 address : {0x04001902u, 0x050fedcbu}) {
        for (const bool equal_token : {false, true}) {
            for (const Mode mode : {Mode::Track, Mode::TrackWithFilter,
                                    Mode::Passthrough, Mode::Replay}) {
                Engine hle{memory};
                Engine reference{memory};
                Engine consumption{memory};
                const std::array<u32, 5> parameters{1, 0x190, 0x190030, 0, address};
                for (Engine* engine : {&hle, &reference, &consumption}) {
                    engine->BindRasterizer(&rasterizer);
                    engine->CallMethod(system, 0x49, static_cast<u32>(Mode::Track), true);
                    for (const auto [method, value] : std::array<std::array<u32, 2>, 7>{{
                             {0x811, 0x1230}, {0xd1d, equal_token ? 0x190u : 0x123u},
                             {0xd23, 0x1230}, {0x8e0, 0x2000}, {0x8e1, 2},
                             {0x8e2, 0x34567800}, {0x904, 0x11}}}) {
                        engine->CallMethod(system, method, value, true);
                    }
                    // instruction_ptr remains the code's base while instruction writes append.
                    engine->CallMethod(system, 0x45, 0x81, true);
                    for (const u32 word : code) {
                        engine->CallMethod(system, 0x46, word, true);
                    }
                    engine->CallMethod(system, 0x47, 12, true);
                    engine->CallMethod(system, 0x48, 0x81, true);
                    engine->CallMethod(system, 0x49, static_cast<u32>(mode), true);
                }
                const auto check = [&](const char* phase, const auto& params) {
                    // The dispatch cache is private. An additional real-engine interpreter run
                    // exposes its public consumption cursor even in NDEBUG builds, and is
                    // compared with dispatched LLE so this audit cannot replace production flow.
                    Tegra::MacroInterpreterImpl interpreter{code};
                    interpreter.Execute(system, consumption, params, 0x81);
                    if (interpreter.next_parameter_index != params.size()) {
                        std::fprintf(stderr, "FAIL mode=%u equal=%u address=%08x phase=%s consumed=%u expected=%zu\n",
                                     static_cast<u32>(mode), unsigned(equal_token), address, phase,
                                     interpreter.next_parameter_index, params.size());
                        return false;
                    }
                    dispatch(reference, params, true);
                    dispatch(hle, params, false);
                    const auto field = [&](const char* name, size_t index, u64 actual, u64 expected,
                                           const char* path) {
                        if (actual == expected) return true;
                        std::fprintf(stderr, "FAIL mode=%u equal=%u address=%08x phase=%s path=%s field=%s[%zu] actual=%llx reference=%llx\n",
                                     static_cast<u32>(mode), unsigned(equal_token), address, phase,
                                     path, name, index, static_cast<unsigned long long>(actual),
                                     static_cast<unsigned long long>(expected));
                        return false;
                    };
                    for (const Engine* actual : {&consumption, &hle}) {
                        const char* path = actual == &hle ? "HLE" : "consumption-audit";
                        // Only semantic fields, never struct padding or unrelated registers.
                        for (const u32 method : {0x811u, 0xd1du, 0xd23u, 0x8e0u,
                                                 0x8e1u, 0x8e2u, 0x904u}) {
                            if (!field("regs", method, actual->regs.reg_array[method],
                                       reference.regs.reg_array[method], path) ||
                                !field("shadow", method, actual->shadow_state.reg_array[method],
                                       reference.shadow_state.reg_array[method], path)) return false;
                        }
                        for (size_t stage = 0; stage < Engine::Regs::MaxShaderStage; ++stage) {
                            for (size_t slot = 0; slot < Engine::Regs::MaxConstBuffers; ++slot) {
                                const auto& a = actual->state.shader_stages[stage].const_buffers[slot];
                                const auto& r = reference.state.shader_stages[stage].const_buffers[slot];
                                const size_t index = stage * Engine::Regs::MaxConstBuffers + slot;
                                if (!field("cb.enabled", index, a.enabled, r.enabled, path) ||
                                    !field("cb.address", index, a.address, r.address, path) ||
                                    !field("cb.size", index, a.size, r.size, path)) return false;
                            }
                        }
                    }
                    return true;
                };
                if (!check("initial", parameters)) return 1;
                if (mode == Mode::Track || mode == Mode::TrackWithFilter) {
                    for (Engine* engine : {&hle, &reference, &consumption}) {
                        engine->CallMethod(system, 0x49, static_cast<u32>(Mode::Replay), true);
                    }
                    const std::array<u32, 5> replacement{1, 0x19a, 0x1f0030, 0, 0x060fedcb};
                    if (!check("track-to-replay", replacement)) return 1;
                }
            }
        }
    }
    // Captured constant-buffer helpers must also update shadow RAM: subsequent replayed
    // SENDs are consumed by the real constant-buffer binding path, not just register checks.
    constexpr std::array<u32, 6> offset_code{
        0x03460215, 0x06380021, 0x1c000041, 0x02308042, 0x460080c2, 0x17804042};
    constexpr std::array<u32, 8> scratch_code{
        0x034a8b15, 0x06380021, 0x034bc915, 0x0230c212,
        0x4600c312, 0x00000841, 0x000010c1, 0x00001841};
    for (const bool offset_helper : {true, false}) {
        Engine hle{memory};
        Engine reference{memory};
        const std::span<const u32> helper_code = offset_helper
                                                   ? std::span<const u32>{offset_code}
                                                   : std::span<const u32>{scratch_code};
        const std::array<u32, 1> helper_parameters{offset_helper ? 0x1a0u : 1u};
        for (Engine* engine : {&hle, &reference}) {
            engine->BindRasterizer(&rasterizer);
            engine->CallMethod(system, 0x49, static_cast<u32>(Mode::TrackWithFilter), true);
            for (const auto [method, value] : std::array<std::array<u32, 2>, 8>{{
                     {0x8e0, 0x2000}, {0x8e1, 2}, {0x8e2, 0x34567800}, {0x8e3, 0},
                     {0xd18, 0x04001902}, {0xd2b, 0x050fedcb}, {0xd30, 0x6000},
                     {0x904, 0x11}}}) {
                engine->CallMethod(system, method, value, true);
            }
            engine->CallMethod(system, 0x45, 0x81, true);
            for (const u32 word : helper_code) {
                engine->CallMethod(system, 0x46, word, true);
            }
            engine->CallMethod(system, 0x47, 12, true);
            engine->CallMethod(system, 0x48, 0x81, true);
            dispatch(*engine, helper_parameters, engine == &reference);
            engine->CallMethod(system, 0x904, 0x11, true);
            engine->CallMethod(system, 0x49, static_cast<u32>(Mode::Replay), true);
            for (const u32 method : {0x8e0u, 0x8e1u, 0x8e2u, 0x8e3u}) {
                engine->CallMethod(system, method, 0, true);
            }
            engine->CallMethod(system, 0x904, 0x11, true);
            const auto& binding = engine->state.shader_stages[0].const_buffers[1];
            const u64 expected_address = offset_helper ? 0x0400190200ULL : 0x050fedcb00ULL;
            const u32 expected_size = offset_helper ? 0x7000 : 0x6000;
            const u32 expected_offset = offset_helper ? 0x680 : 0;
            if (!binding.enabled || binding.address != expected_address ||
                binding.size != expected_size || engine->regs.const_buffer.offset != expected_offset) {
                std::fprintf(stderr, "FAIL helper=%s path=%s replay address=%llx size=%x offset=%x\n",
                             offset_helper ? "c713" : "d733", engine == &hle ? "HLE" : "LLE",
                             static_cast<unsigned long long>(binding.address), binding.size,
                             engine->regs.const_buffer.offset);
                return 1;
            }
        }
    }
    std::puts("PASS captured shader and constant-buffer macro HLE/interpreter shadow replay");
}
