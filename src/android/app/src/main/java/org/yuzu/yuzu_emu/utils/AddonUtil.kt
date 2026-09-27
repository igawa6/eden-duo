// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.utils

object AddonUtil {
    // Dual-screen packages use the dedicated ZIP picker. Keep legacy packages discoverable by
    // PatchManager, while avoiding an unvalidated dualscreen/ directory through the generic mod
    // picker.
    val validAddonDirectories = listOf("cheats", "exefs", "romfs", "romfslite", "romfs_ext")
}
