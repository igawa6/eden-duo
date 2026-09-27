// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "core/mods/mod_msbt.h"
#include "core/mods/mod_types.h"

namespace Core::Mods::Msbt {
namespace {

struct Reader {
    std::span<const u8> d;
    bool big{false};

    [[nodiscard]] bool Has(u64 at, u64 n) const {
        return at <= d.size() && n <= d.size() - at;
    }
    [[nodiscard]] u16 U16(u64 at) const {
        const u16 v = static_cast<u16>(d[at] | (d[at + 1] << 8));
        return big ? static_cast<u16>((v >> 8) | (v << 8)) : v;
    }
    [[nodiscard]] u32 U32(u64 at) const {
        u32 v = 0;
        std::memcpy(&v, d.data() + at, 4);
        return big ? ((v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24)) : v;
    }
};

void Remember(std::vector<u32>& list, u32 value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

/// Decodes one TXT2 entry [begin, end) into `out`.
void DecodeMessage(const Reader& r, u64 begin, u64 end, u8 encoding, const Options& options,
                   File& file, std::string& out) {
    const u64 unit = encoding == 0 ? 1 : encoding == 1 ? 2 : 4;
    const auto read_unit = [&](u64 at) -> u32 {
        return unit == 1 ? r.d[at] : unit == 2 ? r.U16(at) : r.U32(at);
    };
    u64 at = begin;
    while (at + unit <= end) {
        u32 c = read_unit(at);
        if (c == 0) {
            break;
        }
        if (c == 0x0E || c == 0x0F) {
            // Tag: group, type (u16 each), then for an opening tag a u16 parameter size and the
            // parameters themselves.
            const u64 head = at + unit;
            if (head + 4 > end) {
                break;
            }
            const u16 group = r.U16(head);
            const u16 type = r.U16(head + 2);
            if (c == 0x0F) {
                at = head + 4;
                Remember(file.stripped, 0x80000000u | (u32{group} << 16) | type);
                continue;
            }
            if (head + 6 > end) {
                break;
            }
            const u16 size = r.U16(head + 4);
            const u64 params = head + 6;
            if (params + size > end) {
                break;
            }
            at = params + size;
            if (group == options.icon_group && type == options.icon_type) {
                const u32 index = size > 0 ? r.d[params] : 0;
                const auto glyph = options.icon_glyphs != nullptr
                                       ? options.icon_glyphs->find(index)
                                       : std::unordered_map<u32, char32_t>::const_iterator{};
                if (options.icon_glyphs != nullptr && glyph != options.icon_glyphs->end() &&
                    glyph->second <= TextIconLast - TextIconBase) {
                    AppendUtf8(out, TextIconBase + glyph->second);
                } else {
                    Remember(file.unmapped_icons, index);
                }
                continue;
            }
            if (group == 0 && type == 0) {
                continue; // ruby: the reading is dropped, the base text follows as plain text
            }
            Remember(file.stripped, (u32{group} << 16) | type);
            continue;
        }
        if (unit == 1) {
            // UTF-8 copied through; a lone control character other than a line break is dropped.
            if (c < 0x20 && c != '\n') {
                ++at;
                continue;
            }
            out.push_back(static_cast<char>(c));
            ++at;
            continue;
        }
        at += unit;
        if (unit == 2 && c >= 0xD800 && c <= 0xDBFF && at + 2 <= end) {
            const u32 low = r.U16(at);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                at += 2;
            }
        }
        if ((c < 0x20 && c != '\n') || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
            continue;
        }
        AppendUtf8(out, static_cast<char32_t>(c));
    }
}

} // namespace

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

char32_t NextUtf8(std::string_view text, size_t& i) {
    const auto b0 = static_cast<unsigned char>(text[i]);
    size_t len = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07;
    }
    if (len == 0 || i + len > text.size()) {
        ++i;
        return b0;
    }
    for (size_t k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(text[i + k]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return b0;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    i += len;
    return cp;
}

bool Parse(std::span<const u8> bytes, const Options& options, File& out, std::string& error) {
    out = {};
    if (bytes.size() < 0x20 || std::memcmp(bytes.data(), "MsgStdBn", 8) != 0) {
        error = "not an MSBT";
        return false;
    }
    Reader r{bytes};
    if (bytes[8] == 0xFE && bytes[9] == 0xFF) {
        r.big = true;
    } else if (!(bytes[8] == 0xFF && bytes[9] == 0xFE)) {
        error = "bad byte-order mark";
        return false;
    }
    out.encoding = bytes[12];
    if (out.encoding > 2) {
        error = "unknown text encoding " + std::to_string(out.encoding);
        return false;
    }
    const u16 sections = r.U16(14);
    std::vector<std::pair<u32, std::string>> labels;
    u64 txt_at = 0, txt_size = 0;
    u64 at = 0x20;
    for (u16 s = 0; s < sections && r.Has(at, 16); ++s) {
        const u32 size = r.U32(at + 4);
        const u64 body = at + 16;
        if (!r.Has(body, size)) {
            error = "section runs past the end";
            return false;
        }
        if (std::memcmp(bytes.data() + at, "LBL1", 4) == 0 && size >= 4) {
            const u32 buckets = r.U32(body);
            for (u32 b = 0; b < buckets && r.Has(body + 4 + u64{b} * 8, 8); ++b) {
                const u32 count = r.U32(body + 4 + u64{b} * 8);
                u64 p = body + r.U32(body + 8 + u64{b} * 8);
                for (u32 k = 0; k < count && r.Has(p, 1); ++k) {
                    const u8 len = bytes[p];
                    if (!r.Has(p + 1, u64{len} + 4)) {
                        break;
                    }
                    labels.emplace_back(
                        r.U32(p + 1 + len),
                        std::string(reinterpret_cast<const char*>(bytes.data() + p + 1), len));
                    p += 1 + u64{len} + 4;
                }
            }
        } else if (std::memcmp(bytes.data() + at, "TXT2", 4) == 0 && size >= 4) {
            txt_at = body;
            txt_size = size;
        }
        at = body + ((u64{size} + 15) & ~u64{15});
    }
    if (txt_size == 0 || labels.empty()) {
        error = "no LBL1/TXT2";
        return false;
    }
    const u32 count = r.U32(txt_at);
    if (u64{count} * 4 + 4 > txt_size) {
        error = "bad TXT2 table";
        return false;
    }
    out.texts.reserve(labels.size());
    for (const auto& [index, name] : labels) {
        if (index >= count) {
            continue;
        }
        const u64 begin = txt_at + r.U32(txt_at + 4 + u64{index} * 4);
        const u64 end =
            index + 1 < count ? txt_at + r.U32(txt_at + 8 + u64{index} * 4) : txt_at + txt_size;
        if (begin > end || end > txt_at + txt_size) {
            continue;
        }
        std::string text;
        DecodeMessage(r, begin, end, out.encoding, options, out, text);
        out.texts.emplace(name, std::move(text));
    }
    return true;
}

} // namespace Core::Mods::Msbt
