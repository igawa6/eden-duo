// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/sha.h>

#include "core/file_sys/vfs/vfs_vector.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/mod_module.h"

namespace {
constexpr u64 TitleId = 0x0100000000000001ULL;
constexpr std::string_view BuildId = "AAAAAAAAAAAAAAAA";

std::vector<u8> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string Digest(const std::vector<u8>& bytes) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
    SHA256(bytes.data(), bytes.size(), hash.data());
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(SHA256_DIGEST_LENGTH * 2);
    for (const auto byte : hash) {
        result.push_back(Hex[byte >> 4]);
        result.push_back(Hex[byte & 0xF]);
    }
    return result;
}

FileSys::VirtualDir MakeAssets(std::string manifest, const std::vector<u8>& module) {
    using FileSys::VectorVfsDirectory;
    using FileSys::VectorVfsFile;
    const auto module_file = std::make_shared<VectorVfsFile>(module, "0100000000000001.so");
    const auto platform =
        std::make_shared<VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{module_file},
                                             std::vector<FileSys::VirtualDir>{}, "linux-x86_64");
    const auto modules = std::make_shared<VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{platform}, "modules");
    const auto manifest_file = std::make_shared<VectorVfsFile>(
        std::vector<u8>{manifest.begin(), manifest.end()}, "manifest.json");
    return std::make_shared<VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{manifest_file},
                                                std::vector<FileSys::VirtualDir>{modules},
                                                "dualscreen");
}

std::string Manifest(std::string_view module_json) {
    return R"({"format":1,"title_id":"0100000000000001","module":)" + std::string{module_json} +
           "}";
}

std::string GoodModuleJson(const std::string& digest) {
    return R"({"abi":1,"build_ids":["AAAAAAAAAAAAAAAA"],"libraries":{"linux-x86_64":{"path":"modules/linux-x86_64/0100000000000001.so","sha256":")" +
           digest + R"("}}})";
}

std::filesystem::path TestCache() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("eden-dsmod-module-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::create_directories(path);
    return path;
}

} // namespace

TEST_CASE("DSMod native module declarations are optional", "[dsmod][module]") {
    const auto assets = MakeAssets(R"({"format":1,"title_id":"0100000000000001"})", {});
    std::string error;
    REQUIRE_FALSE(Core::Mods::GameModule::Load(assets, TitleId, BuildId, error));
    REQUIRE(error.empty());
}

TEST_CASE("DSMod packages can require a native reader", "[dsmod][module]") {
    const auto assets =
        MakeAssets(R"({"format":1,"title_id":"0100000000000001","requires_module":true})", {});
    std::string error;
    REQUIRE_FALSE(Core::Mods::GameModule::Load(assets, TitleId, BuildId, error));
    REQUIRE(error.find("required reader") != std::string::npos);
}

TEST_CASE("DSMod native module rejects metadata mismatches", "[dsmod][module]") {
    const auto module = ReadFile(DSMOD_TEST_MODULE_PATH);
    const auto digest = Digest(module);

    SECTION("wrong title") {
        auto metadata = Manifest(GoodModuleJson(digest));
        metadata.replace(metadata.find("0100000000000001"), 16, "0100000000000002");
        auto assets = MakeAssets(metadata, module);
        std::string error;
        REQUIRE_FALSE(Core::Mods::GameModule::Load(assets, TitleId, BuildId, error));
        REQUIRE(error.find("title ID") != std::string::npos);
    }
    SECTION("unsupported build") {
        const auto assets = MakeAssets(Manifest(GoodModuleJson(digest)), module);
        std::string error;
        REQUIRE_FALSE(Core::Mods::GameModule::Load(assets, TitleId, "BBBBBBBBBBBBBBBB", error));
        REQUIRE(error.find("support") != std::string::npos);
    }
    SECTION("checksum") {
        const auto assets = MakeAssets(Manifest(GoodModuleJson(std::string(64, '0'))), module);
        std::string error;
        REQUIRE_FALSE(Core::Mods::GameModule::Load(assets, TitleId, BuildId, error));
        REQUIRE(error.find("checksum") != std::string::npos);
    }
    SECTION("ABI") {
        auto module_json = GoodModuleJson(digest);
        module_json.replace(module_json.find(R"("abi":1)"), 7, R"("abi":2)");
        const auto assets = MakeAssets(Manifest(module_json), module);
        std::string error;
        REQUIRE_FALSE(Core::Mods::GameModule::Load(assets, TitleId, BuildId, error));
        REQUIRE(error.find("ABI") != std::string::npos);
    }
    SECTION("native descriptor disagrees with package ABI") {
        const auto incompatible = ReadFile(DSMOD_TEST_BAD_MODULE_PATH);
        const auto assets =
            MakeAssets(Manifest(GoodModuleJson(Digest(incompatible))), incompatible);
        const auto cache = TestCache();
        Core::Mods::SetModuleCacheDirectory(cache);
        std::string error;
        REQUIRE_FALSE(Core::Mods::GameModule::Load(assets, TitleId, BuildId, error));
        REQUIRE(error.find("interface") != std::string::npos);
        REQUIRE(std::filesystem::is_empty(cache));
        Core::Mods::SetModuleCacheDirectory({});
        std::filesystem::remove_all(cache);
    }
}

TEST_CASE("DSMod native module loads and removes its staged copy", "[dsmod][module]") {
    const auto module = ReadFile(DSMOD_TEST_MODULE_PATH);
    const auto assets = MakeAssets(Manifest(GoodModuleJson(Digest(module))), module);
    const auto cache = TestCache();
    Core::Mods::SetModuleCacheDirectory(cache);

    std::string error;
    auto loaded = Core::Mods::GameModule::Load(assets, TitleId, BuildId, error);
    REQUIRE(error.empty());
    REQUIRE(loaded);
    REQUIRE(loaded->Api() != nullptr);
    // Runtime 12: the optional data extension is negotiated and callable.
    REQUIRE(loaded->DataExtensions() != nullptr);
    {
        std::string got;
        REQUIRE(loaded->DataExtensions()->load_data(
            nullptr, nullptr, "module:test:blob", &got,
            [](void* r, const uint8_t* b, size_t n) {
                static_cast<std::string*>(r)->assign(reinterpret_cast<const char*>(b), n);
            }));
        REQUIRE(got == "module-data");
        REQUIRE_FALSE(loaded->DataExtensions()->load_data(nullptr, nullptr, "module:test:x", &got,
                                                          [](void*, const uint8_t*, size_t) {}));
    }
    REQUIRE(std::distance(std::filesystem::directory_iterator(cache),
                          std::filesystem::directory_iterator{}) == 1);
    const auto directory = std::filesystem::directory_iterator(cache)->path();
    const auto permissions =
        std::filesystem::status(directory / "0100000000000001.so").permissions();
    REQUIRE(
        (permissions & (std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                        std::filesystem::perms::others_write)) == std::filesystem::perms::none);

    loaded.reset();
    REQUIRE(std::filesystem::directory_iterator(cache) == std::filesystem::directory_iterator{});
    Core::Mods::SetModuleCacheDirectory({});
    std::error_code ignored;
    std::filesystem::remove_all(cache, ignored);
}
