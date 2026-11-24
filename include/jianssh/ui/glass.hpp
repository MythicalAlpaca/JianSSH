#pragma once
// Procedural Apple Liquid Glass optical renderer and crisp vector icons built
// purely on Dear ImGui's public ImDrawList API.
//
// Performance contract:
// - Zero framebuffers, zero real-time blur shaders, zero extra GPU textures.
// - Dual-corner Fresnel rims, gradients and cursor-reactive specular glints
//   shade existing vertices in place (zero extra draw calls, zero square-corner
//   clipping leaks).
// - Vector navigation and SFTP folder/file icons allocate zero atlas memory and
//   draw only for visible rows.
#include "jianssh/ui/theme.hpp"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace jianssh::ui {

enum class NavIcon {
    Home,
    Up,
    Refresh,
    Upload,
    Download,
    NewFolder,
    Close,
    Eye,
    Check
};

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

// Cursor-reactive specular refraction boost applied in place on emitted vertices
// (zero extra triangles, preserves exact rounded-corner AA fringes, zero idle cost).
inline void shade_verts_cursor_glint(ImDrawList* dl, int vtx_start, int vtx_end,
                                     ImVec2 mouse_pos, float radius,
                                     ImU32 glint_col, float strength) noexcept {
    if (!dl || vtx_end <= vtx_start || strength <= 0.005f || radius <= 1.f) return;
    if (!ImGui::IsMousePosValid(&mouse_pos)) return;
    const float inv_r2 = 1.0f / (radius * radius);
    const float gr = static_cast<float>(glint_col & 0xFFu);
    const float gg = static_cast<float>((glint_col >> 8) & 0xFFu);
    const float gb = static_cast<float>((glint_col >> 16) & 0xFFu);
    const float ga = static_cast<float>((glint_col >> 24) & 0xFFu);

    ImDrawVert* verts = dl->VtxBuffer.Data;
    for (int i = vtx_start; i < vtx_end; ++i) {
        ImDrawVert& v = verts[i];
        const float dx = v.pos.x - mouse_pos.x;
        const float dy = v.pos.y - mouse_pos.y;
        const float d2 = (dx * dx + dy * dy) * inv_r2;
        if (d2 >= 1.0f) continue;
        const float falloff = (1.0f - d2) * (1.0f - d2) * std::clamp(strength, 0.0f, 1.0f);
        const float vr = static_cast<float>(v.col & 0xFFu);
        const float vg = static_cast<float>((v.col >> 8) & 0xFFu);
        const float vb = static_cast<float>((v.col >> 16) & 0xFFu);
        const float va = static_cast<float>((v.col >> 24) & 0xFFu);
        const unsigned nr = static_cast<unsigned>(std::clamp(vr + (gr - vr) * falloff * 0.65f, 0.0f, 255.0f) + 0.5f);
        const unsigned ng = static_cast<unsigned>(std::clamp(vg + (gg - vg) * falloff * 0.65f, 0.0f, 255.0f) + 0.5f);
        const unsigned nb = static_cast<unsigned>(std::clamp(vb + (gb - vb) * falloff * 0.65f, 0.0f, 255.0f) + 0.5f);
        const unsigned na = static_cast<unsigned>(std::clamp(va + (ga - va) * falloff * 0.45f, 0.0f, 255.0f) + 0.5f);
        v.col = IM_COL32(nr, ng, nb, na);
    }
}

// Dual-corner Apple Liquid Glass Fresnel rim shader:
// - Primary bright specular catchlight on the top-left curve (d -> -1)
// - Translucent optical mid-edges along top-right / bottom-left (d -> 0)
// - Secondary internal Fresnel reflection glint on the bottom-right curve (d -> +1)
inline void shade_verts_fresnel_rim(ImDrawList* dl, int vtx_start, int vtx_end,
                                    ImVec2 a, ImVec2 b,
                                    ImU32 col_tl, ImU32 col_mid, ImU32 col_br,
                                    float pointer_boost = 0.0f,
                                    float pointer_radius = 120.0f) noexcept {
    if (!dl || vtx_end <= vtx_start) return;
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float inv_hw = 1.0f / std::max(1.0f, (b.x - a.x) * 0.5f);
    const float inv_hh = 1.0f / std::max(1.0f, (b.y - a.y) * 0.5f);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool use_mouse = pointer_boost > 0.005f && pointer_radius > 1.f && ImGui::IsMousePosValid(&mouse);
    const float inv_mr2 = use_mouse ? (1.0f / (pointer_radius * pointer_radius)) : 0.0f;

    ImDrawVert* verts = dl->VtxBuffer.Data;
    for (int i = vtx_start; i < vtx_end; ++i) {
        ImDrawVert& v = verts[i];
        const float nx = std::clamp((v.pos.x - cx) * inv_hw, -1.0f, 1.0f);
        const float ny = std::clamp((v.pos.y - cy) * inv_hh, -1.0f, 1.0f);
        const float d = std::clamp((nx + ny) * 0.5f, -1.0f, 1.0f);

        ImU32 target = col_mid;
        if (d < 0.0f) {
            const float u = -d;
            target = lerp_color(col_mid, col_tl, std::min(1.0f, u * std::sqrt(u) * 1.08f));
        } else {
            target = lerp_color(col_mid, col_br, std::min(1.0f, d * d * 1.12f));
        }

        if (use_mouse) {
            const float mdx = v.pos.x - mouse.x;
            const float mdy = v.pos.y - mouse.y;
            const float md2 = (mdx * mdx + mdy * mdy) * inv_mr2;
            if (md2 < 1.0f) {
                const float w = (1.0f - md2) * (1.0f - md2) * std::clamp(pointer_boost, 0.0f, 1.0f);
                target = lerp_color(target, IM_COL32(255, 255, 255, 245), w * 0.72f);
            }
        }

        const unsigned orig_a = (v.col >> 24) & 0xFFu;
        const unsigned ta = (target >> 24) & 0xFFu;
        const unsigned final_a = (orig_a * ta + 127u) / 255u;
        v.col = (target & 0x00FFFFFFu) | (final_a << 24);
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

// Dual-corner Fresnel refractive rim with optional cursor proximity specular glint.
inline void draw_fresnel_rim(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                             ImU32 top_left, ImU32 mid_edge, ImU32 bottom_right,
                             float thickness = 1.0f, float pointer_boost = 0.0f,
                             float pointer_radius = 120.0f, ImDrawFlags flags = 0) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y) return;
    const int vtx_start = dl->VtxBuffer.Size;
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 255), rounding, flags, thickness);
    shade_verts_fresnel_rim(dl, vtx_start, dl->VtxBuffer.Size, a, b,
                            top_left, mid_edge, bottom_right, pointer_boost, pointer_radius);
}

// Convenience directional Fresnel glass rim (automatically computes a delicate
// translucent mid-edge between the top-left catchlight and bottom-right bounce).
inline void draw_glass_rim(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                           ImU32 top_left, ImU32 bottom_right,
                           float thickness = 1.0f, ImDrawFlags flags = 0,
                           float pointer_boost = 0.0f, float pointer_radius = 110.0f) noexcept {
    const ImU32 mid = fade_color(lerp_color(top_left, bottom_right, 0.52f), 0.42f);
    draw_fresnel_rim(dl, a, b, rounding, top_left, mid, bottom_right,
                     thickness, pointer_boost, pointer_radius, flags);
}

// Delicate optical perimeter caustic contour (replaces dated heavy top-half
// glossy white curtains with a clean curved-edge dispersion ring).
inline void draw_glass_sheen(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                             ImU32 highlight, float max_height = 28.f, float ratio = 0.22f) noexcept {
    if (!dl || b.x - a.x < 5.f || b.y - a.y < 5.f) return;
    const float inset = 1.15f;
    const float inner_r = std::max(0.f, rounding - inset);
    const ImVec2 ia(a.x + inset, a.y + inset);
    const ImVec2 ib(b.x - inset, b.y - inset);
    if (ib.x <= ia.x || ib.y <= ia.y) return;
    // Subtle inner lens dispersion ring around the curved perimeter.
    draw_fresnel_rim(dl, ia, ib, inner_r,
                     fade_color(highlight, 0.52f),
                     fade_color(highlight, 0.12f),
                     fade_color(highlight, 0.28f), 1.0f);
    // Feather-light upper curvature refraction whisper (kept strictly near top lip).
    const float h = ib.y - ia.y;
    const float band_h = std::min(h, std::max(inner_r * 0.65f, std::min(h * ratio * 0.55f, max_height * 0.45f)));
    if (band_h > 2.5f) {
        const ImVec2 sb(ib.x, ia.y + band_h);
        const int vtx_start = dl->VtxBuffer.Size;
        dl->AddRectFilled(ia, sb, IM_COL32(255, 255, 255, 255), inner_r, ImDrawFlags_RoundCornersTop);
        shade_verts_linear_gradient(dl, vtx_start, dl->VtxBuffer.Size, ia, ImVec2(ia.x, sb.y),
                                    fade_color(highlight, 0.32f), fade_color(highlight, 0.0f));
    }
}

// Rotated elliptical Gaussian light ribbon (zero ring banding, zero textures).
inline void draw_soft_ellipse(ImDrawList* dl, ImVec2 center, float rx, float ry,
                              float angle_rad, ImU32 color) noexcept {
    if (!dl || rx <= 1.f || ry <= 1.f || ((color >> 24) & 0xFFu) == 0u) return;
    constexpr int kSegs = 28;
    constexpr float kTwoPi = 6.2831853071795864769f;
    static constexpr float kRingR[4] = {0.26f, 0.54f, 0.78f, 1.00f};

    const float ca = std::cos(angle_rad);
    const float sa = std::sin(angle_rad);
    ImVec2 ring[4][kSegs + 1];
    for (int i = 0; i <= kSegs; ++i) {
        const float ang = kTwoPi * (static_cast<float>(i) / static_cast<float>(kSegs));
        const float cs = std::cos(ang);
        const float sn = std::sin(ang);
        for (int r = 0; r < 4; ++r) {
            const float lx = cs * rx * kRingR[r];
            const float ly = sn * ry * kRingR[r];
            ring[r][i] = ImVec2(center.x + lx * ca - ly * sa,
                                center.y + lx * sa + ly * ca);
        }
    }

    const ImDrawListFlags saved_flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    const int vtx_start = dl->VtxBuffer.Size;
    for (int i = 0; i < kSegs; ++i) {
        dl->AddTriangleFilled(center, ring[0][i], ring[0][i + 1], IM_COL32(255, 255, 255, 255));
        for (int r = 0; r < 3; ++r) {
            dl->AddQuadFilled(ring[r][i], ring[r + 1][i], ring[r + 1][i + 1], ring[r][i + 1],
                              IM_COL32(255, 255, 255, 255));
        }
    }
    dl->Flags = saved_flags;

    const float inv_rx = 1.0f / rx;
    const float inv_ry = 1.0f / ry;
    const unsigned base_r = color & 0xFFu;
    const unsigned base_g = (color >> 8) & 0xFFu;
    const unsigned base_b = (color >> 16) & 0xFFu;
    const float base_a = static_cast<float>((color >> 24) & 0xFFu);
    ImDrawVert* verts = dl->VtxBuffer.Data;
    const int vtx_end = dl->VtxBuffer.Size;
    for (int i = vtx_start; i < vtx_end; ++i) {
        ImDrawVert& v = verts[i];
        const float wx = v.pos.x - center.x;
        const float wy = v.pos.y - center.y;
        const float lx = (wx * ca + wy * sa) * inv_rx;
        const float ly = (-wx * sa + wy * ca) * inv_ry;
        const float u2 = std::min(1.0f, lx * lx + ly * ly);
        const float w = (1.0f - u2) * (1.0f - u2);
        const unsigned a = static_cast<unsigned>(std::clamp(base_a * w, 0.0f, 255.0f) + 0.5f);
        v.col = IM_COL32(base_r, base_g, base_b, a);
    }
}

inline void draw_soft_orb(ImDrawList* dl, ImVec2 center, float radius, ImU32 color) noexcept {
    draw_soft_ellipse(dl, center, radius, radius, 0.0f, color);
}

// Multi-chromatic Apple fluid mesh backdrop: sweeping aurora ribbons positioned
// directly behind floating glass islands so translucency is rich and unmistakable.
inline void draw_ambient_backdrop(ImDrawList* dl, ImVec2 p, ImVec2 q, const Theme& t) noexcept {
    if (!dl || q.x <= p.x || q.y <= p.y) return;
    const float w = q.x - p.x;
    const float h = q.y - p.y;
    const ImU32 top_left = t.dark ? lerp_color(t.window_bg, t.accent, 0.14f)
                                  : lerp_color(t.window_bg, IM_COL32(255, 255, 255, 255), 0.58f);
    const ImU32 top_right = t.dark ? lerp_color(t.window_bg, t.glass_glow_c, 0.11f)
                                   : lerp_color(t.window_bg, t.glass_glow_c, 0.16f);
    const ImU32 bot_right = t.dark ? lerp_color(t.window_bg, t.glass_glow_b, 0.12f)
                                   : lerp_color(t.window_bg, t.glass_glow_b, 0.14f);
    const ImU32 bot_left = t.dark ? lerp_color(t.window_bg, t.glass_glow_d, 0.08f)
                                  : lerp_color(t.window_bg, t.glass_glow_a, 0.10f);
    dl->AddRectFilledMultiColor(p, q, top_left, top_right, bot_right, bot_left);

    dl->PushClipRect(p, q, true);
    // 1. Sweeping sapphire ribbon across upper-left (illuminates sidebar & tabs).
    draw_soft_ellipse(dl, ImVec2(p.x + w * 0.19f, p.y + h * 0.20f),
                      std::max(300.f, w * 0.44f), std::max(210.f, h * 0.38f), -0.34f, t.glass_glow_a);
    // 2. Prismatic cyan-aqua ribbon across upper-center/right (illuminates SFTP split & hero).
    draw_soft_ellipse(dl, ImVec2(p.x + w * 0.62f, p.y + h * 0.18f),
                      std::max(280.f, w * 0.40f), std::max(185.f, h * 0.34f), 0.28f, t.glass_glow_c);
    // 3. Amethyst-violet bloom in lower-right (illuminates remote SFTP pane & terminal).
    draw_soft_ellipse(dl, ImVec2(p.x + w * 0.82f, p.y + h * 0.74f),
                      std::max(320.f, w * 0.46f), std::max(240.f, h * 0.44f), -0.42f, t.glass_glow_b);
    // 4. Warm rose-magenta ribbon in lower-left/center (illuminates device list & queue).
    draw_soft_ellipse(dl, ImVec2(p.x + w * 0.30f, p.y + h * 0.84f),
                      std::max(260.f, w * 0.38f), std::max(190.f, h * 0.34f), 0.36f, t.glass_glow_d);
    // 5. Soft central refraction bridge so modal sheets & center workspace glow gently.
    draw_soft_ellipse(dl, ImVec2(p.x + w * 0.50f, p.y + h * 0.48f),
                      std::max(240.f, w * 0.32f), std::max(180.f, h * 0.28f), -0.18f,
                      fade_color(t.glass_glow_a, 0.52f));
    dl->PopClipRect();
}

// Floating Apple Liquid Glass island container (sidebar, workspace, nested SFTP cards).
inline void draw_glass_panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                             const Theme& t, float alpha_scale = 1.f,
                             bool sidebar = false, float accent_amount = 0.04f,
                             bool nested = false) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y || alpha_scale <= 0.01f) return;

    // Multi-layered diffused elevation shadow (weightless floating island depth).
    const float sh_scale = (nested ? 0.55f : 1.0f) * alpha_scale;
    dl->AddRectFilled(ImVec2(a.x - 3.f, a.y + 5.f), ImVec2(b.x + 3.f, b.y + 11.f),
                      fade_color(t.shadow, (t.dark ? 0.28f : 0.20f) * sh_scale), rounding + 4.f);
    dl->AddRectFilled(ImVec2(a.x - 1.f, a.y + 2.5f), ImVec2(b.x + 1.f, b.y + 5.5f),
                      fade_color(t.shadow, (t.dark ? 0.34f : 0.22f) * sh_scale), rounding + 1.5f);

    // Translucent crystal body: nested cards use additive ice-white frost in dark
    // mode instead of stacking dark slate, keeping true visionOS glass clarity.
    ImU32 top_col = 0;
    ImU32 bot_col = 0;
    if (nested) {
        top_col = fade_color(
            t.dark ? lerp_color(IM_COL32(226, 240, 255, 26), fade_color(t.accent_hover, 0.16f), accent_amount * 2.2f)
                   : IM_COL32(255, 255, 255, 138),
            alpha_scale);
        bot_col = fade_color(
            t.dark ? IM_COL32(180, 210, 255, 12)
                   : IM_COL32(246, 251, 255, 98),
            alpha_scale);
    } else {
        const ImU32 base = sidebar ? t.sidebar_bg : t.child_bg;
        top_col = fade_color(
            t.dark ? lerp_color(base, lerp_color(IM_COL32(44, 66, 108, 132), t.accent, accent_amount), 0.26f)
                   : lerp_color(base, IM_COL32(255, 255, 255, 168), 0.34f),
            alpha_scale);
        bot_col = fade_color(
            t.dark ? lerp_color(base, IM_COL32(10, 16, 30, 96), 0.22f)
                   : lerp_color(base, IM_COL32(236, 245, 255, 112), 0.20f),
            alpha_scale);
    }

    const int body_vtx = dl->VtxBuffer.Size;
    draw_gradient_rounded_rect(dl, a, b, top_col, bot_col, rounding, 0, true);
    if (nested) {
        shade_verts_cursor_glint(dl, body_vtx, dl->VtxBuffer.Size, ImGui::GetIO().MousePos,
                                 220.f, IM_COL32(255, 255, 255, t.dark ? 48 : 110), 0.35f * alpha_scale);
    }

    // Optical inner dispersion ring (clear center, curved perimeter caustic).
    const ImVec2 ia(a.x + 1.2f, a.y + 1.2f);
    const ImVec2 ib(b.x - 1.2f, b.y - 1.2f);
    if (ib.x > ia.x && ib.y > ia.y) {
        draw_fresnel_rim(dl, ia, ib, std::max(0.f, rounding - 1.2f),
                         fade_color(IM_COL32(255, 255, 255, 255), (t.dark ? 0.14f : 0.48f) * alpha_scale),
                         fade_color(IM_COL32(255, 255, 255, 255), (t.dark ? 0.03f : 0.14f) * alpha_scale),
                         fade_color(t.glass_rim_bottom, (t.dark ? 0.22f : 0.28f) * alpha_scale), 1.0f);
    }

    // Crisp 1px top specular catchlight along the upper straight lip.
    const float inset = std::min(rounding * 0.88f, (b.x - a.x) * 0.25f);
    if (b.x - a.x > inset * 2.f + 4.f) {
        dl->AddLine(ImVec2(a.x + inset, a.y + 0.8f), ImVec2(b.x - inset, a.y + 0.8f),
                    fade_color(IM_COL32(255, 255, 255, 255), (t.dark ? 0.48f : 0.88f) * alpha_scale), 1.0f);
    }

    // Dual-corner Fresnel outer specular rim with subtle cursor refraction.
    draw_fresnel_rim(dl, a, b, rounding,
                     fade_color(t.glass_rim_top, (nested ? 0.82f : 0.96f) * alpha_scale),
                     fade_color(t.border, (t.dark ? 0.46f : 0.62f) * alpha_scale),
                     fade_color(t.glass_rim_bottom, (nested ? 0.78f : 0.92f) * alpha_scale),
                     1.05f, 0.28f * alpha_scale, 160.f);
}

// Recessed frosted crystal well (segmented tracks, breadcrumb bar, metrics pill).
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
        draw_fresnel_rim(dl, a, b, rounding,
                         fade_color(t.border, t.dark ? 0.72f : 0.78f),
                         fade_color(t.border, t.dark ? 0.34f : 0.42f),
                         fade_color(t.glass_rim_top, t.dark ? 0.48f : 0.86f),
                         1.0f, hover * 0.35f, 95.f);
    }
}

// Sliding Apple Liquid Glass droplet capsule (segmented thumb, active tab,
// active group, selected device card).
inline void draw_jelly_pill(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding,
                            float stretch_x, float stretch_y, const Theme& t,
                            bool accented = false, float alpha = 1.f,
                            float glint = 0.f) noexcept {
    if (!dl || b.x <= a.x || b.y <= a.y || alpha <= 0.01f) return;
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const float dx = std::clamp(w * stretch_x * 0.5f, -5.f, 8.f);
    const float dy = std::clamp(h * stretch_y * 0.5f, -3.5f, 5.f);
    const ImVec2 pa(cx - w * 0.5f - dx, cy - h * 0.5f - dy);
    const ImVec2 pb(cx + w * 0.5f + dx, cy + h * 0.5f + dy);
    const float r = std::min(rounding, std::min(pb.x - pa.x, pb.y - pa.y) * 0.5f);

    // Floating liquid lens drop shadow.
    dl->AddRectFilled(ImVec2(pa.x, pa.y + 2.0f), ImVec2(pb.x, pb.y + 4.0f),
                      fade_color(t.shadow, (t.dark ? 0.44f : (accented ? 0.22f : 0.28f)) * alpha), r + 1.0f);

    // Translucent crystal droplet body (clear interior, lets aurora bleed through).
    ImU32 fill_top = 0, fill_bot = 0;
    if (accented) {
        fill_top = fade_color(t.dark ? IM_COL32(56, 162, 255, 76)
                                     : IM_COL32(226, 240, 255, 196), alpha);
        fill_bot = fade_color(t.dark ? IM_COL32(18, 118, 242, 48)
                                     : IM_COL32(198, 226, 255, 168), alpha);
    } else {
        fill_top = fade_color(t.dark ? IM_COL32(238, 246, 255, 58)
                                     : IM_COL32(255, 255, 255, 224), alpha);
        fill_bot = fade_color(t.dark ? IM_COL32(210, 230, 255, 32)
                                     : IM_COL32(242, 248, 255, 186), alpha);
    }

    const int vtx_start = dl->VtxBuffer.Size;
    draw_gradient_rounded_rect(dl, pa, pb, fill_top, fill_bot, r, 0, true);

    // Velocity-driven sweeping specular glint across the moving droplet + cursor glint.
    const float motion_glint = std::clamp(std::abs(glint), 0.f, 1.f);
    if (motion_glint > 0.015f) {
        const ImVec2 glint_pos(cx + (glint > 0.f ? 1.f : -1.f) * w * 0.22f, pa.y + h * 0.25f);
        shade_verts_cursor_glint(dl, vtx_start, dl->VtxBuffer.Size, glint_pos,
                                 std::max(w, h) * 0.85f, IM_COL32(255, 255, 255, 220),
                                 motion_glint * 0.65f * alpha);
    }
    shade_verts_cursor_glint(dl, vtx_start, dl->VtxBuffer.Size, ImGui::GetIO().MousePos,
                             96.f, IM_COL32(255, 255, 255, 200), 0.38f * alpha);

    // Inner lens caustic contour.
    if (pb.x - pa.x > 6.f && pb.y - pa.y > 6.f) {
        draw_fresnel_rim(dl, ImVec2(pa.x + 1.0f, pa.y + 1.0f), ImVec2(pb.x - 1.0f, pb.y - 1.0f),
                         std::max(0.f, r - 1.0f),
                         fade_color(IM_COL32(255, 255, 255, 255), (t.dark ? 0.24f : 0.72f) * alpha),
                         fade_color(IM_COL32(255, 255, 255, 255), (t.dark ? 0.05f : 0.22f) * alpha),
                         fade_color(accented ? t.accent_hover : IM_COL32(255, 255, 255, 255),
                                    (t.dark ? 0.18f : 0.45f) * alpha), 0.9f);
    }

    // Dual-corner Fresnel outer rim.
    const ImU32 rim_tl = accented
        ? fade_color(lerp_color(IM_COL32(255, 255, 255, 235), t.accent_hover, t.dark ? 0.34f : 0.24f), alpha)
        : fade_color(IM_COL32(255, 255, 255, t.dark ? 196 : 252), alpha);
    const ImU32 rim_mid = accented
        ? fade_color(t.accent, (t.dark ? 0.34f : 0.28f) * alpha)
        : fade_color(t.border, (t.dark ? 0.45f : 0.52f) * alpha);
    const ImU32 rim_br = accented
        ? fade_color(t.accent_hover, (t.dark ? 0.62f : 0.48f) * alpha)
        : fade_color(t.glass_rim_bottom, (t.dark ? 0.85f : 0.90f) * alpha);
    draw_fresnel_rim(dl, pa, pb, r, rim_tl, rim_mid, rim_br, 1.05f, 0.35f * alpha, 90.f);
}

// Floating Apple Liquid Glass sheet modal chrome: diffused outer halo, inner
// perimeter dispersion ring and dual-corner Fresnel specular border.
inline void draw_glass_modal_chrome(ImDrawList* dl, ImVec2 pos, ImVec2 size, float rounding, const Theme& t) noexcept {
    if (!dl || size.x <= 4.f || size.y <= 4.f) return;
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    const ImVec2 rim_a(pos.x + 0.5f, pos.y + 0.5f);
    const ImVec2 rim_b(b.x - 0.5f, b.y - 0.5f);
    const float r = std::max(0.f, rounding - 0.5f);
    dl->PushClipRect(ImVec2(pos.x - 16.f, pos.y - 12.f), ImVec2(b.x + 16.f, b.y + 22.f), false);
    // Subtle inner top-left crystal luminance lift inside the modal sheet.
    draw_gradient_rounded_rect(dl, rim_a, rim_b,
                               fade_color(IM_COL32(255, 255, 255, 255), t.dark ? 0.055f : 0.32f),
                               fade_color(t.accent, t.dark ? 0.025f : 0.03f), r, 0, true);
    if (size.x > 8.f && size.y > 8.f) {
        draw_fresnel_rim(dl, ImVec2(pos.x + 1.6f, pos.y + 1.6f), ImVec2(b.x - 1.6f, b.y - 1.6f),
                         std::max(0.f, rounding - 1.6f),
                         fade_color(IM_COL32(255, 255, 255, 255), t.dark ? 0.18f : 0.65f),
                         fade_color(IM_COL32(255, 255, 255, 255), t.dark ? 0.04f : 0.18f),
                         fade_color(t.glass_rim_bottom, 0.32f), 1.0f);
    }
    const ImU32 hi = t.dark ? IM_COL32(250, 253, 255, 210) : IM_COL32(255, 255, 255, 255);
    const ImU32 mid = fade_color(t.border, t.dark ? 0.55f : 0.65f);
    const ImU32 lo = t.dark ? t.glass_rim_bottom : lerp_color(t.glass_rim_bottom, t.accent, 0.22f);
    draw_fresnel_rim(dl, rim_a, rim_b, r, hi, mid, lo, 1.2f, 0.25f, 180.f);
    dl->PopClipRect();
}

// Context-menu / dropdown popup chrome with dual-corner Fresnel glass rim.
inline void draw_glass_popup_chrome(ImDrawList* dl, ImVec2 pos, ImVec2 size, float rounding, const Theme& t) noexcept {
    if (!dl || size.x <= 4.f || size.y <= 4.f) return;
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    const ImVec2 rim_a(pos.x + 0.5f, pos.y + 0.5f);
    const ImVec2 rim_b(b.x - 0.5f, b.y - 0.5f);
    const float r = std::max(0.f, rounding - 0.5f);
    dl->PushClipRect(ImVec2(pos.x - 6.f, pos.y - 6.f), ImVec2(b.x + 6.f, b.y + 10.f), false);
    draw_gradient_rounded_rect(dl, rim_a, rim_b,
                               fade_color(IM_COL32(255, 255, 255, 255), t.dark ? 0.05f : 0.28f),
                               IM_COL32(255, 255, 255, 0), r, 0, true);
    const ImU32 hi = t.dark ? IM_COL32(250, 253, 255, 198) : IM_COL32(255, 255, 255, 255);
    const ImU32 mid = fade_color(t.border, t.dark ? 0.50f : 0.62f);
    const ImU32 lo = t.glass_rim_bottom;
    draw_fresnel_rim(dl, rim_a, rim_b, r, hi, mid, lo, 1.1f);
    dl->PopClipRect();
}

// App brand squircle with layered translucent Apple Liquid Glass depth.
inline void draw_brand_icon_glass(ImDrawList* dl, ImVec2 pos, float size, const Theme& t) noexcept {
    if (!dl || size <= 2.f) return;
    const ImVec2 b(pos.x + size, pos.y + size);
    const float r = size * 0.29f;
    dl->AddRectFilled(ImVec2(pos.x, pos.y + size * 0.06f), ImVec2(b.x, b.y + size * 0.10f),
                      fade_color(t.accent, t.dark ? 0.32f : 0.22f), r + 2.f);
    draw_gradient_rounded_rect(dl, pos, b,
                               lerp_color(t.accent_hover, IM_COL32(120, 224, 255, 255), 0.34f),
                               lerp_color(t.accent, IM_COL32(58, 72, 232, 255), 0.32f), r, 0, true);
    draw_glass_sheen(dl, pos, b, r, fade_color(IM_COL32(255, 255, 255, 255), 0.62f), size * 0.38f, 0.36f);
    draw_fresnel_rim(dl, pos, b, r,
                     IM_COL32(255, 255, 255, 220),
                     IM_COL32(255, 255, 255, 72),
                     IM_COL32(150, 230, 255, 165), 1.05f);
    const float stroke = std::max(1.6f, size * 0.060f);
    const ImU32 white = IM_COL32(255, 255, 255, 250);
    dl->AddLine(ImVec2(pos.x + size * 0.26f, pos.y + size * 0.38f),
                ImVec2(pos.x + size * 0.42f, pos.y + size * 0.50f), white, stroke);
    dl->AddLine(ImVec2(pos.x + size * 0.42f, pos.y + size * 0.50f),
                ImVec2(pos.x + size * 0.26f, pos.y + size * 0.62f), white, stroke);
    dl->AddLine(ImVec2(pos.x + size * 0.50f, pos.y + size * 0.62f),
                ImVec2(pos.x + size * 0.72f, pos.y + size * 0.62f), white, stroke);
}

// ---- Crisp procedural vector navigation & SFTP icons -----------------------

inline void draw_nav_icon(ImDrawList* dl, ImVec2 center, float size, NavIcon icon, ImU32 color) noexcept {
    if (!dl || size <= 3.f) return;
    const float r = size * 0.5f;
    const float sw = std::max(1.25f, size * 0.115f);
    switch (icon) {
        case NavIcon::Home: {
            const ImVec2 roof[3] = {
                ImVec2(center.x - r * 0.86f, center.y - r * 0.05f),
                ImVec2(center.x, center.y - r * 0.82f),
                ImVec2(center.x + r * 0.86f, center.y - r * 0.05f),
            };
            dl->AddPolyline(roof, 3, color, ImDrawFlags_None, sw);
            const ImVec2 body[4] = {
                ImVec2(center.x - r * 0.56f, center.y - r * 0.12f),
                ImVec2(center.x - r * 0.56f, center.y + r * 0.76f),
                ImVec2(center.x + r * 0.56f, center.y + r * 0.76f),
                ImVec2(center.x + r * 0.56f, center.y - r * 0.12f),
            };
            dl->AddPolyline(body, 4, color, ImDrawFlags_None, sw);
            break;
        }
        case NavIcon::Up: {
            dl->AddLine(ImVec2(center.x, center.y + r * 0.76f),
                        ImVec2(center.x, center.y - r * 0.72f), color, sw);
            const ImVec2 head[3] = {
                ImVec2(center.x - r * 0.56f, center.y - r * 0.16f),
                ImVec2(center.x, center.y - r * 0.74f),
                ImVec2(center.x + r * 0.56f, center.y - r * 0.16f),
            };
            dl->AddPolyline(head, 3, color, ImDrawFlags_None, sw);
            break;
        }
        case NavIcon::Refresh: {
            const float rad = r * 0.68f;
            constexpr float end_ang = 5.75f;
            dl->PathArcTo(center, rad, 0.55f, end_ang, 18);
            dl->PathStroke(color, ImDrawFlags_None, sw);
            const ImVec2 tip(center.x + std::cos(end_ang) * rad, center.y + std::sin(end_ang) * rad);
            dl->AddLine(tip, ImVec2(tip.x - r * 0.46f, tip.y - r * 0.08f), color, sw);
            dl->AddLine(tip, ImVec2(tip.x - r * 0.06f, tip.y + r * 0.46f), color, sw);
            break;
        }
        case NavIcon::Upload:
        case NavIcon::Download: {
            const bool up = (icon == NavIcon::Upload);
            const float y_top = center.y - r * 0.74f;
            const float y_bot = center.y + r * 0.38f;
            dl->AddLine(ImVec2(center.x, y_top), ImVec2(center.x, y_bot), color, sw);
            if (up) {
                const ImVec2 head[3] = {
                    ImVec2(center.x - r * 0.46f, y_top + r * 0.46f),
                    ImVec2(center.x, y_top),
                    ImVec2(center.x + r * 0.46f, y_top + r * 0.46f),
                };
                dl->AddPolyline(head, 3, color, ImDrawFlags_None, sw);
            } else {
                const ImVec2 head[3] = {
                    ImVec2(center.x - r * 0.46f, y_bot - r * 0.46f),
                    ImVec2(center.x, y_bot),
                    ImVec2(center.x + r * 0.46f, y_bot - r * 0.46f),
                };
                dl->AddPolyline(head, 3, color, ImDrawFlags_None, sw);
            }
            dl->AddLine(ImVec2(center.x - r * 0.66f, center.y + r * 0.76f),
                        ImVec2(center.x + r * 0.66f, center.y + r * 0.76f), color, sw);
            break;
        }
        case NavIcon::NewFolder: {
            dl->AddLine(ImVec2(center.x - r * 0.68f, center.y),
                        ImVec2(center.x + r * 0.68f, center.y), color, sw);
            dl->AddLine(ImVec2(center.x, center.y - r * 0.68f),
                        ImVec2(center.x, center.y + r * 0.68f), color, sw);
            break;
        }
        case NavIcon::Close: {
            const float d = r * 0.56f;
            dl->AddLine(ImVec2(center.x - d, center.y - d), ImVec2(center.x + d, center.y + d), color, sw);
            dl->AddLine(ImVec2(center.x + d, center.y - d), ImVec2(center.x - d, center.y + d), color, sw);
            break;
        }
        case NavIcon::Eye: {
            dl->AddCircle(center, r * 0.62f, color, 16, sw * 0.9f);
            dl->AddCircleFilled(center, r * 0.24f, color, 10);
            break;
        }
        case NavIcon::Check: {
            const ImVec2 pts[3] = {
                ImVec2(center.x - r * 0.62f, center.y + r * 0.04f),
                ImVec2(center.x - r * 0.14f, center.y + r * 0.54f),
                ImVec2(center.x + r * 0.68f, center.y - r * 0.46f),
            };
            dl->AddPolyline(pts, 3, color, ImDrawFlags_None, sw * 1.05f);
            break;
        }
    }
}

// macOS Tahoe translucent Liquid Glass folder icon (zero bitmap/atlas overhead).
inline void draw_folder_icon(ImDrawList* dl, ImVec2 pos, float size, const Theme& t,
                             bool drop_highlight = false) noexcept {
    if (!dl || size <= 4.f) return;
    const float w = size;
    const float h = size * 0.84f;
    const float x = pos.x;
    const float y = pos.y + (size - h) * 0.5f;
    const float r = std::max(1.8f, size * 0.19f);

    const ImU32 back_col = drop_highlight ? lerp_color(t.folder_back, t.info, 0.45f) : t.folder_back;
    const ImU32 top_col = drop_highlight ? lerp_color(t.folder_front_top, IM_COL32(255, 255, 255, 255), 0.30f)
                                         : t.folder_front_top;
    const ImU32 bot_col = drop_highlight ? lerp_color(t.folder_front_bottom, t.info, 0.28f)
                                         : t.folder_front_bottom;

    // Soft cyan-blue ambient drop glow beneath the glass folder.
    dl->AddRectFilled(ImVec2(x + 0.8f, y + h * 0.36f + 1.2f), ImVec2(x + w - 0.5f, y + h + 1.4f),
                      fade_color(t.shadow, t.dark ? 0.46f : 0.26f), r);

    // Back tab ear + sloped shoulder + upper back plate.
    dl->AddRectFilled(ImVec2(x + 0.5f, y), ImVec2(x + w * 0.46f, y + h * 0.44f),
                      back_col, std::max(1.4f, size * 0.15f), ImDrawFlags_RoundCornersTop);
    const ImVec2 shoulder[3] = {
        ImVec2(x + w * 0.40f, y + h * 0.04f),
        ImVec2(x + w * 0.58f, y + h * 0.21f),
        ImVec2(x + w * 0.40f, y + h * 0.32f),
    };
    dl->AddTriangleFilled(shoulder[0], shoulder[1], shoulder[2], back_col);
    dl->AddRectFilled(ImVec2(x + 0.5f, y + h * 0.17f), ImVec2(x + w - 0.5f, y + h * 0.64f),
                      back_col, std::max(1.4f, size * 0.15f), ImDrawFlags_RoundCornersTopRight);

    // Frosted inner paper sheet peeking inside the translucent glass pocket.
    dl->AddRectFilled(ImVec2(x + w * 0.13f, y + h * 0.22f), ImVec2(x + w * 0.87f, y + h * 0.42f),
                      fade_color(IM_COL32(255, 255, 255, 255), t.dark ? 0.80f : 0.94f), 1.4f);

    // Front translucent Liquid Glass pocket with diagonal luminance gradient.
    const ImVec2 fa(x + 0.5f, y + h * 0.30f);
    const ImVec2 fb(x + w - 0.5f, y + h);
    draw_gradient_rounded_rect(dl, fa, fb, top_col, bot_col, r, 0, true);

    // Crisp specular lip highlight & dual-corner Fresnel glass rim.
    dl->AddLine(ImVec2(fa.x + 2.2f, fa.y + 0.8f), ImVec2(fb.x - 2.2f, fa.y + 0.8f),
                fade_color(IM_COL32(255, 255, 255, 255), 0.78f), 1.0f);
    draw_fresnel_rim(dl, fa, fb, r,
                     IM_COL32(255, 255, 255, 168),
                     IM_COL32(255, 255, 255, 48),
                     IM_COL32(180, 238, 255, 132), 0.9f);
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
        return IM_COL32(176, 136, 255, 255);
    if (eq("png") || eq("jpg") || eq("jpeg") || eq("gif") || eq("webp") || eq("svg") || eq("bmp"))
        return t.success;
    if (eq("pem") || eq("key") || eq("pub") || eq("crt") || eq("cer"))
        return t.danger;
    return t.muted;
}

// Frosted crystal document icon for regular files.
inline void draw_file_icon(ImDrawList* dl, ImVec2 pos, float size, std::string_view filename, const Theme& t) noexcept {
    if (!dl || size <= 4.f) return;
    const float w = size * 0.78f;
    const float h = size * 0.92f;
    const float x = pos.x + (size - w) * 0.5f;
    const float y = pos.y + (size - h) * 0.5f;
    const float fold = size * 0.25f;
    const ImVec2 a(x, y);
    const ImVec2 b(x + w, y + h);

    dl->AddRectFilled(ImVec2(a.x + 0.6f, a.y + 1.2f), ImVec2(b.x + 0.6f, b.y + 1.2f),
                      fade_color(t.shadow, t.dark ? 0.36f : 0.20f), 2.4f);

    const ImVec2 body[5] = {
        ImVec2(a.x, a.y),
        ImVec2(b.x - fold, a.y),
        ImVec2(b.x, a.y + fold),
        ImVec2(b.x, b.y),
        ImVec2(a.x, b.y),
    };
    const int vtx_start = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(body, 5, IM_COL32(255, 255, 255, 255));
    const ImU32 page_top = t.dark ? IM_COL32(236, 244, 255, 204) : IM_COL32(255, 255, 255, 248);
    const ImU32 page_bot = t.dark ? IM_COL32(174, 194, 226, 168) : IM_COL32(224, 234, 250, 236);
    shade_verts_linear_gradient(dl, vtx_start, dl->VtxBuffer.Size, a, b, page_top, page_bot);

    const ImVec2 ear[3] = {
        ImVec2(b.x - fold, a.y),
        ImVec2(b.x, a.y + fold),
        ImVec2(b.x - fold, a.y + fold),
    };
    dl->AddTriangleFilled(ear[0], ear[1], ear[2],
                          t.dark ? IM_COL32(130, 152, 188, 220) : IM_COL32(176, 194, 224, 240));

    dl->AddPolyline(body, 5,
                    t.dark ? fade_color(IM_COL32(255, 255, 255, 255), 0.56f) : IM_COL32(110, 134, 174, 132),
                    ImDrawFlags_Closed, 0.95f);

    const ImU32 accent = file_accent_color(filename, t);
    dl->AddRectFilled(ImVec2(a.x + w * 0.20f, a.y + h * 0.44f),
                      ImVec2(a.x + w * 0.78f, a.y + h * 0.54f), fade_color(accent, 0.90f), 0.9f);
    dl->AddRectFilled(ImVec2(a.x + w * 0.20f, a.y + h * 0.64f),
                      ImVec2(a.x + w * 0.64f, a.y + h * 0.72f), fade_color(t.muted, 0.50f), 0.8f);
}

// Symlink icon: file/folder base with luminous glass link-arrow badge.
inline void draw_symlink_icon(ImDrawList* dl, ImVec2 pos, float size, bool is_dir,
                              std::string_view filename, const Theme& t) noexcept {
    if (is_dir) draw_folder_icon(dl, pos, size, t, false);
    else draw_file_icon(dl, pos, size, filename, t);
    const float badge = size * 0.48f;
    const ImVec2 ba(pos.x + size - badge, pos.y + size - badge);
    const ImVec2 bb(pos.x + size + 0.5f, pos.y + size + 0.5f);
    dl->AddRectFilled(ba, bb, t.dark ? IM_COL32(22, 32, 54, 232) : IM_COL32(255, 255, 255, 244), badge * 0.38f);
    dl->AddRect(ba, bb, fade_color(t.info, 0.86f), badge * 0.38f, 0, 0.9f);
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
    dl->AddConvexPolyFilled(pts, 4, fade_color(t.warning, t.dark ? 0.52f : 0.42f));
    dl->AddPolyline(pts, 4, fade_color(t.warning, 0.92f), ImDrawFlags_Closed, 1.0f);
    dl->AddCircleFilled(c, std::max(1.2f, size * 0.10f), IM_COL32(255, 255, 255, 225), 10);
}

// Header icon badge for SFTP local vs remote panes.
inline void draw_pane_badge_icon(ImDrawList* dl, ImVec2 pos, float size, bool remote, const Theme& t) noexcept {
    if (!dl || size <= 4.f) return;
    if (!remote) {
        draw_folder_icon(dl, pos, size, t, false);
        return;
    }
    draw_folder_icon(dl, pos, size, t, true);
    const ImVec2 c(pos.x + size * 0.78f, pos.y + size * 0.78f);
    dl->AddCircleFilled(c, size * 0.24f, t.dark ? IM_COL32(16, 26, 46, 236) : IM_COL32(255, 255, 255, 246), 14);
    dl->AddCircleFilled(c, size * 0.14f, t.success, 12);
}

// Single-pass UTF-8 elision using ImFont::CalcTextSizeA(..., &remaining):
// eliminates per-character substring allocations on long SFTP filenames/paths.
inline std::string elide_fast(ImFont* font, float size, std::string_view text, float max_width) {
    if (!font || max_width <= 0.f || text.empty()) return {};
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const char* remaining = nullptr;
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
    while (remaining > begin && (static_cast<unsigned char>(*remaining) & 0xC0u) == 0x80u) --remaining;
    std::string out;
    out.reserve(static_cast<std::size_t>(remaining - begin) + sizeof(kEllipsis));
    out.append(begin, remaining);
    out.append(kEllipsis);
    return out;
}

} // namespace jianssh::ui
