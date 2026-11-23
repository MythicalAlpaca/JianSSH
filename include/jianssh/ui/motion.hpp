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
// content that arrives, symmetric curves for elements that change in place,
// gentle smoothstep for hover tints, and analytic spring curves for liquid
// glass jelly slides.
enum class Easing {
    Smoothstep,
    CubicInOut,
    EaseOutCubic,
    EaseOutQuint,
    EaseInOutQuint,
    EaseOutBack,
    LiquidSpring
};

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
        case Easing::EaseOutBack: {
            constexpr double c1 = 1.32;
            constexpr double c3 = c1 + 1.0;
            const double u = t - 1.0;
            return 1.0 + c3 * u * u * u + c1 * u * u;
        }
        case Easing::LiquidSpring: {
            if (t <= 0.0) return 0.0;
            if (t >= 1.0) return 1.0;
            // Normalized underdamped spring response (~4.5% gentle liquid overshoot).
            constexpr double decay = 6.2;
            constexpr double freq = 9.4;
            static const double end_val =
                1.0 - std::exp(-decay) * (std::cos(freq) + (decay / freq) * std::sin(freq));
            const double env = std::exp(-decay * t);
            const double raw = 1.0 - env * (std::cos(freq * t) + (decay / freq) * std::sin(freq * t));
            return raw / end_val;
        }
        case Easing::Smoothstep:
        default:
            return t * t * (3.0 - 2.0 * t);
    }
}

// Analytical damped harmonic oscillator evaluated at monotonic timestamps,
// never Euler-integrated per frame. Guarantees frame-rate independence, exact
// instantaneous velocity for jelly squash-and-stretch, seamless mid-flight
// retargeting, and strictly finite settling time so idle CPU stays at zero.
class Spring {
public:
    struct Params {
        double omega = 22.0;       // Natural angular frequency (rad/s)
        double damping = 0.64;     // Damping ratio zeta (<1 gives soft liquid rebound)
        double pos_eps = 0.08;     // Settling position threshold
        double vel_eps = 0.50;     // Settling velocity threshold
        double max_seconds = 0.56; // Hard settling cap for power saving
    };

    explicit Spring(double initial = 0.0, Params params = {22.0, 0.64, 0.08, 0.50, 0.56}) noexcept
        : params_(params), value_(initial), velocity_(0.0), target_(initial), y0_(0.0), v0_(0.0) {}

    void set_params(Params params) noexcept { params_ = params; }

    void snap(double v) noexcept {
        value_ = target_ = v;
        velocity_ = y0_ = v0_ = 0.0;
        active_ = false;
    }

    void retarget(double to, MotionClock::time_point now, bool reduced_motion = false) noexcept {
        if (!std::isfinite(to)) return;
        update(now, reduced_motion);
        if (reduced_motion) {
            snap(to);
            return;
        }
        if (std::abs(to - target_) <= params_.pos_eps * 0.25 && !active_) {
            target_ = to;
            value_ = to;
            return;
        }
        target_ = to;
        y0_ = value_ - target_;
        v0_ = velocity_;
        start_ = now;
        active_ = std::abs(y0_) > params_.pos_eps || std::abs(v0_) > params_.vel_eps;
        if (!active_) snap(target_);
    }

    // Add an immediate velocity impulse (e.g. for tactile press bounce).
    void impulse(double dv, MotionClock::time_point now, bool reduced_motion = false) noexcept {
        if (reduced_motion || !std::isfinite(dv)) return;
        update(now, false);
        y0_ = value_ - target_;
        v0_ = velocity_ + dv;
        start_ = now;
        active_ = std::abs(y0_) > params_.pos_eps || std::abs(v0_) > params_.vel_eps;
    }

    double update(MotionClock::time_point now, bool reduced_motion = false) noexcept {
        if (!active_) return value_;
        if (reduced_motion) {
            snap(target_);
            return value_;
        }
        const double t = std::max(0.0, std::chrono::duration<double>(now - start_).count());
        if (t >= params_.max_seconds) {
            snap(target_);
            return value_;
        }
        const double w = std::max(1.0, params_.omega);
        const double z = std::clamp(params_.damping, 0.15, 2.0);
        double y = 0.0;
        double v = 0.0;
        if (z < 0.999) {
            const double wd = w * std::sqrt(1.0 - z * z);
            const double decay = std::exp(-z * w * t);
            const double a = y0_;
            const double b = (v0_ + z * w * y0_) / wd;
            const double c = std::cos(wd * t);
            const double s = std::sin(wd * t);
            y = decay * (a * c + b * s);
            v = decay * ((b * wd - a * z * w) * c - (a * wd + b * z * w) * s);
        } else if (z <= 1.001) {
            const double decay = std::exp(-w * t);
            const double a = y0_;
            const double b = v0_ + w * y0_;
            y = decay * (a + b * t);
            v = decay * (b - w * (a + b * t));
        } else {
            const double r = w * std::sqrt(z * z - 1.0);
            const double r1 = -z * w + r;
            const double r2 = -z * w - r;
            const double c1 = (v0_ - r2 * y0_) / (r1 - r2);
            const double c2 = y0_ - c1;
            const double e1 = std::exp(r1 * t);
            const double e2 = std::exp(r2 * t);
            y = c1 * e1 + c2 * e2;
            v = c1 * r1 * e1 + c2 * r2 * e2;
        }
        // Smooth cosine taper across the final 12% of max_seconds so the hard
        // power-saving cap never produces a visible snap.
        const double taper_start = params_.max_seconds * 0.88;
        if (t > taper_start) {
            const double u = (t - taper_start) / (params_.max_seconds - taper_start);
            const double k = 0.5 * (1.0 + std::cos(3.14159265358979323846 * u));
            y *= k;
            v *= k;
        }
        value_ = target_ + y;
        velocity_ = v;
        if (t > 0.03 && std::abs(y) <= params_.pos_eps && std::abs(v) <= params_.vel_eps) {
            snap(target_);
        }
        return value_;
    }

    // Smooth bounded squash-and-stretch magnitude derived from velocity.
    [[nodiscard]] float stretch(float velocity_scale = 0.00055f, float max_stretch = 0.24f) const noexcept {
        if (!active_ || max_stretch <= 0.f) return 0.f;
        const float raw = static_cast<float>(std::abs(velocity_)) * velocity_scale;
        return max_stretch * std::tanh(raw / max_stretch);
    }

    [[nodiscard]] double value() const noexcept { return value_; }
    [[nodiscard]] double velocity() const noexcept { return velocity_; }
    [[nodiscard]] double target() const noexcept { return target_; }
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    Params params_{};
    double value_ = 0.0, velocity_ = 0.0, target_ = 0.0;
    double y0_ = 0.0, v0_ = 0.0;
    MotionClock::time_point start_{};
    bool active_ = false;
};

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

// Two phase page transition: the outgoing page fades and glides away, then the
// incoming page slides in with a soft liquid spring settle. Only one page is
// ever drawn, so every ImGui widget keeps its identity and state.
class PageTransition {
public:
    enum class Phase { Idle, Out, In };
    void reset(std::string key) {
        displayed_ = std::move(key);
        target_ = displayed_;
        phase_ = Phase::Idle;
        direction_ = 0;
        tween_.finish();
        tween_.start(1.0, 1.0, MotionClock::time_point{}, std::chrono::duration<double>{});
    }
    // Call once per frame with the desired key. direction: -1 slides from left,
    // +1 slides from right, 0 glides vertically. Returns true when the
    // displayed key changed during this call (the frame should swap content).
    bool update(const std::string& desired, MotionClock::time_point now, double out_seconds,
                double in_seconds, bool reduced_motion, int direction = 0) {
        if (desired == target_) {
            if (phase_ == Phase::Idle) return false;
        } else {
            target_ = desired;
            direction_ = direction;
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
                             Easing::LiquidSpring, reduced_motion);
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
    // Unclamped incoming progress (allows the subtle LiquidSpring overshoot).
    [[nodiscard]] float progress() const noexcept {
        if (phase_ == Phase::Idle) return 1.f;
        return static_cast<float>(tween_.value());
    }
    // Distance travelled by the outgoing page (fades away while drifting).
    [[nodiscard]] float outgoing_offset(float distance) const noexcept {
        return phase_ == Phase::Out ? (1.f - alpha()) * distance : 0.f;
    }
    // Distance still to travel by the incoming page (starts offset, settles at 0
    // with a gentle liquid spring rebound).
    [[nodiscard]] float incoming_offset(float distance) const noexcept {
        return phase_ == Phase::In ? (1.f - progress()) * distance : 0.f;
    }
    [[nodiscard]] int direction() const noexcept { return direction_; }
    [[nodiscard]] bool active() const noexcept { return phase_ != Phase::Idle; }
    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] const std::string& displayed() const noexcept { return displayed_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; }
private:
    std::string displayed_, target_;
    Phase phase_ = Phase::Idle;
    int direction_ = 0;
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
