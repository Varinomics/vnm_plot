#pragma once

#include <vnm_plot/qt/plot_widget.h>

#include <QBasicTimer>
#include <QQuickItem>

#include <chrono>

namespace vnm::plot {

class Plot_interaction_item : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(Plot_widget* plot_widget READ plot_widget WRITE set_plot_widget NOTIFY plot_widget_changed REQUIRED)
    Q_PROPERTY(
        Plot_widget* time_plot_widget
        READ time_plot_widget
        WRITE set_time_plot_widget
        NOTIFY time_plot_widget_changed)
    Q_PROPERTY(
        bool pin_time_pivot_to_right
        READ pin_time_pivot_to_right
        WRITE set_pin_time_pivot_to_right
        NOTIFY pin_time_pivot_to_right_changed)
    Q_PROPERTY(
        bool interaction_enabled
        READ is_interaction_enabled
        WRITE set_interaction_enabled
        NOTIFY interaction_enabled_changed)

public:
    explicit Plot_interaction_item(QQuickItem* parent = nullptr);
    ~Plot_interaction_item() override;

    static qreal zoom_animation_scale_factor(qreal velocity, qreal elapsed_timer_steps);
    static qreal zoom_animation_velocity_after(qreal velocity, qreal elapsed_timer_steps);

    Plot_widget* plot_widget() const;
    void set_plot_widget(Plot_widget* widget);

    Plot_widget* time_plot_widget() const;
    void set_time_plot_widget(Plot_widget* widget);

    bool pin_time_pivot_to_right() const;
    void set_pin_time_pivot_to_right(bool pinned);

    bool is_interaction_enabled() const;

    void set_interaction_enabled(
        bool           enabled);

    Q_INVOKABLE bool handle_wheel(
        qreal          x,
        qreal          y,
        qreal          angle_delta_x,
        qreal          angle_delta_y,
        qreal          pixel_delta_x,
        qreal          pixel_delta_y,
        int            modifiers);

signals:
    void plot_widget_changed();

    void time_plot_widget_changed();
    void pin_time_pivot_to_right_changed();
    void interaction_enabled_changed();
    void mouse_position_changed(qreal x, qreal y);
    void mouse_exited();
    void mouse_clicked(qreal x, qreal y);
    void mouse_double_clicked(qreal x, qreal y);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void hoverEnterEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private:
    qreal usable_width() const;
    qreal usable_height() const;
    qreal preview_height() const;
    qreal t_stop_min() const;
    qreal t_stop_max() const;
    void apply_zoom_step();
    void apply_zoom_step(std::chrono::steady_clock::time_point now);
    Plot_widget* time_target_widget() const;

    Plot_widget*           m_plot_widget = nullptr;
    Plot_widget*           m_time_plot_widget = nullptr;
    bool                   m_interaction_enabled = true;
    bool                   m_pin_time_pivot_to_right = false;

    bool                   m_dragging = false;
    bool                   m_dragging_preview = false;
    bool                   m_click_candidate = false;
    qreal                  m_press_x = 0;
    qreal                  m_press_y = 0;
    qreal                  m_drag_start_x = 0;
    qreal                  m_drag_last_y = 0;
    qreal                  m_drag_preview_start = 0;

    qreal                  m_zoom_vel_t = 0.0;
    qreal                  m_zoom_vel_v = 0.0;
    qreal                  m_last_pivot_x = 0.5;
    qreal                  m_last_pivot_y = 0.5;
    QBasicTimer            m_zoom_timer;
    std::chrono::steady_clock::time_point
                           m_last_zoom_step_time;

    static constexpr qreal k_zoom_friction           = 0.75;
    static constexpr qreal k_zoom_impulse_per_step   = 1.0;
    static constexpr qreal k_zoom_max_vel            = 5.0;
    static constexpr qreal k_zoom_per_notch          = 1.05;
    static constexpr qreal k_click_move_tolerance_px = 4.0;
    static constexpr int   k_zoom_timer_interval_ms  = 16;
};

} // namespace vnm::plot
