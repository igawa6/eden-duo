// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.libsdl.app;

import android.content.Context;

/** Initializes SDL audio without assigning either emulator window as an SDL Activity. */
public final class EdenAudioContext {
    private EdenAudioContext() {}

    public static void initialize(Context context) {
        SDL.initialize();
        SDLAudioManager.setContext(context.getApplicationContext());
    }
}
