#include "preview_controller.h"

#include <QtCore/QtGlobal>

#include <cmath>
#include <cstddef>
#include <utility>

namespace {

namespace plot_examples = vnm::plot::examples;

constexpr double k_x_min = -20.0;
constexpr double k_x_max = 20.0;
// Plot_view::t_range / t_available_range are int64 nanoseconds; sample x
// values from the example Function_data_source come in as fp seconds and the access
// policy auto-converts at the data boundary, but the view-range setters
// take qint64 directly and need explicit conversion here.
constexpr qint64 k_ns_per_second = 1'000'000'000;
constexpr qint64 k_t_min_ns = static_cast<qint64>(k_x_min * static_cast<double>(k_ns_per_second));
constexpr qint64 k_t_max_ns = static_cast<qint64>(k_x_max * static_cast<double>(k_ns_per_second));
constexpr std::size_t k_main_samples = 4000;
constexpr std::size_t k_preview_samples = 320;
constexpr std::size_t k_top_samples = 1800;
constexpr double k_auto_v_scale = 0.2;

float preview_signal(double x)
{
    return static_cast<float>(std::sin(x) + 0.35 * std::cos(0.35 * x));
}

float top_signal(double x)
{
    return 250.0f * static_cast<float>(2.2 * std::cos(0.4 * x) - 0.6 * std::sin(1.6 * x));
}

} // namespace

Example_plot_controller::Example_plot_controller(Preset preset, QObject* parent)
:
    QObject(parent),
    m_preset(preset),
    m_series_id(preset == Preset::PREVIEW ? 1 : 2),
    m_main_source(std::make_shared<plot_examples::Function_data_source>())
{
    if (m_preset == Preset::PREVIEW) {
        m_preview_source = std::make_shared<plot_examples::Function_data_source>();
    }
    setup_series();
    generate_samples();
}

void Example_plot_controller::set_plot_widget(vnm::plot::Plot_widget* widget)
{
    if (m_plot_widget == widget) return;

    if (m_plot_widget) {
        if (m_time_axis_connection) {
            QObject::disconnect(m_time_axis_connection);
            m_time_axis_connection = {};
        }
        QObject::disconnect(m_plot_widget, nullptr, this, nullptr);
        if (m_series) m_plot_widget->remove_series(m_series_id);
    }

    m_plot_widget = widget;

    if (m_plot_widget) {
        configure_plot_widget();
        if (m_series) m_plot_widget->add_series(m_series_id, m_series);
        vnm::plot::Plot_view view;
        view.t_range = std::make_pair(k_t_min_ns, k_t_max_ns);
        view.t_available_range = std::make_pair(k_t_min_ns, k_t_max_ns);
        view.v_auto = m_preset == Preset::PREVIEW;
        if (m_preset == Preset::TOP) {
            view.v_range = std::make_pair(-800.0f, 800.0f);
        }
        m_plot_widget->set_view(view);
        m_plot_widget->update();
        m_time_axis_connection = QObject::connect(
            m_plot_widget, &vnm::plot::Plot_widget::time_axis_changed, this,
            [this]() {
                if (!m_plot_widget) return;
                vnm::plot::Plot_view view;
                view.t_range = std::make_pair(k_t_min_ns, k_t_max_ns);
                view.t_available_range = std::make_pair(k_t_min_ns, k_t_max_ns);
                m_plot_widget->set_view(view);
            });
    }

    emit plot_widget_changed();
}

void Example_plot_controller::setup_series()
{
    const bool preview = m_preset == Preset::PREVIEW;
    m_series = vnm::plot::Series_builder()
        .enabled(true)
        .style(preview ? vnm::plot::Display_style::LINE : vnm::plot::Display_style::AREA)
        .color(preview ? vnm::plot::rgba_u8(64, 217, 140) : vnm::plot::rgba_u8(230, 89, 51, 230))
        .data_source(m_main_source)
        .access(plot_examples::make_function_sample_policy_typed())
        .build_shared();

    if (preview) {
        vnm::plot::preview_config_t preview_cfg;
        preview_cfg.data_source = m_preview_source;
        preview_cfg.access = m_series->access;
        preview_cfg.style = vnm::plot::Display_style::AREA;
        m_series->preview_config = preview_cfg;
    }
}

void Example_plot_controller::configure_plot_widget()
{
    if (!m_plot_widget) return;

    const bool preview = m_preset == Preset::PREVIEW;
    vnm::plot::Plot_config config;
    config.dark_mode = preview;
    config.clear_to_transparent = true;
    config.show_text = true;
    config.font_size_px = preview ? 11.0 : 10.0;
    config.preview_height_px = preview ? 120.0 : 0.0;
    config.preview_visibility = preview ? 0.6 : 0.0;
    config.line_width_px = preview ? 1.35 : 1.6;
    if (preview) {
        config.auto_v_range_extra_scale = k_auto_v_scale;
    }
    m_plot_widget->set_config(config);
    if (!preview) {
        m_plot_widget->set_relative_preview_height(0.0f);
        m_plot_widget->set_preview_height_min(0.0);
        m_plot_widget->set_preview_height_max(0.0);
        m_plot_widget->set_show_if_calculated_preview_height_below_min(false);
    }
}

void Example_plot_controller::generate_samples()
{
    const bool preview = m_preset == Preset::PREVIEW;
    m_main_source->generate(
        preview ? preview_signal : top_signal,
        k_x_min, k_x_max, preview ? k_main_samples : k_top_samples);
    if (preview) {
        m_preview_source->generate(preview_signal, k_x_min, k_x_max, k_preview_samples);
    }
}
