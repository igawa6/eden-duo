// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Mario Kart 8 Deluxe module: ABI glue and the page-readiness outputs.
//   mk8d_reader.{h,cpp}  game-memory reader (race context, racers, local player, items, minimap
//                        projection, horn action), build-pinned code checks, name tables read
//                        from the game
//   mk8d_assets.{h,cpp}  asset-free art, map cameras, message text and font advances from the
//                        player's romfs
//   mk8d_ids.{h,cpp}     id -> name rules
// Supported builds: 4.0.0 (2C336A9BCF79C304...) and 3.0.3 (6A85262F21B90364..., also with the
// CTGP-DX plugin), each with its own verified pins (mk8d_reader.cpp profiles).

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "core/mods/modules/mk8d_assets.h"
#include "core/mods/modules/mk8d_ids.h"
#include "core/mods/modules/mk8d_reader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
using namespace dsmod_sdk::int_types;

constexpr u64 TitleId = UINT64_C(0x0100152000022000);

/// Exactly the builds with a reader profile (mk8d_reader.cpp): 4.0.0 (2C336A9BCF79C304...,
/// AArch64) and 3.0.3 (6A85262F21B90364..., AArch32, with or without CTGP-DX).
EdenDsmodBool SupportsBuild(const char* build_id) {
    return Mk8dReader::SupportsBuildHex(build_id) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

/// One module instance: the asset library and the reader that uses it.
struct Module {
    Module(const EdenDsmodHostApi& api, const char* config) : reader{api, assets, config} {}
    void Sample(const EdenDsmodHostApi& api) {
        reader.Sample(api);
        if (!late_publish) { // a wrapper module may publish these itself, after its own values
            PublishNameScales(api);
            PublishReadiness(api);
        }
    }
    bool late_publish{false};

    /// The waiting / loading cards appear only with their core data, and each image slot only
    /// with a decodable image. mk.pict_ok / mk.cup_ok = the published key is non-empty AND this
    /// module decodes it (checked off the tick thread; the image then sits in the library cache
    /// for the page's own request); mk.name_ok = mk.course_name is non-empty.
    /// One check runs at a time per output: a key that changes while a check is running is
    /// checked when that one ends (dropping a running std::async future would block the tick
    /// thread until its decode finishes). The last finished result is kept for its key.
    struct KeyCheck {
        std::string key;     ///< the published key
        std::string job_key; ///< the key of the running / last finished check
        std::future<bool> job;
        bool job_done{};
        bool job_ok{};
    };
    void CheckKey(const EdenDsmodHostApi& api, KeyCheck& c, const char* source, const char* out) {
        const char* key = api.get_text(api.userdata, source);
        c.key = key ? key : "";
        if (c.job.valid() && c.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            c.job_ok = c.job.get();
            c.job_done = true;
            if (api.log)
                api.log(api.userdata, EDEN_DSMOD_LOG_INFO,
                        ("MK8D DSMod: " + std::string(out) + " = " + (c.job_ok ? "1" : "0") + " (" +
                         c.job_key + ")")
                            .c_str());
        }
        if (!c.job.valid() && !c.key.empty() && c.job_key != c.key) {
            Mk8dAssets::Library* lib = &assets;
            const EdenDsmodHostApi host = api;
            c.job_key = c.key;
            c.job_done = false;
            c.job = std::async(std::launch::async, [lib, host, k = c.key] {
                const auto image = lib->LoadImage(host, k);
                return image && !image->rgba.empty();
            });
        }
        const bool ok = !c.key.empty() && c.job_done && c.job_key == c.key && c.job_ok;
        api.publish_i64(api.userdata, out, ok ? 1 : 0);
    }
    void PublishReadiness(const EdenDsmodHostApi& api) {
        if (!api.get_text || !api.publish_i64)
            return;
        CheckKey(api, pict_check, "mk.pict_key", "mk.pict_ok");
        CheckKey(api, cup_check, "mk.cup_key", "mk.cup_ok");
        // the map tiles likewise (the page falls back from the fit crop to the whole map, and
        // draws no tile when neither decodes)
        CheckKey(api, fit_check, "mk.mapfit_key", "mk.fit_img");
        CheckKey(api, map_check, "mk.map_key", "mk.map_img");
        const char* name = api.get_text(api.userdata, "mk.course_name");
        api.publish_i64(api.userdata, "mk.name_ok", name && *name ? 1 : 0);
        // The course name's width in font units (sum of the page font's CWDH advances,
        // the same measure as r{i}.name_scale). The page derives the name's line count from it
        // (the runtime wraps on spaces only when the whole name is wider than the column), so the
        // cup / class lines sit right under a 1-line name. Published once the font is read.
        if (font)
            api.publish_i64(api.userdata, "mk.course_name_w",
                            name && *name ? static_cast<std::int64_t>(font->Measure(name)) : 0);
    }

    /// r{i}.name_scale = the largest text scale (5, 4, 3, 2) at which the
    /// racer's published name is no wider than "Rosalina" at scale 5, measured with the page
    /// font's own advance widths (UI/USen/font.sarc#turbo_MARIOFont.bffnt CWDH): the LONG row's
    /// name column is exactly that wide, so no name is ever cropped. The font's advances are
    /// read once, off the tick thread; until then nothing is published (the page hides names).
    void PublishNameScales(const EdenDsmodHostApi& api) {
        if (!api.get_text || !api.publish_i64)
            return;
        if (!font) {
            if (font_failed)
                return; // the page keeps names hidden
            if (!font_job.valid()) {
                Mk8dAssets::Library* lib = &assets;
                const EdenDsmodHostApi host = api;
                font_job = std::async(std::launch::async, [lib, host] {
                    return lib->LoadFontAdvances(host, "USen", "turbo_MARIOFont.bffnt");
                });
            }
            if (font_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            font = font_job.get();
            if (!font) {
                font_failed = true; // not retried (the library caches the failure too)
                return;
            }
            reference = font->Measure("Rosalina") * 5u;
        }
        for (int i = 0; i < 12; ++i) {
            const std::string prefix = "r" + std::to_string(i) + ".";
            const char* name = api.get_text(api.userdata, (prefix + "name").c_str());
            if (!name || !*name)
                continue;
            auto it = scale_of.find(name);
            if (it == scale_of.end()) {
                const std::uint32_t w = font->Measure(name);
                int scale = 1;
                for (const int s : {5, 4, 3, 2}) {
                    if (w * static_cast<std::uint32_t>(s) <= reference) {
                        scale = s;
                        break;
                    }
                }
                if (scale_of.size() > 256)
                    scale_of.clear();
                it = scale_of.emplace(name, scale).first;
            }
            api.publish_i64(api.userdata, (prefix + "name_scale").c_str(), it->second);
        }
    }
    ~Module() {
        reader.Shutdown();
    }
    void Configure(const EdenDsmodHostExtensions* ext) {
        const bool usable = ext && ext->version == EDEN_DSMOD_EXT_VERSION &&
                            ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
                            ext->struct_size >= sizeof(*ext);
        assets.SetAstcDecoder(usable ? Mk8dAssets::AstcDecoder{ext->userdata, ext->decode_astc}
                                     : Mk8dAssets::AstcDecoder{});
    }
    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink) {
        if (!image_host || !key || !sink)
            return EDEN_DSMOD_FALSE;
        const std::string_view public_key{key};
        if (!public_key.starts_with("module:mk8d:"))
            return EDEN_DSMOD_FALSE;
        const auto image = assets.LoadImage(*image_host, public_key);
        if (!image || image->rgba.empty())
            return EDEN_DSMOD_FALSE;
        sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
        return EDEN_DSMOD_TRUE;
    }

    Mk8dAssets::Library assets; ///< declared before the reader that references it
    Mk8dReader::Reader reader;
    std::future<std::shared_ptr<const Mk8dAssets::FontAdvances>> font_job;
    std::shared_ptr<const Mk8dAssets::FontAdvances> font;
    bool font_failed{};
    std::uint32_t reference{};
    std::unordered_map<std::string, int> scale_of;
    KeyCheck pict_check, cup_check, fit_check, map_check;
};

void* Create(const EdenDsmodHostApi* host, const char* config) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory)
            return nullptr;
        // create() re-checks all 32 build-id bytes (supports_build saw the hex form).
        if (!Mk8dReader::SupportsBuildId(host->build_id))
            return nullptr;
        return new Module{*host, config};
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    try {
        delete static_cast<Module*>(p);
    } catch (...) {
    }
}
void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host)
            static_cast<Module*>(p)->Sample(*host);
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "MK8D DSMod sample callback failed");
    }
}
void TickCallback(void*, const EdenDsmodHostApi*) {}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    try {
        if (p)
            static_cast<Module*>(p)->Configure(ext);
    } catch (...) {
    }
}
/// Module actions: "mk.horn_write" (the horn, Reader::HornStep). USE ITEM stays a manifest press
/// of the game's own item button.
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        return p && static_cast<Module*>(p)->reader.OnAction(action, argument) ? EDEN_DSMOD_TRUE
                                                                               : EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}
/// Write extension: the horn pulse is a guarded write_batch (expect-bytes checked).
void ConfigureWriteCallback(void* p, const EdenDsmodHostWriteApi* h) {
    try {
        if (!p || !h || h->version != EDEN_DSMOD_WRITE_EXT_VERSION ||
            h->struct_size != sizeof(EdenDsmodHostWriteApi) ||
            h->abi_hash != EDEN_DSMOD_WRITE_EXT_HASH || !h->write_batch)
            return;
        static_cast<Module*>(p)->reader.SetWriteApi(*h);
    } catch (...) {
    }
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (p)
            return static_cast<Module*>(p)->LoadImage(host, key, receiver, sink);
    } catch (...) {
    }
    return EDEN_DSMOD_FALSE;
}

const EdenDsmodModuleApi ModuleApi{EDEN_DSMOD_MODULE_ABI_VERSION,
                                   sizeof(EdenDsmodModuleApi),
                                   0,
                                   EDEN_DSMOD_MODULE_ABI_HASH,
                                   TitleId,
                                   "Mario Kart 8 Deluxe DSMod",
                                   EDEN_DSMOD_CAP_EXTENSIONS | EDEN_DSMOD_CAP_ROMFS_READ |
                                       EDEN_DSMOD_CAP_WRITE_MEMORY,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,    &ConfigureCallback,
    &ActionCallback,        &LoadImageCallback};
const EdenDsmodModuleWriteExtensions WriteExtensions{
    EDEN_DSMOD_WRITE_EXT_VERSION, sizeof(EdenDsmodModuleWriteExtensions), EDEN_DSMOD_WRITE_EXT_HASH,
    &ConfigureWriteCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
DSMOD_SDK_EXPORT_WRITE_EXTENSIONS(WriteExtensions)
