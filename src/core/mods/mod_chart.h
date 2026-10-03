// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Runtime 17: the Chart widget's ring buffers. ChartSampler keeps one buffer per Chart widget of
// the manifest (every page, so a chart has history the first time its page is shown), takes a
// sample of the widget's `bind` every `interval_ms`, and publishes the buffers into the tick's
// snapshot: StateSnapshot::charts (oldest sample first) and "@chart:<key>" = samples taken, which
// is what marks the chart dirty for the redraw. Runs on the tick thread (ModRuntime::Tick, after
// the derived values); the published map is immutable and shared with the redraw worker.

#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "core/mods/mod_types.h"
#include "core/mods/mod_ui.h"

namespace Core::Mods {

class ChartSampler {
public:
    /// Forgets every buffer (a package reload).
    void Reset() {
        buffers.clear();
        published.reset();
        charts.clear();
        indexed_pages = nullptr;
        indexed_count = 0;
    }

    /// Samples every Chart widget whose interval has elapsed at `now_ms` (a monotonic clock) and
    /// publishes the buffers into `snapshot`. A value that does not resolve is not sampled (the
    /// interval still restarts). A tick late by less than one interval keeps the cadence; a longer
    /// gap (a pause) takes one sample, not a burst.
    void Update(const Manifest& manifest, u64 now_ms, StateSnapshot& snapshot) {
        // The chart widgets are listed once per manifest (Reset on a reload drops the list): a
        // package without charts costs one comparison per tick, not a walk of every widget.
        if (indexed_pages != manifest.pages.data() || indexed_count != manifest.pages.size()) {
            Index(manifest);
        }
        if (charts.empty() && buffers.empty()) {
            snapshot.charts = nullptr;
            return;
        }
        bool changed = false;
        {
            for (const Chart& chart : charts) {
                const ChartSpec& spec = *chart.spec;
                Buffer& b = buffers[spec.key];
                const u64 interval = std::max<u32>(1, spec.interval_ms);
                if (b.started && now_ms - b.last_ms < interval) {
                    continue;
                }
                b.last_ms = b.started && now_ms - b.last_ms < 2 * interval ? b.last_ms + interval
                                                                           : now_ms;
                b.started = true;
                const auto v = SnapshotNumber(snapshot, chart.bind);
                if (!v.has_value() || !std::isfinite(*v)) {
                    continue;
                }
                const size_t cap = std::max<u32>(2, spec.samples);
                if (b.ring.size() != cap) {
                    b.ring.assign(cap, 0.0f);
                    b.head = 0;
                    b.size = 0;
                }
                b.ring[b.head] = static_cast<f32>(*v);
                b.head = (b.head + 1) % cap;
                b.size = std::min(b.size + 1, cap);
                ++b.count;
                changed = true;
            }
        }
        if (changed || (!published && !buffers.empty())) {
            auto map = std::make_shared<ChartSeriesMap>();
            for (const auto& [key, b] : buffers) {
                ChartSeries& series = (*map)[key];
                series.count = b.count;
                series.values.reserve(b.size);
                const size_t cap = b.ring.size();
                for (size_t i = 0; i < b.size; ++i) {
                    series.values.push_back(b.ring[(b.head + cap - b.size + i) % cap]);
                }
            }
            published = std::move(map);
        }
        snapshot.charts = published;
        if (published) {
            for (const auto& [key, series] : *published) {
                snapshot.ints["@chart:" + key] = static_cast<s64>(series.count);
            }
        }
    }

private:
    /// One Chart widget of the manifest: its spec and bound value (copies, so the list never
    /// points into a manifest that was re-parsed).
    struct Chart {
        std::shared_ptr<const ChartSpec> spec;
        std::string bind;
    };

    void Index(const Manifest& manifest) {
        charts.clear();
        for (const Page& page : manifest.pages) {
            for (const Widget& w : page.widgets) {
                if (w.type == WidgetType::Chart && w.chart && w.repeat <= 0) {
                    charts.push_back(Chart{w.chart, w.bind});
                }
            }
        }
        indexed_pages = manifest.pages.data();
        indexed_count = manifest.pages.size();
    }

    struct Buffer {
        std::vector<f32> ring;
        size_t head{0}; ///< the next slot written
        size_t size{0};
        u64 count{0};
        u64 last_ms{0};
        bool started{false};
    };
    std::unordered_map<std::string, Buffer> buffers;
    std::shared_ptr<const ChartSeriesMap> published;
    std::vector<Chart> charts;
    const Page* indexed_pages{nullptr};
    size_t indexed_count{0};
};

} // namespace Core::Mods
