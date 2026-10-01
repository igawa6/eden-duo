// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
// Persisted runtime flags ("persist_flags"); format and location in mod_persist.h.

#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/mods/mod_persist.h"

namespace Core::Mods {

namespace {
std::string SafeName(std::string_view package) {
    std::string out;
    out.reserve(package.size());
    for (const char c : package) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    // never "", "." or ".." (or a hidden file)
    if (out.empty() || out.front() == '.') {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string Utf8(const std::filesystem::path& p) {
    return Common::FS::PathToUTF8String(p);
}
} // namespace

std::filesystem::path PersistFlagsPath(const std::filesystem::path& root, u64 title_id,
                                       std::string_view package) {
    return root / fmt::format("{:016X}", title_id) / (SafeName(package) + ".json");
}

std::filesystem::path PersistFlagsRoot() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "dualscreen" / "persist";
}

FlagPersistence::FlagPersistence(std::filesystem::path file_, std::vector<std::string> keys_)
    : file{std::move(file_)}, keys{std::move(keys_)} {}

std::unordered_map<std::string, s64> FlagPersistence::Pick(
    const std::unordered_map<std::string, s64>& flags) const {
    std::unordered_map<std::string, s64> out;
    for (const auto& key : keys) {
        if (const auto it = flags.find(key); it != flags.end()) {
            out.emplace(key, it->second);
        }
    }
    return out;
}

size_t FlagPersistence::Restore(std::unordered_map<std::string, s64>& flags) {
    size_t restored = 0;
    if (Enabled()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec)) {
            std::ifstream in{file, std::ios::binary};
            std::stringstream text;
            text << in.rdbuf();
            const auto json = nlohmann::json::parse(text.str(), nullptr, false);
            if (json.is_discarded() || !json.is_object() || !json.contains("flags") ||
                !json.at("flags").is_object() || !json.contains("version") ||
                !json.at("version").is_number_integer()) {
                LOG_WARNING(Core, "DSMod: persisted flags file {} is malformed, ignoring it",
                            Utf8(file));
            } else if (json.at("version").get<s64>() != PersistFlagsFileVersion) {
                LOG_INFO(Core, "DSMod: persisted flags file {} has version {}, ignoring it",
                         Utf8(file), json.at("version").get<s64>());
            } else {
                const auto& stored = json.at("flags");
                for (const auto& key : keys) {
                    if (!stored.contains(key)) {
                        continue;
                    }
                    const auto& v = stored.at(key);
                    if (v.is_boolean()) {
                        flags[key] = v.get<bool>() ? 1 : 0;
                    } else if (v.is_number_integer()) {
                        flags[key] = v.get<s64>();
                    } else if (v.is_number_float()) {
                        const double d = v.get<double>();
                        if (!(d >= -9.0e18 && d <= 9.0e18)) {
                            LOG_WARNING(Core, "DSMod: persisted flag '{}' out of range, ignoring",
                                        key);
                            continue;
                        }
                        flags[key] = static_cast<s64>(d);
                    } else {
                        LOG_WARNING(Core, "DSMod: persisted flag '{}' is not a number, ignoring",
                                    key);
                        continue;
                    }
                    ++restored;
                    LOG_INFO(Core, "DSMod: flag '{}' = {} (restored)", key, flags[key]);
                }
            }
        }
    }
    // What is on disk now matches the flags (restored, or defaults with nothing stored): the next
    // write happens on the first real change.
    saved = Pick(flags);
    have_saved = true;
    return restored;
}

bool FlagPersistence::SaveIfChanged(const std::unordered_map<std::string, s64>& flags) {
    if (!Enabled()) {
        return false;
    }
    auto now = Pick(flags);
    if (have_saved && now == saved) {
        return false;
    }
    nlohmann::json values = nlohmann::json::object();
    for (const auto& key : keys) {
        if (const auto it = now.find(key); it != now.end()) {
            values[key] = it->second;
        }
    }
    const nlohmann::json json{{"version", PersistFlagsFileVersion}, {"flags", values}};
    const std::string text = json.dump(1) + "\n";

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        LOG_WARNING(Core, "DSMod: cannot create {}: {}", Utf8(file.parent_path()), ec.message());
        return false;
    }
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            LOG_WARNING(Core, "DSMod: cannot write {}", Utf8(tmp));
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        LOG_WARNING(Core, "DSMod: cannot replace {}: {}", Utf8(file), ec.message());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    saved = std::move(now);
    have_saved = true;
    LOG_DEBUG(Core, "DSMod: persisted flags written to {}", Utf8(file));
    return true;
}

} // namespace Core::Mods
