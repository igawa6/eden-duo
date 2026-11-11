// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#include "core/mods/mod_launch_gate.h"

namespace Core::Mods {

namespace {
std::atomic<bool> companions_disabled{false};
}

void SetCompanionsDisabledForLaunch(bool disabled) {
    companions_disabled.store(disabled);
}

bool CompanionsDisabledForLaunch() {
    return companions_disabled.load();
}

} // namespace Core::Mods
