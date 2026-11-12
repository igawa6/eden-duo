// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Bound default view of a pan_zoom widget (runtime 15; widget keys
// "view_zoom_bind", "view_cx_bind", "view_cy_bind", "view_reset_bind", see ViewDefaultBinds in
// mod_types_page.h). The map widget has its own bound base view (mod_map_view_rect.h); this one is
// for the other pan_zoom widgets, whose view is a window onto their picture (RenderPage /
// DrawImage, mod_ui.cpp): zoom z shows 1/z of the picture, pan (px, py) in widget pixels at zoom 1
// names the top-left of that window, clamped to [0, size * (1 - 1/z)] -- or, with an Image
// "pan_bounds" {u0, v0, u1, v1}, to the part of the picture those bounds name (ClampPanAxis; the
// same clamp is applied by DrawImage, by the default view below and to the pan the gestures store).
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

/// Image "pan_bounds" {u0, v0, u1, v1} (Widget::pan_bounds): the whole picture when unset.
using PanBounds = std::array<float, 4>;
inline constexpr PanBounds WholePicture{0.0f, 0.0f, 1.0f, 1.0f};

/// Clamps a pan_zoom window's origin along one axis. `pan` is in widget px at zoom 1 (`size` px
/// = the whole src_rect span along this axis), the window spans size / zoom of it, and [b0, b1]
/// (fractions of that span, 0..1) is the part of the picture the window may show. A window that
/// fits inside the bounds is kept within them (origin in [b0, b1 - w]); one wider than the bounds
/// is centred on them (origin = (b0 + b1) / 2 - w / 2), while still kept on the picture
/// ([0, 1 - w]). With the whole picture as bounds this is the classic clamp to [0, size * (1 -
/// 1 / zoom)].
[[nodiscard]] inline float ClampPanAxis(float pan, s32 size, float zoom, float b0, float b1) {
    if (size <= 0) {
        return 0.0f;
    }
    const float s = static_cast<float>(size);
    const float window = s / std::max(zoom, 0.01f);
    const float lo = b0 * s;
    const float span = (b1 - b0) * s;
    float origin;
    if (window <= span) {
        origin = std::clamp(pan, lo, lo + span - window);
    } else {
        origin = lo + (span - window) * 0.5f;
    }
    return std::min(std::max(origin, 0.0f), std::max(0.0f, s - window));
}

/// Both axes of a widget's pan at `zoom`: its pan_bounds, else the whole picture.
inline void ClampViewPan(const Widget& w, float zoom, float& pan_x, float& pan_y) {
    const PanBounds& b = w.pan_bounds ? *w.pan_bounds : WholePicture;
    pan_x = ClampPanAxis(pan_x, w.rect[2], zoom, b[0], b[2]);
    pan_y = ClampPanAxis(pan_y, w.rect[3], zoom, b[1], b[3]);
}

/// Keeps a stored view inside the widget's pan_bounds, so a drag past the edge does not pile up
/// overshoot the user then has to drag back through. A widget without pan_bounds is left alone
/// (its pan is clamped when drawn, as before).
inline void ClampViewToBounds(const Widget& w, ViewTransform& v) {
    if (w.pan_bounds) {
        ClampViewPan(w, v.zoom, v.pan_x, v.pan_y);
    }
}

/// The default view of a rw x rh widget for the bound (zoom, cx, cy); nullopt when a value is
/// missing or not finite, or the widget has no size. `bounds` = the widget's pan_bounds (the
/// window is kept inside them, or centred on them when wider), unset = the whole picture.
[[nodiscard]] inline std::optional<ViewHome> DefaultView(
    s32 rw, s32 rh, std::optional<f64> zoom, std::optional<f64> cx, std::optional<f64> cy,
    float min_zoom, float max_zoom, const std::optional<PanBounds>& bounds = std::nullopt) {
    if (rw <= 0 || rh <= 0 || !zoom || !cx || !cy || !std::isfinite(*zoom) ||
        !std::isfinite(*cx) || !std::isfinite(*cy) || !(*zoom > 0.0)) {
        return std::nullopt;
    }
    const float lo = std::max(min_zoom, 0.01f);
    const float hi = std::max(lo, max_zoom);
    const float z = std::clamp(static_cast<float>(*zoom), lo, hi);
    const PanBounds& b = bounds ? *bounds : WholePicture;
    const auto pan = [z](f64 c, s32 size, float b0, float b1) {
        const float s = static_cast<float>(size);
        const float p = (static_cast<float>(c) - 0.5f / z) * s;
        return ClampPanAxis(p, size, z, b0, b1);
    };
    return ViewHome{z, pan(*cx, rw, b[0], b[2]), pan(*cy, rh, b[1], b[3])};
}

/// The view is away from its home (or a reset is gliding): "view_custom:<id>".
[[nodiscard]] inline bool ViewAwayFromHome(const ViewTransform& v) {
    return v.gliding || std::fabs(v.zoom - v.home_zoom) > 0.001f ||
           std::fabs(v.pan_x - v.home_pan_x) > 0.5f || std::fabs(v.pan_y - v.home_pan_y) > 0.5f;
}

/// ViewAwayFromHome as the widget is drawn: with pan_bounds, both pans are compared clamped to
/// them, so a view at the bounds' edge is "home" when its home is there too.
[[nodiscard]] inline bool ViewAwayFromHome(const ViewTransform& v, const Widget& w) {
    if (!w.pan_bounds) {
        return ViewAwayFromHome(v);
    }
    ViewTransform c = v;
    ClampViewPan(w, c.zoom, c.pan_x, c.pan_y);
    ClampViewPan(w, c.home_zoom, c.home_pan_x, c.home_pan_y);
    return ViewAwayFromHome(c);
}

/// Starts a glide back to the view's home (view_reset, view_idle_ms).
inline void GlideViewHome(ViewTransform& v) {
    v.gliding = true;
    v.goal_zoom = v.home_zoom;
    v.goal_pan_x = v.home_pan_x;
    v.goal_pan_y = v.home_pan_y;
}

/// Starts a glide that zooms `v` by `factor` about its OWN current centre (runtime 25,
/// view_zoom_step): the same anchor math the live pinch applies (mod_input.cpp UpdateGestures),
/// but anchored at the widget's centre instead of wherever the two fingers met, so whatever is
/// centred in the view now stays centred. `factor` > 1 zooms in, < 1 zooms out; the caller
/// (RunAction, mod_actions.cpp) has already refused a non-positive one, so this always produces a
/// usable result. The new zoom is clamped to [min_zoom, max_zoom] (ZoomLimits, mod_input.cpp,
/// already folds in a map's own further-out pinch floor); the new pan is then clamped to `w`'s
/// pan_bounds when it has any (ClampViewPan) -- the same two clamps a live pinch is subject to, so
/// a button press behaves exactly like a pinch of that size centred on the view.
inline void GlideViewZoomStep(ViewTransform& v, const Widget& w, float factor, float min_zoom,
                              float max_zoom) {
    const float lo = std::max(min_zoom, 0.01f);
    const float hi = std::max(lo, max_zoom);
    const float before = v.zoom;
    const float after = std::clamp(before * factor, lo, hi);
    const float ox = static_cast<float>(w.rect[2]) * 0.5f;
    const float oy = static_cast<float>(w.rect[3]) * 0.5f;
    const float anchor_x = v.pan_x + ox / before;
    const float anchor_y = v.pan_y + oy / before;
    float goal_x = anchor_x - ox / after;
    float goal_y = anchor_y - oy / after;
    if (w.pan_bounds) {
        ClampViewPan(w, after, goal_x, goal_y);
    }
    v.gliding = true;
    v.goal_zoom = after;
    v.goal_pan_x = goal_x;
    v.goal_pan_y = goal_y;
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
