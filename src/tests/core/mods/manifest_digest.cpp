// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// V1 regression net: every published dual-screen package (Dread, LA, MK8D, P5R) is parsed
// through the runtime's real parser (ParseDualScreenManifest, plus PackageMinRuntime and
// IsUsableDualScreenManifest on the raw JSON), and the parsed Manifest is serialised field by
// field into a deterministic text: one line per page / widget / action / point / map entry,
// listing only the fields that differ from a default-constructed value, maps in key order,
// floats in shortest round-trip form. The text is compared with the checked-in golden
// golden/<Package>/manifest.txt.zst (zstd-compressed, ~20x smaller; read it with `zstd -dc`);
// any change in what the parser produces fails with the lines that changed, and the actual and
// golden texts are written out in plain text for `diff -u`.
//
// Not covered here: the per-build data file merge (points / symbols / spies / patches of
// <BUILDID>.json / data.json) lives inside ModRuntime::Discover and needs a Core::System; those
// files are recorded by a hash of their canonical JSON only. The native-module block ("module")
// is read by GameModule::Load (dlopen) and is not dumped.
//
// When a struct gains a field the dumper must learn it: the layout guard at the bottom fails
// with the struct's name when a size changes (libstdc++ x86-64 only).

#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "core/mods/mod_runtime.h"
#include "core/mods/mod_ui.h"
#include "tests/core/mods/golden_common.h"

using namespace Core::Mods;

namespace {

// --- value -> text ------------------------------------------------------------------------------

std::string R(const std::string& s) {
    return nlohmann::json(s).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
std::string R(bool v) {
    return v ? "true" : "false";
}
std::string R(float v) {
    return fmt::format("{}", v);
}
std::string R(double v) {
    return fmt::format("{}", v);
}
std::string R(char32_t c) {
    return fmt::format("U+{:04X}", static_cast<u32>(c));
}
template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char32_t>)
std::string R(T v) {
    return fmt::format("{}", static_cast<std::conditional_t<std::is_signed_v<T>, s64, u64>>(v));
}
template <typename T>
    requires std::is_enum_v<T>
std::string R(T v) {
    return fmt::format("#{}", static_cast<s64>(v));
}
template <typename T>
std::string R(const std::optional<T>& v);
template <typename T>
std::string R(const std::shared_ptr<T>& v);
template <typename A, typename B>
std::string R(const std::pair<A, B>& v);
template <typename T, size_t N>
std::string R(const std::array<T, N>& v);
template <typename T>
std::string R(const std::vector<T>& v);
template <typename K, typename V>
std::string R(const std::map<K, V>& v);
template <typename K, typename V>
std::string R(const std::unordered_map<K, V>& v);

// Structs (non-default fields only).
std::string R(const PointGate& o);
std::string R(const PatternFind& o);
std::string R(const ChainHop& o);
std::string R(const ArrayFind& o);
std::string R(const PlayerFind& o);
std::string R(const TextScan& o);
std::string R(const DataPoint& o);
std::string R(const std::shared_ptr<const ExprProgram>& o);
std::string R(const DerivedPoint::CmpOperand& o);
std::string R(const DerivedPoint& o);
std::string R(const WidgetAnim& o);
std::string R(const MapWidgetExtras::Group& o);
std::string R(const MapWidgetExtras::LabelStyle& o);
std::string R(const MapWidgetExtras::Overlay& o);
std::string R(const MapWidgetExtras& o);
std::string R(const ViewDefaultBinds& o);
std::string R(const ChartSpec& o);
std::string R(const Widget& o);
std::string R(const ScrollRegion& o);
std::string R(const ActionValue& o);
std::string R(const Action& o);
std::string R(const PageBindTarget& o);
std::string R(const PageBind& o);
std::string R(const MapRoom& o);
std::string R(const MapMarker& o);
std::string R(const MapLayer& o);
std::string R(const DynamicMarkerDef& o);
std::string R(const MapLabel& o);
std::string R(const MapRoomCategory::Bake& o);
std::string R(const MapRoomCategory::VisitedCorrection& o);
std::string R(const MapRoomCategory& o);
std::string R(const MapArea::OverviewRegion& o);
std::string R(const MapArea::MapOccluder& o);
std::string R(const MapStyle& o);
std::string R(const CompositeLayer& o);
std::string R(const CompositeDef& o);
std::string R(const MsbtConfig& o);
std::string R(const HapticsConfig& o);
std::string R(const NavConfig& o);
std::string R(const CallStep& o);
std::string R(const CallSequence& o);
std::string R(const GuestPatch& o);
std::string R(const EnforceRule& o);
std::string R(const SpyPoint& o);
std::string R(const Il2CppLayout& o);
std::string R(const Manifest::Symbol& o);

template <typename T>
std::string R(const std::optional<T>& v) {
    return v ? R(*v) : std::string{"none"};
}
template <typename T>
std::string R(const std::shared_ptr<T>& v) {
    return v ? R(*v) : std::string{"null"};
}
template <typename A, typename B>
std::string R(const std::pair<A, B>& v) {
    return "(" + R(v.first) + "," + R(v.second) + ")";
}
template <typename It>
std::string Seq(It begin, It end) {
    std::string out = "[";
    for (auto it = begin; it != end; ++it) {
        if (it != begin) {
            out += ",";
        }
        out += R(*it);
    }
    return out + "]";
}
template <typename T, size_t N>
std::string R(const std::array<T, N>& v) {
    return Seq(v.begin(), v.end());
}
template <typename T>
std::string R(const std::vector<T>& v) {
    return Seq(v.begin(), v.end());
}
template <typename K, typename V>
std::string R(const std::map<K, V>& v) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, val] : v) {
        out += (first ? "" : ",") + R(k) + ":" + R(val);
        first = false;
    }
    return out + "}";
}
template <typename K, typename V>
std::string R(const std::unordered_map<K, V>& v) {
    std::vector<std::pair<std::string, std::string>> items;
    items.reserve(v.size());
    for (const auto& [k, val] : v) {
        items.emplace_back(R(k), R(val));
    }
    // Numeric keys sort numerically when they print as numbers of the same sign; plain text
    // order is still deterministic, which is all a golden needs.
    std::ranges::sort(items);
    std::string out = "{";
    for (size_t i = 0; i < items.size(); ++i) {
        out += (i ? "," : "") + items[i].first + ":" + items[i].second;
    }
    return out + "}";
}

/// Accumulates " name=value" for every field that differs from the default-constructed object.
template <typename T>
struct Dumper {
    const T& o;
    const T& d;
    std::string out;
    template <typename V>
    void Field(const char* name, const V& value, const V& def) {
        std::string v = R(value);
        if (v != R(def)) {
            out += fmt::format("{}{}={}", out.empty() ? "" : " ", name, v);
        }
    }
    /// A colour field, in hex.
    void Color(const char* name, u32 value, u32 def) {
        if (value != def) {
            out += fmt::format("{}{}=0x{:08X}", out.empty() ? "" : " ", name, value);
        }
    }
    std::string Done() const {
        return "{" + out + "}";
    }
};
template <typename T>
const T& Default() {
    static const T d{};
    return d;
}
#define DUMP_BEGIN(T) Dumper<T> dd{o, Default<T>(), {}};
#define F(name) dd.Field(#name, o.name, dd.d.name)
#define C(name) dd.Color(#name, o.name, dd.d.name)
#define DUMP_END return dd.Done()

std::string R(const PointGate& o) {
    if (o.point.empty() && !o.negate) {
        return "-";
    }
    return (o.negate ? "!" : "") + R(o.point);
}
std::string R(const PatternFind& o) {
    DUMP_BEGIN(PatternFind);
    F(bytes);
    F(mask);
    F(offset);
    F(index);
    F(heap);
    DUMP_END;
}
std::string R(const ChainHop& o) {
    DUMP_BEGIN(ChainHop);
    F(offset);
    F(stride);
    F(index);
    F(index_bind);
    F(static_fields);
    F(is_list);
    F(list_next);
    F(list_node_at);
    F(index_from_array);
    DUMP_END;
}
std::string R(const ArrayFind& o) {
    DUMP_BEGIN(ArrayFind);
    F(vtable);
    F(stride);
    F(index);
    F(min_run);
    DUMP_END;
}
std::string R(const PlayerFind& o) {
    DUMP_BEGIN(PlayerFind);
    F(vtable);
    F(pos_offset);
    F(min_samples);
    F(min_abs);
    DUMP_END;
}
std::string R(const TextScan& o) {
    DUMP_BEGIN(TextScan);
    F(candidates);
    F(offset);
    DUMP_END;
}
std::string R(const DataPoint& o) {
    DUMP_BEGIN(DataPoint);
    F(type);
    F(array);
    F(player);
    F(find);
    F(text_scan);
    F(class_name);
    F(hops);
    F(root_bind);
    F(base);
    F(chain);
    F(offset);
    F(is_pointer);
    F(count);
    F(count_bind);
    F(shift);
    F(has_mask);
    F(mask);
    F(popcount);
    F(stride);
    DUMP_END;
}
std::string R(const std::shared_ptr<const ExprProgram>& o) {
    if (!o) {
        return "-";
    }
    return o->Ok() ? fmt::format("ok:{}n/{}r", o->nodes.size(), o->refs.size())
                   : "err:" + o->error;
}
std::string R(const DerivedPoint::CmpOperand& o) {
    DUMP_BEGIN(DerivedPoint::CmpOperand);
    F(is_const);
    F(const_value);
    F(name);
    DUMP_END;
}
std::string R(const DerivedPoint& o) {
    DUMP_BEGIN(DerivedPoint);
    F(name);
    F(terms);
    F(add);
    F(floor);
    F(round);
    F(hold_last_nonzero);
    F(hold_gate);
    F(select);
    F(select_then);
    F(select_else);
    F(any_eq_array);
    F(any_eq_count);
    F(any_eq_value);
    F(cmp_op);
    F(cmp_a);
    F(cmp_b);
    F(nonzero_sources);
    F(nonzero_require_all);
    F(countdown_target);
    F(countdown_now);
    F(expr);
    F(expr_program);
    DUMP_END;
}
std::string R(const ViewDefaultBinds& o) {
    DUMP_BEGIN(ViewDefaultBinds);
    F(zoom_bind);
    F(cx_bind);
    F(cy_bind);
    F(reset_bind);
    DUMP_END;
}
std::string R(const ChartSpec& o) {
    DUMP_BEGIN(ChartSpec);
    F(key);
    F(samples);
    F(interval_ms);
    F(style);
    F(has_min);
    F(has_max);
    F(min);
    F(max);
    DUMP_END;
}
std::string R(const WidgetAnim& o) {
    DUMP_BEGIN(WidgetAnim);
    F(gate);
    F(key);
    F(from);
    F(origin_widget);
    F(ms);
    F(easing);
    F(has_box);
    F(box);
    DUMP_END;
}
std::string R(const MapWidgetExtras::Group& o) {
    DUMP_BEGIN(MapWidgetExtras::Group);
    F(show);
    F(hide);
    F(hide_in);
    F(opacity);
    F(min_zoom);
    DUMP_END;
}
std::string R(const MapWidgetExtras::LabelStyle& o) {
    DUMP_BEGIN(MapWidgetExtras::LabelStyle);
    F(text_scale);
    C(color);
    C(outline_color);
    F(outline);
    F(opacity);
    F(avoid_overlap);
    DUMP_END;
}
std::string R(const MapWidgetExtras::Overlay& o) {
    DUMP_BEGIN(MapWidgetExtras::Overlay);
    if (!o.src_detail_bind.empty()) {
        F(src_detail_bind);
    }
    if (o.detail_threshold != 210.0f) {
        F(detail_threshold);
    }
    F(src);
    F(src_bind);
    F(x0);
    F(y0);
    F(x1);
    F(y1);
    F(show);
    F(opacity);
    DUMP_END;
}
std::string R(const MapWidgetExtras& o) {
    DUMP_BEGIN(MapWidgetExtras);
    F(groups);
    F(label_style);
    F(on_map_tap);
    F(on_marker_tap);
    F(empty_tap_deselects);
    F(tap_enabled);
    F(marker_hit_px);
    F(marker_tap_groups);
    F(view_rect_binds);
    F(view_rect_pad);
    F(image_bind);
    if (!o.marker_rotate_bind.empty()) {
        F(marker_rotate_bind);
    }
    if (o.marker_tint != 0xFFFFFFFFu) {
        C(marker_tint);
    }
    F(overlays);
    DUMP_END;
}
std::string R(const Widget& o) {
    DUMP_BEGIN(Widget);
    F(type);
    F(rect);
    F(map_extras);
    F(tap_block);
    F(input_block);
    F(haptic);
    F(anim);
    F(text);
    F(x_bind);
    F(y_bind);
    F(x_scale);
    F(y_scale);
    F(names);
    F(pad);
    F(div);
    F(mul);
    F(add);
    F(bind);
    F(max_bind);
    F(max_sep);
    F(max_const);
    C(color);
    C(bg);
    F(text_scale);
    C(text_outline);
    F(text_outline_px);
    F(text_rise);
    F(align);
    F(flip_x);
    F(flip_y);
    F(spin);
    F(shake);
    F(need_bind);
    F(suffix);
    F(area_label);
    F(hidden_icons);
    F(pill);
    F(frame);
    F(border);
    F(text_inset);
    F(gap);
    F(label_offset);
    F(pulse);
    F(on_tap);
    F(on_hold);
    F(hold_ms);
    F(on_swipe_left);
    F(on_swipe_right);
    F(on_swipe_up);
    F(on_swipe_down);
    F(swipe_px);
    F(id);
    F(repeat);
    F(repeat_bind);
    F(repeat_div);
    F(repeat_dx);
    F(repeat_dy);
    F(repeat_cols);
    F(repeat_row_dy);
    F(pack);
    F(scroll);
    F(scroll_clip);
    F(keep_min_i);
    F(keep_max_i);
    F(pan_zoom);
    F(min_zoom);
    F(max_zoom);
    F(view_default);
    F(src);
    F(bind_text);
    F(src_bind);
    F(src_names);
    F(src_thresholds);
    F(table);
    F(src_format);
    F(hide_bind);
    F(keep_min);
    F(keep_max);
    F(hide_eq);
    F(hide_eq_on);
    F(area_bind);
    F(room_bind);
    F(area);
    F(marker_x_bind);
    F(marker_y_bind);
    F(marker_scale);
    F(marker_icon);
    F(actor_x_bind);
    F(actor_y_bind);
    F(actor_icon);
    F(actor_reveal_required);
    F(follow_window);
    F(marker_src);
    F(marker_size);
    F(marker_anchor);
    F(view_idle_ms);
    F(fill_bind);
    F(empty_src);
    F(src_rect);
    F(empty_rect);
    F(payload);
    F(select_group);
    F(draggable);
    F(drag_scale);
    F(drop_action);
    F(accept_group);
    F(highlight_src);
    C(highlight_color);
    F(drag_under_src);
    F(drag_under_src_rect);
    F(drag_under_rect);
    C(drag_under_tint);
    C(drag_under_color);
    F(text_src);
    F(text_bind);
    F(text_map);
    // Runtime 18 defaults preserve the serialized model of older packages.
    if (o.fit_text) { F(fit_text); }
    if (o.text_min_scale != 1) { F(text_min_scale); }
    if (o.text_center_h != 0) { F(text_center_h); }
    F(wrap_width);
    F(max_lines);
    F(line_gap);
    F(icon_silhouette);
    F(color_markup);
    F(outline_copy);
    F(auto_w);
    F(auto_box);
    F(group);
    F(group_sep);
    F(rotate);
    F(rotate_bind);
    F(scale_bind);
    F(pivot);
    F(has_pivot);
    C(tint);
    F(has_tint);
    F(tint_bind);
    F(tint_colors);
    F(fill);
    F(slice);
    F(fill_dir);
    F(fill_image);
    F(chart);
    DUMP_END;
}
std::string R(const ScrollRegion& o) {
    DUMP_BEGIN(ScrollRegion);
    F(id);
    F(rect);
    F(count_bind);
    F(row_h);
    F(cols);
    F(pad);
    F(show);
    F(reset_bind);
    F(fling);
    F(friction);
    if (!o.bar_src.empty()) { F(bar_src); }
    if (!o.bar_track_src.empty()) { F(bar_track_src); }
    C(bar_color);
    C(bar_track);
    F(bar_w);
    DUMP_END;
}
std::string R(const ActionValue& o) {
    DUMP_BEGIN(ActionValue);
    F(value);
    F(as_float);
    F(ref);
    DUMP_END;
}
std::string R(const Action& o) {
    DUMP_BEGIN(Action);
    F(kind);
    F(name);
    F(enabled);
    F(value_ref);
    F(value_float);
    F(slot);
    F(count);
    F(free_value);
    F(writes);
    F(select);
    F(group);
    F(cycle);
    F(flag_has_int);
    F(flag_int);
    F(view);
    F(module_action);
    F(point);
    F(value);
    F(value_from_payload);
    F(swap_point);
    F(button);
    F(frames);
    F(repeat);
    F(counter);
    F(target);
    F(modulo);
    F(button_neg);
    F(delta);
    F(use_delta);
    F(gap);
    F(hold_map);
    F(page);
    F(transition);
    F(duration_ms);
    F(easing);
    F(shadow);
    F(origin);
    F(haptic);
    F(call_fn);
    F(call_fn_symbol);
    F(args);
    F(sequence);
    F(flag);
    F(flag_value);
    DUMP_END;
}
std::string R(const PageBindTarget& o) {
    DUMP_BEGIN(PageBindTarget);
    F(page);
    F(transition);
    F(duration_ms);
    F(easing);
    F(shadow);
    F(origin);
    DUMP_END;
}
std::string R(const PageBind& o) {
    DUMP_BEGIN(PageBind);
    F(point);
    F(equals);
    F(ready);
    F(when_equal);
    F(when_not_equal);
    DUMP_END;
}
std::string R(const MapRoom& o) {
    DUMP_BEGIN(MapRoom);
    F(area);
    F(sprite);
    F(x);
    F(y);
    F(w);
    F(h);
    DUMP_END;
}
std::string R(const MapMarker& o) {
    DUMP_BEGIN(MapMarker);
    F(kind);
    F(icon);
    F(x);
    F(y);
    F(show);
    F(hide);
    F(group);
    F(opacity);
    F(size);
    F(name);
    F(vignette);
    F(hidden);
    F(opened);
    F(collected);
    F(unveiled);
    F(veiled);
    F(has_box);
    F(bx0);
    F(by0);
    F(bx1);
    F(by1);
    F(has_pulse_box);
    F(px0);
    F(py0);
    F(px1);
    F(py1);
    F(has_hint_box);
    F(hx0);
    F(hy0);
    F(hx1);
    F(hy1);
    F(structural);
    F(open_icon);
    F(collected_icon);
    F(collectible);
    DUMP_END;
}
std::string R(const MapLayer& o) {
    DUMP_BEGIN(MapLayer);
    F(geo);
    F(kind);
    C(color);
    F(color_bind);
    F(color_map);
    F(category);
    F(live_clip);
    F(post_fog);
    F(gain);
    DUMP_END;
}
std::string R(const DynamicMarkerDef& o) {
    DUMP_BEGIN(DynamicMarkerDef);
    F(group);
    F(count);
    F(x);
    F(y);
    F(kind);
    F(has_hide_kind);
    F(hide_when_kind);
    F(icon_by_kind);
    F(icon_default);
    F(icon);
    F(scale_x);
    F(scale_y);
    F(offset_x);
    F(offset_y);
    F(size);
    F(selected_size);
    F(selected_icon);
    F(selected_icon_size);
    F(opacity);
    F(anchor_x);
    F(anchor_y);
    F(show);
    F(hide);
    F(icon_src_bind);
    F(size_world);
    if (o.size_max != 0.0f) { F(size_max); }
    F(bar_bind);
    F(bar_max_bind);
    F(bar_max);
    C(bar_color);
    C(bar_bg);
    F(bar_h);
    F(dim_bind);
    F(frame_color_bind);
    F(frame_px);
    F(tint_bind);
    DUMP_END;
}
std::string R(const MapLabel& o) {
    DUMP_BEGIN(MapLabel);
    F(text);
    F(text_src);
    F(x);
    F(y);
    F(show);
    F(hide);
    F(group);
    F(opacity);
    F(text_scale);
    F(color);
    F(outline_color);
    F(outline);
    DUMP_END;
}
std::string R(const MapRoomCategory::Bake& o) {
    DUMP_BEGIN(MapRoomCategory::Bake);
    F(mode);
    F(gain);
    DUMP_END;
}
std::string R(const MapRoomCategory::VisitedCorrection& o) {
    DUMP_BEGIN(MapRoomCategory::VisitedCorrection);
    F(gain);
    C(tint);
    F(amount);
    DUMP_END;
}
std::string R(const MapRoomCategory& o) {
    DUMP_BEGIN(MapRoomCategory);
    F(id);
    F(polys);
    C(color);
    F(color_bind);
    F(color_map);
    F(class_gain);
    F(reveal_before_unlock);
    F(bake);
    F(visited_gain);
    C(visited_tint);
    F(visited_amount);
    F(visited_map);
    DUMP_END;
}
std::string R(const MapArea::OverviewRegion& o) {
    DUMP_BEGIN(MapArea::OverviewRegion);
    F(kind);
    F(tris);
    DUMP_END;
}
std::string R(const MapArea::MapOccluder& o) {
    DUMP_BEGIN(MapArea::MapOccluder);
    F(name);
    F(tris);
    DUMP_END;
}
std::string R(const MapStyle& o) {
    DUMP_BEGIN(MapStyle);
    F(opacity);
    C(room_fill);
    F(room_class_color);
    C(border);
    F(revealed_dim);
    F(visited_gain);
    C(visited_tint);
    F(visited_amount);
    F(hazard_tint);
    F(water_tint);
    F(fade_ticks);
    F(item_icon);
    F(door_icon);
    F(marker_icon);
    F(raster_px);
    F(reveal_required);
    C(wall_color);
    F(category_dim_before_unlock);
    F(hazard_gain);
    F(water_gain);
    F(water_clip_live);
    F(water_full_visible);
    C(occluder_color);
    F(grid_tile_world_size);
    F(marker_colors);
    C(marker_back_color);
    C(marker_glyph_color);
    C(marker_pulse_color);
    F(marker_pulse_gain);
    F(marker_pulse_peak);
    F(marker_pulse_period);
    F(custom_marker_icon);
    F(custom_marker_back_icon);
    F(door_prefix);
    F(structural_prefixes);
    F(door_closed_suffix);
    F(door_open_suffix);
    F(door_opened_left);
    F(door_opened_right);
    F(collected_suffix);
    F(collected_fallback);
    F(collectible_kind);
    F(item_blink_period);
    F(item_blink_low);
    F(player_blink_period);
    F(player_blink_low);
    F(pin_outer);
    F(pin_inner);
    F(pin_core);
    DUMP_END;
}
std::string R(const CompositeLayer& o) {
    DUMP_BEGIN(CompositeLayer);
    F(src);
    F(rect);
    F(src_rect);
    F(src_px);
    F(mask_src);
    F(mask_src_rect);
    F(show_bind);
    F(hide_bind);
    F(fade_ms);
    F(opacity);
    DUMP_END;
}
std::string R(const CompositeDef& o) {
    DUMP_BEGIN(CompositeDef);
    F(name);
    F(w);
    F(h);
    C(background);
    F(levels);
    F(flat);
    // Layers go on their own lines (DumpManifest); here only their count.
    dd.out += fmt::format("{}layers#={}", dd.out.empty() ? "" : " ", o.layers.size());
    DUMP_END;
}
std::string R(const MsbtConfig& o) {
    DUMP_BEGIN(MsbtConfig);
    F(files);
    F(langs);
    F(fallback);
    F(has_fallback);
    F(icon_font);
    F(icon_group);
    F(icon_type);
    F(icon_glyphs);
    DUMP_END;
}
std::string R(const HapticsConfig& o) {
    DUMP_BEGIN(HapticsConfig);
    F(enabled);
    F(respect_system);
    F(strength);
    DUMP_END;
}
std::string R(const NavConfig& o) {
    DUMP_BEGIN(NavConfig);
    F(enabled);
    F(toggle);
    F(toggle_mask);
    C(color);
    F(frame);
    F(src);
    F(haptic);
    DUMP_END;
}
std::string R(const CallStep& o) {
    DUMP_BEGIN(CallStep);
    F(fn);
    F(args);
    F(ret_float);
    F(save);
    F(out);
    F(out_addr);
    F(out_text);
    F(has_expect);
    F(expect);
    F(else_step);
    F(has_reject);
    F(reject);
    DUMP_END;
}
std::string R(const CallSequence& o) {
    DUMP_BEGIN(CallSequence);
    F(steps);
    F(out);
    F(out_float);
    F(every_ms);
    F(flag);
    DUMP_END;
}
std::string R(const GuestPatch& o) {
    DUMP_BEGIN(GuestPatch);
    F(at);
    F(words);
    F(why);
    F(optional);
    DUMP_END;
}
std::string R(const EnforceRule& o) {
    DUMP_BEGIN(EnforceRule);
    F(action);
    F(every_ms);
    F(flag);
    F(flag_value);
    DUMP_END;
}
std::string R(const SpyPoint& o) {
    DUMP_BEGIN(SpyPoint);
    F(name);
    F(symbol);
    F(reg);
    DUMP_END;
}
std::string R(const Il2CppLayout& o) {
    DUMP_BEGIN(Il2CppLayout);
    F(name);
    F(methods);
    F(method_count);
    DUMP_END;
}
std::string R(const Manifest::Symbol& o) {
    DUMP_BEGIN(Manifest::Symbol);
    F(class_name);
    F(find);
    F(address);
    F(class_slot);
    F(method);
    DUMP_END;
}

/// A map area's own fields (its lists get one line per entry in DumpManifest).
std::string AreaHeader(const MapArea& o) {
    DUMP_BEGIN(MapArea);
    F(geo);
    F(image);
    F(no_pin);
    F(follow_pad);
    F(clamp_view);
    F(min_x);
    F(min_y);
    F(max_x);
    F(max_y);
    F(camera_rects);
    dd.out += fmt::format(
        " overview_regions#={} markers#={} dynamic_markers#={} labels#={} "
        "layers#={} room_categories#={} occluders#={} vignettes#={}",
        o.overview_regions ? fmt::format("{}", o.overview_regions->size()) : std::string{"none"},
        o.markers.size(), o.dynamic_markers.size(), o.labels.size(), o.layers.size(),
        o.room_categories.size(), o.occluders.size(), o.vignettes.size());
    DUMP_END;
}
/// A page's own fields (widgets and scroll regions get one line each in DumpManifest).
std::string PageHeader(const Page& o) {
    DUMP_BEGIN(Page);
    F(id);
    F(title);
    F(mirror);
    F(mirror_rect);
    F(no_auto_leave);
    F(nav_order);
    dd.out += fmt::format(" widgets#={} scrolls#={}", o.widgets.size(), o.scrolls.size());
    DUMP_END;
}

/// Sorted keys of an unordered map.
template <typename M>
std::vector<typename M::key_type> SortedKeys(const M& m) {
    std::vector<typename M::key_type> keys;
    keys.reserve(m.size());
    for (const auto& [k, v] : m) {
        keys.push_back(k);
    }
    std::ranges::sort(keys);
    return keys;
}

/// The whole parsed manifest as deterministic text.
std::string DumpManifest(const Manifest& o) {
    std::string text;
    const auto line = [&text](const std::string& key, const std::string& value) {
        text += key + " = " + value + "\n";
    };
    {
        // Top-level scalars (the containers get their own lines below).
        DUMP_BEGIN(Manifest);
        F(format);
        F(title_id);
        F(running_build_id);
        F(name);
        F(mod_dir_name);
        F(data_file);
        F(poll_hz);
        F(canvas_w);
        F(canvas_h);
        C(background);
        F(enforce_gate);
        F(enforce_gate_max);
        F(gameplay_point);
        F(map_areas_src);
        F(icon_atlas);
        F(icon_cell);
        F(icon_cols);
        F(il2cpp);
        F(metadata_anchor);
        F(frame_hook);
        F(frame_hook_symbol);
        F(build_id_file);
        F(debug_page);
        F(font_metrics_src);
        F(font_atlas_src);
        F(font_page_h);
        F(msbt);
        F(haptics);
        F(nav);
        F(anim_hz);
        F(find_spec);
        F(trace_target);
        F(dump_registry);
        F(uses_clock_keys);
        F(describe_string);
        F(valid);
        dd.out += fmt::format(" asset_dir={}", o.asset_dir ? "set" : "null");
        line("manifest", dd.Done());
    }
    line("counts",
         fmt::format("pages={} tables={} sprite_map={} map_rooms={} zone_area={} map_areas={} "
                     "icon_cells={} icon_ink={} flags={} actions={} sequences={} points={} "
                     "derived={} page_binds={} enforce={} spies={} patches={} symbols={} "
                     "composites={}",
                     o.pages.size(), o.tables.size(), o.sprite_map.size(), o.map_rooms.size(),
                     o.zone_area.size(), o.map_areas.size(), o.icon_cells.size(), o.icon_ink.size(),
                     o.flag_defaults.size(), o.actions.size(), o.sequences.size(), o.points.size(),
                     o.derived.size(), o.page_binds.size(), o.enforce.size(), o.spies.size(),
                     o.patches.size(), o.symbols.size(), o.composites.size()));
    line("map_style", R(o.map_style));
    for (const auto& [k, v] : o.tables) {
        line("tables." + k, R(v));
    }
    for (const auto& k : SortedKeys(o.sprite_map)) {
        line("sprite_map." + k, R(o.sprite_map.at(k)));
    }
    for (const auto& k : SortedKeys(o.map_rooms)) {
        line("map_rooms." + k, R(o.map_rooms.at(k)));
    }
    for (const auto& k : SortedKeys(o.zone_area)) {
        line(fmt::format("zone_area.{}", k), R(o.zone_area.at(k)));
    }
    for (const auto& k : SortedKeys(o.icon_cells)) {
        line("icon_cells." + k, R(o.icon_cells.at(k)));
    }
    for (const auto& k : SortedKeys(o.icon_ink)) {
        line("icon_ink." + k, R(o.icon_ink.at(k)));
    }
    for (const auto& name : SortedKeys(o.map_areas)) {
        const MapArea& a = o.map_areas.at(name);
        const std::string p = "map_areas." + name;
        line(p, AreaHeader(a));
        if (a.overview_regions) {
            for (size_t i = 0; i < a.overview_regions->size(); ++i) {
                line(fmt::format("{}.overview_regions[{}]", p, i), R((*a.overview_regions)[i]));
            }
        }
        for (size_t i = 0; i < a.markers.size(); ++i) {
            line(fmt::format("{}.markers[{}]", p, i), R(a.markers[i]));
        }
        for (size_t i = 0; i < a.dynamic_markers.size(); ++i) {
            line(fmt::format("{}.dynamic_markers[{}]", p, i), R(a.dynamic_markers[i]));
        }
        for (size_t i = 0; i < a.labels.size(); ++i) {
            line(fmt::format("{}.labels[{}]", p, i), R(a.labels[i]));
        }
        for (size_t i = 0; i < a.layers.size(); ++i) {
            line(fmt::format("{}.layers[{}]", p, i), R(a.layers[i]));
        }
        for (size_t i = 0; i < a.room_categories.size(); ++i) {
            line(fmt::format("{}.room_categories[{}]", p, i), R(a.room_categories[i]));
        }
        for (size_t i = 0; i < a.occluders.size(); ++i) {
            line(fmt::format("{}.occluders[{}]", p, i), R(a.occluders[i]));
        }
        for (size_t i = 0; i < a.vignettes.size(); ++i) {
            line(fmt::format("{}.vignettes[{}]", p, i), R(a.vignettes[i]));
        }
    }
    for (const auto& k : SortedKeys(o.flag_defaults)) {
        line("flags." + k, R(o.flag_defaults.at(k)));
    }
    if (!o.persist_flags.empty()) { // printed only when present: older goldens stay unchanged
        std::string names;
        for (const auto& k : o.persist_flags) {
            names += (names.empty() ? "" : ",") + k;
        }
        line("persist_flags", names);
    }
    for (const auto& k : SortedKeys(o.actions)) {
        line("actions." + k, R(o.actions.at(k)));
    }
    for (const auto& k : SortedKeys(o.sequences)) {
        line("sequences." + k, R(o.sequences.at(k)));
    }
    for (const auto& k : SortedKeys(o.points)) {
        line("points." + k, R(o.points.at(k)));
    }
    for (size_t i = 0; i < o.derived.size(); ++i) {
        line(fmt::format("derived[{}]", i), R(o.derived[i]));
    }
    for (size_t i = 0; i < o.page_binds.size(); ++i) {
        line(fmt::format("page_binds[{}]", i), R(o.page_binds[i]));
    }
    for (size_t i = 0; i < o.enforce.size(); ++i) {
        line(fmt::format("enforce[{}]", i), R(o.enforce[i]));
    }
    for (size_t i = 0; i < o.spies.size(); ++i) {
        line(fmt::format("spies[{}]", i), R(o.spies[i]));
    }
    for (size_t i = 0; i < o.patches.size(); ++i) {
        line(fmt::format("patches[{}]", i), R(o.patches[i]));
    }
    for (const auto& k : SortedKeys(o.symbols)) {
        line("symbols." + k, R(o.symbols.at(k)));
    }
    for (const auto& [k, def] : o.composites) {
        line("composites." + k, R(def));
        if (def) {
            for (size_t i = 0; i < def->layers.size(); ++i) {
                line(fmt::format("composites.{}.layers[{}]", k, i), R(def->layers[i]));
            }
        }
    }
    for (size_t pi = 0; pi < o.pages.size(); ++pi) {
        const Page& page = o.pages[pi];
        const std::string p = fmt::format("pages[{}]", pi);
        line(p, PageHeader(page));
        for (size_t i = 0; i < page.scrolls.size(); ++i) {
            line(fmt::format("{}.scrolls[{}]", p, i), R(page.scrolls[i]));
        }
        for (size_t i = 0; i < page.widgets.size(); ++i) {
            line(fmt::format("{}.widgets[{}]", p, i), R(page.widgets[i]));
        }
    }
    return text;
}

#undef F
#undef C

/// Guards the dumper's field lists: a struct whose size changed probably gained a field the
/// dumper does not print yet. Update the matching R() and the size here together.
void CheckLayouts() {
#if defined(__GLIBCXX__) && defined(__x86_64__)
    struct Size {
        const char* name;
        size_t actual;
        size_t expected;
    };
    // clang-format off
    const Size sizes[] = {
        {"Manifest", sizeof(Manifest), 2448},
        {"Page", sizeof(Page), 160},
        {"Widget", sizeof(Widget), 2208},
        {"ChartSpec", sizeof(ChartSpec), 64},
        {"ViewDefaultBinds", sizeof(ViewDefaultBinds), 128},
        {"ScrollRegion", sizeof(ScrollRegion), 256},
        {"MapWidgetExtras", sizeof(MapWidgetExtras), 448},
        {"MapWidgetExtras::Group", sizeof(MapWidgetExtras::Group), 112},
        {"MapWidgetExtras::LabelStyle", sizeof(MapWidgetExtras::LabelStyle), 24},
        {"MapWidgetExtras::Overlay", sizeof(MapWidgetExtras::Overlay), 168},
        {"WidgetAnim", sizeof(WidgetAnim), 136},
        {"Action", sizeof(Action), 816},
        {"ActionValue", sizeof(ActionValue), 56},
        {"DataPoint", sizeof(DataPoint), 368},
        {"ChainHop", sizeof(ChainHop), 88},
        {"PatternFind", sizeof(PatternFind), 64},
        {"ArrayFind", sizeof(ArrayFind), 32},
        {"PlayerFind", sizeof(PlayerFind), 24},
        {"TextScan", sizeof(TextScan), 32},
        {"DerivedPoint", sizeof(DerivedPoint), 568},
        {"DerivedPoint::CmpOperand", sizeof(DerivedPoint::CmpOperand), 48},
        {"MapRoom", sizeof(MapRoom), 80},
        {"MapArea", sizeof(MapArea), 320},
        {"MapMarker", sizeof(MapMarker), 400},
        {"DynamicMarkerDef", sizeof(DynamicMarkerDef), 656},
        {"MapLabel", sizeof(MapLabel), 216},
        {"MapLayer", sizeof(MapLayer), 168},
        {"MapRoomCategory", sizeof(MapRoomCategory), 192},
        {"MapArea::OverviewRegion", sizeof(MapArea::OverviewRegion), 56},
        {"MapArea::MapOccluder", sizeof(MapArea::MapOccluder), 56},
        {"MapStyle", sizeof(MapStyle), 520},
        {"CompositeDef", sizeof(CompositeDef), 104},
        {"CompositeLayer", sizeof(CompositeLayer), 200},
        {"MsbtConfig", sizeof(MsbtConfig), 264},
        {"HapticsConfig", sizeof(HapticsConfig), 11},
        {"NavConfig", sizeof(NavConfig), 96},
        {"PageBind", sizeof(PageBind), 240},
        {"PageBindTarget", sizeof(PageBindTarget), 80},
        {"CallStep", sizeof(CallStep), 232},
        {"CallSequence", sizeof(CallSequence), 96},
        {"GuestPatch", sizeof(GuestPatch), 72},
        {"EnforceRule", sizeof(EnforceRule), 80},
        {"SpyPoint", sizeof(SpyPoint), 72},
        {"Il2CppLayout", sizeof(Il2CppLayout), 24},
        {"Manifest::Symbol", sizeof(Manifest::Symbol), 144},
    };
    // clang-format on
    std::string report;
    for (const auto& s : sizes) {
        if (s.actual != s.expected) {
            report += fmt::format("{}: sizeof {} (was {})\n", s.name, s.actual, s.expected);
        }
    }
    INFO("A parsed-model struct changed size. If it gained a field, add the field to its R() in "
         "manifest_digest.cpp, update the size here, and regenerate the goldens "
         "(EDEN_DSMOD_GOLDEN_UPDATE=1).\n"
         << report);
    CHECK(report.empty());
#endif
}

/// V1 digest of one package: raw-JSON checks, the parsed model, and data-file fingerprints.
std::string DigestPackage(const std::string& name, const std::filesystem::path& root) {
    const auto ds = root / "dualscreen";
    const auto raw = DsmodGolden::ReadFile(ds / "manifest.json");
    REQUIRE(raw.has_value());
    const auto json = nlohmann::json::parse(*raw);
    std::optional<nlohmann::json> package;
    if (const auto p = DsmodGolden::ReadFile(root / "package.json")) {
        package = nlohmann::json::parse(*p);
    }
    std::string text = "# DSMod V1 manifest digest: " + name +
                       " (generated by src/tests/core/mods/manifest_digest.cpp; do not edit)\n";
    text += fmt::format("package_json = {}\n", package ? "present" : "absent");
    text += fmt::format(
        "min_runtime = {} (manifest {}, package.json {})\n",
        PackageMinRuntime(&json, package ? &*package : nullptr), PackageMinRuntime(&json, nullptr),
        package ? fmt::format("{}", PackageMinRuntime(nullptr, &*package)) : std::string{"-"});
    text += fmt::format("usable = {}\n", R(IsUsableDualScreenManifest(json)));
    Manifest manifest;
    const bool parsed = ParseDualScreenManifest(json, manifest);
    text += fmt::format("parsed = {}\n", R(parsed));
    text += DumpManifest(manifest);
    // Per-build data files: fingerprinted only (their parse lives in ModRuntime::Discover).
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator{ds}) {
        const auto file = entry.path().filename().string();
        if (entry.is_regular_file() && file != "manifest.json" && file.ends_with(".json")) {
            files.push_back(file);
        }
    }
    std::ranges::sort(files);
    for (const auto& file : files) {
        const auto bytes = DsmodGolden::ReadFile(ds / file);
        std::string canonical = "unparseable";
        try {
            canonical = nlohmann::json::parse(*bytes).dump();
        } catch (const std::exception&) {
        }
        const auto dj = nlohmann::json::parse(*bytes, nullptr, false);
        std::string keys;
        if (dj.is_object()) {
            for (const auto& [k, v] : dj.items()) {
                keys += fmt::format("{}{}#{}", keys.empty() ? "" : ",", k,
                                    v.is_structured() ? v.size() : 1);
            }
        }
        text += fmt::format("data_file.{} = {{canonical_fnv64={} keys=[{}]}}\n", file,
                            DsmodGolden::Hex64(DsmodGolden::Fnv64(canonical)), keys);
    }
    return text;
}

} // namespace

TEST_CASE("DSMod golden: parsed-model layouts match the digest dumper", "[dsmod][golden]") {
    CheckLayouts();
}

TEST_CASE("DSMod golden: manifest digest of every published package", "[dsmod][golden][digest]") {
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
            const std::string text = DigestPackage(name, *root);
            // Deterministic within the process: a second parse prints the same text.
            REQUIRE(DigestPackage(name, *root) == text);
            const auto golden = DsmodGolden::GoldenDir() / name / "manifest.txt.zst";
            const auto actual = DsmodGolden::ActualDir() / name / "manifest.txt";
            const std::string diff = DsmodGolden::CompareText(golden, text, actual);
            INFO(diff);
            CHECK(diff.empty());
        }
    }
}

TEST_CASE("DSMod runtime18 golden digest includes new non-default layout fields", "[dsmod][runtime18][digest]") {
    Widget w; w.fit_text = true; w.text_min_scale = 2; w.text_center_h = 30;
    const auto widget = R(w);
    REQUIRE(widget.find("fit_text=") != std::string::npos);
    REQUIRE(widget.find("text_min_scale=") != std::string::npos);
    REQUIRE(widget.find("text_center_h=") != std::string::npos);
    ScrollRegion scroll; scroll.bar_src = "module:thumb"; scroll.bar_track_src = "module:track";
    REQUIRE(R(scroll).find("bar_src=") != std::string::npos);
    REQUIRE(R(scroll).find("bar_track_src=") != std::string::npos);
    DynamicMarkerDef marker; marker.size_max = 16.0f;
    REQUIRE(R(marker).find("size_max=") != std::string::npos);
}

TEST_CASE("DSMod map digest retains non-default heading tint and detail references",
          "[dsmod][golden][digest]") {
    MapWidgetExtras extras;
    extras.marker_rotate_bind = "player.heading";
    extras.marker_tint = 0xFFFFCC00u;
    MapWidgetExtras::Overlay overlay;
    overlay.src_detail_bind = "tile.detail";
    overlay.detail_threshold = 300.0f;
    extras.overlays.push_back(overlay);
    const auto text = R(extras);
    REQUIRE(text.find("player.heading") != std::string::npos);
    REQUIRE(text.find("marker_tint=") != std::string::npos);
    REQUIRE(text.find("tile.detail") != std::string::npos);
    REQUIRE(text.find("detail_threshold=") != std::string::npos);
}

TEST_CASE("DSMod map label overlap parser defaults and digest",
          "[dsmod][digest][map-label-overlap]") {
    const auto json = nlohmann::json::parse(R"({"pages":[{"id":"p","widgets":[
        {"type":"map","rect":[0,0,100,100],"label_style":{}},
        {"type":"map","rect":[0,0,100,100],"label_style":{"avoid_overlap":true}},
        {"type":"map","rect":[0,0,100,100],"label_style":{"avoid_overlap":false}}
    ]}]})");
    Manifest manifest;
    REQUIRE(ParseDualScreenManifest(json, manifest));
    const auto& widgets = manifest.pages[0].widgets;
    REQUIRE_FALSE(widgets[0].map_extras->label_style.avoid_overlap);
    REQUIRE(widgets[1].map_extras->label_style.avoid_overlap);
    REQUIRE_FALSE(widgets[2].map_extras->label_style.avoid_overlap);
    REQUIRE(R(widgets[0].map_extras->label_style).find("avoid_overlap=") == std::string::npos);
    REQUIRE(R(widgets[1].map_extras->label_style).find("avoid_overlap=true") != std::string::npos);
}
