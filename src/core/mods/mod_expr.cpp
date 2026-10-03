// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 17: compiler and evaluator for derived "expr" values. The grammar and the value rules
// are in mod_expr.h. CompileExpr runs once per derived entry at manifest load (ParseDerived);
// EvaluateExpr runs on the tick thread from ModRuntime::EvaluateDerived.

#include "core/mods/mod_expr.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include <fmt/format.h>

namespace Core::Mods {

namespace {

using Op = ExprProgram::Op;

/// Same tolerance as the derived "cmp" form (mod_state.cpp).
constexpr f64 CmpEps = 1e-6;

struct ParseError {
    std::string message;
};

class Parser {
public:
    Parser(std::string_view src_, ExprProgram& out_) : src{src_}, out{out_} {}

    u32 ParseAll() {
        SkipSpace();
        if (pos >= src.size()) {
            Fail("empty expression");
        }
        const u32 root = Ternary();
        SkipSpace();
        if (pos < src.size()) {
            Fail(fmt::format("unexpected '{}'", src[pos]));
        }
        return root;
    }

private:
    std::string_view src;
    ExprProgram& out;
    size_t pos{0};
    u32 depth{0};

    [[noreturn]] void Fail(const std::string& what) const {
        throw ParseError{fmt::format("{} at column {}", what, pos + 1)};
    }

    void SkipSpace() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos])) != 0) {
            ++pos;
        }
    }

    /// Consumes `tok` when the input continues with it (after whitespace).
    bool Eat(std::string_view tok) {
        SkipSpace();
        if (src.substr(pos, tok.size()) == tok) {
            pos += tok.size();
            return true;
        }
        return false;
    }

    /// Like Eat, but a one-character operator that is also the first character of a two-character
    /// one ('<' of "<=", '!' of "!=", '&' alone) only matches when the longer form does not.
    bool EatSingle(char c, char not_followed_by) {
        SkipSpace();
        if (pos < src.size() && src[pos] == c &&
            (pos + 1 >= src.size() || src[pos + 1] != not_followed_by)) {
            ++pos;
            return true;
        }
        return false;
    }

    u32 Add(ExprProgram::Node node) {
        if (out.nodes.size() >= ExprMaxNodes) {
            Fail("expression too large");
        }
        out.nodes.push_back(node);
        return static_cast<u32>(out.nodes.size() - 1);
    }
    u32 Unary(Op op, u32 a) {
        return Add({op, a, 0, 0, 0.0});
    }
    u32 Binary(Op op, u32 a, u32 b) {
        return Add({op, a, b, 0, 0.0});
    }

    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& p_) : p{p_} {
            if (++p.depth > ExprMaxDepth) {
                p.Fail("expression nested too deeply");
            }
        }
        ~DepthGuard() {
            --p.depth;
        }
    };

    u32 Ternary() {
        const DepthGuard guard{*this};
        const u32 cond = Or();
        if (!Eat("?")) {
            return cond;
        }
        const u32 then_v = Ternary();
        if (!Eat(":")) {
            Fail("expected ':'");
        }
        const u32 else_v = Ternary();
        return Add({Op::Cond, cond, then_v, else_v, 0.0});
    }

    u32 Or() {
        u32 left = And();
        while (Eat("||")) {
            left = Binary(Op::Or, left, And());
        }
        return left;
    }

    u32 And() {
        u32 left = Equality();
        while (Eat("&&")) {
            left = Binary(Op::And, left, Equality());
        }
        return left;
    }

    u32 Equality() {
        u32 left = Relational();
        for (;;) {
            if (Eat("==")) {
                left = Binary(Op::Eq, left, Relational());
            } else if (Eat("!=")) {
                left = Binary(Op::Ne, left, Relational());
            } else {
                return left;
            }
        }
    }

    u32 Relational() {
        u32 left = Additive();
        for (;;) {
            if (Eat("<=")) {
                left = Binary(Op::Le, left, Additive());
            } else if (Eat(">=")) {
                left = Binary(Op::Ge, left, Additive());
            } else if (EatSingle('<', '=')) {
                left = Binary(Op::Lt, left, Additive());
            } else if (EatSingle('>', '=')) {
                left = Binary(Op::Gt, left, Additive());
            } else {
                return left;
            }
        }
    }

    u32 Additive() {
        u32 left = Multiplicative();
        for (;;) {
            if (Eat("+")) {
                left = Binary(Op::Add, left, Multiplicative());
            } else if (Eat("-")) {
                left = Binary(Op::Sub, left, Multiplicative());
            } else {
                return left;
            }
        }
    }

    u32 Multiplicative() {
        u32 left = UnaryExpr();
        for (;;) {
            if (Eat("*")) {
                left = Binary(Op::Mul, left, UnaryExpr());
            } else if (Eat("/")) {
                left = Binary(Op::Div, left, UnaryExpr());
            } else if (Eat("%")) {
                left = Binary(Op::Mod, left, UnaryExpr());
            } else {
                return left;
            }
        }
    }

    u32 UnaryExpr() {
        const DepthGuard guard{*this};
        if (EatSingle('!', '=')) {
            return Unary(Op::Not, UnaryExpr());
        }
        if (Eat("-")) {
            return Unary(Op::Neg, UnaryExpr());
        }
        if (Eat("+")) {
            return UnaryExpr();
        }
        return Primary();
    }

    static bool IsNameStart(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
    }
    static bool IsNameChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
    }

    u32 Ref(std::string name) {
        const auto it = std::ranges::find(out.refs, name);
        const auto index = static_cast<u32>(it - out.refs.begin());
        if (it == out.refs.end()) {
            out.refs.push_back(std::move(name));
        }
        return Add({Op::Ref, index, 0, 0, 0.0});
    }

    u32 Number() {
        const char* const begin = src.data() + pos;
        const char* const end = src.data() + src.size();
        // strtod/strtoull need a terminated buffer: copy the longest run that can be a number.
        size_t n = 0;
        while (begin + n < end && (std::isalnum(static_cast<unsigned char>(begin[n])) != 0 ||
                                   begin[n] == '.' ||
                                   ((begin[n] == '+' || begin[n] == '-') && n > 0 &&
                                    (begin[n - 1] == 'e' || begin[n - 1] == 'E') &&
                                    !(n > 1 && (begin[1] == 'x' || begin[1] == 'X'))))) {
            ++n;
        }
        const std::string text{begin, n};
        f64 value{};
        size_t used{};
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            char* stop = nullptr;
            value = static_cast<f64>(std::strtoull(text.c_str() + 2, &stop, 16));
            used = static_cast<size_t>(stop - text.c_str());
            if (used == 2) {
                Fail("bad hex number");
            }
        } else {
            char* stop = nullptr;
            value = std::strtod(text.c_str(), &stop);
            used = static_cast<size_t>(stop - text.c_str());
        }
        if (used != text.size() || used == 0) {
            Fail(fmt::format("bad number '{}'", text));
        }
        pos += used;
        return Add({Op::Const, 0, 0, 0, value});
    }

    u32 Call(std::string_view fn) {
        struct Fn {
            std::string_view name;
            Op op;
            u32 min_args;
            u32 max_args; ///< 0 = unbounded
        };
        static constexpr std::array<Fn, 7> fns{{
            {"min", Op::Min, 2, 0},
            {"max", Op::Max, 2, 0},
            {"abs", Op::Abs, 1, 1},
            {"floor", Op::Floor, 1, 1},
            {"ceil", Op::Ceil, 1, 1},
            {"round", Op::Round, 1, 1},
            {"clamp", Op::Clamp, 3, 3},
        }};
        const auto it = std::ranges::find(fns, fn, &Fn::name);
        if (it == fns.end()) {
            Fail(fmt::format("unknown function '{}'", fn));
        }
        std::vector<u32> args;
        if (!Eat(")")) {
            do {
                args.push_back(Ternary());
            } while (Eat(","));
            if (!Eat(")")) {
                Fail("expected ')'");
            }
        }
        if (args.size() < it->min_args || (it->max_args != 0 && args.size() > it->max_args)) {
            Fail(fmt::format("wrong number of arguments to {}()", fn));
        }
        switch (it->op) {
        case Op::Min:
        case Op::Max: {
            u32 acc = args[0];
            for (size_t i = 1; i < args.size(); ++i) {
                acc = Binary(it->op, acc, args[i]);
            }
            return acc;
        }
        case Op::Clamp:
            return Add({Op::Clamp, args[0], args[1], args[2], 0.0});
        default:
            return Unary(it->op, args[0]);
        }
    }

    u32 Primary() {
        SkipSpace();
        if (pos >= src.size()) {
            Fail("unexpected end of expression");
        }
        const char c = src[pos];
        if (c == '(') {
            ++pos;
            const u32 inner = Ternary();
            if (!Eat(")")) {
                Fail("expected ')'");
            }
            return inner;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && pos + 1 < src.size() &&
             std::isdigit(static_cast<unsigned char>(src[pos + 1])) != 0)) {
            return Number();
        }
        if (c == '\'') {
            const size_t close = src.find('\'', pos + 1);
            if (close == std::string_view::npos) {
                Fail("unterminated quoted name");
            }
            std::string name{src.substr(pos + 1, close - pos - 1)};
            if (name.empty()) {
                Fail("empty quoted name");
            }
            pos = close + 1;
            return Ref(std::move(name));
        }
        if (c == '@') {
            const size_t start = pos++;
            while (pos < src.size() && (IsNameChar(src[pos]) || src[pos] == ':')) {
                ++pos;
            }
            if (pos == start + 1) {
                Fail("expected a name after '@'");
            }
            return Ref(std::string{src.substr(start, pos - start)});
        }
        if (IsNameStart(c)) {
            const size_t start = pos;
            while (pos < src.size() && IsNameChar(src[pos])) {
                ++pos;
            }
            const std::string_view name = src.substr(start, pos - start);
            // A name directly followed by '(' is a function call; otherwise a published value
            // (a point called "min" stays reachable).
            size_t look = pos;
            while (look < src.size() && std::isspace(static_cast<unsigned char>(src[look])) != 0) {
                ++look;
            }
            if (look < src.size() && src[look] == '(') {
                pos = look + 1;
                return Call(name);
            }
            return Ref(std::string{name});
        }
        Fail(fmt::format("unexpected '{}'", c));
    }
};

class Evaluator {
public:
    Evaluator(const ExprProgram& p_, const ExprLookup& lookup_) : p{p_}, lookup{lookup_} {
        cache.resize(p.refs.size());
        cached.assign(p.refs.size(), 0);
    }

    std::optional<f64> Eval(u32 i) {
        const ExprProgram::Node& n = p.nodes[i];
        switch (n.op) {
        case Op::Const:
            return n.value;
        case Op::Ref:
            if (cached[n.a] == 0) {
                cache[n.a] = lookup ? lookup(p.refs[n.a]) : std::nullopt;
                cached[n.a] = 1;
            }
            return cache[n.a];
        case Op::Neg:
            return Map1(n, [](f64 x) { return -x; });
        case Op::Not:
            return Map1(n, [](f64 x) { return x == 0.0 ? 1.0 : 0.0; });
        case Op::Abs:
            return Map1(n, [](f64 x) { return std::fabs(x); });
        case Op::Floor:
            return Map1(n, [](f64 x) { return std::floor(x); });
        case Op::Ceil:
            return Map1(n, [](f64 x) { return std::ceil(x); });
        case Op::Round:
            return Map1(n, [](f64 x) { return std::round(x); });
        case Op::Add:
            return Map2(n, [](f64 x, f64 y) { return x + y; });
        case Op::Sub:
            return Map2(n, [](f64 x, f64 y) { return x - y; });
        case Op::Mul:
            return Map2(n, [](f64 x, f64 y) { return x * y; });
        case Op::Div:
            return Map2(n, [](f64 x, f64 y) { return y == 0.0 ? 0.0 : x / y; });
        case Op::Mod:
            return Map2(n, [](f64 x, f64 y) { return y == 0.0 ? 0.0 : std::fmod(x, y); });
        case Op::Lt:
            return Map2(n, [](f64 x, f64 y) { return x < y - CmpEps ? 1.0 : 0.0; });
        case Op::Le:
            return Map2(n, [](f64 x, f64 y) { return x <= y + CmpEps ? 1.0 : 0.0; });
        case Op::Gt:
            return Map2(n, [](f64 x, f64 y) { return x > y + CmpEps ? 1.0 : 0.0; });
        case Op::Ge:
            return Map2(n, [](f64 x, f64 y) { return x >= y - CmpEps ? 1.0 : 0.0; });
        case Op::Eq:
            return Map2(n, [](f64 x, f64 y) { return std::fabs(x - y) < CmpEps ? 1.0 : 0.0; });
        case Op::Ne:
            return Map2(n, [](f64 x, f64 y) { return std::fabs(x - y) >= CmpEps ? 1.0 : 0.0; });
        case Op::Min:
            return Map2(n, [](f64 x, f64 y) { return std::min(x, y); });
        case Op::Max:
            return Map2(n, [](f64 x, f64 y) { return std::max(x, y); });
        case Op::And: {
            const auto a = Eval(n.a);
            if (!a || *a == 0.0) {
                return a ? std::optional<f64>(0.0) : std::nullopt;
            }
            const auto b = Eval(n.b);
            return b ? std::optional<f64>(*b != 0.0 ? 1.0 : 0.0) : std::nullopt;
        }
        case Op::Or: {
            const auto a = Eval(n.a);
            if (!a || *a != 0.0) {
                return a ? std::optional<f64>(1.0) : std::nullopt;
            }
            const auto b = Eval(n.b);
            return b ? std::optional<f64>(*b != 0.0 ? 1.0 : 0.0) : std::nullopt;
        }
        case Op::Cond: {
            const auto c = Eval(n.a);
            if (!c) {
                return std::nullopt;
            }
            return Eval(*c != 0.0 ? n.b : n.c);
        }
        case Op::Clamp: {
            const auto x = Eval(n.a);
            const auto lo = Eval(n.b);
            const auto hi = Eval(n.c);
            if (!x || !lo || !hi) {
                return std::nullopt;
            }
            // Like std::clamp, but defined for lo > hi (hi wins) instead of undefined behaviour.
            return std::min(std::max(*x, *lo), *hi);
        }
        }
        return std::nullopt;
    }

private:
    const ExprProgram& p;
    const ExprLookup& lookup;
    /// Each name is looked up once per evaluation, however often the expression repeats it.
    std::vector<std::optional<f64>> cache;
    std::vector<u8> cached;

    template <typename F>
    std::optional<f64> Map1(const ExprProgram::Node& n, F&& f) {
        const auto a = Eval(n.a);
        return a ? std::optional<f64>(f(*a)) : std::nullopt;
    }
    template <typename F>
    std::optional<f64> Map2(const ExprProgram::Node& n, F&& f) {
        const auto a = Eval(n.a);
        if (!a) {
            return std::nullopt;
        }
        const auto b = Eval(n.b);
        return b ? std::optional<f64>(f(*a, *b)) : std::nullopt;
    }
};

} // namespace

std::shared_ptr<const ExprProgram> CompileExpr(std::string_view source) {
    auto program = std::make_shared<ExprProgram>();
    if (source.size() > ExprMaxSource) {
        program->error = fmt::format("expression longer than {} characters", ExprMaxSource);
        return program;
    }
    try {
        Parser parser{source, *program};
        program->root = parser.ParseAll();
    } catch (const ParseError& e) {
        program->error = e.message;
        program->nodes.clear();
        program->refs.clear();
        program->root = 0;
    }
    return program;
}

std::optional<f64> EvaluateExpr(const ExprProgram& program, const ExprLookup& lookup) {
    if (!program.Ok() || program.nodes.empty()) {
        return 0.0;
    }
    Evaluator ev{program, lookup};
    const auto v = ev.Eval(program.root);
    if (!v || !std::isfinite(*v)) {
        return std::nullopt;
    }
    return v;
}

} // namespace Core::Mods
