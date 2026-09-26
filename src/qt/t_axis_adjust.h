#pragma once

// Shared state transitions and math for time-axis pan/zoom operations.

#include <vnm_plot/core/time_units.h>

#include <QtGlobal>

#include <cmath>
#include <cstdint>

namespace vnm::plot::detail {

struct time_axis_update_result_t
{
    bool   accepted = false;
    bool   changed  = false;
};

class Time_axis_model
{
public:
    Time_axis_model() = default;

    Time_axis_model(
        qint64 t_min,
        qint64 t_max,
        qint64 t_available_min,
        qint64 t_available_max,
        bool   t_min_initialized,
        bool   t_max_initialized,
        bool   t_available_min_initialized,
        bool   t_available_max_initialized)
    :
        m_t_min(t_min),
        m_t_max(t_max),
        m_t_available_min(t_available_min),
        m_t_available_max(t_available_max),
        m_t_min_initialized(t_min_initialized),
        m_t_max_initialized(t_max_initialized),
        m_t_available_min_initialized(t_available_min_initialized),
        m_t_available_max_initialized(t_available_max_initialized)
    {}

    static Time_axis_model initialized(
        qint64 t_min,
        qint64 t_max,
        qint64 t_available_min,
        qint64 t_available_max)
    {
        return Time_axis_model(
            t_min,
            t_max,
            t_available_min,
            t_available_max,
            true,
            true,
            true,
            true);
    }

    qint64 t_min()           const { return m_t_min;           }
    qint64 t_max()           const { return m_t_max;           }
    qint64 t_available_min() const { return m_t_available_min; }
    qint64 t_available_max() const { return m_t_available_max; }

    bool view_initialized() const
    {
        return m_t_min_initialized && m_t_max_initialized;
    }

    bool available_initialized() const
    {
        return m_t_available_min_initialized && m_t_available_max_initialized;
    }

    bool any_view_bound_initialized() const
    {
        return m_t_min_initialized || m_t_max_initialized;
    }

    bool any_available_bound_initialized() const
    {
        return m_t_available_min_initialized || m_t_available_max_initialized;
    }

    time_axis_update_result_t set_t_min(qint64 v)
    {
        // Single-bound writes seed an uninitialized view. Completing the pair
        // must enforce ordering before the axis becomes initialized.
        if (!view_initialized()) {
            if (m_t_min_initialized && v == m_t_min) {
                return {true, false};
            }
            if (m_t_max_initialized && !(v < m_t_max)) {
                return {};
            }
            return
                set_limits_if_changed(
                    v,
                    m_t_max,
                    m_t_available_min,
                    m_t_available_max,
                    true,
                    m_t_max_initialized,
                    m_t_available_min_initialized,
                    m_t_available_max_initialized);
        }

        qint64 new_min = v;
        qint64 new_max = m_t_max;
        if (v >= m_t_max) {
            const auto span_ns = positive_span_ns(m_t_min, m_t_max);
            if (!span_ns) {
                return {};
            }
            new_max = saturating_add_duration_ns(v, *span_ns);
            if (positive_span_ns(new_min, new_max) != span_ns) {
                return {};
            }
        }

        return
            set_limits_if_changed(
                new_min,
                new_max,
                m_t_available_min,
                m_t_available_max,
                true,
                true,
                m_t_available_min_initialized,
                m_t_available_max_initialized);
    }

    time_axis_update_result_t set_t_max(qint64 v)
    {
        // See set_t_min(): QML property bindings can initialize the range
        // one side at a time, but a completed range is always ordered.
        if (!view_initialized()) {
            if (m_t_max_initialized && v == m_t_max) {
                return {true, false};
            }
            if (m_t_min_initialized && !(v > m_t_min)) {
                return {};
            }
            return
                set_limits_if_changed(
                    m_t_min,
                    v,
                    m_t_available_min,
                    m_t_available_max,
                    m_t_min_initialized,
                    true,
                    m_t_available_min_initialized,
                    m_t_available_max_initialized);
        }

        qint64 new_min = m_t_min;
        qint64 new_max = v;
        if (v <= m_t_min) {
            const auto span_ns = positive_span_ns(m_t_min, m_t_max);
            if (!span_ns) {
                return {};
            }
            new_min = saturating_sub_duration_ns(v, *span_ns);
            if (positive_span_ns(new_min, new_max) != span_ns) {
                return {};
            }
        }

        return
            set_limits_if_changed(
                new_min,
                new_max,
                m_t_available_min,
                m_t_available_max,
                true,
                true,
                m_t_available_min_initialized,
                m_t_available_max_initialized);
    }

    time_axis_update_result_t set_t_range(qint64 t_min_ns, qint64 t_max_ns)
    {
        if (!(t_max_ns > t_min_ns)) {
            return {};
        }

        return
            set_limits_if_changed(
                t_min_ns,
                t_max_ns,
                m_t_available_min,
                m_t_available_max,
                true,
                true,
                m_t_available_min_initialized,
                m_t_available_max_initialized);
    }

    time_axis_update_result_t set_t_available_min(qint64 v)
    {
        if (!m_t_available_max_initialized) {
            if (m_t_available_min_initialized && v == m_t_available_min) {
                return {true, false};
            }
            return
                set_limits_if_changed(
                    m_t_min,
                    m_t_max,
                    v,
                    m_t_available_max,
                    m_t_min_initialized,
                    m_t_max_initialized,
                    true,
                    false);
        }

        return set_available_t_range(v, m_t_available_max);
    }

    time_axis_update_result_t set_t_available_max(qint64 v)
    {
        if (!m_t_available_min_initialized) {
            if (m_t_available_max_initialized && v == m_t_available_max) {
                return {true, false};
            }
            return
                set_limits_if_changed(
                    m_t_min,
                    m_t_max,
                    m_t_available_min,
                    v,
                    m_t_min_initialized,
                    m_t_max_initialized,
                    false,
                    true);
        }

        return set_available_t_range(m_t_available_min, v);
    }

    time_axis_update_result_t set_available_t_range(
        qint64 t_available_min_ns,
        qint64 t_available_max_ns)
    {
        if (!(t_available_max_ns > t_available_min_ns)) {
            return {};
        }

        qint64 new_t_min           = m_t_min;
        qint64 new_t_max           = m_t_max;
        bool new_t_min_initialized = m_t_min_initialized;
        bool new_t_max_initialized = m_t_max_initialized;
        const bool view_unset      = !m_t_min_initialized && !m_t_max_initialized;
        if (view_unset) {
            // A fully unset view adopts the first complete available range so
            // subsequent interactions have a real window to operate on.
            new_t_min             = t_available_min_ns;
            new_t_max             = t_available_max_ns;
            new_t_min_initialized = true;
            new_t_max_initialized = true;
        }
        else
        if (view_initialized()) {
            // Half-seeded views fall through unchanged; only complete views
            // are clamped.
            const auto clamped = clamp_time_range_to_available_ns(
                time_range_t{new_t_min, new_t_max},
                time_range_t{t_available_min_ns, t_available_max_ns});
            if (clamped) {
                new_t_min = clamped->min_ns;
                new_t_max = clamped->max_ns;
            }
            else {
                new_t_min = t_available_min_ns;
                new_t_max = t_available_max_ns;
            }
        }

        return
            set_limits_if_changed(
                new_t_min,
                new_t_max,
                t_available_min_ns,
                t_available_max_ns,
                new_t_min_initialized,
                new_t_max_initialized,
                true,
                true);
    }

    time_axis_update_result_t adjust_t_to_target(
        qint64 target_min_ns,
        qint64 target_max_ns)
    {
        if (!(target_max_ns > target_min_ns)) {
            return {};
        }

        time_range_t target{target_min_ns, target_max_ns};
        if (available_initialized()) {
            const auto clamped = clamp_time_range_to_available_ns(
                target,
                time_range_t{m_t_available_min, m_t_available_max});
            if (!clamped) {
                return {};
            }
            target = *clamped;
        }

        return
            set_limits_if_changed(
                target.min_ns,
                target.max_ns,
                m_t_available_min,
                m_t_available_max,
                true,
                true,
                m_t_available_min_initialized,
                m_t_available_max_initialized);
    }

    // Translate opposite to the cursor, using the visible span.
    time_axis_update_result_t adjust_t_from_mouse_diff(
        double ref_width,
        double diff)
    {
        if (!view_initialized()) {
            return {};
        }

        if (ref_width <= 0.0) {
            return {};
        }

        const auto span_ns = positive_span_ns(m_t_min, m_t_max);
        if (!span_ns) {
            return {};
        }

        const long double fraction =
            std::abs(static_cast<long double>(diff) / static_cast<long double>(ref_width));
        const std::uint64_t duration_ns = scaled_duration_ns(*span_ns, fraction);
        const auto direction = (diff >= 0.0)
            ? Time_translation_direction::BACKWARD
            : Time_translation_direction::FORWARD;
        const auto shifted = translate_time_range_by_duration_ns(
            time_range_t{m_t_min, m_t_max},
            duration_ns,
            direction);
        if (!shifted) {
            return {};
        }
        return adjust_t_to_target(shifted->min_ns, shifted->max_ns);
    }

    // Preview dragging follows the cursor, using the available span.
    time_axis_update_result_t adjust_t_from_mouse_diff_on_preview(
        double ref_width,
        double diff)
    {
        if (!view_initialized() || !available_initialized()) {
            return {};
        }

        if (ref_width <= 0.0) {
            return {};
        }

        const auto avail_span_ns = positive_span_ns(
            m_t_available_min,
            m_t_available_max);
        if (!avail_span_ns) {
            return {};
        }

        const long double fraction =
            std::abs(static_cast<long double>(diff) / static_cast<long double>(ref_width));
        const std::uint64_t duration_ns = scaled_duration_ns(*avail_span_ns, fraction);
        const auto direction = (diff >= 0.0)
            ? Time_translation_direction::FORWARD
            : Time_translation_direction::BACKWARD;
        const auto shifted = translate_time_range_by_duration_ns(
            time_range_t{m_t_min, m_t_max},
            duration_ns,
            direction);
        if (!shifted) {
            return {};
        }
        return adjust_t_to_target(shifted->min_ns, shifted->max_ns);
    }

    // Recenter at the preview position while preserving the visible span.
    time_axis_update_result_t adjust_t_from_mouse_pos_on_preview(
        double ref_width,
        double x_pos)
    {
        if (!view_initialized() || !available_initialized()) {
            return {};
        }

        if (ref_width <= 0.0) {
            return {};
        }

        const auto span_ns = positive_span_ns(m_t_min, m_t_max);
        if (!span_ns) {
            return {};
        }

        const auto new_center_ns = time_at_fraction_ns(
            time_range_t{m_t_available_min, m_t_available_max},
            static_cast<long double>(x_pos) / static_cast<long double>(ref_width));
        if (!new_center_ns) {
            return {};
        }
        const time_range_t target = centered_time_range_ns(*new_center_ns, *span_ns);
        return adjust_t_to_target(target.min_ns, target.max_ns);
    }

    // Zoom around a normalized pivot; scale < 1 zooms in and scale > 1 out.
    time_axis_update_result_t adjust_t_from_pivot_and_scale(
        double pivot,
        double scale)
    {
        if (!view_initialized()) {
            return {};
        }

        if (scale <= 0.0) {
            return {};
        }

        const auto span_ns = positive_span_ns(m_t_min, m_t_max);
        if (!span_ns) {
            return {};
        }

        const auto t_pivot_ns = time_at_fraction_ns(
            time_range_t{m_t_min, m_t_max},
            static_cast<long double>(pivot));
        if (!t_pivot_ns) {
            return {};
        }

        const auto left_span_ns  = positive_span_ns(m_t_min, *t_pivot_ns);
        const auto right_span_ns = positive_span_ns(*t_pivot_ns, m_t_max);
        const std::uint64_t left_ns = left_span_ns
            ? scaled_duration_ns(*left_span_ns, static_cast<long double>(scale))
            : 0;
        const std::uint64_t right_ns = right_span_ns
            ? scaled_duration_ns(*right_span_ns, static_cast<long double>(scale))
            : 0;
        const time_range_t target = time_range_around_pivot_ns(
            *t_pivot_ns,
            left_ns,
            right_ns);
        return adjust_t_to_target(target.min_ns, target.max_ns);
    }

private:
    time_axis_update_result_t set_limits_if_changed(
        qint64 t_min_ns,
        qint64 t_max_ns,
        qint64 t_available_min_ns,
        qint64 t_available_max_ns,
        bool   t_min_initialized,
        bool   t_max_initialized,
        bool   t_available_min_initialized,
        bool   t_available_max_initialized)
    {
        const bool changed =
            m_t_min                       != t_min_ns                    ||
            m_t_max                       != t_max_ns                    ||
            m_t_available_min             != t_available_min_ns          ||
            m_t_available_max             != t_available_max_ns          ||
            m_t_min_initialized           != t_min_initialized           ||
            m_t_max_initialized           != t_max_initialized           ||
            m_t_available_min_initialized != t_available_min_initialized ||
            m_t_available_max_initialized != t_available_max_initialized;

        if (changed) {
            m_t_min                       = t_min_ns;
            m_t_max                       = t_max_ns;
            m_t_available_min             = t_available_min_ns;
            m_t_available_max             = t_available_max_ns;
            m_t_min_initialized           = t_min_initialized;
            m_t_max_initialized           = t_max_initialized;
            m_t_available_min_initialized = t_available_min_initialized;
            m_t_available_max_initialized = t_available_max_initialized;
        }

        return {true, changed};
    }

    qint64 m_t_min                       = 0;
    qint64 m_t_max                       = 0;
    qint64 m_t_available_min             = 0;
    qint64 m_t_available_max             = 0;

    bool   m_t_min_initialized           = false;
    bool   m_t_max_initialized           = false;
    bool   m_t_available_min_initialized = false;
    bool   m_t_available_max_initialized = false;
};

} // namespace vnm::plot::detail
