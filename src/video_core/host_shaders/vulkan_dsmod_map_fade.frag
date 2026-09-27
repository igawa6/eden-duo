// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Per-cell map reveal for the dual-screen compositor. The CPU publishes full-resolution previous
// and current endpoints only when visibility/content changes; animation advances through a compact
// 650x300 weight texture.

#version 460 core

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 1) in float frag_alpha;
layout (location = 2) in vec3 frag_tint;

layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D previous_texture;
layout (binding = 1) uniform sampler2D current_texture;
layout (binding = 2) uniform sampler2D fade_texture;

void main() {
    ivec2 map_size = textureSize(current_texture, 0);
    // Match the CPU's CellTables exactly: quantise to a source-map texel first, then map that
    // integer texel into the reveal grid. Directly scaling the interpolated UV shifts boundaries.
    ivec2 map_texel = clamp(ivec2(floor(frag_tex_coord * vec2(map_size))), ivec2(0),
                            map_size - ivec2(1));
    ivec2 fade_size = textureSize(fade_texture, 0);
    ivec2 fade_texel = clamp(map_texel * fade_size / map_size, ivec2(0),
                             fade_size - ivec2(1));
    float weight = texelFetch(fade_texture, fade_texel, 0).r;
    color = mix(texelFetch(previous_texture, map_texel, 0),
                texelFetch(current_texture, map_texel, 0), weight);
    color.rgb *= frag_tint;
    color.a *= frag_alpha;
}
