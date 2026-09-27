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
