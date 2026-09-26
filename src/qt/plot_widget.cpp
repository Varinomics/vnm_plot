#include <vnm_plot/qt/plot_widget.h>
#include "plot_renderer.h"
#include "lcd_resolver.h"
#include "plot_render_feedback.h"
#include "qml_resources.h"
#include "t_axis_adjust.h"
#include <vnm_plot/qt/plot_time_axis.h>
#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/rhi/qrhi_series_layer.h>
#include <vnm_plot/rhi/series_data.h>
#include <vnm_plot/rhi/series_renderer.h>
#include "../core/series_window_planner.h"

#include <QGuiApplication>
#include <QDebug>
#include <QQuickWindow>
#include <QScreen>
#include <QWindow>

#include <QColor>
#include <QVariantMap>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace {

double dpi_scaling_for_window(const QQuickWindow* window)
{
    if (window) {
        return window->effectiveDevicePixelRatio();
    }
    const auto* const screen = QGuiApplication::primaryScreen();
    return screen ? screen->devicePixelRatio() : 1.0;
}

} // anonymous namespace

namespace vnm::plot {
using detail::k_vbar_width_change_threshold_d;
using detail::min_v_span_for;

namespace {

detail::Time_axis_model widget_time_axis_model(const data_config_t& cfg)
{
    return detail::Time_axis_model::initialized(
        cfg.t_min,
        cfg.t_max,
        cfg.t_available_min,
        cfg.t_available_max);
}

void apply_time_axis_model_to_data_config(
    const detail::Time_axis_model& model,
    data_config_t&                 cfg)
{
    cfg.t_min           = model.t_min();
    cfg.t_max           = model.t_max();
    cfg.t_available_min = model.t_available_min();
    cfg.t_available_max = model.t_available_max();
}

template<typename Update>
bool apply_time_axis_update_to_data_config(data_config_t& cfg, Update&& update)
{
    auto       model  = widget_time_axis_model(cfg);
    const auto result = std::forward<Update>(update)(model);
    if (!result.accepted) {
        return false;
    }
    apply_time_axis_model_to_data_config(model, cfg);
    return true;
}

std::optional<std::int64_t> indicator_ms_to_ns(double value_ms)
{
    if (!std::isfinite(value_ms)) {
        return std::nullopt;
    }

    constexpr double k_max_ms =
        static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
        static_cast<double>(k_ns_per_ms);
    constexpr double k_min_ms =
        static_cast<double>(std::numeric_limits<std::int64_t>::min()) /
        static_cast<double>(k_ns_per_ms);
    if (value_ms >= k_max_ms) { return std::numeric_limits<std::int64_t>::max(); }
    if (value_ms <= k_min_ms) { return std::numeric_limits<std::int64_t>::min(); }

    double       whole_ms      = 0.0;
    const double fractional_ms = std::modf(value_ms, &whole_ms);
    const auto   fractional_ns = static_cast<std::int64_t>(
        std::round(fractional_ms * static_cast<double>(k_ns_per_ms)));
    return saturating_add_ns(
        saturating_ms_to_ns(static_cast<std::int64_t>(whole_ms)),
        fractional_ns);
}

const char* stack_state_name(Stack_view_state state)
{
    switch (state) {
        case Stack_view_state::PENDING:    return "PENDING";
        case Stack_view_state::ACTIVE:     return "ACTIVE";
        case Stack_view_state::SUPPRESSED: return "SUPPRESSED";
    }
    return "PENDING";
}

const char* stack_reason_name(Stack_rejection_reason reason)
{
    switch (reason) {
        case Stack_rejection_reason::NONE:                    return "NONE";
        case Stack_rejection_reason::MIXED_INTERPOLATION:     return "MIXED_INTERPOLATION";
        case Stack_rejection_reason::NO_DRAWABLE_DATA:        return "NO_DRAWABLE_DATA";
        case Stack_rejection_reason::INCOMPATIBLE_DATA:       return "INCOMPATIBLE_DATA";
        case Stack_rejection_reason::NONMONOTONIC_TIMESTAMPS: return "NONMONOTONIC_TIMESTAMPS";
        case Stack_rejection_reason::NO_COMMON_DOMAIN:        return "NO_COMMON_DOMAIN";
        case Stack_rejection_reason::CUMULATIVE_OVERFLOW:     return "CUMULATIVE_OVERFLOW";
        case Stack_rejection_reason::OUTPUT_LIMIT:            return "OUTPUT_LIMIT";
    }
    return "NONE";
}

} // anonymous namespace

Plot_widget::Plot_widget()
    : QQuickRhiItem()
{
    detail::initialize_qml_resources();

    m_relative_preview_height                     = 0.3f;
    m_preview_height_min                          = 30.0;
    m_preview_height_max                          = 150.0;
    m_show_if_calculated_preview_height_below_min = false;
    m_preview_height_steps                        = 2;

    m_preview_height_animation.setDuration(250);
    QObject::connect(&m_preview_height_animation, &QVariantAnimation::valueChanged,
        this, [this](const QVariant& value) { apply_preview_height(value.toDouble()); });

    update_dpi_scaling_factor();
    m_lcd_settings_observer = observe_lcd_settings([this] { invalidate_display_context(); });
    observe_screen();
    refresh_lcd_order();

    // Keep the compositor texture coordinates unmirrored. Backend clip-space
    // and framebuffer orientation are handled by the renderer's QRhi
    // projection correction.
    setMirrorVertically(false);
    setSmooth(false);
    setAlphaBlending(true);

    // Match the multisample count used by the benchmark and examples.
    setSampleCount(k_msaa_samples);
    setFlag(ItemHasContents, true);

    m_window_changed_connection = QObject::connect(
        this,
        &QQuickItem::windowChanged,
        this,
        &Plot_widget::handle_window_changed);
}

Plot_widget::~Plot_widget()
{
    m_lcd_settings_observer.reset();
    for (const auto& connection : m_screen_connections) {
        QObject::disconnect(connection);
    }
    // ~QQuickItem detaches from its parent item and emits windowChanged from
    // inside the base destructor, long after this subobject and its mutexes
    // are gone. Drop the connection before anything else so that path cannot
    // re-enter a destroyed Plot_widget.
    QObject::disconnect(m_window_changed_connection);
    m_window_changed_connection = {};

    m_vbar_width_timer.stop();
    QObject::disconnect(m_render_feedback_completion_connection);
    m_render_feedback_completion_connection = {};
    QObject::disconnect(m_render_feedback_delivery_connection);
    m_render_feedback_delivery_connection = {};
    m_render_feedback_channel.reset();
    QObject::disconnect(m_window_screen_connection);
    m_window_screen_connection = {};

    if (m_time_axis) {
        if (m_time_axis->sync_vbar_width()) {
            m_time_axis->clear_shared_vbar_width(this);
        }
        if (m_time_axis->indicator_owned_by(this)) {
            m_time_axis->set_indicator_state(this, false, 0.0);
        }
        QObject::disconnect(m_time_axis, nullptr, this, nullptr);
        m_time_axis_connection           = {};
        m_time_axis_destroyed_connection = {};
        m_time_axis_vbar_connection      = {};
        m_time_axis_sync_vbar_connection = {};
        m_time_axis = nullptr;
    }
}

void Plot_widget::add_series(int id, std::shared_ptr<series_data_t> series)
{
    apply_series_updates({{id, std::move(series)}});
}

void Plot_widget::apply_series_updates(const std::vector<std::pair<int, std::shared_ptr<series_data_t>>>& updates)
{
    if (updates.empty()) {
        return;
    }

    std::vector<std::pair<int, std::shared_ptr<const series_data_t>>> copies;
    copies.reserve(updates.size());
    for (const auto& [id, series] : updates) {
        if (series) {
            copies.emplace_back(id, series->clone());
        }
        else {
            copies.emplace_back(id, std::shared_ptr<const series_data_t>{});
        }
    }

    {
        std::unique_lock lock(m_series_mutex);
        for (auto& [id, series] : copies) {
            m_series[id] = std::move(series);
        }
        m_series_revision.fetch_add(1, std::memory_order_release);
    }
    update();
}

void Plot_widget::remove_series(int id)
{
    std::unique_lock lock(m_series_mutex);
    m_series.erase(id);
    m_series_revision.fetch_add(1, std::memory_order_release);
    update();
}

void Plot_widget::clear()
{
    std::unique_lock lock(m_series_mutex);
    m_series.clear();
    m_series_revision.fetch_add(1, std::memory_order_release);
    update();
}

std::map<int, std::shared_ptr<const series_data_t>> Plot_widget::get_series_snapshot() const
{
    std::shared_lock lock(m_series_mutex);
    return m_series;
}

void Plot_widget::set_config(const Plot_config& config)
{
    Plot_config effective_config;
    bool        dark_changed    = false;
    bool        grid_changed    = false;
    bool        preview_changed = false;
    bool        line_changed    = false;
    bool        resume_automatic_preview = false;
    bool        lcd_mode_changed = false;

    {
        std::unique_lock lock(m_config_mutex);
        dark_changed    = m_config.dark_mode != config.dark_mode;
        lcd_mode_changed = m_config.lcd_request.automatic != config.lcd_request.automatic;
        grid_changed    = m_config.grid_visibility != config.grid_visibility;
        preview_changed = m_config.preview_visibility != config.preview_visibility;
        line_changed    = m_config.line_width_px != config.line_width_px;
        resume_automatic_preview = m_config.preview_height_px > 0.0 &&
            !(config.preview_height_px > 0.0);
        m_config        = config;
        m_config_revision.fetch_add(1, std::memory_order_relaxed);
        effective_config = m_config;
    }
    if (lcd_mode_changed) {
        refresh_lcd_order();
    }
    if (dark_changed) {
        emit dark_mode_changed();
    }
    if (grid_changed) {
        emit grid_visibility_changed();
    }
    if (preview_changed) {
        emit preview_visibility_changed();
    }
    if (line_changed) {
        emit line_width_px_changed();
    }
    m_adjusted_font_size = effective_config.font_size_px * m_scaling_factor;
    m_base_label_height = effective_config.base_label_height_px * m_scaling_factor;
    if (effective_config.preview_height_px > 0.0) {
        set_preview_height(effective_config.preview_height_px);
    }
    else {
        recalculate_preview_height(resume_automatic_preview);
    }
    update();
}

Plot_config Plot_widget::config() const
{
    std::shared_lock lock(m_config_mutex);
    return m_config;
}

void Plot_widget::reset_view_state()
{
    m_rendered_v_range_valid = false;
    m_rendered_t_range_valid = false;
    update();
}

bool Plot_widget::dark_mode() const
{
    std::shared_lock lock(m_config_mutex);
    return m_config.dark_mode;
}

template <typename Field, typename Value, typename Signal>
void Plot_widget::update_config_field(Field& field, Value new_value, Signal signal)
{
    {
        std::unique_lock lock(m_config_mutex);
        if (field == new_value) {
            return;
        }
        field = static_cast<Field>(new_value);
        m_config_revision.fetch_add(1, std::memory_order_relaxed);
    }
    (this->*signal)();
    update();
}

void Plot_widget::set_dark_mode(bool dark)
{
    update_config_field(m_config.dark_mode, dark, &Plot_widget::dark_mode_changed);
}

double Plot_widget::grid_visibility() const
{
    std::shared_lock lock(m_config_mutex);
    return m_config.grid_visibility;
}

void Plot_widget::set_grid_visibility(double visibility)
{
    update_config_field(
        m_config.grid_visibility,
        std::clamp(visibility, 0.0, 1.0),
        &Plot_widget::grid_visibility_changed);
}

double Plot_widget::preview_visibility() const
{
    std::shared_lock lock(m_config_mutex);
    return m_config.preview_visibility;
}

void Plot_widget::set_preview_visibility(double visibility)
{
    update_config_field(
        m_config.preview_visibility,
        std::clamp(visibility, 0.0, 1.0),
        &Plot_widget::preview_visibility_changed);
}

double Plot_widget::line_width_px() const
{
    std::shared_lock lock(m_config_mutex);
    return m_config.line_width_px;
}

void Plot_widget::set_line_width_px(double width)
{
    update_config_field(m_config.line_width_px, width, &Plot_widget::line_width_px_changed);
}

qint64 Plot_widget::t_min() const
{
    std::shared_lock lock(m_data_cfg_mutex);
    return m_data_cfg.t_min;
}

qint64 Plot_widget::t_max() const
{
    std::shared_lock lock(m_data_cfg_mutex);
    return m_data_cfg.t_max;
}

qint64 Plot_widget::t_available_min() const
{
    std::shared_lock lock(m_data_cfg_mutex);
    return m_data_cfg.t_available_min;
}

qint64 Plot_widget::t_available_max() const
{
    std::shared_lock lock(m_data_cfg_mutex);
    return m_data_cfg.t_available_max;
}

qint64 Plot_widget::t_min_qml_ms() const
{
    return ns_to_ms_for_qml(t_min());
}

qint64 Plot_widget::t_max_qml_ms() const
{
    return ns_to_ms_for_qml(t_max());
}

qint64 Plot_widget::t_available_min_qml_ms() const
{
    return ns_to_ms_for_qml(t_available_min());
}

qint64 Plot_widget::t_available_max_qml_ms() const
{
    return ns_to_ms_for_qml(t_available_max());
}

template<typename Update>
void Plot_widget::apply_time_update(Update&& update_time)
{
    if (m_time_axis) {
        std::forward<Update>(update_time)(*m_time_axis);
        return;
    }
    bool accepted = false;
    {
        std::unique_lock lock(m_data_cfg_mutex);
        accepted = apply_time_axis_update_to_data_config(
            m_data_cfg, std::forward<Update>(update_time));
    }
    if (accepted) {
        emit t_limits_changed();
        update();
    }
}

void Plot_widget::set_t_range(qint64 t_min_ns, qint64 t_max_ns)
{
    apply_time_update([&](auto& axis) { return axis.set_t_range(t_min_ns, t_max_ns); });
}

void Plot_widget::set_available_t_range(qint64 t_min_ns, qint64 t_max_ns)
{
    apply_time_update([&](auto& axis) { return axis.set_available_t_range(t_min_ns, t_max_ns); });
}

void Plot_widget::set_view(const Plot_view& view)
{
    bool t_changed = false;
    bool v_changed = false;

    const auto t_range_valid = [](const std::pair<qint64, qint64>& r) {
        return r.second > r.first;
    };
    const auto v_range_valid = [](const std::pair<float, float>& r) {
        return std::isfinite(r.first) && std::isfinite(r.second) && r.second > r.first;
    };

    const bool t_range_ok = view.t_range && t_range_valid(*view.t_range);
    const bool t_avail_ok = view.t_available_range && t_range_valid(*view.t_available_range);

    if (m_time_axis) {
        if (t_range_ok) {
            m_time_axis->set_t_range(view.t_range->first, view.t_range->second);
            t_changed = true;
        }
        if (t_avail_ok) {
            m_time_axis->set_available_t_range(view.t_available_range->first, view.t_available_range->second);
            t_changed = true;
        }
    }
    else
    if (t_range_ok || t_avail_ok) {
        std::unique_lock lock(m_data_cfg_mutex);
        auto model = widget_time_axis_model(m_data_cfg);
        if (t_range_ok) {
            const auto result = model.set_t_range(
                view.t_range->first,
                view.t_range->second);
            t_changed = t_changed || result.accepted;
        }
        if (t_avail_ok) {
            const auto result = model.set_available_t_range(
                view.t_available_range->first,
                view.t_available_range->second);
            t_changed = t_changed || result.accepted;
        }
        if (t_changed) {
            apply_time_axis_model_to_data_config(model, m_data_cfg);
        }
    }

    if (view.v_range && v_range_valid(*view.v_range)) {
        std::unique_lock lock(m_data_cfg_mutex);
        m_data_cfg.v_min        = view.v_range->first;
        m_data_cfg.v_max        = view.v_range->second;
        m_data_cfg.v_manual_min = view.v_range->first;
        m_data_cfg.v_manual_max = view.v_range->second;
        v_changed               = true;
    }

    bool v_auto_changed_flag = false;
    if (view.v_auto) {
        if (m_v_auto.exchange(*view.v_auto, std::memory_order_acq_rel) != *view.v_auto) {
            v_auto_changed_flag = true;
        }
    }

    if (t_changed) {
        emit t_limits_changed();
    }
    if (v_changed) {
        emit v_limits_changed();
    }
    if (v_auto_changed_flag) {
        emit v_auto_changed();
    }
    if (t_changed || v_changed || v_auto_changed_flag) {
        update();
    }
}

Plot_time_axis* Plot_widget::time_axis() const
{
    return m_time_axis.data();
}

void Plot_widget::set_time_axis(Plot_time_axis* axis)
{
    if (m_time_axis == axis) {
        return;
    }

    if (m_time_axis) {
        if (m_time_axis->sync_vbar_width()) {
            m_time_axis->clear_shared_vbar_width(this);
        }
        QObject::disconnect(m_time_axis, nullptr, this, nullptr);
        m_time_axis_connection           = {};
        m_time_axis_destroyed_connection = {};
        m_time_axis_vbar_connection      = {};
        m_time_axis_sync_vbar_connection = {};
    }

    m_time_axis = axis;

    if (m_time_axis) {
        m_time_axis_connection           = QObject::connect(
            m_time_axis,
            &Plot_time_axis::t_limits_changed,
            this,
            [this]() { sync_time_axis_state(); });
        m_time_axis_destroyed_connection = QObject::connect(
            m_time_axis,
            &QObject::destroyed,
            this,
            [this]() { clear_time_axis(); });
        m_time_axis_vbar_connection      = QObject::connect(
            m_time_axis,
            &Plot_time_axis::shared_vbar_width_changed,
            this,
            [this](double px) {
                if (!m_time_axis || !m_time_axis->sync_vbar_width()) { return; }
                if (px <= 0.0 || !std::isfinite(px))                 { return; }
                apply_vbar_width_target(px);
            });
        m_time_axis_sync_vbar_connection = QObject::connect(
            m_time_axis,
            &Plot_time_axis::sync_vbar_width_changed,
            this,
            [this]() {
                if (!m_time_axis || !m_time_axis->sync_vbar_width()) {
                    return;
                }
                const double current_px =
                    m_vbar_width_px.load(std::memory_order_acquire);
                if (std::isfinite(current_px) && current_px > 0.0) {
                    m_time_axis->update_shared_vbar_width(this, current_px);
                }
                const double shared_px = m_time_axis->shared_vbar_width_px();
                if (std::isfinite(shared_px) && shared_px > 0.0) {
                    apply_vbar_width_target(shared_px);
                }
            });
        // Seed only ranges with no initialized bounds from the widget's
        // existing configuration before pulling. A half-seeded axis carries a
        // caller-provided bound and must not be overwritten by widget defaults.
        {
            const auto cfg = data_cfg_snapshot();
            if (!m_time_axis->any_view_bound_initialized() && cfg.t_max > cfg.t_min) {
                m_time_axis->set_t_range(cfg.t_min, cfg.t_max);
            }
            if (!m_time_axis->any_available_bound_initialized() &&
                cfg.t_available_max > cfg.t_available_min)
            {
                m_time_axis->set_available_t_range(
                    cfg.t_available_min, cfg.t_available_max);
            }
        }
        sync_time_axis_state();
        if (m_time_axis->sync_vbar_width()) {
            const double current_px = m_vbar_width_px.load(std::memory_order_acquire);
            if (std::isfinite(current_px) && current_px > 0.0) {
                m_time_axis->update_shared_vbar_width(this, current_px);
            }
            const double shared_px = m_time_axis->shared_vbar_width_px();
            if (std::isfinite(shared_px) && shared_px > 0.0) {
                apply_vbar_width_target(shared_px);
            }
        }
    }

    emit time_axis_changed();
    update();
}

void Plot_widget::attach_time_axis(Plot_widget* other)
{
    if (!other) {
        qWarning() << "vnm_plot: attach_time_axis called with null widget.";
        return;
    }
    if (!other->time_axis()) {
        qWarning() << "vnm_plot: attach_time_axis called but other widget has no time axis.";
        return;
    }
    set_time_axis(other->time_axis());
}

float Plot_widget::v_min() const
{
    if (m_v_auto.load(std::memory_order_acquire)) {
        float rendered_min = 0.0f;
        float rendered_max = 0.0f;
        if (rendered_v_range(rendered_min, rendered_max)) {
            return rendered_min;
        }
        std::shared_lock lock(m_data_cfg_mutex);
        return m_data_cfg.v_min;
    }
    std::shared_lock lock(m_data_cfg_mutex);
    return m_data_cfg.v_manual_min;
}

float Plot_widget::v_max() const
{
    if (m_v_auto.load(std::memory_order_acquire)) {
        float rendered_min = 0.0f;
        float rendered_max = 0.0f;
        if (rendered_v_range(rendered_min, rendered_max)) {
            return rendered_max;
        }
        std::shared_lock lock(m_data_cfg_mutex);
        return m_data_cfg.v_max;
    }
    std::shared_lock lock(m_data_cfg_mutex);
    return m_data_cfg.v_manual_max;
}

bool Plot_widget::v_auto() const
{
    return m_v_auto.load(std::memory_order_acquire);
}

void Plot_widget::set_v_auto(bool auto_scale)
{
    if (m_v_auto.exchange(auto_scale, std::memory_order_acq_rel) != auto_scale) {
        emit v_auto_changed();
        update();
    }
}

void Plot_widget::set_v_range(float v_min, float v_max)
{
    if (!std::isfinite(v_min) || !std::isfinite(v_max) || !(v_max > v_min)) {
        return;
    }
    {
        std::unique_lock lock(m_data_cfg_mutex);
        m_data_cfg.v_min        = v_min;
        m_data_cfg.v_max        = v_max;
        m_data_cfg.v_manual_min = v_min;
        m_data_cfg.v_manual_max = v_max;
    }
    emit v_limits_changed();
    update();
}

double Plot_widget::preview_height() const
{
    return m_preview_height;
}

void Plot_widget::set_preview_height(double height)
{
    m_preview_height_animation.stop();
    apply_preview_height(height);
}

void Plot_widget::apply_preview_height(double height)
{
    if (std::abs(m_preview_height - height) > 0.001) {
        m_preview_height = height;
        emit preview_height_changed();
        update();
    }
}

double Plot_widget::preview_height_target() const
{
    return m_preview_height_target;
}

double Plot_widget::preview_height_collapsed() const
{
    return m_preview_height_min;
}

double Plot_widget::reserved_height() const
{
    return (m_scaling_factor > 0.0)
        ? (m_base_label_height / m_scaling_factor + m_preview_height)
        : m_preview_height;
}

double Plot_widget::scaling_factor() const
{
    return m_scaling_factor;
}

double Plot_widget::vbar_width_pixels() const
{
    return m_vbar_width_px.load(std::memory_order_acquire);
}

double Plot_widget::vbar_width_qml() const
{
    return (m_scaling_factor > 0.0)
        ? m_vbar_width_px.load(std::memory_order_acquire) / m_scaling_factor
        : 0.0;
}

void Plot_widget::set_vbar_width(double vbar_width)
{
    const double px = vbar_width * m_scaling_factor;
    m_vbar_width_px.store(px, std::memory_order_release);
    emit vbar_width_changed();
    update();

    if (m_time_axis && m_time_axis->sync_vbar_width()) {
        m_time_axis->update_shared_vbar_width(this, px);
    }
}

void Plot_widget::apply_vbar_width_target(double target, bool publish_shared)
{
    if (!std::isfinite(target) || target <= 0.0) {
        return;
    }

    const double width_px = width() * m_scaling_factor;
    if (std::isfinite(width_px) && width_px > 0.0) {
        const double max_target = std::max(1.0, width_px * 0.5);
        target = std::clamp(target, 1.0, max_target);
    }

    const double current = m_vbar_width_px.load(std::memory_order_acquire);
    const auto publish_shared_width = [&]() {
        if (publish_shared && m_time_axis && m_time_axis->sync_vbar_width()) {
            m_time_axis->update_shared_vbar_width(this, target);
        }
    };

    if (!std::isfinite(current) || current <= 0.0) {
        m_vbar_width_px.store(target, std::memory_order_release);
        emit vbar_width_changed();
        update();
        publish_shared_width();
        return;
    }

    if (std::abs(target - current) <= k_vbar_width_change_threshold_d &&
        !m_vbar_width_timer.isActive())
    {
        publish_shared_width();
        return;
    }

    if (m_vbar_width_timer.isActive() &&
        std::abs(target - m_vbar_width_anim_target_px) <= 1e-6)
    {
        publish_shared_width();
        return;
    }

    publish_shared_width();

    m_vbar_width_anim_start_px = current;
    m_vbar_width_anim_target_px = target;
    m_vbar_width_anim_elapsed.restart();

    if (!m_vbar_width_timer.isActive()) {
        m_vbar_width_timer.start(16, this);
    }
}

void Plot_widget::arm_render_feedback_delivery(
    const std::shared_ptr<detail::plot_render_feedback_channel_t>& channel)
{
    QObject::disconnect(m_render_feedback_completion_connection);
    m_render_feedback_completion_connection = {};
    QObject::disconnect(m_render_feedback_delivery_connection);
    m_render_feedback_delivery_connection = {};

    if (m_render_feedback_channel != channel) {
        m_render_feedback_channel    = channel;
        m_render_feedback_generation = 0;
    }

    QQuickWindow* const quick_window = window();
    if (!m_render_feedback_channel || !quick_window) {
        return;
    }

    // Connection order is intentional: Qt invokes the direct completion
    // marker before it enqueues the item-context delivery for the same signal.
    const auto completion_type = static_cast<Qt::ConnectionType>(
        static_cast<int>(Qt::DirectConnection) |
        static_cast<int>(Qt::SingleShotConnection));
    m_render_feedback_completion_connection = QObject::connect(
        quick_window,
        &QQuickWindow::afterRendering,
        this,
        [channel] {
            channel->mark_latest_completed();
        },
        completion_type);

    const auto delivery_type = static_cast<Qt::ConnectionType>(
        static_cast<int>(Qt::QueuedConnection) |
        static_cast<int>(Qt::SingleShotConnection));
    m_render_feedback_delivery_connection = QObject::connect(
        quick_window,
        &QQuickWindow::afterRendering,
        this,
        [this, channel] {
            deliver_render_feedback(channel);
        },
        delivery_type);
}

void Plot_widget::deliver_render_feedback(
    const std::shared_ptr<detail::plot_render_feedback_channel_t>& channel)
{
    auto feedback = detail::take_completed_feedback(
        m_render_feedback_channel,
        channel,
        m_render_feedback_generation);
    if (!feedback) {
        return;
    }

    m_render_feedback_generation = feedback->generation;
    apply_render_feedback(std::move(*feedback));
}

void Plot_widget::apply_render_feedback(detail::plot_render_feedback_t feedback)
{
    apply_vbar_width_target(feedback.measured_vbar_width, true);
    set_rendered_v_range(feedback.v_min, feedback.v_max);
    set_rendered_t_range(feedback.t_min, feedback.t_max);

    if (!feedback.stack_validity_ready) {
        return;
    }

    m_rendered_stack_t_min           = feedback.t_min;
    m_rendered_stack_t_max           = feedback.t_max;
    m_rendered_stack_available_t_min = feedback.t_available_min;
    m_rendered_stack_available_t_max = feedback.t_available_max;
    m_rendered_stack_series_revision = feedback.series_revision;
    m_rendered_stack_validity = std::move(feedback.stack_validity);
    m_rendered_stack_statuses = std::move(feedback.stack_statuses);
}

double Plot_widget::update_dpi_scaling_factor()
{
    const double scaling = dpi_scaling_for_window(window());
    if (std::abs(m_scaling_factor - scaling) > 1e-6) {
        m_scaling_factor = scaling;
        emit scaling_factor_changed();
    }

    Plot_config cfg;
    {
        std::shared_lock lock(m_config_mutex);
        cfg = m_config;
    }

    m_adjusted_font_size = cfg.font_size_px * m_scaling_factor;
    m_base_label_height = cfg.base_label_height_px * m_scaling_factor;
    if (cfg.preview_height_px > 0.0) {
        set_preview_height(cfg.preview_height_px);
    }
    else {
        recalculate_preview_height();
    }
    update();
    return scaling;
}

void Plot_widget::invalidate_display_context()
{
    refresh_lcd_order();
    update_dpi_scaling_factor();
}

void Plot_widget::refresh_lcd_order()
{
    const auto request = config().lcd_request;
    m_auto_lcd_order = request.automatic
        ? resolve_lcd_subpixel_order_for_window(request, window())
        : lcd_subpixel_order_t::NONE;
}

void Plot_widget::observe_screen()
{
    for (const auto& connection : m_screen_connections) {
        QObject::disconnect(connection);
    }
    m_screen_connections.clear();
    auto* screen = window() ? window()->screen() : QGuiApplication::primaryScreen();
    if (screen) {
        const auto changed = [this] { invalidate_display_context(); };
        m_screen_connections.push_back(connect(screen, &QScreen::geometryChanged, this, changed));
        m_screen_connections.push_back(connect(screen, &QScreen::orientationChanged, this, changed));
        m_screen_connections.push_back(connect(screen, &QScreen::physicalDotsPerInchChanged, this, changed));
        m_screen_connections.push_back(connect(screen, &QScreen::logicalDotsPerInchChanged, this, changed));
    }
}

void Plot_widget::handle_window_changed(QQuickWindow* window)
{
    QObject::disconnect(m_window_screen_connection);
    m_window_screen_connection = {};

    if (window) {
        m_window_screen_connection = QObject::connect(
            window,
            &QWindow::screenChanged,
            this,
            [this](QScreen*) {
                observe_screen();
                invalidate_display_context();
            });
    }

    observe_screen();
    invalidate_display_context();
}

void Plot_widget::set_visible_info(int flags)
{
    const int visible_flags = flags & k_visible_info_all;
    const int prev          = m_visible_info_flags.exchange(visible_flags, std::memory_order_acq_rel);
    if (prev != visible_flags) {
        update();
    }
}

void Plot_widget::set_relative_preview_height(float relative)
{
    set_if_changed(
        m_relative_preview_height,
        std::clamp(relative, 0.0f, 1.0f),
        [this] { recalculate_preview_height(); });
}

void Plot_widget::set_preview_height_min(double v)
{
    set_if_changed(m_preview_height_min, std::max(0.0, v), [this] {
        if (m_preview_height_max < m_preview_height_min) {
            m_preview_height_max = m_preview_height_min;
        }
        recalculate_preview_height();
    });
}

void Plot_widget::set_preview_height_max(double v)
{
    set_if_changed(m_preview_height_max, std::max(0.0, v), [this] {
        if (m_preview_height_max < m_preview_height_min) {
            m_preview_height_min = m_preview_height_max;
        }
        recalculate_preview_height();
    });
}

void Plot_widget::set_show_if_calculated_preview_height_below_min(bool v)
{
    set_if_changed(
        m_show_if_calculated_preview_height_below_min, v,
        [this] { recalculate_preview_height(); });
}

void Plot_widget::set_preview_height_steps(int steps)
{
    set_if_changed(
        m_preview_height_steps, std::max(0, steps),
        [this] { recalculate_preview_height(); });
}

void Plot_widget::adjust_t_from_mouse_diff(double ref_width, double diff)
{
    apply_time_update([&](auto& axis) { return axis.adjust_t_from_mouse_diff(ref_width, diff); });
}

void Plot_widget::adjust_t_from_mouse_diff_on_preview(double ref_width, double diff)
{
    apply_time_update([&](auto& axis) { return axis.adjust_t_from_mouse_diff_on_preview(ref_width, diff); });
}

void Plot_widget::adjust_t_from_mouse_pos_on_preview(double ref_width, double x_pos)
{
    apply_time_update([&](auto& axis) { return axis.adjust_t_from_mouse_pos_on_preview(ref_width, x_pos); });
}

void Plot_widget::adjust_t_from_pivot_and_scale(double pivot, double scale)
{
    apply_time_update([&](auto& axis) { return axis.adjust_t_from_pivot_and_scale(pivot, scale); });
}

void Plot_widget::adjust_v_from_mouse_diff(float ref_height, float diff)
{
    if (ref_height <= 0.0f) {
        return;
    }

    const auto [vmin, vmax] = current_v_range();
    const float span  = vmax - vmin;
    const float delta = diff * span / ref_height;
    adjust_v_to_target(vmin + delta, vmax + delta);
}

void Plot_widget::adjust_v_from_pivot_and_scale(float pivot, float scale)
{
    if (scale <= 0.0f) {
        return;
    }

    const auto [vmin, vmax] = current_v_range();
    const float v_pivot = vmin + (vmax - vmin) * (1.0f - pivot);
    const float v0      = v_pivot - (v_pivot - vmin) * scale;
    const float v1      = v_pivot + (vmax - v_pivot) * scale;
    adjust_v_to_target(v0, v1);
}

void Plot_widget::adjust_v_to_target(float target_vmin, float target_vmax)
{
    const float min_span = min_v_span_for(target_vmin, target_vmax);
    if (target_vmax - target_vmin < min_span) {
        const float mid = 0.5f * (target_vmax + target_vmin);
        target_vmin     = mid - 0.5f * min_span;
        target_vmax     = mid + 0.5f * min_span;
    }

    {
        std::unique_lock lock(m_data_cfg_mutex);
        m_data_cfg.v_manual_min = target_vmin;
        m_data_cfg.v_manual_max = target_vmax;
        m_data_cfg.v_min        = target_vmin;
        m_data_cfg.v_max        = target_vmax;
    }

    set_v_auto(false);
    emit v_limits_changed();
    update();
}

void Plot_widget::auto_adjust_view(bool adjust_t, double extra_v_scale)
{
    auto_adjust_view(adjust_t, extra_v_scale, true);
}

void Plot_widget::auto_adjust_view(bool adjust_t, double extra_v_scale, bool anchor_zero)
{
    const auto   cfg            = data_cfg_snapshot();
    const qint64 window_tmin_ns = cfg.t_min;
    const qint64 window_tmax_ns = cfg.t_max;

    if (!(window_tmax_ns > window_tmin_ns)) {
        return;
    }

    struct aggregated_range_t
    {
        double         vmin;
        double         vmax;
        std::int64_t   tmin_ns;
        std::int64_t   tmax_ns;
    };

    bool have_any = false;
    aggregated_range_t agg{};

    const auto include_aggregate = [&](const detail::visible_sample_aggregate_t& series_agg) {
        if (!series_agg) {
            return;
        }
        if (!have_any) {
            agg = {
                series_agg.vmin,
                series_agg.vmax,
                series_agg.tmin_ns,
                series_agg.tmax_ns
            };
            have_any = true;
            return;
        }
        agg.vmin    = std::min(agg.vmin, series_agg.vmin);
        agg.vmax    = std::max(agg.vmax, series_agg.vmax);
        agg.tmin_ns = std::min(agg.tmin_ns, series_agg.tmin_ns);
        agg.tmax_ns = std::max(agg.tmax_ns, series_agg.tmax_ns);
    };

    std::vector<std::pair<int, std::shared_ptr<const series_data_t>>> sources;
    std::map<int, std::vector<int>> stack_members;
    {
        std::shared_lock lock(m_series_mutex);
        sources.reserve(m_series.size());
        for (const auto& [id, series] : m_series) {
            if (!series || !series->enabled || !series->main_source()) {
                continue;
            }
            const auto& qrhi_layers = qrhi_layers_for(*series);
            const bool has_main_layer = std::any_of(
                qrhi_layers.begin(),
                qrhi_layers.end(),
                [](const auto& layer) {
                    return layer && layer->draws_view(Series_view_kind::MAIN);
                });
            if (!series->style && !has_main_layer) {
                continue;
            }
            sources.emplace_back(id, series);
            if (series->stack_group != 0) {
                stack_members[series->stack_group].push_back(id);
            }
        }
    }

    {
        const bool current =
            m_series_revision.load(std::memory_order_acquire) ==
                m_rendered_stack_series_revision &&
            window_tmin_ns == m_rendered_stack_t_min &&
            window_tmax_ns == m_rendered_stack_t_max;

        if (current) {
            for (const auto& [group, members] : stack_members) {
                if (members.size() < 2) {
                    continue;
                }
                const auto found = m_rendered_stack_validity.find(group);
                if (found                == m_rendered_stack_validity.end() ||
                    found->second.size() != members.size())
                {
                    continue;
                }

                bool visible = true;
                detail::visible_sample_aggregate_t stacked_agg;
                for (std::size_t i = 0; i < members.size() && visible; ++i) {
                    const auto& revision = found->second[i];
                    const auto series_it = std::find_if(
                        sources.begin(), sources.end(),
                        [&](const auto& source) { return source.first == members[i]; });
                    if (revision.series_id != members[i] ||
                        series_it == sources.end() || !revision.source ||
                        revision.sequence == 0 || !revision.cumulative.is_valid() ||
                        revision.source->current_sequence(revision.lod) != revision.sequence)
                    {
                        visible = false;
                        break;
                    }

                    const bool include_base =
                        !!(series_it->second->style & Display_style::AREA);
                    const auto layer_agg = detail::aggregate_visible_sample_range(
                        revision.cumulative,
                        [](const void* sample) {
                            return static_cast<const detail::stacked_sample_t*>(sample)
                                ->timestamp_ns;
                        },
                        [include_base](const void* sample)
                            -> std::optional<std::pair<double, double>> {
                            const auto& value =
                                *static_cast<const detail::stacked_sample_t*>(sample);
                            if (!std::isfinite(value.value) ||
                                (include_base && !std::isfinite(value.base)))
                            {
                                return std::nullopt;
                            }
                            if (include_base) {
                                return
                                    std::make_pair(
                                        static_cast<double>(std::min(value.base, value.value)),
                                        static_cast<double>(std::max(value.base, value.value)));
                            }
                            return std::make_pair(
                                static_cast<double>(value.value),
                                static_cast<double>(value.value));
                        },
                        window_tmin_ns,
                        window_tmax_ns,
                        revision.interpolation,
                        Empty_window_behavior::DRAW_NOTHING);
                    if (!layer_agg) {
                        visible = false;
                        break;
                    }
                    if (!stacked_agg) {
                        stacked_agg = layer_agg;
                    }
                    else {
                        stacked_agg.vmin    = std::min(stacked_agg.vmin, layer_agg.vmin);
                        stacked_agg.vmax    = std::max(stacked_agg.vmax, layer_agg.vmax);
                        stacked_agg.tmin_ns = std::min(stacked_agg.tmin_ns, layer_agg.tmin_ns);
                        stacked_agg.tmax_ns = std::max(stacked_agg.tmax_ns, layer_agg.tmax_ns);
                    }
                }
                if (visible) {
                    include_aggregate(stacked_agg);
                }
            }
        }
    }

    for (const auto& [_, series] : sources) {
        if (!series || !series->main_source() || !series->access.get_timestamp) {
            continue;
        }
        const auto stack_it = stack_members.find(series->stack_group);
        if (series->stack_group != 0 &&
            stack_it != stack_members.end() && stack_it->second.size() > 1)
        {
            continue;
        }

        auto snapshot = series->main_source()->snapshot(0);
        if (!snapshot.is_valid()) {
            continue;
        }

        const auto read_range =
            [series](const void* sample) -> std::optional<std::pair<double, double>> {
                float low  = 0.0f;
                float high = 0.0f;
                if (series->access.get_range) {
                    std::tie(low, high) = series->get_range(sample);
                }
                else
                if (series->access.get_value) {
                    low = series->get_value(sample);
                    high = low;
                }
                else {
                    return std::nullopt;
                }

                return std::make_pair(
                    static_cast<double>(low),
                    static_cast<double>(high));
            };

        include_aggregate(detail::aggregate_visible_sample_range(
            snapshot,
            [series](const void* sample) {
                return series->get_timestamp(sample);
            },
            read_range,
            window_tmin_ns,
            window_tmax_ns,
            series->interpolation,
            series->empty_window_behavior));
    }

    if (!have_any) {
        return;
    }

    if (anchor_zero) {
        agg.vmin = std::min(agg.vmin, 0.0);
        agg.vmax = std::max(agg.vmax, 0.0);
    }

    const double scale     = std::max(0.0, 1.0 + extra_v_scale);
    const double base_span = std::max(0.0, agg.vmax - agg.vmin);
    double       span      = base_span * scale;
    const double min_span = static_cast<double>(
        min_v_span_for(static_cast<float>(agg.vmin), static_cast<float>(agg.vmax)));
    if (!(span > min_span)) {
        span = std::max(span, min_span);
    }

    double new_vmin = 0.0;
    double new_vmax = 0.0;
    if (anchor_zero) {
        // Anchor to zero: keep vmin at the computed minimum, extend upward
        new_vmin = agg.vmin;
        new_vmax = new_vmin + span;
    }
    else {
        // Center the range around the data
        const double v_center = 0.5 * (agg.vmin + agg.vmax);
        new_vmin = v_center - span * 0.5;
        new_vmax = v_center + span * 0.5;
    }

    if (adjust_t && !(agg.tmax_ns > agg.tmin_ns)) {
        adjust_t = false;
    }

    const bool has_time_axis = (m_time_axis != nullptr);
    {
        std::unique_lock lock(m_data_cfg_mutex);
        m_data_cfg.v_manual_min = static_cast<float>(new_vmin);
        m_data_cfg.v_manual_max = static_cast<float>(new_vmax);
        m_data_cfg.v_min        = static_cast<float>(new_vmin);
        m_data_cfg.v_max        = static_cast<float>(new_vmax);
    }

    if (adjust_t && has_time_axis) {
        m_time_axis->set_t_range(agg.tmin_ns, agg.tmax_ns);
    }
    else
    if (adjust_t) {
        std::unique_lock lock(m_data_cfg_mutex);
        apply_time_axis_update_to_data_config(
            m_data_cfg,
            [&](detail::Time_axis_model& model) {
                return model.set_t_range(agg.tmin_ns, agg.tmax_ns);
            });
    }

    set_v_auto(false);

    emit v_limits_changed();
    if (adjust_t && !has_time_axis) {
        emit t_limits_changed();
    }
    update();
}

bool Plot_widget::can_zoom_in() const
{
    const auto cfg = data_cfg_snapshot();
    // 0.1 second is the floor for zoom-in (== 100 ms). Below that the axis
    // is dominated by tick-label clutter and gives no actionable detail.
    constexpr std::int64_t k_min_zoom_span_ns = std::int64_t{100} * 1'000'000;
    const auto span_ns = positive_span_ns(cfg.t_min, cfg.t_max);
    return span_ns && *span_ns > static_cast<std::uint64_t>(k_min_zoom_span_ns);
}

QVariantList Plot_widget::get_indicator_samples(
    double x_ms,
    double plot_width,
    double plot_height,
    double mouse_px) const
{
    return get_samples_for_time(
        x_ms,
        plot_width,
        plot_height,
        mouse_px,
        Indicator_sample_mode::Interpolated);
}

QVariantList Plot_widget::get_nearest_samples(
    double x_ms,
    double plot_width,
    double plot_height,
    double mouse_px) const
{
    return get_samples_for_time(
        x_ms,
        plot_width,
        plot_height,
        mouse_px,
        Indicator_sample_mode::Nearest);
}

QVariantList Plot_widget::get_samples_for_time(
    double                 x_ms,
    double                 plot_width,
    double                 plot_height,
    double                 mouse_px,
    Indicator_sample_mode  mode) const
{
    QVariantList result;

    if (plot_width <= 0.0 || plot_height <= 0.0) {
        return result;
    }

    const auto cfg     = data_cfg_snapshot();
    qint64     tmin_ns = 0;
    qint64     tmax_ns = 0;
    if (!rendered_t_range(tmin_ns, tmax_ns)) {
        tmin_ns = cfg.t_min;
        tmax_ns = cfg.t_max;
    }
    // QML passes x_ms in milliseconds-since-epoch; keep the internal target in
    // integer nanoseconds so nearby epoch timestamps remain distinguishable.
    std::int64_t x    = 0;
    float        vmin = 0.0f;
    float        vmax = 0.0f;
    if (m_v_auto.load(std::memory_order_acquire)) {
        if (!rendered_v_range(vmin, vmax)) {
            vmin = cfg.v_min;
            vmax = cfg.v_max;
        }
    }
    else {
        vmin = cfg.v_manual_min;
        vmax = cfg.v_manual_max;
    }

    const auto  t_span = positive_span_ns(tmin_ns, tmax_ns);
    const float v_span = vmax - vmin;

    if (!t_span || v_span <= 0.0f) {
        return result;
    }

    // Recompute target time from rendered range when the caller supplies pixel x.
    // This keeps pixel->time and time->pixel conversions on the same range and
    // avoids horizontal indicator jitter during high-rate loading.
    if (mouse_px >= 0.0) {
        const auto target = time_at_fraction_ns(
            { tmin_ns, tmax_ns },
            static_cast<long double>(mouse_px) /
                static_cast<long double>(plot_width));
        if (!target) {
            return result;
        }
        x = *target;
    }
    else {
        const auto target = indicator_ms_to_ns(x_ms);
        if (!target) {
            return result;
        }
        x = *target;
    }

    const auto plot_cfg        = config();
    const auto value_formatter = plot_cfg.format_value;
    auto       series_map      = get_series_snapshot();

    struct indicator_stack_t
    {
        std::size_t    member_count                       = 0;
        std::size_t    sampled_count                      = 0;
        std::vector<std::pair<int, qsizetype>>
                       entries;
        QColor         color;
    };
    std::map<int, indicator_stack_t> indicator_stacks;

    for (const auto& [id, series] : series_map) {
        if (!series || !series->enabled) {
            continue;
        }
        if (!series->main_source() || !series->access.get_timestamp || !series->access.get_value) {
            continue;
        }

        indicator_stack_t* stack = nullptr;
        if (mode == Indicator_sample_mode::Interpolated && series->stack_group != 0 && !!series->style) {
            stack = &indicator_stacks[series->stack_group];
            ++stack->member_count;
        }

        auto snap = series->main_source()->snapshot(0);
        if (!snap.is_valid()) {
            continue;
        }

        const auto sample_at = [&](std::size_t index) -> const void* {
            return snap.at(index);
        };

        const detail::timestamp_bracket_t bracket = detail::bracket_timestamp(
            snap,
            [series](const void* sample) {
                return series->get_timestamp(sample);
            },
            x);
        if (!bracket) {
            continue;
        }

        const void* sample0 = sample_at(bracket.i0);
        const void* sample1 = sample_at(bracket.i1);
        if (!sample0 || !sample1) {
            continue;
        }

        const std::int64_t x0 = series->get_timestamp(sample0);
        const std::int64_t x1 = series->get_timestamp(sample1);
        const double       y0 = static_cast<double>(series->get_value(sample0));
        const double       y1 = static_cast<double>(series->get_value(sample1));

        double       y          = y0;
        std::int64_t resolved_x = x;

        if (mode == Indicator_sample_mode::Nearest) {
            const auto distance_ns = [](std::int64_t lhs, std::int64_t rhs) {
                if (lhs <= rhs) {
                    return static_cast<std::uint64_t>(rhs) -
                        static_cast<std::uint64_t>(lhs);
                }
                return static_cast<std::uint64_t>(lhs) -
                    static_cast<std::uint64_t>(rhs);
            };
            const bool use_sample1 = distance_ns(x1, x) < distance_ns(x0, x);
            resolved_x = use_sample1 ? x1 : x0;
            y = use_sample1 ? y1 : y0;
        }
        else {
            if (series->interpolation == Series_interpolation::STEP_AFTER) {
                if (bracket.i0 != bracket.i1 && x >= x1) {
                    y = y1;
                }
            }
            else {
                if (bracket.i0 != bracket.i1 && x0 != x1) {
                    const double t = detail::normalized_time_position_ns(x0, x, x1);
                    y = y0 + t * (y1 - y0);
                }
            }
        }

        double px = detail::normalized_time_position_ns(
            tmin_ns, resolved_x, tmax_ns) * plot_width;
        double py = (1.0 - (y - vmin) / v_span) * plot_height;

        px = std::clamp(px, 0.0, plot_width);
        py = std::clamp(py, 0.0, plot_height);

        QColor color(
            static_cast<int>(series->color.r * 255.0f),
            static_cast<int>(series->color.g * 255.0f),
            static_cast<int>(series->color.b * 255.0f),
            static_cast<int>(series->color.a * 255.0f)
        );

        QVariantMap entry;
        // x_ms keeps the QML side on the milliseconds-since-epoch surface;
        // see the Plot_widget class-level comment for the convention.
        entry["x"] = static_cast<double>(resolved_x) /
            static_cast<double>(k_ns_per_ms);
        entry["y"] = y;
        if (value_formatter) {
            value_format_context_t context;
            context.role                   = Value_format_role::INDICATOR;
            context.suggested_fixed_digits = 3;
            context.series_label           = series->series_label;
            entry["y_text"]                = QString::fromStdString(value_formatter(y, context));
        }
        entry["px"]           = px;
        entry["py"]           = py;
        entry["color"]        = color;
        entry["series_label"] = QString::fromStdString(series->series_label);
        result.append(entry);

        if (stack) {
            ++stack->sampled_count;
            stack->entries.emplace_back(id, result.size() - 1);
            stack->color = color;
        }
    }

    for (const auto& [group, stack] : indicator_stacks) {
        if (stack.member_count < 2) {
            continue;
        }
        for (const auto& [series_id, result_index] : stack.entries) {
            (void) series_id;
            QVariantMap component    = result[result_index].toMap();
            component["show_marker"] = false;
            result[result_index]     = component;
        }
        if (stack.sampled_count != stack.member_count) {
            continue;
        }
        const auto stacked_values = rendered_stack_values(
            group, stack.member_count, tmin_ns, tmax_ns, x);
        if (!stacked_values || stacked_values->size() != stack.entries.size()) {
            continue;
        }
        const bool matching_members = std::equal(
            stack.entries.begin(),
            stack.entries.end(),
            stacked_values->begin(),
            [](const auto& entry, const auto& value) { return entry.first == value.first; });
        if (!matching_members) {
            continue;
        }

        for (std::size_t i = 0; i < stack.entries.size(); ++i) {
            const qsizetype result_index = stack.entries[i].second;
            const double    marker_y     = (*stacked_values)[i].second;
            QVariantMap     component    = result[result_index].toMap();
            component["marker_y"]        = marker_y;
            component["stacked_marker"]  = true;
            component["show_marker"]     = true;
            component["py"]              = std::clamp(
                (1.0 - (marker_y - vmin) / v_span) * plot_height,
                0.0,
                plot_height);
            result[result_index]         = component;
        }

        const double cumulative = stacked_values->back().second;

        QVariantMap entry;
        entry["x"]            = static_cast<double>(x) /
            static_cast<double>(k_ns_per_ms);
        entry["y"]            = cumulative;
        entry["show_marker"]  = false;
        entry["px"]           = detail::normalized_time_position_ns(
            tmin_ns, x, tmax_ns) * plot_width;
        entry["py"]           = std::clamp(
            (1.0 - (cumulative - vmin) / v_span) * plot_height, 0.0, plot_height);
        entry["color"]        = stack.color;
        entry["series_label"] = QStringLiteral("\u03a3");
        if (value_formatter) {
            value_format_context_t context;
            context.role                   = Value_format_role::INDICATOR;
            context.suggested_fixed_digits = 3;
            context.series_label           = "\u03a3";
            entry["y_text"]                = QString::fromStdString(value_formatter(cumulative, context));
        }
        result.append(entry);
    }

    return result;
}

QString Plot_widget::format_timestamp_precise(qint64 timestamp_ms) const
{
    const auto cfg       = config();
    const auto formatter = cfg.format_timestamp ? cfg.format_timestamp : default_format_timestamp;
    // QML passes timestamp_ms (milliseconds-since-epoch); the formatter
    // expects nanoseconds. Convert at the boundary.
    const qint64 timestamp_ns = ms_for_qml_to_ns(timestamp_ms);
    // Step is zero ns: this caller asks for a single-instant rendering, not
    // a tick label, so the step argument is moot.
    return QString::fromStdString(formatter(timestamp_ns, std::int64_t{0}));
}

Stack_view_status Plot_widget::stack_status(
    int                group,
    Series_view_kind   view_kind) const
{
    Stack_view_status pending;
    pending.group     = group;
    pending.view_kind = view_kind;

    const data_config_t cfg = data_cfg_snapshot();
    const qint64 expected_min = view_kind == Series_view_kind::MAIN
        ? cfg.t_min
        : cfg.t_available_min;
    const qint64 expected_max = view_kind == Series_view_kind::MAIN
        ? cfg.t_max
        : cfg.t_available_max;

    const qint64 rendered_min = view_kind == Series_view_kind::MAIN
        ? m_rendered_stack_t_min
        : m_rendered_stack_available_t_min;
    const qint64 rendered_max = view_kind == Series_view_kind::MAIN
        ? m_rendered_stack_t_max
        : m_rendered_stack_available_t_max;
    if (m_series_revision.load(std::memory_order_acquire) != m_rendered_stack_series_revision ||
        expected_min                                      != rendered_min                     ||
        expected_max                                      != rendered_max)
    {
        return pending;
    }

    const auto found = m_rendered_stack_statuses.find({group, view_kind});
    if (found == m_rendered_stack_statuses.end()) {
        return pending;
    }
    for (const auto& source : found->second.sources) {
        if (!source.source) {
            continue;
        }
        const std::uint64_t current = source.source->current_sequence(source.lod);
        if (current != 0 && current != source.sequence) {
            return pending;
        }
    }
    return found->second.status;
}

QVariantMap Plot_widget::get_stack_status(int group, bool preview) const
{
    const Stack_view_status status = stack_status(
        group,
        preview ? Series_view_kind::PREVIEW : Series_view_kind::MAIN);
    QVariantList series_ids;
    series_ids.reserve(static_cast<qsizetype>(status.affected_series_ids.size()));
    for (int id : status.affected_series_ids) {
        series_ids.push_back(id);
    }
    return {
        { "group", status.group },
        { "view", status.view_kind == Series_view_kind::MAIN ? "MAIN" : "PREVIEW" },
        { "state",               stack_state_name(status.state)   },
        { "reason",              stack_reason_name(status.reason) },
        { "affected_series_ids", series_ids                       },
    };
}

std::pair<float, float> Plot_widget::manual_v_range() const
{
    const auto cfg = data_cfg_snapshot();
    return {cfg.v_manual_min, cfg.v_manual_max};
}

std::pair<float, float> Plot_widget::current_v_range() const
{
    const auto cfg = data_cfg_snapshot();
    if (m_v_auto.load(std::memory_order_acquire)) {
        float rendered_min = 0.0f;
        float rendered_max = 0.0f;
        if (rendered_v_range(rendered_min, rendered_max)) {
            return {rendered_min, rendered_max};
        }
        return {cfg.v_min, cfg.v_max};
    }
    return {cfg.v_manual_min, cfg.v_manual_max};
}

data_config_t Plot_widget::data_cfg_snapshot() const
{
    std::shared_lock lock(m_data_cfg_mutex);
    return m_data_cfg;
}

void Plot_widget::sync_time_axis_state()
{
    if (!m_time_axis) {
        return;
    }

    // Only pull bounds the axis has actually been told about. A freshly-
    // attached axis has no initialized ranges; pulling default values into
    // m_data_cfg would silently overwrite a view that was set via set_view
    // before the axis was attached.
    const bool view_init      = m_time_axis->view_initialized();
    const bool available_init = m_time_axis->available_initialized();
    if (!view_init && !available_init) {
        return;
    }

    {
        std::unique_lock lock(m_data_cfg_mutex);
        if (view_init) {
            m_data_cfg.t_min = m_time_axis->t_min();
            m_data_cfg.t_max = m_time_axis->t_max();
        }
        if (available_init) {
            m_data_cfg.t_available_min = m_time_axis->t_available_min();
            m_data_cfg.t_available_max = m_time_axis->t_available_max();
        }
    }

    emit t_limits_changed();
    update();
}

void Plot_widget::clear_time_axis()
{
    m_time_axis                      = nullptr;
    m_time_axis_connection           = {};
    m_time_axis_destroyed_connection = {};
    m_time_axis_vbar_connection      = {};
    m_time_axis_sync_vbar_connection = {};
    emit time_axis_changed();
    update();
}

bool Plot_widget::rendered_v_range(float& out_min, float& out_max) const
{
    if (!m_rendered_v_range_valid) {
        return false;
    }
    out_min = m_rendered_v_min;
    out_max = m_rendered_v_max;
    return true;
}

bool Plot_widget::rendered_t_range(qint64& out_min_ns, qint64& out_max_ns) const
{
    if (!m_rendered_t_range_valid) {
        return false;
    }
    out_min_ns = m_rendered_t_min;
    out_max_ns = m_rendered_t_max;
    return true;
}

std::uint64_t Plot_widget::series_revision() const
{
    return m_series_revision.load(std::memory_order_acquire);
}

std::optional<std::vector<std::pair<int, double>>> Plot_widget::rendered_stack_values(
    int            group,
    std::size_t    member_count,
    qint64         t_min_ns,
    qint64         t_max_ns,
    qint64         x_ns) const
{
    if (m_series_revision.load(std::memory_order_acquire) != m_rendered_stack_series_revision ||
        t_min_ns                                          != m_rendered_stack_t_min           ||
        t_max_ns                                          != m_rendered_stack_t_max)
    {
        return std::nullopt;
    }
    const auto found = m_rendered_stack_validity.find(group);
    if (found == m_rendered_stack_validity.end() || found->second.size() != member_count) {
        return std::nullopt;
    }

    std::vector<std::pair<int, double>> values;
    values.reserve(found->second.size());
    for (const auto& revision : found->second) {
        if (!revision.source || revision.sequence == 0 || !revision.cumulative.is_valid() ||
            revision.source->current_sequence(revision.lod) != revision.sequence)
        {
            return std::nullopt;
        }

        const auto sample_at = [&](std::size_t index) {
            return static_cast<const detail::stacked_sample_t*>(revision.cumulative.at(index));
        };
        const detail::stacked_sample_t* first = sample_at(0);
        const detail::stacked_sample_t* last  = sample_at(revision.cumulative.count - 1);
        if (!first || !last || x_ns < first->timestamp_ns || x_ns > last->timestamp_ns) {
            return std::nullopt;
        }
        const detail::timestamp_bracket_t bracket = detail::bracket_timestamp(
            revision.cumulative,
            [](const void* sample) {
                return static_cast<const detail::stacked_sample_t*>(sample)->timestamp_ns;
            },
            x_ns);
        if (!bracket) {
            return std::nullopt;
        }
        const detail::stacked_sample_t* sample0 = sample_at(bracket.i0);
        const detail::stacked_sample_t* sample1 = sample_at(bracket.i1);
        if (!sample0 || !sample1) {
            return std::nullopt;
        }

        double value = sample0->value;
        if (revision.interpolation == Series_interpolation::STEP_AFTER) {
            if (bracket.i0 != bracket.i1 && x_ns >= sample1->timestamp_ns) {
                value = sample1->value;
            }
        }
        else {
            if (bracket.i0            != bracket.i1 &&
                sample0->timestamp_ns != sample1->timestamp_ns)
            {
                const double position = detail::normalized_time_position_ns(
                    sample0->timestamp_ns,
                    x_ns,
                    sample1->timestamp_ns);
                value = sample0->value + position *
                    (sample1->value - sample0->value);
            }
        }
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
        values.emplace_back(revision.series_id, value);
    }
    return values;
}

void Plot_widget::set_rendered_v_range(float v_min, float v_max)
{
    if (!std::isfinite(v_min) || !std::isfinite(v_max) || v_min > v_max) {
        m_rendered_v_range_valid = false;
        return;
    }
    m_rendered_v_min = v_min;
    m_rendered_v_max = v_max;
    m_rendered_v_range_valid = true;
}

void Plot_widget::set_rendered_t_range(qint64 t_min_ns, qint64 t_max_ns)
{
    // Time span must be strictly positive to keep time<->pixel conversions safe.
    if (t_max_ns <= t_min_ns) {
        m_rendered_t_range_valid = false;
        return;
    }
    m_rendered_t_min = t_min_ns;
    m_rendered_t_max = t_max_ns;
    m_rendered_t_range_valid = true;
}

void Plot_widget::adjust_t_to_target(qint64 target_tmin_ns, qint64 target_tmax_ns)
{
    apply_time_update([&](auto& axis) { return axis.adjust_t_to_target(target_tmin_ns, target_tmax_ns); });
}

double Plot_widget::compute_preview_height_px(double widget_height_px) const
{
    if (m_relative_preview_height <= 0.0f) {
        return 0.0;
    }

    const double font_h    = m_base_label_height;
    const double available = std::max(0.0, widget_height_px - font_h);
    if (available <= 0.0) {
        return 0.0;
    }

    const double r = std::clamp<double>(m_relative_preview_height, 0.0, 1.0);
    if (r <= 0.0) {
        return 0.0;
    }

    double preview_px = (r * available) / (1.0 + r);

    const double min_px = m_preview_height_min > 0.0 ? m_preview_height_min * m_scaling_factor : 0.0;
    const double max_px = m_preview_height_max > 0.0
        ? m_preview_height_max * m_scaling_factor
        : std::numeric_limits<double>::infinity();

    if (preview_px > max_px) {
        preview_px = max_px;
    }

    if (preview_px < min_px) {
        if (!m_show_if_calculated_preview_height_below_min) {
            return 0.0;
        }
        preview_px = min_px;
    }

    if (m_preview_height_steps > 0 && max_px > min_px) {
        const int    steps = m_preview_height_steps;
        const double delta = (max_px - min_px) / static_cast<double>(steps);
        if (delta > 0.0) {
            double       clamped = std::clamp(preview_px, min_px, max_px);
            const double t       = (clamped - min_px) / delta;
            int          idx     = static_cast<int>(std::floor(t));
            if (idx < 0) {
                idx = 0;
            }
            else
            if (idx > steps) {
                idx = steps;
            }
            preview_px = min_px + delta * static_cast<double>(idx);
        }
    }

    preview_px = std::clamp(preview_px, 0.0, available);
    return preview_px;
}

void Plot_widget::recalculate_preview_height(bool resume_automatic)
{
    if (config().preview_height_px > 0.0) {
        return;
    }

    const double widget_h_dp  = height();
    const double widget_h_px  = widget_h_dp * m_scaling_factor;
    const double new_adjusted = compute_preview_height_px(widget_h_px);
    const double new_dp = (m_scaling_factor > 0.0)
        ? new_adjusted / m_scaling_factor
        : 0.0;

    const bool changed_target =
        (std::abs(new_dp - m_preview_height_target) > 0.5) ||
        (!m_preview_height_initialized && std::abs(new_dp - m_preview_height) > 0.5);

    // Fixed mode may have stopped at another height while retaining this
    // target. Re-enabling automatic mode must resume following that target.
    if (!changed_target && !resume_automatic) {
        return;
    }

    m_preview_height_target = new_dp;

    if (!m_preview_height_initialized) {
        m_preview_height_initialized = true;
        apply_preview_height(new_dp);
    }
    else {
        m_preview_height_animation.stop();
        m_preview_height_animation.setStartValue(m_preview_height);
        m_preview_height_animation.setEndValue(new_dp);
        m_preview_height_animation.start();
    }

    if (changed_target) {
        emit preview_height_target_changed(m_preview_height_target);
    }
}

QQuickRhiItemRenderer* Plot_widget::createRenderer()
{
    return new Plot_renderer();
}

void Plot_widget::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    QQuickRhiItem::geometryChange(newGeometry, oldGeometry);

    if (newGeometry.size() != oldGeometry.size()) {
        recalculate_preview_height();
        update();
    }
}

void Plot_widget::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickRhiItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged) {
        invalidate_display_context();
    }
}

void Plot_widget::timerEvent(QTimerEvent* ev)
{
    if (ev->timerId() == m_vbar_width_timer.timerId()) {
        constexpr double k_vbar_anim_duration_ms = 200.0;

        const double elapsed_ms = static_cast<double>(m_vbar_width_anim_elapsed.elapsed());
        double t = (k_vbar_anim_duration_ms > 0.0)
            ? (elapsed_ms / k_vbar_anim_duration_ms)
            : 1.0;
        if (t >= 1.0) {
            t = 1.0;
        }

        const double new_px =
            m_vbar_width_anim_start_px +
            (m_vbar_width_anim_target_px - m_vbar_width_anim_start_px) * t;

        m_vbar_width_px.store(new_px, std::memory_order_release);
        emit vbar_width_changed();
        update();

        if (t >= 1.0) {
            m_vbar_width_timer.stop();
        }
        return;
    }

    QQuickRhiItem::timerEvent(ev);
}

} // namespace vnm::plot
