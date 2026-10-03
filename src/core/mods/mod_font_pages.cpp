// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Paged game-font atlases (runtime 17): the on-demand page loader and its bounded LRU. See
// mod_font_pages.h.

#include <algorithm>
#include <utility>

#include "common/logging.h"
#include "core/mods/mod_font_pages.h"

namespace Core::Mods {

FontPages::FontPages(Loader loader_, size_t budget_, bool synchronous_, s64 protect_ms)
    : loader{std::move(loader_)}, budget{budget_}, synchronous{synchronous_},
      protect{std::max<s64>(0, protect_ms)} {}

FontPages::~FontPages() {
    Shutdown();
}

void FontPages::Shutdown() {
    std::jthread stopping;
    {
        std::scoped_lock lock{mutex};
        stopped = true;
        queue.clear();
        queued.clear();
        stopping = std::move(worker);
    }
    if (stopping.joinable()) {
        stopping.request_stop();
        cv.notify_all();
        stopping.join();
    }
    std::scoped_lock lock{loader_mutex};
    loader = nullptr;
}

std::shared_ptr<const Image> FontPages::Get(u32 page) {
    if (page >= MaxPages) {
        return nullptr;
    }
    {
        std::scoped_lock lock{mutex};
        if (const auto it = pages.find(page); it != pages.end()) {
            it->second.used = Clock::now();
            lru.splice(lru.begin(), lru, it->second.lru);
            return it->second.image;
        }
        if (const auto f = failures.find(page); f != failures.end()) {
            if (f->second.attempts >= MaxAttempts ||
                Clock::now() - f->second.at < std::chrono::milliseconds{RetryMs}) {
                return nullptr;
            }
        }
        if (stopped) {
            return nullptr;
        }
        if (!synchronous) {
            if (queued.insert(page).second) {
                queue.push_back(page);
                if (!worker.joinable()) {
                    worker = std::jthread([this](std::stop_token stop) { WorkerMain(stop); });
                }
                cv.notify_one();
            }
            return nullptr;
        }
    }
    Load(page);
    std::scoped_lock lock{mutex};
    const auto it = pages.find(page);
    return it == pages.end() ? nullptr : it->second.image;
}

void FontPages::Load(u32 page) {
    Image image;
    bool ok = false;
    try {
        std::scoped_lock lock{loader_mutex};
        ok = loader && loader(page, image) && image.Valid();
    } catch (...) {
        ok = false;
    }
    if (ok && Insert(page, std::move(image))) {
        loads.fetch_add(1);
        landed = true;
        std::scoped_lock lock{mutex};
        failures.erase(page);
        return;
    }
    std::scoped_lock lock{mutex};
    auto& failure = failures[page];
    failure.at = Clock::now();
    if (!ok) {
        ++failure.attempts; // a refusal (no room right now) is not counted against the page
        if (failure.attempts == MaxAttempts) {
            LOG_WARNING(Core, "DSMod: font page {} could not be loaded; giving up", page);
        }
    }
}

bool FontPages::Insert(u32 page, Image&& image) {
    const size_t bytes = image.pixels.size() * sizeof(u32);
    std::scoped_lock lock{mutex};
    if (pages.contains(page)) {
        return true;
    }
    if (bytes > budget) {
        refusals.fetch_add(1);
        LOG_WARNING(Core, "DSMod: font page {} ({} bytes) is larger than the page budget ({})",
                    page, bytes, budget);
        return false;
    }
    // Evict from the least recently used end, never a page drawn within ProtectMs: that one is
    // on screen now, and evicting it would only queue it again.
    const auto now = Clock::now();
    size_t evictable = 0;
    for (auto it = lru.rbegin(); it != lru.rend() && bytes_resident - evictable + bytes > budget;
         ++it) {
        const Entry& e = pages.at(*it);
        if (now - e.used < protect) {
            break;
        }
        evictable += e.bytes;
    }
    if (bytes_resident - evictable + bytes > budget) {
        refusals.fetch_add(1);
        return false;
    }
    while (bytes_resident + bytes > budget && !lru.empty()) {
        const u32 victim = lru.back();
        lru.pop_back();
        bytes_resident -= pages.at(victim).bytes;
        pages.erase(victim);
        evictions.fetch_add(1);
    }
    lru.push_front(page);
    Entry entry;
    entry.image = std::make_shared<const Image>(std::move(image));
    entry.bytes = bytes;
    entry.used = now;
    entry.lru = lru.begin();
    pages.emplace(page, std::move(entry));
    bytes_resident += bytes;
    return true;
}

void FontPages::WorkerMain(std::stop_token stop) {
    while (!stop.stop_requested()) {
        u32 page = 0;
        {
            std::unique_lock lock{mutex};
            cv.wait(lock, stop, [this] { return !queue.empty(); });
            if (stop.stop_requested()) {
                return;
            }
            page = queue.front();
            queue.pop_front();
        }
        Load(page);
        std::scoped_lock lock{mutex};
        queued.erase(page);
    }
}

size_t FontPages::ResidentBytes() const {
    std::scoped_lock lock{mutex};
    return bytes_resident;
}

size_t FontPages::ResidentPages() const {
    std::scoped_lock lock{mutex};
    return pages.size();
}

bool FontPages::Resident(u32 page) const {
    std::scoped_lock lock{mutex};
    return pages.contains(page);
}

} // namespace Core::Mods
