// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Widget expansion and hit-testing. ExpandWidgets / ExpandWidgetsInto turn a page into the widgets
// drawn for a snapshot: a repeat template becomes one widget per element ("{i}" substituted, placed
// on its grid, packed, rows outside a scroll region's viewport culled) and x_bind / y_bind move a
// widget (ResolveBindOffset). Drawing, the dirty scan and taps all use this one expansion.
// HitTest / HitTestIndex find the topmost visible widget under a point (a scrolled row only inside
// its viewport); WidgetPayload reads the value a widget carries.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "core/mods/mod_ui.h"

namespace Core::Mods {

s32 ResolveBindOffset(s32 base, const std::string& bind, float scale,
                      const StateSnapshot& snapshot) {
    if (bind.empty())
        return base;
    const auto it = snapshot.floats.find(bind);
    const double value = it == snapshot.floats.end() ? snapshot.GetInt(bind) : it->second;
    const double result = base + value * scale;
    return std::isfinite(result) ? static_cast<s32>(std::clamp(result, -32768.0, 32768.0)) : base;
}

/// Repeating widgets become one widget per element. Both drawing and hit testing need the
/// same answer, so the expansion lives here rather than inside the renderer.

namespace {
/// Builds the elements of one repeat template into `out` from slot `count` on, reusing the Widget
/// objects already there (a copy-assignment keeps their strings' storage: no allocation once the
/// slots are warm); returns the new count. Elements past the returned count are stale leftovers.
size_t ExpandTemplateInto(const Page& page, const Widget& source, const StateSnapshot& snapshot,
                          std::vector<Widget>& out, size_t count, ScrollMemo* memo,
                          std::vector<s64>* slot_index = nullptr) {
    const auto position = [&snapshot](Widget& widget) {
        widget.rect[0] = ResolveBindOffset(widget.rect[0], widget.x_bind, widget.x_scale, snapshot);
        widget.rect[1] = ResolveBindOffset(widget.rect[1], widget.y_bind, widget.y_scale, snapshot);
    };
    // List fields hold "{i}" rarely: look once per template, not per element.
    const auto has_index = [](const std::string& s) { return s.find("{i") != std::string::npos; };
    const bool names_indexed = std::ranges::any_of(source.src_names, has_index);
    const bool text_map_indexed =
        source.text_map != nullptr &&
        std::ranges::any_of(*source.text_map, [&](const auto& kv) { return has_index(kv.second); });
    // Runtime 17: the remaining list fields.
    const bool value_names_indexed = std::ranges::any_of(source.names, has_index);
    const bool hidden_icons_indexed = std::ranges::any_of(source.hidden_icons, has_index);
    const bool thresholds_indexed = std::ranges::any_of(
        source.src_thresholds, [&](const auto& t) { return has_index(t.second); });
    const bool view_default_indexed =
        source.view_default != nullptr &&
        (has_index(source.view_default->zoom_bind) || has_index(source.view_default->cx_bind) ||
         has_index(source.view_default->cy_bind) || has_index(source.view_default->reset_bind));
    {
        const s64 rows = RepeatElementCount(source, snapshot);
        // Scrolled list: only the cells whose rows show through the region's rect are built, at
        // the region's offset. A template naming an unknown region expands as before.
        const ScrollRegion* const region =
            source.scroll.empty() ? nullptr : FindScrollRegion(page, source.scroll);
        s32 scroll_offset = 0;
        s64 cell_first = 0;
        s64 cell_end = std::numeric_limits<s64>::max();
        if (region != nullptr) {
            const s32 max_offset = memo != nullptr
                                       ? memo->Get(page, *region, snapshot).max_offset
                                       : MeasureScroll(page, *region, snapshot).max_offset;
            scroll_offset = std::min(ScrollOffset(*region, snapshot), max_offset);
            // y_bind moves a template as a whole; take it into the visibility maths too.
            const s32 base_y =
                ResolveBindOffset(source.rect[1], source.y_bind, source.y_scale, snapshot);
            // A label's text can show below its (zero or short) rect: cull by what it paints,
            // so a half-visible row keeps its text (clipped) as well as its icon.
            const auto paint = RowPaintSpan(source, snapshot, region->rect[3]);
            const auto range = VisibleElementRange(source, *region, base_y, scroll_offset, rows,
                                                   paint[0], paint[1]);
            cell_first = range.first;
            cell_end = range.second;
        }
        s64 placed = 0; // grid position after packing out the skipped elements
        // Without packing, element i sits in cell i: jump straight to the first visible one.
        const s64 start = region != nullptr && !source.pack ? cell_first : 0;
        for (s64 i = start; i < rows; ++i) {
            if (region != nullptr && !source.pack && i >= cell_end) {
                break;
            }
            std::string index; // std::to_string(i), made on first use
            // "{i}" is the element index; "{i+N}" / "{i-N}" the index offset by a constant (a
            // payload that encodes a tab as well: "{i+1000}").
            const auto substitute = [&index, i](std::string& field) {
                for (size_t at = field.find("{i"); at != std::string::npos;
                     at = field.find("{i", at)) {
                    if (at + 2 < field.size() && field[at + 2] == '}') {
                        if (index.empty()) {
                            index = std::to_string(i);
                        }
                        field.replace(at, 3, index);
                        at += index.size();
                        continue;
                    }
                    const size_t close = field.find('}', at + 2);
                    const char sign = at + 2 < field.size() ? field[at + 2] : '\0';
                    if (close == std::string::npos || (sign != '+' && sign != '-') ||
                        close == at + 3 ||
                        !std::all_of(field.begin() + static_cast<std::ptrdiff_t>(at + 3),
                                     field.begin() + static_cast<std::ptrdiff_t>(close),
                                     [](char c) { return c >= '0' && c <= '9'; }) ||
                        close - at - 3 > 15) {
                        at += 2; // not an index expression: leave it alone
                        continue;
                    }
                    const s64 k = std::stoll(field.substr(at + 3, close - at - 3));
                    const std::string value = std::to_string(sign == '+' ? i + k : i - k);
                    field.replace(at, close - at + 1, value);
                    at += value.size();
                }
            };
            // Packing: drop elements whose gate fails so the grid closes up. The gate is read on
            // the per-element hide_bind (with {i} substituted), using the same keep-range rules as
            // draw.
            if (source.pack && !source.hide_bind.empty()) {
                std::string hb = source.hide_bind;
                substitute(hb);
                const s64 gv = snapshot.GetInt(hb);
                const s64 kmin = source.keep_min_i ? i + source.keep_min : source.keep_min;
                const s64 kmax = source.keep_max_i ? i + source.keep_max : source.keep_max;
                if (gv < kmin || gv > kmax || (source.hide_eq_on && gv == source.hide_eq)) {
                    continue;
                }
            }
            const s64 cell = source.pack ? placed : i;
            if (region != nullptr && source.pack && (cell < cell_first || cell >= cell_end)) {
                ++placed; // packed but scrolled out of view: keeps its cell, costs no clone
                if (cell >= cell_end) {
                    break;
                }
                continue;
            }
            Widget& clone = count < out.size() ? out[count] : out.emplace_back();
            // A slot that already holds element i of this template (the caller keeps one slot
            // vector per template) needs only its position: everything else is a function of i.
            if (slot_index != nullptr) {
                if (slot_index->size() <= count) {
                    slot_index->resize(count + 1, -1);
                }
                if ((*slot_index)[count] == i) {
                    ++count;
                    clone.rect = source.rect;
                    position(clone);
                    if (source.repeat_cols > 0) {
                        clone.rect[0] +=
                            static_cast<s32>(cell % source.repeat_cols) * source.repeat_dx;
                        clone.rect[1] +=
                            static_cast<s32>(cell / source.repeat_cols) * source.repeat_row_dy;
                    } else {
                        clone.rect[0] += static_cast<s32>(cell) * source.repeat_dx;
                        clone.rect[1] += static_cast<s32>(cell) * source.repeat_dy;
                    }
                    if (region != nullptr) {
                        clone.rect[1] -= scroll_offset;
                    }
                    if (source.auto_w) {
                        clone.auto_box = false; // re-measured: its text may have changed
                        ApplyAutoWidth(clone, snapshot);
                    }
                    ++placed;
                    continue;
                }
                (*slot_index)[count] = i;
            }
            ++count;
            clone = source;
            clone.repeat = 0;
            if (source.keep_min_i) {
                clone.keep_min = i + source.keep_min;
            }
            if (source.keep_max_i) {
                clone.keep_max = i + source.keep_max;
            }
            substitute(clone.text);
            substitute(clone.x_bind);
            substitute(clone.y_bind);
            substitute(clone.bind);
            substitute(clone.max_bind);
            substitute(clone.bind_text);
            substitute(clone.src);
            substitute(clone.src_bind);
            substitute(clone.fill_bind);
            substitute(clone.on_tap);
            substitute(clone.on_hold);
            substitute(clone.on_swipe_left);
            substitute(clone.on_swipe_right);
            substitute(clone.on_swipe_up);
            substitute(clone.on_swipe_down);
            substitute(clone.id);
            substitute(clone.hide_bind);
            substitute(clone.need_bind);
            substitute(clone.payload);
            substitute(clone.select_group);
            substitute(clone.accept_group);
            substitute(clone.drop_action);
            substitute(clone.highlight_src);
            substitute(clone.drag_under_src);
            substitute(clone.text_src);
            substitute(clone.text_bind);
            substitute(clone.rotate_bind); // runtime 16
            substitute(clone.scale_bind);
            substitute(clone.tint_bind);
            substitute(clone.fill_image);
            substitute(clone.empty_src);
            substitute(clone.suffix);
            substitute(clone.max_sep);
            substitute(clone.table);
            // Runtime 17: every other per-element string field (not `scroll`, which places the
            // template, nor the page-level anim group or the map's shared extras).
            substitute(clone.src_format);
            substitute(clone.group_sep);
            substitute(clone.area_bind);
            substitute(clone.room_bind);
            substitute(clone.area);
            substitute(clone.marker_x_bind);
            substitute(clone.marker_y_bind);
            substitute(clone.marker_icon);
            substitute(clone.marker_src);
            substitute(clone.actor_x_bind);
            substitute(clone.actor_y_bind);
            substitute(clone.actor_icon);
            if (value_names_indexed) {
                for (auto& name : clone.names) {
                    substitute(name);
                }
            }
            if (hidden_icons_indexed) {
                for (auto& icon : clone.hidden_icons) {
                    substitute(icon);
                }
            }
            if (thresholds_indexed) {
                for (auto& threshold : clone.src_thresholds) {
                    substitute(threshold.second);
                }
            }
            if (view_default_indexed) {
                auto binds = std::make_shared<ViewDefaultBinds>(*source.view_default);
                substitute(binds->zoom_bind);
                substitute(binds->cx_bind);
                substitute(binds->cy_bind);
                substitute(binds->reset_bind);
                clone.view_default = std::move(binds);
            }
            if (names_indexed) {
                for (auto& name : clone.src_names) {
                    substitute(name);
                }
            }
            if (text_map_indexed) {
                // Shared between the template's elements: this element gets its own copy.
                auto map = std::make_shared<std::unordered_map<s64, std::string>>(*source.text_map);
                for (auto& [value, text] : *map) {
                    substitute(text);
                }
                clone.text_map = std::move(map);
            }
            position(clone);
            if (source.repeat_cols > 0) {
                const s64 col = cell % source.repeat_cols;
                const s64 row = cell / source.repeat_cols;
                clone.rect[0] += static_cast<s32>(col) * source.repeat_dx;
                clone.rect[1] += static_cast<s32>(row) * source.repeat_row_dy;
            } else {
                clone.rect[0] += static_cast<s32>(cell) * source.repeat_dx;
                clone.rect[1] += static_cast<s32>(cell) * source.repeat_dy;
            }
            if (region != nullptr) {
                clone.rect[1] -= scroll_offset;
                clone.scroll_clip = region->rect;
            }
            if (source.pack) {
                // already gated here; don't let the draw pass hide it again
                clone.hide_bind.clear();
            }
            ApplyAutoWidth(clone, snapshot);
            ++placed;
        }
    }
    return count;
}
} // namespace

namespace {
// Runtime 17 auto_w: the measuring canvas of this thread's innermost TextMeasureScope.
thread_local Canvas* measure_canvas = nullptr;
thread_local const TextProvider* measure_texts = nullptr;
} // namespace

TextMeasureScope::TextMeasureScope(Canvas& canvas, const TextProvider* texts)
    : prev_canvas{measure_canvas}, prev_texts{measure_texts} {
    measure_canvas = &canvas;
    measure_texts = texts;
}

TextMeasureScope::~TextMeasureScope() {
    measure_canvas = prev_canvas;
    measure_texts = prev_texts;
}

const std::string* LabelShownText(const Widget& widget, const StateSnapshot& snapshot,
                                  const TextProvider& texts,
                                  std::shared_ptr<const std::string>& owner) {
    // texts() returns shared_ptr<const std::string> (see TextProvider's own declaration
    // comment); `owner` keeps a resolved msbt string alive past this call.
    const auto resolve = [&texts, &owner](const std::string& ref) -> const std::string* {
        if (!ref.starts_with("msbt:")) {
            return &ref;
        }
        owner = texts ? texts(ref) : nullptr;
        return owner.get();
    };
    if (!widget.text_bind.empty()) {
        const auto bound = snapshot.ints.find(widget.text_bind);
        if (bound != snapshot.ints.end() && bound->second != -1 && widget.text_map) {
            if (const auto m = widget.text_map->find(bound->second); m != widget.text_map->end()) {
                return resolve(m->second);
            }
        }
        return nullptr;
    }
    if (!widget.text_src.empty()) {
        return resolve(widget.text_src);
    }
    if (!widget.bind_text.empty()) {
        static const std::string dash = "-";
        const auto text = snapshot.texts.find(widget.bind_text);
        return text == snapshot.texts.end() ? &dash : &text->second;
    }
    return &widget.text;
}

std::array<s32, 4> AutoWidthBox(const Widget& widget, const std::array<s32, 4>& rect, s32 text_w,
                                s32 text_h) {
    const s32 pad = std::max(0, widget.pad);
    if (widget.type == WidgetType::Button) {
        const s32 content =
            widget.align == 1 ? text_w + 2 * pad : std::max(0, widget.text_inset) + text_w + pad;
        const s32 width = std::max(rect[2], content);
        const s32 x = widget.align == 1 ? rect[0] + rect[2] / 2 - width / 2 : rect[0];
        return {x, rect[1], width, rect[3]};
    }
    const s32 width = std::max(rect[2], text_w + 2 * pad);
    const s32 x = widget.align == 2   ? rect[0] + pad - width
                  : widget.align == 1 ? rect[0] - width / 2
                                      : rect[0] - pad;
    return {x, rect[1] - pad, width, std::max(rect[3], text_h) + 2 * pad};
}

std::array<s32, 2> AutoBoxTextAnchor(const Widget& widget, s32 x, s32 y, s32 rw) {
    const s32 pad = std::max(0, widget.pad);
    const s32 tx = widget.align == 2 ? x + rw - pad : widget.align == 1 ? x + rw / 2 : x + pad;
    return {tx, y + pad};
}

void ApplyAutoWidth(Widget& widget, const StateSnapshot& snapshot) {
    if (!widget.auto_w || widget.auto_box || measure_canvas == nullptr ||
        (widget.type != WidgetType::Label && widget.type != WidgetType::Button)) {
        return;
    }
    Canvas& canvas = *measure_canvas;
    const s32 scale = std::max(1, widget.text_scale);
    s32 text_w = 0;
    s32 text_h = scale * 5;
    const bool markup = canvas.ColorMarkup();
    if (widget.type == WidgetType::Button) {
        canvas.SetColorMarkup(false); // as DrawButton draws its caption
        text_w = canvas.MeasureText(widget.text, scale);
    } else {
        static const TextProvider no_texts;
        std::shared_ptr<const std::string> owner;
        const std::string* const shown = LabelShownText(
            widget, snapshot, measure_texts != nullptr ? *measure_texts : no_texts, owner);
        canvas.SetColorMarkup(widget.color_markup);
        if (shown != nullptr && !shown->empty()) {
            if (widget.wrap_width > 0 || shown->find('\n') != std::string::npos) {
                const auto& lines =
                    canvas.LayoutLines(*shown, scale, widget.wrap_width, widget.max_lines);
                for (const auto& line : lines) {
                    text_w = std::max(text_w, canvas.MeasureText(line, scale));
                }
                const s32 pitch = scale * 5 + (widget.line_gap >= 0 ? widget.line_gap : scale * 3);
                text_h = static_cast<s32>(std::max<size_t>(lines.size(), 1) - 1) * pitch + scale * 5;
            } else {
                text_w = canvas.MeasureText(*shown, scale);
            }
        }
    }
    canvas.SetColorMarkup(markup);
    widget.rect = AutoWidthBox(widget, widget.rect, text_w, text_h);
    widget.auto_box = true;
}

size_t ExpandRepeatTemplateInto(const Page& page, const Widget& source,
                                const StateSnapshot& snapshot, std::vector<Widget>& slots,
                                std::vector<s64>* slot_index, ScrollMemo* memo) {
    return ExpandTemplateInto(page, source, snapshot, slots, 0, memo, slot_index);
}

size_t ExpandWidgetsInto(const Page& page, const StateSnapshot& snapshot,
                         std::vector<Widget>& slots) {
    // Expand repeating widgets first: one definition per row, drawn once per element. Doing it
    // here rather than in the draw switch keeps every widget type repeatable for free.
    ScrollMemo memo;
    memo.Reset(page.scrolls.size());
    size_t count = 0;
    for (const auto& source : page.widgets) {
        if (source.repeat <= 0) {
            Widget& w = count < slots.size() ? slots[count] : slots.emplace_back();
            ++count;
            w = source;
            w.rect[0] = ResolveBindOffset(source.rect[0], source.x_bind, source.x_scale, snapshot);
            w.rect[1] = ResolveBindOffset(source.rect[1], source.y_bind, source.y_scale, snapshot);
            ApplyAutoWidth(w, snapshot);
            continue;
        }
        count = ExpandTemplateInto(page, source, snapshot, slots, count, &memo);
    }
    return count;
}

void ExpandWidgetRefsInto(const Page& page, const StateSnapshot& snapshot,
                          std::vector<Widget>& slots, std::vector<const Widget*>& refs) {
    // ExpandWidgetsInto, except that a widget the expansion would copy unchanged (no repeat, no
    // x_bind/y_bind: ResolveBindOffset returns the base for an empty bind) is referenced in place.
    // Slots are filled first and referenced after, as the vector may grow while filling.
    ScrollMemo memo;
    memo.Reset(page.scrolls.size());
    static thread_local std::vector<std::pair<const Widget*, size_t>> plan;
    plan.clear();
    size_t count = 0;
    for (const auto& source : page.widgets) {
        if (source.repeat <= 0) {
            if (source.x_bind.empty() && source.y_bind.empty() && !source.auto_w) {
                plan.emplace_back(&source, 0);
                continue;
            }
            Widget& w = count < slots.size() ? slots[count] : slots.emplace_back();
            w = source;
            w.rect[0] = ResolveBindOffset(source.rect[0], source.x_bind, source.x_scale, snapshot);
            w.rect[1] = ResolveBindOffset(source.rect[1], source.y_bind, source.y_scale, snapshot);
            ApplyAutoWidth(w, snapshot);
            plan.emplace_back(nullptr, count++);
            continue;
        }
        const size_t before = count;
        count = ExpandTemplateInto(page, source, snapshot, slots, count, &memo);
        for (size_t k = before; k < count; ++k) {
            plan.emplace_back(nullptr, k);
        }
    }
    refs.clear();
    refs.reserve(plan.size());
    for (const auto& [ptr, k] : plan) {
        refs.push_back(ptr != nullptr ? ptr : &slots[k]);
    }
}

std::vector<Widget> ExpandWidgets(const Page& page, const StateSnapshot& snapshot) {
    std::vector<Widget> expanded;
    expanded.reserve(page.widgets.size());
    expanded.resize(ExpandWidgetsInto(page, snapshot, expanded));
    return expanded;
}

namespace {
/// A scrolled row is only touchable through its region's viewport (the part scrolled under the
/// header or footer is not there).
bool InScrollClip(const Widget& w, s32 x, s32 y) {
    const auto& c = w.scroll_clip;
    return c[2] <= 0 || c[3] <= 0 || (x >= c[0] && x < c[0] + c[2] && y >= c[1] && y < c[1] + c[3]);
}
} // namespace

std::string HitTest(const Page& page, const StateSnapshot& snapshot, s32 x, s32 y) {
    // Later widgets are drawn on top, so search back to front. A widget the page currently hides
    // is not tappable either.
    const std::vector<Widget> expanded = ExpandWidgets(page, snapshot);
    for (auto it = expanded.rbegin(); it != expanded.rend(); ++it) {
        if (it->on_tap.empty() || WidgetHidden(*it, snapshot) || !InScrollClip(*it, x, y)) {
            continue;
        }
        const s32 wx = it->rect[0];
        const s32 wy = it->rect[1];
        if (x >= wx && x < wx + it->rect[2] && y >= wy && y < wy + it->rect[3]) {
            return it->on_tap;
        }
    }
    return {};
}

s64 HitTestIndex(const std::vector<Widget>& expanded, const StateSnapshot& snapshot, s32 x, s32 y,
                 const std::function<bool(const Widget&)>& accept) {
    for (size_t i = expanded.size(); i-- > 0;) {
        const Widget& w = expanded[i];
        if (!accept(w) || WidgetHidden(w, snapshot) || !InScrollClip(w, x, y)) {
            continue;
        }
        if (x >= w.rect[0] && x < w.rect[0] + w.rect[2] && y >= w.rect[1] &&
            y < w.rect[1] + w.rect[3]) {
            return static_cast<s64>(i);
        }
    }
    return -1;
}

std::optional<s64> WidgetPayload(const Widget& widget, const StateSnapshot& snapshot) {
    std::string_view text = widget.payload;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '$') {
        const std::string key{text.substr(1)};
        if (const auto it = snapshot.ints.find(key); it != snapshot.ints.end()) {
            return it->second;
        }
        if (const auto it = snapshot.floats.find(key); it != snapshot.floats.end()) {
            return static_cast<s64>(it->second);
        }
        return std::nullopt;
    }
    const std::string owned{text};
    const size_t sign = owned.front() == '-' || owned.front() == '+' ? 1 : 0;
    const bool hex = owned.compare(sign, 2, "0x") == 0 || owned.compare(sign, 2, "0X") == 0;
    char* end = nullptr;
    const long long value = std::strtoll(owned.c_str(), &end, hex ? 16 : 10);
    if (end == owned.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<s64>(value);
}

} // namespace Core::Mods
