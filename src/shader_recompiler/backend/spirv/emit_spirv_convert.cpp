// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
Id ExtractU16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U16, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractS16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.S16, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractU8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U8, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

Id ExtractS8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpSConvert(ctx.S8, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

// Maxwell keeps fp16 denormals and games depend on them (Fire Emblem Engage's engage attacks
// divide by an fp16 subnormal). Hosts that cannot preserve them (Adreno before A8xx) flush such
// values to zero. With fix_fp16_denormals on (by default only for the titles listed in
// game_settings.cpp), that range is converted by hand; normal values, infinities and NaN still
// use the hardware conversion. It costs a few instructions per conversion, which is why it is
// not on for every game.
bool ConvertFp16DenormalsByHand(const EmitContext& ctx) {
    return !ctx.profile.support_fp16_denorm_preserve && Settings::IsFp16DenormalFixEnabled();
}
} // Anonymous namespace

Id EmitConvertS16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS32F32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        return ctx.OpBitcast(ctx.U32[1], ctx.OpConvertFToS(ctx.S32[1], value));
    } else {
        return ctx.OpConvertFToS(ctx.U32[1], value);
    }
}

Id EmitConvertS32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertU16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U64, value);
}

Id EmitConvertU32U64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

// See ConvertFp16DenormalsByHand.
Id EmitConvertF16F32(EmitContext& ctx, Id value) {
#ifdef __ANDROID__
    const Id result{ctx.OpFConvert(ctx.F16[1], value)};
#else
    const auto converted = ctx.OpFConvert(ctx.F16[1], value);
    const auto isOverflowing = ctx.OpIsNan(ctx.U1, converted);
    const Id result{
        ctx.OpSelect(ctx.F16[1], isOverflowing, ctx.Constant(ctx.F16[1], 0), converted)};
#endif
    if (!ConvertFp16DenormalsByHand(ctx)) {
        return result;
    }
    // Below 2^-14 an fp16 is sign | round(|x| * 2^24). A mantissa that rounds up to 1024 is
    // exactly the encoding of the smallest normal, so the boundary needs no special case.
    const Id abs{ctx.OpFAbs(ctx.F32[1], value)};
    const Id subnormal{ctx.OpFOrdLessThan(ctx.U1, abs, ctx.Const(0x1p-14f))};
    const Id mantissa{ctx.OpConvertFToU(
        ctx.U32[1], ctx.OpRoundEven(ctx.F32[1], ctx.OpFMul(ctx.F32[1], abs, ctx.Const(0x1p24f))))};
    const Id sign{ctx.OpBitwiseAnd(
        ctx.U32[1],
        ctx.OpShiftRightLogical(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], value), ctx.Const(16u)),
        ctx.Const(0x8000u))};
    const Id bits{ctx.OpBitwiseOr(ctx.U32[1], sign, mantissa)};
    const Id half{ctx.OpCompositeExtract(ctx.F16[1], ctx.OpBitcast(ctx.F16[2], bits), 0u)};
    return ctx.OpSelect(ctx.F16[1], subnormal, half, result);
}

Id EmitConvertF32F16(EmitContext& ctx, Id value) {
    const Id result{ctx.OpFConvert(ctx.F32[1], value)};
    if (!ConvertFp16DenormalsByHand(ctx)) {
        return result;
    }
    // An fp16 with a zero exponent is sign * mantissa * 2^-24, exact in fp32.
    const Id bits{ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeConstruct(ctx.F16[2], value, value))};
    const Id subnormal{ctx.OpIEqual(ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], bits, ctx.Const(0x7c00u)),
                                    ctx.u32_zero_value)};
    const Id magnitude{ctx.OpFMul(
        ctx.F32[1],
        ctx.OpConvertUToF(ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], bits, ctx.Const(0x3ffu))),
        ctx.Const(0x1p-24f))};
    const Id negative{ctx.OpINotEqual(
        ctx.U1, ctx.OpBitwiseAnd(ctx.U32[1], bits, ctx.Const(0x8000u)), ctx.u32_zero_value)};
    const Id signed_magnitude{
        ctx.OpSelect(ctx.F32[1], negative, ctx.OpFNegate(ctx.F32[1], magnitude), magnitude)};
    return ctx.OpSelect(ctx.F32[1], subnormal, signed_magnitude, result);
}

Id EmitConvertF32F64(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF64F32(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F64[1], value);
}

Id EmitConvertF16S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS8(ctx, value));
}

Id EmitConvertF16S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS16(ctx, value));
}

Id EmitConvertF16S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU8(ctx, value));
}

Id EmitConvertF16U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU16(ctx, value));
}

Id EmitConvertF16U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF16U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF32S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS8(ctx, value));
}

Id EmitConvertF32S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS16(ctx, value));
}

Id EmitConvertF32S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU8(ctx, value));
}

Id EmitConvertF32U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU16(ctx, value));
}

Id EmitConvertF32U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF32U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF64S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS8(ctx, value));
}

Id EmitConvertF64S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS16(ctx, value));
}

Id EmitConvertF64S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU8(ctx, value));
}

Id EmitConvertF64U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU16(ctx, value));
}

Id EmitConvertF64U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertF64U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertU16U32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U16, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id EmitConvertU32U16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], value);
    } else {
        return ExtractU16(ctx, value);
    }
}

Id EmitConvertU8U32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U8, value);
    } else {
        return ExtractU8(ctx, value);
    }
}

Id EmitConvertU32U8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U32[1], value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

// in signed
Id EmitConvertS32S8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpSConvert(ctx.U32[1], value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

Id EmitConvertS32S16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

} // namespace Shader::Backend::SPIRV
