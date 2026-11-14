#pragma once
// Interaction primitives shared by every surface: motion tokens, the per
// widget hover tween, the toggle switch animation and the loading indicator.
//
// The hover tween is what turns ImGui's instant colour switches into short
// transitions. A widget asks for its factor before it is drawn (the factor
// still describes the previous frame's hover state) and reports the hover
// state afterwards, so the colour it is painted with eases towards the new
// state instead of jumping.
#include "jianssh/ui/motion.hpp"
#include "jianssh/ui/theme.hpp"
#include <cmath>
#include <string>
#include <unordered_map>

namespace jianssh::ui {

// Every duration in the interface, in seconds unless the name says pixels.
// Durations stay short: macOS motion is felt as responsiveness, not as a wait.
struct MotionTokens {
    double entrance = 0.42;
    double selection = 0.18;
    double hover_in = 0.12;
    double hover_out = 0.20;
    double modal = 0.24;
    double modal_slide = 14.0;
    double theme = 0.35;
    double notice = 0.24;
    double loading_step = 0.34;
    double progress = 0.22;
    // Page transitions use an asymmetric pair: leaving is quick, arriving is
    // slower and decelerating so the new content appears to settle.
    double page_out = 0.13;
    double page_in = 0.26;
    double page_slide = 10.0;
    double page_out_slide = 4.0;
    double toggle = 0.18;
};

inline constexpr MotionTokens motion{};

// Eased hover factor per widget identity.
class HoverAnimator {
public:
    // Factor to paint with this frame; describes the previous hover state.
    [[nodiscard]] float factor(const std::string& id, MotionClock::time_point now, bool reduced_motion) {
        Entry& entry = entries_[id];
        entry.tween.update(now, reduced_motion);
        return static_cast<float>(entry.tween.value());
    }
    // Record the hover state observed after the widget was drawn.
    void observe(const std::string& id, bool hovered, MotionClock::time_point now, bool reduced_motion) {
        Entry& entry = entries_[id];
        if (entry.hovered == hovered) return;
        entry.hovered = hovered;
        entry.tween.retarget(hovered ? 1.0 : 0.0, now,
                             std::chrono::duration<double>(hovered ? motion.hover_in : motion.hover_out),
                             Easing::Smoothstep, reduced_motion);
    }
    [[nodiscard]] bool active() const noexcept {
        for (const auto& entry : entries_)
            if (entry.second.tween.active()) return true;
        return false;
    }
private:
    struct Entry {
        Tween tween{0.0};
        bool hovered = false;
    };
    std::unordered_map<std::string, Entry> entries_;
};

// macOS style switch: the knob glides between the two ends while the track
// crossfades from control grey to the accent colour.
class ToggleAnimator {
public:
    [[nodiscard]] float factor(const std::string& id, bool on, MotionClock::time_point now, bool reduced_motion) {
        Entry& entry = entries_[id];
        if (!entry.initialized) {
            entry.initialized = true;
            entry.target = on;
            entry.tween.start(on ? 1.0 : 0.0, on ? 1.0 : 0.0, now,
                              std::chrono::duration<double>(0.0), Easing::EaseOutCubic);
        }
        if (entry.target != on) {
            entry.target = on;
            entry.tween.retarget(on ? 1.0 : 0.0, now, std::chrono::duration<double>(motion.toggle),
                                 Easing::EaseOutCubic, reduced_motion);
        }
        entry.tween.update(now, reduced_motion);
        return static_cast<float>(entry.tween.value());
    }
    [[nodiscard]] bool active() const noexcept {
        for (const auto& entry : entries_)
            if (entry.second.tween.active()) return true;
        return false;
    }
private:
    struct Entry {
        Tween tween{0.0};
        bool target = false;
        bool initialized = false;
    };
    std::unordered_map<std::string, Entry> entries_;
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
