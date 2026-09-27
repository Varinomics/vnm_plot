#pragma once

#include <cmath>

namespace vnm::plot::zoom_inertia {

inline constexpr double k_friction          = 0.75;
inline constexpr double k_impulse_per_step  = 1.0;
inline constexpr double k_max_velocity      = 5.0;
inline constexpr double k_per_notch         = 1.05;
inline constexpr double k_velocity_epsilon  = 1e-3;
inline constexpr int    k_timer_interval_ms = 16;

struct wheel_delta_t
{
    double value  = 0.0;
    bool   pixels = false;
};

// High-resolution events can carry both forms. The platform adapter decides
// whether pixel deltas are reliable; otherwise only angle deltas are used.
inline wheel_delta_t select_wheel_delta(
    double angle_x, double angle_y,
    double pixel_x, double pixel_y,
    bool reliable_pixels)
{
    if (reliable_pixels && (pixel_x != 0.0 || pixel_y != 0.0)) {
        return {pixel_y != 0.0 ? pixel_y : pixel_x, true};
    }
    return {angle_y != 0.0 ? angle_y : angle_x, false};
}

// A 120-pixel stroke and a 120-unit wheel notch have the same zoom impulse.
inline double wheel_impulse(const wheel_delta_t& delta)
{
    return -delta.value / 120.0 * k_impulse_per_step;
}

// Integrating the geometric velocity decay makes delayed timer steps compose
// with smaller steps without changing the final zoom.
inline double scale_factor(double velocity, double elapsed_timer_steps)
{
    if (elapsed_timer_steps <= 0.0) {
        return 1.0;
    }
    const double decay = std::pow(k_friction, elapsed_timer_steps);
    const double integrated_velocity = velocity * (1.0 - decay) / (1.0 - k_friction);
    static const double base = std::pow(k_per_notch, (1.0 - k_friction) / k_impulse_per_step);
    return std::pow(base, integrated_velocity);
}

inline double velocity_after(double velocity, double elapsed_timer_steps)
{
    if (elapsed_timer_steps <= 0.0) {
        return velocity;
    }
    return velocity * std::pow(k_friction, elapsed_timer_steps);
}

} // namespace vnm::plot::zoom_inertia
