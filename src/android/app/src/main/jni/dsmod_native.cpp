// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo: dual-screen (DSMod) state of the Android emulation session and the Duo JNI entry
// points of NativeLibrary and NativeInput. The JNI symbol names follow the Kotlin classes, which
// stay where they are; only the C++ moved out of native.cpp and native_input.cpp.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <android/native_window_jni.h>
#include <jni.h>

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "frontend_common/content_manager.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/submission_package.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/mods/mod_launch_gate.h"
#include "core/mods/mod_module.h"
#include "core/mods/mod_runtime.h"
#include "jni/android_settings.h"
#include "jni/dsmod_native.h"
#include "jni/emu_window/emu_window.h"
#include "jni/native.h"
#include "video_core/dsmod/aux_routing.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace {

// DSMod aux (second screen) surface. Guarded by g_aux_mutex; lock order: the session's m_mutex,
// then g_aux_mutex. The aux surface calls come from the UI thread and take only g_aux_mutex:
// InitializeEmulation holds m_mutex for the whole game load, and waiting on it here froze the UI
// (ANR risk) while a game booted.
std::mutex g_aux_mutex;
std::unique_ptr<EmuWindow_Android> g_aux_window;
ANativeWindow* g_aux_native_window{};
bool g_aux_renderer_ready{}; // the game's renderer and GPU exist
bool g_mod_runtime_live{};   // Eden Duo: Load succeeded; the mod runtime (if any) exists
std::shared_ptr<Common::DynamicLibrary> g_vulkan_library; // the session's, while the renderer exists

// Eden Duo: the running (or next) game's own No Companion value, -1 for the global setting.
// Kept in the Kotlin per-game store, since Off must be known before the game window opens.
std::atomic<s32> g_no_companion_override{-1};

// Eden Duo Stage 1 (Simulated/inline dual screen): the hidden developer switch's wish, set by
// Kotlin (NativeLibrary.setInlineDualScreen) at any time and applied once the renderer/GPU exist
// (OnRendererReady) -- same "override atomic, pulled later" shape as g_no_companion_override
// above, because AuxRouting lives inside GPU and calling GPU() before it exists would crash.
std::atomic<bool> g_inline_dual_screen_enabled{false};
std::atomic<bool> g_inline_dual_screen_landscape{true};

// Eden Duo Stage 2: the geometry-driven inline layout (DualScreenGeometry, Kotlin) -- the real
// Simulated layout, recomputed on rotation/resize/fold change/divider drag while a game runs.
// Same "store the wish, applied when the renderer exists" shape as the hidden switch above, but
// also re-applied immediately when the renderer already exists, since this one changes live.
// The hidden switch still wins when it is on (a developer override), checked first in
// ApplyInlineLayoutLocked.
std::atomic<bool> g_duo_layout_enabled{false};
std::atomic<float> g_duo_layout_game_x{0.0f};
std::atomic<float> g_duo_layout_game_y{0.0f};
std::atomic<float> g_duo_layout_game_w{1.0f};
std::atomic<float> g_duo_layout_game_h{1.0f};
std::atomic<float> g_duo_layout_companion_x{0.0f};
std::atomic<float> g_duo_layout_companion_y{0.0f};
std::atomic<float> g_duo_layout_companion_w{0.0f};
std::atomic<float> g_duo_layout_companion_h{0.0f};

Core::System& SessionSystem() {
    return EmulationSession::GetInstance().System();
}

/// DSMod haptics: runs on AuxRouting's notifier thread (never the emulation thread) and hands the
/// event to Kotlin, which plays it on the second screen's view from the UI thread.
void PlayAuxHaptic(const VideoCore::DSMod::AuxRouting::HapticEvent& event) {
    JNIEnv* const env = Common::Android::GetEnvForThread();
    if (env == nullptr) {
        return;
    }
    static jmethodID method = nullptr;
    static std::once_flag looked_up;
    std::call_once(looked_up, [env] {
        method = env->GetStaticMethodID(Common::Android::GetNativeLibraryClass(), "onAuxHaptic",
                                        "(III)V");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            method = nullptr;
        }
        if (method == nullptr) {
            LOG_ERROR(Frontend, "DSMod haptics: NativeLibrary.onAuxHaptic not found");
        }
    });
    if (method == nullptr) {
        return;
    }
    env->CallStaticVoidMethod(Common::Android::GetNativeLibraryClass(), method,
                              static_cast<jint>(event.strength), static_cast<jint>(event.kind),
                              static_cast<jint>(event.flags));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void AttachAuxWindowLocked() {
    // g_aux_mutex held. Attaches only while the game's renderer exists.
    if (!g_aux_native_window || !g_aux_renderer_ready) {
        return;
    }
    auto& system = SessionSystem();
    if (g_aux_window) {
        system.Renderer().SetAuxWindow(nullptr);
        g_aux_window->OnSurfaceChanged(g_aux_native_window);
    } else {
        g_aux_window =
            std::make_unique<EmuWindow_Android>(g_aux_native_window, g_vulkan_library, true);
    }
    system.Renderer().SetAuxWindow(g_aux_window.get());
    system.GPU().DSModAux().SetHapticSink(PlayAuxHaptic);
    LOG_INFO(Frontend, "DSMod aux surface attached");
}

// Eden Duo Stage 2: applies whichever inline-layout wish is current (the hidden developer switch
// wins when it is on) to AuxRouting. g_aux_mutex held; a no-op before the renderer exists --
// OnRendererReady calls this once it does, and the two JNI setters below call it again on every
// later change so rotation/resize/fold/divider-drag updates reach a live game immediately.
void ApplyInlineLayoutLocked() {
    if (!g_aux_renderer_ready) {
        return;
    }
    auto& aux = SessionSystem().GPU().DSModAux();
    if (g_inline_dual_screen_enabled.load()) {
        aux.SetInlineLayoutSideSplit(true, g_inline_dual_screen_landscape.load());
        return;
    }
    using VideoCore::DSMod::FracRect;
    aux.SetInlineLayout(
        g_duo_layout_enabled.load(),
        FracRect{g_duo_layout_game_x.load(), g_duo_layout_game_y.load(),
                 g_duo_layout_game_w.load(), g_duo_layout_game_h.load()},
        FracRect{g_duo_layout_companion_x.load(), g_duo_layout_companion_y.load(),
                 g_duo_layout_companion_w.load(), g_duo_layout_companion_h.load()});
}

void DetachAuxWindowLocked() {
    // g_aux_mutex held. Synchronous: the renderer lets go of the surface before this returns.
    if (g_aux_window && g_aux_renderer_ready) {
        SessionSystem().Renderer().SetAuxWindow(nullptr);
    }
    g_aux_window.reset();
}

void ReleaseAuxNativeWindowLocked() {
    if (g_aux_native_window) {
        ANativeWindow_release(g_aux_native_window);
        g_aux_native_window = nullptr;
    }
}

// A surface that arrives before the renderer exists is kept and attached by OnRendererReady as
// soon as the renderer is up; one that arrives later is attached right here, whether or not
// RunEmulation has started yet. Takes over the reference of `native_window`.
void AuxSurfaceChanged(ANativeWindow* native_window) {
    std::scoped_lock lock(g_aux_mutex);
    DetachAuxWindowLocked();
    ReleaseAuxNativeWindowLocked();
    g_aux_native_window = native_window;
    AttachAuxWindowLocked();
}

void AuxSurfaceDestroyed() {
    std::scoped_lock lock(g_aux_mutex);
    DetachAuxWindowLocked();
    ReleaseAuxNativeWindowLocked();
    // Eden Duo Stage 4 (RESEARCH.md I4): a lost aux surface (display eject, onStop, a
    // Presentation the window manager dismissed) must not leave a stale companion touch point
    // latched against a dsm:u service the game keeps receiving.
    SetAuxTouchLocked({});
}

// g_aux_mutex held.
bool SetAuxTouchLocked(std::span<const VideoCore::DSMod::AuxTouchPoint> points) {
    if (!g_aux_renderer_ready) {
        return false;
    }
    SessionSystem().GPU().DSModAux().SetTouch(points);
    return true;
}

/// Forwards second-screen touch to the running game; false while no game renderer exists.
bool SetAuxTouch(std::span<const VideoCore::DSMod::AuxTouchPoint> points) {
    // Under g_aux_mutex the GPU cannot be torn down (ShutdownEmulation clears
    // g_aux_renderer_ready under the same lock before ShutdownMainProcess).
    std::scoped_lock lock(g_aux_mutex);
    return SetAuxTouchLocked(points);
}

// Eden Duo: 1 when the running game shows the idle page (no usable companion package), 0 when it
// does not, -1 when no game is loaded yet (ask again later).
int CompanionIsIdle() {
    // g_mod_runtime_live guards the runtime's lifetime only (it is cleared under g_aux_mutex
    // before ShutdownMainProcess destroys the runtime); the aux surface plays no part.
    std::scoped_lock lock(g_aux_mutex);
    if (!g_mod_runtime_live) {
        return -1;
    }
    const auto* const runtime = SessionSystem().ModRuntime();
    return runtime != nullptr && runtime->IsIdlePage() ? 1 : 0;
}

// Developer builds only: an Android app inherits no shell environment, so the EDEN_DSMOD_*
// switches the desktop takes from the environment (profiling, self-checks, kill switches) are read
// from <user dir>/dsmod_env.txt instead -- one KEY=VALUE per line, '#' starts a comment. Applied
// once, when the app directory is set and before any emulation thread exists (setenv is not safe
// against a concurrent getenv); edits take effect after the app restarts.
void ApplyDsmodEnvFile() {
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "dsmod_env.txt";
    std::FILE* const file = std::fopen(path.string().c_str(), "r");
    if (file == nullptr) {
        return;
    }
    const auto trim = [](std::string text) {
        constexpr const char* space = " \t\r\n";
        const auto first = text.find_first_not_of(space);
        if (first == std::string::npos) {
            return std::string{};
        }
        return text.substr(first, text.find_last_not_of(space) - first + 1);
    };
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#') {
            continue;
        }
        const auto eq = text.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(text.substr(0, eq));
        const std::string value = trim(text.substr(eq + 1));
        if (key.empty()) {
            continue;
        }
        setenv(key.c_str(), value.c_str(), 1);
        LOG_INFO(Frontend, "DSMod env file: {}={}", key, value);
    }
    std::fclose(file);
#endif
}

} // namespace

namespace DSModSession {

void ApplySecondScreenOptions() {
    using Options = VideoCore::DSMod::SecondScreenOptions;
    // companion_ratio is a SwitchableSetting: GetValue() is the game's own value when its
    // per-game config was applied before boot.
    Options::companion_ratio.store(
        static_cast<u32>(std::max(0, AndroidSettings::values.companion_ratio.GetValue())));
    const s32 no_companion_override = g_no_companion_override.load();
    const s32 no_companion = no_companion_override >= 0
                                 ? no_companion_override
                                 : std::max(0, AndroidSettings::values.no_companion.GetValue());
    // Eden Duo: "App" (3) is Off for the runtime; the app itself is opened by the frontend.
    Options::no_companion.store(static_cast<u32>(
        no_companion == 3 ? static_cast<s32>(VideoCore::DSMod::NoCompanion::Off) : no_companion));
}

void OnAppDirectorySet() {
    static std::once_flag dsmod_env_applied;
    std::call_once(dsmod_env_applied, ApplyDsmodEnvFile);
}

void OnPerfStats([[maybe_unused]] const Core::PerfStatsResults& stats) {
    // Reuse the result requested by the Android FPS overlay. Do not make another resetting query:
    // other frontends and overlays own the cadence of these cumulative counters.
#if EDEN_DSMOD_BUILD_DEV_TOOLS
    if (SessionSystem().ModRuntime()) {
        // EDEN_DSMOD_PROFILE turns the 5 s line on or off; unset means on in developer builds
        // (dsmod_env.txt) and off in a published build, which writes no periodic log lines.
        static const bool profile = [] {
            const char* value = std::getenv("EDEN_DSMOD_PROFILE");
            if (value == nullptr) {
                return EDEN_DSMOD_BUILD_DEV_TOOLS != 0;
            }
            return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0;
        }();
        static auto report_at = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (profile && now - report_at >= std::chrono::seconds{5}) {
            LOG_INFO(Frontend,
                     "DSMod performance (existing Android stats, 5s): game={:.1f} FPS, "
                     "system={:.1f} FPS, frame={:.2f}ms, speed={:.1f}%",
                     stats.average_game_fps, stats.system_fps, stats.frametime * 1000.0,
                     stats.emulation_speed * 100.0);
            report_at = now;
        }
    }
#endif
}

void OnEmulationLoaded() {
    std::scoped_lock aux_lock(g_aux_mutex);
    g_mod_runtime_live = true; // Load built the mod runtime (Eden Duo CompanionIsIdle)
}

void OnRendererReady(std::shared_ptr<Common::DynamicLibrary> vulkan_library) {
    // The renderer exists from here on; a second-screen surface may already be waiting.
    std::scoped_lock aux_lock(g_aux_mutex);
    g_vulkan_library = std::move(vulkan_library);
    g_aux_renderer_ready = true;
    AttachAuxWindowLocked();
    // Eden Duo Stage 1/2: the GPU (and AuxRouting inside it) exists from here on too.
    ApplyInlineLayoutLocked();
    if (g_inline_dual_screen_enabled.load()) {
        LOG_INFO(Frontend, "DSMod inline dual screen enabled (hidden switch, landscape={})",
                 g_inline_dual_screen_landscape.load());
    } else if (g_duo_layout_enabled.load()) {
        LOG_INFO(Frontend, "DSMod inline dual screen enabled (geometry-driven layout)");
    }
}

void OnShutdownEmulation(bool program_change_pending) {
    if (!program_change_pending) {
        // The game is gone; a later applySettings must not use its No Companion.
        g_no_companion_override.store(-1);
    }
}

void OnBeforeShutdownMainProcess() {
    // The aux surface itself stays cached for the next game.
    std::scoped_lock aux_lock(g_aux_mutex);
    DetachAuxWindowLocked();
    g_aux_renderer_ready = false;
    g_mod_runtime_live = false;
    g_vulkan_library.reset();
}

void OnRenderWindowTeardown() {
    std::scoped_lock aux_lock(g_aux_mutex);
    g_aux_window.reset();
    g_aux_renderer_ready = false;
    g_mod_runtime_live = false;
    g_vulkan_library.reset();
}

} // namespace DSModSession

extern "C" {

void Java_org_yuzu_yuzu_1emu_NativeLibrary_auxSurfaceChanged(JNIEnv* env, jobject instance,
                                                             jobject surf) {
    ANativeWindow* const window = surf != nullptr ? ANativeWindow_fromSurface(env, surf) : nullptr;
    if (window == nullptr) {
        AuxSurfaceDestroyed();
        return;
    }
    // Detach + release of any previous aux surface, the swap and the attach are one locked step.
    AuxSurfaceChanged(window);
}

void Java_org_yuzu_yuzu_1emu_NativeLibrary_auxSurfaceDestroyed(JNIEnv* env, jobject instance) {
    AuxSurfaceDestroyed();
}

jint Java_org_yuzu_yuzu_1emu_NativeLibrary_isCompanionIdle(JNIEnv* env, jobject instance) {
    return static_cast<jint>(CompanionIsIdle());
}

void Java_org_yuzu_yuzu_1emu_NativeLibrary_setNoCompanionOverride(JNIEnv* env, jobject instance,
                                                                  jint value) {
    g_no_companion_override.store(value >= 0 && value <= 3 ? static_cast<s32>(value) : -1);
}

// Eden Duo Stage 1: see g_inline_dual_screen_enabled above. Safe to call any time -- stores the
// wish and, if the renderer already exists, applies it immediately too (Stage 2: a developer may
// flip this mid-session); OnRendererReady applies it otherwise once it exists.
void Java_org_yuzu_yuzu_1emu_NativeLibrary_setInlineDualScreen(JNIEnv* env, jobject instance,
                                                                jboolean enabled,
                                                                jboolean landscape) {
    g_inline_dual_screen_enabled.store(enabled != JNI_FALSE);
    g_inline_dual_screen_landscape.store(landscape != JNI_FALSE);
    std::scoped_lock lock(g_aux_mutex);
    ApplyInlineLayoutLocked();
}

// Eden Duo Stage 2: DualScreenGeometry's own game/companion rects, as fractions (0..1) of the
// main window on each axis -- see FracRect/SetInlineLayout (aux_routing.h). Replaces the fixed
// 60/40 split above for Simulated's real, per-orientation/fold/divider layout; the hidden
// developer switch still overrides it when that is on (ApplyInlineLayoutLocked). Safe to call any
// time, including live while a game runs -- rotation, resize, fold change and divider drag all
// go through this on every change.
void Java_org_yuzu_yuzu_1emu_NativeLibrary_setDualScreenInlineLayout(
    JNIEnv* env, jobject instance, jboolean enabled, jfloat game_x, jfloat game_y, jfloat game_w,
    jfloat game_h, jfloat companion_x, jfloat companion_y, jfloat companion_w,
    jfloat companion_h) {
    g_duo_layout_enabled.store(enabled != JNI_FALSE);
    g_duo_layout_game_x.store(game_x);
    g_duo_layout_game_y.store(game_y);
    g_duo_layout_game_w.store(game_w);
    g_duo_layout_game_h.store(game_h);
    g_duo_layout_companion_x.store(companion_x);
    g_duo_layout_companion_y.store(companion_y);
    g_duo_layout_companion_w.store(companion_w);
    g_duo_layout_companion_h.store(companion_h);
    std::scoped_lock lock(g_aux_mutex);
    ApplyInlineLayoutLocked();
}

// Eden Duo Stage 2: dual-screen Mode "Off" -- set by the frontend before a game launches (true
// for Off, false otherwise, every launch); Core::Mods::SetCompanionsDisabledForLaunch is already
// on the branch (mod_launch_gate.h/.cpp, Stage 3's gate consumes it before the load). This is
// just the JNI binding the frontend calls through.
void Java_org_yuzu_yuzu_1emu_NativeLibrary_setCompanionsDisabledForLaunch(JNIEnv* env,
                                                                          jobject instance,
                                                                          jboolean disabled) {
    Core::Mods::SetCompanionsDisabledForLaunch(disabled != JNI_FALSE);
}

jint Java_org_yuzu_yuzu_1emu_NativeLibrary_dualScreenRuntimeVersion(JNIEnv* env,
                                                                     jobject instance) {
    return static_cast<jint>(Core::Mods::DualScreenRuntimeVersion);
}

void Java_org_yuzu_yuzu_1emu_NativeLibrary_setDualScreenCodeDirectory(JNIEnv* env,
                                                                      jobject instance,
                                                                      jstring directory) {
    Core::Mods::SetModuleCacheDirectory(Common::Android::GetJString(env, directory));
}

// Eden Duo: areKeysPresent without CreateFactories, which rescans every external content
// container. MainActivity's startup check runs right after DirectoryInitialization built them.
jboolean Java_org_yuzu_yuzu_1emu_NativeLibrary_areKeysLoaded(JNIEnv* env, jobject jobj) {
    return static_cast<jboolean>(ContentManager::AreKeysPresent());
}

// Eden Duo: the base title ids a content container (an update/DLC NSP or XCI) carries content
// for, so the library scan can remember which containers a game needs registered at boot.
jlongArray Java_org_yuzu_yuzu_1emu_NativeLibrary_getContainerBaseTitleIds(JNIEnv* env,
                                                                         jobject jobj,
                                                                         jstring jpath) {
    std::set<u64> base_title_ids;
    const auto file = SessionSystem().GetFilesystem()->OpenFile(
        Common::Android::GetJString(env, jpath), FileSys::OpenMode::Read);
    if (file) {
        auto nsp = std::make_shared<FileSys::NSP>(file);
        if (nsp->GetStatus() != Loader::ResultStatus::Success) {
            FileSys::XCI xci{file};
            nsp = xci.GetStatus() == Loader::ResultStatus::Success ? xci.GetSecurePartitionNSP()
                                                                   : nullptr;
        }
        if (nsp) {
            for (const auto& [title_id, ncas] : nsp->GetNCAs()) {
                base_title_ids.insert(FileSys::GetBaseTitleID(title_id));
            }
        }
    }

    const std::vector<jlong> ids(base_title_ids.begin(), base_title_ids.end());
    jlongArray result = env->NewLongArray(static_cast<jsize>(ids.size()));
    env->SetLongArrayRegion(result, 0, static_cast<jsize>(ids.size()), ids.data());
    return result;
}

// Eden Duo: a game's icon straight from its own file. GameMetadata.getIcon also builds the
// update/DLC metadata (PatchManager), which waits for the external content scan, so the library's
// first icons appeared only after that scan finished. The icon itself always comes from the base
// file (Loader::ReadIcon), so nothing else is needed here. Empty when the file has no icon.
jbyteArray Java_org_yuzu_yuzu_1emu_NativeLibrary_readGameIcon(JNIEnv* env, jobject jobj,
                                                              jstring jpath) {
    std::vector<u8> icon;
    auto& system = EmulationSession::GetInstance().System();
    const auto file =
        Core::GetGameFileFromPath(system.GetFilesystem(), Common::Android::GetJString(env, jpath));
    if (file != nullptr) {
        if (const auto loader = Loader::GetLoader(system, file, 0, 0); loader != nullptr) {
            loader->ReadIcon(icon);
        }
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(icon.size()));
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(icon.size()),
                            reinterpret_cast<const jbyte*>(icon.data()));
    return result;
}

// DSMod: the directory the add-on scanner (PatchManager and the dual-screen runtime) reads for
// this title, so the Android installers write exactly where the scan looks. Empty when the
// title has no mod root (id 0, or an update id).
jstring Java_org_yuzu_yuzu_1emu_NativeLibrary_getModLoadDirectory(JNIEnv* env, jobject jobj,
                                                                  jstring jprogramId) {
    const auto program_id = EmulationSession::GetProgramId(env, jprogramId);
    const auto dir = SessionSystem().GetFileSystemController().GetModificationLoadRoot(program_id);
    return Common::Android::ToJString(env, dir ? dir->GetFullPath() : std::string{});
}

void Java_org_yuzu_yuzu_1emu_features_input_NativeInput_onAuxTouchEvent(
    JNIEnv* env, jobject j_obj, jintArray j_ids, jfloatArray j_xs, jfloatArray j_ys, jint j_count,
    jint j_start_mask) {
    const size_t count = std::min<size_t>(static_cast<size_t>(std::max(j_count, 0)),
                                          VideoCore::DSMod::AuxRouting::MaxTouch);
    if (count == 0) {
        SetAuxTouch({});
        return;
    }
    // The arrays may be longer than `count` (the caller reuses fixed-size buffers).
    if (env->GetArrayLength(j_ids) < static_cast<jsize>(count) ||
        env->GetArrayLength(j_xs) < static_cast<jsize>(count) ||
        env->GetArrayLength(j_ys) < static_cast<jsize>(count)) {
        return;
    }
    std::array<jint, VideoCore::DSMod::AuxRouting::MaxTouch> ids{};
    std::array<jfloat, VideoCore::DSMod::AuxRouting::MaxTouch> xs{};
    std::array<jfloat, VideoCore::DSMod::AuxRouting::MaxTouch> ys{};
    env->GetIntArrayRegion(j_ids, 0, static_cast<jsize>(count), ids.data());
    env->GetFloatArrayRegion(j_xs, 0, static_cast<jsize>(count), xs.data());
    env->GetFloatArrayRegion(j_ys, 0, static_cast<jsize>(count), ys.data());

    std::array<VideoCore::DSMod::AuxTouchPoint, VideoCore::DSMod::AuxRouting::MaxTouch> points{};
    for (size_t i = 0; i < count; ++i) {
        points[i] = VideoCore::DSMod::AuxTouchPoint{
            .finger_id = static_cast<u32>(ids[i]),
            .x = static_cast<u32>(std::max(0.0f, xs[i])),
            .y = static_cast<u32>(std::max(0.0f, ys[i])),
            .attributes = (j_start_mask & (1 << i)) != 0 ? 1u : 0u,
            .delta_ns = 0,
        };
    }
    SetAuxTouch(std::span<const VideoCore::DSMod::AuxTouchPoint>{points.data(), count});
}

} // extern "C"
