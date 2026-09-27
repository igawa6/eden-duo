// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/mods/mod_load_plan.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

#include <nlohmann/json.hpp>

#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/memory.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {
namespace {

constexpr u64 MaxPlanBytes = 1024 * 1024;
constexpr u64 MaxMailboxSize = 16 * 1024 * 1024;
constexpr size_t MaxWrites = 4096;
constexpr size_t MaxWriteBytes = 4 * 1024 * 1024;
constexpr size_t MaxRelocations = 4096;

bool AddOverflows(u64 a, u64 b) {
    return b > std::numeric_limits<u64>::max() - a;
}

bool IsHex(std::string_view text) {
    return std::ranges::all_of(text, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::optional<std::vector<u8>> ParseHexBytes(const nlohmann::json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto text = value.get<std::string>();
    if ((text.size() & 1) != 0 || !IsHex(text) || text.size() / 2 > MaxWriteBytes) {
        return std::nullopt;
    }
    return Common::HexStringToVector(text, false);
}

FileSys::VirtualFile ResolveRelativeFile(FileSys::VirtualDir root, std::string_view path) {
    if (!root || path.empty() || path.front() == '/' || path.back() == '/') {
        return nullptr;
    }
    size_t begin = 0;
    while (true) {
        const size_t slash = path.find('/', begin);
        const auto part = path.substr(begin, slash == std::string_view::npos ? path.size() - begin
                                                                             : slash - begin);
        if (part.empty() || part == "." || part == "..") {
            return nullptr;
        }
        if (slash == std::string_view::npos) {
            return root->GetFile(std::string{part});
        }
        root = root->GetSubdirectory(std::string{part});
        if (!root) {
            return nullptr;
        }
        begin = slash + 1;
    }
}

std::optional<nlohmann::json> ReadJson(const FileSys::VirtualFile& file) {
    if (!file || file->GetSize() == 0 || file->GetSize() > MaxPlanBytes) {
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(file->ReadBytes(file->GetSize()));
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

bool ReadU32(const nlohmann::json& object, const char* key, u32& out) {
    if (!object.contains(key) || !object.at(key).is_number_unsigned()) {
        return false;
    }
    const u64 value = object.at(key).get<u64>();
    if (value > std::numeric_limits<u32>::max()) {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

bool ParseWrites(const nlohmann::json& list, bool payload, std::vector<ModLoadPlan::Write>& out) {
    if (!list.is_array() || list.size() > MaxWrites || out.size() + list.size() > MaxWrites) {
        return false;
    }
    size_t relocation_count{};
    for (const auto& write : out) {
        relocation_count += write.relocations.size();
    }
    for (const auto& item : list) {
        if (!item.is_object() || !item.contains("offset") ||
            !item.at("offset").is_number_unsigned() || !item.contains("expected")) {
            return false;
        }
        ModLoadPlan::Write write;
        write.offset = item.at("offset").get<u64>();
        auto expected = ParseHexBytes(item.at("expected"));
        const char* replacement_key = payload ? "bytes" : "replacement";
        if (!expected || !item.contains(replacement_key)) {
            return false;
        }
        auto replacement = ParseHexBytes(item.at(replacement_key));
        if (!replacement || expected->size() != replacement->size() || replacement->empty()) {
            return false;
        }
        write.expected = std::move(*expected);
        write.replacement = std::move(*replacement);

        if (item.contains("relocations")) {
            if (!payload || !item.at("relocations").is_array() ||
                item.at("relocations").size() > MaxRelocations - relocation_count) {
                return false;
            }
            relocation_count += item.at("relocations").size();
            for (const auto& reloc_json : item.at("relocations")) {
                if (!reloc_json.is_object() || !reloc_json.contains("offset") ||
                    !reloc_json.at("offset").is_number_unsigned() ||
                    reloc_json.value("target", std::string{}) != "mailbox") {
                    return false;
                }
                ModLoadPlan::Relocation reloc;
                const u64 offset = reloc_json.at("offset").get<u64>();
                if (offset > std::numeric_limits<u32>::max() || AddOverflows(offset, 4) ||
                    offset + 4 > write.replacement.size()) {
                    return false;
                }
                reloc.offset = static_cast<u32>(offset);
                const auto kind = reloc_json.value("kind", std::string{});
                if (kind == "aarch64_adrp") {
                    reloc.kind = ModLoadPlan::RelocationKind::AArch64Adrp;
                } else if (kind == "aarch64_add_lo12") {
                    reloc.kind = ModLoadPlan::RelocationKind::AArch64AddLo12;
                } else {
                    return false;
                }
                if (reloc_json.contains("addend")) {
                    if (!reloc_json.at("addend").is_number_integer()) {
                        return false;
                    }
                    reloc.addend = reloc_json.at("addend").get<s64>();
                }
                write.relocations.push_back(reloc);
            }
        }
        out.push_back(std::move(write));
    }
    return true;
}

} // namespace

bool ModLoadPlan::Apply(std::span<u8> module_image, VAddr load_base, VAddr mailbox_address) const {
    for (const auto& write : writes) {
        if (write.expected.empty() || write.expected.size() != write.replacement.size() ||
            AddOverflows(write.offset, write.expected.size()) ||
            write.offset + write.expected.size() > module_image.size() ||
            !std::ranges::equal(write.expected, std::span<const u8>{module_image}.subspan(
                                                    write.offset, write.expected.size()))) {
            return false;
        }
    }
    for (size_t i = 0; i < writes.size(); ++i) {
        const u64 end = writes[i].offset + writes[i].replacement.size();
        for (size_t j = i + 1; j < writes.size(); ++j) {
            const u64 other_end = writes[j].offset + writes[j].replacement.size();
            if (writes[i].offset < other_end && writes[j].offset < end) {
                return false;
            }
        }
    }

    std::vector<std::vector<u8>> staged;
    staged.reserve(writes.size());
    for (const auto& write : writes) {
        auto replacement = write.replacement;
        for (const auto& reloc : write.relocations) {
            if (reloc.offset % 4 != 0 || reloc.offset > replacement.size() ||
                replacement.size() - reloc.offset < sizeof(u32)) {
                return false;
            }
            u32 instruction{};
            std::memcpy(&instruction, replacement.data() + reloc.offset, sizeof(instruction));
            if (AddOverflows(load_base, write.offset) ||
                AddOverflows(load_base + write.offset, reloc.offset)) {
                return false;
            }
            const u64 instruction_address = load_base + write.offset + reloc.offset;
            u64 target{};
            if (reloc.addend < 0) {
                return false;
            } else {
                const u64 addend = static_cast<u64>(reloc.addend);
                if (addend >= mailbox_size || AddOverflows(mailbox_address, addend)) {
                    return false;
                }
                target = mailbox_address + addend;
            }
            if (reloc.kind == RelocationKind::AArch64Adrp) {
                if ((instruction & 0x9F000000) != 0x90000000) {
                    return false;
                }
                const s64 pages =
                    static_cast<s64>(target >> 12) - static_cast<s64>(instruction_address >> 12);
                if (pages < -(1LL << 20) || pages >= (1LL << 20)) {
                    return false;
                }
                const u64 immediate = static_cast<u64>(pages) & 0x1FFFFF;
                instruction = (instruction & 0x9F00001F) | static_cast<u32>((immediate & 3) << 29) |
                              static_cast<u32>((immediate >> 2) << 5);
            } else {
                if (reloc.kind != RelocationKind::AArch64AddLo12 ||
                    (instruction & 0xFFC00000) != 0x91000000) {
                    return false;
                }
                instruction =
                    (instruction & ~(0xFFFu << 10)) | (static_cast<u32>(target & 0xFFF) << 10);
            }
            std::memcpy(replacement.data() + reloc.offset, &instruction, sizeof(instruction));
        }
        staged.push_back(std::move(replacement));
    }
    for (size_t i = 0; i < writes.size(); ++i) {
        std::copy(staged[i].begin(), staged[i].end(), module_image.begin() + writes[i].offset);
    }
    return true;
}

LoadPlanDiscovery DiscoverModLoadPlan(System& system, u64 title_id,
                                      const std::array<u8, 0x20>& build_id,
                                      std::string_view module_name,
                                      const LoadPlanLayout& layout) try {
    auto root = system.GetFileSystemController().GetModificationLoadRoot(title_id);
    if (!root) {
        return {};
    }
    auto subdirs = root->GetSubdirectories();
    std::ranges::sort(subdirs,
                      [](const auto& a, const auto& b) { return a->GetName() < b->GetName(); });
    const auto& disabled = Settings::values.disabled_addons[title_id];
    const std::string build = Common::HexToString(build_id, true);

    for (const auto& subdir : subdirs) {
        if (std::ranges::find(disabled, subdir->GetName()) != disabled.end()) {
            continue;
        }
        const auto assets = subdir->GetSubdirectory("dualscreen");
        if (!assets) {
            continue;
        }
        const auto manifest = ReadJson(assets->GetFile("manifest.json"));
        if (!manifest || !manifest->is_object()) {
            continue;
        }
        if (manifest->contains("title_id")) {
            if (!manifest->at("title_id").is_string()) {
                continue;
            }
            auto manifest_title = manifest->at("title_id").get<std::string>();
            std::ranges::transform(manifest_title, manifest_title.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (manifest_title != fmt::format("{:016X}", title_id)) {
                continue;
            }
        }
        // Match ModRuntime::Discover: a package that needs a newer runtime is selected (it shows
        // the built-in "update Eden" page) and contributes nothing -- no load plan.
        {
            const auto package_json = ReadJson(subdir->GetFile("package.json"));
            if (PackageMinRuntime(&*manifest, package_json ? &*package_json : nullptr) >
                DualScreenRuntimeVersion) {
                return {};
            }
        }
        if (manifest->value("format", 1u) != 1u) {
            continue;
        }
        // Match ModRuntime::Discover: malformed manifests and packages without a renderable page
        // do not become the selected package, so they must not mask a later package's load plan.
        if (!IsUsableDualScreenManifest(*manifest)) {
            continue;
        }
        // This is the package ModRuntime::Discover will select. Do not take a load plan from a
        // later package when the selected package deliberately has none.
        if (!manifest->contains("load_plan")) {
            return {};
        }
        // Each NSO has its own build ID. Select the target before expanding <build>, otherwise
        // rtld/sdk would look for a main-module plan using their unrelated build IDs.
        if (manifest->value("load_plan_module", std::string{"main"}) != module_name) {
            return {};
        }
        if (!manifest->at("load_plan").is_string()) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "load_plan is not a path"};
        }
        std::string path = manifest->at("load_plan").get<std::string>();
        constexpr std::string_view token = "<build>";
        if (const size_t pos = path.find(token); pos != std::string::npos) {
            path.replace(pos, token.size(), build);
        }
        const auto json = ReadJson(ResolveRelativeFile(assets, path));
        if (!json || !json->is_object()) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "load plan missing or invalid JSON"};
        }
        if (!json->contains("module") || !json->at("module").is_string()) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "load plan module is invalid"};
        }
        if (json->at("module").get<std::string>() != module_name) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "load plan module mismatch"};
        }
        if (json->value("format", 0u) != 1u ||
            json->value("title_id", std::string{}) != fmt::format("{:016X}", title_id) ||
            json->value("build_id", std::string{}) != build) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "load plan identity mismatch"};
        }
        if (!json->contains("layout") || !json->at("layout").is_object()) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "load plan layout missing"};
        }
        const auto& expected_layout = json->at("layout");
        if (!expected_layout.contains("segments") || !expected_layout.at("segments").is_array() ||
            expected_layout.at("segments").size() != layout.segments.size()) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "invalid segment layout"};
        }
        for (size_t i = 0; i < layout.segments.size(); ++i) {
            u32 location{}, size{};
            const auto& segment = expected_layout.at("segments").at(i);
            if (!segment.is_object() || !ReadU32(segment, "location", location) ||
                !ReadU32(segment, "size", size) || location != layout.segments[i].location ||
                size != layout.segments[i].size) {
                return {LoadPlanDiscovery::Status::Invalid, {}, "NSO segment mismatch"};
            }
        }
        u32 bss_size{};
        if (!ReadU32(expected_layout, "bss_size", bss_size) || bss_size != layout.bss_size ||
            !json->contains("mailbox_size") || !json->at("mailbox_size").is_number_unsigned()) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "NSO BSS or mailbox invalid"};
        }
        const u64 mailbox_size = json->at("mailbox_size").get<u64>();
        if (mailbox_size == 0 || mailbox_size > MaxMailboxSize ||
            (mailbox_size & Core::Memory::YUZU_PAGEMASK) != 0) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "mailbox size is not page aligned"};
        }
        std::vector<ModLoadPlan::Write> writes;
        if ((json->contains("patches") && !ParseWrites(json->at("patches"), false, writes)) ||
            (json->contains("payloads") && !ParseWrites(json->at("payloads"), true, writes)) ||
            writes.empty()) {
            return {LoadPlanDiscovery::Status::Invalid, {}, "invalid or empty write list"};
        }
        ModLoadPlan plan{mailbox_size, std::move(writes)};
        return {LoadPlanDiscovery::Status::Found, std::move(plan), {}};
    }
    return {};
} catch (const nlohmann::json::exception& error) {
    return {LoadPlanDiscovery::Status::Invalid,
            {},
            std::string{"invalid load plan metadata: "} + error.what()};
}

} // namespace Core::Mods
