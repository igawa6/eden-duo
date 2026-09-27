// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/mods/modules/mk8d_assets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

#include <bc_decoder.h>
#include "core/mods/dsmod_module_abi.h"
#include "core/mods/modules/dsmod_module_sdk.h"

namespace Mk8dAssets {
namespace {
using namespace dsmod_sdk::int_types;
constexpr std::size_t MaxSource = 64 * 1024 * 1024;
constexpr u32 MaxDimension = 2048;
constexpr u64 MaxPixels = 4 * 1024 * 1024;

// Bounded little-endian reads: 0 when the value does not fit inside the span.
u16 U16(std::span<const u8> s, std::size_t o) {
    return o + 2 <= s.size() ? dsmod_sdk::Le16(s.data() + o) : 0;
}
u32 U32(std::span<const u8> s, std::size_t o) {
    return o + 4 <= s.size() ? dsmod_sdk::Le32(s.data() + o) : 0;
}
u64 U64(std::span<const u8> s, std::size_t o) {
    return o + 8 <= s.size() ? dsmod_sdk::Le64(s.data() + o) : 0;
}
bool Magic(std::span<const u8> s, std::string_view m) {
    return s.size() >= m.size() && std::memcmp(s.data(), m.data(), m.size()) == 0;
}
bool Safe(std::string_view s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}
std::optional<std::vector<u8>> Read(const EdenDsmodHostApi& host, const std::string& path,
                                    std::size_t limit = MaxSource) {
    if (!host.read_romfs)
        return std::nullopt;
    const auto size = host.read_romfs(host.userdata, path.c_str(), 0, nullptr, 0);
    if (!size || size > limit)
        return std::nullopt;
    std::vector<u8> out(size);
    return host.read_romfs(host.userdata, path.c_str(), 0, out.data(), size) == size
               ? std::optional{std::move(out)}
               : std::nullopt;
}
std::optional<std::vector<u8>> Yaz0(std::span<const u8> in) {
    if (!Magic(in, "Yaz0"))
        return in.size() <= MaxSource ? std::optional{std::vector(in.begin(), in.end())}
                                      : std::nullopt;
    if (in.size() < 16)
        return std::nullopt;
    const u32 size = dsmod_sdk::Be32(in.data() + 4);
    if (!size || size > MaxSource)
        return std::nullopt;
    std::vector<u8> out(size);
    std::size_t si = 16, di = 0;
    u8 code = 0, mask = 0;
    while (di < out.size()) {
        if (!mask) {
            if (si >= in.size())
                return std::nullopt;
            code = in[si++];
            mask = 0x80;
        }
        if (code & mask) {
            if (si >= in.size())
                return std::nullopt;
            out[di++] = in[si++];
        } else {
            if (si + 1 >= in.size())
                return std::nullopt;
            const u8 a = in[si++], b = in[si++];
            std::size_t dist = ((a & 15) << 8) | b;
            if (dist + 1 > di)
                return std::nullopt;
            std::size_t len = a >> 4;
            if (!len) {
                if (si >= in.size())
                    return std::nullopt;
                len = in[si++] + 0x12;
            } else
                len += 2;
            std::size_t copy = di - dist - 1;
            while (len-- && di < out.size())
                out[di++] = out[copy++];
        }
        mask >>= 1;
    }
    return out;
}
u16 EU16(std::span<const u8> s, std::size_t o, bool be) {
    return be ? static_cast<u16>(s[o] << 8 | s[o + 1]) : U16(s, o);
}
u32 EU32(std::span<const u8> s, std::size_t o, bool be) {
    return be ? static_cast<u32>(s[o]) << 24 | static_cast<u32>(s[o + 1]) << 16 |
                    static_cast<u32>(s[o + 2]) << 8 | s[o + 3]
              : U32(s, o);
}
std::optional<std::vector<u8>> Sarc(std::span<const u8> s, std::string_view wanted) {
    if (s.size() < 0x20 || !Magic(s, "SARC"))
        return std::nullopt;
    const bool be = s[6] == 0xfe && s[7] == 0xff;
    const auto hs = EU16(s, 4, be);
    const auto data = EU32(s, 0xc, be);
    if (hs + 12 > s.size() || data > s.size() || !Magic(s.subspan(hs), "SFAT"))
        return std::nullopt;
    const auto count = EU16(s, hs + 6, be);
    const auto nodes = hs + 12;
    if (count > 8192 || nodes + count * 16 > s.size())
        return std::nullopt;
    const auto sfnt = nodes + count * 16;
    if (sfnt + 8 > s.size() || !Magic(s.subspan(sfnt), "SFNT"))
        return std::nullopt;
    const auto names = sfnt + EU16(s, sfnt + 4, be);
    for (u16 i = 0; i < count; ++i) {
        const auto n = nodes + i * 16;
        const auto attr = EU32(s, n + 4, be);
        const auto begin = EU32(s, n + 8, be), end = EU32(s, n + 12, be);
        if (!(attr & 0x01000000))
            continue;
        const auto no = names + (attr & 0xffffff) * 4;
        if (no >= s.size())
            continue;
        auto zero = std::find(s.begin() + no, s.end(), 0);
        if (zero == s.end())
            continue;
        std::string_view name(reinterpret_cast<const char*>(s.data() + no), zero - s.begin() - no);
        if (name == wanted && begin <= end && data + end <= s.size())
            return std::vector<u8>(s.begin() + data + begin, s.begin() + data + end);
    }
    return std::nullopt;
}
std::optional<std::vector<u8>> Nested(const EdenDsmodHostApi& host, const char* path,
                                      std::string_view member) {
    auto outer = Read(host, path);
    if (!outer)
        return std::nullopt;
    auto decoded = Yaz0(*outer);
    if (!decoded)
        return std::nullopt;
    auto nested = Sarc(*decoded, member);
    if (!nested)
        return std::nullopt;
    return Yaz0(*nested);
}
struct Format {
    u32 bw, bh, bpb, channels;
};
struct Texture {
    u32 w, h, format, size, arrays, layout, cmap;
    u16 tile;
    u64 image;
};
std::optional<Format> GetFormat(u32 f) {
    switch (f >> 8) {
    case 2:
        return Format{1, 1, 1, 1};
    case 9:
        return Format{1, 1, 2, 2};
    case 11:
    case 12:
        return Format{1, 1, 4, 4};
    case 26:
        return Format{4, 4, 8, 4};
    case 27:
    case 28:
        return Format{4, 4, 16, 4};
    case 29:
        return Format{4, 4, 8, 1};
    case 30:
        return Format{4, 4, 16, 2};
    case 32:
        return Format{4, 4, 16, 4};
    case 45:
        return Format{4, 4, 16, 4};
    case 46:
        return Format{5, 4, 16, 4};
    case 47:
        return Format{5, 5, 16, 4};
    case 48:
        return Format{6, 5, 16, 4};
    case 49:
        return Format{6, 6, 16, 4};
    case 50:
        return Format{8, 5, 16, 4};
    case 51:
        return Format{8, 6, 16, 4};
    case 52:
        return Format{8, 8, 16, 4};
    case 53:
        return Format{10, 5, 16, 4};
    case 54:
        return Format{10, 6, 16, 4};
    case 55:
        return Format{10, 8, 16, 4};
    case 56:
        return Format{10, 10, 16, 4};
    case 57:
        return Format{12, 10, 16, 4};
    case 58:
        return Format{12, 12, 16, 4};
    default:
        return std::nullopt;
    }
}
u32 Pdep(u32 value, u32 mask) {
    u32 result = 0;
    for (u32 bit = 1; mask; bit += bit) {
        if (value & bit)
            result |= mask & (~mask + 1);
        mask &= mask - 1;
    }
    return result;
}
std::optional<std::vector<u8>> Unswizzle2d(std::span<const u8> source, u32 bpp, u32 width,
                                           u32 height, u32 block_height) {
    if (!bpp || block_height > 5)
        return std::nullopt;
    const u64 row_bytes = static_cast<u64>(width) * bpp;
    const u64 aligned_width = (row_bytes + 63) & ~UINT64_C(63);
    const u64 aligned_height = (static_cast<u64>(height) + (UINT64_C(8) << block_height) - 1) &
                               ~((UINT64_C(8) << block_height) - 1);
    const u64 required = aligned_width * aligned_height;
    if (required > source.size() || row_bytes * height > MaxSource)
        return std::nullopt;
    std::vector<u8> output(static_cast<std::size_t>(row_bytes * height));
    const u32 stride = static_cast<u32>(aligned_width);
    const u32 gobs_x = stride >> 6;
    const u32 block_size = gobs_x << (9 + block_height);
    const u32 height_mask = (1u << block_height) - 1;
    for (u32 y = 0; y < height; ++y) {
        const u32 swizzled_y = Pdep(y, 0b011010000);
        const u32 block_y = y >> 3;
        const u32 offset_y =
            (block_y >> block_height) * block_size + ((block_y & height_mask) << 9);
        for (u32 x = 0; x < width; ++x) {
            const u32 byte_x = x * bpp;
            const u64 offset = static_cast<u64>(offset_y) +
                               (static_cast<u64>(byte_x >> 6) << (9 + block_height)) +
                               (Pdep(byte_x, 0b100101111) | swizzled_y);
            if (offset + bpp > source.size())
                return std::nullopt;
            std::memcpy(output.data() + static_cast<std::size_t>(y) * row_bytes + byte_x,
                        source.data() + offset, bpp);
        }
    }
    return output;
}
std::optional<Image> Decode(std::span<const u8> source, std::string_view wanted, bool first,
                            AstcDecoder astc) {
    const std::array<u8, 4> bm{'B', 'N', 'T', 'X'};
    auto it = std::search(source.begin(), source.end(), bm.begin(), bm.end());
    if (it == source.end())
        return std::nullopt;
    auto b = source.subspan(it - source.begin());
    if (b.size() < 0x30)
        return std::nullopt;
    const u32 declared = U32(b, 0x1c);
    if (declared >= 0x30 && declared <= b.size())
        b = b.first(declared);
    const u32 count = U32(b, 0x24);
    const u64 array = U64(b, 0x28);
    if (!count || count > 4096 || array + count * 8 > b.size())
        return std::nullopt;
    std::optional<Texture> chosen;
    bool matched = false;
    for (u32 i = 0; i < count; ++i) {
        const u64 o = U64(b, array + i * 8);
        if (o + 0xa0 > b.size() || !Magic(b.subspan(o), "BRTI"))
            continue;
        Texture t{U32(b, o + 0x24), U32(b, o + 0x28), U32(b, o + 0x1c),
                  U32(b, o + 0x50), U32(b, o + 0x30), U32(b, o + 0x34),
                  U32(b, o + 0x58), U16(b, o + 0x12), 0};
        const u64 mip = U64(b, o + 0x70);
        if (mip + 8 > b.size())
            continue;
        t.image = U64(b, mip);
        if (!t.w || !t.h || t.w > MaxDimension || t.h > MaxDimension ||
            static_cast<u64>(t.w) * t.h > MaxPixels || !t.arrays || t.image >= b.size())
            continue;
        if (!chosen)
            chosen = t;
        const u64 no = U64(b, o + 0x60);
        if (no + 2 > b.size())
            continue;
        const u16 nl = U16(b, no);
        if (no + 2 + nl > b.size())
            continue;
        std::string_view name(reinterpret_cast<const char*>(b.data() + no + 2), nl);
        if (name.size() == wanted.size() &&
            std::equal(name.begin(), name.end(), wanted.begin(), wanted.end(), [](char a, char c) {
                return std::tolower((unsigned char)a) == std::tolower((unsigned char)c);
            })) {
            chosen = t;
            matched = true;
            break;
        }
    }
    if (!chosen || (!first && !matched))
        return std::nullopt;
    const auto t = *chosen;
    auto f = GetFormat(t.format);
    if (!f)
        return std::nullopt;
    const u32 bx = (t.w + f->bw - 1) / f->bw, by = (t.h + f->bh - 1) / f->bh;
    const size_t linear_size = static_cast<size_t>(bx) * by * f->bpb;
    const size_t array_size = t.size / t.arrays;
    if (t.image + array_size > b.size())
        return std::nullopt;
    auto src = b.subspan(t.image, array_size);
    std::vector<u8> blocks(linear_size);
    if (t.tile == 0) {
        auto linear = Unswizzle2d(src, f->bpb, bx, by, t.layout & 7);
        if (!linear)
            return std::nullopt;
        blocks = std::move(*linear);
    } else if (t.tile == 1) {
        const size_t row = static_cast<size_t>(bx) * f->bpb;
        size_t pitch = (row + 31) / 32 * 32;
        if (pitch * by > src.size())
            pitch = row;
        if (pitch * by > src.size())
            return std::nullopt;
        for (u32 y = 0; y < by; ++y)
            std::memcpy(blocks.data() + y * row, src.data() + y * pitch, row);
    } else
        return std::nullopt;
    std::vector<u8> channels(static_cast<size_t>(t.w) * t.h * f->channels);
    const u32 fc = t.format >> 8;
    if (fc == 2 || fc == 9 || fc == 11) {
        if (blocks.size() < channels.size())
            return std::nullopt;
        std::copy_n(blocks.begin(), channels.size(), channels.begin());
    } else if (fc == 12) {
        if (blocks.size() < channels.size())
            return std::nullopt;
        for (size_t p = 0; p < channels.size(); p += 4) {
            channels[p] = blocks[p + 2];
            channels[p + 1] = blocks[p + 1];
            channels[p + 2] = blocks[p];
            channels[p + 3] = blocks[p + 3];
        }
    } else if (fc >= 45 && fc <= 58) {
        if (!astc.decode || !astc.decode(astc.userdata, t.w, t.h, f->bw, f->bh, blocks.data(),
                                         blocks.size(), channels.data(), channels.size()))
            return std::nullopt;
    } else {
        for (u32 y = 0; y < by; ++y)
            for (u32 x = 0; x < bx; ++x) {
                const u8* s = blocks.data() + (static_cast<size_t>(y) * bx + x) * f->bpb;
                u8* d = channels.data() + (static_cast<size_t>(y) * 4 * t.w + x * 4) * f->channels;
                switch (fc) {
                case 26:
                    bcn::DecodeBc1(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 27:
                    bcn::DecodeBc2(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 28:
                    bcn::DecodeBc3(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                case 29:
                    bcn::DecodeBc4(s, d, x * 4, y * 4, t.w, t.h, (t.format & 255) == 2);
                    break;
                case 30:
                    bcn::DecodeBc5(s, d, x * 4, y * 4, t.w, t.h, (t.format & 255) == 2);
                    break;
                case 32:
                    bcn::DecodeBc7(s, d, x * 4, y * 4, t.w, t.h);
                    break;
                default:
                    return std::nullopt;
                }
            }
    }
    Image out{t.w, t.h, std::vector<u8>(static_cast<size_t>(t.w) * t.h * 4)};
    for (size_t p = 0; p < static_cast<size_t>(t.w) * t.h; ++p)
        for (u32 c = 0; c < 4; ++c) {
            u8 sel = static_cast<u8>(t.cmap >> (c * 8));
            out.rgba[p * 4 + c] =
                sel == 0   ? 0
                : sel == 1 ? 255
                           : (sel - 2 < f->channels ? channels[p * f->channels + sel - 2] : 0);
        }
    return out;
}
std::optional<std::string> Component(std::string_view key, std::string_view pre,
                                     std::string_view suf) {
    if (!key.starts_with(pre) || !key.ends_with(suf) || key.size() <= pre.size() + suf.size())
        return {};
    auto v = key.substr(pre.size(), key.size() - pre.size() - suf.size());
    return Safe(v) ? std::optional{std::string(v)} : std::nullopt;
}
} // namespace

struct Decoder::Impl {
    explicit Impl(size_t b) : budget(b) {}
    size_t budget, used{};
    AstcDecoder astc{};
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const Image>> images;
    std::unordered_map<std::string, std::shared_ptr<const MapCamera>> cameras;
    std::list<std::string> lru;
};
Decoder::Decoder(size_t b) : impl(std::make_unique<Impl>(b)) {}
Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;
void Decoder::Clear() {
    std::scoped_lock l(impl->mutex);
    impl->images.clear();
    impl->cameras.clear();
    impl->lru.clear();
    impl->used = 0;
}
void Decoder::SetAstcDecoder(AstcDecoder decoder) {
    std::scoped_lock l(impl->mutex);
    impl->astc = decoder;
    impl->images.clear();
    impl->lru.clear();
    impl->used = 0;
}
std::string Decoder::DriverImageKey(u32 id, u32 variant) {
    static constexpr std::array<const char*, 53> names{
        "Mario",       "Luigi",      "Peach",      "Daisy",     "Yoshi",      "Kinopio",
        "Kinopico",    "Nokonoko",   "Koopa",      "DK",        "Wario",      "Waluigi",
        "Rosetta",     "MetalMario", "PGoldPeach", "Jugemu",    "Heyho",      "BbMario",
        "BbLuigi",     "BbPeach",    "BbDaisy",    "BbRosetta", "Larry",      "Lemmy",
        "Wendy",       "Ludwig",     "Iggy",       "Roy",       "Morton",     nullptr,
        "TanukiMario", "Link",       "AnimalBoyA", "Shizue",    "CatPeach",   "HoneKoopa",
        "AnimalGirlA", "GoldMario",  "Karon",      "KoopaJr",   "KingTeresa", "SplatoonGirl",
        "SplatoonBoy", "LinkBotw",   "Catherine",  "Kameck",    "BossPakkun", "Hanachan",
        "DiddyKong",   "FK",         "Kinopeach",  "Pauline",   nullptr};
    if (id >= names.size() || !names[id])
        return {};
    std::string alias = names[id];
    int max = -1;
    if (id == 4 || id == 16 || id == 44)
        max = 8;
    else if (id == 41 || id == 42)
        max = 2;
    if (max >= 0) {
        variant = std::min<u32>(variant, max);
        alias.push_back('0' + variant / 10);
        alias.push_back('0' + variant % 10);
    }
    return "mk8d/characters/tc_MapChara_" + alias + ".png";
}
std::shared_ptr<const Image> Decoder::LoadImage(const EdenDsmodHostApi& host,
                                                std::string_view key) {
    std::scoped_lock l(impl->mutex);
    if (auto i = impl->images.find(std::string(key)); i != impl->images.end())
        return i->second;
    std::optional<Image> decoded;
    std::string cache(key);
    if (auto n = Component(key, "mk8d/maps/", ".png")) {
        auto raw = Read(host, "/Course/" + *n + "/course_maptexture.bntx", 4 * 1024 * 1024);
        if (raw)
            decoded = Decode(*raw, {}, true, impl->astc);
    } else if (auto n = Component(key, "mk8d/characters/", ".png");
               n && n->starts_with("tc_MapChara_")) {
        auto a = Nested(host, "/UI/cmn/common.sarc", "cm_L_CharaIcon_00.szs");
        if (a)
            decoded = Decode(*a, *n + "^l", false, impl->astc);
    } else if (key == "mk8d/ui/lap_flag.png") {
        auto a = Nested(host, "/UI/cmn/race.sarc", "rc_L_Lap_00.szs");
        if (a)
            decoded = Decode(*a, "ym_LapFlag_00^f", false, impl->astc);
    } else if (auto n = Component(key, "mk8d/items/item_", ".png")) {
        static constexpr std::array names{
            "tc_Item_Banana^l",  "tc_Item_Koura^l",        "tc_Item_RedKoura^l",
            "tc_Item_Mush^l",    "tc_Item_Bomb^l",         "tc_Item_Gesso^l",
            "tc_Item_Togezo^l",  "tc_Item_Mush3^l",        "tc_Item_Star^l",
            "tc_Item_Killer^l",  "tc_Item_Thunder^l",      "tc_Item_PKinoko^l",
            "tc_Item_Flower^l",  "tc_Item_PackunFlower^l", "tc_Item_Boomerang^l",
            "tc_Item_Coin^l",    "tc_Item_SuperHorn^l",    "tc_Item_Banana3^l",
            "tc_Item_Koura3^l",  "tc_Item_RedKoura3^l",    "tc_Item_SP8^l",
            "tc_Item_Feather^l", "tc_Item_Teresa^l"};
        if (n->empty() ||
            !std::all_of(n->begin(), n->end(), [](unsigned char c) { return std::isdigit(c); }))
            return {};
        size_t id = 0;
        for (char c : *n) {
            if (id > 1000)
                return {};
            id = id * 10 + static_cast<size_t>(c - '0');
        }
        if (id < names.size()) {
            auto a = Nested(host, "/UI/cmn/race.sarc", "rc_L_ItemBox_00.szs");
            if (a)
                decoded = Decode(*a, names[id], false, impl->astc);
        }
    }
    if (!decoded)
        return {};
    auto result = std::make_shared<Image>(std::move(*decoded));
    const auto bytes = result->rgba.size();
    if (bytes > impl->budget)
        return result;
    while (impl->used + bytes > impl->budget && !impl->lru.empty()) {
        auto old = impl->lru.front();
        impl->lru.pop_front();
        auto it = impl->images.find(old);
        if (it != impl->images.end()) {
            impl->used -= it->second->rgba.size();
            impl->images.erase(it);
        }
    }
    impl->images.emplace(cache, result);
    impl->lru.push_back(cache);
    impl->used += bytes;
    return result;
}
std::shared_ptr<const MapCamera> Decoder::LoadMapCamera(const EdenDsmodHostApi& host,
                                                        std::string_view course) {
    std::scoped_lock l(impl->mutex);
    if (!Safe(course))
        return {};
    if (auto i = impl->cameras.find(std::string(course)); i != impl->cameras.end())
        return i->second;
    auto d = Read(host, "/Course/" + std::string(course) + "/course_mapcamera.bin", 4096);
    if (!d || d->size() < 44)
        return {};
    MapCamera c{};
    std::memcpy(c.position, d->data(), 12);
    std::memcpy(c.look_at, d->data() + 12, 12);
    std::memcpy(c.source_up, d->data() + 24, 12);
    std::memcpy(&c.width, d->data() + 36, 4);
    std::memcpy(&c.height, d->data() + 40, 4);
    float f[3]{c.look_at[0] - c.position[0], c.look_at[1] - c.position[1],
               c.look_at[2] - c.position[2]};
    auto norm = [](float* v) {
        float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!std::isfinite(n) || n < .0001f)
            return false;
        for (int i = 0; i < 3; ++i)
            v[i] /= n;
        return true;
    };
    if (!norm(f) || !std::isfinite(c.width) || !std::isfinite(c.height) || c.width <= 0 ||
        c.height <= 0)
        return {};
    c.right[0] = f[1] * c.source_up[2] - f[2] * c.source_up[1];
    c.right[1] = f[2] * c.source_up[0] - f[0] * c.source_up[2];
    c.right[2] = f[0] * c.source_up[1] - f[1] * c.source_up[0];
    if (!norm(c.right))
        return {};
    c.up[0] = c.right[1] * f[2] - c.right[2] * f[1];
    c.up[1] = c.right[2] * f[0] - c.right[0] * f[2];
    c.up[2] = c.right[0] * f[1] - c.right[1] * f[0];
    auto r = std::make_shared<MapCamera>(c);
    impl->cameras.emplace(std::string(course), r);
    return r;
}
bool MapCamera::Project(float x, float y, float z, float& u, float& v) const {
    const float d[3]{x - look_at[0], y - look_at[1], z - look_at[2]};
    const float vx = d[0] * right[0] + d[1] * right[1] + d[2] * right[2],
                vy = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    if (!std::isfinite(vx) || !std::isfinite(vy) || width <= 0 || height <= 0)
        return false;
    u = .5f + vx / width;
    v = .5f - vy / height;
    return true;
}
} // namespace Mk8dAssets
