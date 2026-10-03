// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Paged game-font atlases (runtime 17). A font whose metrics set FontMetrics::page_h keeps its
// glyphs in a virtual atlas of pages stacked top to bottom (glyph y / page_h = the page, y %
// page_h = the row inside it). The pages are loaded on demand, one at a time, on a worker thread,
// and held in an LRU bounded by a byte budget, so a large CJK font never has to be one huge
// image: a page is only loaded once text needs one of its glyphs.
//
// Get() never blocks: a page that is not resident is queued and nullptr returned (the glyph
// draws blank); when it lands TakeLanded() turns true once and the page is redrawn. A page used
// within the last ProtectMs is never evicted for another one: when the budget cannot hold a new
// page without evicting a page in use, the load is refused (retried after RetryMs), so a frame
// needing more pages than fit never loops loading and evicting.
// Threads: Get / TakeLanded / the counters from any thread (one mutex); the loader runs on the
// worker (or inline for a synchronous instance, used by tests).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/common_types.h"
#include "core/mods/mod_ui.h"

namespace Core::Mods {

class FontPages {
public:
    /// Loads one page; false = it could not be read now (retried later).
    using Loader = std::function<bool(u32 page, Image& out)>;
    static constexpr size_t DefaultBudget = size_t{32} << 20; ///< resident pages, bytes
    static constexpr u32 MaxPages = 4096;
    static constexpr s64 ProtectMs = 250;
    static constexpr s64 RetryMs = 1000;
    static constexpr u32 MaxAttempts = 5;

    /// `synchronous`: Get() loads a missing page inline instead of on the worker. `protect_ms`:
    /// how long after its last use a page is safe from eviction.
    explicit FontPages(Loader loader, size_t budget = DefaultBudget, bool synchronous = false,
                       s64 protect_ms = ProtectMs);
    ~FontPages();
    FontPages(const FontPages&) = delete;
    FontPages& operator=(const FontPages&) = delete;

    /// Stops the worker and drops the loader (no load runs after this returns); pages already
    /// resident stay usable. For the owner's teardown, while copies may still be held.
    void Shutdown();
    /// The page if resident (marked used now), else nullptr after queueing its load.
    std::shared_ptr<const Image> Get(u32 page);
    /// Whether a page landed since the last call.
    bool TakeLanded() {
        return landed.exchange(false);
    }

    [[nodiscard]] size_t ResidentBytes() const;
    [[nodiscard]] size_t ResidentPages() const;
    [[nodiscard]] bool Resident(u32 page) const;
    [[nodiscard]] u64 Loads() const {
        return loads.load();
    }
    [[nodiscard]] u64 Evictions() const {
        return evictions.load();
    }
    [[nodiscard]] u64 Refusals() const {
        return refusals.load();
    }
    [[nodiscard]] size_t Budget() const {
        return budget;
    }

private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        std::shared_ptr<const Image> image;
        size_t bytes{};
        Clock::time_point used;
        std::list<u32>::iterator lru;
    };
    struct Failure {
        Clock::time_point at;
        u32 attempts{};
    };

    void Load(u32 page);
    /// Inserts a loaded page, evicting idle pages to fit; false = refused (no room).
    bool Insert(u32 page, Image&& image);
    void WorkerMain(std::stop_token stop);

    std::mutex loader_mutex; ///< held while the loader runs; Shutdown clears it under this
    Loader loader;
    const size_t budget;
    const bool synchronous;
    const std::chrono::milliseconds protect;

    mutable std::mutex mutex;
    std::unordered_map<u32, Entry> pages;
    std::list<u32> lru; ///< most recently used first
    size_t bytes_resident{};
    std::unordered_map<u32, Failure> failures;
    std::deque<u32> queue;
    std::unordered_set<u32> queued;
    std::condition_variable_any cv;
    std::jthread worker;
    bool stopped{false};

    std::atomic<bool> landed{false};
    std::atomic<u64> loads{0};
    std::atomic<u64> evictions{0};
    std::atomic<u64> refusals{0};
};

} // namespace Core::Mods
