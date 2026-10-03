// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared state between the aux ("second screen") presentation path in the renderer,
// the dsm:u HLE service, and the frontend that owns the aux window / touch panel.
// Written from one thread and read from another, hence atomics + one mutex.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/dsmod_dev_tools.h"
#include "common/logging.h"

namespace VideoCore::DSMod {

// Mirrors DsmTouchPoint in the guest-side dsm_ipc.h contract (24 bytes).
struct AuxTouchPoint {
    u32 finger_id;
    u32 x; // pixels, aux surface space
    u32 y;
    u32 attributes; // bit0 = start, bit1 = end
    u64 delta_ns;
};
static_assert(sizeof(AuxTouchPoint) == 24);

// Mirrors DsmAuxDisplayInfo in dsm_ipc.h (24 bytes).
struct AuxDisplayInfo {
    u32 present;
    u32 width;
    u32 height;
    u32 rotation;
    u32 refresh_hz;
    u32 reserved;
};
static_assert(sizeof(AuxDisplayInfo) == 24);

/// Dirty-tile bookkeeping for one published image. The producer
/// diffs each publish against the copy it already holds and marks only the 64x64 tiles whose
/// pixels actually changed; the renderer uploads just those tiles (one VkBufferImageCopy region per
/// merged run). A publish whose pixels are identical to the held copy does not bump the serial at
/// all, so the renderer neither uploads nor re-presents. `all` = the consumer must replace the
/// whole image (first publish, size change, in-place fills that cannot be diffed).
struct TileMask {
    static constexpr u32 Size = 64;
    u32 cols{0};
    u32 rows{0};
    bool all{true};
    std::vector<u8> bits;

    void Reset(u32 w, u32 h) {
        cols = (w + Size - 1) / Size;
        rows = (h + Size - 1) / Size;
        bits.assign(static_cast<size_t>(cols) * rows, 0);
        all = true;
    }
    void Clear() {
        std::fill(bits.begin(), bits.end(), u8{0});
        all = false;
    }
    /// Marks every tile touched by {x, y, w, h} (already clamped to the image).
    void MarkRect(s32 x, s32 y, s32 w, s32 h) {
        if (w <= 0 || h <= 0 || cols == 0) {
            return;
        }
        const u32 c0 = static_cast<u32>(x) / Size;
        const u32 c1 = std::min(cols - 1, static_cast<u32>(x + w - 1) / Size);
        const u32 r0 = static_cast<u32>(y) / Size;
        const u32 r1 = std::min(rows - 1, static_cast<u32>(y + h - 1) / Size);
        for (u32 r = r0; r <= r1; ++r) {
            std::fill_n(bits.begin() + static_cast<size_t>(r) * cols + c0, c1 - c0 + 1, u8{1});
        }
    }
    [[nodiscard]] bool Any() const {
        return all || std::any_of(bits.begin(), bits.end(), [](u8 b) { return b != 0; });
    }
};

/// The one reading of an EDEN_DSMOD_* on/off switch: unset = `fallback`; set = on unless it is
/// "0", "false" or "FALSE". Callers keep the answer in a function-local static (read once).
inline bool DsmodEnvFlag(const char* name, bool fallback) {
    const char* const value = Common::DSMod::DevEnvironment(name);
    if (value == nullptr) {
        return fallback;
    }
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0;
}

/// EDEN_DSMOD_UI_DIFF=0 turns tile diffing off (copy the whole publish/dirty rect and upload its
/// bounding box). Read once.
inline bool UiDiffEnabled() {
    static const bool enabled = DsmodEnvFlag("EDEN_DSMOD_UI_DIFF", true);
    return enabled;
}

/// How a companion page whose shape differs from the second screen's is shown.
enum class CompanionRatio : u32 {
    Fit = 0,     ///< keep the page's shape, centred, the rest in the page background
    Stretch = 1, ///< fill the whole screen
};

/// What the second screen shows for a game without a usable companion package.
enum class NoCompanion : u32 {
    Icon = 0,  ///< the game's icon, dimmed on black (the idle page)
    Black = 1, ///< the idle page without its icon
    Off = 2,   ///< the frontend does not show its second-screen window at all
};

/// The user's second-screen options. Process-wide rather than per AuxRouting: the frontend sets
/// them whenever its settings apply (on Android also before a game boots, so the idle page is
/// built with them), and both the renderer and the mod runtime read them. A frontend that never
/// sets them gets Fit and Icon.
struct SecondScreenOptions {
    static inline std::atomic<u32> companion_ratio{static_cast<u32>(CompanionRatio::Fit)};
    static inline std::atomic<u32> no_companion{static_cast<u32>(NoCompanion::Icon)};

    static CompanionRatio Ratio() {
        return companion_ratio.load(std::memory_order_relaxed) ==
                       static_cast<u32>(CompanionRatio::Stretch)
                   ? CompanionRatio::Stretch
                   : CompanionRatio::Fit;
    }
    static NoCompanion WithoutCompanion() {
        const u32 v = no_companion.load(std::memory_order_relaxed);
        return v <= static_cast<u32>(NoCompanion::Off) ? static_cast<NoCompanion>(v)
                                                       : NoCompanion::Icon;
    }
};

/// Where a companion canvas sits inside the second screen's frame, in frame pixels.
struct CompanionRect {
    u32 x{0};
    u32 y{0};
    u32 w{0};
    u32 h{0};
    bool full{true}; ///< the whole frame: drawn and touched exactly as with Stretch

    bool Contains(u32 px, u32 py) const {
        return px >= x && py >= y && px - x < w && py - y < h;
    }
};

/// The rect a cw x ch canvas takes in a fw x fh frame. Fit keeps the canvas aspect, centred;
/// when the two shapes already agree (or with Stretch, or nothing known) it is the full frame.
inline CompanionRect FitCompanion(u32 fw, u32 fh, u32 cw, u32 ch, CompanionRatio ratio) {
    const CompanionRect whole{0, 0, fw, fh, true};
    if (ratio == CompanionRatio::Stretch || fw == 0 || fh == 0 || cw == 0 || ch == 0) {
        return whole;
    }
    const u64 frame_cross = static_cast<u64>(fw) * ch;
    const u64 canvas_cross = static_cast<u64>(fh) * cw;
    if (frame_cross == canvas_cross) {
        return whole;
    }
    CompanionRect rect{0, 0, fw, fh, false};
    if (frame_cross > canvas_cross) {
        // Frame is wider than the canvas: bars left and right.
        rect.w = static_cast<u32>(std::clamp<u64>((canvas_cross + ch / 2) / ch, 1, fw));
        rect.x = (fw - rect.w) / 2;
    } else {
        // Frame is taller than the canvas: bars above and below.
        rect.h = static_cast<u32>(std::clamp<u64>((frame_cross + cw / 2) / cw, 1, fh));
        rect.y = (fh - rect.h) / 2;
    }
    // Shapes within 2 px of each other (a 1240x1079 surface for a 1240x1080 canvas): draw and
    // touch the whole frame rather than leave a 1-px bar and shift every touch by it.
    if (fw - rect.w <= 2 && fh - rect.h <= 2) {
        return whole;
    }
    return rect;
}

/// The rect a companion's touches map through. A mirror page (the game's own pixels) is always
/// drawn over the whole panel, whatever Companion Ratio says.
inline CompanionRect CompanionTouchRect(u32 fw, u32 fh, u32 cw, u32 ch, bool mirror) {
    return FitCompanion(fw, fh, cw, ch,
                        mirror ? CompanionRatio::Stretch : SecondScreenOptions::Ratio());
}

/// One axis of a second-screen touch, panel pixels -> canvas pixels, for a point inside the
/// rect (origin/extent = CompanionRect x/w or y/h). On a full rect this is v * canvas / panel.
inline s32 PanelToCanvas(u32 v, u32 origin, u32 extent, u32 canvas) {
    return static_cast<s32>(static_cast<u64>(v - origin) * canvas / std::max(1u, extent));
}

/// Marks in `mask` (already Reset to w x h and Cleared) every tile where `a` and `b` (both w x h,
/// tightly packed) differ. Reads both images once; a row equal as a whole costs one memcmp, and
/// a tile already marked is not compared again.
inline void DiffTiles(std::span<const u32> a, std::span<const u32> b, u32 w, u32 h,
                      TileMask& mask) {
    constexpr u32 T = TileMask::Size;
    for (u32 y = 0; y < h; ++y) {
        const u32* const ra = a.data() + static_cast<size_t>(y) * w;
        const u32* const rb = b.data() + static_cast<size_t>(y) * w;
        if (std::memcmp(ra, rb, static_cast<size_t>(w) * sizeof(u32)) == 0) {
            continue;
        }
        u8* const tile_row = mask.bits.data() + static_cast<size_t>(y / T) * mask.cols;
        for (u32 tx = 0; tx < w; tx += T) {
            if (tile_row[tx / T] != 0) {
                continue;
            }
            const size_t n = static_cast<size_t>(std::min(T, w - tx)) * sizeof(u32);
            if (std::memcmp(ra + tx, rb + tx, n) != 0) {
                tile_row[tx / T] = 1;
            }
        }
    }
}

/// Copies the `rect` part of `src` over `dst` (both `w` pixels wide, tightly packed), writing and
/// marking only the tile-row segments whose pixels differ. Returns the bounding box of what
/// changed ({0,0,0,0} when nothing did). A row that is identical as a whole costs one memcmp.
inline std::array<s32, 4> DiffCopyRect(std::span<const u32> src, std::span<u32> dst, u32 w,
                                       std::array<s32, 4> rect, TileMask& mask) {
    const auto [x0, y0, cw, ch] = rect;
    if (cw <= 0 || ch <= 0) {
        return {0, 0, 0, 0};
    }
    s32 bx0 = x0 + cw, by0 = y0 + ch, bx1 = x0, by1 = y0;
    const u32 xend = static_cast<u32>(x0 + cw);
    for (s32 y = y0; y < y0 + ch; ++y) {
        const u32* const s = src.data() + static_cast<size_t>(y) * w;
        u32* const d = dst.data() + static_cast<size_t>(y) * w;
        if (std::memcmp(s + x0, d + x0, static_cast<size_t>(cw) * sizeof(u32)) == 0) {
            continue;
        }
        u8* const tile_row = mask.bits.data() + static_cast<size_t>(y / TileMask::Size) * mask.cols;
        for (u32 tx = static_cast<u32>(x0) / TileMask::Size * TileMask::Size; tx < xend;
             tx += TileMask::Size) {
            const u32 sx = std::max(tx, static_cast<u32>(x0));
            const u32 ex = std::min(tx + TileMask::Size, xend);
            const size_t n = static_cast<size_t>(ex - sx) * sizeof(u32);
            if (std::memcmp(s + sx, d + sx, n) != 0) {
                // Exact horizontal extent of the change inside this <=64 px segment (the bbox lets
                // the consumer upload a lone small widget's own box, not whole tiles).
                u32 first = sx;
                while (s[first] == d[first]) {
                    ++first;
                }
                u32 last = ex - 1;
                while (s[last] == d[last]) {
                    --last;
                }
                std::memcpy(d + sx, s + sx, n);
                tile_row[tx / TileMask::Size] = 1;
                bx0 = std::min(bx0, static_cast<s32>(first));
                bx1 = std::max(bx1, static_cast<s32>(last + 1));
            }
        }
        by0 = std::min(by0, y);
        by1 = std::max(by1, y + 1);
    }
    if (bx1 <= bx0 || by1 <= by0) {
        return {0, 0, 0, 0};
    }
    return {bx0, by0, bx1 - bx0, by1 - by0};
}

class AuxRouting {
public:
    static constexpr size_t MaxTouch = 16;
    static constexpr u64 NoLayer = 0; // VI layer ids start at 1

    // --- written by the renderer (GPU thread) when the aux window attaches/resizes ---
    std::atomic<bool> present{false};
    std::atomic<u32> width{0};
    std::atomic<u32> height{0};
    std::atomic<u32> rotation{0};
    std::atomic<u32> refresh_hz{60};

    // --- written by dsm:u (guest service thread), read by the renderer ---
    std::atomic<u64> bound_layer{NoLayer};

    // --- mirror: show a crop of the guest's own frame on the aux screen ---
    // Normalised to the source layer, so it survives resolution changes. This is how a mod shows
    // the game's real, animated art (a HUD corner, a minimap) without owning any assets.
    // A render target is being captured straight off the GPU and published as the UI image.
    // The mod runtime must stand down while this is on, or its 60 Hz canvas simply overwrites
    // the capture and the screen shows widgets instead of the intercepted pass.
    std::atomic<bool> rt_capture{false};

    /// The companion page's background (canvas pixel order), for the bars around a Fit canvas.
    std::atomic<u32> ui_bg{0xFF000000u};

    std::atomic<bool> mirror_enabled{false};

    /// Eden Duo: where the companion canvas sits on the panel, published by the mod runtime each
    /// tick (w == 0: not known yet, the whole panel). Lets a frontend's scripted 0..1 touches
    /// land on the canvas as drawn (Companion Ratio "Fit").
    std::atomic<u32> canvas_rect_x{0};
    std::atomic<u32> canvas_rect_y{0};
    std::atomic<u32> canvas_rect_w{0};
    std::atomic<u32> canvas_rect_h{0};

    void SetCanvasRect(const CompanionRect& r) {
        canvas_rect_x.store(r.x, std::memory_order_relaxed);
        canvas_rect_y.store(r.y, std::memory_order_relaxed);
        canvas_rect_w.store(r.w, std::memory_order_relaxed);
        canvas_rect_h.store(r.h, std::memory_order_relaxed);
    }

    /// A point normalised 0..1 to the companion canvas, in panel pixels.
    [[nodiscard]] std::pair<u32, u32> NormalisedToPanel(float fx, float fy) const {
        u32 x0 = canvas_rect_x.load(std::memory_order_relaxed);
        u32 y0 = canvas_rect_y.load(std::memory_order_relaxed);
        u32 w = canvas_rect_w.load(std::memory_order_relaxed);
        u32 h = canvas_rect_h.load(std::memory_order_relaxed);
        if (w == 0 || h == 0) {
            x0 = y0 = 0;
            w = width.load();
            h = height.load();
        }
        return {x0 + static_cast<u32>(fx * static_cast<float>(w)),
                y0 + static_cast<u32>(fy * static_cast<float>(h))};
    }
    std::atomic<float> mirror_x{0.0f};
    std::atomic<float> mirror_y{0.0f};
    std::atomic<float> mirror_w{1.0f};
    std::atomic<float> mirror_h{1.0f};

    AuxDisplayInfo GetDisplayInfo() const {
        return AuxDisplayInfo{
            .present = present.load() ? 1u : 0u,
            .width = width.load(),
            .height = height.load(),
            .rotation = rotation.load(),
            .refresh_hz = refresh_hz.load(),
            .reserved = 0,
        };
    }

    // --- touch: frontend snapshots consumed by either the mod runtime or dsm:u ---
    void SetTouch(std::span<const AuxTouchPoint> points) {
        std::scoped_lock lk{touch_mutex};
        std::array<AuxTouchPoint, MaxTouch> next{};
        size_t next_count = 0;
        for (const auto& point : points.first(std::min(points.size(), MaxTouch))) {
            auto& merged = next[next_count++];
            merged = point;
            for (size_t i = 0; i < touch_count; ++i) {
                if (touch_points[i].finger_id == point.finger_id) {
                    // A move must not overwrite a start the consumer has not seen yet.
                    merged.attributes |= touch_points[i].attributes & 1u;
                    break;
                }
            }
        }
        for (size_t i = 0; i < touch_count && next_count < MaxTouch; ++i) {
            const auto& previous = touch_points[i];
            const bool still_present = std::any_of(
                next.begin(), next.begin() + next_count,
                [&](const AuxTouchPoint& point) { return point.finger_id == previous.finger_id; });
            if (!still_present) {
                // Frontends can remove a pointer without an explicit end record, including
                // one pointer of a pinch. Keep its release until the consumer reads it.
                next[next_count] = previous;
                next[next_count++].attributes |= 2u;
            }
        }
        touch_points = next;
        touch_count = next_count;
    }

    size_t GetTouch(std::span<AuxTouchPoint> out) {
        std::scoped_lock lk{touch_mutex};
        const size_t n = std::min(out.size(), touch_count);
        size_t retained = 0;
        for (size_t i = 0; i < touch_count; ++i) {
            auto point = touch_points[i];
            if (i < n) {
                out[i] = point;
                if ((point.attributes & 1u) != 0) {
                    // A complete short tap needs two observations: first down, then up.
                    // Otherwise gesture consumers that skip ended points never see it.
                    out[i].attributes &= ~2u;
                    point.attributes &= ~1u;
                } else if ((point.attributes & 2u) != 0) {
                    continue;
                }
            }
            touch_points[retained++] = point;
        }
        touch_count = retained;
        return n;
    }

    // --- mod UI framebuffer: produced by the mod runtime (core), consumed by the renderer ---
    // RGBA8, tightly packed, ui_w * ui_h pixels. Published at the runtime's tick rate and
    // uploaded by the renderer only when it changed.
    void PublishUi(u32 w, u32 h, std::span<const u32> pixels) {
        if (pixels.size() != static_cast<size_t>(w) * h) {
            return; // a truncated canvas would let the upload copy read past the buffer
        }
        std::scoped_lock lk{ui_mutex};
        const bool identity_changed =
            (w != ui_w || h != ui_h || ui_pixels.size() != static_cast<size_t>(w) * h);
        ui_w = w;
        ui_h = h;
        // A full publish always brings ui_pixels fully in sync with `pixels`, and any
        // composite-mode staleness (see PublishComposite/PublishUiPartial) is moot: this copy
        // already resynced.
        ui_composite_pending.store(false, std::memory_order_relaxed);
        if (identity_changed || !UiDiffEnabled()) {
            // Profiling: isolate this copy's own cost. Same EDEN_DSMOD_PROFILE gate + 5s
            // LOG_INFO cadence as the mod runtime's RuntimeStageTimer.
            TimeAuxCopy([&] { ui_pixels.assign(pixels.begin(), pixels.end()); });
            if (identity_changed) {
                ui_tiles.Reset(w, h);
            }
            ui_tiles.all = true;
            ui_dirty = FullRect(w, h);
        } else {
            // Tile diff: a full redraw usually repaints mostly identical pixels (a spinning disc,
            // one changed number). Copy and mark only what differs; publish nothing if nothing did.
            std::array<s32, 4> changed{};
            TimeAuxCopy(
                [&] { changed = DiffCopyRect(pixels, ui_pixels, w, FullRect(w, h), ui_tiles); });
            if (changed[2] <= 0 || changed[3] <= 0) {
                ui_present.store(true, std::memory_order_release);
                return; // identical frame: the renderer keeps the last present
            }
            ui_dirty = UnionRect(ui_dirty, changed);
        }
        ui_serial++;
        ui_present.store(true, std::memory_order_release);
    }

    /// PublishUi for a producer that writes the frame in place (one copy fewer): `fill` receives
    /// the w * h buffer under the lock and must write every pixel of it.
    template <typename F>
    void PublishUiWith(u32 w, u32 h, F&& fill) {
        std::scoped_lock lk{ui_mutex};
        if (w != ui_w || h != ui_h || ui_pixels.size() != static_cast<size_t>(w) * h) {
            ui_tiles.Reset(w, h);
        }
        ui_w = w;
        ui_h = h;
        ui_pixels.resize(static_cast<size_t>(w) * h);
        fill(std::span<u32>{ui_pixels});
        ui_tiles.all = true; // written in place: nothing to diff against
        ui_dirty = FullRect(w, h);
        ui_composite_pending.store(false, std::memory_order_relaxed);
        ui_serial++;
        ui_present.store(true, std::memory_order_release);
    }

    /// Dirty-rect-aware publish: `dirty` is {x, y, w, h} of the region that actually changed since
    /// the last publish, in `pixels`' own coordinate space. Real body: copies only the
    /// dirty rows' dirty span under `ui_mutex`. Correct, not approximate -- core only ever draws
    /// inside `dirty` on a tick where this is called (PublishUi's `partial`, mod_redraw.cpp), so
    /// `pixels` and `ui_pixels` already agree everywhere outside it.
    ///
    /// Two cases fall back to a full copy instead of trusting `dirty`, both because the "already
    /// agree everywhere else" invariant above does not hold for them:
    ///  - `identity_changed`: first publish ever, or a size/shape change -- there is no valid prior
    ///    `ui_pixels` content to patch a sub-rect into.
    ///  - `composite_resync`: see PublishComposite below. On an `EDEN_DSMOD_GPU_COMPOSITE` page,
    ///    core alternates *per tick* between this canvas path and the quad path
    ///    (`PublishGpuComposite` -> `PublishComposite`), decided by whether the Map widget itself
    ///    is inside that tick's dirty rect (`draw_list->active`, set in the Map widget's own draw
    ///    dispatch). A run of quad-path ticks never touches `ui_pixels` at all -- but core's own
    ///    `canvas` buffer keeps accumulating other widgets' changes underneath, tick after tick,
    ///    since `RenderPageTo` still draws the non-Map widgets into it even when the Map widget
    ///    itself is routed to quads. So the first canvas-path publish after such a run would be
    ///    patching only the LATEST tick's small `dirty` rect onto a `ui_pixels` buffer that is
    ///    stale by however many quad-path ticks preceded it -- a real half-stale-screen bug. The
    ///    fix: `PublishComposite` sets `ui_composite_pending`; the next call here consumes it and
    ///    forces one full resync, after which the row-copy invariant holds again.
    void PublishUiPartial(u32 w, u32 h, std::span<const u32> pixels, std::array<s32, 4> dirty) {
        PublishUiPartialRects(w, h, pixels, std::span<const std::array<s32, 4>>{&dirty, 1});
    }

    /// PublishUiPartial for several dirty rects at once: one lock and one serial for all of them,
    /// so the renderer never takes a frame with only some of the rects copied.
    void PublishUiPartialRects(u32 w, u32 h, std::span<const u32> pixels,
                               std::span<const std::array<s32, 4>> rects) {
        if (pixels.size() != static_cast<size_t>(w) * h || rects.empty()) {
            return; // a truncated canvas would let the copy read past the buffer
        }
        std::scoped_lock lk{ui_mutex};
        const bool identity_changed =
            (w != ui_w || h != ui_h || ui_pixels.size() != static_cast<size_t>(w) * h);
        const bool composite_resync =
            ui_composite_pending.exchange(false, std::memory_order_acq_rel);
        if (identity_changed || (composite_resync && !UiDiffEnabled())) {
            ui_w = w;
            ui_h = h;
            TimeAuxCopy([&] { ui_pixels.assign(pixels.begin(), pixels.end()); });
            if (identity_changed) {
                ui_tiles.Reset(w, h);
            }
            ui_tiles.all = true;
            ui_dirty = FullRect(w, h);
        } else if (UiDiffEnabled()) {
            // Tile diff: diff inside the dirty rects (or, after a composite run, the whole canvas
            // -- a diff against ui_pixels finds exactly what the quad-path ticks left stale).
            bool any = false;
            const auto diff_rect = [&](std::array<s32, 4> rect) {
                std::array<s32, 4> changed{};
                TimeAuxCopy([&] { changed = DiffCopyRect(pixels, ui_pixels, w, rect, ui_tiles); });
                if (changed[2] > 0 && changed[3] > 0) {
                    ui_dirty = UnionRect(ui_dirty, changed);
                    any = true;
                }
            };
            if (composite_resync) {
                diff_rect(FullRect(w, h));
            } else {
                for (const auto& dirty : rects) {
                    const auto rect = ClampRect(dirty, w, h);
                    if (rect[2] > 0 && rect[3] > 0) {
                        diff_rect(rect);
                    }
                }
            }
            if (!any) {
                ui_present.store(true, std::memory_order_release);
                return; // the redraw produced the pixels already held: nothing to upload
            }
        } else {
            for (const auto& dirty : rects) {
                const auto [x0, y0, cw, ch] = ClampRect(dirty, w, h);
                if (cw <= 0 || ch <= 0) {
                    continue;
                }
                TimeAuxCopy([&] {
                    for (s32 y = y0; y < y0 + ch; ++y) {
                        const size_t row = static_cast<size_t>(y) * w + static_cast<size_t>(x0);
                        std::copy_n(pixels.data() + row, static_cast<size_t>(cw),
                                    ui_pixels.data() + row);
                    }
                });
                ui_tiles.MarkRect(x0, y0, cw, ch);
                ui_dirty = UnionRect(ui_dirty, {x0, y0, cw, ch});
            }
        }
        ui_serial++;
        ui_present.store(true, std::memory_order_release);
    }

    // --- haptic feedback: raised by the mod runtime, played by the frontend ---
    /// strength: 1 light, 2 click, 3 confirm, 4 heavy, 5 reject (Core::Mods::HapticStrength);
    /// kind: what raised it (Core::Mods::HapticKind); flags bit 0: honour the system's own
    /// touch-feedback setting.
    struct HapticEvent {
        u8 strength{};
        u8 kind{};
        u8 flags{};
    };
    using HapticSink = std::function<void(const HapticEvent&)>;

    /// Registers the frontend's player (replacing any previous one; empty = none). The sink runs
    /// on a notifier thread owned by this object, never on the thread that raised the event.
    void SetHapticSink(HapticSink sink) {
        std::scoped_lock lk{haptic_mutex};
        haptic_sink = std::move(sink);
        if (haptic_sink && !haptic_thread.joinable()) {
            haptic_thread = std::jthread([this](std::stop_token stop) { HapticLoop(stop); });
        }
    }

    /// Queues one event for the frontend (bounded; dropped while nobody listens). Only takes a
    /// short lock: safe from the emulation thread.
    void RaiseHaptic(HapticEvent event) {
        haptic_raised.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lk{haptic_mutex};
        if (!haptic_sink) {
            return;
        }
        if (haptic_queue.size() >= 8) {
            haptic_queue.pop_front();
        }
        haptic_queue.push_back(event);
        haptic_cv.notify_one();
    }

    /// Events raised so far (with or without a sink), for tests.
    std::atomic<u64> haptic_raised{0};

    void ClearUi() {
        std::scoped_lock lk{ui_mutex};
        ui_pixels.clear();
        ui_w = ui_h = 0;
        ui_tiles.Reset(0, 0);
        ui_dirty = {0, 0, 0, 0};
        ui_composite_pending.store(false, std::memory_order_relaxed);
        ui_serial++;
        ui_present.store(false, std::memory_order_release);
    }

    [[nodiscard]] bool HasUi() const {
        return ui_present.load(std::memory_order_acquire);
    }

    /// Copies the current UI into `out` when it differs from `serial_in_out`. Returns true on copy.
    bool TakeUi(std::vector<u32>& out, u32& w, u32& h, u64& serial_in_out) {
        std::scoped_lock lk{ui_mutex};
        if (ui_serial == serial_in_out || ui_pixels.empty()) {
            return false;
        }
        out = ui_pixels;
        w = ui_w;
        h = ui_h;
        serial_in_out = ui_serial;
        return true;
    }

    // ---------------- GPU compositor path (EDEN_DSMOD_GPU_COMPOSITE) ----------------
    // When enabled, the mod runtime publishes a small display list of textured quads plus up to
    // NumAuxTex source textures (0=current map, 1=icon atlas, 2=HUD overlay, 3=item-room pulse,
    // 4=previous map endpoint, 5=map fade weights). The renderer composites
    // them on the GPU each frame, so a panning map or moving marker costs a few quad draws instead
    // of re-rasterising + re-uploading the whole ~5 MB canvas. Textures re-upload only when their
    // serial moves (map on reveal, atlas once, HUD on change); the quad list is tiny and per-frame.
    static constexpr u32 NumAuxTex = 6;
    static constexpr u32 MapCurrentSlot = 0;
    static constexpr u32 MapPreviousSlot = 4;
    static constexpr u32 MapFadeSlot = 5;
    struct Quad {
        u32 slot;                        // source texture index (0..NumAuxTex-1)
        float u0, v0, u1, v1;            // source UV, normalised 0..1
        float x, y, w, h;                // destination rect, canvas pixels
        float a{1.0f};                   // alpha multiplier (blink pulses); 1 = as drawn
        float r{1.0f}, g{1.0f}, b{1.0f}; // colour multiplier (a white glyph in a marker colour)
        bool map_fade{false}; // sample previous/current/weight textures with the map pipeline
        bool solid{false};    // fill the destination rect with color; slot/UV/tint are ignored
        u32 color{0xFF000000u};
    };

    std::atomic<bool> gpu_composite{false};

    void PublishAuxTexture(u32 slot, u32 tw, u32 th, std::span<const u32> pixels) {
        if (slot >= NumAuxTex || pixels.size() != static_cast<size_t>(tw) * th) {
            return;
        }
        std::scoped_lock lk{comp_mutex};
        if (slot == MapCurrentSlot || slot == MapPreviousSlot) {
            ++map_bundle_epoch; // no longer what the last map-bundle publish left there
        }
        auto& t = comp_tex[slot];
        if (UiDiffEnabled() && t.w == tw && t.h == th &&
            t.pixels.size() == static_cast<size_t>(tw) * th && t.tiles.cols != 0) {
            // Tile diff: the HUD overlay (slot 2) is a full-canvas texture republished whenever any
            // HUD pixel moves; upload only the tiles that did.
            const auto changed = DiffCopyRect(pixels, t.pixels, tw, FullRect(tw, th), t.tiles);
            if (changed[2] > 0 && changed[3] > 0) {
                t.serial++;
            }
            return;
        }
        t.w = tw;
        t.h = th;
        t.pixels.assign(pixels.begin(), pixels.end());
        t.tiles.Reset(tw, th);
        t.serial++;
    }

    /// PublishAuxTexture for a producer that knows `pixels` equals what the slot already holds
    /// everywhere outside `rects` (the HUD canvas of a partial redraw): only the rects are diffed
    /// and copied. A slot of another size, or with tile diffing off, takes the whole picture.
    void PublishAuxTextureRects(u32 slot, u32 tw, u32 th, std::span<const u32> pixels,
                                std::span<const std::array<s32, 4>> rects) {
        if (slot >= NumAuxTex || pixels.size() != static_cast<size_t>(tw) * th) {
            return;
        }
        {
            std::scoped_lock lk{comp_mutex};
            auto& t = comp_tex[slot];
            if (UiDiffEnabled() && t.w == tw && t.h == th &&
                t.pixels.size() == static_cast<size_t>(tw) * th && t.tiles.cols != 0) {
                if (slot == MapCurrentSlot || slot == MapPreviousSlot) {
                    ++map_bundle_epoch;
                }
                bool changed = false;
                for (const auto& r : rects) {
                    const s32 x0 = std::max(r[0], 0), y0 = std::max(r[1], 0);
                    const s32 x1 = std::min(r[0] + r[2], static_cast<s32>(tw));
                    const s32 y1 = std::min(r[1] + r[3], static_cast<s32>(th));
                    if (x1 <= x0 || y1 <= y0) {
                        continue;
                    }
                    const auto c = DiffCopyRect(pixels, t.pixels, tw, {x0, y0, x1 - x0, y1 - y0},
                                                t.tiles);
                    changed = changed || (c[2] > 0 && c[3] > 0);
                }
                if (changed) {
                    t.serial++;
                }
                return;
            }
        }
        PublishAuxTexture(slot, tw, th, pixels);
    }

    /// Publish a coherent map-fade bundle. Holding one lock across all three slots prevents the
    /// renderer from observing a new endpoint beside an old weight grid.
    /// Returns the bundle epoch this publish produced (0: rejected), the base a later
    /// PublishMapFadeTexturesTiles names.
    u64 PublishMapFadeTextures(u32 map_w, u32 map_h, std::span<const u32> current,
                               std::span<const u32> previous, u32 fade_w, u32 fade_h,
                               std::span<const u32> weights) {
        if (current.size() != static_cast<size_t>(map_w) * map_h ||
            previous.size() != current.size() ||
            weights.size() != static_cast<size_t>(fade_w) * fade_h) {
            return 0;
        }
        std::scoped_lock lk{comp_mutex};
        const auto set = [&](u32 slot, u32 w, u32 h, std::span<const u32> pixels) {
            auto& t = comp_tex[slot];
            t.w = w;
            t.h = h;
            t.pixels.assign(pixels.begin(), pixels.end());
            t.tiles.Reset(w, h);
            ++t.serial;
        };
        set(MapCurrentSlot, map_w, map_h, current);
        set(MapPreviousSlot, map_w, map_h, previous);
        set(MapFadeSlot, fade_w, fade_h, weights);
        return ++map_bundle_epoch;
    }

    /// The map bundle again, as a change against the publish that produced `base_epoch`:
    /// `current_dirty`/`previous_dirty` (TileMask::Size tiles over map_w x map_h) mark every tile
    /// where the new endpoint differs from the one that publish carried, and only those tiles are
    /// copied in (and flagged for the renderer's upload). The weight grid is replaced whole.
    /// Returns the new epoch, or 0 without touching anything when the routing buffer no longer
    /// holds that base (another publish came between, or the size changed): publish in full then.
    /// A reveal changes a few cells of a 3072x1052 map, so this keeps the ~26 MB endpoint copy --
    /// under comp_mutex, which the render thread takes every frame -- down to the changed tiles.
    u64 PublishMapFadeTexturesTiles(u32 map_w, u32 map_h, std::span<const u32> current,
                                    std::span<const u32> previous, const TileMask& current_dirty,
                                    const TileMask& previous_dirty, u32 fade_w, u32 fade_h,
                                    std::span<const u32> weights, u64 base_epoch) {
        const size_t map_px = static_cast<size_t>(map_w) * map_h;
        const auto mask_fits = [&](const TileMask& m) {
            return m.cols == (map_w + TileMask::Size - 1) / TileMask::Size &&
                   m.rows == (map_h + TileMask::Size - 1) / TileMask::Size &&
                   m.bits.size() == static_cast<size_t>(m.cols) * m.rows && !m.all;
        };
        if (base_epoch == 0 || current.size() != map_px || previous.size() != map_px ||
            weights.size() != static_cast<size_t>(fade_w) * fade_h || !mask_fits(current_dirty) ||
            !mask_fits(previous_dirty)) {
            return 0;
        }
        std::scoped_lock lk{comp_mutex};
        const auto base_ok = [&](const AuxTex& t) {
            return t.w == map_w && t.h == map_h && t.pixels.size() == map_px &&
                   t.tiles.cols == current_dirty.cols && t.tiles.rows == current_dirty.rows &&
                   t.tiles.bits.size() == current_dirty.bits.size();
        };
        if (map_bundle_epoch != base_epoch || !base_ok(comp_tex[MapCurrentSlot]) ||
            !base_ok(comp_tex[MapPreviousSlot])) {
            return 0;
        }
        const auto patch = [&](u32 slot, std::span<const u32> src, const TileMask& dirty) {
            auto& t = comp_tex[slot];
            bool any = false;
            for (u32 r = 0; r < dirty.rows; ++r) {
                const u32 y0 = r * TileMask::Size;
                const u32 y1 = std::min(y0 + TileMask::Size, map_h);
                for (u32 c = 0; c < dirty.cols; ++c) {
                    const size_t bit = static_cast<size_t>(r) * dirty.cols + c;
                    if (dirty.bits[bit] == 0) {
                        continue;
                    }
                    const u32 x0 = c * TileMask::Size;
                    const size_t n = std::min(TileMask::Size, map_w - x0);
                    for (u32 y = y0; y < y1; ++y) {
                        const size_t at = static_cast<size_t>(y) * map_w + x0;
                        std::memcpy(t.pixels.data() + at, src.data() + at, n * sizeof(u32));
                    }
                    t.tiles.bits[bit] = 1;
                    any = true;
                }
            }
            if (any) {
                ++t.serial;
            }
        };
        patch(MapCurrentSlot, current, current_dirty);
        patch(MapPreviousSlot, previous, previous_dirty);
        auto& fade = comp_tex[MapFadeSlot];
        fade.w = fade_w;
        fade.h = fade_h;
        fade.pixels.assign(weights.begin(), weights.end());
        fade.tiles.Reset(fade_w, fade_h);
        ++fade.serial;
        return ++map_bundle_epoch;
    }

    /// Visit every changed member of the map bundle under one lock. The renderer must not take
    /// the three slots separately: a producer publish between those reads could pair endpoints
    /// from one visibility generation with weights from the next.
    template <typename F>
    bool WithMapFadeTextures(u64& current_serial, u64& previous_serial, u64& fade_serial, F&& fn) {
        std::scoped_lock lk{comp_mutex};
        auto visit = [&](u32 slot, u64& serial) {
            auto& t = comp_tex[slot];
            if (t.serial == serial || t.pixels.empty()) {
                return false;
            }
            // `t.tiles`: every tile changed since the previous take (all, after a full publish).
            fn(slot, std::span<const u32>{t.pixels}, t.w, t.h, std::as_const(t.tiles));
            serial = t.serial;
            t.tiles.Clear();
            return true;
        };
        bool changed = visit(MapCurrentSlot, current_serial);
        changed = visit(MapPreviousSlot, previous_serial) || changed;
        changed = visit(MapFadeSlot, fade_serial) || changed;
        return changed;
    }

    /// Visit a slot's texture in place (under the lock) when its serial moved past
    /// `serial_in_out`: the renderer copies straight into its staging buffer, so a publish costs
    /// one copy, not three plus an allocation. Returns whether `fn` ran.
    template <typename F>
    bool WithAuxTexture(u32 slot, u64& serial_in_out, F&& fn) {
        if (slot >= NumAuxTex) {
            return false;
        }
        std::scoped_lock lk{comp_mutex};
        auto& t = comp_tex[slot];
        if (t.serial == serial_in_out || t.pixels.empty()) {
            return false;
        }
        fn(std::span<const u32>{t.pixels}, t.w, t.h);
        serial_in_out = t.serial;
        t.tiles.Clear();
        return true;
    }

    /// Dirty-tile variant of WithAuxTexture: `fn(px, w, h, mask)`; the mask covers every change
    /// since the previous take of this slot.
    template <typename F>
    bool WithAuxTextureTiles(u32 slot, u64& serial_in_out, F&& fn) {
        if (slot >= NumAuxTex) {
            return false;
        }
        std::scoped_lock lk{comp_mutex};
        auto& t = comp_tex[slot];
        if (t.serial == serial_in_out || t.pixels.empty()) {
            return false;
        }
        fn(std::span<const u32>{t.pixels}, t.w, t.h, static_cast<const TileMask&>(t.tiles));
        serial_in_out = t.serial;
        t.tiles.Clear();
        return true;
    }

    /// Visits the flat CPU canvas (under the lock) when it was published since
    /// `serial_in_out`, and also hands back `out_dirty`: the union of every dirty rect published
    /// since the last successful call here (not just the latest publish's own rect) -- a consumer
    /// may poll less often than the producer publishes, so what changed "since I last looked" can
    /// span several publishes. Only meaningful when this returns true. The consumer (renderer) is
    /// still responsible for its OWN identity check (a fresh/resized destination image has no valid
    /// prior content to patch a sub-rect into, the same reasoning as PublishUiPartial's
    /// `identity_changed` case, one layer up) -- this call cannot see that, it only knows about
    /// `ui_pixels` itself.
    template <typename F>
    bool WithUiPartial(u64& serial_in_out, std::array<s32, 4>& out_dirty, F&& fn) {
        if (ui_serial.load(std::memory_order_acquire) == serial_in_out) {
            return false; // lock-free fast path: nothing published since the last take
        }
        std::scoped_lock lk{ui_mutex};
        if (ui_serial == serial_in_out || ui_pixels.empty() || ui_w == 0 || ui_h == 0) {
            return false;
        }
        out_dirty = ui_dirty;
        fn(std::span<const u32>{ui_pixels}, ui_w, ui_h);
        serial_in_out = ui_serial;
        ui_dirty = {0, 0, 0, 0}; // consumed; the next accumulation starts fresh
        ui_tiles.Clear();
        return true;
    }

    /// Dirty-tile consumer: like WithUiPartial, but hands over the dirty-tile mask accumulated
    /// since the last successful take (mask.all = replace the whole image) plus the bounding box
    /// of every changed pixel since then: `fn(px, w, h, mask, bbox)`. The renderer still forces a
    /// full upload when its own destination image is new or resized.
    template <typename F>
    bool WithUiTiles(u64& serial_in_out, F&& fn) {
        if (ui_serial.load(std::memory_order_acquire) == serial_in_out) {
            return false; // lock-free fast path: nothing published since the last take
        }
        std::scoped_lock lk{ui_mutex};
        if (ui_serial == serial_in_out || ui_pixels.empty() || ui_w == 0 || ui_h == 0) {
            return false;
        }
        // `ui_dirty` (the exact changed bounding box) lets the consumer trim tile-rounded regions:
        // a lone 64x64 widget straddling four tiles uploads its own box, not 128x128.
        fn(std::span<const u32>{ui_pixels}, ui_w, ui_h, static_cast<const TileMask&>(ui_tiles),
           ui_tiles.all ? FullRect(ui_w, ui_h) : ui_dirty);
        serial_in_out = ui_serial;
        ui_dirty = {0, 0, 0, 0};
        ui_tiles.Clear();
        return true;
    }

    void PublishComposite(u32 cw, u32 ch, u32 bg, std::span<const Quad> quads) {
        std::scoped_lock lk{comp_mutex};
        comp_w = cw;
        comp_h = ch;
        comp_bg = bg;
        comp_quads.assign(quads.begin(), quads.end());
        comp_serial++;
        comp_present.store(true, std::memory_order_release);
        // See PublishUiPartial's doc comment ("composite_resync"): this tick took the quad path
        // instead of the canvas path, so `ui_pixels` is now potentially behind whatever core's
        // `canvas` buffer accumulates on subsequent quad-path ticks. Force one full resync on the
        // next canvas-path publish rather than trusting its `dirty` to describe everything that
        // changed while we were over here.
        ui_composite_pending.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool HasComposite() const {
        return comp_present.load(std::memory_order_acquire);
    }

    void ClearComposite() {
        std::scoped_lock lk{comp_mutex};
        comp_quads.clear();
        comp_present.store(false, std::memory_order_release);
    }

    bool TakeComposite(std::vector<Quad>& out, u32& cw, u32& ch, u32& bg, u64& serial_in_out) {
        std::scoped_lock lk{comp_mutex};
        if (comp_serial == serial_in_out) {
            return false;
        }
        out = comp_quads;
        cw = comp_w;
        ch = comp_h;
        bg = comp_bg;
        serial_in_out = comp_serial;
        return true;
    }

private:
    // See TimeAuxCopy: profiling instrumentation for the copy cost, not otherwise
    // load-bearing.
    struct CopyStats {
        using Clock = std::chrono::steady_clock;
        Clock::time_point window = Common::DSMod::DevToolsEnabled ? Clock::now() : Clock::time_point{};
        double total_ms{};
        double max_ms{};
        u64 calls{};
    };
    CopyStats ui_copy_stats;

    /// Times `copy` (expected: a `ui_pixels`-sized buffer copy) and logs a rolling 5s summary,
    /// gated on EDEN_DSMOD_PROFILE exactly like core's RuntimeStageTimer. `copy` runs
    /// unconditionally either way -- this only wraps it, never skips it.
    template <typename F>
    void TimeAuxCopy(F&& copy) {
        static const bool profile_enabled = [] {
            const char* value = Common::DSMod::DevEnvironment("EDEN_DSMOD_PROFILE");
            return Common::DSMod::DevToolsEnabled &&
                   (!value || (std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
                               std::strcmp(value, "FALSE") != 0));
        }();
        if (!profile_enabled) {
            copy();
            return;
        }
        const auto t0 = CopyStats::Clock::now();
        copy();
        const auto now = CopyStats::Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - t0).count();
        ui_copy_stats.total_ms += ms;
        ui_copy_stats.max_ms = std::max(ui_copy_stats.max_ms, ms);
        ++ui_copy_stats.calls;
        if (now - ui_copy_stats.window >= std::chrono::seconds{5}) {
            LOG_INFO(Render_Vulkan,
                     "DSMod perf CPU aux-publish-memcpy: calls={} avg={:.3f}ms max={:.3f}ms",
                     ui_copy_stats.calls, ui_copy_stats.total_ms / ui_copy_stats.calls,
                     ui_copy_stats.max_ms);
            ui_copy_stats = {};
        }
    }

    // ---- PublishUiPartial / WithUiPartial helpers ----
    // All three treat a rect as {x, y, w, h}; a non-positive w or h is "empty" throughout.

    static constexpr std::array<s32, 4> FullRect(u32 w, u32 h) {
        return {0, 0, static_cast<s32>(w), static_cast<s32>(h)};
    }

    /// Clamps `r` to the [0, canvas_w) x [0, canvas_h) rectangle -- never reads or writes outside
    /// the canvas even if core ever hands over an out-of-range or negative rect.
    static std::array<s32, 4> ClampRect(std::array<s32, 4> r, u32 canvas_w, u32 canvas_h) {
        const s32 max_x = static_cast<s32>(canvas_w);
        const s32 max_y = static_cast<s32>(canvas_h);
        const s32 x0 = std::clamp(r[0], 0, max_x);
        const s32 y0 = std::clamp(r[1], 0, max_y);
        const s32 x1 = std::clamp(r[0] + std::max(r[2], 0), 0, max_x);
        const s32 y1 = std::clamp(r[1] + std::max(r[3], 0), 0, max_y);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    /// Bounding-box union, used to accumulate dirty regions across publishes a consumer hasn't
    /// taken (WithUiPartial) yet -- a single "latest rect" would silently drop earlier ones.
    static std::array<s32, 4> UnionRect(std::array<s32, 4> a, std::array<s32, 4> b) {
        if (a[2] <= 0 || a[3] <= 0) {
            return b;
        }
        if (b[2] <= 0 || b[3] <= 0) {
            return a;
        }
        const s32 x0 = std::min(a[0], b[0]);
        const s32 y0 = std::min(a[1], b[1]);
        const s32 x1 = std::max(a[0] + a[2], b[0] + b[2]);
        const s32 y1 = std::max(a[1] + a[3], b[1] + b[3]);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    mutable std::mutex ui_mutex;
    std::vector<u32> ui_pixels;
    u32 ui_w{0};
    u32 ui_h{0};
    /// Atomic so the renderer's per-frame "anything new?" check can skip ui_mutex entirely: the
    /// producer holds that lock for its whole diff/copy (~1 ms desktop per full publish), and the
    /// GPU thread polls every guest frame.
    std::atomic<u64> ui_serial{0};
    std::atomic<bool> ui_present{false};
    /// Union of dirty rects published since the last successful WithUiPartial consume. Protected
    /// by ui_mutex like ui_pixels itself (both producer and consumer touch it only under that
    /// lock).
    std::array<s32, 4> ui_dirty{0, 0, 0, 0};
    /// Tiles changed since the last WithUiTiles/WithUiPartial consume (ui_mutex).
    TileMask ui_tiles;
    /// Set by PublishComposite, consumed by PublishUiPartial -- see both methods' doc comments.
    /// Atomic even though today's only writer/reader pattern is single-producer-thread: it is
    /// written under comp_mutex's critical section and read under ui_mutex's, two different locks,
    /// so it needs its own independent synchronization rather than borrowing either one's.
    std::atomic<bool> ui_composite_pending{false};

    std::mutex touch_mutex;
    std::array<AuxTouchPoint, MaxTouch> touch_points{};
    size_t touch_count{0};

    struct AuxTex {
        std::vector<u32> pixels;
        u32 w{0};
        u32 h{0};
        u64 serial{0};
        TileMask tiles; ///< tiles changed since the renderer's last take of this slot
    };
    std::mutex comp_mutex;
    std::array<AuxTex, NumAuxTex> comp_tex;
    u64 map_bundle_epoch{0}; ///< bumped by every write of the map endpoint slots (comp_mutex)
    std::vector<Quad> comp_quads;
    u32 comp_w{0};
    u32 comp_h{0};
    u32 comp_bg{0};
    u64 comp_serial{0};
    std::atomic<bool> comp_present{false};

    void HapticLoop(std::stop_token stop) {
        std::unique_lock lk{haptic_mutex};
        while (!stop.stop_requested()) {
            haptic_cv.wait(lk, stop, [this] { return !haptic_queue.empty(); });
            while (!haptic_queue.empty()) {
                const HapticEvent event = haptic_queue.front();
                haptic_queue.pop_front();
                const HapticSink sink = haptic_sink; // copied: it may be replaced meanwhile
                lk.unlock();
                if (sink) {
                    sink(event);
                }
                lk.lock();
            }
        }
    }
    std::mutex haptic_mutex;
    std::condition_variable_any haptic_cv;
    std::deque<HapticEvent> haptic_queue;
    HapticSink haptic_sink;
    std::jthread haptic_thread; ///< last member: stopped and joined before the state above goes
};

} // namespace VideoCore::DSMod
