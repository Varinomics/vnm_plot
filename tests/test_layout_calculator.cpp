// Tests for vnm_plot's Layout_calculator covering the formatter contract:
// the format_timestamp callback must receive both arguments as int64
// nanoseconds. The function_plotter example relies on this contract; a
// regression that started passing seconds or milliseconds would silently
// shift labels by 9 orders of magnitude.

#include "test_macros.h"

#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/layout_calculator.h>
#include <vnm_plot/core/types.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace plot = vnm::plot;

namespace {

constexpr std::int64_t k_ns_per_second = 1'000'000'000LL;

struct Recorded_call
{
    std::int64_t timestamp_ns = 0;
    std::int64_t step_ns      = 0;
};

plot::Layout_calculator::parameters_t make_minimal_params(
    std::int64_t                   t_min_ns,
    std::int64_t                   t_max_ns,
    std::vector<Recorded_call>&    recorded_calls)
{
    plot::Layout_calculator::parameters_t params;
    params.v_min                         = 0.0f;
    params.v_max                         = 1.0f;
    params.t_min                         = t_min_ns;
    params.t_max                         = t_max_ns;
    params.usable_width                  = 800.0;
    params.usable_height                 = 480.0;
    params.vbar_width                    = 56.0;
    params.label_visible_height          = 480.0;
    params.adjusted_font_size_in_pixels  = 14.0;
    params.h_label_vertical_nudge_factor = 0.0f;
    params.measure_text_cache_key        = 0;
    // Emulate a monospace font so the calculator does not need a
    // measure_text callback to estimate label widths.
    params.monospace_char_advance_px     = 8.0f;
    params.monospace_advance_is_reliable = true;
    params.measure_text_func             = [](const char* text) {
        return static_cast<float>(std::strlen(text)) * 8.0f;
    };
    // Recording formatter: capture every (timestamp, step) pair the
    // calculator passes through.
    params.format_timestamp_func = [&recorded_calls](
        std::int64_t timestamp_ns,
        std::int64_t step_ns) -> std::string
    {
        recorded_calls.push_back({timestamp_ns, step_ns});
        return std::to_string(timestamp_ns);
    };
    return params;
}

bool test_format_timestamp_receives_nanosecond_units()
{
    // 1-minute window centered around a modern wall-clock timestamp. The
    // calculator should produce horizontal labels and call the formatter
    // for each of them.
    constexpr std::int64_t k_t_min_ns =
        1'700'000'000LL * k_ns_per_second;     // ~late 2023
    constexpr std::int64_t k_t_max_ns =
        k_t_min_ns + 60LL * k_ns_per_second;   // 60-second span

    std::vector<Recorded_call> recorded;
    plot::Layout_calculator calc;
    auto params = make_minimal_params(k_t_min_ns, k_t_max_ns, recorded);
    auto result = calc.calculate(params);

    TEST_ASSERT(!recorded.empty(),
        "expected the layout calculator to invoke the format_timestamp "
        "callback for at least one horizontal label");
    TEST_ASSERT(!result.h_labels.empty(),
        "expected at least one horizontal label for a 60-second window");

    // Step values: build_time_steps_covering yields steps in seconds. The
    // formatter receives them as ns-quantised int64s (llround at the
    // boundary). For a 60-second span, the chosen finest step lies in the
    // 1 s ... 60 s window. Anything below 1 ms or above the span itself
    // would be a unit slip. (The calculator may also probe the formatter
    // for cache-signature purposes; both probe and label calls share the
    // same step_ns, so the step bound applies uniformly.)
    constexpr std::int64_t k_one_ms_ns = 1'000'000LL;
    for (const auto& call : recorded) {
        TEST_ASSERT(call.step_ns >= k_one_ms_ns,
            std::string("step_ns = ") + std::to_string(call.step_ns) +
            " is below 1 ms; if the formatter were receiving seconds "
            "instead of nanoseconds, every step would land here");
        TEST_ASSERT(call.step_ns <= 2 * (k_t_max_ns - k_t_min_ns),
            "step_ns is larger than the visible span; a likely sign of a "
            "ns-vs-us unit slip in the calculator -> formatter handoff");
    }

    // Timestamps: the calculator separates two kinds of formatter calls:
    //
    // 1. Format-signature probes at fixed ns-scale samples (currently 0,
    //    1.23e8, 1.23e13) used only to compute a cache-key hash. These do
    //    not depend on t_min/t_max.
    // 2. Actual label calls inside or near the visible window.
    //
    // The contract under test is that *both* receive ns-scale int64
    // values. Any actual-label call inside the visible-window region
    // confirms ns scale (a seconds-scale unit slip would compress the
    // values into a ~60 ns range starting near zero, far below the
    // expected ~1.7e18 magnitude).
    const std::int64_t margin_ns           = (k_t_max_ns - k_t_min_ns);
    bool               saw_label_in_window = false;
    for (const auto& call : recorded) {
        if (call.timestamp_ns >= k_t_min_ns - margin_ns &&
            call.timestamp_ns <= k_t_max_ns + margin_ns)
        {
            saw_label_in_window = true;
            break;
        }
    }
    TEST_ASSERT(saw_label_in_window,
        std::string("expected at least one formatter call with a timestamp "
            "inside the visible window [") +
        std::to_string(k_t_min_ns) + " .. " + std::to_string(k_t_max_ns) +
        "]; if the calculator passed t_seconds instead of t_ns, the "
        "values would land near 1.7e9 instead of 1.7e18");

    // Sanity: the formatter must have been invoked with valid (non-negative
    // step, finite timestamp) arguments. A ns-scale arg can be any int64;
    // we only check no nonsense like INT64_MIN/MAX leaking through.
    constexpr std::int64_t k_sentinel_lo = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_sentinel_hi = std::numeric_limits<std::int64_t>::max();
    for (const auto& call : recorded) {
        TEST_ASSERT(call.timestamp_ns != k_sentinel_lo &&
            call.timestamp_ns != k_sentinel_hi,
            "no formatter call should receive an INT64 sentinel timestamp");
    }

    return true;
}

bool test_format_timestamp_step_matches_nanosecond_seconds_grid()
{
    // The horizontal-axis ladder is built from build_time_steps_covering(),
    // which lives in algo.h and produces fixed multiples of seconds (with
    // sub-second 1-2-5 steps starting at 1 ms). For a few-second visible
    // window the calculator should pick a finest step from that ladder; the
    // step it passes to the formatter (after seconds -> ns conversion) must
    // round-trip exactly to one of the ladder entries.
    constexpr std::int64_t k_t_min_ns = 0LL;
    constexpr std::int64_t k_t_max_ns = 5LL * k_ns_per_second;

    std::vector<Recorded_call> recorded;
    plot::Layout_calculator calc;
    auto params = make_minimal_params(k_t_min_ns, k_t_max_ns, recorded);
    auto result = calc.calculate(params);

    TEST_ASSERT(!recorded.empty(),
        "expected the formatter to be invoked");
    (void)result;

    const auto ladder_seconds = plot::detail::build_time_steps_covering(
        static_cast<double>(k_t_max_ns - k_t_min_ns) / 1.0e9);

    // Convert ladder steps to ns and verify each recorded step matches one
    // entry exactly. If the calculator passed seconds, the ladder values
    // would be 1, 2, 5, ... rather than 1e9, 2e9, 5e9, ... and this check
    // would catch the slip.
    bool any_match = false;
    for (const auto& call : recorded) {
        bool matched = false;
        for (double step_seconds : ladder_seconds) {
            const std::int64_t step_ns_expected =
                static_cast<std::int64_t>(step_seconds * 1.0e9 + 0.5);
            if (call.step_ns == step_ns_expected) {
                matched = true;
                break;
            }
        }
        if (matched) {
            any_match = true;
        }
    }
    TEST_ASSERT(any_match,
        "expected at least one formatter step_ns to match a ladder entry "
        "after seconds -> ns conversion. Mismatch typically means the "
        "calculator passed step_ns in seconds (or in milliseconds) instead "
        "of nanoseconds.");

    return true;
}

bool test_subsecond_ticks_are_exact_epoch_nanoseconds()
{
    constexpr std::int64_t k_step_ns = 1'000'000;
    for (const std::int64_t start : {1'758'000'000'000'000'000LL,
                                   -1'758'000'000'000'000'000LL,
                                   std::numeric_limits<std::int64_t>::min() + 13,
                                   std::numeric_limits<std::int64_t>::max() - 50 * k_step_ns})
    {
        std::vector<Recorded_call> calls;
        auto params = make_minimal_params(start, start + 40 * k_step_ns, calls);
        params.usable_width = 1600.0;
        params.format_timestamp_func = [](std::int64_t timestamp_ns, std::int64_t) {
            return std::to_string(plot::floor_div_int64(timestamp_ns, k_step_ns) % 1000);
        };
        plot::Layout_calculator calculator;
        const auto result = calculator.calculate(params);
        TEST_ASSERT(result.h_labels.size() > 2,
            "subsecond window should emit multiple ticks");
        for (const auto& label : result.h_labels) {
            TEST_ASSERT(label.value % k_step_ns == 0,
                "epoch ticks must remain exact multiples of the integer nanosecond step");
        }
    }
    return true;
}

bool test_formatter_capture_change_matches_fresh_signature()
{
    std::vector<Recorded_call> calls;
    auto params = make_minimal_params(0, 60 * k_ns_per_second, calls);
    const auto formatter = [](const std::string& prefix) {
        return [prefix](std::int64_t timestamp_ns, std::int64_t) {
            return prefix + std::to_string(timestamp_ns / k_ns_per_second);
        };
    };
    plot::Layout_calculator calculator;
    params.format_timestamp_func = formatter("");
    calculator.calculate(params);
    params.format_timestamp_func = formatter("elapsed seconds: ");
    const auto replaced = calculator.calculate(params);
    ++params.format_timestamp_revision;
    const auto fresh = calculator.calculate(params);
    TEST_ASSERT(replaced.h_labels.size() == fresh.h_labels.size(),
        "formatter replacement must select the same ticks as a fresh format signature");
    for (std::size_t index = 0; index < fresh.h_labels.size(); ++index) {
        TEST_ASSERT(replaced.h_labels[index].value == fresh.h_labels[index].value &&
            replaced.h_labels[index].text == fresh.h_labels[index].text,
            "formatter cache must not reuse widths from a different capture");
    }
    return true;
}

bool test_horizontal_axis_handles_full_int64_time_span()
{
    constexpr std::int64_t k_int64_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_int64_max = std::numeric_limits<std::int64_t>::max();

    std::vector<Recorded_call> recorded;
    auto params = make_minimal_params(k_int64_min, k_int64_max, recorded);
    params.format_timestamp_revision = 1;

    plot::Layout_calculator calc;
    const auto result = calc.calculate(params);

    TEST_ASSERT(result.horizontal_seed_index >= 0,
        "full int64 timestamp range should enter horizontal-axis layout");
    TEST_ASSERT(result.horizontal_seed_step > 0.0,
        "full int64 timestamp range should compute a positive horizontal step");
    TEST_ASSERT(!recorded.empty(),
        "formatter-enabled full int64 timestamp range should call the formatter");
    TEST_ASSERT(result.h_labels.size() > 1,
        "formatter-enabled full int64 timestamp range should emit multiple horizontal labels");

    bool saw_signature_zero      = false;
    bool saw_signature_subsecond = false;
    bool saw_signature_large     = false;
    for (const auto& call : recorded) {
        saw_signature_zero = saw_signature_zero || call.timestamp_ns == 0;
        saw_signature_subsecond =
            saw_signature_subsecond || call.timestamp_ns == 123'456'789;
        saw_signature_large =
            saw_signature_large || call.timestamp_ns == 12'345'678'900'000;
        TEST_ASSERT(call.step_ns > 0,
            "formatter step_ns should remain a positive representable duration");
    }

    TEST_ASSERT(saw_signature_zero && saw_signature_subsecond && saw_signature_large,
        "formatter-enabled full int64 range should exercise format-signature probes");
    return true;
}

bool test_horizontal_axis_suppresses_only_consecutive_equal_text()
{
    std::vector<Recorded_call> recorded;
    auto params = make_minimal_params(0, 60LL * k_ns_per_second, recorded);
    params.format_timestamp_revision     = 1;
    params.vbar_width                    = 0.0;
    params.monospace_char_advance_px     = 0.0f;
    params.monospace_advance_is_reliable = false;
    params.measure_text_func             = [](const char*) { return 0.0f; };
    params.format_timestamp_func         = [](std::int64_t timestamp_ns, std::int64_t) {
        return std::to_string(timestamp_ns / (10LL * k_ns_per_second) % 2);
    };

    plot::Layout_calculator calc;
    const auto result = calc.calculate(params);

    TEST_ASSERT(result.h_labels.size() > 2,
        "alternating formatter runs should retain nonconsecutive equal strings");
    for (std::size_t i = 1; i < result.h_labels.size(); ++i) {
        TEST_ASSERT(result.h_labels[i - 1].text != result.h_labels[i].text,
            "consecutive equal rendered strings should be suppressed");
    }

    const std::string first_text = result.h_labels.front().text;
    TEST_ASSERT(std::count_if(
        result.h_labels.begin(),
        result.h_labels.end(),
        [&](const plot::h_label_t& label) { return label.text == first_text; }) > 1,
        "equal strings separated by a different string should remain visible");

    params.format_timestamp_revision = 2;
    params.format_timestamp_func     = [](std::int64_t timestamp_ns, std::int64_t) {
        return std::to_string(timestamp_ns);
    };
    const auto unique_result = calc.calculate(params);
    TEST_ASSERT(unique_result.h_labels.size() > 1,
        "unique formatting should expose both candidate endpoints");
    const float        leftmost_x      = unique_result.h_labels.front().position.x;
    const float        rightmost_x     = unique_result.h_labels.back().position.x;
    const std::int64_t leftmost_value  = unique_result.h_labels.front().value;
    const std::int64_t rightmost_value = unique_result.h_labels.back().value;

    params.format_timestamp_revision = 3;
    params.format_timestamp_func     = [leftmost_value](
        std::int64_t timestamp_ns,
        std::int64_t)
    {
        return timestamp_ns < leftmost_value ? std::string("outside") : std::string("same");
    };
    const auto constant_result = calc.calculate(params);
    TEST_ASSERT(constant_result.h_labels.size() == 1,
        "a consecutive run starting in view should retain only its first label");
    TEST_ASSERT(constant_result.h_labels.front().position.x == leftmost_x,
        "left-to-right duplicate suppression should retain the exact leftmost label");

    params.horizontal_axis_left_to_right = false;
    params.format_timestamp_revision     = 4;
    params.format_timestamp_func         = [rightmost_value](
        std::int64_t timestamp_ns,
        std::int64_t)
    {
        return timestamp_ns > rightmost_value
            ? std::string("outside")
            : std::string("same");
    };
    const auto reversed_result = calc.calculate(params);
    TEST_ASSERT(reversed_result.h_labels.size() == 1,
        "a consecutive run ending in view should retain only its last label");
    TEST_ASSERT(reversed_result.h_labels.front().position.x == rightmost_x,
        "right-to-left duplicate suppression should retain the exact rightmost label");

    return true;
}

bool test_horizontal_axis_hides_duplicate_run_entering_viewport()
{
    plot::Layout_calculator calc;
    const auto whole_seconds = [](std::int64_t timestamp_ns, std::int64_t) {
        return plot::default_format_elapsed_time(timestamp_ns, k_ns_per_second);
    };

    std::vector<Recorded_call> recorded_ltr;
    auto params_ltr = make_minimal_params(
        770'000'000,
        2LL * k_ns_per_second,
        recorded_ltr);
    params_ltr.usable_width              = 1435.0;
    params_ltr.format_timestamp_revision = 1;
    params_ltr.format_timestamp_func     = whole_seconds;
    const auto result_ltr = calc.calculate(params_ltr);

    TEST_ASSERT(std::none_of(
        result_ltr.h_labels.begin(),
        result_ltr.h_labels.end(),
        [](const plot::h_label_t& label) {
            return label.text == "00:00:00";
        }),
        "an LTR duplicate run whose retained end is off-screen should remain hidden");
    TEST_ASSERT(std::any_of(
        result_ltr.h_labels.begin(),
        result_ltr.h_labels.end(),
        [](const plot::h_label_t& label) {
            return label.text == "00:00:01";
        }),
        "the next LTR label run starting inside the viewport should remain visible");

    std::vector<Recorded_call> recorded_rtl;
    auto params_rtl = make_minimal_params(
        0,
        1'230'000'000,
        recorded_rtl);
    params_rtl.usable_width                  = 1435.0;
    params_rtl.format_timestamp_revision     = 2;
    params_rtl.format_timestamp_func         = whole_seconds;
    params_rtl.horizontal_axis_left_to_right = false;
    const auto result_rtl = calc.calculate(params_rtl);

    TEST_ASSERT(std::none_of(
        result_rtl.h_labels.begin(),
        result_rtl.h_labels.end(),
        [](const plot::h_label_t& label) {
            return label.text == "00:00:01";
        }),
        "an RTL duplicate run whose retained end is off-screen should remain hidden");
    TEST_ASSERT(std::any_of(
        result_rtl.h_labels.begin(),
        result_rtl.h_labels.end(),
        [](const plot::h_label_t& label) {
            return label.text == "00:00:00";
        }),
        "the preceding RTL label run ending inside the viewport should remain visible");

    return true;
}

bool test_default_subsecond_layout_labels_are_distinct()
{
    std::vector<Recorded_call> recorded;
    auto params = make_minimal_params(0, 2LL * k_ns_per_second, recorded);
    params.usable_width                  = 1600.0;
    params.monospace_char_advance_px     = 4.0f;
    params.monospace_advance_is_reliable = true;
    params.measure_text_func             = [](const char* text) {
        return static_cast<float>(std::strlen(text)) * 4.0f;
    };
    params.format_timestamp_func = plot::default_format_timestamp;

    plot::Layout_calculator calc;
    const auto result = calc.calculate(params);

    TEST_ASSERT(result.h_labels.size() > 2,
        "a two-second view should produce multiple horizontal labels");
    TEST_ASSERT(std::any_of(
        result.h_labels.begin(),
        result.h_labels.end(),
        [](const plot::h_label_t& label) {
            return label.text.find('.') != std::string::npos;
        }),
        "subsecond layout labels should include fractional seconds");
    for (std::size_t i = 1; i < result.h_labels.size(); ++i) {
        TEST_ASSERT(result.h_labels[i - 1].text != result.h_labels[i].text,
            "adjacent default subsecond layout labels should be distinguishable");
    }

    return true;
}

bool test_vertical_labels_keep_dense_level_when_glyphs_fit()
{
    std::vector<Recorded_call> recorded;
    auto params = make_minimal_params(0LL, 60LL * k_ns_per_second, recorded);
    params.v_min                        = -5000.0f;
    params.v_max                        = 45000.0f;
    params.usable_height                = 140.0;
    params.label_visible_height         = params.usable_height;
    params.adjusted_font_size_in_pixels = 10.0;

    plot::Layout_calculator calc;
    const auto result = calc.calculate(params);

    const std::vector<double> expected_values{
        0.0,
        10000.0,
        20000.0,
        30000.0,
        40000.0
    };

    TEST_ASSERT(result.v_labels.size() >= expected_values.size(),
        std::string("expected dense vertical labels to fit, got ") +
            std::to_string(result.v_labels.size()));

    for (const double value : expected_values) {
        const bool found = std::any_of(
            result.v_labels.begin(),
            result.v_labels.end(),
            [value](const plot::v_label_t& label) {
                return std::abs(label.value - value) < 1e-6;
            });
        TEST_ASSERT(found,
            std::string("expected vertical label value ") + std::to_string(value));
    }

    return true;
}

bool test_default_small_vertical_layout_labels_are_distinct()
{
    std::vector<Recorded_call> recorded;
    auto params = make_minimal_params(0LL, 60LL * k_ns_per_second, recorded);

    params.v_min = -0.002f;
    params.v_max = 0.002f;

    plot::Layout_calculator calc;
    const auto result = calc.calculate(params);

    TEST_ASSERT(result.v_labels.size() > 2,
        "a small vertical range should produce multiple labels");
    TEST_ASSERT(result.v_label_fixed_digits > 2,
        "vertical precision should follow a tick step below one hundredth");
    for (std::size_t i = 1; i < result.v_labels.size(); ++i) {
        TEST_ASSERT(result.v_labels[i - 1].text != result.v_labels[i].text,
            "adjacent small-range vertical labels should be distinguishable");
    }

    params.get_required_fixed_digits_func = [](double) { return 0; };
    const auto overridden_result = calc.calculate(params);
    TEST_ASSERT(overridden_result.v_label_fixed_digits == 0,
        "an explicit vertical precision override should remain supported");
    TEST_ASSERT(overridden_result.v_labels.size() == 1,
        "equal strings produced by an explicit precision should still be suppressed");

    return true;
}

bool test_vertical_axis_suppresses_only_consecutive_equal_text()
{
    std::vector<Recorded_call> recorded;
    auto params = make_minimal_params(0LL, 60LL * k_ns_per_second, recorded);

    params.v_min             = -1.0f;
    params.v_max             = 1.0f;
    params.format_value_func = [](double value, const plot::value_format_context_t&) {
        return std::abs(value) < 0.25 ? std::string("middle") : std::string("edge");
    };

    plot::Layout_calculator calc;
    const auto result = calc.calculate(params);

    TEST_ASSERT(result.v_labels.size() == 3,
        "vertical edge/middle/edge formatter runs should retain three labels");
    TEST_ASSERT(
        result.v_labels[0].text == " edge" &&
        result.v_labels[1].text == " middle" &&
        result.v_labels[2].text == " edge",
        "only consecutive equal vertical strings should be suppressed");

    params.format_value_func = [](double, const plot::value_format_context_t&) {
        return std::string("same");
    };
    const auto constant_result = calc.calculate(params);
    TEST_ASSERT(constant_result.v_labels.size() == 1,
        "a single vertical formatter run should retain one label");
    TEST_ASSERT(constant_result.v_labels.front().value < 0.0,
        "bottom-to-top duplicate suppression should retain the lowest label");

    return true;
}

} // namespace

int main()
{
    std::cout << "Layout calculator tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_format_timestamp_receives_nanosecond_units);
    RUN_TEST(test_format_timestamp_step_matches_nanosecond_seconds_grid);
    RUN_TEST(test_subsecond_ticks_are_exact_epoch_nanoseconds);
    RUN_TEST(test_formatter_capture_change_matches_fresh_signature);
    RUN_TEST(test_horizontal_axis_handles_full_int64_time_span);
    RUN_TEST(test_horizontal_axis_suppresses_only_consecutive_equal_text);
    RUN_TEST(test_horizontal_axis_hides_duplicate_run_entering_viewport);
    RUN_TEST(test_default_subsecond_layout_labels_are_distinct);
    RUN_TEST(test_vertical_labels_keep_dense_level_when_glyphs_fit);
    RUN_TEST(test_default_small_vertical_layout_labels_are_distinct);
    RUN_TEST(test_vertical_axis_suppresses_only_consecutive_equal_text);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
