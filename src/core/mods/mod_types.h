// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Declarative dual-screen mod package (Tier A): everything the runtime needs comes from JSON,
// so a new game needs data, not a new emulator build.
//
//   load/<TITLEID>/<mod>/dualscreen/manifest.json      meta + pages + widgets + actions
//   load/<TITLEID>/<mod>/dualscreen/<BUILDID8>.json    per-build data points (offsets)
//
// This header holds Manifest and StateSnapshot and includes the themed declarations:
//   mod_types_points.h     data points, derived values, finders, PointGate
//   mod_types_map.h        geometry map: rooms, areas, markers, layers, map.style, fog grid
//   mod_types_text.h       game font metrics, MSBT config, inline icon code points
//   mod_types_page.h       pages, widgets, scroll regions, transitions, haptics, easing
//   mod_types_action.h     actions
//   mod_types_guest.h      guest call sequences, patches, enforce rules, IL2CPP layout
//   mod_types_composite.h  composite images
//   mod_types_nav.h        controller navigation ("nav", runtime 17)
// Include this header, not the themed ones, unless a file needs only one theme.

#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <ankerl/unordered_dense.h>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/mods/mod_types_action.h"
#include "core/mods/mod_types_composite.h"
#include "core/mods/mod_types_guest.h"
#include "core/mods/mod_types_map.h"
#include "core/mods/mod_types_nav.h"
#include "core/mods/mod_types_page.h"
#include "core/mods/mod_types_points.h"
#include "core/mods/mod_types_text.h"

namespace Core::Mods {

struct Manifest {
    u32 format{1};
    u64 title_id{};
    std::string running_build_id;
    std::string name;
    std::string mod_dir_name;
    std::string data_file; ///< the per-build data json name, for hot-reload.
    /// Shared value->name lists (items, characters).
    std::map<std::string, std::vector<std::string>> tables;
    /// Byte length of each table's longest name (filled with `tables` at parse): the text bound
    /// of a table Value without scanning the table per widget per pass.
    std::map<std::string, size_t> table_max_len;
    /// The package's own dualscreen/ folder, kept open so "file:" assets can be read from it.
    FileSys::VirtualDir asset_dir;
    u32 poll_hz{60};
    u32 canvas_w{0}; ///< 0 = use the aux panel size
    u32 canvas_h{0};
    u32 background{0xFF0E151E};
    std::vector<Page> pages;
    std::vector<EnforceRule> enforce;
    std::vector<SpyPoint> spies;
    std::vector<GuestPatch> patches;
    /// Enforce rules stay silent until this point reads a sane value, so nothing is called into
    /// a game that is still starting up.
    std::string enforce_gate;
    s64 enforce_gate_max{64};
    /// The point name InGameplayHonest() treats as "a real game session is active", the one
    /// signal every RE search tool gates its baseline on. Historically this was a hardcoded
    /// literal ("energy", Dread's HP stat); it is now a manifest field so any package can name
    /// its own always-positive-during-play point. Default kept as "energy" so every package that
    /// predates this field keeps behaving exactly as before with no JSON change.
    std::string gameplay_point{"energy"};
    /// name -> asset reference, for src_bind
    std::unordered_map<std::string, std::string> sprite_map;
    std::unordered_map<std::string, MapRoom> map_rooms;
    std::unordered_map<s64, std::string> zone_area; ///< map-zone value -> area name
    std::unordered_map<std::string, MapArea> map_areas;
    /// map.areas_src (runtime 12): a "module:" source whose JSON object replaces map_areas once the
    /// module has generated it; empty for packages that carry their areas inline.
    std::string map_areas_src;
    std::string icon_atlas; ///< asset reference for the icon sheet
    s32 icon_cell{0};       ///< cell size in the sheet
    s32 icon_cols{0};
    MapStyle map_style; ///< map renderer palette / sizes (map.style)
    std::unordered_map<std::string, std::pair<s32, s32>> icon_cells; ///< icon id -> row, col
    /// icon id -> tight ink bbox [x0,y0,x1,y1] within its cell, px. Baked from the atlas so a
    /// stretched door/blockage glyph fills its world box with INK instead of transparent margin.
    std::unordered_map<std::string, std::array<s32, 4>> icon_ink;
    Il2CppLayout il2cpp;
    /// A class slot we already know, used to locate IL2CPP's metadata: the class' name pointer
    /// lands inside the metadata string region, which is where every other class name lives too.
    s64 metadata_anchor{};
    std::unordered_map<std::string, s64> flag_defaults;
    /// "persist_flags" (runtime 15): runtime flags saved on change and restored at load
    /// (mod_persist.h).
    std::vector<std::string> persist_flags;
    std::unordered_map<std::string, Action> actions;
    std::unordered_map<std::string, CallSequence> sequences;
    std::unordered_map<std::string, DataPoint> points; ///< merged from the build-id data file
    /// Derived values, evaluated every tick after sampling. Manifest and data file both declare
    /// them; a data-file entry replaces a manifest entry of the same name.
    std::vector<DerivedPoint> derived;
    /// Manifest top level "page_binds": edge-triggered automatic page switches driven by live
    /// state (DrivePageBinds, mod_pages.cpp).
    std::vector<PageBind> page_binds;
    s64 frame_hook{}; ///< guest address of a per-frame function used to borrow the game thread
    std::string frame_hook_symbol;
    /// A callable target. Either a fixed address, or a class + method name resolved at runtime
    /// out of IL2CPP metadata -- the latter survives game updates, since only the class address
    /// stays build-specific.
    struct Symbol {
        std::string class_name; ///< look the class up by name at runtime
        PatternFind find;       ///< found by scanning, when there is nothing to look up by name
        s64 address{};          ///< main-relative address, when known outright
        s64 class_slot{};       ///< main-relative address of the Il2CppClass* slot (TypeInfo)
        std::string method;     ///< method name to look up in that class
    };
    std::unordered_map<std::string, Symbol> symbols;
    std::string build_id_file; ///< which data file was used (for logging)
    bool debug_page{false};    ///< append a generated page listing point addresses
    /// The game's own font, read from its romfs: an MFNT metrics file and the atlas it names.
    /// Set both and every label on the page is drawn in the game's lettering.
    std::string font_metrics_src;
    std::string font_atlas_src;
    /// Runtime 17 "font_page_h": with "{p}" in font_atlas, the atlas is paged -- glyph y runs
    /// through pages of this many rows stacked top to bottom, page p loaded from font_atlas with
    /// "{p}" replaced by p, on demand (FontPages). 0 = one atlas image.
    u32 font_page_h{0};
    /// name -> composite image definition (shared with the asset worker as an immutable copy).
    std::map<std::string, std::shared_ptr<const CompositeDef>> composites;
    /// Message files of the running game ("msbt", "msbt_lang", "msbt_lang_fallback", "msbt_icons").
    MsbtConfig msbt;
    /// Haptic feedback on the second screen ("haptics"; disabled when absent).
    HapticsConfig haptics;
    /// Controller navigation of the second screen ("nav"; runtime 17, mod_types_nav.h).
    NavConfig nav;
    /// Republish rate while an animation runs ("anim_hz": 60 = every tick, else the idle 30 Hz).
    u32 anim_hz{60};
    /// Value search, for a device where no environment variable can reach the runtime.
    std::string find_spec;
    /// A published address to derive a shippable pointer chain for.
    std::string trace_target;
    bool dump_registry{false};
    /// Runtime 16/17: the manifest or its data file names a "@clock." / "@game." key (or has a
    /// derived "countdown"): only then are those keys published each tick (ClockPublisher).
    bool uses_clock_keys{false};
    /// "module_tick_hidden": tick the native module while the second screen is hidden (true) or
    /// not (false). Unset: the module's own flag decides, else whether it has actions.
    std::optional<bool> module_tick_hidden;
    std::string describe_string; ///< report where the game uses this string (DescribeStringUses)
    bool valid{false};
};

/// Values sampled on one tick; published for the UI thread. Plain data, no locks needed.
/// The per-tick value maps. Open addressing over one contiguous array (ankerl::unordered_dense,
/// wyhash): the snapshot is rebuilt and read by name thousands of times per tick, and a node-based
/// std::unordered_map spent that time in node allocation and pointer chasing.
template <typename T>
using SnapshotMap = ankerl::unordered_dense::map<std::string, T>;

struct StateSnapshot {
    SnapshotMap<s64> ints;
    SnapshotMap<std::string> texts;
    SnapshotMap<f64> floats;
    /// Player-placed map markers of the drawn area (the game's custom markers): world position
    /// and colour slot (0..4 = the marker dialog's red/green/yellow/magenta/cyan).
    struct CustomMarker {
        float x{};
        float y{};
        s32 color{0};
    };
    std::vector<CustomMarker> custom_markers;
    /// A drag in flight on the current page, for the renderer: the dragged widget (expanded copy)
    /// is drawn last, following the finger, and the drop target under it is highlighted.
    struct DragOverlay {
        bool active{false};
        Widget widget;
        s32 x{}; ///< finger, canvas px
        s32 y{};
        s32 grab_dx{}; ///< finger offset inside the widget when it was picked up
        s32 grab_dy{};
        s64 source{-1}; ///< expanded index of the dragged widget
        s64 hover{-1};  ///< expanded index of the drop target under the finger, -1 none
    };
    DragOverlay drag;
    /// Resolved guest address per point (0 when the pointer chain broke). Drives the debug page.
    SnapshotMap<u64> addresses;
    u64 tick{0};
    /// Runtime 17: every Chart widget's samples (ChartSampler, mod_chart.h); null = none yet.
    std::shared_ptr<const ChartSeriesMap> charts;

    [[nodiscard]] s64 GetInt(const std::string& key, s64 fallback = 0) const {
        const auto it = ints.find(key);
        return it == ints.end() ? fallback : it->second;
    }
};

} // namespace Core::Mods
