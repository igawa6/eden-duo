// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include "common/settings_enums.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include <fmt/ostream.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/string_util.h"
#include <filesystem>
#include "core/frontend/framebuffer_layout.h"
#include "core/perf_stats.h"
#include <array>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include "input_common/drivers/keyboard.h"
#include "common/param_package.h"
#include "common/settings_input.h"
#include "hid_core/hid_core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "video_core/gpu.h"
#include <span>
#include <sstream>
#include <vector>
#include <fstream>
#include "core/file_sys/vfs/vfs_vector.h"
#include <atomic>
#include <csignal>
#include <stb_image_write.h>
#include "video_core/dsmod/aux_routing.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/submission_package.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "input_common/main.h"
#include "network/network.h"
#include "sdl_config.h"
#include "video_core/renderer_base.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"
#ifdef HAS_OPENGL
#include "yuzu_cmd/emu_window/emu_window_sdl3_gl.h"
#endif
#include "yuzu_cmd/emu_window/emu_window_sdl3_null.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_aux.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_vk.h"

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#include <windows.h>
#include <shellapi.h>
#include "common/windows/timer_resolution.h"
#endif

#undef _UNICODE
#include <getopt.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _WIN32
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

static void PrintHelp(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " [options] <filename>\n"
                 "-c, --config          Load the specified configuration file\n"
                 "-f, --fullscreen      Start in fullscreen mode\n"
                 "-g, --game            File path of the game to load\n"
                 "-h, --help            Display this help and exit\n"
                 "-m, --multiplayer=nick:password@address:port"
                 " Nickname, password, address and port for multiplayer\n"
                 "-p, --program         Pass following string as arguments to executable\n"
                 "-u, --user            Select a specific user profile from 0 to 7\n"
                 "-d, --debug           Run the GDB stub on a port from 1 to 65535\n"
                 "-v, --version         Output version information and exit\n";
}

static void PrintVersion() {
    std::cout << "Eden " << Common::g_scm_branch << " " << Common::g_scm_desc << std::endl;
}

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::IpCollision:
        LOG_ERROR(Network, "You tried to use the same fake IP-Address as another user that is "
                           "connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    case Network::RoomMember::Error::UnknownError:
        LOG_ERROR(Network, "UnknownError");
        break;
    case Network::RoomMember::Error::PermissionDenied:
        LOG_ERROR(Network, "PermissionDenied");
        break;
    case Network::RoomMember::Error::NoSuchUser:
        LOG_ERROR(Network, "NoSuchUser");
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message = [&]() {
        switch (msg.type) {
        case Network::IdMemberJoin:
            return fmt::format("{} has joined", msg.nickname);
        case Network::IdMemberLeave:
            return fmt::format("{} has left", msg.nickname);
        case Network::IdMemberKicked:
            return fmt::format("{} has been kicked", msg.nickname);
        case Network::IdMemberBanned:
            return fmt::format("{} has been banned", msg.nickname);
        case Network::IdAddressUnbanned:
            return fmt::format("{} has been unbanned", msg.nickname);
        default:
            return std::string{};
        }
    }();
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

struct SdlState {
    Core::System system{};
    InputCommon::InputSubsystem input_subsystem{};
    std::unique_ptr<EmuWindow_SDL3> emu_window;
    std::unique_ptr<AuxWindow_SDL3> aux_window;
};


// DSMod: pull the two files an IL2CPP dump needs out of a game -- the executable ("main", which is
// the IL2CPP binary on Switch) and global-metadata.dat from the romfs. Eden already has all the
// NCA crypto, so this beats standing up a separate extraction toolchain.
static FileSys::VirtualFile FindFileRecursive(const FileSys::VirtualDir& dir,
                                              std::string_view wanted, std::string& path_out,
                                              const std::string& prefix = "") {
    if (!dir) {
        return {};
    }
    for (const auto& file : dir->GetFiles()) {
        if (file->GetName() == wanted) {
            path_out = prefix + "/" + file->GetName();
            return file;
        }
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (auto found = FindFileRecursive(sub, wanted, path_out, prefix + "/" + sub->GetName())) {
            return found;
        }
    }
    return {};
}

static bool WriteVfsFile(const FileSys::VirtualFile& file, const std::filesystem::path& out) {
    if (!file) {
        return false;
    }
    std::vector<u8> bytes = file->ReadAllBytes();
    std::ofstream stream(out, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    LOG_INFO(Frontend, "dumped {} ({} bytes)", out.string(), bytes.size());
    return true;
}

/// Writes a whole VFS directory out to disk. Used by --dump-romfs so a mod author can look at the
/// game's own files (their own dump, staying on their own machine).
static u64 WriteVfsTree(const FileSys::VirtualDir& dir, const std::filesystem::path& out) {
    if (!dir) {
        return 0;
    }
    u64 written = 0;
    std::filesystem::create_directories(out);
    for (const auto& file : dir->GetFiles()) {
        if (WriteVfsFile(file, out / file->GetName())) {
            written += file->GetSize();
        }
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        written += WriteVfsTree(sub, out / sub->GetName());
    }
    return written;
}

static int DumpIl2Cpp(const std::string& game_path, const std::string& out_dir,
                      bool whole_romfs = false) {
    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto file = vfs->OpenFile(game_path, FileSys::OpenMode::Read);
    if (!file) {
        LOG_CRITICAL(Frontend, "cannot open {}", game_path);
        return 1;
    }
    std::filesystem::create_directories(out_dir);

    // An XCI is a cartridge image; its secure partition is an NSP, so unwrap it and share the
    // rest of the path.
    std::shared_ptr<FileSys::NSP> nsp_ptr;
    if (game_path.ends_with(".xci") || game_path.ends_with(".XCI")) {
        auto xci = std::make_shared<FileSys::XCI>(file);
        if (xci->GetStatus() != Loader::ResultStatus::Success) {
            LOG_CRITICAL(Frontend, "not a usable XCI: status {}", static_cast<int>(xci->GetStatus()));
            return 1;
        }
        nsp_ptr = xci->GetSecurePartitionNSP();
    } else {
        nsp_ptr = std::make_shared<FileSys::NSP>(file);
    }
    if (!nsp_ptr || nsp_ptr->GetStatus() != Loader::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "not a usable package: status {}",
                     nsp_ptr ? static_cast<int>(nsp_ptr->GetStatus()) : -1);
        return 1;
    }
    FileSys::NSP& nsp = *nsp_ptr;
    const u64 title_id = nsp.GetProgramTitleID();
    LOG_INFO(Frontend, "title {:016X}", title_id);

    // Update packages do not answer GetExeFS()/GetNCA() for their own title id, so just walk the
    // NCAs and take the Program one.
    std::shared_ptr<FileSys::NCA> nca;
    for (const auto& candidate : nsp.GetNCAsCollapsed()) {
        LOG_INFO(Frontend, "  NCA title {:016X} type {} status {} rights_id_set {}",
                 candidate->GetTitleId(), static_cast<int>(candidate->GetType()),
                 static_cast<int>(candidate->GetStatus()),
                 candidate->GetRightsId() != FileSys::RightsId{});
        if (candidate->GetType() == FileSys::NCAContentType::Program) {
            nca = candidate;
        }
    }
    if (!nca) {
        LOG_WARNING(Frontend, "no program NCA in this package");
        return 1;
    }
    if (const auto exefs = nca->GetExeFS()) {
        for (const auto& f : exefs->GetFiles()) {
            WriteVfsFile(f, std::filesystem::path(out_dir) / f->GetName());
        }
    } else {
        LOG_WARNING(Frontend, "program NCA has no exefs");
    }
    const auto romfs = nca->GetRomFS();
    if (!romfs) {
        LOG_WARNING(Frontend, "program NCA has no romfs");
        return 0;
    }
    const auto romfs_dir = FileSys::ExtractRomFS(romfs);
    if (whole_romfs) {
        const u64 written = WriteVfsTree(romfs_dir, std::filesystem::path(out_dir) / "romfs");
        LOG_INFO(Frontend, "romfs extracted to {}/romfs ({} MiB)", out_dir, written / 0x100000);
        return 0;
    }
    std::string found_at;
    if (auto meta = FindFileRecursive(romfs_dir, "global-metadata.dat", found_at)) {
        LOG_INFO(Frontend, "global-metadata.dat at romfs{}", found_at);
        WriteVfsFile(meta, std::filesystem::path(out_dir) / "global-metadata.dat");
    } else {
        LOG_WARNING(Frontend, "global-metadata.dat not found in romfs; romfs root holds:");
        if (romfs_dir) {
            for (const auto& d : romfs_dir->GetSubdirectories()) {
                LOG_WARNING(Frontend, "  dir  {}", d->GetName());
            }
            for (const auto& f : romfs_dir->GetFiles()) {
                LOG_WARNING(Frontend, "  file {} ({} bytes)", f->GetName(), f->GetSize());
            }
        }
    }
    return 0;
}


// DSMod: install a package to NAND so updates apply. Uses Eden's own installer, which imports the
// ticket as well -- dropping NCAs into the registered directory by hand would not.
static int InstallPackage(const std::string& path) {
    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    auto file = vfs->OpenFile(path, FileSys::OpenMode::Read);
    if (!file) {
        LOG_CRITICAL(Frontend, "cannot open {}", path);
        return 1;
    }
    Core::System system;
    system.Initialize();
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(vfs);
    system.GetFileSystemController().CreateFactories(*vfs);

    auto* const cache = system.GetFileSystemController().GetUserNANDContents();
    if (cache == nullptr) {
        LOG_CRITICAL(Frontend, "no user NAND content cache");
        return 1;
    }
    const auto copy = [](const FileSys::VirtualFile& src, const FileSys::VirtualFile& dst,
                         std::size_t block) { return FileSys::VfsRawCopy(src, dst, block); };

    FileSys::InstallResult result{};
    if (path.ends_with(".xci") || path.ends_with(".XCI")) {
        FileSys::XCI xci{file};
        result = cache->InstallEntry(xci, true, copy);
    } else {
        FileSys::NSP nsp{file};
        result = cache->InstallEntry(nsp, true, copy);
    }
    LOG_INFO(Frontend, "install result {}", static_cast<int>(result));
    return result == FileSys::InstallResult::Success ? 0 : 1;
}


// DSMod: take screenshots from inside the emulator. Wayland refuses programmatic screen capture,
// so `kill -USR1 <pid>` is the only way to see what a mod page actually renders here.
static std::atomic<bool> g_screenshot_requested{false};
static std::string g_screenshot_prefix{"eden-shot"};
/// True once a real aux window presents the second screen (--aux-window). Without one
/// (--aux-virtual) nothing ever draws a GPU-composited page, see TakeScreenshots.
static std::atomic<bool> g_aux_window_presents{false};

static void ScreenshotSignalHandler(int) {
    g_screenshot_requested.store(true);
}

static void WritePng(const std::string& path, const void* rgba, u32 width, u32 height) {
    if (stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba,
                       static_cast<int>(width) * 4) != 0) {
        LOG_INFO(Frontend, "wrote {} ({}x{})", path, width, height);
    } else {
        LOG_ERROR(Frontend, "failed to write {}", path);
    }
}

static void TakeScreenshots(Core::System& system) {
    // Second screen first: it is the mod runtime's own canvas, no GPU round trip needed.
    auto& aux = system.GPU().DSModAux();
    std::vector<u32> ui;
    u32 ui_w{}, ui_h{};
    u64 serial{};
    if (aux.TakeUi(ui, ui_w, ui_h, serial) && !ui.empty()) {
        // The canvas is kept in upload byte order (R and B swapped for the GPU); undo that.
        for (auto& pixel : ui) {
            pixel = (pixel & 0xFF00FF00u) | ((pixel & 0x00FF0000u) >> 16) |
                    ((pixel & 0x000000FFu) << 16);
        }
        WritePng(g_screenshot_prefix + "-screen2.png", ui.data(), ui_w, ui_h);
        // A GPU-composite page (manifest flag gpu_composite / EDEN_DSMOD_GPU_COMPOSITE=1) is
        // published as quads + textures that only the renderer composites, on the aux window. The
        // canvas above is then just the last canvas-path publish (e.g. the loading page from
        // before the map was ready) -- NOT what the panel shows. Only the aux window path with
        // EDEN_DSMOD_GPU_READBACK=1 writes the composited frame back into it.
        const bool readback = g_aux_window_presents.load() &&
                              std::getenv("EDEN_DSMOD_GPU_READBACK") != nullptr;
        if (aux.HasComposite() && !readback) {
            LOG_WARNING(Frontend,
                        "{}-screen2.png is STALE: the second screen is GPU-composited and this "
                        "capture holds only the last canvas publish. Capture it with --aux-window "
                        "and EDEN_DSMOD_GPU_READBACK=1, or run the canvas path with "
                        "EDEN_DSMOD_GPU_COMPOSITE=0",
                        g_screenshot_prefix);
        }
    } else {
        LOG_INFO(Frontend, "second screen has no mod UI to capture");
    }

    auto layout = system.Renderer().GetRenderWindow().GetFramebufferLayout();
    if (layout.width == 0 || layout.height == 0) {
        layout = Layout::DefaultFrameLayout(1280, 720);
    }
    LOG_INFO(Frontend, "capturing primary screen at {}x{}", layout.width, layout.height);
    auto* const pixels = new u32[static_cast<size_t>(layout.width) * layout.height];
    system.Renderer().RequestScreenshot(
        pixels,
        [pixels, layout](bool) {
            WritePng(g_screenshot_prefix + "-screen1.png", pixels, layout.width, layout.height);
            delete[] pixels;
        },
        layout);
}


// DSMod: press a controller button from outside the process. Wayland gives no way to synthesise
// input into the emulator window, so `kill -USR2 <pid>` presses whatever button is named in
// <prefix>.btn (default A) -- enough to get through a title screen headlessly.
static std::atomic<bool> g_button_requested{false};
static std::chrono::steady_clock::time_point g_button_until{};
static std::chrono::steady_clock::time_point g_wait_until{};
static bool g_button_held{false};
static bool g_tap_active{false};
static std::chrono::steady_clock::time_point g_tap_until{};
static std::vector<int> g_button_keys;
static std::vector<InputCommon::VirtualGamepad::VirtualButton> g_button_all;
static InputCommon::VirtualGamepad::VirtualButton g_button_current{
    InputCommon::VirtualGamepad::VirtualButton::ButtonA};

static void ButtonSignalHandler(int) {
    g_button_requested.store(true);
}

/// Maps a script button name to the player-1 controller binding, so synthetic presses go through
/// whatever input Eden is actually configured with (keyboard by default on desktop).
static int NativeButtonFromName(const std::string& name) {
    using NB = Settings::NativeButton::Values;
    static const std::unordered_map<std::string, int> kMap{
        {"A", NB::A},           {"B", NB::B},          {"X", NB::X},
        {"Y", NB::Y},           {"PLUS", NB::Plus},    {"MINUS", NB::Minus},
        {"DUP", NB::DUp},       {"DDOWN", NB::DDown},  {"DLEFT", NB::DLeft},
        {"DRIGHT", NB::DRight}, {"L", NB::L},          {"R", NB::R},
        {"ZL", NB::ZL},         {"ZR", NB::ZR},        {"HOME", NB::Home},
    };
    std::string upper = name;
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return std::toupper(c); });
    const auto it = kMap.find(upper);
    return it == kMap.end() ? static_cast<int>(NB::A) : it->second;
}

/// Returns the keyboard scancode bound to a player-1 button, or -1 when it is not a keyboard bind.
static int KeyCodeForButton(const std::string& name) {
    const auto& players = Settings::values.players.GetValue();
    if (players.empty()) {
        return -1;
    }
    const auto& binding = players[0].buttons[static_cast<size_t>(NativeButtonFromName(name))];
    Common::ParamPackage param{binding};
    if (param.Get("engine", std::string{}) != "keyboard") {
        return -1;
    }
    return param.Get("code", -1);
}

static InputCommon::VirtualGamepad::VirtualButton ButtonFromName(const std::string& name) {
    using VB = InputCommon::VirtualGamepad::VirtualButton;
    if (name == "B") return VB::ButtonB;
    if (name == "X") return VB::ButtonX;
    if (name == "Y") return VB::ButtonY;
    if (name == "Plus") return VB::ButtonPlus;
    if (name == "Minus") return VB::ButtonMinus;
    if (name == "DUp") return VB::ButtonUp;
    if (name == "DDown") return VB::ButtonDown;
    if (name == "DLeft") return VB::ButtonLeft;
    if (name == "DRight") return VB::ButtonRight;
    if (name == "L") return VB::TriggerL;
    if (name == "R") return VB::TriggerR;
    if (name == "ZL") return VB::TriggerZL;
    if (name == "ZR") return VB::TriggerZR;
    if (name == "Home") return VB::ButtonHome;
    return VB::ButtonA;
}

struct BtnStep {
    std::string name;
    int frames;   ///< duration in milliseconds
    float x{};    ///< tap position, normalised to the aux screen
    float y{};
    float x2{};   ///< drag: where the finger ends up. pinch: start/end finger separation.
    float y2{};
    std::string arg;  ///< which button, for hold/release
};
// A gesture is not an event but a movement, so it has to be played out over several polls
// rather than written once. Kind 1 = drag one finger, kind 2 = pinch two.
static int g_gesture_kind{0};
static std::chrono::steady_clock::time_point g_gesture_start{};
static std::chrono::milliseconds g_gesture_len{};
static BtnStep g_gesture_step{};
static bool g_gesture_first{false};

static std::vector<BtnStep> g_btn_queue;
static std::size_t g_btn_index{0};
static int g_btn_poll{0};

/// Drives synthetic input. A script is dropped at "<screenshot-prefix>.btn": one step per line,
/// either "<BUTTON> [ms]" to press, or "wait <ms>" to idle. The file is consumed on read.
static void ServiceButtonRequests(SdlState* state) {
    auto* const pad = state->input_subsystem.GetVirtualGamepad();

    // Screenshots ride this poll too: signals are not reliably delivered in this process.
    static int shot_poll{0};
    if (++shot_poll >= 10) {
        shot_poll = 0;
        std::error_code shot_ec;
        const std::filesystem::path shot{g_screenshot_prefix + ".shot"};
        if (std::filesystem::exists(shot, shot_ec)) {
            std::filesystem::remove(shot, shot_ec);
            g_screenshot_requested.store(true);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_gesture_kind != 0) {
        auto& aux = state->system.GPU().DSModAux();
        const float width = static_cast<float>(aux.width.load());
        const float height = static_cast<float>(aux.height.load());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                                  g_gesture_start);
        // Rest on the landing spot and again over the end point before lifting, so a consumer
        // polling at 60 Hz sees where the finger went down and where it let go (a drop target).
        constexpr auto Dwell = std::chrono::milliseconds(80);
        const auto moving = elapsed - Dwell;
        const float t = moving.count() <= 0 ? 0.0f
                        : g_gesture_len.count() <= 0
                            ? 1.0f
                            : std::clamp(static_cast<float>(moving.count()) /
                                             static_cast<float>(g_gesture_len.count()),
                                         0.0f, 1.0f);
        const u32 attr = g_gesture_first ? 1u : 0u;
        g_gesture_first = false;
        if (g_gesture_kind == 1) {
            const float fx = g_gesture_step.x + (g_gesture_step.x2 - g_gesture_step.x) * t;
            const float fy = g_gesture_step.y + (g_gesture_step.y2 - g_gesture_step.y) * t;
            const VideoCore::DSMod::AuxTouchPoint point{
                .finger_id = 0,
                .x = static_cast<u32>(fx * width),
                .y = static_cast<u32>(fy * height),
                .attributes = attr,
                .delta_ns = 0,
            };
            aux.SetTouch(std::span{&point, 1});
        } else {
            // Two fingers either side of the centre, separating or closing along x.
            const float span = g_gesture_step.x2 + (g_gesture_step.y2 - g_gesture_step.x2) * t;
            const std::array<VideoCore::DSMod::AuxTouchPoint, 2> pair{{
                {0, static_cast<u32>((g_gesture_step.x - span * 0.5f) * width),
                 static_cast<u32>(g_gesture_step.y * height), attr, 0},
                {1, static_cast<u32>((g_gesture_step.x + span * 0.5f) * width),
                 static_cast<u32>(g_gesture_step.y * height), attr, 0},
            }};
            aux.SetTouch(std::span{pair});
        }
        if (elapsed >= g_gesture_len + Dwell * 2) {
            g_gesture_kind = 0;
            aux.SetTouch({});
            g_wait_until = now + std::chrono::milliseconds(150);
        }
        return;
    }
    if (g_tap_active) {
        if (now < g_tap_until) {
            return;
        }
        g_tap_active = false;
        state->system.GPU().DSModAux().SetTouch({});
        g_wait_until = now + std::chrono::milliseconds(200);
        return;
    }
    if (g_button_held) {
        if (now < g_button_until) {
            return;
        }
        g_button_held = false;
        if (pad != nullptr) {
            for (const auto button : g_button_all) {
                pad->SetButtonState(0, button, false);
                pad->SetButtonState(8, button, false);
                for (const std::size_t player : {std::size_t{0}, std::size_t{8}}) {
                    pad->SetStickPosition(player, InputCommon::VirtualGamepad::VirtualStick::Left,
                                          0.0f, 0.0f);
                }
            }
        }
        if (auto* const keyboard = state->input_subsystem.GetKeyboard(); keyboard != nullptr) {
            for (const int key : g_button_keys) {
                keyboard->ReleaseKey(key);
            }
        }
        g_button_keys.clear();
        g_button_all.clear();
        // Leave a gap after every press so the guest samples the release too.
        g_wait_until = now + std::chrono::milliseconds(120);
        return;
    }
    if (now < g_wait_until) {
        return;
    }

    if (g_btn_index < g_btn_queue.size()) {
        const BtnStep step = g_btn_queue[g_btn_index++];
        if (step.name == "wait") {
            g_wait_until = now + std::chrono::milliseconds(step.frames);
            return;
        }
        if (step.name == "tap") {
            auto& aux = state->system.GPU().DSModAux();
            const VideoCore::DSMod::AuxTouchPoint point{
                .finger_id = 0,
                .x = static_cast<u32>(step.x * static_cast<float>(aux.width.load())),
                .y = static_cast<u32>(step.y * static_cast<float>(aux.height.load())),
                .attributes = 1u,  // start of a new touch
                .delta_ns = 0,
            };
            aux.SetTouch(std::span{&point, 1});
            g_tap_until = now + std::chrono::milliseconds(step.frames);
            g_tap_active = true;
            LOG_INFO(Frontend, "aux tap at ({}, {}) for {} ms", point.x, point.y, step.frames);
            return;
        }
        if (step.name == "drag" || step.name == "pinch") {
            g_gesture_kind = step.name == "drag" ? 1 : 2;
            g_gesture_step = step;
            g_gesture_start = now;
            g_gesture_len = std::chrono::milliseconds(step.frames);
            g_gesture_first = true;
            LOG_INFO(Frontend, "aux {} ({},{}) -> ({},{}) over {} ms", step.name, step.x, step.y,
                     step.x2, step.y2, step.frames);
            return;
        }
        if (step.name == "hold" || step.name == "release") {
            // A held button that outlives the step. Pressing two buttons on the same frame is not
            // the same gesture as holding one and tapping another -- Metroid Dread readies a
            // missile while R is down and fires it on Y, and a simultaneous press just shoots the
            // beam. Without this the missile count never moves, and a search that identifies a
            // value by watching it change has nothing to watch.
            const bool down = step.name == "hold";
            const std::string& which = step.arg;
            if (pad != nullptr && !which.empty()) {
                const auto button = ButtonFromName(which);
                for (const std::size_t player : {std::size_t{0}, std::size_t{8}}) {
                    pad->SetButtonState(player, button, down);
                }
                LOG_INFO(Frontend, "virtual button '{}' {}", which, down ? "held" : "released");
            }
            g_wait_until = now + std::chrono::milliseconds(60);
            return;
        }
        if (step.name == "stick") {
            // Hold the left stick for a while. A game that walks on the analog stick ignores the
            // d-pad entirely, so button steps alone can never move the character.
            if (pad != nullptr) {
                for (const std::size_t player : {std::size_t{0}, std::size_t{8}}) {
                    pad->SetStickPosition(player, InputCommon::VirtualGamepad::VirtualStick::Left,
                                          step.x, step.y);
                }
            }
            g_button_held = true;
            g_button_until = now + std::chrono::milliseconds(step.frames);
            LOG_INFO(Frontend, "left stick held at ({:.2f}, {:.2f}) for {} ms", step.x, step.y,
                     step.frames);
            return;
        }
        if (pad == nullptr) {
            LOG_ERROR(Frontend, "virtual button ignored: no virtual gamepad driver");
            g_btn_queue.clear();
            g_btn_index = 0;
            return;
        }
        // One-time diagnostic + repair: the emulated pad must be connected for input to land.
        static bool checked_pad = false;
        if (!checked_pad) {
            checked_pad = true;
            auto& hid = state->system.HIDCore();
            for (const auto id : {Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld}) {
                auto* const controller = hid.GetEmulatedController(id);
                if (controller == nullptr) {
                    continue;
                }
                LOG_INFO(Frontend, "npad {} connected={} style={}", static_cast<int>(id),
                         controller->IsConnected(),
                         static_cast<int>(controller->GetNpadStyleIndex()));
                if (!controller->IsConnected()) {
                    controller->Connect();
                    LOG_INFO(Frontend, "npad {} connected on demand", static_cast<int>(id));
                }
            }
        }

        // "A" presses one button; "DRIGHT+A" holds several at once, which platforming needs.
        g_button_keys.clear();
        g_button_all.clear();
        auto* const keyboard = state->input_subsystem.GetKeyboard();
        for (size_t start = 0; start <= step.name.size();) {
            const size_t plus = step.name.find('+', start);
            const std::string name = step.name.substr(
                start, plus == std::string::npos ? std::string::npos : plus - start);
            if (!name.empty()) {
                const auto button = ButtonFromName(name);
                g_button_all.push_back(button);
                // Both the handheld port and player 1, since which one the game reads depends
                // on setup.
                pad->SetButtonState(0, button, true);
                pad->SetButtonState(8, button, true);
                // Also drive the configured player-1 binding: on desktop that is the keyboard,
                // which is the path the emulated controller actually reads from.
                if (const int key = KeyCodeForButton(name); key >= 0) {
                    g_button_keys.push_back(key);
                    if (keyboard != nullptr) {
                        keyboard->PressKey(key);
                    }
                }
            }
            if (plus == std::string::npos) {
                break;
            }
            start = plus + 1;
        }
        g_button_current = g_button_all.empty()
                               ? InputCommon::VirtualGamepad::VirtualButton::ButtonA
                               : g_button_all.front();
        const int g_button_key = g_button_keys.empty() ? -1 : g_button_keys.front();
        g_button_held = true;
        g_button_until = now + std::chrono::milliseconds(step.frames);
        u64 npad_raw{};
        if (auto* const c = state->system.HIDCore().GetEmulatedController(
                Core::HID::NpadIdType::Player1);
            c != nullptr) {
            npad_raw = static_cast<u64>(c->GetNpadButtons().raw);
        }
        LOG_INFO(Frontend, "virtual button '{}' pressed for {} ms (key {}, npad raw {:#x})",
                 step.name, step.frames, g_button_key, npad_raw);
        return;
    }

    if (++g_btn_poll < 10) {
        return;
    }
    g_btn_poll = 0;
    std::error_code ec;
    const std::filesystem::path script{g_screenshot_prefix + ".btn"};
    if (!std::filesystem::exists(script, ec)) {
        return;
    }
    g_btn_queue.clear();
    g_btn_index = 0;
    if (std::ifstream f(script); f) {
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            BtnStep step{};
            step.frames = 180;  // milliseconds
            if (!(iss >> step.name)) {
                continue;
            }
            if (step.name == "hold" || step.name == "release") {
                iss >> step.arg;
            }
            if (step.name == "drag" || step.name == "pinch") {
                // "drag <x0> <y0> <x1> <y1> [ms]", "pinch <cx> <cy> <span0> <span1> [ms]",
                // all normalised to the aux screen.
                if (!(iss >> step.x >> step.y >> step.x2 >> step.y2)) {
                    continue;
                }
            }
            if (step.name == "tap" || step.name == "stick") {
                // "stick <x> <y> [ms]" -- the left stick, in -1..1. Buttons alone cannot walk a
                // character in a game that moves on the analog stick, which is most of them.
                if (!(iss >> step.x >> step.y)) {
                    continue;
                }
            }
            int frames{};
            if (iss >> frames && frames > 0) {
                step.frames = frames;
            }
            g_btn_queue.push_back(step);
        }
    }
    if (g_btn_queue.empty()) {
        // The writer may still be filling the file; leave it for the next poll rather than
        // consuming an empty read.
        return;
    }
    std::filesystem::remove(script, ec);
    LOG_INFO(Frontend, "virtual input script loaded: {} steps", g_btn_queue.size());
}

extern "C" SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    SdlState* state = new SdlState();
    *appstate = state;

#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
    }
#endif

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();

    int option_index = 0;
#ifdef _WIN32
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        return SDL_APP_FAILURE;
    }
#endif
    std::string filepath;
    std::optional<std::string> config_path{};
    std::string program_args;
    std::optional<int> selected_user{};
    std::optional<u16> override_gdb_port{};
    bool use_multiplayer = false;
    bool fullscreen = false;
    bool aux_window_requested = std::getenv("EDEN_AUX_WINDOW") != nullptr;
    bool aux_virtual_requested = std::getenv("EDEN_AUX_VIRTUAL") != nullptr;
    std::string dump_il2cpp_dir;
    bool dump_whole_romfs = false;
    std::string install_path;
    bool force_null_render = false;
    bool force_single_core = false;
    std::string nickname{};
    std::string password{};
    std::string address{};
    std::string input_profile{};
    std::optional<std::string> log_filter{};
    u16 port = Network::DefaultRoomPort;

    static struct option long_options[] = {
        // clang-format off
        {"debug", no_argument, 0, 'd'},
        {"config", required_argument, 0, 'c'},
        {"fullscreen", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {"game", required_argument, 0, 'g'},
        {"multiplayer", required_argument, 0, 'm'},
        {"program", optional_argument, 0, 'p'},
        {"user", required_argument, 0, 'u'},
        {"version", no_argument, 0, 'v'},
        {"input-profile", no_argument, 0, 'i'},
        {"null-render", no_argument, 0, 'n'},
        {"singlecore", no_argument, 0, 's'},
        {"filter", no_argument, 0, 'x'},
        {"aux-window", no_argument, 0, 'a'},
        {"aux-virtual", no_argument, 0, 'V'},
        {"dump-il2cpp", required_argument, 0, 'D'},
        {"dump-romfs", required_argument, 0, 'R'},
        {"install", required_argument, 0, 'I'},
        {"screenshot-prefix", required_argument, 0, 'S'},
        {0, 0, 0, 0},
        // clang-format on
    };

    while (optind < argc) {
        int arg = getopt_long(argc, argv, "g:fhvcip::c:u:d:D:R:I:S:aVxn", long_options, &option_index);
        if (arg != -1) {
            switch (char(arg)) {
            case 'd':
                override_gdb_port = uint16_t(atoi(optarg));
                break;
            case 'c':
                config_path = optarg;
                break;
            case 'f':
                fullscreen = true;
                LOG_INFO(Frontend, "Starting in fullscreen mode...");
                break;
            case 'h':
                PrintHelp(argv[0]);
                return SDL_APP_FAILURE;
            case 'g':
                filepath = std::string(optarg);
                break;
            case 'i': {
                input_profile = std::string(optarg);
                break;
            }
            case 'm': {
                use_multiplayer = true;
                const std::string str_arg(optarg);
                // regex to check if the format is nickname:password@ip:port
                // with optional :password
                const std::regex re("^([^:]+)(?::(.+))?@([^:]+)(?::([0-9]+))?$");
                if (!std::regex_match(str_arg, re)) {
                    std::cout << "Wrong format for option --multiplayer\n";
                    PrintHelp(argv[0]);
                    return SDL_APP_FAILURE;
                }

                std::smatch match;
                std::regex_search(str_arg, match, re);
                ASSERT(match.size() == 5);
                nickname = match[1];
                password = match[2];
                address = match[3];
                if (!match[4].str().empty()) {
                    port = u16(std::strtoul(match[4].str().c_str(), nullptr, 0));
                }
                std::regex nickname_re("^[a-zA-Z0-9._\\- ]+$");
                if (!std::regex_match(nickname, nickname_re)) {
                    LOG_ERROR(Frontend, "Nickname is not valid. Must be 4 to 20 alphanumeric characters");
                    return SDL_APP_FAILURE;
                }
                if (address.empty()) {
                    LOG_ERROR(Frontend, "Address to room must not be empty");
                    return SDL_APP_FAILURE;
                }
                break;
            }
            case 'p':
                program_args = argv[optind];
                ++optind;
                break;
            case 'u':
                selected_user = atoi(optarg);
                break;
            case 'v':
                PrintVersion();
                return SDL_APP_FAILURE;
            case 'n':
                force_null_render = true;
                break;
            case 's':
                force_single_core = true;
                break;
            case 'x':
                log_filter = argv[optind];
                ++optind;
                break;
            case 'a':
                aux_window_requested = true;
                break;
            case 'V':
                aux_virtual_requested = true;
                break;
            case 'D':
                dump_il2cpp_dir = optarg;
                break;
            case 'R':
                dump_il2cpp_dir = optarg;
                dump_whole_romfs = true;
                break;
            case 'I':
                install_path = optarg;
                break;
            case 'S':
                g_screenshot_prefix = optarg;
                break;
            }
        } else {
#ifdef _WIN32
            filepath = Common::UTF16ToUTF8(argv_w[optind]);
#else
            filepath = argv[optind];
#endif
            optind++;
        }
    }

    // DSMod: extraction is a CLI utility -- do it before any window or emulation setup, so it
    // works headless and never touches a Core::System.
    if (!dump_il2cpp_dir.empty()) {
        SdlConfig dump_config{config_path};
        std::exit(DumpIl2Cpp(filepath, dump_il2cpp_dir, dump_whole_romfs));
    }
    if (!install_path.empty()) {
        SdlConfig install_config{config_path};
        std::exit(InstallPackage(install_path));
    }

    SdlConfig config{config_path};

    // apply the log_filter setting
    // the logger was initialized before and doesn't pick up the filter on its own
    Common::Log::Filter filter;
    filter.ParseFilterString(log_filter.value_or(Settings::values.log_filter.GetValue()));
    Common::Log::SetGlobalFilter(filter);

    if (!program_args.empty()) {
        Settings::values.program_args = program_args;
    }

    if (!input_profile.empty()) {
        auto& players = Settings::values.players.GetValue();
        players[0].profile_name = input_profile;
    }

    if (selected_user.has_value()) {
        Settings::values.current_user = std::clamp(*selected_user, 0, 7);
    }

    if (override_gdb_port.has_value()) {
        Settings::values.use_gdbstub = true;
        Settings::values.gdbstub_port = *override_gdb_port;
    }

    if (force_single_core) {
        Settings::values.use_multi_core = false;
    }

    if (force_null_render) {
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
    }

#ifdef _WIN32
    LocalFree(argv_w);
#endif

    if (filepath.empty()) {
        LOG_CRITICAL(Frontend, "Failed to load ROM: No ROM specified");
        return SDL_APP_FAILURE;
    }

    state->system.Initialize();

    // Apply the command line arguments
    state->system.ApplySettings();

    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        state->emu_window = std::make_unique<EmuWindow_SDL3_GL>(&state->input_subsystem, state->system, fullscreen);
        break;
#endif
    case Settings::RendererBackend::Vulkan:
        state->emu_window = std::make_unique<EmuWindow_SDL3_VK>(&state->input_subsystem, state->system, fullscreen);
        break;
    case Settings::RendererBackend::Null:
        state->emu_window = std::make_unique<EmuWindow_SDL3_Null>(&state->input_subsystem, state->system, fullscreen);
        break;
    default:
        LOG_CRITICAL(Frontend, "Invalid renderer backend");
        return SDL_APP_FAILURE;
    }

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    state->system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    state->system.SetInputSubsystem(&state->input_subsystem);
    state->system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    state->system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    state->system.GetFileSystemController().CreateFactories(*state->system.GetFilesystem());
    state->system.GetUserChannel().clear();

    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
    };
    const Core::SystemResultStatus load_result = state->system.Load(*state->emu_window, filepath, load_parameters);
    switch (load_result) {
    case Core::SystemResultStatus::Success:
        break; // Expected case
    case Core::SystemResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filepath);
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorVideoCore:
        LOG_CRITICAL(Frontend, "Failed to initialize VideoCore!");
        return SDL_APP_FAILURE;
    default:
        const u16 loader_id = u16(Core::SystemResultStatus::ErrorLoader);
        const u16 error_id = u16(load_result) - loader_id;
        LOG_CRITICAL(Frontend,
            "While attempting to load the ROM requested, an error occurred. Please "
            "refer to the Eden wiki for more information or the Eden discord for "
            "additional help.\n\nError Code: {:04X}-{:04X}\nError Description: {}",
            loader_id, error_id, Loader::ResultStatus(error_id));
        return SDL_APP_FAILURE;
    }

    if (use_multiplayer) {
        if (auto member = Network::GetRoomMember().lock()) {
            member->BindOnChatMessageReceived(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", address, port, nickname);
            member->Join(nickname, address.c_str(), port, 0, Network::NoPreferredIP, password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            return SDL_APP_FAILURE;
        }
    }

    // DSMod: second output window (desktop stand-in for a dual-screen handheld's bottom panel)
    // Headless runs sit behind a compositor that stops sending frame callbacks to hidden windows,
    // which blocks a FIFO swapchain and freezes the guest. EDEN_VSYNC lets a run force Immediate
    // (0) / Mailbox (1) without touching the config file, which Eden rewrites on exit anyway.
    if (const char* const vsync_env = std::getenv("EDEN_VSYNC"); vsync_env != nullptr) {
        const int mode = std::atoi(vsync_env);
        Settings::values.vsync_mode.SetValue(static_cast<Settings::VSyncMode>(mode));
        LOG_INFO(Frontend, "vsync mode forced to {} by EDEN_VSYNC", mode);
    }
    // Same reasoning for the video decoder: a title that dies inside a cutscene needs the CPU
    // path tried against the GPU one, and the config is not a reliable place to say so.
    // 0 = off, 1 = CPU, 2 = GPU.
    if (const char* const nvdec_env = std::getenv("EDEN_NVDEC"); nvdec_env != nullptr) {
        const int mode = std::atoi(nvdec_env);
        Settings::values.nvdec_emulation.SetValue(static_cast<Settings::NvdecEmulation>(mode));
        LOG_INFO(Frontend, "nvdec emulation forced to {} by EDEN_NVDEC", mode);
    }
    if (aux_virtual_requested) {
        // Headless verification: run the mod runtime with no second window at all. The mod UI is
        // still rasterised into the aux canvas (captured as -screen2.png) and taps still work, but
        // nothing presents, so a compositor that stops sending frame callbacks to a hidden window
        // cannot stall the primary swapchain.
        auto& aux = state->system.GPU().DSModAux();
        aux.width.store(1240);
        aux.height.store(1080);
        aux.refresh_hz.store(60);
        aux.present.store(true);
        LOG_INFO(Frontend, "DSMod virtual aux screen enabled (1240x1080, no window)");
    }
    // DSMod haptics: a desktop has nothing to vibrate, so the sink only logs what a device would
    // play (it runs on the routing's notifier thread, like the Android one).
    state->system.GPU().DSModAux().SetHapticSink(
        [](const VideoCore::DSMod::AuxRouting::HapticEvent& event) {
            LOG_INFO(Frontend, "DSMod haptic sink (desktop, no output): strength {} kind {} "
                               "respect_system {}",
                     event.strength, event.kind, event.flags & 1);
        });
    if (aux_window_requested) {
        state->aux_window = std::make_unique<AuxWindow_SDL3>(state->system, 640, 740);
        if (state->aux_window->IsValid()) {
            state->system.Renderer().SetAuxWindow(state->aux_window.get());
            g_aux_window_presents.store(true);
        } else {
            state->aux_window.reset();
        }
    }

    std::signal(SIGUSR1, ScreenshotSignalHandler);
    std::signal(SIGUSR2, ButtonSignalHandler);
    LOG_INFO(Frontend, "screenshots: kill -USR1 {} writes {}-screen1.png / -screen2.png", getpid(),
             g_screenshot_prefix);

    // Core is loaded, start the GPU (makes the GPU contexts current to this thread)
    state->system.GPU().Start();
    state->system.GetCpuManager().OnGpuReady();

    if (Settings::values.use_disk_shader_cache.GetValue()) {
        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            state->system.GetApplicationProcessProgramID(), std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t value, size_t total) {});
    }

    // don't do anything, SDL3 already exists for us :D
    state->system.RegisterExitCallback([] {});
    void(state->system.Run());
    if (state->system.DebuggerEnabled())
        state->system.InitializeDebugger();
    return SDL_APP_CONTINUE;
}
extern "C" SDL_AppResult SDL_AppIterate(void *appstate) {
    SdlState *state = (SdlState *)appstate;

    // A periodic speed line, so "it feels laggy" can be checked against a number.
    static auto next_perf = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (const auto now = std::chrono::steady_clock::now(); now >= next_perf) {
        next_perf = now + std::chrono::seconds(5);
        const auto results = state->system.GetAndResetPerfStats();
        if (results.average_game_fps > 0.0) {
            LOG_INFO(Frontend, "perf: {:.1f} game fps, {:.0f}% speed, {:.2f} ms/frame",
                     results.average_game_fps, results.emulation_speed * 100.0,
                     results.frametime * 1000.0);
        }
    }
    if (g_screenshot_requested.exchange(false)) {
        TakeScreenshots(state->system);
    }
    ServiceButtonRequests(state);
    return state->emu_window->IsOpen() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}
extern "C" SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    SdlState *state = (SdlState *)appstate;
    if (state->aux_window && state->aux_window->OwnsEvent(*event)) {
        state->aux_window->OnEvent(*event);
    } else {
        state->emu_window->OnEvent(*event);
    }
    return SDL_APP_CONTINUE;
}
extern "C" void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    SdlState *state = (SdlState *)appstate;
    if (!state) return;
    state->system.DetachDebugger();
    void(state->system.Pause());
    state->system.ShutdownMainProcess();
    delete state;
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
