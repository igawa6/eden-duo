# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later

# Eden Duo test sources and test targets, included with one line from src/tests/CMakeLists.txt so
# the upstream file carries no Duo list. include() keeps the including directory's scope: relative
# paths and ${CMAKE_CURRENT_SOURCE_DIR} below resolve against src/tests.

target_sources(tests PRIVATE
    core/mods/input_tokens.cpp
    core/mods/asset_sources.cpp
    core/mods/map_cache_identity.cpp
    core/mods/map_overview.cpp
    core/mods/manifest_digest.cpp
    core/mods/firmware_version_leak.cpp
    core/mods/firmware_version_nca_path.cpp
    core/mods/map_pan_zoom_host_value.cpp
    core/mods/profile_reload.cpp
    core/mods/map_room_category.cpp
    core/mods/image_fallback.cpp
    core/mods/mod_load_plan.cpp
    core/mods/pan_bounds.cpp
    core/mods/quarter_turn_markers.cpp
    core/mods/render_golden.cpp
    core/mods/runtime14.cpp
    core/mods/runtime18.cpp
    core/mods/runtime21.cpp
    core/mods/runtime22.cpp
    core/mods/runtime16_image.cpp
    core/mods/text_markup.cpp
    core/mods/text_slant.cpp
    core/mods/runtime17_text.cpp
    core/mods/ui_render.cpp
    core/mods/widget_position.cpp
    video_core/aux_routing.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_x86_64)
    add_library(dsmod_test_module SHARED core/mods/fake_module.cpp)
    target_include_directories(dsmod_test_module PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(dsmod_test_module PROPERTIES PREFIX "" OUTPUT_NAME "dsmod-test-module")
    add_library(dsmod_test_bad_module SHARED core/mods/fake_module.cpp)
    target_include_directories(dsmod_test_bad_module PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(dsmod_test_bad_module PRIVATE DSMOD_BAD_INTERFACE=1)
    set_target_properties(dsmod_test_bad_module PROPERTIES PREFIX "" OUTPUT_NAME "dsmod-test-bad-module")
    target_sources(tests PRIVATE core/mods/module.cpp)
    target_compile_definitions(tests PRIVATE
        DSMOD_TEST_MODULE_PATH="$<TARGET_FILE:dsmod_test_module>"
        DSMOD_TEST_BAD_MODULE_PATH="$<TARGET_FILE:dsmod_test_bad_module>")
    add_dependencies(tests dsmod_test_module dsmod_test_bad_module)
endif()

# DSMod package regression goldens (core/mods/manifest_digest.cpp, core/mods/render_golden.cpp):
# the published packages are read from these folders at test time (':'-separated, each holding
# <Package>/dualscreen/manifest.json; EDEN_DSMOD_GOLDEN_PACKAGE_ROOTS overrides at run time, a
# package found nowhere is skipped). The goldens live in the source tree.
set(DSMOD_GOLDEN_PACKAGE_ROOTS
    "$ENV{HOME}/eden-duo-companions/packages"
    CACHE STRING "':'-separated folders holding the published DSMod packages for the golden tests")
target_compile_definitions(tests PRIVATE
    DSMOD_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/core/mods/golden"
    DSMOD_GOLDEN_PACKAGE_ROOTS="${DSMOD_GOLDEN_PACKAGE_ROOTS}")

# A standalone, boot-free, two-real-thread TSan harness for the DSMod off-thread bottom-screen
# redraw (RenderPage driven from two std::threads against hand-built fixtures -- no
# ModRuntime/System/guest CPU, so no fiber annotation and no guest boot are needed to run it).
# Its own executable target, so it compiles only its one file and can be built and run on its
# own, for example in a TSan build.
add_executable(tsan_redraw_harness
    core/mods/tsan_redraw_harness.cpp
)

create_target_directory_groups(tsan_redraw_harness)

target_link_libraries(tsan_redraw_harness PRIVATE common core input_common video_core)
target_link_libraries(tsan_redraw_harness PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain
    Threads::Threads)
add_test(NAME tsan_redraw_harness COMMAND tsan_redraw_harness)

if (NOT MSVC)
    target_compile_options(tsan_redraw_harness PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

# Drag-to-scroll lists (Page::scrolls): boot-free checks of offset clamp, visible-row
# materialisation, hit-test index mapping and clipped drawing. Own target, like
# tsan_redraw_harness above.
add_executable(dsmod_scroll_test
    core/mods/scroll_2d.cpp
    core/mods/scroll_list.cpp
)
create_target_directory_groups(dsmod_scroll_test)
target_link_libraries(dsmod_scroll_test PRIVATE common core input_common video_core)
target_link_libraries(dsmod_scroll_test PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain
    Threads::Threads)
add_test(NAME dsmod_scroll_test COMMAND dsmod_scroll_test)
if (NOT MSVC)
    target_compile_options(dsmod_scroll_test PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

# Package compatibility gating (min_runtime + the built-in update page). Own target, like
# dsmod_scroll_test.
add_executable(dsmod_min_runtime_test
    core/mods/min_runtime.cpp
    core/mods/map_areas_src.cpp
    core/mods/runtime13.cpp
    core/mods/runtime14.cpp
    core/mods/runtime15.cpp
    core/mods/runtime15_persist.cpp
    core/mods/runtime16_input.cpp
    core/mods/runtime17_settings.cpp
    core/mods/runtime17_user.cpp
    core/mods/runtime17_nav.cpp
    core/mods/runtime16.cpp
    core/mods/runtime17.cpp
    core/mods/runtime18.cpp
)
create_target_directory_groups(dsmod_min_runtime_test)
target_link_libraries(dsmod_min_runtime_test PRIVATE common core input_common frontend_common video_core)
target_link_libraries(dsmod_min_runtime_test PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain
    Threads::Threads)
add_test(NAME dsmod_min_runtime_test COMMAND dsmod_min_runtime_test)
if (NOT MSVC)
    target_compile_options(dsmod_min_runtime_test PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

# Offline check of the merged base + update (BKTR patch) romfs read through Eden's NCA stack
# against a reference dump. Not a unit test (needs the user's own game files); run by hand.
add_executable(bktr_romfs_check
    core/file_sys/bktr_romfs_check.cpp
)
create_target_directory_groups(bktr_romfs_check)
target_link_libraries(bktr_romfs_check PRIVATE common core)
target_link_libraries(bktr_romfs_check PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)

# Boot-free check that NSO helper (companion / guest-helper) RX pages are reserved before NCE
# PatchText/RelocateAndCopy, so every module's SVC trampolines land in mapped patch pages (the
# 1.2.1 Android "load plan on a non-last module" hang). `--reproduce-1.2.1` runs the old layout
# and must fail. Needs the NCE patcher (oaknut), so arm64 builds only.
if (HAS_NCE)
    add_executable(nso_nce_layout_test
        core/loader/nso_nce_layout.cpp
        ../core/arm/nce/patcher.cpp
    )
    target_include_directories(nso_nce_layout_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(nso_nce_layout_test PRIVATE common merry::oaknut)
    add_test(NAME nso_nce_layout_test COMMAND nso_nce_layout_test)
endif()

# Boot-free buffer-cache, macro and staging-ring regression checks (tools/duo). The staging-ring and
# upload-order checks need a Vulkan device and exit with 2 (skipped) without one.
add_executable(buffer_usage_test ${PROJECT_SOURCE_DIR}/tools/duo/buffer-usage-check.cpp)
target_include_directories(buffer_usage_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME buffer_usage_test COMMAND buffer_usage_test)

add_executable(macro_shader_binding_test ${PROJECT_SOURCE_DIR}/tools/duo/macro-shader-binding-check.cpp)
target_link_libraries(macro_shader_binding_test PRIVATE core video_core ${PLATFORM_LIBRARIES})
add_test(NAME macro_shader_binding_test COMMAND macro_shader_binding_test)

add_executable(staging_upload_ring_test ${PROJECT_SOURCE_DIR}/tools/duo/staging-upload-ring-check.cpp)
target_link_libraries(staging_upload_ring_test PRIVATE video_core ${PLATFORM_LIBRARIES})
add_test(NAME staging_upload_ring_test COMMAND staging_upload_ring_test)
set_tests_properties(staging_upload_ring_test PROPERTIES SKIP_RETURN_CODE 2)

add_executable(buffer_upload_order_test ${PROJECT_SOURCE_DIR}/tools/duo/buffer-upload-order-check.cpp)
target_link_libraries(buffer_upload_order_test PRIVATE core video_core ${PLATFORM_LIBRARIES})
add_test(NAME buffer_upload_order_test COMMAND buffer_upload_order_test)
set_tests_properties(buffer_upload_order_test PROPERTIES SKIP_RETURN_CODE 2)
