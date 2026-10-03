// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 17: the derived-value expression language ("expr"). A manifest string is compiled once,
// at load, into a flat node tree (ExprProgram) and evaluated every tick against the published
// values. No loops, no assignment, no side effects; evaluation is bounded by the node count.
//
// Grammar (lowest precedence first, C-like):
//   expr    := or ( '?' expr ':' expr )?            right-associative
//   or      := and ( '||' and )*                    short-circuit
//   and     := eq ( '&&' eq )*                      short-circuit
//   eq      := rel ( ('=='|'!=') rel )*
//   rel     := add ( ('<'|'<='|'>'|'>=') add )*
//   add     := mul ( ('+'|'-') mul )*
//   mul     := unary ( ('*'|'/'|'%') unary )*
//   unary   := ('!'|'-'|'+') unary | primary
//   primary := number | name | 'quoted name' | func '(' expr (',' expr)* ')' | '(' expr ')'
// Numbers: decimal (1, 2.5, .5, 1e3) or hex (0x1F). Names: [A-Za-z_][A-Za-z0-9_.]*, or '@'
// followed by [A-Za-z0-9_.:]* (so "@flag:x", "@clock.hour" are bare names -- put spaces around a
// ternary's ':' after one), or any name in single quotes ('view_custom:map'). Functions: min, max
// (two or more arguments), abs, floor, ceil, round (one), clamp(x, lo, hi).
// Values are f64. '/' is real division; '/' and '%' by zero give 0. Comparisons use the same 1e-6
// tolerance as the "cmp" form. A name that is not published makes the result missing (nothing
// published, as every other derived form), unless a short-circuit or ternary skipped it.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods {

struct ExprProgram {
    enum class Op : u8 {
        Const,
        Ref,
        Neg,
        Not,
        Add,
        Sub,
        Mul,
        Div,
        Mod,
        Lt,
        Le,
        Gt,
        Ge,
        Eq,
        Ne,
        And,
        Or,
        Cond,
        Min,
        Max,
        Abs,
        Floor,
        Ceil,
        Round,
        Clamp,
    };
    struct Node {
        Op op{Op::Const};
        u32 a{0}; ///< first child; for Ref, the index into `refs`
        u32 b{0};
        u32 c{0};
        f64 value{0.0}; ///< Const only
    };
    std::vector<Node> nodes;
    u32 root{0};
    /// Every published name the expression reads (deduplicated, first-use order): the derived
    /// pass orders and marks volatility from these.
    std::vector<std::string> refs;
    /// Non-empty when the source failed to compile; the program then always evaluates to 0.
    std::string error;
    [[nodiscard]] bool Ok() const {
        return error.empty();
    }
};

/// Limits that keep a hostile or broken manifest from costing more than a few microseconds.
constexpr size_t ExprMaxSource = 4096;
constexpr size_t ExprMaxNodes = 1024;
constexpr u32 ExprMaxDepth = 64;

/// Compiles `source`. Never null; a failed compile carries `error` (with a column) and no nodes.
std::shared_ptr<const ExprProgram> CompileExpr(std::string_view source);

using ExprLookup = std::function<std::optional<f64>(const std::string&)>;

/// Evaluates a compiled program. A failed program gives 0; a missing name gives nullopt (see the
/// header comment); a non-finite result gives nullopt.
std::optional<f64> EvaluateExpr(const ExprProgram& program, const ExprLookup& lookup);

} // namespace Core::Mods
