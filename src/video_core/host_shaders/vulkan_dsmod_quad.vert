// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Textured quad for the dual-screen mod compositor: canvas-pixel vertices mapped to clip space by
// a scale/offset pair (no matrix, so the push block also fits a per-quad alpha for blink pulses).

#version 460 core

layout (location = 0) out vec2 frag_tex_coord;
layout (location = 1) out float frag_alpha;
layout (location = 2) out vec3 frag_tint;

struct ScreenRectVertex {
    vec2 position;
    vec2 tex_coord;
};

layout (push_constant) uniform PushConstants {
    vec4 scale_offset;  // xy: clip units per canvas pixel, zw: clip offset
    ScreenRectVertex vertices[4];
    float alpha;
    float tint_r;  // three scalars: a vec3 would round up past the 96-byte block
    float tint_g;
    float tint_b;
};

// Vulkan spec 15.8.1: push-constant arrays may only be indexed dynamically uniformly.
ScreenRectVertex GetVertex(int index) {
    if (index < 1) {
        return vertices[0];
    } else if (index < 2) {
        return vertices[1];
    } else if (index < 3) {
        return vertices[2];
    } else {
        return vertices[3];
    }
}

void main() {
    ScreenRectVertex vertex = GetVertex(gl_VertexIndex);
    gl_Position = vec4(vertex.position * scale_offset.xy + scale_offset.zw, 0.0, 1.0);
    frag_tex_coord = vertex.tex_coord;
    frag_alpha = alpha;
    frag_tint = vec3(tint_r, tint_g, tint_b);
}
