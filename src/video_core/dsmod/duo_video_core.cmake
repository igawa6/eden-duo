# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later

# Eden Duo additions to the `video_core` target, included with one line from
# src/video_core/CMakeLists.txt so the upstream source list carries no Duo files. include() keeps
# the including directory's scope: relative paths below resolve against src/video_core.

target_sources(video_core PRIVATE
    dsmod/aux_routing.h
    dsmod/texture_readback.h
    renderer_vulkan/vk_dsmod_aux.cpp
    renderer_vulkan/vk_dsmod_aux.h
    renderer_vulkan/vk_dsmod_rt_probe.cpp
    renderer_vulkan/vk_dsmod_texture_readback.cpp
)
