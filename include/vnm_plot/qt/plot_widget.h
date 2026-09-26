#pragma once

// VNM Plot Library - Plot Widget
// QQuickRhiItem-based plot widget for Qt Quick.

#include <vnm_plot/core/types.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/series_window.h>
#include <vnm_plot/core/time_units.h>

#include <QBasicTimer>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QQuickRhiItem>

#include <QString>
#include <QVariantAnimation>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

class QQuickWindow;

namespace vnm::plot {

class Plot_renderer;
class Plot_time_axis;

namespace detail {
class plot_render_feedback_channel_t;
struct plot_render_feedback_t;
} // namespace detail

// -----------------------------------------------------------------------------
// Plot_view
// -----------------------------------------------------------------------------
struct Plot_view
{
    // Timestamps are int64 nanoseconds (API convention).
    std::optional<std::pair<qint64, qint64>> t_range;
    std::optional<std::pair<qint64, qint64>> t_available_range;
    std::optional<std::pair<float, float>>   v_range;
    std::optional<bool>                      v_auto;
};

// -----------------------------------------------------------------------------
// Plot Widget
// -----------------------------------------------------------------------------
// Qt Quick widget for rendering GPU-accelerated plots.
// This is the main public interface for the vnm_plot library.
//
// Public setters silently ignore non-finite or inverted ranges.
//
// Time-unit convention:
//
// The C++ API keeps every time value as int64 nanoseconds. QML, however,
// receives values as JS doubles, which only represent integers exactly up
// to 2^53. Modern epoch-ns values (~1.78e18) overflow that limit and would
// silently lose sub-microsecond precision in the QML layer, including any
// round-trip back to C++. To dodge the JS-double precision loss, every
// QML-facing property, signal, and Q_INVOKABLE timestamp parameter is
// expressed in milliseconds-since-epoch instead. ms-since-epoch fits
// exactly in a JS double until ~285,000 AD, so cursor/indicator tooling
// keeps integer fidelity. The conversions happen in the property
// accessors via ns_to_ms_for_qml() and ms_for_qml_to_ns(); C++ callers
// stay on the int64-ns surface (t_min(), t_max(), set_t_range(...) etc.).
class Plot_widget : public QQuickRhiItem
{
    Q_OBJECT

    // Time properties expose milliseconds-since-epoch to QML. The Q_PROPERTY
    // READ method names end with _qml_ms to make the unit obvious at the
    // boundary. Internally the widget stores int64 nanoseconds; conversion
    // happens in the getter.
    Q_PROPERTY(qint64 t_min READ t_min_qml_ms NOTIFY t_limits_changed)
    Q_PROPERTY(qint64 t_max READ t_max_qml_ms NOTIFY t_limits_changed)
    Q_PROPERTY(qint64 t_available_min READ t_available_min_qml_ms NOTIFY t_limits_changed)
    Q_PROPERTY(qint64 t_available_max READ t_available_max_qml_ms NOTIFY t_limits_changed)
    Q_PROPERTY(float v_min READ v_min NOTIFY v_limits_changed)
    Q_PROPERTY(float v_max READ v_max NOTIFY v_limits_changed)
    Q_PROPERTY(bool v_auto READ v_auto WRITE set_v_auto NOTIFY v_auto_changed)
    Q_PROPERTY(double preview_height READ preview_height WRITE set_preview_height NOTIFY preview_height_changed)
    Q_PROPERTY(double preview_height_target READ preview_height_target NOTIFY preview_height_target_changed)
    Q_PROPERTY(double preview_height_collapsed READ preview_height_collapsed CONSTANT)
    Q_PROPERTY(double reserved_height READ reserved_height NOTIFY preview_height_changed)
    Q_PROPERTY(double scaling_factor READ scaling_factor NOTIFY scaling_factor_changed)
    Q_PROPERTY(bool dark_mode READ dark_mode WRITE set_dark_mode NOTIFY dark_mode_changed)
    Q_PROPERTY(double grid_visibility READ grid_visibility WRITE set_grid_visibility NOTIFY grid_visibility_changed)
    Q_PROPERTY(
        double preview_visibility
        READ preview_visibility
        WRITE set_preview_visibility
        NOTIFY preview_visibility_changed)
    Q_PROPERTY(double line_width_px READ line_width_px WRITE set_line_width_px NOTIFY line_width_px_changed)
    Q_PROPERTY(double vbar_width_px READ vbar_width_pixels NOTIFY vbar_width_changed)
    Q_PROPERTY(double vbar_width_qml READ vbar_width_qml NOTIFY vbar_width_changed)
    Q_PROPERTY(Plot_time_axis* time_axis READ time_axis WRITE set_time_axis NOTIFY time_axis_changed)

public:
    Plot_widget();
    ~Plot_widget() override;

    // --- Data Management ---

    // Add or update a data series
    void add_series(
        int    id,
        std::shared_ptr<series_data_t>
               series);

    void apply_series_updates(
        const std::vector<std::pair<int, std::shared_ptr<series_data_t>>>&
               updates);

    // Remove a data series
    void remove_series(int id);

    // Clear all series
    void clear();

    // Get a snapshot of all series data
    std::map<int, std::shared_ptr<const series_data_t>> get_series_snapshot() const;

    // --- Configuration ---

    // Replace the complete plot configuration. Config-backed QML properties
    // are notified when their values change.
    void set_config(const Plot_config& config);
    Plot_config config() const;
    // Invalidate cached rendered ranges and request a fresh frame.
    Q_INVOKABLE void reset_view_state();

    // Dark mode
    bool dark_mode() const;
    void set_dark_mode(bool dark);

    // Grid visibility (0..1 alpha; 0 = hidden, 1 = fully visible)
    double grid_visibility() const;
    void set_grid_visibility(double visibility);

    // Preview visibility (0..1 alpha; 0 = hidden, 1 = fully visible)
    double preview_visibility() const;
    void set_preview_visibility(double visibility);

    // Line width in pixels
    double line_width_px() const;
    void set_line_width_px(double width);

    // --- Time Range ---

    qint64 t_min() const;
    qint64 t_max() const;
    qint64 t_available_min() const;
    qint64 t_available_max() const;
    // QML-facing accessors return milliseconds-since-epoch; see the
    // class-level comment for why. Not part of the C++ API surface.
    qint64 t_min_qml_ms() const;
    qint64 t_max_qml_ms() const;
    qint64 t_available_min_qml_ms() const;
    qint64 t_available_max_qml_ms() const;
    void set_t_range(qint64 t_min_ns, qint64 t_max_ns);
    void set_available_t_range(qint64 t_min_ns, qint64 t_max_ns);
    void set_view(const Plot_view& view);
    // Optional shared time axis (non-owning). When set, this widget mirrors its values.
    Plot_time_axis* time_axis() const;
    void set_time_axis(Plot_time_axis* axis);
    // Attach to another widget's time axis (no-op if missing).
    Q_INVOKABLE void attach_time_axis(Plot_widget* other);

    // --- Value Range ---

    float v_min() const;
    float v_max() const;
    bool v_auto() const;
    void set_v_auto(bool auto_scale);
    void set_v_range(float v_min, float v_max);

    // --- Preview ---

    double preview_height() const;
    void set_preview_height(double height);
    double preview_height_target() const;
    double preview_height_collapsed() const;
    double reserved_height() const;
    double scaling_factor() const;
    double vbar_width_pixels() const;
    double vbar_width_qml() const;
    Q_INVOKABLE void set_vbar_width(double vbar_width);

    // --- Interaction ---

    Q_INVOKABLE virtual void adjust_t_from_mouse_diff(double ref_width, double diff);
    Q_INVOKABLE virtual void adjust_t_from_mouse_diff_on_preview(double ref_width, double diff);
    Q_INVOKABLE virtual void adjust_t_from_mouse_pos_on_preview(double ref_width, double x_pos);
    Q_INVOKABLE virtual void adjust_t_from_pivot_and_scale(double pivot, double scale);
    Q_INVOKABLE virtual void adjust_v_from_mouse_diff(float ref_height, float diff);
    Q_INVOKABLE virtual void adjust_v_from_pivot_and_scale(float pivot, float scale);
    Q_INVOKABLE void         adjust_v_to_target(float target_vmin, float target_vmax);
    // Fit visible data. Current renderer-composed stacks contribute their
    // cumulative geometry (including AREA bases), not raw component ranges.
    Q_INVOKABLE void        auto_adjust_view(bool adjust_t, double extra_v_scale);
    Q_INVOKABLE void        auto_adjust_view(bool adjust_t, double extra_v_scale, bool anchor_zero);
    Q_INVOKABLE virtual bool can_zoom_in() const;
    Q_INVOKABLE double      update_dpi_scaling_factor();
    Q_INVOKABLE void        set_visible_info(int flags);
    Q_INVOKABLE void        set_relative_preview_height(float relative);
    Q_INVOKABLE void        set_preview_height_min(double v);
    Q_INVOKABLE void        set_preview_height_max(double v);
    Q_INVOKABLE void        set_show_if_calculated_preview_height_below_min(bool v);
    Q_INVOKABLE void        set_preview_height_steps(int steps);

    // Q_INVOKABLEs that take or return timestamps cross the QML boundary in
    // milliseconds-since-epoch. Each result map contains x, y, px, py, color,
    // and series_label; y_text is present when a value formatter is configured.
    // Interpolated stacked results keep component y values raw, add marker_y
    // and stacked_marker for cumulative marker positions, and may set
    // show_marker false when no rendered position is available. A successful
    // stack also adds a text-only synthetic series_label "Σ" row with its
    // cumulative y and show_marker false. Nearest results remain raw samples.
    Q_INVOKABLE QVariantList get_indicator_samples(
        double x_ms,
        double plot_width,
        double plot_height,
        double mouse_px = -1.0) const;

    Q_INVOKABLE QVariantList get_nearest_samples(
        double x_ms,
        double plot_width,
        double plot_height,
        double mouse_px = -1.0) const;

    Q_INVOKABLE QString format_timestamp_precise(
        qint64 timestamp_ms) const;

    // Current renderer result for a configured stack group and view. A
    // pending result means the renderer has not yet published a result for
    // the current series revision/range, or its source data has advanced.
    Stack_view_status stack_status(
        int                    group,
        Series_view_kind       view_kind) const;

    // QML schema: group (int), view/state/reason (uppercase strings), and
    // affected_series_ids (list<int>). Pass preview=true for PREVIEW.
    Q_INVOKABLE QVariantMap get_stack_status(
        int    group,
        bool   preview = false) const;

    // --- Qt Quick RHI Interface ---

    QQuickRhiItemRenderer* createRenderer() override;

signals:
    void t_limits_changed();

    void v_limits_changed();
    void v_auto_changed();
    void preview_height_changed();
    void preview_height_target_changed(double target);
    void scaling_factor_changed();
    void dark_mode_changed();
    void grid_visibility_changed();
    void preview_visibility_changed();
    void line_width_px_changed();
    void vbar_width_changed();
    void time_axis_changed();

protected:
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData& data) override;
    void timerEvent(QTimerEvent* ev) override;
    void adjust_t_to_target(qint64 target_tmin_ns, qint64 target_tmax_ns);
    std::pair<float, float> manual_v_range() const;

    bool rendered_v_range(
        float& out_min,
        float& out_max) const;

    bool rendered_t_range(
        qint64&                out_min_ns,
        qint64&                out_max_ns) const;

    // Revision of the series set, bumped whenever series are added, replaced or
    // removed. A pass renders one revision and reports it back through
    // apply_render_feedback, which is how the widget tells a current rendered
    // result from one the series have moved past.
    std::uint64_t series_revision() const;

    // Applies a completed render pass to the rendered-state mirror that QML
    // reads. The renderer delivers its passes through the feedback channel,
    // which ends here.
    void apply_render_feedback(detail::plot_render_feedback_t feedback);

private:
    enum class Indicator_sample_mode
    {
        Interpolated,
        Nearest,
    };

    QVariantList get_samples_for_time(
        double x_ms,
        double plot_width,
        double plot_height,
        double mouse_px,
        Indicator_sample_mode
               mode) const;

    std::optional<std::vector<std::pair<int, double>>> rendered_stack_values(
        int                    group,
        std::size_t            member_count,
        qint64                 t_min_ns,
        qint64                 t_max_ns,
        qint64                 x_ns) const;

    // Plot_renderer reads m_config / m_data_cfg under the matching shared_mutexes
    // during synchronize(); friending lets it touch those members directly
    // instead of going through accessors that would re-take the same locks.
    friend class Plot_renderer;

    // Lock order (if ever needed concurrently): config -> data_cfg -> series.
    // Prefer holding only one lock at a time.

    // Configuration
    Plot_config                    m_config;
    std::atomic<std::uint64_t>     m_config_revision{0};
    mutable std::shared_mutex      m_config_mutex;

    // Data configuration
    data_config_t                  m_data_cfg;
    mutable std::shared_mutex      m_data_cfg_mutex;

    // Series data
    std::map<int, std::shared_ptr<const series_data_t>>
                                   m_series;
    mutable std::shared_mutex      m_series_mutex;
    std::atomic<std::uint64_t>     m_series_revision{0};

    // UI state
    std::atomic<bool>              m_v_auto{true};
    std::atomic<int>               m_visible_info_flags{k_visible_info_all};
    // GUI-thread mirror. The feedback channel owns cross-thread transfer.
    float                         m_rendered_v_min = 0.0f;
    float                         m_rendered_v_max = 1.0f;
    bool                          m_rendered_v_range_valid = false;
    // Rendered time range cached for QML/tooltips, in int64 nanoseconds.
    qint64                        m_rendered_t_min = 0;
    qint64                        m_rendered_t_max = 1;
    bool                          m_rendered_t_range_valid = false;
    std::shared_ptr<detail::plot_render_feedback_channel_t>
                                   m_render_feedback_channel;
    QMetaObject::Connection        m_render_feedback_completion_connection;
    QMetaObject::Connection        m_render_feedback_delivery_connection;
    std::uint64_t                  m_render_feedback_generation = 0;
    std::map<int, std::vector<detail::Rendered_stack_source>>
                                   m_rendered_stack_validity;
    std::map<std::pair<int, Series_view_kind>, detail::Rendered_stack_status>
                                   m_rendered_stack_statuses;
    qint64                         m_rendered_stack_t_min           = 0;
    qint64                         m_rendered_stack_t_max           = 0;
    qint64                         m_rendered_stack_available_t_min = 0;
    qint64                         m_rendered_stack_available_t_max = 0;
    std::uint64_t                  m_rendered_stack_series_revision = 0;
    std::atomic<double>            m_vbar_width_px{0.0};
    QBasicTimer                    m_vbar_width_timer;
    QElapsedTimer                  m_vbar_width_anim_elapsed;
    double                         m_vbar_width_anim_start_px                    = 0.0;
    double                         m_vbar_width_anim_target_px                   = 0.0;

    double                         m_preview_height                              = 0.0;
    double                         m_preview_height_target                       = 0.0;
    QVariantAnimation              m_preview_height_animation;
    bool                           m_preview_height_initialized                  = false;
    float                          m_relative_preview_height                     = 0.0f;
    double                         m_preview_height_min                          = 0.0;
    double                         m_preview_height_max                          = 0.0;
    bool                           m_show_if_calculated_preview_height_below_min = true;
    int                            m_preview_height_steps                        = 0;

    double                         m_adjusted_font_size                          = 12.0;
    double                         m_base_label_height                           = 14.0;
    double                         m_scaling_factor                              = 1.0;

    void apply_preview_height(double height);
    void recalculate_preview_height(bool resume_automatic = false);
    double compute_preview_height_px(double widget_height_px) const;
    std::pair<float, float> current_v_range() const;
    data_config_t data_cfg_snapshot() const;

    template<typename Field, typename Value, typename Signal>
    void update_config_field(Field& field, Value new_value, Signal signal);

    // Assigns new_value into field if it differs and invokes on_change.
    // Returns true on a real change. Unlike update_config_field, this does
    // not lock m_config_mutex or emit a signal directly; the on_change
    // closure is responsible for whatever follow-up the caller needs
    // (typically recalculate_preview_height).
    template<typename Field, typename Value, typename OnChange>
    static bool set_if_changed(Field& field, Value new_value, OnChange&& on_change)
    {
        if (field == static_cast<Field>(new_value)) {
            return false;
        }
        field = static_cast<Field>(new_value);
        std::forward<OnChange>(on_change)();
        return true;
    }

    void arm_render_feedback_delivery(
        const std::shared_ptr<detail::plot_render_feedback_channel_t>& channel);
    void deliver_render_feedback(
        const std::shared_ptr<detail::plot_render_feedback_channel_t>& channel);
    void set_rendered_v_range(float v_min, float v_max);
    void set_rendered_t_range(qint64 t_min_ns, qint64 t_max_ns);
    template<typename Update>
    void apply_time_update(Update&& update_time);
    void sync_time_axis_state();
    void clear_time_axis();
    void handle_window_changed(QQuickWindow* window);
    void invalidate_display_context();
    void observe_screen();
    void refresh_lcd_order();
    void apply_vbar_width_target(double px, bool publish_shared = false);

    QPointer<Plot_time_axis>       m_time_axis;
    QMetaObject::Connection        m_time_axis_connection;
    QMetaObject::Connection        m_time_axis_destroyed_connection;
    QMetaObject::Connection        m_time_axis_vbar_connection;
    QMetaObject::Connection        m_time_axis_sync_vbar_connection;
    QMetaObject::Connection        m_window_changed_connection;
    QMetaObject::Connection        m_window_screen_connection;
    std::vector<QMetaObject::Connection> m_screen_connections;
    std::unique_ptr<QObject>       m_lcd_settings_observer;
    lcd_subpixel_order_t           m_auto_lcd_order = lcd_subpixel_order_t::NONE;
};

} // namespace vnm::plot
