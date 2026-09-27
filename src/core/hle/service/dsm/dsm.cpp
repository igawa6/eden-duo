// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/dsm/dsm.h"
#include "core/hle/service/server_manager.h"
#include "video_core/gpu.h"

namespace Service::DSM {

constexpr u32 DsmIpcVersion = 1;

IDsmUser::IDsmUser(Core::System& system_) : ServiceFramework{system_, "dsm:u"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, C<&IDsmUser::GetVersion>, "GetVersion"},
        {1, C<&IDsmUser::GetAuxDisplayInfo>, "GetAuxDisplayInfo"},
        {2, C<&IDsmUser::GetAuxTouch>, "GetAuxTouch"},
        {3, C<&IDsmUser::BindLayer>, "BindLayer"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

IDsmUser::~IDsmUser() = default;

Result IDsmUser::GetVersion(Out<u32> out_version) {
    LOG_DEBUG(Service, "called");
    *out_version = DsmIpcVersion;
    R_SUCCEED();
}

Result IDsmUser::GetAuxDisplayInfo(Out<VideoCore::DSMod::AuxDisplayInfo> out_info) {
    *out_info = system.GPU().DSModAux().GetDisplayInfo();
    LOG_DEBUG(Service, "called, present={} {}x{}", out_info->present, out_info->width,
              out_info->height);
    R_SUCCEED();
}

Result IDsmUser::GetAuxTouch(
    Out<u32> out_count,
    OutArray<VideoCore::DSMod::AuxTouchPoint, BufferAttr_HipcMapAlias> out_points) {
    auto& aux = system.GPU().DSModAux();
    if (aux.HasUi() || aux.HasComposite()) {
        // A declarative mod package owns the second screen; its widgets consume the touches.
        *out_count = 0;
        R_SUCCEED();
    }
    *out_count = static_cast<u32>(aux.GetTouch(out_points));
    R_SUCCEED();
}

Result IDsmUser::BindLayer(u32 mode, u64 layer_id) {
    LOG_INFO(Service, "called, layer_id={} mode={}", layer_id, mode);
    auto& aux = system.GPU().DSModAux();
    if (mode == 0) {
        u64 expected = layer_id;
        aux.bound_layer.compare_exchange_strong(expected, VideoCore::DSMod::AuxRouting::NoLayer);
    } else {
        aux.bound_layer.store(layer_id);
    }
    R_SUCCEED();
}

void LoopProcess(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    server_manager->RegisterNamedService("dsm:u", std::make_shared<IDsmUser>(system));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::DSM
