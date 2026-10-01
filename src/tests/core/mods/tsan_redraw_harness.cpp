// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone, boot-free, two-real-thread TSan harness for the DSMod off-thread bottom-screen
// redraw.
//
// No ModRuntime, no System, no guest CPU, no fiber code anywhere here: this only drives
// RenderPage/ExpandWidgets (core/mods/mod_ui.h) from two real std::threads against hand-built
// fixtures shaped like the real ModRuntime members and mutexes (mod_runtime.h) -- reproducing the
// real access pattern of the off-thread redraw (including the tick thread's own synchronous
// RenderPage carve-out for the debug/transition/GPU_COMPOSITE case), not a toy one.
//
// The race TEST_CASEs are separated so a routine run can pick what it covers. All of them mirror
// the production locking and are expected CLEAN:
//   1. "[redraw-worker]"              -- the main stress test: everything the redraw worker put a
//      lock around (map_visited/map_geometry, map_follow_state, map_draw_records, image_cache,
//      the msbt bucket's general traffic, marker flags), driven hard from two real threads,
//      including the tick thread's own direct RenderPage calls. Also exercises the map widget's
//      function-local statics (mod_ui_map_widget.cpp: GeometryMapDraw::FrameView's pan/zoom
//      debug-log rate limiter, MapLabelBitmap's memoisation cache), which must be synchronised, via
//      this page's pan_zoom Map widget + area label. The draw thread's job alternates gpu_composite
//      on/off across dispatches (RedrawJob::gpu_composite/draw_list, job-owned, mirroring the real
//      one), so RenderPage's `draw_list != nullptr` branches (the Map widget's quad emission,
//      mod_ui_map_widget.cpp) are reached from the worker thread concurrently with everything else
//      this test drives.
//   2. "[msbt-pointer]"                -- an isolated probe for GetMsbtText/TextProvider handing
//      out a pointer into a shared_ptr-owned map past its lock (a raw pointer there crashed with
//      SIGSEGV in under a second). TextProvider returns shared_ptr<const std::string>, aliased
//      onto the label map's own shared_ptr; MsbtFixture's Resolve() mirrors that.
//   3. "[known-gap]"                   -- the manifest.map_areas[...].markers[...] flags race.
//      RenderPage takes manifest_markers_mutex (reusing map_state_mutex) around its marker reads;
//      this test's writer takes the same lock, mirroring UpdateHiddenMarkers. The tag name is
//      historical and kept for command-line compatibility.
//
// Routine command (clean, ~HarnessDuration() * 2 wall clock): filter to "[redraw-worker]" and NOT
// "[known-gap]", e.g. `./tsan_redraw_harness "[redraw-worker]~[known-gap]"`. Under TSan, run with
// TSAN_OPTIONS suppressions for the third-party code it reports. Env knobs: TSAN_HARNESS_MS (run
// length per stress case) and TSAN_HARNESS_DISABLE_GENERATION_GUARD (see the transition-publish
// case below).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/mods/mod_ui.h"

using namespace Core::Mods;

namespace {

// How long each stress TEST_CASE's two threads run, wall clock. Overridable (TSAN_HARNESS_MS) to
// dial up a deeper soak or dial down an even faster smoke check. The default is picked so the
// WHOLE binary stays comfortably inside "seconds to a couple of minutes" even under TSan's
// overhead.
std::chrono::milliseconds HarnessDuration() {
    if (const char* const env = std::getenv("TSAN_HARNESS_MS")) {
        const long ms = std::strtol(env, nullptr, 10);
        if (ms > 0) {
            return std::chrono::milliseconds{ms};
        }
    }
    return std::chrono::milliseconds{1500};
}

// -------------------------------------------------------------------------------------------
// Fixture: image_cache, mirrors ModRuntime::image_cache + asset_cache_mutex (mod_runtime.h) and
// its CacheFindImage/CachePutImage/CacheEraseImagesIf helpers (mod_assets.cpp). The cache holds
// shared_ptr<const Image> rather than a raw Image for exactly this reason: a
// caller's returned handle must survive the cache's own eviction/reinsertion once the caller can be
// a second thread -- reproduced here by giving BOTH threads real concurrent access to it.
struct AssetCacheFixture {
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const Image>> image_cache;

    std::shared_ptr<const Image> Find(const std::string& key) const {
        std::scoped_lock lock{mutex};
        const auto it = image_cache.find(key);
        return it == image_cache.end() ? nullptr : it->second;
    }
    void Put(const std::string& key, std::shared_ptr<const Image> img) {
        std::scoped_lock lock{mutex};
        image_cache[key] = std::move(img);
    }
    // Mirrors CacheEraseImagesIf / DrainModuleImages' LRU eviction / ReloadManifest's clear().
    void EraseSome(std::size_t max_keep) {
        std::scoped_lock lock{mutex};
        while (image_cache.size() > max_keep) {
            image_cache.erase(image_cache.begin());
        }
    }
};

// GetImage-equivalent: on-demand rasterise-and-cache, invoked from RenderPage's own ImageProvider
// callback -- exercised from whichever thread is mid-RenderPage, exactly like the real GetImage.
std::shared_ptr<const Image> SimGetImage(AssetCacheFixture& cache, const std::string& key) {
    if (auto hit = cache.Find(key)) {
        return hit;
    }
    // Content is irrelevant -- only the cache traffic (the thing under test) matters.
    auto img = std::make_shared<Image>(Image{4, 4, std::vector<u32>(16, 0xFF203040u)});
    cache.Put(key, img);
    return img;
}

// -------------------------------------------------------------------------------------------
// Fixture: map_visited/map_geometry, mirrors ModRuntime::map_visited/map_geometry + the recursive
// map_state_mutex (declared in mod_runtime.h, taken in mod_map.cpp). Recursive because
// MarkVisitedAt calls GeometryMask while already holding the lock in the real code -- reproduced
// here, not simplified away, since the reentrancy itself is part of what needs to hold up under
// TSan's own mutex-ordering checks.
struct MapStateFixture {
    mutable std::recursive_mutex mutex;
    std::map<std::string, VisitedGrid> visited;
    std::map<std::string, std::vector<u8>> geometry;

    // Caller must already hold `mutex` -- mirrors GeometryMask's own "caller-locked" contract
    // (mod_runtime.h's comment on the real function).
    const std::vector<u8>& GeometryMaskLocked(const std::string& area) {
        auto it = geometry.find(area);
        if (it == geometry.end()) {
            it = geometry.emplace(area, std::vector<u8>(64, 1)).first;
        }
        return it->second;
    }

    static std::pair<int, int> Cell(float wx, float wy) {
        const int cx = static_cast<int>(std::clamp(wx, 0.0f, float{VisitedGrid::Cols - 1}));
        const int cy = static_cast<int>(std::clamp(wy, 0.0f, float{VisitedGrid::Rows - 1}));
        return {cx, cy};
    }

    // Mirrors MarkVisitedAt: reveals a small radius around (wx, wy), bumps `generation` once if
    // anything actually changed this call.
    void MarkVisitedAt(const std::string& area, float wx, float wy) {
        std::scoped_lock lock{mutex};
        (void)GeometryMaskLocked(area); // reentrant lock, same thread -- see comment above
        auto& grid = visited[area];
        const auto [cx, cy] = Cell(wx, wy);
        bool changed = false;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int px = cx + dx;
                const int py = cy + dy;
                if (px < 0 || py < 0 || px >= VisitedGrid::Cols || py >= VisitedGrid::Rows) {
                    continue;
                }
                const std::size_t idx =
                    static_cast<std::size_t>(py) * VisitedGrid::Cols + static_cast<std::size_t>(px);
                if (grid.cells[idx] != VisitedGrid::Visited) {
                    grid.prev[idx] = grid.cells[idx];
                    grid.cells[idx] = VisitedGrid::Visited;
                    grid.change_tick[idx] = static_cast<u32>(grid.generation);
                    changed = true;
                }
            }
        }
        if (changed) {
            ++grid.generation;
            grid.last_reveal_tick = grid.generation;
        }
    }

    bool IsVisited(const std::string& area, float wx, float wy) const {
        std::scoped_lock lock{mutex};
        const auto it = visited.find(area);
        if (it == visited.end()) {
            return false;
        }
        const auto [cx, cy] = Cell(wx, wy);
        const std::size_t idx =
            static_cast<std::size_t>(cy) * VisitedGrid::Cols + static_cast<std::size_t>(cx);
        return it->second.cells[idx] != VisitedGrid::Unexplored;
    }

    // Mirrors AcceptModuleMap/AcceptModuleMapState (mod_module_host.cpp): a module callback that
    // writes wall/door/zone-shaped state directly, from the tick thread, during sampling -- a
    // THIRD writer of this family, under this same lock.
    void ModuleWrite(const std::string& area, u64 tick) {
        std::scoped_lock lock{mutex};
        auto& g = geometry[area];
        if (g.empty()) {
            g.assign(64, 0);
        }
        g[tick % g.size()] = static_cast<u8>((tick / g.size()) % 2);
    }
};

// -------------------------------------------------------------------------------------------
// Fixture: the msbt text bucket, mirrors NxAssetState::msbt.texts + its recursive state_mutex
// (mod_nx_runtime.cpp). Resolve() is shaped EXACTLY like the real GetMsbtText: it
// takes the lock, looks the alias up, and returns a shared_ptr ALIASED onto the alias's own
// shared_ptr<const MsbtLabelMap> -- so the returned handle shares that object's refcount (keeping
// the whole label map alive, one atomic increment) while pointing at just the one string, and
// survives Land()'s reassignment of `texts[alias]` regardless of which thread does the
// reassigning. A raw pointer into the map returned past the lock's release crashed with SIGSEGV;
// this fixture mirrors the production shape (ModRuntime::GetMsbtText, mod_nx_runtime.cpp).
using MsbtLabelMap = std::unordered_map<std::string, std::string>;
struct MsbtFixture {
    mutable std::recursive_mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const MsbtLabelMap>> texts;

    std::shared_ptr<const std::string> Resolve(const std::string& ref) {
        std::scoped_lock lock{mutex};
        const std::size_t hash = ref.find('#');
        if (!ref.starts_with("msbt:") || hash == std::string::npos) {
            return nullptr;
        }
        const std::string alias = ref.substr(5, hash - 5);
        const auto it = texts.find(alias);
        if (it == texts.end() || !it->second) {
            return nullptr;
        }
        const auto lit = it->second->find(ref.substr(hash + 1));
        return lit == it->second->end()
                   ? nullptr
                   : std::shared_ptr<const std::string>(it->second, &lit->second);
    }

    // Mirrors AcceptMsbt's `m.texts[alias] = r.texts;` (mod_nx_runtime.cpp) -- a plain
    // reassignment of the whole per-alias label map. `insert_if_absent_only = true` models "first
    // ever resolution of this alias" (TEST_CASE 1's usage: never replaces, so it never exercises
    // the hazard TEST_CASE 2 exists to isolate); false models the re-resolution/language-switch
    // case (the `serial`/"bumped on a language switch" field right next to `texts` in the real
    // MsbtState says this path is real, not invented).
    void Land(const std::string& alias, std::shared_ptr<const MsbtLabelMap> fresh,
              bool insert_if_absent_only) {
        std::scoped_lock lock{mutex};
        if (insert_if_absent_only && texts.contains(alias)) {
            return;
        }
        texts[alias] = std::move(fresh);
    }
};

// -------------------------------------------------------------------------------------------
// The single-slot coalescing mailbox: mirrors redraw_job_mutex/redraw_job_cv/redraw_pending_job
// (mod_runtime.h) -- "coalescing, not queueing": a not-yet-picked-up job is REPLACED, never
// queued, so the worker can never fall permanently behind. `RedrawJob` itself mirrors
// mod_runtime.h's own struct: every field an OWNED value, in particular `snapshot` an owned
// StateSnapshot COPY, never a reference into a live tick_snapshot (a reused snapshot would be
// written by the tick thread while the worker reads it) -- exercised for real, under contention,
// here.
struct RedrawJob {
    u64 generation{};
    Page page_copy;
    StateSnapshot snapshot;
    ViewState views;
    // Mirrors the real RedrawJob::gpu_composite/draw_list (mod_runtime.h), used when
    // GPU_COMPOSITE pages dispatch to the worker. `draw_list` is job-owned, exactly like the real
    // one -- RenderPage fills it during the draw thread's own call, never touching a
    // shared/tick-owned instance.
    bool gpu_composite{false};
    AuxDrawList draw_list;
};

class Mailbox {
public:
    void Dispatch(RedrawJob job) {
        std::scoped_lock lock{mutex_};
        job.generation = ++next_generation_;
        pending_ = std::move(job); // coalesces: replaces whatever wasn't picked up yet
        cv_.notify_one();
    }
    // Blocks until a job is available or Stop() was called; nullopt means "stop, nothing left".
    std::optional<RedrawJob> WaitTake() {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [&] { return pending_.has_value() || stop_; });
        if (!pending_) {
            return std::nullopt;
        }
        RedrawJob job = std::move(*pending_);
        pending_.reset();
        return job;
    }
    void Stop() {
        std::scoped_lock lock{mutex_};
        stop_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<RedrawJob> pending_;
    u64 next_generation_{0};
    bool stop_{false};
};

// -------------------------------------------------------------------------------------------
// Shared manifest/page fixtures. A geometry-mode area (no `image`, so MarkVisitedAt/report_visit
// is live), follow_window > 0 (so the glide-easing mid-draw mutation of map_follow_state is live
// on every call), pan_zoom + clamp_view (so the pan-correction write is live on a realistic
// fraction of calls), one marker (exercises is_visited + the icon atlas fetch), one msbt-sourced
// label on both the map area AND a plain Label widget (exercises the TextProvider path from two
// different call sites: the map's labels in mod_ui_map_widget.cpp and DrawLabel in mod_ui.cpp).

Manifest BuildManifest() {
    Manifest m;
    m.icon_atlas = "harness:icon_atlas";
    m.icon_cell = 16;
    m.icon_cells.emplace("Pickup", std::pair{0, 0});
    m.map_style.raster_px = 64; // kept small: this harness's own images() ignores it anyway

    MapArea area;
    area.min_x = 0.0f;
    area.min_y = 0.0f;
    area.max_x = 600.0f;
    area.max_y = 280.0f;
    area.follow_pad = 80.0f;
    area.clamp_view = true;

    MapMarker marker;
    marker.kind = "Pickup";
    marker.icon = "Pickup";
    marker.x = 100.0f;
    marker.y = 100.0f;
    area.markers.push_back(marker);

    MapLabel label;
    label.text_src = "msbt:companion#greeting";
    label.x = 50.0f;
    label.y = 50.0f;
    area.labels.push_back(label);

    m.map_areas.emplace("overworld", std::move(area));
    return m;
}

Page BuildPage() {
    Page page;
    page.id = "harness_page";

    Widget map;
    map.type = WidgetType::Map;
    map.id = "mainmap";
    map.rect = {0, 0, 360, 220};
    map.area = "overworld";
    map.marker_x_bind = "player.x";
    map.marker_y_bind = "player.y";
    map.follow_window = 180.0f;
    map.pan_zoom = true;
    map.min_zoom = 1.0f;
    map.max_zoom = 3.0f;
    page.widgets.push_back(map);

    Widget label;
    label.type = WidgetType::Label;
    label.rect = {0, 220, 360, 32};
    label.text_src = "msbt:companion#greeting";
    label.text_scale = 3;
    page.widgets.push_back(label);

    // A static Image widget -- no bind, fixed rect, fixed src/color -- drawn by both of this
    // harness's concurrent RenderPage callers (DrawThread's worker-shaped call and TickThread's
    // own synchronous carve-out call). Placed at a rect disjoint from the Map widget above so it
    // has nothing to do with Map-widget occlusion.
    Widget chrome;
    chrome.type = WidgetType::Image;
    chrome.id = "chrome";
    chrome.rect = {0, 260, 360, 40};
    chrome.src = "harness_chrome_tex";
    chrome.color = 0xFFFFFFFFu;
    page.widgets.push_back(chrome);
    return page;
}

// One tick's worth of published state, matching StateSnapshot's own reuse pattern in the real
// code: clear + refill every call (ModRuntime::Tick's `snapshot.ints.clear(); ...;
// SampleState(snapshot);`). The player position walks a small ellipse so report_visit/is_visited
// and the follow-glide target genuinely change every call, not just once.
StateSnapshot BuildSnapshot(u64 tick) {
    StateSnapshot s;
    s.tick = tick;
    const float t = static_cast<float>(tick) * 0.13f;
    s.floats["player.x"] = 100.0f + std::sin(t) * 60.0f;
    s.floats["player.y"] = 100.0f + std::cos(t) * 40.0f;
    s.ints["frame_parity"] = static_cast<s64>(tick % 2);
    s.texts["room"] = "HarnessRoom";
    return s;
}

// A per-call view, varied so the pan-clamp correction write (GeometryMapDraw::FrameView's
// pan_zoom + clamp_view block, mod_ui_map_widget.cpp) fires on a realistic fraction of calls
// instead of never (a single fixed view would only ever exercise one branch of that code).
ViewState BuildViews(u64 tick) {
    ViewState v;
    ViewTransform t;
    t.zoom = 1.0f + 0.5f * static_cast<float>(tick % 5); // 1.0 .. 3.0
    t.pan_x = static_cast<float>(tick % 7) * 15.0f - 45.0f;
    t.pan_y = static_cast<float>(tick % 11) * 8.0f - 40.0f;
    v["mainmap"] = t;
    return v;
}

// -------------------------------------------------------------------------------------------
// Everything two threads share in TEST_CASE 1 and 2. TEST_CASE 3 builds its own separate pair of
// fixtures (see below) so its deliberately-unlocked race stays isolated from these.
struct RaceFixture {
    AssetCacheFixture assets;
    MapStateFixture map_state;
    MsbtFixture msbt;
    std::mutex view_mutex;                // mirrors ModRuntime::view_mutex, reused for follow_state
    MapFollowState map_follow_state;      // mirrors ModRuntime::map_follow_state
    mutable std::mutex map_records_mutex; // mirrors ModRuntime::map_records_mutex (also mutable)
    MapDrawRecords map_draw_records_published;
    Mailbox mailbox;
    std::atomic<u64> draw_iterations{0};
    std::atomic<u64> tick_iterations{0};
    std::atomic<u64> sync_carveout_calls{0};

    void PublishRecords(MapDrawRecords records) {
        std::scoped_lock lock{map_records_mutex};
        map_draw_records_published = std::move(records);
    }
    MapDrawRecords ReadPublishedRecords() const {
        std::scoped_lock lock{map_records_mutex};
        return map_draw_records_published;
    }
};

// One RenderPage call, wired exactly like ModRuntime::RenderPageTo wires it (mod_redraw.cpp),
// against `fixture`'s shared, lock-guarded state. Used identically from BOTH the draw thread (the
// worker's own RunRedrawJob-equivalent) and the tick thread (the GPU_COMPOSITE/debug-page/
// page-transition synchronous carve-out, which stays on the tick thread even with the redraw
// worker running) -- calling this from two real threads concurrently
// against the SAME fixture is this harness's sharpest test.
bool RunOneRenderPage(RaceFixture& fixture, const Manifest& manifest, const Page& page,
                      const StateSnapshot& snapshot, const ViewState& views,
                      MapDrawRecords& out_records, AuxDrawList* dl = nullptr) {
    Canvas canvas;
    canvas.Resize(360, 260);
    bool animating = false;
    out_records.clear();

    const ImageProvider images = [&](const std::string& key) {
        return SimGetImage(fixture.assets, key);
    };
    const VisitReporter report_visit = [&](const std::string& area, float wx, float wy) {
        fixture.map_state.MarkVisitedAt(area, wx, wy);
    };
    const VisitedQuery is_visited = [&](const std::string& area, float wx, float wy) {
        return fixture.map_state.IsVisited(area, wx, wy);
    };
    const TextProvider texts = [&](const std::string& ref) { return fixture.msbt.Resolve(ref); };

    // manifest_markers_mutex reuses fixture.map_state.mutex, exactly mirroring production
    // (ModRuntime::RenderPageTo/RunRedrawJob both pass &map_state_mutex) -- see mod_ui.h's
    // RenderPage declaration comment.
    // `dl` defaults to nullptr (canvas mode), but a caller passing a non-null, job-owned
    // AuxDrawList exercises the `draw_list != nullptr` branches of the Map widget's draw (DrawMap,
    // mod_ui_map_widget.cpp), which production reaches from a second real thread (the worker).
    return RenderPage(canvas, manifest, page, snapshot, images, views, &fixture.map_follow_state,
                      report_visit, is_visited, dl, &animating, texts, &out_records,
                      /*extras=*/nullptr, &fixture.view_mutex, &fixture.map_state.mutex);
}

// The tick thread's other real duties besides dispatching a job: PumpNxAssets-shaped asset
// landing, AcceptModuleMapState-shaped map writes, ApplyViewCorrections-shaped follow_state
// consumption, DrainTaps-shaped published-records reads -- all against the SAME fixtures the draw
// thread's RenderPage call touches, every "tick".
void RunTickSideDuties(RaceFixture& fixture, u64 tick) {
    // PumpNxAssets-shaped: land a couple of freshly "decoded" images, occasionally evict -- the
    // real concurrent-writer pattern (GetImage on-demand-rasterise vs. PumpNxAssets-landed, both
    // reachable from either thread with the redraw worker running).
    fixture.assets.Put(
        "tick-landed:" + std::to_string(tick % 8),
        std::make_shared<Image>(Image{2, 2, {0xFFAABBCCu, 0xFFAABBCCu, 0xFFAABBCCu, 0xFFAABBCCu}}));
    if (tick % 37 == 0) {
        fixture.assets.EraseSome(24);
    }

    // AcceptModuleMapState-shaped: a module callback writing map state during sampling.
    fixture.map_state.ModuleWrite("overworld", tick);

    // msbt landing: only ever INSERTS a new alias here (see the file header -- TEST_CASE 1
    // exercises general state_mutex traffic; TEST_CASE 2 alone stresses the reassignment hazard).
    if (tick % 5 == 0) {
        auto labels = std::make_shared<MsbtLabelMap>(MsbtLabelMap{{"greeting", "Hello!"}});
        fixture.msbt.Land("companion", labels, /*insert_if_absent_only=*/true);
    }

    // ApplyViewCorrections-shaped: consume (read + erase) whatever the draw thread's pan-clamp
    // write left in map_follow_state, under the SAME lock RenderPage's Map widget case uses.
    {
        std::scoped_lock lock{fixture.view_mutex};
        for (auto it = fixture.map_follow_state.begin(); it != fixture.map_follow_state.end();) {
            if (it->first.ends_with("#pan")) {
                it = fixture.map_follow_state.erase(it);
            } else {
                ++it;
            }
        }
    }

    // DrainTaps-shaped: read the published, one-tick-delayed map_draw_records copy.
    const MapDrawRecords records = fixture.ReadPublishedRecords();
    (void)records;
}

} // namespace

TEST_CASE("DSMod redraw worker: locking pattern holds under two real concurrent threads",
          "[dsmod][tsan][redraw-worker]") {
    RaceFixture fixture;
    const Manifest manifest = BuildManifest();
    const Page page = BuildPage();
    const auto deadline = std::chrono::steady_clock::now() + HarnessDuration();

    std::thread draw_thread([&] {
        while (true) {
            auto job = fixture.mailbox.WaitTake();
            if (!job) {
                break;
            }
            MapDrawRecords records;
            // A job dispatched with gpu_composite=true passes its OWN job->draw_list (job-owned,
            // exactly mirroring the real RedrawJob::draw_list) -- this is the worker thread
            // reaching RenderPage's `draw_list != nullptr` branches, concurrently with the tick
            // thread's own duties below (RunTickSideDuties, and the unrelated debug/transition
            // carve-out a few lines down, which stays draw_list=nullptr,
            // matching production: a GPU_COMPOSITE page's own carve-out only fires when
            // EDEN_DSMOD_SYNC_REDRAW forces sync, which is single-threaded by construction and not
            // this harness's concern).
            RunOneRenderPage(fixture, manifest, job->page_copy, job->snapshot, job->views, records,
                             job->gpu_composite ? &job->draw_list : nullptr);
            fixture.PublishRecords(std::move(records));
            fixture.draw_iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread tick_thread([&] {
        u64 tick = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ++tick;
            RedrawJob job;
            job.page_copy = page;               // owned copy, mirrors RedrawJob::page_copy
            job.snapshot = BuildSnapshot(tick); // owned copy, mirrors RedrawJob::snapshot
            job.views = BuildViews(tick);
            // Alternate composite/canvas mode across dispatches so both the canvas path
            // (dl == nullptr) and the GPU_COMPOSITE path run under the SAME two-thread contention
            // this routine check already stresses.
            job.gpu_composite = (tick % 2) == 1;
            fixture.mailbox.Dispatch(std::move(job));

            RunTickSideDuties(fixture, tick);

            // The GPU_COMPOSITE/debug-page/page-transition synchronous carve-out: a fraction of
            // ticks, the tick thread ALSO calls RenderPage directly, concurrently with whatever
            // the draw thread is mid-rendering. See the file header.
            if (tick % 3 == 0) {
                MapDrawRecords records;
                RunOneRenderPage(fixture, manifest, page, BuildSnapshot(tick), BuildViews(tick),
                                 records);
                fixture.PublishRecords(std::move(records));
                fixture.sync_carveout_calls.fetch_add(1, std::memory_order_relaxed);
            }
            fixture.tick_iterations.fetch_add(1, std::memory_order_relaxed);
        }
        fixture.mailbox.Stop();
    });

    tick_thread.join();
    draw_thread.join();

    INFO("tick iterations: " << fixture.tick_iterations.load());
    INFO("draw iterations: " << fixture.draw_iterations.load());
    INFO("synchronous carve-out calls: " << fixture.sync_carveout_calls.load());
    REQUIRE(fixture.tick_iterations.load() > 0);
    REQUIRE(fixture.draw_iterations.load() > 0);
}

TEST_CASE("DSMod redraw worker: msbt text pointer lifetime under concurrent re-resolution",
          "[dsmod][tsan][redraw-worker][msbt-pointer]") {
    // Isolated probe: GetMsbtText/TextProvider used to return a raw `const std::string*` into a
    // shared_ptr-owned label map (mod_nx_runtime.cpp), past the lock that protects the map itself
    // -- unlike image_cache, which holds shared_ptr specifically so a caller's handle survives the
    // cache's own eviction. That crashed with SIGSEGV in well under a second. The fix is the same
    // as image_cache's: TextProvider returns shared_ptr<const std::string>, aliased onto the label
    // map's own
    // shared_ptr (mod_nx_runtime.cpp's GetMsbtText, mirrored here by MsbtFixture::Resolve). This
    // test is a regression guard now -- expected clean, not a SIGSEGV.
    RaceFixture fixture;
    const Manifest manifest = BuildManifest();
    const Page page = BuildPage();
    fixture.msbt.Land("companion",
                      std::make_shared<MsbtLabelMap>(MsbtLabelMap{{"greeting", "Hello!"}}),
                      /*insert_if_absent_only=*/false);

    const auto deadline = std::chrono::steady_clock::now() + HarnessDuration();
    std::atomic<u64> reads{0};
    std::atomic<u64> lands{0};

    std::thread reader([&] {
        u64 tick = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ++tick;
            MapDrawRecords records;
            RunOneRenderPage(fixture, manifest, page, BuildSnapshot(tick), BuildViews(tick),
                             records);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread lander([&] {
        u64 i = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ++i;
            // Simulates a locale re-resolution landing a FRESH decode for the SAME alias --
            // AcceptMsbt's `m.texts[r.alias] = r.texts;` (mod_nx_runtime.cpp), exactly this
            // reassignment shape. A fresh string value each time also means a stale read would show
            // garbled text, not just "old but valid" -- easier to eyeball without TSan if this ever
            // needs debugging by hand.
            auto labels = std::make_shared<MsbtLabelMap>(
                MsbtLabelMap{{"greeting", "Hello #" + std::to_string(i) + "!"}});
            fixture.msbt.Land("companion", std::move(labels), /*insert_if_absent_only=*/false);
            lands.fetch_add(1, std::memory_order_relaxed);
        }
    });

    reader.join();
    lander.join();
    INFO("reads: " << reads.load());
    INFO("lands: " << lands.load());
    REQUIRE(reads.load() > 0);
    REQUIRE(lands.load() > 0);
}

TEST_CASE("DSMod redraw worker: manifest marker flags read live outside the snapshot",
          "[dsmod][tsan][redraw-worker][known-gap]") {
    // manifest.map_areas[...].markers[...].hidden/.opened/.collected/.unveiled/.veiled are read
    // live off the Manifest (not through StateSnapshot) inside the Map widget's marker-visibility
    // checks, while UpdateHiddenMarkers (a module callback, tick-thread today) writes them --
    // a real race while both sides were unlocked.
    // The fix: RenderPage takes manifest_markers_mutex (reusing map_state_mutex, the
    // SAME lock UpdateHiddenMarkers already held for its own whole body) and locks it around the
    // marker-read loop -- see mod_ui.h's RenderPage comment, GeometryMapDraw::DrawAtlasLayer's
    // marker passes (mod_ui_map_widget.cpp), and UpdateHiddenMarkers's own comment (mod_map.cpp).
    // This test mirrors that shape (writer locks the same mutex RunOneRenderPage's reader takes)
    // and is EXPECTED to be CLEAN -- a regression guard, not a known-gap reproduction. Tag kept as
    // [known-gap] for command-line compatibility.
    Manifest manifest = BuildManifest();
    const Page page = BuildPage();
    RaceFixture fixture; // assets/map_state/msbt/view_mutex/records stay properly locked here;
                         // marker flags now share map_state's own mutex (fixture.map_state.mutex)
                         // via RunOneRenderPage's manifest_markers_mutex argument.
    MapMarker& marker = manifest.map_areas.at("overworld").markers.front();

    const auto deadline = std::chrono::steady_clock::now() + HarnessDuration();
    std::atomic<u64> reads{0};
    std::atomic<u64> writes{0};

    std::thread reader([&] {
        u64 tick = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ++tick;
            MapDrawRecords records;
            RunOneRenderPage(fixture, manifest, page, BuildSnapshot(tick), BuildViews(tick),
                             records);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread writer([&] {
        u64 i = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ++i;
            // UpdateHiddenMarkers-shaped, now under the SAME lock the real (fixed) function holds
            // for its whole body -- see mod_map.cpp's UpdateHiddenMarkers.
            std::scoped_lock lock{fixture.map_state.mutex};
            marker.hidden = (i % 2) == 0;
            marker.opened = (i % 3) == 0;
            marker.collected = (i % 5) == 0;
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    reader.join();
    writer.join();
    INFO("reads: " << reads.load());
    INFO("writes: " << writes.load());
    REQUIRE(reads.load() > 0);
    REQUIRE(writes.load() > 0);
}

// -------------------------------------------------------------------------------------------
// TEST_CASE 4: a backlogged worker job must not publish over a page transition's
// already-more-current output.
//
// TEST_CASE 1-3 above all drive RenderPage's own locking; none of them ever exercise the PUBLISH
// side of the redraw worker -- DispatchRedraw's mailbox, dispatch_page/canvas_page continuity, or
// RunRedrawJob's decision to publish -- because that logic lives in ModRuntime itself, which this
// boot-free harness deliberately does not include (no System/guest-CPU dependency). So this
// TEST_CASE cannot call the real DispatchRedraw/RunRedrawJob/DrivePageTransition -- it is a
// hand-built, deliberately narrow mirror of ONLY the decision logic those functions use (a shared
// "display generation" counter, bumped by both an ordinary dispatch and a page transition taking
// over; a worker that checks it before publishing). A regression in the REAL mod_redraw.cpp logic
// this mirrors would not automatically fail this test; closing that gap needs RunRedrawJob's
// actual generation check pulled into something callable from here, without pulling in the rest
// of ModRuntime.
//
// The race modelled: a worker job dispatched for page A is already dequeued (in flight, no longer
// in the mailbox, so nothing can coalesce against it) when the tick thread starts a PAGE TRANSITION
// to page B -- DrivePageTransition never touches the dispatch mailbox at all, so that in-flight job
// has nothing stopping it from publishing page A's stale pixels right over page B's already-shown
// ones once it finishes. TSAN_HARNESS_DISABLE_GENERATION_GUARD=1 runs the SAME test with the guard
// compiled out (the unguarded shape) -- expected to FAIL (confirms the scenario is genuinely
// reachable and the guard is load-bearing, not vacuous); unset (default) mirrors shipped production
// and is expected to stay clean.
namespace {

// Mirrors AuxRouting's single publish target for this test's purposes: whichever call lands last
// wins, same as the real aux.PublishUi/PublishUiPartial/PublishUiWith all writing into the same
// ui_pixels under one mutex. `violations` counts a publish whose generation is OLDER than one
// already recorded -- exactly what a backlogged worker job overwriting a transition's output would
// produce.
struct PublishSink {
    std::mutex mutex;
    u64 highest_generation_seen{0};
    int violations{0};
    int publishes{0};

    // Unconditional publish -- mirrors DrivePageTransition's own aux.PublishUiWith call, which
    // always runs once a transition tick decides to publish (no generation gate on that side; a
    // transition IS the tick thread taking authority back, by construction).
    void Publish(int page, u64 generation) {
        std::scoped_lock lock{mutex};
        Record(page, generation);
    }

    // Gated publish -- mirrors RunRedrawJob's now-guarded publish. Re-checks `generation_source`
    // under THIS lock (not just the worker's own earlier, unlocked check) so the test's own
    // pass/fail is deterministic rather than depending on the same few-instruction TOCTOU gap
    // production's own fix narrows but does not fully close (see mod_redraw.cpp's RunRedrawJob
    // comment on the staleness check) -- that gap
    // is production's to accept as a bounded, narrow, already-existing-class-of-tolerance risk; it
    // should not also make THIS regression test flaky.
    void TryPublish(int page, u64 generation, const std::atomic<u64>& generation_source,
                    bool disable_guard, std::atomic<u64>& suppressed_counter) {
        std::scoped_lock lock{mutex};
        if (!disable_guard && generation != generation_source.load(std::memory_order_relaxed)) {
            suppressed_counter.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Record(page, generation);
    }

private:
    void Record(int page, u64 generation) {
        (void)page;
        if (generation < highest_generation_seen) {
            ++violations;
        } else {
            highest_generation_seen = generation;
        }
        ++publishes;
    }
};

struct TransitionJob {
    int page{};
    u64 generation{};
};

// Single-slot coalescing mailbox, same "replace whatever wasn't picked up yet" shape as Mailbox
// above (TEST_CASE 1-3's own) -- kept as a separate type so this test cannot perturb their
// RedrawJob/Mailbox.
class TransitionMailbox {
public:
    void Dispatch(TransitionJob job) {
        std::scoped_lock lock{mutex_};
        pending_ = job;
        cv_.notify_one();
    }
    std::optional<TransitionJob> WaitTake() {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [&] { return pending_.has_value() || stop_; });
        if (!pending_) {
            return std::nullopt;
        }
        TransitionJob job = *pending_;
        pending_.reset();
        return job;
    }
    void Stop() {
        std::scoped_lock lock{mutex_};
        stop_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<TransitionJob> pending_;
    bool stop_{false};
};

} // namespace

TEST_CASE("DSMod redraw worker: a backlogged worker job must not publish over a page transition",
          "[dsmod][tsan][redraw-worker][transition-publish]") {
    std::atomic<u64> redraw_dispatch_generation{0}; // mirrors ModRuntime's own
    PublishSink sink;
    TransitionMailbox mailbox;
    const bool disable_guard = std::getenv("TSAN_HARNESS_DISABLE_GENERATION_GUARD") != nullptr;

    std::atomic<u64> worker_iterations{0};
    std::atomic<u64> transition_count{0};
    std::atomic<u64> suppressed_stale{0};

    const auto deadline = std::chrono::steady_clock::now() + HarnessDuration();

    // WORKER thread: mirrors RedrawWorkerMain + RunRedrawJob's tail (mod_redraw.cpp).
    std::thread worker([&] {
        while (true) {
            auto job = mailbox.WaitTake();
            if (!job) {
                break;
            }
            // Simulate real render latency (RunRedrawJob's own RenderPage call measured
            // ~3.5-4.7ms on a real page) so the race window opens deterministically
            // instead of depending on scheduler luck, same spirit as the rest of this harness
            // forcing contention rather than hoping for it.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            sink.TryPublish(job->page, job->generation, redraw_dispatch_generation, disable_guard,
                            suppressed_stale);
            worker_iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // TICK thread: mirrors PublishUi's ordinary dispatch branch AND DrivePageTransition's
    // completion path (both bump the same generation counter).
    std::thread tick([&] {
        int page = 0;
        u64 i = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ++i;
            // Ordinary dispatch tick (DispatchRedraw, mod_redraw.cpp): bump generation, hand off a
            // job for the CURRENT page, never blocking -- exactly DispatchRedraw's own contract.
            const u64 gen = redraw_dispatch_generation.fetch_add(1, std::memory_order_relaxed) + 1;
            mailbox.Dispatch(TransitionJob{page, gen});

            // Every few ticks, a page transition begins immediately after that dispatch -- the
            // exact interleaving that leaves a job in flight with nothing left to coalesce against
            // (DrivePageTransition never touches the mailbox). Mirrors DrivePageTransition's
            // fresh-transition-start completion (mod_pages.cpp; its dispatch_page + generation
            // bump) publishing directly, same as its own aux.PublishUiWith call.
            if (i % 4 == 0) {
                page = 1 - page; // standby<->map / map<->gear: switches to the OTHER page
                const u64 transition_gen =
                    redraw_dispatch_generation.fetch_add(1, std::memory_order_relaxed) + 1;
                sink.Publish(page, transition_gen);
                transition_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
        mailbox.Stop();
    });

    tick.join();
    worker.join();

    INFO("worker iterations: " << worker_iterations.load());
    INFO("transitions: " << transition_count.load());
    INFO("stale jobs the guard suppressed: " << suppressed_stale.load());
    INFO("publish violations (a stale publish landed after a newer one already showed): "
         << sink.violations);
    INFO("guard " << (disable_guard
                          ? "DISABLED (TSAN_HARNESS_DISABLE_GENERATION_GUARD set) -- demonstrating "
                            "the PRE-FIX shape; a violation here is EXPECTED, confirming the "
                            "scenario is real"
                          : "enabled (default) -- matches shipped, fixed production"));

    REQUIRE(worker_iterations.load() > 0);
    REQUIRE(transition_count.load() > 0);
    if (!disable_guard) {
        // Confirms the race is genuinely reachable with this interleaving (not a vacuous test that
        // would pass even if the guard were deleted) -- with the guard's own env-var escape hatch
        // disabled, this same scenario is expected to produce violations instead (see above).
        REQUIRE(suppressed_stale.load() > 0);
    }
    REQUIRE(sink.violations == 0);
}

// Coverage for a real device regression -- "cannot tap the map to place or select a pin". Not a
// race (TSan would never have caught this; it is a plain, deterministic logic bug), but this
// file's own RenderPage-against-real-fixtures machinery is exactly the right, cheapest home for
// it: no ModRuntime, no guest, seconds not minutes, and it calls the REAL RenderPage function
// production links against.
//
// The bug had two independent halves, both fixed:
//   1. RenderPage itself (mod_ui.cpp) used to clear its caller's MapDrawRecords container
//      unconditionally at the top of EVERY call -- including a genuinely partial redraw (a real,
//      non-whole-canvas `extras->clip`) whose dirty rect has nothing to do with the Map widget at
//      all. The per-widget bbox pre-reject further down RenderPage then `continue`s past the Map
//      widget without ever reaching the code that re-adds its record, so `*map_records` (and, one
//      publish later, `map_draw_records_published`) ends up with ZERO records for a map that is
//      still fully visible and completely unchanged on screen. Reproduced live: opening Link's
//      Awakening's own pin panel (a "pin_pane" widget-group animation whose box sits entirely to
//      the right of the Map widget's own rect) was enough to wipe every map tap's hit-testing data
//      on the very next partial redraw -- this test's own fixture reproduces that exact shape with
//      the harness's pre-existing "chrome" Image widget (BuildPage(), disjoint from the Map
//      widget's rect) standing in for the pin panel.
//   2. `RunRedrawJob` (mod_redraw.cpp, the worker's own real caller, not reachable from this
//      boot-free harness -- see below) used to hand RenderPage a FRESH `MapDrawRecords records;`
//      local on every single call instead of a persistent, cross-job buffer, so even a fixed
//      RenderPage had nothing to preserve: an empty container staying empty is not a bug in
//      RenderPage's own clearing policy. Fixed by giving the worker its own persistent
//      `worker_map_draw_records` member (mod_runtime.h), mirroring the exact precedent
//      `worker_canvas` right next to it already established for pixels. This half is production
//      wiring this harness cannot reach directly (it drives RenderPage standalone, not through
//      ModRuntime/RunRedrawJob) -- what IS directly tested below (repeated RenderPage calls
//      against the SAME caller-owned buffer, never reset by hand in between) is the exact contract
//      `worker_map_draw_records` now relies on, so a regression in RenderPage's own half of the
//      fix would fail here regardless of which caller (tick-thread sync path or worker) it was
//      reached from.
TEST_CASE("DSMod redraw worker: a persistent MapDrawRecords buffer must survive a partial "
          "redraw that does not touch the Map widget",
          "[dsmod][map-records-persist]") {
    const Manifest manifest = BuildManifest();
    const Page page = BuildPage(); // widget 0: Map {0,0,360,220}; widget 2 "chrome": Image
                                   // {0,260,360,40} -- disjoint from the map, exactly like the
                                   // pin panel's own box in production.
    RaceFixture fixture; // reused purely for its ready-made view_mutex/map_state/msbt/assets --
                         // this test is single-threaded, no race under test here.

    const auto images = [&](const std::string& k) { return SimGetImage(fixture.assets, k); };
    const auto report_visit = [&](const std::string& a, float x, float y) {
        fixture.map_state.MarkVisitedAt(a, x, y);
    };
    const auto is_visited = [&](const std::string& a, float x, float y) {
        return fixture.map_state.IsVisited(a, x, y);
    };
    const auto texts = [&](const std::string& r) { return fixture.msbt.Resolve(r); };
    // One call, reused for every step below: caller-owned Canvas + a `RenderExtras*` that is
    // either nullptr (full redraw) or a real, narrow clip (partial) -- `records` is passed BY
    // REFERENCE and never manually cleared by this test, exactly mirroring a persistent
    // ModRuntime member (map_draw_records / worker_map_draw_records) rather than RunOneRenderPage's
    // own per-call-fresh local a few hundred lines up (which would make this whole test vacuous).
    const auto render = [&](u64 tick, RenderExtras* extras, MapDrawRecords& records) {
        Canvas canvas;
        canvas.Resize(360, 300);
        bool animating = false;
        return RenderPage(canvas, manifest, page, BuildSnapshot(tick), images, BuildViews(tick),
                          &fixture.map_follow_state, report_visit, is_visited,
                          /*draw_list=*/nullptr, &animating, texts, &records, extras,
                          &fixture.view_mutex, &fixture.map_state.mutex);
    };

    MapDrawRecords records;

    // 1. A full redraw (extras == nullptr, the {0,0,0,0}-clip "whole canvas" sentinel's other
    //    spelling): must populate the Map widget's own record.
    REQUIRE(render(0, /*extras=*/nullptr, records));
    REQUIRE(records.size() == 1);
    CHECK(records[0].widget_index == 0);
    CHECK(records[0].page_id == page.id);

    // 2. Five genuinely partial redraws in a row, each clipped to ONLY the "chrome" widget's own
    //    rect {0,260,360,40} -- entirely outside the Map widget's rect {0,0,360,220} -- modelling
    //    the pin panel's repeated re-renders while it settles (the "target redrawn Nx" pattern).
    //    THE regression, exactly as reproduced live: before the fix,
    //    `records` is cleared back to empty on iteration 0 and never refilled (the Map widget is
    //    outside this pass's clip, so it is skipped before ever reaching the code that would
    //    re-add it) -- every map tap in between would report "the map is not drawn yet".
    for (int i = 0; i < 5; ++i) {
        RenderExtras extras;
        extras.clip = {0, 260, 360, 40};
        REQUIRE(render(static_cast<u64>(i) + 1, &extras, records));
        INFO("after partial redraw #" << i << " (clip disjoint from the map)");
        REQUIRE(records.size() == 1); // the actual regression check
        CHECK(records[0].widget_index == 0);
    }

    // 3. A full redraw again must still work AND must not have accumulated a duplicate alongside
    //    the survivor from step 1/2 (the Map widget IS inside its own dirty rect on a full redraw,
    //    so it refreshes its own entry -- the emplace-site `erase_if`, GeometryMapDraw::BeginRecord
    //    in mod_ui_map_widget.cpp -- rather than appending a second one next to it).
    REQUIRE(render(99, /*extras=*/nullptr, records));
    REQUIRE(records.size() == 1);

    // 4. A Map widget that becomes HIDDEN on a partial pass that DOES cover it must not leave its
    //    previous record behind as a stale, still-tappable target. Gated via `need_bind` (hidden
    //    unless the bound int is non-zero) on a page-local copy so TEST_CASE 1-4's own shared
    //    BuildPage() fixture stays untouched.
    Page hideable_page = page;
    hideable_page.widgets[0].need_bind = "map_shown";
    MapDrawRecords hide_records;
    StateSnapshot shown = BuildSnapshot(200);
    shown.ints["map_shown"] = 1;
    {
        Canvas canvas;
        canvas.Resize(360, 300);
        bool animating = false;
        REQUIRE(RenderPage(canvas, manifest, hideable_page, shown, images, BuildViews(200),
                           &fixture.map_follow_state, report_visit, is_visited, nullptr, &animating,
                           texts, &hide_records, /*extras=*/nullptr, &fixture.view_mutex,
                           &fixture.map_state.mutex));
    }
    REQUIRE(hide_records.size() == 1);
    StateSnapshot hidden = BuildSnapshot(201);
    hidden.ints["map_shown"] = 0;
    RenderExtras whole_map_clip;
    whole_map_clip.clip = {0, 0, 360, 220}; // covers the Map widget's own rect: reaches the
                                            // `hidden` continue, not the bbox pre-reject above it
    {
        Canvas canvas;
        canvas.Resize(360, 300);
        bool animating = false;
        REQUIRE(RenderPage(canvas, manifest, hideable_page, hidden, images, BuildViews(201),
                           &fixture.map_follow_state, report_visit, is_visited, nullptr, &animating,
                           texts, &hide_records, &whole_map_clip, &fixture.view_mutex,
                           &fixture.map_state.mutex));
    }
    CHECK(hide_records.empty()); // the just-hidden widget's stale record must not survive
}
