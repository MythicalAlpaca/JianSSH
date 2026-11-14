#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace jianssh::ui {
using MotionClock = std::chrono::steady_clock;
// Curves chosen from Apple's motion vocabulary: decelerating curves for
// content that arrives, symmetric curves for elements that change in place and
// a gentle smoothstep for hover tints.
enum class Easing { Smoothstep, CubicInOut, EaseOutCubic, EaseOutQuint, EaseInOutQuint };

inline double ease(double t, Easing easing = Easing::Smoothstep) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    switch (easing) {
        case Easing::CubicInOut:
            return t < 0.5 ? 4.0 * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 3) / 2.0;
        case Easing::EaseOutCubic: {
            const double u = 1.0 - t;
            return 1.0 - u * u * u;
        }
        case Easing::EaseOutQuint: {
            const double u = 1.0 - t;
            return 1.0 - u * u * u * u * u;
        }
        case Easing::EaseInOutQuint:
            return t < 0.5 ? 16.0 * t * t * t * t * t : 1.0 - std::pow(-2.0 * t + 2.0, 5) / 2.0;
        case Easing::Smoothstep:
        default:
            return t * t * (3.0 - 2.0 * t);
    }
}

// Finite scalar tween sampled at absolute monotonic times, never integrated per frame.
class Tween {
public:
    explicit Tween(double initial = 0.0) : value_(initial), from_(initial), to_(initial) {}
    void start(double from, double to, MotionClock::time_point now,
               std::chrono::duration<double> duration,
               Easing easing = Easing::Smoothstep, bool reduced_motion = false) {
        if (!std::isfinite(from) || !std::isfinite(to) || !std::isfinite(duration.count()))
            throw std::invalid_argument("动画数值和时长必须是有限值");
        from_ = value_ = from;
        to_ = to;
        start_ = now;
        duration_ = duration;
        easing_ = easing;
        active_ = from != to && duration.count() > 0.0 && !reduced_motion;
        if (!active_) finish();
    }
    void retarget(double to, MotionClock::time_point now, std::chrono::duration<double> duration,
                  Easing easing = Easing::Smoothstep, bool reduced_motion = false) {
        const auto from = update(now, reduced_motion);
        start(from, to, now, duration, easing, reduced_motion);
    }
    double update(MotionClock::time_point now, bool reduced_motion = false) noexcept {
        if (!active_) return value_;
        if (reduced_motion || now - start_ >= duration_) { finish(); return value_; }
        const double t = std::chrono::duration<double>(now - start_).count() / duration_.count();
        value_ = std::lerp(from_, to_, ease(t, easing_));
        return value_;
    }
    void finish() noexcept { value_ = to_; active_ = false; }
    [[nodiscard]] double value() const noexcept { return value_; }
    [[nodiscard]] bool active() const noexcept { return active_; }
private:
    double value_, from_, to_;
    MotionClock::time_point start_{};
    std::chrono::duration<double> duration_{};
    Easing easing_ = Easing::Smoothstep;
    bool active_ = false;
};

// Two phase page transition: the outgoing page fades and lifts away, then the
// incoming page settles into place. Only one page is ever drawn, so every
// ImGui widget keeps its identity and state; the choreography lives here.
class PageTransition {
public:
    enum class Phase { Idle, Out, In };
    void reset(std::string key) {
        displayed_ = std::move(key);
        target_ = displayed_;
        phase_ = Phase::Idle;
        tween_.finish();
        tween_.start(1.0, 1.0, MotionClock::time_point{}, std::chrono::duration<double>{});
    }
    // Call once per frame with the desired key. Returns true when the
    // displayed key changed during this call (the frame should swap content).
    bool update(const std::string& desired, MotionClock::time_point now, double out_seconds,
                double in_seconds, bool reduced_motion) {
        if (desired == target_) {
            if (phase_ == Phase::Idle) return false;
        } else {
            target_ = desired;
            if (reduced_motion) {
                const bool changed = displayed_ != target_;
                displayed_ = target_;
                phase_ = Phase::Idle;
                tween_.finish();
                return changed;
            }
            if (phase_ != Phase::Out) {
                phase_ = Phase::Out;
                tween_.retarget(0.0, now, std::chrono::duration<double>(out_seconds),
                                Easing::EaseOutCubic, false);
            }
        }
        if (phase_ == Phase::Out) {
            tween_.update(now, reduced_motion);
            if (!tween_.active()) {
                const bool changed = displayed_ != target_;
                displayed_ = target_;
                phase_ = Phase::In;
                tween_.start(0.0, 1.0, now, std::chrono::duration<double>(in_seconds),
                             Easing::EaseOutQuint, reduced_motion);
                return changed;
            }
        } else if (phase_ == Phase::In) {
            tween_.update(now, reduced_motion);
            if (!tween_.active()) phase_ = Phase::Idle;
        }
        return false;
    }
    [[nodiscard]] float alpha() const noexcept {
        if (phase_ == Phase::Idle) return 1.f;
        return static_cast<float>(std::clamp(tween_.value(), 0.0, 1.0));
    }
    // Distance travelled by the outgoing page (fades away while drifting).
    [[nodiscard]] float outgoing_offset(float distance) const noexcept {
        return phase_ == Phase::Out ? (1.f - alpha()) * distance : 0.f;
    }
    // Distance still to travel by the incoming page (starts offset, settles at 0).
    [[nodiscard]] float incoming_offset(float distance) const noexcept {
        return phase_ == Phase::In ? (1.f - alpha()) * distance : 0.f;
    }
    [[nodiscard]] bool active() const noexcept { return phase_ != Phase::Idle; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] const std::string& displayed() const noexcept { return displayed_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; }
private:
    std::string displayed_, target_;
    Phase phase_ = Phase::Idle;
    Tween tween_{1.0};
};

struct FramePlan {
    bool render = false;
    // nullopt means wait indefinitely for an event, zero means do not wait.
    std::optional<std::chrono::nanoseconds> wait;
};

// Caller waits for events with the indicated timeout; no sleeping or platform APIs here.
// Mark rendered frames with did_render(). Dirty events wake idle loops immediately.
class FrameScheduler {
public:
    static constexpr auto frame_interval = std::chrono::nanoseconds{16'666'667};
    explicit FrameScheduler(std::chrono::milliseconds idle_wait = std::chrono::milliseconds{250},
                            std::chrono::nanoseconds refresh_interval = frame_interval)
        : idle_wait_(std::clamp(idle_wait, std::chrono::milliseconds{1}, std::chrono::milliseconds{250})),
          refresh_interval_(refresh_interval > std::chrono::nanoseconds{0} ? refresh_interval : frame_interval) {}
    [[nodiscard]] FramePlan plan(MotionClock::time_point now, bool visible, bool active,
                                 bool dirty = false) const noexcept {
        if (!visible) return {false, std::nullopt};
        if (!last_frame_) return {true, std::chrono::nanoseconds{0}};
        if (active) {
            const auto remaining = refresh_interval_ - std::chrono::duration_cast<std::chrono::nanoseconds>(now - *last_frame_);
            if (remaining > std::chrono::nanoseconds{0}) return {false, remaining};
            return {true, std::chrono::nanoseconds{0}};
        }
        if (dirty) return {true, std::chrono::nanoseconds{0}};
        return {false, std::chrono::duration_cast<std::chrono::nanoseconds>(idle_wait_)};
    }
    void did_render(MotionClock::time_point now) noexcept { last_frame_ = now; }
    [[nodiscard]] std::chrono::nanoseconds refresh_interval() const noexcept { return refresh_interval_; }
private:
    std::chrono::milliseconds idle_wait_;
    std::chrono::nanoseconds refresh_interval_;
    std::optional<MotionClock::time_point> last_frame_;
};
} // namespace jianssh::ui
