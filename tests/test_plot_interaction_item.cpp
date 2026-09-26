// vnm_plot interaction math tests

#include "test_macros.h"

#include "../src/qt/plot_render_feedback.h"

#include <vnm_plot/core/access_policy.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/time_units.h>
#include <vnm_plot/rhi/asset_loader.h>
#include <vnm_plot/rhi/series_renderer.h>
#include <vnm_plot/qt/plot_widget.h>
#include <vnm_plot/qt/plot_interaction_item.h>
#include <vnm_plot/qt/plot_time_axis.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QThread>
#include <QMouseEvent>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace plot = vnm::plot;

namespace {

class test_interaction_item_t : public plot::Plot_interaction_item
{
public:
    using plot::Plot_interaction_item::mousePressEvent;
    using plot::Plot_interaction_item::mouseDoubleClickEvent;
};

class indicator_test_widget_t : public plot::Plot_widget
{
public:
    // Stamps a render pass with the revision it renders, the way the renderer
    // does when it captures the widget's series.
    std::uint64_t rendered_series_revision() const
    {
        return series_revision();
    }

    // Hands a completed pass to the widget through the same entry point the
    // renderer's feedback channel delivers to.
    void publish_render_feedback(const plot::detail::plot_render_feedback_t& feedback)
    {
        apply_render_feedback(feedback);
    }
};

struct zoom_state_t
{
    double         scale    = 1.0;
    double         velocity = 0.0;
};

struct indicator_sample_t
{
    std::int64_t   t_ns  = 0;
    float          value = 0.0f;
};

class unsupported_sequence_source_t final : public plot::Data_source
{
public:
    std::vector<indicator_sample_t> samples;
    std::uint64_t                   snapshot_sequence = 41;

    plot::snapshot_result_t try_snapshot(std::size_t) override
    {
        if (samples.empty()) {
            return {{}, plot::snapshot_result_t::Snapshot_status::EMPTY};
        }
        return {{
            samples.data(), samples.size(), sizeof(indicator_sample_t), snapshot_sequence},
            plot::snapshot_result_t::Snapshot_status::READY};
    }

    std::uint64_t current_sequence(std::size_t) const override { return 0; }
    std::size_t sample_stride() const override { return sizeof(indicator_sample_t); }
};

class advancing_after_snapshot_source_t final : public plot::Data_source
{
public:
    std::vector<indicator_sample_t> samples{{0, 1.0f}, {100, 2.0f}};
    std::uint64_t                   sequence = 1;

    plot::snapshot_result_t try_snapshot(std::size_t) override
    {
        const std::uint64_t rendered_sequence = sequence++;
        return {{
            samples.data(), samples.size(), sizeof(indicator_sample_t), rendered_sequence},
            plot::snapshot_result_t::Snapshot_status::READY};
    }

    std::uint64_t current_sequence(std::size_t) const override { return sequence; }
    std::size_t sample_stride() const override { return sizeof(indicator_sample_t); }
};

plot::Data_access_policy indicator_access_policy()
{
    plot::Data_access_policy_typed<indicator_sample_t> access;
    access.get_timestamp = [](const indicator_sample_t& sample) {
        return sample.t_ns;
    };
    access.get_value = [](const indicator_sample_t& sample) {
        return sample.value;
    };
    return access.erase();
}

std::shared_ptr<plot::series_data_t> make_stack_status_series(
    std::shared_ptr<plot::Data_source> source,
    plot::Nonfinite_sample_policy      policy = plot::Nonfinite_sample_policy::BREAK_SEGMENT)
{
    auto series = std::make_shared<plot::series_data_t>();
    series->style            = plot::Display_style::LINE;
    series->stack_group      = 7;
    series->nonfinite_policy = policy;
    series->data_source      = std::move(source);
    series->access           = indicator_access_policy();
    return series;
}

bool nearly_equal(double a, double b, double epsilon = 1e-12)
{
    return std::abs(a - b) <= epsilon * std::max({1.0, std::abs(a), std::abs(b)});
}

zoom_state_t advance_zoom(double initial_velocity, const std::vector<double>& elapsed_steps)
{
    zoom_state_t state{1.0, initial_velocity};
    for (double elapsed_step : elapsed_steps) {
        state.scale    *= plot::Plot_interaction_item::zoom_animation_scale_factor(state.velocity, elapsed_step);
        state.velocity  = plot::Plot_interaction_item::zoom_animation_velocity_after(state.velocity, elapsed_step);
    }
    return state;
}

std::shared_ptr<plot::series_data_t> make_sample_series(
    std::vector<indicator_sample_t>    samples,
    plot::Series_interpolation         interpolation,
    plot::Empty_window_behavior        empty_window_behavior = plot::Empty_window_behavior::DRAW_NOTHING)
{
    auto source = std::make_shared<plot::Vector_data_source<indicator_sample_t>>(
        std::move(samples));

    const plot::Data_access_policy access = indicator_access_policy();

    auto series = std::make_shared<plot::series_data_t>();
    series->series_label          = "indicator";
    series->interpolation         = interpolation;
    series->empty_window_behavior = empty_window_behavior;
    series->data_source           = source;
    series->access                = access;

    return series;
}

void configure_view(
    plot::Plot_widget&         widget,
    std::int64_t               tmin_ns,
    std::int64_t               tmax_ns,
    float                      vmin,
    float                      vmax)
{
    plot::Plot_view view;
    view.t_range           = std::pair<qint64, qint64>{tmin_ns, tmax_ns};
    view.t_available_range = std::pair<qint64, qint64>{tmin_ns, tmax_ns};
    view.v_range           = std::pair<float, float>{vmin, vmax};
    view.v_auto            = false;

    widget.set_view(view);
}

void configure_time_window(
    plot::Plot_widget&         widget,
    std::int64_t               tmin_ns,
    std::int64_t               tmax_ns,
    std::int64_t               t_available_min_ns,
    std::int64_t               t_available_max_ns)
{
    widget.set_t_range(tmin_ns, tmax_ns);
    widget.set_available_t_range(t_available_min_ns, t_available_max_ns);
}

void configure_indicator_widget(
    plot::Plot_widget&         widget,
    plot::Series_interpolation interpolation)
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;

    configure_view(widget, 0, k_second_ns, 0.0f, 10.0f);
    widget.add_series(1, make_sample_series(
        {
            { 0, 0.0f },
            {k_second_ns, 10.0f}
        },
        interpolation));
}

void publish_rendered_stack_validity(
    indicator_test_widget_t&   widget,
    const std::map<int, std::shared_ptr<const plot::series_data_t>>&
                               series,
    std::int64_t               t_min_ns,
    std::int64_t               t_max_ns,
    bool                       include_preview = false)
{
    plot::frame_layout_result_t layout;
    layout.usable_width  = 100.0;
    layout.usable_height = 100.0;
    plot::Plot_config config;
    plot::frame_context_t context{layout};
    context.t0                      = t_min_ns;
    context.t1                      = t_max_ns;
    context.t_available_min         = t_min_ns;
    context.t_available_max         = t_max_ns;
    context.win_w                   = 100;
    context.win_h                   = 100;
    context.adjusted_preview_height = include_preview ? 20.0 : 0.0;
    context.config                  = &config;

    plot::Asset_loader assets;
    plot::Series_renderer renderer;
    renderer.initialize(assets);

    plot::detail::plot_render_feedback_t feedback;
    feedback.t_min           = t_min_ns;
    feedback.t_max           = t_max_ns;
    feedback.t_available_min = t_min_ns;
    feedback.t_available_max = t_max_ns;
    feedback.series_revision = widget.rendered_series_revision();
    renderer.render(context, series);
    plot::detail::fill_stack_feedback(renderer, feedback);
    widget.publish_render_feedback(feedback);
}

bool test_zoom_math_is_invariant_to_timer_cadence()
{
    const zoom_state_t single_gap = advance_zoom(4.0, {10.0});
    const zoom_state_t split_gap  = advance_zoom(4.0, {4.0, 6.0});
    const zoom_state_t fine_steps = advance_zoom(
        4.0,
        {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});

    TEST_ASSERT(nearly_equal(single_gap.scale, split_gap.scale), "scale should match for split elapsed time");
    TEST_ASSERT(nearly_equal(single_gap.velocity, split_gap.velocity), "velocity should match for split elapsed time");
    TEST_ASSERT(nearly_equal(single_gap.scale, fine_steps.scale), "scale should match for fine-grained elapsed time");
    TEST_ASSERT(
        nearly_equal(single_gap.velocity, fine_steps.velocity), "velocity should match for fine-grained elapsed time");

    return true;
}

bool test_zoom_math_stays_composable_across_small_velocity_decay()
{
    const zoom_state_t single_gap = advance_zoom(0.01, {10.0});
    const zoom_state_t fine_steps = advance_zoom(
        0.01,
        {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});

    TEST_ASSERT(nearly_equal(single_gap.scale, fine_steps.scale),       "small-velocity scale should stay composable");
    TEST_ASSERT(nearly_equal(single_gap.velocity, fine_steps.velocity), "small-velocity decay should stay composable");

    return true;
}

bool test_zoom_math_handles_negative_velocity()
{
    const zoom_state_t single_gap = advance_zoom(-4.0, {10.0});
    const zoom_state_t split_gap  = advance_zoom(-4.0, {4.0, 6.0});
    const zoom_state_t fractional_steps = advance_zoom(
        -4.0,
        {0.3, 0.7, 1.5, 2.5, 5.0});

    TEST_ASSERT(nearly_equal(single_gap.scale, split_gap.scale), "negative scale should match for split elapsed time");
    TEST_ASSERT(
        nearly_equal(single_gap.velocity, split_gap.velocity), "negative velocity should match for split elapsed time");
    TEST_ASSERT(
        nearly_equal(single_gap.scale, fractional_steps.scale), "negative scale should match for fractional steps");
    TEST_ASSERT(nearly_equal(single_gap.velocity, fractional_steps.velocity), "negative velocity should "
        "match for fractional steps");

    return true;
}

bool test_zoom_math_handles_zero_velocity()
{
    const zoom_state_t state = advance_zoom(0.0, {0.3, 0.7, 1.5, 2.5, 5.0});

    TEST_ASSERT(nearly_equal(state.scale, 1.0),    "zero velocity should keep identity scale");
    TEST_ASSERT(nearly_equal(state.velocity, 0.0), "zero velocity should stay zero");

    return true;
}

bool test_wheel_zoom_handles_near_zero_value_range()
{
    constexpr std::int64_t tmin_ns = 0;
    constexpr std::int64_t tmax_ns = 2'000'000'000;
    constexpr float        vmin    = -0.4e-6f;
    constexpr float        vmax    = 3.1e-6f;

    {
        plot::Plot_widget widget;
        configure_view(widget, tmin_ns, tmax_ns, vmin, vmax);

        test_interaction_item_t item;
        item.setWidth(1000.0);
        item.setHeight(500.0);
        item.set_plot_widget(&widget);

        TEST_ASSERT(
            item.handle_wheel(500.0, 250.0, 0.0, 120.0, 0.0, 0.0, Qt::NoModifier),
            "ordinary wheel zoom should be handled");
        TEST_ASSERT(widget.t_max() - widget.t_min() < tmax_ns - tmin_ns,
            "ordinary wheel zoom should shrink the time range");
        TEST_ASSERT(widget.v_min() == vmin && widget.v_max() == vmax,
            "ordinary wheel zoom should leave the value range unchanged");
    }

    {
        plot::Plot_widget widget;
        configure_view(widget, tmin_ns, tmax_ns, 50.0f, 190.0f);

        test_interaction_item_t item;
        item.setWidth(1000.0);
        item.setHeight(500.0);
        item.set_plot_widget(&widget);

        TEST_ASSERT(
            item.handle_wheel(
                500.0, 250.0, 0.0, 120.0, 0.0, 0.0, static_cast<int>(Qt::AltModifier)),
            "Alt+wheel zoom should be handled for the normal-sized control range");
        TEST_ASSERT(widget.v_max() - widget.v_min() < 140.0f,
            "Alt+wheel zoom should shrink the normal-sized control range");
    }

    {
        plot::Plot_widget widget;
        configure_view(widget, tmin_ns, tmax_ns, vmin, vmax);

        test_interaction_item_t item;
        item.setWidth(1000.0);
        item.setHeight(500.0);
        item.set_plot_widget(&widget);

        TEST_ASSERT(
            item.handle_wheel(
                500.0, 250.0, 0.0, 120.0, 0.0, 0.0, static_cast<int>(Qt::AltModifier)),
            "Alt+wheel zoom should be handled for the near-zero range");
        TEST_ASSERT(widget.v_max() - widget.v_min() < vmax - vmin,
            "Alt+wheel zoom should shrink, not expand, the near-zero range");
        TEST_ASSERT(widget.t_min() == tmin_ns && widget.t_max() == tmax_ns,
            "Alt+wheel zoom should leave the time range unchanged");

        bool reached_precision_limit = false;
        for (int step = 0; step < 512; ++step) {
            const float previous_span = widget.v_max() - widget.v_min();
            const float min_span = plot::detail::min_v_span_for(widget.v_min(), widget.v_max());
            if (previous_span <= 1.05f * min_span) {
                reached_precision_limit = true;
                break;
            }

            widget.adjust_v_from_pivot_and_scale(0.5f, 0.95f);
            TEST_ASSERT(widget.v_max() - widget.v_min() < previous_span,
                "repeated near-zero zoom steps should progress above the float-precision limit");
        }
        TEST_ASSERT(reached_precision_limit,
            "repeated near-zero zoom should reach the float-precision limit");
    }

    {
        plot::Plot_widget widget;
        configure_view(widget, tmin_ns, tmax_ns, vmin, vmax);

        test_interaction_item_t item;
        item.setWidth(1000.0);
        item.setHeight(500.0);
        item.set_plot_widget(&widget);

        TEST_ASSERT(
            item.handle_wheel(
                500.0, 250.0, 0.0, 120.0, 0.0, 0.0, static_cast<int>(Qt::ControlModifier)),
            "Ctrl+wheel zoom should be handled for the near-zero range");
        TEST_ASSERT(widget.v_max() - widget.v_min() < vmax - vmin,
            "Ctrl+wheel zoom should shrink the near-zero value range");
        TEST_ASSERT(widget.t_max() - widget.t_min() < tmax_ns - tmin_ns,
            "Ctrl+wheel zoom should shrink the time range");
    }

    return true;
}

bool test_indicator_samples_linearly_interpolate_between_samples()
{
    plot::Plot_widget widget;
    configure_indicator_widget(widget, plot::Series_interpolation::LINEAR);

    const QVariantList samples = widget.get_indicator_samples(500.0, 100.0, 100.0);
    TEST_ASSERT(samples.size() == 1, "linear indicator query should return one sample");

    const QVariantMap entry = samples.front().toMap();
    TEST_ASSERT(nearly_equal(entry.value("y").toDouble(), 5.0),
        "linear indicator query should interpolate y between neighboring samples");
    TEST_ASSERT(nearly_equal(entry.value("x").toDouble(), 500.0),
        "linear indicator query should report the requested timestamp in milliseconds");

    return true;
}

bool test_indicator_samples_step_after_holds_previous_sample()
{
    plot::Plot_widget widget;
    configure_indicator_widget(widget, plot::Series_interpolation::STEP_AFTER);

    const QVariantList samples = widget.get_indicator_samples(500.0, 100.0, 100.0);
    TEST_ASSERT(samples.size() == 1, "step-after indicator query should return one sample");

    const QVariantMap entry = samples.front().toMap();
    TEST_ASSERT(nearly_equal(entry.value("y").toDouble(), 0.0),
        "step-after indicator query should hold the previous sample value");
    TEST_ASSERT(nearly_equal(entry.value("x").toDouble(), 500.0),
        "step-after indicator query should keep the requested timestamp in milliseconds");

    return true;
}

bool test_indicator_reports_stack_sum_only_inside_common_domain()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    indicator_test_widget_t widget;
    configure_view(widget, 0, 2 * k_second_ns, 0.0f, 10.0f);
    plot::Plot_config indicator_config;
    indicator_config.format_value = [](double value, const plot::value_format_context_t&) {
        return std::to_string(value);
    };
    widget.set_config(indicator_config);

    auto lower = make_sample_series(
        {{0, 1.0f}, {k_second_ns, 3.0f}}, plot::Series_interpolation::LINEAR);
    auto upper = make_sample_series(
        {{k_second_ns / 2, 2.0f}, {2 * k_second_ns, 2.0f}}, plot::Series_interpolation::LINEAR);
    lower->series_label = "f(x1)";
    upper->series_label = "f(x2)";
    lower->stack_group  = upper->stack_group = 1;
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    publish_rendered_stack_validity(
        widget,
        { {1, lower}, {2, upper} },
        0,
        2 * k_second_ns);

    const QVariantList stacked = widget.get_indicator_samples(750.0, 100.0, 100.0);
    TEST_ASSERT(stacked.size() == 3, "stacked indicator should retain components and add one sum");
    TEST_ASSERT(stacked[0].toMap().value("series_label").toString() == QStringLiteral("f(x1)") &&
        stacked[1].toMap().value("series_label").toString() == QStringLiteral("f(x2)"),
        "stacked indicator should preserve component labels");
    const QVariantMap sum          = stacked[2].toMap();
    const QVariantMap lower_marker = stacked[0].toMap();
    const QVariantMap upper_marker = stacked[1].toMap();
    TEST_ASSERT(nearly_equal(lower_marker.value("y").toDouble(), 2.5) &&
        lower_marker.value("y_text").toString() == QStringLiteral("2.500000") &&
        nearly_equal(upper_marker.value("y").toDouble(), 2.0) &&
        upper_marker.value("y_text").toString() == QStringLiteral("2.000000"),
        "stacked indicator component data and text should remain raw values");
    TEST_ASSERT(nearly_equal(lower_marker.value("marker_y").toDouble(), 2.5) &&
        nearly_equal(lower_marker.value("py").toDouble(), 75.0) &&
        nearly_equal(upper_marker.value("marker_y").toDouble(), 4.5) &&
        nearly_equal(upper_marker.value("py").toDouble(), 55.0) &&
        lower_marker.value("stacked_marker").toBool() &&
        upper_marker.value("stacked_marker").toBool() &&
        lower_marker.value("show_marker").toBool() &&
        upper_marker.value("show_marker").toBool(),
        "stacked component marker metadata and geometry should use cumulative rendered values");
    TEST_ASSERT(sum.value("series_label").toString() == QStringLiteral("\u03a3"),
        "stacked indicator should label the aggregate with sigma");
    TEST_ASSERT(nearly_equal(sum.value("y").toDouble(), 4.5),
        "stacked indicator should report the interpolated cumulative value");
    TEST_ASSERT(!sum.value("show_marker").toBool(),
        "stack sum row should not duplicate the top cumulative marker");

    const QVariantList outside_common_domain = widget.get_indicator_samples(250.0, 100.0, 100.0);
    TEST_ASSERT(outside_common_domain.size() == 2,
        "stacked indicator should omit the sum outside the common source domain");
    TEST_ASSERT(!outside_common_domain[0].toMap().value("show_marker").toBool() &&
        !outside_common_domain[1].toMap().value("show_marker").toBool(),
        "stacked component markers should stay hidden outside the common source domain");

    upper->stack_group = 0;
    widget.add_series(2, upper);
    const QVariantList unstacked = widget.get_indicator_samples(750.0, 100.0, 100.0);
    TEST_ASSERT(unstacked.size() == 2,
        "unstacked indicator should contain only component values");
    TEST_ASSERT(!unstacked[0].toMap().contains("show_marker"),
        "a singleton nonzero stack group should retain its ordinary raw marker");

    return true;
}

bool test_stacked_indicator_matches_sub_256ns_epoch_composition()
{
    constexpr std::int64_t k_epoch = 1'750'000'000'000'000'000LL;
    indicator_test_widget_t widget;
    configure_view(widget, k_epoch, k_epoch + 254, 0.0f, 20.0f);

    auto lower = make_sample_series(
        { {k_epoch, 0.0f}, {k_epoch + 254, 10.0f} },
        plot::Series_interpolation::LINEAR);
    auto upper = make_sample_series(
        {
            { k_epoch,       1.0f},
            { k_epoch + 127, 1.0f},
            { k_epoch + 254, 1.0f}
        },
        plot::Series_interpolation::LINEAR);
    lower->stack_group = upper->stack_group = 1;
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    publish_rendered_stack_validity(
        widget, {{1, lower}, {2, upper}}, k_epoch, k_epoch + 254);

    const QVariantList samples = widget.get_indicator_samples(
        0.0, 100.0, 100.0, 50.0);
    TEST_ASSERT(samples.size() == 3,
        "sub-256ns epoch stack should publish components and sigma");
    TEST_ASSERT(nearly_equal(samples[0].toMap().value("y").toDouble(), 5.0) &&
        nearly_equal(samples[0].toMap().value("marker_y").toDouble(), 5.0),
        "ordinary and cumulative indicator interpolation should match at the epoch midpoint");
    TEST_ASSERT(nearly_equal(samples[1].toMap().value("marker_y").toDouble(), 6.0) &&
        nearly_equal(samples[2].toMap().value("y").toDouble(), 6.0),
        "stacked marker and sigma should match composed epoch geometry");

    return true;
}

bool test_stack_status_reports_views_freshness_rejection_and_recovery()
{
    auto lower_source = std::make_shared<plot::Vector_data_source<indicator_sample_t>>(
        std::vector<indicator_sample_t>{{0, 1.0f}, {100, 2.0f}});
    auto upper_source = std::make_shared<plot::Vector_data_source<indicator_sample_t>>(
        std::vector<indicator_sample_t>{{0, 3.0f}, {100, 4.0f}});
    auto lower = make_stack_status_series(lower_source);
    auto upper = make_stack_status_series(upper_source);

    indicator_test_widget_t widget;
    configure_view(widget, 0, 100, 0.0f, 10.0f);
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    std::map<int, std::shared_ptr<const plot::series_data_t>> series{
        {1, lower}, {2, upper}};

    TEST_ASSERT(widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::PENDING,
        "a stack should be pending before the renderer publishes its current result");

    publish_rendered_stack_validity(widget, series, 0, 100, true);
    const auto main_status    = widget.stack_status(7, plot::Series_view_kind::MAIN);
    const auto preview_status = widget.stack_status(7, plot::Series_view_kind::PREVIEW);
    TEST_ASSERT(main_status.state == plot::Stack_view_state::ACTIVE &&
        preview_status.state == plot::Stack_view_state::ACTIVE,
        "main and preview should publish independent active stack results");
    TEST_ASSERT(main_status.affected_series_ids == std::vector<int>({1, 2}),
        "active status should identify every affected series");

    const QVariantMap qml = widget.get_stack_status(7, true);
    TEST_ASSERT(qml.value("group").toInt() == 7 &&
        qml.value("view").toString() == "PREVIEW" &&
        qml.value("state").toString() == "ACTIVE" &&
        qml.value("reason").toString() == "NONE" &&
        qml.value("affected_series_ids").toList().size() == 2,
        "QML status should expose the documented stable schema");

    lower_source->set_data({{0, 5.0f}, {100, 6.0f}});
    TEST_ASSERT(widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::PENDING,
        "advanced source data should make a published status stale");
    publish_rendered_stack_validity(widget, series, 0, 100, true);
    TEST_ASSERT(widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::ACTIVE,
        "a fresh renderer publication should recover a stale status");

    upper->interpolation = plot::Series_interpolation::STEP_AFTER;
    widget.add_series(2, upper);
    TEST_ASSERT(widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::PENDING,
        "a series revision should make the previous renderer result pending");
    publish_rendered_stack_validity(widget, series, 0, 100, true);
    const auto rejected_main    = widget.stack_status(7, plot::Series_view_kind::MAIN);
    const auto rejected_preview = widget.stack_status(7, plot::Series_view_kind::PREVIEW);
    TEST_ASSERT(rejected_main.state == plot::Stack_view_state::SUPPRESSED &&
        rejected_preview.state == plot::Stack_view_state::SUPPRESSED &&
        rejected_main.reason == plot::Stack_rejection_reason::MIXED_INTERPOLATION,
        "main and preview should expose the renderer's typed rejection");

    upper->interpolation = plot::Series_interpolation::LINEAR;
    widget.add_series(2, upper);
    publish_rendered_stack_validity(widget, series, 0, 100, true);
    TEST_ASSERT(widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::ACTIVE,
        "a compatible group should recover from suppression");

    widget.remove_series(2);
    series.erase(2);
    publish_rendered_stack_validity(widget, series, 0, 100, true);
    const auto singleton = widget.stack_status(7, plot::Series_view_kind::MAIN);
    TEST_ASSERT(singleton.state == plot::Stack_view_state::ACTIVE &&
        singleton.reason == plot::Stack_rejection_reason::NONE,
        "a singleton group should remain a non-error");

    return true;
}

bool test_stack_status_handles_unsupported_and_empty_source_revisions()
{
    auto unsupported_lower = std::make_shared<unsupported_sequence_source_t>();
    auto unsupported_upper = std::make_shared<unsupported_sequence_source_t>();
    unsupported_lower->samples = {{0, 1.0f}, {100, 2.0f}};
    unsupported_upper->samples = {{0, 3.0f}, {100, 4.0f}};
    auto lower = make_stack_status_series(unsupported_lower);
    auto upper = make_stack_status_series(unsupported_upper);

    indicator_test_widget_t unsupported_widget;
    configure_view(unsupported_widget, 0, 100, 0.0f, 10.0f);
    unsupported_widget.add_series(1, lower);
    unsupported_widget.add_series(2, upper);
    std::map<int, std::shared_ptr<const plot::series_data_t>> unsupported_series{
        {1, lower}, {2, upper}};
    publish_rendered_stack_validity(
        unsupported_widget, unsupported_series, 0, 100);
    TEST_ASSERT(unsupported_widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::ACTIVE,
        "unsupported source revisions must not make a fresh status pending");
    unsupported_lower->samples = {{0, 5.0f}, {100, 6.0f}};
    TEST_ASSERT(unsupported_widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::ACTIVE,
        "unsupported revision sources remain current until the next renderer publication");

    auto empty_source = std::make_shared<plot::Vector_data_source<indicator_sample_t>>();
    auto valid_source = std::make_shared<plot::Vector_data_source<indicator_sample_t>>(
        std::vector<indicator_sample_t>{{0, 3.0f}, {100, 4.0f}});
    lower = make_stack_status_series(empty_source);
    upper = make_stack_status_series(valid_source);
    indicator_test_widget_t empty_widget;
    configure_view(empty_widget, 0, 100, 0.0f, 10.0f);
    empty_widget.add_series(1, lower);
    empty_widget.add_series(2, upper);
    std::map<int, std::shared_ptr<const plot::series_data_t>> empty_series{
        {1, lower}, {2, upper}};
    publish_rendered_stack_validity(empty_widget, empty_series, 0, 100);
    auto status = empty_widget.stack_status(7, plot::Series_view_kind::MAIN);
    TEST_ASSERT(status.state == plot::Stack_view_state::SUPPRESSED &&
        status.reason == plot::Stack_rejection_reason::NO_DRAWABLE_DATA,
        "an empty source should publish a current no-drawable-data rejection");

    empty_source->set_data({{0, 1.0f}, {100, 2.0f}});
    TEST_ASSERT(empty_widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::PENDING,
        "an empty source becoming drawable should stale its rejection");
    publish_rendered_stack_validity(empty_widget, empty_series, 0, 100);
    TEST_ASSERT(empty_widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::ACTIVE,
        "renderer publication should recover an empty-source rejection");

    empty_source->set_data({});
    TEST_ASSERT(empty_widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::PENDING,
        "a drawable source becoming empty should stale its active result");
    publish_rendered_stack_validity(empty_widget, empty_series, 0, 100);
    status = empty_widget.stack_status(7, plot::Series_view_kind::MAIN);
    TEST_ASSERT(status.state == plot::Stack_view_state::SUPPRESSED &&
        status.reason == plot::Stack_rejection_reason::NO_DRAWABLE_DATA,
        "a new publication should restore the current empty-source rejection");

    return true;
}

bool test_stack_status_detects_source_advance_after_snapshot()
{
    auto advancing_source = std::make_shared<advancing_after_snapshot_source_t>();
    auto stable_source = std::make_shared<plot::Vector_data_source<indicator_sample_t>>(
        std::vector<indicator_sample_t>{{0, 3.0f}, {100, 4.0f}});
    auto lower = make_stack_status_series(advancing_source);
    auto upper = make_stack_status_series(stable_source);
    indicator_test_widget_t widget;
    configure_view(widget, 0, 100, 0.0f, 10.0f);
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    const std::map<int, std::shared_ptr<const plot::series_data_t>> series{
        {1, lower}, {2, upper}};

    publish_rendered_stack_validity(widget, series, 0, 100);
    TEST_ASSERT(widget.stack_status(7, plot::Series_view_kind::MAIN).state ==
        plot::Stack_view_state::PENDING,
        "a source advancing after its rendered snapshot should stale the publication");

    return true;
}

bool test_stack_status_labels_nonfinite_planner_outcomes_truthfully()
{
    const auto status_for = [](plot::Nonfinite_sample_policy policy) {
        auto affected = std::make_shared<plot::Vector_data_source<indicator_sample_t>>(
            std::vector<indicator_sample_t>{
                { 0, 1.0f },
                { 50, std::numeric_limits<float>::quiet_NaN() },
                {100, 2.0f}});
        auto valid = std::make_shared<plot::Vector_data_source<indicator_sample_t>>(
            std::vector<indicator_sample_t>{{0, 3.0f}, {100, 4.0f}});
        auto lower = make_stack_status_series(affected, policy);
        auto upper = make_stack_status_series(valid);
        indicator_test_widget_t widget;
        configure_view(widget, 0, 100, 0.0f, 10.0f);
        widget.add_series(1, lower);
        widget.add_series(2, upper);
        const std::map<int, std::shared_ptr<const plot::series_data_t>> series{
            {1, lower}, {2, upper}};
        publish_rendered_stack_validity(widget, series, 0, 100);
        return widget.stack_status(7, plot::Series_view_kind::MAIN);
    };

    const auto broken   = status_for(plot::Nonfinite_sample_policy::BREAK_SEGMENT);
    const auto skipped  = status_for(plot::Nonfinite_sample_policy::SKIP);
    const auto rejected = status_for(plot::Nonfinite_sample_policy::REJECT_WINDOW);
    TEST_ASSERT(broken.state == plot::Stack_view_state::SUPPRESSED &&
        broken.reason == plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "BREAK_SEGMENT gaps should report incompatible composed data");
    TEST_ASSERT(skipped.state == plot::Stack_view_state::SUPPRESSED &&
        skipped.reason == plot::Stack_rejection_reason::INCOMPATIBLE_DATA,
        "SKIP gaps should report incompatible composed data");
    TEST_ASSERT(rejected.state == plot::Stack_view_state::SUPPRESSED &&
        rejected.reason == plot::Stack_rejection_reason::NO_DRAWABLE_DATA,
        "REJECT_WINDOW should report that the planner supplied no drawable data");

    return true;
}

bool test_indicator_omits_sum_when_renderer_rejects_stack()
{
    constexpr std::int64_t k_second_ns = 1'000'000'000LL;
    indicator_test_widget_t widget;
    configure_view(widget, 0, 2 * k_second_ns, 0.0f, 10.0f);

    auto lower = make_sample_series(
        {
            { 0, 1.0f },
            { k_second_ns / 2, 1.0f },
            { k_second_ns, std::numeric_limits<float>::quiet_NaN() },
            { 3 * k_second_ns / 2, 1.0f },
            { 2 * k_second_ns, 1.0f },
        },
        plot::Series_interpolation::LINEAR);
    auto upper = make_sample_series(
        {{0, 2.0f}, {2 * k_second_ns, 2.0f}}, plot::Series_interpolation::LINEAR);
    lower->stack_group = upper->stack_group = 1;
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    publish_rendered_stack_validity(
        widget,
        { {1, lower}, {2, upper} },
        0,
        2 * k_second_ns);

    const QVariantList nonfinite = widget.get_indicator_samples(250.0, 100.0, 100.0);
    TEST_ASSERT(nonfinite.size() == 2,
        "finite hover bracket must not show a sum for a nonfinite-rejected rendered stack");
    TEST_ASSERT(!nonfinite[0].toMap().value("show_marker").toBool() &&
        !nonfinite[1].toMap().value("show_marker").toBool(),
        "nonfinite-rejected stack component markers should stay hidden");

    auto* source = dynamic_cast<plot::Vector_data_source<indicator_sample_t>*>(lower->main_source());
    TEST_ASSERT(source, "stack regression fixture should retain its vector source");
    source->set_data({
        { 0,                   1.0f },
        { k_second_ns / 2,     1.0f },
        { 3 * k_second_ns / 2, 1.0f },
        { k_second_ns,         1.0f },
        { 2 * k_second_ns,     1.0f },
    });
    publish_rendered_stack_validity(
        widget,
        { {1, lower}, {2, upper} },
        0,
        2 * k_second_ns);
    const QVariantList nonmonotonic = widget.get_indicator_samples(250.0, 100.0, 100.0);
    TEST_ASSERT(nonmonotonic.size() == 2,
        "finite hover bracket must not show a sum for a nonmonotonic-rejected rendered stack");
    TEST_ASSERT(!nonmonotonic[0].toMap().value("show_marker").toBool() &&
        !nonmonotonic[1].toMap().value("show_marker").toBool(),
        "nonmonotonic-rejected stack component markers should stay hidden");

    return true;
}

bool test_nearest_samples_choose_closer_sample()
{
    plot::Plot_widget widget;
    configure_indicator_widget(widget, plot::Series_interpolation::LINEAR);

    const QVariantList samples = widget.get_nearest_samples(750.0, 100.0, 100.0);
    TEST_ASSERT(samples.size() == 1, "nearest indicator query should return one sample");

    const QVariantMap entry = samples.front().toMap();
    TEST_ASSERT(nearly_equal(entry.value("y").toDouble(), 10.0),
        "nearest query should choose the closer second sample");
    TEST_ASSERT(nearly_equal(entry.value("x").toDouble(), 1000.0),
        "nearest query should report the resolved sample timestamp in milliseconds");

    return true;
}

bool test_auto_adjust_view_uses_visible_samples_for_value_and_time_range()
{
    plot::Plot_widget widget;
    configure_view(widget, 5, 25, -100.0f, 100.0f);
    widget.add_series(1, make_sample_series(
        {
            { 0,  100.0f },
            { 10, 2.0f   },
            { 20, 8.0f   },
            { 30, -100.0f}
        },
        plot::Series_interpolation::LINEAR));

    widget.auto_adjust_view(true, 0.0, false);

    TEST_ASSERT(nearly_equal(widget.v_min(), 2.0),
        "auto-adjust should use the visible value minimum");
    TEST_ASSERT(nearly_equal(widget.v_max(), 8.0),
        "auto-adjust should use the visible value maximum");
    TEST_ASSERT(widget.t_min() == 10 && widget.t_max() == 20,
        "auto-adjust should shrink time range to visible samples");

    return true;
}

bool test_auto_adjust_view_includes_step_after_held_sample()
{
    plot::Plot_widget widget;
    configure_view(widget, 10, 20, -100.0f, 100.0f);
    widget.add_series(1, make_sample_series(
        {
            { 0, 2.0f },
            {15, 8.0f}
        },
        plot::Series_interpolation::STEP_AFTER));

    widget.auto_adjust_view(false, 0.0, false);

    TEST_ASSERT(nearly_equal(widget.v_min(), 2.0),
        "step-after auto-adjust should include the held value before the window");
    TEST_ASSERT(nearly_equal(widget.v_max(), 8.0),
        "step-after auto-adjust should include the in-window value");

    return true;
}

bool test_auto_adjust_view_uses_rendered_stack_and_preserves_unstacked_range()
{
    indicator_test_widget_t widget;
    configure_view(widget, 0, 100, -100.0f, 100.0f);

    auto lower = make_sample_series(
        {{0, 1.0f}, {100, 3.0f}}, plot::Series_interpolation::LINEAR);
    auto upper = make_sample_series(
        {{0, 10.0f}, {100, 20.0f}}, plot::Series_interpolation::LINEAR);
    lower->stack_group = upper->stack_group = 1;
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    publish_rendered_stack_validity(widget, {{1, lower}, {2, upper}}, 0, 100);

    widget.auto_adjust_view(false, 0.0, false);
    TEST_ASSERT(nearly_equal(widget.v_min(), 1.0) && nearly_equal(widget.v_max(), 23.0),
        "auto-adjust should fit cumulative rendered stack values rather than raw components");

    lower->stack_group = upper->stack_group = 0;
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    widget.auto_adjust_view(false, 0.0, false);
    TEST_ASSERT(nearly_equal(widget.v_min(), 1.0) && nearly_equal(widget.v_max(), 20.0),
        "auto-adjust should preserve ordinary unstacked range fitting");

    lower->stack_group = upper->stack_group = 1;
    upper->style = plot::Display_style::NONE;
    widget.add_series(1, lower);
    widget.add_series(2, upper);
    publish_rendered_stack_validity(widget, {{1, lower}, {2, upper}}, 0, 100);
    widget.auto_adjust_view(false, 0.0, false);
    TEST_ASSERT(nearly_equal(widget.v_min(), 1.0) && nearly_equal(widget.v_max(), 3.0),
        "a hidden stack peer should leave the rendered singleton's ordinary range fitting intact");

    return true;
}

bool test_shared_vbar_explicit_width_publishes_when_sync_enabled()
{
    plot::Plot_time_axis shared_axis;
    plot::Plot_widget widget;

    shared_axis.set_sync_vbar_width(true);
    widget.set_time_axis(&shared_axis);
    widget.set_vbar_width(64.0);

    TEST_ASSERT(nearly_equal(shared_axis.shared_vbar_width_px(), widget.vbar_width_pixels()),
        "explicit vbar width should publish to the shared axis when sync is enabled");

    return true;
}

bool test_shared_vbar_attach_publishes_existing_current_width()
{
    plot::Plot_time_axis shared_axis;
    plot::Plot_widget widget;

    widget.set_vbar_width(80.0);
    const double owner_width_px = widget.vbar_width_pixels();

    shared_axis.set_sync_vbar_width(true);
    widget.set_time_axis(&shared_axis);

    TEST_ASSERT(nearly_equal(shared_axis.shared_vbar_width_px(), owner_width_px),
        "attaching to a sync-enabled axis should publish the widget's current vbar width");

    return true;
}

bool test_shared_vbar_enabling_sync_publishes_current_owner_width()
{
    plot::Plot_time_axis shared_axis;
    plot::Plot_widget widget;

    widget.set_time_axis(&shared_axis);
    widget.set_vbar_width(96.0);

    TEST_ASSERT(nearly_equal(shared_axis.shared_vbar_width_px(), 0.0),
        "disabled vbar sync should not publish the current owner width");

    const double owner_width_px = widget.vbar_width_pixels();
    shared_axis.set_sync_vbar_width(true);

    TEST_ASSERT(nearly_equal(shared_axis.shared_vbar_width_px(), owner_width_px),
        "enabling vbar sync should publish the current owner width");

    return true;
}

bool test_widget_local_available_clamp_matches_shared_axis()
{
    plot::Plot_widget local_widget;
    configure_time_window(local_widget, 100, 200, 0, 500);

    plot::Plot_time_axis shared_axis;
    plot::Plot_widget shared_widget;
    shared_widget.set_time_axis(&shared_axis);
    configure_time_window(shared_widget, 100, 200, 0, 500);

    local_widget.set_available_t_range(0, 150);
    shared_widget.set_available_t_range(0, 150);

    TEST_ASSERT(local_widget.t_min() == shared_widget.t_min(),
        "widget-local available clamp should match shared-axis t_min");
    TEST_ASSERT(local_widget.t_max() == shared_widget.t_max(),
        "widget-local available clamp should match shared-axis t_max");
    TEST_ASSERT(local_widget.t_min() == 50 && local_widget.t_max() == 150,
        "available clamp should preserve the 100 ns view span at the right edge");

    return true;
}

bool test_widget_local_preview_adjustment_matches_shared_axis()
{
    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

    plot::Plot_widget local_widget;
    configure_time_window(local_widget, k_min, k_min + 100, k_min, k_max);

    plot::Plot_time_axis shared_axis;
    plot::Plot_widget shared_widget;
    shared_widget.set_time_axis(&shared_axis);
    configure_time_window(shared_widget, k_min, k_min + 100, k_min, k_max);

    local_widget.adjust_t_from_mouse_diff_on_preview(100.0, 75.0);
    shared_widget.adjust_t_from_mouse_diff_on_preview(100.0, 75.0);

    TEST_ASSERT(local_widget.t_min() == shared_widget.t_min(),
        "widget-local preview drag should match shared-axis t_min");
    TEST_ASSERT(local_widget.t_max() == shared_widget.t_max(),
        "widget-local preview drag should match shared-axis t_max");

    const auto moved_span = plot::positive_span_ns(
        local_widget.t_min(),
        local_widget.t_max());
    TEST_ASSERT(moved_span && *moved_span == 100,
        "widget-local preview drag should preserve the 100 ns view span");

    return true;
}

bool test_set_config_applies_complete_config_and_notifies()
{
    plot::Plot_widget widget;
    int               dark_notifications    = 0;
    int               grid_notifications    = 0;
    int               preview_notifications = 0;
    int               line_notifications    = 0;

    QObject::connect(&widget, &plot::Plot_widget::dark_mode_changed,
        &widget, [&]() { ++dark_notifications; });
    QObject::connect(&widget, &plot::Plot_widget::grid_visibility_changed,
        &widget, [&]() { ++grid_notifications; });
    QObject::connect(&widget, &plot::Plot_widget::preview_visibility_changed,
        &widget, [&]() { ++preview_notifications; });
    QObject::connect(&widget, &plot::Plot_widget::line_width_px_changed,
        &widget, [&]() { ++line_notifications; });

    auto config = widget.config();
    config.dark_mode          = true;
    config.grid_visibility    = 0.25;
    config.preview_visibility = 0.5;
    config.line_width_px      = 2.0;
    widget.set_config(config);

    const auto applied = widget.config();
    TEST_ASSERT(applied.dark_mode && widget.dark_mode(),
        "set_config should apply dark_mode");
    TEST_ASSERT(applied.grid_visibility == 0.25 && widget.grid_visibility() == 0.25,
        "set_config should apply grid_visibility");
    TEST_ASSERT(applied.preview_visibility == 0.5 && widget.preview_visibility() == 0.5,
        "set_config should apply preview_visibility");
    TEST_ASSERT(applied.line_width_px == 2.0 && widget.line_width_px() == 2.0,
        "set_config should apply line_width_px");
    TEST_ASSERT(dark_notifications == 1 && grid_notifications == 1 &&
            preview_notifications == 1 && line_notifications == 1,
        "set_config should notify every changed config-backed QML property");

    widget.set_config(applied);
    TEST_ASSERT(dark_notifications == 1 && grid_notifications == 1 &&
            preview_notifications == 1 && line_notifications == 1,
        "set_config should not notify unchanged config-backed QML properties");

    return true;
}

bool test_preview_thumb_press_handles_full_int64_availability()
{
    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

    plot::Plot_widget widget;
    widget.set_available_t_range(k_min, k_max);
    widget.set_t_range(-100, 100);
    widget.set_preview_height(20.0);

    test_interaction_item_t item;
    item.set_plot_widget(&widget);
    item.setWidth(100.0);
    item.setHeight(100.0);

    QMouseEvent press(
        QEvent::MouseButtonPress,
        QPointF{50.0, 90.0},
        QPointF{50.0, 90.0},
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier);
    item.mousePressEvent(&press);

    TEST_ASSERT(press.isAccepted(),
        "preview-thumb press should be handled");
    TEST_ASSERT(widget.t_min() == -100 && widget.t_max() == 100,
        "center preview-thumb press should not recenter over full int64 availability");

    return true;
}

bool wait_for_preview_height(plot::Plot_widget& widget, double target)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (std::abs(widget.preview_height() - target) > 0.001 && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return std::abs(widget.preview_height() - target) <= 0.001;
}

bool test_automatic_preview_tracks_resize()
{
    plot::Plot_widget widget;
    widget.setHeight(700.0);
    TEST_ASSERT(wait_for_preview_height(widget, 150.0), "automatic preview should reach its upper bound");
    widget.setHeight(200.0);
    TEST_ASSERT(widget.preview_height_target() == 30.0, "smaller geometry should calculate the collapsed preview");
    TEST_ASSERT(wait_for_preview_height(widget, 30.0), "automatic preview should apply its new target without a QML handler");
    widget.setHeight(700.0);
    TEST_ASSERT(wait_for_preview_height(widget, 150.0), "automatic preview should grow again");

    auto config = widget.config();
    config.preview_height_px = 45.0;
    widget.set_config(config);
    widget.setHeight(200.0);
    TEST_ASSERT(widget.preview_height() == 45.0, "fixed preview height should survive geometry changes");
    return true;
}

bool test_automatic_preview_resumes_after_fixed_height()
{
    plot::Plot_widget widget;
    widget.setHeight(700.0);
    TEST_ASSERT(wait_for_preview_height(widget, 150.0), "automatic preview should initialize at the tall target");

    auto config = widget.config();
    config.preview_height_px = 45.0;
    widget.set_config(config);
    TEST_ASSERT(widget.preview_height() == 45.0, "fixed mode should apply its configured height");
    config.preview_height_px = 0.0;
    widget.set_config(config);
    TEST_ASSERT(wait_for_preview_height(widget, 150.0), "returning to automatic mode should reapply the unchanged target");

    widget.set_preview_height(45.0);
    widget.setWidth(640.0);
    TEST_ASSERT(widget.preview_height() == 45.0, "an explicit height override should remain until its automatic target changes");

    widget.setHeight(200.0);
    config.preview_height_px = 45.0;
    widget.set_config(config);
    config.preview_height_px = 0.0;
    widget.set_config(config);
    TEST_ASSERT(wait_for_preview_height(widget, 30.0), "automatic sizing should resume after fixed mode interrupts a transition");
    return true;
}

bool test_opt_in_preview_collapse()
{
    // The Lumis wrapper previously applied this interpolation in QML.
    plot::Plot_widget widget;
    widget.setHeight(700.0);
    TEST_ASSERT(wait_for_preview_height(widget, 150.0), "preview should reach its full height");
    widget.set_preview_visibility(0.5);
    TEST_ASSERT(widget.preview_height() == 150.0, "opacity alone must retain existing host geometry");
    widget.set_collapse_hidden_preview(true);
    TEST_ASSERT(widget.preview_height() == 90.0, "half visibility should interpolate 30 and 150 DIPs");
    widget.set_preview_visibility(0.0);
    TEST_ASSERT(widget.preview_height() == 30.0, "hidden preview should retain its collapsed height");
    int height_notifications = 0;
    QObject::connect(&widget, &plot::Plot_widget::preview_height_changed,
        [&height_notifications] { ++height_notifications; });
    widget.set_preview_height_min(35.0);
    TEST_ASSERT(widget.preview_height() == 35.0 && height_notifications > 0,
        "collapsed minimum changes should notify dependent geometry even with a stable full target");

    auto config = widget.config();
    config.preview_height_px = 45.0;
    widget.set_config(config);
    TEST_ASSERT(widget.preview_height() == 45.0, "explicit preview height must remain authoritative");
    return true;
}

bool test_double_click_and_focus()
{
    // Lumis's former MouseArea focused the plot and routed double clicks to its host.
    QQuickWindow window;
    plot::Plot_widget widget;
    test_interaction_item_t item;
    item.setParentItem(window.contentItem());
    item.set_plot_widget(&widget);
    item.setWidth(640.0);
    item.setHeight(480.0);
    int double_clicks = 0;
    QObject::connect(&item, &plot::Plot_interaction_item::mouse_double_clicked,
        [&double_clicks](qreal x, qreal y) {
            if (x == 30.0 && y == 40.0) {
                ++double_clicks;
            }
        });
    QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(30.0, 40.0), QPointF(30.0, 40.0),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    item.mouseDoubleClickEvent(&event);
    TEST_ASSERT(double_clicks == 1 && event.isAccepted(), "double click must route to the host");
    TEST_ASSERT(item.hasFocus(), "plot input should acquire keyboard focus");
    item.set_interaction_enabled(false);
    item.mouseDoubleClickEvent(&event);
    TEST_ASSERT(double_clicks == 1 && !event.isAccepted(), "disabled input must not dispatch double clicks");
    return true;
}

bool test_same_screen_pixel_ratio_change_updates_plot_scaling()
{
    // Qt defines effectiveDevicePixelRatio from the redirected render target.
    // Its normal DevicePixelRatioChange event sends that scale to content items.
    QQuickRenderControl control;
    QQuickWindow window(&control);
    QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
    auto target = QQuickRenderTarget::fromPaintDevice(&image);
    const double initial_scale = window.devicePixelRatio();
    target.setDevicePixelRatio(initial_scale);
    window.setRenderTarget(target);
    plot::Plot_widget widget;
    auto config = widget.config();
    config.preview_height_px = 30.0;
    widget.set_config(config);
    widget.setParentItem(window.contentItem());
    widget.setSize(QSizeF(400.0, 300.0));
    TEST_ASSERT(widget.scaling_factor() == initial_scale, "initial render target uses the window scale");
    const auto screen = window.screen();

    target.setDevicePixelRatio(initial_scale * 2.0);
    window.setRenderTarget(target);
    QEvent changed(QEvent::DevicePixelRatioChange);
    QCoreApplication::sendEvent(&window, &changed);
    TEST_ASSERT(window.screen() == screen, "DPR regression must stay on the same screen");
    TEST_ASSERT(widget.scaling_factor() == initial_scale * 2.0, "item DPR notification should update scale without screenChanged");
    TEST_ASSERT(widget.preview_height() == 30.0, "DPR should preserve logical preview height");
    TEST_ASSERT(widget.reserved_height() == 44.0, "DPR should preserve logical preview and label reservation");
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);

    std::cout << "Plot interaction item tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_zoom_math_is_invariant_to_timer_cadence);
    RUN_TEST(test_zoom_math_stays_composable_across_small_velocity_decay);
    RUN_TEST(test_zoom_math_handles_negative_velocity);
    RUN_TEST(test_zoom_math_handles_zero_velocity);
    RUN_TEST(test_wheel_zoom_handles_near_zero_value_range);
    RUN_TEST(test_indicator_samples_linearly_interpolate_between_samples);
    RUN_TEST(test_indicator_samples_step_after_holds_previous_sample);
    RUN_TEST(test_indicator_reports_stack_sum_only_inside_common_domain);
    RUN_TEST(test_stacked_indicator_matches_sub_256ns_epoch_composition);
    RUN_TEST(test_stack_status_reports_views_freshness_rejection_and_recovery);
    RUN_TEST(test_stack_status_handles_unsupported_and_empty_source_revisions);
    RUN_TEST(test_stack_status_detects_source_advance_after_snapshot);
    RUN_TEST(test_stack_status_labels_nonfinite_planner_outcomes_truthfully);
    RUN_TEST(test_indicator_omits_sum_when_renderer_rejects_stack);
    RUN_TEST(test_nearest_samples_choose_closer_sample);
    RUN_TEST(test_auto_adjust_view_uses_visible_samples_for_value_and_time_range);
    RUN_TEST(test_auto_adjust_view_includes_step_after_held_sample);
    RUN_TEST(test_auto_adjust_view_uses_rendered_stack_and_preserves_unstacked_range);
    RUN_TEST(test_shared_vbar_explicit_width_publishes_when_sync_enabled);
    RUN_TEST(test_shared_vbar_attach_publishes_existing_current_width);
    RUN_TEST(test_shared_vbar_enabling_sync_publishes_current_owner_width);
    RUN_TEST(test_widget_local_available_clamp_matches_shared_axis);
    RUN_TEST(test_widget_local_preview_adjustment_matches_shared_axis);
    RUN_TEST(test_set_config_applies_complete_config_and_notifies);
    RUN_TEST(test_preview_thumb_press_handles_full_int64_availability);
    RUN_TEST(test_automatic_preview_tracks_resize);
    RUN_TEST(test_automatic_preview_resumes_after_fixed_height);
    RUN_TEST(test_opt_in_preview_collapse);
    RUN_TEST(test_double_click_and_focus);
    RUN_TEST(test_same_screen_pixel_ratio_change_updates_plot_scaling);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
