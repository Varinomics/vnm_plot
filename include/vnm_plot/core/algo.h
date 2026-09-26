#pragma once

// VNM Plot Library - Algorithm Utilities
// Small, header-only helpers for axis calculations and formatting.
// Pure C++ with no framework dependencies.
//
// Public API (vnm::plot):
//   - format_axis_fixed_or_int: Format numeric values for axis labels
//
// Internal API (vnm::plot::detail):
//   - Grid/time step calculation helpers
//   - Binary search for timestamps
//   - LOD selection algorithms

#include "types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vnm::plot {

// =============================================================================
// Public API
// =============================================================================

// Format a numeric value with either integer or fixed precision.
// Used for axis label formatting. Can be used in custom format_timestamp callbacks.
inline std::string format_axis_fixed_or_int(double v, int digits)
{
    if (!std::isfinite(v)) {
        return "0";
    }

    if (digits <= 0) {
        const double      rounded    = std::round(v);
        const long double rounded_ld = static_cast<long double>(rounded);
        if (rounded_ld < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
            rounded_ld > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        {
            return "0";
        }
        const std::int64_t iv = static_cast<std::int64_t>(rounded);
        if (iv == 0) {
            return "0";
        }
        return std::to_string(iv);
    }

    const double scale = std::pow(10.0, double(digits));
    if (!std::isfinite(scale) || scale <= 0.0) {
        return "0";
    }
    const long double scale_ld = static_cast<long double>(scale);
    const long double scaled   = static_cast<long double>(v) * scale_ld;
    if (!std::isfinite(scaled) ||
        std::abs(scaled) > static_cast<long double>(std::numeric_limits<double>::max()))
    {
        return "0";
    }

    const long double rounded_scaled = std::round(scaled);
    const long double rounded        = rounded_scaled / scale_ld;
    if (!std::isfinite(rounded) ||
        std::abs(rounded) > static_cast<long double>(std::numeric_limits<double>::max()))
    {
        return "0";
    }

    double       r   = static_cast<double>(rounded);
    const double eps = 0.5 / scale;

    if (std::abs(r) < eps) {
        r = 0.0;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(digits) << r;
    std::string s = oss.str();

    // Collapse "-0.0..." to "0.0..."
    if (s.size() >= 3 && s[0] == '-' && s[1] == '0' && s[2] == '.' && std::abs(r) < eps) {
        s.erase(0, 1);
    }

    return s;
}

// =============================================================================
// Internal Implementation Details
// =============================================================================

namespace detail {

// -----------------------------------------------------------------------------
// Decimal Analysis
// -----------------------------------------------------------------------------

// Check if any value has non-zero fractional part at the given precision.
inline bool any_fractional_at_precision(const std::vector<double>& values, int digits)
{
    if (digits <= 0) {
        return false;
    }

    const double scale = std::pow(10.0, double(digits));
    if (!std::isfinite(scale) || scale <= 0.0) {
        return false;
    }
    const double eps = 0.5 / scale;

    for (double v : values) {
        const double scaled = v * scale;
        if (!std::isfinite(scaled) ||
            static_cast<long double>(std::abs(scaled)) >
                static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        {
            return false;
        }

        const double rounded_scaled = std::round(scaled);
        if (!std::isfinite(rounded_scaled)) {
            return false;
        }

        const double r       = rounded_scaled / scale;
        const double rounded = std::round(r);
        if (!std::isfinite(r) || !std::isfinite(rounded)) {
            return false;
        }
        if (std::abs(r - rounded) > eps) {
            return true;
        }
    }
    return false;
}

// Returns true if all values' last decimal digit is zero at given precision.
inline bool trailing_zero_decimal_for_all(const std::vector<double>& values, int digits)
{
    if (digits <= 0) {
        return false;
    }

    const double scale = std::pow(10.0, double(digits));
    for (double v : values) {
        const double scaled = std::abs(v) * scale;
        if (!std::isfinite(scaled) ||
            static_cast<long double>(scaled) >
                static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        {
            return false;
        }
        const std::int64_t q = static_cast<std::int64_t>(std::round(scaled));
        if ((q % 10) != 0) {
            return false;
        }
    }
    return true;
}

// Reduce digits while last decimal place is zero for all values.
inline int trim_trailing_zero_decimals(const std::vector<double>& values, int digits)
{
    int d = std::max(0, digits);
    while (d > 0 && trailing_zero_decimal_for_all(values, d)) {
        --d;
    }
    return d;
}

// -----------------------------------------------------------------------------
// Grid Alignment
// -----------------------------------------------------------------------------

// Compute phase shift for grid lines to align nicely with a minimum value.
inline double get_shift(double section_size, double minval)
{
    if (!(section_size > 0.0) || !std::isfinite(section_size) || !std::isfinite(minval)) {
        return 0.0;
    }

    double a = std::floor(std::log10(std::fabs(section_size)));
    if (!std::isfinite(a)) {
        return 0.0;
    }

    double m = 1.0;
    if (a < 0.0) {
        m = std::pow(10.0, -a);
        if (!std::isfinite(m) || !(m > 0.0)) {
            return 0.0;
        }
    }

    const double denom      = section_size * m;
    const double scaled_min = minval * m;
    if (!(denom > 0.0) || !std::isfinite(denom) || !std::isfinite(scaled_min)) {
        return 0.0;
    }

    const double numerator = denom - scaled_min + denom;
    if (!std::isfinite(numerator)) {
        return 0.0;
    }

    double ret = std::fmod(numerator, denom);
    if (!std::isfinite(ret)) {
        return 0.0;
    }
    ret /= m;
    if (!std::isfinite(ret)) { return 0.0;          }
    if (ret < 0)             { ret += section_size; }
    return std::isfinite(ret) ? ret : 0.0;
}

// -----------------------------------------------------------------------------
// Time Axis Steps
// -----------------------------------------------------------------------------

// Build ascending list of time steps (in seconds) covering max_span.
inline std::vector<double> build_time_steps_covering(double max_span)
{
    std::vector<double> steps;
    steps.reserve(64);

    // Sub-second: 1/5 multiples from 1ms up to 0.5s.
    static const double subsec[] = {0.001, 0.005, 0.01, 0.05, 0.1, 0.5};
    steps.insert(steps.end(), std::begin(subsec), std::end(subsec));

    // Exact sequence from seconds up to 2 days
    static const double exact[] = {
        1, 2, 10, 30,             // seconds
        60, 300, 900, 1800,       // minutes: 1m, 5m, 15m, 30m
        3600, 7200, 21600, 43200, // hours: 1h, 2h, 6h, 12h
        86400                     // 1 day
    };
    steps.insert(steps.end(), std::begin(exact), std::end(exact));

    // Power-of-two chain beyond 2 days to keep exact multiples.
    double       s     = 172800.0; // 2 days
    const double limit = std::max(max_span * 2.0, s * 2.0);
    while (s < limit && s < 1e12) {
        s *= 2.0;
        steps.push_back(s);
    }
    return steps;
}

// Find index of largest step <= t_range.
inline int find_time_step_start_index(const std::vector<double>& steps, double t_range)
{
    if (steps.empty()) {
        return -1;
    }

    int idx = 0;
    while (idx + 1 < static_cast<int>(steps.size()) && steps[idx + 1] <= t_range) {
        ++idx;
    }
    return idx;
}

// -----------------------------------------------------------------------------
// Range Utilities
// -----------------------------------------------------------------------------

// Minimal usable span for the vertical axis (prevents float precision collapse).
inline float min_v_span_for(float a, float b)
{
    const float magnitude = std::max(std::abs(a), std::abs(b));
    return 64.0f * std::max(
        std::numeric_limits<float>::epsilon() * magnitude,
        std::numeric_limits<float>::denorm_min());
}

// -----------------------------------------------------------------------------
// LOD Scale Computation
// -----------------------------------------------------------------------------

// Compute LOD scales vector from a data source.
// Works with any type that has lod_levels() and lod_scale(level) methods.
// Each scale represents the subsampling factor at that LOD level.
//
// Note: Scales are clamped to >= 1 to prevent division-by-zero and other
// edge cases downstream. A scale of 0 is semantically invalid (would mean
// "zero samples per coarse sample"). Data sources should always return >= 1.
template<typename DataSourceT>
std::vector<std::size_t> compute_lod_scales(const DataSourceT& data_source)
{
    std::vector<std::size_t> scales;
    const std::size_t levels = data_source.lod_levels();
    scales.reserve(levels);
    for (std::size_t lvl = 0; lvl < levels; ++lvl) {
        const std::size_t scale = std::max<std::size_t>(1, data_source.lod_scale(lvl));
        scales.push_back(scale);
    }
    return scales;
}

// -----------------------------------------------------------------------------
// Binary Search for Timestamps
// -----------------------------------------------------------------------------
// These functions perform binary search on a contiguous array of samples,
// using a callback to extract timestamps. Assumes ascending timestamp order.

// Core binary-search loop shared by lower_bound / upper_bound and by raw
// strided / data_snapshot_t addressing. The Addr functor maps an index to a
// sample pointer; nullptr means malformed or torn input and fails the search.
template<typename AddrFn, typename GetTimestampFn, typename Cmp>
std::optional<std::size_t> bsearch_ts_impl(
    std::size_t        count,
    AddrFn&&           addr,
    GetTimestampFn&&   get_timestamp,
    Cmp&&              cmp,
    std::int64_t       t_ns)
{
    std::size_t lo = 0;
    std::size_t hi = count;
    while (lo < hi) {
        std::size_t mid    = lo + (hi - lo) / 2;
        const void* sample = addr(mid);
        if (!sample) {
            return std::nullopt;
        }
        if (cmp(get_timestamp(sample), t_ns)) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return lo;
}

// Returns index of first sample with timestamp >= t_ns (lower_bound semantics).
// Query value t_ns is in nanoseconds (API convention); the user-supplied
// get_timestamp accessor must return the same unit.
template<typename GetTimestampFn>
std::size_t lower_bound_timestamp(
    const void*        data,
    std::size_t        count,
    std::size_t        stride,
    GetTimestampFn&&   get_timestamp,
    std::int64_t       t_ns)
{
    if (data == nullptr || count == 0 || stride == 0) {
        return 0;
    }
    const auto* base = static_cast<const std::uint8_t*>(data);
    const auto result = bsearch_ts_impl(
        count,
        [base, stride](std::size_t i) -> const void* { return base + i * stride; },
        std::forward<GetTimestampFn>(get_timestamp),
        [](std::int64_t ts, std::int64_t t) { return ts < t; },
        t_ns);
    return result.value_or(0);
}

// Overload for data_snapshot_t (supports segmented snapshots).
template<typename GetTimestampFn>
std::size_t lower_bound_timestamp(
    const data_snapshot_t& snapshot,
    GetTimestampFn&&       get_timestamp,
    std::int64_t           t_ns)
{
    if (!snapshot.is_valid()) {
        return 0;
    }
    const auto result = bsearch_ts_impl(
        snapshot.count,
        [&snapshot](std::size_t i) { return snapshot.at(i); },
        std::forward<GetTimestampFn>(get_timestamp),
        [](std::int64_t ts, std::int64_t t) { return ts < t; },
        t_ns);
    return result.value_or(0);
}

// Returns index of first sample with timestamp > t_ns (upper_bound semantics).
// Query value t_ns is in nanoseconds (API convention).
template<typename GetTimestampFn>
std::size_t upper_bound_timestamp(
    const void*        data,
    std::size_t        count,
    std::size_t        stride,
    GetTimestampFn&&   get_timestamp,
    std::int64_t       t_ns)
{
    if (data == nullptr || count == 0 || stride == 0) {
        return 0;
    }
    const auto* base = static_cast<const std::uint8_t*>(data);
    const auto result = bsearch_ts_impl(
        count,
        [base, stride](std::size_t i) -> const void* { return base + i * stride; },
        std::forward<GetTimestampFn>(get_timestamp),
        [](std::int64_t ts, std::int64_t t) { return ts <= t; },
        t_ns);
    return result.value_or(0);
}

// Overload for data_snapshot_t (supports segmented snapshots).
template<typename GetTimestampFn>
std::size_t upper_bound_timestamp(
    const data_snapshot_t& snapshot,
    GetTimestampFn&&       get_timestamp,
    std::int64_t           t_ns)
{
    if (!snapshot.is_valid()) {
        return 0;
    }
    const auto result = bsearch_ts_impl(
        snapshot.count,
        [&snapshot](std::size_t i) { return snapshot.at(i); },
        std::forward<GetTimestampFn>(get_timestamp),
        [](std::int64_t ts, std::int64_t t) { return ts <= t; },
        t_ns);
    return result.value_or(0);
}

struct timestamp_bracket_t
{
    std::size_t    i0    = 0;
    std::size_t    i1    = 0;
    bool           valid = false;

    explicit operator bool() const noexcept { return valid; }
};

struct visible_sample_window_t
{
    std::size_t    first          = 0;
    std::size_t    last_exclusive = 0;
    bool           valid          = false;

    explicit operator bool() const noexcept { return valid; }
};

template<typename GetTimestampFn>
visible_sample_window_t select_visible_sample_window(
    const data_snapshot_t& snapshot,
    GetTimestampFn&&       get_timestamp,
    std::int64_t           t_min_ns,
    std::int64_t           t_max_ns,
    bool                   timestamps_monotonic)
{
    if (!snapshot.is_valid()) {
        return {};
    }

    if (t_max_ns < t_min_ns) {
        return {snapshot.count, snapshot.count, true};
    }

    if (!timestamps_monotonic) {
        std::size_t match_first = snapshot.count;
        std::size_t match_last  = 0;
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            const void* sample = snapshot.at(i);
            if (!sample) {
                continue;
            }
            const std::int64_t ts = get_timestamp(sample);
            if (ts < t_min_ns || ts > t_max_ns) { continue;        }
            if (match_first == snapshot.count)  { match_first = i; }
            match_last = i + 1;
        }
        if (match_first < match_last) {
            return {
                (match_first > 0) ? (match_first - 1) : 0,
                std::min(match_last + 2, snapshot.count),
                true
            };
        }
        return {snapshot.count, snapshot.count, true};
    }

    std::size_t first_idx = lower_bound_timestamp(
        snapshot,
        get_timestamp,
        t_min_ns);
    if (first_idx > 0) {
        --first_idx;
    }
    std::size_t last_idx = upper_bound_timestamp(
        snapshot,
        get_timestamp,
        t_max_ns);
    last_idx = std::min(last_idx + 2, snapshot.count);

    return {first_idx, last_idx, true};
}

struct visible_sample_aggregate_t
{
    double         vmin    = 0.0;
    double         vmax    = 0.0;
    std::int64_t   tmin_ns = 0;
    std::int64_t   tmax_ns = 0;
    bool           valid   = false;

    explicit operator bool() const noexcept { return valid; }
};

template<typename GetTimestampFn, typename GetRangeFn>
visible_sample_aggregate_t aggregate_visible_sample_range(
    const data_snapshot_t& snapshot,
    GetTimestampFn&&       get_timestamp,
    GetRangeFn&&           get_range,
    std::int64_t           window_tmin_ns,
    std::int64_t           window_tmax_ns,
    Series_interpolation   interpolation,
    Empty_window_behavior  empty_window_behavior)
{
    visible_sample_aggregate_t aggregate;
    if (!snapshot.is_valid() || window_tmax_ns < window_tmin_ns) {
        return aggregate;
    }

    const auto include_sample =
        [&aggregate](std::int64_t ts_ns, double low, double high) {
            if (!std::isfinite(low) || !std::isfinite(high)) {
                return;
            }
            const double dlow  = std::min(low, high);
            const double dhigh = std::max(low, high);
            if (!aggregate.valid) {
                aggregate = {dlow, dhigh, ts_ns, ts_ns, true};
                return;
            }
            aggregate.vmin    = std::min(aggregate.vmin, dlow);
            aggregate.vmax    = std::max(aggregate.vmax, dhigh);
            aggregate.tmin_ns = std::min(aggregate.tmin_ns, ts_ns);
            aggregate.tmax_ns = std::max(aggregate.tmax_ns, ts_ns);
        };

    const void* held_sample                          = nullptr;
    bool        have_sample_at_or_after_window_start = false;
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        const void* sample = snapshot.at(i);
        if (!sample) {
            continue;
        }
        const std::int64_t ts_ns = get_timestamp(sample);
        if (interpolation == Series_interpolation::STEP_AFTER &&
            ts_ns         <  window_tmin_ns)
        {
            held_sample = sample;
            continue;
        }
        if (interpolation == Series_interpolation::STEP_AFTER &&
            ts_ns         >= window_tmin_ns)
        {
            have_sample_at_or_after_window_start = true;
        }
        if (ts_ns < window_tmin_ns || ts_ns > window_tmax_ns) {
            continue;
        }

        const auto range = get_range(sample);
        if (range) {
            include_sample(ts_ns, range->first, range->second);
        }
    }

    const bool held_sample_reaches_window =
        have_sample_at_or_after_window_start ||
        empty_window_behavior == Empty_window_behavior::HOLD_LAST_FORWARD;
    if (held_sample && held_sample_reaches_window) {
        const auto range = get_range(held_sample);
        if (range) {
            include_sample(window_tmin_ns, range->first, range->second);
        }
    }

    return aggregate;
}

template<typename AddrFn, typename GetTimestampFn>
timestamp_bracket_t bracket_timestamp_impl(
    std::size_t        count,
    AddrFn&&           addr,
    GetTimestampFn&&   get_timestamp,
    std::int64_t       t_ns)
{
    if (count == 0) {
        return {};
    }

    const void* first_sample = addr(0);
    const void* last_sample  = addr(count - 1);
    if (!first_sample || !last_sample) {
        return {};
    }

    const std::int64_t first_ts  = get_timestamp(first_sample);
    const std::int64_t last_ts   = get_timestamp(last_sample);
    const bool         ascending = first_ts <= last_ts;

    std::size_t lo = 0;
    std::size_t hi = count - 1;
    while (lo < hi) {
        const std::size_t mid        = lo + (hi - lo) / 2;
        const void*       mid_sample = addr(mid);
        if (!mid_sample) {
            return {};
        }

        const std::int64_t ts = get_timestamp(mid_sample);
        if (ascending ? (ts <= t_ns) : (ts >= t_ns)) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }

    if (count == 1) {
        return {0, 0, true};
    }

    if (ascending) {
        if (t_ns < first_ts) {
            return {0, 0, true};
        }
        if (t_ns >= last_ts) {
            return {count - 1, count - 1, true};
        }
    }
    else {
        if (t_ns > first_ts) {
            return {0, 0, true};
        }
        if (t_ns <= last_ts) {
            return {count - 1, count - 1, true};
        }
    }

    return {lo > 0 ? lo - 1 : 0, lo, true};
}

template<typename GetTimestampFn>
timestamp_bracket_t bracket_timestamp(
    const data_snapshot_t& snapshot,
    GetTimestampFn&&       get_timestamp,
    std::int64_t           t_ns)
{
    if (!snapshot.is_valid()) {
        return {};
    }

    return bracket_timestamp_impl(
        snapshot.count,
        [&snapshot](std::size_t i) { return snapshot.at(i); },
        std::forward<GetTimestampFn>(get_timestamp),
        t_ns);
}

// -----------------------------------------------------------------------------
// Origin Selection for fp32 GPU Time Rebasing
// -----------------------------------------------------------------------------
// The renderer uploads sample timestamps as fp32 seconds relative to a
// per-view origin. Origin selection has two jobs: pick a snap step coarse
// enough that rebased seconds keep usable fp32 precision over the view
// span, and align the origin to that step so the cache key stays stable
// for small camera moves within the same snap bucket.
//
// Precision argument: fp32 has 24 mantissa bits, so it represents every
// integer up to 2^24 ~= 1.677e7 exactly. The maximum absolute rebased
// value is bounded by (snap_ns + span_ns) * 1e-9. The bucket policy below
// keeps span_ns/snap_ns <= ~1e6 across all spans, so the snap-step
// resolution within the rebased range is preserved by fp32's mantissa
// even when the view span itself exceeds that range. For spans wider
// than ~193 days (1.67e16 ns) the absolute rebased value exceeds 2^24
// seconds; precision then degrades to whole snap steps, but the snap
// step is coarse enough (>= 1 day) that this is the intended trade-off.

inline std::int64_t choose_snap_ns(std::int64_t span_ns)
{
    constexpr std::int64_t k_ns_per_us     = 1000LL;
    constexpr std::int64_t k_ns_per_ms     = 1000000LL;
    constexpr std::int64_t k_ns_per_second = 1000000000LL;
    constexpr std::int64_t k_ns_per_hour   = k_ns_per_second * 3600LL;
    constexpr std::int64_t k_ns_per_day    = k_ns_per_second * 86400LL;
    constexpr std::int64_t k_ns_per_year   = k_ns_per_day * 365LL;

    if (span_ns <= k_ns_per_ms)     { return 1LL;             }
    if (span_ns <= k_ns_per_second) { return k_ns_per_us;     }
    if (span_ns <= k_ns_per_day)    { return k_ns_per_second; }
    if (span_ns <= k_ns_per_year)   { return k_ns_per_hour;   }
    return k_ns_per_day;
}

// Pick a per-view time origin in nanoseconds. The result is the largest
// multiple of choose_snap_ns(span_ns) that is <= t_view_min_ns, so the
// origin lies on a stable bucket boundary and small camera moves within
// the same bucket reuse the same upload cache key.
//
// Saturates at the int64 floor: t_view_min_ns within one snap step of
// INT64_MIN cannot have a snap-aligned origin <= t_view_min_ns inside
// int64 range, so we return INT64_MIN. INT64_MIN is a valid public
// timestamp, and saturating keeps the multiply from overflowing into UB
// on signed wrap.
inline std::int64_t choose_origin_ns(std::int64_t t_view_min_ns, std::int64_t span_ns)
{
    const std::int64_t     snap_ns = choose_snap_ns(span_ns);
    const std::int64_t     q       = floor_div_int64(t_view_min_ns, snap_ns);
    constexpr std::int64_t k_min   = std::numeric_limits<std::int64_t>::min();
    if (snap_ns > 1 && q < k_min / snap_ns) {
        return k_min;
    }
    return q * snap_ns;
}

// -----------------------------------------------------------------------------
// LOD Level Selection
// -----------------------------------------------------------------------------
// Level selection based on pixels-per-sample, without hysteresis.
// Chooses the LOD level whose pixels-per-sample is closest to 1.0.

inline std::size_t choose_lod_level(
    const std::vector<std::size_t>&    scales,
    double                             base_pps)
{
    if (scales.empty() || !(base_pps > 0.0)) {
        return 0;
    }

    constexpr double target_pps = 1.0;
    std::size_t      best_level = 0;
    double           best_error = std::abs(base_pps * static_cast<double>(scales[0]) - target_pps);

    for (std::size_t i = 1; i < scales.size(); ++i) {
        const double pps   = base_pps * static_cast<double>(scales[i]);
        const double error = std::abs(pps - target_pps);
        if (error < best_error) {
            best_error = error;
            best_level = i;
        }
    }

    return best_level;
}

} // namespace detail
} // namespace vnm::plot
