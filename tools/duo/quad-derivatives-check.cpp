// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

#include <array>
#include <cstdio>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "video_core/shader_environment.h"

int main() {
    using namespace Shader;
    struct Case {
        u32 direction;
        u32 swizzle;
        bool same_source;
        IR::Opcode expected;
    };
    constexpr std::array cases{
        Case{0, 0x99, true, IR::Opcode::DPdxFine},
        Case{1, 0xA5, true, IR::Opcode::DPdyFine},
        Case{2, 0x99, true, IR::Opcode::FSwizzleAdd},
        Case{0, 0xA5, true, IR::Opcode::FSwizzleAdd},
        Case{0, 0x99, false, IR::Opcode::FSwizzleAdd},
    };
    for (const auto& test : cases) {
        ObjectPool<IR::Inst> pool;
        IR::Block block{pool};
        IR::IREmitter ir{block};
        const auto own = ir.GetAttribute(IR::Attribute::Generic0X);
        const auto other = test.same_source ? own : ir.GetAttribute(IR::Attribute::Generic0Y);
        const auto peer = ir.BitCast<IR::F32>(
            ir.QuadSwap(ir.BitCast<IR::U32>(own), ir.Imm32(test.direction)));
        const auto result = ir.FSwizzleAdd(peer, other, ir.Imm32(test.swizzle));
        ir.Reference(result);
        IR::Program program;
        program.post_order_blocks.push_back(&block);
        VideoCommon::FileEnvironment env;
        Optimization::ConstantPropagationPass(env, program);
        const auto actual = result.Resolve().InstRecursive()->GetOpcode();
        std::printf("direction=%u swizzle=%02x same-source=%d: %s\n", test.direction,
                    test.swizzle, test.same_source, actual == test.expected ? "PASS" : "FAIL");
        if (actual != test.expected) {
            return 1;
        }
    }
}
