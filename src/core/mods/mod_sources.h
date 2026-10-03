// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Named asset sources: where the bytes behind a "<prefix>:<path>" asset string come from.
//
// A package names assets as "romfs:/ui/x.bntx", "file:icons.png", "module:p5r:portrait" -- the
// prefix picks a source, the rest is the path inside it. Every reader in the runtime resolves
// those strings through one AssetSources registry (ReadAssetBytes, the Nx asset worker, the
// module host's read_romfs), so adding a source is one Register call, not an edit at each site.
//
// Two kinds of source:
//   - directory sources (romfs, file, base, aoc, user): `open_dir` returns the root directory.
//     It is called once per session, from whichever thread asks first, behind the slot's
//     mutex; the result, null included ("unavailable"), is kept for the session -- unless the
//     source sets `retry_ms` (runtime 16, the game's "romfs"): a null root is opened again on a
//     later ask, at most once per retry_ms, so a read that came too early (a module's create()
//     before the game's filesystem is registered) never makes the source unavailable for good.
//   - byte sources (module): `read_bytes` returns the whole asset for the full source string.
//
// Registration happens while the runtime is built, before any reader or worker can run; after
// that the slot list never changes, so every lookup is lock-free and every method is safe to call
// from any thread. Archive members ("<file>#<member>") are resolved one layer up (ReadAssetBytes,
// the Nx worker): a source only ever opens files.
//
// Failure is always quiet and uniform: an unknown prefix, an unavailable root and a missing file
// all give nullptr / empty / 0, exactly as a missing romfs file always has. A package whose
// optional source is absent (no DLC installed) shows its fallback, not an error.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Core::Mods {

struct AssetSource {
    /// The prefix without its colon: "romfs", "file", "module", "base", "aoc". Lower-case
    /// letters, digits and '_' only (see AssetSources::PrefixOf).
    std::string prefix;
    /// Directory source: opens the root. Called at most once per session (any thread).
    std::function<FileSys::VirtualDir()> open_dir;
    /// Byte source: the whole asset for `src` (the full "<prefix>:..." string); empty = missing.
    /// Used instead of open_dir when set. May block; the callee serialises itself.
    std::function<std::vector<u8>(const std::string& src)> read_bytes;
    /// Module host capability bit(s) advertised while this source is registered (0 = none), so a
    /// module can tell "this host understands aoc:" from "the file is missing".
    u64 capability{0};
    /// Runtime 17: directory sources only. When set, Open refuses every path (PathOf form) this
    /// returns false for -- a source over a real folder rejects "..", absolute paths and links
    /// leading out of it ("user:", mod_user_source.h).
    std::function<bool(std::string_view path)> accept_path;
    /// Runtime 17: directory sources only. A file larger than this opens as missing (0 = no cap).
    u64 max_file_size{0};
    /// Directory source, runtime 16: when open_dir gave nothing, open again on an ask at least
    /// this many milliseconds later. Negative: never (the null root is kept for the session).
    s64 retry_ms{-1};
};

class AssetSources {
public:
    /// Adds a source, replacing one with the same prefix. Only while the runtime is being built:
    /// never after a reader may have started (the slot list is read without a lock).
    void Register(AssetSource source);

    /// The prefix of a source string: "romfs" for "romfs:/a/b". Empty when `src` has no prefix:
    /// no ':' at all, or characters before it that no prefix can hold ('/', '#', '.', upper
    /// case), so a bare romfs path that happens to contain ':' still reads as a bare path.
    [[nodiscard]] static std::string_view PrefixOf(std::string_view src);
    /// The path inside the source: everything after "<prefix>:", leading '/' removed.
    [[nodiscard]] static std::string_view PathOf(std::string_view src);

    /// Whether `src` ("aoc:x" or just "aoc") names a registered source.
    [[nodiscard]] bool Known(std::string_view src) const;
    /// Whether `src` names a registered directory source (one Open/Root can serve).
    [[nodiscard]] bool IsDirectorySource(std::string_view src) const;
    /// Whether the source is usable: a byte source, or a directory source whose root opens.
    /// Opens the root on first ask. False for an unknown prefix.
    [[nodiscard]] bool Available(std::string_view src);

    /// The root of a directory source, opened on first use; nullptr when unknown, unavailable
    /// or a byte source.
    [[nodiscard]] FileSys::VirtualDir Root(std::string_view prefix);
    /// The file `src` names in a directory source; nullptr otherwise.
    [[nodiscard]] FileSys::VirtualFile Open(std::string_view src);
    /// The whole asset: a byte source's read_bytes, else Open(src)'s bytes. Empty when missing.
    [[nodiscard]] std::vector<u8> ReadAll(const std::string& src);
    /// Up to `size` bytes at `offset` of a directory-source file into `out`; with out == nullptr
    /// the file's size. 0 on any failure (and for byte sources, which have no ranges).
    size_t ReadRange(std::string_view src, u64 offset, void* out, size_t size);

    /// The OR of every registered source's capability bits.
    [[nodiscard]] u64 Capabilities() const;
    /// Every registered prefix, in registration order (diagnostics).
    [[nodiscard]] std::vector<std::string> Prefixes() const;

private:
    struct Slot {
        AssetSource def;
        std::mutex mutex;               ///< serialises open_dir; guards the fields below
        std::atomic<bool> ready{false}; ///< root opened (non-null): read without the mutex
        bool tried{false};              ///< open_dir has run at least once
        s64 tried_ms{0};                ///< steady-clock ms of the last open_dir
        FileSys::VirtualDir root;       ///< never changes once non-null
    };
    [[nodiscard]] Slot* Find(std::string_view prefix) const;
    FileSys::VirtualDir RootOf(Slot& slot);

    std::vector<std::unique_ptr<Slot>> slots;
};

} // namespace Core::Mods
