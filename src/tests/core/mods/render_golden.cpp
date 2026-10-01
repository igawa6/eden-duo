// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// V3 regression net: every page of every published dual-screen package is rendered by the real
// RenderPage into a CPU canvas from a deterministic synthetic snapshot and compared pixel-exactly
// with a checked-in golden PNG (golden/<Package>/<page>.png). Then one displayed value is changed
// and the page is redrawn through the partial path -- RenderPage with a clip per dirty rect, as
// ModRuntime::RenderPageRects does, on top of the previous frame -- and the result must equal a
// full redraw of the new state pixel for pixel; the dirty region of that frame is compared with
// golden/<Package>/<page>.partial.png (a crop). golden/<Package>/render.txt records the inputs
// (snapshot fingerprint, changed key, dirty rects) and output hashes per page. A picture that is
// pixel-identical to one an earlier page of the same package produced is stored once (render.txt
// says "same_as <file>") and checked against it in-process.
//
// Inputs that normally come from the game or a native module are synthetic and deterministic:
//   * Images: a stub ImageProvider answers EVERY key with a generated picture (a hash-coloured
//     4x4 checker with transparent corners, a translucent band and a border). Sizes:
//     "map:<area>@WxH" rasters get exactly WxH, "composite:<name>" gets the packed size of the
//     manifest's own definition, the icon atlas gets its cell grid, anything else 16..79 px
//     square-ish from the key's hash. So no widget takes the missing-image path; image drawing
//     (scaling, src_rect, flips, tint, fill, spin at a fixed tick) is exercised with stand-in
//     pixels.
//   * Font: a synthetic game font (95 ASCII glyphs, proportional advances, descenders, soft
//     alpha edges) is set as the canvas font when the package names one, so the game-font text
//     path (the one every package uses on device) is covered; inline icons have no icon font.
//   * Text keys ("msbt:alias#label"): the stub TextProvider answers with the label's words.
//   * Snapshot: built from the page's own widget binds (SnapshotBuilder): gates opened the way
//     the first widget that uses them wants, counts, values inside names/tables, positions,
//     map areas' markers/dynamic markers/labels opened, flags at their manifest defaults,
//     tick = 600. Nothing is random; hashes are FNV-1a over canonical text.
//
// Regenerate intentionally with EDEN_DSMOD_GOLDEN_UPDATE=1 (see golden_common.h for the other
// environment knobs). A package that is not found is skipped.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"
#include "tests/core/mods/golden_common.h"

using namespace Core::Mods;
using DsmodGolden::Fnv64;
using DsmodGolden::Hex64;
using DsmodGolden::Picture;

namespace {

constexpr u32 DefaultCanvasW = 1240;
constexpr u32 DefaultCanvasH = 1080;
constexpr u64 FixedTick = 600;

u64 KeyHash(std::string_view key) {
    return Fnv64(key);
}

// --- synthetic font -----------------------------------------------------------------------------

struct StubFont {
    Image atlas;
    FontMetrics metrics;
};

/// 95 printable ASCII glyphs in a 16-column grid of 32x48 cells. Cap height 34 design px
/// (line_height, which the canvas scales to text_scale * 5), descenders on gjpqy, proportional
/// advances, a glyph shape from a 5x7 hash bitmap with anti-aliased (half-alpha) edges.
const StubFont& SyntheticFont() {
    static const StubFont font = [] {
        StubFont f;
        constexpr u32 CellW = 32, CellH = 48, Cols = 16, First = 0x20, Count = 95;
        const u32 rows = (Count + Cols - 1) / Cols;
        f.atlas.w = CellW * Cols;
        f.atlas.h = CellH * rows;
        f.atlas.pixels.assign(static_cast<size_t>(f.atlas.w) * f.atlas.h, 0x00FFFFFFu);
        f.metrics.line_height = 34;
        f.metrics.first_codepoint = First;
        f.metrics.ascent = 40;
        for (u32 i = 0; i < Count; ++i) {
            const char c = static_cast<char>(First + i);
            // splitmix64 of the code point: every glyph gets its own bitmap and width.
            u64 h = 0x9E3779B97F4A7C15ull * (i + 1);
            h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
            h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
            h ^= h >> 31;
            const bool desc = c == 'g' || c == 'j' || c == 'p' || c == 'q' || c == 'y';
            const u16 gw = c == ' ' ? 0 : static_cast<u16>(12 + h % 14);
            const u16 gh = c == ' ' ? 0 : static_cast<u16>(desc ? 44 : 34);
            FontGlyph g;
            g.x = static_cast<u16>((i % Cols) * CellW);
            g.y = static_cast<u16>((i / Cols) * CellH);
            g.w = gw;
            g.h = gh;
            g.bearing_x = static_cast<s16>(1 + (h >> 8) % 3);
            g.bearing_y = 34;
            g.advance = static_cast<u16>(c == ' ' ? 12 : gw + 4);
            f.metrics.glyphs.push_back(g);
            // 5x7 bitmap, top row and left column always on so every glyph has ink.
            for (u32 y = 0; y < gh; ++y) {
                for (u32 x = 0; x < gw; ++x) {
                    const u32 bx = x * 5 / std::max<u32>(1, gw);
                    const u32 by = y * 7 / std::max<u32>(1, gh);
                    const bool on = by == 0 || bx == 0 || ((h >> ((by * 5 + bx) % 61)) & 1) != 0;
                    const bool edge = x == 0 || y == 0 || x + 1 == gw || y + 1 == gh;
                    const u32 a = on ? (edge ? 0x80u : 0xFFu) : 0u;
                    f.atlas.pixels[static_cast<size_t>(g.y + y) * f.atlas.w + g.x + x] =
                        (a << 24) | 0x00FFFFFFu;
                }
            }
        }
        return f;
    }();
    return font;
}

// --- stub image and text providers -------------------------------------------------------------

std::shared_ptr<Image> MakeStubImage(std::string_view key, u32 w, u32 h) {
    auto img = std::make_shared<Image>();
    img->w = std::max<u32>(1, w);
    img->h = std::max<u32>(1, h);
    img->pixels.resize(static_cast<size_t>(img->w) * img->h);
    const u64 hash = KeyHash(key);
    const u32 base = 0xFF000000u | static_cast<u32>(hash & 0x00FFFFFFu) | 0x00202020u;
    const u32 dark = 0xFF000000u | ((base >> 1) & 0x007F7F7Fu);
    const u32 band = 0x80000000u | static_cast<u32>((hash >> 24) & 0x00FFFFFFu);
    // A 4x4 checker (axis-aligned, so golden PNGs stay small): a sampling offset of one source
    // texel moves a cell edge.
    const u32 cell_w = std::max<u32>(1, img->w / 4), cell_h = std::max<u32>(1, img->h / 4);
    const u32 corner = std::min(img->w, img->h) / 6;
    for (u32 y = 0; y < img->h; ++y) {
        for (u32 x = 0; x < img->w; ++x) {
            u32 p = (x / cell_w + y / cell_h) % 2 == 0 ? base : dark;
            if (x == 0 || y == 0 || x + 1 == img->w || y + 1 == img->h) {
                p = 0xFFFFFFFFu;
            }
            if (y >= img->h / 3 && y < img->h / 2) {
                p = band;
            }
            // Transparent corners (a rounded sprite on a clear background).
            const u32 dx = std::min(x, img->w - 1 - x), dy = std::min(y, img->h - 1 - y);
            if (dx + dy < corner) {
                p = 0x00000000u;
            }
            img->pixels[static_cast<size_t>(y) * img->w + x] = p;
        }
    }
    return img;
}

struct StubImages {
    const Manifest* manifest{};
    std::unordered_map<std::string, std::shared_ptr<const Image>> cache;
    std::set<std::string> requested;

    std::shared_ptr<const Image> Get(const std::string& key) {
        requested.insert(key);
        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second;
        }
        u32 w = 0, h = 0;
        std::string_view k = key;
        if (k.ends_with("#prev")) {
            k.remove_suffix(5);
        }
        if (k.starts_with("map:")) {
            // map:<area>@<w>x<h>, the geometry raster the runtime would rasterise.
            const auto at = k.rfind('@');
            const auto x = k.rfind('x');
            if (at != std::string_view::npos && x != std::string_view::npos && x > at) {
                w = static_cast<u32>(
                    std::strtoul(std::string{k.substr(at + 1)}.c_str(), nullptr, 10));
                h = static_cast<u32>(
                    std::strtoul(std::string{k.substr(x + 1)}.c_str(), nullptr, 10));
            }
        } else if (k.starts_with("composite:")) {
            const auto c = manifest->composites.find(std::string{k.substr(10)});
            if (c != manifest->composites.end() && c->second) {
                CompositeLevels(*c->second, &w, &h);
            }
        } else if (!manifest->icon_atlas.empty() && k == manifest->icon_atlas &&
                   manifest->icon_cell > 0) {
            s32 rows = 1;
            for (const auto& [id, rc] : manifest->icon_cells) {
                rows = std::max(rows, rc.first + 1);
            }
            w = static_cast<u32>(std::max(1, manifest->icon_cols) * manifest->icon_cell);
            h = static_cast<u32>(rows * manifest->icon_cell);
        }
        if (w == 0 || h == 0 || w > 8192 || h > 8192) {
            const u64 hash = KeyHash(k);
            w = 16 + static_cast<u32>(hash % 64);
            h = 16 + static_cast<u32>((hash >> 16) % 64);
        }
        auto img = MakeStubImage(k, w, h);
        cache.emplace(key, img);
        return img;
    }
};

std::shared_ptr<const std::string> StubText(const std::string& ref) {
    // "msbt:<alias>#<label>" -> the label's words ("Item_Name_03" -> "Item Name 03").
    std::string label = ref.substr(ref.find('#') == std::string::npos ? 0 : ref.find('#') + 1);
    std::ranges::replace(label, '_', ' ');
    return std::make_shared<const std::string>(label.empty() ? std::string{"Text"} : label);
}

// --- synthetic snapshot -------------------------------------------------------------------------

const char* const Words[] = {"Blade", "Crest", "Lance", "Arrow", "Sage",  "Tome",
                             "Ember", "Frost", "Storm", "Shade", "Relic", "Aegis"};

std::string SampleText(std::string_view key) {
    const u64 h = KeyHash(key);
    const size_t n = 1 + h % 3;
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out += (i ? " " : "") + std::string{Words[(h >> (8 * (i + 1))) % std::size(Words)]};
    }
    return out;
}

struct SnapshotBuilder {
    const Manifest& manifest;
    const Page& page;
    StateSnapshot s;

    bool Has(const std::string& key) const {
        return key.empty() || s.ints.contains(key) || s.floats.contains(key) ||
               s.texts.contains(key);
    }
    bool Int(const std::string& key, s64 v) {
        if (Has(key)) {
            return false;
        }
        s.ints[key] = v;
        return true;
    }
    bool Float(const std::string& key, f64 v) {
        if (Has(key)) {
            return false;
        }
        s.floats[key] = v;
        return true;
    }
    bool Text(const std::string& key, std::string v) {
        if (Has(key)) {
            return false;
        }
        s.texts[key] = std::move(v);
        return true;
    }
    bool Gate(const PointGate& g) {
        return !g.point.empty() && Int(g.point, g.negate ? 0 : 1);
    }

    /// The area a Map widget shows with this snapshot (widget.area, else the zone's, else the
    /// first area).
    std::string AreaFor(const Widget& w) const {
        if (!w.area_bind.empty() && !manifest.zone_area.empty()) {
            if (const auto z = manifest.zone_area.find(s.GetInt(w.area_bind, -1));
                z != manifest.zone_area.end()) {
                return z->second;
            }
        }
        if (!w.area.empty()) {
            return w.area;
        }
        std::string first;
        for (const auto& [name, a] : manifest.map_areas) {
            if (first.empty() || name < first) {
                first = name;
            }
        }
        return first;
    }

    bool OpenArea(const std::string& area_name) {
        const auto it = manifest.map_areas.find(area_name);
        if (it == manifest.map_areas.end()) {
            return false;
        }
        const auto& a = it->second;
        bool added = false;
        for (const auto& m : a.markers) {
            added |= Gate(m.show);
            if (!m.hide.point.empty()) {
                added |= Int(m.hide.point, m.hide.negate ? 1 : 0);
            }
        }
        for (const auto& l : a.labels) {
            added |= Gate(l.show);
            if (!l.hide.point.empty()) {
                added |= Int(l.hide.point, l.hide.negate ? 1 : 0);
            }
        }
        for (const auto& layer : a.layers) {
            if (!layer.color_bind.empty() && layer.color_map && !layer.color_map->empty()) {
                s64 k = std::numeric_limits<s64>::max();
                for (const auto& [v, c] : *layer.color_map) {
                    k = std::min(k, v);
                }
                added |= Int(layer.color_bind, k);
            }
        }
        for (const auto& cat : a.room_categories) {
            if (!cat.color_bind.empty() && cat.color_map && !cat.color_map->empty()) {
                s64 k = std::numeric_limits<s64>::max();
                for (const auto& [v, c] : *cat.color_map) {
                    k = std::min(k, v);
                }
                added |= Int(cat.color_bind, k);
            }
        }
        const f64 sx = a.max_x - a.min_x, sy = a.max_y - a.min_y;
        for (const auto& d : a.dynamic_markers) {
            added |= Gate(d.show);
            if (!d.hide.point.empty()) {
                added |= Int(d.hide.point, d.hide.negate ? 1 : 0);
            }
            const s64 n = std::min<s64>(d.count, 16);
            for (s64 i = 0; i < n; ++i) {
                const auto sub = [i](std::string t) {
                    for (size_t p; (p = t.find("{i}")) != std::string::npos;) {
                        t.replace(p, 3, std::to_string(i));
                    }
                    return t;
                };
                const u64 h = KeyHash(sub(d.x) + "|" + sub(d.y));
                const f64 wx = a.min_x + sx * (0.1 + 0.8 * static_cast<f64>(h % 1000) / 1000.0);
                const f64 wy =
                    a.min_y + sy * (0.1 + 0.8 * static_cast<f64>((h >> 20) % 1000) / 1000.0);
                if (!d.x.empty()) {
                    added |= Float(sub(d.x), d.scale_x != 0 ? (wx - d.offset_x) / d.scale_x : wx);
                }
                if (!d.y.empty()) {
                    added |= Float(sub(d.y), d.scale_y != 0 ? (wy - d.offset_y) / d.scale_y : wy);
                }
                if (!d.kind.empty()) {
                    s64 kind = 1;
                    if (!d.icon_by_kind.empty()) {
                        std::vector<s64> kinds;
                        for (const auto& [k, icon] : d.icon_by_kind) {
                            kinds.push_back(k);
                        }
                        std::ranges::sort(kinds);
                        kind = kinds[static_cast<size_t>(i) % kinds.size()];
                    }
                    if (d.has_hide_kind && kind == d.hide_when_kind) {
                        kind = d.hide_when_kind + 1;
                    }
                    added |= Int(sub(d.kind), kind);
                }
                if (!d.icon_src_bind.empty()) {
                    added |= Text(sub(d.icon_src_bind), "module:stub/marker" + std::to_string(i));
                }
                if (!d.bar_bind.empty()) {
                    added |= Int(sub(d.bar_bind), 10 + static_cast<s64>(h % 90));
                }
                if (!d.bar_max_bind.empty()) {
                    added |= Int(sub(d.bar_max_bind), 100);
                }
                if (!d.dim_bind.empty()) {
                    added |= Int(sub(d.dim_bind), i % 3 == 0 ? 1 : 0);
                }
                if (!d.frame_color_bind.empty()) {
                    added |= Int(sub(d.frame_color_bind), i % 2 == 0 ? 0xFF3060C0 : 0xFFC03030);
                }
                if (!d.tint_bind.empty()) {
                    added |= Int(sub(d.tint_bind), 0);
                }
            }
        }
        return added;
    }

    bool MapWidget(const Widget& w) {
        bool added = false;
        if (!w.area_bind.empty()) {
            s64 zone = 0;
            if (!manifest.zone_area.empty()) {
                zone = std::numeric_limits<s64>::max();
                for (const auto& [z, name] : manifest.zone_area) {
                    zone = std::min(zone, z);
                }
            }
            added |= Int(w.area_bind, zone);
        }
        const std::string area = AreaFor(w);
        if (!w.room_bind.empty()) {
            std::string room;
            for (const auto& [name, r] : manifest.map_rooms) {
                if (r.area == area && (room.empty() || name < room)) {
                    room = name;
                }
            }
            added |= Text(w.room_bind, room);
        }
        const auto a = manifest.map_areas.find(area);
        if (a != manifest.map_areas.end()) {
            const auto& ar = a->second;
            const f64 cx = (ar.min_x + ar.max_x) / 2.0, cy = (ar.min_y + ar.max_y) / 2.0;
            const f64 scale = w.marker_scale != 0.0f ? w.marker_scale : 1.0;
            added |= Float(w.marker_x_bind, cx / scale);
            added |= Float(w.marker_y_bind, cy / scale);
            added |= Float(w.actor_x_bind, (cx + (ar.max_x - ar.min_x) * 0.15) / scale);
            added |= Float(w.actor_y_bind, (cy + (ar.max_y - ar.min_y) * 0.1) / scale);
            if (w.map_extras && w.map_extras->HasViewRect()) {
                const auto& b = w.map_extras->view_rect_binds;
                added |= Float(b[0], ar.min_x);
                added |= Float(b[1], ar.min_y);
                added |= Float(b[2], ar.max_x);
                added |= Float(b[3], ar.max_y);
            }
            added |= OpenArea(area);
        } else {
            added |= Float(w.marker_x_bind, 0.0);
            added |= Float(w.marker_y_bind, 0.0);
        }
        if (w.map_extras) {
            for (const auto& [name, g] : w.map_extras->groups) {
                added |= Gate(g.show);
                if (!g.hide.point.empty()) {
                    added |= Int(g.hide.point, g.hide.negate ? 1 : 0);
                }
            }
            for (const auto& ov : w.map_extras->overlays) {
                added |= Gate(ov.show);
            }
        }
        return added;
    }

    /// Value for a hide_bind so the widget that asks first is shown.
    static s64 KeepValue(const Widget& w) {
        s64 v = 1;
        if (w.keep_min != std::numeric_limits<s64>::min()) {
            v = std::min(w.keep_min, w.keep_max);
        } else if (w.keep_max != std::numeric_limits<s64>::max()) {
            v = std::min<s64>(w.keep_max, 1);
        }
        if (w.hide_eq_on && v == w.hide_eq) {
            v = v + 1 <= w.keep_max ? v + 1 : v - 1;
        }
        return v;
    }

    bool Widget_(const Widget& w) {
        bool added = false;
        if (w.anim) {
            added |= Gate(w.anim->gate);
        }
        if (!w.hide_bind.empty()) {
            added |= Int(w.hide_bind, KeepValue(w));
        }
        added |= Int(w.need_bind, 1);
        if (!w.text_bind.empty()) {
            s64 k = 0;
            if (w.text_map && !w.text_map->empty()) {
                k = std::numeric_limits<s64>::max();
                for (const auto& [v, t] : *w.text_map) {
                    if (v != -1) {
                        k = std::min(k, v);
                    }
                }
            }
            added |= Int(w.text_bind, k);
        }
        if (!w.payload.empty() && w.payload.front() == '$') {
            added |= Int(w.payload.substr(1), 1);
        }
        const u64 h = KeyHash(w.bind);
        if (!w.bind.empty()) {
            s64 v = 1 + static_cast<s64>(h % 99);
            if (!w.names.empty()) {
                v = static_cast<s64>(h % w.names.size());
            } else if (!w.table.empty()) {
                const auto t = manifest.tables.find(w.table);
                v = t == manifest.tables.end() || t->second.empty()
                        ? 0
                        : static_cast<s64>(h % t->second.size());
            } else if (!w.src_names.empty()) {
                v = static_cast<s64>(h % w.src_names.size());
            } else if (!w.src_thresholds.empty()) {
                v = w.src_thresholds[h % w.src_thresholds.size()].first;
            } else if (w.type == WidgetType::Pips) {
                v = 1 + static_cast<s64>(h % 7);
            }
            added |= Int(w.bind, v);
        }
        if (!w.max_bind.empty()) {
            added |= Int(w.max_bind, w.type == WidgetType::Pips ? 8 : 100);
        }
        if (!w.fill_bind.empty()) {
            added |= Int(w.fill_bind, 1 + static_cast<s64>(KeyHash(w.fill_bind) % 99));
        }
        const auto pos = [&](const std::string& key, float scale) {
            if (key.empty()) {
                return false;
            }
            const u64 ph = KeyHash(key);
            return Float(key, std::fabs(scale) >= 50.0f ? 0.1 + static_cast<f64>(ph % 80) / 100.0
                                                        : static_cast<f64>(ph % 40));
        };
        added |= pos(w.x_bind, w.x_scale);
        added |= pos(w.y_bind, w.y_scale);
        if (!w.bind_text.empty()) {
            added |= Text(w.bind_text, SampleText(w.bind_text));
        }
        if (!w.src_bind.empty()) {
            if (manifest.sprite_map.empty()) {
                added |= Text(w.src_bind, "module:stub/" + w.src_bind);
            } else {
                std::vector<std::string> keys;
                for (const auto& [k, v] : manifest.sprite_map) {
                    keys.push_back(k);
                }
                std::ranges::sort(keys);
                added |= Text(w.src_bind, keys[KeyHash(w.src_bind) % keys.size()]);
            }
        }
        if (w.type == WidgetType::Map) {
            added |= MapWidget(w);
        }
        return added;
    }

    StateSnapshot Build() {
        s.tick = FixedTick;
        for (const auto& [name, v] : manifest.flag_defaults) {
            s.ints["@flag:" + name] = v;
        }
        for (const auto& w : page.widgets) {
            if (!w.select_group.empty()) {
                s.ints["@sel:" + w.select_group] = -1;
            }
        }
        for (const auto& region : page.scrolls) {
            s.ints["@scroll_on:" + region.id] = 1;
            Int(region.count_bind, 12);
            Gate(region.show);
            Int(region.reset_bind, 0);
        }
        // Counts first (they decide how many elements the expansion makes), then everything the
        // expanded widgets read, until a pass adds nothing (a gate can reveal new binds).
        for (const auto& w : page.widgets) {
            if (w.repeat > 0 && !w.repeat_bind.empty()) {
                Int(w.repeat_bind, std::min<s64>(w.repeat, 12) * std::max(1, w.repeat_div));
            }
        }
        for (int pass = 0; pass < 6; ++pass) {
            bool added = false;
            for (const auto& w : ExpandWidgets(page, s)) {
                added |= Widget_(w);
            }
            if (!added) {
                break;
            }
        }
        return s;
    }
};

/// Canonical text of a snapshot (sorted), for its fingerprint in render.txt.
std::string SnapshotText(const StateSnapshot& s) {
    std::vector<std::string> lines;
    for (const auto& [k, v] : s.ints) {
        lines.push_back(fmt::format("i {}={}", k, v));
    }
    for (const auto& [k, v] : s.floats) {
        lines.push_back(fmt::format("f {}={}", k, v));
    }
    for (const auto& [k, v] : s.texts) {
        lines.push_back(fmt::format("t {}={}", k, v));
    }
    std::ranges::sort(lines);
    std::string out = fmt::format("tick={}\n", s.tick);
    for (const auto& l : lines) {
        out += l + "\n";
    }
    return out;
}

// --- the value the partial redraw changes --------------------------------------------------------

struct Change {
    std::string key;
    bool text{false};
    s64 old_int{}, new_int{};
    std::string old_text, new_text;
};

/// Every key the page reads for something other than a displayed value (visibility, counts,
/// positions, map state): changing one of those is a layout change, not a value change.
std::set<std::string> StructuralKeys(const Page& page, const std::vector<Widget>& expanded) {
    std::set<std::string> keys;
    const auto add = [&keys](const std::string& k) {
        if (!k.empty()) {
            keys.insert(k);
        }
    };
    for (const auto* list : {&page.widgets, &expanded}) {
        for (const auto& w : *list) {
            add(w.hide_bind);
            add(w.need_bind);
            add(w.repeat_bind);
            add(w.x_bind);
            add(w.y_bind);
            add(w.text_bind);
            add(w.src_bind);
            add(w.area_bind);
            add(w.room_bind);
            add(w.marker_x_bind);
            add(w.marker_y_bind);
            add(w.actor_x_bind);
            add(w.actor_y_bind);
            if (w.anim) {
                add(w.anim->gate.point);
            }
            if (!w.payload.empty() && w.payload.front() == '$') {
                add(w.payload.substr(1));
            }
        }
    }
    for (const auto& r : page.scrolls) {
        add(r.count_bind);
        add(r.show.point);
        add(r.reset_bind);
    }
    return keys;
}

bool OnCanvas(const Widget& w, u32 cw, u32 ch) {
    return w.rect[0] >= 0 && w.rect[1] >= 0 && w.rect[0] < static_cast<s32>(cw) &&
           w.rect[1] < static_cast<s32>(ch);
}

/// Visible, purely displayed values on the page, in preference order: Values' numbers (not
/// through names / a table, no divisor), then Labels' bound texts, then Bars' values. The caller
/// takes the first one whose change actually shows (another widget may cover it).
std::vector<Change> ChangeCandidates(const Page& page, const StateSnapshot& s,
                                     const std::vector<Widget>& expanded, u32 cw, u32 ch) {
    const auto structural = StructuralKeys(page, expanded);
    std::vector<Change> out;
    std::set<std::string> seen;
    const auto usable = [&](const Widget& w, const std::string& key) {
        return !key.empty() && !structural.contains(key) && !seen.contains(key) &&
               !WidgetHidden(w, s) && OnCanvas(w, cw, ch) && w.scroll_clip[2] <= 0;
    };
    for (const auto& w : expanded) {
        if (w.type == WidgetType::Value && w.names.empty() && w.table.empty() && w.div == 1 &&
            usable(w, w.bind) && s.ints.contains(w.bind)) {
            const s64 v = s.ints.at(w.bind);
            out.push_back(Change{w.bind, false, v, v + 1, {}, {}});
            seen.insert(w.bind);
        }
    }
    for (const auto& w : expanded) {
        if (w.type == WidgetType::Label && w.text_src.empty() && w.text_bind.empty() &&
            usable(w, w.bind_text) && s.texts.contains(w.bind_text)) {
            const std::string& t = s.texts.at(w.bind_text);
            out.push_back(Change{w.bind_text, true, 0, 0, t, t + " Lv"});
            seen.insert(w.bind_text);
        }
    }
    for (const auto& w : expanded) {
        if (w.type == WidgetType::Bar && usable(w, w.bind) && s.ints.contains(w.bind)) {
            const s64 v = s.ints.at(w.bind);
            out.push_back(Change{w.bind, false, v, v > 50 ? v - 37 : v + 37, {}, {}});
            seen.insert(w.bind);
        }
    }
    return out;
}

bool Reads(const Widget& w, const std::string& key) {
    return w.bind == key || w.max_bind == key || w.bind_text == key || w.fill_bind == key;
}

// Test-side mirrors of mod_redraw.cpp's WidgetEffectiveRect (expanded widgets) and
// MergeDirtyRects, which live in an anonymous namespace there. Kept identical in behaviour.
std::array<s32, 4> EffectiveRect(const Widget& w, u32 canvas_w, u32 canvas_h,
                                 const StateSnapshot& snapshot, const Manifest& manifest,
                                 const FontMetrics* font) {
    if (w.rect[2] == 0 && w.rect[3] == 0 &&
        (w.type == WidgetType::Rect || w.type == WidgetType::Image)) {
        const s32 cw = static_cast<s32>(canvas_w), ch = static_cast<s32>(canvas_h);
        if (w.type == WidgetType::Image) {
            const s32 side = std::max(64, std::min(cw, ch) / 3);
            const s32 pad = WidgetDrawOverhang(w, side, side);
            return {(cw - side) / 2 - pad, (ch - side) / 2 - pad, side + 2 * pad, side + 2 * pad};
        }
        return {0, 0, cw, ch};
    }
    std::array<s32, 4> r = w.rect;
    if (r[2] == 0 && (w.type == WidgetType::Label || w.type == WidgetType::Value)) {
        const s32 scale = std::max<s32>(1, w.text_scale);
        const s32 span = scale * 5 * 16;
        const s32 h = r[3] > 0 ? r[3] : scale * 9;
        const s32 x = w.align == 2 ? r[0] - span : w.align == 1 ? r[0] - span / 2 : r[0];
        r = {x, r[1] - scale, span, h + 2 * scale};
    }
    if (const s32 pad = WidgetDrawOverhang(w, r[2], r[3]); pad > 0) {
        r = {r[0] - pad, r[1] - pad, r[2] + 2 * pad, r[3] + 2 * pad};
    }
    if (w.type == WidgetType::Label || w.type == WidgetType::Value ||
        w.type == WidgetType::Button || w.type == WidgetType::Pips) {
        const auto t =
            WidgetPaintBounds(w, w.rect[0], w.rect[1], w.rect[2], w.rect[3], snapshot, manifest,
                              font, static_cast<s32>(canvas_w), static_cast<s32>(canvas_h));
        if (t[2] > 0 && t[3] > 0) {
            if (r[2] <= 0 || r[3] <= 0) {
                r = t;
            } else {
                const s32 x0 = std::min(r[0], t[0]), y0 = std::min(r[1], t[1]);
                const s32 x1 = std::max(r[0] + r[2], t[0] + t[2]);
                const s32 y1 = std::max(r[1] + r[3], t[1] + t[3]);
                r = {x0, y0, x1 - x0, y1 - y0};
            }
        }
    }
    return r;
}

void MergeDirtyRects(std::vector<std::array<s32, 4>>& rects, s32 canvas_w, s32 canvas_h) {
    constexpr size_t MaxRects = 4;
    constexpr s32 Gap = 24;
    using Rect = std::array<s32, 4>;
    std::erase_if(rects, [&](Rect& r) {
        const s32 rx0 = std::max(0, r[0]), ry0 = std::max(0, r[1]);
        const s32 rx1 = std::min(canvas_w, r[0] + r[2]), ry1 = std::min(canvas_h, r[1] + r[3]);
        r = {rx0, ry0, rx1 - rx0, ry1 - ry0};
        return r[2] <= 0 || r[3] <= 0;
    });
    const auto bbox = [](const Rect& a, const Rect& b) {
        const s32 bx0 = std::min(a[0], b[0]), by0 = std::min(a[1], b[1]);
        const s32 bx1 = std::max(a[0] + a[2], b[0] + b[2]);
        const s32 by1 = std::max(a[1] + a[3], b[1] + b[3]);
        return Rect{bx0, by0, bx1 - bx0, by1 - by0};
    };
    const auto area = [](const Rect& r) { return static_cast<s64>(r[2]) * r[3]; };
    const auto near = [](const Rect& a, const Rect& b) {
        return a[0] < b[0] + b[2] + Gap && b[0] < a[0] + a[2] + Gap && a[1] < b[1] + b[3] + Gap &&
               b[1] < a[1] + a[3] + Gap;
    };
    for (;;) {
        bool merged = false;
        for (size_t i = 0; i < rects.size() && !merged; ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                if (near(rects[i], rects[j])) {
                    rects[i] = bbox(rects[i], rects[j]);
                    rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
        }
        if (merged) {
            continue;
        }
        if (rects.size() <= MaxRects) {
            break;
        }
        size_t bi = 0, bj = 1;
        s64 best = std::numeric_limits<s64>::max();
        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                const s64 grow = area(bbox(rects[i], rects[j])) - area(rects[i]) - area(rects[j]);
                if (grow < best) {
                    best = grow;
                    bi = i;
                    bj = j;
                }
            }
        }
        rects[bi] = bbox(rects[bi], rects[bj]);
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(bj));
    }
}

// --- rendering ----------------------------------------------------------------------------------

struct Renderer {
    const Manifest& manifest;
    StubImages images;
    u32 cw{DefaultCanvasW}, ch{DefaultCanvasH};

    explicit Renderer(const Manifest& m) : manifest{m} {
        images.manifest = &m;
        if (m.canvas_w != 0 && m.canvas_h != 0) {
            cw = m.canvas_w;
            ch = m.canvas_h;
        }
    }
    const FontMetrics* Font() const {
        return manifest.font_metrics_src.empty() ? nullptr : &SyntheticFont().metrics;
    }
    void Prepare(Canvas& canvas) const {
        canvas.Resize(cw, ch);
        if (Font() != nullptr) {
            canvas.SetFont(&SyntheticFont().atlas, Font());
        }
        canvas.SetIconFont(nullptr, nullptr, false);
    }
    /// One page draw: the full canvas (extras == nullptr) or one clipped pass per rect.
    void Draw(Canvas& canvas, const Page& page, const StateSnapshot& s,
              const std::vector<std::array<s32, 4>>* rects = nullptr) {
        MapFollowState follow;
        MapDrawRecords records;
        const ImageProvider provider = [this](const std::string& src) { return images.Get(src); };
        const TextProvider texts = [](const std::string& ref) { return StubText(ref); };
        const VisitReporter report = [](const std::string&, float, float) {};
        const VisitedQuery visited = [](const std::string&, float, float) { return true; };
        if (rects == nullptr) {
            RenderPage(canvas, manifest, page, s, provider, ViewState{}, &follow, report, visited,
                       nullptr, nullptr, texts, &records, nullptr);
            return;
        }
        RenderExtras extras;
        extras.clips = *rects;
        for (const auto& r : *rects) {
            extras.clip = r;
            bool animating = false;
            RenderPage(canvas, manifest, page, s, provider, ViewState{}, &follow, report, visited,
                       nullptr, &animating, texts, &records, &extras);
        }
    }
};

Picture ToPicture(const Canvas& canvas) {
    return Picture{canvas.Width(), canvas.Height(), canvas.Pixels()};
}

Picture Crop(const Picture& p, const std::array<s32, 4>& r) {
    Picture out{static_cast<u32>(r[2]), static_cast<u32>(r[3]), {}};
    out.argb.reserve(static_cast<size_t>(r[2]) * r[3]);
    for (s32 y = r[1]; y < r[1] + r[3]; ++y) {
        const auto row = p.argb.begin() + static_cast<std::ptrdiff_t>(y) * p.w + r[0];
        out.argb.insert(out.argb.end(), row, row + r[2]);
    }
    return out;
}

u64 PixelHash(const Picture& p) {
    return Fnv64(p.argb.data(), p.argb.size() * sizeof(u32));
}

/// Identity of a picture for de-duplicating golden files (size + pixel hash).
std::string PictureKey(const Picture& p) {
    return fmt::format("{}x{}:{}", p.w, p.h, Hex64(PixelHash(p)));
}

std::string SafeName(const std::string& id) {
    std::string out = id.empty() ? std::string{"page"} : id;
    for (char& c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            c = '_';
        }
    }
    return out;
}

/// Compares (or, in update mode, writes) one golden PNG. Returns "" when fine.
std::string CheckPng(const std::string& package, const std::string& file, const Picture& actual) {
    const auto golden = DsmodGolden::GoldenDir() / package / file;
    if (DsmodGolden::UpdateMode()) {
        return DsmodGolden::WritePng(golden, actual) ? std::string{}
                                                     : "cannot write " + golden.string();
    }
    const auto out = DsmodGolden::ActualDir() / package;
    const auto expected = DsmodGolden::ReadPng(golden);
    if (!expected) {
        DsmodGolden::WritePng(out / file, actual);
        return fmt::format("missing golden {} (actual: {})", golden.string(),
                           (out / file).string());
    }
    std::string diff = DsmodGolden::ComparePictures(*expected, actual);
    if (!diff.empty()) {
        DsmodGolden::WritePng(out / file, actual);
        if (expected->w == actual.w && expected->h == actual.h) {
            DsmodGolden::WritePng(out / (file + ".diff.png"),
                                  DsmodGolden::DiffPicture(*expected, actual));
        }
        diff = fmt::format("{}: {} (actual and diff in {})", golden.string(), diff, out.string());
    }
    return diff;
}

std::string RectsText(const std::vector<std::array<s32, 4>>& rects) {
    std::string out = "[";
    for (size_t i = 0; i < rects.size(); ++i) {
        out += fmt::format("{}{},{},{},{}", i ? " " : "", rects[i][0], rects[i][1], rects[i][2],
                           rects[i][3]);
    }
    return out + "]";
}

void RenderPackage(const std::string& name, const std::filesystem::path& root) {
    const auto raw = DsmodGolden::ReadFile(root / "dualscreen" / "manifest.json");
    REQUIRE(raw.has_value());
    Manifest manifest;
    REQUIRE(ParseDualScreenManifest(nlohmann::json::parse(*raw), manifest));
    REQUIRE_FALSE(manifest.pages.empty());
    Renderer renderer{manifest};
    std::string report = fmt::format(
        "# DSMod V3 render goldens: {} (generated by src/tests/core/mods/render_golden.cpp; do not "
        "edit)\n# canvas {}x{}, font {}, tick {}\n",
        name, renderer.cw, renderer.ch, renderer.Font() ? "synthetic" : "built-in", FixedTick);
    std::vector<std::string> failures;
    std::set<std::string> used_files;
    // picture key -> the golden file holding it and the picture (exact re-check of a dup)
    std::map<std::string, std::pair<std::string, Picture>> stored;
    for (const Page& page : manifest.pages) {
        const std::string id = SafeName(page.id);
        std::string file = id;
        for (int n = 2; used_files.contains(file); ++n) {
            file = fmt::format("{}_{}", id, n);
        }
        used_files.insert(file);
        if (page.mirror) {
            report +=
                fmt::format("page {}: mirror page (the game's own frame), not rendered\n", page.id);
            continue;
        }
        const StateSnapshot a = SnapshotBuilder{manifest, page, {}}.Build();
        const auto expanded = ExpandWidgets(page, a);

        Canvas canvas;
        renderer.Prepare(canvas);
        renderer.Draw(canvas, page, a);
        const Picture full = ToPicture(canvas);
        report += fmt::format("page {}: widgets={} expanded={} snapshot=i{}/f{}/t{} "
                              "snapshot_fnv={} full_fnv={}\n",
                              page.id, page.widgets.size(), expanded.size(), a.ints.size(),
                              a.floats.size(), a.texts.size(), Hex64(Fnv64(SnapshotText(a))),
                              Hex64(PixelHash(full)));
        // A picture identical to one an earlier page of this package produced (P5R's 16 music
        // pages) is stored once: the page records "same_as" and is compared in-process.
        if (const auto dup = stored.find(PictureKey(full)); dup != stored.end()) {
            report += fmt::format("  full: same_as {}\n", dup->second.first);
            if (dup->second.second.argb != full.argb) {
                failures.push_back(fmt::format("{} page {}: hash twin of {} differs", name, page.id,
                                               dup->second.first));
            }
        } else if (auto e = CheckPng(name, file + ".png", full); !e.empty()) {
            failures.push_back(e);
        } else {
            stored.emplace(PictureKey(full), std::make_pair(file + ".png", full));
        }

        // The partial path: one displayed value changes; redraw only its dirty rects over the
        // previous frame; it must equal a full redraw of the new state.
        // The first candidate whose change shows in a full redraw (up to 16 tried).
        std::optional<Change> change;
        StateSnapshot b;
        Picture expect_after;
        size_t tried = 0;
        for (const auto& c : ChangeCandidates(page, a, expanded, renderer.cw, renderer.ch)) {
            if (tried++ == 16) {
                break;
            }
            StateSnapshot next = a;
            if (c.text) {
                next.texts[c.key] = c.new_text;
            } else {
                next.ints[c.key] = c.new_int;
            }
            Canvas after_full;
            renderer.Prepare(after_full);
            renderer.Draw(after_full, page, next);
            Picture pic = ToPicture(after_full);
            if (pic.argb != full.argb) {
                change = c;
                b = std::move(next);
                expect_after = std::move(pic);
                break;
            }
        }
        if (!change) {
            report += fmt::format("  partial: no visible displayed value on this page ({} tried)\n",
                                  tried);
        } else {
            std::vector<std::array<s32, 4>> rects;
            const StateSnapshot* const snaps[] = {&a, &b};
            for (const StateSnapshot* snap : snaps) {
                for (const auto& w : ExpandWidgets(page, *snap)) {
                    if (Reads(w, change->key)) {
                        rects.push_back(EffectiveRect(w, renderer.cw, renderer.ch, *snap, manifest,
                                                      renderer.Font()));
                    }
                }
            }
            MergeDirtyRects(rects, static_cast<s32>(renderer.cw), static_cast<s32>(renderer.ch));
            renderer.Draw(canvas, page, b, &rects); // canvas still holds frame A
            const Picture partial = ToPicture(canvas);
            const std::string vs_full = DsmodGolden::ComparePictures(expect_after, partial);
            if (!vs_full.empty()) {
                DsmodGolden::WritePng(
                    DsmodGolden::ActualDir() / name / (file + ".partial-full.png"), partial);
                failures.push_back(fmt::format("{} page {}: partial redraw != full redraw: {}",
                                               name, page.id, vs_full));
            }
            // The golden crop: the bounding box of the dirty rects in the partially redrawn frame.
            std::array<s32, 4> box{0, 0, 0, 0};
            if (!rects.empty()) {
                s32 x0 = rects[0][0], y0 = rects[0][1], x1 = x0 + rects[0][2],
                    y1 = y0 + rects[0][3];
                for (const auto& r : rects) {
                    x0 = std::min(x0, r[0]);
                    y0 = std::min(y0, r[1]);
                    x1 = std::max(x1, r[0] + r[2]);
                    y1 = std::max(y1, r[1] + r[3]);
                }
                box = {x0, y0, x1 - x0, y1 - y0};
            }
            const std::string old_v =
                change->text ? change->old_text : fmt::format("{}", change->old_int);
            const std::string new_v =
                change->text ? change->new_text : fmt::format("{}", change->new_int);
            size_t changed_px = 0;
            for (size_t i = 0; i < full.argb.size(); ++i) {
                changed_px += full.argb[i] != expect_after.argb[i] ? 1 : 0;
            }
            report +=
                fmt::format("  partial: {} {} -> {} rects={} changed_px={} after_fnv={}\n",
                            change->key, nlohmann::json(old_v).dump(), nlohmann::json(new_v).dump(),
                            RectsText(rects), changed_px, Hex64(PixelHash(expect_after)));
            if (box[2] > 0 && box[3] > 0) {
                const Picture crop = Crop(partial, box);
                const std::string key = PictureKey(crop) + fmt::format("@{},{}", box[0], box[1]);
                if (const auto dup = stored.find(key); dup != stored.end()) {
                    report += fmt::format("  partial: same_as {}\n", dup->second.first);
                    if (dup->second.second.argb != crop.argb) {
                        failures.push_back(fmt::format("{} page {}: hash twin of {} differs", name,
                                                       page.id, dup->second.first));
                    }
                } else if (auto e = CheckPng(name, file + ".partial.png", crop); !e.empty()) {
                    failures.push_back(e);
                } else {
                    stored.emplace(key, std::make_pair(file + ".partial.png", crop));
                }
            }
        }

        // No hidden state: drawing the first snapshot again on a fresh canvas gives frame A.
        Canvas again;
        renderer.Prepare(again);
        renderer.Draw(again, page, a);
        if (const auto e = DsmodGolden::ComparePictures(full, ToPicture(again)); !e.empty()) {
            failures.push_back(
                fmt::format("{} page {}: second full render differs: {}", name, page.id, e));
        }
    }
    report += fmt::format("images_requested = {}\n", renderer.images.requested.size());
    const std::string diff =
        DsmodGolden::CompareText(DsmodGolden::GoldenDir() / name / "render.txt", report,
                                 DsmodGolden::ActualDir() / name / "render.txt");
    if (!diff.empty()) {
        failures.push_back(diff);
    }
    for (const auto& f : failures) {
        UNSCOPED_INFO(f);
    }
    CHECK(failures.empty());
}

} // namespace

TEST_CASE("DSMod golden: every page of every published package renders as recorded",
          "[dsmod][golden][render]") {
    // Catch2 re-runs the case once per section, so decide the skip up front.
    if (!DsmodGolden::AnyPackage()) {
        SKIP("no published package found (set EDEN_DSMOD_GOLDEN_PACKAGE_ROOTS)");
    }
    for (const auto& name : DsmodGolden::PackageNames()) {
        DYNAMIC_SECTION(name) {
            const auto root = DsmodGolden::FindPackage(name);
            if (!root) {
                WARN("package " << name
                                << " not found under EDEN_DSMOD_GOLDEN_PACKAGE_ROOTS; "
                                   "skipped");
                continue;
            }
            RenderPackage(name, *root);
        }
    }
}
