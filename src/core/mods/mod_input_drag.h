// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The "@drag*" ints a page reads (PublishInteraction, mod_input.cpp). Kept free of ModRuntime so
// it can be unit tested (src/tests/core/mods/runtime16_input.cpp).
//
// Runtime 16: PublishInteraction also runs before DrainTaps, so an action's enabled_bind (or a
// derived value it reads) sees "@sel:<group>" and "@drag*" while the tap or drop runs. On the tick
// a drag is dropped, that early pass still describes the dropped drag ("@drag" 1, its payload,
// release point and target); the pass after DrainTaps then reports no drag, as before.

#include <optional>

#include "core/mods/mod_types.h"

namespace Core::Mods {

struct DragInts {
    s64 payload{};
    s32 x{};
    s32 y{};
    s64 hover{-1}; ///< expanded index of the drop target under the finger, -1 none
};

/// "@drag" = 1 and "@drag_payload" / "@drag_x" / "@drag_y" / "@drag_hover" for `drag`; without
/// one "@drag" = 0 and the other four are removed (an earlier pass this tick may have set them).
inline void WriteDragInts(StateSnapshot& snapshot, const std::optional<DragInts>& drag) {
    snapshot.ints["@drag"] = drag ? 1 : 0;
    if (!drag) {
        for (const char* key : {"@drag_payload", "@drag_x", "@drag_y", "@drag_hover"}) {
            snapshot.ints.erase(key);
        }
        return;
    }
    snapshot.ints["@drag_payload"] = drag->payload;
    snapshot.ints["@drag_x"] = drag->x;
    snapshot.ints["@drag_y"] = drag->y;
    snapshot.ints["@drag_hover"] = drag->hover;
}

} // namespace Core::Mods
