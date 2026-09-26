// vnm_plot Plot_time_axis tests

#include "test_macros.h"

#include <vnm_plot/core/time_units.h>
#include <vnm_plot/qt/plot_time_axis.h>

#include <cstdint>
#include <iostream>
#include <limits>

namespace plot = vnm::plot;

namespace {

constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

bool test_range_notifications_observe_committed_state()
{
    plot::Plot_time_axis axis;
    int notifications = 0;
    qint64 observed_min = 0;
    qint64 observed_max = 0;
    bool observed_initialized = false;
    QObject::connect(&axis, &plot::Plot_time_axis::t_limits_changed, &axis, [&]() {
        ++notifications;
        observed_min = axis.t_min();
        observed_max = axis.t_max();
        observed_initialized = axis.view_initialized();
    });

    axis.set_t_min(100);
    TEST_ASSERT(notifications == 1 && observed_min == 100 && !observed_initialized,
        "seeding a bound should notify after committing its partial state");
    axis.set_t_min(100);
    axis.set_t_max(100);
    TEST_ASSERT(notifications == 1 && !axis.view_initialized(),
        "unchanged and rejected bound writes should not notify");

    axis.set_t_max(200);
    TEST_ASSERT(notifications == 2 && observed_min == 100 && observed_max == 200 &&
            observed_initialized,
        "completing a view should notify after both bounds are readable");
    axis.set_t_range(100, 200);
    axis.set_t_range(200, 100);
    axis.adjust_t_from_mouse_diff(100.0, 0.0);
    axis.adjust_t_from_mouse_diff(0.0, 10.0);
    TEST_ASSERT(notifications == 2,
        "unchanged and rejected range adjustments should not notify");

    axis.set_available_t_range(0, 150);
    TEST_ASSERT(notifications == 3 && observed_min == 50 && observed_max == 150 &&
            axis.t_available_min() == 0 && axis.t_available_max() == 150,
        "available-range clamping should commit the complete transition before notifying");
    axis.adjust_t_from_mouse_diff(100.0, 50.0);
    TEST_ASSERT(notifications == 4 && observed_min == 0 && observed_max == 100,
        "a pan should commit and notify once");

    return true;
}

bool test_uninitialized_qml_properties_return_zero()
{
    plot::Plot_time_axis axis;

    TEST_ASSERT(axis.t_min_qml_ms() == 0,
        "fresh axis should expose zero QML view minimum");
    TEST_ASSERT(axis.t_max_qml_ms() == 0,
        "fresh axis should expose zero QML view maximum");
    TEST_ASSERT(axis.t_available_min_qml_ms() == 0,
        "fresh axis should expose zero QML available minimum");
    TEST_ASSERT(axis.t_available_max_qml_ms() == 0,
        "fresh axis should expose zero QML available maximum");

    axis.set_t_min(k_min);
    axis.set_t_available_max(k_max);

    TEST_ASSERT(axis.t_min_qml_ms() == 0,
        "half-seeded view should still expose zero QML minimum");
    TEST_ASSERT(axis.t_max_qml_ms() == 0,
        "half-seeded view should still expose zero QML maximum");
    TEST_ASSERT(axis.t_available_min_qml_ms() == 0,
        "half-seeded available range should still expose zero QML minimum");
    TEST_ASSERT(axis.t_available_max_qml_ms() == 0,
        "half-seeded available range should still expose zero QML maximum");

    return true;
}

bool test_full_int64_range_initializes()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(k_min, k_max);

    TEST_ASSERT(axis.view_initialized(),
        "full int64 view range should initialize");
    TEST_ASSERT(axis.t_min() == k_min,
        "full int64 view range should preserve INT64_MIN");
    TEST_ASSERT(axis.t_max() == k_max,
        "full int64 view range should preserve INT64_MAX");

    return true;
}

bool test_full_int64_available_range_initializes()
{
    plot::Plot_time_axis axis;

    axis.set_available_t_range(k_min, k_max);

    TEST_ASSERT(axis.available_initialized(),
        "full int64 available range should initialize");
    TEST_ASSERT(axis.t_available_min() == k_min,
        "full int64 available range should preserve INT64_MIN");
    TEST_ASSERT(axis.t_available_max() == k_max,
        "full int64 available range should preserve INT64_MAX");

    return true;
}

bool test_qml_negative_overflow_seed_is_real_timestamp()
{
    plot::Plot_time_axis axis;

    axis.set_t_min_qml_ms(k_min);

    TEST_ASSERT(!axis.view_initialized(),
        "single QML-side minimum write should remain half-seeded");
    TEST_ASSERT(axis.t_min() == k_min,
        "negative-overflow QML conversion should store saturated INT64_MIN as data");

    axis.set_t_max_qml_ms(0);

    TEST_ASSERT(axis.view_initialized(),
        "second QML-side bound should complete the view range");
    TEST_ASSERT(axis.t_min() == k_min,
        "completed QML range should keep saturated INT64_MIN as data");
    TEST_ASSERT(axis.t_max() == 0,
        "completed QML range should keep the explicit maximum");

    return true;
}

bool test_half_seeded_available_range_does_not_clamp_target()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(0, 100);
    axis.set_t_available_max(50);
    axis.adjust_t_to_target(200, 300);

    TEST_ASSERT(!axis.available_initialized(),
        "single available bound should remain half-seeded");
    TEST_ASSERT(axis.t_min() == 200 && axis.t_max() == 300,
        "half-seeded available range should not clamp target adjustment");

    return true;
}

bool test_half_seeded_bounds_are_queryable()
{
    plot::Plot_time_axis axis;

    TEST_ASSERT(!axis.any_view_bound_initialized(),
        "fresh axis should report no initialized view bounds");
    TEST_ASSERT(!axis.any_available_bound_initialized(),
        "fresh axis should report no initialized available bounds");

    axis.set_t_min(k_min);
    axis.set_t_available_max(1000);

    TEST_ASSERT(axis.any_view_bound_initialized(),
        "single view bound should be queryable for attach seeding");
    TEST_ASSERT(!axis.view_initialized(),
        "single view bound should not initialize the full view range");
    TEST_ASSERT(axis.t_min() == k_min,
        "single initialized view bound should preserve INT64_MIN");
    TEST_ASSERT(axis.any_available_bound_initialized(),
        "single available bound should be queryable for attach seeding");
    TEST_ASSERT(!axis.available_initialized(),
        "single available bound should not initialize the full available range");
    TEST_ASSERT(axis.t_available_max() == 1000,
        "single initialized available bound should preserve its value");

    return true;
}

bool test_preview_recenter_near_int64_max_preserves_span()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(k_max - 1000, k_max - 900);
    axis.set_available_t_range(k_max - 1000, k_max);
    axis.adjust_t_from_mouse_pos_on_preview(100.0, 100.0);

    TEST_ASSERT(axis.t_min() == k_max - 100,
        "preview recenter at INT64_MAX should preserve the 100 ns span");
    TEST_ASSERT(axis.t_max() == k_max,
        "preview recenter at INT64_MAX should clamp to the right edge");

    return true;
}

bool test_full_available_preview_drag_reaches_right_half()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(k_min, k_min + 100);
    axis.set_available_t_range(k_min, k_max);
    axis.adjust_t_from_mouse_diff_on_preview(100.0, 75.0);

    constexpr std::int64_t k_expected_min = 4'611'686'018'427'387'903LL;
    TEST_ASSERT(
        axis.t_min() >= k_expected_min - 128 &&
        axis.t_min() <= k_expected_min + 128,
        "full-range preview drag should move into the right half, not near -1");

    const auto moved_span = plot::positive_span_ns(axis.t_min(), axis.t_max());
    TEST_ASSERT(moved_span && *moved_span == 100,
        "full-range preview drag should preserve the 100 ns view span");

    return true;
}

bool test_full_available_main_drag_reaches_right_edge()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(k_min, k_min + 100);
    axis.set_available_t_range(k_min, k_max);
    axis.adjust_t_from_mouse_diff(1.0, -200'000'000'000'000'000.0);

    TEST_ASSERT(axis.t_min() == k_max - 100,
        "full-range main drag should move to the right edge, not near -1");
    TEST_ASSERT(axis.t_max() == k_max,
        "full-range main drag should clamp to INT64_MAX");

    const auto moved_span = plot::positive_span_ns(axis.t_min(), axis.t_max());
    TEST_ASSERT(moved_span && *moved_span == 100,
        "full-range main drag should preserve the 100 ns view span");

    return true;
}

bool test_full_available_preview_recenter_reaches_right_edge()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(-50, 50);
    axis.set_available_t_range(k_min, k_max);
    axis.adjust_t_from_mouse_pos_on_preview(100.0, 100.0);

    TEST_ASSERT(axis.t_min() == k_max - 100,
        "full-range preview recenter should preserve span at the right edge");
    TEST_ASSERT(axis.t_max() == k_max,
        "full-range preview recenter should reach INT64_MAX instead of -1");

    return true;
}

bool test_full_range_zoom_pivot_reaches_right_edge()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(k_min, k_max);
    axis.adjust_t_from_pivot_and_scale(1.0, 0.5);

    TEST_ASSERT(axis.t_min() == -1,
        "right-edge zoom on full int64 range should scale from INT64_MAX");
    TEST_ASSERT(axis.t_max() == k_max,
        "right-edge zoom on full int64 range should keep INT64_MAX as pivot");

    return true;
}

bool test_right_edge_zoom_out_preserves_scaled_span()
{
    plot::Plot_time_axis axis;

    axis.set_t_range(k_max - 100, k_max);
    axis.set_available_t_range(k_max - 1000, k_max);
    axis.adjust_t_from_pivot_and_scale(0.0, 2.0);

    TEST_ASSERT(axis.t_min() == k_max - 200,
        "right-edge zoom-out should preserve the intended 200 ns span");
    TEST_ASSERT(axis.t_max() == k_max,
        "right-edge zoom-out should clamp to available maximum");

    return true;
}

} // namespace

int main()
{
    std::cout << "Plot time-axis tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_range_notifications_observe_committed_state);
    RUN_TEST(test_uninitialized_qml_properties_return_zero);
    RUN_TEST(test_full_int64_range_initializes);
    RUN_TEST(test_full_int64_available_range_initializes);
    RUN_TEST(test_qml_negative_overflow_seed_is_real_timestamp);
    RUN_TEST(test_half_seeded_available_range_does_not_clamp_target);
    RUN_TEST(test_half_seeded_bounds_are_queryable);
    RUN_TEST(test_preview_recenter_near_int64_max_preserves_span);
    RUN_TEST(test_full_available_preview_drag_reaches_right_half);
    RUN_TEST(test_full_available_main_drag_reaches_right_edge);
    RUN_TEST(test_full_available_preview_recenter_reaches_right_edge);
    RUN_TEST(test_full_range_zoom_pivot_reaches_right_edge);
    RUN_TEST(test_right_edge_zoom_out_preserves_scaled_span);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
