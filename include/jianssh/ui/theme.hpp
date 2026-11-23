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

// High-contrast Liquid Glass dark appearance: deep obsidian-midnight canvas
// with luminous aurora blooms, frosted high-contrast glass panels, crisp
// specular rims and WCAG AAA text legibility.
inline Theme dark_theme() {
    Theme t;
#define JIANSSH_THEME_SET(name) t.name = 0;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_SET)
#undef JIANSSH_THEME_SET
    t.window_bg = IM_COL32(9, 13, 22, 255);
    t.child_bg = IM_COL32(18, 24, 38, 206);
    t.sidebar_bg = IM_COL32(20, 27, 42, 198);
    t.card_bg = IM_COL32(26, 35, 54, 196);
    t.card_bg_alt = IM_COL32(42, 54, 80, 220);
    t.popup_bg = IM_COL32(19, 26, 42, 244);
    t.border = IM_COL32(212, 232, 255, 78);
    t.border_shadow = IM_COL32(0, 4, 12, 145);
    t.shadow = IM_COL32(0, 4, 14, 124);
    t.separator = IM_COL32(186, 214, 255, 84);
    t.separator_hovered = IM_COL32(56, 168, 255, 255);
    t.separator_active = IM_COL32(20, 126, 242, 255);
    t.text = IM_COL32(250, 252, 255, 255);
    t.text_disabled = IM_COL32(142, 157, 184, 255);
    t.text_link = IM_COL32(98, 186, 255, 255);
    t.text_selected_bg = IM_COL32(30, 150, 255, 118);
    t.frame_bg = IM_COL32(176, 204, 248, 44);
    t.frame_hovered = IM_COL32(196, 220, 255, 68);
    t.frame_active = IM_COL32(210, 232, 255, 92);
    t.title_bg = IM_COL32(24, 33, 52, 248);
    t.title_bg_active = IM_COL32(28, 39, 62, 250);
    t.title_bg_collapsed = IM_COL32(20, 27, 42, 240);
    t.menu_bar_bg = IM_COL32(20, 27, 42, 235);
    t.scrollbar_bg = IM_COL32(0, 0, 0, 0);
    t.scrollbar_grab = IM_COL32(226, 240, 255, 74);
    t.scrollbar_grab_hovered = IM_COL32(232, 244, 255, 128);
    t.scrollbar_grab_active = IM_COL32(242, 250, 255, 172);
    t.check_mark = IM_COL32(64, 178, 255, 255);
    t.slider_grab = IM_COL32(46, 164, 255, 255);
    t.slider_grab_active = IM_COL32(20, 126, 242, 255);
    t.button = IM_COL32(190, 214, 255, 52);
    t.button_hovered = IM_COL32(210, 230, 255, 84);
    t.button_active = IM_COL32(222, 238, 255, 110);
    t.header = IM_COL32(30, 150, 255, 84);
    t.header_hovered = IM_COL32(225, 240, 255, 36);
    t.header_active = IM_COL32(30, 150, 255, 118);
    t.tab = IM_COL32(255, 255, 255, 0);
    t.tab_hovered = IM_COL32(225, 240, 255, 30);
    t.tab_selected = IM_COL32(68, 92, 134, 186);
    t.tab_selected_overline = IM_COL32(64, 178, 255, 255);
    t.tab_dimmed = IM_COL32(255, 255, 255, 0);
    t.tab_dimmed_selected = IM_COL32(48, 64, 94, 158);
    t.tab_dimmed_selected_overline = IM_COL32(64, 178, 255, 150);
    t.resize_grip = IM_COL32(220, 236, 255, 48);
    t.resize_grip_hovered = IM_COL32(46, 164, 255, 235);
    t.resize_grip_active = IM_COL32(46, 164, 255, 255);
    t.plot_lines = IM_COL32(220, 236, 255, 58);
    t.plot_lines_hovered = IM_COL32(46, 164, 255, 255);
    t.plot_histogram = IM_COL32(46, 164, 255, 232);
    t.plot_histogram_hovered = IM_COL32(94, 188, 255, 255);
    t.table_header_bg = IM_COL32(198, 222, 255, 32);
    t.table_border_strong = IM_COL32(204, 228, 255, 62);
    t.table_border_light = IM_COL32(204, 228, 255, 28);
    t.table_row_bg = IM_COL32(255, 255, 255, 0);
    t.table_row_bg_alt = IM_COL32(210, 232, 255, 12);
    t.drag_drop_target = IM_COL32(58, 174, 255, 225);
    t.nav_cursor = IM_COL32(64, 178, 255, 255);
    t.nav_windowing_highlight = IM_COL32(64, 178, 255, 220);
    t.nav_windowing_dim_bg = IM_COL32(4, 8, 16, 152);
    t.modal_dim_bg = IM_COL32(4, 8, 16, 164);
    t.ink = IM_COL32(250, 252, 255, 255);
    t.muted = IM_COL32(186, 198, 220, 255);
    t.accent = IM_COL32(30, 150, 255, 255);
    t.accent_hover = IM_COL32(84, 180, 255, 255);
    t.accent_active = IM_COL32(14, 114, 224, 255);
    t.accent_soft = IM_COL32(30, 150, 255, 62);
    t.surface = IM_COL32(42, 54, 80, 224);
    t.danger = IM_COL32(255, 82, 82, 255);
    t.warning = IM_COL32(255, 184, 48, 255);
    t.success = IM_COL32(48, 232, 122, 255);
    t.info = IM_COL32(92, 225, 255, 255);
    t.terminal_bg = IM_COL32(11, 15, 25, 246);
    t.terminal_fg = IM_COL32(242, 246, 255, 255);
    t.terminal_cursor = IM_COL32(64, 178, 255, 255);
    t.chart_bg = IM_COL32(18, 24, 38, 215);
    t.chart_line = IM_COL32(64, 178, 255, 255);
    t.chart_grid = IM_COL32(204, 228, 255, 40);
    t.glass_bg = IM_COL32(26, 35, 54, 192);
    t.glass_bg_strong = IM_COL32(48, 63, 94, 220);
    t.glass_bg_subtle = IM_COL32(12, 18, 30, 168);
    t.glass_highlight = IM_COL32(236, 246, 255, 58);
    t.glass_rim_top = IM_COL32(244, 250, 255, 148);
    t.glass_rim_bottom = IM_COL32(116, 172, 255, 62);
    t.glass_glow_a = IM_COL32(30, 148, 255, 92);
    t.glass_glow_b = IM_COL32(142, 92, 255, 76);
    t.glass_glow_c = IM_COL32(24, 218, 240, 62);
    t.folder_back = IM_COL32(26, 123, 232, 255);
    t.folder_front_top = IM_COL32(104, 206, 255, 255);
    t.folder_front_bottom = IM_COL32(27, 132, 255, 255);
    t.ansi = {IM_COL32(24, 30, 44, 255),   IM_COL32(255, 102, 108, 255), IM_COL32(68, 232, 136, 255),
              IM_COL32(255, 208, 96, 255), IM_COL32(92, 178, 255, 255),  IM_COL32(208, 142, 255, 255),
              IM_COL32(82, 232, 245, 255), IM_COL32(242, 246, 255, 255), IM_COL32(130, 144, 170, 255),
              IM_COL32(255, 136, 140, 255), IM_COL32(118, 245, 168, 255), IM_COL32(255, 224, 134, 255),
              IM_COL32(138, 202, 255, 255), IM_COL32(226, 176, 255, 255), IM_COL32(132, 244, 252, 255),
              IM_COL32(255, 255, 255, 255)};
    t.dark = true;
    return t;
}

// High-contrast Liquid Glass light appearance: luminous porcelain-glacier sky
// backdrop with cerulean and violet glows, frosted crystal white cards, crisp
// slate-rimmed controls and deep obsidian ink typography.
inline Theme light_theme() {
    Theme t;
#define JIANSSH_THEME_SET(name) t.name = 0;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_SET)
#undef JIANSSH_THEME_SET
    t.window_bg = IM_COL32(214, 226, 245, 255);
    t.child_bg = IM_COL32(252, 254, 255, 208);
    t.sidebar_bg = IM_COL32(246, 250, 255, 200);
    t.card_bg = IM_COL32(255, 255, 255, 202);
    t.card_bg_alt = IM_COL32(238, 246, 255, 232);
    t.popup_bg = IM_COL32(252, 254, 255, 246);
    t.border = IM_COL32(38, 66, 114, 82);
    t.border_shadow = IM_COL32(18, 36, 72, 42);
    t.shadow = IM_COL32(20, 40, 82, 44);
    t.separator = IM_COL32(42, 72, 122, 78);
    t.separator_hovered = IM_COL32(0, 98, 230, 255);
    t.separator_active = IM_COL32(0, 75, 184, 255);
    t.text = IM_COL32(15, 23, 42, 255);
    t.text_disabled = IM_COL32(100, 116, 139, 255);
    t.text_link = IM_COL32(0, 98, 230, 255);
    t.text_selected_bg = IM_COL32(0, 98, 230, 72);
    t.frame_bg = IM_COL32(204, 219, 242, 154);
    t.frame_hovered = IM_COL32(218, 232, 252, 192);
    t.frame_active = IM_COL32(255, 255, 255, 248);
    t.title_bg = IM_COL32(238, 245, 255, 246);
    t.title_bg_active = IM_COL32(244, 249, 255, 250);
    t.title_bg_collapsed = IM_COL32(236, 244, 255, 238);
    t.menu_bar_bg = IM_COL32(240, 246, 255, 236);
    t.scrollbar_bg = IM_COL32(0, 0, 0, 0);
    t.scrollbar_grab = IM_COL32(34, 58, 102, 78);
    t.scrollbar_grab_hovered = IM_COL32(34, 58, 102, 126);
    t.scrollbar_grab_active = IM_COL32(34, 58, 102, 168);
    t.check_mark = IM_COL32(0, 98, 230, 255);
    t.slider_grab = IM_COL32(0, 98, 230, 255);
    t.slider_grab_active = IM_COL32(0, 75, 184, 255);
    t.button = IM_COL32(226, 237, 254, 218);
    t.button_hovered = IM_COL32(208, 226, 254, 240);
    t.button_active = IM_COL32(184, 212, 252, 250);
    t.header = IM_COL32(0, 98, 230, 56);
    t.header_hovered = IM_COL32(22, 62, 130, 26);
    t.header_active = IM_COL32(0, 98, 230, 82);
    t.tab = IM_COL32(0, 0, 0, 0);
    t.tab_hovered = IM_COL32(255, 255, 255, 155);
    t.tab_selected = IM_COL32(255, 255, 255, 244);
    t.tab_selected_overline = IM_COL32(0, 98, 230, 255);
    t.tab_dimmed = IM_COL32(0, 0, 0, 0);
    t.tab_dimmed_selected = IM_COL32(255, 255, 255, 215);
    t.tab_dimmed_selected_overline = IM_COL32(0, 98, 230, 155);
    t.resize_grip = IM_COL32(34, 58, 102, 54);
    t.resize_grip_hovered = IM_COL32(0, 98, 230, 220);
    t.resize_grip_active = IM_COL32(0, 75, 184, 255);
    t.plot_lines = IM_COL32(42, 68, 112, 66);
    t.plot_lines_hovered = IM_COL32(0, 98, 230, 255);
    t.plot_histogram = IM_COL32(0, 98, 230, 225);
    t.plot_histogram_hovered = IM_COL32(30, 126, 255, 255);
    t.table_header_bg = IM_COL32(212, 226, 248, 168);
    t.table_border_strong = IM_COL32(42, 72, 122, 72);
    t.table_border_light = IM_COL32(42, 72, 122, 36);
    t.table_row_bg = IM_COL32(255, 255, 255, 0);
    t.table_row_bg_alt = IM_COL32(26, 62, 126, 13);
    t.drag_drop_target = IM_COL32(0, 98, 230, 215);
    t.nav_cursor = IM_COL32(0, 98, 230, 255);
    t.nav_windowing_highlight = IM_COL32(0, 98, 230, 210);
    t.nav_windowing_dim_bg = IM_COL32(15, 28, 54, 116);
    t.modal_dim_bg = IM_COL32(15, 28, 54, 118);
    t.ink = IM_COL32(15, 23, 42, 255);
    t.muted = IM_COL32(66, 80, 107, 255);
    t.accent = IM_COL32(0, 98, 230, 255);
    t.accent_hover = IM_COL32(30, 126, 255, 255);
    t.accent_active = IM_COL32(0, 75, 184, 255);
    t.accent_soft = IM_COL32(0, 98, 230, 48);
    t.surface = IM_COL32(255, 255, 255, 242);
    t.danger = IM_COL32(220, 38, 38, 255);
    t.warning = IM_COL32(200, 109, 0, 255);
    t.success = IM_COL32(14, 159, 78, 255);
    t.info = IM_COL32(2, 132, 199, 255);
    t.terminal_bg = IM_COL32(250, 252, 255, 246);
    t.terminal_fg = IM_COL32(15, 23, 42, 255);
    t.terminal_cursor = IM_COL32(0, 98, 230, 255);
    t.chart_bg = IM_COL32(255, 255, 255, 224);
    t.chart_line = IM_COL32(0, 98, 230, 255);
    t.chart_grid = IM_COL32(42, 72, 122, 44);
    t.glass_bg = IM_COL32(252, 254, 255, 202);
    t.glass_bg_strong = IM_COL32(255, 255, 255, 244);
    t.glass_bg_subtle = IM_COL32(202, 218, 242, 162);
    t.glass_highlight = IM_COL32(255, 255, 255, 196);
    t.glass_rim_top = IM_COL32(255, 255, 255, 250);
    t.glass_rim_bottom = IM_COL32(38, 70, 126, 106);
    t.glass_glow_a = IM_COL32(56, 148, 255, 112);
    t.glass_glow_b = IM_COL32(138, 106, 255, 88);
    t.glass_glow_c = IM_COL32(24, 198, 228, 82);
    t.folder_back = IM_COL32(12, 99, 214, 255);
    t.folder_front_top = IM_COL32(75, 168, 255, 255);
    t.folder_front_bottom = IM_COL32(11, 110, 240, 255);
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
