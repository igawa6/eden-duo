// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Core::Mods {

/// Eden Duo dual screen mode "Off": set by the frontend before a game launches. While set, no
/// companion package is selected for the launch and the companion runtime is not started, so
/// no companion patch reaches the game. It does not change which add-ons the user has enabled.
void SetCompanionsDisabledForLaunch(bool disabled);
[[nodiscard]] bool CompanionsDisabledForLaunch();

} // namespace Core::Mods
