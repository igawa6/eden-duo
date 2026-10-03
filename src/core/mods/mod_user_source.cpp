// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The "user:" asset source (runtime 17); behaviour in mod_user_source.h.

#include "core/mods/mod_user_source.h"

#include <memory>
#include <system_error>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/mods/dsmod_module_abi.h"

namespace Core::Mods {

std::filesystem::path UserSourceRoot() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "dualscreen" / "user";
}

std::filesystem::path UserSourceDir(const std::filesystem::path& root, u64 title_id) {
    return root / fmt::format("{:016X}", title_id);
}

bool IsSafeUserSourcePath(std::string_view path) {
    if (path.empty() || path.front() == '/') {
        return false;
    }
    for (const char c : path) {
        if (c == '\\' || c == ':' || c == '\0') {
            return false;
        }
    }
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string_view part =
            path.substr(start, slash == std::string_view::npos ? std::string_view::npos
                                                                : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

namespace {
/// Whether `inner` (canonical) is `outer` (canonical) or below it.
bool IsWithin(const std::filesystem::path& outer, const std::filesystem::path& inner) {
    auto o = outer.begin();
    auto i = inner.begin();
    for (; o != outer.end(); ++o, ++i) {
        if (i == inner.end() || *o != *i) {
            return false;
        }
    }
    return true;
}
} // namespace

AssetSource MakeUserSource(std::filesystem::path dir) {
    // The canonical folder, filled by open_dir (which runs once, before any Open).
    auto canonical = std::make_shared<std::filesystem::path>();
    AssetSource source;
    source.prefix = "user";
    source.capability = EDEN_DSMOD_CAP_SOURCE_USER;
    source.max_file_size = UserSourceMaxFileSize;
    source.open_dir = [dir, canonical]() -> FileSys::VirtualDir {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!std::filesystem::is_directory(dir, ec)) {
            LOG_INFO(Core, "DSMod: user: folder {} unavailable",
                     Common::FS::PathToUTF8String(dir));
            return nullptr;
        }
        *canonical = std::filesystem::canonical(dir, ec);
        if (ec) {
            return nullptr;
        }
        static const auto real_fs = std::make_shared<FileSys::RealVfsFilesystem>();
        auto root =
            real_fs->OpenDirectory(Common::FS::PathToUTF8String(*canonical), FileSys::OpenMode::Read);
        LOG_INFO(Core, "DSMod: user: folder {} {}", Common::FS::PathToUTF8String(*canonical),
                 root ? "opened" : "unavailable");
        return root;
    };
    source.accept_path = [canonical](std::string_view path) {
        if (!IsSafeUserSourcePath(path) || canonical->empty()) {
            return false;
        }
        // A link inside the folder must not lead out of it.
        std::error_code ec;
        const auto target =
            std::filesystem::weakly_canonical(*canonical / std::filesystem::path{path}, ec);
        return !ec && IsWithin(*canonical, target);
    };
    return source;
}

} // namespace Core::Mods
