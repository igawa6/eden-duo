// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Pages over time: automatic page switches, page transitions and widget-group animations.
//   - DrivePageBinds: manifest "page_binds". A bind arms once its point (and ready_bind)
//     reads valid, fires on the edge into or out of `equals`, waits while a finger is down, and
//     is dropped while the current page is no_auto_leave; the switch itself goes through RunAction.
//   - DrivePageTransition: plays the PageAnimRequest RunAction left (slide / grow / shrink / fade).
//     It renders the "from" and "to" pages with RenderPageTo into the tick thread's own `canvas`
//     and publishes each blended frame to AuxRouting directly, bumping redraw_dispatch_generation
//     so a redraw-worker job still in flight cannot publish over it.
//   - UpdateGroupAnims / CancelAnimations: widget-group animations of the current page;
//     AnimTimeScale and DumpAnimFrame are the EDEN_DSMOD_ANIM_SCALE / EDEN_DSMOD_ANIM_DUMP hooks.
// Not here: the ordinary redraw path and dirty tracking (mod_redraw.cpp), drawing (mod_ui.cpp).
// Flow: DrivePageBinds is the page-binds stage of Tick, after ApplyEnforceRules. UpdateGroupAnims
// and DrivePageTransition are called from PublishUi (mod_redraw.cpp) in the redraw/publish stage.
// All of it runs on the tick thread; map_records_mutex is taken to publish the map draw records
// of a transition's render.

#include <algorithm>
#include <cmath>
#include <span>
#include "common/stb.h"

#include <cstdlib>
#include <fstream>
#include "common/logging.h"
#include "core/core.h"
#include "core/mods/mod_runtime.h"
#include "core/mods/mod_runtime_shared.h"
#include "video_core/gpu.h"

namespace Core::Mods {

void ModRuntime::DrivePageBinds(const StateSnapshot& snapshot) {
    if (manifest.page_binds.empty()) {
        return;
    }
    if (page_bind_state.size() != manifest.page_binds.size()) {
        page_bind_state.assign(manifest.page_binds.size(), PageBindState{});
    }
    for (size_t i = 0; i < manifest.page_binds.size(); ++i) {
        const PageBind& bind = manifest.page_binds[i];
        PageBindState& st = page_bind_state[i];

        // Not-before-game-loaded: stays fully unarmed (no baseline captured, nothing can edge)
        // until BOTH the bound point and (when given) ready_bind read a real value this tick.
        // Losing either later re-arms cold rather than comparing across the gap -- a save quit
        // back to a menu and a different file loaded must not fire off the old file's leftover
        // branch the instant it reads valid again.
        const auto v = SnapshotNumber(snapshot, bind.point);
        const bool ready = v.has_value() && (bind.ready.Empty() || GateOpen(bind.ready, snapshot));
        if (!ready) {
            st.armed = false;
            st.pending = false;
            continue;
        }
        const bool is_equal = std::fabs(*v - bind.equals) < 0.5;
        if (!st.armed) {
            // First valid read: capture the baseline silently. This is what keeps a package's
            // first page the one a user sees at the title screen even once the bind arms, and
            // what keeps a save that loads straight into "equals" state from firing a switch it
            // never crossed into.
            st.armed = true;
            st.last_equal = is_equal;
            st.pending = false;
            continue;
        }
        if (is_equal != st.last_equal) {
            // Edge: queue it. A second edge before the first fires (deferred below) replaces it
            // outright -- the newest edge always wins, matching a rapid double-tap on an ordinary
            // page action (RunAction/DrivePageTransition already let a fresh page action interrupt
            // one in flight; nothing extra is needed here for that case).
            st.pending = true;
            st.pending_equal = is_equal;
            st.last_equal = is_equal;
        }
        if (!st.pending) {
            continue;
        }
        // Defer while any finger is on the second screen: covers a mid-drag/mid-pan gesture and
        // the Pins-pane-style modal group generically (it already sets input_block, but this does
        // not need to know that -- any gesture in flight is reason enough not to rip the page out
        // from under it). Fires the instant every finger lifts; still a single edge, just a few
        // ticks late.
        if (!live_fingers.empty()) {
            continue;
        }
        const PageBindTarget& target = st.pending_equal ? bind.when_equal : bind.when_not_equal;
        st.pending = false; // consumed either way below
        if (target.page.empty()) {
            continue; // this branch is intentionally one-directional
        }
        if (current_page < manifest.pages.size() && manifest.pages[current_page].no_auto_leave) {
            // The page the user is currently on opted out of being auto-navigated away from.
            // Dropped, not queued: "never" means never for this edge, not "as soon as you leave".
            // The package's own manual entry point (if any) still works once they do leave by hand.
            LOG_INFO(Core, "DSMod: page_bind '{}' skipped: current page '{}' is no_auto_leave",
                     bind.point, manifest.pages[current_page].id);
            continue;
        }
        Action action;
        action.name = fmt::format("page_bind:{}", bind.point);
        action.kind = ActionKind::Page;
        action.page = target.page;
        action.transition = target.transition;
        action.duration_ms = target.duration_ms;
        action.easing = target.easing;
        action.shadow = target.shadow;
        action.origin = target.origin;
        const ActionResult result = RunAction(action, snapshot);
        LOG_INFO(Core, "DSMod: page_bind '{}' {} {:g} -> page '{}' ({})", bind.point,
                 st.pending_equal ? "==" : "!=", bind.equals, target.page,
                 result == ActionResult::Done ? "done" : "skipped");
    }
}

float ModRuntime::AnimTimeScale() {
    static const float scale = [] {
        const char* const v = std::getenv("EDEN_DSMOD_ANIM_SCALE");
        const float f = v != nullptr ? std::strtof(v, nullptr) : 1.0f;
        return f > 0.0f ? std::min(f, 100.0f) : 1.0f;
    }();
    return scale;
}

void ModRuntime::DumpAnimFrame(std::span<const u32> pixels, u32 w, u32 h,
                               const std::string& label) {
    static const std::string dir = [] {
        const char* const v = std::getenv("EDEN_DSMOD_ANIM_DUMP");
        return v != nullptr ? std::string{v} : std::string{};
    }();
    if (dir.empty() || pixels.size() < static_cast<size_t>(w) * h) {
        return;
    }
    // Test hook only: the PNG is written on a detached thread so the frame timing stays honest.
    std::vector<u8> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const u32 p = pixels[i];
        rgba[i * 4] = static_cast<u8>(p >> 16);
        rgba[i * 4 + 1] = static_cast<u8>(p >> 8);
        rgba[i * 4 + 2] = static_cast<u8>(p);
        rgba[i * 4 + 3] = 0xFF;
    }
    const std::string path = fmt::format("{}/{:04}_{}.png", dir, anim_dump_seq++, label);
    std::thread([path, w, h, rgba = std::move(rgba)] {
        stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(),
                       static_cast<int>(w) * 4);
    }).detach();
}

void ModRuntime::CancelAnimations(const char* why) {
    if (page_anim.active || page_anim_request.has_value() || group_moving) {
        LOG_INFO(Core, "DSMod: animations cancelled ({})", why);
    }
    page_anim.active = false;
    page_anim_request.reset();
    group_anims.clear();
    group_defs.clear();
    group_defs_page = ~size_t{0};
    group_moving = false;
    render_extras.groups.clear();
    canvas_page = ~size_t{0};
    dispatch_page = ~size_t{0};   // worker_canvas's own continuity baseline, mirrored
    widget_sig_page = ~size_t{0}; // dirty-rect baselines go stale the same way canvas_page does
}

void ModRuntime::UpdateGroupAnims(const StateSnapshot& snapshot) {
    group_moving = false;
    if (manifest.pages.empty()) {
        return;
    }
    const size_t page_index = std::min(current_page, manifest.pages.size() - 1);
    if (group_defs_page != page_index) {
        // A new page: every group starts at its state, nothing slides.
        group_defs.clear();
        group_anims.clear();
        group_defs_page = page_index;
        const auto& widgets = manifest.pages[page_index].widgets;
        for (const auto& w : widgets) {
            if (!w.anim || std::ranges::any_of(group_defs, [&](const GroupDef& d) {
                    return d.anim->key == w.anim->key;
                })) {
                continue;
            }
            GroupDef def{w.anim, AnimGroupBox(widgets, w.anim->key)};
            if (def.box[2] <= 0 || def.box[3] <= 0) {
                LOG_WARNING(Core,
                            "DSMod: anim group '{}' has no box (give \"box\" or sized widgets)",
                            w.anim->key);
            }
            if (w.anim->from == WidgetAnim::From::Widget) {
                if (const auto origin = WidgetRectById(widgets, w.anim->origin_widget)) {
                    def.origin_box = *origin;
                } else {
                    LOG_WARNING(Core,
                                "DSMod: anim group '{}' from widget:'{}' not found on this page "
                                "(no grow/shrink, just an instant appear)",
                                w.anim->key, w.anim->origin_widget);
                    def.origin_box = def.box; // degrade to an instant appear, not a crash
                }
            }
            group_defs.push_back(std::move(def));
        }
    }
    if (group_defs.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& def : group_defs) {
        const bool open = GateOpen(def.anim->gate, snapshot);
        const auto [it, inserted] = group_anims.try_emplace(def.anim->key);
        GroupAnim& g = it->second;
        if (inserted) {
            g.open = open;
            g.pos = open ? 1.0f : 0.0f;
            continue;
        }
        if (open != g.open) {
            g.open = open;
            g.moving = def.anim->ms > 0 && def.box[2] > 0 && def.box[3] > 0;
            g.start = now;
            g.start_pos = g.pos;
            g.frames = 0;
            g.frame_ms = 0.0;
            g.frame_max_ms = 0.0;
            // A sliding group redraws its box every frame; an instant one needs one more frame.
            g.settle_pending = !g.moving;
            if (!g.moving) {
                g.pos = open ? 1.0f : 0.0f;
            }
            LOG_INFO(Core, "DSMod anim group '{}' {} from {:.2f} ({} ms)", def.anim->key,
                     open ? "opening" : "closing", g.start_pos, def.anim->ms);
        }
        if (!g.moving) {
            continue;
        }
        const float target = g.open ? 1.0f : 0.0f;
        const float span = std::fabs(target - g.start_pos);
        const double duration =
            static_cast<double>(def.anim->ms) * AnimTimeScale() * static_cast<double>(span);
        const double elapsed = std::chrono::duration<double, std::milli>(now - g.start).count();
        if (span <= 0.0f || elapsed >= duration) {
            g.pos = target;
            g.moving = false;
            g.settle_pending = true; // one last frame of the box at rest
        } else {
            g.pos = g.start_pos + (target - g.start_pos) * static_cast<float>(elapsed / duration);
            group_moving = true;
        }
    }
}

namespace {
const char* TransitionLogName(PageTransition kind) {
    switch (kind) {
    case PageTransition::SlideUp:
        return "slide_up";
    case PageTransition::SlideDown:
        return "slide_down";
    case PageTransition::Grow:
        return "grow";
    case PageTransition::Shrink:
        return "shrink";
    case PageTransition::Fade:
        return "fade";
    case PageTransition::None:
    default:
        return "none";
    }
}
} // namespace

bool ModRuntime::DrivePageTransition(const StateSnapshot& snapshot, u64 sig, u32 target_w,
                                     u32 target_h) {
    auto& aux = system.GPU().DSModAux();
    const auto now = std::chrono::steady_clock::now();
    const auto& pages = manifest.pages;
    const auto usable = [&](size_t i) {
        return i < pages.size() && !pages[i].mirror && pages[i].id != "__debug";
    };
    if (page_anim_request.has_value()) {
        const PageAnimRequest req = *page_anim_request;
        page_anim_request.reset();
        if (!usable(req.from) || !usable(req.to) || req.to != current_page) {
            page_anim.active = false;
            return false; // the normal path shows the target at once
        }
        const auto t0 = std::chrono::steady_clock::now();
        canvas.Resize(target_w, target_h);
        LoadFont();
        if (font_metrics.Valid()) {
            // Same reasoning as PublishUi's identical block (mod_redraw.cpp).
            canvas.SetFont(GetImage(manifest.font_atlas_src).get(), &font_metrics);
        }
        {
            std::shared_ptr<const Image> icon_atlas;
            bool icon_pending = false;
            const FontMetrics* const icons =
                manifest.msbt.Enabled() ? NxIconFont(icon_atlas, icon_pending) : nullptr;
            canvas.SetIconFont(icon_atlas.get(), icons, icon_pending);
        }
        // 1. The page being left, frozen as it was on screen: the canvas already holds it after a
        //    CPU frame (or the previous target, when a transition is interrupted); otherwise
        //    (GPU path, HUD-only canvas) it is drawn once on the CPU.
        const bool reuse =
            page_anim.active || (canvas_page == req.from && !canvas_hud &&
                                 canvas.Width() == target_w && canvas.Height() == target_h);
        if (!reuse) {
            RenderPageTo(pages[req.from], snapshot, nullptr, nullptr, nullptr, nullptr);
        }
        anim_from_px.assign(canvas.Pixels().begin(), canvas.Pixels().end());
        // 2. The target page, drawn once.
        bool animating = false;
        if (!RenderPageTo(pages[req.to], snapshot, nullptr, &animating, &map_draw_records,
                          nullptr)) {
            page_anim.active = false;
            canvas_page = ~size_t{0};
            return false;
        }
        {
            // Same copy-out as PublishUi's own (mod_runtime.h's declaration comment on
            // map_draw_records_published).
            std::scoped_lock rlk{map_records_mutex};
            map_draw_records_published = map_draw_records;
            map_records_page = current_page;
        }
        canvas_page = current_page;
        canvas_hud = false;
        // This transition just became the authority for what's on screen, drawn into
        // ModRuntime::canvas -- but worker_canvas (the dispatch path's own canvas) was NOT touched
        // by any of this. dispatch_page must be invalidated the same way canvas_page just was, or a
        // later tick can find dispatch_page coincidentally equal to current_page (left over from
        // BEFORE this transition, if the target page was ever dispatched to directly in the past)
        // and wrongly conclude worker_canvas already holds this page's pixels, producing a partial
        // redraw painted onto a stale worker_canvas background. The abnormal "switched again
        // without a transition, or resized" branch (further down) already resets dispatch_page for
        // its own case; this is the ordinary/successful completion path, which needs the same
        // reset. Reachable in practice, e.g. by page actions that carry a "fade" transition back to
        // and from a map page.
        dispatch_page = ~size_t{0};
        // This transition is about to start publishing every tick via its own aux.PublishUiWith
        // call below, outside the dispatch mailbox -- bump so a worker job already in flight for
        // the page being LEFT cannot land its stale publish after this transition's.
        redraw_dispatch_generation.fetch_add(1, std::memory_order_relaxed);
        redraw_authority_generation.fetch_add(1, std::memory_order_relaxed);
        ApplyViewCorrections();
        page_anim = {};
        page_anim.active = true;
        page_anim.req = req;
        page_anim.start = now;
        page_anim.to_sig = sig;
        page_anim.to_assets_epoch = asset_epoch;
        page_anim.start_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        std::string origin_note;
        if (req.kind == PageTransition::Grow || req.kind == PageTransition::Shrink) {
            if (page_anim.req.origin[2] <= 0 || page_anim.req.origin[3] <= 0) {
                // Unresolved (no tapped widget and no "origin" match): grow/shrink about the
                // canvas centre rather than draw nothing there.
                page_anim.req.origin = {static_cast<s32>(target_w) / 2,
                                        static_cast<s32>(target_h) / 2, 0, 0};
                origin_note = " origin unresolved, using canvas centre";
            } else {
                origin_note = fmt::format(" origin [{},{},{},{}]", page_anim.req.origin[0],
                                          page_anim.req.origin[1], page_anim.req.origin[2],
                                          page_anim.req.origin[3]);
            }
        }
        const std::string line = fmt::format(
            "DSMod transition {} '{}' -> '{}' {} ms: start {:.2f} ms ({}){}",
            TransitionLogName(req.kind), pages[req.from].id, pages[req.to].id, req.duration_ms,
            page_anim.start_ms, reuse ? "old page reused, new page drawn" : "both pages drawn",
            origin_note);
        LOG_INFO(Core, "{}", line);
        if (const char* const p = std::getenv("EDEN_DSMOD_CMD")) {
            std::ofstream f(std::string(p) + ".out", std::ios::app);
            if (f) {
                f << line << '\n';
            }
        }
    }
    if (!page_anim.active) {
        return false;
    }
    if (page_anim.req.to != current_page || canvas.Width() != target_w ||
        canvas.Height() != target_h || anim_from_px.size() != canvas.Pixels().size()) {
        // Switched again without a transition (console), or resized: show the page as it is.
        page_anim.active = false;
        ui_signature_valid = false;
        widget_sig_page = ~size_t{0}; // re-baseline dirty-rect tracking, same as canvas_page below
        canvas_page = ~size_t{0};
        dispatch_page = ~size_t{0}; // a resize/page-switch invalidates worker_canvas's
                                    // own continuity baseline too
        // Defense in depth: this tick may not immediately re-dispatch (e.g. if PublishUi's own sig
        // check finds nothing changed right after falling through), so make sure a worker job
        // already in flight for whatever was showing before can't publish over whatever gets shown
        // next.
        redraw_dispatch_generation.fetch_add(1, std::memory_order_relaxed);
        redraw_authority_generation.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    static thread_local RuntimeStageStats stats;
    const RuntimeStageTimer timer{stats, "page-transition frame"};
    const auto t0 = std::chrono::steady_clock::now();
    // First-open snapshot race: a page's target-page snapshot above was rendered once,
    // synchronously; an image/composite that render asked for (romfs decode is async, off the
    // render path) can land a few ms later, and UiSignature does not depend on asset state, so
    // `sig` alone never notices. asset_epoch does: bumped by PumpNxAssets whenever anything lands,
    // it catches exactly the case the signature can't -- one extra redraw, same as the existing
    // sig-changed path below, and only when something actually landed since the last one.
    if (sig != page_anim.to_sig || asset_epoch != page_anim.to_assets_epoch) {
        // The target page changed while sliding in (a value, a glide, a landed image): draw it
        // again, once.
        bool animating = false;
        if (RenderPageTo(manifest.pages[current_page], snapshot, nullptr, &animating,
                         &map_draw_records, nullptr)) {
            // Same copy-out as PublishUi's own. Guarded on success only --
            // matching the other two call sites, a failed RenderPageTo leaves whatever was already
            // published alone rather than publishing a page that wasn't actually drawn.
            std::scoped_lock rlk{map_records_mutex};
            map_draw_records_published = map_draw_records;
            map_records_page = current_page;
        }
        ApplyViewCorrections();
        page_anim.to_sig = animating ? sig ^ 1 : sig;
        page_anim.to_assets_epoch = asset_epoch;
        ++page_anim.redraws;
    }
    const double duration =
        std::max(1.0, static_cast<double>(page_anim.req.duration_ms) * AnimTimeScale());
    const double elapsed = std::chrono::duration<double, std::milli>(now - page_anim.start).count();
    const float t = static_cast<float>(std::min(1.0, elapsed / duration));
    const float eased = ApplyEasing(page_anim.req.easing, t);
    if (gpu_composite_mode) {
        aux.ClearComposite(); // transition frames are whole canvases
    }
    const bool grow_style =
        page_anim.req.kind == PageTransition::Grow || page_anim.req.kind == PageTransition::Shrink;
    aux.PublishUiWith(target_w, target_h, [&](std::span<u32> out) {
        if (grow_style) {
            // Grow: background = the page being left (from), moving = the page being revealed
            // (to), p 0->1 origin->canvas. Shrink: the same two canvases the other way around
            // (background = to, moving = from), p 1->0 canvas->origin -- it starts full-screen
            // and collapses into the origin rect, mirroring Grow exactly.
            const bool grow = page_anim.req.kind == PageTransition::Grow;
            const std::span<const u32> background =
                grow ? std::span<const u32>{anim_from_px} : std::span<const u32>{canvas.Pixels()};
            const std::span<const u32> moving =
                grow ? std::span<const u32>{canvas.Pixels()} : std::span<const u32>{anim_from_px};
            const float p = grow ? eased : 1.0f - eased;
            ComposePageGrow(out, background, moving, target_w, target_h, page_anim.req.origin,
                            {0, 0, static_cast<s32>(target_w), static_cast<s32>(target_h)}, p,
                            page_anim.req.shadow);
        } else if (page_anim.req.kind == PageTransition::Fade) {
            // Both whole-canvas snapshots already exist (the same "from"/"to" pair every
            // other transition style uses) -- a plain alpha blend, no origin, no shadow. See
            // ComposePageFade's own comment for why both action keys are ignored here rather than
            // silently accepted.
            ComposePageFade(out, anim_from_px, canvas.Pixels(), target_w, target_h, eased);
        } else {
            ComposePageSlide(out, anim_from_px, canvas.Pixels(), target_w, target_h,
                             page_anim.req.kind, eased, page_anim.req.shadow);
        }
        DumpAnimFrame(out, target_w, target_h,
                      fmt::format("{}_f{:02}_p{:03}", TransitionLogName(page_anim.req.kind),
                                  page_anim.frames, static_cast<int>(t * 1000.0f)));
    });
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++page_anim.frames;
    page_anim.compose_ms += ms;
    page_anim.compose_max_ms = std::max(page_anim.compose_max_ms, ms);
    if (t >= 1.0f) {
        page_anim.active = false;
        const std::string line = fmt::format(
            "DSMod transition done: {} frame(s) in {:.0f} ms, frame avg {:.2f} ms max {:.2f} ms, "
            "target redrawn {}x",
            page_anim.frames, elapsed, page_anim.compose_ms / page_anim.frames,
            page_anim.compose_max_ms, page_anim.redraws);
        LOG_INFO(Core, "{}", line);
        if (const char* const p = std::getenv("EDEN_DSMOD_CMD")) {
            std::ofstream f(std::string(p) + ".out", std::ios::app);
            if (f) {
                f << line << '\n';
            }
        }
        const bool gpu_page =
            gpu_composite_mode &&
            std::ranges::any_of(manifest.pages[current_page].widgets,
                                [](const Widget& w) { return w.type == WidgetType::Map; });
        if (gpu_page || sig != page_anim.to_sig) {
            ui_signature_valid = false; // the GPU path (or a changed page) takes over next tick
        } else {
            last_ui_signature = sig; // the last frame IS the target page as drawn
            ui_signature_valid = true;
        }
    }
    return true;
}

} // namespace Core::Mods
