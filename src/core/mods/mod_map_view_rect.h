// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Map widget bound default view (runtime 14): "view_rect_x0_bind" .. "view_rect_y1_bind" name
// four published values (world units, the area's min/max space) and "view_rect_pad" pads them.
// When all four resolve, the map's BASE view (view zoom 1, pan 0) is that rect fitted into the
// widget (uniform scale, centred) instead of the whole-area fit; pan / pinch apply on top as
// before. The pinch may zoom OUT from the rect fit down to exactly the whole-area fit
// (RectMinZoom). A new rect (the player entered another room) glides the base view there
// (StepBaseGlide) unless the user's own view is in effect. Kept free of the renderer so it can
// be unit tested (src/tests/core/mods/runtime14.cpp); used by GeometryMapDraw::FrameView
// (mod_ui_map_widget.cpp) and ModRuntime::ZoomLimits (mod_input.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "common/common_types.h"

namespace Core::Mods {

/// A map's base view: the world point at the widget centre and canvas px per world unit.
struct MapBaseView {
    float cx{};
    float cy{};
    float ppw{};
};

/// The four bound values (any order per axis) grown by `pad` on every side, as
/// {min_x, min_y, max_x, max_y}; nullopt when a value is missing or not finite, or the padded rect
/// has no area.
[[nodiscard]] inline std::optional<std::array<float, 4>> NormaliseViewRect(std::optional<f64> x0,
                                                                           std::optional<f64> y0,
                                                                           std::optional<f64> x1,
                                                                           std::optional<f64> y1,
                                                                           float pad) {
    if (!x0 || !y0 || !x1 || !y1 || !std::isfinite(*x0) || !std::isfinite(*y0) ||
        !std::isfinite(*x1) || !std::isfinite(*y1) || !std::isfinite(pad)) {
        return std::nullopt;
    }
    const float ax = static_cast<float>(std::min(*x0, *x1)) - pad;
    const float bx = static_cast<float>(std::max(*x0, *x1)) + pad;
    const float ay = static_cast<float>(std::min(*y0, *y1)) - pad;
    const float by = static_cast<float>(std::max(*y0, *y1)) + pad;
    if (!(bx - ax > 0.0f) || !(by - ay > 0.0f)) {
        return std::nullopt;
    }
    return std::array<float, 4>{ax, ay, bx, by};
}

/// {min_x, min_y, max_x, max_y} fitted into a rw x rh widget: centred, one uniform scale
/// min(rw / width, rh / height) -- the whole rect visible, its proportions kept.
[[nodiscard]] inline MapBaseView FitWorldRect(const std::array<float, 4>& r, s32 rw, s32 rh) {
    const float w = r[2] - r[0];
    const float h = r[3] - r[1];
    return {(r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f,
            std::min(static_cast<float>(rw) / w, static_cast<float>(rh) / h)};
}

/// The pinch's lower zoom limit, relative to a rect-fit base view of `base_ppw`: exactly the
/// whole-area fit (`area_ppw`), never further out. A rect that already shows the whole area
/// (ratio >= 1) keeps the widget's own limit, capped at 1.
[[nodiscard]] inline float RectMinZoom(float area_ppw, float base_ppw, float widget_min_zoom) {
    if (!(base_ppw > 0.0f) || !(area_ppw > 0.0f)) {
        return widget_min_zoom;
    }
    const float ratio = area_ppw / base_ppw;
    return ratio < 1.0f ? ratio : std::min(widget_min_zoom, 1.0f);
}

/// One glide step of the shown base view toward `target` (a quarter of the way per frame, like
/// the follow glide), snapping once close. `hold`: the user's own view is in effect (dragged,
/// pinched, a reset gliding) -- the base holds still so the map stays as they left it. Returns
/// true while still moving (the caller keeps redrawing).
inline bool StepBaseGlide(MapBaseView& shown, const MapBaseView& target, bool hold) {
    if (hold) {
        return false;
    }
    shown.cx += (target.cx - shown.cx) * 0.25f;
    shown.cy += (target.cy - shown.cy) * 0.25f;
    shown.ppw += (target.ppw - shown.ppw) * 0.25f;
    const float ppw = std::max(target.ppw, 1e-6f);
    if ((std::fabs(target.cx - shown.cx) + std::fabs(target.cy - shown.cy)) * ppw > 0.5f ||
        std::fabs(target.ppw - shown.ppw) / ppw > 0.002f) {
        return true;
    }
    shown = target;
    return false;
}

/// The view the user moved away from the base view (or a reset still gliding): the same test as
/// the published "view_custom:<id>".
[[nodiscard]] inline bool MapViewCustom(float zoom, float pan_x, float pan_y, bool gliding) {
    return gliding || std::fabs(zoom - 1.0f) > 0.001f || std::fabs(pan_x) > 0.5f ||
           std::fabs(pan_y) > 0.5f;
}

} // namespace Core::Mods
