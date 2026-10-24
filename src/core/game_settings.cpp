// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/game_settings.h"

#include <algorithm>
#include <cctype>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_base.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace Core::GameSettings {

static GPUVendor GetGPU(const std::string& gpu_vendor_string) {
    struct Entry { const char* name; GPUVendor vendor; };
    static constexpr Entry GpuVendor[] = {
        // NVIDIA
        {"NVIDIA",   GPUVendor::Nvidia},
        {"Nouveau",  GPUVendor::Nvidia},
        {"NVK",      GPUVendor::Nvidia},
        {"Tegra",    GPUVendor::Nvidia},
        // AMD
        {"AMD",       GPUVendor::AMD},
        {"RadeonSI",  GPUVendor::AMD},
        {"RADV",      GPUVendor::AMD},
        {"AMDVLK",    GPUVendor::AMD},
        {"R600",      GPUVendor::AMD},
        // Intel
        {"Intel",     GPUVendor::Intel},
        {"ANV",       GPUVendor::Intel},
        {"i965",      GPUVendor::Intel},
        {"i915",      GPUVendor::Intel},
        {"OpenSWR",   GPUVendor::Intel},
        // Apple
        {"Apple",     GPUVendor::Apple},
        {"MoltenVK",  GPUVendor::Apple},
        // Qualcomm / Adreno
        {"Qualcomm",  GPUVendor::Qualcomm},
        {"Turnip",    GPUVendor::Qualcomm},
        // ARM / Mali
        {"Mali",      GPUVendor::ARM},
        {"PanVK",     GPUVendor::ARM},
        // Imagination / PowerVR
        {"PowerVR",   GPUVendor::Imagination},
        {"PVR",       GPUVendor::Imagination},
        // Microsoft / WARP / D3D12 GL
        {"D3D12",     GPUVendor::Microsoft},
        {"Microsoft", GPUVendor::Microsoft},
        {"WARP",      GPUVendor::Microsoft},
    };

    for (const auto& entry : GpuVendor) {
        if (gpu_vendor_string == entry.name) {
            return entry.vendor;
        }
    }

    // legacy (shouldn't be needed anymore, but just in case)
    std::string gpu = gpu_vendor_string;
    std::transform(gpu.begin(), gpu.end(), gpu.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (gpu.find("geforce") != std::string::npos) {
        return GPUVendor::Nvidia;
    }
    if (gpu.find("radeon") != std::string::npos || gpu.find("ati") != std::string::npos) {
        return GPUVendor::AMD;
    }

    return GPUVendor::Unknown;
}

static OS DetectOS() {
#if defined(_WIN32)
    return OS::Windows;
#elif defined(__FIREOS__)
    return OS::FireOS;
#elif defined(__ANDROID__)
    return OS::Android;
#elif defined(__OHOS__)
    return OS::HarmonyOS;
#elif defined(__HAIKU__)
    return OS::HaikuOS;
#elif defined(__DragonFly__)
    return OS::DragonFlyBSD;
#elif defined(__NetBSD__)
    return OS::NetBSD;
#elif defined(__OpenBSD__)
    return OS::OpenBSD;
#elif defined(_AIX)
    return OS::AIX;
#elif defined(__managarm__)
    return OS::Managarm;
#elif defined(__redox__)
    return OS::RedoxOS;
#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return OS::IOS;
#elif defined(__APPLE__)
    return OS::MacOS;
#elif defined(__FreeBSD__)
    return OS::FreeBSD;
#elif defined(__sun) && defined(__SVR4)
    return OS::Solaris;
#elif defined(__linux__)
    return OS::Linux;
#else
    return OS::Unknown;
#endif
}

EnvironmentInfo DetectEnvironment(const VideoCore::RendererBase& renderer) {
    EnvironmentInfo env{};
    env.os = DetectOS();
    env.vendor_string = renderer.GetDeviceVendor();
    env.vendor = GetGPU(env.vendor_string);
    return env;
}

namespace {
/// At boot ([force] false) a title value only fills a setting the user left on global; applying
/// the optimal settings ([force] true) replaces the game's own value too.
template <typename T, typename Setting>
void DefaultForTitle(Setting& setting, T value, bool force) {
    if (force || setting.UsingGlobal()) {
        setting.SetGlobal(false);
        setting.SetValue(value);
    }
}

bool TitleSettings(std::uint64_t program_id, bool force) {
    const auto DefaultForTitle = [force](auto& setting, auto value) {
        GameSettings::DefaultForTitle(setting, value, force);
    };
    switch (static_cast<TitleID>(program_id)) {
        case TitleID::FinalFantasyResonance:
            // The bloom hack scales sampled RGBA values and corrupts this game's sprite colours.
            DefaultForTitle(Settings::values.fix_bloom_effects, false);
            // Asynchronous GPU emulation is what reaches 30 FPS on the AYN Thor. Its HUD used to
            // drop out of single frames under it: buffers went back to the game before their
            // composite ran (fixed in HardwareComposer). Accurate GPU emulation, safe DMA and
            // reactive flushing keep its GPU readbacks in order; dynamic vertex input keeps vertex
            // formats valid.
            DefaultForTitle(Settings::values.use_asynchronous_gpu_emulation, true);
            DefaultForTitle(Settings::values.use_docked_mode, Settings::ConsoleMode::Docked);
            DefaultForTitle(Settings::values.cpu_clock, Settings::CpuClock::Boost);
            DefaultForTitle(Settings::values.vsync_mode, Settings::VSyncMode::Mailbox);
            DefaultForTitle(Settings::values.gpu_accuracy, Settings::GpuAccuracy::High);
            DefaultForTitle(Settings::values.dma_accuracy, Settings::DmaAccuracy::Safe);
            DefaultForTitle(Settings::values.sync_memory_operations, true);
            DefaultForTitle(Settings::values.use_disk_shader_cache, true);
            DefaultForTitle(Settings::values.use_reactive_flushing, true);
            DefaultForTitle(Settings::values.enable_buffer_history, true);
#ifdef __ANDROID__
            DefaultForTitle(Settings::values.use_optimized_vertex_buffers, true);
#endif
            DefaultForTitle(Settings::values.use_asynchronous_shaders, true);
            DefaultForTitle(Settings::values.dyna_state, Settings::ExtendedDynamicState::EDS3);
            DefaultForTitle(Settings::values.vertex_input_dynamic_state, true);
            // FXAA smears its character faces; native resolution keeps them sharp without it.
            DefaultForTitle(Settings::values.anti_aliasing, Settings::AntiAliasing::None);
            LOG_INFO(Core, "Title defaults: GPU {}, DMA {}, async GPU {}, reactive flushing {}, "
                           "EDS {}, vertex input {}, docked {}",
                     Settings::CanonicalizeEnum(Settings::values.gpu_accuracy.GetValue()),
                     Settings::CanonicalizeEnum(Settings::values.dma_accuracy.GetValue()),
                     Settings::values.use_asynchronous_gpu_emulation.GetValue(),
                     Settings::values.use_reactive_flushing.GetValue(),
                     Settings::CanonicalizeEnum(Settings::values.dyna_state.GetValue()),
                     Settings::values.vertex_input_dynamic_state.GetValue(),
                     Settings::CanonicalizeEnum(Settings::values.use_docked_mode.GetValue()));
            return true;
        default:
            return false;
    }
}
} // namespace

void LoadBootOverrides(std::uint64_t program_id) {
    TitleSettings(program_id, false);
}

bool HasOptimalSettings(std::uint64_t program_id) {
    switch (static_cast<TitleID>(program_id)) {
        case TitleID::FinalFantasyResonance:
            return true;
        default:
            return false;
    }
}

bool ApplyOptimalSettings(std::uint64_t program_id) {
    return TitleSettings(program_id, true);
}

void LoadOverrides(std::uint64_t program_id, const VideoCore::RendererBase& renderer) {
    const auto env = DetectEnvironment(renderer);

    Settings::values.delay_fence_reports_by_title = false;
    Settings::values.fix_fp16_denormals_by_title = false;
    switch (static_cast<TitleID>(program_id)) {
        case TitleID::NinjaGaidenRagebound:
            Settings::values.use_squashed_iterated_blend = true;
            break;
        case TitleID::DoubleDragonRevive:
            // Its Unreal occlusion culling reads the query reports once the frame's fence
            // signals. Released on the CPU, the fence signals before the GPU has written them,
            // so stale results cull visible buildings and props for single frames.
            Settings::values.delay_fence_reports_by_title = true;
            break;
        case TitleID::FireEmblemEngage:
        case TitleID::DragonQuestVIIReimagined:
        case TitleID::FinalFantasyResonance:
            // Their effect shaders divide by fp16 subnormals; flushed to zero the result goes
            // inf/NaN and the bloom blows characters and lights out to white.
            Settings::values.fix_fp16_denormals_by_title = true;
            break;
        default:
            break;
    }

    LOG_INFO(Core, "Applied game settings for title ID {:016X} on OS {}, GPU vendor {} ({})",
             program_id,
             static_cast<int>(env.os),
             static_cast<int>(env.vendor),
             env.vendor_string);
}

} // namespace Core::GameSettings
