// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Asset bytes, image and font decoders, and the decoded-image cache.
//   - ReadAssetBytes / ReadAssetBytesRaw: "file:<path>" from the package folder, "romfs:<path>"
//     from the running game's romfs (a private storage chain, OpenPrivateRomFS, wrapped in
//     SerialVfsFile so readers never share cipher or table state with the game), and
//     "<archive>#<member>" for SARC members (ReadNxMember) or Story of Seasons lzs archives
//     (DecodeLzss / ExtractArchiveMember, engine_ichigo.cpp).
//   - Decoders (static): DecodeBntx, DecodeDds. The engine formats live in engine_mercury.cpp
//     (DecodeBctex) and engine_ichigo.cpp (DecodeSosXtx). Font metrics: LoadFont (one attempt
//     until a reload) tries a BFFNT on the Nx asset worker, then ModuleDecodeFont (the module's
//     font extension), then the in-core ParseMfnt (engine_mercury.cpp) / ParseSosFont
//     (engine_ichigo.cpp) fallbacks.
//   - Image cache: CacheFindImage / CachePutImage / CacheEraseImagesIf, the only sanctioned access
//     to image_cache (its shared_ptr<const Image> handles survive eviction).
// Not here: choosing the picture for a widget and the "map:" rasteriser (GetImage, mod_map.cpp);
// Nintendo containers, composites and msbt (mod_nx_runtime.cpp); module images
// (mod_module_services.cpp).
// Flow: no stage of its own -- used by the redraw/publish stage (LoadFont from PublishUi and
// DrivePageTransition, reads and cache access from GetImage) and at start-up. Threads: the tick
// thread, the redraw worker (through GetImage) and callers of the module host's read_romfs. The
// cache helpers take asset_cache_mutex; romfs reads serialise on SerialVfsFile's own mutex.

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "bc_decoder.h"
#include "common/logging.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/loader/loader.h"
#include "core/mods/mod_nx_assets.h"
#include "core/mods/mod_romfs_sources.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {

namespace {

/// Serialises reads of the runtime's romfs: the asset workers and the tick thread share one
/// storage chain, and its cipher and table layers keep state across a read. Long reads go in
/// chunks, so another reader never waits for more than one of them.
class SerialVfsFile final : public FileSys::VfsFile {
public:
    explicit SerialVfsFile(FileSys::VirtualFile base_) : base{std::move(base_)} {}

    std::string GetName() const override {
        return base->GetName();
    }
    std::size_t GetSize() const override {
        return base->GetSize();
    }
    bool Resize(std::size_t) override {
        return false;
    }
    FileSys::VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }
    bool IsWritable() const override {
        return false;
    }
    bool IsReadable() const override {
        return true;
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        constexpr std::size_t Chunk = 1 << 20;
        std::size_t done = 0;
        while (done < length) {
            const std::size_t want = std::min(Chunk, length - done);
            std::size_t got = 0;
            {
                std::scoped_lock lock{mutex};
                got = base->Read(data + done, want, offset + done);
            }
            done += got;
            if (got != want) {
                break;
            }
        }
        return done;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }
    bool Rename(std::string_view) override {
        return false;
    }

private:
    FileSys::VirtualFile base;
    mutable std::mutex mutex;
};

/// The game's romfs as RomFSFactory::OpenCurrentProcess opens it, built from objects of our own:
/// a second loader over the game file gives the base romfs and any packed update, and PatchRomFS
/// makes its own update NCA over a base NCA of its own. Only raw files are shared with the game's
/// storage chain, never a cipher or table layer.
FileSys::VirtualFile OpenPrivateRomFS(Core::System& system, u64 program_id) {
    const auto started = std::chrono::steady_clock::now();
    const auto loader = Loader::GetLoader(system, system.GetAppLoader().GetFile(), program_id, 0);
    if (!loader) {
        return nullptr;
    }
    FileSys::VirtualFile base;
    if (loader->ReadRomFS(base) != Loader::ResultStatus::Success) {
        base = nullptr;
    }
    FileSys::VirtualFile romfs = base;
    if (loader->IsRomFSUpdatable()) {
        FileSys::VirtualFile packed_update;
        if (loader->ReadUpdateRaw(packed_update) != Loader::ResultStatus::Success) {
            packed_update = nullptr;
        }
        const auto& provider = system.GetContentProvider();
        std::shared_ptr<FileSys::NCA> base_nca =
            provider.GetEntry(program_id, FileSys::ContentRecordType::Program);
        if (base_nca == nullptr) {
            // Booted from a file no content provider knows (eden-cli with a loose NSP): take the
            // base Program NCA from the game file itself, as a fresh object of our own, or the
            // update would be skipped and this chain would serve the base romfs only.
            base_nca = FileSys::OpenProgramNcaFromGameFile(system.GetAppLoader().GetFile(),
                                                           program_id);
        }
        const FileSys::PatchManager patch_manager{program_id, system.GetFileSystemController(),
                                                  provider};
        romfs = patch_manager.PatchRomFS(base_nca.get(), base, FileSys::ContentRecordType::Program,
                                         packed_update);
    }
    if (romfs) {
        LOG_INFO(
            Core, "DSMod: private romfs chain ({}, {:.1f} MB) built in {:.1f} ms",
            romfs == base ? "base" : "patched", romfs->GetSize() / 1048576.0,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count());
    }
    return romfs;
}
} // namespace

std::vector<u8> ModRuntime::ReadAssetBytes(const std::string& src) {
    // "<archive.lzs>#<member>" pulls one file out of a Story of Seasons lzs archive at runtime.
    if (const auto h = src.find('#'); h != std::string::npos) {
        // A Nintendo SARC resolves member by member without reading the whole archive.
        if (std::vector<u8> member; ReadNxMember(src, member)) {
            return member;
        }
        const std::vector<u8> archive = ReadAssetBytesRaw(src.substr(0, h));
        if (archive.empty())
            return {};
        const std::vector<u8> raw = DecodeLzss(archive);
        return ExtractArchiveMember(raw, src.substr(h + 1));
    }
    return ReadAssetBytesRaw(src);
}

std::vector<u8> ModRuntime::ReadAssetBytesRaw(const std::string& src) {
    // Every "<prefix>:" source resolves through the registry; an unknown prefix reads as empty.
    return asset_sources->ReadAll(src);
}

void ModRuntime::RegisterAssetSources() {
    // The package's own dualscreen/ folder.
    asset_sources->Register({.prefix = "file", .open_dir = [this] { return manifest.asset_dir; }});
    // The game's own filesystem. Reading art from here means a package can use the game's assets
    // without redistributing any of them: the copy on disk is the user's own. Opened once, by
    // whichever thread reads first; a failed open (no process yet) stays failed for the session.
    asset_sources->Register({.prefix = "romfs", .open_dir = [this] { return OpenGameRomFS(); }});
    // Runtime 12: data the package's module generates (asset-free packages).
    asset_sources->Register(
        {.prefix = "module",
         .read_bytes = [this](const std::string& src) { return LoadModuleData(src); }});
    // The program romfs as the base NCA ships it: no update, no LayeredFS.
    asset_sources->Register({.prefix = "base",
                             .open_dir = [this] { return OpenBaseRomfs(system); },
                             .capability = EDEN_DSMOD_CAP_SOURCE_BASE});
    // The add-on content data romfs the game mounts (null without DLC or with it disabled).
    asset_sources->Register({.prefix = "aoc",
                             .open_dir = [this] { return OpenAocRomfs(system); },
                             .capability = EDEN_DSMOD_CAP_SOURCE_AOC});
}

FileSys::VirtualDir ModRuntime::OpenGameRomFS() {
    FileSys::VirtualDir root;
    auto* const process = system.ApplicationProcess();
    if (process != nullptr) {
        Service::FileSystem::ProgramId program_id{};
        std::shared_ptr<Service::FileSystem::SaveDataController> save_data;
        std::shared_ptr<Service::FileSystem::RomFsController> romfs;
        const auto result = system.GetFileSystemController().OpenProcess(
            &program_id, &save_data, &romfs, process->GetProcessId());
        if (R_SUCCEEDED(result) && romfs) {
            // The asset worker reads while the game does: never through the game's own chain.
            auto raw = OpenPrivateRomFS(system, program_id);
            if (!raw) {
                raw = romfs->OpenRomFSCurrentProcess();
                LOG_WARNING(Core, "DSMod: no private romfs chain, sharing the game's ({})",
                            raw ? "reads may race the game's" : "unavailable");
            }
            if (raw) {
                root = FileSys::ExtractRomFS(std::make_shared<SerialVfsFile>(std::move(raw)));
            }
        }
    }
    LOG_INFO(Core, "DSMod: game romfs {}", root ? "opened" : "unavailable");
    return root;
}

/// Decode a BNTX texture -- Nintendo's own container, used by their first-party titles.
///
/// The hard part is shared with Mercury's format and already written: the pixels are stored in
/// Tegra's block-linear tiling either way, so only the wrapper differs. BNTX is a plain header
/// with a table of textures, each carrying its own size, format and data offset -- no compression
/// around it, which makes it simpler than the MTXT case rather than harder.
///
/// Reading these from the player's own copy is what lets a package show a first-party game's map
/// without carrying a single pixel of it.
bool ModRuntime::DecodeBntx(std::span<const u8> file, Image& out) {
    // Texture 0 of a BNTX held in memory; every format the Switch UI uses (mod_nx_assets.cpp).
    // Named textures ("...bntx#<name>") take the asynchronous path in GetImage instead.
    std::string error;
    if (!NxAssets::DecodeBntxImage(file, {}, out, error)) {
        LOG_WARNING(Core, "DSMod: BNTX not decoded: {}", error);
        return false;
    }
    return true;
}

// A .dds (linear block order, not swizzled). DX10 header -> DXGI BC7; else DXT1/DXT5.
bool ModRuntime::DecodeDds(std::span<const u8> file, Image& out) {
    if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0)
        return false;
    u32 h{}, w{}, fourcc{};
    std::memcpy(&h, file.data() + 12, 4);
    std::memcpy(&w, file.data() + 16, 4);
    std::memcpy(&fourcc, file.data() + 84, 4);
    size_t data_off = 128;
    u32 dxgi = 0;
    if (fourcc == 0x30315844u) { // 'DX10'
        if (file.size() < 148)
            return false;
        std::memcpy(&dxgi, file.data() + 128, 4);
        data_off = 148;
    }
    const bool bc7 = dxgi == 98 || dxgi == 99 || dxgi == 100;
    const bool bc1 = fourcc == 0x31545844u; // 'DXT1'
    const bool bc3 = fourcc == 0x35545844u; // 'DXT5'
    if (!bc7 && !bc1 && !bc3)
        return false;
    if (w == 0 || h == 0 || data_off > file.size())
        return false;
    const u8* blk = file.data() + data_off;
    const size_t avail = file.size() - data_off;
    const u32 block_bytes = bc1 ? 8u : 16u;
    const u32 wide = (w + 3) / 4, tall = (h + 3) / 4;
    std::vector<u8> rgba(static_cast<size_t>(w) * h * 4, 0);
    size_t src = 0;
    for (u32 by = 0; by < tall; ++by) {
        for (u32 bx = 0; bx < wide; ++bx) {
            if (src + block_bytes > avail)
                break;
            const u32 px = bx * 4, py = by * 4;
            u8* const dst = rgba.data() + (static_cast<size_t>(py) * w + px) * 4;
            if (bc1)
                bcn::DecodeBc1(blk + src, dst, px, py, w, h);
            else if (bc3)
                bcn::DecodeBc3(blk + src, dst, px, py, w, h);
            else
                bcn::DecodeBc7(blk + src, dst, px, py, w, h);
            src += block_bytes;
        }
    }
    out.w = w;
    out.h = h;
    out.pixels.resize(static_cast<size_t>(w) * h);
    for (size_t k = 0; k < out.pixels.size(); ++k) {
        const u8 rr = rgba[k * 4], gg = rgba[k * 4 + 1], bb = rgba[k * 4 + 2], aa = rgba[k * 4 + 3];
        out.pixels[k] = (u32(aa) << 24) | (u32(rr) << 16) | (u32(gg) << 8) | bb;
    }
    return true;
}

bool ModRuntime::ModuleDecodeFont(std::span<const u8> bytes, FontMetrics& out) {
    if (!game_module || !game_module_instance) {
        return false;
    }
    const auto* font_ext = game_module->FontExtensions();
    if (!font_ext || !font_ext->decode_font) {
        return false;
    }
    struct Receiver {
        FontMetrics* out;
        bool filled{false};
    } receiver{&out};
    const auto accepted =
        font_ext->decode_font(game_module_instance, bytes.data(), bytes.size(), &receiver,
                              [](void* r, u32 line_height, u32 first_codepoint,
                                 const EdenDsmodFontGlyph* glyphs, u32 glyph_count) {
                                  auto& rc = *static_cast<Receiver*>(r);
                                  if (glyph_count != 0 && !glyphs) {
                                      return;
                                  }
                                  rc.out->line_height = line_height;
                                  rc.out->first_codepoint = first_codepoint;
                                  rc.out->glyphs.clear();
                                  rc.out->glyphs.reserve(glyph_count);
                                  for (u32 i = 0; i < glyph_count; ++i) {
                                      const auto& g = glyphs[i];
                                      rc.out->glyphs.push_back(FontGlyph{
                                          g.x, g.y, g.w, g.h, g.bearing_x, g.bearing_y, g.advance});
                                  }
                                  rc.filled = true;
                              });
    return accepted && receiver.filled && out.Valid();
}

void ModRuntime::LoadFont() {
    if (font_ready || manifest.font_metrics_src.empty() || manifest.font_atlas_src.empty() ||
        tick_count < font_retry_tick) {
        return;
    }
    font_ready = true; // one attempt: a missing font should not be retried every frame, except
                       // for a module decoder that may not see the game yet (see below)
    if (RequestNxFont()) {
        return; // a BFFNT: built on the asset worker, installed by PumpNxAssets
    }
    const auto bytes = ReadAssetBytes(manifest.font_metrics_src);
    // A module's own font extension (if the loaded module exports one) always wins first. MFNT
    // (Dread) and Story of Seasons' own layout are both proprietary formats parsed in core as
    // fallbacks -- MFNT because a package's module may predate eden_dsmod_get_font_extensions
    // (ParseMfnt is the same algorithm the module carries, kept in sync deliberately), Story of
    // Seasons because it has no module to move into at all.
    const char* font_source = "module";
    if (bytes.empty()) {
        LOG_WARNING(Core, "DSMod: could not read font metrics '{}'", manifest.font_metrics_src);
        font_metrics.glyphs.clear();
        return;
    }
    if (!ModuleDecodeFont(bytes, font_metrics)) {
        font_source = "core MFNT fallback";
        if (!ParseMfnt(bytes, font_metrics)) {
            font_source = "core SoS fallback";
            if (!ParseSosFont(bytes, font_metrics)) {
                font_metrics.glyphs.clear();
                // A module that declares a font decoder may need the running game for it: try
                // again about once a second, at most 30 times, instead of giving up for good.
                constexpr u32 MaxModuleFontAttempts = 30;
                constexpr u64 ModuleFontRetryTicks = 60;
                const auto* font_ext =
                    game_module && game_module_instance ? game_module->FontExtensions() : nullptr;
                if (font_ext && font_ext->decode_font &&
                    ++font_module_attempts < MaxModuleFontAttempts) {
                    font_ready = false;
                    font_retry_tick = tick_count + ModuleFontRetryTicks;
                    if (font_module_attempts == 1) {
                        LOG_INFO(Core, "DSMod: module font '{}' not ready yet; retrying",
                                 manifest.font_metrics_src);
                    }
                    return;
                }
                LOG_WARNING(Core, "DSMod: could not read font metrics '{}'",
                            manifest.font_metrics_src);
                return;
            }
        }
    }
    LOG_INFO(Core, "DSMod: font '{}' has {} glyph(s), line height {} (via {})",
             manifest.font_metrics_src, font_metrics.glyphs.size(), font_metrics.line_height,
             font_source);
}

// The only sanctioned ways to touch image_cache: the redraw worker and the tick thread both read
// and write it. See mod_runtime.h's declaration comments for the full rationale.
std::shared_ptr<const Image> ModRuntime::CacheFindImage(const std::string& key) const {
    std::scoped_lock lk{asset_cache_mutex};
    const auto it = image_cache.find(key);
    return it == image_cache.end() ? nullptr : it->second;
}

std::shared_ptr<const Image> ModRuntime::CachePutImage(const std::string& key, Image&& image) {
    auto ptr = std::make_shared<const Image>(std::move(image));
    std::scoped_lock lk{asset_cache_mutex};
    image_cache.insert_or_assign(key, ptr);
    return ptr;
}

void ModRuntime::CacheEraseImagesIf(const std::function<bool(const std::string&)>& should_erase) {
    std::scoped_lock lk{asset_cache_mutex};
    for (auto it = image_cache.begin(); it != image_cache.end();) {
        it = should_erase(it->first) ? image_cache.erase(it) : std::next(it);
    }
}

} // namespace Core::Mods
