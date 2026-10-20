// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

#include <cstddef>
#include <cstdio>
#include <exception>

#include "common/dynamic_library.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

int main() {
    bool ready = false;
    try {
        // OpenLibrary(nullptr) needs an Android frontend GraphicsContext; load the system loader
        // directly instead. No Java, window, surface, guest, or custom-driver setup is required.
        Common::DynamicLibrary library;
        if (!library.Open(Common::DynamicLibrary::GetVersionedFilename("vulkan", 1).c_str()) &&
            !library.Open(Common::DynamicLibrary::GetVersionedFilename("vulkan").c_str())) {
            std::fprintf(stderr, "ERROR: Vulkan loader is unavailable\n");
            return 2;
        }
        Vulkan::vk::InstanceDispatch dispatch{};
        auto instance = Vulkan::CreateInstance(library, dispatch, VK_API_VERSION_1_1,
                                               Core::Frontend::WindowSystemType::Headless, false);
        const auto physical_devices = instance.EnumeratePhysicalDevices();
        if (physical_devices.empty()) {
            std::fprintf(stderr, "ERROR: no Vulkan physical device\n");
            return 2;
        }
        const Vulkan::vk::PhysicalDevice physical{physical_devices.front(), dispatch};
        if (physical.GetProperties().apiVersion < VK_API_VERSION_1_1) {
            std::fprintf(stderr, "ERROR: first Vulkan physical device does not support Vulkan 1.1\n");
            return 2;
        }
        Vulkan::Device device{*instance, physical, VkSurfaceKHR{}, dispatch};
        Vulkan::MemoryAllocator allocator{device};
        Vulkan::StateTracker state_tracker;
        Vulkan::Scheduler scheduler{device, state_tracker};
        Vulkan::StagingBufferPool pool{device, allocator, scheduler};
        ready = true;

        // The production constructor caps the stream at 256 MiB (128 MiB on FreeBSD), split
        // into NUM_SYNCS regions. A 1 MiB request fits the normal 16 MiB Android region.
        // Tooling can shrink the ring; reject a runtime where this request cannot use it.
        constexpr size_t request_size = 1024 * 1024;
        constexpr size_t max_requests = 256 * 1024 * 1024 / request_size + 1;
        const u64 tick = scheduler.CurrentTick();
        if (tick == 0 || scheduler.IsFree(tick)) {
            std::fprintf(stderr, "ERROR: scheduler does not have an unsubmitted busy tick\n");
            return 1;
        }

        // Never Flush/Finish/Wait: every region remains owned by this unsubmitted tick.
        // At most two fallback buffers are allocated in addition to the real stream buffer.
        size_t stream_requests = 0;
        bool busy_wrap = false;
        for (size_t i = 0; i < max_requests; ++i) {
            const auto ref = pool.Request(request_size, Vulkan::MemoryUsage::Upload);
            if (ref.buffer != pool.StreamBuf()) {
                busy_wrap = true;
                break;
            }
            ++stream_requests;
        }
        if (!busy_wrap || stream_requests == 0) {
            std::fprintf(stderr,
                         "ERROR: did not reach a busy stream wrap (stream requests=%zu)\n",
                         stream_requests);
            return stream_requests == 0 ? 2 : 1;
        }

        const auto next = pool.Request(request_size, Vulkan::MemoryUsage::Upload);
        if (scheduler.CurrentTick() != tick || scheduler.IsFree(tick)) {
            std::fprintf(stderr, "ERROR: busy tick changed or completed during the check\n");
            return 1;
        }
        if (next.buffer == pool.StreamBuf()) {
            std::fprintf(stderr,
                         "FAIL: allocation after busy-wrap fallback reused the busy stream ring "
                         "(offset=%llu, stream requests=%zu)\n",
                         static_cast<unsigned long long>(next.offset), stream_requests);
            return 1;
        }
        std::printf("PASS: consecutive busy-wrap requests stay outside the stream ring "
                    "(stream requests=%zu)\n",
                    stream_requests);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR: headless Vulkan fixture failed: %s\n", error.what());
        return ready ? 1 : 2;
    }
}
