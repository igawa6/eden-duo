# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later

# Eden Duo additions to the `core` target, included with one line from src/core/CMakeLists.txt so
# the upstream file carries no Duo source list. include() keeps the including directory's scope:
# relative paths below resolve against src/core.

target_sources(core PRIVATE
    file_sys/duo_external_content_index.cpp
    file_sys/duo_external_content_index.h
    mods/mod_runtime.cpp
    mods/mod_manifest.cpp
    mods/mod_persist.cpp
    mods/mod_persist.h
    mods/mod_clock.cpp
    mods/mod_clock.h
    mods/mod_host_values.cpp
    mods/mod_host_values.h
    mods/mod_expr.cpp
    mods/mod_expr.h
    mods/mod_state.cpp
    mods/mod_actions.cpp
    mods/mod_input.cpp
    mods/mod_nav.cpp
    mods/mod_nav.h
    mods/mod_pages.cpp
    mods/mod_assets.cpp
    mods/mod_sources.cpp
    mods/mod_sources.h
    mods/mod_settings.cpp
    mods/mod_settings.h
    mods/mod_user_source.cpp
    mods/mod_user_source.h
    mods/mod_romfs_sources.cpp
    mods/mod_romfs_sources.h
    mods/mod_map.cpp
    mods/mod_redraw.cpp
    mods/mod_guest_bridge.cpp
    mods/engine_formats.h
    mods/engine_ichigo.cpp
    mods/engine_il2cpp.cpp
    mods/engine_mercury.cpp
    mods/mod_runtime.h
    mods/mod_runtime_shared.h
    mods/mod_re_tools.cpp
    mods/mod_console.cpp
    mods/dsmod_module_abi.h
    mods/mod_module.cpp
    mods/mod_msbt.cpp
    mods/mod_msbt.h
    mods/mod_module_host.cpp
    mods/mod_module_services.cpp
    mods/mod_nx_assets.cpp
    mods/mod_nx_assets.h
    mods/mod_nx_runtime.cpp
    mods/mod_font_pages.cpp
    mods/mod_font_pages.h
    mods/mod_font_epoch.h
    mods/mod_load_plan.cpp
    mods/mod_load_plan.h
    mods/dsmod_module_extensions.h
    mods/mod_module.h
    mods/mod_types.h
    mods/mod_types_action.h
    mods/mod_types_composite.h
    mods/mod_types_guest.h
    mods/mod_types_map.h
    mods/mod_types_nav.h
    mods/mod_types_page.h
    mods/mod_types_points.h
    mods/mod_types_text.h
    mods/mod_ui.cpp
    mods/mod_ui.h
    mods/mod_ui_canvas.cpp
    mods/mod_ui_expand.cpp
    mods/mod_ui_image.cpp
    mods/mod_ui_internal.h
    mods/mod_ui_map_widget.cpp
    mods/mod_ui_scroll.cpp
    mods/mod_ui_text.cpp
    mods/mod_ui_transitions.cpp
    mods/mod_ui_widget_state.cpp
    hle/service/dsm/dsm.cpp
    hle/service/dsm/dsm.h
)

# Duo's per-tick snapshots still use the pinned dense map implementation (see
# .upstream/DIVERGENCES.md; the package is added in externals/CMakeLists.txt).
target_link_libraries(core PUBLIC unordered_dense::unordered_dense)

# dsm:u is written against both upstream service handler-table forms. Eden #4480 replaced the
# constructor-time RegisterHandlers table with a per-service FindRequest override built by
# CreateStaticMap; select the form the service.h in this tree provides, and re-run the configure
# when that header changes (an upstream merge can switch the form).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/hle/service/service.h")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/hle/service/service.h" DUO_HLE_STATIC_DISPATCH
    REGEX "CreateStaticMap")
if (DUO_HLE_STATIC_DISPATCH)
    set_property(SOURCE hle/service/dsm/dsm.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS DUO_HLE_STATIC_DISPATCH=1)
endif()
unset(DUO_HLE_STATIC_DISPATCH)

# Title-specific DSMod code is an optional, separately distributable shared object.  It is kept
# outside the core archive so Eden APKs contain only the generic host and a package can provide a
# module matching one title/build.  The module links no Eden target and receives all emulator
# services through dsmod_module_abi.h.  The title modules are built from eden-duo-companions/native/modules.
# This repository retains only the generic runtime, ABI headers and shared module SDK.

# The RE memory-analysis toolkit (mods/mod_re_tools.cpp: heap scan/diff/class-dump, plus
# mods/mod_console.cpp: the EDEN_DSMOD_CMD live console dispatch -- both split physically out of
# the runtime; mods/mod_runtime.cpp keeps only empty same-named shells that call into them) is not
# game-specific, but it has no business in a build a player installs: every entry point is already
# gated behind a desktop-only env var (an
# Android app inherits no environment, so none of it is even reachable there today), it is roughly
# twice the size of all the actually game-specific code in this same directory combined, and it is
# pure dead weight once compiled into a release APK. It stays available -- ON by default, same as
# a plain local `cmake -B build` with no extra flags -- because it is actively used for RE work;
# only the release build turns it OFF: one option, default chosen for the common (dev) case,
# flipped for the one build that ships.
# The option is declared at the root and propagated by common to all consumers.
