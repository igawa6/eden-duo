// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Package discovery and manifest parsing: turns load/<TITLEID>/*/dualscreen/manifest.json plus
// its per-build data file into a Manifest.
//   - Discover (static): scans the title's mod folders in name order, skipping disabled add-ons,
//     checks title_id, applies the min_runtime gate (PackageMinRuntime; a package that needs a
//     newer runtime gets UpdateRequiredManifest instead), checks the format, parses the manifest
//     and loads <BUILD16>.json / its lower-case form / data.json. IdleManifest is the page for a
//     title without a package.
//   - ParseManifestJson and its Parse* helpers (points, derived, widgets, actions, haptics,
//     animations, page binds, scroll regions, map extras), ReadJson, ParseAddressToken.
//   - ParseDualScreenManifest / IsUsableDualScreenManifest: the same parser for tools and for
//     DiscoverModLoadPlan (mod_load_plan.cpp).
//   - ReloadManifest: the console "reload" -- re-parses in place and resets every cache the old
//     pages fed.
// Not here: reading the values a manifest describes (mod_state.cpp), running its actions
// (mod_actions.cpp), drawing its pages (mod_redraw.cpp, mod_ui.cpp).
// Flow: no per-frame stage. Discover runs once at title load, before Initialize, on the thread
// that loads the title; the parsers are pure functions of their JSON. ReloadManifest runs on the
// tick thread (DriveCmdImpl, mod_console.cpp) and holds guest_bridge_mutex for its whole body,
// plus asset_cache_mutex, map_state_mutex, gpu_composite_mutex, view_mutex and map_records_mutex
// around the resets the redraw worker could otherwise race.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <tuple>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/mods/mod_nx_assets.h"
#include "core/mods/mod_package_io.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "core/mods/mod_settings.h"

namespace Core::Mods {

namespace {

/// Manifests use human-readable #AARRGGBB, and the canvas now stores pixels as that same ARGB u32
/// (the aux UI is uploaded as B8G8R8A8_UNORM, whose little-endian bytes B,G,R,A ARE 0xAARRGGBB).
/// So this is the identity: no channel swap. It used to swap R and B, back when the upload was
/// R8G8B8A8 -- which is exactly what left the hardcoded map/marker colours wrong on Adreno until
/// the format was corrected. Kept as a named no-op so the call sites still read intentionally.
constexpr u32 ToPixelOrder(u32 argb) {
    return argb;
}

u32 ParseColor(const nlohmann::json& j, const char* key, u32 fallback) {
    if (!j.contains(key)) {
        return fallback;
    }
    const auto& value = j.at(key);
    if (value.is_number_unsigned()) {
        return ToPixelOrder(value.get<u32>());
    }
    if (value.is_string()) {
        auto text = value.get<std::string>();
        if (!text.empty() && text.front() == '#') {
            text.erase(text.begin());
        }
        try {
            const u32 raw = static_cast<u32>(std::stoul(text, nullptr, 16));
            return ToPixelOrder(text.size() <= 6 ? (0xFF000000u | raw) : raw);
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

/// "value -> colour" table for a color_bind field, same shape/parsing convention as Widget's
/// text_map (decimal or "0x.."-prefixed integer keys), but ARGB colour values (number or
/// "#AARRGGBB"/"AARRGGBB" string, via ParseColor's own parsing) instead of text references. A
/// malformed entry is skipped with a warning, exactly like text_map's own malformed-entry handling.
std::shared_ptr<const std::unordered_map<s64, u32>> ParseColorMap(const nlohmann::json& j,
                                                                  const char* key) {
    if (!j.contains(key) || !j.at(key).is_object()) {
        return nullptr;
    }
    auto map = std::make_shared<std::unordered_map<s64, u32>>();
    for (const auto& [text_key, value] : j.at(key).items()) {
        const bool hex = text_key.starts_with("0x") || text_key.starts_with("-0x");
        char* end = nullptr;
        const long long ikey = std::strtoll(text_key.c_str(), &end, hex ? 16 : 10);
        if (end == text_key.c_str() || *end != '\0') {
            LOG_WARNING(Core, "DSMod: {} entry '{}' ignored (bad key)", key, text_key);
            continue;
        }
        nlohmann::json wrap;
        wrap["c"] = value;
        map->emplace(static_cast<s64>(ikey), ParseColor(wrap, "c", 0u));
    }
    return map;
}

/// j[key] when it is a string, else `fallback` (json::value throws on a wrong type; the runtime
/// 16/17 parsers use this instead so a mistyped key is ignored, not a failed package).
std::string StringOr(const nlohmann::json& j, const char* key, std::string fallback = {}) {
    if (const auto it = j.find(key); it != j.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return fallback;
}

s64 ParseNumber(const nlohmann::json& value, s64 fallback = 0) {
    if (value.is_number_integer()) {
        return value.get<s64>();
    }
    if (value.is_string()) {
        auto text = value.get<std::string>();
        try {
            // "-0x2C" is a real offset -- Cheat Engine signatures routinely sit after the
            // value they identify. Testing for the prefix at position zero misses the sign and
            // silently parses it as zero, which reads back as "the field is right here".
            const size_t digits = (!text.empty() && (text[0] == '-' || text[0] == '+')) ? 1 : 0;
            const bool hex =
                text.compare(digits, 2, "0x") == 0 || text.compare(digits, 2, "0X") == 0;
            return static_cast<s64>(std::stoll(text, nullptr, hex ? 16 : 10));
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

// Coordinates accept decimal JSON numbers; game addresses keep integer parsing.
s32 ParseCoordinate(const nlohmann::json& value) {
    if (value.is_number_float()) {
        const double n = value.get<double>();
        if (!std::isfinite(n))
            return 0;
        return static_cast<s32>(
            std::round(std::clamp(n, static_cast<double>(std::numeric_limits<s32>::min()),
                                  static_cast<double>(std::numeric_limits<s32>::max()))));
    }
    return static_cast<s32>(std::clamp<s64>(ParseNumber(value), std::numeric_limits<s32>::min(),
                                            std::numeric_limits<s32>::max()));
}

ValueType ParseValueType(const std::string& text) {
    if (text == "u8")
        return ValueType::U8;
    if (text == "s8")
        return ValueType::S8;
    if (text == "u16")
        return ValueType::U16;
    if (text == "s16")
        return ValueType::S16;
    if (text == "u32")
        return ValueType::U32;
    if (text == "u64")
        return ValueType::U64;
    if (text == "s64")
        return ValueType::S64;
    if (text == "f32")
        return ValueType::F32;
    if (text == "bool")
        return ValueType::Bool;
    if (text == "string" || text == "utf16")
        return ValueType::Utf16String;
    if (text == "cstring")
        return ValueType::CString;
    if (text == "utf32")
        return ValueType::Utf32String;
    return ValueType::S32;
}

WidgetType ParseWidgetType(const std::string& text) {
    if (text == "rect")
        return WidgetType::Rect;
    if (text == "value")
        return WidgetType::Value;
    if (text == "bar")
        return WidgetType::Bar;
    if (text == "button")
        return WidgetType::Button;
    if (text == "pips")
        return WidgetType::Pips;
    if (text == "image")
        return WidgetType::Image;
    if (text == "map")
        return WidgetType::Map;
    if (text == "chart")
        return WidgetType::Chart; // runtime 17
    return WidgetType::Label;
}

/// "48 8B ?? ?? 89" -> bytes plus a mask. Anything that is not two hex digits is a wildcard.
PatternFind ParsePattern(const nlohmann::json& def) {
    PatternFind find;
    const auto text = def.value("pattern", std::string{});
    std::istringstream stream{text};
    std::string token;
    while (stream >> token) {
        if (token.find('?') != std::string::npos) {
            find.bytes.push_back(0);
            find.mask.push_back(0);
            continue;
        }
        try {
            find.bytes.push_back(static_cast<u8>(std::stoul(token, nullptr, 16)));
            find.mask.push_back(1);
        } catch (const std::exception&) {
            return {};
        }
    }
    if (def.contains("offset")) {
        find.offset = ParseNumber(def.at("offset"));
    }
    find.index = def.value("index", 0u);
    find.heap = def.value("region", std::string{}) == "heap";
    return find;
}

/// One token of a point's address chain: "main+0x1234" / "0x8000000" / "+0x10" (the first token
/// may also start with "abs"), or "static_fields" / "statics" (StaticFieldsToken).
void ParseAddressToken(const std::string& token, DataPoint& point, bool first) {
    if (token == "static_fields" || token == "statics") {
        point.chain.push_back(StaticFieldsToken);
        return;
    }
    std::string rest = token;
    if (rest.rfind("main", 0) == 0) {
        point.base = BaseRegion::Main;
        rest.erase(0, 4);
    } else if (first && rest.rfind("abs", 0) == 0) {
        point.base = BaseRegion::Absolute;
        rest.erase(0, 3);
    }
    if (!rest.empty() && (rest.front() == '+' || rest.front() == ' ')) {
        rest.erase(rest.begin());
    }
    if (rest.empty()) {
        point.chain.push_back(0);
        return;
    }
    try {
        const bool hex = rest.rfind("0x", 0) == 0 || rest.rfind("0X", 0) == 0;
        point.chain.push_back(static_cast<s64>(std::stoll(rest, nullptr, hex ? 16 : 10)));
    } catch (const std::exception&) {
        point.chain.push_back(0);
    }
}

void ParsePoints(const nlohmann::json& points_json, Manifest& manifest) {
    for (const auto& [name, def] : points_json.items()) {
        DataPoint point;
        if (def.contains("type")) {
            point.type = ParseValueType(def.at("type").get<std::string>());
        }
        point.is_pointer = def.value("pointer", false);
        point.root_bind = def.value("root", std::string{});
        if (def.contains("player") && def.at("player").is_object()) {
            const auto& pl = def.at("player");
            point.player.vtable = pl.contains("vtable") ? ParseNumber(pl.at("vtable")) : 0;
            point.player.pos_offset =
                pl.contains("pos_offset") ? ParseNumber(pl.at("pos_offset")) : 0;
            point.player.min_samples = static_cast<int>(
                pl.contains("min_samples") ? ParseNumber(pl.at("min_samples")) : 90);
            point.player.min_abs =
                static_cast<float>(pl.contains("min_abs") ? ParseNumber(pl.at("min_abs")) : 1000);
            if (point.chain.empty()) {
                point.chain.push_back(0);
            }
        }
        if (def.contains("array") && def.at("array").is_object()) {
            const auto& a = def.at("array");
            point.array.vtable = a.contains("vtable") ? ParseNumber(a.at("vtable")) : 0;
            point.array.stride = a.contains("stride") ? ParseNumber(a.at("stride")) : 0;
            point.array.index = a.contains("index") ? ParseNumber(a.at("index")) : 0;
            point.array.min_run =
                static_cast<int>(a.contains("min_run") ? ParseNumber(a.at("min_run")) : 8);
            if (point.chain.empty()) {
                point.chain.push_back(0);
            }
        }
        point.count = def.contains("count") ? ParseNumber(def.at("count")) : 1;
        point.count_bind = def.value("count_bind", std::string{});
        // Integer post-read modifiers: shift, mask, bit (= shift n, mask 1), popcount.
        if (def.contains("shift")) {
            point.shift = static_cast<s32>(std::clamp<s64>(ParseNumber(def.at("shift")), 0, 63));
        }
        if (def.contains("mask")) {
            const auto& m = def.at("mask");
            point.has_mask = true;
            if (m.is_number_unsigned()) {
                point.mask = m.get<u64>();
            } else if (m.is_number_integer()) {
                point.mask = static_cast<u64>(m.get<s64>());
            } else if (m.is_string()) {
                const auto text = m.get<std::string>();
                const bool hex = text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0;
                point.mask = std::strtoull(text.c_str(), nullptr, hex ? 16 : 10);
            }
        }
        if (def.contains("bit")) {
            point.shift = static_cast<s32>(std::clamp<s64>(ParseNumber(def.at("bit")), 0, 63));
            point.has_mask = true;
            point.mask = 1;
        }
        if (def.contains("popcount") && def.at("popcount").is_boolean()) {
            point.popcount = def.at("popcount").get<bool>();
        }
        if (def.contains("stride")) {
            point.stride = ParseNumber(def.at("stride"));
        }
        if (point.HasModifiers() && (point.type == ValueType::F32 || point.is_pointer)) {
            LOG_WARNING(Core, "DSMod: point '{}': shift/mask/bit/popcount ignored on a {} point",
                        name, point.is_pointer ? "pointer" : "f32");
            point.shift = 0;
            point.has_mask = false;
            point.popcount = false;
        }
        if (def.contains("ptr") && def.at("ptr").is_array()) {
            bool first = true;
            for (const auto& hop : def.at("ptr")) {
                if (hop.is_object()) {
                    // Indexed hop: step into an array, optionally at an index the game itself
                    // chooses (index_bind), which is how you follow a selection.
                    ChainHop parsed;
                    parsed.offset = hop.contains("offset") ? ParseNumber(hop.at("offset")) : 0;
                    parsed.stride = hop.contains("stride") ? ParseNumber(hop.at("stride")) : 0;
                    // "index": "$i" means this hop walks with the array element being sampled,
                    // which is what turns one declaration into a whole inventory.
                    if (hop.contains("index") && hop.at("index").is_string() &&
                        hop.at("index").get<std::string>() == "$i") {
                        parsed.index_from_array = true;
                    } else {
                        parsed.index = hop.contains("index") ? ParseNumber(hop.at("index")) : 0;
                    }
                    parsed.index_bind = hop.value("index_bind", std::string{});
                    parsed.static_fields = hop.value("static_fields", false);
                    if (hop.contains("list_next")) {
                        parsed.is_list = true;
                        parsed.list_next = ParseNumber(hop.at("list_next"));
                        parsed.list_node_at =
                            hop.contains("list_node_at") ? ParseNumber(hop.at("list_node_at")) : 0;
                    }
                    point.hops.push_back(parsed);
                    point.chain.push_back(parsed.offset);
                    first = false;
                    continue;
                }
                ParseAddressToken(hop.get<std::string>(), point, first);
                if (!first && !point.chain.empty()) {
                    ChainHop parsed;
                    if (point.chain.back() == StaticFieldsToken) {
                        parsed.static_fields = true;
                    } else {
                        parsed.offset = point.chain.back();
                    }
                    point.hops.push_back(parsed);
                }
                first = false;
            }
        } else if (def.contains("addr")) {
            ParseAddressToken(def.at("addr").get<std::string>(), point, true);
        }
        point.class_name = def.value("class_name", std::string{});
        if (!point.class_name.empty() && point.chain.empty()) {
            point.chain.push_back(0);
        }
        if (def.contains("text_scan")) {
            const auto& ts = def.at("text_scan");
            if (ts.contains("any") && ts.at("any").is_array()) {
                for (const auto& c : ts.at("any"))
                    point.text_scan.candidates.push_back(c.get<std::string>());
            }
            point.text_scan.offset = ts.contains("offset") ? ParseNumber(ts.at("offset")) : 0;
            if (point.chain.empty()) {
                point.chain.push_back(0);
            }
        }
        if (def.contains("find")) {
            // A scanned address stands in for the chain's first hop, so a package can point at
            // code or data it located by shape rather than by a written-down address.
            point.find = ParsePattern(def.at("find"));
            if (point.chain.empty()) {
                point.chain.push_back(0);
            }
        }
        if (point.chain.empty()) {
            continue;
        }
        if (def.contains("offset")) {
            point.offset = ParseNumber(def.at("offset"));
        }
        manifest.points.emplace(name, std::move(point));
    }
}

/// "derived": [{"name", "terms": [[src, factor], ...], "add", "floor"|"round"} or
/// {"name", "hold_last_nonzero": src, "hold_gate": gate}]. An entry replaces an earlier one of
/// the same name, which is how the data file (parsed after the manifest) wins a clash.
void ParseDerived(const nlohmann::json& list, std::vector<DerivedPoint>& out) {
    if (!list.is_array()) {
        return;
    }
    const auto number = [](const nlohmann::json& v, f64 fallback) {
        return v.is_number() ? v.get<f64>() : fallback;
    };
    for (const auto& d : list) {
        if (!d.is_object() || !d.contains("name") || !d.at("name").is_string()) {
            continue;
        }
        DerivedPoint entry;
        entry.name = d.at("name").get<std::string>();
        if (entry.name.empty()) {
            continue;
        }
        if (d.contains("terms") && d.at("terms").is_array()) {
            for (const auto& t : d.at("terms")) {
                if (t.is_string()) {
                    entry.terms.emplace_back(t.get<std::string>(), 1.0);
                } else if (t.is_array() && !t.empty() && t[0].is_string()) {
                    entry.terms.emplace_back(t[0].get<std::string>(),
                                             t.size() > 1 ? number(t[1], 1.0) : 1.0);
                } else if (t.is_object()) {
                    const std::string src = t.contains("point") && t.at("point").is_string()
                                                ? t.at("point").get<std::string>()
                                                : std::string{};
                    const f64 factor = t.contains("factor") ? number(t.at("factor"), 1.0) : 1.0;
                    if (!src.empty()) {
                        entry.terms.emplace_back(src, factor);
                    }
                }
            }
        }
        if (d.contains("add")) {
            entry.add = number(d.at("add"), 0.0);
        }
        entry.floor =
            d.contains("floor") && d.at("floor").is_boolean() && d.at("floor").get<bool>();
        entry.round =
            d.contains("round") && d.at("round").is_boolean() && d.at("round").get<bool>();
        if (d.contains("hold_last_nonzero") && d.at("hold_last_nonzero").is_string()) {
            entry.hold_last_nonzero = d.at("hold_last_nonzero").get<std::string>();
        }
        if (d.contains("hold_gate") && d.at("hold_gate").is_string()) {
            entry.hold_gate = d.at("hold_gate").get<std::string>();
        }
        if (d.contains("any_eq") && d.at("any_eq").is_object()) {
            const auto& a = d.at("any_eq");
            entry.any_eq_array = a.value("array", std::string{});
            entry.any_eq_count = a.contains("count") ? ParseNumber(a.at("count")) : 0;
            entry.any_eq_value = a.contains("value") ? ParseNumber(a.at("value")) : 0;
        }
        if (d.contains("select") && d.at("select").is_string()) {
            entry.select = d.at("select").get<std::string>();
            entry.select_then = d.value("then", std::string{});
            entry.select_else = d.value("else", std::string{});
        }
        // "cmp": "eq"|"ne"|"ge"|"gt"|"le"|"lt" with "a"/"b", each either a JSON number (a
        // constant) or a string (a published name).
        if (d.contains("cmp") && d.at("cmp").is_string()) {
            entry.cmp_op = d.at("cmp").get<std::string>();
            const auto parse_operand = [](const nlohmann::json& v) {
                DerivedPoint::CmpOperand op;
                if (v.is_number()) {
                    op.is_const = true;
                    op.const_value = v.get<f64>();
                } else if (v.is_string()) {
                    op.name = v.get<std::string>();
                }
                return op;
            };
            entry.cmp_a = d.contains("a") ? parse_operand(d.at("a")) : DerivedPoint::CmpOperand{};
            entry.cmp_b = d.contains("b") ? parse_operand(d.at("b")) : DerivedPoint::CmpOperand{};
        }
        // "all_nonzero"/"any_nonzero": [<published name>, ...].
        if (d.contains("all_nonzero") && d.at("all_nonzero").is_array()) {
            entry.nonzero_require_all = true;
            for (const auto& s : d.at("all_nonzero")) {
                if (s.is_string()) {
                    entry.nonzero_sources.push_back(s.get<std::string>());
                }
            }
        } else if (d.contains("any_nonzero") && d.at("any_nonzero").is_array()) {
            entry.nonzero_require_all = false;
            for (const auto& s : d.at("any_nonzero")) {
                if (s.is_string()) {
                    entry.nonzero_sources.push_back(s.get<std::string>());
                }
            }
        }
        // Runtime 16: "countdown": <target name or constant>, "now": <name> (default
        // "@clock.epoch") -> max(0, target - now).
        if (d.contains("countdown")) {
            const auto& t = d.at("countdown");
            if (t.is_number()) {
                entry.countdown_target.is_const = true;
                entry.countdown_target.const_value = t.get<f64>();
            } else if (t.is_string()) {
                entry.countdown_target.name = t.get<std::string>();
            }
            if (t.is_number() || t.is_string()) {
                const auto now = StringOr(d, "now");
                entry.countdown_now = now.empty() ? std::string{"@clock.epoch"} : now;
            }
        }
        // Runtime 17: "expr": "<expression>" (mod_expr.h), compiled once here. A compile error is
        // logged and the value publishes 0.
        if (d.contains("expr") && d.at("expr").is_string()) {
            entry.expr = d.at("expr").get<std::string>();
            entry.expr_program = CompileExpr(entry.expr);
            if (!entry.expr_program->Ok()) {
                LOG_WARNING(Core, "DSMod: derived '{}': expr \"{}\": {} (publishes 0)", entry.name,
                            entry.expr, entry.expr_program->error);
            }
        }
        const auto same = std::ranges::find(out, entry.name, &DerivedPoint::name);
        if (same != out.end()) {
            *same = std::move(entry);
        } else {
            out.push_back(std::move(entry));
        }
    }
}

/// "name" or "!name" -> a gate on a published value.
PointGate ParseGate(const nlohmann::json& j, const char* key) {
    PointGate gate;
    if (!j.contains(key) || !j.at(key).is_string()) {
        return gate;
    }
    std::string text = j.at(key).get<std::string>();
    if (!text.empty() && text.front() == '!') {
        gate.negate = true;
        text.erase(text.begin());
    }
    gate.point = std::move(text);
    return gate;
}

/// A drag-to-scroll region: page "scrolls" entry or a widget's inline "scroll" object.
std::optional<ScrollRegion> ParseScrollRegion(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("id") || !j.at("id").is_string() || !j.contains("rect") ||
        !j.at("rect").is_array() || j.at("rect").size() != 4) {
        LOG_WARNING(Core, "DSMod: scroll region needs a string \"id\" and a 4-number \"rect\"");
        return std::nullopt;
    }
    ScrollRegion r;
    r.id = j.at("id").get<std::string>();
    for (size_t i = 0; i < 4; ++i) {
        r.rect[i] = static_cast<s32>(ParseNumber(j.at("rect")[i]));
    }
    r.count_bind = j.value("count_bind", std::string{});
    r.row_h = static_cast<s32>(j.contains("row_h") ? ParseNumber(j.at("row_h")) : 0);
    r.cols = std::max<s32>(1, static_cast<s32>(j.contains("cols") ? ParseNumber(j.at("cols")) : 1));
    r.pad = static_cast<s32>(j.contains("pad") ? ParseNumber(j.at("pad")) : 0);
    r.show = ParseGate(j, "show_bind");
    r.reset_bind = j.value("reset_bind", std::string{});
    r.fling = j.value("fling", true);
    r.friction = std::clamp(static_cast<float>(j.value("friction", 0.135)), 0.0001f, 0.99f);
    r.bar_src = StringOr(j, "bar_src");
    r.bar_track_src = StringOr(j, "bar_track_src");
    r.bar_color = ParseColor(j, "bar", 0);
    r.bar_track = ParseColor(j, "bar_track", 0);
    r.bar_w =
        std::max<s32>(1, static_cast<s32>(j.contains("bar_w") ? ParseNumber(j.at("bar_w")) : 6));
    if (r.rect[2] <= 0 || r.rect[3] <= 0) {
        LOG_WARNING(Core, "DSMod: scroll region '{}' has an empty rect", r.id);
    }
    return r;
}

/// "keep_min"/"keep_max": a number, or (repeat templates) "{i}", "{i}+N", "{i}-N" -- the value is
/// then the offset from the element index and `follows_index` is set.
s64 ParseKeepBound(const nlohmann::json& v, bool& follows_index) {
    follows_index = false;
    if (v.is_string()) {
        std::string text = v.get<std::string>();
        std::erase(text, ' ');
        if (text.starts_with("{i}")) {
            follows_index = true;
            const std::string rest = text.substr(3);
            if (rest.empty()) {
                return 0;
            }
            return ParseNumber(nlohmann::json(rest.front() == '+' ? rest.substr(1) : rest), 0);
        }
        return ParseNumber(v, 0);
    }
    return static_cast<s64>(v.get<long long>());
}

/// "light" / "click" / ... (also false / "none" = off); nullopt for anything else.
std::optional<HapticStrength> ParseHapticStrength(const nlohmann::json& v) {
    if (v.is_boolean()) {
        return v.get<bool>() ? HapticStrength::Light : HapticStrength::Off;
    }
    if (!v.is_string()) {
        return std::nullopt;
    }
    const auto text = v.get<std::string>();
    for (size_t i = 0; i < HapticStrengthNames.size(); ++i) {
        if (text == HapticStrengthNames[i]) {
            return static_cast<HapticStrength>(i);
        }
    }
    if (text == "none") {
        return HapticStrength::Off;
    }
    if (text == "tick") {
        return HapticStrength::Light;
    }
    if (text == "strong") {
        return HapticStrength::Heavy;
    }
    return std::nullopt;
}

/// A widget's / action's "haptic" key: -1 when absent (or not understood).
HapticOverride ParseHapticOverride(const nlohmann::json& j) {
    if (!j.contains("haptic")) {
        return -1;
    }
    if (const auto strength = ParseHapticStrength(j.at("haptic"))) {
        return static_cast<HapticOverride>(*strength);
    }
    LOG_WARNING(Core, "DSMod: haptic '{}' not understood (off/light/click/confirm/heavy/reject)",
                j.at("haptic").dump());
    return -1;
}

Easing ParseEasing(const nlohmann::json& j, const char* key, Easing fallback) {
    if (!j.contains(key) || !j.at(key).is_string()) {
        return fallback;
    }
    const auto text = j.at(key).get<std::string>();
    if (text == "linear") {
        return Easing::Linear;
    }
    if (text == "ease_in") {
        return Easing::EaseIn;
    }
    if (text == "ease_out") {
        return Easing::EaseOut;
    }
    if (text == "ease_in_out") {
        return Easing::EaseInOut;
    }
    LOG_WARNING(Core, "DSMod: easing '{}' not understood (linear/ease_in/ease_out/ease_in_out)",
                text);
    return fallback;
}

/// "haptics": {...} (or true / false).
void ParseHaptics(const nlohmann::json& j, HapticsConfig& out) {
    if (j.is_boolean()) {
        out.enabled = j.get<bool>();
        return;
    }
    if (!j.is_object()) {
        return;
    }
    out.enabled =
        !j.contains("enabled") || !j.at("enabled").is_boolean() || j.at("enabled").get<bool>();
    if (j.contains("respect_system") && j.at("respect_system").is_boolean()) {
        out.respect_system = j.at("respect_system").get<bool>();
    }
    for (size_t k = 0; k < HapticKindNames.size(); ++k) {
        if (!j.contains(HapticKindNames[k])) {
            continue;
        }
        if (const auto strength = ParseHapticStrength(j.at(HapticKindNames[k]))) {
            out.strength[k] = *strength;
        } else {
            LOG_WARNING(Core, "DSMod: haptics.{} '{}' not understood", HapticKindNames[k],
                        j.at(HapticKindNames[k]).dump());
        }
    }
}

/// Manifest "nav" (runtime 17) and "haptics": {"nav": ...}. Absent = on for a package written for
/// runtime 17 or later (min_runtime >= 17), off for an older one so a published package keeps its
/// behaviour; "nav": true or an object (without "enabled": false) turns it on for any package.
/// (A min_runtime >= 17 given only in package.json is applied by the loader: NavDefaultOn.)
void ParseNav(const nlohmann::json& json, NavConfig& out) {
    out = NavConfig{};
    out.enabled = NavDefaultOn(&json, nullptr);
    if (json.contains("haptics") && json.at("haptics").is_object() &&
        json.at("haptics").contains("nav")) {
        if (const auto strength = ParseHapticStrength(json.at("haptics").at("nav"))) {
            out.haptic = static_cast<s8>(*strength);
        }
    }
    if (!json.contains("nav")) {
        return;
    }
    const auto& j = json.at("nav");
    if (j.is_boolean()) {
        out.enabled = j.get<bool>();
        return;
    }
    if (!j.is_object()) {
        LOG_WARNING(Core, "DSMod: \"nav\" must be an object or a boolean");
        return;
    }
    out.enabled = true; // an explicit "nav" object opts in, whatever the package's min_runtime
    if (j.contains("enabled") && j.at("enabled").is_boolean()) {
        out.enabled = j.at("enabled").get<bool>();
    }
    if (j.contains("toggle") && j.at("toggle").is_string()) {
        const auto text = j.at("toggle").get<std::string>();
        if (const auto mask = Nav::ParseChord(text)) {
            out.toggle = text;
            out.toggle_mask = *mask;
        } else {
            LOG_WARNING(Core,
                        "DSMod: nav.toggle '{}' is not a chord of two or more buttons; using {}",
                        text, Nav::DefaultToggle);
        }
    }
    out.color = ParseColor(j, "color", out.color);
    if (j.contains("frame") && j.at("frame").is_number()) {
        out.frame = static_cast<s32>(std::clamp(j.at("frame").get<f64>(), -1.0, 64.0));
    }
    if (j.contains("src") && j.at("src").is_string()) {
        out.src = j.at("src").get<std::string>();
    }
    if (j.contains("haptic")) {
        if (const auto strength = ParseHapticStrength(j.at("haptic"))) {
            out.haptic = static_cast<s8>(*strength);
        }
    }
}

/// Runtime 16: image transforms and fills ("rotate", "rotate_bind", "pivot", "scale_bind",
/// "tint", "tint_bind", "tint_colors", "fill", "slice", "fill_dir", Bar "image"). Every key is
/// optional and its absence leaves the widget exactly as before runtime 16.
void ParseImageStyleKeys(const nlohmann::json& w, Widget& widget) {
    const auto number = [](const nlohmann::json& v) {
        return v.is_number() ? v.get<f64>() : static_cast<f64>(ParseNumber(v));
    };
    if (w.contains("rotate") && w.at("rotate").is_number()) {
        const f64 deg = w.at("rotate").get<f64>();
        widget.rotate = std::isfinite(deg) ? static_cast<float>(std::fmod(deg, 360.0)) : 0.0f;
    }
    widget.rotate_bind = StringOr(w, "rotate_bind");
    widget.scale_bind = StringOr(w, "scale_bind");
    if (w.contains("pivot") && w.at("pivot").is_array() && w.at("pivot").size() == 2) {
        const f64 px = number(w.at("pivot")[0]), py = number(w.at("pivot")[1]);
        if (std::isfinite(px) && std::isfinite(py)) {
            widget.pivot = {static_cast<float>(px), static_cast<float>(py)};
            widget.has_pivot = true;
        }
    }
    if (w.contains("tint")) {
        widget.tint = ParseColor(w, "tint", 0xFFFFFFFFu);
        widget.has_tint = true;
    }
    widget.tint_bind = StringOr(w, "tint_bind");
    if (w.contains("tint_colors") && w.at("tint_colors").is_array()) {
        for (const auto& c : w.at("tint_colors")) {
            nlohmann::json wrap = nlohmann::json::object();
            wrap["c"] = c;
            widget.tint_colors.push_back(ParseColor(wrap, "c", 0xFFFFFFFFu));
        }
    }
    if (w.contains("fill") && w.at("fill").is_string()) {
        const std::string fill = w.at("fill").get<std::string>();
        if (fill == "tile") {
            widget.fill = ImageFill::Tile;
        } else if (fill == "slice") {
            widget.fill = ImageFill::Slice;
        } else if (fill != "stretch") {
            LOG_WARNING(Core, "DSMod: widget '{}': fill '{}' not understood; stretch", widget.id,
                        fill);
        }
    }
    if (w.contains("slice") && w.at("slice").is_array() && w.at("slice").size() == 4) {
        for (size_t i = 0; i < 4; ++i) {
            widget.slice[i] = static_cast<s32>(std::clamp<s64>(ParseNumber(w.at("slice")[i]), 0,
                                                               1 << 16));
        }
    }
    if (w.contains("fill_dir") && w.at("fill_dir").is_string()) {
        const std::string dir = w.at("fill_dir").get<std::string>();
        widget.fill_dir = dir == "left"   ? BarFillDir::Left
                          : dir == "up"   ? BarFillDir::Up
                          : dir == "down" ? BarFillDir::Down
                                          : BarFillDir::Right;
    }
    if (widget.type == WidgetType::Bar && w.contains("image") && w.at("image").is_string()) {
        widget.fill_image = w.at("image").get<std::string>();
    }
}

/// Runtime 17: a Chart widget's "samples", "interval_ms", "style", "min" and "max". `index` is the
/// widget's position on its page, naming the ring buffer when the widget has no id.
std::shared_ptr<const ChartSpec> ParseChartSpec(const nlohmann::json& w, const Widget& widget,
                                                const std::string& page_id, size_t index) {
    auto spec = std::make_shared<ChartSpec>();
    spec->key = widget.id.empty() ? page_id + "#" + std::to_string(index) : widget.id;
    if (w.contains("samples")) {
        spec->samples = static_cast<u32>(std::clamp<s64>(ParseNumber(w.at("samples"), 60), 2, 1024));
    }
    if (w.contains("interval_ms")) {
        spec->interval_ms =
            static_cast<u32>(std::clamp<s64>(ParseNumber(w.at("interval_ms"), 1000), 16, 3600000));
    }
    spec->style =
        StringOr(w, "style", "line") == "bar" ? ChartSpec::Style::Bar : ChartSpec::Style::Line;
    for (const auto& [key, has, out] :
         {std::tuple{"min", &spec->has_min, &spec->min}, std::tuple{"max", &spec->has_max, &spec->max}}) {
        if (w.contains(key) && w.at(key).is_number() && std::isfinite(w.at(key).get<f64>())) {
            *has = true;
            *out = w.at(key).get<f64>();
        }
    }
    return spec;
}

/// A widget's "anim" (null when absent or unusable).
std::shared_ptr<const WidgetAnim> ParseWidgetAnim(const nlohmann::json& w) {
    if (!w.contains("anim") || !w.at("anim").is_object()) {
        return nullptr;
    }
    const auto& a = w.at("anim");
    auto anim = std::make_shared<WidgetAnim>();
    anim->gate = ParseGate(a, "bind");
    if (anim->gate.Empty()) {
        LOG_WARNING(Core, "DSMod: widget anim without a \"bind\" ignored");
        return nullptr;
    }
    anim->key = a.contains("group") && a.at("group").is_string() ? a.at("group").get<std::string>()
                                                                 : a.at("bind").get<std::string>();
    const std::string from =
        a.contains("from") && a.at("from").is_string() ? a.at("from").get<std::string>() : "right";
    if (from == "left") {
        anim->from = WidgetAnim::From::Left;
    } else if (from == "top") {
        anim->from = WidgetAnim::From::Top;
    } else if (from == "bottom") {
        anim->from = WidgetAnim::From::Bottom;
    } else if (from == "fade") {
        anim->from = WidgetAnim::From::Fade;
    } else if (from.starts_with("widget:")) {
        // The group grows out of / shrinks into a named widget's rect instead of sliding in
        // from a box edge.
        anim->from = WidgetAnim::From::Widget;
        anim->origin_widget = from.substr(7);
        if (anim->origin_widget.empty()) {
            LOG_WARNING(Core, "DSMod: anim from 'widget:' needs an id after the colon");
        }
    } else if (from != "right") {
        LOG_WARNING(Core,
                    "DSMod: anim from '{}' not understood (right/left/top/bottom/fade/widget:<id>)",
                    from);
    }
    if (a.contains("ms") && a.at("ms").is_number()) {
        anim->ms = static_cast<u32>(std::clamp(a.at("ms").get<f64>(), 0.0, 5000.0));
    }
    anim->easing = ParseEasing(a, "easing", Easing::EaseOut);
    if (a.contains("box") && a.at("box").is_array() && a.at("box").size() == 4) {
        for (size_t i = 0; i < 4; ++i) {
            anim->box[i] = static_cast<s32>(ParseNumber(a.at("box")[i]));
        }
        anim->has_box = anim->box[2] > 0 && anim->box[3] > 0;
    }
    return anim;
}

/// One branch of a "page_binds" entry ("when_equal" / "when_not_equal"): the same transition keys
/// a `{"kind": "page", ...}` action takes, duplicated rather than shared with that parsing
/// block so this stays a small, anchored addition.
void ParsePageBindTarget(const nlohmann::json& t, PageBindTarget& out) {
    out.page = t.value("page", std::string{});
    if (t.contains("transition") && t.at("transition").is_string()) {
        const auto tr = t.at("transition").get<std::string>();
        if (tr == "slide_up") {
            out.transition = PageTransition::SlideUp;
        } else if (tr == "slide_down") {
            out.transition = PageTransition::SlideDown;
        } else if (tr == "grow") {
            out.transition = PageTransition::Grow;
        } else if (tr == "shrink") {
            out.transition = PageTransition::Shrink;
        } else if (tr == "fade") {
            out.transition = PageTransition::Fade;
        } else if (tr != "none") {
            LOG_WARNING(Core, "DSMod: page_binds transition '{}' not understood", tr);
        }
    }
    if (t.contains("duration_ms") && t.at("duration_ms").is_number()) {
        out.duration_ms = static_cast<u32>(std::clamp(t.at("duration_ms").get<f64>(), 0.0, 2000.0));
    }
    out.easing = ParseEasing(t, "easing", Easing::EaseOut);
    if (t.contains("shadow") && t.at("shadow").is_number()) {
        out.shadow = static_cast<float>(std::clamp(t.at("shadow").get<f64>(), 0.0, 1.0));
    }
    out.origin = t.value("origin", std::string{});
}

/// An action value: integer, float, "$payload" (flagged separately) or "$name".
ActionValue ParseActionValue(const nlohmann::json& v) {
    ActionValue out;
    if (v.is_string()) {
        const auto text = v.get<std::string>();
        if (text.size() > 1 && text.front() == '$') {
            out.ref = text.substr(1);
        } else {
            out.value = ParseNumber(v);
        }
    } else if (v.is_boolean()) {
        out.value = v.get<bool>() ? 1 : 0;
    } else if (v.is_number_float()) {
        out.as_float = v.get<f64>();
        out.value = static_cast<s64>(*out.as_float);
    } else {
        out.value = ParseNumber(v);
    }
    return out;
}

float JsonFloat(const nlohmann::json& j, const char* key, float fallback) {
    return j.contains(key) && j.at(key).is_number() ? static_cast<float>(j.at(key).get<f64>())
                                                    : fallback;
}

void ParseFloatPair(const nlohmann::json& j, const char* key, float& a, float& b) {
    if (j.contains(key) && j.at(key).is_array() && j.at(key).size() == 2 &&
        j.at(key)[0].is_number() && j.at(key)[1].is_number()) {
        a = static_cast<float>(j.at(key)[0].get<f64>());
        b = static_cast<float>(j.at(key)[1].get<f64>());
    }
}

/// Map widget "groups", "label_style" and tap keys; null when none is present.
std::shared_ptr<const MapWidgetExtras> ParseMapWidgetExtras(const nlohmann::json& w) {
    static constexpr std::array keys{"groups",
                                     "label_style",
                                     "on_map_tap",
                                     "on_marker_tap",
                                     "tap_enabled_bind",
                                     "marker_hit_px",
                                     "marker_tap_groups",
                                     "empty_tap_deselects",
                                     "view_rect_x0_bind",
                                     "view_rect_y0_bind",
                                     "view_rect_x1_bind",
                                     "view_rect_y1_bind",
                                     "view_rect_pad",
                                     "image_bind",
                                     "marker_rotate_bind",
                                     "marker_tint",
                                     "overlays"};
    if (std::ranges::none_of(keys, [&](const char* k) { return w.contains(k); })) {
        return nullptr;
    }
    auto extras = std::make_shared<MapWidgetExtras>();
    if (w.contains("groups") && w.at("groups").is_object()) {
        for (const auto& [name, g] : w.at("groups").items()) {
            if (!g.is_object()) {
                continue;
            }
            MapWidgetExtras::Group group;
            group.show = ParseGate(g, "show_bind");
            group.hide = ParseGate(g, "hide_bind");
            if (g.contains("hide_in") && g.at("hide_in").is_array()) {
                for (const auto& v : g.at("hide_in")) {
                    group.hide_in.push_back(ParseNumber(v));
                }
                group.hide.negate = false;
            }
            group.opacity = std::clamp(JsonFloat(g, "opacity", 1.0f), 0.0f, 1.0f);
            group.min_zoom = std::max(0.0f, JsonFloat(g, "min_zoom", 0.0f));
            extras->groups[name] = std::move(group);
        }
    }
    if (w.contains("label_style") && w.at("label_style").is_object()) {
        const auto& ls = w.at("label_style");
        auto& st = extras->label_style;
        st.text_scale = static_cast<s32>(ls.value("text_scale", st.text_scale));
        st.color = ParseColor(ls, "color", st.color);
        st.outline_color = ParseColor(ls, "outline_color", st.outline_color);
        st.outline = static_cast<s32>(ls.value("outline", st.outline));
        st.opacity = std::clamp(JsonFloat(ls, "opacity", st.opacity), 0.0f, 1.0f);
        st.avoid_overlap = ls.value("avoid_overlap", st.avoid_overlap);
    }
    extras->on_map_tap = w.value("on_map_tap", std::string{});
    extras->on_marker_tap = w.value("on_marker_tap", std::string{});
    extras->empty_tap_deselects = w.value("empty_tap_deselects", true);
    extras->tap_enabled = ParseGate(w, "tap_enabled_bind");
    extras->marker_hit_px = std::max<s32>(32, static_cast<s32>(w.value("marker_hit_px", 32)));
    // Runtime 14: the bound default view rect (all four binds, or none takes effect).
    static constexpr std::array rect_keys{"view_rect_x0_bind", "view_rect_y0_bind",
                                          "view_rect_x1_bind", "view_rect_y1_bind"};
    for (size_t i = 0; i < rect_keys.size(); ++i) {
        extras->view_rect_binds[i] = w.value(rect_keys[i], std::string{});
    }
    extras->view_rect_pad = std::max(0.0f, JsonFloat(w, "view_rect_pad", 0.0f));
    // Runtime 14: a bound base picture and world-space overlays.
    extras->image_bind = w.value("image_bind", std::string{});
    extras->marker_rotate_bind = w.value("marker_rotate_bind", std::string{});
    extras->marker_tint = ParseColor(w, "marker_tint", 0xFFFFFFFFu);
    if (w.contains("overlays") && w.at("overlays").is_array()) {
        for (const auto& o : w.at("overlays")) {
            if (!o.is_object()) {
                continue;
            }
            MapWidgetExtras::Overlay ov;
            ov.src = o.value("src", std::string{});
            ov.src_bind = o.value("src_bind", std::string{});
            ov.src_detail_bind = o.value("src_detail_bind", std::string{});
            ov.detail_threshold = std::max(1.0f, JsonFloat(o, "detail_threshold", 210.0f));
            ov.x0 = JsonFloat(o, "x0", 0.0f);
            ov.y0 = JsonFloat(o, "y0", 0.0f);
            ov.x1 = JsonFloat(o, "x1", 0.0f);
            ov.y1 = JsonFloat(o, "y1", 0.0f);
            ov.show = ParseGate(o, "show_bind");
            ov.opacity = std::clamp(JsonFloat(o, "opacity", 1.0f), 0.0f, 1.0f);
            if ((ov.src.empty() && ov.src_bind.empty()) || ov.x0 == ov.x1 || ov.y0 == ov.y1) {
                LOG_WARNING(Core,
                            "DSMod: map overlay without src/src_bind or an empty box ignored");
                continue;
            }
            extras->overlays.push_back(std::move(ov));
        }
    }
    if (!extras->HasViewRect() &&
        std::ranges::any_of(extras->view_rect_binds, [](const auto& b) { return !b.empty(); })) {
        LOG_WARNING(Core, "DSMod: map widget has only some view_rect_*_bind keys; the view rect "
                          "needs all four and is ignored");
    }
    if (w.contains("marker_tap_groups") && w.at("marker_tap_groups").is_array()) {
        for (const auto& g : w.at("marker_tap_groups")) {
            if (g.is_string()) {
                extras->marker_tap_groups.push_back(g.get<std::string>());
            }
        }
    }
    return extras;
}

/// One map.areas object -> MapArea entries (emplace: an existing name keeps its entry). Pure;
/// shared by the inline manifest path and the module-provided "map.areas_src" (runtime 12).
void ParseMapAreasInto(const nlohmann::json& areas,
                       std::unordered_map<std::string, MapArea>& out) {
    for (const auto& [name, area] : areas.items()) {
        MapArea entry;
        entry.geo = area.value("geo", std::string{});
        entry.image = area.value("image", std::string{});
        entry.no_pin = area.value("no_pin", false);
        entry.follow_pad = area.value("follow_pad", entry.follow_pad);
        entry.clamp_view = area.value("clamp_view", false);
        if (area.contains("overview_regions")) {
            entry.overview_regions.emplace();
            for (const auto& region : area.at("overview_regions")) {
                MapArea::OverviewRegion value;
                value.kind = region.value("kind", std::string{});
                if (value.kind != "room" && value.kind != "station" &&
                    value.kind != "transport" && value.kind != "zone") {
                    continue;
                }
                for (const auto& triangle : region.at("t")) {
                    if (!triangle.is_array() || triangle.size() != 6) {
                        continue;
                    }
                    const auto points = triangle.get<std::array<float, 6>>();
                    if (std::ranges::all_of(points, [](float v) { return std::isfinite(v); })) {
                        value.tris.push_back(points);
                    }
                }
                if (!value.tris.empty()) {
                    entry.overview_regions->push_back(std::move(value));
                }
            }
        }
        if (area.contains("min") && area.at("min").size() == 2) {
            entry.min_x = area.at("min")[0].get<float>();
            entry.min_y = area.at("min")[1].get<float>();
        }
        if (area.contains("max") && area.at("max").size() == 2) {
            entry.max_x = area.at("max")[0].get<float>();
            entry.max_y = area.at("max")[1].get<float>();
        }
        if (area.contains("icons")) {
            for (const auto& m : area.at("icons")) {
                MapMarker marker;
                marker.kind = m.value("k", std::string{});
                marker.icon = m.value("i", std::string{});
                marker.x = m.value("x", 0.0f);
                marker.y = m.value("y", 0.0f);
                marker.name = m.value("n", std::string{});
                marker.vignette = m.value("v", std::string{});
                marker.hidden = !marker.vignette.empty(); // hidden until dispel confirmed
                marker.show = ParseGate(m, "show_bind");
                marker.hide = ParseGate(m, "hide_bind");
                marker.group = m.value("group", std::string{});
                marker.opacity = std::clamp(JsonFloat(m, "opacity", 1.0f), 0.0f, 1.0f);
                marker.size = static_cast<s32>(m.value("size", 0));
                // Explicit per-icon behaviour, in place of the old Door/Blockage/ Adquired
                // string conventions. Unset here, each is computed from `icon` at draw time
                // using that same convention -- see mod_types_map.h's MapMarker.
                if (m.contains("structural")) {
                    marker.structural = m.at("structural").get<bool>();
                }
                marker.open_icon = m.value("open_icon", std::string{});
                marker.collected_icon = m.value("collected_icon", std::string{});
                if (m.contains("collectible")) {
                    marker.collectible = m.at("collectible").get<bool>();
                }
                if (m.contains("bx") && m.at("bx").size() == 4) {
                    const auto& bx = m.at("bx");
                    marker.bx0 = bx[0].get<float>();
                    marker.by0 = bx[1].get<float>();
                    marker.bx1 = bx[2].get<float>();
                    marker.by1 = bx[3].get<float>();
                    marker.has_box = true;
                }
                if (m.contains("pb") && m.at("pb").size() == 4) {
                    const auto& pb = m.at("pb");
                    marker.px0 = pb[0].get<float>();
                    marker.py0 = pb[1].get<float>();
                    marker.px1 = pb[2].get<float>();
                    marker.py1 = pb[3].get<float>();
                    marker.has_pulse_box = true;
                }
                if (m.contains("hb") && m.at("hb").size() == 4) {
                    const auto& hb = m.at("hb");
                    marker.hx0 = hb[0].get<float>();
                    marker.hy0 = hb[1].get<float>();
                    marker.hx1 = hb[2].get<float>();
                    marker.hy1 = hb[3].get<float>();
                    marker.has_hint_box = true;
                }
                entry.markers.push_back(std::move(marker));
            }
        }
        if (area.contains("dynamic_markers") && area.at("dynamic_markers").is_array()) {
            for (const auto& d : area.at("dynamic_markers")) {
                if (!d.is_object()) {
                    continue;
                }
                DynamicMarkerDef dm;
                dm.group = d.value("group", std::string{});
                dm.count = d.contains("count") ? ParseNumber(d.at("count")) : 0;
                dm.x = d.value("x", std::string{});
                dm.y = d.value("y", std::string{});
                dm.kind = d.value("kind", std::string{});
                if (d.contains("hide_when_kind")) {
                    dm.has_hide_kind = true;
                    dm.hide_when_kind = ParseNumber(d.at("hide_when_kind"));
                }
                if (d.contains("icon_by_kind") && d.at("icon_by_kind").is_object()) {
                    for (const auto& [k, icon] : d.at("icon_by_kind").items()) {
                        if (icon.is_string()) {
                            dm.icon_by_kind[ParseNumber(nlohmann::json(k))] =
                                icon.get<std::string>();
                        }
                    }
                }
                dm.icon_default = d.value("icon_default", std::string{});
                dm.icon = d.value("icon", std::string{});
                ParseFloatPair(d, "scale", dm.scale_x, dm.scale_y);
                ParseFloatPair(d, "offset", dm.offset_x, dm.offset_y);
                ParseFloatPair(d, "anchor", dm.anchor_x, dm.anchor_y);
                dm.size = static_cast<s32>(d.value("size", 0));
                dm.selected_size = static_cast<s32>(d.value("selected_size", 0));
                dm.selected_icon = d.value("selected_icon", std::string{});
                dm.selected_icon_size = static_cast<s32>(d.value("selected_icon_size", 0));
                dm.opacity = std::clamp(JsonFloat(d, "opacity", 1.0f), 0.0f, 1.0f);
                dm.show = ParseGate(d, "show_bind");
                dm.hide = ParseGate(d, "hide_bind");
                // Runtime 14: per-slot pictures, world size, bar, dim and frame / tint.
                dm.icon_src_bind = d.value("icon_src_bind", std::string{});
                dm.size_world = std::max(0.0f, JsonFloat(d, "size_world", 0.0f));
                dm.size_max = std::max(0.0f, JsonFloat(d, "size_max", 0.0f));
                dm.bar_bind = d.value("bar_bind", std::string{});
                dm.bar_max_bind = d.value("bar_max_bind", std::string{});
                dm.bar_max = d.contains("bar_max") ? ParseNumber(d.at("bar_max")) : 100;
                dm.bar_color = ParseColor(d, "bar_color", dm.bar_color);
                dm.bar_bg = ParseColor(d, "bar_bg", dm.bar_bg);
                dm.bar_h = static_cast<s32>(d.value("bar_h", 0));
                dm.dim_bind = d.value("dim_bind", std::string{});
                dm.frame_color_bind = d.value("frame_color_bind", std::string{});
                dm.frame_px = static_cast<s32>(d.value("frame_px", 0));
                dm.tint_bind = d.value("tint_bind", std::string{});
                if (dm.count > 0 && !dm.x.empty() && !dm.y.empty()) {
                    entry.dynamic_markers.push_back(std::move(dm));
                } else {
                    LOG_WARNING(Core,
                                "DSMod: area '{}': dynamic_markers entry without "
                                "count/x/y ignored",
                                name);
                }
            }
        }
        if (area.contains("labels") && area.at("labels").is_array()) {
            for (const auto& l : area.at("labels")) {
                if (!l.is_object()) {
                    continue;
                }
                MapLabel label;
                if (l.contains("text") && l.at("text").is_string()) {
                    label.text = l.at("text").get<std::string>();
                }
                if (l.contains("text_src") && l.at("text_src").is_string()) {
                    label.text_src = l.at("text_src").get<std::string>();
                }
                label.x = JsonFloat(l, "x", 0.0f);
                label.y = JsonFloat(l, "y", 0.0f);
                label.show = ParseGate(l, "show_bind");
                label.hide = ParseGate(l, "hide_bind");
                label.group = l.value("group", std::string{});
                label.opacity = std::clamp(JsonFloat(l, "opacity", 1.0f), 0.0f, 1.0f);
                label.text_scale = static_cast<s32>(l.value("text_scale", 0));
                if (l.contains("color")) {
                    label.color = ParseColor(l, "color", 0xFFFFFFFFu);
                }
                if (l.contains("outline_color")) {
                    label.outline_color = ParseColor(l, "outline_color", 0xFF000000u);
                }
                label.outline = static_cast<s32>(l.value("outline", -1));
                entry.labels.push_back(std::move(label));
            }
        }
        if (area.contains("layers")) {
            for (const auto& l : area.at("layers")) {
                MapLayer layer;
                layer.geo = l.value("geo", std::string{});
                layer.kind = l.value("kind", std::string{});
                if (layer.kind.empty()) {
                    // Legacy manifests used asset names as an implicit type. Keep those
                    // packages loadable while making new manifests explicit and generic.
                    std::string lower = layer.geo;
                    std::transform(
                        lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (lower.find(".emmy.") != std::string::npos) {
                        layer.kind = "zone";
                    }
                    for (const auto& candidate :
                         {"water", "magnet", "zone", "heat", "freeze"}) {
                        if (lower.find(std::string{"."} + candidate + ".") !=
                            std::string::npos) {
                            layer.kind = candidate;
                            break;
                        }
                    }
                    if (layer.kind.empty()) {
                        layer.kind = "hazard";
                    }
                }
                layer.color = ParseColor(l, "color", 0xFF808080u);
                // Generic colour-by-state (the color_bind/color_map primitive), and the
                // three behaviour flags that replace matching `kind` against a string
                // literal.
                layer.color_bind = l.value("color_bind", std::string{});
                layer.color_map = ParseColorMap(l, "color_map");
                layer.category = l.value("category", std::string{});
                layer.live_clip = l.value("live_clip", false);
                layer.post_fog = l.value("post_fog", false);
                if (l.contains("gain")) {
                    layer.gain = static_cast<float>(l.value("gain", 1.0));
                }
                if (!layer.geo.empty()) {
                    entry.layers.push_back(std::move(layer));
                }
            }
        }
        const auto parse_polys = [](const auto& arr, std::vector<std::vector<float>>& out) {
            for (const auto& poly : arr) {
                std::vector<float> flat;
                for (const auto& c : poly)
                    flat.push_back(c.template get<float>());
                if (flat.size() >= 6)
                    out.push_back(std::move(flat));
            }
        };
        // N named, ordered room categories, replacing gold_polys/transport_polys/emmy_polys
        // (a plain reshape of the same already-baked poly data). Array order = precedence,
        // highest first.
        if (area.contains("room_categories") && area.at("room_categories").is_array()) {
            for (const auto& rc : area.at("room_categories")) {
                MapRoomCategory cat;
                cat.id = rc.value("id", std::string{});
                if (cat.id.empty()) {
                    LOG_WARNING(Core,
                                "DSMod: area '{}': room_categories entry without an id "
                                "ignored",
                                name);
                    continue;
                }
                if (rc.contains("polys")) {
                    parse_polys(rc.at("polys"), cat.polys);
                }
                cat.color = ParseColor(rc, "color", 0u);
                cat.color_bind = rc.value("color_bind", std::string{});
                cat.color_map = ParseColorMap(rc, "color_map");
                cat.class_gain = static_cast<float>(rc.value("class_gain", 1.0));
                cat.reveal_before_unlock = rc.value("reveal_before_unlock", false);
                if (rc.contains("bake")) {
                    const auto& bk = rc.at("bake");
                    cat.bake.mode = bk.value("mode", std::string{"polys"});
                    cat.bake.gain = static_cast<float>(bk.value("gain", 1.0));
                }
                // Per-category counterpart of map.style's visited_gain/
                // visited_tint/visited_amount -- see MapRoomCategory's own doc comment
                // (mod_types_map.h). Same field names/JSON keys as the style-level ones, same
                // no-op defaults, just scoped to this one category's matched cells.
                cat.visited_gain = static_cast<s32>(rc.value("visited_gain", cat.visited_gain));
                cat.visited_tint = ParseColor(rc, "visited_tint", cat.visited_tint);
                cat.visited_amount =
                    static_cast<int>(rc.value("visited_amount", cat.visited_amount));
                // "visited_map": {"<state>": {visited_gain/visited_tint/visited_amount}} --
                // per-state overrides keyed like color_map (MapRoomCategory::visited_map);
                // an omitted field falls back to the category-level value just parsed.
                if (rc.contains("visited_map") && rc.at("visited_map").is_object()) {
                    auto vm = std::make_shared<
                        std::unordered_map<s64, MapRoomCategory::VisitedCorrection>>();
                    for (const auto& [text_key, value] : rc.at("visited_map").items()) {
                        const bool hex =
                            text_key.starts_with("0x") || text_key.starts_with("-0x");
                        char* end = nullptr;
                        const long long ikey =
                            std::strtoll(text_key.c_str(), &end, hex ? 16 : 10);
                        if (end == text_key.c_str() || *end != '\0' || !value.is_object()) {
                            LOG_WARNING(Core,
                                        "DSMod: area '{}' category '{}': visited_map "
                                        "entry '{}' ignored",
                                        name, cat.id, text_key);
                            continue;
                        }
                        MapRoomCategory::VisitedCorrection vc{
                            static_cast<s32>(value.value("visited_gain", cat.visited_gain)),
                            ParseColor(value, "visited_tint", cat.visited_tint),
                            static_cast<int>(
                                value.value("visited_amount", cat.visited_amount))};
                        vm->insert_or_assign(static_cast<s64>(ikey), vc);
                    }
                    cat.visited_map = std::move(vm);
                }
                entry.room_categories.push_back(std::move(cat));
            }
        }
        if (area.contains("occluders")) {
            for (const auto& oc : area.at("occluders")) {
                MapArea::MapOccluder o;
                o.name = oc.value("n", std::string{});
                for (const auto& tr : oc.at("t")) {
                    if (tr.size() == 6) {
                        o.tris.push_back({tr[0].get<float>(), tr[1].get<float>(),
                                          tr[2].get<float>(), tr[3].get<float>(),
                                          tr[4].get<float>(), tr[5].get<float>()});
                    }
                }
                if (!o.name.empty() && !o.tris.empty()) {
                    entry.occluders.push_back(std::move(o));
                }
            }
        }
        if (area.contains("vignettes")) {
            for (const auto& vg : area.at("vignettes")) {
                MapArea::MapOccluder v;
                v.name = vg.value("n", std::string{});
                for (const auto& tr : vg.at("t")) {
                    if (tr.size() == 6) {
                        v.tris.push_back({tr[0].get<float>(), tr[1].get<float>(),
                                          tr[2].get<float>(), tr[3].get<float>(),
                                          tr[4].get<float>(), tr[5].get<float>()});
                    }
                }
                if (!v.name.empty() && !v.tris.empty()) {
                    entry.vignettes.push_back(std::move(v));
                }
            }
        }
        if (area.contains("camera_rects")) {
            for (const auto& r : area.at("camera_rects")) {
                if (r.size() == 4) {
                    entry.camera_rects.push_back({r[0].get<float>(), r[1].get<float>(),
                                                  r[2].get<float>(), r[3].get<float>()});
                }
            }
        }
        out.emplace(name, std::move(entry));
    }
}

void ParseManifestJson(const nlohmann::json& json, Manifest& manifest) {
    manifest.format = json.value("format", 1u);
    manifest.name = json.value("name", std::string{"dual screen mod"});
    manifest.poll_hz = json.value("poll_hz", 60u);
    manifest.canvas_w = json.value("canvas_w", 0u);
    manifest.canvas_h = json.value("canvas_h", 0u);
    manifest.debug_page = Common::DSMod::DevToolsEnabled && json.value("debug_page", false);
    manifest.font_metrics_src = json.value("font", std::string{});
    manifest.font_atlas_src = json.value("font_atlas", std::string{});
    if (json.contains("font_page_h") && json.at("font_page_h").is_number_unsigned()) {
        manifest.font_page_h = static_cast<u32>(
            std::min<u64>(json.at("font_page_h").get<u64>(), 65535)); // runtime 17
    }
    NxAssets::ParseManifestExtras(json, manifest); // "composites", long font keys
    if (json.contains("tables") && json.at("tables").is_object()) {
        for (const auto& [tname, arr] : json.at("tables").items()) {
            if (!arr.is_array())
                continue;
            std::vector<std::string> vals;
            vals.reserve(arr.size());
            for (const auto& v : arr)
                vals.push_back(v.is_string() ? v.get<std::string>() : std::string{});
            size_t longest = 0;
            for (const auto& v : vals)
                longest = std::max(longest, v.size());
            if (manifest.tables.emplace(tname, std::move(vals)).second)
                manifest.table_max_len[tname] = longest;
        }
    }
    manifest.find_spec = json.value("find", std::string{});
    manifest.trace_target = json.value("trace", std::string{});
    manifest.dump_registry = Common::DSMod::DevToolsEnabled && json.value("dump_registry", false);
    manifest.uses_clock_keys = JsonReferencesClockKeys(json); // the data file may add to it
    if (json.contains("module_tick_hidden")) {
        manifest.module_tick_hidden = json.at("module_tick_hidden").get<bool>();
    }
    manifest.describe_string = json.value("describe", std::string{});
    if (json.contains("frame_hook")) {
        const auto hook = json.at("frame_hook").get<std::string>();
        if (!hook.empty() && hook.front() == '$') {
            manifest.frame_hook_symbol = hook.substr(1);
        } else {
            DataPoint hook_point;
            ParseAddressToken(hook, hook_point, true);
            manifest.frame_hook = hook_point.chain.empty() ? 0 : hook_point.chain.front();
        }
    }
    manifest.background = ParseColor(json, "background", ToPixelOrder(manifest.background));
    if (json.contains("haptics")) {
        ParseHaptics(json.at("haptics"), manifest.haptics);
    }
    // "nav" (runtime 17): controller navigation of the second screen (mod_nav.cpp).
    ParseNav(json, manifest.nav);
    if (json.contains("anim_hz") && json.at("anim_hz").is_number()) {
        manifest.anim_hz = static_cast<u32>(std::clamp(json.at("anim_hz").get<f64>(), 1.0, 240.0));
    }
    // "page_binds" -- edge-triggered automatic page switches. Absent/empty = no behaviour change
    // for every package that predates this.
    if (json.contains("page_binds") && json.at("page_binds").is_array()) {
        for (const auto& b : json.at("page_binds")) {
            if (!b.is_object() || !b.contains("point") || !b.at("point").is_string() ||
                !b.contains("equals")) {
                LOG_WARNING(Core,
                            "DSMod: page_binds entry needs a string \"point\" and \"equals\"");
                continue;
            }
            PageBind bind;
            bind.point = b.at("point").get<std::string>();
            bind.equals = b.at("equals").is_number_float()
                              ? b.at("equals").get<f64>()
                              : static_cast<f64>(ParseNumber(b.at("equals")));
            bind.ready = ParseGate(b, "ready_bind");
            if (b.contains("when_equal") && b.at("when_equal").is_object()) {
                ParsePageBindTarget(b.at("when_equal"), bind.when_equal);
            }
            if (b.contains("when_not_equal") && b.at("when_not_equal").is_object()) {
                ParsePageBindTarget(b.at("when_not_equal"), bind.when_not_equal);
            }
            if (bind.when_equal.page.empty() && bind.when_not_equal.page.empty()) {
                LOG_WARNING(
                    Core,
                    "DSMod: page_binds on '{}' has no when_equal.page / when_not_equal.page, "
                    "ignored",
                    bind.point);
                continue;
            }
            manifest.page_binds.push_back(std::move(bind));
        }
    }

    if (json.contains("pages")) {
        for (const auto& page_json : json.at("pages")) {
            Page page;
            page.id = page_json.value("id", std::string{});
            page.title = page_json.value("title", std::string{});
            if (page_json.contains("scrolls") && page_json.at("scrolls").is_array()) {
                for (const auto& sj : page_json.at("scrolls")) {
                    if (auto region = ParseScrollRegion(sj)) {
                        if (FindScrollRegion(page, region->id) != nullptr) {
                            LOG_WARNING(Core, "DSMod: page '{}' repeats scroll id '{}'", page.id,
                                        region->id);
                            continue;
                        }
                        page.scrolls.push_back(std::move(*region));
                    }
                }
            }
            if (page_json.contains("widgets")) {
                for (const auto& w : page_json.at("widgets")) {
                    Widget widget;
                    widget.type = ParseWidgetType(w.value("type", std::string{"label"}));
                    widget.src = w.value("src", std::string{});
                    widget.x_bind = w.value("x_bind", std::string{});
                    widget.y_bind = w.value("y_bind", std::string{});
                    widget.x_scale = w.value("x_scale", 1.0f);
                    widget.y_scale = w.value("y_scale", 1.0f);
                    widget.fill_bind = w.value("fill_bind", std::string{});
                    widget.bind_text = w.value("bind_text", std::string{});
                    widget.src_bind = w.value("src_bind", std::string{});
                    if (w.contains("src_names") && w.at("src_names").is_array()) {
                        for (const auto& n : w.at("src_names"))
                            widget.src_names.push_back(n.get<std::string>());
                    }
                    if (w.contains("src_thresholds") && w.at("src_thresholds").is_array()) {
                        for (const auto& trow : w.at("src_thresholds")) {
                            widget.src_thresholds.emplace_back(
                                static_cast<s64>(trow.value("le", 0)),
                                trow.value("src", std::string{}));
                        }
                    }
                    widget.table = w.value("table", std::string{});
                    widget.src_format = w.value("src_format", std::string{});
                    widget.hide_bind = w.value("hide_bind", std::string{});
                    {
                        const std::string al = w.value("align", std::string{"left"});
                        widget.align = al == "center" ? 1 : al == "right" ? 2 : 0;
                    }
                    widget.flip_x = w.value("flip_x", false);
                    widget.flip_y = w.value("flip_y", false);
                    widget.spin = static_cast<float>(w.value("spin", 0.0));
                    widget.shake = static_cast<float>(w.value("shake", 0.0));
                    widget.need_bind = w.value("need_bind", std::string{});
                    widget.suffix = w.value("suffix", std::string{});
                    widget.area_label = w.value("area_label", true);
                    widget.hidden_icons = w.value("hidden_icons", std::vector<std::string>{});
                    widget.pill = w.value("pill", false);
                    widget.frame = w.value("frame", 2);
                    widget.border = static_cast<s32>(w.value("border", 3));
                    widget.text_inset = static_cast<s32>(w.value("text_inset", 12));
                    widget.gap = static_cast<s32>(w.value("gap", -1));
                    if (w.contains("label_offset") && w.at("label_offset").is_array() &&
                        w.at("label_offset").size() == 2) {
                        const auto& lo = w.at("label_offset");
                        widget.label_offset = {static_cast<s32>(ParseNumber(lo[0])),
                                               static_cast<s32>(ParseNumber(lo[1]))};
                    }
                    widget.pulse = w.value("pulse", false);
                    if (w.contains("keep_min"))
                        widget.keep_min = ParseKeepBound(w.at("keep_min"), widget.keep_min_i);
                    if (w.contains("keep_max"))
                        widget.keep_max = ParseKeepBound(w.at("keep_max"), widget.keep_max_i);
                    if (w.contains("hide_eq")) {
                        widget.hide_eq = static_cast<s64>(w.at("hide_eq").get<long long>());
                        widget.hide_eq_on = true;
                    }
                    widget.area_bind = w.value("area_bind", std::string{});
                    widget.marker_x_bind = w.value("marker_x_bind", std::string{});
                    widget.marker_y_bind = w.value("marker_y_bind", std::string{});
                    widget.marker_scale = static_cast<float>(w.value("marker_scale", 1.0));
                    widget.marker_icon = w.value("marker_icon", std::string{});
                    widget.actor_x_bind = w.value("actor_x_bind", std::string{});
                    widget.actor_y_bind = w.value("actor_y_bind", std::string{});
                    widget.actor_icon = w.value("actor_icon", std::string{});
                    widget.actor_reveal_required = w.value("actor_reveal_required", false);
                    widget.follow_window = static_cast<float>(w.value("follow_window", 0.0));
                    widget.marker_src = w.value("marker_src", std::string{});
                    for (const auto& [key, target] :
                         {std::pair{"marker_size", &widget.marker_size},
                          std::pair{"marker_anchor", &widget.marker_anchor}}) {
                        if (w.contains(key) && w.at(key).is_array() && w.at(key).size() == 2) {
                            (*target)[0] = static_cast<float>(w.at(key)[0].get<double>());
                            (*target)[1] = static_cast<float>(w.at(key)[1].get<double>());
                        }
                    }
                    widget.view_idle_ms = w.value("view_idle_ms", 0u);
                    widget.room_bind = w.value("room_bind", std::string{});
                    widget.area = w.value("area", std::string{});
                    widget.empty_src = w.value("empty_src", std::string{});
                    for (const auto& [key, target] :
                         {std::pair{"src_rect", &widget.src_rect},
                          std::pair{"empty_rect", &widget.empty_rect}}) {
                        if (w.contains(key) && w.at(key).is_array() && w.at(key).size() == 4) {
                            for (size_t i = 0; i < 4; ++i) {
                                (*target)[i] = static_cast<float>(w.at(key)[i].get<double>());
                            }
                        }
                    }
                    if (w.contains("rect") && w.at("rect").is_array() && w.at("rect").size() == 4) {
                        for (size_t i = 0; i < 4; ++i) {
                            widget.rect[i] = ParseCoordinate(w.at("rect")[i]);
                        }
                    }
                    widget.text = StringOr(w, "text");
                    widget.bind = w.value("bind", std::string{});
                    if (w.contains("names") && w.at("names").is_array()) {
                        for (const auto& n : w.at("names"))
                            widget.names.push_back(n.get<std::string>());
                    }
                    widget.pad = static_cast<s32>(w.value("pad", 0));
                    widget.div = static_cast<s32>(w.value("div", 1));
                    widget.mul = static_cast<s32>(w.value("mul", 1));
                    widget.add = static_cast<s32>(w.value("add", 0));
                    widget.max_bind = w.value("max_bind", std::string{});
                    widget.max_sep = w.value("max_sep", std::string{});
                    if (w.contains("max")) {
                        widget.max_const = ParseNumber(w.at("max"));
                    } else if (w.contains("max_const")) {
                        widget.max_const = ParseNumber(w.at("max_const"));
                    }
                    widget.color = ParseColor(w, "color", ToPixelOrder(widget.color));
                    widget.bg = ParseColor(w, "bg", ToPixelOrder(widget.bg));
                    if (w.contains("outline")) {
                        widget.text_outline = ParseColor(w, "outline", 0u);
                        widget.text_outline_px =
                            std::clamp(static_cast<s32>(w.value("outline_px", 2)), 0, 12);
                    }
                    if (w.contains("rise") && w.at("rise").is_number())
                        widget.text_rise = std::clamp(w.at("rise").get<float>(), -8.0f, 8.0f);
                    widget.text_scale = static_cast<s32>(w.value("text_scale", 3));
                    widget.on_tap = w.value("on_tap", std::string{});
                    widget.on_hold = w.value("on_hold", std::string{});
                    widget.hold_ms = static_cast<s32>(
                        w.contains("hold_ms") ? ParseNumber(w.at("hold_ms")) : 0);
                    widget.on_swipe_left = w.value("on_swipe_left", std::string{});
                    widget.on_swipe_right = w.value("on_swipe_right", std::string{});
                    widget.on_swipe_up = w.value("on_swipe_up", std::string{});
                    widget.on_swipe_down = w.value("on_swipe_down", std::string{});
                    widget.swipe_px = static_cast<s32>(
                        w.contains("swipe_px") ? ParseNumber(w.at("swipe_px")) : 0);
                    widget.id = w.value("id", std::string{});
                    widget.repeat = w.contains("repeat") ? ParseNumber(w.at("repeat")) : 0;
                    widget.repeat_bind = w.value("repeat_bind", std::string{});
                    widget.repeat_div = static_cast<s32>(w.value("repeat_div", 1));
                    widget.repeat_cols = static_cast<s32>(w.value("repeat_cols", 0));
                    widget.repeat_row_dy =
                        w.contains("repeat_row_dy") ? ParseCoordinate(w.at("repeat_row_dy")) : 0;
                    widget.pack = w.value("pack", false);
                    if (w.contains("scroll")) {
                        // A region id, or an inline region definition (first one wins per id).
                        const auto& sv = w.at("scroll");
                        if (sv.is_string()) {
                            widget.scroll = sv.get<std::string>();
                        } else if (auto region = ParseScrollRegion(sv)) {
                            widget.scroll = region->id;
                            if (FindScrollRegion(page, region->id) == nullptr) {
                                page.scrolls.push_back(std::move(*region));
                            }
                        }
                    }
                    widget.repeat_dx =
                        w.contains("repeat_dx") ? ParseCoordinate(w.at("repeat_dx")) : 0;
                    widget.repeat_dy =
                        w.contains("repeat_dy") ? ParseCoordinate(w.at("repeat_dy")) : 0;
                    widget.pan_zoom = w.value("pan_zoom", false);
                    widget.min_zoom = w.value("min_zoom", 1.0f);
                    widget.max_zoom = w.value("max_zoom", 8.0f);
                    // Bound default view (unreleased runtime 15 addition, mod_view_default.h):
                    // a non-map pan_zoom widget only (a map has view_rect_*_bind).
                    if (w.contains("view_zoom_bind") || w.contains("view_cx_bind") ||
                        w.contains("view_cy_bind")) {
                        ViewDefaultBinds vd;
                        vd.zoom_bind = w.value("view_zoom_bind", std::string{});
                        vd.cx_bind = w.value("view_cx_bind", std::string{});
                        vd.cy_bind = w.value("view_cy_bind", std::string{});
                        vd.reset_bind = w.value("view_reset_bind", std::string{});
                        if (!widget.pan_zoom || widget.type == WidgetType::Map ||
                            vd.zoom_bind.empty() || vd.cx_bind.empty() || vd.cy_bind.empty()) {
                            LOG_WARNING(Core,
                                        "DSMod: widget '{}': view_zoom_bind / view_cx_bind / "
                                        "view_cy_bind need all three on a non-map pan_zoom "
                                        "widget; ignored",
                                        widget.id);
                        } else {
                            widget.view_default =
                                std::make_shared<const ViewDefaultBinds>(std::move(vd));
                        }
                    }
                    // Tap-select / drag-and-drop.
                    if (w.contains("payload")) {
                        const auto& pv = w.at("payload");
                        if (pv.is_string()) {
                            widget.payload = pv.get<std::string>();
                        } else if (pv.is_number_integer()) {
                            widget.payload = std::to_string(pv.get<s64>());
                        } else if (pv.is_number()) {
                            widget.payload = std::to_string(static_cast<s64>(pv.get<f64>()));
                        }
                    }
                    widget.select_group = w.value("select_group", std::string{});
                    widget.draggable = w.value("draggable", false);
                    widget.drag_scale = static_cast<float>(w.value("drag_scale", 1.15));
                    widget.drop_action = w.value("drop_action", std::string{});
                    widget.accept_group = w.value("accept_group", std::string{});
                    widget.highlight_src = w.value("highlight_src", std::string{});
                    widget.highlight_color = ParseColor(w, "highlight_color", 0u);
                    // Drag ghost underlay.
                    widget.drag_under_src = w.value("drag_under_src", std::string{});
                    if (w.contains("drag_under_src_rect") &&
                        w.at("drag_under_src_rect").is_array() &&
                        w.at("drag_under_src_rect").size() == 4) {
                        for (size_t i = 0; i < 4; ++i) {
                            widget.drag_under_src_rect[i] =
                                static_cast<float>(w.at("drag_under_src_rect")[i].get<double>());
                        }
                    }
                    if (w.contains("drag_under_rect") && w.at("drag_under_rect").is_array() &&
                        w.at("drag_under_rect").size() == 4) {
                        for (size_t i = 0; i < 4; ++i) {
                            widget.drag_under_rect[i] =
                                static_cast<s32>(ParseNumber(w.at("drag_under_rect")[i]));
                        }
                    }
                    widget.drag_under_tint = ParseColor(w, "drag_under_tint", 0xFFFFFFFFu);
                    widget.drag_under_color = ParseColor(w, "drag_under_color", 0u);
                    // Text by key and multi-line labels.
                    widget.text_src = w.value("text_src", std::string{});
                    widget.text_bind = w.value("text_bind", std::string{});
                    if (w.contains("text_map") && w.at("text_map").is_object()) {
                        auto map = std::make_shared<std::unordered_map<s64, std::string>>();
                        for (const auto& [key, ref] : w.at("text_map").items()) {
                            const bool hex = key.starts_with("0x") || key.starts_with("-0x");
                            char* end = nullptr;
                            const long long value = std::strtoll(key.c_str(), &end, hex ? 16 : 10);
                            if (!ref.is_string() || end == key.c_str() || *end != '\0') {
                                LOG_WARNING(Core, "DSMod: text_map entry '{}' ignored", key);
                                continue;
                            }
                            map->emplace(static_cast<s64>(value), ref.get<std::string>());
                        }
                        widget.text_map = std::move(map);
                    }
                    widget.wrap_width = static_cast<s32>(w.value("wrap_width", 0));
                    widget.max_lines = static_cast<s32>(w.value("max_lines", 0));
                    widget.line_gap = static_cast<s32>(w.value("line_gap", -1));
                    widget.icon_silhouette =
                        w.value("icon_style", std::string{"color"}) == "silhouette";
                    widget.color_markup = w.value("color_markup", false);
                    widget.outline_copy = w.value("outline_copy", false);
                    // Runtime 17: text-sized Label/Button boxes and grouped Value numbers.
                    const auto r17_flag = [&w](const char* key) {
                        return w.contains(key) && w.at(key).is_boolean() && w.at(key).get<bool>();
                    };
                    widget.auto_w = r17_flag("auto_w");
                    widget.fit_text = r17_flag("fit_text");
                    widget.text_min_scale =
                        std::clamp(static_cast<s32>(w.value("text_min_scale", 1)), 1, 128);
                    widget.text_center_h =
                        std::clamp(static_cast<s32>(w.value("text_center_h", 0)), 0, 16384);
                    widget.group = r17_flag("group");
                    if (w.contains("group_sep") && w.at("group_sep").is_string()) {
                        widget.group_sep = w.at("group_sep").get<std::string>();
                    }
                    widget.tap_block = w.value("tap_block", false);
                    // A tap-only block let a drag on a panel pan the map underneath; both keys
                    // now own the whole gesture.
                    widget.input_block = widget.tap_block || (w.contains("input_block") &&
                                                              w.at("input_block").is_boolean() &&
                                                              w.at("input_block").get<bool>());
                    widget.haptic = ParseHapticOverride(w);
                    widget.anim = ParseWidgetAnim(w);
                    ParseImageStyleKeys(w, widget); // runtime 16
                    if (widget.type == WidgetType::Chart) {
                        widget.chart = ParseChartSpec(w, widget, page.id, page.widgets.size());
                    }
                    if (widget.type == WidgetType::Map) {
                        widget.map_extras = ParseMapWidgetExtras(w);
                    }
                    page.widgets.push_back(std::move(widget));
                }
            }
            if (page_json.contains("mirror") && page_json.at("mirror").is_array() &&
                page_json.at("mirror").size() == 4) {
                page.mirror = true;
                for (size_t i = 0; i < 4; ++i) {
                    page.mirror_rect[i] =
                        static_cast<float>(page_json.at("mirror")[i].get<double>());
                }
            }
            page.no_auto_leave = page_json.value("no_auto_leave", false);
            if (page_json.contains("nav_order") && page_json.at("nav_order").is_array()) {
                for (const auto& id : page_json.at("nav_order")) {
                    if (id.is_string()) {
                        page.nav_order.push_back(id.get<std::string>());
                    }
                }
            }
            manifest.pages.push_back(std::move(page));
        }
    }

    if (json.contains("il2cpp")) {
        const auto& layout = json.at("il2cpp");
        if (layout.contains("name")) {
            manifest.il2cpp.name = ParseNumber(layout.at("name"));
        }
        if (layout.contains("methods")) {
            manifest.il2cpp.methods = ParseNumber(layout.at("methods"));
        }
        if (layout.contains("method_count")) {
            manifest.il2cpp.method_count = ParseNumber(layout.at("method_count"));
        }
        LOG_INFO(Core, "DSMod: Il2CppClass layout name={:X} methods={:X} count={:X}",
                 manifest.il2cpp.name, manifest.il2cpp.methods, manifest.il2cpp.method_count);
    }
    if (json.contains("enforce_gate")) {
        manifest.enforce_gate = json.at("enforce_gate").value("point", std::string{});
        manifest.enforce_gate_max = json.at("enforce_gate").value("max", 64);
    }
    // Which point name means "a real game session is active" for InGameplayHonest() (the baseline
    // every RE search tool gates on). Default "energy" -- unset, this is byte-for-byte the old
    // hardcoded behaviour.
    if (json.contains("gameplay_point")) {
        manifest.gameplay_point = json.at("gameplay_point").get<std::string>();
    }
    if (json.contains("map")) {
        const auto& map_json = json.at("map");
        if (map_json.contains("zone_area")) {
            for (const auto& [zone, area] : map_json.at("zone_area").items()) {
                manifest.zone_area[std::stoll(zone)] = area.get<std::string>();
            }
        }
        if (map_json.contains("rooms")) {
            for (const auto& [name, room] : map_json.at("rooms").items()) {
                MapRoom entry;
                entry.area = room.value("area", std::string{});
                entry.sprite = room.value("sprite", std::string{});
                entry.x = room.value("x", 0.0f);
                entry.y = room.value("y", 0.0f);
                entry.w = room.value("w", 0.0f);
                entry.h = room.value("h", 0.0f);
                manifest.map_rooms.emplace(name, entry);
            }
        }
        LOG_INFO(Core, "DSMod: map has {} rooms across {} zones", manifest.map_rooms.size(),
                 manifest.zone_area.size());
    }
    if (json.contains("map") &&
        (json.at("map").contains("areas") || json.at("map").contains("areas_src"))) {
        const auto& mj = json.at("map");
        manifest.icon_atlas = mj.value("atlas", std::string{});
        // Runtime 12: the module generates the areas (asset-free package); the inline "areas"
        // are the authored template shown until the module's data is installed.
        manifest.map_areas_src = mj.value("areas_src", std::string{});
        manifest.icon_cell = static_cast<s32>(mj.value("cell", 0));
        // map.style overrides the renderer palette / sizes; anything omitted keeps its default.
        if (mj.contains("style")) {
            const auto& st = mj.at("style");
            auto& s = manifest.map_style;
            s.opacity = std::clamp(st.value("opacity", s.opacity), 0.0f, 1.0f);
            s.room_fill = ParseColor(st, "room_fill", s.room_fill);
            if (st.contains("room_class_color")) {
                s.room_class_color = ParseColor(st, "room_class_color", s.room_fill);
            }
            s.border = ParseColor(st, "border", s.border);
            s.revealed_dim = static_cast<int>(st.value("revealed_dim", s.revealed_dim));
            s.visited_gain = static_cast<s32>(st.value("visited_gain", s.visited_gain));
            s.visited_tint = ParseColor(st, "visited_tint", s.visited_tint);
            s.visited_amount = static_cast<int>(st.value("visited_amount", s.visited_amount));
            s.hazard_tint = static_cast<int>(st.value("hazard_tint", s.hazard_tint));
            s.water_tint = static_cast<int>(st.value("water_tint", s.water_tint));
            s.fade_ticks = static_cast<u32>(st.value("fade_ticks", s.fade_ticks));
            s.item_icon = static_cast<s32>(st.value("item_icon", s.item_icon));
            s.door_icon = static_cast<s32>(st.value("door_icon", s.door_icon));
            s.marker_icon = static_cast<s32>(st.value("marker_icon", s.marker_icon));
            s.raster_px =
                std::clamp(static_cast<s32>(st.value("raster_px", s.raster_px)), 256, 8192);
            s.reveal_required = st.value("reveal_required", s.reveal_required);
            s.wall_color = ParseColor(st, "wall_color", s.wall_color);
            // station_color/transport_color/zone_active_color/zone_inactive_color/
            // zone_class_scale/zone_color_gain moved to per-category fields on
            // map.areas.<a>.room_categories[].
            s.category_dim_before_unlock =
                st.value("category_dim_before_unlock", s.category_dim_before_unlock);
            s.hazard_gain = st.value("hazard_gain", s.hazard_gain);
            s.water_gain = st.value("water_gain", s.water_gain);
            s.water_clip_live = st.value("water_clip_live", s.water_clip_live);
            s.water_full_visible = st.value("water_full_visible", s.water_full_visible);
            s.occluder_color = ParseColor(st, "occluder_color", s.occluder_color);
            s.grid_tile_world_size =
                std::max(1.0f, st.value("grid_tile_world_size", s.grid_tile_world_size));
            if (st.contains("marker_colors") && st.at("marker_colors").is_array()) {
                const auto& colors = st.at("marker_colors");
                for (size_t i = 0; i < s.marker_colors.size() && i < colors.size(); ++i) {
                    nlohmann::json value;
                    value["color"] = colors[i];
                    s.marker_colors[i] = ParseColor(value, "color", s.marker_colors[i]);
                }
            }
            s.marker_back_color = ParseColor(st, "marker_back_color", s.marker_back_color);
            s.marker_glyph_color = ParseColor(st, "marker_glyph_color", s.marker_glyph_color);
            s.marker_pulse_color = ParseColor(st, "marker_pulse_color", s.marker_pulse_color);
            s.marker_pulse_gain =
                std::max(0.0f, st.value("marker_pulse_gain", s.marker_pulse_gain));
            // Formerly the two hardcoded literals at the pulse composite call site
            // (GeometryMapDraw::DrawBaseLayer, mod_ui_map_widget.cpp)
            // -- see MapStyle's own doc comment.
            s.marker_pulse_peak =
                std::clamp(st.value("marker_pulse_peak", s.marker_pulse_peak), 0.0f, 1.0f);
            s.marker_pulse_period = std::max<u32>(
                2u, static_cast<u32>(st.value("marker_pulse_period", s.marker_pulse_period)));
            s.custom_marker_icon = st.value("custom_marker_icon", s.custom_marker_icon);
            s.custom_marker_back_icon =
                st.value("custom_marker_back_icon", s.custom_marker_back_icon);
            s.door_prefix = st.value("door_prefix", s.door_prefix);
            if (st.contains("structural_prefixes") && st.at("structural_prefixes").is_array()) {
                s.structural_prefixes.clear();
                for (const auto& prefix : st.at("structural_prefixes")) {
                    if (prefix.is_string()) {
                        s.structural_prefixes.push_back(prefix.get<std::string>());
                    }
                }
            }
            s.door_closed_suffix = st.value("door_closed_suffix", s.door_closed_suffix);
            s.door_open_suffix = st.value("door_open_suffix", s.door_open_suffix);
            s.door_opened_left = st.value("door_opened_left", s.door_opened_left);
            s.door_opened_right = st.value("door_opened_right", s.door_opened_right);
            s.collected_suffix = st.value("collected_suffix", s.collected_suffix);
            s.collected_fallback = st.value("collected_fallback", s.collected_fallback);
            s.collectible_kind = st.value("collectible_kind", s.collectible_kind);
            s.item_blink_period = std::max<u32>(
                2u, static_cast<u32>(st.value("item_blink_period", s.item_blink_period)));
            s.item_blink_low = std::clamp(st.value("item_blink_low", s.item_blink_low), 0.0f, 1.0f);
            s.player_blink_period = std::max<u32>(
                2u, static_cast<u32>(st.value("player_blink_period", s.player_blink_period)));
            s.player_blink_low =
                std::clamp(st.value("player_blink_low", s.player_blink_low), 0.0f, 1.0f);
            s.pin_outer = std::clamp(static_cast<s32>(st.value("pin_outer", s.pin_outer)), 0, 256);
            s.pin_inner = std::clamp(static_cast<s32>(st.value("pin_inner", s.pin_inner)), 0, 256);
            s.pin_core = std::clamp(static_cast<s32>(st.value("pin_core", s.pin_core)), 0, 256);
        }
        if (mj.contains("icons")) {
            for (const auto& [name, cell] : mj.at("icons").items()) {
                manifest.icon_cells[name] = {static_cast<s32>(cell.value("r", 0)),
                                             static_cast<s32>(cell.value("c", 0))};
                if (cell.contains("ink") && cell.at("ink").size() == 4) {
                    const auto& ink = cell.at("ink");
                    manifest.icon_ink[name] = {
                        static_cast<s32>(ink[0].get<int>()), static_cast<s32>(ink[1].get<int>()),
                        static_cast<s32>(ink[2].get<int>()), static_cast<s32>(ink[3].get<int>())};
                }
            }
        }
        if (mj.contains("areas")) {
            ParseMapAreasInto(mj.at("areas"), manifest.map_areas);
        }
        LOG_INFO(Core, "DSMod: geometry map with {} areas, {} icon cells",
                 manifest.map_areas.size(), manifest.icon_cells.size());
    }
    if (json.contains("sprite_map")) {
        for (const auto& [key, value] : json.at("sprite_map").items()) {
            manifest.sprite_map[key] = value.get<std::string>();
        }
    }
    if (json.contains("flags")) {
        for (const auto& [name, value] : json.at("flags").items()) {
            // Flags are ints: true/false stay 1/0, a number is a multi-state value.
            manifest.flag_defaults[name] =
                value.is_boolean() ? (value.get<bool>() ? 1 : 0) : ParseNumber(value);
        }
    }
    if (json.contains("persist_flags")) {
        // Runtime flags kept across sessions (mod_persist.h): restored at load over "flags".
        const auto& list = json.at("persist_flags");
        if (!list.is_array()) {
            LOG_WARNING(Core, "DSMod: \"persist_flags\" must be an array of flag names");
        } else {
            for (const auto& entry : list) {
                if (!entry.is_string() || entry.get<std::string>().empty()) {
                    LOG_WARNING(Core, "DSMod: \"persist_flags\" entry {} is not a flag name",
                                entry.dump());
                    continue;
                }
                auto name = entry.get<std::string>();
                if (std::find(manifest.persist_flags.begin(), manifest.persist_flags.end(),
                              name) == manifest.persist_flags.end()) {
                    manifest.persist_flags.push_back(std::move(name));
                }
            }
        }
    }
    if (json.contains("derived")) {
        ParseDerived(json.at("derived"), manifest.derived);
    }
    if (json.contains("enforce")) {
        for (const auto& e : json.at("enforce")) {
            EnforceRule rule;
            rule.action = e.value("action", std::string{});
            rule.every_ms = e.value("every_ms", 500u);
            rule.flag = e.value("flag", std::string{});
            rule.flag_value = e.value("value", true);
            if (!rule.action.empty()) {
                manifest.enforce.push_back(std::move(rule));
            }
        }
    }
    if (json.contains("sequences")) {
        for (const auto& [name, sj] : json.at("sequences").items()) {
            CallSequence sequence;
            sequence.every_ms = sj.value("every_ms", 0u);
            sequence.flag = sj.value("flag", std::string{});
            sequence.out = sj.value("out", std::string{});
            for (const auto& st : sj.at("steps")) {
                CallStep step;
                step.fn = st.value("fn", std::string{});
                step.ret_float = st.value("float", false);
                step.save = st.value("save", std::string{});
                step.out = st.value("out", std::string{});
                step.out_addr = st.value("out_addr", std::string{});
                step.out_text = st.value("out_text", std::string{});
                if (st.contains("reject")) {
                    step.has_reject = true;
                    step.reject = ParseNumber(st.at("reject"));
                    step.else_step = st.value("else_step", -1);
                }
                if (st.contains("expect")) {
                    step.has_expect = true;
                    step.expect = ParseNumber(st.at("expect"));
                    step.else_step = st.value("else_step", -1);
                }
                if (st.contains("args")) {
                    for (const auto& a : st.at("args")) {
                        step.args.push_back(a.is_string() ? a.get<std::string>()
                                                          : std::to_string(ParseNumber(a)));
                    }
                }
                sequence.steps.push_back(std::move(step));
            }
            manifest.sequences[name] = std::move(sequence);
        }
        LOG_INFO(Core, "DSMod: {} call sequence(s) declared", manifest.sequences.size());
    }
    if (json.contains("actions")) {
        for (const auto& [name, a] : json.at("actions").items()) {
            Action action;
            action.name = name;
            action.enabled = ParseGate(a, "enabled_bind");
            action.haptic = ParseHapticOverride(a);
            const auto kind = a.value("kind", std::string{});
            // "value": integer, float, "$payload", or "$<name>" (map/marker context, published).
            const auto parse_value = [&action](const nlohmann::json& v) {
                if (v.is_string() && v.get<std::string>() == "$payload") {
                    action.value_from_payload = true;
                    return;
                }
                const ActionValue parsed = ParseActionValue(v);
                action.value = parsed.value;
                action.value_float = parsed.as_float;
                action.value_ref = parsed.ref;
            };
            if (kind == "write") {
                action.kind = ActionKind::Write;
                action.point = a.value("point", std::string{});
                if (a.contains("value")) {
                    parse_value(a.at("value"));
                }
                action.swap_point = a.value("swap_point", std::string{});
            } else if (kind == "slot_write") {
                action.kind = ActionKind::SlotWrite;
                action.slot = a.value("slot", std::string{});
                action.count = a.contains("count") ? ParseNumber(a.at("count")) : 0;
                action.free_value = a.contains("free_value") ? ParseNumber(a.at("free_value")) : 0;
                action.select = a.value("select", std::string{});
                if (a.contains("writes") && a.at("writes").is_array()) {
                    for (const auto& wr : a.at("writes")) {
                        if (wr.is_object() && wr.contains("point") && wr.contains("value")) {
                            action.writes.emplace_back(wr.at("point").get<std::string>(),
                                                       ParseActionValue(wr.at("value")));
                        }
                    }
                }
            } else if (kind == "map_select") {
                action.kind = ActionKind::MapSelect;
                action.group = a.value("group", std::string{});
                action.value = -1;
                if (a.contains("value")) {
                    parse_value(a.at("value"));
                }
            } else if (kind == "button") {
                action.kind = ActionKind::Button;
                action.button = a.value("button", std::string{});
                action.frames = a.value("frames", 4u);
                action.repeat = a.value("repeat", 1u);
                action.button_neg = a.value("button_neg", std::string{});
                action.counter = a.value("counter", std::string{});
                action.target = a.contains("target") ? ParseNumber(a.at("target")) : 0;
                action.modulo = a.contains("modulo") ? ParseNumber(a.at("modulo")) : 0;
                if (a.contains("delta")) {
                    action.delta = ParseNumber(a.at("delta"));
                    action.use_delta = true;
                }
                action.gap = a.value("gap", 18u);
                if (a.contains("hold_map") && a.at("hold_map").is_array()) {
                    for (const auto& h : a.at("hold_map"))
                        action.hold_map.push_back(h.get<u32>());
                }
            } else if (kind == "call") {
                action.kind = ActionKind::Call;
                DataPoint fn_point;
                if (a.contains("fn")) {
                    const auto fn = a.at("fn").get<std::string>();
                    if (!fn.empty() && fn.front() == '$') {
                        action.call_fn_symbol = fn.substr(1);
                    } else {
                        ParseAddressToken(fn, fn_point, true);
                        action.call_fn = fn_point.chain.empty() ? 0 : fn_point.chain.front();
                    }
                }
                if (a.contains("args")) {
                    for (const auto& arg : a.at("args")) {
                        action.args.push_back(arg.is_string() ? arg.get<std::string>()
                                                              : std::to_string(ParseNumber(arg)));
                    }
                }
            } else if (kind == "module") {
                action.kind = ActionKind::Module;
                action.module_action = a.value("action", std::string{});
                if (a.contains("argument") && a.at("argument").is_string() &&
                    a.at("argument").get<std::string>() == "$payload") {
                    action.value_from_payload = true;
                } else {
                    action.value = a.value("argument", s64{0});
                }
            } else if (kind == "sequence") {
                action.kind = ActionKind::Sequence;
                action.sequence = a.value("sequence", std::string{});
            } else if (kind == "page") {
                action.kind = ActionKind::Page;
                action.page = a.value("page", std::string{});
                if (a.contains("transition") && a.at("transition").is_string()) {
                    const auto tr = a.at("transition").get<std::string>();
                    if (tr == "slide_up") {
                        action.transition = PageTransition::SlideUp;
                    } else if (tr == "slide_down") {
                        action.transition = PageTransition::SlideDown;
                    } else if (tr == "grow") {
                        action.transition = PageTransition::Grow;
                    } else if (tr == "shrink") {
                        action.transition = PageTransition::Shrink;
                    } else if (tr == "fade") {
                        action.transition = PageTransition::Fade;
                    } else if (tr != "none") {
                        LOG_WARNING(Core, "DSMod: page transition '{}' not understood", tr);
                    }
                }
                if (a.contains("duration_ms") && a.at("duration_ms").is_number()) {
                    action.duration_ms =
                        static_cast<u32>(std::clamp(a.at("duration_ms").get<f64>(), 0.0, 2000.0));
                }
                action.easing = ParseEasing(a, "easing", Easing::EaseOut);
                if (a.contains("shadow") && a.at("shadow").is_number()) {
                    action.shadow =
                        static_cast<float>(std::clamp(a.at("shadow").get<f64>(), 0.0, 1.0));
                }
                // Grow/Shrink only; harmless on slide_up/slide_down, just unused.
                action.origin = a.value("origin", std::string{});
            } else if (kind == "flag" || kind == "set_value") {
                action.kind = ActionKind::Flag;
                action.flag = a.value("flag", std::string{});
                action.flag_value = -1;
                if (a.contains("value")) {
                    const auto& v = a.at("value");
                    if (v.is_boolean()) {
                        action.flag_value = v.get<bool>() ? 1 : 0;
                    } else {
                        parse_value(v);
                        action.flag_has_int = true;
                        action.flag_int = action.value;
                    }
                }
                action.cycle = a.contains("cycle") ? ParseNumber(a.at("cycle")) : 0;
            } else if (kind == "view_reset") {
                action.kind = ActionKind::ViewReset;
                action.view = a.value("view", std::string{});
            }
            if (action.kind != ActionKind::None) {
                manifest.actions.emplace(name, std::move(action));
            }
        }
    }
    if (json.contains("settings")) {
        // Runtime 17: the built-in "@settings" page (mod_settings.h). Last: it reads the canvas,
        // pages, actions, "flags" and "persist_flags" parsed above.
        ApplySettings(ParseSettings(json.at("settings")), manifest);
    }
}


} // namespace

bool ParseMapAreasJson(const nlohmann::json& areas,
                       std::unordered_map<std::string, MapArea>& out) noexcept {
    try {
        if (!areas.is_object()) {
            return false;
        }
        ParseMapAreasInto(areas, out);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool ParseDualScreenManifest(const nlohmann::json& json, Manifest& out) noexcept {
    try {
        ParseManifestJson(json, out);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool PrepareDualScreenManifestReload(const nlohmann::json& json, const Manifest& current,
                                     Manifest& out) noexcept {
    try {
        if (!json.is_object()) {
            return false;
        }
        if (const auto title = json.find("title_id"); title != json.end()) {
            if (!title->is_string()) {
                return false;
            }
            auto value = title->get<std::string>();
            std::ranges::transform(value, value.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (value != fmt::format("{:016X}", current.title_id)) {
                return false;
            }
        }
        Manifest candidate = current;
        candidate.pages.clear();
        candidate.actions.clear();
        candidate.tables.clear();
        candidate.table_max_len.clear();
        candidate.derived.clear();
        candidate.page_binds.clear();
        ParseManifestJson(json, candidate);
        if (candidate.format != 1 || (!candidate.debug_page && candidate.pages.empty())) {
            return false;
        }
        out = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool IsUsableDualScreenManifest(const nlohmann::json& json) noexcept {
    try {
        Manifest candidate;
        ParseManifestJson(json, candidate);
        // Discover appends this page after parsing, so a debug-only package is usable too.
        return candidate.debug_page || !candidate.pages.empty();
    } catch (const std::exception&) {
        return false;
    }
}

bool NavDefaultOn(const nlohmann::json* manifest, const nlohmann::json* package) noexcept {
    // (A package needing more than this runtime is gated before it is parsed.)
    return PackageMinRuntime(manifest, package) >= 17;
}

u32 PackageMinRuntime(const nlohmann::json* manifest, const nlohmann::json* package) noexcept {
    const auto one = [](const nlohmann::json* json) -> u32 {
        if (json == nullptr || !json->is_object()) {
            return 0;
        }
        const auto it = json->find("min_runtime");
        if (it == json->end() || it->is_null()) {
            return 0;
        }
        if (it->is_number_unsigned()) {
            const auto v = it->get<u64>();
            return v > std::numeric_limits<u32>::max() ? std::numeric_limits<u32>::max()
                                                       : static_cast<u32>(v);
        }
        if (it->is_number_integer()) {
            // Parsed text gives a non-negative number as unsigned; JSON built in code may hold a
            // signed one.
            const s64 v = it->get<s64>();
            return v < 0 ? std::numeric_limits<u32>::max()
                         : static_cast<u32>(std::min<s64>(v, std::numeric_limits<u32>::max()));
        }
        if (it->is_string()) {
            const auto& text = it->get_ref<const std::string&>();
            u64 v = 0;
            if (text.empty() || text.size() > 9) {
                return std::numeric_limits<u32>::max();
            }
            for (const char c : text) {
                if (c < '0' || c > '9') {
                    return std::numeric_limits<u32>::max();
                }
                v = v * 10 + static_cast<u64>(c - '0');
            }
            return static_cast<u32>(v);
        }
        return std::numeric_limits<u32>::max(); // float, object, array, bool: unknown -> gate
    };
    try {
        return std::max(one(manifest), one(package));
    } catch (...) {
        return std::numeric_limits<u32>::max();
    }
}

Manifest ModRuntime::UpdateRequiredManifest(u64 title_id, u32 required,
                                            const std::string& mod_dir) {
    // Built-in page, drawn with the runtime's own font (nothing of the package is loaded: its
    // widgets, font or module may use features this runtime does not have).
    Manifest m;
    m.title_id = title_id;
    m.name = "update required";
    m.mod_dir_name = mod_dir;
    m.canvas_w = 1240;
    m.canvas_h = 1080;
    m.background = 0xFF101014u;
    Page page;
    page.id = "update_required";
    page.title = "UPDATE EDEN DUO";
    const auto label = [&page](std::string id, s32 y, std::string text, s32 scale, u32 color) {
        Widget w;
        w.type = WidgetType::Label;
        w.id = std::move(id);
        w.rect = {620, y, 0, 0};
        w.align = 1;
        w.text = std::move(text);
        w.text_scale = scale;
        w.color = color;
        page.widgets.push_back(std::move(w));
    };
    Widget band;
    band.type = WidgetType::Rect;
    band.id = "update_band";
    band.rect = {0, 300, 1240, 12};
    band.bg = 0xFFE0202Au;
    band.color = 0;
    page.widgets.push_back(std::move(band));
    label("update_title", 360, "UPDATE EDEN DUO", 14, 0xFFFFFFFFu);
    label("update_line1", 500, "This package needs a newer Eden Duo", 7, 0xFFE6ECF2u);
    const std::string runtime =
        required == std::numeric_limits<u32>::max()
            ? fmt::format("runtime ?, have {}", DualScreenRuntimeVersion)
            : fmt::format("runtime {}, have {}", required, DualScreenRuntimeVersion);
    label("update_line2", 580, runtime, 7, 0xFFFFC040u);
    label("update_line3", 660, "Update Eden Duo to use it.", 7, 0xFFE6ECF2u);
    std::string dir = mod_dir.substr(0, 60);
    label("update_package", 800, dir, 4, 0xFF8890A0u);
    m.pages.push_back(std::move(page));
    m.valid = true;
    return m;
}

std::optional<Manifest> ModRuntime::IdleManifest(u64 title_id) {
    // A title with no dual-screen package still owns the second screen: black, with the game's
    // own icon (from its control data) dimmed in the middle -- a quiet placeholder rather than
    // a duplicate of the top screen. The canvas is the panel's own size; the icon sits square
    // in the centre at a third of the shorter side.
    Manifest m;
    m.title_id = title_id;
    m.name = "idle";
    m.mod_dir_name = "(none)";
    m.background = 0xFF000000u;
    Page page;
    page.id = "idle";
    Widget back;
    back.type = WidgetType::Rect;
    back.rect = {0, 0, 0, 0}; // resolved to the whole canvas at draw time (see RenderPage)
    back.bg = 0xFF000000u;
    back.color = 0;
    back.id = "idle_back";
    page.widgets.push_back(std::move(back));
    // Eden Duo "No Companion: Black": the same page, black only.
    if (VideoCore::DSMod::SecondScreenOptions::WithoutCompanion() ==
        VideoCore::DSMod::NoCompanion::Black) {
        m.pages.push_back(std::move(page));
        m.valid = true;
        LOG_INFO(Core, "DSMod: title {:016X} has no package; the second screen stays black",
                 title_id);
        return m;
    }
    Widget icon;
    icon.type = WidgetType::Image;
    icon.src = "icon:";
    icon.rect = {0, 0, 0, 0}; // resolved at draw time: centred square
    icon.color = 0xFF505050u; // dimmed
    icon.id = "idle_icon";
    page.widgets.push_back(std::move(icon));
    m.pages.push_back(std::move(page));
    m.valid = true;
    LOG_INFO(Core, "DSMod: title {:016X} has no package; the second screen shows its icon dimmed",
             title_id);
    return m;
}

std::optional<Manifest> ModRuntime::Discover(System& system, u64 title_id,
                                             const std::array<u8, 0x20>& build_id) {
    auto load_dir = system.GetFileSystemController().GetModificationLoadRoot(title_id);
    if (!load_dir) {
        LOG_INFO(Core, "DSMod: no mod load root for title {:016X}", title_id);
        return std::nullopt;
    }
    const auto& disabled = Settings::values.disabled_addons[title_id];

    auto build_id_full = Common::HexToString(build_id, true);
    while (build_id_full.size() > 16 && build_id_full.back() == '0') {
        build_id_full.pop_back();
    }
    const auto build_id_short = build_id_full.substr(0, std::min<size_t>(16, build_id_full.size()));

    auto subdirs = load_dir->GetSubdirectories();
    std::ranges::sort(subdirs,
                      [](const auto& a, const auto& b) { return a->GetName() < b->GetName(); });
    {
        std::string names;
        for (const auto& subdir : subdirs) {
            names += names.empty() ? "" : ", ";
            names += subdir->GetName();
        }
        LOG_INFO(Core, "DSMod: scanning '{}' for title {:016X}: [{}], {} disabled add-on(s)",
                 load_dir->GetFullPath(), title_id, names, disabled.size());
    }

    for (const auto& subdir : subdirs) {
        if (std::ranges::find(disabled, subdir->GetName()) != disabled.end()) {
            if (subdir->GetSubdirectory("dualscreen")) {
                LOG_INFO(Core, "DSMod: '{}' skipped: turned off in Add-ons", subdir->GetName());
            }
            continue;
        }
        const auto ds_dir = subdir->GetSubdirectory("dualscreen");
        if (!ds_dir) {
            LOG_INFO(Core, "DSMod: '{}' skipped: no dualscreen/ folder", subdir->GetName());
            continue;
        }
        try {
            const auto manifest_json = ReadPackageJson(ds_dir->GetFile("manifest.json"));
            if (!manifest_json) {
                continue;
            }
            if (!manifest_json->is_object()) {
                LOG_WARNING(Core, "DSMod: '{}' uses an unsupported package format",
                            subdir->GetName());
                continue;
            }
            if (manifest_json->contains("title_id")) {
                const auto& id = manifest_json->at("title_id");
                if (!id.is_string()) {
                    LOG_WARNING(Core, "DSMod: '{}' has an invalid title ID", subdir->GetName());
                    continue;
                }
                std::string title = id.get<std::string>();
                std::ranges::transform(title, title.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                if (title != fmt::format("{:016X}", title_id)) {
                    LOG_WARNING(Core, "DSMod: '{}' belongs to a different title",
                                subdir->GetName());
                    continue;
                }
            }
            // Compatibility gate, on the raw JSON and before anything else is interpreted: a
            // package for a newer runtime may use keys, formats or module features this build
            // cannot parse. It is selected (it is the user's package for this game) but replaced
            // by the built-in "update Eden" page.
            const auto package_file = subdir->GetFile("package.json");
            const auto package_json = ReadPackageJson(package_file);
            if (package_file && (!package_json || !package_json->is_object())) {
                LOG_WARNING(Core, "DSMod: malformed package metadata in {}", subdir->GetName());
                continue;
            }
            {
                const u32 required =
                    PackageMinRuntime(&*manifest_json, package_json ? &*package_json : nullptr);
                if (required > DualScreenRuntimeVersion) {
                    LOG_WARNING(
                        Core,
                        "DSMod: '{}' needs dual-screen runtime {} (this Eden has {}): "
                        "This package needs a newer Eden (runtime {}, have {}). Update Eden.",
                        subdir->GetName(), required, DualScreenRuntimeVersion, required,
                        DualScreenRuntimeVersion);
                    return UpdateRequiredManifest(title_id, required, subdir->GetName());
                }
            }
            if (manifest_json->value("format", 1u) != 1u) {
                LOG_WARNING(Core, "DSMod: '{}' uses an unsupported package format",
                            subdir->GetName());
                continue;
            }
            Manifest manifest;
            manifest.title_id = title_id;
            manifest.running_build_id = Common::HexToString(build_id, true);
            manifest.mod_dir_name = subdir->GetName();
            manifest.asset_dir = ds_dir;
            ParseManifestJson(*manifest_json, manifest);
            if (!manifest_json->contains("nav") && package_json &&
                NavDefaultOn(&*manifest_json, &*package_json)) {
                manifest.nav.enabled = true; // min_runtime >= 17 declared in package.json
            }

            // Per-build data file, named like the cheat convention: first 8 bytes of the build id.
            // Cheat files use both cases in the wild (Eden's own loader probes upper then lower),
            // and the VFS compares names exactly even on a case-insensitive filesystem.
            std::string build_id_lower = build_id_short;
            std::ranges::transform(build_id_lower, build_id_lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            {
                std::string listing;
                for (const auto& f : ds_dir->GetFiles()) {
                    listing += f->GetName() + " ";
                }
                LOG_INFO(Core, "DSMod: '{}/dualscreen' contains [{}], looking for '{}.json'",
                         subdir->GetName(), listing, build_id_short);
            }
            for (const auto& candidate :
                 {build_id_short + ".json", build_id_lower + ".json", std::string{"data.json"}}) {
                const auto data_json = ReadPackageJson(ds_dir->GetFile(candidate));
                if (!data_json) {
                    continue;
                }
                manifest.data_file = candidate;
                manifest.uses_clock_keys =
                    manifest.uses_clock_keys || JsonReferencesClockKeys(*data_json);
                if (data_json->contains("points")) {
                    ParsePoints(data_json->at("points"), manifest);
                }
                if (data_json->contains("derived")) {
                    ParseDerived(data_json->at("derived"), manifest.derived);
                }
                // Symbols let a manifest stay version-independent: only this file changes per
                // build.
                if (data_json->contains("map")) {
                    const auto& map_json = data_json->at("map");
                    if (map_json.contains("zone_area")) {
                        for (const auto& [zone, area] : map_json.at("zone_area").items()) {
                            manifest.zone_area[std::stoll(zone)] = area.get<std::string>();
                        }
                    }
                    if (map_json.contains("rooms")) {
                        for (const auto& [name, room] : map_json.at("rooms").items()) {
                            MapRoom entry;
                            entry.area = room.value("area", std::string{});
                            entry.sprite = room.value("sprite", std::string{});
                            entry.x = room.value("x", 0.0f);
                            entry.y = room.value("y", 0.0f);
                            entry.w = room.value("w", 0.0f);
                            entry.h = room.value("h", 0.0f);
                            manifest.map_rooms.emplace(name, entry);
                        }
                    }
                    LOG_INFO(Core, "DSMod: map has {} rooms across {} zones",
                             manifest.map_rooms.size(), manifest.zone_area.size());
                }
                if (data_json->contains("sprite_map")) {
                    for (const auto& [key, value] : data_json->at("sprite_map").items()) {
                        manifest.sprite_map[key] = value.get<std::string>();
                    }
                }
                if (data_json->contains("metadata_anchor")) {
                    DataPoint anchor;
                    ParseAddressToken(data_json->at("metadata_anchor").get<std::string>(), anchor,
                                      true);
                    manifest.metadata_anchor = anchor.chain.empty() ? 0 : anchor.chain.front();
                }
                if (data_json->contains("spies")) {
                    for (const auto& spy : data_json->at("spies")) {
                        SpyPoint point;
                        point.name = spy.value("name", std::string{});
                        point.symbol = spy.value("symbol", std::string{});
                        point.reg = spy.value("reg", 0u);
                        if (!point.name.empty() && !point.symbol.empty()) {
                            manifest.spies.push_back(std::move(point));
                        }
                    }
                    LOG_INFO(Core, "DSMod: {} spy point(s) declared", manifest.spies.size());
                }
                if (data_json->contains("patches")) {
                    for (const auto& pj : data_json->at("patches")) {
                        GuestPatch patch;
                        DataPoint at_point;
                        ParseAddressToken(pj.at("at").get<std::string>(), at_point, true);
                        patch.at = at_point.chain.empty() ? 0 : at_point.chain.front();
                        patch.why = pj.value("why", std::string{});
                        patch.optional = pj.value("optional", false);
                        for (const auto& word : pj.at("write")) {
                            patch.words.push_back(static_cast<u32>(
                                std::strtoull(word.get<std::string>().c_str(), nullptr, 16)));
                        }
                        if (patch.at != 0 && !patch.words.empty()) {
                            manifest.patches.push_back(std::move(patch));
                        }
                    }
                    LOG_INFO(Core, "DSMod: {} code patch(es) declared", manifest.patches.size());
                }
                if (data_json->contains("il2cpp")) {
                    const auto& layout = data_json->at("il2cpp");
                    if (layout.contains("name")) {
                        manifest.il2cpp.name = ParseNumber(layout.at("name"));
                    }
                    if (layout.contains("methods")) {
                        manifest.il2cpp.methods = ParseNumber(layout.at("methods"));
                    }
                    if (layout.contains("method_count")) {
                        manifest.il2cpp.method_count = ParseNumber(layout.at("method_count"));
                    }
                    LOG_INFO(Core, "DSMod: Il2CppClass layout name={:X} methods={:X} count={:X}",
                             manifest.il2cpp.name, manifest.il2cpp.methods,
                             manifest.il2cpp.method_count);
                }
                if (data_json->contains("symbols")) {
                    for (const auto& [name, value] : data_json->at("symbols").items()) {
                        Manifest::Symbol symbol;
                        if (value.is_string()) {
                            DataPoint sym;
                            ParseAddressToken(value.get<std::string>(), sym, true);
                            symbol.address = sym.chain.empty() ? 0 : sym.chain.front();
                        } else if (value.is_object()) {
                            if (value.contains("class")) {
                                DataPoint cls;
                                ParseAddressToken(value.at("class").get<std::string>(), cls, true);
                                symbol.class_slot = cls.chain.empty() ? 0 : cls.chain.front();
                            }
                            symbol.method = value.value("method", std::string{});
                            if (value.contains("find")) {
                                symbol.find = ParsePattern(value.at("find"));
                            }
                            symbol.class_name = value.value("class_name", std::string{});
                            if (value.contains("address")) {
                                DataPoint addr;
                                ParseAddressToken(value.at("address").get<std::string>(), addr,
                                                  true);
                                symbol.address = addr.chain.empty() ? 0 : addr.chain.front();
                            }
                        }
                        manifest.symbols[name] = std::move(symbol);
                    }
                }
                manifest.build_id_file = candidate;
                break;
            }

            if (manifest.debug_page) {
                Page debug;
                debug.id = "__debug";
                debug.title = "OFFSET DEBUG";
                manifest.pages.push_back(std::move(debug));
            }
            manifest.valid = !manifest.pages.empty();
            if (!manifest.valid) {
                LOG_WARNING(Core, "DSMod: '{}' has no pages, ignoring", manifest.mod_dir_name);
                continue;
            }
            LOG_INFO(Core,
                     "DSMod: loaded '{}' ({} pages, {} points, data file '{}') for title {:016X}",
                     manifest.mod_dir_name, manifest.pages.size(), manifest.points.size(),
                     manifest.build_id_file.empty() ? "none" : manifest.build_id_file, title_id);
            return manifest;
        } catch (const std::exception& error) {
            LOG_WARNING(Core, "DSMod: ignoring invalid package '{}': {}", subdir->GetName(),
                        error.what());
        }
    }
    LOG_INFO(Core, "DSMod: title {:016X} has no dual-screen package ({} mod dirs scanned)",
             title_id, subdirs.size());
    return std::nullopt;
}

#if EDEN_DSMOD_BUILD_DEV_TOOLS
/// Console "reload" (dev-tools builds only).
void ModRuntime::ReloadManifest() {
    std::scoped_lock bridge_lock{guest_bridge_mutex};
    if (system.GetDualScreenGuestMailbox().second != 0) {
        LOG_WARNING(Core,
                    "DSMod: restart the game to replace a package with installed guest hooks");
        return;
    }
    if (call_state != CallState::Idle || !spy_armed.empty()) {
        LOG_WARNING(Core, "DSMod reload skipped: guest bridge is active; retry after it completes");
        return;
    }
    if (!manifest.asset_dir) {
        LOG_WARNING(Core, "DSMod reload: no asset dir");
        return;
    }
    const auto mj = ReadPackageJson(manifest.asset_dir->GetFile("manifest.json"));
    if (!mj || !mj->is_object()) {
        // Nothing may change then: the data file's "derived" would otherwise be appended to the
        // old list, and every cache below would be dropped for a package that did not reload.
        LOG_WARNING(Core, "DSMod reload skipped: manifest.json does not parse");
        return;
    }
    const auto package_parent = manifest.asset_dir->GetParentDirectory();
    const auto package_file = package_parent ? package_parent->GetFile("package.json") : nullptr;
    const auto package_json = ReadPackageJson(package_file);
    if (package_file && (!package_json || !package_json->is_object())) {
        LOG_WARNING(Core, "DSMod reload skipped: invalid package.json");
        return;
    }
    if (const u32 required = PackageMinRuntime(&*mj, package_json ? &*package_json : nullptr);
        required > DualScreenRuntimeVersion) {
        LOG_WARNING(Core, "DSMod reload skipped: package needs runtime {} (have {}); restart",
                    required, DualScreenRuntimeVersion);
        return;
    }
    Manifest replacement;
    if (!PrepareDualScreenManifestReload(*mj, manifest, replacement)) {
        LOG_WARNING(Core, "DSMod reload skipped: invalid or unusable manifest.json");
        return;
    }
    // Validate the complete replacement before stopping a worker or shutting down the module.
    StopRedrawWorker();
    {
        ShutdownGameModule();
        manifest = std::move(replacement);
        derived_order_list = nullptr;
        shared_page.reset();
        if (!mj->contains("nav")) {
            // As Discover: a min_runtime >= 17 in package.json (next to dualscreen/) turns the
            // default navigation on too.
            const auto parent = manifest.asset_dir->GetParentDirectory();
            const auto pj = parent ? ReadPackageJson(parent->GetFile("package.json")) : std::nullopt;
            if (pj && NavDefaultOn(&*mj, &*pj)) {
                manifest.nav.enabled = true;
            }
        }
    }
    if (!manifest.data_file.empty()) {
        if (const auto dj = ReadPackageJson(manifest.asset_dir->GetFile(manifest.data_file))) {
            manifest.uses_clock_keys = manifest.uses_clock_keys || JsonReferencesClockKeys(*dj);
            if (dj->contains("points")) {
                manifest.points.clear();
                ParsePoints(dj->at("points"), manifest);
            }
            if (dj->contains("derived")) {
                ParseDerived(dj->at("derived"), manifest.derived);
            }
            if (dj->contains("sprite_map")) {
                manifest.sprite_map.clear();
                for (const auto& [k, v] : dj->at("sprite_map").items())
                    manifest.sprite_map[k] = v.get<std::string>();
            }
        }
    }
    {
        // This console-"reload"-triggered reset is a tick-thread writer of image_cache outside
        // GetImage's own call chain -- guard it the same way every other image_cache touch is
        // guarded.
        std::scoped_lock ilk{asset_cache_mutex};
        image_cache.clear();
    }
    map_water_solid.clear();
    map_water_pixels.clear();
    image_failed.clear();
    ResetNxAssets();
    canvas.SetFont(nullptr, nullptr);
    canvas.SetFontPages(nullptr);
    font_metrics = {};
    font_ready = false;
    DropFontPages(); // runtime 17 paged font
    font_module_attempts = 0;
    font_retry_tick = 0;
    font_epoch = {};
    font_redecode = false;
    {
        // Same reasoning as above, for the map_state_mutex cluster.
        std::scoped_lock mlk{map_state_mutex};
        map_geometry.clear();
        map_category.clear();
        map_walls.clear();
        map_occ_dead.clear();
        map_vig_dispelled.clear();
        map_door_open.clear();
        map_item_picked.clear();
        map_item_unveiled.clear();
        map_item_veiled.clear();
        map_visited.clear();
        game_vis_areas.clear();
        water_boxes.clear();
        map_border.clear();
        map_unlocked = map_zone_inactive = map_zone_alert = false;
        ++water_gen;
        ++zone_gen;
        ++wall_gen;
        ++marker_gen;
    }
    live_scenario.clear();
    last_in_game = -1;
    {
        // This console-"reload"-triggered reset is a tick-thread writer of the
        // PublishGpuComposite bookkeeping OUTSIDE that function's own call chain -- the same class
        // of hazard already guarded for `image_cache`/`map_state_mutex` above, real for these
        // fields too because PublishGpuComposite can run on the redraw worker.
        std::scoped_lock composite_lock{gpu_composite_mutex};
        last_atlas_key.clear();
        last_atlas_image.reset();
        last_hud_hash_valid = false;
        hud_slot_canvas = nullptr;
        map_pub_current.reset();
        map_pub_previous.reset();
        map_pub_epoch = 0;
        last_map_key.clear();
        last_pulse_key.clear();
        last_pulse_present = false;
        last_map_fade_epoch = std::numeric_limits<u64>::max();
    }
    ui_signature_valid = false;
    widget_sig_page = ~size_t{0}; // a hot-reload can change widget definitions/order outright
    sequence_last_run.clear();
    {
        std::scoped_lock view_lock{view_mutex};
        view_state.clear();
        map_follow_state.clear();
        ++follow_state_epoch;
    }
    if (!manifest.pages.empty() && current_page >= manifest.pages.size())
        current_page = manifest.pages.size() - 1;
    interact_groups_ready = false;
    map_groups_ready = false;
    map_draw_records.clear();
    {
        // Reset both halves of the map_records_mutex-guarded pair together.
        std::scoped_lock rlk{map_records_mutex};
        map_draw_records_published.clear();
        map_records_page = ~size_t{0};
    }
    derived_index.clear();
    derived_held.clear();
    // Caches keyed by the old manifest's objects or its symbols.
    text_scan_cache.clear();
    text_scan_attempt.clear();
    entry_array_cache.clear();
    pattern_cache.clear();
    symbol_cache.clear();
    method_info_cache.clear();
    page_bind_state.clear(); // re-arm cold against the fresh manifest.page_binds
    chart_sampler.Reset();   // runtime 17: chart keys follow the new pages
    ResetInteraction("reload");
    CancelAnimations("reload");
    LOG_INFO(Core, "DSMod reloaded: {} pages, {} points, {} derived", manifest.pages.size(),
             manifest.points.size(), manifest.derived.size());
    InitializeGameModule();
}
#endif

} // namespace Core::Mods
