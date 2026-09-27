// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone, read-only Mario Kart 8 Deluxe companion reader. The pointer chains and field
// validation are ported from Eden-DS mk8d_companion.cpp by Joe Correll; this translation unit
// replaces its Core/JNI integration with the generic Eden DSMod ABI.

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "core/mods/modules/mk8d_assets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace dsmod_sdk::int_types;
using VAddr = u64;

constexpr u64 TitleId = UINT64_C(0x0100152000022000);
constexpr std::string_view BuildId =
    "FE941ED5BA14BE5D505698DA1BBF4FE700000000000000000000000000000000";
constexpr VAddr FrameworkRootOffset = 0x012F6388;
constexpr VAddr SceneObjectEngineOffset = 0x1B0;
constexpr VAddr SceneActiveFlagsOffset = 0x180;
constexpr u32 SceneActiveMask = 0x04;
constexpr VAddr ObjectRaceDirectorOffset = 0x218;
constexpr VAddr ObjectKartDirectorOffset = 0x238;
constexpr VAddr ObjectItemDirectorOffset = 0x240;
constexpr VAddr KartDirectorHoldersOffset = 0xC8;
constexpr VAddr KartHolderVehicleOffset = 0x08;
constexpr VAddr KartVehicleMoveOffset = 0x28;
constexpr VAddr KartVehiclePlayerIdOffset = 0xA8;
constexpr VAddr KartVehicleMasterOffset = 0xD0;
constexpr VAddr KartMovePositionPointerOffset = 0x50;
constexpr VAddr RaceDirectorLapRankCheckerOffset = 0x58;
constexpr VAddr RaceDirectorPlayerCountOffset = 0x60;
constexpr VAddr RaceDirectorCheckersOffset = 0x68;
constexpr VAddr RaceKartCheckerRankOffset = 0x40;
constexpr VAddr RaceKartCheckerCoinOffset = 0x50;
constexpr VAddr LapRankCheckerLapCountOffset = 0x64;
constexpr VAddr RaceInfoRuleOffset = 0x08;
constexpr VAddr RaceInfoDriverArrayOffset = 0x30;
constexpr VAddr RaceInfoDriverStride = 0x1C;
constexpr VAddr RaceInfoDriverIdOffset = 0x0C;
constexpr VAddr RaceInfoDriverVariantOffset = 0x10;
constexpr VAddr RaceInfoPlayerTypeOffset = 0x14;
constexpr VAddr RaceInfoPlayerCountOffset = 0x180;
constexpr VAddr RaceInfoCourseIdOffset = 0x1A0;
constexpr VAddr ItemOwnerPlayerIdOffset = 0x40;
constexpr VAddr ItemOwnerSlot0Offset = 0x60;
constexpr VAddr ItemSlotStateOffset = 0x41;
constexpr VAddr ItemSlotIndexOffset = 0x68;
constexpr VAddr ItemSlotOwnerOffset = 0x70;
constexpr VAddr ItemSlotUseCountOffset = 0x98;
constexpr VAddr ItemSlotStockItemOffset = 0x9C;
constexpr VAddr ItemSlotStockCountOffset = 0xA0;
constexpr VAddr ItemSlotCurrentItemOffset = 0xC8;
constexpr int MaxRacers = 12;
constexpr u32 MaxDriverId = 52;
constexpr u32 MaxRawCourseId = 0x7A;
constexpr int NoItem = -1;
constexpr int MaxItem = 22;
constexpr u64 SamplePeriod = 4;
constexpr u32 MailboxIdle = 0, MailboxQueued = 1, MailboxRunning = 2;
constexpr u32 MailboxSucceeded = 3, MailboxRejected = 4;
constexpr u32 MailboxRequestOffset = 0, MailboxCompletedOffset = 8, MailboxStateOffset = 16;
constexpr u64 MailboxSize = 0x20;
constexpr u64 ActionTimeoutTicks = 600;

struct Racer {
    bool active{};
    bool local{};
    u8 rank{};
    u8 lap{};
    s16 driver_id{-1};
    s8 item0{NoItem};
    s8 item1{NoItem};
    u8 item0_count{};
    u8 item1_count{};
    u8 driver_variant{};
    u8 coins{};
    bool item0_rolling{};
    bool item1_rolling{};
    float x{}, y{}, z{};
    bool map_valid{};
    float map_x{}, map_y{};
};

struct Snapshot {
    bool active{};
    u32 course_id{};
    u8 player_count{};
    s8 local_player_id{-1};
    u8 lap_total{};
    std::array<Racer, MaxRacers> racers{};
};

struct FrameworkPointers {
    VAddr scene{}, object_engine{}, race_info{};
};

class Reader {
public:
    explicit Reader(const EdenDsmodHostApi& api) : host{api} {
        for (int id = 0; id < MaxRacers; ++id) {
            player_names[static_cast<size_t>(id)] =
                MakeRacerNames(("player." + std::to_string(id)).c_str());
            display_names[static_cast<size_t>(id)] =
                MakeRacerNames(("display." + std::to_string(id)).c_str());
        }
        local_names = MakeRacerNames("local");
    }
    void UpdateHost(const EdenDsmodHostApi& api) {
        host = api;
    }
    void Sample();
    void PollAction();
    void Configure(const EdenDsmodHostExtensions* extensions);
    EdenDsmodBool OnAction(const char* action, std::int64_t argument);
    EdenDsmodBool LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                            EdenDsmodImageSink sink);

private:
    // A host without is_mapped cannot verify a pointer: the read fails (create() requires it).
    template <typename T>
    std::optional<T> Read(VAddr address) const {
        T value{};
        if (!ReadBlock(address, &value, sizeof(T)))
            return std::nullopt;
        return value;
    }
    bool ReadBlock(VAddr address, void* output, size_t size) const {
        return dsmod_sdk::ReadGuest<dsmod_sdk::MissingIsMapped::Reject>(host, address, output,
                                                                        size);
    }
    std::optional<FrameworkPointers> ResolveFramework() const;
    bool ValidateItemOwner(VAddr owner, int expected, bool require_expected) const;
    VAddr FindItemOwnerArray(VAddr director, int count);
    std::array<VAddr, MaxRacers> MapOwners(VAddr array, int count) const;
    int FindLocal(VAddr race_info, VAddr holders, int count) const;
    Snapshot ReadSnapshot();
    void UpdateProjection(Snapshot& snapshot);
    const char* CourseInternal(u32 course) const;
    VAddr FindOwnerByPlayer(VAddr array, int count, int player) const;
    void Publish() const;
    /// The output names of one racer block ("player.3.rank", ...), formatted once
    /// instead of snprintf'd for ~575 outputs every tick (~30 us of the ~80 us a sample costs).
    struct RacerNames {
        std::array<std::string, 19> numeric;
        std::string portrait;
        std::array<std::string, 2> item_image;
        std::string player_id; ///< "display.N.player_id" (display rows only)
    };
    static RacerNames MakeRacerNames(const char* prefix);
    void PublishRacer(const RacerNames& names, const Racer& racer) const;
    std::array<RacerNames, MaxRacers> player_names{};
    std::array<RacerNames, MaxRacers> display_names{};
    RacerNames local_names{};
    void I64(const char* name, std::int64_t value) const {
        if (host.publish_i64)
            host.publish_i64(host.userdata, name, value);
    }
    void F64(const char* name, double value) const {
        if (host.publish_f64)
            host.publish_f64(host.userdata, name, value);
    }
    EdenDsmodHostApi host{};
    EdenDsmodHostExtensions extensions{};
    Snapshot current{};
    bool sampled{};
    u64 last_sample_tick{};
    VAddr cached_item_director{};
    VAddr cached_owner_array{};
    u64 action_queued_tick{};
    bool action_tracked{};
    u32 action_state{};
    u64 action_result{};
    bool action_timed_out{};
    u32 camera_course{};
    bool camera_loaded{};
    struct Camera {
        float look[3]{}, right[3]{}, up[3]{}, width{}, height{};
    } camera{};
    Mk8dAssets::Decoder assets{};
};

std::optional<FrameworkPointers> Reader::ResolveFramework() const {
    if (!host.main_base || host.main_size <= FrameworkRootOffset)
        return std::nullopt;
    const auto root_cell = Read<u64>(host.main_base + FrameworkRootOffset);
    const auto root = root_cell && *root_cell ? Read<u64>(*root_cell) : std::nullopt;
    if (!root || !*root)
        return std::nullopt;
    FrameworkPointers out{};
    const auto scene_holder = Read<u64>(*root + 0x20);
    const auto scene =
        scene_holder && *scene_holder ? Read<u64>(*scene_holder + 0x08) : std::nullopt;
    if (scene && *scene) {
        const auto flags = Read<u32>(*scene + SceneActiveFlagsOffset);
        if (flags && (*flags & SceneActiveMask)) {
            out.scene = *scene;
            out.object_engine = Read<u64>(*scene + SceneObjectEngineOffset).value_or(0);
        }
    }
    const auto race_holder = Read<u64>(*root + 0x28);
    const auto race_engine =
        race_holder && *race_holder ? Read<u64>(*race_holder + 0x240) : std::nullopt;
    out.race_info = race_engine && *race_engine ? Read<u64>(*race_engine + 0x10).value_or(0) : 0;
    return out;
}

bool Reader::ValidateItemOwner(VAddr owner, int expected, bool require_expected) const {
    const auto player = Read<u32>(owner + ItemOwnerPlayerIdOffset);
    const auto slot0 = Read<u64>(owner + ItemOwnerSlot0Offset);
    const auto slot1 = Read<u64>(owner + ItemOwnerSlot0Offset + 8);
    if (!player || *player >= MaxRacers || !slot0 || !*slot0 || !slot1 || !*slot1 ||
        (require_expected && static_cast<int>(*player) != expected))
        return false;
    const auto index0 = Read<u32>(*slot0 + ItemSlotIndexOffset);
    const auto index1 = Read<u32>(*slot1 + ItemSlotIndexOffset);
    const auto owner0 = Read<u64>(*slot0 + ItemSlotOwnerOffset);
    const auto owner1 = Read<u64>(*slot1 + ItemSlotOwnerOffset);
    return index0 && *index0 == 0 && index1 && *index1 == 1 && owner0 && *owner0 == owner &&
           owner1 && *owner1 == owner;
}

VAddr Reader::FindItemOwnerArray(VAddr director, int count) {
    if (director == cached_item_director && cached_owner_array) {
        const auto first = Read<u64>(cached_owner_array);
        if (first && ValidateItemOwner(*first, 0, false))
            return cached_owner_array;
    }
    cached_item_director = director;
    cached_owner_array = 0;
    for (VAddr offset = 0x38; offset <= 0x160; offset += 8) {
        const auto candidate = Read<u64>(director + offset);
        if (!candidate || !*candidate)
            continue;
        std::array<bool, MaxRacers> seen{};
        bool valid = true;
        for (int i = 0; i < count; ++i) {
            const auto owner = Read<u64>(*candidate + static_cast<VAddr>(i) * 8);
            const auto player =
                owner && *owner ? Read<u32>(*owner + ItemOwnerPlayerIdOffset) : std::nullopt;
            if (!owner || !*owner || !player || *player >= MaxRacers || seen[*player] ||
                !ValidateItemOwner(*owner, i, false)) {
                valid = false;
                break;
            }
            seen[*player] = true;
        }
        if (valid)
            return cached_owner_array = *candidate;
    }
    return 0;
}

std::array<VAddr, MaxRacers> Reader::MapOwners(VAddr array, int count) const {
    std::array<VAddr, MaxRacers> result{}, owners{};
    if (!array || !ReadBlock(array, owners.data(), static_cast<size_t>(count) * 8))
        return result;
    for (int i = 0; i < count; ++i) {
        const auto player =
            owners[i] ? Read<u32>(owners[i] + ItemOwnerPlayerIdOffset) : std::nullopt;
        if (player && *player < MaxRacers && !result[*player] &&
            ValidateItemOwner(owners[i], static_cast<int>(*player), true))
            result[*player] = owners[i];
    }
    return result;
}

int Reader::FindLocal(VAddr race_info, VAddr holders, int count) const {
    if (holders)
        for (int i = 0; i < count; ++i) {
            const auto holder = Read<u64>(holders + static_cast<VAddr>(i) * 8);
            const auto vehicle =
                holder && *holder ? Read<u64>(*holder + KartHolderVehicleOffset) : std::nullopt;
            const auto master =
                vehicle && *vehicle ? Read<u8>(*vehicle + KartVehicleMasterOffset) : std::nullopt;
            if (master && *master) {
                const auto id = Read<u32>(*vehicle + KartVehiclePlayerIdOffset);
                return id && *id < static_cast<u32>(count) ? static_cast<int>(*id) : i;
            }
        }
    for (int i = 0; i < count; ++i) {
        const auto type =
            Read<u32>(race_info + RaceInfoDriverArrayOffset +
                      static_cast<VAddr>(i) * RaceInfoDriverStride + RaceInfoPlayerTypeOffset);
        if (type && *type == 0)
            return i;
    }
    return -1;
}

VAddr Reader::FindOwnerByPlayer(VAddr array, int count, int player) const {
    if (!array || player < 0 || player >= count)
        return 0;
    for (int i = 0; i < count; ++i) {
        const auto owner = Read<u64>(array + static_cast<VAddr>(i) * 8);
        if (owner && *owner && ValidateItemOwner(*owner, player, true))
            return *owner;
    }
    return 0;
}

const char* Reader::CourseInternal(u32 id) const {
    // Public course identifiers and camera math follow Mk8dCompanionView.kt from the same
    // Eden-DS research tree. Camera bytes always come from the user's own RomFS.
    static constexpr const char* Base[] = {
        "Gu_MarioCircuit",     "Gu_DossunIseki",     "Gu_City",           "Gu_Cake",
        "Gu_HorrorHouse",      "Gu_Expert",          "Gu_Desert",         "Gu_Cloud",
        "Gu_SnowMountain",     "Gu_Techno",          "Gu_Airport",        "Gu_FirstCircuit",
        "Gu_WaterPark",        "Gu_Ocean",           "Gu_BowserCastle",   "Gu_RainbowRoad",
        "G3ds_DKJungle",       "Gwii_MooMooMeadows", "G64_PeachCircuit",  "G64_KinopioHighway",
        "Gds_PukupukuBeach",   "Ggc_SherbetLand",    "Gagb_MarioCircuit", "G3ds_MusicPark",
        "Gwii_GrumbleVolcano", "Gsfc_DonutsPlain3",  "Ggc_DryDryDesert",  "G3ds_PackunSlider",
        "Gds_TickTockClock",   "G64_YoshiValley",    "Gds_WarioStadium",  "G64_RainbowRoad"};
    static constexpr const char* Dlc[] = {
        "Du_Metro",           "Du_MuteCity",      "Du_DragonRoad",    "Du_Hyrule",
        "Du_Animal_Summer",   "Du_ExciteBike",    "Du_Woods",         "Du_IcePark",
        "Dgc_YoshiCircuit",   "Dwii_WariosMine",  "Dsfc_RainbowRoad", "Dagb_RibbonRoad",
        "D3ds_NeoBowserCity", "Dgc_BabyPark",     "Dagb_CheeseLand",  "Du_BigBlue",
        "Du_Animal_Spring",   "Du_Animal_Autumn", "Du_Animal_Winter", "B3ds_WuhuTown",
        "Bgc_LuigiMansion",   "Bsfc_Battle1",     "Bu_DekaLine",      "Bu_Moon",
        "Bu_BattleStadium",   "Bu_Dojo",          "Bu_Sweets"};
    static constexpr const char* Booster[] = {
        "Cnsw_11", "Cnsw_12", "Cnsw_13", "Cnsw_14", "Cnsw_15", "Cnsw_16", "Cnsw_17", "Cnsw_18",
        "Cnsw_21", "Cnsw_22", "Cnsw_23", "Cnsw_24", "Cnsw_25", "Cnsw_26", "Cnsw_27", "Cnsw_28",
        "Cnsw_31", "Cnsw_33", "Cnsw_34", "Cnsw_62", "Cnsw_35", "Cnsw_32", "Cnsw_37", "Cnsw_38",
        "Cnsw_41", "Cnsw_47", "Cnsw_42", "Cnsw_44", "Cnsw_55", "Cnsw_43", "Cnsw_36", "Cnsw_45",
        "Cnsw_65", "Cnsw_46", "Cnsw_63", "Cnsw_58", "Cnsw_48", "Cnsw_53", "Cnsw_52", "Cnsw_61",
        "Cnsw_54", "Cnsw_56", "Cnsw_66", "Cnsw_64", "Cnsw_51", "Cnsw_67", "Cnsw_57", "Cnsw_68"};
    if (id >= 0x11 && id < 0x11 + std::size(Base))
        return Base[id - 0x11];
    if (id >= 0x31 && id < 0x31 + std::size(Dlc))
        return Dlc[id - 0x31];
    if (id >= 0x4c && id < 0x4c + std::size(Booster))
        return Booster[id - 0x4c];
    return nullptr;
}

void Reader::UpdateProjection(Snapshot& snapshot) {
    if (!snapshot.active)
        return;
    if (camera_course != snapshot.course_id) {
        camera_course = snapshot.course_id;
        camera_loaded = false;
        camera = {};
        const char* course = CourseInternal(snapshot.course_id);
        if (course && host.read_romfs) {
            const std::string path = std::string{"/Course/"} + course + "/course_mapcamera.bin";
            std::array<u8, 44> bytes{};
            if (host.read_romfs(host.userdata, path.c_str(), 0, bytes.data(), bytes.size()) ==
                bytes.size()) {
                std::array<float, 11> f{};
                std::memcpy(f.data(), bytes.data(), bytes.size());
                const float fx = f[3] - f[0], fy = f[4] - f[1], fz = f[5] - f[2];
                const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
                if (std::isfinite(fl) && fl > .0001f && std::isfinite(f[9]) &&
                    std::isfinite(f[10]) && f[9] > 0 && f[10] > 0) {
                    const float nx = fx / fl, ny = fy / fl, nz = fz / fl;
                    float rx = ny * f[8] - nz * f[7], ry = nz * f[6] - nx * f[8],
                          rz = nx * f[7] - ny * f[6];
                    const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
                    if (std::isfinite(rl) && rl > .0001f) {
                        rx /= rl;
                        ry /= rl;
                        rz /= rl;
                        camera.look[0] = f[3];
                        camera.look[1] = f[4];
                        camera.look[2] = f[5];
                        camera.right[0] = rx;
                        camera.right[1] = ry;
                        camera.right[2] = rz;
                        camera.up[0] = ry * nz - rz * ny;
                        camera.up[1] = rz * nx - rx * nz;
                        camera.up[2] = rx * ny - ry * nx;
                        camera.width = f[9];
                        camera.height = f[10];
                        camera_loaded = true;
                    }
                }
            }
        }
    }
    if (!camera_loaded)
        return;
    for (auto& racer : snapshot.racers)
        if (racer.active) {
            const float dx = racer.x - camera.look[0], dy = racer.y - camera.look[1],
                        dz = racer.z - camera.look[2];
            const float vx = dx * camera.right[0] + dy * camera.right[1] + dz * camera.right[2];
            const float vy = dx * camera.up[0] + dy * camera.up[1] + dz * camera.up[2];
            racer.map_x = .5f + vx / camera.width;
            racer.map_y = .5f - vy / camera.height;
            racer.map_valid = std::isfinite(racer.map_x) && std::isfinite(racer.map_y);
        }
}

struct Slot {
    s8 item{NoItem};
    u8 count{};
    bool rolling{};
};

Snapshot Reader::ReadSnapshot() {
    Snapshot out{};
    const auto fw = ResolveFramework();
    if (!fw || !fw->scene || !fw->object_engine || !fw->race_info)
        return out;
    const auto info_count = Read<u32>(fw->race_info + RaceInfoPlayerCountOffset);
    const auto rule = Read<u32>(fw->race_info + RaceInfoRuleOffset);
    const auto race_director = Read<u64>(fw->object_engine + ObjectRaceDirectorOffset);
    if (!info_count || *info_count < 1 || *info_count > MaxRacers || !rule || *rule > 3 ||
        !race_director || !*race_director)
        return out;
    const auto director_count = Read<u32>(*race_director + RaceDirectorPlayerCountOffset);
    const auto checkers_ptr = Read<u64>(*race_director + RaceDirectorCheckersOffset);
    if (!director_count || *director_count < 1 || *director_count > MaxRacers || !checkers_ptr ||
        !*checkers_ptr)
        return out;
    const int count = static_cast<int>(std::min(*info_count, *director_count));
    std::array<VAddr, MaxRacers> checkers{};
    if (!ReadBlock(*checkers_ptr, checkers.data(), static_cast<size_t>(count) * 8) ||
        std::none_of(checkers.begin(), checkers.begin() + count, [](VAddr p) { return p != 0; }))
        return out;

    const auto kart_director = Read<u64>(fw->object_engine + ObjectKartDirectorOffset);
    const VAddr holders = kart_director && *kart_director
                              ? Read<u64>(*kart_director + KartDirectorHoldersOffset).value_or(0)
                              : 0;
    std::array<VAddr, MaxRacers> raw_holders{}, vehicles{};
    std::array<u32, MaxRacers> live_drivers{};
    live_drivers.fill(std::numeric_limits<u32>::max());
    if (holders)
        ReadBlock(holders, raw_holders.data(), static_cast<size_t>(count) * 8);
    for (int h = 0; h < count; ++h) {
        const auto vehicle =
            raw_holders[h] ? Read<u64>(raw_holders[h] + KartHolderVehicleOffset) : std::nullopt;
        if (!vehicle || !*vehicle)
            continue;
        const auto identity = Read<std::array<u32, 4>>(*vehicle + KartVehiclePlayerIdOffset);
        const int id = identity && (*identity)[0] < static_cast<u32>(count)
                           ? static_cast<int>((*identity)[0])
                           : h;
        if (!vehicles[id]) {
            vehicles[id] = *vehicle;
            if (identity)
                live_drivers[id] = (*identity)[3];
        }
    }
    const int local = FindLocal(fw->race_info, holders, count);
    const auto item_director = Read<u64>(fw->object_engine + ObjectItemDirectorOffset);
    const VAddr owner_array =
        item_director && *item_director ? FindItemOwnerArray(*item_director, count) : 0;
    const auto owners = MapOwners(owner_array, count);
    const auto lap_checker = Read<u64>(*race_director + RaceDirectorLapRankCheckerOffset);
    const u8 lap_total = lap_checker && *lap_checker
                             ? Read<u8>(*lap_checker + LapRankCheckerLapCountOffset).value_or(0)
                             : 0;
    out.active = true;
    out.player_count = static_cast<u8>(count);
    out.local_player_id = local >= 0 && local < count ? static_cast<s8>(local) : -1;
    out.lap_total = lap_total >= 1 && lap_total <= 9 ? lap_total : 0;
    const u32 course =
        Read<u32>(fw->race_info + RaceInfoCourseIdOffset).value_or(std::numeric_limits<u32>::max());
    out.course_id = course <= MaxRawCourseId ? course + 1 : 0;

    std::array<u8, RaceInfoDriverStride * MaxRacers> driver_info{};
    const bool have_info = ReadBlock(fw->race_info + RaceInfoDriverArrayOffset, driver_info.data(),
                                     static_cast<size_t>(count) * RaceInfoDriverStride);
    for (int i = 0; i < count; ++i) {
        auto& racer = out.racers[i];
        racer.active = true;
        racer.local = i == local;
        u32 driver = std::numeric_limits<u32>::max();
        if (have_info) {
            const size_t base = static_cast<size_t>(i) * RaceInfoDriverStride;
            std::memcpy(&driver, driver_info.data() + base + RaceInfoDriverIdOffset, 4);
            racer.driver_variant = driver_info[base + RaceInfoDriverVariantOffset];
        }
        if (driver > MaxDriverId && live_drivers[i] <= MaxDriverId)
            driver = live_drivers[i];
        racer.driver_id = driver <= MaxDriverId ? static_cast<s16>(driver) : -1;
        const auto move =
            vehicles[i] ? Read<u64>(vehicles[i] + KartVehicleMoveOffset) : std::nullopt;
        const auto pos =
            move && *move ? Read<u64>(*move + KartMovePositionPointerOffset) : std::nullopt;
        std::array<float, 3> xyz{};
        if (pos && *pos && ReadBlock(*pos, xyz.data(), sizeof(xyz)) &&
            std::all_of(xyz.begin(), xyz.end(), [](float v) { return std::isfinite(v); })) {
            racer.x = xyz[0];
            racer.y = xyz[1];
            racer.z = xyz[2];
        }
        const auto rank_lap =
            checkers[i] ? Read<std::array<u32, 2>>(checkers[i] + RaceKartCheckerRankOffset)
                        : std::nullopt;
        const u32 rank = rank_lap ? (*rank_lap)[0] : std::numeric_limits<u32>::max();
        racer.rank =
            rank < static_cast<u32>(count) ? static_cast<u8>(rank + 1) : static_cast<u8>(i + 1);
        const u32 completed = rank_lap ? (*rank_lap)[1] : std::numeric_limits<u32>::max();
        if (out.lap_total && completed <= out.lap_total)
            racer.lap = static_cast<u8>(std::min<u32>(completed + 1, out.lap_total));
        racer.coins =
            checkers[i] ? Read<u8>(checkers[i] + RaceKartCheckerCoinOffset).value_or(0) : 0;
        if (owners[i]) {
            std::array<VAddr, 2> slots{};
            if (ReadBlock(owners[i] + ItemOwnerSlot0Offset, slots.data(), sizeof(slots))) {
                const auto read_slot = [&](VAddr slot) {
                    Slot value{};
                    constexpr VAddr Start = 0x40;
                    std::array<u8, ItemSlotCurrentItemOffset - Start + sizeof(s32)> state{};
                    if (!slot || !ReadBlock(slot + Start, state.data(), state.size()))
                        return value;
                    const u8 status = state[ItemSlotStateOffset - Start];
                    s32 stock{}, rolling{};
                    u32 used{}, stocked{};
                    std::memcpy(&used, state.data() + ItemSlotUseCountOffset - Start, 4);
                    std::memcpy(&stock, state.data() + ItemSlotStockItemOffset - Start, 4);
                    std::memcpy(&stocked, state.data() + ItemSlotStockCountOffset - Start, 4);
                    std::memcpy(&rolling, state.data() + ItemSlotCurrentItemOffset - Start, 4);
                    if (status == 1 || status == 2) {
                        value.item =
                            rolling >= 0 && rolling <= MaxItem ? static_cast<s8>(rolling) : NoItem;
                        value.rolling = true;
                    } else if (status == 3 && stock >= 0 && stock <= MaxItem) {
                        value.item = static_cast<s8>(stock);
                        value.count = static_cast<u8>(std::min<u32>(used ? used : stocked, 99));
                    }
                    return value;
                };
                const Slot a = read_slot(slots[0]), b = read_slot(slots[1]);
                racer.item0 = a.item;
                racer.item0_count = a.count;
                racer.item0_rolling = a.rolling;
                racer.item1 = b.item;
                racer.item1_count = b.count;
                racer.item1_rolling = b.rolling;
            }
        }
    }
    UpdateProjection(out);
    return out;
}

Reader::RacerNames Reader::MakeRacerNames(const char* prefix) {
    static constexpr std::array<const char*, 19> Suffixes{"active",
                                                          "local",
                                                          "driver_id",
                                                          "driver_variant",
                                                          "item.0.id",
                                                          "item.0.count",
                                                          "item.0.roulette",
                                                          "item.1.id",
                                                          "item.1.count",
                                                          "item.1.roulette",
                                                          "rank",
                                                          "lap",
                                                          "coins",
                                                          "map_valid",
                                                          "x",
                                                          "y",
                                                          "z",
                                                          "map_x",
                                                          "map_y"};
    RacerNames names;
    for (size_t i = 0; i < Suffixes.size(); ++i) {
        names.numeric[i] = std::string{prefix} + "." + Suffixes[i];
    }
    names.portrait = std::string{prefix} + ".portrait";
    for (int slot = 0; slot < 2; ++slot) {
        names.item_image[static_cast<size_t>(slot)] =
            std::string{prefix} + ".item." + std::to_string(slot) + ".image";
    }
    names.player_id = std::string{prefix} + ".player_id";
    return names;
}

void Reader::PublishRacer(const RacerNames& names, const Racer& r) const {
    // Same outputs, same order as before; only the name formatting moved out of the tick.
    size_t k = 0;
    const auto i = [&](std::int64_t value) { I64(names.numeric[k++].c_str(), value); };
    const auto f = [&](double value) { F64(names.numeric[k++].c_str(), value); };
    i(r.active);
    i(r.local);
    i(r.driver_id);
    i(r.driver_variant);
    i(r.item0);
    i(r.item0_count);
    i(r.item0_rolling);
    i(r.item1);
    i(r.item1_count);
    i(r.item1_rolling);
    i(r.rank);
    i(r.lap);
    i(r.coins);
    i(r.map_valid);
    f(r.x);
    f(r.y);
    f(r.z);
    f(r.map_x);
    f(r.map_y);
    if (host.publish_text) {
        std::string value;
        if (r.active && r.driver_id >= 0)
            value = "module:character/" + std::to_string(r.driver_id) + "/" +
                    std::to_string(r.driver_variant);
        host.publish_text(host.userdata, names.portrait.c_str(), value.c_str());
        for (int slot = 0; slot < 2; ++slot) {
            const int item = slot ? r.item1 : r.item0;
            value.clear();
            if (r.active && item >= 0)
                value = "module:item/" + std::to_string(item);
            host.publish_text(host.userdata, names.item_image[static_cast<size_t>(slot)].c_str(),
                              value.c_str());
        }
    }
}

void Reader::Publish() const {
    if (host.begin_output)
        host.begin_output(host.userdata);
    if (host.publish_text)
        host.publish_text(host.userdata, "status", current.active ? "active" : "waiting_for_race");
    I64("race.active", current.active);
    I64("race.player_count", current.player_count);
    I64("race.local_player_id", current.local_player_id);
    I64("race.course_id", current.course_id);
    I64("race.lap_total", current.lap_total);
    if (host.publish_text) {
        std::string image;
        if (const char* course = CourseInternal(current.course_id); current.active && course)
            image = std::string{"module:map/"} + course;
        host.publish_text(host.userdata, "race.map_image", image.c_str());
    }
    I64("action.available", extensions.version == EDEN_DSMOD_EXT_VERSION && current.active &&
                                current.local_player_id >= 0);
    I64("action.busy", action_state == MailboxQueued || action_state == MailboxRunning);
    I64("action.state", action_state);
    I64("action.result", static_cast<std::int64_t>(action_result));
    I64("action.timed_out", action_timed_out);
    std::array<int, MaxRacers> order{};
    for (int id = 0; id < MaxRacers; ++id)
        order[id] = id;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& x = current.racers[a];
        const auto& y = current.racers[b];
        if (x.active != y.active)
            return x.active > y.active;
        if (x.rank != y.rank)
            return x.rank < y.rank;
        return a < b;
    });
    for (int id = 0; id < MaxRacers; ++id) {
        PublishRacer(player_names[static_cast<size_t>(id)], current.racers[id]);
    }
    const Racer empty{};
    const Racer& local = current.local_player_id >= 0 && current.local_player_id < MaxRacers
                             ? current.racers[static_cast<size_t>(current.local_player_id)]
                             : empty;
    PublishRacer(local_names, local);
    for (int row = 0; row < MaxRacers; ++row) {
        const RacerNames& names = display_names[static_cast<size_t>(row)];
        I64(names.player_id.c_str(), current.racers[order[row]].active ? order[row] : -1);
        PublishRacer(names, current.racers[order[row]]);
    }
    if (host.end_output)
        host.end_output(host.userdata);
}

void Reader::Sample() {
    const u64 tick = host.get_tick ? host.get_tick(host.userdata) : last_sample_tick + SamplePeriod;
    PollAction();
    if (!sampled || tick < last_sample_tick || tick - last_sample_tick >= SamplePeriod) {
        current = ReadSnapshot();
        last_sample_tick = tick;
        sampled = true;
        if (!current.active) {
            cached_item_director = 0;
            cached_owner_array = 0;
        }
    }
    Publish();
}

void Reader::PollAction() {
    const u64 tick = host.get_tick ? host.get_tick(host.userdata) : last_sample_tick;
    if (extensions.version == EDEN_DSMOD_EXT_VERSION && extensions.load_u32 &&
        extensions.load_u64) {
        u32 state{};
        u64 result{};
        if (extensions.load_u32(extensions.userdata, MailboxStateOffset, &state)) {
            action_state = state;
            if (extensions.load_u64(extensions.userdata, MailboxCompletedOffset, &result))
                action_result = result;
            if (action_state == MailboxSucceeded || action_state == MailboxRejected ||
                action_state == MailboxIdle) {
                action_timed_out = false;
                action_tracked = false;
            } else if ((action_state == MailboxQueued || action_state == MailboxRunning) &&
                       action_tracked && tick >= action_queued_tick &&
                       tick - action_queued_tick > ActionTimeoutTicks)
                action_timed_out = true;
        }
    }
}

void Reader::Configure(const EdenDsmodHostExtensions* ext) {
    extensions = {};
    if (ext && ext->version == EDEN_DSMOD_EXT_VERSION && ext->abi_hash == EDEN_DSMOD_EXT_HASH &&
        ext->struct_size >= sizeof(*ext) && ext->mailbox_address &&
        ext->mailbox_size >= MailboxSize && (ext->mailbox_address & 7) == 0 && ext->load_u32 &&
        ext->load_u64 && ext->store_u32 && ext->store_u64)
        extensions = *ext;
    assets.SetAstcDecoder({extensions.userdata, extensions.decode_astc});
}

EdenDsmodBool Reader::OnAction(const char* action, std::int64_t slot_index) {
    if (!action || std::string_view{action} != "activate_item" || slot_index < 0 ||
        slot_index > 1 || extensions.version != EDEN_DSMOD_EXT_VERSION)
        return EDEN_DSMOD_FALSE;
    const Snapshot live = ReadSnapshot();
    if (!live.active || live.local_player_id < 0 || !cached_owner_array)
        return EDEN_DSMOD_FALSE;
    const VAddr owner =
        FindOwnerByPlayer(cached_owner_array, live.player_count, live.local_player_id);
    const auto slot =
        owner ? Read<u64>(owner + ItemOwnerSlot0Offset + static_cast<VAddr>(slot_index) * 8)
              : std::nullopt;
    if (!slot || !*slot)
        return EDEN_DSMOD_FALSE;
    constexpr VAddr Start = 0x40;
    std::array<u8, ItemSlotCurrentItemOffset - Start + sizeof(s32)> bytes{};
    if (!ReadBlock(*slot + Start, bytes.data(), bytes.size()))
        return EDEN_DSMOD_FALSE;
    const u8 slot_state = bytes[ItemSlotStateOffset - Start];
    s32 stock{};
    std::memcpy(&stock, bytes.data() + ItemSlotStockItemOffset - Start, sizeof(stock));
    if (slot_state != 3 || stock < 0 || stock > MaxItem)
        return EDEN_DSMOD_FALSE;
    u32 state{};
    u64 pending{};
    if (!extensions.load_u32(extensions.userdata, MailboxStateOffset, &state) ||
        !extensions.load_u64(extensions.userdata, MailboxRequestOffset, &pending) ||
        state == MailboxQueued || state == MailboxRunning ||
        (state != MailboxIdle && state != MailboxSucceeded && state != MailboxRejected) ||
        pending != 0)
        return EDEN_DSMOD_FALSE;
    const u64 payload = static_cast<u64>(slot_index) + 1;
    // The host callbacks provide the ordering contract: publish payload fields before the
    // release store of Queued, matching the original companion hook protocol.
    if (!extensions.store_u64(extensions.userdata, MailboxCompletedOffset, 0) ||
        !extensions.store_u64(extensions.userdata, MailboxRequestOffset, payload) ||
        !extensions.store_u32(extensions.userdata, MailboxStateOffset, MailboxQueued))
        return EDEN_DSMOD_FALSE;
    action_state = MailboxQueued;
    action_result = 0;
    action_timed_out = false;
    action_queued_tick = host.get_tick ? host.get_tick(host.userdata) : last_sample_tick;
    action_tracked = true;
    return EDEN_DSMOD_TRUE;
}

EdenDsmodBool Reader::LoadImage(const EdenDsmodHostApi* image_host, const char* key, void* receiver,
                                EdenDsmodImageSink sink) {
    if (!image_host || !key || !sink)
        return EDEN_DSMOD_FALSE;
    const std::string_view public_key{key};
    std::string logical;
    if (constexpr std::string_view prefix{"module:map/"}; public_key.starts_with(prefix)) {
        const auto course = public_key.substr(prefix.size());
        if (course.empty() || course.find('/') != std::string_view::npos)
            return EDEN_DSMOD_FALSE;
        logical = "mk8d/maps/" + std::string{course} + ".png";
    } else if (constexpr std::string_view prefix{"module:item/"}; public_key.starts_with(prefix)) {
        const auto number = public_key.substr(prefix.size());
        unsigned item{};
        if (number.empty() || number.size() > 2)
            return EDEN_DSMOD_FALSE;
        for (const char c : number) {
            if (c < '0' || c > '9')
                return EDEN_DSMOD_FALSE;
            item = item * 10 + static_cast<unsigned>(c - '0');
        }
        if (item > MaxItem)
            return EDEN_DSMOD_FALSE;
        logical = "mk8d/items/item_" + std::to_string(item) + ".png";
    } else if (constexpr std::string_view prefix{"module:character/"};
               public_key.starts_with(prefix)) {
        const auto spec = public_key.substr(prefix.size());
        const auto slash = spec.find('/');
        if (slash == std::string_view::npos)
            return EDEN_DSMOD_FALSE;
        const auto parse = [](std::string_view value, u32& out) {
            if (value.empty() || value.size() > 3)
                return false;
            out = 0;
            for (const char c : value) {
                if (c < '0' || c > '9')
                    return false;
                out = out * 10 + static_cast<u32>(c - '0');
            }
            return true;
        };
        u32 driver{}, variant{};
        if (!parse(spec.substr(0, slash), driver) || !parse(spec.substr(slash + 1), variant))
            return EDEN_DSMOD_FALSE;
        logical = Mk8dAssets::Decoder::DriverImageKey(driver, variant);
        if (logical.empty())
            return EDEN_DSMOD_FALSE;
    } else
        return EDEN_DSMOD_FALSE;
    const auto image = assets.LoadImage(*image_host, logical);
    if (!image || image->rgba.empty())
        return EDEN_DSMOD_FALSE;
    sink(receiver, image->width, image->height, image->rgba.data(), image->rgba.size());
    return EDEN_DSMOD_TRUE;
}

EdenDsmodBool SupportsBuild(const char* build_id) {
    return build_id && std::string_view{build_id} == BuildId ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}
void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != TitleId || !host->is_mapped ||
            !host->read_memory)
            return nullptr;
        return new Reader{*host};
    } catch (...) {
        return nullptr;
    }
}
void Destroy(void* p) {
    try {
        delete static_cast<Reader*>(p);
    } catch (...) {
    }
}
void SampleCallback(void* p, const EdenDsmodHostApi* host) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    try {
        if (p && host) {
            auto& r = *static_cast<Reader*>(p);
            r.UpdateHost(*host);
            r.Sample();
        }
    } catch (...) {
        if (!reported.test_and_set() && host && host->log)
            host->log(host->userdata, EDEN_DSMOD_LOG_ERROR, "MK8D DSMod sample callback failed");
    }
}
void TickCallback(void* p, const EdenDsmodHostApi* host) {
    try {
        if (p && host) {
            auto& reader = *static_cast<Reader*>(p);
            reader.UpdateHost(*host);
            reader.PollAction();
        }
    } catch (...) {
    }
}
void ConfigureCallback(void* p, const EdenDsmodHostExtensions* ext) {
    if (p)
        static_cast<Reader*>(p)->Configure(ext);
}
EdenDsmodBool ActionCallback(void* p, const char* action, std::int64_t argument) {
    try {
        if (p)
            return static_cast<Reader*>(p)->OnAction(action, argument);
        return EDEN_DSMOD_FALSE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}
EdenDsmodBool LoadImageCallback(void* p, const EdenDsmodHostApi* host, const char* key,
                                void* receiver, EdenDsmodImageSink sink) {
    try {
        if (p)
            return static_cast<Reader*>(p)->LoadImage(host, key, receiver, sink);
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
                                   EDEN_DSMOD_CAP_EXTENSIONS,
                                   &SupportsBuild,
                                   &Create,
                                   &Destroy,
                                   &SampleCallback,
                                   &TickCallback};
const EdenDsmodModuleExtensions ModuleExtensions{
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions),
    EDEN_DSMOD_EXT_HASH,    &ConfigureCallback,
    &ActionCallback,        &LoadImageCallback};
} // namespace

DSMOD_SDK_EXPORT_MODULE(ModuleApi)
DSMOD_SDK_EXPORT_EXTENSIONS(ModuleExtensions)
