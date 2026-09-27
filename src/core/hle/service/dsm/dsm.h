// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// dsm:u — fork-only service that lets guest code (exlaunch modules, homebrew) talk to the
// emulator's second-screen support. Command ids (dsm.cpp) and payload types
// (video_core/dsmod/aux_routing.h) define the guest-side contract.

#pragma once

#include "core/hle/service/cmif_types.h"
#include "core/hle/service/service.h"
#include "video_core/dsmod/aux_routing.h"

namespace Core {
class System;
}

namespace Service::DSM {

class IDsmUser final : public ServiceFramework<IDsmUser> {
public:
    explicit IDsmUser(Core::System& system_);
    ~IDsmUser() override;

private:
    Result GetVersion(Out<u32> out_version);
    Result GetAuxDisplayInfo(Out<VideoCore::DSMod::AuxDisplayInfo> out_info);
    Result GetAuxTouch(
        Out<u32> out_count,
        OutArray<VideoCore::DSMod::AuxTouchPoint, BufferAttr_HipcMapAlias> out_points);
    Result BindLayer(u32 mode, u64 layer_id);
};

void LoopProcess(Core::System& system);

} // namespace Service::DSM
