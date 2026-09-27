// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace Core::Mods {

// Geometry and style are immutable between manifest reloads, which clear the caches. Within
// that lifetime, only shapes that actually contain hole polygons can change the silhouette.
// Store their active bits directly: unrelated actor records cannot invalidate the cache, and
// unlike a hash this identity cannot collide. The separator keeps the two shape groups distinct.
template <typename Shapes, typename Names>
std::string ActiveHoleIdentity(const Shapes& occluders, const Shapes& vignettes, const Names& dead,
                               const Names& dispelled) {
    std::string identity;
    identity.reserve(occluders.size() + vignettes.size() + 1);
    const auto append = [&](const Shapes& shapes, const Names& removed) {
        for (const auto& shape : shapes) {
            if (!shape.tris.empty()) {
                identity += removed.contains(shape.name) ? '0' : '1';
            }
        }
    };
    append(occluders, dead);
    identity += '.';
    append(vignettes, dispelled);
    return identity;
}

} // namespace Core::Mods
