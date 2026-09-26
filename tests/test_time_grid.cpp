#include "test_macros.h"

#include <vnm_plot/core/time_grid.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace plot = vnm::plot;

namespace {

bool nearly_equal(double actual, double expected, double tolerance = 1e-5)
{
    return std::abs(actual - expected) <= tolerance;
}

float expected_alpha(float spacing_px)
{
    constexpr double k_cell_span_min = 1.0;
    constexpr double k_fade_den      = 59.0;
    constexpr double k_alpha_base    = 0.75;
    const double     fade            = (double(spacing_px) - k_cell_span_min) / k_fade_den;
    return static_cast<float>(std::clamp(fade, 0.0, 1.0) * k_alpha_base);
}

float expected_thickness(float alpha)
{
    constexpr float k_alpha_base = 0.75f;
    return 0.6f + 0.6f * (alpha / k_alpha_base);
}

std::string describe_levels(const plot::grid_layer_params_t& levels)
{
    std::ostringstream out;
    out << "count=" << levels.count;
    for (int i = 0; i < levels.count; ++i) {
        out << " [" << i << "] spacing=" << levels.spacing_px[i]
            << " start=" << levels.start_px[i];
    }
    return out.str();
}

bool test_time_grid_layers_preserve_spacing_and_style()
{
    const plot::grid_layer_params_t levels =
        plot::build_time_grid_layers(0.0, 60.0, 600.0, 10.0);

    const std::vector<float> expected_spacings = {600.0f, 300.0f, 100.0f, 20.0f, 10.0f, 5.0f};

    TEST_ASSERT(levels.count == static_cast<int>(expected_spacings.size()),
        std::string("60-second window levels mismatch: ") + describe_levels(levels));

    for (int i = 0; i < levels.count; ++i) {
        const float alpha = expected_alpha(expected_spacings[static_cast<std::size_t>(i)]);
        TEST_ASSERT(nearly_equal(levels.spacing_px[i], expected_spacings[static_cast<std::size_t>(i)]),
            "grid level spacing should match the preserved time-step ladder");
        TEST_ASSERT(nearly_equal(levels.start_px[i], 0.0f),
            "zero-origin grid levels should start at zero pixels");
        TEST_ASSERT(nearly_equal(levels.alpha[i], alpha),
            "grid level alpha should follow the preserved fade ramp");
        TEST_ASSERT(nearly_equal(levels.thickness_px[i], expected_thickness(alpha)),
            "grid level thickness should follow the preserved alpha ramp");
    }

    return true;
}

bool test_time_grid_layers_preserve_phase()
{
    const plot::grid_layer_params_t levels =
        plot::build_time_grid_layers(7.25, 67.25, 600.0, 10.0);

    const std::vector<float> expected_starts = {527.5f, 227.5f, 27.5f, 7.5f, 7.5f, 2.5f};

    TEST_ASSERT(levels.count == static_cast<int>(expected_starts.size()),
        std::string("shifted 60-second window levels mismatch: ") + describe_levels(levels));

    for (int i = 0; i < levels.count; ++i) {
        TEST_ASSERT(nearly_equal(levels.start_px[i], expected_starts[static_cast<std::size_t>(i)]),
            "grid level start should preserve get_shift phase alignment");
    }

    return true;
}

bool test_time_grid_layers_reject_degenerate_ranges()
{
    bool dropped_non_multiple_step = true;
    TEST_ASSERT(plot::build_time_grid_layers(
        5.0,
        5.0,
        600.0,
        10.0,
        &dropped_non_multiple_step).count == 0,
        "zero-width time range should produce no grid levels");
    TEST_ASSERT(!dropped_non_multiple_step,
        "degenerate ranges should clear the non-multiple diagnostic flag");
    TEST_ASSERT(plot::build_time_grid_layers(6.0, 5.0, 600.0, 10.0).count == 0,
        "negative time range should produce no grid levels");
    TEST_ASSERT(plot::build_time_grid_layers(0.0, 60.0, 0.0, 10.0).count == 0,
        "zero pixel width should produce no grid levels");
    TEST_ASSERT(plot::build_time_grid_layers(0.0, 60.0, -1.0, 10.0).count == 0,
        "negative pixel width should produce no grid levels");

    return true;
}

bool test_time_grid_layers_do_not_report_non_multiple_for_current_ladder()
{
    bool dropped_non_multiple_step = true;
    plot::build_time_grid_layers(
        0.0,
        60.0,
        600.0,
        10.0,
        &dropped_non_multiple_step);

    TEST_ASSERT(!dropped_non_multiple_step,
        "current time-step ladder should not report non-multiple diagnostics");

    return true;
}

bool test_time_grid_layers_cover_seven_days()
{
    constexpr double k_seconds_per_day = 24.0 * 60.0 * 60.0;
    constexpr double k_view_seconds    = 7.0 * k_seconds_per_day;
    constexpr double k_width_px        = 1200.0;
    const plot::grid_layer_params_t levels =
        plot::build_time_grid_layers(0.0, k_view_seconds, k_width_px, 10.0);
    const float day_spacing_px = float(k_seconds_per_day * k_width_px / k_view_seconds);

    TEST_ASSERT(std::any_of(
        levels.spacing_px,
        levels.spacing_px + levels.count,
        [day_spacing_px](float spacing_px) {
            return nearly_equal(spacing_px, day_spacing_px);
        }),
        "a seven-day grid should retain a daily level for elapsed labels");

    return true;
}

bool test_time_step_selection_preserves_timeline_spacing()
{
    // The timeline requests 92px for elapsed labels and 176px for date/time
    // labels. These are the first ladder steps meeting those pixel budgets.
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(60.0, 600.0, 92.0), 10.0),
        "elapsed labels should use ten-second steps at ten pixels per second");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(60.0, 600.0, 176.0), 30.0),
        "date/time labels should use thirty-second steps at ten pixels per second");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(0.06, 600.0, 92.0), 0.01),
        "subsecond spans should retain millisecond-scale steps");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(0.06, 600.0, 176.0), 0.05),
        "wide subsecond labels should advance to the next ladder step");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(1e-9, 600.0, 92.0), 0.001),
        "nanosecond spans should select the minimum one-millisecond step");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(60.0, 600.0, 100.0), 10.0),
        "a step exactly meeting the minimum must be retained");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(60.0, 600.0, 100.01), 30.0),
        "a step below the minimum must advance");
    return true;
}

bool test_time_step_selection_aligns_with_grid()
{
    constexpr double k_day_seconds = 86400.0;
    constexpr double k_span_seconds = 7.0 * k_day_seconds;
    constexpr double k_width_px = 1200.0;
    const double step = plot::select_time_step_seconds(k_span_seconds, k_width_px, 92.0);
    TEST_ASSERT(step == k_day_seconds, "week-long views should use elapsed daily labels");

    const auto levels = plot::build_time_grid_layers(7.25, 7.25 + k_span_seconds, k_width_px, 10.0);
    const double spacing = step * k_width_px / k_span_seconds;
    const double start = (step - 7.25) * k_width_px / k_span_seconds;
    bool found = false;
    for (int i = 0; i < levels.count; ++i) {
        if (nearly_equal(levels.spacing_px[i], spacing)) {
            TEST_ASSERT(nearly_equal(levels.start_px[i], start),
                "selected tick multiples must align with the daily grid phase");
            found = true;
        }
    }
    TEST_ASSERT(found, "selected daily ticks should share the daily grid level");
    return true;
}

bool test_time_step_selection_invalid_inputs()
{
    const double invalid[] = {0.0, -1.0, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()};
    for (double value : invalid) {
        TEST_ASSERT(plot::select_time_step_seconds(value, 600.0, 92.0) == 1.0,
            "invalid spans should use the one-second default");
        TEST_ASSERT(plot::select_time_step_seconds(60.0, value, 92.0) == 1.0,
            "invalid widths should use the one-second default");
        TEST_ASSERT(plot::select_time_step_seconds(60.0, 600.0, value) == 1.0,
            "invalid spacing requests should use the one-second default");
    }
    return true;
}

bool test_time_step_selection_extreme_scale()
{
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(1e-310, 1.0, 1e308), 0.01),
        "overflowing pixel density must still enforce the minimum spacing");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(1e300, 1e-25, 1e-320), 172800.0 * 2.0),
        "underflowing pixel density must still select a sufficient ladder step");
    TEST_ASSERT(nearly_equal(plot::select_time_step_seconds(1e300, 1e-23, 1.78e-320), 1800.0),
        "subnormal pixel density must retain enough precision to select the first fitting step");
    return true;
}

} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    RUN_TEST(test_time_grid_layers_preserve_spacing_and_style);
    RUN_TEST(test_time_grid_layers_preserve_phase);
    RUN_TEST(test_time_grid_layers_reject_degenerate_ranges);
    RUN_TEST(test_time_grid_layers_do_not_report_non_multiple_for_current_ladder);
    RUN_TEST(test_time_grid_layers_cover_seven_days);
    RUN_TEST(test_time_step_selection_preserves_timeline_spacing);
    RUN_TEST(test_time_step_selection_aligns_with_grid);
    RUN_TEST(test_time_step_selection_invalid_inputs);
    RUN_TEST(test_time_step_selection_extreme_scale);

    std::cout << "\nTime grid tests: " << passed << " passed, " << failed << " failed\n";

    return failed > 0 ? 1 : 0;
}
