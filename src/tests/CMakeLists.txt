# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/mods/map_cache_identity.cpp
    core/mods/map_overview.cpp
    core/mods/map_room_category.cpp
    core/mods/mod_load_plan.cpp
    core/mods/widget_position.cpp
    core/internal_network/network.cpp
    video_core/aux_routing.cpp
    video_core/memory_tracker.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)
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

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()

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
