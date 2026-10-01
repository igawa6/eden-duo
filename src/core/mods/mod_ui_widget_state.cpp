// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Per-widget answers shared by drawing, the dirty scan (mod_redraw.cpp) and input (mod_input.cpp):
//   - Values and gates: SnapshotNumber, GateOpen, WidgetHidden, WidgetHiddenHolding (a group
//     sliding out keeps its gated widgets drawn).
//   - Paint bounds: WidgetDrawOverhang (spin / shake), WidgetTextBounds (text past an unsized rect,
//     from the font's real extents), WidgetPaintBounds (text, or a run of pips).
//   - Animation groups: AnimGroupBox, WidgetRectById.
// Pure functions of their arguments (FontBounds keeps a per-thread cache of one font's maxima).

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include "core/mods/mod_ui.h"
#include "core/mods/mod_ui_internal.h"

namespace Core::Mods {

std::optional<f64> SnapshotNumber(const StateSnapshot& snapshot, const std::string& key) {
    if (const auto i = snapshot.ints.find(key); i != snapshot.ints.end()) {
        return static_cast<f64>(i->second);
    }
    if (const auto f = snapshot.floats.find(key); f != snapshot.floats.end()) {
        return f->second;
    }
    return std::nullopt;
}

bool GateOpen(const PointGate& gate, const StateSnapshot& snapshot) {
    const auto v = SnapshotNumber(snapshot, gate.point);
    const bool nonzero = v.has_value() && *v != 0.0;
    return gate.negate ? !nonzero : nonzero;
}

s32 WidgetDrawOverhang(const Widget& widget, s32 rw, s32 rh) {
    if (widget.type != WidgetType::Image) {
        return 0;
    }
    s32 pad = 0;
    if (widget.spin != 0.0f && rw > 0 && rh > 0) {
        // DrawImageRegionRotated covers the rect's bounding circle (radius + 1) around its centre.
        const float hw = static_cast<float>(rw) * 0.5f, hh = static_cast<float>(rh) * 0.5f;
        const float radius = std::sqrt(hw * hw + hh * hh) + 1.0f;
        pad = static_cast<s32>(std::ceil(radius - std::min(hw, hh))) + 1;
    }
    if (widget.shake != 0.0f) {
        const s32 span = std::max(1, static_cast<s32>(std::lround(widget.shake)) * 2 + 1);
        pad += span / 2;
    }
    return pad;
}

namespace {
/// Per-font maxima over every glyph (advance, ascent above the baseline, descent below it, ink
/// past the pen), cached by font identity.
struct FontBounds {
    f32 advance{}, ascent{}, descent{}, overhang{};
};
FontBounds BoundsOf(const FontMetrics& font) {
    struct Cached {
        const FontMetrics* font{};
        size_t glyphs{}, extra{};
        u32 line_height{};
        FontBounds bounds;
    };
    static thread_local Cached cache;
    if (cache.font == &font && cache.glyphs == font.glyphs.size() &&
        cache.extra == font.extra.size() && cache.line_height == font.line_height) {
        return cache.bounds;
    }
    FontBounds b;
    const auto add = [&b](const FontGlyph& g) {
        b.advance = std::max(b.advance, static_cast<f32>(g.advance));
        b.ascent = std::max(b.ascent, static_cast<f32>(g.bearing_y));
        b.descent = std::max(b.descent, static_cast<f32>(g.h) - static_cast<f32>(g.bearing_y));
        b.overhang = std::max({b.overhang, static_cast<f32>(g.bearing_x) + g.w - g.advance,
                               -static_cast<f32>(g.bearing_x)});
    };
    for (const auto& g : font.glyphs) {
        add(g);
    }
    for (const auto& [c, g] : font.extra) {
        add(g);
    }
    cache = {&font, font.glyphs.size(), font.extra.size(), font.line_height, b};
    return b;
}
} // namespace

std::array<s32, 4> WidgetTextBounds(const Widget& widget, s32 x, s32 y, s32 rw, s32 rh,
                                    const StateSnapshot& snapshot, const Manifest& manifest,
                                    const FontMetrics* font, s32 canvas_w, s32 canvas_h) {
    // The text the draw would show (its byte length bounds its glyph count); nullopt = unknown.
    std::optional<size_t> bytes;
    bool block = false;
    if (widget.type == WidgetType::Label) {
        const auto known = [](const std::string& ref) -> std::optional<size_t> {
            if (ref.starts_with("msbt:")) {
                return std::nullopt; // resolved off-thread by the text provider: size unknown
            }
            return ref.size();
        };
        const std::string* literal = nullptr;
        if (!widget.text_bind.empty()) {
            const auto bound = snapshot.ints.find(widget.text_bind);
            bytes = 0; // nothing shown unless a mapped entry exists
            if (bound != snapshot.ints.end() && bound->second != -1 && widget.text_map) {
                if (const auto m = widget.text_map->find(bound->second);
                    m != widget.text_map->end()) {
                    bytes = known(m->second);
                    literal = &m->second;
                }
            }
        } else if (!widget.text_src.empty()) {
            bytes = known(widget.text_src);
            literal = &widget.text_src;
        } else if (!widget.bind_text.empty()) {
            const auto text = snapshot.texts.find(widget.bind_text);
            bytes = text == snapshot.texts.end() ? size_t{1} : text->second.size();
            if (text != snapshot.texts.end()) {
                literal = &text->second;
            }
        } else {
            bytes = widget.text.size();
            literal = &widget.text;
        }
        if (widget.color_markup && bytes.has_value() && literal != nullptr) {
            *bytes -= TextMarkupBytes(*literal); // colour tags draw nothing
        }
        block =
            widget.wrap_width > 0 || literal == nullptr || literal->find('\n') != std::string::npos;
    } else if (widget.type == WidgetType::Button) {
        // The caption (a literal), at the anchor the Button case draws it from.
        if (widget.text.empty()) {
            return {0, 0, 0, 0};
        }
        bytes = widget.text.size();
        if (widget.align == 1) {
            x = x + rw / 2;
            y = y + (rh - widget.text_scale * 5) / 2;
        } else {
            x = x + widget.text_inset;
            y = y + widget.text_inset;
        }
    } else if (widget.type == WidgetType::Value) {
        size_t label = 20; // a formatted s64 (with sign) or a padded one
        if (!widget.table.empty()) {
            label = 0;
            if (const auto known = manifest.table_max_len.find(widget.table);
                known != manifest.table_max_len.end()) {
                label = known->second;
            } else if (const auto tbl = manifest.tables.find(widget.table);
                       tbl != manifest.tables.end()) {
                for (const auto& name : tbl->second) {
                    label = std::max(label, name.size());
                }
            }
        } else if (!widget.names.empty()) {
            label = 0;
            for (const auto& name : widget.names) {
                label = std::max(label, name.size());
            }
            label = std::max<size_t>(label, 20);
        }
        label = std::max<size_t>(label, static_cast<size_t>(std::max(0, widget.pad)));
        bytes = std::min<size_t>(127, widget.text.size() + label + widget.max_sep.size() + 20) +
                widget.suffix.size();
        if (widget.color_markup) {
            *bytes -=
                std::min(*bytes, TextMarkupBytes(widget.text) + TextMarkupBytes(widget.suffix));
        }
    } else {
        return {0, 0, 0, 0};
    }
    const s32 scale = std::max<s32>(1, widget.text_scale);
    const f32 wanted = static_cast<f32>(scale) * 5.0f;
    // Per-glyph advance and vertical ink, the larger of the game font and the built-in one.
    f32 adv = 4.0f * static_cast<f32>(scale);
    f32 above = 1.0f; // built-in glyphs: rows y .. y + 5 * scale
    f32 below = wanted + 1.0f;
    f32 slack = wanted;
    if (font != nullptr && font->line_height > 0) {
        const FontBounds b = BoundsOf(*font);
        const f32 ratio = wanted / static_cast<f32>(font->line_height);
        adv = std::max(adv, b.advance * ratio);
        above = std::max(above, b.ascent * ratio - wanted + 2.0f);
        below = std::max(below, wanted + b.descent * ratio + 2.0f);
        slack = std::max(slack, b.overhang * ratio + 2.0f);
    }
    // Inline icons: sized to the font's ascent, never wider than one em and a half per glyph.
    adv = std::max(adv, wanted * 1.5f);
    const s32 pitch = scale * 5 + (widget.line_gap >= 0 ? widget.line_gap : scale * 3);
    s32 width = 0;
    s32 lines = 1;
    if (!bytes.has_value()) {
        width = canvas_w;
        lines =
            widget.max_lines > 0 ? widget.max_lines : std::max(1, canvas_h / std::max(1, pitch));
    } else if (block) {
        width = widget.wrap_width > 0 ? widget.wrap_width
                                      : static_cast<s32>(std::ceil(adv * static_cast<f32>(*bytes)));
        lines = widget.max_lines > 0 ? widget.max_lines
                                     : static_cast<s32>(std::min<size_t>(*bytes + 1, 4096));
    } else {
        width = static_cast<s32>(std::ceil(adv * static_cast<f32>(*bytes)));
    }
    width = std::min(width, 4 * std::max(canvas_w, 1));
    const s32 pad = static_cast<s32>(std::ceil(slack));
    s32 x0 = x;
    if (widget.type == WidgetType::Button) {
        if (widget.align == 1) {
            x0 = x - width / 2 - 1;
        }
    } else if (widget.align == 1) {
        x0 = x - width / 2 - 1;
    } else if (widget.align == 2) {
        x0 = x - width - 1;
    }
    const s32 top = y - static_cast<s32>(std::ceil(above));
    const s64 bottom = static_cast<s64>(y) + static_cast<s64>(lines - 1) * pitch +
                       static_cast<s64>(std::ceil(below));
    const s32 bottom_c = static_cast<s32>(std::min<s64>(bottom, static_cast<s64>(canvas_h) + 1));
    return {x0 - pad, top, width + 2 * pad + 2, std::max(1, bottom_c - top)};
}

std::array<s32, 4> WidgetPaintBounds(const Widget& widget, s32 x, s32 y, s32 rw, s32 rh,
                                     const StateSnapshot& snapshot, const Manifest& manifest,
                                     const FontMetrics* font, s32 canvas_w, s32 canvas_h) {
    if (widget.type == WidgetType::Pips) {
        // A run of pips starts at the rect and continues right, one per point of the maximum
        // (clamped exactly as the Pips case clamps it): far past a rect sized for one pip.
        const s64 value = widget.bind.empty() ? 0 : snapshot.GetInt(widget.bind);
        const s64 maximum = widget.max_bind.empty()
                                ? widget.max_const
                                : snapshot.GetInt(widget.max_bind, widget.max_const);
        const s64 total = std::clamp<s64>(maximum > 0 ? maximum : value, 0, MaxPips);
        if (total <= 0) {
            return {0, 0, 0, 0};
        }
        // Sprite pips: rect-sized sprites every rect width + gap (8) px.
        const s32 sw = std::max(0, widget.rect[2]), sh = std::max(0, widget.rect[3]);
        const s32 sprite_w =
            static_cast<s32>(total - 1) * (sw + (widget.gap >= 0 ? widget.gap : 8)) + sw;
        // Drawn pips: squares of max(4, rh), every pip + gap (pip / 3) px (from the resolved
        // rect).
        const s32 pip = std::max(4, rh);
        const s32 box_w =
            static_cast<s32>(total) * (pip + (widget.gap >= 0 ? widget.gap : pip / 3));
        const s32 x0 = std::min(x, widget.rect[0]);
        const s32 y0 = std::min(y, widget.rect[1]);
        const s32 x1 = std::max(x + box_w, widget.rect[0] + sprite_w);
        const s32 y1 = std::max(y + pip, widget.rect[1] + sh);
        return {x0, y0, x1 - x0, y1 - y0};
    }
    return WidgetTextBounds(widget, x, y, rw, rh, snapshot, manifest, font, canvas_w, canvas_h);
}

/// Widgets hidden by their hide_bind gate are neither drawn nor tappable; one answer for both.
bool WidgetHidden(const Widget& widget, const StateSnapshot& snapshot) {
    if (widget.anim && !GateOpen(widget.anim->gate, snapshot)) {
        return true;
    }
    if (!widget.need_bind.empty() && snapshot.GetInt(widget.need_bind) == 0) {
        return true;
    }
    if (widget.hide_bind.empty()) {
        return false;
    }
    const s64 hv = snapshot.GetInt(widget.hide_bind);
    return hv < widget.keep_min || hv > widget.keep_max ||
           (widget.hide_eq_on && hv == widget.hide_eq);
}

bool WidgetHiddenHolding(const Widget& widget, const StateSnapshot& snapshot,
                         const std::string& held_point) {
    // The held point reads as it did while the group was open: a need gate on it passes, a hide
    // gate on it does not hide (the group only closes because that point changed).
    if (!widget.need_bind.empty() && widget.need_bind != held_point &&
        snapshot.GetInt(widget.need_bind) == 0) {
        return true;
    }
    if (widget.hide_bind.empty() || widget.hide_bind == held_point) {
        return false;
    }
    const s64 hv = snapshot.GetInt(widget.hide_bind);
    return hv < widget.keep_min || hv > widget.keep_max ||
           (widget.hide_eq_on && hv == widget.hide_eq);
}

std::array<s32, 4> AnimGroupBox(const std::vector<Widget>& widgets, const std::string& key) {
    s32 x0 = std::numeric_limits<s32>::max(), y0 = x0;
    s32 x1 = std::numeric_limits<s32>::min(), y1 = x1;
    for (const auto& w : widgets) {
        if (!w.anim || w.anim->key != key) {
            continue;
        }
        if (w.anim->has_box) {
            return w.anim->box;
        }
        if (w.rect[2] <= 0 || w.rect[3] <= 0) {
            continue;
        }
        x0 = std::min(x0, w.rect[0]);
        y0 = std::min(y0, w.rect[1]);
        x1 = std::max(x1, w.rect[0] + w.rect[2]);
        y1 = std::max(y1, w.rect[1] + w.rect[3]);
    }
    if (x1 <= x0 || y1 <= y0) {
        return {0, 0, 0, 0};
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

std::optional<std::array<s32, 4>> WidgetRectById(const std::vector<Widget>& widgets,
                                                 const std::string& id) {
    if (id.empty()) {
        return std::nullopt;
    }
    for (const auto& w : widgets) {
        if (w.id == id) {
            return w.rect;
        }
    }
    return std::nullopt;
}

} // namespace Core::Mods
