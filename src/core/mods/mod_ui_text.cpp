// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Text: the built-in 3x5 font, UTF-8 decoding (a byte that is not well-formed UTF-8 draws as
// Latin-1), the plain stand-in for a code point a font lacks, and the Canvas text calls:
// MeasureText, LayoutText (wrap, max_lines, ellipsis; cached per canvas), DrawTextBlock,
// DrawTextAligned, DrawText, FindGlyph, inline icons from a second font (TextIconBase code
// points), and colour tags ("{c:#AARRGGBB}".."{/c}", SetColorMarkup). With a game font set, a
// glyph is an alpha mask blitted by DrawImageRegion (mod_ui_image.cpp) in the caller's colour;
// the built-in font draws FillRect cells.
// Not here: the map's label bitmaps (mod_ui_map_widget.cpp), text bounds for dirty rects
// (WidgetTextBounds, mod_ui_widget_state.cpp).

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "common/cityhash.h"
#include "core/mods/mod_msbt.h"
#include "core/mods/mod_ui.h"

namespace Core::Mods {

namespace {
// 3x5 bitmap font, 3 bits per row, MSB = leftmost pixel.
const std::array<u8, 5>& Glyph(char c) {
    static const std::array<std::array<u8, 5>, 10> digits{{{7, 5, 5, 5, 7},
                                                           {2, 6, 2, 2, 7},
                                                           {7, 1, 7, 4, 7},
                                                           {7, 1, 7, 1, 7},
                                                           {5, 5, 7, 1, 1},
                                                           {7, 4, 7, 1, 7},
                                                           {7, 4, 7, 5, 7},
                                                           {7, 1, 1, 1, 1},
                                                           {7, 5, 7, 5, 7},
                                                           {7, 5, 7, 1, 7}}};
    static const std::array<std::array<u8, 5>, 26> letters{
        {{2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {7, 4, 4, 4, 7}, {6, 5, 5, 5, 6}, {7, 4, 7, 4, 7},
         {7, 4, 7, 4, 4}, {7, 4, 5, 5, 7}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 7},
         {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {7, 5, 5, 5, 7},
         {7, 5, 7, 4, 4}, {7, 5, 5, 7, 1}, {7, 5, 6, 5, 5}, {7, 4, 7, 1, 7}, {7, 2, 2, 2, 2},
         {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, {5, 5, 2, 2, 2},
         {7, 1, 2, 4, 7}}};
    static const std::array<u8, 5> colon{0, 2, 0, 2, 0};
    static const std::array<u8, 5> dash{0, 0, 7, 0, 0};
    static const std::array<u8, 5> dot{0, 0, 0, 0, 2};
    static const std::array<u8, 5> slash{1, 1, 2, 4, 4};
    static const std::array<u8, 5> percent{5, 1, 2, 4, 5};
    static const std::array<u8, 5> plus{0, 2, 7, 2, 0};
    static const std::array<u8, 5> comma{0, 0, 0, 2, 4};
    static const std::array<u8, 5> space{0, 0, 0, 0, 0};

    if (c >= '0' && c <= '9') {
        return digits[static_cast<size_t>(c - '0')];
    }
    if (c >= 'A' && c <= 'Z') {
        return letters[static_cast<size_t>(c - 'A')];
    }
    if (c >= 'a' && c <= 'z') {
        return letters[static_cast<size_t>(c - 'a')];
    }
    switch (c) {
    case ':':
        return colon;
    case '-':
        return dash;
    case '.':
        return dot;
    case '/':
        return slash;
    case '%':
        return percent;
    case '+':
        return plus;
    case ',':
        return comma;
    default:
        return space;
    }
}

/// Next codepoint of a UTF-8 string, advancing `i`. Manifest text is UTF-8 ("Dampé's Shack"); a
/// byte that does not start a well-formed sequence is taken as Latin-1 on its own, so a string
/// that was written byte-wise still draws as before.
char32_t NextCodepoint(std::string_view text, size_t& i) {
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
    static constexpr std::array<char32_t, 5> min_for_len{0, 0, 0x80, 0x800, 0x10000};
    if (cp < min_for_len[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return b0; // overlong / out of range: not UTF-8 after all
    }
    i += len;
    return cp;
}

/// A stand-in for a codepoint a font has no glyph for: typographic punctuation becomes its ASCII
/// twin, Latin-1 letters their base letter (the built-in font has A-Z only). 0 = no stand-in.
char32_t FallbackCodepoint(char32_t c) {
    // U+00C0..U+00FF
    static constexpr std::string_view latin1 =
        "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTSaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (c >= 0xC0 && c <= 0xFF) {
        return static_cast<char32_t>(latin1[c - 0xC0]);
    }
    switch (c) {
    case 0x2018:
    case 0x2019:
        return '\'';
    case 0x201C:
    case 0x201D:
        return '"';
    case 0x2013:
    case 0x2014:
        return '-';
    case 0x2026:
        return '.';
    case 0x00A0:
        return ' ';
    default:
        return 0;
    }
}

bool IsIconCodepoint(char32_t c) {
    return c >= TextIconBase && c <= TextIconLast;
}

/// A colour tag at text[i]: "{c:#AARRGGBB}" (open) or "{/c}" (close). len 0 = not a tag.
struct MarkupTag {
    size_t len{0};
    bool close{false};
    u32 color{0};
};
MarkupTag ReadMarkupTag(std::string_view text, size_t i) {
    const std::string_view rest = text.substr(i);
    if (rest.starts_with("{/c}")) {
        return {4, true, 0};
    }
    if (rest.size() < 13 || !rest.starts_with("{c:#") || rest[12] != '}') {
        return {};
    }
    u32 color = 0;
    for (size_t k = 4; k < 12; ++k) {
        const char c = rest[k];
        const u32 digit = c >= '0' && c <= '9'   ? static_cast<u32>(c - '0')
                          : c >= 'a' && c <= 'f' ? static_cast<u32>(c - 'a' + 10)
                          : c >= 'A' && c <= 'F' ? static_cast<u32>(c - 'A' + 10)
                                                 : 16u;
        if (digit == 16u) {
            return {};
        }
        color = (color << 4) | digit;
    }
    return {13, false, color};
}

/// The end of the whole tag when `markup` is on and one starts at text[i], else of one code point.
size_t NextUnitEnd(std::string_view text, size_t i, bool markup) {
    if (markup) {
        if (const MarkupTag tag = ReadMarkupTag(text, i); tag.len > 0) {
            return i + tag.len;
        }
    }
    NextCodepoint(text, i);
    return i;
}

/// Drops trailing spaces; with `markup`, also the spaces before trailing colour tags, which stay
/// ("red {/c}" -> "red{/c}"), so a centred line or an ellipsis is not pushed off by a blank.
void TrimTrailingSpaces(std::string& s, bool markup) {
    std::string tags;
    while (true) {
        while (!s.empty() && s.back() == ' ') {
            s.pop_back();
        }
        if (!markup || s.empty() || s.back() != '}') {
            break;
        }
        const size_t brace = s.rfind('{');
        if (brace == std::string::npos || brace + ReadMarkupTag(s, brace).len != s.size()) {
            break;
        }
        tags.insert(0, s, brace);
        s.resize(brace);
    }
    s += tags;
}

/// Whether `s` holds anything besides colour tags (with `markup`; else whether it is non-empty).
bool HasTextBesidesTags(std::string_view s, bool markup) {
    if (!markup) {
        return !s.empty();
    }
    return TextMarkupBytes(s) < s.size();
}

/// Whether a colour span is still open at the end of `s`.
bool SpanOpenAtEnd(std::string_view s) {
    bool open = false;
    for (size_t i = 0; i < s.size();) {
        const MarkupTag tag = ReadMarkupTag(s, i);
        if (tag.len > 0) {
            open = !tag.close;
            i += tag.len;
        } else {
            ++i;
        }
    }
    return open;
}
} // namespace

size_t TextMarkupBytes(std::string_view text) {
    size_t bytes = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t len = ReadMarkupTag(text, i).len;
        bytes += len;
        i += len > 0 ? len : 1;
    }
    return bytes;
}

char32_t TextFallbackCodepoint(char32_t c) {
    return FallbackCodepoint(c);
}

s32 Canvas::MeasureText(std::string_view text, s32 scale) const {
    if (scale <= 0) {
        scale = 1;
    }
    const f32 wanted = static_cast<f32>(scale) * 5.0f;
    if (HasFont()) {
        const f32 ratio = wanted / static_cast<f32>(font_metrics->line_height);
        f32 pen = 0.0f;
        for (size_t i = 0; i < text.size();) {
            if (const size_t tag = color_markup ? ReadMarkupTag(text, i).len : 0; tag > 0) {
                i += tag;
                continue;
            }
            const char32_t cp = NextCodepoint(text, i);
            if (cp == '\n') {
                continue;
            }
            if (IsIconCodepoint(cp)) {
                pen += IconAdvance(cp - TextIconBase, wanted, scale);
                continue;
            }
            const auto* g = FindGlyph(cp);
            pen += g == nullptr ? wanted * 0.5f : static_cast<f32>(g->advance) * ratio;
        }
        return static_cast<s32>(pen);
    }
    s32 glyphs = 0; // 3 px glyph + 1 px gap per codepoint, built-in font
    f32 icons = 0.0f;
    for (size_t i = 0; i < text.size();) {
        if (const size_t tag = color_markup ? ReadMarkupTag(text, i).len : 0; tag > 0) {
            i += tag;
            continue;
        }
        const char32_t cp = NextCodepoint(text, i);
        if (cp == '\n') {
            continue;
        }
        if (IsIconCodepoint(cp)) {
            icons += IconAdvance(cp - TextIconBase, wanted, scale);
            continue;
        }
        ++glyphs;
    }
    return glyphs * 4 * scale + static_cast<s32>(icons);
}

f32 Canvas::TextAscentPx(f32 cap) const {
    if (HasFont() && font_metrics->ascent > 0) {
        return static_cast<f32>(font_metrics->ascent) * cap /
               static_cast<f32>(font_metrics->line_height);
    }
    return cap * 1.44f; // the game font's ascent over its cap height (49 / 34)
}

bool Canvas::IconDrawable(char32_t glyph) const {
    return icon_atlas != nullptr && icon_metrics != nullptr && icon_atlas->Valid() &&
           (icon_metrics->ascent > 0 || icon_metrics->line_height > 0) &&
           icon_metrics->Find(glyph) != nullptr;
}

f32 Canvas::IconAdvance(char32_t glyph, f32 cap, s32 scale) const {
    if (IconDrawable(glyph)) {
        const f32 icon_ascent = static_cast<f32>(
            icon_metrics->ascent > 0 ? icon_metrics->ascent : icon_metrics->line_height);
        return static_cast<f32>(icon_metrics->Find(glyph)->advance) * TextAscentPx(cap) /
               icon_ascent;
    }
    if (icon_pending) {
        return TextAscentPx(cap); // the icon's space, left blank until its font lands
    }
    std::string alt = "(";
    Msbt::AppendUtf8(alt, glyph);
    alt += ')';
    return static_cast<f32>(MeasureText(alt, scale));
}

f32 Canvas::DrawIcon(f32 pen, f32 baseline, char32_t glyph, f32 cap, s32 scale, u32 argb) {
    if (IconDrawable(glyph)) {
        const FontGlyph* const g = icon_metrics->Find(glyph);
        const f32 icon_ascent = static_cast<f32>(
            icon_metrics->ascent > 0 ? icon_metrics->ascent : icon_metrics->line_height);
        const f32 ratio = TextAscentPx(cap) / icon_ascent;
        if (g->w > 0 && g->h > 0) {
            const s32 gx = static_cast<s32>(pen + static_cast<f32>(g->bearing_x) * ratio);
            const s32 gy = static_cast<s32>(baseline - static_cast<f32>(g->bearing_y) * ratio);
            const s32 gw = std::max(1, static_cast<s32>(static_cast<f32>(g->w) * ratio));
            const s32 gh = std::max(1, static_cast<s32>(static_cast<f32>(g->h) * ratio));
            if (icon_silhouette) {
                DrawImageMask(gx, gy, gw, gh, *icon_atlas, g->x, g->y, g->w, g->h, argb);
            } else {
                // Full-colour icons: only the text's alpha applies.
                DrawImageRegion(gx, gy, gw, gh, *icon_atlas, g->x, g->y, g->w, g->h,
                                (argb & 0xFF000000u) | 0x00FFFFFFu);
            }
        }
        return static_cast<f32>(g->advance) * ratio;
    }
    if (icon_pending) {
        return TextAscentPx(cap);
    }
    std::string alt = "(";
    Msbt::AppendUtf8(alt, glyph);
    alt += ')';
    DrawText(static_cast<s32>(pen), static_cast<s32>(baseline - cap), alt, scale, argb);
    return static_cast<f32>(MeasureText(alt, scale));
}

namespace {
u64 MixLayoutHash(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

/// Everything about a font that text layout reads: its glyph table, the out-of-run glyphs and the
/// metrics that scale them.
u64 FontContentHash(const FontMetrics& font) {
    u64 h = Common::CityHash64(reinterpret_cast<const char*>(font.glyphs.data()),
                               font.glyphs.size() * sizeof(FontGlyph));
    h = MixLayoutHash(h, font.line_height);
    h = MixLayoutHash(h, font.first_codepoint);
    h = MixLayoutHash(h, font.ascent);
    h = MixLayoutHash(h, font.extra.size());
    u64 extra = 0; // order-free: an unordered_map's iteration order is not part of its content
    for (const auto& [c, g] : font.extra) {
        u64 e = MixLayoutHash(c, (u64{g.x} << 48) | (u64{g.y} << 32) | (u64{g.w} << 16) | g.h);
        e = MixLayoutHash(e, (u64{static_cast<u16>(g.bearing_x)} << 32) |
                                 (u64{static_cast<u16>(g.bearing_y)} << 16) | g.advance);
        extra += e;
    }
    return MixLayoutHash(h, extra);
}
} // namespace

u64 Canvas::FontsFingerprint() const {
    u64 h = HasFont() ? MixLayoutHash(1, FontContentHash(*font_metrics)) : 2;
    // Inline icons take their advance from the icon font only while it can draw them; otherwise
    // from whether it is still loading.
    const bool icons = icon_atlas != nullptr && icon_metrics != nullptr && icon_atlas->Valid();
    h = MixLayoutHash(h, icons ? FontContentHash(*icon_metrics) : 3);
    return MixLayoutHash(h, icon_pending ? 5 : 7);
}

std::vector<std::string> Canvas::LayoutText(std::string_view text, s32 scale, s32 wrap_width,
                                            s32 max_lines) const {
    return LayoutLines(text, scale, wrap_width, max_lines);
}

const std::vector<std::string>& Canvas::LayoutLines(std::string_view text, s32 scale,
                                                    s32 wrap_width, s32 max_lines) const {
    if (scale <= 0) {
        scale = 1;
    }
    if (layout.fonts_changed) {
        layout.fonts_changed = false;
        if (const u64 fp = FontsFingerprint(); fp != layout.fonts) {
            layout.fonts = fp;
            layout.lru.clear();
            layout.index.clear();
        }
    }
    u64 hash = Common::CityHash64(text.data(), text.size());
    hash = MixLayoutHash(hash, static_cast<u32>(scale));
    hash = MixLayoutHash(hash, static_cast<u32>(wrap_width));
    hash = MixLayoutHash(hash, static_cast<u32>(max_lines));
    hash = MixLayoutHash(hash, color_markup ? 11 : 13);
    const auto same = [&](const LayoutEntry& e) {
        return e.scale == scale && e.wrap == wrap_width && e.lines == max_lines &&
               e.markup == color_markup && e.fonts == layout.fonts && e.text == text;
    };
    if (const auto found = layout.index.find(hash); found != layout.index.end()) {
        if (same(*found->second)) {
            layout.lru.splice(layout.lru.begin(), layout.lru, found->second);
            return found->second->out;
        }
        layout.lru.erase(found->second); // a hash collision: the newer text takes the slot
        layout.index.erase(found);
    }
    const auto width_of = [&](std::string_view part) { return MeasureText(part, scale); };
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        const size_t nl = text.find('\n', start);
        const std::string_view para =
            text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (wrap_width <= 0) {
            lines.emplace_back(para);
        } else {
            const size_t first_line = lines.size();
            std::string cur;
            bool has_cur = false;
            size_t p = 0;
            while (true) {
                const size_t sp = para.find(' ', p);
                const std::string_view word =
                    para.substr(p, sp == std::string_view::npos ? std::string_view::npos : sp - p);
                if (has_cur) {
                    std::string cand = cur;
                    cand += ' ';
                    cand += word;
                    if (width_of(cand) > wrap_width) {
                        lines.push_back(std::move(cur));
                        cur.clear();
                        has_cur = false;
                        continue; // the same word starts the next line
                    }
                    cur = std::move(cand);
                } else if (width_of(word) <= wrap_width) {
                    cur = std::string{word};
                    has_cur = true;
                } else {
                    // A word longer than a whole line (or text without spaces): by character.
                    std::string piece;
                    for (size_t i = 0; i < word.size();) {
                        const size_t j = NextUnitEnd(word, i, color_markup);
                        std::string next = piece;
                        next += word.substr(i, j - i);
                        // A piece of colour tags only (no width) never makes a line of its own.
                        if (HasTextBesidesTags(piece, color_markup) &&
                            width_of(next) > wrap_width) {
                            lines.push_back(std::move(piece));
                            piece = std::string{word.substr(i, j - i)};
                        } else {
                            piece = std::move(next);
                        }
                        i = j;
                    }
                    cur = std::move(piece);
                    has_cur = true;
                }
                if (sp == std::string_view::npos) {
                    break;
                }
                p = sp + 1;
            }
            lines.push_back(std::move(cur));
            for (size_t i = first_line; i < lines.size(); ++i) {
                // a trailing space would pull a centred line off-centre
                TrimTrailingSpaces(lines[i], color_markup);
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    if (color_markup) {
        // A span left open at a line's end is re-opened at the next line's start: each line is
        // drawn on its own, from the draw colour.
        std::string open;
        for (auto& line : lines) {
            const std::string carried = open;
            for (size_t i = 0; i < line.size();) {
                const MarkupTag tag = ReadMarkupTag(line, i);
                if (tag.len > 0) {
                    open = tag.close ? std::string{} : line.substr(i, tag.len);
                }
                i += tag.len > 0 ? tag.len : 1;
            }
            line.insert(0, carried);
        }
    }
    if (max_lines > 0 && lines.size() > static_cast<size_t>(max_lines)) {
        lines.resize(static_cast<size_t>(max_lines));
        const bool has_ellipsis = HasFont() && font_metrics->Find(0x2026) != nullptr &&
                                  font_metrics->Find(0x2026)->advance > 0;
        const std::string ellipsis = has_ellipsis ? "\xE2\x80\xA6" : "...";
        std::string& last = lines.back();
        const auto trim_spaces = [&last, this] { TrimTrailingSpaces(last, color_markup); };
        trim_spaces();
        while (!last.empty() && wrap_width > 0 && width_of(last + ellipsis) > wrap_width) {
            size_t cut = last.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(last[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            if (color_markup && last.back() == '}') {
                // A tag is dropped whole, never cut into (which would print its remains).
                const size_t brace = last.rfind('{');
                if (brace != std::string::npos &&
                    brace + ReadMarkupTag(last, brace).len == last.size()) {
                    cut = brace;
                }
            }
            last.resize(cut);
            trim_spaces();
        }
        if (color_markup) {
            // A span opened right at the cut colours nothing: drop its tag. One the cut ended
            // early is closed: the ellipsis is not part of it.
            while (last.ends_with('}')) {
                const size_t brace = last.rfind('{');
                const MarkupTag tag =
                    brace == std::string::npos ? MarkupTag{} : ReadMarkupTag(last, brace);
                if (tag.len == 0 || tag.close || brace + tag.len != last.size()) {
                    break;
                }
                last.resize(brace);
                trim_spaces();
            }
            if (SpanOpenAtEnd(last)) {
                last += "{/c}";
            }
        }
        last += ellipsis;
    }
    if (layout.lru.size() >= LayoutCacheSize) {
        layout.index.erase(layout.lru.back().hash);
        layout.lru.pop_back();
    }
    layout.lru.push_front(LayoutEntry{hash, std::string{text}, scale, wrap_width, max_lines,
                                      layout.fonts, color_markup, std::move(lines)});
    layout.index.emplace(hash, layout.lru.begin());
    return layout.lru.front().out;
}

void Canvas::DrawTextBlock(s32 x, s32 y, std::string_view text, s32 scale, u32 argb, s32 align,
                           s32 wrap_width, s32 max_lines, s32 line_gap) {
    if (scale <= 0) {
        scale = 1;
    }
    const s32 pitch = scale * 5 + (line_gap >= 0 ? line_gap : scale * 3);
    // By reference: nothing below lays text out again (DrawTextAligned only measures and draws).
    const auto& lines = LayoutLines(text, scale, wrap_width, max_lines);
    for (size_t i = 0; i < lines.size(); ++i) {
        DrawTextAligned(x, y + static_cast<s32>(i) * pitch, lines[i], scale, argb, align);
    }
}

const FontGlyph* Canvas::FindGlyph(char32_t c) const {
    if (font_metrics == nullptr) {
        return nullptr;
    }
    if (const auto* g = font_metrics->Find(c); g != nullptr) {
        return g;
    }
    const char32_t alt = FallbackCodepoint(c);
    return alt != 0 ? font_metrics->Find(alt) : nullptr;
}

void Canvas::DrawTextAligned(s32 x, s32 y, std::string_view text, s32 scale, u32 argb, s32 align) {
    if (align == 1) {
        x -= MeasureText(text, scale) / 2;
    } else if (align == 2) {
        x -= MeasureText(text, scale);
    }
    DrawText(x, y, text, scale, argb);
}

void Canvas::DrawText(s32 x, s32 y, std::string_view text, s32 scale, u32 argb) {
    if (scale <= 0) {
        scale = 1;
    }
    // Colour tags: always skipped, obeyed only off an outline copy. A package outlines a label
    // with offset copies of it in the outline colour ("outline_copy": everything in one colour);
    // those must not pick up the spans, only the main copy does.
    const u32 base = argb;
    const auto markup_tag = [&](size_t& i) {
        if (!color_markup) {
            return false;
        }
        const MarkupTag tag = ReadMarkupTag(text, i);
        if (tag.len == 0) {
            return false;
        }
        if (!outline_copy) {
            argb = tag.close ? base : tag.color;
        }
        i += tag.len;
        return true;
    };
    if (HasFont()) {
        // The game's own font. Its atlas keeps coverage in alpha, so each glyph is a mask that
        // the caller's colour is painted through -- the same label code gets the game's
        // lettering without any of it being shipped.
        //
        // The built-in font is five pixels tall and drawn at `scale`, so match that: a scale of
        // three means fifteen pixels, and the real font is scaled to suit rather than to its
        // own design size.
        const f32 wanted = static_cast<f32>(scale) * 5.0f;
        const f32 ratio = wanted / static_cast<f32>(font_metrics->line_height);
        const f32 baseline = static_cast<f32>(y) + wanted;
        // Optional outline: the glyph mask stamped in the outline colour around a ring first
        // (16 directions), then the fill on top -- the game's HUD lettering. Optional rise: glyph
        // i sits i * text_rise pixels higher, as the game's HUD digits step up.
        const bool outline = text_outline_px > 0 && (text_outline >> 24) != 0;
        for (int pass = outline ? 0 : 1; pass < 2; ++pass) {
            f32 pen = static_cast<f32>(x);
            s32 index = 0;
            argb = base; // colour spans restart with each pass
            for (size_t i = 0; i < text.size();) {
                // The outline pass skips colour tags too; it always paints the outline colour.
                if (markup_tag(i)) {
                    continue;
                }
                const char32_t cp = NextCodepoint(text, i);
                if (cp == '\n') {
                    continue;
                }
                if (IsIconCodepoint(cp)) {
                    if (pass == 1)
                        pen += DrawIcon(pen, baseline, cp - TextIconBase, wanted, scale, argb);
                    else
                        pen += wanted; // placeholder advance in the outline pass
                    continue;
                }
                const auto* g = FindGlyph(cp);
                if (g == nullptr) {
                    pen += wanted * 0.5f;
                    continue;
                }
                // Only digits rise ("35 WONDER SEEDS": the 3 and 5 step up, the words do not).
                const bool digit = cp >= '0' && cp <= '9';
                const f32 lift = digit ? text_rise * static_cast<f32>(index++) : 0.0f;
                if (g->w > 0 && g->h > 0) {
                    const s32 gx = static_cast<s32>(pen + static_cast<f32>(g->bearing_x) * ratio);
                    const s32 gy = static_cast<s32>(baseline - lift -
                                                    static_cast<f32>(g->bearing_y) * ratio);
                    const s32 gw = std::max(1, static_cast<s32>(static_cast<f32>(g->w) * ratio));
                    const s32 gh = std::max(1, static_cast<s32>(static_cast<f32>(g->h) * ratio));
                    if (pass == 0) {
                        const f32 r = static_cast<f32>(text_outline_px);
                        for (int k = 0; k < 16; ++k) {
                            const f32 a = static_cast<f32>(k) * 0.39269908f; // 2*pi/16
                            DrawImageMask(gx + static_cast<s32>(std::lround(r * std::cos(a))),
                                          gy + static_cast<s32>(std::lround(r * std::sin(a))), gw,
                                          gh, *font_atlas, g->x, g->y, g->w, g->h, text_outline);
                        }
                    } else {
                        DrawImageRegion(gx, gy, gw, gh, *font_atlas, g->x, g->y, g->w, g->h,
                                        argb);
                    }
                }
                pen += static_cast<f32>(g->advance) * ratio;
            }
        }
        return;
    }
    s32 cx = x;
    for (size_t i = 0; i < text.size();) {
        if (markup_tag(i)) {
            continue;
        }
        char32_t cp = NextCodepoint(text, i);
        if (cp == '\n') {
            continue;
        }
        if (IsIconCodepoint(cp)) {
            const f32 cap = static_cast<f32>(scale) * 5.0f;
            cx += static_cast<s32>(DrawIcon(static_cast<f32>(cx), static_cast<f32>(y) + cap,
                                            cp - TextIconBase, cap, scale, argb));
            continue;
        }
        if (cp >= 0x80) {
            cp = FallbackCodepoint(cp); // é -> e; anything else draws as a blank cell
        }
        const auto& g = Glyph(cp < 0x80 ? static_cast<char>(cp) : ' ');
        for (s32 row = 0; row < 5; ++row) {
            for (s32 bit = 0; bit < 3; ++bit) {
                if ((g[static_cast<size_t>(row)] & (4 >> bit)) != 0) {
                    FillRect(cx + bit * scale, y + row * scale, scale, scale, argb);
                }
            }
        }
        cx += 4 * scale;
    }
}

} // namespace Core::Mods
