#include <vnm_plot/qt/plot_time_axis.h>

#include "t_axis_adjust.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vnm::plot {

Plot_time_axis::Plot_time_axis(QObject* parent)
    : QObject(parent),
      m_model(std::make_unique<detail::Time_axis_model>())
{}

Plot_time_axis::~Plot_time_axis() = default;

qint64 Plot_time_axis::t_min() const
{
    return m_model->t_min();
}

qint64 Plot_time_axis::t_max() const
{
    return m_model->t_max();
}

qint64 Plot_time_axis::t_available_min() const
{
    return m_model->t_available_min();
}

qint64 Plot_time_axis::t_available_max() const
{
    return m_model->t_available_max();
}

bool Plot_time_axis::view_initialized() const
{
    return m_model->view_initialized();
}

bool Plot_time_axis::available_initialized() const
{
    return m_model->available_initialized();
}

bool Plot_time_axis::any_view_bound_initialized() const
{
    return m_model->any_view_bound_initialized();
}

bool Plot_time_axis::any_available_bound_initialized() const
{
    return m_model->any_available_bound_initialized();
}

// QML-facing readers surface 0 until both bounds of the relevant range have
// been initialized. QML eagerly evaluates property bindings before user code
// runs set_t_range, and default values would feed straight into spans and
// pixel math in PlotIndicator-style consumers.
qint64 Plot_time_axis::t_min_qml_ms() const
{
    return view_initialized() ? ns_to_ms_for_qml(m_model->t_min()) : 0;
}

qint64 Plot_time_axis::t_max_qml_ms() const
{
    return view_initialized() ? ns_to_ms_for_qml(m_model->t_max()) : 0;
}

qint64 Plot_time_axis::t_available_min_qml_ms() const
{
    return available_initialized() ? ns_to_ms_for_qml(m_model->t_available_min()) : 0;
}

qint64 Plot_time_axis::t_available_max_qml_ms() const
{
    return available_initialized() ? ns_to_ms_for_qml(m_model->t_available_max()) : 0;
}

void Plot_time_axis::set_t_min_qml_ms(qint64 v_ms)
{
    set_t_min(ms_for_qml_to_ns(v_ms));
}

void Plot_time_axis::set_t_max_qml_ms(qint64 v_ms)
{
    set_t_max(ms_for_qml_to_ns(v_ms));
}

void Plot_time_axis::set_t_available_min_qml_ms(qint64 v_ms)
{
    set_t_available_min(ms_for_qml_to_ns(v_ms));
}

void Plot_time_axis::set_t_available_max_qml_ms(qint64 v_ms)
{
    set_t_available_max(ms_for_qml_to_ns(v_ms));
}

bool Plot_time_axis::sync_vbar_width() const
{
    return m_sync_vbar_width;
}

void Plot_time_axis::set_sync_vbar_width(bool v)
{
    if (m_sync_vbar_width == v) {
        return;
    }

    m_sync_vbar_width = v;
    if (!m_sync_vbar_width) {
        m_vbar_width_by_owner.clear();
        if (m_shared_vbar_width_px != 0.0) {
            m_shared_vbar_width_px = 0.0;
            emit shared_vbar_width_changed(m_shared_vbar_width_px);
        }
    }
    emit sync_vbar_width_changed();
}

double Plot_time_axis::shared_vbar_width_px() const
{
    return m_shared_vbar_width_px;
}

void Plot_time_axis::update_shared_vbar_width(const QObject* owner, double width_px)
{
    if (!m_sync_vbar_width       || !owner)          { return; }
    if (!std::isfinite(width_px) || width_px <= 0.0) { return; }

    m_vbar_width_by_owner[owner] = width_px;

    double max_width = 0.0;
    for (const auto& entry : m_vbar_width_by_owner) {
        max_width = std::max(max_width, entry.second);
    }

    if (std::abs(max_width - m_shared_vbar_width_px) > 1e-12) {
        m_shared_vbar_width_px = max_width;
        emit shared_vbar_width_changed(m_shared_vbar_width_px);
    }
}

void Plot_time_axis::clear_shared_vbar_width(const QObject* owner)
{
    if (!owner) {
        return;
    }

    const auto erased = m_vbar_width_by_owner.erase(owner);
    if (erased == 0) {
        return;
    }

    double max_width = 0.0;
    for (const auto& entry : m_vbar_width_by_owner) {
        max_width = std::max(max_width, entry.second);
    }

    if (std::abs(max_width - m_shared_vbar_width_px) > 1e-12) {
        m_shared_vbar_width_px = max_width;
        emit shared_vbar_width_changed(m_shared_vbar_width_px);
    }
}

void Plot_time_axis::set_t_min(qint64 v)
{
    if (m_model->set_t_min(v).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::set_t_max(qint64 v)
{
    if (m_model->set_t_max(v).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::set_t_available_min(qint64 v)
{
    if (m_model->set_t_available_min(v).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::set_t_available_max(qint64 v)
{
    if (m_model->set_t_available_max(v).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::set_t_range(qint64 t_min_ns, qint64 t_max_ns)
{
    if (m_model->set_t_range(t_min_ns, t_max_ns).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::set_t_range_qml_ms(qint64 t_min_ms, qint64 t_max_ms)
{
    set_t_range(ms_for_qml_to_ns(t_min_ms), ms_for_qml_to_ns(t_max_ms));
}

void Plot_time_axis::set_available_t_range_qml_ms(
    qint64 t_available_min_ms,
    qint64 t_available_max_ms)
{
    set_available_t_range(
        ms_for_qml_to_ns(t_available_min_ms),
        ms_for_qml_to_ns(t_available_max_ms));
}

void Plot_time_axis::set_available_t_range(qint64 t_available_min_ns, qint64 t_available_max_ns)
{
    if (m_model->set_available_t_range(t_available_min_ns, t_available_max_ns).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::adjust_t_from_mouse_diff(double ref_width, double diff)
{
    if (m_model->adjust_t_from_mouse_diff(ref_width, diff).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::adjust_t_from_mouse_diff_on_preview(double ref_width, double diff)
{
    if (m_model->adjust_t_from_mouse_diff_on_preview(ref_width, diff).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::adjust_t_from_mouse_pos_on_preview(double ref_width, double x_pos)
{
    if (m_model->adjust_t_from_mouse_pos_on_preview(ref_width, x_pos).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::adjust_t_from_pivot_and_scale(double pivot, double scale)
{
    if (m_model->adjust_t_from_pivot_and_scale(pivot, scale).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::adjust_t_to_target(qint64 target_min_ns, qint64 target_max_ns)
{
    if (m_model->adjust_t_to_target(target_min_ns, target_max_ns).changed) {
        emit t_limits_changed();
    }
}

void Plot_time_axis::set_indicator_state(QObject* owner, bool active, qint64 t_ms)
{
    set_indicator_state(owner, active, t_ms, std::numeric_limits<double>::quiet_NaN());
}

void Plot_time_axis::set_indicator_state(QObject* owner, bool active, qint64 t_ms, double x_norm)
{
    if (!owner) {
        return;
    }

    // QML callers pass t_ms in milliseconds-since-epoch; internal storage
    // is nanoseconds.
    const qint64 t_ns = ms_for_qml_to_ns(t_ms);

    bool changed = false;
    if (active) {
        bool owner_changed = false;
        if (m_indicator_owner != owner) {
            m_indicator_owner = owner;
            owner_changed     = true;
            changed           = true;
        }
        if (!m_indicator_active) {
            m_indicator_active = true;
            changed = true;
        }
        if (m_indicator_t != t_ns) {
            m_indicator_t = t_ns;
            changed = true;
        }
        if (std::isfinite(x_norm)) {
            const double clamped_x_norm = std::clamp(x_norm, 0.0, 1.0);
            if (!m_indicator_x_norm_valid || std::abs(m_indicator_x_norm - clamped_x_norm) > 1e-12) {
                m_indicator_x_norm = clamped_x_norm;
                changed = true;
            }
            if (!m_indicator_x_norm_valid) {
                m_indicator_x_norm_valid = true;
                changed = true;
            }
        }
        else
        if (owner_changed && m_indicator_x_norm_valid) {
            m_indicator_x_norm_valid = false;
            changed = true;
        }
    }
    else {
        if (m_indicator_owner == owner && (m_indicator_active || m_indicator_x_norm_valid)) {
            m_indicator_owner        = nullptr;
            m_indicator_active       = false;
            m_indicator_x_norm_valid = false;
            changed                  = true;
        }
    }

    if (changed) {
        emit indicator_state_changed();
    }
}

bool Plot_time_axis::indicator_active() const
{
    return m_indicator_active;
}

qint64 Plot_time_axis::indicator_t() const
{
    return m_indicator_t;
}

qint64 Plot_time_axis::indicator_t_qml_ms() const
{
    return ns_to_ms_for_qml(m_indicator_t);
}

bool Plot_time_axis::indicator_x_norm_valid() const
{
    return m_indicator_x_norm_valid;
}

double Plot_time_axis::indicator_x_norm() const
{
    return m_indicator_x_norm;
}

bool Plot_time_axis::indicator_owned_by(QObject* owner) const
{
    if (!owner) {
        return false;
    }
    return m_indicator_owner == owner;
}

} // namespace vnm::plot
