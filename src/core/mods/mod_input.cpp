// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Second-screen touch input: from raw touch points to taps, drags, pans and scrolls, and the
// interaction state that pages read back.
//   - Gestures: UpdateGestures (tap slop, pan, pinch-zoom on pan_zoom widgets, drag-and-drop,
//     scroll-region drags, press-and-hold, horizontal / vertical swipe; ignored during a page transition,
//     absorbed by input_block widgets),
//     UpdateScrollGesture, GlideViews / ReturnIdleViews / GetViewState, ApplyViewCorrections.
//   - DrainTaps: hit-tests queued taps and drops against the widgets and the previous render's
//     map draw records (map_draw_records_published), runs the matching action (RunAction,
//     mod_actions.cpp) and queues its haptic.
//   - Haptics: QueueHaptic / FlushHaptic (at most one per tick, handed to AuxRouting).
//   - Published interaction state: PublishScroll ("@scroll*:"), PublishMapState ("@map_sel*",
//     "@map_tap_*", "@flag:*", slot state), PublishInteraction ("@sel:", "@drag*").
//   - DriveCmdDrag: the console "drag" command's synthetic finger.
// Not here: reading the touch points (Tick, mod_runtime.cpp, via AuxRouting::GetTouch), what an
// action does (mod_actions.cpp), drawing (mod_redraw.cpp, mod_ui.cpp).
// Flow: the input stage of Tick, after SampleState and the first EvaluateDerived pass. Runs on the
// tick thread. Locks: view_mutex (view_state / map_follow_state, which the redraw worker's
// RenderPage also updates), map_records_mutex (the published map draw records the worker
// writes), tap_mutex (pending_taps, also fed by the console tap command).

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include <cstdlib>
#include <fstream>
#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/mods/mod_input_drag.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "core/mods/mod_view_default.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/gpu.h"

namespace Core::Mods {

namespace {
/// A widget's identity for remembering its pan across frames: whatever the package called it, or
/// its position on the page when it went unnamed.
std::string WidgetKey(const Page& page, size_t index) {
    const auto& widget = page.widgets[index];
    if (!widget.id.empty()) {
        return widget.id;
    }
    return page.id + "#" + std::to_string(index);
}
} // namespace

const Widget* ModRuntime::FindWidgetByKey(const std::string& key) const {
    if (current_page >= manifest.pages.size()) {
        return nullptr;
    }
    const auto& page = manifest.pages[current_page];
    for (size_t i = 0; i < page.widgets.size(); ++i) {
        if (WidgetKey(page, i) == key) {
            return &page.widgets[i];
        }
    }
    return nullptr;
}

std::string ModRuntime::PannableAt(const StateSnapshot& snapshot, s32 x, s32 y) const {
    if (current_page >= manifest.pages.size()) {
        return {};
    }
    const auto& page = manifest.pages[current_page];
    // Later widgets are drawn on top, so search backwards and let the topmost one win.
    for (size_t i = page.widgets.size(); i-- > 0;) {
        const auto& w = page.widgets[i];
        if (!w.pan_zoom || WidgetHidden(w, snapshot)) {
            continue;
        }
        if (x >= w.rect[0] && x < w.rect[0] + w.rect[2] && y >= w.rect[1] &&
            y < w.rect[1] + w.rect[3]) {
            return WidgetKey(page, i);
        }
    }
    return {};
}

std::pair<float, float> ModRuntime::ZoomLimits(const std::string& key) const {
    if (const auto* w = FindWidgetByKey(key)) {
        float lo = w->min_zoom;
        // A map with a bound view rect (runtime 14): the renderer publishes how far out the pinch
        // may go -- down to the whole-area fit -- as "<key>#zmin". The caller holds view_mutex.
        if (w->map_extras != nullptr && w->map_extras->HasViewRect()) {
            if (const auto it = map_follow_state.find(key + "#zmin");
                it != map_follow_state.end() && it->second[0] > 0.0f) {
                lo = it->second[0];
            }
        }
        return {std::min(lo, w->max_zoom), w->max_zoom};
    }
    return {1.0f, 8.0f};
}

std::array<s32, 4> ModRuntime::WidgetRect(const std::string& key) const {
    if (const auto* w = FindWidgetByKey(key)) {
        return w->rect;
    }
    return {0, 0, 0, 0};
}

float ModRuntime::ViewRotation(const std::string& key) const {
    // Runtime 22: the turn the last published draw of this map used (view_rotate_bind). Read
    // before view_mutex is taken, never under it.
    std::scoped_lock lk{map_records_mutex};
    if (map_records_page != current_page) {
        return 0.0f;
    }
    for (const auto& r : map_draw_records_published) {
        if (r.view_key == key && r.ppw > 0.0f) {
            return r.rotation;
        }
    }
    return 0.0f;
}

ViewState ModRuntime::GetViewState() const {
    std::scoped_lock lk{view_mutex};
    return view_state;
}

bool ModRuntime::GlideViews() {
    // One easing step for every view a reset put in flight: a fifth of the way home per tick,
    // landing exactly once close. Returns true while any view is still moving.
    std::scoped_lock lk{view_mutex};
    bool moving = false;
    for (auto& [key, view] : view_state) {
        if (!view.gliding) {
            continue;
        }
        view.zoom += (view.goal_zoom - view.zoom) * 0.2f;
        view.pan_x += (view.goal_pan_x - view.pan_x) * 0.2f;
        view.pan_y += (view.goal_pan_y - view.pan_y) * 0.2f;
        if (std::fabs(view.goal_zoom - view.zoom) < 0.004f &&
            std::fabs(view.goal_pan_x - view.pan_x) < 0.5f &&
            std::fabs(view.goal_pan_y - view.pan_y) < 0.5f) {
            view.zoom = view.goal_zoom;
            view.pan_x = view.goal_pan_x;
            view.pan_y = view.goal_pan_y;
            view.gliding = false;
        }
        moving = true;
    }
    return moving;
}

namespace {
/// Milliseconds on the steady clock (press-and-hold timing).
s64 SteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

s64 ModRuntime::CallbackAt(const std::vector<Widget>& expanded, const StateSnapshot& snapshot,
                           s32 x, s32 y) const {
    const s64 hit = HitTestIndex(expanded, snapshot, x, y, [](const Widget& w) {
        return !w.on_pan.empty() || !w.on_pinch.empty() || w.input_block || w.pan_zoom ||
               w.draggable || w.type == WidgetType::Map;
    });
    if (hit < 0) return -1;
    const Widget& w = expanded[static_cast<size_t>(hit)];
    return !w.input_block && !w.pan_zoom && !w.draggable && w.type != WidgetType::Map &&
                   (!w.on_pan.empty() || !w.on_pinch.empty()) ? hit : -1;
}

bool ModRuntime::CallbackValid(const CallbackTarget& target, const std::vector<Widget>& expanded,
                               const StateSnapshot& snapshot) const {
    if (target.index < 0 || target.page != current_page ||
        static_cast<size_t>(target.index) >= expanded.size()) return false;
    const Widget& w = expanded[static_cast<size_t>(target.index)];
    return !WidgetHidden(w, snapshot) && w.rect == target.rect && w.id == target.id &&
           w.on_pan == target.on_pan && w.on_pinch == target.on_pinch;
}

void ModRuntime::QueueGesture(const std::string& action, s64 payload, s32 x, s32 y) {
    if (action.empty()) return;
    PendingTap gesture{x, y};
    gesture.gesture_action = action;
    gesture.gesture_payload = payload;
    std::scoped_lock lock{tap_mutex};
    pending_taps.push_back(std::move(gesture));
}

void ModRuntime::UpdateNativePinch(const StateSnapshot& snapshot,
                                   std::span<const VideoCore::DSMod::AuxPinchEvent> events,
                                   u32 panel_w, u32 panel_h, u32 canvas_w, u32 canvas_h,
                                   bool touch_active) {
    const auto now = std::chrono::steady_clock::now();
    if (touch_active || page_anim.active || page_anim_request.has_value() || HasPendingInput() ||
        current_page >= manifest.pages.size()) {
        native_pinch_target = {};
        return;
    }
    if (now - native_pinch_clock > std::chrono::seconds{2}) native_pinch_target = {};
    if (events.empty()) return;
    native_pinch_clock = now;
    const auto fit = TouchRect(panel_w, panel_h, canvas_w, canvas_h);
    const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
    size_t budget = 4;
    double scale_product = 1.0;
    s32 anchor_x{}, anchor_y{};
    const auto flush = [&] {
        if (scale_product != 1.0 && budget != 0 && native_pinch_target.index >= 0) {
            const auto& w = native_pinch_target;
            if (const auto payload = VideoCore::DSMod::PackPinchPayload(
                    scale_product, anchor_x - w.rect[0], anchor_y - w.rect[1])) {
                QueueGesture(w.on_pinch, *payload, anchor_x, anchor_y);
                --budget;
            }
        }
        scale_product = 1.0;
    };
    for (const auto& event : events) {
        using Phase = VideoCore::DSMod::AuxPinchEvent::Phase;
        if (event.phase == Phase::End) {
            flush();
            native_pinch_target = {};
            continue;
        }
        if (!fit.Contains(event.x, event.y)) {
            scale_product = 1.0;
            native_pinch_target = {};
            continue;
        }
        const s32 x = VideoCore::DSMod::PanelToCanvas(event.x, fit.x, fit.w, canvas_w);
        const s32 y = VideoCore::DSMod::PanelToCanvas(event.y, fit.y, fit.h, canvas_h);
        if (event.phase == Phase::Begin) {
            flush();
            native_pinch_target = {};
            const s64 hit = CallbackAt(expanded, snapshot, x, y);
            if (hit >= 0 && !expanded[static_cast<size_t>(hit)].on_pinch.empty()) {
                native_pinch_target.index = hit;
                native_pinch_target.page = current_page;
                native_pinch_target.SetWidget(expanded[static_cast<size_t>(hit)]);
            }
            continue;
        }
        if (!CallbackValid(native_pinch_target, expanded, snapshot) ||
            CallbackAt(expanded, snapshot, x, y) != native_pinch_target.index) {
            scale_product = 1.0;
            native_pinch_target = {};
            continue;
        }
        scale_product = std::clamp(scale_product * event.scale, 1.0 / 1024.0, 1024.0);
        anchor_x = x;
        anchor_y = y;
    }
    flush();
}

void ModRuntime::UpdateWheel(const StateSnapshot& snapshot,
                             std::span<const VideoCore::DSMod::AuxWheelEvent> events,
                             u32 panel_w, u32 panel_h, u32 canvas_w, u32 canvas_h,
                             bool touch_active) {
    const auto now = std::chrono::steady_clock::now();
    const auto reset = [&] {
        wheel_target.clear();
        wheel_dx = wheel_dy = 0.0f;
    };
    // A click/tab change in the same tick wins over queued wheel input. Do not carry a
    // touchpad fraction across a pause, touch gesture, transition or a long idle interval.
    if (touch_active || page_anim.active || page_anim_request.has_value() || HasPendingInput() ||
        current_page >= manifest.pages.size()) {
        reset();
        return;
    }
    if (now - wheel_clock > std::chrono::milliseconds{350}) {
        reset();
    }
    if (events.empty()) {
        return;
    }
    wheel_clock = now;
    const auto fit = TouchRect(panel_w, panel_h, canvas_w, canvas_h);
    const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
    size_t budget = 4; // consume the entire bounded queue, but never replay a burst for many ticks
    for (const auto& event : events) {
        if (!fit.Contains(event.x, event.y)) {
            reset();
            continue;
        }
        const s32 x = VideoCore::DSMod::PanelToCanvas(event.x, fit.x, fit.w, canvas_w);
        const s32 y = VideoCore::DSMod::PanelToCanvas(event.y, fit.y, fit.h, canvas_h);
        const s64 hit = HitTestIndex(expanded, snapshot, x, y, [](const Widget& w) {
            return SwipeHitFilter(w) || w.draggable;
        });
        if (hit < 0 || !SwipeArms(expanded[static_cast<size_t>(hit)]) ||
            expanded[static_cast<size_t>(hit)].draggable) {
            reset();
            continue;
        }
        const Widget& w = expanded[static_cast<size_t>(hit)];
        const std::string target = fmt::format("{}:{}:{}:{}:{}:{}", current_page, hit,
                                               w.on_swipe_left, w.on_swipe_right,
                                               w.on_swipe_up, w.on_swipe_down);
        if (wheel_target != target) {
            reset();
            wheel_target = target;
        }
        // Axis dominance prevents diagonal touchpad noise from paging two dimensions.
        const bool horizontal = std::fabs(event.dx) > std::fabs(event.dy);
        float& residual = horizontal ? wheel_dx : wheel_dy;
        float& other = horizontal ? wheel_dy : wheel_dx;
        other = 0.0f;
        const float delta = horizontal ? event.dx : event.dy;
        if (delta == 0.0f) {
            continue;
        }
        const std::string& action = horizontal
            ? (delta > 0.0f ? w.on_swipe_left : w.on_swipe_right)
            : (delta > 0.0f ? w.on_swipe_down : w.on_swipe_up);
        if (action.empty()) {
            residual = 0.0f;
            continue;
        }
        if (residual * delta < 0.0f) {
            residual = 0.0f; // reversing the wheel responds immediately
        }
        residual = std::clamp(residual + delta, -8.0f, 8.0f);
        while (std::fabs(residual) >= 1.0f && budget != 0) {
            PendingTap wheel{x, y};
            wheel.swipe_action = action;
            {
                std::scoped_lock lock{tap_mutex};
                pending_taps.push_back(std::move(wheel));
            }
            residual += residual > 0.0f ? -1.0f : 1.0f;
            --budget;
        }
        if (budget == 0) {
            residual = std::fmod(residual, 1.0f); // discard whole-notch backlog, retain precision
        }
    }
}

void ModRuntime::UpdateGestures(const StateSnapshot& snapshot,
                                std::span<const VideoCore::DSMod::AuxTouchPoint> points,
                                size_t count, u32 panel_w, u32 panel_h, u32 canvas_w,
                                u32 canvas_h) {
    // Travel further than this from where the finger landed and it stops being a tap. Small
    // enough that a firm press on a button still counts, large enough that a thumb resting on a
    // map does not fire whatever is underneath it when lifted.
    constexpr float TapSlop = 12.0f;

    // Eden Duo: with Companion Ratio "Fit" the canvas may sit in a rect of the panel (bars
    // around it, see FitCompanion); a full rect maps exactly as v * canvas / panel.
    const auto fit = TouchRect(panel_w, panel_h, canvas_w, canvas_h);
    const auto to_canvas_x = [&](u32 v) {
        if (!fit.full) {
            v = std::clamp(v, fit.x, fit.x + fit.w - 1);
        }
        return VideoCore::DSMod::PanelToCanvas(v, fit.x, fit.w, canvas_w);
    };
    const auto to_canvas_y = [&](u32 v) {
        if (!fit.full) {
            v = std::clamp(v, fit.y, fit.y + fit.h - 1);
        }
        return VideoCore::DSMod::PanelToCanvas(v, fit.y, fit.h, canvas_h);
    };

    const auto previous = [&](u32 id) -> const TrackedFinger* {
        for (const auto& f : live_fingers) {
            if (f.id == id) {
                return &f;
            }
        }
        return nullptr;
    };

    // Fingers still down this frame. A point flagged "end" is already leaving.
    std::vector<TrackedFinger> now;
    now.reserve(count);
    const auto is_ignored = [&](u32 id) {
        return std::ranges::find(ignored_fingers, id) != ignored_fingers.end();
    };
    for (size_t i = 0; i < count; ++i) {
        if ((points[i].attributes & 2u) != 0) {
            if (is_ignored(points[i].finger_id)) {
                std::erase(ignored_fingers, points[i].finger_id); // a bar finger let go
                continue;
            }
            // A leaving finger still reports where it let go: that is where a drag drops.
            if (drag_state.candidate && points[i].finger_id == drag_state.finger) {
                drag_state.x = to_canvas_x(points[i].x);
                drag_state.y = to_canvas_y(points[i].y);
            }
            continue;
        }
        // A finger that lands in a Fit bar is ignored until it lifts, even once it slides onto
        // the canvas; one that slides out from the canvas keeps going along its edge.
        if (is_ignored(points[i].finger_id)) {
            continue;
        }
        if (!fit.full && !fit.Contains(points[i].x, points[i].y) &&
            previous(points[i].finger_id) == nullptr) {
            ignored_fingers.push_back(points[i].finger_id);
            continue;
        }
        now.push_back({points[i].finger_id, to_canvas_x(points[i].x), to_canvas_y(points[i].y)});
    }
    if (count == 0) {
        ignored_fingers.clear();
    }

    if (!now.empty() && live_fingers.empty()) {
        // First finger down: remember where, and find out whether it landed on something that
        // can be panned. Everything else this gesture does depends on that answer.
        gesture_down_x = now.front().x;
        gesture_down_y = now.front().y;
        gesture_moved = false;
        gesture_span = 0.0f;
        gesture_blocked = false;
        callback_target = {};
        // A page transition holds input off until it ends: this whole gesture is ignored (a
        // double tap on a tab must not land on the page sliding in).
        gesture_ignored = page_anim.active || page_anim_request.has_value();
        gesture_target =
            gesture_ignored ? std::string{} : PannableAt(snapshot, gesture_down_x, gesture_down_y);
        // A draggable widget under the finger owns this gesture: moving it drags, never pans.
        drag_state = {};
        if (gesture_ignored) {
            LOG_INFO(Core, "DSMod: gesture at canvas({},{}) ignored: page transition running",
                     gesture_down_x, gesture_down_y);
        } else if (current_page < manifest.pages.size()) {
            const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
            // An input_block above everything movable owns the whole gesture: no pan, no pinch,
            // no drag (a tap is still offered to the widgets above the block).
            if (const s64 block = InputBlockAt(expanded, snapshot, gesture_down_x, gesture_down_y);
                block >= 0) {
                gesture_blocked = true;
                gesture_target.clear();
                const Widget& b = expanded[static_cast<size_t>(block)];
                LOG_INFO(Core, "DSMod gesture at ({},{}) blocked by input_block widget '{}'",
                         gesture_down_x, gesture_down_y,
                         b.id.empty() ? fmt::format("#{}", block) : b.id);
            }
            if (!gesture_blocked) {
                const s64 callback = CallbackAt(expanded, snapshot, gesture_down_x, gesture_down_y);
                if (callback >= 0) {
                    callback_target.index = callback;
                    callback_target.page = current_page;
                    callback_target.SetWidget(expanded[static_cast<size_t>(callback)]);
                    callback_target.pan.Reset(gesture_down_x, gesture_down_y);
                    callback_target.fingers = 1;
                    gesture_target.clear();
                }
            }
            const s64 hit = gesture_blocked || callback_target.index >= 0
                                ? -1
                                : HitTestIndex(expanded, snapshot, gesture_down_x, gesture_down_y,
                                               [](const Widget& w) { return w.draggable; });
            if (hit >= 0) {
                const Widget& picked = expanded[static_cast<size_t>(hit)];
                if (const auto carried = WidgetPayload(picked, snapshot)) {
                    drag_state.candidate = true;
                    drag_state.finger = now.front().id;
                    drag_state.page = current_page;
                    drag_state.widget = picked;
                    drag_state.source = hit;
                    drag_state.payload = *carried;
                    drag_state.grab_dx = gesture_down_x - picked.rect[0];
                    drag_state.grab_dy = gesture_down_y - picked.rect[1];
                    drag_state.x = gesture_down_x;
                    drag_state.y = gesture_down_y;
                    gesture_target.clear();
                } else {
                    LOG_WARNING(Core, "DSMod: draggable widget has no usable payload '{}'",
                                picked.payload);
                }
            }
        }
        // Press-and-hold (runtime 13): an on_hold widget under the first finger arms the hold.
        // Like a tap (not a pan or drag), a hold reaches on_hold widgets drawn above an
        // input_block and never those beneath one.
        // Horizontal swipe (runtime 14): the topmost swipe widget under the first finger arms the
        // swipe, chosen the same way; never on a map / pan_zoom widget, over a pannable widget
        // (the finger pans it) or on a draggable one (the finger drags it).
        // Vertical swipe (runtime 15): the same widget choice; never armed
        // over a scroll region that can scroll (a vertical drag scrolls the list there).
        hold_action.clear();
        std::optional<s64> hold_payload;
        swipe_left_action.clear();
        swipe_right_action.clear();
        swipe_up_action.clear();
        swipe_down_action.clear();
        {
            bool has_hold = false;
            s32 need_ms = 0;
            bool swipe_left = false;
            bool swipe_right = false;
            bool swipe_up = false;
            bool swipe_down = false;
            s32 swipe_px = 0;
            if (!gesture_ignored && callback_target.index < 0 && current_page < manifest.pages.size()) {
                const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
                if (gesture_target.empty() && !drag_state.candidate) {
                    const s64 swipe_hit = HitTestIndex(expanded, snapshot, gesture_down_x,
                                                       gesture_down_y, SwipeHitFilter);
                    if (swipe_hit >= 0 && SwipeArms(expanded[static_cast<size_t>(swipe_hit)])) {
                        const Widget& sw = expanded[static_cast<size_t>(swipe_hit)];
                        swipe_left_action = sw.on_swipe_left;
                        swipe_right_action = sw.on_swipe_right;
                        swipe_left = !sw.on_swipe_left.empty();
                        swipe_right = !sw.on_swipe_right.empty();
                        if ((!sw.on_swipe_up.empty() || !sw.on_swipe_down.empty()) &&
                            ScrollRegionAt(snapshot, gesture_down_x, gesture_down_y).empty()) {
                            swipe_up_action = sw.on_swipe_up;
                            swipe_down_action = sw.on_swipe_down;
                            swipe_up = !sw.on_swipe_up.empty();
                            swipe_down = !sw.on_swipe_down.empty();
                        }
                        swipe_px = sw.swipe_px;
                    }
                }
                const s64 hold_hit = HitTestIndex(
                    expanded, snapshot, gesture_down_x, gesture_down_y,
                    [](const Widget& w) { return !w.on_hold.empty() || w.input_block; });
                // Runtime 16: also over a drag candidate (HoldArmsOverDrag, mod_input_hold.h).
                if (hold_hit >= 0 && !expanded[static_cast<size_t>(hold_hit)].on_hold.empty() &&
                    HoldArmsOverDrag(hold_hit, drag_state.candidate ? drag_state.source : -1)) {
                    const Widget& hw = expanded[static_cast<size_t>(hold_hit)];
                    hold_action = hw.on_hold;
                    hold_payload = WidgetPayload(hw, snapshot);
                    need_ms = hw.hold_ms;
                    has_hold = true;
                }
            }
            hold_tracker.Down(SteadyMs(), has_hold, need_ms, current_page, hold_payload);
            swipe_tracker.Down(SteadyMs(), gesture_down_x, gesture_down_y, swipe_left, swipe_right,
                               swipe_px, current_page, swipe_up, swipe_down);
        }
        // A scrollable list under the finger owns a gesture no draggable widget claimed: a
        // vertical drag scrolls it (never pans a map beneath), a still finger is still a tap on
        // the row, and a finger landing on a gliding list just stops it.
        scroll_target.clear();
        scroll_dragging = false;
        scroll_caught = false;
        scroll_samples.clear();
        if (!gesture_ignored && !gesture_blocked && !drag_state.candidate && callback_target.index < 0) {
            scroll_target = ScrollRegionAt(snapshot, gesture_down_x, gesture_down_y);
            if (!scroll_target.empty()) {
                gesture_target.clear();
                auto& st = scroll_state[scroll_target];
                if (st.flinging || st.flinging_x) {
                    st.flinging = false;
                    st.velocity = 0.0f;
                    st.flinging_x = false;
                    st.velocity_x = 0.0f;
                    scroll_caught = true;
                    LOG_INFO(Core, "DSMod: scroll '{}' caught at offset {:.0f},{:.0f}",
                             scroll_target, st.offset_x, st.offset);
                }
            }
        }
        if (!gesture_target.empty()) {
            std::scoped_lock lk{view_mutex};
            view_state[gesture_target].gliding = false; // the finger takes over from a reset
        }
    }

    if (callback_target.index >= 0 && !now.empty()) {
        const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
        const auto cancel = [&] {
            callback_target = {};
            gesture_blocked = true;
            gesture_moved = true;
            gesture_target.clear();
        };
        if (gesture_ignored || page_anim.active || page_anim_request.has_value() ||
            !CallbackValid(callback_target, expanded, snapshot) || now.size() > 2) {
            cancel();
        } else if (now.size() == 1) {
            if (callback_target.fingers != 1) {
                callback_target.pan.Reset(now[0].x, now[0].y);
                callback_target.span = 0.0f;
            } else if (!callback_target.on_pan.empty()) {
                if (const auto payload = callback_target.pan.Move(now[0].x, now[0].y)) {
                    QueueGesture(callback_target.on_pan, *payload, now[0].x, now[0].y);
                    gesture_moved = true;
                }
            }
            callback_target.fingers = 1;
        } else {
            const auto& rect = callback_target.rect;
            const auto inside = [&](const TrackedFinger& finger) {
                return finger.x >= rect[0] && finger.y >= rect[1] &&
                       finger.x - rect[0] < rect[2] && finger.y - rect[1] < rect[3];
            };
            gesture_moved = true;
            if (!inside(now[0]) || !inside(now[1]) || callback_target.on_pinch.empty() ||
                CallbackAt(expanded, snapshot, now[0].x, now[0].y) != callback_target.index ||
                CallbackAt(expanded, snapshot, now[1].x, now[1].y) != callback_target.index) {
                cancel();
            } else {
                const double dx = double(now[0].x) - now[1].x;
                const double dy = double(now[0].y) - now[1].y;
                const float span = static_cast<float>(std::hypot(dx, dy));
                const s32 cx = (now[0].x + now[1].x) / 2;
                const s32 cy = (now[0].y + now[1].y) / 2;
                if (callback_target.fingers == 2 && callback_target.span > 1.0f && span > 1.0f &&
                    span != callback_target.span) {
                    if (const auto payload = VideoCore::DSMod::PackPinchPayload(
                            span / callback_target.span, cx - rect[0], cy - rect[1])) {
                        QueueGesture(callback_target.on_pinch, *payload, cx, cy);
                    }
                }
                callback_target.span = span;
                callback_target.fingers = 2;
            }
        }
    }

    if (now.size() == 1 && !gesture_target.empty()) {
        if (const auto* was = previous(now[0].id)) {
            float dx = static_cast<float>(now[0].x - was->x);
            float dy = static_cast<float>(now[0].y - was->y);
            if (dx != 0.0f || dy != 0.0f) {
                // Runtime 22: a turned map pans in its own (unturned) frame.
                if (const float turn = ViewRotation(gesture_target); turn != 0.0f) {
                    MapDrawRecord r;
                    r.rotation = turn;
                    const auto d = r.Unturn(dx, dy);
                    dx = d[0];
                    dy = d[1];
                }
                std::scoped_lock lk{view_mutex};
                auto& view = view_state[gesture_target];
                // Drag moves the content with the finger, so pan travels the other way.
                view.pan_x -= dx / view.zoom;
                view.pan_y -= dy / view.zoom;
                if (const auto* w = FindWidgetByKey(gesture_target)) {
                    ClampViewToBounds(*w, view); // pan_bounds: no hidden overshoot past them
                }
            }
        }
    }

    if (now.size() >= 2 && drag_state.candidate) {
        // A second finger turns a drag into a pinch; the drag is abandoned.
        LOG_INFO(Core, "DSMod: drag of {} cancelled by a second finger", drag_state.payload);
        drag_state = {};
    }
    if (now.size() >= 2 && callback_target.index < 0) {
        const float dx = static_cast<float>(now[0].x - now[1].x);
        const float dy = static_cast<float>(now[0].y - now[1].y);
        const float span = std::sqrt(dx * dx + dy * dy);
        const float cx = static_cast<float>(now[0].x + now[1].x) * 0.5f;
        const float cy = static_cast<float>(now[0].y + now[1].y) * 0.5f;
        if (gesture_target.empty() && !gesture_blocked && !gesture_ignored &&
            current_page < manifest.pages.size()) {
            // A pinch that started on nothing pannable is judged where the fingers meet.
            const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
            if (InputBlockAt(expanded, snapshot, static_cast<s32>(cx), static_cast<s32>(cy)) >= 0) {
                gesture_blocked = true;
                LOG_INFO(Core, "DSMod gesture at ({},{}) blocked by input_block widget (pinch)",
                         static_cast<s32>(cx), static_cast<s32>(cy));
            } else {
                gesture_target = PannableAt(snapshot, static_cast<s32>(cx), static_cast<s32>(cy));
            }
        }
        if (gesture_span > 1.0f && span > 1.0f && !gesture_target.empty()) {
            const float turn = ViewRotation(gesture_target); // runtime 22, before view_mutex
            std::scoped_lock lk{view_mutex};
            auto& view = view_state[gesture_target];
            const auto limits = ZoomLimits(gesture_target);
            const float before = view.zoom;
            const float after =
                std::clamp(before * (span / gesture_span), limits.first, limits.second);
            if (after != before) {
                // Zoom about the point between the fingers: the map detail under them should
                // stay under them. Anywhere else and the thing you were looking at slides away
                // exactly when you were trying to look closer.
                const auto rect = WidgetRect(gesture_target);
                float ox = cx - static_cast<float>(rect[0]);
                float oy = cy - static_cast<float>(rect[1]);
                if (turn != 0.0f) {
                    // A turned map: the point between the fingers, in the unturned view.
                    MapDrawRecord r;
                    r.rotation = turn;
                    const float hw = static_cast<float>(rect[2]) * 0.5f;
                    const float hh = static_cast<float>(rect[3]) * 0.5f;
                    const auto d = r.Unturn(ox - hw, oy - hh);
                    ox = d[0] + hw;
                    oy = d[1] + hh;
                }
                const float anchor_x = view.pan_x + ox / before;
                const float anchor_y = view.pan_y + oy / before;
                view.zoom = after;
                view.pan_x = anchor_x - ox / after;
                view.pan_y = anchor_y - oy / after;
                if (const auto* w = FindWidgetByKey(gesture_target)) {
                    ClampViewToBounds(*w, view);
                }
            }
        }
        gesture_span = span;
        gesture_moved = true;
    } else {
        gesture_span = 0.0f;
    }

    // Did this gesture wander far enough to disqualify itself as a tap?
    if (!now.empty()) {
        const float dx = static_cast<float>(now.front().x - gesture_down_x);
        const float dy = static_cast<float>(now.front().y - gesture_down_y);
        if (std::sqrt(dx * dx + dy * dy) > TapSlop) {
            gesture_moved = true;
        }
    }
    // Horizontal swipe: judged when the finger leaves the slop; a horizontal start owns the
    // gesture, so a scroll region under it does not start a vertical drag.
    // A region that scrolls on x owns horizontal drags too: the page swipe never arms over it.
    bool scroll_takes_x = false;
    if (!scroll_target.empty() && current_page < manifest.pages.size()) {
        const ScrollRegion* const r =
            FindScrollRegion(manifest.pages[current_page], scroll_target);
        scroll_takes_x = r != nullptr && (r->axes & ScrollRegion::AxisX) != 0;
    }
    if (!now.empty()) {
        swipe_tracker.Move(SteadyMs(), now.front().x, now.front().y,
                           now.size() > 1 || drag_state.active || gesture_ignored ||
                               page_anim.active || page_anim_request.has_value() ||
                               hold_tracker.Fired() || scroll_takes_x,
                           current_page);
    }
    if (!scroll_target.empty() && !now.empty()) {
        UpdateScrollGesture(snapshot, now.size(), now.front().x, now.front().y,
                            gesture_moved && !swipe_tracker.OwnsGesture(), false);
    }
    // Press-and-hold: fires once, on the tick the still single finger reaches hold_ms.
    if (!now.empty() &&
        hold_tracker.Update(SteadyMs(), gesture_moved || now.size() > 1 || drag_state.active ||
                                            gesture_ignored || page_anim.active ||
                                            page_anim_request.has_value(),
                            current_page)) {
        PendingTap hold{gesture_down_x, gesture_down_y, hold_action};
        hold.hold_payload = hold_tracker.Payload();
        LOG_INFO(Core, "DSMod: aux hold at canvas({},{}) -> '{}'", hold.x, hold.y, hold_action);
        if (drag_state.candidate) {
            // Runtime 16: the hold fired first, so this touch does not drag.
            LOG_INFO(Core, "DSMod: drag of {} given up for the hold", drag_state.payload);
            drag_state = {};
        }
        std::scoped_lock lk{tap_mutex};
        pending_taps.push_back(hold);
    }

    if (drag_state.candidate && now.size() == 1) {
        drag_state.x = now.front().x;
        drag_state.y = now.front().y;
        if (!drag_state.active && gesture_moved) {
            drag_state.active = true;
            if (!drag_state.widget.select_group.empty() && drag_state.payload != -1) {
                last_selection[drag_state.widget.select_group] = drag_state.payload;
            }
            LOG_INFO(Core, "DSMod: drag of payload {} started at canvas({},{})", drag_state.payload,
                     gesture_down_x, gesture_down_y);
            QueueHaptic(HapticKind::Drag, drag_state.widget.haptic, -1,
                        fmt::format("drag of payload {}", drag_state.payload));
        }
        if (drag_state.active) {
            drag_state.hover = DropTargetAt(snapshot, drag_state.x, drag_state.y);
        }
    }

    // Every finger gone: the gesture is over. A still one was a tap on whatever it landed on.
    if (now.empty() && !live_fingers.empty()) {
        callback_target = {};
        const SwipeDir swipe_dir = swipe_tracker.Up();
        if (!scroll_target.empty()) {
            UpdateScrollGesture(snapshot, 0, live_fingers.front().x, live_fingers.front().y,
                                gesture_moved, true);
        }
        if (scroll_caught && !gesture_moved) {
            LOG_INFO(Core, "DSMod: aux touch at canvas({},{}) stopped a scrolling list (no tap)",
                     gesture_down_x, gesture_down_y);
        } else if (drag_state.active) {
            // Released: drop on the target under the finger, or cancel anywhere else.
            const s64 target = DropTargetAt(snapshot, drag_state.x, drag_state.y);
            std::string action;
            HapticOverride target_haptic = -1;
            if (target >= 0 && current_page < manifest.pages.size()) {
                const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
                if (static_cast<size_t>(target) < expanded.size()) {
                    action = expanded[static_cast<size_t>(target)].drop_action;
                    target_haptic = expanded[static_cast<size_t>(target)].haptic;
                }
            }
            if (!action.empty()) {
                LOG_INFO(Core, "DSMod: drag of {} dropped at canvas({},{}) -> '{}'",
                         drag_state.payload, drag_state.x, drag_state.y, action);
                pending_drops.push_back({action, drag_state.payload, target_haptic,
                                         drag_state.widget.select_group, drag_state.x, drag_state.y,
                                         target});
            } else {
                LOG_INFO(Core, "DSMod: drag of {} cancelled (released at canvas({},{}))",
                         drag_state.payload, drag_state.x, drag_state.y);
                if (const char* const p = Common::DSMod::DevEnvironment("EDEN_DSMOD_CMD")) {
                    std::ofstream f(std::string(p) + ".out", std::ios::app);
                    if (f) {
                        f << fmt::format("DSMod drag cancelled ({},{}) payload {}\n", drag_state.x,
                                         drag_state.y, drag_state.payload);
                    }
                }
            }
        } else if (gesture_ignored) {
            LOG_INFO(Core, "DSMod: aux gesture from canvas({},{}) dropped (page transition)",
                     gesture_down_x, gesture_down_y);
        } else if (hold_tracker.Fired()) {
            LOG_INFO(Core, "DSMod: aux hold at canvas({},{}) released (no tap)", gesture_down_x,
                     gesture_down_y);
        } else if (swipe_dir != SwipeDir::None) {
            // A fired swipe (runtime 14): queued like a hold, with the widget's action; the lift
            // is not a tap (the finger left the slop).
            PendingTap swipe{gesture_down_x, gesture_down_y};
            const char* dir_name = "right";
            swipe.swipe_action = swipe_right_action;
            if (swipe_dir == SwipeDir::Left) {
                dir_name = "left";
                swipe.swipe_action = swipe_left_action;
            } else if (swipe_dir == SwipeDir::Up) {
                dir_name = "up";
                swipe.swipe_action = swipe_up_action;
            } else if (swipe_dir == SwipeDir::Down) {
                dir_name = "down";
                swipe.swipe_action = swipe_down_action;
            }
            LOG_INFO(Core, "DSMod: aux swipe {} from canvas({},{}) -> '{}'", dir_name, swipe.x,
                     swipe.y, swipe.swipe_action);
            std::scoped_lock lk{tap_mutex};
            pending_taps.push_back(std::move(swipe));
        } else if (!gesture_moved) {
            const PendingTap tap{gesture_down_x, gesture_down_y};
            LOG_INFO(Core, "DSMod: aux tap at canvas({},{})", tap.x, tap.y);
            std::scoped_lock lk{tap_mutex};
            pending_taps.push_back(tap);
        } else if (gesture_blocked) {
            LOG_INFO(Core, "DSMod: blocked gesture from canvas({},{}) ended (nothing moved)",
                     gesture_down_x, gesture_down_y);
        } else {
            std::scoped_lock lk{view_mutex};
            const auto& v = view_state[gesture_target];
            LOG_INFO(Core, "DSMod: aux gesture on '{}' ended: zoom {:.2f} pan ({:.0f},{:.0f})",
                     gesture_target, v.zoom, v.pan_x, v.pan_y);
        }
        hold_tracker.Up();
        hold_action.clear();
        swipe_left_action.clear();
        swipe_right_action.clear();
        swipe_up_action.clear();
        swipe_down_action.clear();
        gesture_target.clear();
        gesture_moved = false;
        gesture_blocked = false;
        gesture_ignored = false;
        drag_state = {};
        scroll_target.clear();
        scroll_dragging = false;
        scroll_caught = false;
        scroll_samples.clear();
    }

    if (!now.empty() && !gesture_target.empty()) {
        view_touched[gesture_target] = std::chrono::steady_clock::now();
    }
    live_fingers = std::move(now);
    tap_was_down = !live_fingers.empty();
}

void ModRuntime::ReturnIdleViews() {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lk{view_mutex};
    for (auto& [key, view] : view_state) {
        // away from its home: its bound default view, else zoom 1 / no pan (mod_view_default.h)
        if (view.gliding || (!live_fingers.empty() && gesture_target == key)) {
            continue;
        }
        const Widget* const widget = FindWidgetByKey(key);
        if (widget == nullptr || widget->view_idle_ms == 0 || !ViewAwayFromHome(view, *widget)) {
            continue;
        }
        const auto touched = view_touched.find(key);
        if (touched != view_touched.end() &&
            now - touched->second < std::chrono::milliseconds{widget->view_idle_ms}) {
            continue;
        }
        GlideViewHome(view);
        LOG_INFO(Core,
                 "DSMod: view '{}' idle for {} ms, gliding home from zoom {:.2f} pan "
                 "({:.0f},{:.0f})",
                 key, widget->view_idle_ms, view.zoom, view.pan_x, view.pan_y);
    }
}

void ModRuntime::ApplyViewDefaults(const StateSnapshot& snapshot) {
    if (current_page >= manifest.pages.size()) {
        return;
    }
    const Page& page = manifest.pages[current_page];
    std::scoped_lock lk{view_mutex};
    for (size_t i = 0; i < page.widgets.size(); ++i) {
        const Widget& w = page.widgets[i];
        if (!w.pan_zoom || !w.view_default) {
            continue;
        }
        const ViewDefaultBinds& b = *w.view_default;
        const std::string key = WidgetKey(page, i);
        const auto home =
            DefaultView(w.rect[2], w.rect[3], SnapshotNumber(snapshot, b.zoom_bind),
                        SnapshotNumber(snapshot, b.cx_bind), SnapshotNumber(snapshot, b.cy_bind),
                        w.min_zoom, w.max_zoom, w.pan_bounds);
        const auto reset = b.reset_bind.empty() ? std::optional<f64>{}
                                                : SnapshotNumber(snapshot, b.reset_bind);
        const bool held = !live_fingers.empty() && gesture_target == key;
        auto& view = view_state[key];
        if (ApplyViewDefault(view, home, reset, held)) {
            LOG_DEBUG(Core, "DSMod: view '{}' home zoom {:.3f} pan ({:.0f},{:.0f}), view zoom {:.3f}",
                      key, view.home_zoom, view.home_pan_x, view.home_pan_y, view.zoom);
        }
    }
}

std::string ModRuntime::ScrollRegionAt(const StateSnapshot& snapshot, s32 x, s32 y) const {
    if (current_page >= manifest.pages.size()) {
        return {};
    }
    const Page& page = manifest.pages[current_page];
    for (size_t i = page.scrolls.size(); i-- > 0;) {
        const ScrollRegion& r = page.scrolls[i];
        if (x < r.rect[0] || x >= r.rect[0] + r.rect[2] || y < r.rect[1] ||
            y >= r.rect[1] + r.rect[3]) {
            continue;
        }
        if (!r.show.Empty() && !GateOpen(r.show, snapshot)) {
            continue;
        }
        // A list that fits its rect has nothing to scroll: the touch stays an ordinary one.
        const ScrollMetrics m = MeasureScroll(page, r, snapshot);
        if (m.max_offset <= 0 && m.max_offset_x <= 0) {
            continue;
        }
        return r.id;
    }
    return {};
}

void ModRuntime::UpdateScrollGesture(const StateSnapshot& snapshot, size_t fingers, s32 x,
                                     s32 y, bool moved, bool released) {
    if (current_page >= manifest.pages.size()) {
        return;
    }
    const Page& page = manifest.pages[current_page];
    const ScrollRegion* const region = FindScrollRegion(page, scroll_target);
    if (region == nullptr) {
        return;
    }
    auto& st = scroll_state[scroll_target];
    const auto now = std::chrono::steady_clock::now();
    // The axes this drag moves: the region's, narrowed by lock_axis once the finger has chosen.
    const u8 axes = scroll_axis_lock != 0 ? static_cast<u8>(region->axes & scroll_axis_lock)
                                          : region->axes;
    const bool on_x = (axes & ScrollRegion::AxisX) != 0;
    const bool on_y = (axes & ScrollRegion::AxisY) != 0;
    if (released) {
        if (scroll_dragging && region->fling && !scroll_samples.empty()) {
            // Speed over the ~100 ms before the lift (measured back from the lift itself, so a
            // finger that rested before lifting has none, however slowly frames arrive). The
            // sample just before that window anchors it, so one slow frame still yields a speed.
            const auto window = std::chrono::milliseconds{100};
            const ScrollSample& last = scroll_samples.back();
            const ScrollSample* first = nullptr;
            for (auto it = scroll_samples.rbegin(); it != scroll_samples.rend(); ++it) {
                first = &*it;
                if (now - it->t > window) {
                    break;
                }
            }
            const float dt = std::chrono::duration<float>(last.t - first->t).count();
            // Rested = the finger has been still for 50 ms or more when it lifts.
            auto still_since = last.t;
            for (auto it = scroll_samples.rbegin();
                 it != scroll_samples.rend() && it->y == last.y && it->x == last.x; ++it) {
                still_since = it->t;
            }
            const bool rested = now - still_since >= std::chrono::milliseconds{50};
            const auto speed = [&](s32 from, s32 to) {
                const float v = dt > 0.008f && !rested ? -static_cast<float>(to - from) / dt : 0.0f;
                return std::clamp(v, -8000.0f, 8000.0f);
            };
            if (on_y) {
                const float v = speed(first->y, last.y);
                if (std::fabs(v) > 150.0f) {
                    st.velocity = v;
                    st.flinging = true;
                }
            }
            if (on_x) {
                const float v = speed(first->x, last.x);
                if (std::fabs(v) > 150.0f) {
                    st.velocity_x = v;
                    st.flinging_x = true;
                }
            }
        }
        if (scroll_dragging) {
            std::string fling;
            if (st.flinging || st.flinging_x) {
                fling = fmt::format(", fling {:.0f},{:.0f} px/s", st.velocity_x, st.velocity);
            }
            LOG_INFO(Core, "DSMod: scroll '{}' released at offset {:.0f},{:.0f}{}", scroll_target,
                     st.offset_x, st.offset, fling);
        }
        scroll_axis_lock = 0;
        return;
    }
    if (fingers >= 2) {
        scroll_dragging = false; // a pinch holds the list still; lifting to one finger re-anchors
        scroll_samples.clear();
        return;
    }
    scroll_samples.push_back({now, x, y});
    while (scroll_samples.size() > 2 &&
           now - scroll_samples.front().t > std::chrono::milliseconds{200}) {
        scroll_samples.erase(scroll_samples.begin());
    }
    if (!moved) {
        return; // inside the tap slop: still a tap on the row
    }
    if (!scroll_dragging) {
        // Anchor where the slop was crossed so the list does not jump by the slop distance.
        scroll_dragging = true;
        scroll_anchor_y = y;
        scroll_anchor_x = x;
        scroll_anchor_offset = st.offset;
        scroll_anchor_offset_x = st.offset_x;
        st.flinging = false;
        st.velocity = 0.0f;
        st.flinging_x = false;
        st.velocity_x = 0.0f;
        if (region->lock_axis && region->axes == (ScrollRegion::AxisX | ScrollRegion::AxisY)) {
            // The direction the finger left the slop along is the axis this drag follows.
            const s32 dx = std::abs(x - gesture_down_x);
            const s32 dy = std::abs(y - gesture_down_y);
            scroll_axis_lock = dx > dy ? ScrollRegion::AxisX : ScrollRegion::AxisY;
        }
        LOG_INFO(Core, "DSMod: scroll '{}' drag from offset {:.0f},{:.0f}{}", scroll_target,
                 st.offset_x, st.offset,
                 scroll_axis_lock == ScrollRegion::AxisX   ? " (x only)"
                 : scroll_axis_lock == ScrollRegion::AxisY ? " (y only)"
                                                           : "");
        return;
    }
    const ScrollMetrics m = MeasureScroll(page, *region, snapshot);
    if (on_y) {
        const float max_offset = static_cast<float>(m.max_offset);
        const float wanted = scroll_anchor_offset - static_cast<float>(y - scroll_anchor_y);
        st.offset = std::clamp(wanted, 0.0f, max_offset);
        if (st.offset != wanted) {
            // Pinned at an end: re-anchor so reversing direction moves the list at once.
            scroll_anchor_offset = st.offset;
            scroll_anchor_y = y;
        }
    }
    if (on_x) {
        const float max_offset_x = static_cast<float>(m.max_offset_x);
        const float wanted = scroll_anchor_offset_x - static_cast<float>(x - scroll_anchor_x);
        st.offset_x = std::clamp(wanted, 0.0f, max_offset_x);
        if (st.offset_x != wanted) {
            scroll_anchor_offset_x = st.offset_x;
            scroll_anchor_x = x;
        }
    }
}

void ModRuntime::PublishScroll(StateSnapshot& snapshot, bool advance) {
    if (current_page >= manifest.pages.size()) {
        return;
    }
    const Page& page = manifest.pages[current_page];
    if (scroll_page != current_page) {
        // A new page starts every list at its top.
        scroll_state.clear();
        scroll_page = current_page;
        scroll_target.clear();
        scroll_dragging = false;
        scroll_caught = false;
    }
    if (page.scrolls.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (advance) {
        if (scroll_clock.time_since_epoch().count() != 0) {
            dt = std::clamp(std::chrono::duration<float>(now - scroll_clock).count(), 0.0f, 0.1f);
        }
        scroll_clock = now;
    }
    for (const ScrollRegion& region : page.scrolls) {
        auto& st = scroll_state[region.id];
        bool was_reset = false; ///< reset_bind changed this tick: the follow is re-applied
        if (!region.reset_bind.empty()) {
            const s64 v = snapshot.GetInt(region.reset_bind);
            was_reset = st.has_reset && v != st.reset_value;
            if (was_reset && (st.offset != 0.0f || st.offset_x != 0.0f)) {
                LOG_INFO(Core, "DSMod: scroll '{}' reset ({} {} -> {})", region.id,
                         region.reset_bind, st.reset_value, v);
                st.offset = 0.0f;
                st.flinging = false;
                st.velocity = 0.0f;
                st.offset_x = 0.0f;
                st.flinging_x = false;
                st.velocity_x = 0.0f;
                if (scroll_target == region.id && scroll_dragging) {
                    scroll_anchor_offset = 0.0f;
                    scroll_anchor_y = live_fingers.empty() ? 0 : live_fingers.front().y;
                    scroll_anchor_offset_x = 0.0f;
                    scroll_anchor_x = live_fingers.empty() ? 0 : live_fingers.front().x;
                }
            }
            st.has_reset = true;
            st.reset_value = v;
        }
        const ScrollMetrics m = MeasureScroll(page, region, snapshot);
        const float max_offset = static_cast<float>(m.max_offset);
        if (!region.follow_bind.empty()) {
            // Keep the bound element (the selected row) in view when it changes from outside
            // the list. A finger in the list -- a tap on a row -- owns the change instead.
            const s64 now_ms = SteadyMs();
            if (scroll_target == region.id) {
                st.touched_ms = now_ms;
            }
            const bool touched =
                st.touched_ms >= 0 && now_ms - st.touched_ms < FollowQuietMs;
            const auto v = SnapshotNumber(snapshot, region.follow_bind);
            const std::optional<s64> index =
                v ? std::optional<s64>{static_cast<s64>(std::llround(*v))} : std::nullopt;
            if (was_reset || (st.has_follow ? index != st.follow_value : region.follow_open)) {
                st.follow_pending = !touched && index && *index >= 0;
            }
            st.has_follow = true;
            st.follow_value = index;
            if (touched) {
                st.follow_pending = false;
            }
            // Applied once the list holds the element (its count may arrive after the index).
            if (st.follow_pending && index && *index < m.count) {
                st.follow_pending = false;
                const float to = FollowOffset(m, region.rect[3], *index, st.offset);
                if (std::fabs(to - st.offset) >= 0.5f) {
                    LOG_INFO(Core, "DSMod: scroll '{}' follows {} = {} (offset {:.0f} -> {:.0f})",
                             region.id, region.follow_bind, *index, st.offset, to);
                    st.offset = to;
                    st.flinging = false;
                    st.velocity = 0.0f;
                }
            }
        }
        if (st.flinging && dt > 0.0f) {
            st.offset += st.velocity * dt;
            st.velocity *= std::pow(region.friction, dt);
            if (std::fabs(st.velocity) < 20.0f) {
                st.flinging = false;
                st.velocity = 0.0f;
            }
        }
        if (st.offset <= 0.0f || st.offset >= max_offset) {
            st.offset = std::clamp(st.offset, 0.0f, max_offset);
            if (st.flinging) {
                st.flinging = false; // hit an end: stop dead
                st.velocity = 0.0f;
            }
        }
        // The x axis of a two-axis region: the same fling, clamp and publish.
        const float max_offset_x = static_cast<float>(m.max_offset_x);
        if (st.flinging_x && dt > 0.0f) {
            st.offset_x += st.velocity_x * dt;
            st.velocity_x *= std::pow(region.friction, dt);
            if (std::fabs(st.velocity_x) < 20.0f) {
                st.flinging_x = false;
                st.velocity_x = 0.0f;
            }
        }
        if (st.offset_x <= 0.0f || st.offset_x >= max_offset_x) {
            st.offset_x = std::clamp(st.offset_x, 0.0f, max_offset_x);
            if (st.flinging_x) {
                st.flinging_x = false;
                st.velocity_x = 0.0f;
            }
        }
        const s32 offset = static_cast<s32>(std::lround(st.offset));
        const s32 offset_x = static_cast<s32>(std::lround(st.offset_x));
        const bool on = (m.max_offset > 0 || m.max_offset_x > 0) &&
                        (region.show.Empty() || GateOpen(region.show, snapshot));
        snapshot.ints[ScrollOffsetKey(region.id)] = offset;
        snapshot.ints["@scroll_max:" + region.id] = m.max_offset;
        if ((region.axes & ScrollRegion::AxisX) != 0) {
            snapshot.ints[ScrollOffsetXKey(region.id)] = offset_x;
            snapshot.ints["@scroll_max_x:" + region.id] = m.max_offset_x;
        }
        snapshot.ints["@scroll_on:" + region.id] = on ? 1 : 0;
        snapshot.ints["@scroll_count:" + region.id] = m.count;
        snapshot.ints["@scroll_first:" + region.id] =
            m.row_h > 0 ? static_cast<s64>(offset / m.row_h) * m.cols : 0;
    }
}

void ModRuntime::ApplyViewCorrections() {
    std::scoped_lock lk{view_mutex};
    ++follow_state_epoch;
    for (auto it = map_follow_state.begin(); it != map_follow_state.end();) {
        if (!it->first.ends_with("#pan")) {
            ++it;
            continue;
        }
        const std::string key = it->first.substr(0, it->first.size() - 4);
        if (const auto view = view_state.find(key);
            view != view_state.end() && !view->second.gliding) {
            view->second.pan_x = it->second[0];
            view->second.pan_y = it->second[1];
        }
        it = map_follow_state.erase(it);
    }
}

bool ModRuntime::HasPendingInput() const {
    if (!pending_drops.empty()) {
        return true;
    }
    std::scoped_lock lk{tap_mutex};
    return !pending_taps.empty();
}

void ModRuntime::DrainTaps(const StateSnapshot& snapshot) {
    std::vector<PendingTap> taps;
    {
        std::scoped_lock lk{tap_mutex};
        taps.swap(pending_taps);
    }
    std::vector<PendingDrop> drops;
    drops.swap(pending_drops);
    if ((taps.empty() && drops.empty()) || manifest.pages.empty()) {
        return;
    }
    // Runs a named action for a tap/drop, carrying a payload, and leaves the same trace line the
    // plain tap path does (the .out file is what headless verification reads).
    struct Ran {
        ActionResult result{ActionResult::Skipped};
        const Action* action{nullptr};
    };
    const auto run_named = [&](const std::string& name, std::optional<s64> payload, const char* how,
                               s32 x, s32 y, const std::array<s32, 4>* origin = nullptr) -> Ran {
        const auto it = manifest.actions.find(name);
        if (it == manifest.actions.end()) {
            LOG_WARNING(Core, "DSMod: widget references unknown action '{}'", name);
            return {};
        }
        const std::string carried = payload ? fmt::format(" payload {}", *payload) : std::string{};
        LOG_INFO(Core, "DSMod: {} ({},{}) -> action '{}'{}", how, x, y, name, carried);
        if (const char* const p = Common::DSMod::DevEnvironment("EDEN_DSMOD_CMD")) {
            std::ofstream f(std::string(p) + ".out", std::ios::app);
            if (f) {
                f << fmt::format("DSMod {} ({},{}) -> action '{}'{}\n", how, x, y, name, carried);
            }
        }
        return Ran{RunAction(it->second, snapshot, payload, origin), &it->second};
    };
    // Haptics for an action a tap ran: its kind's strength when it did its work, "refused" when a
    // gate or a full slot turned it down, nothing when it was skipped.
    const auto tap_haptic = [&](const Ran& ran, HapticOverride widget_haptic,
                                std::optional<HapticKind> done_kind, const std::string& source) {
        if (ran.action == nullptr) {
            return;
        }
        if (ran.result == ActionResult::Done) {
            QueueHaptic(done_kind.value_or(TapHapticKind(*ran.action)), widget_haptic,
                        ran.action->haptic, source);
        } else if (ran.result == ActionResult::Refused) {
            QueueHaptic(HapticKind::Refused, -1, -1, source);
        }
    };
    for (const auto& drop : drops) {
        const Ran ran = run_named(drop.action, drop.payload, "drop", drop.x, drop.y);
        tap_haptic(ran, drop.haptic, HapticKind::Drop, "drop '" + drop.action + "'");
        if (!drop.group.empty()) {
            selections.erase(drop.group);
        }
    }
    if (taps.empty()) {
        return;
    }
    const auto& page = manifest.pages[std::min(current_page, manifest.pages.size() - 1)];
    // Pages without selectable widgets or drop targets keep the original hit test exactly.
    const bool interactive = std::ranges::any_of(page.widgets, [](const Widget& w) {
        return !w.select_group.empty() || !w.drop_action.empty();
    });
    // Map taps and tap blockers: only pages that use them take this path first.
    const auto map_tappable = [](const Widget& w) {
        return w.type == WidgetType::Map && w.map_extras && w.map_extras->Tappable();
    };
    const bool map_page = std::ranges::any_of(
        page.widgets, [&](const Widget& w) { return w.input_block || map_tappable(w); });
    const auto out_line = [](const std::string& line) {
        LOG_INFO(Core, "{}", line);
        if (const char* const p = Common::DSMod::DevEnvironment("EDEN_DSMOD_CMD")) {
            std::ofstream f(std::string(p) + ".out", std::ios::app);
            if (f) {
                f << line << '\n';
            }
        }
    };
    const auto map_tap = [&](const Widget& w, size_t index, const PendingTap& tap) {
        const MapWidgetExtras& ex = *w.map_extras;
        if (!ex.tap_enabled.Empty()) {
            const auto v = ResolveActionRef(ex.tap_enabled.point, snapshot, std::nullopt);
            const bool nonzero = v.has_value() && v->f.value_or(static_cast<f64>(v->i)) != 0.0;
            if (!v.has_value() || nonzero == ex.tap_enabled.negate) {
                out_line(fmt::format("DSMod map tap ({},{}) ignored: view only ({}{} = {})", tap.x,
                                     tap.y, ex.tap_enabled.negate ? "!" : "", ex.tap_enabled.point,
                                     v ? fmt::format("{}", v->f.value_or(static_cast<f64>(v->i)))
                                       : std::string{"missing"}));
                return;
            }
        }
        // Reads the published, map_records_mutex-guarded copy -- see
        // map_draw_records_published's own declaration comment (mod_runtime.h). Copies the
        // matching record BY VALUE (not a pointer into the map, which stays valid only while the
        // lock is held) -- the same "hold nothing but an owned copy past the lock" discipline
        // CacheFindImage's shared_ptr gives image_cache, applied here to a plain struct instead.
        std::optional<MapDrawRecord> rec;
        {
            std::scoped_lock rlk{map_records_mutex};
            if (map_records_page == current_page) {
                for (const auto& r : map_draw_records_published) {
                    if (r.widget_index == index && r.ppw > 0.0f) {
                        rec = r;
                    }
                }
            }
        }
        if (!rec.has_value()) {
            out_line(fmt::format("DSMod map tap ({},{}) ignored: the map is not drawn yet", tap.x,
                                 tap.y));
            return;
        }
        const auto area = manifest.map_areas.find(rec->area);
        const auto group_tappable = [&](const std::string& g) {
            return ex.marker_tap_groups.empty() ||
                   std::ranges::find(ex.marker_tap_groups, g) != ex.marker_tap_groups.end();
        };
        if (!ex.on_marker_tap.empty()) {
            const MapDrawRecord::Hit* best = nullptr;
            const float radius = static_cast<float>(std::max(32, ex.marker_hit_px));
            float best_d2 = radius * radius;
            for (const auto& h : rec->hits) {
                if (!group_tappable(h.group)) {
                    continue;
                }
                const float dx = h.sx - static_cast<float>(tap.x);
                const float dy = h.sy - static_cast<float>(tap.y);
                const float d2 = dx * dx + dy * dy;
                if (d2 <= best_d2) {
                    best = &h;
                    best_d2 = d2;
                }
            }
            if (best != nullptr) {
                const auto sel = map_selections.find(best->group);
                const bool clear = sel != map_selections.end() && sel->second == best->index;
                const auto marker_action = manifest.actions.find(ex.on_marker_tap);
                QueueHaptic(HapticKind::Marker, w.haptic,
                            marker_action != manifest.actions.end() ? marker_action->second.haptic
                                                                    : HapticOverride{-1},
                            fmt::format("marker {} slot {}", best->group, best->index));
                if (clear) {
                    map_selections.erase(sel);
                } else {
                    map_selections[best->group] = best->index;
                    map_selection_tick[best->group] = tick_count;
                }
                out_line(fmt::format("DSMod map marker tap ({},{}) group '{}' slot {} at "
                                     "({:.3f},{:.3f}) -> {}",
                                     tap.x, tap.y, best->group, best->index, best->vx, best->vy,
                                     clear ? "deselected" : "selected"));
                map_tap_ctx = {true,     best->vx,    best->vy,   best->wx,
                               best->wy, best->index, best->group};
                const Ran ran = run_named(ex.on_marker_tap, static_cast<s64>(best->index),
                                          "marker tap", tap.x, tap.y, &w.rect);
                if (ran.action != nullptr && TapHapticKind(*ran.action) == HapticKind::Write) {
                    tap_haptic(ran, w.haptic, std::nullopt,
                               "marker action '" + ex.on_marker_tap + "'");
                }
                map_tap_ctx = {};
                return;
            }
        }
        // An empty spot: first drop a selection of this map's groups.
        bool had_selection = false;
        if (area != manifest.map_areas.end()) {
            for (const auto& dm : area->second.dynamic_markers) {
                if (group_tappable(dm.group) && map_selections.erase(dm.group) > 0) {
                    had_selection = true;
                }
            }
        }
        if (had_selection && ex.empty_tap_deselects && !ex.on_marker_tap.empty()) {
            out_line(fmt::format("DSMod map tap ({},{}) cleared the selection", tap.x, tap.y));
            QueueHaptic(HapticKind::Marker, w.haptic, -1, "map tap deselect");
            return;
        }
        if (ex.on_map_tap.empty()) {
            return;
        }
        // Runtime 22: through the view's turn (view_rotate_bind), so the tap names the world
        // point drawn under the finger.
        const auto world = rec->World(static_cast<float>(tap.x), static_cast<float>(tap.y));
        const f64 wx = world[0];
        const f64 wy = world[1];
        if (wx < rec->min_x || wx > rec->max_x || wy < rec->min_y || wy > rec->max_y) {
            out_line(fmt::format("DSMod map tap ({},{}) ignored: world ({:.1f},{:.1f}) is outside "
                                 "the area",
                                 tap.x, tap.y, wx, wy));
            return;
        }
        f64 vx = wx, vy = wy;
        if (area != manifest.map_areas.end() && !area->second.dynamic_markers.empty()) {
            const auto& d = area->second.dynamic_markers.front();
            if (d.scale_x != 0.0f) {
                vx = (wx - d.offset_x) / d.scale_x;
            }
            if (d.scale_y != 0.0f) {
                vy = (wy - d.offset_y) / d.scale_y;
            }
        }
        last_map_tap = std::pair{wx, wy};
        ++map_tap_seq;
        out_line(fmt::format("DSMod map tap ({},{}) -> world ({:.3f},{:.3f}) map ({:.3f},{:.3f})",
                             tap.x, tap.y, wx, wy, vx, vy));
        map_tap_ctx = {true, vx, vy, wx, wy, -1, {}};
        const Ran ran = run_named(ex.on_map_tap, std::nullopt, "map tap", tap.x, tap.y, &w.rect);
        tap_haptic(ran, w.haptic, std::nullopt, "map tap '" + ex.on_map_tap + "'");
        map_tap_ctx = {};
    };
    for (const auto& tap : taps) {
        if (page_anim_request.has_value()) {
            // A tap just started a page transition: the rest of this tick's taps would land on the
            // page sliding in, which input does not reach until the transition ends.
            out_line(fmt::format("DSMod tap ({},{}) dropped (page transition)", tap.x, tap.y));
            continue;
        }
        if (!tap.gesture_action.empty()) {
            // Continuous view callbacks use named-action gates and ABI1 payload transport,
            // without per-frame touch haptics or transforming any host widget pixels.
            run_named(tap.gesture_action, tap.gesture_payload, "gesture", tap.x, tap.y);
            continue;
        }
        if (!tap.hold_action.empty()) {
            // A fired press-and-hold (runtime 13): the on_hold widget's action, no hit test; the
            // hold haptic (manifest haptics "hold", strength "heavy" by default) when it ran.
            const Ran ran = run_named(tap.hold_action, tap.hold_payload, "hold", tap.x, tap.y);
            if (ran.result == ActionResult::Done) {
                // The hold's own strength (not the action's tap haptic): stronger than a tap.
                QueueHaptic(HapticKind::Hold, -1, -1, "hold '" + tap.hold_action + "'");
            } else if (ran.result == ActionResult::Refused) {
                QueueHaptic(HapticKind::Refused, -1, -1, "hold '" + tap.hold_action + "'");
            }
            continue;
        }
        if (!tap.swipe_action.empty()) {
            // A fired swipe (runtime 14): the swipe widget's action, no hit test; the swipe haptic
            // (manifest haptics "swipe", strength "light" by default) when it ran.
            const Ran ran = run_named(tap.swipe_action, std::nullopt, "swipe", tap.x, tap.y);
            if (ran.result == ActionResult::Done) {
                QueueHaptic(HapticKind::Swipe, -1, -1, "swipe '" + tap.swipe_action + "'");
            } else if (ran.result == ActionResult::Refused) {
                QueueHaptic(HapticKind::Refused, -1, -1, "swipe '" + tap.swipe_action + "'");
            }
            continue;
        }
        if (map_page) {
            const auto expanded = ExpandWidgets(page, snapshot);
            const s64 hit = HitTestIndex(expanded, snapshot, tap.x, tap.y, [&](const Widget& w) {
                return !w.on_tap.empty() || !w.select_group.empty() || !w.drop_action.empty() ||
                       w.input_block || map_tappable(w);
            });
            if (hit >= 0) {
                const Widget& w = expanded[static_cast<size_t>(hit)];
                if (map_tappable(w)) {
                    map_tap(w, static_cast<size_t>(hit), tap);
                    continue;
                }
                if (w.input_block && w.on_tap.empty() && w.select_group.empty() &&
                    w.drop_action.empty()) {
                    out_line(
                        fmt::format("DSMod tap ({},{}) swallowed by an input_block widget '{}'",
                                    tap.x, tap.y, w.id.empty() ? fmt::format("#{}", hit) : w.id));
                    continue;
                }
            }
        }
        if (interactive) {
            const auto expanded = ExpandWidgets(page, snapshot);
            const s64 hit = HitTestIndex(expanded, snapshot, tap.x, tap.y, [](const Widget& w) {
                return !w.on_tap.empty() || !w.select_group.empty() || !w.drop_action.empty();
            });
            if (hit < 0) {
                continue;
            }
            const Widget& w = expanded[static_cast<size_t>(hit)];
            if (!w.drop_action.empty() && !w.accept_group.empty()) {
                // Tap-then-tap: a target tapped while its group holds a selection takes it.
                if (const auto sel = selections.find(w.accept_group); sel != selections.end()) {
                    const s64 carried = sel->second;
                    selections.erase(sel);
                    const Ran ran =
                        run_named(w.drop_action, carried, "tap-drop", tap.x, tap.y, &w.rect);
                    tap_haptic(ran, w.haptic, HapticKind::Drop, "tap-drop '" + w.drop_action + "'");
                    continue;
                }
            }
            if (!w.select_group.empty()) {
                if (const auto carried = WidgetPayload(w, snapshot)) {
                    const auto sel = selections.find(w.select_group);
                    const bool clear = sel != selections.end() && sel->second == *carried;
                    if (clear) {
                        selections.erase(sel);
                    } else {
                        selections[w.select_group] = *carried;
                        if (*carried >= 0) {
                            last_selection[w.select_group] = *carried;
                        }
                    }
                    const std::string line =
                        fmt::format("DSMod select ({},{}) group '{}' = {}", tap.x, tap.y,
                                    w.select_group, clear ? -1 : *carried);
                    QueueHaptic(
                        HapticKind::Select, w.haptic, -1,
                        fmt::format("select {} = {}", w.select_group, clear ? -1 : *carried));
                    LOG_INFO(Core, "{}", line);
                    if (const char* const p = Common::DSMod::DevEnvironment("EDEN_DSMOD_CMD")) {
                        std::ofstream f(std::string(p) + ".out", std::ios::app);
                        if (f) {
                            f << line << '\n';
                        }
                    }
                }
            }
            if (!w.on_tap.empty()) {
                const Ran ran = run_named(w.on_tap, TapPayload(page, w, snapshot, tap.x, tap.y),
                                          "tap", tap.x, tap.y, &w.rect);
                tap_haptic(ran, w.haptic, std::nullopt, "tap '" + w.on_tap + "'");
            }
            continue;
        }
        const auto expanded = ExpandWidgets(page, snapshot);
        const s64 hit = HitTestIndex(expanded, snapshot, tap.x, tap.y,
                                     [](const Widget& w) { return !w.on_tap.empty(); });
        if (hit < 0) {
            continue;
        }
        const Widget& tapped = expanded[static_cast<size_t>(hit)];
        const std::string& action_name = tapped.on_tap;
        const auto it = manifest.actions.find(action_name);
        if (it == manifest.actions.end()) {
            LOG_WARNING(Core, "DSMod: widget references unknown action '{}'", action_name);
            continue;
        }
        // A widget with a payload hands it to its on_tap action (a picker cell -> set_value);
        // "$tap" carries the content-relative tap point instead (TapPayload).
        const auto carried = TapPayload(page, tapped, snapshot, tap.x, tap.y);
        const std::string suffix = carried ? fmt::format(" payload {}", *carried) : std::string{};
        LOG_INFO(Core, "DSMod: tap ({},{}) -> action '{}'{}", tap.x, tap.y, action_name, suffix);
        if (const char* const p = Common::DSMod::DevEnvironment("EDEN_DSMOD_CMD")) {
            std::ofstream f(std::string(p) + ".out", std::ios::app);
            if (f) {
                f << fmt::format("DSMod tap ({},{}) -> action '{}'{}\n", tap.x, tap.y, action_name,
                                 suffix);
            }
        }
        tap_haptic(Ran{RunAction(it->second, snapshot, carried, &tapped.rect), &it->second},
                   tapped.haptic, std::nullopt, "tap '" + action_name + "'");
    }
}

s64 ModRuntime::DropTargetAt(const StateSnapshot& snapshot, s32 x, s32 y) const {
    if (!drag_state.active || current_page >= manifest.pages.size() ||
        drag_state.page != current_page) {
        return -1;
    }
    const auto expanded = ExpandWidgets(manifest.pages[current_page], snapshot);
    const Widget* const source =
        drag_state.source >= 0 && static_cast<size_t>(drag_state.source) < expanded.size()
            ? &expanded[static_cast<size_t>(drag_state.source)]
            : nullptr;
    const std::string& group = drag_state.widget.select_group;
    const auto is_target = [&](const Widget& w) {
        // A drop target takes any drag unless both sides name a group and the groups differ;
        // the widget being dragged is never its own target.
        return !w.drop_action.empty() && &w != source &&
               (w.accept_group.empty() || group.empty() || w.accept_group == group);
    };
    // An input_block above the targets hides them: no hover, and a release there cancels.
    const s64 hit = HitTestIndex(expanded, snapshot, x, y,
                                 [&](const Widget& w) { return is_target(w) || w.input_block; });
    if (hit >= 0 && !is_target(expanded[static_cast<size_t>(hit)])) {
        return -1;
    }
    return hit;
}

s64 ModRuntime::InputBlockAt(const std::vector<Widget>& expanded, const StateSnapshot& snapshot,
                             s32 x, s32 y) const {
    const s64 hit = HitTestIndex(expanded, snapshot, x, y, [](const Widget& w) {
        return w.input_block || w.draggable || w.pan_zoom || !w.on_pan.empty() || !w.on_pinch.empty();
    });
    return hit >= 0 && expanded[static_cast<size_t>(hit)].input_block ? hit : -1;
}

void ModRuntime::ResetInteraction(const char* why) {
    system.GPU().DSModAux().ClearWheel();
    system.GPU().DSModAux().ClearPinch();
    callback_target = {};
    native_pinch_target = {};
    wheel_target.clear();
    wheel_dx = wheel_dy = 0.0f;
    if (drag_state.candidate || !selections.empty() || !pending_drops.empty()) {
        LOG_INFO(Core, "DSMod: selection/drag cleared ({})", why);
    }
    if (drag_state.candidate && !live_fingers.empty()) {
        gesture_moved = true; // the finger still down must not fire a tap on the new page
    }
    drag_state = {};
    selections.clear();
    last_selection.clear();
    pending_drops.clear();
    if (!map_selections.empty()) {
        LOG_INFO(Core, "DSMod: map selection cleared ({})", why);
    }
    map_selections.clear();
}

HapticKind ModRuntime::TapHapticKind(const Action& action) {
    return action.kind == ActionKind::Write || action.kind == ActionKind::SlotWrite
               ? HapticKind::Write
               : HapticKind::Tap;
}

void ModRuntime::QueueHaptic(HapticKind kind, HapticOverride widget_haptic,
                             HapticOverride action_haptic, std::string_view source) {
    const HapticsConfig& cfg = manifest.haptics;
    if (!cfg.enabled || kind >= HapticKind::Count) {
        return;
    }
    HapticStrength strength = cfg.strength[static_cast<size_t>(kind)];
    if (action_haptic >= 0) {
        strength = static_cast<HapticStrength>(action_haptic);
    }
    if (widget_haptic >= 0) {
        strength = static_cast<HapticStrength>(widget_haptic);
    }
    if (strength == HapticStrength::Off || strength <= pending_haptic.strength) {
        return; // one haptic per tick: the strongest request wins
    }
    pending_haptic = {strength, kind, std::string{source}};
}

void ModRuntime::FlushHaptic() {
    if (pending_haptic.strength == HapticStrength::Off) {
        return;
    }
    const PendingHaptic event = std::exchange(pending_haptic, PendingHaptic{});
    const std::string line = fmt::format(
        "DSMod haptic {} ({}: {})", HapticStrengthNames[static_cast<size_t>(event.strength)],
        HapticKindNames[static_cast<size_t>(event.kind)], event.source);
    LOG_INFO(Core, "{}", line);
    if (const char* const p = Common::DSMod::DevEnvironment("EDEN_DSMOD_CMD")) {
        std::ofstream f(std::string(p) + ".out", std::ios::app);
        if (f) {
            f << line << '\n';
        }
    }
    system.GPU().DSModAux().RaiseHaptic({static_cast<u8>(event.strength),
                                         static_cast<u8>(event.kind),
                                         static_cast<u8>(manifest.haptics.respect_system ? 1 : 0)});
}

void ModRuntime::PublishMapTap(StateSnapshot& snapshot) const {
    if (!last_map_tap) {
        return;
    }
    snapshot.floats["@map_tap_x"] = last_map_tap->first;
    snapshot.floats["@map_tap_y"] = last_map_tap->second;
    snapshot.ints["@map_tap_x"] = SaturatingToS64(last_map_tap->first);
    snapshot.ints["@map_tap_y"] = SaturatingToS64(last_map_tap->second);
    snapshot.ints["@map_tap_seq"] = map_tap_seq;
}

void ModRuntime::PublishFlags(StateSnapshot& snapshot) {
    for (const auto& [name, value] : flags) {
        snapshot.ints[flag_keys(name)] = value;
    }
}

std::optional<s64> FlagHostValueFrom(std::string_view name,
                                     const std::unordered_map<std::string, s64>& flags) {
    constexpr std::string_view Prefix = "@flag:";
    if (!name.starts_with(Prefix) || name.size() <= Prefix.size()) {
        return std::nullopt;
    }
    const auto it = flags.find(std::string{name.substr(Prefix.size())});
    if (it == flags.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<f64> PanZoomViewHostValueFrom(std::string_view name, const ViewState& states) {
    constexpr std::string_view Zoom = "@view_zoom:";
    constexpr std::string_view PanX = "@view_pan_x:";
    constexpr std::string_view PanY = "@view_pan_y:";
    const std::string_view prefix = name.starts_with(Zoom)   ? Zoom
                                    : name.starts_with(PanX) ? PanX
                                    : name.starts_with(PanY) ? PanY
                                                             : std::string_view{};
    if (prefix.empty() || name.size() <= prefix.size()) {
        return std::nullopt;
    }
    const std::string key{name.substr(prefix.size())};
    const auto it = states.find(key);
    if (it == states.end()) {
        return std::nullopt;
    }
    return prefix == Zoom ? it->second.zoom : prefix == PanX ? it->second.pan_x : it->second.pan_y;
}

std::optional<f64> ModRuntime::PanZoomViewHostValue(std::string_view name) const {
    std::scoped_lock lk{view_mutex};
    return PanZoomViewHostValueFrom(name, view_state);
}

std::optional<f64> MapViewHostValueFrom(std::string_view name, const MapDrawRecords& records,
                                         size_t records_page, size_t current_page) {
    constexpr std::string_view Prefix = "@map_view_";
    if (!name.starts_with(Prefix)) {
        return std::nullopt;
    }
    const size_t colon = name.find(':', Prefix.size());
    if (colon == std::string_view::npos || colon + 1 >= name.size()) {
        return std::nullopt;
    }
    const std::string_view field = name.substr(Prefix.size(), colon - Prefix.size());
    const std::string_view area = name.substr(colon + 1);
    if (records_page != current_page) {
        return std::nullopt;
    }
    // The last record of the area wins, exactly as PublishMapState's loop leaves the published
    // keys when two Map widgets draw the same area.
    const MapDrawRecord* found = nullptr;
    for (const auto& rec : records) {
        if (rec.area == area) {
            found = &rec;
        }
    }
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto& rec = *found;
    if (field == "cx") return rec.cx;
    if (field == "cy") return rec.cy;
    if (field == "ppw") return rec.ppw;
    if (field == "x") return rec.rect[0];
    if (field == "y") return rec.rect[1];
    if (field == "w") return rec.rect[2];
    if (field == "h") return rec.rect[3];
    if (field == "rot") return rec.rotation;
    return std::nullopt;
}

std::optional<f64> ModRuntime::MapViewHostValue(std::string_view name) const {
    std::scoped_lock rlk{map_records_mutex};
    return MapViewHostValueFrom(name, map_draw_records_published, map_records_page, current_page);
}

void ModRuntime::PublishMapState(StateSnapshot& snapshot, bool with_flags) {
    if (with_flags) {
        PublishFlags(snapshot);
    }
    if (!map_groups_ready) {
        map_groups.clear();
        for (const auto& [aname, area] : manifest.map_areas) {
            for (const auto& dm : area.dynamic_markers) {
                map_groups.insert(dm.group);
            }
        }
        map_groups_ready = true;
    }
    // map_records_page/map_draw_records_published are read under map_records_mutex from here on
    // -- see mod_runtime.h's own comment.
    const bool records_current = [this] {
        std::scoped_lock rlk{map_records_mutex};
        return map_records_page == current_page;
    }();
    // Each map widget's view as last drawn ("@map_view_cx:<area>", _cy, _ppw world units / canvas
    // px per unit, _x/_y/_w/_h its canvas rect): a package can place things over a world point the
    // way the map draws it (screen = rect centre + (world - c) * ppw, y up), one tick behind.
    if (records_current) {
        std::scoped_lock rlk{map_records_mutex};
        for (const auto& rec : map_draw_records_published) {
            snapshot.floats["@map_view_cx:" + rec.area] = rec.cx;
            snapshot.floats["@map_view_cy:" + rec.area] = rec.cy;
            snapshot.floats["@map_view_ppw:" + rec.area] = rec.ppw;
            snapshot.floats["@map_view_x:" + rec.area] = rec.rect[0];
            snapshot.floats["@map_view_y:" + rec.area] = rec.rect[1];
            snapshot.floats["@map_view_w:" + rec.area] = rec.rect[2];
            snapshot.floats["@map_view_h:" + rec.area] = rec.rect[3];
            // Runtime 22: the turn (view_rotate_bind), degrees clockwise; 0 = north-up.
            snapshot.floats["@map_view_rot:" + rec.area] = rec.rotation;
        }
    }
    if (!map_selections.empty() && !manifest.pages.empty()) {
        const Page& page = manifest.pages[std::min(current_page, manifest.pages.size() - 1)];
        // Pin mode off: a map whose taps are gated drops its selections.
        std::vector<std::string> drop;
        for (size_t i = 0; i < page.widgets.size(); ++i) {
            const Widget& w = page.widgets[i];
            if (w.type != WidgetType::Map || !w.map_extras || w.map_extras->tap_enabled.Empty() ||
                WidgetHidden(w, snapshot)) {
                continue;
            }
            const auto v =
                ResolveActionRef(w.map_extras->tap_enabled.point, snapshot, std::nullopt);
            const bool nonzero = v.has_value() && v->f.value_or(static_cast<f64>(v->i)) != 0.0;
            if (v.has_value() && nonzero != w.map_extras->tap_enabled.negate) {
                continue;
            }
            for (const auto& [group, idx] : map_selections) {
                drop.push_back(group);
            }
        }
        // A selected slot that is gone (removed pin) is no longer selected.
        if (records_current) {
            for (const auto& [group, idx] : map_selections) {
                if (const auto t = map_selection_tick.find(group);
                    t != map_selection_tick.end() && t->second >= tick_count) {
                    continue; // made this tick: the snapshot predates the write
                }
                std::scoped_lock rlk{map_records_mutex};
                for (const auto& rec : map_draw_records_published) {
                    const auto area = manifest.map_areas.find(rec.area);
                    if (area == manifest.map_areas.end()) {
                        continue;
                    }
                    for (const auto& dm : area->second.dynamic_markers) {
                        if (dm.group != group || dm.kind.empty()) {
                            continue;
                        }
                        std::string key = dm.kind;
                        if (const size_t at = key.find("{i}"); at != std::string::npos) {
                            key.replace(at, 3, std::to_string(idx));
                        }
                        const auto kv = snapshot.ints.find(key);
                        if (idx >= dm.count || kv == snapshot.ints.end() ||
                            (dm.has_hide_kind && kv->second == dm.hide_when_kind)) {
                            drop.push_back(group);
                        }
                    }
                }
            }
        }
        for (const auto& group : drop) {
            if (map_selections.erase(group) > 0) {
                LOG_INFO(Core, "DSMod: map selection '{}' cleared (pin mode off or slot gone)",
                         group);
            }
        }
    }
    for (const auto& group : map_groups) {
        const auto sel = map_selections.find(group);
        const s64 idx = sel == map_selections.end() ? -1 : sel->second;
        snapshot.ints[map_sel_keys(group)] = idx;
        s64 sx = -1, sy = -1;
        if (idx >= 0 && records_current) {
            std::scoped_lock rlk{map_records_mutex};
            for (const auto& rec : map_draw_records_published) {
                for (const auto& h : rec.hits) {
                    if (h.index == idx && h.group == group) {
                        sx = static_cast<s64>(std::lround(h.sx));
                        sy = static_cast<s64>(std::lround(h.sy));
                    }
                }
            }
        }
        snapshot.ints[map_sel_sx_keys(group)] = sx;
        snapshot.ints[map_sel_sy_keys(group)] = sy;
    }
    PublishMapTap(snapshot);
    for (const auto& [name, action] : manifest.actions) {
        if (action.kind != ActionKind::SlotWrite) {
            continue;
        }
        const auto slot = manifest.points.find(action.slot);
        if (slot == manifest.points.end()) {
            continue;
        }
        const s64 n = action.count > 0 ? action.count : std::max<s64>(1, slot->second.count);
        const s64 free_value = NormaliseToType(slot->second.type, action.free_value);
        const bool plain = slot->second.count <= 1 && slot->second.count_bind.empty();
        s64 used = 0;
        for (s64 i = 0; i < n; ++i) {
            const auto v = snapshot.ints.find(
                plain ? action.slot : element_keys(action.slot, static_cast<size_t>(i)));
            if (v != snapshot.ints.end() &&
                NormaliseToType(slot->second.type, v->second) != free_value) {
                ++used;
            }
        }
        snapshot.ints[slot_used_keys(name)] = used;
        snapshot.ints[slot_full_keys(name)] = used >= n ? 1 : 0;
    }
}

void ModRuntime::PublishInteraction(StateSnapshot& snapshot, bool before_taps) {
    // Before the taps (runtime 16) the page-change reset waits for the pass after them, so a
    // drop queued on this tick still runs, as it did before that pass existed.
    if (!before_taps && current_page != interact_page) {
        interact_page = current_page;
        ResetInteraction("page change");
    }
    if (!interact_groups_ready) {
        interact_groups.clear();
        for (const auto& page : manifest.pages) {
            for (const auto& w : page.widgets) {
                for (const std::string* g : {&w.select_group, &w.accept_group}) {
                    if (!g->empty() && g->find("{i}") == std::string::npos) {
                        interact_groups.insert(*g);
                    }
                }
            }
        }
        interact_groups_ready = true;
    }
    if (!before_taps) {
        // Dynamic groups the pre-tap pass published: DrainTaps may have cleared them since.
        for (const auto& key : early_dynamic_sel_keys) {
            snapshot.ints.erase(key);
        }
    }
    early_dynamic_sel_keys.clear();
    for (const auto& group : interact_groups) {
        snapshot.ints[sel_keys(group)] = -1;
        snapshot.ints[last_keys(group)] = -1;
    }
    for (const auto& [group, value] : selections) {
        snapshot.ints[sel_keys(group)] = value;
        if (before_taps && !interact_groups.contains(group)) {
            early_dynamic_sel_keys.push_back(sel_keys(group));
        }
    }
    for (const auto& [group, value] : last_selection) {
        snapshot.ints[last_keys(group)] = value;
        if (before_taps && !interact_groups.contains(group)) {
            early_dynamic_sel_keys.push_back(last_keys(group));
        }
    }
    const bool active = drag_state.active && drag_state.page == current_page;
    std::optional<DragInts> drag;
    if (active) {
        drag = DragInts{drag_state.payload, drag_state.x, drag_state.y, drag_state.hover};
    } else if (before_taps && !pending_drops.empty()) {
        // Runtime 16: the drag released this tick, until its drop action has run.
        const PendingDrop& d = pending_drops.back();
        drag = DragInts{d.payload, d.x, d.y, d.hover};
    }
    WriteDragInts(snapshot, drag);
    if (before_taps) {
        return; // the drag overlay is the renderer's: published after the taps
    }
    snapshot.drag.active = active;
    if (!active) {
        return;
    }
    snapshot.drag.widget = drag_state.widget;
    snapshot.drag.x = drag_state.x;
    snapshot.drag.y = drag_state.y;
    snapshot.drag.grab_dx = drag_state.grab_dx;
    snapshot.drag.grab_dy = drag_state.grab_dy;
    snapshot.drag.source = drag_state.source;
    snapshot.drag.hover = drag_state.hover;
}

VideoCore::DSMod::CompanionRect ModRuntime::TouchRect(u32 panel_w, u32 panel_h, u32 canvas_w,
                                                      u32 canvas_h) const {
    // A mirror page is drawn over the whole panel (renderer mirror path), so it maps as Stretch.
    return VideoCore::DSMod::CompanionTouchRect(
        panel_w, panel_h, canvas_w, canvas_h,
        system.GPU().DSModAux().mirror_enabled.load(std::memory_order_relaxed));
}

void ModRuntime::DriveCmdDrag([[maybe_unused]] u32 panel_w, [[maybe_unused]] u32 panel_h,
                              [[maybe_unused]] u32 canvas_w, [[maybe_unused]] u32 canvas_h) {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    if (!cmd_drag.active) {
        return;
    }
    auto& aux = system.GPU().DSModAux();
    // Land, rest a few ticks so the landing is observed, glide, rest over the target, release:
    // the same observable sequence a finger produces.
    constexpr u64 Dwell = 4;
    const u64 t = tick_count - cmd_drag.start;
    if (t >= Dwell + cmd_drag.move_ticks + cmd_drag.end_dwell) {
        aux.SetTouch({});
        cmd_drag.active = false;
        LOG_INFO(Core, "DSMod: console drag released");
        return;
    }
    const float f =
        t < Dwell ? 0.0f
                  : std::min(1.0f, static_cast<float>(t - Dwell) /
                                       static_cast<float>(std::max<u64>(1, cmd_drag.move_ticks)));
    const float cx = cmd_drag.x0 + (cmd_drag.x1 - cmd_drag.x0) * f;
    const float cy = cmd_drag.y0 + (cmd_drag.y1 - cmd_drag.y0) * f;
    // Both forms land where the canvas is drawn (Companion Ratio "Fit" may inset it): 0..1 is
    // normalised to the canvas rect, anything else is canvas pixels. A full rect is the panel.
    const auto fit = TouchRect(panel_w, panel_h, canvas_w, canvas_h);
    const float sx = cmd_drag.normalised ? 1.0f : static_cast<float>(std::max(1u, canvas_w));
    const float sy = cmd_drag.normalised ? 1.0f : static_cast<float>(std::max(1u, canvas_h));
    const float px = static_cast<float>(fit.x) + cx * static_cast<float>(fit.w) / sx;
    const float py = static_cast<float>(fit.y) + cy * static_cast<float>(fit.h) / sy;
    const VideoCore::DSMod::AuxTouchPoint point{
        .finger_id = 0x44,
        .x = static_cast<u32>(std::clamp(px, 0.0f, static_cast<float>(panel_w - 1))),
        .y = static_cast<u32>(std::clamp(py, 0.0f, static_cast<float>(panel_h - 1))),
        .attributes = t == 0 ? 1u : 0u,
        .delta_ns = 0,
    };
    aux.SetTouch(std::span{&point, 1});
#endif
}

} // namespace Core::Mods
