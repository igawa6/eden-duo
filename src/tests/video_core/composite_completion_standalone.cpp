// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <stdexcept>

#include "video_core/composite_completion.h"

void Require(bool condition) {
    if (!condition) {
        throw std::runtime_error("composite completion ordering failed");
    }
}

int main() {
    Tegra::CompositeCompletion completion;
    Require(completion.IsDone(0));
    Require(!completion.IsDone(1));
    completion.MarkDone(2);
    Require(completion.IsDone(2));
    Require(!completion.IsDone(1)); // Finishing another display must not release this frame.
    completion.MarkDone(4);
    completion.MarkDone(2); // Duplicate callback must not advance the watermark.
    Require(!completion.IsDone(3));
    completion.MarkDone(1);
    Require(completion.IsDone(1) && completion.IsDone(2));
    Require(!completion.IsDone(3));
    completion.MarkDone(3);
    Require(completion.IsDone(4));
    Require(!completion.IsDone(5));
    for (unsigned long long id = 5; id < 10000; ++id) {
        completion.MarkDone(id);
        Require(completion.IsDone(id));
        Require(!completion.IsDone(id + 1));
    }
    std::cout << "Composite completion ordering checks passed\n";
}
