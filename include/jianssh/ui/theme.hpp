#pragma once
// Single source of truth for every colour the interface draws.
//
// The palette implements a macOS-inspired Liquid Glass appearance: translucent
// frosted panels, directional specular glass rims, subtle ambient backdrop
// blooms and luminous system-blue accents. The dark and light presets are
// generated from one field list, so a slot can never exist in one theme and be
// forgotten in the other. Everything the UI paints - ImGui widgets, hand drawn
// rows, vector icons, the terminal and glass surfaces - reads from the same
// palette instance, enabling smooth theme transitions.
#include "jianssh/ui/workspace.hpp"
#include "jianssh/ui/system_theme.hpp"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace jianssh::ui {

// Every palette slot, listed once. X expands to a field name.
#define JIANSSH_THEME_FIELDS(X) \
    X(window_bg) \
    X(child_bg) \
    X(sidebar_bg) \
    X(card_bg) \
    X(card_bg_alt) \
    X(popup_bg) \
    X(border) \
    X(border_shadow) \
    X(shadow) \
    X(separator) \
    X(separator_hovered) \
    X(separator_active) \
    X(text) \
    X(text_disabled) \
    X(text_link) \
    X(text_selected_bg) \
    X(frame_bg) \
    X(frame_hovered) \
    X(frame_active) \
    X(title_bg) \
    X(title_bg_active) \
    X(title_bg_collapsed) \
    X(menu_bar_bg) \
    X(scrollbar_bg) \
    X(scrollbar_grab) \
    X(scrollbar_grab_hovered) \
    X(scrollbar_grab_active) \
    X(check_mark) \
    X(slider_grab) \
    X(slider_grab_active) \
    X(button) \
    X(button_hovered) \
    X(button_active) \
    X(header) \
    X(header_hovered) \
    X(header_active) \
    X(tab) \
    X(tab_hovered) \
    X(tab_selected) \
    X(tab_selected_overline) \
    X(tab_dimmed) \
    X(tab_dimmed_selected) \
    X(tab_dimmed_selected_overline) \
    X(resize_grip) \
    X(resize_grip_hovered) \
    X(resize_grip_active) \
    X(plot_lines) \
    X(plot_lines_hovered) \
    X(plot_histogram) \
    X(plot_histogram_hovered) \
    X(table_header_bg) \
    X(table_border_strong) \
    X(table_border_light) \
    X(table_row_bg) \
    X(table_row_bg_alt) \
    X(drag_drop_target) \
    X(nav_cursor) \
    X(nav_windowing_highlight) \
    X(nav_windowing_dim_bg) \
    X(modal_dim_bg) \
    X(ink) \
    X(muted) \
    X(accent) \
    X(accent_hover) \
    X(accent_active) \
    X(accent_soft) \
    X(surface) \
    X(danger) \
    X(warning) \
    X(success) \
    X(info) \
    X(terminal_bg) \
    X(terminal_fg) \
    X(terminal_cursor) \
    X(chart_bg) \
    X(chart_line) \
    X(chart_grid) \
    X(glass_bg) \
    X(glass_bg_strong) \
    X(glass_bg_subtle) \
    X(glass_highlight) \
    X(glass_rim_top) \
    X(glass_rim_bottom) \
    X(glass_glow_a) \
    X(glass_glow_b) \
    X(glass_glow_c) \
    X(glass_glow_d) \
    X(folder_back) \
    X(folder_front_top) \
    X(folder_front_bottom)

struct Theme {
#define JIANSSH_THEME_FIELD(name) ImU32 name = 0;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_FIELD)
#undef JIANSSH_THEME_FIELD
    std::array<ImU32, 16> ansi{};
    bool dark = true;
};

inline bool operator==(const Theme& a, const Theme& b) noexcept {
#define JIANSSH_THEME_EQ(name) \
    if (a.name != b.name) return false;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_EQ)
#undef JIANSSH_THEME_EQ
    return a.ansi == b.ansi && a.dark == b.dark;
}
inline bool operator!=(const Theme& a, const Theme& b) noexcept { return !(a == b); }

// Per channel interpolation; alpha is interpolated as well so a theme switch
// never pops a hard edge.
inline ImU32 lerp_color(ImU32 a, ImU32 b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto mix = [t](unsigned ca, unsigned cb) {
        return static_cast<unsigned>(
            std::lround(static_cast<double>(ca) + (static_cast<double>(cb) - static_cast<double>(ca)) * t));
    };
    const unsigned a_r = a & 0xFFu, a_g = (a >> 8) & 0xFFu, a_b = (a >> 16) & 0xFFu, a_a = (a >> 24) & 0xFFu;
    const unsigned b_r = b & 0xFFu, b_g = (b >> 8) & 0xFFu, b_b = (b >> 16) & 0xFFu, b_a = (b >> 24) & 0xFFu;
    return IM_COL32(mix(a_r, b_r), mix(a_g, b_g), mix(a_b, b_b), mix(a_a, b_a));
}

// Scale the alpha channel; used by view transitions and the notice fade.
inline ImU32 fade_color(ImU32 color, float factor) noexcept {
    factor = std::clamp(factor, 0.0f, 1.0f);
    const unsigned alpha = (color >> 24) & 0xFFu;
    const unsigned faded = static_cast<unsigned>(std::lround(alpha * factor));
    return (color & 0x00FFFFFFu) | (faded << 24);
}

inline Theme lerp_theme(const Theme& a, const Theme& b, float t) noexcept {
    Theme out;
#define JIANSSH_THEME_LERP(name) out.name = lerp_color(a.name, b.name, t);
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_LERP)
#undef JIANSSH_THEME_LERP
    for (std::size_t i = 0; i < out.ansi.size(); ++i) out.ansi[i] = lerp_color(a.ansi[i], b.ansi[i], t);
    out.dark = t < 0.5f ? a.dark : b.dark;
    return out;
}

// Apple Liquid Glass dark appearance: deep cosmic-indigo canvas with multi-stop
// jewel-toned fluid aurora ribbons, ultra-translucent crystal-smoke floating
// islands, dual-corner Fresnel specular rims and WCAG AAA crisp typography.
inline Theme dark_theme() {
    Theme t;
#define JIANSSH_THEME_SET(name) t.name = 0;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_SET)
#undef JIANSSH_THEME_SET
    t.window_bg = IM_COL32(7, 11, 22, 255);
    t.child_bg = IM_COL32(14, 21, 38, 108);
    t.sidebar_bg = IM_COL32(16, 24, 44, 96);
    t.card_bg = IM_COL32(230, 242, 255, 18);
    t.card_bg_alt = IM_COL32(230, 242, 255, 32);
    t.popup_bg = IM_COL32(16, 24, 44, 198);
    t.border = IM_COL32(226, 240, 255, 68);
    t.border_shadow = IM_COL32(0, 4, 14, 110);
    t.shadow = IM_COL32(0, 4, 16, 118);
    t.separator = IM_COL32(210, 232, 255, 48);
    t.separator_hovered = IM_COL32(64, 172, 255, 255);
    t.separator_active = IM_COL32(10, 132, 255, 255);
    t.text = IM_COL32(250, 253, 255, 255);
    t.text_disabled = IM_COL32(138, 156, 186, 215);
    t.text_link = IM_COL32(88, 182, 255, 255);
    t.text_selected_bg = IM_COL32(10, 132, 255, 112);
    t.frame_bg = IM_COL32(220, 238, 255, 22);
    t.frame_hovered = IM_COL32(232, 244, 255, 40);
    t.frame_active = IM_COL32(240, 248, 255, 58);
    t.title_bg = IM_COL32(16, 24, 44, 198);
    t.title_bg_active = IM_COL32(20, 30, 54, 206);
    t.title_bg_collapsed = IM_COL32(16, 24, 44, 184);
    t.menu_bar_bg = IM_COL32(16, 24, 44, 190);
    t.scrollbar_bg = IM_COL32(0, 0, 0, 0);
    t.scrollbar_grab = IM_COL32(232, 244, 255, 68);
    t.scrollbar_grab_hovered = IM_COL32(240, 248, 255, 124);
    t.scrollbar_grab_active = IM_COL32(248, 252, 255, 176);
    t.check_mark = IM_COL32(82, 184, 255, 255);
    t.slider_grab = IM_COL32(42, 158, 255, 255);
    t.slider_grab_active = IM_COL32(10, 132, 255, 255);
    t.button = IM_COL32(226, 240, 255, 30);
    t.button_hovered = IM_COL32(238, 246, 255, 56);
    t.button_active = IM_COL32(245, 250, 255, 80);
    t.header = IM_COL32(42, 156, 255, 68);
    t.header_hovered = IM_COL32(232, 244, 255, 28);
    t.header_active = IM_COL32(42, 156, 255, 102);
    t.tab = IM_COL32(255, 255, 255, 0);
    t.tab_hovered = IM_COL32(232, 244, 255, 26);
    t.tab_selected = IM_COL32(226, 240, 255, 48);
    t.tab_selected_overline = IM_COL32(82, 184, 255, 255);
    t.tab_dimmed = IM_COL32(255, 255, 255, 0);
    t.tab_dimmed_selected = IM_COL32(226, 240, 255, 32);
    t.tab_dimmed_selected_overline = IM_COL32(82, 184, 255, 148);
    t.resize_grip = IM_COL32(226, 240, 255, 42);
    t.resize_grip_hovered = IM_COL32(42, 158, 255, 230);
    t.resize_grip_active = IM_COL32(10, 132, 255, 255);
    t.plot_lines = IM_COL32(226, 240, 255, 56);
    t.plot_lines_hovered = IM_COL32(42, 158, 255, 255);
    t.plot_histogram = IM_COL32(42, 158, 255, 232);
    t.plot_histogram_hovered = IM_COL32(98, 192, 255, 255);
    t.table_header_bg = IM_COL32(226, 240, 255, 16);
    t.table_border_strong = IM_COL32(226, 240, 255, 38);
    t.table_border_light = IM_COL32(226, 240, 255, 16);
    t.table_row_bg = IM_COL32(255, 255, 255, 0);
    t.table_row_bg_alt = IM_COL32(255, 255, 255, 0);
    t.drag_drop_target = IM_COL32(64, 178, 255, 228);
    t.nav_cursor = IM_COL32(64, 178, 255, 255);
    t.nav_windowing_highlight = IM_COL32(64, 178, 255, 220);
    t.nav_windowing_dim_bg = IM_COL32(3, 6, 15, 138);
    t.modal_dim_bg = IM_COL32(3, 6, 15, 142);
    t.ink = IM_COL32(250, 253, 255, 255);
    t.muted = IM_COL32(190, 206, 232, 238);
    t.accent = IM_COL32(10, 132, 255, 255);
    t.accent_hover = IM_COL32(72, 172, 255, 255);
    t.accent_active = IM_COL32(0, 104, 222, 255);
    t.accent_soft = IM_COL32(10, 132, 255, 58);
    t.surface = IM_COL32(226, 240, 255, 36);
    t.danger = IM_COL32(255, 76, 88, 255);
    t.warning = IM_COL32(255, 192, 56, 255);
    t.success = IM_COL32(48, 228, 128, 255);
    t.info = IM_COL32(96, 224, 255, 255);
    t.terminal_bg = IM_COL32(8, 12, 22, 192);
    t.terminal_fg = IM_COL32(244, 248, 255, 255);
    t.terminal_cursor = IM_COL32(72, 182, 255, 255);
    t.chart_bg = IM_COL32(12, 18, 34, 140);
    t.chart_line = IM_COL32(72, 182, 255, 255);
    t.chart_grid = IM_COL32(220, 238, 255, 32);
    t.glass_bg = IM_COL32(18, 28, 50, 102);
    t.glass_bg_strong = IM_COL32(226, 240, 255, 48);
    t.glass_bg_subtle = IM_COL32(7, 12, 24, 88);
    t.glass_highlight = IM_COL32(246, 251, 255, 44);
    t.glass_rim_top = IM_COL32(250, 253, 255, 182);
    t.glass_rim_bottom = IM_COL32(122, 198, 255, 96);
    t.glass_glow_a = IM_COL32(18, 128, 255, 148);
    t.glass_glow_b = IM_COL32(142, 72, 255, 126);
    t.glass_glow_c = IM_COL32(0, 212, 238, 114);
    t.glass_glow_d = IM_COL32(236, 68, 168, 86);
    t.folder_back = IM_COL32(16, 128, 242, 238);
    t.folder_front_top = IM_COL32(126, 220, 255, 238);
    t.folder_front_bottom = IM_COL32(28, 148, 255, 232);
    t.ansi = {IM_COL32(24, 30, 44, 255),   IM_COL32(255, 102, 108, 255), IM_COL32(68, 232, 136, 255),
              IM_COL32(255, 208, 96, 255), IM_COL32(92, 178, 255, 255),  IM_COL32(208, 142, 255, 255),
              IM_COL32(82, 232, 245, 255), IM_COL32(242, 246, 255, 255), IM_COL32(130, 144, 170, 255),
              IM_COL32(255, 136, 140, 255), IM_COL32(118, 245, 168, 255), IM_COL32(255, 224, 134, 255),
              IM_COL32(138, 202, 255, 255), IM_COL32(226, 176, 255, 255), IM_COL32(132, 244, 252, 255),
              IM_COL32(255, 255, 255, 255)};
    t.dark = true;
    return t;
}

// Apple Liquid Glass daylight appearance: luminous pearl-sky fluid mesh with
// cerulean, lavender, aqua and warm rose blooms refracting visibly through
// airy milk-crystal floating islands and pure white specular rims.
inline Theme light_theme() {
    Theme t;
#define JIANSSH_THEME_SET(name) t.name = 0;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_SET)
#undef JIANSSH_THEME_SET
    t.window_bg = IM_COL32(224, 235, 252, 255);
    t.child_bg = IM_COL32(255, 255, 255, 126);
    t.sidebar_bg = IM_COL32(255, 255, 255, 114);
    t.card_bg = IM_COL32(255, 255, 255, 118);
    t.card_bg_alt = IM_COL32(255, 255, 255, 168);
    t.popup_bg = IM_COL32(249, 252, 255, 232);
    t.border = IM_COL32(52, 86, 146, 58);
    t.border_shadow = IM_COL32(18, 38, 78, 28);
    t.shadow = IM_COL32(20, 44, 92, 34);
    t.separator = IM_COL32(52, 86, 146, 52);
    t.separator_hovered = IM_COL32(0, 112, 245, 255);
    t.separator_active = IM_COL32(0, 88, 204, 255);
    t.text = IM_COL32(12, 20, 38, 255);
    t.text_disabled = IM_COL32(100, 116, 142, 220);
    t.text_link = IM_COL32(0, 112, 245, 255);
    t.text_selected_bg = IM_COL32(0, 112, 245, 68);
    t.frame_bg = IM_COL32(255, 255, 255, 118);
    t.frame_hovered = IM_COL32(255, 255, 255, 168);
    t.frame_active = IM_COL32(255, 255, 255, 224);
    t.title_bg = IM_COL32(248, 252, 255, 212);
    t.title_bg_active = IM_COL32(250, 253, 255, 220);
    t.title_bg_collapsed = IM_COL32(246, 250, 255, 198);
    t.menu_bar_bg = IM_COL32(248, 252, 255, 208);
    t.scrollbar_bg = IM_COL32(0, 0, 0, 0);
    t.scrollbar_grab = IM_COL32(38, 64, 112, 72);
    t.scrollbar_grab_hovered = IM_COL32(38, 64, 112, 122);
    t.scrollbar_grab_active = IM_COL32(38, 64, 112, 168);
    t.check_mark = IM_COL32(0, 112, 245, 255);
    t.slider_grab = IM_COL32(0, 112, 245, 255);
    t.slider_grab_active = IM_COL32(0, 88, 204, 255);
    t.button = IM_COL32(255, 255, 255, 146);
    t.button_hovered = IM_COL32(255, 255, 255, 204);
    t.button_active = IM_COL32(236, 245, 255, 232);
    t.header = IM_COL32(0, 112, 245, 48);
    t.header_hovered = IM_COL32(255, 255, 255, 110);
    t.header_active = IM_COL32(0, 112, 245, 76);
    t.tab = IM_COL32(0, 0, 0, 0);
    t.tab_hovered = IM_COL32(255, 255, 255, 128);
    t.tab_selected = IM_COL32(255, 255, 255, 218);
    t.tab_selected_overline = IM_COL32(0, 112, 245, 255);
    t.tab_dimmed = IM_COL32(0, 0, 0, 0);
    t.tab_dimmed_selected = IM_COL32(255, 255, 255, 182);
    t.tab_dimmed_selected_overline = IM_COL32(0, 112, 245, 152);
    t.resize_grip = IM_COL32(38, 64, 112, 48);
    t.resize_grip_hovered = IM_COL32(0, 112, 245, 220);
    t.resize_grip_active = IM_COL32(0, 88, 204, 255);
    t.plot_lines = IM_COL32(42, 68, 112, 66);
    t.plot_lines_hovered = IM_COL32(0, 112, 245, 255);
    t.plot_histogram = IM_COL32(0, 112, 245, 225);
    t.plot_histogram_hovered = IM_COL32(38, 138, 255, 255);
    t.table_header_bg = IM_COL32(255, 255, 255, 112);
    t.table_border_strong = IM_COL32(52, 86, 146, 48);
    t.table_border_light = IM_COL32(52, 86, 146, 22);
    t.table_row_bg = IM_COL32(255, 255, 255, 0);
    t.table_row_bg_alt = IM_COL32(255, 255, 255, 0);
    t.drag_drop_target = IM_COL32(0, 112, 245, 218);
    t.nav_cursor = IM_COL32(0, 112, 245, 255);
    t.nav_windowing_highlight = IM_COL32(0, 112, 245, 210);
    t.nav_windowing_dim_bg = IM_COL32(18, 34, 68, 108);
    t.modal_dim_bg = IM_COL32(18, 34, 68, 116);
    t.ink = IM_COL32(12, 20, 38, 255);
    t.muted = IM_COL32(62, 78, 108, 236);
    t.accent = IM_COL32(0, 112, 245, 255);
    t.accent_hover = IM_COL32(42, 138, 255, 255);
    t.accent_active = IM_COL32(0, 88, 204, 255);
    t.accent_soft = IM_COL32(0, 112, 245, 42);
    t.surface = IM_COL32(255, 255, 255, 198);
    t.danger = IM_COL32(224, 38, 48, 255);
    t.warning = IM_COL32(198, 108, 0, 255);
    t.success = IM_COL32(12, 158, 78, 255);
    t.info = IM_COL32(2, 132, 199, 255);
    t.terminal_bg = IM_COL32(250, 253, 255, 214);
    t.terminal_fg = IM_COL32(12, 20, 38, 255);
    t.terminal_cursor = IM_COL32(0, 112, 245, 255);
    t.chart_bg = IM_COL32(255, 255, 255, 152);
    t.chart_line = IM_COL32(0, 112, 245, 255);
    t.chart_grid = IM_COL32(52, 86, 146, 36);
    t.glass_bg = IM_COL32(255, 255, 255, 132);
    t.glass_bg_strong = IM_COL32(255, 255, 255, 212);
    t.glass_bg_subtle = IM_COL32(204, 222, 248, 108);
    t.glass_highlight = IM_COL32(255, 255, 255, 178);
    t.glass_rim_top = IM_COL32(255, 255, 255, 252);
    t.glass_rim_bottom = IM_COL32(118, 158, 222, 118);
    t.glass_glow_a = IM_COL32(56, 152, 255, 142);
    t.glass_glow_b = IM_COL32(158, 110, 255, 122);
    t.glass_glow_c = IM_COL32(20, 212, 232, 114);
    t.glass_glow_d = IM_COL32(255, 122, 174, 96);
    t.folder_back = IM_COL32(14, 112, 232, 242);
    t.folder_front_top = IM_COL32(96, 192, 255, 244);
    t.folder_front_bottom = IM_COL32(20, 126, 248, 238);
    t.ansi = {IM_COL32(15, 23, 42, 255),   IM_COL32(198, 36, 36, 255),  IM_COL32(16, 126, 56, 255),
              IM_COL32(162, 94, 0, 255),   IM_COL32(12, 86, 198, 255), IM_COL32(124, 58, 186, 255),
              IM_COL32(8, 116, 134, 255),  IM_COL32(72, 84, 104, 255), IM_COL32(100, 116, 139, 255),
              IM_COL32(226, 52, 52, 255),  IM_COL32(28, 152, 72, 255), IM_COL32(196, 120, 0, 255),
              IM_COL32(30, 112, 226, 255), IM_COL32(148, 78, 212, 255), IM_COL32(14, 144, 164, 255),
              IM_COL32(15, 23, 42, 255)};
    t.dark = false;
    return t;
}

// Resolve the palette for a preference. In System mode the desktop decides;
// when the desktop publishes no preference the dark identity is kept, so the
// product never flips to light by accident.
inline Theme theme_for(app::ThemeMode mode, SystemTheme system) {
    const bool dark = mode == app::ThemeMode::Dark ? true
                      : mode == app::ThemeMode::Light ? false
                                                      : system != SystemTheme::Light;
    return dark ? dark_theme() : light_theme();
}

inline const char* theme_mode_label(app::ThemeMode mode) {
    switch (mode) {
        case app::ThemeMode::Dark: return "深色";
        case app::ThemeMode::Light: return "浅色";
        case app::ThemeMode::System: break;
    }
    return "跟随系统";
}

// Number of ImGui colour slots the theme owns. push_theme/pop_theme must stay
// in sync with the table below.
inline constexpr int theme_color_count = 56;

inline void push_theme(const Theme& t) {
    const std::pair<ImGuiCol, ImU32> colors[theme_color_count] = {
        {ImGuiCol_Text, t.text},
        {ImGuiCol_TextDisabled, t.text_disabled},
        {ImGuiCol_WindowBg, t.window_bg},
        {ImGuiCol_ChildBg, t.child_bg},
        {ImGuiCol_PopupBg, t.popup_bg},
        {ImGuiCol_Border, t.border},
        {ImGuiCol_BorderShadow, t.border_shadow},
        {ImGuiCol_FrameBg, t.frame_bg},
        {ImGuiCol_FrameBgHovered, t.frame_hovered},
        {ImGuiCol_FrameBgActive, t.frame_active},
        {ImGuiCol_TitleBg, t.title_bg},
        {ImGuiCol_TitleBgActive, t.title_bg_active},
        {ImGuiCol_TitleBgCollapsed, t.title_bg_collapsed},
        {ImGuiCol_MenuBarBg, t.menu_bar_bg},
        {ImGuiCol_ScrollbarBg, t.scrollbar_bg},
        {ImGuiCol_ScrollbarGrab, t.scrollbar_grab},
        {ImGuiCol_ScrollbarGrabHovered, t.scrollbar_grab_hovered},
        {ImGuiCol_ScrollbarGrabActive, t.scrollbar_grab_active},
        {ImGuiCol_CheckMark, t.check_mark},
        {ImGuiCol_SliderGrab, t.slider_grab},
        {ImGuiCol_SliderGrabActive, t.slider_grab_active},
        {ImGuiCol_Button, t.button},
        {ImGuiCol_ButtonHovered, t.button_hovered},
        {ImGuiCol_ButtonActive, t.button_active},
        {ImGuiCol_Header, t.header},
        {ImGuiCol_HeaderHovered, t.header_hovered},
        {ImGuiCol_HeaderActive, t.header_active},
        {ImGuiCol_Separator, t.separator},
        {ImGuiCol_SeparatorHovered, t.separator_hovered},
        {ImGuiCol_SeparatorActive, t.separator_active},
        {ImGuiCol_ResizeGrip, t.resize_grip},
        {ImGuiCol_ResizeGripHovered, t.resize_grip_hovered},
        {ImGuiCol_ResizeGripActive, t.resize_grip_active},
        {ImGuiCol_Tab, t.tab},
        {ImGuiCol_TabHovered, t.tab_hovered},
        {ImGuiCol_TabSelected, t.tab_selected},
        {ImGuiCol_TabSelectedOverline, t.tab_selected_overline},
        {ImGuiCol_TabDimmed, t.tab_dimmed},
        {ImGuiCol_TabDimmedSelected, t.tab_dimmed_selected},
        {ImGuiCol_TabDimmedSelectedOverline, t.tab_dimmed_selected_overline},
        {ImGuiCol_PlotLines, t.plot_lines},
        {ImGuiCol_PlotLinesHovered, t.plot_lines_hovered},
        {ImGuiCol_PlotHistogram, t.plot_histogram},
        {ImGuiCol_PlotHistogramHovered, t.plot_histogram_hovered},
        {ImGuiCol_TableHeaderBg, t.table_header_bg},
        {ImGuiCol_TableBorderStrong, t.table_border_strong},
        {ImGuiCol_TableBorderLight, t.table_border_light},
        {ImGuiCol_TableRowBg, t.table_row_bg},
        {ImGuiCol_TableRowBgAlt, t.table_row_bg_alt},
        {ImGuiCol_TextLink, t.text_link},
        {ImGuiCol_TextSelectedBg, t.text_selected_bg},
        {ImGuiCol_DragDropTarget, t.drag_drop_target},
        {ImGuiCol_NavCursor, t.nav_cursor},
        {ImGuiCol_NavWindowingHighlight, t.nav_windowing_highlight},
        {ImGuiCol_NavWindowingDimBg, t.nav_windowing_dim_bg},
        {ImGuiCol_ModalWindowDimBg, t.modal_dim_bg},
    };
    for (const auto& entry : colors)
        ImGui::PushStyleColor(entry.first, ImGui::ColorConvertU32ToFloat4(entry.second));
}

inline void pop_theme() { ImGui::PopStyleColor(theme_color_count); }

// Relative luminance, used by tests and by the contrast guard below.
inline double relative_luminance(ImU32 color) {
    const auto channel = [](unsigned raw) {
        const double v = static_cast<double>(raw) / 255.0;
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    const double r = channel(color & 0xFFu);
    const double g = channel((color >> 8) & 0xFFu);
    const double b = channel((color >> 16) & 0xFFu);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

inline double contrast_ratio(ImU32 a, ImU32 b) {
    const double la = relative_luminance(a);
    const double lb = relative_luminance(b);
    const double hi = std::max(la, lb);
    const double lo = std::min(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}
} // namespace jianssh::ui
