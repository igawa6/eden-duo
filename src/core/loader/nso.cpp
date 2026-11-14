// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"
#include "core/mods/mod_load_plan.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<std::string> AppLoader_NSO::LoadPlanTargetModule(Core::System& system,
                                                               u64 title_id) {
    return Core::Mods::LoadPlanTargetModule(system, title_id);
}

u64 AppLoader_NSO::PeekLoadPlanMailboxSize(Core::System& system, const FileSys::VfsFile& nso_file,
                                           u64 title_id) {
    NSOHeader nso_header{};
    if (nso_file.GetSize() < sizeof(NSOHeader) ||
        sizeof(NSOHeader) != nso_file.ReadObject(&nso_header) ||
        nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0')) {
        return 0;
    }
    Core::Mods::LoadPlanLayout layout;
    for (size_t i = 0; i < layout.segments.size(); ++i) {
        layout.segments[i] = {nso_header.segments[i].location, nso_header.segments[i].size};
    }
    layout.bss_size = nso_header.segments[2].bss_size;
    const auto discovery = Core::Mods::DiscoverModLoadPlan(system, title_id, nso_header.build_id,
                                                           nso_file.GetName(), layout);
    return discovery.plan ? discovery.plan->MailboxSize() : 0;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system,
                                               const FileSys::VfsFile& nso_file, VAddr load_base,
                                               bool should_pass_arguments, bool load_into_process,
                                               std::optional<FileSys::PatchManager> pm,
                                               std::vector<Core::NCE::Patcher>* patches,
                                               s32 patch_index, std::optional<u64> title_id,
                                               DualScreenMailboxSlot* mailbox_slot) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    auto const last_segment_it = &nso_header.segments[nso_header.segments.size() - 1];
    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);
    {
        std::vector<u8> compressed_data(*std::ranges::max_element(nso_header.segments_compressed_size));
        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
            if (nso_header.IsSegmentCompressed(i)) {
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(nso_header.segments[i].size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    ASSERT(r > 0);
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        nso_header.segments[i].size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(nso_header.segments[i].size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    const bool mailbox_host =
        mailbox_slot && mailbox_slot->size != 0 && nso_file.GetName() == mailbox_slot->host;
    if (mailbox_host) {
        if (mailbox_slot->size > std::numeric_limits<u32>::max() - image_size) {
            LOG_ERROR(Loader, "Dual-screen mailbox is too large for {}", nso_file.GetName());
            return std::nullopt;
        }
        if (!load_into_process) {
            // Relative to this module: under NCE the layout pass spaces modules differently from
            // the load pass, so the loader turns it into an address itself.
            mailbox_slot->layout_end = image_size;
        } else if (mailbox_slot->address != load_base + image_size) {
            LOG_ERROR(Loader, "Dual-screen mailbox moved: planned {:#x}, {} ends at {:#x}",
                      mailbox_slot->address, nso_file.GetName(), load_base + image_size);
            return std::nullopt;
        }
        // Data segment and image end are page aligned here, so the mailbox starts on a page.
        codeset.DataSegment().size = image_size - static_cast<u32>(GetInteger(codeset.DataSegment().addr));
        codeset.memory.resize(image_size + mailbox_slot->size);
        codeset.DataSegment().size += static_cast<u32>(mailbox_slot->size);
        image_size += static_cast<u32>(mailbox_slot->size);
    }

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    std::optional<Core::Mods::ModLoadPlan> load_plan;
    VAddr mailbox_address{};
    // Set when the plan's bytes do not match the loaded executable (another game version, or an
    // ExeFS patch that changes the same code). Its pages stay reserved so this pass keeps the
    // layout pass's addresses, but nothing is written and no mailbox is published.
    bool plan_skipped = false;
    std::string plan_package;
    std::string plan_build;
    const auto skip_plan = [&](std::string_view reason) {
        LOG_WARNING(Loader,
                    "Dual-screen package '{}' load plan not applied to {} (build {}): {}. "
                    "Starting the game without it; companion guest actions are off",
                    plan_package, name, plan_build, reason);
        plan_skipped = true;
    };
    if (title_id) {
        Core::Mods::LoadPlanLayout layout;
        for (size_t i = 0; i < layout.segments.size(); ++i) {
            layout.segments[i] = {nso_header.segments[i].location, nso_header.segments[i].size};
        }
        layout.bss_size = nso_header.segments[2].bss_size;
        auto discovery =
            Core::Mods::DiscoverModLoadPlan(system, *title_id, nso_header.build_id, name, layout);
        plan_package = discovery.package;
        plan_build = discovery.build;
        if (discovery.status == Core::Mods::LoadPlanDiscovery::Status::Unsupported ||
            discovery.status == Core::Mods::LoadPlanDiscovery::Status::Invalid) {
            // A companion's load plan is optional for the game: start it without the plan (no
            // guest helper, no mailbox) rather than failing to load the executable. The
            // companion still runs and sees no mailbox. Logged on the load pass only.
            if (load_into_process) {
                LOG_WARNING(Loader,
                            "Dual-screen package '{}' {} load plan for {} (build {}): {}. "
                            "Starting the game without it; companion guest actions are off",
                            discovery.package,
                            discovery.status == Core::Mods::LoadPlanDiscovery::Status::Invalid
                                ? "has an invalid"
                                : "has no matching",
                            name, discovery.build, discovery.error);
            }
        }
        if (discovery.plan) {
            load_plan = std::move(discovery.plan);
            if (load_plan->MailboxSize() > std::numeric_limits<u32>::max() - image_size ||
                load_plan->MailboxSize() >
                    std::numeric_limits<u32>::max() - codeset.DataSegment().size ||
                load_base > std::numeric_limits<VAddr>::max() - image_size) {
                LOG_ERROR(Loader, "Dual-screen mailbox is too large for {}", name);
                return std::nullopt;
            }
            const bool reserved = mailbox_slot && mailbox_slot->size != 0;
            if (reserved && load_into_process &&
                (mailbox_slot->address == 0 || mailbox_slot->size < load_plan->MailboxSize())) {
                skip_plan("its mailbox was not reserved");
            }
            mailbox_address = reserved ? mailbox_slot->address : load_base + image_size;
            if (load_into_process && !plan_skipped && load_plan->GuestCodeSize() == 0) {
                auto module_image = std::span<u8>{codeset.memory}.subspan(module_start);
                if (!load_plan->Apply(module_image, load_base + module_start, mailbox_address)) {
                    skip_plan("the executable's bytes differ from the plan (another game "
                              "version or an ExeFS patch)");
                }
            }
            if (!reserved) {
                codeset.memory.resize(codeset.memory.size() + load_plan->MailboxSize());
                codeset.DataSegment().size += static_cast<u32>(load_plan->MailboxSize());
                image_size += static_cast<u32>(load_plan->MailboxSize());
            }
        }
    }

    // Reserve the helper's RX pages before NCE sees the image in either pass. PatchText records
    // the complete module size to relocate branches to a patch section shared by later modules.
    // Appending these pages after RelocateAndCopy would leave earlier modules' branch targets
    // short by this size (for example, rtld would branch into sdk's data instead of its patches).
    if (load_plan && load_plan->GuestCodeSize() != 0) {
        const u64 code_size = load_plan->GuestCodeSize();
        if (code_size > std::numeric_limits<u32>::max() - image_size ||
            load_base > std::numeric_limits<VAddr>::max() - image_size - code_size) {
            LOG_ERROR(Loader, "Dual-screen guest code is too large for {}", name);
            return std::nullopt;
        }
        const auto offset = codeset.memory.size();
        if (offset % Core::Memory::YUZU_PAGESIZE != 0) {
            LOG_ERROR(Loader, "Dual-screen guest code placement is unaligned for {}", name);
            return std::nullopt;
        }
        codeset.memory.resize(offset + code_size);
        codeset.CompanionCodeSegment() = {offset, offset, static_cast<u32>(code_size)};
        image_size += static_cast<u32>(code_size);
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    if (load_plan && load_plan->GuestCodeSize() != 0) {
        if (load_into_process) {
            const auto offset = codeset.CompanionCodeSegment().offset;
            auto module_image = std::span<u8>{codeset.memory}.subspan(module_start);
            if (!plan_skipped &&
                !load_plan->Apply(module_image, load_base + module_start, mailbox_address,
                                  load_base + offset)) {
                skip_plan("the executable's bytes differ from the plan (another game version "
                          "or an ExeFS patch)");
            }
        }
    }

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        auto cheat_list = pm->CreateCheatList(nso_header.build_id);
        if (!cheat_list.cheats.empty()) {
            system.RegisterCheatList(process, std::move(cheat_list), nso_header.build_id, load_base,
                                     image_size);
        }
        // Only "main" matters: its build id names the package's data file and its base is what
        // "main+..." offsets are relative to. rtld loads first, so taking the first module is wrong.
        if (nso_file.GetName() == "main") {
            system.RegisterDualScreenMod(nso_header.build_id, load_base + module_start,
                                         image_size - module_start);
        }
    }

    if (load_plan && !plan_skipped) {
        system.SetDualScreenGuestMailbox(mailbox_address, load_plan->MailboxSize(),
                                         load_plan->MailboxEpochOffset());
    }

    // Load codeset for current process
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
