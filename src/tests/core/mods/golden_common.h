// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Shared plumbing of the DSMod package regression goldens (manifest_digest.cpp,
// render_golden.cpp): where the published packages and the checked-in goldens live, the
// regenerate switch, a stable hash, text-golden comparison with a readable diff, and PNG I/O.
//
// Environment:
//   EDEN_DSMOD_GOLDEN_PACKAGE_ROOTS  ':'-separated folders holding <Package>/dualscreen/ (default:
//                                    the CMake define DSMOD_GOLDEN_PACKAGE_ROOTS). A package that
//                                    is found in none of them is SKIPPED, not failed.
//   EDEN_DSMOD_GOLDEN_DIR            golden folder (default: DSMOD_GOLDEN_DIR, the source tree's
//                                    src/tests/core/mods/golden).
//   EDEN_DSMOD_GOLDEN_UPDATE=1       write the goldens from this run instead of comparing.
//   EDEN_DSMOD_GOLDEN_OUT            where a failing run drops its actual output (+ diff images);
//                                    default <temp>/dsmod-golden-actual.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/stb.h"
#include "common/zstd_compression.h"

#ifndef DSMOD_GOLDEN_DIR
#define DSMOD_GOLDEN_DIR ""
#endif
#ifndef DSMOD_GOLDEN_PACKAGE_ROOTS
#define DSMOD_GOLDEN_PACKAGE_ROOTS ""
#endif

namespace DsmodGolden {

/// The published packages under test, by folder name.
inline const std::vector<std::string>& PackageNames() {
    static const std::vector<std::string> names{"MetroidDread", "LinksAwakening",
                                                "MarioKart8Deluxe", "Persona5Royal"};
    return names;
}

inline std::string EnvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? std::string{v} : std::string{fallback};
}

inline bool UpdateMode() {
    const char* v = std::getenv("EDEN_DSMOD_GOLDEN_UPDATE");
    return v != nullptr && *v != '\0' && std::string_view{v} != "0";
}

inline std::filesystem::path GoldenDir() {
    return EnvOr("EDEN_DSMOD_GOLDEN_DIR", DSMOD_GOLDEN_DIR);
}

inline std::filesystem::path ActualDir() {
    const std::string out = EnvOr("EDEN_DSMOD_GOLDEN_OUT", "");
    if (!out.empty()) {
        return out;
    }
    return std::filesystem::temp_directory_path() / "dsmod-golden-actual";
}

/// The package's root folder (the one holding dualscreen/ and maybe package.json), or nullopt.
inline std::optional<std::filesystem::path> FindPackage(const std::string& name) {
    const std::string roots = EnvOr("EDEN_DSMOD_GOLDEN_PACKAGE_ROOTS", DSMOD_GOLDEN_PACKAGE_ROOTS);
    std::stringstream ss{roots};
    std::string root;
    while (std::getline(ss, root, ':')) {
        if (root.empty()) {
            continue;
        }
        const std::filesystem::path dir = std::filesystem::path{root} / name;
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / "dualscreen" / "manifest.json", ec)) {
            return dir;
        }
    }
    return std::nullopt;
}

/// Whether any of the packages is present (a test case with none of them is skipped).
inline bool AnyPackage() {
    return std::ranges::any_of(PackageNames(),
                               [](const std::string& n) { return FindPackage(n).has_value(); });
}

inline std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool WriteFile(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

/// FNV-1a 64: stable across platforms, runs and standard libraries (std::hash is none of these).
inline u64 Fnv64(std::string_view bytes, u64 h = 0xCBF29CE484222325ull) {
    for (const unsigned char c : bytes) {
        h ^= c;
        h *= 0x100000001B3ull;
    }
    return h;
}
inline u64 Fnv64(const void* data, size_t size, u64 h = 0xCBF29CE484222325ull) {
    return Fnv64(std::string_view{static_cast<const char*>(data), size}, h);
}
inline std::string Hex64(u64 v) {
    return fmt::format("{:016x}", v);
}

/// Compares a text golden. Returns "" when equal (or when written in update mode), else a
/// readable report: the first lines found only in the golden and only in the actual text. The
/// actual text is always written next to the other actual outputs on a mismatch.
/// A golden whose name ends in ".zst" is stored zstd-compressed (level 19) and compared after
/// decompression; on a mismatch its plain text is written beside the actual output
/// ("<actual>.golden") so `diff -u` works on both.
inline std::string CompareText(const std::filesystem::path& golden, const std::string& actual,
                               const std::filesystem::path& actual_path) {
    const bool zst = golden.extension() == ".zst";
    if (UpdateMode()) {
        std::string bytes = actual;
        if (zst) {
            const auto packed = Common::Compression::CompressDataZSTD(
                reinterpret_cast<const u8*>(actual.data()), actual.size(), 19);
            bytes.assign(packed.begin(), packed.end());
        }
        return WriteFile(golden, bytes) ? std::string{}
                                        : fmt::format("cannot write golden {}", golden.string());
    }
    auto expected = ReadFile(golden);
    if (expected && zst) {
        const auto plain = Common::Compression::DecompressDataZSTD(
            std::span{reinterpret_cast<const u8*>(expected->data()), expected->size()});
        expected = std::string(plain.begin(), plain.end());
    }
    if (!expected) {
        WriteFile(actual_path, actual);
        return fmt::format("missing golden {} (actual written to {}; regenerate with "
                           "EDEN_DSMOD_GOLDEN_UPDATE=1)",
                           golden.string(), actual_path.string());
    }
    if (*expected == actual) {
        return {};
    }
    WriteFile(actual_path, actual);
    if (zst) {
        WriteFile(actual_path.string() + ".golden", *expected);
    }
    const auto split = [](const std::string& s) {
        std::vector<std::string> lines;
        std::stringstream ss{s};
        std::string line;
        while (std::getline(ss, line)) {
            lines.push_back(line);
        }
        return lines;
    };
    const auto a = split(*expected);
    const auto b = split(actual);
    // Lines only in one side (sorted multiset difference): readable even when a line was
    // inserted and everything after it shifted.
    std::vector<std::string> sa = a, sb = b;
    std::ranges::sort(sa);
    std::ranges::sort(sb);
    std::vector<std::string> only_a, only_b;
    std::ranges::set_difference(sa, sb, std::back_inserter(only_a));
    std::ranges::set_difference(sb, sa, std::back_inserter(only_b));
    const auto clip = [](const std::string& s) {
        return s.size() > 600 ? s.substr(0, 600) + " ...(" + std::to_string(s.size()) + " chars)"
                              : s;
    };
    std::string report = fmt::format(
        "{} differs: {} golden lines, {} actual lines; {} only in golden, {} only in actual.\n"
        "actual: {}\n(diff -u {} {})\n",
        golden.string(), a.size(), b.size(), only_a.size(), only_b.size(), actual_path.string(),
        zst ? actual_path.string() + ".golden" : golden.string(), actual_path.string());
    constexpr size_t Show = 12;
    for (size_t i = 0; i < std::min(Show, only_a.size()); ++i) {
        report += "- " + clip(only_a[i]) + "\n";
    }
    for (size_t i = 0; i < std::min(Show, only_b.size()); ++i) {
        report += "+ " + clip(only_b[i]) + "\n";
    }
    if (only_a.empty() && only_b.empty()) {
        report += "(same lines, different order)\n";
    }
    return report;
}

/// An RGBA picture as it is stored in a golden PNG: canvas ARGB words (0xAARRGGBB).
struct Picture {
    u32 w{};
    u32 h{};
    std::vector<u32> argb;
};

inline bool WritePng(const std::filesystem::path& path, const Picture& pic) {
    std::vector<u8> rgba(static_cast<size_t>(pic.w) * pic.h * 4);
    for (size_t i = 0; i < pic.argb.size(); ++i) {
        const u32 p = pic.argb[i];
        rgba[i * 4 + 0] = static_cast<u8>(p >> 16);
        rgba[i * 4 + 1] = static_cast<u8>(p >> 8);
        rgba[i * 4 + 2] = static_cast<u8>(p);
        rgba[i * 4 + 3] = static_cast<u8>(p >> 24);
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    stbi_write_png_compression_level = 9;
    return stbi_write_png(path.string().c_str(), static_cast<int>(pic.w), static_cast<int>(pic.h),
                          4, rgba.data(), static_cast<int>(pic.w * 4)) != 0;
}

inline std::optional<Picture> ReadPng(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path);
    if (!bytes) {
        return std::nullopt;
    }
    int w = 0, h = 0, n = 0;
    u8* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes->data()),
                                     static_cast<int>(bytes->size()), &w, &h, &n, 4);
    if (data == nullptr) {
        return std::nullopt;
    }
    Picture pic{static_cast<u32>(w), static_cast<u32>(h), {}};
    pic.argb.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < pic.argb.size(); ++i) {
        pic.argb[i] = (u32{data[i * 4 + 3]} << 24) | (u32{data[i * 4 + 0]} << 16) |
                      (u32{data[i * 4 + 1]} << 8) | u32{data[i * 4 + 2]};
    }
    stbi_image_free(data);
    return pic;
}

/// Pixel-exact comparison: "" when equal, else the count and bounding box of the differing
/// pixels and the first one.
inline std::string ComparePictures(const Picture& expected, const Picture& actual) {
    if (expected.w != actual.w || expected.h != actual.h) {
        return fmt::format("size {}x{} != golden {}x{}", actual.w, actual.h, expected.w,
                           expected.h);
    }
    size_t count = 0;
    u32 x0 = expected.w, y0 = expected.h, x1 = 0, y1 = 0;
    std::string first;
    for (u32 y = 0; y < expected.h; ++y) {
        for (u32 x = 0; x < expected.w; ++x) {
            const size_t i = static_cast<size_t>(y) * expected.w + x;
            if (expected.argb[i] == actual.argb[i]) {
                continue;
            }
            if (count++ == 0) {
                first = fmt::format("first at ({},{}): golden {:08X} actual {:08X}", x, y,
                                    expected.argb[i], actual.argb[i]);
            }
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    if (count == 0) {
        return {};
    }
    return fmt::format("{} pixels differ in box x={}..{} y={}..{}; {}", count, x0, x1, y0, y1,
                       first);
}

/// A visual diff: differing pixels magenta, equal ones a dimmed copy of the golden.
inline Picture DiffPicture(const Picture& expected, const Picture& actual) {
    Picture d{expected.w, expected.h, std::vector<u32>(expected.argb.size())};
    for (size_t i = 0; i < d.argb.size() && i < actual.argb.size(); ++i) {
        if (expected.argb[i] != actual.argb[i]) {
            d.argb[i] = 0xFFFF00FFu;
        } else {
            const u32 p = expected.argb[i];
            d.argb[i] = 0xFF000000u | ((p >> 2) & 0x003F3F3Fu);
        }
    }
    return d;
}

} // namespace DsmodGolden
