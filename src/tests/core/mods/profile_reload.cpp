// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// ProfileManager::ResetUserSaveFile re-opens the selected user. On Android the System's
// ProfileManager is built before the user folder is known (it makes a random user and opens it);
// the later reload must leave the *selected* profile opened, which is what the account service
// and the game see -- the companion save reader asks this manager for the running account.

#include <filesystem>
#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/hle/service/acc/profile_manager.h"

namespace {
std::filesystem::path FreshDir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE("DSMod profiles: a reload re-opens the selected user, not the pre-reload one",
          "[dsmod][profiles]") {
    const auto previous_nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    const auto previous_user = Settings::values.current_user.GetValue();

    // The real user folder: two profiles, the second one selected.
    const auto real = FreshDir("duo-profile-reload-real");
    Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, real);
    const auto second = Common::UUID::MakeRandom();
    {
        Service::Account::ProfileManager setup; // makes the first ("Eden") profile
        REQUIRE(setup.CreateNewUser(second, "Second").IsSuccess());
        setup.WriteUserSaveFile();
    }

    // Built before the user folder is known: a random user of its own, opened.
    const auto early = FreshDir("duo-profile-reload-early");
    Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, early);
    Service::Account::ProfileManager manager;
    REQUIRE(manager.GetLastOpenedUser() != second);
    // The selection is set (loaded from the config) after that early manager exists: its
    // constructor resets an index it cannot see to 0.
    Settings::values.current_user = 1;

    // The folder becomes known and the profiles are reloaded (Android reloadProfiles).
    Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, real);
    manager.ResetUserSaveFile();
    CHECK(manager.GetUserCount() == 2);
    CHECK(manager.GetLastOpenedUser() == second);

    // A selection that no longer exists falls back to the first profile.
    Settings::values.current_user = 7;
    manager.ResetUserSaveFile();
    CHECK(manager.GetLastOpenedUser() == *manager.GetUser(0));

    Settings::values.current_user = previous_user;
    Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, previous_nand);
    std::filesystem::remove_all(real);
    std::filesystem::remove_all(early);
}
