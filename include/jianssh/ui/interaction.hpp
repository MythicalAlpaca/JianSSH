#pragma once
// Interaction primitives shared by every surface: motion tokens, allocation-free
// hash-keyed hover/press animators, spring toggle switches, liquid jelly sliding
// pill indicators, smooth wheel scroll physics and the loading indicator.
//
// Every animator uses monotonic timestamps and strictly finite settling windows
// so idle windows drop to 0 FPS and stale entries are pruned automatically.
#include "jianssh/ui/motion.hpp"
#include "jianssh/ui/theme.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace jianssh::ui {

// Allocation-free 64-bit FNV-1a keys for per-widget animation state.
constexpr std::uint64_t anim_key(std::string_view label) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : label) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

constexpr std::uint64_t anim_key(std::string_view prefix, std::uint64_t id) noexcept {
    std::uint64_t h = anim_key(prefix);
    h ^= 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < 8; ++i) {
        h ^= (id >> (i * 8)) & 0xFFu;
        h *= 1099511628211ULL;
    }
    return h;
}

// Every duration in the interface, in seconds unless the name says pixels.
// Durations stay short so liquid motion feels responsive rather than sluggish.
struct MotionTokens {
    double entrance = 0.44;
    double selection = 0.22;
    double hover_in = 0.12;
    double hover_out = 0.22;
    double modal = 0.30;
    double modal_slide = 18.0;
    double theme = 0.34;
    double notice = 0.24;
    double loading_step = 0.34;
    double progress = 0.22;
    // Page transitions: crisp fade out, liquid-spring directional glide in.
    double page_out = 0.10;
    double page_in = 0.32;
    double page_slide = 20.0;
    double page_out_slide = 8.0;
    double toggle = 0.26;
};

inline constexpr MotionTokens motion{};

// Eased hover tint + elastic jelly press feedback per widget identity.
class HoverAnimator {
public:
    struct State {
        float hover = 0.f;
        float press = 0.f; // 0 at rest, >0 compressed, slightly <0 on jelly rebound
    };

    void begin_frame(std::uint64_t frame_id) noexcept {
        frame_ = frame_id;
        if (entries_.size() > 96 && (frame_ & 63u) == 0u) {
            std::erase_if(entries_, [this](const auto& item) {
                return !item.second.tween.active() && !item.second.press.active() &&
                       frame_ - item.second.last_seen > 240;
            });
        }
    }

    [[nodiscard]] float factor(std::uint64_t key, MotionClock::time_point now, bool reduced_motion) {
        return state(key, now, reduced_motion).hover;
    }
    [[nodiscard]] float factor(std::string_view id, MotionClock::time_point now, bool reduced_motion) {
        return state(anim_key(id), now, reduced_motion).hover;
    }

    [[nodiscard]] State state(std::uint64_t key, MotionClock::time_point now, bool reduced_motion) {
        Entry& entry = entries_[key];
        entry.last_seen = frame_;
        entry.tween.update(now, reduced_motion);
        entry.press.update(now, reduced_motion);
        return {static_cast<float>(entry.tween.value()), static_cast<float>(entry.press.value())};
    }
    [[nodiscard]] State state(std::string_view id, MotionClock::time_point now, bool reduced_motion) {
        return state(anim_key(id), now, reduced_motion);
    }

    void observe(std::uint64_t key, bool hovered, MotionClock::time_point now, bool reduced_motion,
                 bool pressed = false) {
        Entry& entry = entries_[key];
        entry.last_seen = frame_;
        if (entry.hovered != hovered) {
            entry.hovered = hovered;
            entry.tween.retarget(hovered ? 1.0 : 0.0, now,
                                 std::chrono::duration<double>(hovered ? motion.hover_in : motion.hover_out),
                                 Easing::Smoothstep, reduced_motion);
        }
        if (entry.pressed != pressed) {
            entry.pressed = pressed;
            if (!entry.press_init) {
                entry.press_init = true;
                entry.press.set_params({28.0, 0.56, 0.004, 0.03, 0.34});
            }
            entry.press.retarget(pressed ? 1.0 : 0.0, now, reduced_motion);
            if (!pressed && !reduced_motion) {
                // Soft jelly rebound impulse upon release.
                entry.press.impulse(-3.2, now, false);
            }
        }
    }
    void observe(std::string_view id, bool hovered, MotionClock::time_point now, bool reduced_motion,
                 bool pressed = false) {
        observe(anim_key(id), hovered, now, reduced_motion, pressed);
    }

    [[nodiscard]] bool active() const noexcept {
        for (const auto& entry : entries_)
            if (entry.second.tween.active() || entry.second.press.active()) return true;
        return false;
    }

private:
    struct Entry {
        Tween tween{0.0};
        Spring press{0.0};
        std::uint64_t last_seen = 0;
        bool hovered = false;
        bool pressed = false;
        bool press_init = false;
    };
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t frame_ = 0;
};

// Liquid Glass toggle switch: the crystal knob glides with underdamped spring
// physics and elongates into a sleek droplet proportional to velocity.
class ToggleAnimator {
public:
    struct State {
        float pos = 0.f;     // Smooth spring position (0..1 with slight soft overshoot)
        float stretch = 0.f; // 0..0.38 horizontal droplet elongation while moving
    };

    [[nodiscard]] State state(std::uint64_t key, bool on, MotionClock::time_point now, bool reduced_motion) {
        Entry& entry = entries_[key];
        if (!entry.initialized) {
            entry.initialized = true;
            entry.target = on;
            entry.spring.set_params({25.0, 0.62, 0.003, 0.025, 0.44});
            entry.spring.snap(on ? 1.0 : 0.0);
        }
        if (entry.target != on) {
            entry.target = on;
            entry.spring.retarget(on ? 1.0 : 0.0, now, reduced_motion);
        }
        const float pos = static_cast<float>(entry.spring.update(now, reduced_motion));
        const float stretch = reduced_motion ? 0.f : entry.spring.stretch(0.055f, 0.36f);
        return {pos, stretch};
    }
    [[nodiscard]] State state(std::string_view id, bool on, MotionClock::time_point now, bool reduced_motion) {
        return state(anim_key(id), on, now, reduced_motion);
    }
    [[nodiscard]] float factor(std::string_view id, bool on, MotionClock::time_point now, bool reduced_motion) {
        return std::clamp(state(id, on, now, reduced_motion).pos, 0.f, 1.f);
    }
    [[nodiscard]] bool active() const noexcept {
        for (const auto& entry : entries_)
            if (entry.second.spring.active()) return true;
        return false;
    }

private:
    struct Entry {
        Spring spring{0.0};
        bool target = false;
        bool initialized = false;
    };
    std::unordered_map<std::uint64_t, Entry> entries_;
};

// Sliding Apple Liquid Glass droplet indicator for segmented controls, session
// tabs, sidebar groups and device cards. Leading and trailing edges use
// asymmetric springs so the lens organically stretches across items like real
// liquid mercury, pinches its waist slightly to conserve volume, and snaps
// smoothly into place within a finite settling window (0 FPS when idle).
class LiquidPillAnimator {
public:
    struct Rect {
        float x = 0.f;
        float y = 0.f;
        float w = 0.f;
        float h = 0.f;
        float stretch_x = 0.f; // Volume-preserving cross-axis deformation
        float stretch_y = 0.f; // Volume-preserving cross-axis deformation
        float glint = 0.f;     // -1..+1 velocity-driven sweeping refraction highlight
        bool active = false;
    };

    void begin_frame(std::uint64_t frame_id) noexcept {
        frame_ = frame_id;
        if (entries_.size() > 32 && (frame_ & 63u) == 0u) {
            std::erase_if(entries_, [this](const auto& item) {
                return !item.second.active() && frame_ - item.second.last_seen > 240;
            });
        }
    }

    // Update and return the current liquid pill geometry in local coordinates.
    // selection_token changes only when the user actually switches selection;
    // passive layout resizes snap silently instead of launching a jelly slide.
    [[nodiscard]] Rect sample(std::uint64_t key, std::uint64_t selection_token,
                              float local_x, float local_y, float w, float h,
                              MotionClock::time_point now, bool reduced_motion) {
        Entry& e = entries_[key];
        e.last_seen = frame_;
        const double tx1 = static_cast<double>(local_x);
        const double ty1 = static_cast<double>(local_y);
        const double tx2 = static_cast<double>(local_x + w);
        const double ty2 = static_cast<double>(local_y + h);

        static constexpr Spring::Params kLead{30.0, 0.72, 0.12, 0.80, 0.44};
        static constexpr Spring::Params kTrail{19.5, 0.58, 0.12, 0.80, 0.48};

        if (!e.initialized) {
            e.initialized = true;
            e.selection = selection_token;
            e.x1.set_params(kLead);
            e.x2.set_params(kLead);
            e.y1.set_params(kLead);
            e.y2.set_params(kLead);
            e.x1.snap(tx1);
            e.x2.snap(tx2);
            e.y1.snap(ty1);
            e.y2.snap(ty2);
        } else if (e.selection != selection_token) {
            const double cx1 = e.x1.update(now, reduced_motion);
            const double cx2 = e.x2.update(now, reduced_motion);
            const double cy1 = e.y1.update(now, reduced_motion);
            const double cy2 = e.y2.update(now, reduced_motion);
            e.selection = selection_token;

            if (tx2 > cx2 + 1.0) {
                e.x2.set_params(kLead);
                e.x1.set_params(kTrail);
            } else if (tx1 < cx1 - 1.0) {
                e.x1.set_params(kLead);
                e.x2.set_params(kTrail);
            } else {
                e.x1.set_params(kLead);
                e.x2.set_params(kLead);
            }

            if (ty2 > cy2 + 1.0) {
                e.y2.set_params(kLead);
                e.y1.set_params(kTrail);
            } else if (ty1 < cy1 - 1.0) {
                e.y1.set_params(kLead);
                e.y2.set_params(kTrail);
            } else {
                e.y1.set_params(kLead);
                e.y2.set_params(kLead);
            }

            e.x1.retarget(tx1, now, reduced_motion);
            e.x2.retarget(tx2, now, reduced_motion);
            e.y1.retarget(ty1, now, reduced_motion);
            e.y2.retarget(ty2, now, reduced_motion);
        } else if (!e.active()) {
            // Layout changed (e.g. window resize) while settled: follow silently.
            e.x1.snap(tx1);
            e.x2.snap(tx2);
            e.y1.snap(ty1);
            e.y2.snap(ty2);
        } else {
            // Update destination if layout shifted mid-flight without resetting velocity.
            if (std::abs(e.x1.target() - tx1) > 0.5) e.x1.retarget(tx1, now, reduced_motion);
            if (std::abs(e.x2.target() - tx2) > 0.5) e.x2.retarget(tx2, now, reduced_motion);
            if (std::abs(e.y1.target() - ty1) > 0.5) e.y1.retarget(ty1, now, reduced_motion);
            if (std::abs(e.y2.target() - ty2) > 0.5) e.y2.retarget(ty2, now, reduced_motion);
        }

        const float nx1 = static_cast<float>(e.x1.update(now, reduced_motion));
        const float nx2 = static_cast<float>(e.x2.update(now, reduced_motion));
        const float ny1 = static_cast<float>(e.y1.update(now, reduced_motion));
        const float ny2 = static_cast<float>(e.y2.update(now, reduced_motion));

        const float left = std::min(nx1, nx2);
        const float right = std::max(nx1, nx2);
        const float top = std::min(ny1, ny2);
        const float bot = std::max(ny1, ny2);
        const float cw = std::max(4.f, right - left);
        const float ch = std::max(4.f, bot - top);

        // Volume-preserving cross-axis waist pinch + sweeping lens glint.
        float sx = 0.f, sy = 0.f, glint = 0.f;
        if (!reduced_motion && e.active()) {
            const float elong_x = std::clamp((cw - w) / std::max(8.f, w), -0.14f, 0.45f);
            const float elong_y = std::clamp((ch - h) / std::max(8.f, h), -0.14f, 0.45f);
            sx = -elong_y * 0.14f;
            sy = -elong_x * 0.20f;
            const float vx = static_cast<float>(0.5 * (e.x1.velocity() + e.x2.velocity()));
            const float vy = static_cast<float>(0.5 * (e.y1.velocity() + e.y2.velocity()));
            glint = std::tanh((vx + vy * 0.65f) * 0.0022f);
        }
        return {left, top, cw, ch, sx, sy, glint, e.active()};
    }

    [[nodiscard]] bool active() const noexcept {
        for (const auto& entry : entries_)
            if (entry.second.active()) return true;
        return false;
    }

private:
    struct Entry {
        Spring x1{0.0}, x2{0.0}, y1{0.0}, y2{0.0};
        std::uint64_t selection = 0;
        std::uint64_t last_seen = 0;
        bool initialized = false;
        [[nodiscard]] bool active() const noexcept {
            return x1.active() || x2.active() || y1.active() || y2.active();
        }
    };
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t frame_ = 0;
};

// Lightweight smooth wheel scrolling with bounded elastic rubber-band feedback
// at list boundaries. Settles quickly so idle frames stay at 0 FPS and never
// fights manual scrollbar dragging.
class SmoothScrollAnimator {
public:
    struct Output {
        bool apply_scroll = false;
        float scroll_y = 0.f;
        float rubber_offset = 0.f; // Visual boundary bounce in pixels (clamped)
    };

    void begin_frame(std::uint64_t frame_id) noexcept {
        frame_ = frame_id;
        if (entries_.size() > 32 && (frame_ & 63u) == 0u) {
            std::erase_if(entries_, [this](const auto& item) {
                return !item.second.active() && frame_ - item.second.last_seen > 240;
            });
        }
    }

    void reset(std::uint64_t key) noexcept {
        if (auto it = entries_.find(key); it != entries_.end()) {
            it->second.scroll.snap(0.0);
            it->second.rubber.snap(0.0);
            it->second.initialized = false;
        }
    }

    [[nodiscard]] Output update(std::uint64_t key, float current_y, float max_y,
                                float wheel_delta, bool hovered, bool dragging_mouse,
                                float step_px, MotionClock::time_point now, bool reduced_motion) {
        Entry& e = entries_[key];
        e.last_seen = frame_;
        const float safe_max = std::max(0.f, max_y);
        if (!e.initialized) {
            e.initialized = true;
            e.scroll.set_params({18.0, 0.90, 0.25, 1.5, 0.36});
            e.rubber.set_params({22.0, 0.56, 0.10, 0.8, 0.42});
            e.scroll.snap(current_y);
            e.rubber.snap(0.0);
            e.last_applied = current_y;
        }
        if (reduced_motion || dragging_mouse) {
            e.scroll.snap(current_y);
            e.rubber.snap(0.0);
            e.last_applied = current_y;
            return {};
        }

        // ImGui applies a discrete wheel jump before our child/table body runs;
        // measure that delta relative to our last applied scroll position and
        // smoothly spring-glide to the accumulated target without double-scrolling.
        const float imgui_delta = current_y - e.last_applied;
        if (hovered && wheel_delta != 0.f) {
            const float step = std::abs(imgui_delta) > 0.25f ? imgui_delta : (-wheel_delta * step_px);
            const float base = e.scroll.active() ? static_cast<float>(e.scroll.target()) : e.last_applied;
            const float raw_next = base + step;
            const float clamped = std::clamp(raw_next, 0.f, safe_max);
            if (safe_max > 0.5f && std::abs(clamped - e.last_applied) > 0.25f) {
                if (!e.scroll.active()) e.scroll.snap(e.last_applied);
                e.scroll.retarget(clamped, now, false);
            }
            // Boundary rubber-band jelly impulse when scrolling past top or bottom.
            if ((wheel_delta > 0.f && clamped <= 0.5f && raw_next < -0.5f) ||
                (wheel_delta < 0.f && clamped >= safe_max - 0.5f && raw_next > safe_max + 0.5f) ||
                (safe_max <= 0.5f && std::abs(wheel_delta) > 0.01f)) {
                const float dir = wheel_delta > 0.f ? 1.f : -1.f;
                e.rubber.impulse(static_cast<double>(dir) * 115.0, now, false);
            }
        } else if (!e.scroll.active()) {
            e.scroll.snap(current_y);
            e.last_applied = current_y;
        }

        Output out{};
        if (e.scroll.active()) {
            const float next_y = std::clamp(static_cast<float>(e.scroll.update(now, false)), 0.f, safe_max);
            out.apply_scroll = true;
            out.scroll_y = next_y;
            e.last_applied = next_y;
        }
        if (e.rubber.active()) {
            out.rubber_offset = std::clamp(static_cast<float>(e.rubber.update(now, false)), -10.f, 10.f);
        }
        return out;
    }

    [[nodiscard]] bool active() const noexcept {
        for (const auto& entry : entries_)
            if (entry.second.active()) return true;
        return false;
    }

private:
    struct Entry {
        Spring scroll{0.0};
        Spring rubber{0.0};
        float last_applied = 0.f;
        std::uint64_t last_seen = 0;
        bool initialized = false;
        [[nodiscard]] bool active() const noexcept { return scroll.active() || rubber.active(); }
    };
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::uint64_t frame_ = 0;
};

// Stepped loading feedback. The active dot advances on a fixed cadence and
// each step is a short transition, never a continuous rotation. With reduced
// motion the indicator stays static.
class LoadingIndicator {
public:
    // Returns the index of the highlighted dot, or -1 when motion is reduced.
    int step(MotionClock::time_point now, bool reduced_motion) {
        if (reduced_motion) {
            active_ = -1;
            return -1;
        }
        if (now - last_ >= std::chrono::duration<double>(motion.loading_step)) {
            last_ = now;
            phase_ = (phase_ + 1) % 3;
        }
        active_ = phase_;
        return phase_;
    }
    [[nodiscard]] bool active() const noexcept { return active_ >= 0; }
private:
    MotionClock::time_point last_{};
    int phase_ = 0;
    int active_ = -1;
};

// Frame rate independent smoothing towards a target value.
inline float smooth_towards(float current, float target, float delta_seconds, double tau) {
    if (delta_seconds <= 0.f) return current;
    const float k = 1.0f - static_cast<float>(std::exp(-static_cast<double>(delta_seconds) / tau));
    return current + (target - current) * k;
}
} // namespace jianssh::ui
