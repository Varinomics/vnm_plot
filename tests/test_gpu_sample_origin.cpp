// Tests for origin-selection helpers (choose_snap_ns, floor_div_int64,
// choose_origin_ns).

#include "test_macros.h"

#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/time_units.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace plot = vnm::plot;

namespace {

constexpr std::int64_t k_ns_per_us     = 1000LL;
constexpr std::int64_t k_ns_per_ms     = 1000000LL;
constexpr std::int64_t k_ns_per_second = 1000000000LL;
constexpr std::int64_t k_ns_per_hour   =  3600LL * k_ns_per_second;
constexpr std::int64_t k_ns_per_day    = 86400LL * k_ns_per_second;
constexpr std::int64_t k_ns_per_year   = 365LL * k_ns_per_day;

bool test_choose_snap_ns_is_positive_for_representative_spans()
{
    const std::int64_t spans[] = {
        1LL,
        k_ns_per_us,
        k_ns_per_ms,
        k_ns_per_second,
        k_ns_per_day,
        k_ns_per_year,
        std::numeric_limits<std::int64_t>::max() / 2,
    };

    for (std::int64_t span_ns : spans) {
        const std::int64_t snap_ns = plot::detail::choose_snap_ns(span_ns);
        TEST_ASSERT(snap_ns > 0,
            "choose_snap_ns must return a positive snap for every representative span");
    }
    return true;
}

bool test_choose_snap_ns_bucket_progression()
{
    // Verify the documented bucket boundaries: progressively coarser snap
    // steps as spans widen, so origin alignment stays meaningful at each
    // scale.
    TEST_ASSERT(plot::detail::choose_snap_ns(1LL)            == 1LL,
        "1 ns span snaps at 1 ns");
    TEST_ASSERT(plot::detail::choose_snap_ns(k_ns_per_us)    == 1LL,
        "1 us span fits in the sub-millisecond bucket");
    TEST_ASSERT(plot::detail::choose_snap_ns(k_ns_per_ms)    == 1LL,
        "1 ms span boundary stays at 1 ns snap");
    TEST_ASSERT(plot::detail::choose_snap_ns(k_ns_per_second) == k_ns_per_us,
        "1 s span snaps at 1 us");
    TEST_ASSERT(plot::detail::choose_snap_ns(k_ns_per_day)   == k_ns_per_second,
        "1 day span snaps at 1 s");
    TEST_ASSERT(plot::detail::choose_snap_ns(k_ns_per_year)  == k_ns_per_hour,
        "1 year span snaps at 1 hour");
    TEST_ASSERT(plot::detail::choose_snap_ns(10LL * k_ns_per_year) == k_ns_per_day,
        "10 year span snaps at 1 day");
    return true;
}

bool test_floor_div_int64_rounds_toward_negative_infinity()
{
    using plot::floor_div_int64;

    // C++ integer division truncates toward zero: -7 / 3 == -2 with
    // remainder -1. Floor division must return -3.
    TEST_ASSERT(floor_div_int64(-7,  3) == -3, "floor_div_int64(-7, 3) must be -3, not -2");
    TEST_ASSERT(floor_div_int64(-1,  3) == -1, "floor_div_int64(-1, 3) must be -1, not 0");
    TEST_ASSERT(floor_div_int64(-3,  3) == -1, "floor_div_int64(-3, 3) must be -1");
    TEST_ASSERT(floor_div_int64( 0,  3) ==  0, "floor_div_int64(0, 3) must be 0");
    TEST_ASSERT(floor_div_int64( 7,  3) ==  2, "floor_div_int64(7, 3) must be 2");
    TEST_ASSERT(floor_div_int64( 9,  3) ==  3, "floor_div_int64(9, 3) must be 3");
    return true;
}

bool test_choose_origin_ns_floors_for_negative_timestamps()
{
    // A timestamp of -1 ns with a 1 s snap must round down to -1 s, not 0.
    // This is the case naive C++ truncation would get wrong: -1 / 1e9 == 0,
    // putting the origin above t_view_min and breaking the rebasing
    // contract that t_view_min - origin >= 0.
    const std::int64_t t_view_min_ns = -1LL;
    const std::int64_t span_ns       = k_ns_per_hour;
    const std::int64_t snap_ns       = plot::detail::choose_snap_ns(span_ns);
    const std::int64_t origin_ns     = plot::detail::choose_origin_ns(t_view_min_ns, span_ns);

    TEST_ASSERT(snap_ns == k_ns_per_second,
        "1 hour span uses 1 s snap (precondition for this test)");
    TEST_ASSERT(origin_ns == -k_ns_per_second,
        "choose_origin_ns(-1 ns, 1 hour span) must floor to -1 s, not 0");
    TEST_ASSERT(origin_ns <= t_view_min_ns,
        "origin must never exceed t_view_min");
    TEST_ASSERT(origin_ns % snap_ns == 0,
        "origin must land on a snap boundary");
    return true;
}

bool test_choose_origin_ns_aligns_for_positive_timestamps()
{
    // 1 hour span -> 1 s snap. 1234567890 ns floor-aligned to 1 s is
    // 1000000000 ns; the residual 234567890 ns < 1 s confirms the
    // snap boundary is below t_view_min and within one snap step.
    const std::int64_t t_view_min_ns = 1234567890LL;
    const std::int64_t span_ns       = k_ns_per_hour;
    const std::int64_t origin_ns     = plot::detail::choose_origin_ns(t_view_min_ns, span_ns);

    TEST_ASSERT(origin_ns == 1000000000LL,
        "choose_origin_ns must align to the 1 s snap boundary below t_view_min");
    TEST_ASSERT(t_view_min_ns - origin_ns < k_ns_per_second,
        "rebased t_view_min stays within one snap step of the origin");
    return true;
}

bool test_fp32_round_trip_within_2e24_for_bounded_spans()
{
    // For span <= 1 day the rebased seconds at t_view_min + span is at most
    // (snap_ns + span_ns) * 1e-9 ~= 86401 s, well inside fp32's "every
    // integer representable" bound of 2^24 ~= 1.677e7. The same holds for
    // sub-day buckets. Spans beyond that exceed fp32's integer-second
    // precision, which is the documented trade-off in the snap policy.
    constexpr float k_fp32_integer_bound = 16777216.0f; // 2^24

    const std::int64_t spans[] = {
        1LL,
        k_ns_per_us,
        k_ns_per_ms,
        k_ns_per_second,
        k_ns_per_hour,
        k_ns_per_day,
    };

    const std::int64_t t_view_min_candidates[] = {
        0LL,
        -k_ns_per_day,
        1234567890LL,
        -1234567890LL,
    };

    for (std::int64_t span_ns : spans) {
        for (std::int64_t t_view_min_ns : t_view_min_candidates) {
            const std::int64_t origin_ns = plot::detail::choose_origin_ns(t_view_min_ns, span_ns);
            const std::int64_t t_end_ns  = t_view_min_ns + span_ns;
            const float        t_end_rel = static_cast<float>(t_end_ns - origin_ns) * 1e-9f;

            TEST_ASSERT(std::fabs(t_end_rel) < k_fp32_integer_bound,
                "rebased end-of-view seconds must fit inside fp32's exact-integer range");
        }
    }
    return true;
}

bool test_fp32_snap_step_resolution_at_bucket_boundaries()
{
    // The snap policy promises that snap-step resolution survives fp32
    // rebasing across every bucket. For each bucket the ratio
    // span_ns / snap_ns is bounded by ~1e6, so the rebased end-of-view
    // in seconds, while it can exceed 2^24 in the wider buckets, still
    // has a fp32 ulp at most one snap step. Cover the upper end of each
    // bucket plus the very-large-span fallback.
    const struct {
        std::int64_t span_ns;
        std::int64_t expected_snap_ns;
        std::int64_t t_view_min_ns;
    } cases[] = {
        // 1 us bucket (span <= 1 s). End-rel <= ~1 s; ulp << 1 us.
        { k_ns_per_second, k_ns_per_us, 0LL },
        // 1 s bucket (span <= 1 day). End-rel <= ~86400 s; ulp ~6 ms.
        { k_ns_per_day, k_ns_per_second, 0LL },
        // 1 hour bucket (span <= 1 year). End-rel ~3.15e7 s exceeds 2^24;
        // ulp at end ~2 s vs 3600 s snap, so rounding is preserved.
        { k_ns_per_year, k_ns_per_hour, 0LL },
        // 1 day fallback (span > 1 year). End-rel ~3.15e9 s; ulp at end
        // ~256 s vs 86400 s snap, so rounding is still preserved.
        { 100LL * k_ns_per_year, k_ns_per_day, 0LL },
    };

    for (const auto& c : cases) {
        const std::int64_t snap_ns = plot::detail::choose_snap_ns(c.span_ns);
        TEST_ASSERT(snap_ns == c.expected_snap_ns,
            "bucket boundary must select the documented snap step");

        const std::int64_t origin_ns = plot::detail::choose_origin_ns(c.t_view_min_ns, c.span_ns);
        const std::int64_t t_end_ns  = c.t_view_min_ns + c.span_ns;
        const float        t_end_rel = static_cast<float>(t_end_ns - origin_ns) * 1e-9f;

        // ulp at |t_end_rel| in fp32. std::nextafter gives the next
        // representable float; subtracting yields the local step size.
        const float ulp          = std::nextafter(t_end_rel, std::numeric_limits<float>::infinity()) - t_end_rel;
        const float snap_seconds = static_cast<float>(snap_ns) * 1e-9f;

        TEST_ASSERT(ulp <= snap_seconds,
            "fp32 ulp at end-of-view must not exceed the snap step (resolution preserved)");
    }
    return true;
}

bool test_main_and_preview_can_have_different_origins_in_same_frame()
{
    // The renderer picks origins independently for main and preview because
    // their visible windows can differ substantially. A typical case: the
    // main view is zoomed into a few seconds, while the preview shows the
    // full available range over hours or days. Each view's origin is
    // floored to its own snap bucket, so the two values must be allowed to
    // disagree within the same frame.

    // Main view: 1-hour visible window starting at t = 86400s + 1234567890ns.
    // 1-hour span -> 1-second snap; the origin floors below the visible min.
    const std::int64_t main_t_view_min = k_ns_per_day + 1234567890LL;
    const std::int64_t main_span       = k_ns_per_hour;
    const std::int64_t main_origin     =
        plot::detail::choose_origin_ns(main_t_view_min, main_span);

    // Preview view: 10-year range starting from 0. 10-year span -> 1-day snap.
    const std::int64_t preview_t_view_min = 0LL;
    const std::int64_t preview_span       = 10LL * k_ns_per_year;
    const std::int64_t preview_origin     =
        plot::detail::choose_origin_ns(preview_t_view_min, preview_span);

    TEST_ASSERT(main_origin != preview_origin,
        "main and preview must be allowed to pick different origins for the "
        "same frame state when their spans land in different snap buckets");

    // Each origin is on its own snap boundary and below its view min.
    const std::int64_t main_snap    = plot::detail::choose_snap_ns(main_span);
    const std::int64_t preview_snap = plot::detail::choose_snap_ns(preview_span);
    TEST_ASSERT(main_snap != preview_snap,
        "differently sized spans must select different snap steps "
        "(precondition for the per-view origin contract)");
    TEST_ASSERT(main_origin <= main_t_view_min,
        "main origin must not exceed main t_view_min");
    TEST_ASSERT(preview_origin <= preview_t_view_min,
        "preview origin must not exceed preview t_view_min");
    TEST_ASSERT(main_origin    % main_snap    == 0,
        "main origin must land on its own snap step");
    TEST_ASSERT(preview_origin % preview_snap == 0,
        "preview origin must land on its own snap step");

    // Sharing one origin between the views would push one view's rebased
    // seconds out of fp32 precision. Confirm by computing both views'
    // worst-case rebased magnitudes if they shared the main origin.
    const std::int64_t preview_t_max = preview_t_view_min + preview_span;
    const float preview_rebased_against_main =
        static_cast<float>(preview_t_max - main_origin) * 1.0e-9f;
    constexpr float k_fp32_integer_bound = 16777216.0f; // 2^24
    TEST_ASSERT(preview_rebased_against_main >= k_fp32_integer_bound,
        "the preview's far end re-based against the main origin would "
        "exceed fp32 integer precision (justifies independent origins)");

    return true;
}

bool test_choose_origin_ns_handles_int64_min()
{
    // INT64_MIN is a valid public timestamp. Naive floor-then-multiply UBs:
    // floor(INT64_MIN / 1e9) == -9223372037, and -9223372037 * 1e9 wraps
    // below INT64_MIN. The function must saturate, not invoke UB.
    constexpr std::int64_t k_int64_min = std::numeric_limits<std::int64_t>::min();

    const std::int64_t spans[] = {
        k_ns_per_second + 1,     // 1 us snap
        k_ns_per_day + 1,        // 1 s snap
        k_ns_per_year + 1,       // 1 hour snap
        100LL * k_ns_per_year,   // 1 day snap
    };

    for (std::int64_t span_ns : spans) {
        const std::int64_t origin_ns = plot::detail::choose_origin_ns(k_int64_min, span_ns);
        const std::int64_t snap_ns   = plot::detail::choose_snap_ns(span_ns);

        // Saturation contract: origin lands at or below t_view_min plus one
        // snap step, and stays a representable int64 (no overflow).
        TEST_ASSERT(origin_ns == k_int64_min,
            "choose_origin_ns at INT64_MIN must saturate to INT64_MIN");
        TEST_ASSERT(origin_ns <= k_int64_min + snap_ns,
            "saturated origin must be within one snap step of t_view_min");
    }
    return true;
}

bool test_full_int64_span_saturates_for_origin_api()
{
    constexpr std::int64_t k_int64_min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t k_int64_max = std::numeric_limits<std::int64_t>::max();

    const std::int64_t span_ns =
        plot::detail::positive_span_ns_for_signed_api(k_int64_min, k_int64_max);
    TEST_ASSERT(span_ns == k_int64_max,
        "full int64 timestamp range must saturate to INT64_MAX for signed span APIs");

    const std::int64_t origin_ns =
        plot::detail::choose_origin_ns(k_int64_min, span_ns);
    TEST_ASSERT(origin_ns == k_int64_min,
        "full-range origin selection must keep INT64_MIN without signed subtraction overflow");

    return true;
}

} // namespace

int main()
{
    std::cout << "Origin tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_choose_snap_ns_is_positive_for_representative_spans);
    RUN_TEST(test_choose_snap_ns_bucket_progression);
    RUN_TEST(test_floor_div_int64_rounds_toward_negative_infinity);
    RUN_TEST(test_choose_origin_ns_floors_for_negative_timestamps);
    RUN_TEST(test_choose_origin_ns_aligns_for_positive_timestamps);
    RUN_TEST(test_fp32_round_trip_within_2e24_for_bounded_spans);
    RUN_TEST(test_fp32_snap_step_resolution_at_bucket_boundaries);
    RUN_TEST(test_main_and_preview_can_have_different_origins_in_same_frame);
    RUN_TEST(test_choose_origin_ns_handles_int64_min);
    RUN_TEST(test_full_int64_span_saturates_for_origin_api);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
