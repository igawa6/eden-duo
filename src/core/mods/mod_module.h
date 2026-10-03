// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "core/file_sys/vfs/vfs_types.h"

struct EdenDsmodModuleApi;
struct EdenDsmodModuleExtensions;
struct EdenDsmodFontExtensions;
struct EdenDsmodModuleSaveExtensions;
struct EdenDsmodModuleWriteExtensions;
struct EdenDsmodModuleDataExtensions;

namespace Core::Mods {

/// Runtime 16: the longest module image key ("module:..." string) the host passes to load_image
/// (was 256 through runtime 15). Longer keys are refused before they reach the module.
inline constexpr size_t MaxModuleImageKey = 4096;
[[nodiscard]] inline bool ModuleImageKeyOk(std::string_view key) {
    return key.size() <= MaxModuleImageKey;
}

/// The longest module action name the host passes to on_action.
inline constexpr size_t MaxModuleActionName = 256;

/// What a module action call came to (runtime 16: Declined makes the action Refused).
enum class ModuleActionOutcome : u8 {
    NotRun,   ///< no module instance or on_action, or an empty / over-long name
    Accepted, ///< on_action returned true
    Declined, ///< on_action returned false or threw
};
/// Calls `extensions->on_action(instance, name, argument)` and classifies the result.
ModuleActionOutcome CallModuleAction(const EdenDsmodModuleExtensions* extensions, void* instance,
                                     const std::string& name, s64 argument);

// Android supplies an app-private code-cache directory before a game starts. Desktop uses the
// regular cache directory. Installed mod assets remain in the existing load/<title>/<mod> tree.
void SetModuleCacheDirectory(const std::filesystem::path& directory);

class GameModule final {
public:
    // An absent module declaration returns nullptr with an empty error (declarative package).
    // A declared but unusable module returns nullptr with an actionable diagnostic.
    static std::unique_ptr<GameModule> Load(FileSys::VirtualDir assets, u64 title_id,
                                            std::string_view build_id, std::string& error);
    ~GameModule();

    const EdenDsmodModuleApi* Api() const {
        return api;
    }
    const EdenDsmodModuleExtensions* Extensions() const {
        return extensions;
    }
    // Absent (nullptr) for a module that decodes no proprietary font format -- optional and
    // independently negotiated from Extensions() above, see dsmod_module_extensions.h.
    const EdenDsmodFontExtensions* FontExtensions() const {
        return font_extensions;
    }
    // Absent (nullptr) for a module that reads no save data -- optional and independently
    // negotiated, same rationale as FontExtensions() above.
    const EdenDsmodModuleSaveExtensions* SaveExtensions() const {
        return save_extensions;
    }
    const EdenDsmodModuleWriteExtensions* WriteExtensions() const {
        return write_extensions;
    }
    // Absent (nullptr) for a module that serves no "module:" byte sources (runtime 12).
    const EdenDsmodModuleDataExtensions* DataExtensions() const {
        return data_extensions;
    }
    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

private:
    GameModule() = default;
    Common::DynamicLibrary library;
    const EdenDsmodModuleApi* api{};
    const EdenDsmodModuleExtensions* extensions{};
    const EdenDsmodFontExtensions* font_extensions{};
    const EdenDsmodModuleSaveExtensions* save_extensions{};
    const EdenDsmodModuleWriteExtensions* write_extensions{};
    const EdenDsmodModuleDataExtensions* data_extensions{};
    std::filesystem::path staging_directory;
};

} // namespace Core::Mods
