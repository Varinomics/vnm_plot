#include <vnm_plot/core/types.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/plot_config.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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

struct validated_time_window_t
{
    std::size_t    first                = 0;
    std::size_t    last_exclusive       = 0;
    std::size_t    match_first          = 0;
    std::size_t    match_last_exclusive = 0;
    std::size_t    held_index           = 0;
    bool           has_match            = false;
    bool           has_held             = false;
    bool           valid                = true;
};

struct time_window_candidates_t
{
    std::size_t    match_first          = 0;
    std::size_t    match_last_exclusive = 0;
    std::size_t    held_index           = 0;
    bool           has_held             = false;
    bool           valid                = true;
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

bool timestamp_at(
    const data_snapshot_t&     snapshot,
    const Data_access_policy&  access,
    std::size_t                index,
    std::int64_t&              timestamp_ns)
{
    const void* sample = snapshot.at(index);
    if (!sample) {
        return false;
    }
    timestamp_ns = access.get_timestamp(sample);
    return true;
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

sample_scan_status sample_status_for_staging(
    const Data_access_policy&      access,
    const void*                    sample,
    Nonfinite_sample_policy        policy)
{
    value_range_t ignored;
    return sample_value_range(access, sample, policy, ignored);
}

bool include_sample_range(
    value_range_t&                 range,
    bool&                          has_value,
    const Data_access_policy&      access,
    const void*                    sample,
    Nonfinite_sample_policy        policy)
{
    value_range_t sample_range;
    const sample_scan_status scan_status =
        sample_value_range(access, sample, policy, sample_range);
    if (scan_status == sample_scan_status::FAILED) {
        return false;
    }
    if (scan_status == sample_scan_status::ACCEPTED) {
        include_range(range, has_value, sample_range.min, sample_range.max);
    }
    return true;
}

template<typename Compare>
std::size_t timestamp_bound(
    const data_snapshot_t&    snapshot,
    const Data_access_policy& access,
    std::int64_t              target_ns,
    Compare                   compare,
    bool&                     valid)
{
    const auto index = detail::bsearch_ts_impl(
        snapshot.count,
        [&snapshot](std::size_t index) { return snapshot.at(index); },
        access.get_timestamp,
        compare,
        target_ns);
    valid = index.has_value();
    return index.value_or(0);
}

time_window_candidates_t ascending_candidates(
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    const data_query_context_t&    query,
    bool                           include_visible_step)
{
    bool valid = true;
    time_window_candidates_t out;
    out.match_first = timestamp_bound(snapshot, access, query.time_window.min_ns, std::less<>{}, valid);
    if (!valid) {
        out.valid = false;
        return out;
    }
    out.match_last_exclusive = timestamp_bound(snapshot, access, query.time_window.max_ns, std::less_equal<>{}, valid);
    if (!valid) {
        out.valid = false;
        return out;
    }
    const bool held_step_reaches_window = wants_hold_forward(query) ||
        (include_visible_step && query.interpolation == Series_interpolation::STEP_AFTER &&
            out.match_first < snapshot.count);
    if (held_step_reaches_window && out.match_first > 0) {
        out.has_held   = true;
        out.held_index = out.match_first - 1;
    }
    return out;
}

time_window_candidates_t descending_candidates(
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    const data_query_context_t&    query,
    bool                           include_visible_step)
{
    bool valid = true;
    time_window_candidates_t out;
    out.match_first = timestamp_bound(snapshot, access, query.time_window.max_ns, std::greater<>{}, valid);
    if (!valid) {
        out.valid = false;
        return out;
    }
    out.match_last_exclusive = timestamp_bound(snapshot, access, query.time_window.min_ns, std::greater_equal<>{}, valid);
    if (!valid) {
        out.valid = false;
        return out;
    }
    const bool held_step_reaches_window = wants_hold_forward(query) ||
        (include_visible_step && query.interpolation == Series_interpolation::STEP_AFTER &&
            out.match_last_exclusive > 0);
    if (held_step_reaches_window && out.match_last_exclusive < snapshot.count) {
        out.has_held   = true;
        out.held_index = out.match_last_exclusive;
    }
    return out;
}

time_window_candidates_t linear_candidates(
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    const data_query_context_t&    query)
{
    bool        found           = false;
    bool        gap_after_match = false;
    bool        discontinuous   = false;
    bool        held_found      = false;
    std::size_t held_index      = 0;

    std::int64_t held_timestamp_ns = 0;
    time_window_candidates_t out;
    const bool hold_forward = wants_hold_forward(query);

    for (std::size_t index = 0; index < snapshot.count; ++index) {
        std::int64_t timestamp_ns = 0;
        if (!timestamp_at(snapshot, access, index, timestamp_ns)) {
            out.valid = false;
            return out;
        }

        if (time_window_contains(query.time_window, timestamp_ns)) {
            if (!found) {
                out.match_first = index;
                found = true;
            }
            else
            if (gap_after_match) {
                discontinuous = true;
            }
            out.match_last_exclusive = index + 1;
            gap_after_match = false;
        }
        else
        if (found) {
            gap_after_match = true;
        }

        if (hold_forward && timestamp_ns < query.time_window.min_ns &&
            (!held_found || timestamp_ns > held_timestamp_ns))
        {
            held_found        = true;
            held_index        = index;
            held_timestamp_ns = timestamp_ns;
        }
    }

    if (discontinuous) {
        out.valid = false;
        return out;
    }
    if (held_found) {
        if (found &&
            !(held_index + 1 == out.match_first || held_index == out.match_last_exclusive))
        {
            out.valid = false;
            return out;
        }
        out.has_held   = true;
        out.held_index = held_index;
    }
    return out;
}

time_window_candidates_t time_window_candidates(
    const Data_source&             source,
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    std::size_t                    lod,
    const data_query_context_t&    query,
    bool                           include_visible_step = false)
{
    switch (source.time_order(lod)) {
        case Time_order::ASCENDING:
            return ascending_candidates(snapshot, access, query, include_visible_step);
        case Time_order::DESCENDING:
            return descending_candidates(snapshot, access, query, include_visible_step);
        case Time_order::UNKNOWN:
        case Time_order::UNORDERED:  return linear_candidates(snapshot, access, query);
    }
    return linear_candidates(snapshot, access, query);
}

bool validate_match_range(
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    const data_query_context_t&    query,
    std::size_t                    first,
    std::size_t                    last_exclusive)
{
    for (std::size_t index = first; index < last_exclusive; ++index) {
        const void* sample = snapshot.at(index);
        if (!sample) {
            return false;
        }
        if (sample_status_for_staging(access, sample, query.nonfinite_policy) ==
            sample_scan_status::FAILED)
        {
            return false;
        }
    }
    return true;
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

sample_scan_status held_sample_status(
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    const data_query_context_t&    query,
    std::size_t                    index)
{
    const void* sample = snapshot.at(index);
    if (!sample) {
        return sample_scan_status::FAILED;
    }
    return sample_status_for_staging(access, sample, query.nonfinite_policy);
}

bool select_held_sample_index(
    const data_snapshot_t&         snapshot,
    const Data_access_policy&      access,
    const data_query_context_t&    query,
    Time_order                     source_order,
    std::size_t                    candidate_index,
    std::size_t&                   out_index,
    bool&                          out_failed)
{
    out_failed = false;
    if (candidate_index >= snapshot.count) {
        out_failed = true;
        return false;
    }

    if (query.nonfinite_policy == Nonfinite_sample_policy::SKIP) {
        if (source_order == Time_order::ASCENDING) {
            for (std::size_t offset = candidate_index + 1u; offset > 0; --offset) {
                const std::size_t index = offset - 1u;
                const sample_scan_status status = held_sample_status(
                    snapshot,
                    access,
                    query,
                    index);
                if (status == sample_scan_status::ACCEPTED) {
                    out_index = index;
                    return true;
                }
                if (status == sample_scan_status::FAILED) {
                    out_failed = true;
                    return false;
                }
            }
            return false;
        }

        if (source_order == Time_order::DESCENDING) {
            for (std::size_t index = candidate_index; index < snapshot.count; ++index) {
                const sample_scan_status status = held_sample_status(
                    snapshot,
                    access,
                    query,
                    index);
                if (status == sample_scan_status::ACCEPTED) {
                    out_index = index;
                    return true;
                }
                if (status == sample_scan_status::FAILED) {
                    out_failed = true;
                    return false;
                }
            }
            return false;
        }

        bool found = false;
        std::int64_t held_timestamp_ns = 0;
        for (std::size_t index = 0; index < snapshot.count; ++index) {
            std::int64_t timestamp_ns = 0;
            if (!timestamp_at(snapshot, access, index, timestamp_ns)) {
                out_failed = true;
                return false;
            }
            if (timestamp_ns >= query.time_window.min_ns) {
                continue;
            }
            const sample_scan_status status = held_sample_status(
                snapshot,
                access,
                query,
                index);
            if (status == sample_scan_status::ACCEPTED) {
                if (!found || timestamp_ns > held_timestamp_ns) {
                    found             = true;
                    held_timestamp_ns = timestamp_ns;
                    out_index         = index;
                }
                continue;
            }
            if (status == sample_scan_status::FAILED) {
                out_failed = true;
                return false;
            }
        }
        return found;
    }

    const sample_scan_status status = held_sample_status(
        snapshot,
        access,
        query,
        candidate_index);
    if (status == sample_scan_status::ACCEPTED) {
        out_index = candidate_index;
        return true;
    }
    if (status == sample_scan_status::FAILED) {
        out_failed = true;
    }
    return false;
}

validated_time_window_t validated_time_window(
    const data_snapshot_t&             snapshot,
    const Data_access_policy&          access,
    const data_query_context_t&        query,
    Time_order                         source_order,
    const time_window_candidates_t&    candidates)
{
    validated_time_window_t out;
    if (!candidates.valid) {
        out.valid = false;
        return out;
    }

    const bool has_match =
        candidates.match_first < candidates.match_last_exclusive;
    if (has_match &&
        !validate_match_range(
            snapshot, access, query, candidates.match_first, candidates.match_last_exclusive))
    {
        out.valid = false;
        return out;
    }

    bool        held_accepted = false;
    std::size_t held_index    = candidates.held_index;
    if (candidates.has_held) {
        bool held_failed = false;
        held_accepted = select_held_sample_index(
            snapshot,
            access,
            query,
            source_order,
            candidates.held_index,
            held_index,
            held_failed);
        if (held_failed) {
            out.valid = false;
            return out;
        }
    }

    if (!has_match && !held_accepted) {
        return out;
    }

    if (!has_match) {
        out.first          = held_index;
        out.last_exclusive = held_index + 1;
        out.held_index     = held_index;
        out.has_held       = true;
        return out;
    }

    out.first                = candidates.match_first;
    out.last_exclusive       = candidates.match_last_exclusive;
    out.match_first          = candidates.match_first;
    out.match_last_exclusive = candidates.match_last_exclusive;
    out.has_match            = true;
    if (held_accepted) {
        out.first          = std::min(out.first, held_index);
        out.last_exclusive = std::max(out.last_exclusive, held_index + 1);
        out.held_index     = held_index;
        out.has_held       = true;
    }
    return out;
}

bool selected_by_time_window(const validated_time_window_t& window, std::size_t index)
{
    return (window.has_match &&
        index >= window.match_first &&
        index < window.match_last_exclusive) ||
        (window.has_held && index == window.held_index);
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

Time_order Data_source::time_order(std::size_t lod) const
{
    (void)lod;
    return Time_order::UNKNOWN;
}

data_query_result_t<time_range_t> Data_source::time_range(std::size_t lod) const
{
    (void)lod;
    return {};
}

std::vector<std::size_t> Data_source::lod_scales() const
{
    return detail::compute_lod_scales(*this);
}

data_query_result_t<sample_index_window_t> Data_source::query_time_window(
    std::size_t                    lod,
    const data_query_context_t&    query)
{
    data_query_result_t<sample_index_window_t> result;
    if (!query.access || !query.access->get_timestamp) {
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

    const Time_order order = time_order(lod);
    const time_window_candidates_t candidates = time_window_candidates(
        *this,
        snapshot_result.snapshot,
        *query.access,
        lod,
        query);
    const validated_time_window_t window = validated_time_window(
        snapshot_result.snapshot,
        *query.access,
        query,
        order,
        candidates);
    if (!window.valid) {
        result.status = Data_query_status::FAILED;
        return result;
    }
    if (window.first == window.last_exclusive) {
        result.status = Data_query_status::EMPTY;
        return result;
    }

    result.status = Data_query_status::READY;
    result.value  = {window.first, window.last_exclusive - window.first};
    return result;
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
    bool             has_value = false;
    const Time_order order     = time_order(lod);
    if (order == Time_order::UNKNOWN || order == Time_order::UNORDERED) {
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

    const time_window_candidates_t candidates = time_window_candidates(
        *this,
        snapshot_result.snapshot,
        *query.access,
        lod,
        query,
        true);
    const validated_time_window_t window = validated_time_window(
        snapshot_result.snapshot,
        *query.access,
        query,
        order,
        candidates);
    if (!window.valid) {
        result.status = Data_query_status::FAILED;
        return result;
    }
    if (window.first == window.last_exclusive) {
        result.status = Data_query_status::EMPTY;
        return result;
    }

    for (std::size_t index = window.first; index < window.last_exclusive; ++index) {
        if (!selected_by_time_window(window, index)) {
            continue;
        }

        const void* sample = snapshot_result.snapshot.at(index);
        if (!sample) {
            result.status = Data_query_status::FAILED;
            return result;
        }

        if (!include_sample_range(
                range, has_value, *query.access, sample, query.nonfinite_policy))
        {
            result.status = Data_query_status::FAILED;
            return result;
        }
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
