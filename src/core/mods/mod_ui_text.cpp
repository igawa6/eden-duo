// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Text: the built-in 3x5 font, UTF-8 decoding (a byte that is not well-formed UTF-8 draws as
// Latin-1), the plain stand-in for a code point a font lacks, and the Canvas text calls:
// MeasureText, LayoutText (wrap, max_lines, ellipsis; cached per canvas), DrawTextBlock,
// DrawTextAligned, DrawText, FindGlyph, and inline icons from a second font (TextIconBase code
// points). With a game font set, a glyph is an alpha mask blitted by DrawImageRegion
// (mod_ui_image.cpp) in the caller's colour; the built-in font draws FillRect cells.
// Not here: the map's label bitmaps (mod_ui_map_widget.cpp), text bounds for dirty rects
// (WidgetTextBounds, mod_ui_widget_state.cpp).

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

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
} // namespace

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

std::vector<std::string> Canvas::LayoutText(std::string_view text, s32 scale, s32 wrap_width,
                                            s32 max_lines) const {
    if (scale <= 0) {
        scale = 1;
    }
    LayoutKey key{std::string{text},
                  scale,
                  wrap_width,
                  max_lines,
                  HasFont() ? font_metrics : nullptr,
                  HasFont() ? font_metrics->line_height ^
                                  (static_cast<u32>(font_metrics->extra.size()) << 16)
                            : 0,
                  icon_atlas != nullptr ? icon_metrics : nullptr,
                  icon_pending};
    for (const auto& [k, v] : layout_cache) {
        if (k == key) {
            return v;
        }
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
                        size_t j = i;
                        NextCodepoint(word, j);
                        std::string next = piece;
                        next += word.substr(i, j - i);
                        if (!piece.empty() && width_of(next) > wrap_width) {
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
                while (!lines[i].empty() && lines[i].back() == ' ') {
                    lines[i].pop_back(); // a trailing space would pull a centred line off-centre
                }
            }
        }
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    if (max_lines > 0 && lines.size() > static_cast<size_t>(max_lines)) {
        lines.resize(static_cast<size_t>(max_lines));
        const bool has_ellipsis = HasFont() && font_metrics->Find(0x2026) != nullptr &&
                                  font_metrics->Find(0x2026)->advance > 0;
        const std::string ellipsis = has_ellipsis ? "\xE2\x80\xA6" : "...";
        std::string& last = lines.back();
        const auto trim_spaces = [&last] {
            while (!last.empty() && last.back() == ' ') {
                last.pop_back();
            }
        };
        trim_spaces();
        while (!last.empty() && wrap_width > 0 && width_of(last + ellipsis) > wrap_width) {
            size_t cut = last.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(last[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            last.resize(cut);
            trim_spaces();
        }
        last += ellipsis;
    }
    if (layout_cache.size() >= 64) {
        layout_cache.erase(layout_cache.begin());
    }
    layout_cache.emplace_back(std::move(key), lines);
    return lines;
}

void Canvas::DrawTextBlock(s32 x, s32 y, std::string_view text, s32 scale, u32 argb, s32 align,
                           s32 wrap_width, s32 max_lines, s32 line_gap) {
    if (scale <= 0) {
        scale = 1;
    }
    const s32 pitch = scale * 5 + (line_gap >= 0 ? line_gap : scale * 3);
    const auto lines = LayoutText(text, scale, wrap_width, max_lines);
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
        f32 pen = static_cast<f32>(x);
        const f32 baseline = static_cast<f32>(y) + wanted;
        for (size_t i = 0; i < text.size();) {
            const char32_t cp = NextCodepoint(text, i);
            if (cp == '\n') {
                continue;
            }
            if (IsIconCodepoint(cp)) {
                pen += DrawIcon(pen, baseline, cp - TextIconBase, wanted, scale, argb);
                continue;
            }
            const auto* g = FindGlyph(cp);
            if (g == nullptr) {
                pen += wanted * 0.5f;
                continue;
            }
            if (g->w > 0 && g->h > 0) {
                const s32 gx = static_cast<s32>(pen + static_cast<f32>(g->bearing_x) * ratio);
                const s32 gy = static_cast<s32>(baseline - static_cast<f32>(g->bearing_y) * ratio);
                const s32 gw = std::max(1, static_cast<s32>(static_cast<f32>(g->w) * ratio));
                const s32 gh = std::max(1, static_cast<s32>(static_cast<f32>(g->h) * ratio));
                DrawImageRegion(gx, gy, gw, gh, *font_atlas, g->x, g->y, g->w, g->h, argb);
            }
            pen += static_cast<f32>(g->advance) * ratio;
        }
        return;
    }
    s32 cx = x;
    for (size_t i = 0; i < text.size();) {
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
