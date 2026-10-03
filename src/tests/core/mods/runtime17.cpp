// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Runtime 17: derived "expr" values (mod_expr.h): parser precedence and errors, evaluation, the
// manifest key.
#include <map>
#include <optional>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/mods/mod_expr.h"
#include "core/mods/mod_runtime.h"

using namespace Core::Mods;

namespace {
std::map<std::string, f64> g_values;

std::optional<f64> Lookup(const std::string& name) {
    const auto it = g_values.find(name);
    return it != g_values.end() ? std::optional<f64>(it->second) : std::nullopt;
}

std::optional<f64> Eval(const std::string& source) {
    const auto program = CompileExpr(source);
    INFO(source << " -> " << program->error);
    REQUIRE(program->Ok());
    return EvaluateExpr(*program, Lookup);
}

std::string Error(const std::string& source) {
    const auto program = CompileExpr(source);
    INFO(source);
    REQUIRE_FALSE(program->Ok());
    REQUIRE(program->nodes.empty());
    REQUIRE(EvaluateExpr(*program, Lookup) == 0.0); // a broken expression publishes 0
    return program->error;
}
} // namespace

TEST_CASE("DSMod expr: precedence and associativity", "[dsmod][runtime17][expr]") {
    g_values.clear();
    REQUIRE(Eval("1 + 2 * 3") == 7.0);
    REQUIRE(Eval("(1 + 2) * 3") == 9.0);
    REQUIRE(Eval("10 - 4 - 3") == 3.0);   // left-associative
    REQUIRE(Eval("64 / 4 / 2") == 8.0);   // left-associative
    REQUIRE(Eval("7 % 4 * 2") == 6.0);    // same level as *, left to right
    REQUIRE(Eval("-2 * 3") == -6.0);
    REQUIRE(Eval("- -3") == 3.0);
    REQUIRE(Eval("+4") == 4.0);
    REQUIRE(Eval("2 + 3 < 6") == 1.0);    // arithmetic binds tighter than comparison
    REQUIRE(Eval("1 < 2 == 1") == 1.0);   // relational tighter than equality
    REQUIRE(Eval("1 == 1 && 0 || 1") == 1.0);
    REQUIRE(Eval("0 || 1 && 0") == 0.0);  // && tighter than ||
    REQUIRE(Eval("!0 + 1") == 2.0);       // unary tighter than +
    REQUIRE(Eval("!(0 + 1)") == 0.0);
    REQUIRE(Eval("1 ? 2 : 3") == 2.0);
    REQUIRE(Eval("0 ? 2 : 1 ? 4 : 5") == 4.0); // right-associative
    REQUIRE(Eval("1 ? 0 ? 6 : 7 : 8") == 7.0);
    REQUIRE(Eval("0 || 0 ? 1 : 2") == 2.0);    // ?: is lowest
    REQUIRE(Eval("1 + 1 ? 10 : 20") == 10.0);
    REQUIRE(Eval("3 != 4") == 1.0);
    REQUIRE(Eval("3 >= 3 && 3 <= 3 && !(3 > 3) && !(3 < 3)") == 1.0);
}

TEST_CASE("DSMod expr: numbers, functions and arithmetic rules", "[dsmod][runtime17][expr]") {
    g_values.clear();
    REQUIRE(Eval("2.5 * 2") == 5.0);
    REQUIRE(Eval(".5 + 0.25") == 0.75);
    REQUIRE(Eval("1e3") == 1000.0);
    REQUIRE(Eval("2E-1 * 10") == 2.0);
    REQUIRE(Eval("0x1F") == 31.0);
    REQUIRE(Eval("7 / 2") == 3.5); // real division
    REQUIRE(Eval("7.5 % 2") == 1.5);
    REQUIRE(Eval("-7 % 3") == -1.0); // sign of the dividend, like C
    REQUIRE(Eval("5 / 0") == 0.0);   // division by zero is 0
    REQUIRE(Eval("5 % 0") == 0.0);
    REQUIRE(Eval("min(4, 2, 9)") == 2.0);
    REQUIRE(Eval("max(4, 2, 9)") == 9.0);
    REQUIRE(Eval("abs(-3)") == 3.0);
    REQUIRE(Eval("clamp(15, 0, 10)") == 10.0);
    REQUIRE(Eval("clamp(-1, 0, 10)") == 0.0);
    REQUIRE(Eval("clamp(5, 0, 10)") == 5.0);
    REQUIRE(Eval("floor(2.7) + ceil(2.2) + round(2.5)") == 8.0);
    REQUIRE(Eval("max (1, 2)") == 2.0); // space before '('
    REQUIRE(Eval("0.1 + 0.2 == 0.3") == 1.0); // the cmp form's 1e-6 tolerance
}

TEST_CASE("DSMod expr: references", "[dsmod][runtime17][expr]") {
    g_values = {{"hp", 30.0},       {"hp_max", 120.0},  {"@flag:hard", 1.0},
                {"@clock.hour", 21.0}, {"bt.f0", 4.0}, {"view_custom:map", 1.0},
                {"min", 5.0}};
    REQUIRE(Eval("hp / hp_max * 100") == 25.0);
    REQUIRE(Eval("@flag:hard ? hp : 0") == 30.0);
    REQUIRE(Eval("@clock.hour >= 18 || @clock.hour < 6") == 1.0);
    REQUIRE(Eval("bt.f0 * 2") == 8.0);
    REQUIRE(Eval("'view_custom:map' + 1") == 2.0);
    REQUIRE(Eval("min + min(1, 2)") == 6.0); // a point called "min" stays reachable

    const auto program = CompileExpr("hp + hp * hp_max - @flag:hard");
    REQUIRE(program->refs == std::vector<std::string>{"hp", "hp_max", "@flag:hard"});

    // A missing name makes the result missing...
    REQUIRE_FALSE(Eval("hp + nothing").has_value());
    REQUIRE_FALSE(Eval("nothing ? 1 : 2").has_value());
    // ...unless short-circuit or the ternary skips it.
    REQUIRE(Eval("0 && nothing") == 0.0);
    REQUIRE(Eval("1 || nothing") == 1.0);
    REQUIRE(Eval("1 ? hp : nothing") == 30.0);
    REQUIRE_FALSE(Eval("1 && nothing").has_value());
}

TEST_CASE("DSMod expr: parse errors", "[dsmod][runtime17][expr]") {
    g_values.clear();
    REQUIRE(Error("").find("empty") != std::string::npos);
    REQUIRE(Error("   ").find("empty") != std::string::npos);
    REQUIRE(Error("1 +").find("end of expression") != std::string::npos);
    REQUIRE(Error("(1 + 2").find("expected ')'") != std::string::npos);
    REQUIRE(Error("1 + 2)").find("unexpected ')'") != std::string::npos);
    REQUIRE(Error("1 ? 2").find("expected ':'") != std::string::npos);
    REQUIRE(Error("foo(1)").find("unknown function 'foo'") != std::string::npos);
    REQUIRE(Error("clamp(1, 2)").find("wrong number") != std::string::npos);
    REQUIRE(Error("min(1)").find("wrong number") != std::string::npos);
    REQUIRE(Error("abs(1, 2)").find("wrong number") != std::string::npos);
    REQUIRE(Error("a = 1").find("unexpected '='") != std::string::npos);
    REQUIRE(Error("a & b").find("unexpected '&'") != std::string::npos);
    REQUIRE(Error("1 2").find("unexpected '2'") != std::string::npos);
    REQUIRE(Error("2x").find("bad number") != std::string::npos);
    REQUIRE(Error("0x").find("bad") != std::string::npos);
    REQUIRE(Error("'unterminated").find("unterminated") != std::string::npos);
    REQUIRE(Error("@ + 1").find("name after '@'") != std::string::npos);
    REQUIRE(Error("1 + 2 $").find("column 7") != std::string::npos); // 1-based column
    // Limits: nesting depth, node count, source length.
    REQUIRE(Error(std::string(200, '(') + "1" + std::string(200, ')')).find("deeply") !=
            std::string::npos);
    REQUIRE(Error(std::string(100, '-') + "1").find("deeply") != std::string::npos);
    std::string wide = "1";
    for (int i = 0; i < 700; ++i) {
        wide += "+1";
    }
    REQUIRE(Error(wide).find("too large") != std::string::npos);
    REQUIRE(Error(std::string(ExprMaxSource + 1, '1')).find("longer") != std::string::npos);
}

TEST_CASE("DSMod expr: manifest key", "[dsmod][runtime17][expr]") {
    const auto json = nlohmann::json::parse(R"js({"format":1,"name":"Pkg",
        "pages":[{"id":"p","widgets":[]}],
        "derived":[
          {"name":"pct","expr":"hp * 100 / max(hp_max, 1)","floor":true},
          {"name":"broken","expr":"hp +"},
          {"name":"not_text","expr":5}
        ]})js");
    Manifest m;
    REQUIRE(ParseDualScreenManifest(json, m));
    REQUIRE(m.derived.size() == 3);
    REQUIRE(m.derived[0].expr_program);
    REQUIRE(m.derived[0].expr_program->Ok());
    REQUIRE(m.derived[0].expr_program->refs == std::vector<std::string>{"hp", "hp_max"});
    REQUIRE(m.derived[0].floor);
    REQUIRE(m.derived[1].expr_program);
    REQUIRE_FALSE(m.derived[1].expr_program->Ok()); // logged; publishes 0
    REQUIRE(EvaluateExpr(*m.derived[1].expr_program, {}) == 0.0);
    REQUIRE_FALSE(m.derived[2].expr_program); // not a string: not an expr entry
}
