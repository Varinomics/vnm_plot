#include <vnm_plot/core/types.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/plot_config.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace vnm::plot {

namespace {

enum class sample_scan_status
{
    ACCEPTED,
    SKIPPED,
    FAILED,
};

Data_query_status status_from_snapshot(snapshot_result_t::Snapshot_status status)
{
    switch (status) {
        case snapshot_result_t::Snapshot_status::READY:  return Data_query_status::READY;
        case snapshot_result_t::Snapshot_status::EMPTY:  return Data_query_status::EMPTY;
        case snapshot_result_t::Snapshot_status::BUSY:   return Data_query_status::BUSY;
        case snapshot_result_t::Snapshot_status::FAILED: return Data_query_status::FAILED;
    }
    return Data_query_status::FAILED;
}

Data_query_status invalid_ready_snapshot_status(const data_snapshot_t& snapshot)
{
    return snapshot.count == 0
        ? Data_query_status::EMPTY
        : Data_query_status::FAILED;
}

bool time_window_contains(time_range_t window, std::int64_t timestamp_ns)
{
    return
        window.min_ns <= window.max_ns &&
        timestamp_ns  >= window.min_ns &&
        timestamp_ns  <= window.max_ns;
}

bool wants_hold_forward(const data_query_context_t& query)
{
    return
        query.interpolation         == Series_interpolation::STEP_AFTER &&
        query.empty_window_behavior == Empty_window_behavior::HOLD_LAST_FORWARD;
}

void include_value(value_range_t& range, bool& has_value, float value)
{
    if (!has_value) {
        range.min = value;
        range.max = value;
        has_value = true;
        return;
    }
    if (value < range.min) { range.min = value; }
    if (value > range.max) { range.max = value; }
}

void include_range(value_range_t& range, bool& has_value, float low, float high)
{
    if (high < low) {
        std::swap(low, high);
    }
    include_value(range, has_value, low);
    include_value(range, has_value, high);
}

sample_scan_status sample_value_range(
    const Data_access_policy&      access,
    const void*                    sample,
    Nonfinite_sample_policy        policy,
    value_range_t&                 range)
{
    detail::sample_draw_value_t draw_value;
    const detail::sample_draw_status_t draw_status =
        detail::read_sample_draw_value(access, sample, policy, draw_value);
    if (draw_status == detail::sample_draw_status_t::DRAWABLE) {
        range = {draw_value.y_min, draw_value.y_max};
        return sample_scan_status::ACCEPTED;
    }
    if (draw_status == detail::sample_draw_status_t::FAILED) {
        return sample_scan_status::FAILED;
    }
    return sample_scan_status::SKIPPED;
}

bool scan_value_range(
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    const data_query_context_t&    query,
    value_range_t&                 range,
    bool&                          has_value)
{
    value_range_t held_range;
    bool         has_held_candidate    = false;
    bool         has_held_value        = false;
    bool         held_candidate_failed = false;
    std::int64_t held_timestamp_ns     = 0;
    const bool   step_after            = query.interpolation == Series_interpolation::STEP_AFTER;
    bool         have_following_sample = false;

    for (std::size_t index = 0; index < snapshot.count; ++index) {
        const void* sample = snapshot.at(index);
        if (!sample) {
            return false;
        }

        const std::int64_t timestamp_ns  = query.access->get_timestamp(sample);
        const bool         in_window     = time_window_contains(query.time_window, timestamp_ns);
        const bool         before_window = timestamp_ns < query.time_window.min_ns;
        have_following_sample = have_following_sample || !before_window;
        if (!in_window && !(step_after && before_window)) {
            continue;
        }

        if (step_after && before_window &&
            has_held_candidate && timestamp_ns <= held_timestamp_ns)
        {
            continue;
        }

        value_range_t sample_range;
        const sample_scan_status scan_status = sample_value_range(
            access,
            sample,
            query.nonfinite_policy,
            sample_range);

        if (in_window) {
            if (scan_status == sample_scan_status::FAILED) {
                return false;
            }
            if (scan_status == sample_scan_status::ACCEPTED) {
                include_range(range, has_value, sample_range.min, sample_range.max);
            }
            continue;
        }

        if (scan_status == sample_scan_status::ACCEPTED) {
            has_held_candidate    = true;
            has_held_value        = true;
            held_candidate_failed = false;
            held_timestamp_ns     = timestamp_ns;
            held_range            = sample_range;
        }
        else
        if (query.nonfinite_policy == Nonfinite_sample_policy::BREAK_SEGMENT) {
            has_held_candidate    = true;
            has_held_value        = false;
            held_candidate_failed = false;
            held_timestamp_ns     = timestamp_ns;
        }
        else
        if (scan_status == sample_scan_status::FAILED) {
            has_held_candidate    = true;
            has_held_value        = false;
            held_candidate_failed = true;
            held_timestamp_ns     = timestamp_ns;
        }
    }

    const bool held_step_reaches_window = have_following_sample || wants_hold_forward(query);
    if (held_step_reaches_window && held_candidate_failed) {
        return false;
    }

    if (held_step_reaches_window && has_held_value) {
        include_range(range, has_value, held_range.min, held_range.max);
    }
    return true;
}

} // namespace

namespace detail {

namespace {

bool normalize_draw_component(
    float&                     value,
    Nonfinite_sample_policy    policy)
{
    if (std::isfinite(value)) {
        return true;
    }
    if (policy == Nonfinite_sample_policy::REPLACE_WITH_ZERO) {
        value = 0.0f;
        return true;
    }
    return false;
}

sample_draw_status_t status_for_nonfinite(
    Nonfinite_sample_policy policy)
{
    return policy == Nonfinite_sample_policy::REJECT_WINDOW
        ? sample_draw_status_t::FAILED
        : sample_draw_status_t::SKIPPED;
}

} // namespace

sample_draw_status_t read_sample_draw_value(
    const erased_access_policy_t&  access,
    const void*                    sample,
    Nonfinite_sample_policy        policy,
    sample_draw_value_t&           out)
{
    out = sample_draw_value_t{};
    if (!sample) {
        return sample_draw_status_t::FAILED;
    }

    float y = access.has_value() ? access.value(sample) : 0.0f;
    if (!normalize_draw_component(y, policy)) {
        return status_for_nonfinite(policy);
    }

    float low  = y;
    float high = y;
    if (access.has_range()) {
        std::tie(low, high) = access.range(sample);
    }
    if (!normalize_draw_component(low,  policy) ||
        !normalize_draw_component(high, policy))
    {
        return status_for_nonfinite(policy);
    }
    if (high < low) {
        std::swap(low, high);
    }

    out.y     = y;
    out.y_min = low;
    out.y_max = high;
    return sample_draw_status_t::DRAWABLE;
}

sample_draw_status_t read_sample_draw_value(
    const Data_access_policy&  access,
    const void*                sample,
    Nonfinite_sample_policy    policy,
    sample_draw_value_t&       out)
{
    return read_sample_draw_value(
        make_erased_access_policy_view(access),
        sample,
        policy,
        out);
}

} // namespace detail

std::vector<std::size_t> Data_source::lod_scales() const
{
    return detail::compute_lod_scales(*this);
}

data_query_result_t<value_range_t> Data_source::query_v_range(
    std::size_t                    lod,
    const data_query_context_t&    query)
{
    data_query_result_t<value_range_t> result;
    if (!query.access || !query.access->is_valid()) {
        return result;
    }

    const auto snapshot_result = try_snapshot(lod);
    result.sequence = snapshot_result.snapshot.sequence;
    if (snapshot_result.status != snapshot_result_t::Snapshot_status::READY) {
        result.status = status_from_snapshot(snapshot_result.status);
        return result;
    }
    if (!snapshot_result.snapshot) {
        result.status = invalid_ready_snapshot_status(snapshot_result.snapshot);
        return result;
    }
    if (query.time_window.min_ns > query.time_window.max_ns) {
        result.status = Data_query_status::EMPTY;
        return result;
    }

    if (query.profiler) {
        query.profiler->record_counter("renderer.auto_range.range_scan_count");
    }

    value_range_t range;
    bool          has_value = false;
    if (!scan_value_range(
            snapshot_result.snapshot, *query.access, query, range, has_value))
    {
        result.status = Data_query_status::FAILED;
        return result;
    }

    if (!has_value) {
        result.status = Data_query_status::EMPTY;
        return result;
    }

    result.status = Data_query_status::READY;
    result.value  = range;
    return result;
}

} // namespace vnm::plot
