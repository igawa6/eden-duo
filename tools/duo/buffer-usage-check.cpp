// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include "video_core/buffer_cache/usage_tracker.h"

int main() {
    VideoCommon::UsageTracker tracker(8192);
    tracker.Track(4031, 64);
    if (!tracker.IsUsed(4094, 128)) {
        std::puts("FAIL: cross-page upload overlaps a tracked read");
        return 1;
    }
    tracker.Reset();
    tracker.Track(4095, 2);
    if (!tracker.IsUsed(4095, 1) || !tracker.IsUsed(4096, 1)) {
        std::puts("FAIL: short read crossing a page loses a touched unit");
        return 1;
    }
    tracker.Reset();
    tracker.Track(1, 1);
    if (!tracker.IsUsed(1, 1) || tracker.IsUsed(64, 1)) {
        std::puts("FAIL: short read usage leaks into an untouched unit");
        return 1;
    }
    tracker.Reset();
    tracker.Track(0, 4096);
    if (!tracker.IsUsed(4095, 1) || tracker.IsUsed(4096, 1) || tracker.IsUsed(0, 0)) {
        std::puts("FAIL: exact-page or empty range coverage is incorrect");
        return 1;
    }
    std::puts("PASS: cross-page, short, exact-page and empty GPU read ranges");
    return 0;
}
