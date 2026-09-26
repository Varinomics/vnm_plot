#include <vnm_plot/qt/plot_interaction_item.h>

#include <vnm_plot/core/time_units.h>

#include <QMouseEvent>
#include <QWheelEvent>
#include <QHoverEvent>
#include <QTimerEvent>

#include <algorithm>
#include <cmath>

namespace vnm::plot {

Plot_interaction_item::Plot_interaction_item(QQuickItem* parent)
:
    QQuickItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
}

Plot_interaction_item::~Plot_interaction_item()
{
    m_zoom_timer.stop();
}

Plot_widget* Plot_interaction_item::plot_widget() const
{
    return m_plot_widget;
}

void Plot_interaction_item::set_plot_widget(Plot_widget* widget)
{
    if (m_plot_widget == widget) {
        return;
    }
    m_plot_widget = widget;
    emit plot_widget_changed();
}

Plot_widget* Plot_interaction_item::time_plot_widget() const
{
    return m_time_plot_widget;
}

void Plot_interaction_item::set_time_plot_widget(Plot_widget* widget)
{
    if (m_time_plot_widget == widget) {
        return;
    }
    m_time_plot_widget = widget;
    emit time_plot_widget_changed();
}

bool Plot_interaction_item::pin_time_pivot_to_right() const
{
    return m_pin_time_pivot_to_right;
}

void Plot_interaction_item::set_pin_time_pivot_to_right(bool pinned)
{
    if (m_pin_time_pivot_to_right == pinned) {
        return;
    }
    m_pin_time_pivot_to_right = pinned;
    emit pin_time_pivot_to_right_changed();
}

bool Plot_interaction_item::is_interaction_enabled() const
{
    return m_interaction_enabled;
}

void Plot_interaction_item::set_interaction_enabled(bool enabled)
{
    if (m_interaction_enabled == enabled) {
        return;
    }
    m_interaction_enabled = enabled;
    emit interaction_enabled_changed();
}

qreal Plot_interaction_item::usable_width() const
{
    if (!m_plot_widget) {
        return width();
    }
    return width() - m_plot_widget->vbar_width_qml();
}

qreal Plot_interaction_item::usable_height() const
{
    if (!m_plot_widget) {
        return height();
    }
    return height() - m_plot_widget->reserved_height();
}

qreal Plot_interaction_item::preview_height() const
{
    if (!m_plot_widget) {
        return 0.0;
    }
    return m_plot_widget->preview_height();
}

qreal Plot_interaction_item::t_stop_min() const
{
    const auto* target = time_target_widget();
    if (!target) {
        return 0.0;
    }

    const auto t_available_span = positive_span_ns_as_long_double(
        target->t_available_min(),
        target->t_available_max());
    if (!t_available_span) {
        return 0.0;
    }

    const long double denominator = std::max(1.0L, *t_available_span);
    const long double numerator = span_ns_as_long_double(
        target->t_available_min(),
        target->t_min());
    return static_cast<qreal>(numerator / denominator);
}

qreal Plot_interaction_item::t_stop_max() const
{
    const auto* target = time_target_widget();
    if (!target) {
        return 1.0;
    }

    const auto t_available_span = positive_span_ns_as_long_double(
        target->t_available_min(),
        target->t_available_max());
    if (!t_available_span) {
        return 1.0;
    }

    const long double denominator = std::max(1.0L, *t_available_span);
    const long double numerator = span_ns_as_long_double(
        target->t_max(),
        target->t_available_max());
    return 1.0 - static_cast<qreal>(numerator / denominator);
}

qreal Plot_interaction_item::zoom_animation_scale_factor(qreal velocity, qreal elapsed_timer_steps)
{
    if (elapsed_timer_steps <= 0.0) {
        return 1.0;
    }

    const qreal velocity_decay = std::pow(k_zoom_friction, elapsed_timer_steps);
    const qreal integrated_velocity = std::abs(1.0 - k_zoom_friction) > 1e-12
        ? velocity * (1.0 - velocity_decay) / (1.0 - k_zoom_friction)
        : velocity * elapsed_timer_steps;
    static const qreal s_base_k =
        std::pow(k_zoom_per_notch, (1.0 - k_zoom_friction) / k_zoom_impulse_per_step);

    return std::pow(s_base_k, integrated_velocity);
}

qreal Plot_interaction_item::zoom_animation_velocity_after(
    qreal  velocity,
    qreal  elapsed_timer_steps)
{
    if (elapsed_timer_steps <= 0.0) {
        return velocity;
    }

    return velocity * std::pow(k_zoom_friction, elapsed_timer_steps);
}

void Plot_interaction_item::apply_zoom_step()
{
    apply_zoom_step(std::chrono::steady_clock::now());
}

void Plot_interaction_item::apply_zoom_step(std::chrono::steady_clock::time_point now)
{
    if (!m_plot_widget) {
        m_zoom_timer.stop();
        return;
    }

    constexpr qreal eps        = 1e-3;
    bool            active     = false;
    qreal           elapsed_ms = std::chrono::duration<qreal, std::milli>(now - m_last_zoom_step_time).count();
    m_last_zoom_step_time = now;

    if (elapsed_ms <= 0.0) {
        elapsed_ms = k_zoom_timer_interval_ms;
    }

    const qreal dt = elapsed_ms / k_zoom_timer_interval_ms;

    if (std::abs(m_zoom_vel_t) > eps) {
        Plot_widget* target = time_target_widget();
        if (!target) {
            m_zoom_vel_t = 0.0;
        }
        else {
            const qreal factor_t = zoom_animation_scale_factor(m_zoom_vel_t, dt);
            if (factor_t < 1.0 && !target->can_zoom_in()) {
                m_zoom_vel_t = 0.0;
            }
            else {
                target->adjust_t_from_pivot_and_scale(m_last_pivot_x, factor_t);
                m_zoom_vel_t = zoom_animation_velocity_after(m_zoom_vel_t, dt);
                active = true;
            }
        }
    }

    if (std::abs(m_zoom_vel_v) > eps) {
        const qreal factor_v = zoom_animation_scale_factor(m_zoom_vel_v, dt);
        m_plot_widget->adjust_v_from_pivot_and_scale(m_last_pivot_y, factor_v);
        m_zoom_vel_v = zoom_animation_velocity_after(m_zoom_vel_v, dt);
        active = true;
    }

    if (!active) {
        m_zoom_timer.stop();
    }
}

Plot_widget* Plot_interaction_item::time_target_widget() const
{
    return m_time_plot_widget ? m_time_plot_widget : m_plot_widget;
}

void Plot_interaction_item::mousePressEvent(QMouseEvent* event)
{
    if (!m_interaction_enabled || !m_plot_widget) {
        event->ignore();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const qreal y  = event->position().y();
    const qreal x  = event->position().x();
    const qreal uw = usable_width();
    const qreal uh = usable_height();
    const qreal ph = preview_height();

    forceActiveFocus(Qt::MouseFocusReason);

    if (x >= 0.0 && x <= uw && y >= 0.0 && y < uh) {
        m_dragging        = true;
        m_click_candidate = true;
        m_press_x         = x;
        m_press_y         = y;
        m_drag_start_x    = x;
        m_drag_last_y     = y;
        event->accept();
    }
    else
    if (ph > 0 && y > height() - ph) {
        const qreal stop_min = t_stop_min();
        const qreal stop_max = t_stop_max();
        if (x < width() * stop_min || x > width() * stop_max) {
            if (auto* target = time_target_widget()) {
                target->adjust_t_from_mouse_pos_on_preview(width(), x);
            }
        }
        m_dragging_preview = true;
        m_drag_preview_start = x;
        event->accept();
    }
    else {
        event->ignore();
    }
}

void Plot_interaction_item::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_interaction_enabled) {
        return;
    }

    const qreal x = event->position().x();
    const qreal y = event->position().y();

    emit mouse_position_changed(x, y);

    if (!m_plot_widget) {
        return;
    }

    if (m_dragging) {
        const qreal move_dx = x - m_press_x;
        const qreal move_dy = y - m_press_y;
        if ((move_dx * move_dx + move_dy * move_dy) <=
            (k_click_move_tolerance_px * k_click_move_tolerance_px))
        {
            return;
        }
        if (m_click_candidate) {
            m_click_candidate = false;
        }

        const auto mods      = event->modifiers();
        const bool ctrl_held = mods & Qt::ControlModifier;
        const bool alt_held  = mods & Qt::AltModifier;

        if (ctrl_held || alt_held) {
            const qreal dy = y - m_drag_last_y;
            m_drag_last_y = y;
            m_plot_widget->adjust_v_from_mouse_diff(usable_height(), dy);
        }

        if (!alt_held) {
            if (auto* target = time_target_widget()) {
                target->adjust_t_from_mouse_diff(usable_width(), x - m_drag_start_x);
            }
        }
        m_drag_start_x = x;
    }
    else
    if (m_dragging_preview) {
        if (auto* target = time_target_widget()) {
            target->adjust_t_from_mouse_diff_on_preview(width(), x - m_drag_preview_start);
        }
        m_drag_preview_start = x;
    }
}

void Plot_interaction_item::mouseReleaseEvent(QMouseEvent* event)
{
    const qreal x       = event->position().x();
    const qreal y       = event->position().y();
    const qreal move_dx = x - m_press_x;
    const qreal move_dy = y - m_press_y;
    const bool within_click_tolerance =
        (move_dx * move_dx + move_dy * move_dy) <=
        (k_click_move_tolerance_px * k_click_move_tolerance_px);
    const bool clicked = m_interaction_enabled &&
        m_dragging &&
        m_click_candidate &&
        within_click_tolerance &&
        m_press_x >= 0.0 && m_press_x <= usable_width() &&
        m_press_y >= 0.0 && m_press_y < usable_height();

    m_dragging           = false;
    m_dragging_preview   = false;
    m_click_candidate    = false;
    m_drag_start_x       = 0;
    m_drag_preview_start = 0;

    if (clicked) {
        emit mouse_clicked(m_press_x, m_press_y);
    }
}

void Plot_interaction_item::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!m_interaction_enabled || !m_plot_widget || event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    m_dragging = false;
    m_dragging_preview = false;
    m_click_candidate = false;
    forceActiveFocus(Qt::MouseFocusReason);
    emit mouse_double_clicked(event->position().x(), event->position().y());
    event->accept();
}

void Plot_interaction_item::wheelEvent(QWheelEvent* event)
{
    if (!m_interaction_enabled || !m_plot_widget) {
        event->ignore();
        return;
    }

    const QPoint angle_delta = event->angleDelta();
    const QPoint pixel_delta = event->pixelDelta();
    if (!handle_wheel(
            event->position().x(), event->position().y(), angle_delta.x(),
            angle_delta.y(), pixel_delta.x(), pixel_delta.y(), event->modifiers().toInt()))
    {
        event->ignore();
        return;
    }

    m_click_candidate = false;
    forceActiveFocus(Qt::MouseFocusReason);
    event->accept();
}

bool Plot_interaction_item::handle_wheel(
    qreal  x,
    qreal  y,
    qreal  angle_delta_x,
    qreal  angle_delta_y,
    qreal  pixel_delta_x,
    qreal  pixel_delta_y,
    int    modifiers)
{
    if (!m_interaction_enabled || !m_plot_widget) {
        return false;
    }

    const qreal ph = preview_height();

    if (ph > 0 && y > height() - (ph - 1)) {
        return false;
    }

    qreal dy = angle_delta_y;
    if (dy == 0.0) { dy = pixel_delta_y; }
    if (dy == 0.0) { dy = angle_delta_x; }
    if (dy == 0.0) { dy = pixel_delta_x; }
    if (dy == 0.0) { return false;       }

    const auto  mods       = Qt::KeyboardModifiers::fromInt(modifiers);
    const qreal steps      = dy / 120.0;
    const qreal impulse    = -steps * k_zoom_impulse_per_step;
    const bool  zoom_both  = mods.testFlag(Qt::ControlModifier);
    const bool  zoom_alt   = mods.testFlag(Qt::AltModifier);
    const bool  zoom_value = zoom_both || zoom_alt;
    const bool  zoom_time  = zoom_both || !zoom_value;

    Plot_widget* time_target   = time_target_widget();
    const bool   time_allowed  = zoom_time && time_target && (impulse >= 0 || time_target->can_zoom_in());
    const bool   value_allowed = zoom_value;
    if (!time_allowed && !value_allowed) {
        return false;
    }

    if (m_zoom_timer.isActive()) {
        apply_zoom_step();
    }

    const qreal uw = std::max(qreal{0.0}, usable_width());
    const qreal uh = std::max(qreal{0.0}, usable_height());
    const qreal wx = m_pin_time_pivot_to_right && time_allowed
        ? uw
        : std::clamp(x, qreal{0.0}, uw);
    const qreal wy = std::clamp(y, qreal{0.0}, uh);
    const qreal px = uw > 0 ? (wx / uw) : 0.5;
    const qreal py = uh > 0 ? (wy / uh) : 0.5;

    m_last_pivot_x = px;
    m_last_pivot_y = py;

    if (time_allowed) {
        m_zoom_vel_t += impulse;
    }
    else {
        m_zoom_vel_t = 0.0;
    }

    if (value_allowed) {
        m_zoom_vel_v += impulse;
    }
    else {
        m_zoom_vel_v = 0.0;
    }

    m_zoom_vel_t = std::clamp(m_zoom_vel_t, -k_zoom_max_vel, k_zoom_max_vel);
    m_zoom_vel_v = std::clamp(m_zoom_vel_v, -k_zoom_max_vel, k_zoom_max_vel);

    const auto now = std::chrono::steady_clock::now();
    m_last_zoom_step_time = now - std::chrono::milliseconds(k_zoom_timer_interval_ms);

    apply_zoom_step(now);

    if (!m_zoom_timer.isActive()) {
        m_zoom_timer.start(k_zoom_timer_interval_ms, this);
    }

    return true;
}

void Plot_interaction_item::hoverEnterEvent(QHoverEvent* event)
{
    // Always emit hover signals regardless of interaction_enabled.
    // The indicator (value display) should work even when pan/zoom is disabled.
    emit mouse_position_changed(event->position().x(), event->position().y());
}

void Plot_interaction_item::hoverLeaveEvent(QHoverEvent* event)
{
    Q_UNUSED(event)
    // Always emit mouse_exited regardless of interaction_enabled.
    emit mouse_exited();
}

void Plot_interaction_item::hoverMoveEvent(QHoverEvent* event)
{
    // Always emit hover signals regardless of interaction_enabled.
    emit mouse_position_changed(event->position().x(), event->position().y());
}

void Plot_interaction_item::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == m_zoom_timer.timerId()) {
        apply_zoom_step();
    }
    else {
        QQuickItem::timerEvent(event);
    }
}

} // namespace vnm::plot
