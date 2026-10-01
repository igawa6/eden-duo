// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// The runtime side of Nintendo asset references and composite images:
//   "romfs:/<file.arc>#<member>[#<member>...]#<texture>"  a texture out of a BNTX inside a SARC
//   "romfs:/<file>.bntx[#<texture>]"                      a texture of a loose BNTX (none = 0)
//   "romfs:/<file>.bffnt[#U+XXXX]"                        a font's sheet atlas, or one glyph cell
//   "composite:<name>"                                    manifest "composites" entry
// Decoding runs on one worker thread; GetImage returns nullptr until the picture has landed, and
// PumpNxAssets (start of every publish) moves finished pictures into the image cache and asks for
// a redraw. Composites are re-composed on the worker whenever a layer's visibility or fade moves.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/ns/language.h"
#include "core/hle/service/set/settings_types.h"
#include "core/mods/mod_msbt.h"
#include "core/mods/mod_nx_assets.h"
#include "core/mods/mod_runtime.h"

namespace Core::Mods {
namespace NX = NxAssets;
using NxClock = std::chrono::steady_clock;

namespace {

constexpr u64 MaxWholeRead = 64ull * 1024 * 1024;
constexpr std::string_view CompositePrefix = "composite:";

double MsSince(NxClock::time_point start) {
    return std::chrono::duration<double, std::milli>(NxClock::now() - start).count();
}

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    for (size_t i = 0; i < suffix.size(); ++i) {
        const auto a = static_cast<unsigned char>(text[text.size() - suffix.size() + i]);
        const auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

/// Where a job's "<prefix>:" sources resolve: the runtime's source registry (mod_sources.h).
using Roots = std::shared_ptr<AssetSources>;

enum class JobKind : u8 { Image, Font, Msbt };

/// A message file to decode, and what to check it against.
struct MsbtJob {
    std::string alias;
    u64 serial{};
    Msbt::Options options;
    std::unordered_map<u32, char32_t> icon_glyphs; ///< options.icon_glyphs points here
    std::vector<std::string> labels;               ///< the labels the package uses (coverage)
    std::string font_src;                          ///< page font ("" = built-in)
};

struct Job {
    JobKind kind{JobKind::Image};
    std::string key;
    Roots roots;
    u64 generation{};
    NxClock::time_point queued{};
    bool icon_font{false};         ///< Font: the inline-icon font, not the page font
    std::shared_ptr<MsbtJob> msbt; ///< Msbt
};

} // namespace

/// A decoded message file, as the worker hands it to the tick thread (declared in mod_runtime.h).
struct MsbtResult {
    std::string alias;
    std::string path;
    u64 serial{};
    bool ok{false};
    std::string error;
    std::shared_ptr<const std::unordered_map<std::string, std::string>> texts;
    std::vector<u32> stripped;
    std::vector<u32> unmapped_icons;
    std::vector<std::string> missing_labels;
    u32 uncovered{}; ///< characters (with repeats) the page font cannot draw
    std::vector<char32_t> uncovered_sample;
    std::string font_used;
    double ms{};
};

namespace {

struct CompositeRequest {
    std::shared_ptr<const CompositeDef> def;
    std::vector<float> alpha;
    u64 serial{};
    Roots roots;
    u64 generation{};
    bool dirty{};
};

enum class ResultKind : u8 { Image, Font, Composite, Failed, Fallback, Msbt };

struct Result {
    ResultKind kind{ResultKind::Image};
    std::string key;
    Image image;
    FontMetrics metrics;
    std::string message;
    u64 generation{};
    bool icon_font{false};
    std::shared_ptr<const MsbtResult> msbt;
};

/// Session state of the "msbt:" text source (tick thread).
struct MsbtState {
    bool resolved{false};
    s32 settings_index{-1};
    std::string settings_name;
    std::string desired; ///< the language the game is given (NS name), "" = unknown
    u32 nacp_languages{};
    std::string matched_key; ///< the msbt_lang key that matched ("" = none)
    std::pair<std::string, std::string> chosen;
    bool have_language{false};
    bool on_fallback{false};
    std::string fallback_reason;
    u64 serial{1}; ///< bumped on a language switch: older results are dropped
    std::unordered_map<std::string,
                       std::shared_ptr<const std::unordered_map<std::string, std::string>>>
        texts;
    std::unordered_set<std::string> queued;
    std::unordered_set<std::string> failed;
    std::unordered_set<std::string> logged; ///< one-time warnings (bad refs, missing labels, tags)
    // inline icon font
    std::string icon_key;
    bool icon_requested{false};
    bool icon_failed{false};
    bool icon_ready{false};
    FontMetrics icon_metrics;
    struct ConsoleAsk {
        std::string ref;
        NxClock::time_point asked;
    };
    std::vector<ConsoleAsk> console;
};

struct FontBuild {
    FontMetrics metrics;
    Image atlas;
    NX::BffntInfo info;
    u32 sheet_h{};
};

enum class LayerState : u8 { Todo, Done, Failed };

/// A layer whose picture could not be read is tried again a few times, further apart each time,
/// rather than left out until its composite is rebuilt.
struct LayerRetries {
    static constexpr u8 Attempts = 4; ///< the first read and three retries
    std::vector<u8> failures;         ///< per layer: failed reads in a row
    std::vector<NxClock::time_point> due;

    void Reset(size_t n) {
        failures.assign(n, 0);
        due.assign(n, {});
    }
    void Clear(size_t i) {
        failures[i] = 0;
    }
    /// Layer `i` has failed and waits for another read.
    bool Pending(size_t i) const {
        return failures[i] != 0 && failures[i] < Attempts;
    }
    /// Layer `i` may be read now (never failed, or its retry is due).
    bool Ready(size_t i, NxClock::time_point now) const {
        return failures[i] == 0 || (Pending(i) && now >= due[i]);
    }
    bool AnyDue(NxClock::time_point now) const {
        for (size_t i = 0; i < failures.size(); ++i) {
            if (Pending(i) && now >= due[i]) {
                return true;
            }
        }
        return false;
    }
    NxClock::time_point NextDue() const {
        auto next = NxClock::time_point::max();
        for (size_t i = 0; i < failures.size(); ++i) {
            if (Pending(i)) {
                next = std::min(next, due[i]);
            }
        }
        return next;
    }
    /// Counts a failed read of layer `i`; false once it is given up.
    bool Fail(const std::string& name, size_t i) {
        static constexpr std::array<std::chrono::milliseconds, Attempts - 1> Delay{
            std::chrono::milliseconds{250}, std::chrono::milliseconds{1000},
            std::chrono::milliseconds{3000}};
        if (++failures[i] >= Attempts) {
            LOG_ERROR(Core, "DSMod composite '{}': layer {} given up after {} attempts", name, i,
                      failures[i]);
            return false;
        }
        const auto delay = Delay[failures[i] - 1];
        due[i] = NxClock::now() + delay;
        LOG_WARNING(Core, "DSMod composite '{}': layer {} retry {}/{} in {} ms", name, i,
                    failures[i], Attempts - 1, delay.count());
        return true;
    }
};

/// Worker-side state of one composite: every layer resampled once into composite space.
struct CompositeCache {
    std::shared_ptr<const CompositeDef> def;
    std::vector<NX::PlacedRaster> rasters;
    std::vector<NX::PlacedRaster> masks;
    std::vector<LayerState> state;
    std::vector<LayerState> mask_state;
    size_t todo{};
    u64 composed_serial{};
    // Everything below the first partially faded layer, reused while layers above it animate.
    std::vector<u8> base;
    std::vector<float> base_alpha;
    LayerRetries retries;
    bool recompose{}; ///< a retried layer landed: compose again although the request is clean
    // memory accounting (bytes)
    u64 raster_bytes{};
    u64 peak_transient{};
    bool announced{};
    NxClock::time_point started{};
    double decode_ms{};
    double resample_ms{};
    // recompose statistics, logged every few seconds
    u32 recomposes{};
    double recompose_total_ms{};
    double recompose_max_ms{};
    NxClock::time_point stats_window{};
};

/// Tick-side state of one composite: which layers are visible and since when.
struct CompositeLive {
    bool wanted{};
    bool initialized{};
    std::shared_ptr<const CompositeDef> def;
    std::vector<u8> shown;
    std::vector<NxClock::time_point> shown_at;
    std::vector<float> posted;
    u64 serial{};
    u32 pending_fade_ms{}; ///< flat: a layer appeared; cross-fade when its picture lands
    /// Per layer, the show/hide binds pre-split into (key, negated), built once per `def`, so the
    /// per-tick visibility pass looks the snapshot up without building a key string per layer.
    struct Bind {
        std::string key;
        bool present{}; ///< the layer has this bind at all
        bool negate{};
    };
    std::vector<Bind> show;
    std::vector<Bind> hide;
    std::vector<float> alpha; ///< per-tick scratch, kept so its storage is reused tick to tick
};

/// A layer's alpha resampled onto the composite, trimmed to where it is non-zero (flat mode keeps
/// these for the layers still to come instead of full RGBA masks).
struct AlphaMask {
    s32 x{};
    s32 y{};
    u32 w{};
    u32 h{};
    std::vector<u8> a;
};

/// Worker-side state of a flat composite: one retained canvas holding every visible layer.
struct FlatCache {
    std::shared_ptr<const CompositeDef> def;
    std::vector<u8> base;    ///< premultiplied RGBA, def.w x def.h: the published picture
    std::vector<u8> in_base; ///< per layer: folded into base
    bool built{};
    bool publish{}; ///< base changed since the last published picture
    // a full build in progress (first picture, or a layer that must disappear again)
    bool building{};
    std::vector<u8> target;
    std::vector<u8> canvas;
    size_t cursor{};
    std::unordered_map<size_t, AlphaMask> masks; ///< alpha of later layers' mask_src, by layer
    // statistics
    bool announced{};
    NxClock::time_point started{};
    double decode_ms{};
    double resample_ms{};
    u64 peak_transient{};
    u64 peak_masks{};
    u32 adds{};
    u32 rebuilds{}; ///< full builds since the last published picture (after the first)
    LayerRetries retries;
};

/// Tick-side cross-fade of a flat composite: the picture it replaced, drawn under the new one.
struct CompositeFadeState {
    /// The replaced picture itself (the cache entry it was, kept alive here): the "#prev" lookup
    /// the renderer makes every frame of the fade hands this out without copying the pixels.
    std::shared_ptr<const Image> prev;
    NxClock::time_point start{};
    u32 ms{};
};

AlphaMask ToAlphaMask(const NX::PlacedRaster& r) {
    AlphaMask m;
    if (r.Empty()) {
        return m;
    }
    u32 x0 = r.w, y0 = r.h, x1 = 0, y1 = 0;
    for (u32 j = 0; j < r.h; ++j) {
        const u8* const row = r.premul.data() + u64{j} * r.w * 4;
        for (u32 i = 0; i < r.w; ++i) {
            if (row[u64{i} * 4 + 3] != 0) {
                x0 = std::min(x0, i);
                x1 = std::max(x1, i + 1);
                y0 = std::min(y0, j);
                y1 = std::max(y1, j + 1);
            }
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        return m;
    }
    m.x = r.x + static_cast<s32>(x0);
    m.y = r.y + static_cast<s32>(y0);
    m.w = x1 - x0;
    m.h = y1 - y0;
    m.a.resize(u64{m.w} * m.h);
    for (u32 j = 0; j < m.h; ++j) {
        const u8* const src = r.premul.data() + (u64{y0 + j} * r.w + x0) * 4;
        for (u32 i = 0; i < m.w; ++i) {
            m.a[u64{j} * m.w + i] = src[u64{i} * 4 + 3];
        }
    }
    return m;
}

/// Multiplies a layer by a trimmed alpha mask placed anywhere in the same composite (zero outside).
void ApplyAlphaMask(NX::PlacedRaster& layer, const AlphaMask& m) {
    for (u32 j = 0; j < layer.h; ++j) {
        const s64 my = s64{layer.y} + j - m.y;
        u8* const row = layer.premul.data() + u64{j} * layer.w * 4;
        for (u32 i = 0; i < layer.w; ++i) {
            const s64 mx = s64{layer.x} + i - m.x;
            u32 a = 0;
            if (my >= 0 && mx >= 0 && my < s64{m.h} && mx < s64{m.w}) {
                a = m.a[static_cast<u64>(my) * m.w + static_cast<u64>(mx)];
            }
            if (a == 255) {
                continue;
            }
            u8* const p = row + u64{i} * 4;
            for (u32 c = 0; c < 4; ++c) {
                p[c] = static_cast<u8>((u32{p[c]} * a + 127) / 255);
            }
        }
    }
}

/// The published picture of a composite: the premultiplied canvas as an image, with the
/// definition's extra levels (area-averaged copies) packed beside it (CompositeLevels).
Image MakePackedImage(const CompositeDef& def, std::span<const u8> premul) {
    Image out;
    u32 pw = def.w, ph = def.h;
    const auto levels = CompositeLevels(def, &pw, &ph);
    if (levels.size() <= 1) {
        NX::PremulToImage(premul, def.w, def.h, out);
        return out;
    }
    Image full;
    NX::PremulToImage(premul, def.w, def.h, full);
    out.w = pw;
    out.h = ph;
    out.pixels.assign(u64{pw} * ph, 0);
    const auto blit = [&out](const Image& src, u32 dx, u32 dy) {
        for (u32 j = 0; j < src.h && dy + j < out.h; ++j) {
            const u32 n = std::min(src.w, out.w - dx);
            std::memcpy(out.pixels.data() + u64{dy + j} * out.w + dx,
                        src.pixels.data() + u64{j} * src.w, u64{n} * sizeof(u32));
        }
    };
    blit(full, 0, 0);
    for (size_t k = 1; k < levels.size(); ++k) {
        const auto& lv = levels[k];
        const NX::PlacedRaster r =
            NX::ResampleArea(full, 0, 0, full.w, full.h, 0, 0, lv.w, lv.h, lv.w, lv.h);
        if (r.Empty()) {
            continue;
        }
        Image small;
        NX::PremulToImage(r.premul, r.w, r.h, small);
        blit(small, lv.x + static_cast<u32>(r.x), lv.y + static_cast<u32>(r.y));
    }
    return out;
}

struct PendingDump {
    std::string src;
    std::string out;
    NxClock::time_point requested{};
};

struct Located {
    FileSys::VirtualFile file;
    u64 base{};
    u64 size{};
    std::string chain; ///< cache key of the innermost container reached
    std::array<u8, 4> magic{};
    std::string rest; ///< what follows the innermost container's '#'
    bool has_rest{};
};

enum class LocateStatus : u8 { Ok, NotFound, Error };

NX::ReadAt FileReader(const FileSys::VirtualFile& file, u64 base) {
    return [file, base](u64 offset, std::span<u8> out) {
        return file->Read(out.data(), out.size(), static_cast<size_t>(base + offset)) == out.size();
    };
}

void EmitConsole(const std::string& line) {
    LOG_INFO(Core, "{}", line);
    if (const char* const p = std::getenv("EDEN_DSMOD_CMD")) {
        std::ofstream f(std::string(p) + ".out", std::ios::app);
        if (f) {
            f << line << '\n';
        }
    }
}

/// Is a layer bind non-zero, with its '!' (negation) already split off (CompositeLive::Bind).
/// Looks the key up in ints first, then floats; a key in neither counts as zero.
bool BindKeyNonZero(const StateSnapshot& snapshot, const std::string& key, bool negate) {
    bool value = false;
    if (const auto it = snapshot.ints.find(key); it != snapshot.ints.end()) {
        value = it->second != 0;
    } else if (const auto ft = snapshot.floats.find(key); ft != snapshot.floats.end()) {
        value = ft->second != 0.0;
    }
    return negate ? !value : value;
}

CompositeLive::Bind SplitBind(const std::string& bind) {
    CompositeLive::Bind out;
    out.present = !bind.empty();
    out.negate = out.present && bind.front() == '!';
    out.key = out.negate ? bind.substr(1) : bind;
    return out;
}

/// Whether layer `i` of a live composite is visible: its show bind (if any) is non-zero and its
/// hide bind (if any) is not, read through the pre-split binds.
bool LiveLayerVisible(const CompositeLive& live, size_t i, const StateSnapshot& snapshot) {
    const auto& show = live.show[i];
    const auto& hide = live.hide[i];
    const bool shown = !show.present || BindKeyNonZero(snapshot, show.key, show.negate);
    const bool hidden = hide.present && BindKeyNonZero(snapshot, hide.key, hide.negate);
    return shown && !hidden;
}

} // namespace

struct NxAssetState {
    // ---- shared between the tick thread and the worker (queue_mutex) ----
    std::mutex queue_mutex;
    std::condition_variable_any cv;
    std::deque<Job> jobs;
    std::unordered_map<std::string, CompositeRequest> requests;
    std::deque<Result> results;
    u64 generation{1};

    // ---- container caches (io_mutex; the worker and the sync member read) ----
    std::mutex io_mutex;
    std::unordered_map<std::string, FileSys::VirtualFile> files;
    std::unordered_map<std::string, NX::SarcIndex> sarcs;
    std::unordered_map<std::string, std::shared_ptr<const NX::BntxIndex>> bntxs;
    std::unordered_map<std::string, std::shared_ptr<const FontBuild>> fonts;

    // ---- worker only ----
    std::unordered_map<std::string, CompositeCache> caches;
    std::unordered_map<std::string, FlatCache> flat_caches;
    u64 worker_generation{0};

    // ---- guarded by state_mutex: the bucket below, formerly "tick thread only" ----
    // PumpNxAssets/GetNxImage/GetCompositeImage/GetMsbtText/NxIconFont used to run only on the
    // tick thread, back to back, every tick. GetImage (mod_map.cpp) reaches
    // GetNxImage/GetCompositeImage for nx:/composite-sourced images, and RenderPage's own
    // TextProvider callback is GetMsbtText -- both are reachable from the redraw worker's own
    // RenderPage call (RunRedrawJob), which can run concurrently with the tick thread's own
    // PumpNxAssets (every tick, unconditionally) and its own synchronous RenderPageTo calls (the
    // debug page / a page transition / a GPU_COMPOSITE page, all of which stay on the tick thread
    // but can overlap the worker in time).
    // std::recursive_mutex, not std::mutex: PumpNxAssets calls GetMsbtText/GetImage while already
    // holding it (the msbt-console and imgdump-poller loops inside its own body), PublishNxFades
    // calls CompositeFade while holding it, GetMsbtText calls RequestMsbt/ResolveMsbtLanguage while
    // holding it -- same reasoning as map_state_mutex (mod_runtime.h): a recursive mutex makes
    // same-thread re-entry safe by construction instead of requiring a hand-verified call-graph
    // audit across this whole file. Held for a whole function body (coarse grain, the same choice
    // as map_state_mutex's "map:" span); a TSan run, not a hand-derived set of short critical
    // sections, is what verifies sufficiency.
    mutable std::recursive_mutex state_mutex;

    // ---- tick thread only, until a caller crosses state_mutex above ----
    std::unordered_set<std::string> pending;
    std::unordered_set<std::string> failed;
    std::unordered_set<std::string> fallback;
    std::unordered_set<std::string> missing_composites;
    std::unordered_map<std::string, CompositeLive> live;
    std::unordered_map<std::string, CompositeFadeState> fades;
    std::vector<PendingDump> dumps;
    std::string font_key;
    MsbtState msbt;
    // tick-side cost of PumpNxAssets, summarised like the runtime's other stage timers
    u64 pump_calls{};
    double pump_total_ms{};
    double pump_max_ms{};
    u64 posts{};
    NxClock::time_point pump_window{NxClock::now()};

    ModRuntime*
        rt{}; ///< serves "module:" layer sources (asset-free packages); set once at creation
    std::jthread worker; ///< last member: stopped and joined before the rest goes away

    void Stop() {
        if (worker.joinable()) {
            worker.request_stop();
            cv.notify_all();
            worker.join();
        }
    }

    /// Container walk: open the file, then follow '#' members while the bytes are a SARC.
    LocateStatus Locate(const std::string& src, const Roots& roots, Located& out,
                        std::string& error);
    bool ReadAll(const Located& at, std::vector<u8>& bytes, std::string& error) const;
    std::shared_ptr<const FontBuild> Font(const Located& at, std::string& error);
    /// Decodes any picture reference the worker understands.
    LocateStatus LoadImage(const std::string& src, const Roots& roots, Image& out,
                           std::string& error);

    void WorkerMain(std::stop_token stop);
    bool HasCompositeWork() const;
    bool CompositeWants(const std::string& name, const CompositeRequest& request) const;
    /// When the earliest failed composite layer may be read again (max: none waiting).
    NxClock::time_point NextRetry() const;
    void RunJob(Job& job);
    void RunMsbtJob(const Job& job, Result& result);
    void RunCompositeStep(const std::string& name);
    void RunFlatStep(const std::string& name, const CompositeRequest& request);
    /// Flat mode: layer `index` decoded, resampled, cut by its mask and trimmed (empty on failure).
    NX::PlacedRaster FlatLayer(FlatCache& cache, const CompositeRequest& request, size_t index,
                               bool stash, bool& failed);
    void PushComposite(const std::string& name, const CompositeRequest& request, Image&& image);
    void Rasterize(CompositeCache& cache, const CompositeRequest& request, size_t index);
    void Push(Result&& result) {
        std::scoped_lock lock{queue_mutex};
        results.push_back(std::move(result));
    }
    void EnsureWorker() {
        if (!worker.joinable()) {
            worker = std::jthread([this](std::stop_token stop) { WorkerMain(stop); });
        }
    }
};

void NxAssetStateDeleter::operator()(NxAssetState* state) const {
    if (state != nullptr) {
        state->Stop();
        delete state;
    }
}

NxAssetState* MakeNxAssetState(ModRuntime* rt) {
    auto* state = new NxAssetState();
    state->rt = rt;
    return state;
}

// ---------------------------------------------------------------------------------------------
// Container walk and decoding (worker thread, and the sync member read)

LocateStatus NxAssetState::Locate(const std::string& src, const Roots& roots, Located& out,
                                  std::string& error) {
    out = {};
    const std::string_view prefix = AssetSources::PrefixOf(src);
    if (!roots || prefix.empty() || !roots->IsDirectorySource(src)) {
        error = "unsupported scheme";
        return LocateStatus::Error;
    }
    const size_t scheme = prefix.size() + 1;
    const size_t hash = src.find('#');
    std::string path =
        src.substr(scheme, hash == std::string::npos ? std::string::npos : hash - scheme);
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    out.chain = src.substr(0, scheme) + path;
    out.has_rest = hash != std::string::npos;
    out.rest = out.has_rest ? src.substr(hash + 1) : std::string{};
    {
        std::scoped_lock lock{io_mutex};
        auto& file = files[out.chain];
        if (!file) {
            const auto root = roots->Root(prefix);
            file = root ? root->GetFileRelative(path) : nullptr;
        }
        out.file = file;
        if (!file) {
            files.erase(out.chain);
        }
    }
    if (!out.file) {
        error = prefix == "file" ? std::string{"no such package file"}
                : roots->Root(prefix) ? fmt::format("no such {} file", prefix)
                                      : fmt::format("game {} unavailable", prefix);
        return LocateStatus::NotFound;
    }
    out.size = out.file->GetSize();
    for (u32 depth = 0; depth < 8; ++depth) {
        out.magic = {};
        if (out.size < 4 ||
            out.file->Read(out.magic.data(), 4, static_cast<size_t>(out.base)) != 4) {
            error = "file too short";
            return LocateStatus::Error;
        }
        if (!out.has_rest || std::memcmp(out.magic.data(), "SARC", 4) != 0) {
            return LocateStatus::Ok;
        }
        const size_t next = out.rest.find('#');
        const std::string member = out.rest.substr(0, next);
        NX::SarcMember found{};
        {
            std::scoped_lock lock{io_mutex};
            auto it = sarcs.find(out.chain);
            if (it == sarcs.end()) {
                NX::SarcIndex index;
                if (!NX::ParseSarc(FileReader(out.file, out.base), out.size, index, error)) {
                    return LocateStatus::Error;
                }
                it = sarcs.emplace(out.chain, std::move(index)).first;
            }
            const NX::SarcMember* const m = it->second.Find(member);
            if (m == nullptr) {
                error = "archive has no member '" + member + "'";
                return LocateStatus::NotFound;
            }
            found = *m;
        }
        out.base += found.offset;
        out.size = found.size;
        out.chain += "#" + member;
        out.has_rest = next != std::string::npos;
        out.rest = out.has_rest ? out.rest.substr(next + 1) : std::string{};
    }
    error = "archives nested too deeply";
    return LocateStatus::Error;
}

bool NxAssetState::ReadAll(const Located& at, std::vector<u8>& bytes, std::string& error) const {
    if (at.size > MaxWholeRead) {
        error = "asset too large to read whole";
        return false;
    }
    bytes.resize(static_cast<size_t>(at.size));
    if (at.file->Read(bytes.data(), bytes.size(), static_cast<size_t>(at.base)) != bytes.size()) {
        error = "short read";
        return false;
    }
    return true;
}

std::shared_ptr<const FontBuild> NxAssetState::Font(const Located& at, std::string& error) {
    {
        std::scoped_lock lock{io_mutex};
        if (const auto it = fonts.find(at.chain); it != fonts.end()) {
            return it->second;
        }
    }
    std::vector<u8> bytes;
    if (!ReadAll(at, bytes, error)) {
        return nullptr;
    }
    auto build = std::make_shared<FontBuild>();
    if (!NX::BuildBffntFont(bytes, build->metrics, build->atlas, error) ||
        !NX::ParseBffnt(bytes, build->info, error)) {
        return nullptr;
    }
    const u32 sheets = std::max<u32>(1, build->info.sheet_count);
    build->sheet_h = build->atlas.h / sheets;
    std::scoped_lock lock{io_mutex};
    return fonts.emplace(at.chain, std::move(build)).first->second;
}

LocateStatus NxAssetState::LoadImage(const std::string& src, const Roots& roots, Image& out,
                                     std::string& error) {
    if (src.starts_with("module:")) {
        // A layer the game module builds (asset-free package: no image file exists at all).
        if (rt != nullptr && rt->LoadModuleImageSync(src, out)) {
            return LocateStatus::Ok;
        }
        error = "the module did not build it";
        return LocateStatus::NotFound;
    }
    Located at;
    const LocateStatus status = Locate(src, roots, at, error);
    if (status != LocateStatus::Ok) {
        return status;
    }
    const auto is = [&at](const char* magic) {
        return std::memcmp(at.magic.data(), magic, 4) == 0;
    };
    if (is("BNTX")) {
        const NX::ReadAt read = FileReader(at.file, at.base);
        std::shared_ptr<const NX::BntxIndex> index;
        {
            std::scoped_lock lock{io_mutex};
            if (const auto it = bntxs.find(at.chain); it != bntxs.end()) {
                index = it->second;
            }
        }
        if (!index) {
            auto parsed = std::make_shared<NX::BntxIndex>();
            if (!NX::ParseBntx(read, at.size, *parsed, error)) {
                return LocateStatus::Error;
            }
            std::scoped_lock lock{io_mutex};
            index = bntxs.emplace(at.chain, std::move(parsed)).first->second;
        }
        const NX::BntxTexture* const texture = index->Find(at.rest);
        if (texture == nullptr) {
            error = "no texture named '" + at.rest + "'";
            return LocateStatus::NotFound;
        }
        if (!NX::DecodeTextureImage(read, at.size, *texture, 0, out, error)) {
            return LocateStatus::Error;
        }
        return LocateStatus::Ok;
    }
    if (is("FFNT")) {
        const auto font = Font(at, error);
        if (!font) {
            return LocateStatus::Error;
        }
        if (at.rest.empty()) {
            out = font->atlas;
            return LocateStatus::Ok;
        }
        // "U+E0A0": one glyph cell, e.g. the pictogram font's button icons.
        if (at.rest.size() < 3 || (at.rest[0] != 'U' && at.rest[0] != 'u') || at.rest[1] != '+') {
            error = "glyph must be written U+XXXX";
            return LocateStatus::Error;
        }
        const u32 code = static_cast<u32>(std::strtoul(at.rest.c_str() + 2, nullptr, 16));
        const auto glyph = font->info.cmap.find(code);
        const u32 per_sheet = u32{font->info.cols} * font->info.rows;
        if (glyph == font->info.cmap.end() || per_sheet == 0) {
            error = "font has no glyph " + at.rest;
            return LocateStatus::NotFound;
        }
        const u32 cell = glyph->second % per_sheet;
        const u32 cx = (cell % font->info.cols) * (u32{font->info.cell_w} + 1) + 1;
        const u32 cy = (cell / font->info.cols) * (u32{font->info.cell_h} + 1) + 1 +
                       (glyph->second / per_sheet) * font->sheet_h;
        const u32 cw = font->info.cell_w;
        const u32 ch = font->info.cell_h;
        if (cx + cw > font->atlas.w || cy + ch > font->atlas.h) {
            error = "glyph cell outside the atlas";
            return LocateStatus::Error;
        }
        out.w = cw;
        out.h = ch;
        out.pixels.resize(u64{cw} * ch);
        for (u32 y = 0; y < ch; ++y) {
            std::copy_n(font->atlas.pixels.begin() +
                            static_cast<std::ptrdiff_t>(u64{cy + y} * font->atlas.w + cx),
                        cw, out.pixels.begin() + static_cast<std::ptrdiff_t>(u64{y} * cw));
        }
        return LocateStatus::Ok;
    }
    if (at.has_rest) {
        // Not a Nintendo container: the runtime's older archive kinds (.lzs members ...).
        error = "fallback";
        return LocateStatus::NotFound;
    }
    std::vector<u8> bytes;
    if (!ReadAll(at, bytes, error)) {
        return LocateStatus::Error;
    }
    int w = 0, h = 0, comps = 0;
    stbi_uc* const decoded =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comps, 4);
    if (decoded == nullptr || w <= 0 || h <= 0) {
        stbi_image_free(decoded);
        error = "not a decodable picture";
        return LocateStatus::Error;
    }
    NX::RgbaToImage({decoded, static_cast<size_t>(w) * h * 4}, static_cast<u32>(w),
                    static_cast<u32>(h), out);
    stbi_image_free(decoded);
    return LocateStatus::Ok;
}

// ---------------------------------------------------------------------------------------------
// Worker

bool NxAssetState::CompositeWants(const std::string& name, const CompositeRequest& request) const {
    if (request.generation != generation) {
        return false;
    }
    if (request.dirty) {
        return true;
    }
    const auto now = NxClock::now();
    if (request.def && request.def->flat) {
        const auto flat = flat_caches.find(name);
        return flat == flat_caches.end() || flat->second.def != request.def ||
               flat->second.building || flat->second.publish || flat->second.retries.AnyDue(now);
    }
    const auto cache = caches.find(name);
    return cache == caches.end() || cache->second.def != request.def || cache->second.todo > 0 ||
           cache->second.recompose || cache->second.retries.AnyDue(now);
}

bool NxAssetState::HasCompositeWork() const {
    for (const auto& [name, request] : requests) {
        if (CompositeWants(name, request)) {
            return true;
        }
    }
    return false;
}

NxClock::time_point NxAssetState::NextRetry() const {
    auto next = NxClock::time_point::max();
    for (const auto& [name, request] : requests) {
        if (request.generation != generation) {
            continue;
        }
        if (const auto flat = flat_caches.find(name); flat != flat_caches.end()) {
            next = std::min(next, flat->second.retries.NextDue());
        }
        if (const auto cache = caches.find(name); cache != caches.end()) {
            next = std::min(next, cache->second.retries.NextDue());
        }
    }
    return next;
}

void NxAssetState::WorkerMain(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::optional<Job> job;
        std::string composite;
        {
            std::unique_lock lock{queue_mutex};
            const auto has_work = [this] { return !jobs.empty() || HasCompositeWork(); };
            // Sleep until there is work, or until a failed layer is due for another read.
            if (const auto retry = NextRetry(); retry != NxClock::time_point::max()) {
                cv.wait_until(lock, stop, retry, has_work);
            } else {
                cv.wait(lock, stop, has_work);
            }
            if (stop.stop_requested()) {
                return;
            }
            if (worker_generation != generation) {
                worker_generation = generation;
                caches.clear();
                flat_caches.clear();
            }
            if (!jobs.empty()) {
                job = std::move(jobs.front());
                jobs.pop_front();
            } else {
                // Round-robin-free but fair enough: the first composite with work.
                for (const auto& [name, request] : requests) {
                    if (CompositeWants(name, request)) {
                        composite = name;
                        break;
                    }
                }
            }
        }
        try {
            if (job) {
                RunJob(*job);
            } else if (!composite.empty()) {
                RunCompositeStep(composite);
            }
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "DSMod nx: worker step failed: {}", e.what());
        }
    }
}

void NxAssetState::RunJob(Job& job) {
    const auto started = NxClock::now();
    const double waited = std::chrono::duration<double, std::milli>(started - job.queued).count();
    std::string error;
    Result result;
    result.key = job.key;
    result.generation = job.generation;
    if (job.kind == JobKind::Msbt) {
        RunMsbtJob(job, result);
        Push(std::move(result));
        return;
    }
    if (job.kind == JobKind::Font) {
        result.icon_font = job.icon_font;
        Located at;
        std::shared_ptr<const FontBuild> font;
        if (Locate(job.key, job.roots, at, error) == LocateStatus::Ok) {
            if (std::memcmp(at.magic.data(), "FFNT", 4) == 0) {
                font = Font(at, error);
            } else {
                error = "not a BFFNT";
            }
        }
        if (!font) {
            result.kind = ResultKind::Failed;
            result.message = error;
        } else {
            result.kind = ResultKind::Font;
            result.metrics = font->metrics;
            result.image = font->atlas;
            LOG_INFO(Core,
                     "DSMod nx: font '{}' built in {:.1f} ms ({} glyphs, cap height {}, atlas "
                     "{}x{}, {} sheet(s))",
                     job.key, MsSince(started), font->metrics.glyphs.size(),
                     font->metrics.line_height, font->atlas.w, font->atlas.h,
                     font->info.sheet_count);
        }
        Push(std::move(result));
        return;
    }
    const LocateStatus status = LoadImage(job.key, job.roots, result.image, error);
    if (status == LocateStatus::Ok) {
        result.kind = ResultKind::Image;
        LOG_INFO(Core, "DSMod nx: decoded '{}' ({}x{}) in {:.2f} ms (queued {:.1f} ms)", job.key,
                 result.image.w, result.image.h, MsSince(started), waited);
    } else if (error == "fallback") {
        result.kind = ResultKind::Fallback;
    } else {
        result.kind = ResultKind::Failed;
        result.message = error;
    }
    Push(std::move(result));
}

void NxAssetState::RunMsbtJob(const Job& job, Result& result) {
    const auto started = NxClock::now();
    const MsbtJob& mj = *job.msbt;
    auto out = std::make_shared<MsbtResult>();
    out->alias = mj.alias;
    out->path = job.key;
    out->serial = mj.serial;
    result.kind = ResultKind::Msbt;
    std::string error;
    Located at;
    std::vector<u8> bytes;
    Msbt::File file;
    if (Locate(job.key, job.roots, at, error) != LocateStatus::Ok || !ReadAll(at, bytes, error) ||
        !Msbt::Parse(bytes, mj.options, file, error)) {
        out->error = error;
        out->ms = MsSince(started);
        result.msbt = std::move(out);
        return;
    }
    // Which characters can the page draw? The BFFNT's own character map (every glyph of it is
    // drawn), else the built-in font's Latin-1 with its stand-ins.
    std::shared_ptr<const FontBuild> font;
    if (!mj.font_src.empty()) {
        Located font_at;
        std::string font_error;
        if (Locate(mj.font_src, job.roots, font_at, font_error) == LocateStatus::Ok &&
            std::memcmp(font_at.magic.data(), "FFNT", 4) == 0) {
            font = Font(font_at, font_error);
        }
    }
    out->font_used = font ? mj.font_src : std::string{"built-in"};
    const auto drawable = [&font](char32_t c) {
        if (c == '\n' || c == ' ' || (c >= TextIconBase && c <= TextIconLast)) {
            return true;
        }
        const char32_t alt = TextFallbackCodepoint(c);
        if (font) {
            return font->info.cmap.contains(c) || (alt != 0 && font->info.cmap.contains(alt));
        }
        return (c >= 0x20 && c < 0x100) || alt != 0;
    };
    const auto check = [&](const std::string& text) {
        for (size_t i = 0; i < text.size();) {
            const char32_t c = Msbt::NextUtf8(text, i);
            if (drawable(c)) {
                continue;
            }
            ++out->uncovered;
            if (out->uncovered_sample.size() < 8 &&
                std::find(out->uncovered_sample.begin(), out->uncovered_sample.end(), c) ==
                    out->uncovered_sample.end()) {
                out->uncovered_sample.push_back(c);
            }
        }
    };
    if (mj.labels.empty()) {
        for (const auto& [label, text] : file.texts) {
            check(text);
        }
    } else {
        for (const auto& label : mj.labels) {
            const auto it = file.texts.find(label);
            if (it == file.texts.end()) {
                out->missing_labels.push_back(label);
            } else {
                check(it->second);
            }
        }
    }
    out->ok = true;
    out->stripped = std::move(file.stripped);
    out->unmapped_icons = std::move(file.unmapped_icons);
    const size_t label_count = file.texts.size();
    out->texts =
        std::make_shared<const std::unordered_map<std::string, std::string>>(std::move(file.texts));
    out->ms = MsSince(started);
    LOG_INFO(Core,
             "DSMod msbt: '{}' decoded in {:.2f} ms ({} labels, encoding {}, {} used label(s) "
             "checked against {}: {} character(s) not drawable)",
             job.key, out->ms, label_count, file.encoding, mj.labels.size(), out->font_used,
             out->uncovered);
    result.msbt = std::move(out);
}

void NxAssetState::Rasterize(CompositeCache& cache, const CompositeRequest& request, size_t index) {
    const CompositeDef& def = *request.def;
    const CompositeLayer& layer = def.layers[index];
    const auto started = NxClock::now();
    std::string error;
    Image source;
    if (LoadImage(layer.src, request.roots, source, error) != LocateStatus::Ok) {
        LOG_WARNING(Core, "DSMod composite '{}': layer {} '{}' unavailable: {}", def.name, index,
                    layer.src, error);
        cache.state[index] = LayerState::Failed;
        cache.retries.Fail(def.name, index);
        --cache.todo;
        return;
    }
    const auto decoded = NxClock::now();
    cache.decode_ms += std::chrono::duration<double, std::milli>(decoded - started).count();
    cache.peak_transient = std::max<u64>(cache.peak_transient, u64{source.pixels.size()} * 4);
    const auto dest = [&](size_t i) {
        const auto& r = def.layers[i].rect;
        return std::array<double, 4>{r[0], r[1], double{r[0]} + r[2], double{r[1]} + r[3]};
    };
    const auto build_mask = [&](size_t i, const Image& mask) {
        const auto& m = def.layers[i].mask_src_rect;
        const auto d = dest(i);
        cache.masks[i] =
            NX::ResampleArea(mask, m[0] * mask.w, m[1] * mask.h, m[2] * mask.w, m[3] * mask.h, d[0],
                             d[1], d[2], d[3], def.w, def.h, false);
        cache.mask_state[i] = LayerState::Done;
    };
    double sx0 = layer.src_rect[0] * source.w;
    double sy0 = layer.src_rect[1] * source.h;
    double sx1 = layer.src_rect[2] * source.w;
    double sy1 = layer.src_rect[3] * source.h;
    if (layer.src_px[2] > 0 && layer.src_px[3] > 0) {
        sx0 = layer.src_px[0];
        sy0 = layer.src_px[1];
        sx1 = double{layer.src_px[0]} + layer.src_px[2];
        sy1 = double{layer.src_px[1]} + layer.src_px[3];
    }
    const auto d = dest(index);
    cache.rasters[index] =
        NX::ResampleArea(source, sx0, sy0, sx1, sy1, d[0], d[1], d[2], d[3], def.w, def.h);
    // The same picture may cut other layers: resample those masks while it is decoded.
    for (size_t j = 0; j < def.layers.size(); ++j) {
        if (cache.mask_state[j] == LayerState::Todo && def.layers[j].mask_src == layer.src) {
            build_mask(j, source);
        }
    }
    source = {};
    if (!layer.mask_src.empty()) {
        if (cache.mask_state[index] == LayerState::Todo) {
            const auto mask_started = NxClock::now();
            Image mask;
            if (LoadImage(layer.mask_src, request.roots, mask, error) == LocateStatus::Ok) {
                cache.decode_ms += MsSince(mask_started);
                cache.peak_transient =
                    std::max<u64>(cache.peak_transient, u64{mask.pixels.size()} * 4);
                for (size_t j = 0; j < def.layers.size(); ++j) {
                    if (cache.mask_state[j] == LayerState::Todo &&
                        def.layers[j].mask_src == layer.mask_src) {
                        build_mask(j, mask);
                    }
                }
            } else {
                LOG_WARNING(Core, "DSMod composite '{}': mask '{}' unavailable: {}", def.name,
                            layer.mask_src, error);
                cache.mask_state[index] = LayerState::Failed;
            }
        }
        if (cache.mask_state[index] == LayerState::Done) {
            NX::ApplyMask(cache.rasters[index], cache.masks[index]);
            cache.masks[index] = {};
        } else {
            // A masked layer without its mask would paint its whole rectangle: leave it out.
            cache.rasters[index] = {};
            cache.state[index] = LayerState::Failed;
            cache.retries.Fail(def.name, index);
            --cache.todo;
            return;
        }
    }
    NX::TrimTransparent(cache.rasters[index]);
    cache.resample_ms += MsSince(decoded);
    cache.raster_bytes += cache.rasters[index].premul.size();
    cache.state[index] = LayerState::Done;
    --cache.todo;
    if (cache.retries.failures[index] != 0) {
        LOG_INFO(Core, "DSMod composite '{}': layer {} read after {} failed attempt(s)", def.name,
                 index, cache.retries.failures[index]);
        cache.retries.Clear(index);
        cache.recompose = true;
    }
}

void NxAssetState::PushComposite(const std::string& name, const CompositeRequest& request,
                                 Image&& image) {
    Result result;
    result.kind = ResultKind::Composite;
    result.key = name;
    result.generation = request.generation;
    result.image = std::move(image);
    std::scoped_lock lock{queue_mutex};
    results.push_back(std::move(result));
}

NX::PlacedRaster NxAssetState::FlatLayer(FlatCache& cache, const CompositeRequest& request,
                                         size_t index, bool stash, bool& failed) {
    const CompositeDef& def = *request.def;
    const CompositeLayer& layer = def.layers[index];
    const auto started = NxClock::now();
    std::string error;
    Image source;
    failed = false;
    if (LoadImage(layer.src, request.roots, source, error) != LocateStatus::Ok) {
        LOG_WARNING(Core, "DSMod composite '{}': layer {} '{}' unavailable: {}", def.name, index,
                    layer.src, error);
        failed = true;
        return {};
    }
    cache.decode_ms += MsSince(started);
    cache.peak_transient = std::max<u64>(cache.peak_transient, u64{source.pixels.size()} * 4);
    const auto resampled = NxClock::now();
    const auto dest = [&](size_t i) {
        const auto& r = def.layers[i].rect;
        return std::array<double, 4>{r[0], r[1], double{r[0]} + r[2], double{r[1]} + r[3]};
    };
    const auto mask_of = [&](size_t i, const Image& mask) {
        const auto& m = def.layers[i].mask_src_rect;
        const auto d = dest(i);
        return ToAlphaMask(NX::ResampleArea(mask, m[0] * mask.w, m[1] * mask.h, m[2] * mask.w,
                                            m[3] * mask.h, d[0], d[1], d[2], d[3], def.w, def.h,
                                            false));
    };
    double sx0 = layer.src_rect[0] * source.w;
    double sy0 = layer.src_rect[1] * source.h;
    double sx1 = layer.src_rect[2] * source.w;
    double sy1 = layer.src_rect[3] * source.h;
    if (layer.src_px[2] > 0 && layer.src_px[3] > 0) {
        sx0 = layer.src_px[0];
        sy0 = layer.src_px[1];
        sx1 = double{layer.src_px[0]} + layer.src_px[2];
        sy1 = double{layer.src_px[1]} + layer.src_px[3];
    }
    const auto d = dest(index);
    NX::PlacedRaster raster =
        NX::ResampleArea(source, sx0, sy0, sx1, sy1, d[0], d[1], d[2], d[3], def.w, def.h);
    // A later layer of the build cut by this very picture: keep just its alpha until then, so the
    // picture is not decoded a second time.
    if (stash) {
        for (size_t j = index + 1; j < def.layers.size(); ++j) {
            if (j < cache.target.size() && cache.target[j] != 0 &&
                def.layers[j].mask_src == layer.src && !cache.masks.contains(j)) {
                cache.masks.emplace(j, mask_of(j, source));
            }
        }
        u64 held = 0;
        for (const auto& [k, m] : cache.masks) {
            held += m.a.size();
        }
        cache.peak_masks = std::max(cache.peak_masks, held);
    }
    source = {};
    if (!layer.mask_src.empty()) {
        AlphaMask mask;
        if (const auto kept = cache.masks.find(index); kept != cache.masks.end()) {
            mask = std::move(kept->second);
            cache.masks.erase(kept);
        } else {
            const auto mask_started = NxClock::now();
            Image image;
            if (LoadImage(layer.mask_src, request.roots, image, error) != LocateStatus::Ok) {
                LOG_WARNING(Core, "DSMod composite '{}': mask '{}' unavailable: {}", def.name,
                            layer.mask_src, error);
                failed = true;
                return {};
            }
            cache.decode_ms += MsSince(mask_started);
            mask = mask_of(index, image);
        }
        ApplyAlphaMask(raster, mask);
    }
    NX::TrimTransparent(raster);
    cache.resample_ms += MsSince(resampled);
    return raster;
}

void NxAssetState::RunFlatStep(const std::string& name, const CompositeRequest& request) {
    const CompositeDef& def = *request.def;
    FlatCache& fc = flat_caches[name];
    const size_t n = def.layers.size();
    if (fc.def != request.def) {
        fc = {};
        fc.def = request.def;
        fc.in_base.assign(n, 0);
        fc.retries.Reset(n);
        fc.started = NxClock::now();
    }
    std::vector<u8> wanted(n, 0);
    for (size_t i = 0; i < n && i < request.alpha.size(); ++i) {
        wanted[i] = request.alpha[i] > 0.0f ? 1 : 0;
    }
    const auto mark_clean = [&] {
        std::scoped_lock lock{queue_mutex};
        if (const auto it = requests.find(name);
            it != requests.end() && it->second.serial == request.serial) {
            it->second.dirty = false;
        }
    };
    const auto blank = [&def] {
        std::vector<u8> canvas(u64{def.w} * def.h * 4, 0);
        const u32 bg = def.background;
        const u32 ba = bg >> 24;
        if (ba != 0) {
            const std::array<u8, 4> fill{static_cast<u8>(((bg >> 16) & 0xFF) * ba / 255),
                                         static_cast<u8>(((bg >> 8) & 0xFF) * ba / 255),
                                         static_cast<u8>((bg & 0xFF) * ba / 255),
                                         static_cast<u8>(ba)};
            for (size_t p = 0; p < canvas.size(); p += 4) {
                std::memcpy(canvas.data() + p, fill.data(), 4);
            }
        }
        return canvas;
    };
    if (fc.building) {
        // A layer already drawn into the build must go again: start over. Newly wanted layers
        // still ahead of the cursor simply join the build; ones behind it are added afterwards.
        bool restart = false;
        for (size_t i = 0; i < fc.cursor && i < n; ++i) {
            restart = restart || (fc.target[i] != 0 && wanted[i] == 0);
        }
        if (restart) {
            fc.building = false;
        } else {
            for (size_t i = fc.cursor; i < n; ++i) {
                fc.target[i] = wanted[i];
            }
        }
    }
    if (!fc.building) {
        bool rebuild = !fc.built;
        for (size_t i = 0; i < n; ++i) {
            rebuild = rebuild || (fc.in_base[i] != 0 && wanted[i] == 0);
        }
        if (rebuild) {
            fc.building = true;
            fc.target = wanted;
            fc.cursor = 0;
            fc.canvas = blank();
            fc.masks.clear();
            fc.retries.Reset(n);
            return;
        }
        // Settled picture: draw each newly wanted layer on top, one per step. A layer whose
        // picture could not be read waits for its retry (NextRetry wakes the worker for it).
        const auto now = NxClock::now();
        for (size_t i = 0; i < n; ++i) {
            if (wanted[i] == 0) {
                fc.retries.Clear(i); // a later reveal starts with fresh attempts
            }
        }
        for (size_t i = 0; i < n; ++i) {
            if (wanted[i] != 0 && fc.in_base[i] == 0 && fc.retries.Ready(i, now)) {
                bool failed = false;
                const NX::PlacedRaster raster = FlatLayer(fc, request, i, false, failed);
                if (failed) {
                    fc.retries.Fail(name, i);
                    return;
                }
                if (fc.retries.failures[i] != 0) {
                    LOG_INFO(Core, "DSMod composite '{}': layer {} read after {} failed attempt(s)",
                             name, i, fc.retries.failures[i]);
                    fc.retries.Clear(i);
                }
                NX::BlendPremul(fc.base, def.w, def.h, raster, def.layers[i].opacity);
                fc.in_base[i] = 1;
                fc.publish = true;
                ++fc.adds;
                return;
            }
        }
        if (fc.publish) {
            fc.publish = false;
            const auto started = NxClock::now();
            Image image = MakePackedImage(def, fc.base);
            const u64 packed_bytes = u64{image.w} * image.h * 4;
            const u32 pw = image.w, ph = image.h;
            PushComposite(name, request, std::move(image));
            if (!fc.announced) {
                fc.announced = true;
                LOG_INFO(Core,
                         "DSMod composite '{}' (flat {}x{}, packed {}x{}): first image after "
                         "{:.1f} ms -- decode {:.1f} ms, resample {:.1f} ms, publish {:.1f} ms; "
                         "retained canvas {:.2f} MB, packed picture {:.2f} MB, largest full-size "
                         "decode {:.2f} MB (freed), mask alphas held at most {:.2f} MB",
                         name, def.w, def.h, pw, ph, MsSince(fc.started), fc.decode_ms,
                         fc.resample_ms, MsSince(started), fc.base.size() / 1048576.0,
                         packed_bytes / 1048576.0, fc.peak_transient / 1048576.0,
                         fc.peak_masks / 1048576.0);
            } else {
                LOG_INFO(Core,
                         "DSMod composite '{}': {}{} layer(s) added on top, published in {:.1f} ms "
                         "(decode total {:.1f} ms, mask alphas held at most {:.2f} MB)",
                         name, fc.rebuilds != 0 ? "rebuilt (a layer went away), " : "", fc.adds,
                         MsSince(started), fc.decode_ms, fc.peak_masks / 1048576.0);
            }
            fc.adds = 0;
            fc.rebuilds = 0;
        }
        mark_clean();
        return;
    }
    // Building: the next wanted layer, in order.
    while (fc.cursor < n && fc.target[fc.cursor] == 0) {
        ++fc.cursor;
    }
    if (fc.cursor < n) {
        bool failed = false;
        const NX::PlacedRaster raster = FlatLayer(fc, request, fc.cursor, true, failed);
        if (failed) {
            // Left out of this build; the settled pass tries it again on top.
            fc.target[fc.cursor] = 2;
            fc.retries.Fail(name, fc.cursor);
        } else {
            NX::BlendPremul(fc.canvas, def.w, def.h, raster, def.layers[fc.cursor].opacity);
        }
        ++fc.cursor;
        return;
    }
    fc.base = std::move(fc.canvas);
    fc.canvas = {};
    fc.in_base = fc.target;
    for (auto& v : fc.in_base) {
        v = v == 1 ? 1 : 0; // 2 = wanted but its picture failed: not in the base
    }
    fc.masks.clear();
    if (fc.built) {
        ++fc.rebuilds;
    }
    fc.building = false;
    fc.built = true;
    fc.publish = true;
}

void NxAssetState::RunCompositeStep(const std::string& name) {
    CompositeRequest request;
    {
        std::scoped_lock lock{queue_mutex};
        const auto it = requests.find(name);
        if (it == requests.end()) {
            return;
        }
        request = it->second;
    }
    if (request.def && request.def->flat) {
        RunFlatStep(name, request);
        return;
    }
    const CompositeDef& def = *request.def;
    CompositeCache& cache = caches[name];
    if (cache.def != request.def) {
        cache = {};
        cache.def = request.def;
        const size_t n = def.layers.size();
        cache.rasters.assign(n, {});
        cache.masks.assign(n, {});
        cache.state.assign(n, LayerState::Todo);
        cache.mask_state.assign(n, LayerState::Todo);
        for (size_t i = 0; i < n; ++i) {
            if (def.layers[i].mask_src.empty()) {
                cache.mask_state[i] = LayerState::Done;
            }
        }
        cache.todo = n;
        cache.retries.Reset(n);
        cache.started = NxClock::now();
        cache.stats_window = cache.started;
    }
    // A failed layer whose retry is due goes back on the list.
    const auto now = NxClock::now();
    for (size_t i = 0; i < def.layers.size(); ++i) {
        if (cache.state[i] == LayerState::Failed && cache.retries.Pending(i) &&
            cache.retries.Ready(i, now)) {
            cache.state[i] = LayerState::Todo;
            if (cache.mask_state[i] == LayerState::Failed) {
                cache.mask_state[i] = LayerState::Todo;
            }
            ++cache.todo;
        }
    }
    // 1. Visible layers first, one per step, so plain image requests interleave.
    for (size_t i = 0; i < def.layers.size(); ++i) {
        if (cache.state[i] == LayerState::Todo && i < request.alpha.size() &&
            request.alpha[i] > 0.0f) {
            Rasterize(cache, request, i);
            return;
        }
    }
    // 2. Everything visible is ready: compose the requested state.
    if (request.dirty && cache.composed_serial == request.serial && !cache.recompose) {
        std::scoped_lock lock{queue_mutex};
        if (const auto it = requests.find(name);
            it != requests.end() && it->second.serial == request.serial) {
            it->second.dirty = false;
        }
        return;
    }
    if (request.dirty || cache.recompose) {
        cache.recompose = false;
        const auto started = NxClock::now();
        const size_t n = def.layers.size();
        const auto alpha_of = [&](size_t i) {
            return i < request.alpha.size() && cache.state[i] == LayerState::Done ? request.alpha[i]
                                                                                  : 0.0f;
        };
        // Layers below the first one still fading are identical from frame to frame.
        size_t split = n;
        for (size_t i = 0; i < n; ++i) {
            const float a = alpha_of(i);
            if (a > 0.0f && a < 1.0f) {
                split = i;
                break;
            }
        }
        std::vector<float> below(split);
        for (size_t i = 0; i < split; ++i) {
            below[i] = alpha_of(i);
        }
        std::vector<u8> canvas;
        const bool reuse = split < n && !cache.base.empty() && cache.base_alpha == below;
        if (reuse) {
            canvas = cache.base;
        } else {
            canvas.assign(u64{def.w} * def.h * 4, 0);
        }
        const u32 bg = def.background;
        const u32 ba = bg >> 24;
        const std::array<u8, 4> fill{static_cast<u8>(((bg >> 16) & 0xFF) * ba / 255),
                                     static_cast<u8>(((bg >> 8) & 0xFF) * ba / 255),
                                     static_cast<u8>((bg & 0xFF) * ba / 255), static_cast<u8>(ba)};
        size_t drawn = 0;
        const auto blend = [&](size_t from, size_t to) {
            for (size_t i = from; i < to; ++i) {
                const float a = alpha_of(i);
                if (a > 0.0f) {
                    NX::BlendPremul(canvas, def.w, def.h, cache.rasters[i],
                                    a * def.layers[i].opacity);
                    ++drawn;
                }
            }
        };
        if (!reuse) {
            if (ba != 0) {
                for (size_t p = 0; p < canvas.size(); p += 4) {
                    std::memcpy(canvas.data() + p, fill.data(), 4);
                }
            }
            blend(0, split);
            if (split < n) {
                cache.base = canvas;
                cache.base_alpha = std::move(below);
            } else {
                cache.base.clear(); // nothing animating: no copy to keep
                cache.base_alpha.clear();
            }
        }
        blend(split, n);
        Result result;
        result.kind = ResultKind::Composite;
        result.key = name;
        result.generation = request.generation;
        result.image = MakePackedImage(def, canvas);
        const double compose_ms = MsSince(started);
        cache.composed_serial = request.serial;
        if (!cache.announced) {
            cache.announced = true;
            LOG_INFO(Core,
                     "DSMod composite '{}' ({}x{}): first image after {:.1f} ms -- {} layer(s) "
                     "drawn, decode {:.1f} ms, resample {:.1f} ms, compose {:.2f} ms; layer "
                     "rasters {:.2f} MB, largest full-size decode {:.2f} MB (freed)",
                     name, def.w, def.h, MsSince(cache.started), drawn, cache.decode_ms,
                     cache.resample_ms, compose_ms, cache.raster_bytes / 1048576.0,
                     cache.peak_transient / 1048576.0);
        } else {
            ++cache.recomposes;
            cache.recompose_total_ms += compose_ms;
            cache.recompose_max_ms = std::max(cache.recompose_max_ms, compose_ms);
            if (NxClock::now() - cache.stats_window >= std::chrono::seconds{5}) {
                LOG_INFO(Core,
                         "DSMod composite '{}': {} recompose(s) avg {:.2f} ms max {:.2f} ms "
                         "(worker thread)",
                         name, cache.recomposes, cache.recompose_total_ms / cache.recomposes,
                         cache.recompose_max_ms);
                cache.recomposes = 0;
                cache.recompose_total_ms = 0;
                cache.recompose_max_ms = 0;
                cache.stats_window = NxClock::now();
            }
        }
        std::scoped_lock lock{queue_mutex};
        results.push_back(std::move(result));
        if (const auto it = requests.find(name);
            it != requests.end() && it->second.serial == request.serial) {
            it->second.dirty = false;
        }
        return;
    }
    // 3. Idle: prepare the hidden layers too, so a reveal can start fading at once.
    for (size_t i = 0; i < def.layers.size(); ++i) {
        if (cache.state[i] == LayerState::Todo) {
            Rasterize(cache, request, i);
            if (cache.todo == 0) {
                LOG_INFO(Core,
                         "DSMod composite '{}': all {} layers prepared after {:.1f} ms (decode "
                         "{:.1f} ms, resample {:.1f} ms); resting layer rasters {:.2f} MB, "
                         "largest full-size decode {:.2f} MB (freed), output {:.2f} MB",
                         name, def.layers.size(), MsSince(cache.started), cache.decode_ms,
                         cache.resample_ms, cache.raster_bytes / 1048576.0,
                         cache.peak_transient / 1048576.0, u64{def.w} * def.h * 4 / 1048576.0);
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// ModRuntime side (tick thread)

bool ModRuntime::IsNxAssetSource(const std::string& src) const {
    if (!asset_sources->IsDirectorySource(src)) {
        return false;
    }
    const size_t hash = src.find('#');
    const std::string_view file = std::string_view{src}.substr(0, hash);
    if (EndsWithNoCase(file, ".lzs")) {
        return false; // Story of Seasons archives keep their synchronous path
    }
    if (hash != std::string::npos) {
        return true;
    }
    return EndsWithNoCase(file, ".bntx") || EndsWithNoCase(file, ".bffnt") ||
           EndsWithNoCase(file, ".arc") || EndsWithNoCase(file, ".sarc");
}

bool ModRuntime::NxFallback(const std::string& src) const {
    std::scoped_lock lock{nx_assets->state_mutex};
    return nx_assets->fallback.contains(src);
}

std::shared_ptr<const Image> ModRuntime::GetNxImage(const std::string& src) {
    auto& s = *nx_assets;
    std::scoped_lock state_lock{s.state_mutex}; // pending/failed/fallback
    if (const auto cached = CacheFindImage(src)) {
        return cached;
    }
    if (s.pending.contains(src) || s.failed.contains(src) || s.fallback.contains(src)) {
        return nullptr;
    }
    s.pending.insert(src);
    {
        std::scoped_lock lock{s.queue_mutex};
        s.jobs.push_back(
            {JobKind::Image, src, asset_sources, s.generation, NxClock::now()});
    }
    s.EnsureWorker();
    s.cv.notify_one();
    return nullptr;
}

std::shared_ptr<const Image> ModRuntime::GetCompositeImage(const std::string& name) {
    auto& s = *nx_assets;
    std::scoped_lock state_lock{s.state_mutex}; // fades/missing_composites/live
    if (name.ends_with("#prev")) {
        // The picture a flat composite is cross-fading away from (only while it fades). `s.fades`
        // is NxAssetState's own state_mutex bucket, not image_cache -- a plain-copy shared_ptr
        // (not CacheFindImage) is the correct, safe treatment here whether PumpNxAssets stays on
        // the tick thread or moves with GetImage; correct either way, just not the cheapest
        // possible answer once that is settled.
        const auto fade = s.fades.find(name.substr(0, name.size() - 5));
        return fade != s.fades.end() && fade->second.prev && fade->second.prev->Valid()
                   ? fade->second.prev
                   : nullptr;
    }
    const auto def = manifest.composites.find(name);
    if (def == manifest.composites.end() || !def->second || def->second->w == 0 ||
        def->second->h == 0) {
        if (s.missing_composites.insert(name).second) {
            LOG_WARNING(Core, "DSMod: no usable composite named '{}'", name);
        }
        return nullptr;
    }
    auto& live = s.live[name];
    live.wanted = true;
    return CacheFindImage(std::string{CompositePrefix} + name);
}

bool ModRuntime::ReadNxMember(const std::string& src, std::vector<u8>& out) {
    if (!asset_sources->IsDirectorySource(src)) {
        return false;
    }
    auto& s = *nx_assets;
    // Only an archive at the top decides this path; anything else keeps the older handling.
    const size_t hash = src.find('#');
    Located at;
    std::string error;
    const LocateStatus status =
        s.Locate(src.substr(0, hash), asset_sources, at, error);
    if (status != LocateStatus::Ok || std::memcmp(at.magic.data(), "SARC", 4) != 0) {
        return false;
    }
    out.clear();
    if (s.Locate(src, asset_sources, at, error) != LocateStatus::Ok) {
        LOG_WARNING(Core, "DSMod: '{}': {}", src, error);
        return true;
    }
    if (!s.ReadAll(at, out, error)) {
        LOG_WARNING(Core, "DSMod: '{}': {}", src, error);
        out.clear();
    }
    return true;
}

bool ModRuntime::RequestNxFont() {
    auto& s = *nx_assets;
    std::scoped_lock state_lock{s.state_mutex}; // s.font_key
    if (!IsNxAssetSource(manifest.font_metrics_src) &&
        !EndsWithNoCase(manifest.font_metrics_src.substr(0, manifest.font_metrics_src.find('#')),
                        ".bffnt")) {
        return false;
    }
    s.font_key = manifest.font_metrics_src;
    {
        std::scoped_lock lock{s.queue_mutex};
        s.jobs.push_front({JobKind::Font,
                           s.font_key,
                           asset_sources,
                           s.generation,
                           NxClock::now()});
    }
    s.EnsureWorker();
    s.cv.notify_one();
    LOG_INFO(Core, "DSMod: font '{}' queued for the asset worker", s.font_key);
    return true;
}

void ModRuntime::ResetNxAssets() {
    auto& s = *nx_assets;
    std::scoped_lock state_lock{s.state_mutex}; // clears the whole bucket below
    {
        std::scoped_lock lock{s.queue_mutex};
        ++s.generation;
        s.jobs.clear();
        s.requests.clear();
        s.results.clear();
    }
    s.pending.clear();
    s.failed.clear();
    s.fallback.clear();
    s.missing_composites.clear();
    s.live.clear();
    s.fades.clear();
    s.font_key.clear();
    s.dumps.clear();
    s.msbt = {};
    std::scoped_lock lock{s.io_mutex};
    s.files.clear();
    s.sarcs.clear();
    s.bntxs.clear();
    s.fonts.clear();
}

void ModRuntime::PumpNxAssets(const StateSnapshot& snapshot) {
    auto& s = *nx_assets;
    // Held for the WHOLE function (including PumpTimer's destructor below, which runs before
    // this lock does -- locals destruct in reverse declaration order), covering every touch of
    // the state_mutex bucket this function makes.
    std::scoped_lock state_lock{s.state_mutex};
    const auto pump_started = NxClock::now();
    struct PumpTimer {
        NxAssetState& s;
        NxClock::time_point started;
        ~PumpTimer() {
            const auto now = NxClock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - started).count();
            ++s.pump_calls;
            s.pump_total_ms += ms;
            s.pump_max_ms = std::max(s.pump_max_ms, ms);
            if (now - s.pump_window >= std::chrono::seconds{5}) {
                if (s.posts != 0 || !s.live.empty() || s.pump_max_ms > 1.0) {
                    LOG_INFO(Core,
                             "DSMod perf CPU nx-pump: calls={} avg={:.3f}ms max={:.3f}ms "
                             "composite-requests={}",
                             s.pump_calls, s.pump_total_ms / std::max<u64>(1, s.pump_calls),
                             s.pump_max_ms, s.posts);
                }
                s.pump_calls = 0;
                s.pump_total_ms = 0;
                s.pump_max_ms = 0;
                s.posts = 0;
                s.pump_window = now;
            }
        }
    } pump_timer{s, pump_started};
    std::deque<Result> results;
    {
        std::scoped_lock lock{s.queue_mutex};
        results.swap(s.results);
    }
    bool landed = false;
    for (auto& r : results) {
        if (r.generation != s.generation) {
            continue;
        }
        // Every image_cache touch in this switch goes through CacheFindImage/
        // CachePutImage (asset_cache_mutex-guarded) instead of the raw map -- see those helpers'
        // own comments in mod_runtime.h and mod_assets.cpp.
        switch (r.kind) {
        case ResultKind::Image:
            s.pending.erase(r.key);
            CachePutImage(r.key, std::move(r.image));
            landed = true;
            break;
        case ResultKind::Composite: {
            const std::string key = std::string{CompositePrefix} + r.key;
            auto live = s.live.find(r.key);
            const auto old = CacheFindImage(key);
            if (live != s.live.end() && live->second.pending_fade_ms != 0 && old && old->Valid() &&
                old->w == r.image.w && old->h == r.image.h) {
                // A flat composite gained a layer: keep the picture it replaces and cross-fade.
                // fade.prev shares the old cache entry's immutable Image (CachePutImage below only
                // replaces the map's pointer; readers holding the old one keep it alive).
                auto& fade = s.fades[r.key];
                fade.prev = old;
                fade.start = NxClock::now();
                fade.ms = live->second.pending_fade_ms;
                live->second.pending_fade_ms = 0;
            }
            CachePutImage(key, std::move(r.image));
            ++composite_epoch;
            landed = true;
            break;
        }
        case ResultKind::Msbt:
            if (r.msbt && AcceptMsbt(*r.msbt)) {
                landed = true;
            }
            break;
        case ResultKind::Font: {
            if (r.icon_font) {
                CachePutImage(r.key, std::move(r.image));
                s.msbt.icon_metrics = std::move(r.metrics);
                s.msbt.icon_ready = r.key == s.msbt.icon_key;
                landed = true;
                break;
            }
            const std::string atlas_key =
                manifest.font_atlas_src.empty() ? r.key : manifest.font_atlas_src;
            CachePutImage(atlas_key, std::move(r.image));
            font_metrics = std::move(r.metrics);
            landed = true;
            break;
        }
        case ResultKind::Failed:
            s.pending.erase(r.key);
            if (r.icon_font && r.key == s.msbt.icon_key) {
                s.msbt.icon_failed = true;
                landed = true; // icons are drawn as "(X)" from now on
            }
            if (s.failed.insert(r.key).second) {
                LOG_WARNING(Core, "DSMod: '{}' could not be loaded: {}", r.key, r.message);
            }
            break;
        case ResultKind::Fallback:
            s.pending.erase(r.key);
            s.fallback.insert(r.key);
            landed = true; // the synchronous path gets its turn on the next draw
            break;
        }
    }
    if (landed) {
        ui_signature_valid = false;
        ++asset_epoch; // lets a page transition mid-flight notice an image/composite landing
                       // (UiSignature itself does not depend on asset state -- see mod_runtime.h)
    }

    // Message files: decide the language and start decoding at the first publish (during the
    // game's own start-up), so neither the NACP read nor a decode lands when a page first opens.
    if (manifest.msbt.Enabled() && !s.msbt.resolved) {
        ResolveMsbtLanguage();
        for (const auto& [alias, path] : manifest.msbt.files) {
            RequestMsbt(alias);
        }
    }

    // Console "msbt" asks waiting for their text.
    for (auto it = s.msbt.console.begin(); it != s.msbt.console.end();) {
        const std::shared_ptr<const std::string> text = GetMsbtText(it->ref);
        const size_t hash = it->ref.find('#');
        const std::string alias =
            hash == std::string::npos || hash < 5 ? std::string{} : it->ref.substr(5, hash - 5);
        // Settled without a text: a bad reference, an unreadable file, or a missing label.
        const bool settled = hash == std::string::npos || s.msbt.failed.contains(alias) ||
                             s.msbt.texts.contains(alias);
        const bool timeout = NxClock::now() - it->asked > std::chrono::seconds{30};
        if (text == nullptr && !settled && !timeout) {
            ++it;
            continue;
        }
        std::string shown;
        if (text != nullptr) {
            for (size_t i = 0; i < text->size();) {
                const char32_t c = Msbt::NextUtf8(*text, i);
                if (c >= TextIconBase && c <= TextIconLast) {
                    shown += fmt::format("{{U+{:04X}}}", static_cast<u32>(c - TextIconBase));
                } else if (c == '\n') {
                    shown += "\\n";
                } else {
                    Msbt::AppendUtf8(shown, c);
                }
            }
        }
        EmitConsole(text != nullptr ? fmt::format("DSMod msbt {} [{}/{}] = {}", it->ref,
                                                  s.msbt.chosen.first, s.msbt.chosen.second, shown)
                                    : fmt::format("DSMod msbt {} FAILED ({})", it->ref,
                                                  timeout ? "timeout" : "no such text"));
        it = s.msbt.console.erase(it);
    }

    // Composites: evaluate each layer's binds, animate fades, ask the worker for a new picture
    // whenever what should be on screen moved.
    const auto now = NxClock::now();
    for (auto& [name, live] : s.live) {
        if (!live.wanted) {
            continue;
        }
        const auto found = manifest.composites.find(name);
        if (found == manifest.composites.end() || !found->second) {
            continue;
        }
        const auto& def = found->second;
        const size_t n = def->layers.size();
        if (live.def != def || live.show.size() != n) {
            live = {};
            live.wanted = true;
            live.def = def;
            live.shown.assign(n, 0);
            live.shown_at.assign(n, now);
            live.show.reserve(n);
            live.hide.reserve(n);
            for (const auto& layer : def->layers) {
                live.show.push_back(SplitBind(layer.show_bind));
                live.hide.push_back(SplitBind(layer.hide_bind));
            }
        }
        auto& alpha = live.alpha;
        alpha.assign(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const auto& layer = def->layers[i];
            const bool visible = LiveLayerVisible(live, i, snapshot);
            if (!live.initialized) {
                live.shown[i] = visible; // what is visible at first sight does not fade in
                live.shown_at[i] = now - std::chrono::hours{1};
            } else if (visible && !live.shown[i]) {
                live.shown[i] = 1;
                live.shown_at[i] = now;
            } else if (!visible) {
                live.shown[i] = 0;
            }
            if (!live.shown[i]) {
                continue;
            }
            if (def->flat) {
                // Flat composites fade at draw time: post the final state at once and remember
                // that the picture which lands for it should cross-fade in.
                if (layer.fade_ms > 0 && live.shown_at[i] == now) {
                    live.pending_fade_ms = std::max(live.pending_fade_ms, layer.fade_ms);
                }
                alpha[i] = 1.0f;
                continue;
            }
            float a = 1.0f;
            if (layer.fade_ms > 0) {
                const double ms =
                    std::chrono::duration<double, std::milli>(now - live.shown_at[i]).count();
                a = static_cast<float>(std::clamp(ms / layer.fade_ms, 0.0, 1.0));
                a = std::max(1.0f / 64.0f, std::round(a * 64.0f) / 64.0f);
            }
            alpha[i] = a;
        }
        live.initialized = true;
        if (live.serial != 0 && alpha == live.posted) {
            continue;
        }
        live.posted = alpha;
        ++live.serial;
        ++s.posts;
        {
            std::scoped_lock lock{s.queue_mutex};
            auto& request = s.requests[name];
            request.def = def;
            request.alpha = alpha;
            request.serial = live.serial;
            request.roots = asset_sources;
            request.generation = s.generation;
            request.dirty = true;
        }
        s.EnsureWorker();
        s.cv.notify_one();
    }

    // Console image dumps waiting for their picture.
    for (auto it = s.dumps.begin(); it != s.dumps.end();) {
        const auto cached = CacheFindImage(it->src);
        const bool failed = s.failed.contains(it->src) || image_failed.contains(it->src);
        const bool timeout = now - it->requested > std::chrono::seconds{60};
        if (!cached && !failed && !timeout) {
            if (it->src.starts_with(CompositePrefix)) {
                GetImage(it->src); // keeps the composite wanted
            }
            ++it;
            continue;
        }
        if (cached) {
            const auto rgba = NX::ImageToRgba(*cached);
            const bool ok = stbi_write_png(it->out.c_str(), static_cast<int>(cached->w),
                                           static_cast<int>(cached->h), 4, rgba.data(),
                                           static_cast<int>(cached->w) * 4) != 0;
            EmitConsole(fmt::format(
                "DSMod imgdump {} '{}' {}x{} -> {} ({:.0f} ms)", ok ? "ok" : "write-failed",
                it->src, cached->w, cached->h, it->out,
                std::chrono::duration<double, std::milli>(now - it->requested).count()));
        } else {
            EmitConsole(fmt::format("DSMod imgdump FAILED '{}' ({})", it->src,
                                    timeout ? "timeout" : "not decodable"));
        }
        it = s.dumps.erase(it);
    }
}

float ModRuntime::CompositeFade(const std::string& name) {
    auto& s = *nx_assets;
    std::scoped_lock state_lock{s.state_mutex}; // s.fades
    const auto it = s.fades.find(name);
    if (it == s.fades.end()) {
        return 1.0f;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(NxClock::now() - it->second.start).count();
    if (it->second.ms == 0 || ms >= it->second.ms) {
        s.fades.erase(it);
        ui_signature_valid = false; // draw the settled picture once more
        return 1.0f;
    }
    return static_cast<float>(ms / it->second.ms);
}

void ModRuntime::PublishNxFades(StateSnapshot& snapshot) {
    auto& s = *nx_assets;
    std::scoped_lock state_lock{s.state_mutex}; // s.fades (+ recurses into CompositeFade)
    std::vector<std::string> names;
    names.reserve(s.fades.size());
    for (const auto& [name, fade] : s.fades) {
        names.push_back(name);
    }
    for (const auto& name : names) {
        const float t = CompositeFade(name);
        if (s.fades.contains(name)) {
            snapshot.ints["@fade:" + name] = static_cast<s64>(std::lround(t * 1000.0f));
        }
    }
}

// Console "imgdump" only (DriveCmdImpl, mod_console.cpp), so a dev-tools build only.
#if EDEN_DSMOD_BUILD_DEV_TOOLS
void ModRuntime::NxImageDump(const std::string& args) {
    // s.dumps below, plus GetImage's own nested GetNxImage/GetCompositeImage --
    // recursive_mutex, safe to hold across that call.
    std::scoped_lock state_lock{nx_assets->state_mutex};
    // "imgdump <src> <out.png>": the source may contain spaces, the output path may not.
    std::string text = args;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    const size_t split = text.find_last_of(" \t");
    if (split == std::string::npos) {
        EmitConsole("DSMod imgdump usage: imgdump <src> <out.png>");
        return;
    }
    std::string out = text.substr(split + 1);
    std::string src = text.substr(0, split);
    while (!src.empty() && std::isspace(static_cast<unsigned char>(src.front()))) {
        src.erase(src.begin());
    }
    while (!src.empty() && std::isspace(static_cast<unsigned char>(src.back()))) {
        src.pop_back();
    }
    // A "map:"/"pulse:"/"icon:" source (and any already-cached source) resolves synchronously
    // inside GetImage itself -- but GetImage caches it under its OWN internally-stamped key (a
    // "map:" source appends a fog/fade generation before it ever looks the cache up; see
    // GetImage's append_map_stamp), not under the literal `src` typed here. The old code always
    // queued `src` (this function's own unstamped copy) for PumpNxAssets to poll image_cache by
    // later, so a "map:" dump could NEVER be found by that later lookup and always timed out at
    // 60s ("FAILED (timeout)") -- a real, pre-existing bug, not a slow decode. Write it out right
    // here from the pointer GetImage just handed back instead of re-deriving (and getting wrong)
    // the stamped key. Only a source that is still loading asynchronously (composite:/NX assets
    // not yet decoded by the asset worker) returns nullptr on this first call; that case is
    // unaffected and still falls through to the existing queue-and-poll path below.
    if (const std::shared_ptr<const Image> image = GetImage(src); image != nullptr) {
        const auto rgba = NX::ImageToRgba(*image);
        const bool ok =
            stbi_write_png(out.c_str(), static_cast<int>(image->w), static_cast<int>(image->h), 4,
                           rgba.data(), static_cast<int>(image->w) * 4) != 0;
        EmitConsole(fmt::format("DSMod imgdump {} '{}' {}x{} -> {} (0 ms)",
                                ok ? "ok" : "write-failed", src, image->w, image->h, out));
        return;
    }
    nx_assets->dumps.push_back({src, out, NxClock::now()});
    EmitConsole(fmt::format("DSMod imgdump queued '{}'", src));
}
#endif // EDEN_DSMOD_BUILD_DEV_TOOLS

// ---------------------------------------------------------------------------------------------
// "msbt:" text

namespace {

std::string_view ApplicationLanguageName(Service::NS::ApplicationLanguage lang) {
    using L = Service::NS::ApplicationLanguage;
    switch (lang) {
    case L::AmericanEnglish:
        return "AmericanEnglish";
    case L::BritishEnglish:
        return "BritishEnglish";
    case L::Japanese:
        return "Japanese";
    case L::French:
        return "French";
    case L::German:
        return "German";
    case L::LatinAmericanSpanish:
        return "LatinAmericanSpanish";
    case L::Spanish:
        return "Spanish";
    case L::Italian:
        return "Italian";
    case L::Dutch:
        return "Dutch";
    case L::CanadianFrench:
        return "CanadianFrench";
    case L::Portuguese:
        return "Portuguese";
    case L::Russian:
        return "Russian";
    case L::Korean:
        return "Korean";
    case L::TraditionalChinese:
        return "TraditionalChinese";
    case L::SimplifiedChinese:
        return "SimplifiedChinese";
    case L::BrazilianPortuguese:
        return "BrazilianPortuguese";
    case L::Polish:
        return "Polish";
    case L::Thai:
        return "Thai";
    default:
        return {};
    }
}

std::string ReplaceAll(std::string text, std::string_view what, std::string_view with) {
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at)) {
        text.replace(at, what.size(), with);
        at += with.size();
    }
    return text;
}

} // namespace

void ModRuntime::ResolveMsbtLanguage() {
    std::scoped_lock state_lock{nx_assets->state_mutex};
    auto& m = nx_assets->msbt;
    m.resolved = true;
    const auto started = NxClock::now();
    m.settings_index = static_cast<s32>(Settings::values.language_index.GetValue());
    m.settings_name =
        std::string{Settings::CanonicalizeEnum(Settings::values.language_index.GetValue())};
    // What the game itself is given: IApplicationFunctions::GetDesiredLanguage = the settings
    // language, walked down its priority list until the title's NACP supports one.
    if (m.settings_index >= 0 &&
        static_cast<size_t>(m.settings_index) < Service::Set::available_language_codes.size()) {
        const auto code =
            Service::Set::available_language_codes[static_cast<size_t>(m.settings_index)];
        const auto metadata =
            FileSys::PatchManager::GetMetadataFromBaseOrUpdate(system, manifest.title_id);
        m.nacp_languages = metadata.first != nullptr ? metadata.first->GetSupportedLanguages() : 0;
        if (const auto app = Service::NS::ConvertToApplicationLanguage(code)) {
            if (const auto* const list = Service::NS::GetApplicationLanguagePriorityList(*app)) {
                for (const auto lang : *list) {
                    const u32 flag = Service::NS::GetSupportedLanguageFlag(lang);
                    if (m.nacp_languages == 0 || (m.nacp_languages & flag) == flag) {
                        m.desired = std::string{ApplicationLanguageName(lang)};
                        break;
                    }
                }
            }
        }
    }
    const auto& cfg = manifest.msbt;
    for (const std::string& key : {m.desired, std::to_string(m.settings_index), m.settings_name}) {
        if (key.empty()) {
            continue;
        }
        if (const auto it = cfg.langs.find(key); it != cfg.langs.end()) {
            m.matched_key = key;
            m.chosen = it->second;
            m.have_language = true;
            break;
        }
    }
    if (!m.have_language && cfg.has_fallback) {
        m.chosen = cfg.fallback;
        m.have_language = true;
        m.on_fallback = true;
        m.fallback_reason = "no msbt_lang entry";
    }
    LOG_INFO(Core,
             "DSMod msbt: settings language {} ({}), the game is given '{}' (NACP languages "
             "{:08X}) -> key '{}' -> {}/{}{} ({:.2f} ms)",
             m.settings_index, m.settings_name, m.desired, m.nacp_languages, m.matched_key,
             m.chosen.first, m.chosen.second, m.on_fallback ? " (fallback)" : "", MsSince(started));
}

void ModRuntime::RequestMsbt(const std::string& alias) {
    auto& s = *nx_assets;
    std::scoped_lock state_lock{s.state_mutex};
    auto& m = s.msbt;
    if (m.queued.contains(alias) || m.failed.contains(alias)) {
        return;
    }
    const auto file = manifest.msbt.files.find(alias);
    if (file == manifest.msbt.files.end() || !m.have_language) {
        m.failed.insert(alias);
        LOG_WARNING(Core, "DSMod msbt: {} '{}'",
                    m.have_language ? "no \"msbt\" file for alias" : "no language for", alias);
        return;
    }
    const std::string path =
        ReplaceAll(ReplaceAll(file->second, "{REGION}", m.chosen.first), "{LANG}", m.chosen.second);
    auto job = std::make_shared<MsbtJob>();
    job->alias = alias;
    job->serial = m.serial;
    job->icon_glyphs = manifest.msbt.icon_glyphs;
    job->options = {manifest.msbt.icon_group, manifest.msbt.icon_type, &job->icon_glyphs};
    const std::string prefix = "msbt:" + alias + "#";
    const auto add_label = [&](const std::string& ref) {
        if (ref.starts_with(prefix) && ref.find("{i}") == std::string::npos) {
            std::string label = ref.substr(prefix.size());
            if (std::find(job->labels.begin(), job->labels.end(), label) == job->labels.end()) {
                job->labels.push_back(std::move(label));
            }
        }
    };
    for (const auto& page : manifest.pages) {
        for (const auto& w : page.widgets) {
            add_label(w.text_src);
            if (w.text_map) {
                for (const auto& [value, ref] : *w.text_map) {
                    add_label(ref);
                }
            }
        }
    }
    const std::string_view font_file =
        std::string_view{manifest.font_metrics_src}.substr(0, manifest.font_metrics_src.find('#'));
    if (IsNxAssetSource(manifest.font_metrics_src) || EndsWithNoCase(font_file, ".bffnt")) {
        job->font_src = manifest.font_metrics_src;
    }
    m.queued.insert(alias);
    const size_t used_labels = job->labels.size();
    {
        std::scoped_lock lock{s.queue_mutex};
        Job queued{
            JobKind::Msbt, path, asset_sources, s.generation, NxClock::now()};
        queued.msbt = std::move(job);
        s.jobs.push_back(std::move(queued));
        if (!manifest.msbt.icon_font.empty() && !m.icon_requested) {
            m.icon_requested = true;
            m.icon_key = manifest.msbt.icon_font;
            Job icon{JobKind::Font,
                     m.icon_key,
                     asset_sources,
                     s.generation,
                     NxClock::now()};
            icon.icon_font = true;
            s.jobs.push_back(std::move(icon));
        }
    }
    s.EnsureWorker();
    s.cv.notify_one();
    LOG_INFO(Core, "DSMod msbt: '{}' queued ({} used label(s))", path, used_labels);
}

bool ModRuntime::AcceptMsbt(const MsbtResult& r) {
    std::scoped_lock state_lock{nx_assets->state_mutex};
    auto& m = nx_assets->msbt;
    if (r.serial != m.serial) {
        return false; // decoded for a language no longer in use
    }
    // Unknown tags and unmapped icons: stripped, each reported once per session (one line).
    std::string tags, icons;
    for (const u32 tag : r.stripped) {
        if (m.logged.insert(fmt::format("tag {}", tag)).second) {
            tags += fmt::format(" {}{}.{}", (tag >> 31) != 0 ? "/" : "", (tag >> 16) & 0x7FFF,
                                tag & 0xFFFF);
        }
    }
    for (const u32 index : r.unmapped_icons) {
        if (m.logged.insert(fmt::format("icon {}", index)).second) {
            icons += fmt::format(" {}", index);
        }
    }
    if (!tags.empty() || !icons.empty()) {
        LOG_INFO(Core,
                 "DSMod msbt: '{}' stripped: tag(s) [{} ] (group.type, / = closing), icon "
                 "parameter(s) without an msbt_icons glyph [{} ]",
                 r.path, tags, icons);
    }
    const bool usable = r.ok && r.uncovered == 0;
    const auto& fallback = manifest.msbt.fallback;
    if (!usable && !m.on_fallback && manifest.msbt.has_fallback && m.chosen != fallback) {
        std::string why;
        if (!r.ok) {
            why = fmt::format("'{}' could not be read ({})", r.path, r.error);
        } else {
            std::string sample;
            for (const char32_t c : r.uncovered_sample) {
                sample += fmt::format(" U+{:04X}", static_cast<u32>(c));
            }
            why = fmt::format("{} character(s) of the used labels are not in {} (e.g.{})",
                              r.uncovered, r.font_used, sample);
        }
        LOG_WARNING(Core, "DSMod msbt: {}/{} not usable: {} -> fallback {}/{}", m.chosen.first,
                    m.chosen.second, why, fallback.first, fallback.second);
        m.fallback_reason = why;
        m.on_fallback = true;
        m.chosen = fallback;
        ++m.serial;
        m.texts.clear();
        m.queued.clear();
        m.failed.clear();
        return true;
    }
    if (!r.ok) {
        m.failed.insert(r.alias);
        LOG_WARNING(Core, "DSMod msbt: '{}' could not be read: {}", r.path, r.error);
        return true;
    }
    if (r.uncovered != 0) {
        LOG_WARNING(Core, "DSMod msbt: '{}' has {} character(s) {} cannot draw; shown anyway",
                    r.path, r.uncovered, r.font_used);
    }
    for (const auto& label : r.missing_labels) {
        if (m.logged.insert("label " + r.alias + "#" + label).second) {
            LOG_WARNING(Core, "DSMod msbt: '{}' has no label '{}'", r.path, label);
        }
    }
    m.texts[r.alias] = r.texts;
    return true;
}

std::shared_ptr<const std::string> ModRuntime::GetMsbtText(const std::string& ref) {
    // Called as RenderPage's TextProvider, so reachable from the redraw worker. Recursive: calls
    // ResolveMsbtLanguage/RequestMsbt below while already holding this.
    //
    // Returns a shared_ptr, not a raw `const std::string*` into `*texts->second` (the alias's
    // label map): a raw pointer would outlive this lock's release -- the same hazard shape
    // image_cache avoids (ImageProvider -> shared_ptr<const Image>). `AcceptMsbt`'s
    // `m.texts[r.alias] = r.texts;` (a language re-resolution landing a fresh decode for an
    // already-resolved alias -- MsbtState::serial's own doc comment says this is a real path, not
    // contrived) drops the OLD shared_ptr's refcount; a reader still mid-use of a raw pointer from
    // an earlier call would read freed memory (the tsan_redraw_harness msbt-pointer probe crashes
    // with SIGSEGV in under a second against the raw-pointer version). So, the same way as
    // image_cache: the ALIASING shared_ptr constructor below shares texts->second's own
    // refcount (keeping the whole label map alive, cheap -- one atomic increment) while pointing at
    // the one string the caller actually asked for, so the caller's returned handle survives
    // whatever AcceptMsbt does to the map meanwhile, exactly like CacheFindImage's shared_ptr does
    // for image_cache.
    std::scoped_lock state_lock{nx_assets->state_mutex};
    auto& m = nx_assets->msbt;
    const size_t hash = ref.find('#');
    if (!ref.starts_with("msbt:") || hash == std::string::npos || hash == 5) {
        if (m.logged.insert("ref " + ref).second) {
            LOG_WARNING(Core, "DSMod msbt: bad text reference '{}' (msbt:<alias>#<label>)", ref);
        }
        return nullptr;
    }
    if (!m.resolved) {
        ResolveMsbtLanguage();
    }
    const std::string alias = ref.substr(5, hash - 5);
    const auto texts = m.texts.find(alias);
    if (texts == m.texts.end()) {
        RequestMsbt(alias);
        return nullptr;
    }
    const auto it = texts->second->find(ref.substr(hash + 1));
    if (it == texts->second->end()) {
        if (m.logged.insert("label " + ref.substr(5)).second) {
            LOG_WARNING(Core, "DSMod msbt: no label '{}' in '{}'", ref.substr(hash + 1), alias);
        }
        return nullptr;
    }
    return std::shared_ptr<const std::string>(texts->second, &it->second);
}

const FontMetrics* ModRuntime::NxIconFont(std::shared_ptr<const Image>& atlas, bool& pending,
                                          FontMetrics* metrics_copy) {
    // `metrics_copy`, when given, is filled from `m.icon_metrics` while STILL holding
    // this lock -- PublishUi (mod_redraw.cpp, a different translation unit) cannot take
    // nx_assets->state_mutex itself (NxAssetState is an opaque/incomplete type there, PIMPL'd on
    // purpose), so this is the one sanctioned way a caller outside this file gets a safe, owned
    // copy instead of racing a caller-side dereference against PumpNxAssets's own Font-case
    // reassignment of m.icon_metrics after this function has already returned and unlocked.
    std::scoped_lock state_lock{nx_assets->state_mutex};
    const auto& m = nx_assets->msbt;
    atlas = nullptr;
    pending = m.icon_requested && !m.icon_failed && !m.icon_ready;
    if (!m.icon_ready) {
        return nullptr;
    }
    atlas = CacheFindImage(m.icon_key);
    if (!atlas) {
        return nullptr;
    }
    if (metrics_copy != nullptr) {
        *metrics_copy = m.icon_metrics;
    }
    return &m.icon_metrics;
}

// Console "msbt" only (DriveCmdImpl, mod_console.cpp), so a dev-tools build only.
#if EDEN_DSMOD_BUILD_DEV_TOOLS
void ModRuntime::MsbtConsole(const std::string& args) {
    std::scoped_lock state_lock{nx_assets->state_mutex};
    auto& m = nx_assets->msbt;
    std::string ref = args;
    while (!ref.empty() && std::isspace(static_cast<unsigned char>(ref.front()))) {
        ref.erase(ref.begin());
    }
    while (!ref.empty() && std::isspace(static_cast<unsigned char>(ref.back()))) {
        ref.pop_back();
    }
    if (!m.resolved) {
        ResolveMsbtLanguage();
    }
    if (ref.empty()) {
        EmitConsole(fmt::format("DSMod msbt lang: settings {} ({}), game given '{}', NACP {:08X}, "
                                "key '{}' -> {}/{}{}{}",
                                m.settings_index, m.settings_name, m.desired, m.nacp_languages,
                                m.matched_key, m.chosen.first, m.chosen.second,
                                m.on_fallback ? " FALLBACK: " : "", m.fallback_reason));
        return;
    }
    if (!ref.starts_with("msbt:")) {
        ref = "msbt:" + ref;
    }
    GetMsbtText(ref);
    m.console.push_back({ref, NxClock::now()});
}
#endif // EDEN_DSMOD_BUILD_DEV_TOOLS

// ---------------------------------------------------------------------------------------------
// Manifest

namespace NxAssets {

namespace {

void ParseMsbtConfig(const nlohmann::json& json, MsbtConfig& out) {
    out = {};
    const auto pair_of = [](const nlohmann::json& v, std::pair<std::string, std::string>& pair) {
        if (!v.is_array() || v.size() != 2 || !v[0].is_string() || !v[1].is_string()) {
            return false;
        }
        pair = {v[0].get<std::string>(), v[1].get<std::string>()};
        return true;
    };
    if (json.contains("msbt") && json.at("msbt").is_object()) {
        for (const auto& [alias, path] : json.at("msbt").items()) {
            if (path.is_string()) {
                out.files[alias] = path.get<std::string>();
            }
        }
    }
    if (json.contains("msbt_lang") && json.at("msbt_lang").is_object()) {
        for (const auto& [key, value] : json.at("msbt_lang").items()) {
            std::pair<std::string, std::string> pair;
            if (pair_of(value, pair)) {
                out.langs[key] = std::move(pair);
            } else {
                LOG_WARNING(Core, "DSMod: msbt_lang '{}' must be [\"<REGION>\", \"<LANG>\"]", key);
            }
        }
    }
    if (json.contains("msbt_lang_fallback")) {
        out.has_fallback = pair_of(json.at("msbt_lang_fallback"), out.fallback);
    }
    if (json.contains("msbt_icons") && json.at("msbt_icons").is_object()) {
        const auto& icons = json.at("msbt_icons");
        out.icon_font = icons.value("font", std::string{});
        if (icons.contains("tag") && icons.at("tag").is_array() && icons.at("tag").size() == 2 &&
            icons.at("tag")[0].is_number_unsigned() && icons.at("tag")[1].is_number_unsigned()) {
            out.icon_group = static_cast<u16>(icons.at("tag")[0].get<u32>());
            out.icon_type = static_cast<u16>(icons.at("tag")[1].get<u32>());
        }
        if (icons.contains("glyphs") && icons.at("glyphs").is_object()) {
            for (const auto& [key, value] : icons.at("glyphs").items()) {
                const bool hex = key.starts_with("0x") || key.starts_with("0X");
                char* end = nullptr;
                const unsigned long index = std::strtoul(key.c_str(), &end, hex ? 16 : 10);
                if (end == key.c_str() || *end != '\0' || !value.is_string()) {
                    continue;
                }
                const std::string glyph = value.get<std::string>();
                char32_t code = 0;
                if (glyph.size() > 2 && (glyph[0] == 'U' || glyph[0] == 'u') && glyph[1] == '+') {
                    code = static_cast<char32_t>(std::strtoul(glyph.c_str() + 2, nullptr, 16));
                } else if (!glyph.empty()) {
                    size_t i = 0;
                    code = Msbt::NextUtf8(glyph, i);
                }
                if (code != 0 && code < 0x10000) {
                    out.icon_glyphs[static_cast<u32>(index)] = code;
                }
            }
        }
    }
    if (out.Enabled()) {
        LOG_DEBUG(Core,
                  "DSMod: msbt text: {} file(s), {} language entr(ies), fallback {}/{}, icon font "
                  "'{}' tag {}.{} with {} glyph(s)",
                  out.files.size(), out.langs.size(), out.fallback.first, out.fallback.second,
                  out.icon_font, out.icon_group, out.icon_type, out.icon_glyphs.size());
    }
}

} // namespace

void ParseManifestExtras(const nlohmann::json& json, Manifest& manifest) {
    if (manifest.font_metrics_src.empty()) {
        manifest.font_metrics_src = json.value("font_metrics_src", std::string{});
    }
    if (manifest.font_atlas_src.empty()) {
        manifest.font_atlas_src = json.value("font_atlas_src", std::string{});
    }
    if (manifest.font_atlas_src.empty() &&
        EndsWithNoCase(manifest.font_metrics_src.substr(0, manifest.font_metrics_src.find('#')),
                       ".bffnt")) {
        manifest.font_atlas_src = manifest.font_metrics_src; // a BFFNT carries its own sheets
    }

    ParseMsbtConfig(json, manifest.msbt);
    manifest.composites.clear();
    if (!json.contains("composites") || !json.at("composites").is_object()) {
        return;
    }
    const auto color = [](const nlohmann::json& j, const char* key, u32 fallback) -> u32 {
        if (!j.contains(key)) {
            return fallback;
        }
        const auto& v = j.at(key);
        if (v.is_number_unsigned()) {
            return v.get<u32>();
        }
        if (!v.is_string()) {
            return fallback;
        }
        std::string text = v.get<std::string>();
        if (!text.empty() && text.front() == '#') {
            text.erase(text.begin());
        }
        char* end = nullptr;
        const u32 raw = static_cast<u32>(std::strtoul(text.c_str(), &end, 16));
        if (text.empty() || end == nullptr || *end != '\0') {
            return fallback;
        }
        return text.size() <= 6 ? (0xFF000000u | raw) : raw;
    };
    const auto quad = [](const nlohmann::json& j, const char* key, std::array<float, 4>& out) {
        if (j.contains(key) && j.at(key).is_array() && j.at(key).size() == 4) {
            for (size_t i = 0; i < 4; ++i) {
                out[i] = static_cast<float>(j.at(key)[i].get<double>());
            }
        }
    };
    for (const auto& [name, cj] : json.at("composites").items()) {
        if (!cj.is_object()) {
            continue;
        }
        auto def = std::make_shared<CompositeDef>();
        def->name = name;
        def->w = std::min<u32>(cj.value("w", 0u), 8192);
        def->h = std::min<u32>(cj.value("h", 0u), 8192);
        def->background = color(cj, "background", 0);
        def->flat = cj.value("flat", false);
        if (cj.contains("levels") && cj.at("levels").is_array()) {
            for (const auto& lv : cj.at("levels")) {
                if (lv.is_number()) {
                    def->levels.push_back(static_cast<float>(lv.get<double>()));
                }
            }
        }
        if (cj.contains("layers") && cj.at("layers").is_array()) {
            for (const auto& lj : cj.at("layers")) {
                CompositeLayer layer;
                layer.src = lj.value("src", std::string{});
                quad(lj, "rect", layer.rect);
                quad(lj, "src_rect", layer.src_rect);
                quad(lj, "src_px", layer.src_px);
                layer.mask_src = lj.value("mask_src", std::string{});
                quad(lj, "mask_src_rect", layer.mask_src_rect);
                layer.show_bind = lj.value("show_bind", std::string{});
                layer.hide_bind = lj.value("hide_bind", std::string{});
                layer.fade_ms = lj.value("fade_ms", 0u);
                layer.opacity = static_cast<float>(lj.value("opacity", 1.0));
                if (layer.src.empty() || layer.rect[2] <= 0.0f || layer.rect[3] <= 0.0f) {
                    LOG_WARNING(Core, "DSMod: composite '{}' layer {} has no src or rect", name,
                                def->layers.size());
                    continue;
                }
                def->layers.push_back(std::move(layer));
            }
        }
        LOG_DEBUG(Core, "DSMod: composite '{}' {}x{} with {} layer(s)", name, def->w, def->h,
                  def->layers.size());
        manifest.composites.emplace(name, std::move(def));
    }
}

} // namespace NxAssets

} // namespace Core::Mods
