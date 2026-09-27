// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The second-screen page renderer: RenderPage and the widget draw functions it dispatches to.
//   - RenderPage: clears the canvas (fully transparent on the GPU path), expands the page's
//     widgets and the drag ghost (ExpandWidgetsInto, mod_ui_expand.cpp), then per widget: the
//     dirty-rect pre-reject on a partial redraw, the occlusion skip under a later opaque Map
//     widget, the visibility gates, the anim-group / scrolled-row clip and opacity, and the draw.
//     Selection highlights and scroll bars are painted after every widget, the ghost last.
//   - The draw functions, one per widget type, each taking a WidgetDrawContext
//     (mod_ui_internal.h): DrawImage (with the opt-in chrome cache), DrawRect, DrawLabel,
//     DrawValue, DrawBar, DrawPips, DrawButton. DrawMap is in mod_ui_map_widget.cpp.
//   - RenderDebugPage: the generated "__debug" page (every data point, its address and value).
// Not here: the Canvas calls (mod_ui_canvas.cpp, mod_ui_image.cpp, mod_ui_text.cpp), visibility
// and paint bounds (mod_ui_widget_state.cpp), deciding what to redraw and when (mod_redraw.cpp).
// Threads: RenderPage runs on the DSModRedraw worker and, for page transitions, the debug page and
// EDEN_DSMOD_SYNC_REDRAW, on the tick thread, at the same time. The process-wide state here (the
// widget profile, the chrome cache) and in the map widget (label bitmaps) each has its own mutex;
// follow_state_mutex and manifest_markers_mutex come from the caller.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>

#include "common/logging.h"
#include "core/mods/mod_ui.h"
#include "core/mods/mod_ui_internal.h"

namespace Core::Mods {

namespace {
// Chrome cache: cache a plain Image widget's own RENDERED OUTPUT (the actual pixels
// canvas.DrawImage/DrawImageRegion just wrote) and blit it back verbatim on a later tick
// instead of resampling+blending the source texture again, whenever every input that could
// change that output is still exactly what produced the cached copy. Measured
// (EDEN_DSMOD_PROFILE_WIDGETS=1, dungeon page, real walk): with no eligible Map-type occluder
// there (the occlusion skip below only recognises WidgetType::Map, hidden on this page's
// dungeon sub-state), LA's "bg"/"map_sheet" AND, more expensively, the active dungeon floor's
// own "dgn_grid_<i>"/"dgn_panel_<i>_f<n>" composite images (~5-8ms and ~2.5-3.1ms per draw)
// redraw on 90-100% of walking ticks though none of them depend on anything that changes while
// walking -- only the small player marker on top of them does. Chosen over generalising the
// occlusion skip (recognising more widget TYPES as occluders) because occlusion only ever helps
// the widgets UNDER something else that's opaque and unconditional; the dungeon page's own
// grid/panel widgets have nothing later drawn over their full rect to occlude them with, so
// nothing short of caching THEIR OWN output helps there.
//
// Deliberately NOT keyed on the widget's declared bind fields (the way the dirty-scan's own
// WidgetDependencyHash is, per-name): keying on the actual RESOLVED draw inputs instead --
// the resolved Image identity, tint, source-rect window, flip flags, destination rect, and
// canvas size -- is strictly sufficient (a deterministic draw call with identical inputs has
// identical output, full stop) without maintaining a hand-written enumeration of "which bind
// fields this widget type reads" that the dirty-scan's own comment already flags as an easy
// thing to miss. It also means a bind that happens to be constant right now is cached exactly
// as safely as a widget declared with no bind at all, and a bind that starts changing
// self-invalidates the moment its resolved effect on the draw actually differs -- satisfying
// "do not assume no-bind holds for every package" by never depending on that in the first
// place. The resolved Image* pointer specifically is what catches a hot asset/theme reload (a
// freshly decoded Image object at a new address) even though the widget's own src string,
// tint and rect are all unchanged.
struct ChromeCacheKey {
    const void* image{};
    u32 tint{};
    std::array<float, 4> src_rect{};
    bool flip_x{};
    bool flip_y{};
    s32 x{}, y{}, rw{}, rh{};
    u32 canvas_w{}, canvas_h{};
    bool operator==(const ChromeCacheKey&) const = default;
};
struct ChromeCacheEntry {
    ChromeCacheKey key{};
    std::vector<u32> pixels; // rw * rh, tightly packed row-major (NOT canvas stride)
};
/// The chrome cache's process-wide state, made on first use and shared by every thread's
/// RenderPage.
struct ChromeCacheState {
    std::unordered_map<std::string, ChromeCacheEntry> chrome_cache;
    std::mutex chrome_cache_mutex; // shared across threads, like RenderPage's profile_mutex
    // Chrome cache: OPT-IN, default OFF -- EDEN_DSMOD_CHROME_CACHE=1 turns it on.
    //
    // A live A/B (cache on vs off) found the measured perf win is real (dungeon-walking worker cost
    // 10-14ms -> 3-3.7ms, zero over-16.7ms ticks vs up to 38/144) but ALSO found a real,
    // reproducible pixel bug even after narrowing the cache to exclude "composite:" sources and the
    // src_rect/flip draw path entirely (leaving only the plain whole-image DrawImage path, exactly
    // where bg/map_sheet/a dungeon floor's own grid background live, all independently confirmed
    // pixel-correct on their own): with that narrowed cache still ON by default, the Gear/Items X
    // and Y equip-slot discs (both "composite:slot_disc", drawn through a DIFFERENT widget than
    // anything this cache touches) render with a wrong, stuck highlight colour, while the SAME
    // composite family at a different screen position ("composite:slot_disc_small", the B disc,
    // gated behind need_bind/hide_bind so it draws later) renders correctly. Turning the cache off
    // entirely (this flag) makes X/Y correct again on the same binary; turning it back on
    // reproduces the wrong colour again -- repeatable, not a one-off. The most likely mechanism
    // (not confirmed): this cache's own speedup shifts WHEN "composite:slot_disc" is first
    // requested relative to whatever game-state read decides its highlight colour, landing on an
    // unsettled value that then gets memoized -- a latent timing sensitivity in that composite's
    // own build path (outside this cache, outside this file's Image-widget case) that this cache's
    // speed happens to expose, not a correctness bug in the cache's OWN blit logic (which only ever
    // replays bytes a real draw already produced). Not root-caused further. Because pixel exactness
    // must not regress, the cache is OFF by default; OFF costs nothing (identical to having no
    // cache, verified pixel-identical with the flag unset) and leaves the code, the measurement,
    // and the diagnosis in the tree for whoever resolves the composite-timing question next.
    const bool chrome_cache_enabled = [] {
        const char* on = std::getenv("EDEN_DSMOD_CHROME_CACHE");
        return on != nullptr && std::strcmp(on, "0") != 0 && std::strcmp(on, "false") != 0 &&
               std::strcmp(on, "FALSE") != 0;
    }();
};

ChromeCacheState& GetChromeCacheState() {
    static ChromeCacheState state;
    return state;
}

// Tries a cached blit for `cache_key`/`key`; returns whether it drew anything. Refuses whenever
// the canvas's current opacity isn't exactly 1.0 -- a page-transition fade or an animating
// widget-group's own SetLayerOpacity means this tick's correct output is a partial blend the
// cache (captured only at full opacity) cannot reproduce.
bool TryChromeBlit(Canvas& canvas, const std::string& cache_key, const ChromeCacheKey& key) {
    auto& [chrome_cache, chrome_cache_mutex, chrome_cache_enabled] = GetChromeCacheState();
    if (!chrome_cache_enabled || canvas.CurrentOpacity() < 1.0f) {
        return false;
    }
    std::scoped_lock lk{chrome_cache_mutex};
    const auto it = chrome_cache.find(cache_key);
    if (it == chrome_cache.end() || !(it->second.key == key)) {
        return false;
    }
    canvas.BlitRaw(key.x, key.y, key.rw, key.rh, it->second.pixels.data(), key.rw);
    return true;
}

// Captures the canvas region [x,y,rw,rh] a real draw just wrote, for a future tick's hit.
// Refuses (a) below full opacity, same reasoning as TryChromeBlit, and (b) whenever the
// CURRENT CLIP does not fully contain the widget's own rect -- on a narrow partial redraw
// (the dirty region only overlapping PART of this widget) the real draw only touched that
// sub-rect, so capturing the "full" rect would bake in stale pixels for the untouched part.
// A hit can still use a cache built earlier during a wider draw even on a later narrow-clip
// tick (BlitRaw intersects with the current clip itself) -- only building/refreshing the
// cache is gated this way, not using it.
void CaptureChrome(Canvas& canvas, const std::string& cache_key, const ChromeCacheKey& key) {
    auto& [chrome_cache, chrome_cache_mutex, chrome_cache_enabled] = GetChromeCacheState();
    if (!chrome_cache_enabled || canvas.CurrentOpacity() < 1.0f || key.rw <= 0 || key.rh <= 0) {
        return;
    }
    const auto clip = canvas.Clip();
    if (clip[0] > key.x || clip[1] > key.y || clip[0] + clip[2] < key.x + key.rw ||
        clip[1] + clip[3] < key.y + key.rh) {
        return;
    }
    const s32 cw = static_cast<s32>(canvas.Width());
    const s32 ch = static_cast<s32>(canvas.Height());
    if (key.x < 0 || key.y < 0 || key.x + key.rw > cw || key.y + key.rh > ch) {
        return; // off-canvas: refuse rather than mis-handle the clamp
    }
    std::vector<u32> snap(static_cast<size_t>(key.rw) * static_cast<size_t>(key.rh));
    const auto& px = canvas.Pixels();
    for (s32 row = 0; row < key.rh; ++row) {
        std::memcpy(snap.data() + static_cast<size_t>(row) * static_cast<size_t>(key.rw),
                    px.data() + static_cast<size_t>(key.y + row) * static_cast<size_t>(cw) +
                        static_cast<size_t>(key.x),
                    static_cast<size_t>(key.rw) * sizeof(u32));
    }
    std::scoped_lock lk{chrome_cache_mutex};
    chrome_cache[cache_key] = ChromeCacheEntry{key, std::move(snap)};
}

/// WidgetType::Image: a picture (by src, bound name, table, threshold or format), whole,
/// cropped, mirrored, turning, trembling or filled as a gauge.
void DrawImage(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Manifest& manifest = ctx.manifest;
    const Page& page = ctx.page;
    const StateSnapshot& snapshot = ctx.snapshot;
    const ImageProvider& images = ctx.images;
    const Widget& widget = ctx.widget;
    const size_t widget_index = ctx.widget_index;
    const ViewTransform& view = ctx.view;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    if (!images || (widget.src.empty() && widget.src_bind.empty())) {
        return;
    }
    // Images get the same linear transform as values, so an icon can be addressed as
    // "base id + upgrade level" straight out of two numbers the game keeps apart.
    s64 img_value = value;
    if (widget.mul != 1)
        img_value *= widget.mul;
    if (widget.div > 1)
        img_value /= widget.div;
    img_value += widget.add;
    if (!widget.empty_src.empty()) {
        if (const std::shared_ptr<const Image> under = images(widget.empty_src); under != nullptr) {
            const auto& r = widget.empty_rect;
            canvas.DrawImageRegion(
                x, y, rw, rh, *under, static_cast<s32>(r[0] * static_cast<float>(under->w)),
                static_cast<s32>(r[1] * static_cast<float>(under->h)),
                std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(under->w))),
                std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(under->h))),
                widget.color);
        }
    }
    std::string source = widget.src;
    if (!widget.src_names.empty() && img_value >= 0 &&
        img_value < static_cast<s64>(widget.src_names.size())) {
        source = widget.src_names[static_cast<size_t>(img_value)];
    }
    if (!widget.src_thresholds.empty()) {
        // Fatigue and similar bands: the first entry whose ceiling the value fits under
        // wins, so an irregular 0 / 1-49 / 50-79 / 80-100 split maps straight to four
        // faces.
        source = widget.src_thresholds.back().second;
        for (const auto& [ceiling, path] : widget.src_thresholds) {
            if (value <= ceiling) {
                source = path;
                break;
            }
        }
    }
    if (!widget.src_format.empty()) {
        // Build an icon path from a numeric id, e.g. "...#item_%03d_00.xtx". Only a single
        // integer conversion is allowed, so a stray %s/%n in the manifest cannot be abused.
        bool safe = true;
        int convs = 0;
        for (size_t i = 0; i + 1 < widget.src_format.size(); ++i) {
            if (widget.src_format[i] != '%')
                continue;
            size_t j = i + 1;
            while (j < widget.src_format.size() &&
                   (std::isdigit(static_cast<unsigned char>(widget.src_format[j])) ||
                    widget.src_format[j] == '0' || widget.src_format[j] == '-' ||
                    widget.src_format[j] == '.')) {
                ++j;
            }
            const char c = j < widget.src_format.size() ? widget.src_format[j] : '\0';
            if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X') {
                ++convs;
            } else if (c == '%') {
                // literal %% is fine
            } else {
                safe = false;
                break;
            }
            i = j;
        }
        if (safe && convs == 1) {
            char sbuf[512];
            std::snprintf(sbuf, sizeof(sbuf), widget.src_format.c_str(),
                          static_cast<int>(img_value));
            source = sbuf;
        }
    }
    if (!widget.src_bind.empty()) {
        // The game names what it is showing; the package says which picture that is.
        const auto text = snapshot.texts.find(widget.src_bind);
        if (text == snapshot.texts.end()) {
            return;
        }
        const auto mapped = manifest.sprite_map.find(text->second);
        if (mapped == manifest.sprite_map.end()) {
            // Module-owned images use logical keys, validated by that module's decoder.
            if (!text->second.starts_with("module:")) {
                return;
            }
            source = text->second;
        } else {
            source = mapped->second;
        }
    }
    const std::shared_ptr<const Image> image = images(source);
    // Pan and zoom on a picture is a narrower window onto it: at 2x we sample half the
    // source across the same rectangle. Panning is clamped so the content cannot be
    // dragged off its own frame and leave the user staring at nothing.
    std::array<float, 4> eff = widget.src_rect;
    if (widget.pan_zoom && widget.rect[2] > 0 && widget.rect[3] > 0) {
        const float zoom = std::max(view.zoom, 0.01f);
        const float span_u = (eff[2] - eff[0]) / zoom;
        const float span_v = (eff[3] - eff[1]) / zoom;
        const float max_pan_x = static_cast<float>(widget.rect[2]) * (1.0f - 1.0f / zoom);
        const float max_pan_y = static_cast<float>(widget.rect[3]) * (1.0f - 1.0f / zoom);
        const float px = std::clamp(view.pan_x, 0.0f, std::max(0.0f, max_pan_x));
        const float py = std::clamp(view.pan_y, 0.0f, std::max(0.0f, max_pan_y));
        const float u0 = eff[0] + (px / static_cast<float>(widget.rect[2])) * (eff[2] - eff[0]);
        const float v0 = eff[1] + (py / static_cast<float>(widget.rect[3])) * (eff[3] - eff[1]);
        eff = {u0, v0, u0 + span_u, v0 + span_v};
    }
    const auto region = [&](const Image& img, s32 tint) {
        const auto& r = eff;
        const s32 sx = static_cast<s32>(r[0] * static_cast<float>(img.w));
        const s32 sy = static_cast<s32>(r[1] * static_cast<float>(img.h));
        const s32 sw = std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(img.w)));
        const s32 sh = std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(img.h)));
        canvas.DrawImageRegion(x, y, rw, rh, img, sx, sy, sw, sh, static_cast<u32>(tint),
                               widget.flip_x, widget.flip_y);
    };
    if (image == nullptr) {
        return;
    }
    // Chrome cache, found by pixel verification, not assumed -- "composite:" sources (e.g.
    // "composite:slot_disc_small", the Gear/Items equip-slot highlight disc;
    // "composite:dgn_7_f0", a dungeon floor's room-reveal-state panel) can be REBUILT IN
    // PLACE, the same Image object's pixels overwritten to reflect new state (a
    // newly-revealed room, a newly-equipped slot) without the resolved image pointer ever
    // changing -- the exact opposite of the "a reload allocates a new Image at a new
    // address" assumption the cache's pointer-identity key relies on for correctness. A
    // live A/B (this binary with EDEN_DSMOD_CHROME_CACHE=0 vs on) caught this directly: the
    // X/Y equip-slot disc froze at whatever highlight state it was first cached in (a
    // wrong, permanently-green disc instead of tracking the real selection), and a Tail
    // Cave room's reveal-state tint froze the same way. Plain, non-"composite:" texture
    // sources (bg's "ItemBGTex_00^o", map_sheet's "Paper_02^_D", a dungeon floor's own
    // "DgnMapGrid_00^_A") are ordinary, once-decoded, never-mutated assets -- confirmed
    // pixel-identical to an uncached draw in the same A/B -- so only composites are
    // excluded here, not every Image widget.
    const bool chrome_eligible = !source.starts_with("composite:");
    const bool whole = eff[0] == 0.0f && eff[1] == 0.0f && eff[2] == 1.0f && eff[3] == 1.0f;
    if (widget.shake != 0.0f) {
        // A trembling piece (the version warning's X): a fresh pseudo-random offset within
        // +-shake px every 3 ticks (20 Hz), hashed off the tick so it never settles into
        // a visible loop. The page's 15 Hz signature keeps it redrawn while visible.
        const u32 step = static_cast<u32>((snapshot.tick / 3) * 2654435761u);
        const s32 span = std::max(1, static_cast<s32>(std::lround(widget.shake)) * 2 + 1);
        const s32 amp = span / 2;
        x += static_cast<s32>((step >> 8) % static_cast<u32>(span)) - amp;
        y += static_cast<s32>((step >> 20) % static_cast<u32>(span)) - amp;
    }
    if (widget.fill_bind.empty()) {
        if (widget.spin != 0.0f) {
            // A turning piece (loading wheel): the angle advances with the tick, the
            // page's 15 Hz signature keeps it redrawn while the widget is visible.
            const float deg =
                std::fmod(widget.spin * static_cast<float>(snapshot.tick % 216000) / 60.0f, 360.0f);
            const auto& r = eff;
            canvas.DrawImageRegionRotated(
                x, y, rw, rh, *image, static_cast<s32>(r[0] * static_cast<float>(image->w)),
                static_cast<s32>(r[1] * static_cast<float>(image->h)),
                std::max(1, static_cast<s32>((r[2] - r[0]) * static_cast<float>(image->w))),
                std::max(1, static_cast<s32>((r[3] - r[1]) * static_cast<float>(image->h))),
                widget.color, deg * 3.14159265f / 180.0f);
        } else if (whole && !widget.flip_x && !widget.flip_y) {
            // Chrome cache: see chrome_eligible above and the comment on ChromeCacheKey for
            // the full reasoning. The key is built from this exact call's real inputs, not
            // from the widget's declared bind names.
            const std::string chrome_key = page.id + "#" + std::to_string(widget_index);
            const ChromeCacheKey ck{image.get(),
                                    static_cast<u32>(widget.color),
                                    eff,
                                    widget.flip_x,
                                    widget.flip_y,
                                    x,
                                    y,
                                    rw,
                                    rh,
                                    canvas.Width(),
                                    canvas.Height()};
            if (!(chrome_eligible && TryChromeBlit(canvas, chrome_key, ck))) {
                canvas.DrawImage(x, y, rw, rh, *image, widget.color);
                if (chrome_eligible) {
                    CaptureChrome(canvas, chrome_key, ck);
                }
            }
        } else {
            // A mirrored whole image takes the region path too (same sampling as
            // DrawImage), so flip_x/flip_y work with or without src_rect. Chrome cache: NOT
            // cached here, deliberately -- see the comment on ChromeCacheKey. Live
            // verification found a second, separate real bug specifically on
            // this src_rect/flip path (a Gear/Items equip-slot's green HUD ring, a PLAIN,
            // non-"composite:" texture cropped via src_rect, rendered wrong only when its
            // own cache entry was ever consulted -- confirmed by direct A/B,
            // `EDEN_DSMOD_CHROME_CACHE=0` restores it), not root-caused. Every measured win
            // (bg, map_sheet, a dungeon floor's own grid background) draws through the
            // OTHER branch (whole, unrotated, unflipped, no src_rect) just above, so
            // narrowing the cache to that branch keeps all of the win without that
            // correctness risk.
            region(*image, static_cast<s32>(widget.color));
        }
    } else {
        const s64 fill_value = snapshot.GetInt(widget.fill_bind);
        const s64 span = maximum > 0 ? maximum : 1;
        const auto& sr = widget.src_rect;
        canvas.DrawImageFilled(x, y, rw, rh, *image, widget.color,
                               static_cast<float>(fill_value) / static_cast<float>(span),
                               static_cast<s32>(sr[0] * static_cast<float>(image->w)),
                               static_cast<s32>(sr[1] * static_cast<float>(image->h)),
                               static_cast<s32>((sr[2] - sr[0]) * static_cast<float>(image->w)),
                               static_cast<s32>((sr[3] - sr[1]) * static_cast<float>(image->h)));
    }
}

/// WidgetType::Rect: a filled box with an optional (breathing) frame.
void DrawRect(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const StateSnapshot& snapshot = ctx.snapshot;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    u32 bg = widget.bg;
    u32 col = widget.color;
    if (widget.pulse) {
        // breathing: the fill's and the outline's alpha swing 20..100 % of their set
        // value over ~0.7 s (the page's 15 Hz signature keeps it redrawn while visible)
        const float a = BlinkAlpha(snapshot.tick, 40, 0.2f);
        const auto breathe = [&](u32 c) {
            const u32 amax = (c >> 24) & 0xFF;
            return (c & 0x00FFFFFFu) |
                   (static_cast<u32>(std::clamp(a * static_cast<float>(amax), 0.0f, 255.0f)) << 24);
        };
        bg = breathe(bg);
        col = breathe(col);
    }
    canvas.FillRect(x, y, rw, rh, bg);
    if (widget.color != 0) {
        canvas.FrameRect(x, y, rw, rh, std::max(1, widget.frame), col);
    }
}

/// WidgetType::Label: text picked by value, by key, from a string point or literal.
void DrawLabel(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const StateSnapshot& snapshot = ctx.snapshot;
    const TextProvider& texts = ctx.texts;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    // The text: picked by a value (text_bind + text_map), named by key (text_src), a string
    // point (bind_text), or the literal. A key the runtime cannot answer yet draws nothing;
    // the page is redrawn when the text lands. texts() returns shared_ptr<const
    // std::string> (see TextProvider's own declaration comment); msbt_owner keeps a
    // resolved msbt string alive past resolve()'s return, for as long as `shown` (below) is
    // still in use.
    std::shared_ptr<const std::string> msbt_owner;
    const auto resolve = [&texts, &msbt_owner](const std::string& ref) -> const std::string* {
        if (!ref.starts_with("msbt:")) {
            return &ref;
        }
        msbt_owner = texts ? texts(ref) : nullptr;
        return msbt_owner.get();
    };
    const std::string* shown = nullptr;
    if (!widget.text_bind.empty()) {
        const auto bound = snapshot.ints.find(widget.text_bind);
        if (bound != snapshot.ints.end() && bound->second != -1 && widget.text_map) {
            if (const auto m = widget.text_map->find(bound->second); m != widget.text_map->end()) {
                shown = resolve(m->second);
            }
        }
    } else if (!widget.text_src.empty()) {
        shown = resolve(widget.text_src);
    } else if (!widget.bind_text.empty()) {
        static const std::string dash = "-";
        const auto text = snapshot.texts.find(widget.bind_text);
        shown = text == snapshot.texts.end() ? &dash : &text->second;
    } else {
        shown = &widget.text;
    }
    if (shown == nullptr || shown->empty()) {
        return;
    }
    canvas.SetIconSilhouette(widget.icon_silhouette);
    if (widget.wrap_width > 0 || shown->find('\n') != std::string::npos) {
        canvas.DrawTextBlock(x, y, *shown, widget.text_scale, widget.color, widget.align,
                             widget.wrap_width, widget.max_lines, widget.line_gap);
    } else {
        canvas.DrawTextAligned(x, y, *shown, widget.text_scale, widget.color, widget.align);
    }
    canvas.SetIconSilhouette(false);
}

/// WidgetType::Value: a bound number, transformed, named or padded, as text.
void DrawValue(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Manifest& manifest = ctx.manifest;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    char buf[128];
    // Linear transform on the raw read: max stamina = berries*15 + 150; stamina/fatigue
    // are stored at twice the shown value (div folds that out).
    s64 shown = value;
    if (widget.mul != 1)
        shown *= widget.mul;
    if (widget.div > 1)
        shown /= widget.div;
    shown += widget.add;
    if (!widget.table.empty()) {
        // Shared value->name list declared once in the manifest (item names, character
        // names).
        const auto tbl = manifest.tables.find(widget.table);
        const char* label = "";
        if (tbl != manifest.tables.end() && shown >= 0 &&
            shown < static_cast<s64>(tbl->second.size())) {
            label = tbl->second[static_cast<size_t>(shown)].c_str();
        }
        std::snprintf(buf, sizeof(buf), "%s%s", widget.text.c_str(), label);
    } else if (!widget.names.empty() && shown >= 0 &&
               shown < static_cast<s64>(widget.names.size())) {
        std::snprintf(buf, sizeof(buf), "%s%s", widget.text.c_str(),
                      widget.names[static_cast<size_t>(shown)].c_str());
    } else if (widget.pad > 0) {
        std::snprintf(buf, sizeof(buf), "%s%0*lld", widget.text.c_str(), widget.pad,
                      static_cast<long long>(shown));
    } else if (!widget.max_sep.empty() && !widget.max_bind.empty()) {
        // "184/199" as one run of text, so the slash hugs the digits whatever their count
        std::snprintf(buf, sizeof(buf), "%s%lld%s%lld", widget.text.c_str(),
                      static_cast<long long>(shown), widget.max_sep.c_str(),
                      static_cast<long long>(maximum));
    } else {
        std::snprintf(buf, sizeof(buf), "%s%lld", widget.text.c_str(),
                      static_cast<long long>(shown));
    }
    if (widget.suffix.empty()) {
        canvas.DrawTextAligned(x, y, buf, widget.text_scale, widget.color, widget.align);
    } else {
        canvas.DrawTextAligned(x, y, std::string{buf} + widget.suffix, widget.text_scale,
                               widget.color, widget.align);
    }
}

/// WidgetType::Bar: a horizontal gauge of value / maximum.
void DrawBar(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    canvas.FillRect(x, y, rw, rh, widget.bg);
    const s64 span = maximum > 0 ? maximum : 1;
    const s64 clamped = std::clamp<s64>(value, 0, span);
    const s32 filled = static_cast<s32>(static_cast<s64>(rw) * clamped / span);
    canvas.FillRect(x, y, filled, rh, widget.color);
    canvas.FrameRect(x, y, rw, rh, 2, widget.color);
}

/// WidgetType::Pips: a run of sprites or squares, `value` of them lit.
void DrawPips(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const ImageProvider& images = ctx.images;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rh = ctx.rh;
    const s64 value = ctx.value;
    const s64 maximum = ctx.maximum;
    if (!widget.src.empty() && images) {
        if (const std::shared_ptr<const Image> pip = images(widget.src); pip != nullptr) {
            // Values are garbage until the game's own state exists, and a run of pips is
            // drawn one sprite at a time: without a bound, a nonsense maximum on the
            // first frames asks for millions of draws and takes the app down.
            const s64 total = std::clamp<s64>(maximum > 0 ? maximum : value, 0, MaxPips);
            const s32 step = widget.rect[2] + 8;
            for (s64 i = 0; i < total; ++i) {
                const s32 px = widget.rect[0] + static_cast<s32>(i) * step;
                const u32 tint = i < value ? widget.color : widget.bg;
                canvas.DrawImage(px, widget.rect[1], widget.rect[2], widget.rect[3], *pip, tint);
            }
            return;
        }
    }
    const s64 total = maximum > 0 ? maximum : value;
    const s32 count = static_cast<s32>(std::clamp<s64>(total, 0, MaxPips));
    const s32 pip = std::max(4, rh);
    for (s32 i = 0; i < count; ++i) {
        const s32 px = x + i * (pip + pip / 3);
        const bool on = i < value;
        canvas.FillRect(px, y, pip, pip, on ? widget.color : widget.bg);
        canvas.FrameRect(px, y, pip, pip, 1, widget.color);
    }
}

/// WidgetType::Button: a boxed or pill-shaped caption.
void DrawButton(const WidgetDrawContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Widget& widget = ctx.widget;
    s32& x = ctx.x;
    s32& y = ctx.y;
    const s32 rw = ctx.rw;
    const s32 rh = ctx.rh;
    if (widget.pill) {
        canvas.Pill(x, y, rw, rh, 3, widget.bg, widget.color);
    } else {
        canvas.FillRect(x, y, rw, rh, widget.bg);
        canvas.FrameRect(x, y, rw, rh, 3, widget.color);
    }
    if (widget.align == 1) {
        canvas.DrawTextAligned(x + rw / 2, y + (rh - widget.text_scale * 5) / 2, widget.text,
                               widget.text_scale, widget.color, 1);
    } else {
        canvas.DrawText(x + 12, y + 12, widget.text, widget.text_scale, widget.color);
    }
}

} // namespace

bool RenderPage(Canvas& canvas, const Manifest& manifest, const Page& page,
                const StateSnapshot& snapshot, const ImageProvider& images, const ViewState& views,
                MapFollowState* follow_state, const VisitReporter& report_visit,
                const VisitedQuery& is_visited, AuxDrawList* draw_list, bool* animating,
                const TextProvider& texts, MapDrawRecords* map_records, const RenderExtras* extras,
                std::mutex* follow_state_mutex, std::recursive_mutex* manifest_markers_mutex) {
    // A genuinely narrowed partial redraw (extras != nullptr with a real, non-whole-canvas clip --
    // the same test the per-widget bbox pre-reject below uses) must NOT blow away every Map
    // widget's tap-hit-testing record just because this pass didn't happen to touch the map. A
    // partial pass triggered by something with no spatial overlap with the map (a panel sliding in,
    // a HUD icon, anything outside the Map widget's own rect) `continue`s past the Map widget at
    // the bbox pre-reject a few hundred lines down, without ever reaching the `map_records->
    // emplace_back()` call that would re-add its entry. Clearing unconditionally would leave
    // `*map_records` (and, one publish later, `map_draw_records_published`) with ZERO records for a
    // map that is still fully visible and unchanged on screen, so the map could not be tapped. This
    // holds on both the redraw worker and EDEN_DSMOD_SYNC_REDRAW=1; it is not a threading matter
    // but the interaction between the bbox pre-reject and a clear-every-call policy. A full redraw
    // (extras == nullptr, or its clip is the {0,0,0,0} "whole canvas" sentinel -- Canvas::SetClip's
    // own contract, same test the bbox pre-reject uses) still clears and rebuilds from scratch
    // exactly as before: every widget is visited that pass, so nothing is lost by starting empty.
    // On a real partial pass, entries simply survive unless something below explicitly touches them
    // -- see the matching `erase_if` in the Map widget's BeginRecord, mod_ui_map_widget.cpp (a
    // widget that DOES redraw this
    // pass refreshes its own entry, never leaving a stale duplicate) and at the widget-hidden
    // `continue` just below (a Map widget that just became hidden -- reachable here on a partial
    // pass only when its own dependency hash changed, i.e. it IS inside this pass's dirty rect --
    // must not leave a now-invalid record a tap could still hit).
    const bool whole_canvas_draw =
        extras == nullptr || extras->clip[2] <= 0 || extras->clip[3] <= 0;
    if (map_records != nullptr && whole_canvas_draw) {
        map_records->clear();
    }
    if (canvas.Width() == 0 || canvas.Height() == 0) {
        return false;
    }
    // An animation frame redraws only its dirty box; everything else keeps last frame's pixels.
    if (extras != nullptr) {
        canvas.SetClip(extras->clip[0], extras->clip[1], extras->clip[2], extras->clip[3]);
    } else {
        canvas.ResetClip();
    }
    canvas.SetLayerOpacity(1.0f);
    const std::array<s32, 4> frame_clip = canvas.Clip();
    bool settling = false; // a follow-view glide is still in flight: caller must redraw again
    if (draw_list != nullptr) {
        // GPU composite: the canvas is only the HUD overlay (map/icons/marker are emitted as
        // quads). Clear it fully transparent each frame so the composited map shows through where
        // no HUD sits; the map widget emits its own bounded background fill before its textures.
        draw_list->bg = manifest.background;
        draw_list->canvas_w = canvas.Width();
        draw_list->canvas_h = canvas.Height();
        canvas.Clear(0x00000000u);
    } else {
        // Widgets repaint only their own pixels (text draws glyph strokes, not boxes), and a
        // widget that hides between redraws leaves its old pixels behind, so every redraw starts
        // from the background. A full-canvas fill is ~0.3 ms; the stale-pixel bugs it prevents
        // (old digits under new ones, a hidden picture peeking out around the panels) are not
        // worth saving it.
        canvas.Clear(manifest.background);
    }

    bool any_repeat = false;
    for (const auto& rw_ : page.widgets) {
        if (rw_.repeat > 0 || !rw_.x_bind.empty() || !rw_.y_bind.empty()) {
            any_repeat = true;
            break;
        }
    }
    // The expanded widgets live in this thread's reusable slots (copy-assigned each call, so their
    // strings keep their storage): expanding a large page no longer allocates per widget.
    static thread_local std::vector<Widget> expanded_storage;
    size_t expanded_count = 0;
    // A drag in flight appends its ghost -- a scaled copy of the dragged widget pinned under the
    // finger -- so it is drawn last, over everything, by the same code as the widget itself.
    const Widget* const ghost_source =
        snapshot.drag.active && snapshot.drag.widget.type != WidgetType::Map &&
                snapshot.drag.widget.rect[2] > 0 && snapshot.drag.widget.rect[3] > 0
            ? &snapshot.drag.widget
            : nullptr;
    size_t ghost_index = std::numeric_limits<size_t>::max();
    if (ghost_source != nullptr) {
        if (any_repeat) {
            expanded_count = ExpandWidgetsInto(page, snapshot, expanded_storage);
        } else {
            if (expanded_storage.size() < page.widgets.size()) {
                expanded_storage.resize(page.widgets.size());
            }
            std::copy(page.widgets.begin(), page.widgets.end(), expanded_storage.begin());
            expanded_count = page.widgets.size();
        }
        Widget ghost = *ghost_source;
        const float s = std::clamp(ghost.drag_scale, 0.25f, 4.0f);
        const auto scaled = [s](s32 v) {
            return static_cast<s32>(std::lround(static_cast<float>(v) * s));
        };
        ghost.rect = {snapshot.drag.x - scaled(snapshot.drag.grab_dx),
                      snapshot.drag.y - scaled(snapshot.drag.grab_dy), scaled(ghost.rect[2]),
                      scaled(ghost.rect[3])};
        ghost.text_scale = std::max(1, scaled(ghost.text_scale));
        ghost.hide_bind.clear();
        ghost.need_bind.clear();
        ghost.anim.reset();
        ghost.pan_zoom = false;
        ghost.repeat = 0;
        ghost_index = expanded_count;
        if (expanded_count < expanded_storage.size()) {
            expanded_storage[expanded_count] = std::move(ghost);
        } else {
            expanded_storage.push_back(std::move(ghost));
        }
        ++expanded_count;
    } else if (any_repeat) {
        expanded_count = ExpandWidgetsInto(page, snapshot, expanded_storage);
    }
    const std::span<const Widget> expanded =
        any_repeat || ghost_source != nullptr
            ? std::span<const Widget>{expanded_storage.data(), expanded_count}
            : std::span<const Widget>{page.widgets};

    // Selection / drop-hover highlights are painted after every widget (so an icon drawn over a
    // cell cannot hide its cell's frame) and before the ghost.
    struct HighlightAt {
        std::string src;
        u32 color;
        s32 frame;
        s32 x, y, w, h;
        std::array<s32, 4> clip; ///< the widget's clip (an animated group's box)
        float alpha;
    };
    std::vector<HighlightAt> highlights;
    const auto draw_highlights = [&] {
        for (const auto& hl : highlights) {
            canvas.SetClip(hl.clip[0], hl.clip[1], hl.clip[2], hl.clip[3]);
            canvas.SetLayerOpacity(hl.alpha);
            bool drawn = false;
            if (!hl.src.empty() && images) {
                if (const std::shared_ptr<const Image> img = images(hl.src); img != nullptr) {
                    canvas.DrawImage(hl.x, hl.y, hl.w, hl.h, *img, 0xFFFFFFFFu);
                    drawn = true;
                }
            }
            if (!drawn && hl.color != 0) {
                if (hl.frame <= 0) {
                    canvas.FillRect(hl.x, hl.y, hl.w, hl.h, hl.color);
                } else {
                    canvas.FrameRect(hl.x, hl.y, hl.w, hl.h, hl.frame, hl.color);
                }
            }
        }
        canvas.SetClip(frame_clip[0], frame_clip[1], frame_clip[2], frame_clip[3]);
        canvas.SetLayerOpacity(1.0f);
        highlights.clear();
    };

    // Scrollbars of the page's drag-to-scroll regions: over the rows, under highlights and the
    // drag ghost. Drawn only while the runtime says the region is live ("@scroll_on:<id>").
    bool scroll_bars_drawn = page.scrolls.empty();
    const auto draw_scroll_bars = [&] {
        if (scroll_bars_drawn) {
            return;
        }
        scroll_bars_drawn = true;
        canvas.SetClip(frame_clip[0], frame_clip[1], frame_clip[2], frame_clip[3]);
        canvas.SetLayerOpacity(1.0f);
        canvas.SetDrawOpacity(1.0f);
        for (const auto& region : page.scrolls) {
            if (region.bar_color == 0 || snapshot.GetInt("@scroll_on:" + region.id) == 0) {
                continue;
            }
            const ScrollMetrics m = MeasureScroll(page, region, snapshot);
            const auto thumb = ScrollBarThumb(region, m, ScrollOffset(region, snapshot));
            if (thumb[2] <= 0 || thumb[3] <= 0) {
                continue;
            }
            if (region.bar_track != 0) {
                canvas.FillRect(thumb[0], region.rect[1], thumb[2], region.rect[3],
                                region.bar_track);
            }
            canvas.FillRect(thumb[0], thumb[1], thumb[2], thumb[3], region.bar_color);
        }
    };

    // EDEN_DSMOD_PROFILE_WIDGETS: per-widget draw time, the slowest ones logged every 5 s.
    static const bool profile_widgets = std::getenv("EDEN_DSMOD_PROFILE_WIDGETS") != nullptr;
    struct WidgetCost {
        double ms{};
        u64 calls{};
        std::string what;
    };
    // widget_costs/profile_window are opt-in (EDEN_DSMOD_PROFILE_WIDGETS), and RenderPage runs on
    // two threads (the redraw worker and the tick thread's synchronous carve-out) concurrently --
    // unsynchronised, that would be an unordered_map mutation race (real corruption risk, not just
    // a stale read) plus a plain-scalar race on profile_window. One mutex for both: they are only
    // ever touched together (every read/write site below takes it for its whole critical section),
    // and this is debug instrumentation, not the hot path -- correctness over fine-grained cost
    // here.
    static std::unordered_map<std::string, WidgetCost> widget_costs;
    static auto profile_window = std::chrono::steady_clock::now();
    static std::mutex profile_mutex;
    // Occlusion skip: skip widgets fully repainted by a later, unconditionally-opaque Map widget.
    // Measured (EDEN_DSMOD_PROFILE_WIDGETS=1, real outdoor overworld walk): LA's map page's "bg"
    // (full-canvas Image) and "map_sheet" (paper Image) widgets -- both drawn BEFORE the Map widget
    // in z-order, both with no bind of their own -- cost ~0.85-1.0ms EACH, ~35% of the ~5.0-5.9ms
    // partial-redraw average, on every ordinary walking tick. They are redrawn only because their
    // static rects intersect the tick's dirty union (the dirty-region redraw's "everything
    // intersecting participates" correctness rule) -- which is almost exactly the Map widget's own
    // rect while walking -- even though the Map widget's draw (DrawMap, mod_ui_map_widget.cpp)
    // unconditionally does `canvas.FillRect(x, y, rw, rh, widget.bg)` as the very FIRST thing it
    // does, before anything else, regardless of area/follow/zoom state. When widget.bg is fully
    // opaque (alpha byte == 0xFF) that FillRect is a complete, unconditional overwrite of every
    // pixel in its declared rect -- Blend() (mod_ui_internal.h): `if (a == 0xFF) return src;`,
    // no dependency whatsoever on what was drawn there before. So any earlier (lower z-order)
    // widget whose own rect is entirely inside an eligible Map widget's rect can never be visible
    // this tick and does not need to draw.
    //
    // Eligibility (all checked below, static_occluders built once per RenderPage call): the Map
    // widget itself must not be hidden this tick (WidgetHidden(), the same pure predicate the
    // dirty-region scan already relies on); its widget.bg alpha must be exactly 0xFF;
    // manifest.map_style.opacity must be exactly 1.0 (else the Map's own draw is itself only
    // partially opaque -- see "translucent_map" below); and it must not
    // carry a widget.anim (a moving/fading group member), which is excluded outright so this never
    // has to reason about anim_clip/widget_alpha for the occluder side -- LA's map_overworld is not
    // one, so this excludes nothing in practice. A Map widget's own declared rect is never
    // x_bind/y_bind-repositioned (the map marker moves via marker_x_bind/marker_y_bind, consumed
    // entirely inside DrawMap, never the widget's own rect), so using widget.rect
    // verbatim here is correct without replicating ExpandWidgets.
    //
    // Scope: draw_list != nullptr (EDEN_DSMOD_GPU_COMPOSITE) is excluded entirely -- LA's shipped
    // package never sets that flag and the Map widget's draw-list branch behaves differently there
    // (queues quads instead of a canvas fill); this change does not enter that, unverified,
    // territory. Applies to both a clipped (partial) and an unclipped (full) RenderPage call alike
    // -- frame_clip already equals the whole canvas on a full draw (Canvas::SetClip's own contract,
    // matching the existing bbox pre-reject below's use of the same property) -- so no
    // extras/partial-specific branch is needed; the correctness argument above does not depend on
    // why RenderPage is being called.
    std::vector<std::pair<size_t, std::array<s32, 4>>> static_occluders;
    if (draw_list == nullptr) {
        for (size_t oi = 0; oi < expanded.size(); ++oi) {
            const Widget& ow = expanded[oi];
            if (ow.type != WidgetType::Map || ow.anim || manifest.map_style.opacity < 1.0f ||
                ((ow.bg >> 24) & 0xFFu) != 0xFFu) {
                continue;
            }
            const s32 ox0 = std::max(frame_clip[0], ow.rect[0]);
            const s32 oy0 = std::max(frame_clip[1], ow.rect[1]);
            const s32 ox1 = std::min(frame_clip[0] + frame_clip[2], ow.rect[0] + ow.rect[2]);
            const s32 oy1 = std::min(frame_clip[1] + frame_clip[3], ow.rect[1] + ow.rect[3]);
            if (ox1 <= ox0 || oy1 <= oy0 || WidgetHidden(ow, snapshot)) {
                continue; // not actually painting anything (this tick) that could occlude
            }
            static_occluders.emplace_back(oi, std::array<s32, 4>{ox0, oy0, ox1 - ox0, oy1 - oy0});
        }
    }
    Widget anim_moved; ///< a widget of a sliding group, at this frame's offset
    for (size_t widget_index = 0; widget_index < expanded.size(); ++widget_index) {
        const Widget* widget_ptr = &expanded[widget_index];
        const RenderAnimGroup* anim_group = nullptr;
        if (extras != nullptr && widget_ptr->anim) {
            anim_group = extras->Find(widget_ptr->anim->key);
            if (anim_group != nullptr && anim_group->moving &&
                (anim_group->dx != 0 || anim_group->dy != 0)) {
                anim_moved = *widget_ptr;
                anim_moved.rect[0] += anim_group->dx;
                anim_moved.rect[1] += anim_group->dy;
                widget_ptr = &anim_moved;
            }
        }
        const auto& widget = *widget_ptr;
        const bool is_ghost = widget_index == ghost_index;
        if (is_ghost) {
            draw_scroll_bars();
            draw_highlights();
            if (!widget.drag_under_src.empty() && images) {
                // The underlay (a cell's disc) travels with the ghost: placed relative to the
                // widget's own rect, scaled about the grab point like the ghost itself.
                if (const std::shared_ptr<const Image> under = images(widget.drag_under_src);
                    under != nullptr) {
                    const float s = std::clamp(ghost_source->drag_scale, 0.25f, 4.0f);
                    const auto scaled = [s](s32 v) {
                        return static_cast<s32>(std::lround(static_cast<float>(v) * s));
                    };
                    const auto& ur = widget.drag_under_rect;
                    const s32 uw = ur[2] > 0 ? ur[2] : ghost_source->rect[2];
                    const s32 uh = ur[3] > 0 ? ur[3] : ghost_source->rect[3];
                    const auto& sr = widget.drag_under_src_rect;
                    const s32 sx = static_cast<s32>(sr[0] * static_cast<float>(under->w));
                    const s32 sy = static_cast<s32>(sr[1] * static_cast<float>(under->h));
                    const s32 sw = std::max(
                        1, static_cast<s32>((sr[2] - sr[0]) * static_cast<float>(under->w)));
                    const s32 sh = std::max(
                        1, static_cast<s32>((sr[3] - sr[1]) * static_cast<float>(under->h)));
                    canvas.SetDrawOpacity(0.8f);
                    if (widget.drag_under_color != 0) {
                        canvas.DrawImageMask(widget.rect[0] + scaled(ur[0]),
                                             widget.rect[1] + scaled(ur[1]), scaled(uw), scaled(uh),
                                             *under, sx, sy, sw, sh, widget.drag_under_color);
                    } else {
                        canvas.DrawImageRegion(
                            widget.rect[0] + scaled(ur[0]), widget.rect[1] + scaled(ur[1]),
                            scaled(uw), scaled(uh), *under, sx, sy, sw, sh, widget.drag_under_tint);
                    }
                    canvas.SetDrawOpacity(1.0f);
                }
            }
        }
        // Where this widget is currently looking, if the user has dragged or pinched it.
        ViewTransform view{};
        if (widget.pan_zoom) {
            const std::string key =
                widget.id.empty() ? page.id + "#" + std::to_string(widget_index) : widget.id;
            if (const auto found = views.find(key); found != views.end()) {
                view = found->second;
            }
        }
        s32 x = widget.rect[0];
        s32 y = widget.rect[1];
        s32 rw = widget.rect[2];
        s32 rh = widget.rect[3];
        if (rw == 0 && rh == 0 &&
            (widget.type == WidgetType::Rect || widget.type == WidgetType::Image)) {
            // An unsized rect spans the canvas (a background); an unsized image sits centred
            // as a square a third of the shorter side (the idle page's title icon). Text
            // widgets are unsized by design and keep their anchor.
            const s32 cw = static_cast<s32>(canvas.Width()), ch = static_cast<s32>(canvas.Height());
            if (widget.type == WidgetType::Image) {
                const s32 side = std::max(64, std::min(cw, ch) / 3);
                x = (cw - side) / 2;
                y = (ch - side) / 2;
                rw = rh = side;
            } else {
                x = 0;
                y = 0;
                rw = cw;
                rh = ch;
            }
        }
        // Dirty-region redraw: on a clipped (partial) frame, a widget whose resolved rect doesn't
        // even touch the clip pays nothing for its pixel writes already
        // (Canvas::FillRect/DrawImage/etc. clamp their own loops to the clip) -- but it still paid
        // a WidgetHidden() gate check plus a full switch-dispatch, for every one of a page's
        // widgets, every clipped frame, before this. Skipped here using the SAME x/y/rw/rh already
        // resolved just above (including the unsized-widget expansion right above this line --
        // using the raw, pre-expansion widget.rect here would wrongly reject an unsized full-canvas
        // background, since its own rect is {0,0,0,0}). Conservatively skipped for a widget
        // belonging to a currently-moving anim group (anim_group != nullptr): that widget's
        // effective draw rect is the group's own (already unioned-in) box offset by dx/dy, not its
        // own resolved rect, and it is few enough in practice that leaving it on the existing,
        // already-proven anim_clip path untouched is not worth the risk of this rewriting that
        // interaction (the reject would inherit the anim_clip path's own risk under a different
        // trigger). extras->clip with w or h <= 0 means "whole canvas" (the Canvas::SetClip
        // contract), so nothing is ever wrongly rejected on a full-page draw.
        if (extras != nullptr && anim_group == nullptr && extras->clip[2] > 0 &&
            extras->clip[3] > 0) {
            const s32 cx0 = extras->clip[0], cy0 = extras->clip[1];
            const s32 cx1 = cx0 + extras->clip[2], cy1 = cy0 + extras->clip[3];
            // Unsized text is anchored at rect x and grows right (left align), both ways (centre)
            // or LEFT (right align). Reject against the same conservative extent the dirty-region
            // scan uses (WidgetEffectiveRect, mod_redraw.cpp); testing the zero-width anchor
            // alone skipped a right-aligned number whenever the dirty rect ended left of its
            // anchor, after the background under its leading digits had been repainted.
            s32 bx = x, bw = rw, by = y, bh = rh;
            if (rw == 0 && (widget.type == WidgetType::Label || widget.type == WidgetType::Value)) {
                const s32 scale = std::max<s32>(1, widget.text_scale);
                const s32 span = scale * 5 * 16;
                bx = widget.align == 2 ? x - span : widget.align == 1 ? x - span / 2 : x;
                bw = span;
                by = y - scale;
                bh = (rh > 0 ? rh : scale * 9) + 2 * scale;
            }
            if (const s32 pad = WidgetDrawOverhang(widget, rw, rh); pad > 0) {
                bx -= pad;
                by -= pad;
                bw += 2 * pad;
                bh += 2 * pad;
            }
            if (const auto t = WidgetPaintBounds(
                    widget, x, y, rw, rh, snapshot, manifest, canvas.ActiveFont(),
                    static_cast<s32>(canvas.Width()), static_cast<s32>(canvas.Height()));
                t[2] > 0 && t[3] > 0) {
                // Long text, a tall font, a run of pips paint outside the fixed box: reject on the
                // real bound.
                const s32 ux0 = std::min(bx, t[0]), uy0 = std::min(by, t[1]);
                const s32 ux1 = std::max(bx + bw, t[0] + t[2]),
                          uy1 = std::max(by + bh, t[1] + t[3]);
                bx = ux0;
                by = uy0;
                bw = ux1 - ux0;
                bh = uy1 - uy0;
            }
            if (bx + bw <= cx0 || bx >= cx1 || by + bh <= cy0 || by >= cy1) {
                continue; // entirely outside this frame's dirty rect
            }
        }
        // Occlusion skip -- see static_occluders' own build-site comment above for the full safety
        // argument. widget_index >= occ_index is excluded (only a strictly LATER, higher-z-order
        // occluder can repaint over this widget; one drawn before or at the same index cannot).
        // x/y/rw/rh here already reflect anim_moved's shifted rect when anim_group != nullptr
        // (widget_ptr was reassigned above), so this is correct for a moving group member too -- it
        // is simply the widget actually being drawn this tick, wherever that is. Frame_clip is
        // intersected in, exactly like the bbox pre-reject just above, so a widget only partly
        // inside the occluder (never fully covered) is correctly NOT skipped.
        if (!static_occluders.empty()) {
            const s32 wx0 = std::max(frame_clip[0], x);
            const s32 wy0 = std::max(frame_clip[1], y);
            const s32 wx1 = std::min(frame_clip[0] + frame_clip[2], x + rw);
            const s32 wy1 = std::min(frame_clip[1] + frame_clip[3], y + rh);
            bool occluded = false;
            for (const auto& [occ_index, occ_rect] : static_occluders) {
                if (widget_index >= occ_index) {
                    continue;
                }
                if (wx0 >= occ_rect[0] && wy0 >= occ_rect[1] && wx1 <= occ_rect[0] + occ_rect[2] &&
                    wy1 <= occ_rect[1] + occ_rect[3]) {
                    occluded = true;
                    break;
                }
            }
            if (occluded) {
                continue; // fully repainted by a later, opaque Map widget this tick
            }
        }
        const s64 value = widget.bind.empty() ? 0 : snapshot.GetInt(widget.bind);
        const bool hidden = anim_group != nullptr
                                ? !anim_group->shown ||
                                      WidgetHiddenHolding(widget, snapshot, widget.anim->gate.point)
                                : WidgetHidden(widget, snapshot);
        if (hidden) {
            // A Map widget reaches this `continue` on a partial pass only when its OWN dependency
            // hash changed (otherwise the bbox pre-reject above already skipped it without ever
            // getting this far) -- i.e. it just became hidden this very tick. Its old `map_records`
            // entry (surviving the conditional clear at the top of RenderPage) is now stale and
            // must not stay tap-hittable. Cheap: `map_records` holds at most a handful of entries
            // per page even in the worst case, and this is a `size_t` equality scan, not a redraw.
            if (widget.type == WidgetType::Map && map_records != nullptr) {
                std::erase_if(*map_records, [&](const MapDrawRecord& r) {
                    return r.widget_index == widget_index;
                });
            }
            continue; // empty slot / out of this panel's category
        }
        // A moving group draws inside its box only, at its own opacity.
        std::array<s32, 4> widget_clip = frame_clip;
        float widget_alpha = 1.0f;
        const bool anim_clip = anim_group != nullptr && anim_group->moving;
        if (anim_clip) {
            const auto& b = anim_group->box;
            const s32 cx0 = std::max(frame_clip[0], b[0]);
            const s32 cy0 = std::max(frame_clip[1], b[1]);
            const s32 cx1 = std::min(frame_clip[0] + frame_clip[2], b[0] + b[2]);
            const s32 cy1 = std::min(frame_clip[1] + frame_clip[3], b[1] + b[3]);
            if (cx1 <= cx0 || cy1 <= cy0 || anim_group->alpha <= 0.0f) {
                continue; // entirely outside this frame's dirty area, or fully faded
            }
            widget_clip = {cx0, cy0, cx1 - cx0, cy1 - cy0};
            widget_alpha = anim_group->alpha;
        }
        // A scrolled list row draws only inside its region's viewport.
        const bool scroll_clip = widget.scroll_clip[2] > 0 && widget.scroll_clip[3] > 0;
        if (scroll_clip) {
            const auto& b = widget.scroll_clip;
            const s32 cx0 = std::max(widget_clip[0], b[0]);
            const s32 cy0 = std::max(widget_clip[1], b[1]);
            const s32 cx1 = std::min(widget_clip[0] + widget_clip[2], b[0] + b[2]);
            const s32 cy1 = std::min(widget_clip[1] + widget_clip[3], b[1] + b[3]);
            if (cx1 <= cx0 || cy1 <= cy0) {
                continue; // scrolled out of view, or outside this frame's dirty area
            }
            widget_clip = {cx0, cy0, cx1 - cx0, cy1 - cy0};
        }
        const bool own_clip = anim_clip || scroll_clip;
        if (!is_ghost && (widget.highlight_color != 0 || !widget.highlight_src.empty())) {
            bool lit = snapshot.drag.active && snapshot.drag.hover >= 0 &&
                       static_cast<size_t>(snapshot.drag.hover) == widget_index;
            if (!lit && !widget.select_group.empty()) {
                const auto sel = snapshot.ints.find("@sel:" + widget.select_group);
                if (sel != snapshot.ints.end() && sel->second != -1) {
                    const auto carried = WidgetPayload(widget, snapshot);
                    lit = carried.has_value() && *carried == sel->second;
                }
            }
            if (lit) {
                highlights.push_back({widget.highlight_src, widget.highlight_color, widget.frame, x,
                                      y, rw, rh, widget_clip, widget_alpha});
            }
        }
        const s64 maximum = widget.max_bind.empty()
                                ? widget.max_const
                                : snapshot.GetInt(widget.max_bind, widget.max_const);
        // Establish a clean state for every widget as well as resetting below after a map. This
        // keeps a future early-exit path from leaking map opacity into later HUD widgets.
        canvas.SetDrawOpacity(is_ghost ? 0.8f : 1.0f);
        const bool translucent_map =
            widget.type == WidgetType::Map && manifest.map_style.opacity < 1.0f;
        const size_t map_quad_begin = draw_list != nullptr ? draw_list->quads.size() : 0;
        if (translucent_map) {
            canvas.SetDrawOpacity(manifest.map_style.opacity);
        }
        const auto widget_started = profile_widgets ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
        if (own_clip) {
            canvas.SetClip(widget_clip[0], widget_clip[1], widget_clip[2], widget_clip[3]);
            canvas.SetLayerOpacity(widget_alpha);
        }
        const WidgetDrawContext ctx{.canvas = canvas,
                                    .manifest = manifest,
                                    .page = page,
                                    .snapshot = snapshot,
                                    .images = images,
                                    .texts = texts,
                                    .draw_list = draw_list,
                                    .follow_state = follow_state,
                                    .report_visit = report_visit,
                                    .is_visited = is_visited,
                                    .map_records = map_records,
                                    .follow_state_mutex = follow_state_mutex,
                                    .manifest_markers_mutex = manifest_markers_mutex,
                                    .settling = settling,
                                    .widget = widget,
                                    .widget_index = widget_index,
                                    .view = view,
                                    .x = x,
                                    .y = y,
                                    .rw = rw,
                                    .rh = rh,
                                    .value = value,
                                    .maximum = maximum};
        switch (widget.type) {
        case WidgetType::Image:
            DrawImage(ctx);
            break;
        case WidgetType::Map:
            DrawMap(ctx);
            break;
        case WidgetType::Rect:
            DrawRect(ctx);
            break;
        case WidgetType::Label:
            DrawLabel(ctx);
            break;
        case WidgetType::Value:
            DrawValue(ctx);
            break;
        case WidgetType::Bar:
            DrawBar(ctx);
            break;
        case WidgetType::Pips:
            DrawPips(ctx);
            break;
        case WidgetType::Button:
            DrawButton(ctx);
            break;
        }
        if (own_clip) {
            canvas.SetClip(frame_clip[0], frame_clip[1], frame_clip[2], frame_clip[3]);
            canvas.SetLayerOpacity(1.0f);
        }
        if (translucent_map) {
            canvas.SetDrawOpacity(1.0f);
            if (draw_list != nullptr) {
                const u32 amount = static_cast<u32>(manifest.map_style.opacity * 255.0f + 0.5f);
                for (size_t i = map_quad_begin; i < draw_list->quads.size(); ++i) {
                    auto& quad = draw_list->quads[i];
                    if (quad.solid) {
                        // Solid rectangles use a bounded attachment clear, so resolve their
                        // opacity over the compositor background before submission. The GPU path
                        // always draws the HUD canvas last, so it does not model a non-map widget
                        // underneath this background; packages should use the page background for
                        // a translucent map when widgets overlap.
                        u32 mixed = 0;
                        for (u32 shift = 0; shift < 32; shift += 8) {
                            const u32 below = (draw_list->bg >> shift) & 0xFF;
                            const u32 above = (quad.color >> shift) & 0xFF;
                            mixed |= ((below * (255 - amount) + above * amount) / 255) << shift;
                        }
                        quad.color = mixed;
                    } else {
                        quad.a *= manifest.map_style.opacity;
                    }
                }
            }
        }
        if (profile_widgets) {
            std::scoped_lock plk{profile_mutex};
            auto& cost = widget_costs[page.id + "#" + std::to_string(widget_index)];
            cost.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 widget_started)
                           .count();
            ++cost.calls;
            if (cost.what.empty()) {
                const std::string& src = widget.src;
                cost.what = fmt::format("type {} id '{}' rect {}x{} src '{}'",
                                        static_cast<int>(widget.type), widget.id, rw, rh,
                                        src.substr(src.find_last_of('#') == std::string::npos
                                                       ? 0
                                                       : src.find_last_of('#') + 1));
            }
        }
    }
    if (profile_widgets) {
        // Snapshot-and-clear under the lock, sort/log outside it -- keeps the critical section
        // short without letting two threads race the "is it time yet" check or the clear/reset.
        std::vector<std::pair<std::string, WidgetCost>> sorted;
        bool should_log = false;
        {
            std::scoped_lock plk{profile_mutex};
            if (std::chrono::steady_clock::now() - profile_window >= std::chrono::seconds{5}) {
                should_log = true;
                sorted.assign(widget_costs.begin(), widget_costs.end());
                widget_costs.clear();
                profile_window = std::chrono::steady_clock::now();
            }
        }
        if (should_log) {
            std::ranges::sort(
                sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
            double total = 0;
            for (const auto& [k, c] : sorted) {
                total += c.ms;
            }
            LOG_INFO(Core, "DSMod widget cost: {} widget(s), {:.1f} ms total in 5 s", sorted.size(),
                     total);
            for (size_t i = 0; i < sorted.size() && i < 14; ++i) {
                const auto& [k, c] = sorted[i];
                LOG_INFO(Core, "DSMod widget cost: {} {:.3f} ms/draw x{} ({})", k, c.ms / c.calls,
                         c.calls, c.what);
            }
        }
    }
    draw_scroll_bars(); // no ghost this frame: bars and highlights still need painting
    draw_highlights();
    canvas.SetDrawOpacity(1.0f);
    canvas.SetLayerOpacity(1.0f);
    canvas.ResetClip();
    if (animating != nullptr) {
        *animating = settling;
    }
    return true;
}

void RenderDebugPage(Canvas& canvas, const Manifest& manifest, const StateSnapshot& snapshot) {
    canvas.Clear(manifest.background);
    const u32 head = 0xFFF2E7A8u; // colours here are already in upload byte order
    const u32 ok = 0xFF87C29Cu;
    const u32 bad = 0xFF7070EFu;
    const u32 dim = 0xFFA6988Au;

    canvas.DrawText(32, 28, "DSMOD OFFSET DEBUG", 6, head);
    char line[192];
    std::snprintf(line, sizeof(line), "%s   DATA %s", manifest.name.c_str(),
                  manifest.build_id_file.empty() ? "none" : manifest.build_id_file.c_str());
    canvas.DrawText(32, 82, line, 3, dim);

    // Stable order so the list does not jump around between frames.
    std::vector<std::string> names;
    names.reserve(manifest.points.size());
    for (const auto& [name, point] : manifest.points) {
        names.push_back(name);
    }
    std::ranges::sort(names);

    s32 y = 130;
    const s32 row = 34;
    for (const auto& name : names) {
        const auto addr_it = snapshot.addresses.find(name);
        const auto val_it = snapshot.ints.find(name);
        const bool resolved = addr_it != snapshot.addresses.end() && addr_it->second != 0;
        const bool readable = val_it != snapshot.ints.end();
        if (readable) {
            std::snprintf(line, sizeof(line), "%-14s %010llX  %lld", name.c_str(),
                          static_cast<unsigned long long>(resolved ? addr_it->second : 0),
                          static_cast<long long>(val_it->second));
        } else {
            std::snprintf(line, sizeof(line), "%-14s %010llX  FAIL", name.c_str(),
                          static_cast<unsigned long long>(resolved ? addr_it->second : 0));
        }
        canvas.DrawText(32, y, line, 3, readable ? ok : bad);
        y += row;
        if (y > static_cast<s32>(canvas.Height()) - row) {
            break;
        }
    }
}

} // namespace Core::Mods
