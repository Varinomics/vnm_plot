// vnm_plot core algorithm tests

#include "test_macros.h"

#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/color_palette.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/lcd.h>
#include <vnm_plot/core/time_units.h>
#include <vnm_plot/core/zoom_inertia.h>
#include <vnm_plot/core/types.h>
#include <vnm_plot/rhi/text_renderer.h>
#include "../src/core/label_fade_tracker.h"
#include "../src/core/lcd_policy.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace plot = vnm::plot;

static_assert(
    std::is_same_v<plot::Text_renderer::vertical_axis_fade_tracker_t::key_type, double>,
    "vertical label fade tracking remains keyed by double values");
static_assert(
    std::is_same_v<plot::Text_renderer::horizontal_axis_fade_tracker_t::key_type, std::int64_t>,
    "horizontal label fade tracking must be keyed by int64 nanosecond timestamps");
static_assert(
    std::is_same_v<decltype(plot::lcd_request_t{}.automatic), bool>,
    "LCD request state must keep AUTO separate from resolved order");
static_assert(
    std::is_same_v<
        decltype(plot::lcd_request_t{}.resolved_order),
        plot::lcd_subpixel_order_t>,
    "LCD request state must compose the shared resolved order type");
static_assert(
    std::is_same_v<
        plot::lcd_subpixel_order_t,
        vnm::msdf_text::lcd::Resolved_lcd_subpixel_order>,
    "LCD resolved order must use the shared vnm_msdf_text contract type");

namespace {

struct sample_t
{
    // Timestamps are int64 nanoseconds (API convention).
    std::int64_t   t = 0;
    float          v = 0.0f;
};

class Lod_source final : public plot::Data_source
{
public:
    explicit Lod_source(std::vector<std::size_t> scales)
        : m_scales(std::move(scales))
    {}

    plot::snapshot_result_t try_snapshot(std::size_t /*lod*/) override
    {
        return {plot::data_snapshot_t{}, plot::snapshot_result_t::Snapshot_status::EMPTY};
    }

    std::size_t lod_levels() const override { return m_scales.size(); }
    std::size_t lod_scale(std::size_t level) const override
    {
        return (level < m_scales.size()) ? m_scales[level] : 1;
    }
    std::size_t sample_stride() const override { return sizeof(sample_t); }

private:
    std::vector<std::size_t> m_scales;
};

bool test_choose_lod_level_picks_closest_pps()
{
    const std::vector<std::size_t> scales = {1, 4, 16, 64, 256, 1024};
    TEST_ASSERT(plot::detail::choose_lod_level(scales, 0.01) == 3,
        "expected choose_lod_level to pick scale 64");
    return true;
}

bool test_choose_lod_level_handles_degenerate_inputs()
{
    TEST_ASSERT(plot::detail::choose_lod_level({}, 1.0) == 0,
        "empty scales should yield level 0");
    TEST_ASSERT(plot::detail::choose_lod_level({1, 2, 4}, 0.0) == 0,
        "non-positive base_pps should yield level 0");
    TEST_ASSERT(plot::detail::choose_lod_level({1, 2, 4}, -1.0) == 0,
        "negative base_pps should yield level 0");
    return true;
}

bool test_compute_lod_scales_forces_minimum_of_one()
{
    class Zero_scale_source final : public plot::Data_source
    {
    public:
        plot::snapshot_result_t try_snapshot(std::size_t) override
        {
            return {plot::data_snapshot_t{}, plot::snapshot_result_t::Snapshot_status::EMPTY};
        }
        std::size_t lod_levels() const override { return 3; }
        std::size_t lod_scale(std::size_t level) const override
        {
            return (level == 0) ? 0 : (level == 1 ? 1 : 0);
        }
        std::size_t sample_stride() const override { return sizeof(sample_t); }
    };

    Zero_scale_source src;
    auto scales = plot::detail::compute_lod_scales(src);
    TEST_ASSERT(scales.size() == 3, "lod_levels should give 3 entries");
    TEST_ASSERT(scales[0] == 1,     "zero scale at level 0 should be clamped to 1");
    TEST_ASSERT(scales[1] == 1,     "unit scale at level 1 should survive");
    TEST_ASSERT(scales[2] == 1,     "zero scale at level 2 should be clamped to 1");
    return true;
}

bool test_get_shift_handles_invalid_inputs()
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    TEST_ASSERT(plot::detail::get_shift(0.0, 10.0) == 0.0,
        "zero section size should produce zero shift");
    TEST_ASSERT(plot::detail::get_shift(-1.0, 10.0) == 0.0,
        "negative section size should produce zero shift");
    TEST_ASSERT(plot::detail::get_shift(inf, 10.0) == 0.0,
        "infinite section size should produce zero shift");
    TEST_ASSERT(plot::detail::get_shift(nan, 10.0) == 0.0,
        "NaN section size should produce zero shift");
    TEST_ASSERT(plot::detail::get_shift(10.0, inf) == 0.0,
        "infinite minimum value should produce zero shift");
    TEST_ASSERT(plot::detail::get_shift(10.0, nan) == 0.0,
        "NaN minimum value should produce zero shift");
    TEST_ASSERT(std::abs(plot::detail::get_shift(10.0, 23.0) - 7.0) < 1e-12,
        "positive finite get_shift behavior should be preserved");

    return true;
}

bool test_decimal_precision_helpers_reject_invalid_scaled_values()
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    TEST_ASSERT(!plot::detail::any_fractional_at_precision({1.0, 2.0}, 1),
        "integer values should not require fractional precision");
    TEST_ASSERT(plot::detail::any_fractional_at_precision({1.0, 2.5}, 1),
        "ordinary fractional values should still be detected");
    TEST_ASSERT(!plot::detail::any_fractional_at_precision({inf}, 1),
        "infinite values should be rejected cleanly");
    TEST_ASSERT(!plot::detail::any_fractional_at_precision({nan}, 1),
        "NaN values should be rejected cleanly");
    TEST_ASSERT(!plot::detail::any_fractional_at_precision(
        { std::numeric_limits<double>::max() },
        1),
        "values whose scaled form exceeds the safe integer range should be rejected");
    TEST_ASSERT(!plot::detail::any_fractional_at_precision({1.25}, 400),
        "overflowing precision scale should be rejected cleanly");

    return true;
}

bool test_min_v_span_follows_float_magnitude()
{
    const float zero_span = plot::detail::min_v_span_for(0.0f, 0.0f);
    TEST_ASSERT(zero_span > 0.0f && std::isfinite(zero_span),
        "a degenerate zero range should retain a finite precision floor");

    const float mixed_near_zero_span = plot::detail::min_v_span_for(-0.4e-6f, 3.1e-6f);
    TEST_ASSERT(mixed_near_zero_span > 0.0f && mixed_near_zero_span < 3.5e-6f,
        "a mixed-sign micro range should not inherit a unit-scale floor");

    const float negative_span = plot::detail::min_v_span_for(-3.1e-6f, -0.4e-6f);
    TEST_ASSERT(negative_span > 0.0f && negative_span < 2.7e-6f,
        "a negative-only micro range should remain zoomable");

    constexpr float equal_value = 1.0e-6f;
    const float equal_span = plot::detail::min_v_span_for(equal_value, equal_value);
    TEST_ASSERT(
        equal_value - 0.5f * equal_span < equal_value &&
        equal_value + 0.5f * equal_span > equal_value,
        "an equal nonzero range should expand to distinct float endpoints");

    const float large_value = std::ldexp(1.0f, 60);
    const float large_ulp = std::nextafter(
        large_value,
        std::numeric_limits<float>::infinity()) - large_value;
    const float large_span = plot::detail::min_v_span_for(large_value, large_value);
    TEST_ASSERT(large_span >= 64.0f * large_ulp,
        "a large range should retain the existing float-precision protection");
    TEST_ASSERT(large_span < large_value * 1.0e-3f,
        "the precision floor should remain small relative to large values");

    return true;
}

bool test_lower_and_upper_bound_on_contiguous_buffer()
{
    // Sample timestamps are int64 nanoseconds; the test uses small ordinals
    // (0..9 ns) for clarity. Query values are also nanosecond integers.
    std::vector<sample_t> samples;
    for (int i = 0; i < 10; ++i) {
        samples.push_back({static_cast<std::int64_t>(i), static_cast<float>(i)});
    }

    const auto get_ts = [](const void* p) -> std::int64_t {
        return static_cast<const sample_t*>(p)->t;
    };

    // The original "3.5" probe has no integer-only equivalent; use 4 as the
    // first timestamp >= 3.5, which is what lower_bound is meant to find.
    TEST_ASSERT(
        plot::detail::lower_bound_timestamp(
            samples.data(), samples.size(), sizeof(sample_t), get_ts, std::int64_t{4}) == 4,
        "lower_bound(4) should land on index 4");
    TEST_ASSERT(
        plot::detail::lower_bound_timestamp(
            samples.data(), samples.size(), sizeof(sample_t), get_ts, std::int64_t{3}) == 3,
        "lower_bound(3) should land on index 3");
    TEST_ASSERT(
        plot::detail::upper_bound_timestamp(
            samples.data(), samples.size(), sizeof(sample_t), get_ts, std::int64_t{3}) == 4,
        "upper_bound(3) should land on index 4");
    TEST_ASSERT(
        plot::detail::lower_bound_timestamp(
            samples.data(), samples.size(), sizeof(sample_t), get_ts, std::int64_t{-5}) == 0,
        "lower_bound below range should be 0");
    TEST_ASSERT(
        plot::detail::lower_bound_timestamp(samples.data(), samples.size(), sizeof(sample_t), get_ts, std::int64_t{100})
            == samples.size(),
        "lower_bound above range should be count");
    return true;
}

bool test_timestamp_search_rejects_null_samples()
{
    std::vector<sample_t> samples;
    for (int i = 0; i < 4; ++i) {
        samples.push_back({static_cast<std::int64_t>(i), static_cast<float>(i)});
    }

    const auto get_ts = [](const void* p) -> std::int64_t {
        return static_cast<const sample_t*>(p)->t;
    };
    const auto result = plot::detail::bsearch_ts_impl(
        samples.size(),
        [&samples](std::size_t i) -> const void* {
            return i == 2 ? nullptr : static_cast<const void*>(&samples[i]);
        },
        get_ts,
        [](std::int64_t ts, std::int64_t t) { return ts < t; },
        std::int64_t{3});

    TEST_ASSERT(!result,
        "timestamp binary search should fail explicitly when addressing returns nullptr");

    return true;
}

bool test_raw_timestamp_search_rejects_zero_stride()
{
    std::vector<sample_t> samples = {
        { 0, 0.0f},
        { 1, 1.0f},
        { 2, 2.0f}
    };

    int get_timestamp_calls = 0;
    const auto get_ts = [&get_timestamp_calls](const void* p) -> std::int64_t {
        ++get_timestamp_calls;
        return static_cast<const sample_t*>(p)->t;
    };

    TEST_ASSERT(plot::detail::lower_bound_timestamp(
        samples.data(),
        samples.size(),
        0,
        get_ts,
        std::int64_t{1}) == 0,
        "raw lower_bound should reject stride-zero input");
    TEST_ASSERT(plot::detail::upper_bound_timestamp(
        samples.data(),
        samples.size(),
        0,
        get_ts,
        std::int64_t{1}) == 0,
        "raw upper_bound should reject stride-zero input");
    TEST_ASSERT(get_timestamp_calls == 0,
        "stride-zero search should not call the timestamp accessor");

    return true;
}

bool test_bounds_on_segmented_snapshot()
{
    std::vector<sample_t> tail;
    std::vector<sample_t> head;
    for (int i = 0; i < 6; ++i) {
        tail.push_back({static_cast<std::int64_t>(i), 0.0f});
    }
    for (int i = 6; i < 10; ++i) {
        head.push_back({static_cast<std::int64_t>(i), 0.0f});
    }

    plot::data_snapshot_t snap;
    snap.data   = tail.data();
    snap.count  = 10;
    snap.stride = sizeof(sample_t);
    snap.data2  = head.data();
    snap.count2 = head.size();

    const auto get_ts = [](const void* p) -> std::int64_t {
        return static_cast<const sample_t*>(p)->t;
    };

    TEST_ASSERT(get_ts(snap.at(0)) == 0, "segmented at(0) should be 0");
    TEST_ASSERT(get_ts(snap.at(5)) == 5, "segmented at(5) should be 5");
    TEST_ASSERT(get_ts(snap.at(6)) == 6, "segmented at(6) should cross into second segment");
    TEST_ASSERT(get_ts(snap.at(9)) == 9, "segmented at(9) should be 9");
    // Original "6.5" probe equivalent: first index with timestamp >= 7 is 7.
    TEST_ASSERT(plot::detail::lower_bound_timestamp(snap, get_ts, std::int64_t{7}) == 7,
        "segmented lower_bound(7) should land on index 7");
    TEST_ASSERT(plot::detail::upper_bound_timestamp(snap, get_ts, std::int64_t{6}) == 7,
        "segmented upper_bound(6) should land on index 7");
    return true;
}

bool test_select_visible_sample_window_monotonic_extends_bounds()
{
    std::vector<sample_t> samples;
    for (int i = 0; i < 10; ++i) {
        samples.push_back({static_cast<std::int64_t>(i), static_cast<float>(i)});
    }

    plot::data_snapshot_t snap;
    snap.data   = samples.data();
    snap.count  = samples.size();
    snap.stride = sizeof(sample_t);

    const auto get_ts = [](const void* p) -> std::int64_t {
        return static_cast<const sample_t*>(p)->t;
    };

    const auto window = plot::detail::select_visible_sample_window(
        snap,
        get_ts,
        std::int64_t{3},
        std::int64_t{5},
        true);
    TEST_ASSERT(window.valid, "monotonic visible window should be valid");
    TEST_ASSERT(window.first == 2 && window.last_exclusive == 8,
        "monotonic visible window should include one preceding and two following samples");

    const auto trailing_window = plot::detail::select_visible_sample_window(
        snap,
        get_ts,
        std::int64_t{100},
        std::int64_t{110},
        true);
    TEST_ASSERT(trailing_window.valid, "trailing monotonic visible window should be valid");
    TEST_ASSERT(trailing_window.first == 9 && trailing_window.last_exclusive == 10,
        "trailing monotonic window should preserve the planner's preceding-sample behavior");

    return true;
}

bool test_select_visible_sample_window_non_monotonic_scans()
{
    std::vector<sample_t> samples = {
        { 10, 0.0f},
        { 4,  0.0f},
        { 20, 0.0f},
        { 6,  0.0f},
        { 2,  0.0f}
    };

    plot::data_snapshot_t snap;
    snap.data   = samples.data();
    snap.count  = samples.size();
    snap.stride = sizeof(sample_t);

    const auto get_ts = [](const void* p) -> std::int64_t {
        return static_cast<const sample_t*>(p)->t;
    };

    const auto window = plot::detail::select_visible_sample_window(
        snap,
        get_ts,
        std::int64_t{4},
        std::int64_t{6},
        false);
    TEST_ASSERT(window.valid, "non-monotonic visible window should be valid");
    TEST_ASSERT(window.first == 0 && window.last_exclusive == samples.size(),
        "non-monotonic visible window should linearly include surrounding samples");

    const auto empty_window = plot::detail::select_visible_sample_window(
        snap,
        get_ts,
        std::int64_t{30},
        std::int64_t{40},
        false);
    TEST_ASSERT(empty_window.valid, "empty non-monotonic visible window should be valid");
    TEST_ASSERT(empty_window.first == samples.size() && empty_window.last_exclusive == samples.size(),
        "non-monotonic no-match window should stay empty");

    return true;
}

bool test_aggregate_visible_sample_range_includes_step_after_hold()
{
    std::vector<sample_t> samples = {
        { 0, 2.0f },
        {15, 8.0f}
    };

    plot::data_snapshot_t snap;
    snap.data   = samples.data();
    snap.count  = samples.size();
    snap.stride = sizeof(sample_t);

    const auto get_ts = [](const void* p) -> std::int64_t {
        return static_cast<const sample_t*>(p)->t;
    };
    const auto get_range = [](const void* p) -> std::optional<std::pair<double, double>> {
        const double value = static_cast<const sample_t*>(p)->v;
        return std::make_pair(value, value);
    };

    const auto aggregate = plot::detail::aggregate_visible_sample_range(
        snap,
        get_ts,
        get_range,
        std::int64_t{10},
        std::int64_t{20},
        plot::Series_interpolation::STEP_AFTER,
        plot::Empty_window_behavior::DRAW_NOTHING);

    TEST_ASSERT(aggregate.valid, "step-after visible aggregate should be valid");
    TEST_ASSERT(aggregate.vmin == 2.0 && aggregate.vmax == 8.0,
        "step-after visible aggregate should include the held and in-window values");
    TEST_ASSERT(aggregate.tmin_ns == 10 && aggregate.tmax_ns == 15,
        "held sample should be aggregated at the window start timestamp");

    return true;
}

bool test_aggregate_visible_sample_range_rejects_nonfinite_range_endpoints()
{
    std::vector<sample_t> samples = {
        {10, 1.0f}
    };

    plot::data_snapshot_t snap;
    snap.data   = samples.data();
    snap.count  = samples.size();
    snap.stride = sizeof(sample_t);

    const auto get_ts = [](const void* p) -> std::int64_t {
        return static_cast<const sample_t*>(p)->t;
    };
    const auto get_range = [](const void*) -> std::optional<std::pair<double, double>> {
        return std::make_pair(
            2.0,
            std::numeric_limits<double>::quiet_NaN());
    };

    const auto aggregate = plot::detail::aggregate_visible_sample_range(
        snap,
        get_ts,
        get_range,
        std::int64_t{0},
        std::int64_t{20},
        plot::Series_interpolation::LINEAR,
        plot::Empty_window_behavior::DRAW_NOTHING);

    TEST_ASSERT(!aggregate.valid,
        "visible aggregate must reject nonfinite range endpoints before normalization");

    return true;
}

bool test_snapshot_truthiness_requires_usable_stride()
{
    std::vector<sample_t> samples = {{0, 1.0f}};

    plot::data_snapshot_t snap;
    snap.data   = samples.data();
    snap.count  = samples.size();
    snap.stride = 0;

    TEST_ASSERT(!snap.is_valid(),         "stride-zero snapshot should be invalid");
    TEST_ASSERT(!static_cast<bool>(snap), "stride-zero snapshot should be falsey");

    plot::snapshot_result_t result{
        snap,
        plot::snapshot_result_t::Snapshot_status::READY
    };
    TEST_ASSERT(!static_cast<bool>(result),
        "READY snapshot_result_t should be falsey when snapshot is unusable");

    return true;
}

bool test_snapshot_count1_clamps_malformed_count2()
{
    std::vector<sample_t> first  = {{0, 1.0f}};
    std::vector<sample_t> second = {{1, 2.0f}};

    plot::data_snapshot_t snap;
    snap.data   = first.data();
    snap.count  = first.size();
    snap.stride = sizeof(sample_t);
    snap.data2  = second.data();
    snap.count2 = 2;

    TEST_ASSERT(snap.count1() == 0,
        "malformed count2 > count should clamp count1 to zero");
    TEST_ASSERT(!snap.is_valid(),         "count2 > count should be invalid");
    TEST_ASSERT(!static_cast<bool>(snap), "count2 > count should be falsey");
    TEST_ASSERT(snap.at(0) == nullptr,
        "count2 > count should make sample access fail explicitly");

    return true;
}

bool test_layout_cache_key_distinguishes_adjacent_int64_time_windows()
{
    // The horizontal-axis label cache keys layouts by t0/t1 in nanoseconds.
    // Two windows that differ by a single nanosecond must be treated as
    // distinct keys, otherwise the layout cache returns stale axis labels
    // when the user pans or zooms by a sub-step amount. Before the int64
    // migration, t0/t1 lived as doubles; adjacent ns-resolution windows
    // collided in the cache because doubles cannot hold modern absolute
    // timestamps with single-nanosecond precision.

    plot::layout_cache_key_t base{};
    base.v0                           = 0.0f;
    base.v1                           = 1.0f;
    base.t0                           = 1'700'000'000'000'000'000LL;      // ~late 2023 in ns since epoch
    base.t1                           = base.t0 + 60LL * 1'000'000'000LL; // 60-second window
    base.viewport_size                = plot::Size_2i{1024, 768};
    base.adjusted_reserved_height     = 24.0;
    base.adjusted_preview_height      = 32.0;
    base.adjusted_font_size_in_pixels = 13.5;
    base.vbar_width_pixels            = 56.0;
    base.font_metrics_key             = 0xfeedface;

    // Adjacent t0 windows (differ by 1 ns) must compare unequal.
    plot::layout_cache_key_t shifted_t0 = base;
    shifted_t0.t0 = base.t0 + 1;
    TEST_ASSERT(!(base == shifted_t0),
        "layout_cache_key_t must distinguish t0 windows that differ by 1 ns");

    // Adjacent t1 windows (differ by 1 ns) must compare unequal.
    plot::layout_cache_key_t shifted_t1 = base;
    shifted_t1.t1 = base.t1 + 1;
    TEST_ASSERT(!(base == shifted_t1),
        "layout_cache_key_t must distinguish t1 windows that differ by 1 ns");

    // Identical keys still equal (sanity).
    plot::layout_cache_key_t copy = base;
    TEST_ASSERT(base == copy,
        "layout_cache_key_t equality must hold for identical fields");

    // INT64_MAX-adjacent windows: the boundary case where double-typed
    // fields would lose precision entirely.
    plot::layout_cache_key_t high_a = base;
    plot::layout_cache_key_t high_b = base;
    high_a.t0                       = std::numeric_limits<std::int64_t>::max() - 1;
    high_a.t1                       = std::numeric_limits<std::int64_t>::max();
    high_b.t0                       = std::numeric_limits<std::int64_t>::max() - 2;
    high_b.t1                       = std::numeric_limits<std::int64_t>::max() - 1;
    TEST_ASSERT(!(high_a == high_b),
        "layout_cache_key_t must distinguish adjacent ns windows even "
        "near INT64_MAX where double fields would lose precision");

    return true;
}

bool test_format_axis_fixed_or_int()
{
    TEST_ASSERT(plot::format_axis_fixed_or_int(0.0, 0) == "0", "zero as integer");
    TEST_ASSERT(plot::format_axis_fixed_or_int(3.0, 0) == "3", "integer rounding");
    TEST_ASSERT(plot::format_axis_fixed_or_int(-3.49999, 0) == "-3",
        "negative rounding towards zero at 0 digits");
    TEST_ASSERT(plot::format_axis_fixed_or_int(1.234, 2) == "1.23", "fixed 2 digits");
    TEST_ASSERT(plot::format_axis_fixed_or_int(-0.0001, 2) == "0.00",
        "near-zero negative value should format as 0.00");
    TEST_ASSERT(plot::format_axis_fixed_or_int(std::numeric_limits<double>::max(), 1) == "0",
        "finite values that overflow fixed rounding should use the invalid-value label");
    return true;
}

bool test_default_timestamp_precision_follows_step()
{
    constexpr std::int64_t k_second = plot::k_ns_per_second;

    TEST_ASSERT(plot::default_format_timestamp(0, k_second) == "00:00:00",
        "whole-second ticks should not include a fractional part");
    TEST_ASSERT(plot::default_format_timestamp(200'000'000, 200'000'000) == "00:00:00.2",
        "200 ms ticks should use tenths");
    TEST_ASSERT(plot::default_format_timestamp(20'000'000, 20'000'000) == "00:00:00.02",
        "20 ms ticks should use hundredths");
    TEST_ASSERT(plot::default_format_timestamp(1, 1) == "00:00:00.000000001",
        "1 ns ticks should retain nanosecond precision");
    TEST_ASSERT(plot::default_format_timestamp(-200'000'000, 200'000'000) == "23:59:59.8",
        "negative timestamps should use the preceding second's positive fraction");

    const std::string minimum =
        plot::default_format_timestamp(std::numeric_limits<std::int64_t>::min(), 1);
    TEST_ASSERT(
        minimum == "00:12:43.145224192" ||
            minimum == "-9223372036.854775808s",
        "the minimum timestamp should be exact on calendar and fallback paths");

    return true;
}

bool test_default_elapsed_time_distinguishes_day_boundaries()
{
    constexpr std::int64_t k_minute = 60 * plot::k_ns_per_second;
    constexpr std::int64_t k_day    = 24 * 60 * k_minute;

    TEST_ASSERT(plot::default_format_elapsed_time(0, k_minute) == "00:00",
        "zero elapsed time should retain a compact clock label");
    TEST_ASSERT(plot::default_format_elapsed_time(k_day - k_minute, k_minute) == "23:59",
        "the final minute before a day boundary should retain its hour");
    TEST_ASSERT(plot::default_format_elapsed_time(k_day, k_minute) == "1d 00:00",
        "one elapsed day should not wrap to the zero label");
    TEST_ASSERT(plot::default_format_elapsed_time(3 * k_day + 6 * 60 * k_minute, k_minute) ==
        "3d 06:00",
        "multi-day labels should include the elapsed day count and time");
    TEST_ASSERT(plot::default_format_elapsed_time(3 * k_day + 200'000'000, 200'000'000) ==
        "3d 00:00:00.2",
        "subsecond elapsed labels should remain distinguishable");
    TEST_ASSERT(plot::default_format_elapsed_time(
        std::numeric_limits<std::int64_t>::min(), k_minute) == "-106751d 23:47",
        "the minimum elapsed timestamp should format without overflow");
    TEST_ASSERT(plot::default_format_elapsed_time(
        std::numeric_limits<std::int64_t>::max(), k_minute) == "106751d 23:47",
        "the maximum elapsed timestamp should format without overflow");

    return true;
}

bool test_duration_format_preserves_full_nanosecond_domain()
{
    constexpr auto k_ms = plot::k_ns_per_ms;
    TEST_ASSERT(plot::default_format_duration(0, k_ms) == "00:00:00.000",
        "zero report duration retains millisecond precision");
    TEST_ASSERT(plot::default_format_duration(86'400'123'999'999ULL, k_ms) == "1d 00:00:00.123",
        "duration formatting retains days and truncates fractional milliseconds");
    TEST_ASSERT(plot::default_format_elapsed_time(-999'999, k_ms) == "-00:00:00.000",
        "submillisecond elapsed timestamps retain their sign");
    TEST_ASSERT(plot::default_format_elapsed_time(
        std::numeric_limits<std::int64_t>::min(), k_ms) == "-106751d 23:47:16.854",
        "minimum signed elapsed time must not overflow its magnitude");
    TEST_ASSERT(plot::default_format_duration(
        std::numeric_limits<std::uint64_t>::max(), k_ms) == "213503d 23:34:33.709",
        "the duration between signed endpoint extremes must not saturate");
    TEST_ASSERT(plot::default_format_duration(
        std::numeric_limits<std::uint64_t>::max(), 1) == "213503d 23:34:33.709551615",
        "full unsigned duration retains all requested nanosecond digits");
    return true;
}

bool test_wheel_delta_selection_preserves_units_and_reliability()
{
    namespace zoom = plot::zoom_inertia;
    struct wheel_case_t
    {
        double angle_x;
        double angle_y;
        double pixel_x;
        double pixel_y;
        bool   reliable_pixels;
        double expected_value;
        bool   expected_pixels;
    };
    const wheel_case_t cases[] = {
        {  0, 120,  0, -30, true,  -30, true },
        {  0, 120,  0, -30, false, 120, false},
        {  0, 120, 15,   0, true,   15, true },
        {120,   0,  0,   0, true,  120, false},
        {120, -60,  0,   0, false, -60, false},
        {  0,   0,  0,  30, false,   0, false},
        {  0,   0,  0,   0, true,    0, false},
    };
    for (const auto& sample : cases) {
        const auto delta = zoom::select_wheel_delta(
            sample.angle_x, sample.angle_y, sample.pixel_x, sample.pixel_y,
            sample.reliable_pixels);
        TEST_ASSERT(delta.value == sample.expected_value && delta.pixels == sample.expected_pixels,
            "wheel deltas choose reliable pixels before angles and retain scrolling units");
    }
    TEST_ASSERT(zoom::wheel_impulse({120, false}) == -1.0,
        "one wheel notch retains its signed zoom impulse");
    TEST_ASSERT(zoom::wheel_impulse({120, true}) == -1.0,
        "a 120-pixel stroke has the same impulse as one wheel notch");
    return true;
}

bool test_horizontal_label_fades_do_not_restore_duplicate_text()
{
    struct label_t
    {
        std::int64_t   value = 0;
        std::string    text;
    };

    using clock_t = std::chrono::steady_clock;
    for (const bool left_to_right : {true, false}) {
        for (const auto [old_key, new_key] : {
            std::pair<std::int64_t, std::int64_t>{1, 2},
            std::pair<std::int64_t, std::int64_t>{2, 1}})
        {
            plot::Text_renderer::horizontal_axis_fade_tracker_t tracker;
            std::vector<label_t> labels{{old_key, "same"}};
            std::map<std::int64_t, float> drawn;
            auto now = clock_t::time_point{} + std::chrono::seconds(1);

            const auto update = [&] {
                drawn.clear();
                return plot::detail::update_and_draw_faded_labels(
                    labels,
                    tracker,
                    180.0f,
                    true,
                    now,
                    [](const label_t& label) { return label.value; },
                    [&](std::int64_t key, const std::string&, float alpha) {
                        drawn[key] += alpha;
                    },
                    true,
                    left_to_right);
            };

            TEST_ASSERT(!update() && drawn.count(old_key) == 1,
                "the initial horizontal label should render normally");

            labels = {{new_key, "same"}};
            update();
            now += std::chrono::milliseconds(179);
            TEST_ASSERT(update(), "a moving label should remain active near the fade boundary");

            const std::int64_t expected_key = left_to_right
                ? std::min(old_key, new_key)
                : std::max(old_key, new_key);
            TEST_ASSERT(
                drawn.size() == 1 &&
                    drawn.count(expected_key) == 1 &&
                    drawn.at(expected_key) >= 0.99f &&
                    tracker.visible_alphas.size() == 1 &&
                    tracker.visible_alphas.count(expected_key) == 1 &&
                    tracker.visible_alphas.at(expected_key) >= 0.99f,
                "the axis-first moving label and extension should retain the stronger opacity");

            now += std::chrono::milliseconds(1);
            TEST_ASSERT(!update() &&
                drawn.size() == 1 &&
                drawn.count(new_key) == 1 &&
                tracker.states.size() == 1 &&
                tracker.states.count(new_key) == 1,
                "the fade should converge to the current label position");
        }
    }

    return true;
}

bool test_horizontal_label_crossfades_suppress_rendered_duplicate_text()
{
    struct label_t
    {
        std::int64_t   value = 0;
        std::string    text;
    };
    struct draw_t
    {
        std::int64_t   value = 0;
        std::string    text;
        float          alpha = 0.0f;
    };

    using clock_t = std::chrono::steady_clock;
    for (const bool left_to_right : {true, false}) {
        plot::Text_renderer::horizontal_axis_fade_tracker_t tracker;
        std::vector<label_t> labels{{1, "A"}, {2, "B"}};
        std::vector<draw_t> drawn;
        auto now = clock_t::time_point{} + std::chrono::seconds(1);

        const auto update = [&] {
            drawn.clear();
            return plot::detail::update_and_draw_faded_labels(
                labels,
                tracker,
                180.0f,
                true,
                now,
                [](const label_t& label) { return label.value; },
                [&](std::int64_t key, const std::string& text, float alpha) {
                    drawn.push_back({key, text, alpha});
                },
                true,
                left_to_right);
        };

        update();
        labels = {{1, "B"}, {2, "C"}};
        update();
        now += std::chrono::milliseconds(90);
        TEST_ASSERT(update(), "the midpoint should remain an active text crossfade");

        const auto first_b = std::find_if(
            drawn.begin(),
            drawn.end(),
            [](const draw_t& draw) { return draw.text == "B"; });
        TEST_ASSERT(first_b != drawn.end(),
            "the shared crossfade text should remain visible");
        TEST_ASSERT(std::count_if(
            drawn.begin(),
            drawn.end(),
            [](const draw_t& draw) { return draw.text == "B"; }) == 1,
            "shared text emitted by adjacent crossfade states should render once");
        TEST_ASSERT(first_b->value == 1,
            "a final string should take precedence over an adjacent transitional copy");
        TEST_ASSERT(std::abs(first_b->alpha - 0.5f) < 1e-6f,
            "duplicate suppression should preserve the text crossfade opacity budget");

        tracker = {};
        labels  = {{1, "A"}, {2, "B"}};
        now     = clock_t::time_point{} + std::chrono::seconds(2);
        update();
        labels = {{1, "B"}, {2, "A"}};
        update();
        now += std::chrono::milliseconds(90);
        TEST_ASSERT(update(), "swapped text should remain an active midpoint crossfade");

        const auto drawn_a = std::find_if(
            drawn.begin(),
            drawn.end(),
            [](const draw_t& draw) { return draw.text == "A"; });
        const auto drawn_b = std::find_if(
            drawn.begin(),
            drawn.end(),
            [](const draw_t& draw) { return draw.text == "B"; });
        TEST_ASSERT(
            drawn.size() == 2 &&
                drawn_a != drawn.end() &&
                drawn_a->value == 2 &&
                std::abs(drawn_a->alpha - 0.5f) < 1e-6f &&
                drawn_b != drawn.end() &&
                drawn_b->value == 1 &&
                std::abs(drawn_b->alpha - 0.5f) < 1e-6f &&
                tracker.visible_alphas.size() == 2 &&
                std::abs(tracker.visible_alphas.at(1) - 0.5f) < 1e-6f &&
                std::abs(tracker.visible_alphas.at(2) - 0.5f) < 1e-6f,
            "swapped text should keep final copies, crossfade opacity, and matching extensions");
    }

    for (const bool left_to_right : {true, false}) {
        plot::Text_renderer::horizontal_axis_fade_tracker_t tracker;
        const auto add_state = [&](std::int64_t key, const std::string& text) {
            auto& state    = tracker.states[key];
            state.alpha    = 1.0f;
            state.text     = text;
            state.text_mix = 1.0f;
        };
        add_state(1, "A");
        add_state(2, "B");
        add_state(3, "A");
        tracker.states.at(2).previous_text = "A";
        tracker.states.at(2).text_mix      = 0.5f;
        tracker.initialized                = true;
        tracker.last_update =
            clock_t::time_point{} + std::chrono::seconds(3);

        const std::vector<label_t> labels{{1, "A"}, {2, "B"}, {3, "A"}};
        std::vector<draw_t> drawn;
        plot::detail::update_and_draw_faded_labels(
            labels,
            tracker,
            180.0f,
            true,
            tracker.last_update,
            [](const label_t& label) { return label.value; },
            [&](std::int64_t key, const std::string& text, float alpha) {
                drawn.push_back({key, text, alpha});
            },
            true,
            left_to_right);

        TEST_ASSERT(std::count_if(
            drawn.begin(),
            drawn.end(),
            [](const draw_t& draw) { return draw.text == "A"; }) == 2,
            "transitional text must not bridge nonconsecutive final equal strings");
        TEST_ASSERT(tracker.visible_alphas.size() == 3,
            "nonconsecutive final labels should retain both pane extensions");
    }

    return true;
}

bool test_time_unit_helpers_handle_edges()
{
    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

    TEST_ASSERT(plot::ns_to_ms_for_qml(-1) == -1,
        "negative ns-to-ms conversion should floor");
    TEST_ASSERT(
        plot::floor_div_int64(k_min, -1) == k_max,
        "INT64_MIN / -1 floor division should saturate instead of overflowing");
    TEST_ASSERT(
        plot::ms_for_qml_to_ns(k_max) == k_max,
        "positive ms-to-ns overflow should saturate");
    TEST_ASSERT(
        plot::ms_for_qml_to_ns(k_min) == k_min,
        "negative ms-to-ns overflow should saturate");

    TEST_ASSERT(!plot::checked_add_ns(k_max, 1),
        "checked add should reject positive int64 overflow");
    TEST_ASSERT(!plot::checked_sub_ns(k_min, 1),
        "checked subtract should reject negative int64 overflow");
    TEST_ASSERT(plot::saturating_add_ns(k_max, 1) == k_max,
        "saturating add should clamp positive int64 overflow");
    TEST_ASSERT(plot::saturating_sub_ns(k_min, 1) == k_min,
        "saturating subtract should clamp negative int64 overflow");

    const auto full_range_span = plot::positive_span_ns(k_min, k_max);
    TEST_ASSERT(full_range_span && *full_range_span == std::numeric_limits<std::uint64_t>::max(),
        "positive_span_ns should represent the full int64 range without signed overflow");
    TEST_ASSERT(plot::span_ns_as_long_double(k_min, k_max) > static_cast<long double>(k_max),
        "long-double span conversion should handle spans larger than INT64_MAX");
    TEST_ASSERT(plot::midpoint_ns(k_min, k_max) == -1,
        "midpoint should avoid overflow for full int64 range");

    const plot::time_range_t full_centered = plot::centered_time_range_ns(
        -1,
        std::numeric_limits<std::uint64_t>::max());
    TEST_ASSERT(full_centered.min_ns == k_min && full_centered.max_ns == k_max,
        "centered full-range construction should preserve both int64 endpoints");

    const plot::time_range_t centered_at_max = plot::centered_time_range_ns(k_max, 100);
    TEST_ASSERT(centered_at_max.min_ns == k_max - 100 && centered_at_max.max_ns == k_max,
        "centered range at INT64_MAX should edge-clamp while preserving span");

    const plot::time_range_t centered_at_min = plot::centered_time_range_ns(k_min, 100);
    TEST_ASSERT(centered_at_min.min_ns == k_min && centered_at_min.max_ns == k_min + 100,
        "centered range at INT64_MIN should edge-clamp while preserving span");

    const auto full_right_edge = plot::time_at_fraction_ns(
        plot::time_range_t{k_min, k_max},
        1.0L);
    TEST_ASSERT(full_right_edge && *full_right_edge == k_max,
        "fractional mapping across the full int64 range should reach INT64_MAX");

    const auto shifted_right = plot::translate_time_range_ns(
        plot::time_range_t{k_max - 20, k_max - 10},
        50);
    TEST_ASSERT(shifted_right
        && shifted_right->min_ns == k_max - 10 &&
            shifted_right->max_ns == k_max,
        "positive translation at INT64_MAX should clamp while preserving span");

    const auto shifted_left = plot::translate_time_range_ns(
        plot::time_range_t{k_min + 10, k_min + 20},
        -50);
    TEST_ASSERT(shifted_left
        && shifted_left->min_ns == k_min &&
            shifted_left->max_ns == k_min + 10,
        "negative translation at INT64_MIN should clamp while preserving span");

    const auto clamped_to_edge = plot::clamp_time_range_to_available_ns(
        plot::time_range_t{k_max - 12, k_max - 2},
        plot::time_range_t{k_min + 4, k_max - 5});
    TEST_ASSERT(clamped_to_edge
        && clamped_to_edge->min_ns == k_max - 15 &&
            clamped_to_edge->max_ns == k_max - 5,
        "availability clamp should preserve target span near INT64_MAX");

    const auto clamped_full_target = plot::clamp_time_range_to_available_ns(
        plot::time_range_t{k_min, k_max},
        plot::time_range_t{-500, 500});
    TEST_ASSERT(clamped_full_target
        && clamped_full_target->min_ns == -500 &&
            clamped_full_target->max_ns == 500,
        "target wider than availability should adopt the available range");

    const std::string formatted = plot::default_format_timestamp(k_min, plot::k_ns_per_second);
    TEST_ASSERT(!formatted.empty(),
        "default timestamp formatter should handle INT64_MIN without overflow");

    return true;
}

bool test_timestamp_positions_preserve_epoch_nanoseconds()
{
    constexpr std::int64_t k_epoch          = 1'750'000'000'000'000'000LL;
    constexpr std::int64_t k_negative_epoch = -k_epoch;
    constexpr std::int64_t k_min            = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max            = std::numeric_limits<std::int64_t>::max();

    TEST_ASSERT(plot::span_ns_as_long_double(k_epoch, k_epoch + 1) == 1.0L,
        "relative span math should preserve adjacent current-epoch nanoseconds");
    TEST_ASSERT(plot::span_ns_as_long_double(k_negative_epoch, k_negative_epoch + 1) == 1.0L,
        "relative span math should preserve adjacent negative-epoch nanoseconds");
    TEST_ASSERT(plot::span_ns_as_long_double(k_epoch + 127, k_epoch) == -127.0L,
        "relative span math should preserve reversed sub-256ns intervals");
    TEST_ASSERT(plot::detail::normalized_time_position_ns(
        k_epoch, k_epoch + 127, k_epoch + 254) == 0.5,
        "normalized position should interpolate inside a sub-256ns epoch interval");
    TEST_ASSERT(plot::detail::normalized_time_position_ns(
        k_negative_epoch, k_negative_epoch + 127, k_negative_epoch + 254) == 0.5,
        "normalized position should interpolate inside a negative epoch interval");
    TEST_ASSERT(std::abs(plot::detail::normalized_time_position_ns(k_min, -1, k_max) - 0.5) < 1e-15,
        "normalized position should avoid overflow across the full int64 range");

    const std::vector<sample_t> samples{
        { k_epoch,       0.0f },
        { k_epoch + 127, 1.0f },
        { k_epoch + 127, 2.0f },
        { k_epoch + 254, 3.0f },
    };
    const plot::data_snapshot_t snapshot{
        samples.data(), samples.size(), sizeof(sample_t), 1};
    const auto bracket = plot::detail::bracket_timestamp(
        snapshot,
        [](const void* sample) {
            return static_cast<const sample_t*>(sample)->t;
        },
        k_epoch + 127);
    TEST_ASSERT(bracket && bracket.i0 == 2 && bracket.i1 == 3,
        "exact integer bracketing should retain the last duplicate timestamp");

    return true;
}

bool test_horizontal_label_fade_keys_preserve_int64_timestamps()
{
    constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

    plot::Text_renderer::horizontal_axis_fade_tracker_t tracker;
    tracker.states.emplace(k_min,     plot::Text_renderer::label_fade_state_t{});
    tracker.states.emplace(k_min + 1, plot::Text_renderer::label_fade_state_t{});
    tracker.states.emplace(k_max - 1, plot::Text_renderer::label_fade_state_t{});
    tracker.states.emplace(k_max,     plot::Text_renderer::label_fade_state_t{});

    TEST_ASSERT(tracker.states.size() == 4,
        "horizontal fade tracker should distinguish adjacent extreme int64 timestamps");

    const auto full_span = plot::positive_span_ns_as_long_double(k_min, k_max);
    TEST_ASSERT(full_span && std::isfinite(*full_span),
        "horizontal label span math should represent the full int64 range");

    const long double center_delta    = plot::span_ns_as_long_double(k_min, -1);
    const long double right_delta     = plot::span_ns_as_long_double(k_min, k_max);
    const long double center_fraction = center_delta / *full_span;
    const long double right_fraction  = right_delta / *full_span;

    TEST_ASSERT(center_fraction > 0.499999999999999L
        && center_fraction < 0.500000000000001L,
        "full-range horizontal label math should keep the midpoint near 0.5");
    TEST_ASSERT(right_fraction == 1.0L,
        "full-range horizontal label math should reach the right edge exactly");

    return true;
}

bool test_label_fades_start_at_threshold_and_finish_after_zoom_stops()
{
    struct label_t
    {
        std::int64_t   value = 0;
        std::string    text;
    };

    using clock_t = std::chrono::steady_clock;
    plot::Text_renderer::horizontal_axis_fade_tracker_t tracker;
    std::vector<label_t> labels{{1, "one"}};
    std::map<std::int64_t, float> drawn;
    std::map<std::string, float> drawn_text;
    auto now = clock_t::time_point{} + std::chrono::seconds(1);

    const auto update = [&](bool animate_changes) {
        drawn.clear();
        drawn_text.clear();
        return plot::detail::update_and_draw_faded_labels(
            labels,
            tracker,
            180.0f,
            animate_changes,
            now,
            [](const label_t& label) { return label.value; },
            [&](std::int64_t key, const std::string& text, float alpha) {
                drawn[key] += alpha;
                drawn_text[text] = alpha;
            });
    };

    TEST_ASSERT(!update(true) && drawn[1] == 1.0f,
        "the first label set should render opaque without a startup fade");

    labels[0].text = "one.0";
    TEST_ASSERT(update(true) && drawn_text["one"] == 1.0f && drawn_text.count("one.0") == 0,
        "a formatting change should retain the old text on its threshold-crossing frame");
    now += std::chrono::milliseconds(90);
    TEST_ASSERT(update(false)
        && std::abs(drawn_text["one"] - 0.5f) < 1e-6f
        && std::abs(drawn_text["one.0"] - 0.5f) < 1e-6f,
        "old and new text should crossfade on the label fade clock");
    labels[0].text = "one";
    TEST_ASSERT(update(true)
        && std::abs(drawn_text["one"] - 0.5f) < 1e-6f
        && std::abs(drawn_text["one.0"] - 0.5f) < 1e-6f,
        "reversing a formatting change should not pop either text");
    now += std::chrono::milliseconds(90);
    TEST_ASSERT(!update(false) && drawn_text["one"] == 1.0f && drawn_text.count("one.0") == 0,
        "a reversed formatting change should settle on the original text");

    now += std::chrono::seconds(10);
    labels.push_back({2, "two"});
    TEST_ASSERT(update(true) && tracker.states.at(2).alpha == 0.0f && drawn.count(2) == 0,
        "a label crossing the zoom threshold should start at zero despite prior idle time");

    now += std::chrono::milliseconds(90);
    TEST_ASSERT(update(false) && std::abs(tracker.states.at(2).alpha - 0.5f) < 1e-6f,
        "an incoming label should keep fading after the zoom span stops changing");
    now += std::chrono::milliseconds(90);
    TEST_ASSERT(!update(false) && tracker.states.at(2).alpha == 1.0f,
        "an incoming label should settle fully opaque");

    now += std::chrono::seconds(10);
    labels.pop_back();
    TEST_ASSERT(update(true) && tracker.states.at(2).alpha == 1.0f && drawn.count(2) == 1,
        "an outgoing label should remain visible on its threshold-crossing frame");
    now += std::chrono::milliseconds(90);
    TEST_ASSERT(update(false) && std::abs(tracker.states.at(2).alpha - 0.5f) < 1e-6f,
        "an outgoing label should keep fading after the zoom span stops changing");

    labels.push_back({2, "two"});
    TEST_ASSERT(update(true) && tracker.states.at(2).direction == 1,
        "a zoom reversal should reverse the active fade without popping");
    now += std::chrono::milliseconds(90);
    TEST_ASSERT(!update(false) && tracker.states.at(2).alpha == 1.0f,
        "a reversed fade should settle fully opaque");

    labels.pop_back();
    TEST_ASSERT(update(true), "the final fade-out should start");
    now += std::chrono::milliseconds(180);
    TEST_ASSERT(!update(false) && tracker.states.count(2) == 0 && drawn.count(2) == 0,
        "an outgoing label should be removed after the fade completes");

    now += std::chrono::seconds(10);
    labels.push_back({3, "three"});
    TEST_ASSERT(!update(false) && tracker.states.at(3).alpha == 1.0f,
        "non-zoom label-set changes should remain immediate");

    return true;
}

bool test_lcd_policy_helpers()
{
    using request_t  = plot::lcd_request_t;
    using resolved_t = plot::lcd_subpixel_order_t;
    using surface_t  = plot::detail::text_lcd_draw_surface_t;

    const request_t auto_request = plot::lcd_auto_request();
    const request_t none_request = plot::lcd_none_request();
    const request_t bgr_request  = plot::lcd_explicit_request(resolved_t::BGR);
    const request_t invalid_request{false, static_cast<resolved_t>(255)};

    TEST_ASSERT(auto_request.automatic, "LCD request should model AUTO as request state");
    TEST_ASSERT(auto_request.resolved_order == resolved_t::NONE,
        "AUTO request should not carry a draw order");
    TEST_ASSERT(!none_request.automatic, "NONE request should be explicit");
    TEST_ASSERT(none_request.resolved_order == resolved_t::NONE,
        "NONE request should carry the shared resolved NONE value");
    TEST_ASSERT(
        plot::lcd_explicit_request(static_cast<resolved_t>(255)).resolved_order ==
            resolved_t::NONE,
        "invalid explicit LCD request factory should fail closed to NONE");

    const resolved_t display_orders[] = {
        resolved_t::RGB,
        resolved_t::BGR,
        resolved_t::VRGB,
        resolved_t::VBGR,
    };
    for (resolved_t order : display_orders) {
        const request_t explicit_request = plot::lcd_explicit_request(order);
        TEST_ASSERT(!explicit_request.automatic, "display LCD request should be explicit");
        TEST_ASSERT(explicit_request.resolved_order == order,
            "display LCD request should preserve the shared resolved order");
        TEST_ASSERT(plot::detail::lcd_auto_order_from_detections(order, resolved_t::NONE) == order,
            "Qt LCD detection should preserve every display-specific order");
        TEST_ASSERT(plot::detail::lcd_auto_order_from_detections(resolved_t::NONE, order) == order,
            "OS LCD detection should preserve every display-specific order");
        TEST_ASSERT(plot::detail::lcd_effective_order(auto_request, order) == order,
            "AUTO should resolve to every display-specific detected order");
        TEST_ASSERT(plot::detail::text_lcd_draw_is_eligible(
            surface_t::VERTICAL_AXIS_LABEL, order, 1.0f, true),
            "vertical axis labels may be LCD-eligible for every display-specific order");
        TEST_ASSERT(plot::detail::text_lcd_draw_is_eligible(
            surface_t::HORIZONTAL_AXIS_LABEL, order, 1.0f, true),
            "horizontal axis labels may be LCD-eligible for every display-specific order");
    }

    TEST_ASSERT(plot::detail::lcd_auto_order_from_detections(resolved_t::RGB, resolved_t::BGR) == resolved_t::RGB,
        "Qt LCD detection should win over OS detection");
    TEST_ASSERT(plot::detail::lcd_auto_order_from_detections(resolved_t::NONE, resolved_t::BGR) == resolved_t::BGR,
        "OS LCD detection should be used when Qt has no order");
    TEST_ASSERT(plot::detail::lcd_auto_order_from_detections(
        resolved_t::NONE, resolved_t::NONE) == resolved_t::NONE,
        "AUTO detection should fail closed when no source has an order");

    TEST_ASSERT(plot::detail::lcd_effective_order(auto_request, resolved_t::RGB) == resolved_t::RGB,
        "AUTO should resolve to a display-specific detected order");
    TEST_ASSERT(plot::detail::lcd_effective_order(auto_request, resolved_t::NONE) == resolved_t::NONE,
        "AUTO should fail closed without platform detection");
    TEST_ASSERT(
        plot::detail::lcd_effective_order(
            plot::lcd_explicit_request(resolved_t::RGB),
            resolved_t::NONE) == resolved_t::RGB,
        "explicit RGB should not depend on platform detection");
    TEST_ASSERT(plot::detail::lcd_effective_order(none_request, resolved_t::RGB) == resolved_t::NONE,
        "explicit NONE should stay grayscale");
    TEST_ASSERT(plot::detail::lcd_effective_order(invalid_request, resolved_t::RGB) == resolved_t::NONE,
        "invalid requested frame order should fail closed");

    for (resolved_t order : display_orders) {
        TEST_ASSERT(
            plot::detail::lcd_effective_order(
                plot::lcd_explicit_request(order),
                resolved_t::NONE) == order,
            "explicit display LCD requests should produce a resolved draw order");
    }

    TEST_ASSERT(plot::detail::lcd_effective_order_for_frame(auto_request, resolved_t::RGB) == resolved_t::RGB,
        "AUTO should use platform order at frame level");
    TEST_ASSERT(plot::detail::lcd_effective_order_for_frame(auto_request, resolved_t::NONE) == resolved_t::NONE,
        "AUTO with no platform order should stay grayscale");
    TEST_ASSERT(plot::detail::lcd_effective_order_for_frame(none_request, resolved_t::RGB) == resolved_t::NONE,
        "explicit NONE should stay grayscale at frame level");
    TEST_ASSERT(
        plot::detail::lcd_effective_order_for_frame(
            &auto_request,
            resolved_t::NONE) == resolved_t::NONE,
        "direct RHI AUTO should fail closed without an AUTO-resolved frame order");
    TEST_ASSERT(
        plot::detail::lcd_effective_order_for_frame(
            &bgr_request,
            resolved_t::NONE) == resolved_t::BGR,
        "direct RHI explicit requests should resolve without an AUTO-resolved frame order");
    TEST_ASSERT(
        plot::detail::lcd_effective_order_for_frame(
            &bgr_request,
            resolved_t::RGB) == resolved_t::BGR,
        "direct RHI explicit requests should override AUTO-resolved frame order");
    TEST_ASSERT(
        plot::detail::lcd_effective_order_for_frame(
            nullptr,
            resolved_t::VRGB) == resolved_t::VRGB,
        "manual frame order should still work without config");

    TEST_ASSERT(!plot::detail::text_lcd_draw_is_eligible(
        surface_t::HORIZONTAL_AXIS_LABEL, resolved_t::RGB, 1.0f, false),
        "horizontal axis labels should not be LCD-eligible without backing");
    TEST_ASSERT(!plot::detail::text_lcd_draw_is_eligible(
        surface_t::VERTICAL_AXIS_LABEL, resolved_t::RGB, 1.0f, false),
        "vertical axis labels should not be LCD-eligible without backing");
    TEST_ASSERT(!plot::detail::text_lcd_draw_is_eligible(
        surface_t::VERTICAL_AXIS_LABEL, resolved_t::NONE, 1.0f, true),
        "axis labels should not be LCD-eligible for NONE frame order");
    TEST_ASSERT(!plot::detail::text_lcd_draw_is_eligible(
        surface_t::HORIZONTAL_AXIS_LABEL, resolved_t::NONE, 1.0f, true),
        "axis labels should not be LCD-eligible for NONE frame order");
    TEST_ASSERT(!plot::detail::text_lcd_draw_is_eligible(
        surface_t::VERTICAL_AXIS_LABEL, resolved_t::RGB, 0.998f, true),
        "axis labels should not be LCD-eligible with translucent backing");
    TEST_ASSERT(!plot::detail::text_lcd_draw_is_eligible(
        surface_t::VERTICAL_AXIS_LABEL, resolved_t::RGB,
        std::numeric_limits<float>::quiet_NaN(), true),
        "axis labels should not be LCD-eligible with unknown backing alpha");
    TEST_ASSERT(!plot::detail::text_lcd_draw_is_eligible(
        static_cast<surface_t>(255), resolved_t::RGB, 1.0f, true),
        "invalid draw surfaces should fail closed");

    return true;
}

bool test_built_in_label_backgrounds_are_opaque_for_lcd()
{
    const plot::Color_palette dark                    = plot::Color_palette::dark();
    const plot::Color_palette light                   = plot::Color_palette::light();
    const auto                expected_dark_body      = plot::hex_to_vec4("ff1b1c20");
    const auto                expected_light_body     = plot::hex_to_vec4("ffCDD2D5");
    const auto                expected_light_vertical = plot::hex_to_vec4("ffc1c6c9");

    TEST_ASSERT(dark.h_label_background.a >= 0.999f,
        "dark horizontal label background should be opaque for LCD backing");
    TEST_ASSERT(dark.v_label_background.a >= 0.999f,
        "dark vertical label background should be opaque for LCD backing");
    TEST_ASSERT(light.h_label_background.a >= 0.999f,
        "light horizontal label background should be opaque for LCD backing");
    TEST_ASSERT(light.v_label_background.a >= 0.999f,
        "light vertical label background should be opaque for LCD backing");
    TEST_ASSERT(dark.background == expected_dark_body,
        "dark plot-body background should remain unchanged");
    TEST_ASSERT(light.background == expected_light_body,
        "light plot-body background should use the approved opaque color");
    TEST_ASSERT(
        light.v_label_background.r == expected_light_vertical.r &&
        light.v_label_background.g == expected_light_vertical.g &&
        light.v_label_background.b == expected_light_vertical.b,
        "light vertical label background should preserve RGB while becoming opaque");

    return true;
}

bool test_grid_lcd_order_selection()
{
    using order_t = plot::lcd_subpixel_order_t;

    TEST_ASSERT(
        plot::detail::grid_lcd_subpixel_order(order_t::RGB, 1.0f, true) == order_t::RGB,
        "horizontal RGB should enable vertical-grid LCD correction");
    TEST_ASSERT(
        plot::detail::grid_lcd_subpixel_order(order_t::BGR, 1.0f, true) == order_t::BGR,
        "horizontal BGR should preserve reverse subpixel order");

    const order_t fallback_orders[] = {
        order_t::NONE,
        order_t::VRGB,
        order_t::VBGR,
        static_cast<order_t>(255),
    };
    for (order_t order : fallback_orders) {
        TEST_ASSERT(
            plot::detail::grid_lcd_subpixel_order(order, 1.0f, true) == order_t::NONE,
            "unknown or vertical subpixel order should keep ordinary grid rendering");
    }

    TEST_ASSERT(
        plot::detail::grid_lcd_subpixel_order(order_t::RGB, 0.998f, true) == order_t::NONE,
        "translucent plot bodies should keep ordinary grid rendering");
    TEST_ASSERT(
        plot::detail::grid_lcd_subpixel_order(
            order_t::RGB,
            std::numeric_limits<float>::quiet_NaN(),
            true) == order_t::NONE,
        "unknown plot-body opacity should keep ordinary grid rendering");
    TEST_ASSERT(
        plot::detail::grid_lcd_subpixel_order(order_t::RGB, 1.0f, false) == order_t::NONE,
        "transparent render targets should keep ordinary grid rendering");
    return true;
}

bool test_plot_config_default_lcd_request_is_auto_but_fails_closed()
{
    const plot::Plot_config config;
    TEST_ASSERT(
        config.lcd_request.automatic,
        "plot LCD request should default to AUTO");
    TEST_ASSERT(
        config.lcd_request.resolved_order == plot::lcd_subpixel_order_t::NONE,
        "plot default AUTO request should not carry a draw order");
    TEST_ASSERT(
        plot::detail::lcd_effective_order(
            config.lcd_request,
            plot::lcd_subpixel_order_t::NONE) == plot::lcd_subpixel_order_t::NONE,
        "default AUTO request should fail closed without a trusted platform order");
    return true;
}

} // namespace

int main()
{
    std::cout << "Core algorithm tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_choose_lod_level_picks_closest_pps);
    RUN_TEST(test_choose_lod_level_handles_degenerate_inputs);
    RUN_TEST(test_compute_lod_scales_forces_minimum_of_one);
    RUN_TEST(test_get_shift_handles_invalid_inputs);
    RUN_TEST(test_decimal_precision_helpers_reject_invalid_scaled_values);
    RUN_TEST(test_min_v_span_follows_float_magnitude);
    RUN_TEST(test_lower_and_upper_bound_on_contiguous_buffer);
    RUN_TEST(test_timestamp_search_rejects_null_samples);
    RUN_TEST(test_raw_timestamp_search_rejects_zero_stride);
    RUN_TEST(test_bounds_on_segmented_snapshot);
    RUN_TEST(test_select_visible_sample_window_monotonic_extends_bounds);
    RUN_TEST(test_select_visible_sample_window_non_monotonic_scans);
    RUN_TEST(test_aggregate_visible_sample_range_includes_step_after_hold);
    RUN_TEST(test_aggregate_visible_sample_range_rejects_nonfinite_range_endpoints);
    RUN_TEST(test_snapshot_truthiness_requires_usable_stride);
    RUN_TEST(test_snapshot_count1_clamps_malformed_count2);
    RUN_TEST(test_layout_cache_key_distinguishes_adjacent_int64_time_windows);
    RUN_TEST(test_format_axis_fixed_or_int);
    RUN_TEST(test_default_timestamp_precision_follows_step);
    RUN_TEST(test_default_elapsed_time_distinguishes_day_boundaries);
    RUN_TEST(test_duration_format_preserves_full_nanosecond_domain);
    RUN_TEST(test_wheel_delta_selection_preserves_units_and_reliability);
    RUN_TEST(test_horizontal_label_fades_do_not_restore_duplicate_text);
    RUN_TEST(test_horizontal_label_crossfades_suppress_rendered_duplicate_text);
    RUN_TEST(test_time_unit_helpers_handle_edges);
    RUN_TEST(test_timestamp_positions_preserve_epoch_nanoseconds);
    RUN_TEST(test_horizontal_label_fade_keys_preserve_int64_timestamps);
    RUN_TEST(test_label_fades_start_at_threshold_and_finish_after_zoom_stops);
    RUN_TEST(test_lcd_policy_helpers);
    RUN_TEST(test_built_in_label_backgrounds_are_opaque_for_lcd);
    RUN_TEST(test_grid_lcd_order_selection);
    RUN_TEST(test_plot_config_default_lcd_request_is_auto_but_fails_closed);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
