#pragma once
// Procedural Liquid Glass renderer and crisp vector SFTP icons built purely on
// Dear ImGui's public ImDrawList API.
//
// Performance contract:
// - Zero framebuffers, zero real-time blur shaders, zero extra GPU textures.
// - Rounded gradients and directional specular rims shade existing vertices in
//   place (zero extra draw calls and zero square-corner clipping artifacts).
// - Vector folder/file icons allocate zero atlas memory and draw only for
//   visible clipper rows.
#include "jianssh/ui/theme.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace jianssh::ui {

// Shade emitted ImDrawList vertices [vtx_start, vtx_end) along p0 -> p1 while
// preserving each vertex's original anti-aliasing alpha fringe.
inline void shade_verts_linear_gradient(ImDrawList* dl, int vtx_start, int vtx_end,
                                        ImVec2 p0, ImVec2 p1, ImU32 col0, ImU32 col1) noexcept {
    if (!dl || vtx_end <= vtx_start) return;
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;
    const float len2 = dx * dx + dy * dy;
    const float inv_len2 = len2 > 1e-6f ? (1.0f / len2) : 0.0f;

    const float r0 = static_cast<float>(col0 & 0xFFu);
    const float g0 = static_cast<float>((col0 >> 8) & 0xFFu);
    const float b0 = static_cast<float>((col0 >> 16) & 0xFFu);
    const float a0 = static_cast<float>((col0 >> 24) & 0xFFu);

    const float dr = static_cast<float>(col1 & 0xFFu) - r0;
    const float dg = static_cast<float>((col1 >> 8) & 0xFFu) - g0;
    const float db = static_cast<float>((col1 >> 16) & 0xFFu) - b0;
    const float da = static_cast<float>((col1 >> 24) & 0xFFu) - a0;

    ImDrawVert* verts = dl->VtxBuffer.Data;
    for (int i = vtx_start; i < vtx_end; ++i) {
        ImDrawVert& v = verts[i];
        const float t = std::clamp(((v.pos.x - p0.x) * dx + (v.pos.y - p0.y) * dy) * inv_len2, 0.0f, 1.0f);
        const unsigned orig_a = (v.col >> 24) & 0xFFu;
        const unsigned r = static_cast<unsigned>(std::clamp(r0 + dr * t, 0.0f, 255.0f) + 0.5f);
        const unsigned g = static_cast<unsigned>(std::clamp(g0 + dg * t, 0.0f, 255.0f) + 0.5f);
        const unsigned b = static_cast<unsigned>(std::clamp(b0 + db * t, 0.0f, 255.0f) + 0.5f);
        const unsigned grad_a = static_cast<unsigned>(std::clamp(a0 + da * t, 0.0f, 255.0f) + 0.5f);
        const unsigned final_a = (orig_a * grad_a + 127u) / 255u;
        v.col = IM_COL32(r, g, b, final_a);
    }
}

// Rounded rectangle with smooth vertex-shaded vertical or diagonal gradient.
inline void draw_gradient_rounded_rect(ImDrawList* dl, ImVec2 a, ImVec2 b,
                                       ImU32 col_start, ImU32 col_end,
                                       float rounding, ImDrawFlags flags = 0,
                                       bool diagonal = false) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y) return;
    const int vtx_start = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 255), rounding, flags);
    const ImVec2 p0 = a;
    const ImVec2 p1 = diagonal ? b : ImVec2(a.x, b.y);
    shade_verts_linear_gradient(dl, vtx_start, dl->VtxBuffer.Size, p0, p1, col_start, col_end);
}

// Directional refractive rim: bright top-left specular edge fading smoothly
// into a softer bottom-right glass contour.
inline void draw_glass_rim(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                           ImU32 top_left, ImU32 bottom_right,
                           float thickness = 1.0f, ImDrawFlags flags = 0) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y) return;
    const int vtx_start = dl->VtxBuffer.Size;
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), rounding, flags, thickness);
    shade_verts_linear_gradient(dl, vtx_start, dl->VtxBuffer.Size, a, b, top_left, bottom_right);
}

// Rounded top-half liquid glass meniscus highlight with zero square-corner leaks.
inline void draw_glass_sheen(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                             ImU32 highlight, float max_height = 68.f, float ratio = 0.46f) noexcept {
    if (!dl || b.x - a.x < 4.f || b.y - a.y < 4.f) return;
    const float inset = 1.f;
    const float inner_r = std::max(0.f, rounding - inset);
    const float h = b.y - a.y - inset * 2.f;
    if (h <= 2.f) return;
    const float band_h = std::min(h, std::max(inner_r + 1.f, std::min(h * ratio, max_height)));
    const ImVec2 sa(a.x + inset, a.y + inset);
    const ImVec2 sb(b.x - inset, sa.y + band_h);
    const ImVec2 grad_end(sa.x, sa.y + std::min(band_h, std::max(8.f, h * ratio)));
    const int vtx_start = dl->VtxBuffer.Size;
    dl->AddRectFilled(sa, sb, IM_COL32(255, 255, 255, 255), inner_r, ImDrawFlags_RoundCornersTop);
    shade_verts_linear_gradient(dl, vtx_start, dl->VtxBuffer.Size, sa, grad_end, highlight, fade_color(highlight, 0.f));
}

// Continuous radial Gaussian glow mesh (140 triangles, zero ring banding, zero shaders).
inline void draw_soft_orb(ImDrawList* dl, ImVec2 center, float radius, ImU32 color) noexcept {
    if (!dl || radius <= 1.f || ((color >> 24) & 0xFFu) == 0u) return;
    constexpr int kSegs = 28;
    constexpr float kTwoPi = 6.2831853071795864769f;
    static constexpr float kRingR[3] = {0.34f, 0.68f, 1.00f};

    ImVec2 ring[3][kSegs + 1];
    for (int i = 0; i <= kSegs; ++i) {
        const float ang = kTwoPi * (static_cast<float>(i) / static_cast<float>(kSegs));
        const float cs = std::cos(ang);
        const float sn = std::sin(ang);
        for (int r = 0; r < 3; ++r) {
            const float rad = radius * kRingR[r];
            ring[r][i] = ImVec2(center.x + cs * rad, center.y + sn * rad);
        }
    }

    const ImDrawListFlags saved_flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    const int vtx_start = dl->VtxBuffer.Size;
    for (int i = 0; i < kSegs; ++i) {
        dl->AddTriangleFilled(center, ring[0][i], ring[0][i + 1], IM_COL32(255, 255, 255, 255));
        dl->AddQuadFilled(ring[0][i], ring[1][i], ring[1][i + 1], ring[0][i + 1], IM_COL32(255, 255, 255, 255));
        dl->AddQuadFilled(ring[1][i], ring[2][i], ring[2][i + 1], ring[1][i + 1], IM_COL32(255, 255, 255, 255));
    }
    dl->Flags = saved_flags;

    const float inv_r = 1.0f / radius;
    const unsigned base_r = color & 0xFFu;
    const unsigned base_g = (color >> 8) & 0xFFu;
    const unsigned base_b = (color >> 16) & 0xFFu;
    const float base_a = static_cast<float>((color >> 24) & 0xFFu);
    ImDrawVert* verts = dl->VtxBuffer.Data;
    const int vtx_end = dl->VtxBuffer.Size;
    for (int i = vtx_start; i < vtx_end; ++i) {
        ImDrawVert& v = verts[i];
        const float dx = (v.pos.x - center.x) * inv_r;
        const float dy = (v.pos.y - center.y) * inv_r;
        const float u2 = std::min(1.0f, dx * dx + dy * dy);
        // Smooth (1 - u^2)^2 bell: zero slope at both center and rim.
        const float w = (1.0f - u2) * (1.0f - u2);
        const unsigned a = static_cast<unsigned>(std::clamp(base_a * w, 0.0f, 255.0f) + 0.5f);
        v.col = IM_COL32(base_r, base_g, base_b, a);
    }
}

// Atmospheric Liquid Glass window backdrop so translucent panels refract real depth.
inline void draw_ambient_backdrop(ImDrawList* dl, ImVec2 p, ImVec2 q, const Theme& t) noexcept {
    if (!dl || q.x <= p.x || q.y <= p.y) return;
    const float w = q.x - p.x;
    const float h = q.y - p.y;
    const ImU32 top_left = t.dark ? lerp_color(t.window_bg, t.accent, 0.08f)
                                  : lerp_color(t.window_bg, IM_COL32(255, 255, 255, 255), 0.52f);
    const ImU32 top_right = t.dark ? lerp_color(t.window_bg, t.glass_glow_c, 0.05f)
                                   : lerp_color(t.window_bg, t.glass_glow_b, 0.06f);
    const ImU32 bot_right = t.dark ? lerp_color(t.window_bg, IM_COL32(6, 9, 16, 255), 0.35f)
                                   : lerp_color(t.window_bg, t.glass_glow_a, 0.05f);
    const ImU32 bot_left = t.dark ? lerp_color(t.window_bg, t.glass_glow_b, 0.04f)
                                  : t.window_bg;
    dl->AddRectFilledMultiColor(p, q, top_left, top_right, bot_right, bot_left);

    dl->PushClipRect(p, q, true);
    const float diag = std::min(w, h);
    draw_soft_orb(dl, ImVec2(p.x + w * 0.16f, p.y + h * 0.14f), std::max(220.f, diag * 0.56f), t.glass_glow_a);
    draw_soft_orb(dl, ImVec2(p.x + w * 0.84f, p.y + h * 0.82f), std::max(240.f, diag * 0.62f), t.glass_glow_b);
    draw_soft_orb(dl, ImVec2(p.x + w * 0.56f, p.y + h * 0.22f), std::max(180.f, diag * 0.44f), t.glass_glow_c);
    dl->PopClipRect();
}

// Primary frosted Liquid Glass container (sidebar, main workspace, SFTP panes).
inline void draw_glass_panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                             const Theme& t, float alpha_scale = 1.f,
                             bool sidebar = false, float accent_amount = 0.04f) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y || alpha_scale <= 0.01f) return;
    // Soft ambient elevation shadow behind the floating glass sheet.
    dl->AddRectFilled(ImVec2(a.x - 1.f, a.y + 3.f), ImVec2(b.x + 1.f, b.y + 6.f),
                      fade_color(t.shadow, (t.dark ? 0.55f : 0.42f) * alpha_scale), rounding + 2.f);
    dl->AddRectFilled(ImVec2(a.x, a.y + 1.f), ImVec2(b.x, b.y + 2.5f),
                      fade_color(t.shadow, (t.dark ? 0.40f : 0.28f) * alpha_scale), rounding + 0.5f);

    // Translucent frosted glass body with subtle diagonal luminance falloff.
    const ImU32 base = sidebar ? t.sidebar_bg : t.child_bg;
    const ImU32 top_col = fade_color(
        lerp_color(base, t.dark ? lerp_color(t.glass_bg_strong, t.accent, accent_amount) : IM_COL32(255, 255, 255, 220),
                   t.dark ? 0.28f : 0.36f),
        alpha_scale);
    const ImU32 bot_col = fade_color(
        lerp_color(base, t.dark ? t.glass_bg_subtle : t.glass_bg, t.dark ? 0.24f : 0.14f),
        alpha_scale);
    draw_gradient_rounded_rect(dl, a, b, top_col, bot_col, rounding, 0, true);

    // Convex liquid specular highlight across the upper meniscus.
    draw_glass_sheen(dl, a, b, rounding,
                     fade_color(t.glass_highlight, (t.dark ? 0.85f : 0.72f) * alpha_scale),
                     96.f, 0.34f);

    // Crisp 1px top specular refraction hairline inside the upper rim.
    const float inset = std::min(rounding * 0.85f, (b.x - a.x) * 0.25f);
    if (b.x - a.x > inset * 2.f + 4.f) {
        dl->AddLine(ImVec2(a.x + inset, a.y + 1.2f), ImVec2(b.x - inset, a.y + 1.2f),
                    fade_color(t.glass_rim_top, (t.dark ? 0.72f : 0.90f) * alpha_scale), 1.0f);
    }

    // Directional glass rim border (top-left bright specular -> bottom-right cool edge).
    draw_glass_rim(dl, a, b, rounding,
                   fade_color(t.glass_rim_top, alpha_scale),
                   fade_color(t.glass_rim_bottom, alpha_scale), 1.0f);
}

// Recessed frosted glass well (segmented tracks, breadcrumb bar, search well).
inline void draw_glass_well(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                            const Theme& t, float hover = 0.f, bool focused = false) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y) return;
    const ImU32 top = lerp_color(t.glass_bg_subtle, t.frame_hovered, hover * 0.45f);
    const ImU32 bot = lerp_color(t.frame_bg, t.frame_hovered, hover * 0.55f);
    draw_gradient_rounded_rect(dl, a, b, top, bot, rounding, 0, false);
    if (focused) {
        dl->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f),
                    fade_color(t.accent, 0.78f), rounding + 1.f, 0, 1.5f);
    } else {
        draw_glass_rim(dl, a, b, rounding,
                       fade_color(t.border, t.dark ? 0.92f : 0.96f),
                       fade_color(t.glass_rim_top, t.dark ? 0.55f : 0.85f), 1.0f);
    }
}

// Sliding Liquid Glass jelly capsule (segmented thumb, active tab, active group).
inline void draw_jelly_pill(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                            float stretch_x, float stretch_y, const Theme& t,
                            bool accented = false, float alpha = 1.f) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y || alpha <= 0.01f) return;
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float dx = std::clamp(w * stretch_x * 0.5f, -6.f, 10.f);
    const float dy = std::clamp(h * stretch_y * 0.5f, -4.f, 6.f);
    const ImVec2 pa(cx - w * 0.5f - dx, cy - h * 0.5f - dy);
    const ImVec2 pb(cx + w * 0.5f + dx, cy + h * 0.5f + dy);
    const float r = std::min(rounding, std::min(pb.x - pa.x, pb.y - pa.y) * 0.5f);

    // Soft floating drop shadow beneath the liquid lens.
    dl->AddRectFilled(ImVec2(pa.x, pa.y + 1.8f), ImVec2(pb.x, pb.y + 3.0f),
                      fade_color(t.shadow, (t.dark ? 0.62f : (accented ? 0.30f : 0.44f)) * alpha), r + 0.5f);

    // Translucent refractive body.
    ImU32 fill_top = 0, fill_bot = 0;
    if (accented) {
        fill_top = fade_color(t.dark ? lerp_color(t.header, t.accent, 0.28f)
                                     : lerp_color(IM_COL32(236, 245, 255, 244), t.accent, 0.18f), alpha);
        fill_bot = fade_color(t.dark ? lerp_color(t.header, t.glass_bg_strong, 0.28f)
                                     : lerp_color(IM_COL32(214, 232, 255, 240), t.accent, 0.24f), alpha);
    } else {
        fill_top = fade_color(t.dark ? lerp_color(t.glass_bg_strong, IM_COL32(255, 255, 255, 235), 0.16f)
                                     : IM_COL32(255, 255, 255, 250), alpha);
        fill_bot = fade_color(t.dark ? t.glass_bg_strong
                                     : IM_COL32(236, 244, 255, 240), alpha);
    }
    draw_gradient_rounded_rect(dl, pa, pb, fill_top, fill_bot, r, 0, false);

    // Convex liquid glass specular sheen on top half.
    draw_glass_sheen(dl, pa, pb, r,
                     fade_color(IM_COL32(255, 255, 255, 255),
                                (t.dark ? (accented ? 0.26f : 0.22f) : 0.72f) * alpha),
                     24.f, 0.50f);

    // Luminous directional glass rim.
    const ImU32 rim_hi = accented ? fade_color(lerp_color(t.glass_rim_top, t.accent_hover, t.dark ? 0.46f : 0.32f), alpha)
                                  : fade_color(t.dark ? t.glass_rim_top : t.border, t.dark ? alpha : 0.85f * alpha);
    const ImU32 rim_lo = accented ? fade_color(t.accent, (t.dark ? 0.48f : 0.52f) * alpha)
                                  : fade_color(t.glass_rim_bottom, alpha);
    draw_glass_rim(dl, pa, pb, r, rim_hi, rim_lo, 1.05f);
}

// Floating popup/modal chrome: rounded specular top refraction band and smooth
// directional glass rim matching the window's exact corner radius (no square leaks).
inline void draw_glass_modal_chrome(ImDrawList* dl, ImVec2 pos, ImVec2 size, float rounding, const Theme& t) noexcept {
    if (!dl || size.x <= 4.f || size.y <= 4.f) return;
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    const ImVec2 rim_a(pos.x + 0.5f, pos.y + 0.5f);
    const ImVec2 rim_b(b.x - 0.5f, b.y - 0.5f);
    const float r = std::max(0.f, rounding - 0.5f);
    dl->PushClipRect(ImVec2(pos.x - 2.f, pos.y - 2.f), ImVec2(b.x + 2.f, b.y + 2.f), false);
    draw_glass_sheen(dl, rim_a, rim_b, r,
                     fade_color(t.glass_highlight, t.dark ? 0.55f : 0.56f), 40.f, 0.18f);
    const ImU32 hi = t.dark ? t.glass_rim_top : lerp_color(t.border, t.glass_rim_top, 0.35f);
    const ImU32 lo = t.dark ? t.glass_rim_bottom : t.border;
    draw_glass_rim(dl, rim_a, rim_b, r, hi, lo, 1.15f);
    dl->PopClipRect();
}

// Context-menu / dropdown popup chrome with matching rounded rim.
inline void draw_glass_popup_chrome(ImDrawList* dl, ImVec2 pos, ImVec2 size, float rounding, const Theme& t) noexcept {
    if (!dl || size.x <= 4.f || size.y <= 4.f) return;
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    const ImVec2 rim_a(pos.x + 0.5f, pos.y + 0.5f);
    const ImVec2 rim_b(b.x - 0.5f, b.y - 0.5f);
    const float r = std::max(0.f, rounding - 0.5f);
    dl->PushClipRect(ImVec2(pos.x - 2.f, pos.y - 2.f), ImVec2(b.x + 2.f, b.y + 2.f), false);
    draw_glass_sheen(dl, rim_a, rim_b, r,
                     fade_color(t.glass_highlight, t.dark ? 0.42f : 0.50f), 26.f, 0.28f);
    const ImU32 hi = t.dark ? t.glass_rim_top : lerp_color(t.border, t.glass_rim_top, 0.28f);
    const ImU32 lo = t.dark ? t.glass_rim_bottom : t.border;
    draw_glass_rim(dl, rim_a, rim_b, r, hi, lo, 1.1f);
    dl->PopClipRect();
}

// Upgraded brand icon with crisp rounded Liquid Glass sheen (no square corners).
inline void draw_brand_icon_glass(ImDrawList* dl, ImVec2 pos, float size, const Theme& t) noexcept {
    if (!dl || size <= 2.f) return;
    const ImVec2 b(pos.x + size, pos.y + size);
    const float r = size * 0.28f;
    dl->AddRectFilled(ImVec2(pos.x, pos.y + size * 0.05f), ImVec2(b.x, b.y + size * 0.08f),
                      fade_color(t.accent, t.dark ? 0.26f : 0.20f), r + 1.5f);
    draw_gradient_rounded_rect(dl, pos, b,
                               lerp_color(t.accent, t.accent_hover, 0.38f),
                               lerp_color(t.accent, t.accent_active, 0.42f), r, 0, true);
    draw_glass_sheen(dl, pos, b, r, fade_color(IM_COL32(255, 255, 255, 255), 0.34f), size * 0.56f, 0.54f);
    draw_glass_rim(dl, pos, b, r, fade_color(IM_COL32(255, 255, 255, 255), 0.56f),
                   fade_color(IM_COL32(255, 255, 255, 255), 0.10f), 1.0f);
    const float stroke = std::max(1.5f, size * 0.058f);
    const ImU32 white = IM_COL32(255, 255, 255, 245);
    dl->AddLine(ImVec2(pos.x + size * 0.26f, pos.y + size * 0.38f),
                ImVec2(pos.x + size * 0.42f, pos.y + size * 0.50f), white, stroke);
    dl->AddLine(ImVec2(pos.x + size * 0.42f, pos.y + size * 0.50f),
                ImVec2(pos.x + size * 0.26f, pos.y + size * 0.62f), white, stroke);
    dl->AddLine(ImVec2(pos.x + size * 0.50f, pos.y + size * 0.62f),
                ImVec2(pos.x + size * 0.72f, pos.y + size * 0.62f), white, stroke);
}

// ---- Crisp procedural vector icons for the SFTP directory bar --------------

// macOS-inspired Liquid Glass folder icon (zero bitmap/atlas overhead).
inline void draw_folder_icon(ImDrawList* dl, ImVec2 pos, float size, const Theme& t,
                             bool drop_highlight = false) noexcept {
    if (!dl || size <= 4.f) return;
    const float w = size;
    const float h = size * 0.84f;
    const float x = pos.x;
    const float y = pos.y + (size - h) * 0.5f;
    const float r = std::max(1.5f, size * 0.16f);

    const ImU32 back_col = drop_highlight ? lerp_color(t.folder_back, t.info, 0.45f) : t.folder_back;
    const ImU32 top_col = drop_highlight ? lerp_color(t.folder_front_top, IM_COL32(255, 255, 255, 255), 0.24f)
                                         : t.folder_front_top;
    const ImU32 bot_col = drop_highlight ? lerp_color(t.folder_front_bottom, t.info, 0.28f)
                                         : t.folder_front_bottom;

    // Grounded micro-shadow.
    dl->AddRectFilled(ImVec2(x + 1.0f, y + h * 0.34f + 1.0f), ImVec2(x + w - 0.5f, y + h + 1.1f),
                      fade_color(t.shadow, t.dark ? 0.55f : 0.36f), r);

    // Back tab ear + sloped shoulder + upper back plate.
    dl->AddRectFilled(ImVec2(x + 0.5f, y), ImVec2(x + w * 0.46f, y + h * 0.42f),
                      back_col, std::max(1.2f, size * 0.13f), ImDrawFlags_RoundCornersTop);
    const ImVec2 shoulder[3] = {
        ImVec2(x + w * 0.41f, y + h * 0.04f),
        ImVec2(x + w * 0.57f, y + h * 0.20f),
        ImVec2(x + w * 0.41f, y + h * 0.30f),
    };
    dl->AddTriangleFilled(shoulder[0], shoulder[1], shoulder[2], back_col);
    dl->AddRectFilled(ImVec2(x + 0.5f, y + h * 0.16f), ImVec2(x + w - 0.5f, y + h * 0.62f),
                      back_col, std::max(1.2f, size * 0.14f), ImDrawFlags_RoundCornersTopRight);

    // Frosted inner sheet peek inside folder crease.
    dl->AddRectFilled(ImVec2(x + w * 0.12f, y + h * 0.24f), ImVec2(x + w * 0.88f, y + h * 0.38f),
                      fade_color(IM_COL32(255, 255, 255, 255), t.dark ? 0.62f : 0.88f), 1.0f);

    // Front luminous glass body with smooth vertical gradient.
    const ImVec2 fa(x + 0.5f, y + h * 0.30f);
    const ImVec2 fb(x + w - 0.5f, y + h);
    draw_gradient_rounded_rect(dl, fa, fb, top_col, bot_col, r, 0, false);

    // Top specular glass highlight along the front flap lip.
    dl->AddLine(ImVec2(fa.x + 2.0f, fa.y + 0.9f), ImVec2(fb.x - 2.0f, fa.y + 0.9f),
                fade_color(IM_COL32(255, 255, 255, 255), 0.62f), 1.0f);

    // Delicate refractive edge rim.
    draw_glass_rim(dl, fa, fb, r,
                   fade_color(IM_COL32(255, 255, 255, 255), t.dark ? 0.34f : 0.48f),
                   fade_color(IM_COL32(0, 20, 60, 255), 0.22f), 0.85f);
}

// Extension tint helper for regular file icons (zero allocations).
inline ImU32 file_accent_color(std::string_view name, const Theme& t) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) return t.muted;
    const std::string_view ext = name.substr(dot + 1);
    auto eq = [&](std::string_view target) noexcept {
        if (ext.size() != target.size()) return false;
        for (std::size_t i = 0; i < ext.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(ext[i])) != target[i]) return false;
        return true;
    };
    if (eq("zip") || eq("tar") || eq("gz") || eq("tgz") || eq("bz2") || eq("xz") || eq("7z") || eq("rar"))
        return t.warning;
    if (eq("sh") || eq("bash") || eq("zsh") || eq("py") || eq("c") || eq("cpp") || eq("cc") || eq("h") ||
        eq("hpp") || eq("rs") || eq("go") || eq("zig") || eq("js") || eq("ts") || eq("java"))
        return t.info;
    if (eq("json") || eq("yaml") || eq("yml") || eq("toml") || eq("ini") || eq("conf") || eq("cfg") || eq("xml"))
        return IM_COL32(168, 130, 255, 255);
    if (eq("png") || eq("jpg") || eq("jpeg") || eq("gif") || eq("webp") || eq("svg") || eq("bmp"))
        return t.success;
    if (eq("pem") || eq("key") || eq("pub") || eq("crt") || eq("cer"))
        return t.danger;
    return t.muted;
}

// Frosted folded-corner document icon for regular files.
inline void draw_file_icon(ImDrawList* dl, ImVec2 pos, float size, std::string_view filename, const Theme& t) noexcept {
    if (!dl || size <= 4.f) return;
    const float w = size * 0.78f;
    const float h = size * 0.92f;
    const float x = pos.x + (size - w) * 0.5f;
    const float y = pos.y + (size - h) * 0.5f;
    const float fold = size * 0.26f;
    const ImVec2 a(x, y);
    const ImVec2 b(x + w, y + h);

    // Subtle shadow.
    dl->AddRectFilled(ImVec2(a.x + 0.6f, a.y + 1.2f), ImVec2(b.x + 0.6f, b.y + 1.2f),
                      fade_color(t.shadow, t.dark ? 0.42f : 0.25f), 2.2f);

    // Page silhouette (clockwise convex polygon with top-right corner folded).
    const ImVec2 body[5] = {
        ImVec2(a.x, a.y),
        ImVec2(b.x - fold, a.y),
        ImVec2(b.x, a.y + fold),
        ImVec2(b.x, b.y),
        ImVec2(a.x, b.y),
    };
    const int vtx_start = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(body, 5, IM_COL32(255, 255, 255, 255));
    const ImU32 page_top = t.dark ? IM_COL32(218, 230, 248, 210) : IM_COL32(252, 254, 255, 250);
    const ImU32 page_bot = t.dark ? IM_COL32(158, 174, 200, 185) : IM_COL32(214, 224, 242, 245);
    shade_verts_linear_gradient(dl, vtx_start, dl->VtxBuffer.Size, a, b, page_top, page_bot);

    // Folded dog-ear triangle.
    const ImVec2 ear[3] = {
        ImVec2(b.x - fold, a.y),
        ImVec2(b.x, a.y + fold),
        ImVec2(b.x - fold, a.y + fold),
    };
    dl->AddTriangleFilled(ear[0], ear[1], ear[2],
                          t.dark ? IM_COL32(118, 134, 162, 225) : IM_COL32(168, 184, 212, 245));

    // Crisp outline.
    dl->AddPolyline(body, 5,
                    t.dark ? fade_color(t.glass_rim_top, 0.58f) : IM_COL32(106, 126, 162, 138),
                    ImDrawFlags_Closed, 0.95f);

    // Extension accent bar + miniature text strokes.
    const ImU32 accent = file_accent_color(filename, t);
    dl->AddRectFilled(ImVec2(a.x + w * 0.20f, a.y + h * 0.44f),
                      ImVec2(a.x + w * 0.78f, a.y + h * 0.54f), fade_color(accent, 0.88f), 0.8f);
    dl->AddRectFilled(ImVec2(a.x + w * 0.20f, a.y + h * 0.64f),
                      ImVec2(a.x + w * 0.64f, a.y + h * 0.72f), fade_color(t.muted, 0.52f), 0.7f);
}

// Symlink icon: subtle file/folder base with luminous curved arrow badge.
inline void draw_symlink_icon(ImDrawList* dl, ImVec2 pos, float size, bool is_dir,
                              std::string_view filename, const Theme& t) noexcept {
    if (is_dir) draw_folder_icon(dl, pos, size, t, false);
    else draw_file_icon(dl, pos, size, filename, t);
    const float badge = size * 0.48f;
    const ImVec2 ba(pos.x + size - badge, pos.y + size - badge);
    const ImVec2 bb(pos.x + size + 0.5f, pos.y + size + 0.5f);
    dl->AddRectFilled(ba, bb, t.dark ? IM_COL32(28, 38, 58, 240) : IM_COL32(255, 255, 255, 245), badge * 0.34f);
    dl->AddRect(ba, bb, fade_color(t.info, 0.85f), badge * 0.34f, 0, 0.9f);
    // Crisp diagonal link arrow inside badge.
    const ImVec2 p0(ba.x + badge * 0.26f, bb.y - badge * 0.26f);
    const ImVec2 p1(bb.x - badge * 0.26f, ba.y + badge * 0.26f);
    dl->AddLine(p0, p1, t.info, 1.25f);
    dl->AddLine(p1, ImVec2(p1.x - badge * 0.32f, p1.y), t.info, 1.2f);
    dl->AddLine(p1, ImVec2(p1.x, p1.y + badge * 0.32f), t.info, 1.2f);
}

// Special node icon (pipes, sockets, devices).
inline void draw_other_file_icon(ImDrawList* dl, ImVec2 pos, float size, const Theme& t) noexcept {
    if (!dl || size <= 4.f) return;
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = size * 0.36f;
    const ImVec2 pts[4] = {
        ImVec2(c.x, c.y - r),
        ImVec2(c.x + r, c.y),
        ImVec2(c.x, c.y + r),
        ImVec2(c.x - r, c.y),
    };
    dl->AddConvexPolyFilled(pts, 4, fade_color(t.warning, t.dark ? 0.55f : 0.45f));
    dl->AddPolyline(pts, 4, fade_color(t.warning, 0.92f), ImDrawFlags_Closed, 1.0f);
    dl->AddCircleFilled(c, std::max(1.2f, size * 0.10f), IM_COL32(255, 255, 255, 220), 10);
}

// Header icon badge for SFTP local vs remote panes.
inline void draw_pane_badge_icon(ImDrawList* dl, ImVec2 pos, float size, bool remote, const Theme& t) noexcept {
    if (!dl || size <= 4.f) return;
    if (!remote) {
        draw_folder_icon(dl, pos, size, t, false);
        return;
    }
    // Remote SFTP: luminous folder with subtle network ring indicator.
    draw_folder_icon(dl, pos, size, t, true);
    const ImVec2 c(pos.x + size * 0.78f, pos.y + size * 0.78f);
    dl->AddCircleFilled(c, size * 0.24f, t.dark ? IM_COL32(18, 28, 46, 240) : IM_COL32(255, 255, 255, 245), 14);
    dl->AddCircleFilled(c, size * 0.14f, t.success, 12);
}

// Single-pass UTF-8 elision using ImFont::CalcTextSizeA(..., &remaining):
// eliminates per-character substring allocations on long SFTP filenames/paths.
inline std::string elide_fast(ImFont* font, float size, std::string_view text, float max_width) {
    if (!font || max_width <= 0.f || text.empty()) return {};
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const char* remaining = nullptr;
    // Allow 0.5px tolerance so widths measured via CalcTextSizeA don't trigger
    // off-by-1-ULP truncation of the final character.
    const float limit = max_width + 0.5f;
    if (font->CalcTextSizeA(size, limit, 0.f, begin, end, &remaining).x <= limit && remaining == end) {
        return std::string(text);
    }
    static constexpr const char kEllipsis[] = "…";
    const float ellipsis_w = font->CalcTextSizeA(size, 1000.f, 0.f, kEllipsis).x;
    const float budget = max_width - ellipsis_w;
    if (budget <= 0.f) return std::string(kEllipsis);
    remaining = begin;
    font->CalcTextSizeA(size, budget, 0.f, begin, end, &remaining);
    // Ensure we never split inside a multi-byte UTF-8 sequence.
    while (remaining > begin && (static_cast<unsigned char>(*remaining) & 0xC0u) == 0x80u) --remaining;
    std::string out;
    out.reserve(static_cast<std::size_t>(remaining - begin) + sizeof(kEllipsis));
    out.append(begin, remaining);
    out.append(kEllipsis);
    return out;
}

} // namespace jianssh::ui
