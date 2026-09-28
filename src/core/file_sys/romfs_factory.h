// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/result.h"

namespace Loader {
class AppLoader;
} // namespace Loader

namespace Service::FileSystem {
class FileSystemController;
}

namespace FileSys {

class ContentProvider;
class NCA;

enum class ContentRecordType : u8;

enum class StorageId : u8 {
    None = 0,
    Host = 1,
    GameCard = 2,
    NandSystem = 3,
    NandUser = 4,
    SdCard = 5,
};

/// Opens a fresh (unshared) NCA object for the base Program content `program_id` inside a bootable
/// game file (NCA, NSP or XCI). This is the base that PatchRomFS needs to layer an update over when
/// the booted file is not registered with any content provider. Returns null when the file holds no
/// valid, non-update Program NCA for that title.
[[nodiscard]] std::shared_ptr<NCA> OpenProgramNcaFromGameFile(const VirtualFile& file,
                                                              u64 program_id);

/// File system interface to the RomFS archive
class RomFSFactory {
public:
    explicit RomFSFactory(Loader::AppLoader& app_loader, ContentProvider& provider,
                          Service::FileSystem::FileSystemController& controller);
    ~RomFSFactory();

    void SetPackedUpdate(VirtualFile packed_update_raw);
    [[nodiscard]] VirtualFile OpenCurrentProcess(u64 current_process_title_id) const;
    [[nodiscard]] VirtualFile OpenPatchedRomFS(u64 title_id, ContentRecordType type) const;
    [[nodiscard]] VirtualFile OpenPatchedRomFSWithProgramIndex(u64 title_id, u8 program_index,
                                                               ContentRecordType type) const;
    [[nodiscard]] VirtualFile Open(u64 title_id, StorageId storage, ContentRecordType type) const;
    [[nodiscard]] std::shared_ptr<NCA> GetEntry(u64 title_id, StorageId storage,
                                                ContentRecordType type) const;

private:
    /// The base program NCA to layer an update over: the content provider's entry, or, when the
    /// game was booted from a file no content provider knows about, the NCA that was loaded.
    [[nodiscard]] std::shared_ptr<NCA> GetBaseProgramNca(u64 title_id) const;

    VirtualFile file;
    VirtualFile packed_update_raw;

    /// The program NCA the loader booted (null unless this factory was made by the NCA loader).
    std::shared_ptr<NCA> loaded_program_nca;

    VirtualFile base;

    bool updatable;

    ContentProvider& content_provider;
    Service::FileSystem::FileSystemController& filesystem_controller;
};

} // namespace FileSys
