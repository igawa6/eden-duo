// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Companion settings page (runtime 17); behaviour in mod_settings.h.

#include "core/mods/mod_settings.h"

#include <algorithm>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "core/mods/mod_types.h"

namespace Core::Mods {

namespace {
// Built-in look, shared with the "update Eden" page (UpdateRequiredManifest).
constexpr u32 SettingsBackground = 0xFF101014u;
constexpr u32 SettingsText = 0xFFE6ECF2u;
constexpr u32 SettingsValue = 0xFFFFC040u;
constexpr u32 SettingsRow = 0xFF1C2230u;
constexpr u32 SettingsAccent = 0xFF3C8CE6u;
constexpr u32 SettingsDefaultW = 1240;
constexpr u32 SettingsDefaultH = 1080;

std::optional<s64> IntOf(const nlohmann::json& v) {
    if (v.is_boolean()) {
        return v.get<bool>() ? 1 : 0;
    }
    if (v.is_number_integer()) {
        return v.get<s64>();
    }
    if (v.is_number_float()) {
        return static_cast<s64>(v.get<double>());
    }
    return std::nullopt;
}
} // namespace

std::vector<SettingDef> ParseSettings(const nlohmann::json& list) {
    std::vector<SettingDef> out;
    if (!list.is_array()) {
        LOG_WARNING(Core, "DSMod: \"settings\" must be an array of entries");
        return out;
    }
    for (const auto& e : list) {
        if (!e.is_object() || !e.contains("flag") || !e.at("flag").is_string() ||
            e.at("flag").get<std::string>().empty()) {
            LOG_WARNING(Core, "DSMod: \"settings\" entry {} has no flag", e.dump());
            continue;
        }
        SettingDef def;
        def.flag = e.at("flag").get<std::string>();
        if (std::ranges::any_of(out, [&](const SettingDef& d) { return d.flag == def.flag; })) {
            LOG_WARNING(Core, "DSMod: \"settings\" lists flag '{}' twice", def.flag);
            continue;
        }
        def.label = e.contains("label") && e.at("label").is_string()
                        ? e.at("label").get<std::string>()
                        : def.flag;
        const std::string type =
            e.contains("type") && e.at("type").is_string() ? e.at("type").get<std::string>()
                                                            : std::string{"toggle"};
        std::vector<std::string> choices;
        if (e.contains("choices") && e.at("choices").is_array()) {
            for (const auto& c : e.at("choices")) {
                choices.push_back(c.is_string() ? c.get<std::string>() : c.dump());
            }
        }
        if (type == "toggle") {
            def.type = SettingDef::Type::Toggle;
            // Two "choices" rename OFF / ON.
            def.choices = choices.size() == 2 ? std::move(choices)
                                              : std::vector<std::string>{"OFF", "ON"};
        } else if (type == "choice") {
            if (choices.size() < 2) {
                LOG_WARNING(Core, "DSMod: setting '{}' needs at least two choices", def.flag);
                continue;
            }
            def.type = SettingDef::Type::Choice;
            def.choices = std::move(choices);
        } else {
            LOG_WARNING(Core, "DSMod: setting '{}' has unknown type '{}'", def.flag, type);
            continue;
        }
        if (e.contains("default")) {
            def.default_value = IntOf(e.at("default")).value_or(0);
        }
        def.default_value =
            std::clamp<s64>(def.default_value, 0, static_cast<s64>(def.choices.size()) - 1);
        out.push_back(std::move(def));
    }
    return out;
}

void ApplySettings(const std::vector<SettingDef>& settings, Manifest& manifest) {
    if (settings.empty() || manifest.pages.empty()) {
        return;
    }
    if (std::ranges::any_of(manifest.pages,
                            [](const Page& p) { return p.id == SettingsPageId; })) {
        LOG_WARNING(Core, "DSMod: the package has its own \"@settings\" page; not generating one");
        return;
    }
    const s32 w = static_cast<s32>(manifest.canvas_w != 0 ? manifest.canvas_w : SettingsDefaultW);
    const s32 h = static_cast<s32>(manifest.canvas_h != 0 ? manifest.canvas_h : SettingsDefaultH);
    const s32 margin = std::max(16, w / 20);
    const s32 title_h = std::max(64, h / 9);
    const s32 title_scale = std::max(4, h / 90);
    const s32 band_h = std::max(4, h / 180);
    const s32 gap = std::max(8, h / 90);

    Page page;
    page.id = std::string{SettingsPageId};
    page.title = "SETTINGS";
    page.no_auto_leave = true; // a page bind never pulls the player out of their settings

    Widget back;
    back.type = WidgetType::Rect;
    back.id = "@settings.bg";
    back.rect = {0, 0, w, h};
    back.bg = SettingsBackground;
    back.color = 0;
    page.widgets.push_back(std::move(back));

    Widget back_button;
    back_button.type = WidgetType::Button;
    back_button.id = "@settings.back";
    back_button.rect = {margin, margin, std::max(160, w / 5), title_h};
    back_button.pill = true;
    back_button.bg = SettingsRow;
    back_button.color = SettingsText;
    back_button.align = 1;
    back_button.text = "< BACK";
    back_button.text_scale = std::max(3, title_h / 20);
    back_button.on_tap = fmt::format("{}back", SettingsActionPrefix);
    page.widgets.push_back(std::move(back_button));

    Widget title;
    title.type = WidgetType::Label;
    title.id = "@settings.title";
    title.rect = {w / 2, margin + (title_h - title_scale * 5) / 2, 0, 0};
    title.align = 1;
    title.text = "SETTINGS";
    title.text_scale = title_scale;
    title.color = SettingsText;
    page.widgets.push_back(std::move(title));

    const s32 band_y = margin + title_h + margin / 2;
    Widget band;
    band.type = WidgetType::Rect;
    band.id = "@settings.band";
    band.rect = {0, band_y, w, band_h};
    band.bg = SettingsAccent;
    band.color = 0;
    page.widgets.push_back(std::move(band));

    // Big rows: up to an eighth of the canvas each, shrunk to fit every entry.
    const s32 rows_y = band_y + band_h + margin / 2;
    const s32 count = static_cast<s32>(settings.size());
    const s32 pitch = std::min(h / 8, std::max(1, (h - margin - rows_y) / count));
    const s32 row_h = std::max(1, pitch - gap);
    const s32 row_scale = std::clamp(h / 150, 1, std::max(1, (row_h - 8) / 5));
    const s32 inset = std::max(0, (row_h - row_scale * 5) / 2);
    for (s32 i = 0; i < count; ++i) {
        const SettingDef& def = settings[static_cast<size_t>(i)];
        const s32 y = rows_y + i * pitch;
        const std::string action = fmt::format("{}{}", SettingsActionPrefix, def.flag);

        Widget row;
        row.type = WidgetType::Button;
        row.id = fmt::format("@settings.row.{}", def.flag);
        row.rect = {margin, y, w - 2 * margin, row_h};
        row.bg = SettingsRow;
        row.color = SettingsText;
        row.text = def.label;
        row.text_scale = row_scale;
        row.text_inset = inset;
        row.on_tap = action;
        page.widgets.push_back(std::move(row));

        Widget value;
        value.type = WidgetType::Value;
        value.id = fmt::format("@settings.value.{}", def.flag);
        value.rect = {w - margin - std::max(inset, 12), y + inset, 0, 0};
        value.align = 2;
        value.bind = "@flag:" + def.flag;
        value.names = def.choices;
        value.text_scale = row_scale;
        value.color = SettingsValue;
        page.widgets.push_back(std::move(value));

        Action next;
        next.kind = ActionKind::Flag;
        next.name = action;
        next.flag = def.flag;
        next.cycle = static_cast<s64>(def.choices.size());
        manifest.actions.insert_or_assign(action, std::move(next));

        // Seed the flag (published as "@flag:<name>", so the row shows a value from the start)
        // and persist it.
        manifest.flag_defaults.try_emplace(def.flag, def.default_value);
        if (std::ranges::find(manifest.persist_flags, def.flag) == manifest.persist_flags.end()) {
            manifest.persist_flags.push_back(def.flag);
        }
    }

    Action leave;
    leave.kind = ActionKind::Page;
    leave.name = fmt::format("{}back", SettingsActionPrefix);
    leave.page = std::string{SettingsBackPageId};
    manifest.actions.insert_or_assign(leave.name, std::move(leave));

    manifest.pages.push_back(std::move(page));
}

std::optional<size_t> ResolvePageTarget(const std::vector<Page>& pages, std::string_view target,
                                        size_t current, size_t& settings_return) {
    if (target == SettingsBackPageId) {
        if (pages.empty()) {
            return std::nullopt;
        }
        return settings_return < pages.size() ? settings_return : 0;
    }
    for (size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].id == target) {
            if (pages[i].id == SettingsPageId && i != current) {
                settings_return = current;
            }
            return i;
        }
    }
    return std::nullopt;
}

} // namespace Core::Mods
