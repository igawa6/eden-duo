// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#version 460 core

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 1) in float frag_alpha;
layout (location = 2) in vec3 frag_tint;

layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D color_texture;

void main() {
    color = texture(color_texture, frag_tex_coord);
    color.rgb *= frag_tint;
    color.a *= frag_alpha;
}
