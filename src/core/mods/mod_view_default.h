// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bound default view of a pan_zoom widget (unreleased runtime 15 addition; widget keys
// "view_zoom_bind", "view_cx_bind", "view_cy_bind", "view_reset_bind", see ViewDefaultBinds in
// mod_types_page.h). The map widget has its own bound base view (mod_map_view_rect.h); this one is
// for the other pan_zoom widgets, whose view is a window onto their picture (RenderPage /
// DrawImage, mod_ui.cpp): zoom z shows 1/z of the picture, pan (px, py) in widget pixels at zoom 1
// names the top-left of that window, clamped to [0, size * (1 - 1/z)].
//   - The default view is zoom = the bound zoom (clamped to the widget's min/max zoom) with the
//     window centred on the bound point (cx, cy in 0..1 of the picture), clamped like a pan.
//   - It is the view's "home": a new view starts there; view_reset and view_idle_ms glide back to
//     it; "view_custom:<id>" is 1 while the view is away from it.
//   - When the bound default changes, a view that is at its old home (not moved by the user, no
//     reset gliding) snaps to the new one; a user-moved view stays where the user left it until
//     the "view_reset_bind" value changes, which snaps any view home.
// Kept free of ModRuntime so it can be unit tested (src/tests/core/mods/runtime15.cpp).

#include <algorithm>
#include <cmath>
#include <optional>

#include "common/common_types.h"
#include "core/mods/mod_types_page.h"

namespace Core::Mods {

struct ViewHome {
    float zoom{1.0f};
    float pan_x{0.0f};
    float pan_y{0.0f};
};

/// The default view of a rw x rh widget for the bound (zoom, cx, cy); nullopt when a value is
/// missing or not finite, or the widget has no size.
[[nodiscard]] inline std::optional<ViewHome> DefaultView(s32 rw, s32 rh, std::optional<f64> zoom,
                                                         std::optional<f64> cx,
                                                         std::optional<f64> cy, float min_zoom,
                                                         float max_zoom) {
    if (rw <= 0 || rh <= 0 || !zoom || !cx || !cy || !std::isfinite(*zoom) ||
        !std::isfinite(*cx) || !std::isfinite(*cy) || !(*zoom > 0.0)) {
        return std::nullopt;
    }
    const float lo = std::max(min_zoom, 0.01f);
    const float hi = std::max(lo, max_zoom);
    const float z = std::clamp(static_cast<float>(*zoom), lo, hi);
    const auto pan = [z](f64 c, s32 size) {
        const float s = static_cast<float>(size);
        const float p = (static_cast<float>(c) - 0.5f / z) * s;
        return std::clamp(p, 0.0f, std::max(0.0f, s * (1.0f - 1.0f / z)));
    };
    return ViewHome{z, pan(*cx, rw), pan(*cy, rh)};
}

/// The view is away from its home (or a reset is gliding): "view_custom:<id>".
[[nodiscard]] inline bool ViewAwayFromHome(const ViewTransform& v) {
    return v.gliding || std::fabs(v.zoom - v.home_zoom) > 0.001f ||
           std::fabs(v.pan_x - v.home_pan_x) > 0.5f || std::fabs(v.pan_y - v.home_pan_y) > 0.5f;
}

/// Starts a glide back to the view's home (view_reset, view_idle_ms).
inline void GlideViewHome(ViewTransform& v) {
    v.gliding = true;
    v.goal_zoom = v.home_zoom;
    v.goal_pan_x = v.home_pan_x;
    v.goal_pan_y = v.home_pan_y;
}

/// One tick of the bound default for a view: `home` = this tick's default (nullopt = none:
/// zoom 1, no pan), `reset` = the view_reset_bind value (nullopt = unbound / missing), `held` =
/// a finger is on the view. Returns true when the view changed.
inline bool ApplyViewDefault(ViewTransform& v, const std::optional<ViewHome>& home,
                             std::optional<f64> reset, bool held) {
    const ViewHome h = home.value_or(ViewHome{});
    const bool reset_changed = reset && v.has_reset_value && *reset != v.reset_value;
    if (reset) {
        v.has_reset_value = true;
        v.reset_value = *reset;
    }
    const bool moved_home = std::fabs(h.zoom - v.home_zoom) > 1e-6f ||
                            std::fabs(h.pan_x - v.home_pan_x) > 1e-3f ||
                            std::fabs(h.pan_y - v.home_pan_y) > 1e-3f;
    if (!moved_home && !reset_changed) {
        return false;
    }
    const bool follow = reset_changed || (!held && !ViewAwayFromHome(v));
    v.home_zoom = h.zoom;
    v.home_pan_x = h.pan_x;
    v.home_pan_y = h.pan_y;
    if (follow) {
        v.zoom = h.zoom;
        v.pan_x = h.pan_x;
        v.pan_y = h.pan_y;
        v.gliding = false;
    }
    return true;
}

} // namespace Core::Mods
