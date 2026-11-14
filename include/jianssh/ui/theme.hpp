#pragma once
// Single source of truth for every colour the interface draws.
//
// The palette follows the macOS system appearance: neutral, slightly warm
// greys, translucent control fills, hairline separators and the system blue
// accent. The dark and light presets are generated from one field list, so a
// slot can never exist in one theme and be forgotten in the other. Everything
// the UI paints - ImGui widgets, hand drawn rows, the terminal and the charts
// - reads from the same palette instance, which is what makes a smooth theme
// switch possible.
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
    X(chart_grid)

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

// macOS dark appearance: #1E1E20 window, #2C2C2F elevated surfaces and the
// system blue accent (#0A84FF). Control fills are translucent white, the way
// AppKit composes materials over a window background.
inline Theme dark_theme() {
    Theme t;
#define JIANSSH_THEME_SET(name) t.name = 0;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_SET)
#undef JIANSSH_THEME_SET
    t.window_bg = IM_COL32(30, 30, 32, 255);
    t.child_bg = IM_COL32(38, 38, 41, 255);
    t.sidebar_bg = IM_COL32(35, 35, 38, 255);
    t.card_bg = IM_COL32(43, 43, 46, 255);
    t.card_bg_alt = IM_COL32(50, 50, 53, 255);
    t.popup_bg = IM_COL32(44, 44, 47, 255);
    t.border = IM_COL32(255, 255, 255, 26);
    t.border_shadow = IM_COL32(0, 0, 0, 110);
    t.shadow = IM_COL32(0, 0, 0, 78);
    t.separator = IM_COL32(84, 84, 88, 120);
    t.separator_hovered = IM_COL32(10, 132, 255, 255);
    t.separator_active = IM_COL32(0, 100, 210, 255);
    t.text = IM_COL32(245, 245, 247, 255);
    t.text_disabled = IM_COL32(142, 142, 147, 255);
    t.text_link = IM_COL32(64, 156, 255, 255);
    t.text_selected_bg = IM_COL32(10, 132, 255, 90);
    t.frame_bg = IM_COL32(118, 118, 128, 48);
    t.frame_hovered = IM_COL32(118, 118, 128, 70);
    t.frame_active = IM_COL32(118, 118, 128, 92);
    t.title_bg = IM_COL32(44, 44, 47, 255);
    t.title_bg_active = IM_COL32(44, 44, 47, 255);
    t.title_bg_collapsed = IM_COL32(40, 40, 43, 255);
    t.menu_bar_bg = IM_COL32(34, 34, 37, 255);
    t.scrollbar_bg = IM_COL32(0, 0, 0, 0);
    t.scrollbar_grab = IM_COL32(255, 255, 255, 44);
    t.scrollbar_grab_hovered = IM_COL32(255, 255, 255, 92);
    t.scrollbar_grab_active = IM_COL32(255, 255, 255, 128);
    t.check_mark = IM_COL32(10, 132, 255, 255);
    t.slider_grab = IM_COL32(10, 132, 255, 255);
    t.slider_grab_active = IM_COL32(0, 100, 210, 255);
    t.button = IM_COL32(118, 118, 128, 46);
    t.button_hovered = IM_COL32(118, 118, 128, 70);
    t.button_active = IM_COL32(118, 118, 128, 96);
    t.header = IM_COL32(10, 132, 255, 54);
    t.header_hovered = IM_COL32(255, 255, 255, 18);
    t.header_active = IM_COL32(10, 132, 255, 78);
    t.tab = IM_COL32(255, 255, 255, 0);
    t.tab_hovered = IM_COL32(255, 255, 255, 14);
    t.tab_selected = IM_COL32(58, 58, 61, 255);
    t.tab_selected_overline = IM_COL32(10, 132, 255, 255);
    t.tab_dimmed = IM_COL32(255, 255, 255, 0);
    t.tab_dimmed_selected = IM_COL32(48, 48, 51, 255);
    t.tab_dimmed_selected_overline = IM_COL32(10, 132, 255, 130);
    t.resize_grip = IM_COL32(255, 255, 255, 26);
    t.resize_grip_hovered = IM_COL32(10, 132, 255, 220);
    t.resize_grip_active = IM_COL32(10, 132, 255, 255);
    t.plot_lines = IM_COL32(255, 255, 255, 34);
    t.plot_lines_hovered = IM_COL32(10, 132, 255, 255);
    t.plot_histogram = IM_COL32(10, 132, 255, 220);
    t.plot_histogram_hovered = IM_COL32(64, 156, 255, 255);
    t.table_header_bg = IM_COL32(255, 255, 255, 12);
    t.table_border_strong = IM_COL32(255, 255, 255, 30);
    t.table_border_light = IM_COL32(255, 255, 255, 14);
    t.table_row_bg = IM_COL32(255, 255, 255, 0);
    t.table_row_bg_alt = IM_COL32(255, 255, 255, 6);
    t.drag_drop_target = IM_COL32(10, 132, 255, 190);
    t.nav_cursor = IM_COL32(10, 132, 255, 255);
    t.nav_windowing_highlight = IM_COL32(10, 132, 255, 190);
    t.nav_windowing_dim_bg = IM_COL32(0, 0, 0, 120);
    t.modal_dim_bg = IM_COL32(0, 0, 0, 130);
    t.ink = IM_COL32(245, 245, 247, 255);
    t.muted = IM_COL32(152, 152, 157, 255);
    t.accent = IM_COL32(10, 132, 255, 255);
    t.accent_hover = IM_COL32(64, 156, 255, 255);
    t.accent_active = IM_COL32(0, 100, 210, 255);
    t.accent_soft = IM_COL32(10, 132, 255, 38);
    t.surface = IM_COL32(44, 44, 47, 255);
    t.danger = IM_COL32(255, 69, 58, 255);
    t.warning = IM_COL32(255, 159, 10, 255);
    t.success = IM_COL32(48, 209, 88, 255);
    t.info = IM_COL32(100, 210, 255, 255);
    t.terminal_bg = IM_COL32(24, 24, 26, 255);
    t.terminal_fg = IM_COL32(232, 232, 237, 255);
    t.terminal_cursor = IM_COL32(10, 132, 255, 255);
    t.chart_bg = IM_COL32(32, 32, 34, 255);
    t.chart_line = IM_COL32(10, 132, 255, 255);
    t.chart_grid = IM_COL32(255, 255, 255, 26);
    t.ansi = {IM_COL32(30, 30, 32, 255),   IM_COL32(255, 107, 106, 255), IM_COL32(79, 214, 124, 255),
              IM_COL32(240, 198, 116, 255), IM_COL32(90, 167, 255, 255),  IM_COL32(201, 139, 255, 255),
              IM_COL32(95, 224, 230, 255),  IM_COL32(232, 232, 237, 255), IM_COL32(110, 110, 118, 255),
              IM_COL32(255, 137, 133, 255), IM_COL32(116, 232, 154, 255), IM_COL32(255, 214, 138, 255),
              IM_COL32(135, 190, 255, 255), IM_COL32(220, 169, 255, 255), IM_COL32(138, 240, 244, 255),
              IM_COL32(255, 255, 255, 255)};
    t.dark = true;
    return t;
}

// macOS light appearance: #F2F2F7 window, white cards and the light system
// blue accent (#007AFF). Borders and control fills are a fraction of black so
// surfaces stay airy instead of greyed out.
inline Theme light_theme() {
    Theme t;
#define JIANSSH_THEME_SET(name) t.name = 0;
    JIANSSH_THEME_FIELDS(JIANSSH_THEME_SET)
#undef JIANSSH_THEME_SET
    t.window_bg = IM_COL32(242, 242, 247, 255);
    t.child_bg = IM_COL32(255, 255, 255, 246);
    t.sidebar_bg = IM_COL32(246, 246, 249, 255);
    t.card_bg = IM_COL32(255, 255, 255, 255);
    t.card_bg_alt = IM_COL32(249, 249, 251, 255);
    t.popup_bg = IM_COL32(255, 255, 255, 255);
    t.border = IM_COL32(60, 60, 67, 38);
    t.border_shadow = IM_COL32(0, 0, 0, 34);
    t.shadow = IM_COL32(0, 0, 0, 22);
    t.separator = IM_COL32(60, 60, 67, 44);
    t.separator_hovered = IM_COL32(0, 122, 255, 255);
    t.separator_active = IM_COL32(0, 98, 204, 255);
    t.text = IM_COL32(29, 29, 31, 255);
    t.text_disabled = IM_COL32(142, 142, 147, 255);
    t.text_link = IM_COL32(0, 122, 255, 255);
    t.text_selected_bg = IM_COL32(0, 122, 255, 54);
    t.frame_bg = IM_COL32(118, 118, 128, 28);
    t.frame_hovered = IM_COL32(118, 118, 128, 44);
    t.frame_active = IM_COL32(118, 118, 128, 62);
    t.title_bg = IM_COL32(255, 255, 255, 255);
    t.title_bg_active = IM_COL32(255, 255, 255, 255);
    t.title_bg_collapsed = IM_COL32(250, 250, 252, 255);
    t.menu_bar_bg = IM_COL32(250, 250, 252, 255);
    t.scrollbar_bg = IM_COL32(0, 0, 0, 0);
    t.scrollbar_grab = IM_COL32(0, 0, 0, 40);
    t.scrollbar_grab_hovered = IM_COL32(0, 0, 0, 76);
    t.scrollbar_grab_active = IM_COL32(0, 0, 0, 108);
    t.check_mark = IM_COL32(0, 122, 255, 255);
    t.slider_grab = IM_COL32(0, 122, 255, 255);
    t.slider_grab_active = IM_COL32(0, 98, 204, 255);
    t.button = IM_COL32(118, 118, 128, 30);
    t.button_hovered = IM_COL32(118, 118, 128, 46);
    t.button_active = IM_COL32(118, 118, 128, 64);
    t.header = IM_COL32(0, 122, 255, 34);
    t.header_hovered = IM_COL32(0, 0, 0, 10);
    t.header_active = IM_COL32(0, 122, 255, 52);
    t.tab = IM_COL32(0, 0, 0, 0);
    t.tab_hovered = IM_COL32(0, 0, 0, 10);
    t.tab_selected = IM_COL32(255, 255, 255, 255);
    t.tab_selected_overline = IM_COL32(0, 122, 255, 255);
    t.tab_dimmed = IM_COL32(0, 0, 0, 0);
    t.tab_dimmed_selected = IM_COL32(255, 255, 255, 235);
    t.tab_dimmed_selected_overline = IM_COL32(0, 122, 255, 140);
    t.resize_grip = IM_COL32(0, 0, 0, 28);
    t.resize_grip_hovered = IM_COL32(0, 122, 255, 200);
    t.resize_grip_active = IM_COL32(0, 98, 204, 255);
    t.plot_lines = IM_COL32(60, 60, 67, 40);
    t.plot_lines_hovered = IM_COL32(0, 122, 255, 255);
    t.plot_histogram = IM_COL32(0, 122, 255, 210);
    t.plot_histogram_hovered = IM_COL32(51, 149, 255, 255);
    t.table_header_bg = IM_COL32(118, 118, 128, 18);
    t.table_border_strong = IM_COL32(60, 60, 67, 38);
    t.table_border_light = IM_COL32(60, 60, 67, 20);
    t.table_row_bg = IM_COL32(255, 255, 255, 0);
    t.table_row_bg_alt = IM_COL32(0, 0, 0, 6);
    t.drag_drop_target = IM_COL32(0, 122, 255, 180);
    t.nav_cursor = IM_COL32(0, 122, 255, 255);
    t.nav_windowing_highlight = IM_COL32(0, 122, 255, 180);
    t.nav_windowing_dim_bg = IM_COL32(0, 0, 0, 80);
    t.modal_dim_bg = IM_COL32(0, 0, 0, 70);
    t.ink = IM_COL32(29, 29, 31, 255);
    t.muted = IM_COL32(110, 110, 115, 255);
    t.accent = IM_COL32(0, 122, 255, 255);
    t.accent_hover = IM_COL32(51, 149, 255, 255);
    t.accent_active = IM_COL32(0, 98, 204, 255);
    t.accent_soft = IM_COL32(0, 122, 255, 30);
    t.surface = IM_COL32(255, 255, 255, 255);
    t.danger = IM_COL32(255, 59, 48, 255);
    t.warning = IM_COL32(255, 149, 0, 255);
    t.success = IM_COL32(52, 199, 89, 255);
    t.info = IM_COL32(0, 122, 255, 255);
    t.terminal_bg = IM_COL32(252, 252, 253, 255);
    t.terminal_fg = IM_COL32(29, 29, 31, 255);
    t.terminal_cursor = IM_COL32(0, 122, 255, 255);
    t.chart_bg = IM_COL32(255, 255, 255, 255);
    t.chart_line = IM_COL32(0, 122, 255, 255);
    t.chart_grid = IM_COL32(60, 60, 67, 26);
    t.ansi = {IM_COL32(40, 44, 52, 255),   IM_COL32(215, 60, 48, 255),  IM_COL32(36, 138, 61, 255),
              IM_COL32(190, 126, 20, 255), IM_COL32(30, 108, 200, 255), IM_COL32(148, 85, 190, 255),
              IM_COL32(24, 140, 140, 255), IM_COL32(110, 118, 128, 255), IM_COL32(132, 140, 150, 255),
              IM_COL32(235, 80, 70, 255),  IM_COL32(66, 168, 85, 255),  IM_COL32(220, 160, 40, 255),
              IM_COL32(62, 134, 220, 255), IM_COL32(168, 108, 210, 255), IM_COL32(30, 160, 158, 255),
              IM_COL32(29, 29, 31, 255)};
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
