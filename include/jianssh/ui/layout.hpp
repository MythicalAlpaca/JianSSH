#pragma once
// Layout contract: every size, gap and breakpoint the workspace uses.
//
// Nothing in the drawing code invents a number. Regions are computed from the
// available width so the three columns always sum to exactly the space they
// were given, and every hand drawn element is clipped to the rectangle it was
// allocated.
//
// The metric set follows the macOS visual language: generous vertical rhythm,
// 8-12 px corner radii, hairline separators and compact 30-34 px controls.
#include <algorithm>

namespace jianssh::ui {

struct Layout {
    // ---- Window chrome -------------------------------------------------
    float window_padding_x = 18.f;
    float window_padding_y = 16.f;
    float frame_padding_x = 12.f;
    float frame_padding_y = 8.f;
    float item_spacing = 10.f;
    float section_spacing = 18.f;
    float frame_rounding = 8.f;
    float child_rounding = 12.f;
    float popup_rounding = 14.f;
    float window_rounding = 0.f;
    float scrollbar_size = 8.f;
    float scrollbar_rounding = 4.f;
    float grab_rounding = 4.f;
    float grab_min_size = 24.f;
    float tab_rounding = 8.f;
    float disabled_alpha = 0.45f;

    // ---- Regions --------------------------------------------------------
    float status_bar_height = 30.f;
    float sidebar_ratio = 0.22f;
    float sidebar_min = 220.f;
    float sidebar_max = 280.f;
    float central_min = 210.f;
    float column_gap = 14.f;
    float region_min_height = 100.f;
    // Session chrome is deliberately tighter than the welcome/workspace page.
    float session_padding = 10.f;
    float session_spacing = 6.f;
    float session_sidebar_ratio = 0.15f;
    float session_sidebar_min = 166.f;
    float session_sidebar_max = 230.f;
    float session_sidebar_breakpoint = 960.f;
    float session_sidebar_min_height = 700.f;
    float session_sidebar_gap = 10.f;
    float session_tab_bar_height = 40.f;

    // ---- Device list ----------------------------------------------------
    float device_row_height = 54.f;
    float device_row_text_x = 54.f;
    float device_row_title_dy = 9.f;
    float device_row_address_dy = 30.f;
    float device_avatar_size = 34.f;
    float device_outline_rounding = 8.f;
    float device_hover_alpha = 46.f;
    float device_list_min_height = 45.f;
    float device_list_reserve = 58.f;

    // ---- Session tabs ---------------------------------------------------
    float tab_bar_height = 42.f;
    float tab_min_width = 120.f;
    float tab_max_width = 240.f;
    float tab_height = 30.f;
    float tab_gap = 4.f;

    // ---- Controls -------------------------------------------------------
    float button_height = 34.f;
    float button_height_large = 38.f;
    float button_height_small = 30.f;
    float button_height_xl = 44.f;
    float select_height = 30.f;
    float input_port_width = 110.f;
    float note_field_height = 64.f;
    float progress_bar_height = 6.f;
    float segment_height = 30.f;
    float segment_padding = 3.f;
    float segment_rounding = 9.f;

    // ---- Modals ---------------------------------------------------------
    float modal_width_device = 620.f;
    float modal_width_settings = 520.f;
    float modal_width_confirm = 400.f;
    float modal_width_paste = 540.f;
    float modal_width_trust = 500.f;
    float modal_width_operation = 420.f;
    float confirm_min_width = 270.f;
    float confirm_viewport_margin = 12.f;
    float confirm_padding = 14.f;
    float confirm_footer_gap = 8.f;
    float confirm_button_min = 88.f;
    float modal_viewport_margin = 32.f;
    float modal_min_width = 280.f;
    // Space reserved outside the scrolling body: window padding, popup title
    // and the fixed footer row. Used to cap the body before a modal is laid out.
    float modal_chrome_height = 126.f;
    float modal_body_min_height = 84.f;
    float modal_body_max_ratio = 0.92f;

    // ---- Files ----------------------------------------------------------
    float file_pane_height = 260.f;
    float paste_preview_height = 220.f;

    // ---- Metrics --------------------------------------------------------
    float chart_height = 48.f;
    float chart_rounding = 4.f;
    float metrics_bar_height = 36.f;
    float metrics_bar_gap = 6.f;

    // ---- Terminal -------------------------------------------------------
    float terminal_padding = 10.f;
    float terminal_rounding = 10.f;
    float terminal_grid_gap = 20.f;

    // ---- Welcome --------------------------------------------------------
    float welcome_top_ratio = 0.16f;
    float welcome_top_min = 18.f;
    float welcome_button_width = 200.f;
};

inline constexpr Layout layout{};

// Column widths exclude the explicit gap placed between them.
struct Regions {
    float sidebar = 0.f;
    float central = 0.f;
    [[nodiscard]] float gaps(const Layout& l = layout) const {
        return l.column_gap;
    }
    [[nodiscard]] float occupied_width(const Layout& l = layout) const {
        return sidebar + central + gaps(l);
    }
};

inline Regions compute_regions(float width, const Layout& l = layout) {
    Regions r;
    width = std::max(0.f, width);
    r.sidebar = std::clamp(width * l.sidebar_ratio, l.sidebar_min, l.sidebar_max);
    r.central = width - r.sidebar - r.gaps(l);
    if (r.central < l.central_min) {
        const float desired_sidebar = std::max(0.f, width - l.central_min - r.gaps(l));
        r.sidebar = std::min(r.sidebar, desired_sidebar);
        r.central = std::max(0.f, width - r.sidebar - r.gaps(l));
    }
    return r;
}

inline float session_sidebar_width(float width,const Layout& l=layout) {
    return std::clamp(width*l.session_sidebar_ratio,l.session_sidebar_min,l.session_sidebar_max);
}

// Height of the region that holds the three columns; the status bar sits
// below it. The result never exceeds the offered height.
inline float region_height(float available_height, const Layout& l = layout) {
    return std::max(0.f, available_height - l.status_bar_height);
}
} // namespace jianssh::ui
