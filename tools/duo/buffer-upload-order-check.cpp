// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <utility>

#include "common/dynamic_library.h"
#include "common/host_memory.h"
#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

int main() {
    bool ready = false;
    try {
        Core::System system;
        system.Initialize();
        Kernel::KProcess process{system.Kernel()};
        auto& page_table = process.GetPageTable().GetImpl();
        page_table.Resize(32, 12);
        auto& memory = process.GetMemory();
        memory.SetCurrentPageTable(process);
        constexpr VAddr virtual_address = 0x10000000;
        constexpr DAddr address = 0x10000;
        constexpr size_t mapped_size = 0x10000;
        memory.MapMemoryRegion(page_table, Common::ProcessAddress{virtual_address}, mapped_size,
                               Common::PhysicalAddress{Core::DramMemoryMap::Base + 0x100000},
                               Common::MemoryPermission::ReadWrite, false);
        Tegra::MaxwellDeviceMemoryManager guest{system.DeviceMemory()};
        guest.AllocateFixed(address, mapped_size);
        const auto asid = guest.RegisterProcess(&memory);
        guest.Map(address, virtual_address, mapped_size, asid, true);

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
        Vulkan::Device device{*instance, physical, VkSurfaceKHR{}, dispatch};
        Vulkan::MemoryAllocator allocator{device};
        Vulkan::StateTracker state_tracker;
        Vulkan::Scheduler scheduler{device, state_tracker};
        Vulkan::StagingBufferPool pool{device, allocator, scheduler};
        Vulkan::GuestDescriptorQueue descriptors{
            device, Vulkan::UpdateDescriptorQueue::GUEST_FRAME_PAYLOAD_SIZE};
        Vulkan::ComputePassDescriptorQueue compute_descriptors{
            device, Vulkan::UpdateDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE};
        Vulkan::DescriptorPool descriptor_pool{device, scheduler};
        Vulkan::BufferCacheRuntime runtime{device, allocator, scheduler, pool, descriptors,
                                           compute_descriptors, descriptor_pool};
        Vulkan::BufferCache cache{guest, runtime};
        auto result = allocator.CreateBuffer(VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 3 * sizeof(u32),
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        }, Vulkan::MemoryUsage::Download);
        ready = true;

        // Submit actual upload/main command buffers in production order. No render/query
        // caches are needed; restart only after device completion permits buffer reset.
        const auto submit = [&] {
            scheduler.RecordWithUploadBuffer([&device](Vulkan::vk::CommandBuffer commands,
                                                       Vulkan::vk::CommandBuffer uploads) {
                const VkMemoryBarrier barrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                };
                uploads.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, barrier);
                uploads.End();
                commands.End();
                const std::array<VkCommandBuffer, 2> buffers{*uploads, *commands};
                const VkSubmitInfo submission{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = static_cast<u32>(buffers.size()),
                    .pCommandBuffers = buffers.data(),
                };
                Vulkan::vk::Check(device.GetGraphicsQueue().Submit(submission));
                Vulkan::vk::Check(device.GetLogical().WaitIdle());
                const VkCommandBufferBeginInfo begin{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                uploads.Begin(begin);
                commands.Begin(begin);
            });
            scheduler.WaitWorker();
        };
        const auto host_read_barrier = [&] {
            scheduler.Record([](Vulkan::vk::CommandBuffer commands) {
                const VkMemoryBarrier barrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                };
                commands.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                         0, barrier);
            });
        };

        // A later CPU write must not be hoisted ahead of an already-recorded GPU read of
        // the same exported cached buffer. Both reads execute on the real Vulkan device.
        constexpr u32 before = 0x12345678;
        constexpr u32 after = 0x87654321;
        using Sync = VideoCommon::ObtainBufferSynchronize;
        using Op = VideoCommon::ObtainBufferOperation;
        guest.WriteBlockUnsafe(address, &before, sizeof(before));
        const auto [warmed, warmup_offset] =
            cache.ObtainCPUBuffer(address, sizeof(u32), Sync::FullSynchronize, Op::DoNothing);
        submit();
        // Fresh allocations are marked fully used by their initialization clear. Model
        // the normal post-completion usage reset before exercising a resident buffer.
        warmed->ResetUsageTracking();
        const auto [first, first_offset] =
            cache.ObtainCPUBuffer(address, sizeof(u32), Sync::FullSynchronize, Op::DoNothing);
        const std::array old_copy{VideoCommon::BufferCopy{
            .src_offset = first_offset, .dst_offset = 0, .size = sizeof(u32)}};
        runtime.CopyBuffer(*result, *first, old_copy, true);

        guest.WriteBlockUnsafe(address, &after, sizeof(after));
        cache.WriteMemory(address, sizeof(u32));
        const auto [second, second_offset] =
            cache.ObtainCPUBuffer(address, sizeof(u32), Sync::FullSynchronize, Op::DoNothing);
        const std::array new_copy{VideoCommon::BufferCopy{
            .src_offset = second_offset, .dst_offset = sizeof(u32), .size = sizeof(u32)}};
        runtime.CopyBuffer(*result, *second, new_copy, true);
        host_read_barrier();
        submit();
        result.Invalidate();
        std::array<u32, 2> observed{};
        std::memcpy(observed.data(), result.Mapped().data(), sizeof(observed));
        if (observed != std::array{before, after}) {
            std::fprintf(stderr, "FAIL: cached GPU reads observed %08x/%08x; expected %08x/%08x\n",
                         observed[0], observed[1], before, after);
            return 1;
        }
        std::puts("PASS: cached GPU reads preserve both sides of a later CPU upload");

        // Retiring an older download must not make a newer, unsnapshotted GPU write
        // CPU-current. The three-word case also checks both non-overlapping sides.
        for (const auto [old_words, accumulate] :
             {std::pair{1U, false}, std::pair{3U, false},
              std::pair{1U, true}, std::pair{3U, true}}) {
            const DAddr counter = address + old_words * 0x100;
            const u32 old_size = old_words * sizeof(u32);
            const u32 newer_offset = old_words == 1 ? 0 : sizeof(u32);
            const auto [old_buffer, old_offset] =
                cache.ObtainCPUBuffer(counter, old_size, Sync::FullSynchronize, Op::MarkAsWritten);
            runtime.ClearBuffer(*old_buffer, old_offset, old_size, before);
            cache.CommitAsyncFlushes();
            submit();

            const auto [new_buffer, new_offset] = cache.ObtainCPUBuffer(
                counter + newer_offset, sizeof(u32), Sync::FullSynchronize, Op::MarkAsWritten);
            runtime.ClearBuffer(*new_buffer, new_offset, sizeof(u32), after);
            if (accumulate) {
                cache.AccumulateFlushes();
            }
            const std::array gpu_copy{VideoCommon::BufferCopy{
                .src_offset = old_offset, .dst_offset = 0, .size = old_size}};
            runtime.CopyBuffer(*result, *old_buffer, gpu_copy, true);
            host_read_barrier();
            submit();
            result.Invalidate();
            std::array<u32, 3> gpu_values{};
            std::memcpy(gpu_values.data(), result.Mapped().data(), old_size);
            for (u32 word = 0; word < old_words; ++word) {
                const u32 expected = word * sizeof(u32) == newer_offset ? after : before;
                if (gpu_values[word] != expected) {
                    std::fprintf(stderr, "FAIL: %u-word GPU counter[%u] is %08x; expected %08x\n",
                                 old_words, word, gpu_values[word], expected);
                    return 1;
                }
            }

            cache.PopAsyncFlushes();
            const bool newer_dirty = cache.IsRegionGpuModified(counter + newer_offset, sizeof(u32));
            std::array<u32, 3> cpu_values{};
            guest.ReadBlockUnsafe(counter, cpu_values.data(), old_size);
            if (!newer_dirty) {
                std::fprintf(stderr,
                             "FAIL: %u-word old download retired newer GPU ownership "
                             "(accumulated=%u): GPU=%08x CPU=%08x\n",
                             old_words, accumulate, gpu_values[newer_offset / sizeof(u32)],
                             cpu_values[newer_offset / sizeof(u32)]);
                return 1;
            }
            for (u32 word = 0; word < old_words; ++word) {
                if (cpu_values[word] != before) {
                    std::fprintf(stderr, "FAIL: old snapshot counter[%u] is %08x; expected %08x\n",
                                 word, cpu_values[word], before);
                    return 1;
                }
            }
            if (old_words == 3 &&
                (cache.IsRegionGpuModified(counter, sizeof(u32)) ||
                 cache.IsRegionGpuModified(counter + 2 * sizeof(u32), sizeof(u32)))) {
                std::fprintf(stderr, "FAIL: old-only counter bytes remained GPU-dirty\n");
                return 1;
            }
            cache.CommitAsyncFlushes();
            submit();
            cache.PopAsyncFlushes();
            guest.ReadBlockUnsafe(counter, cpu_values.data(), old_size);
            if (cpu_values != gpu_values ||
                cache.IsRegionGpuModified(counter + newer_offset, sizeof(u32))) {
                std::fprintf(stderr, "FAIL: newest counter snapshot was not published cleanly\n");
                return 1;
            }
        }
        std::puts("PASS: old readback retirement preserves newer whole and partial GPU writes");

        // Cancelling a snapshot must not let its later retirement consume a fresh snapshot's
        // credit for the same bytes, or overwrite the intervening CPU replacement.
        u32 cancellation_case = 0;
        for (const auto [old_words, replacement_word] :
             {std::pair{1U, 0U}, std::pair{3U, 0U}, std::pair{3U, 1U}, std::pair{3U, 2U}}) {
            const DAddr counter = address + 0x1000 + cancellation_case++ * 0x100;
            const u32 old_size = old_words * sizeof(u32);
            const DAddr replacement_address = counter + replacement_word * sizeof(u32);
            constexpr u32 replacement = 0x13579bdf;
            const auto [old_buffer, old_offset] =
                cache.ObtainCPUBuffer(counter, old_size, Sync::FullSynchronize, Op::MarkAsWritten);
            runtime.ClearBuffer(*old_buffer, old_offset, old_size, before);
            cache.CommitAsyncFlushes();
            submit();

            guest.WriteBlockUnsafe(replacement_address, &replacement, sizeof(replacement));
            cache.WriteMemory(replacement_address, sizeof(replacement));
            const auto [new_buffer, new_offset] = cache.ObtainCPUBuffer(
                replacement_address, sizeof(u32), Sync::FullSynchronize, Op::MarkAsWritten);
            runtime.ClearBuffer(*new_buffer, new_offset, sizeof(u32), after);
            cache.CommitAsyncFlushes();
            submit();

            cache.PopAsyncFlushes();
            std::array<u32, 3> observed_cpu{};
            guest.ReadBlockUnsafe(counter, observed_cpu.data(), old_size);
            const bool dirty = cache.IsRegionGpuModified(replacement_address, sizeof(u32));
            if (observed_cpu[replacement_word] != replacement || !dirty) {
                std::fprintf(stderr,
                             "FAIL: cancelled %u-word snapshot overwrote CPU replacement[%u] "
                             "or retired fresh ownership: CPU=%08x expected=%08x GPU-dirty=%u\n",
                             old_words, replacement_word, observed_cpu[replacement_word],
                             replacement, dirty);
                return 1;
            }
            for (u32 word = 0; word < old_words; ++word) {
                if (word != replacement_word && observed_cpu[word] != before) {
                    std::fprintf(stderr, "FAIL: cancellation lost old-only word[%u]\n", word);
                    return 1;
                }
            }
            cache.PopAsyncFlushes();
            guest.ReadBlockUnsafe(counter, observed_cpu.data(), old_size);
            if (observed_cpu[replacement_word] != after ||
                cache.IsRegionGpuModified(replacement_address, sizeof(u32))) {
                std::fprintf(stderr, "FAIL: replacement snapshot did not publish its GPU value\n");
                return 1;
            }
        }
        std::puts("PASS: cancelled whole, prefix, middle and suffix readbacks cannot consume fresh ownership");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR: cached-buffer Vulkan fixture failed: %s\n", error.what());
        return ready ? 1 : 2;
    }
}
