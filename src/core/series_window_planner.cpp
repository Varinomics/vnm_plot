#include "series_window_planner.h"
#include "checked_size.h"

#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/plot_config.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace vnm::plot::detail {

namespace {

void reset_snapshot_cache(Series_window_snapshot_cache& cache)
{
    cache = Series_window_snapshot_cache{};
}

struct drawable_window_result_t
{
    std::size_t    source_first         = 0;
    std::size_t    source_count         = 0;
    std::size_t    synthetic_hold_count = 0;
    std::size_t    gpu_count            = 0;
    std::vector<drawable_sample_span_t>
                   spans;
    Time_order     timestamp_order      = Time_order::UNKNOWN;
    bool           valid                = true;
};

drawable_window_result_t build_drawable_window(
    const data_snapshot_t&         snapshot,
    const erased_access_policy_t&  access,
    Nonfinite_sample_policy        nonfinite_policy,
    std::size_t                    source_first,
    std::size_t                    source_last_exclusive,
    bool                           hold_last_forward,
    bool                           derive_timestamp_order)
{
    drawable_window_result_t result;
    result.source_first = source_first;
    result.source_count = source_last_exclusive > source_first
        ? source_last_exclusive - source_first
        : 0;
    bool         have_previous_timestamp = false;
    bool         timestamps_ascending    = true;
    bool         timestamps_descending   = true;
    std::int64_t previous_timestamp      = 0;

    if (source_first >= source_last_exclusive) {
        return result;
    }

    const auto append_drawable_source = [&](std::size_t source_index) -> bool {
        const void* sample = snapshot.at(source_index);
        sample_draw_value_t ignored;
        const sample_draw_status_t status = read_sample_draw_value(
            access,
            sample,
            nonfinite_policy,
            ignored);
        if (status == sample_draw_status_t::FAILED)  { return false; }
        if (status == sample_draw_status_t::SKIPPED) { return true;  }

        if (derive_timestamp_order && access.has_timestamp()) {
            const std::int64_t timestamp = access.timestamp(sample);
            if (have_previous_timestamp) {
                timestamps_ascending  = timestamps_ascending  &&
                    timestamp >= previous_timestamp;
                timestamps_descending = timestamps_descending &&
                    timestamp <= previous_timestamp;
            }
            previous_timestamp      = timestamp;
            have_previous_timestamp = true;
        }

        if (result.spans.empty() ||
            result.spans.back().source_first +
                result.spans.back().source_count != source_index)
        {
            drawable_sample_span_t span;
            span.source_first = source_index;
            span.gpu_first    = result.gpu_count;
            result.spans.push_back(span);
        }

        drawable_sample_span_t& span = result.spans.back();
        ++span.source_count;
        ++span.gpu_count;
        ++result.gpu_count;
        return true;
    };

    for (std::size_t i = source_first; i < source_last_exclusive; ++i) {
        if (!append_drawable_source(i)) {
            result.valid = false;
            return result;
        }
    }

    if (hold_last_forward) {
        const bool final_source_sample_drawable =
            !result.spans.empty() &&
            result.spans.back().source_first +
                result.spans.back().source_count == source_last_exclusive;
        const bool can_hold_skipping_trailing_invalid =
            nonfinite_policy == Nonfinite_sample_policy::SKIP &&
            !result.spans.empty();
        if (final_source_sample_drawable ||
            can_hold_skipping_trailing_invalid)
        {
            ++result.spans.back().gpu_count;
            ++result.gpu_count;
            result.synthetic_hold_count = 1;
        }
    }

    if (have_previous_timestamp) {
        result.timestamp_order = timestamps_ascending
            ? Time_order::ASCENDING
            : (timestamps_descending
                ? Time_order::DESCENDING
                : Time_order::UNKNOWN);
    }

    return result;
}

bool select_hold_source_index(
    const data_snapshot_t&         snapshot,
    const erased_access_policy_t&  access,
    Nonfinite_sample_policy        nonfinite_policy,
    std::size_t                    candidate_index,
    std::size_t&                   out_index,
    bool&                          out_failed)
{
    out_failed = false;
    if (candidate_index >= snapshot.count) {
        out_failed = true;
        return false;
    }

    const auto status_at = [&](std::size_t index) {
        const void* sample = snapshot.at(index);
        sample_draw_value_t ignored;
        return read_sample_draw_value(
            access,
            sample,
            nonfinite_policy,
            ignored);
    };

    if (nonfinite_policy == Nonfinite_sample_policy::SKIP) {
        for (std::size_t offset = candidate_index + 1u; offset > 0; --offset) {
            const std::size_t          index  = offset - 1u;
            const sample_draw_status_t status = status_at(index);
            if (status == sample_draw_status_t::DRAWABLE) {
                out_index = index;
                return true;
            }
            if (status == sample_draw_status_t::FAILED) {
                out_failed = true;
                return false;
            }
        }
        return false;
    }

    const sample_draw_status_t status = status_at(candidate_index);
    if (status == sample_draw_status_t::DRAWABLE) {
        out_index = candidate_index;
        return true;
    }
    if (status == sample_draw_status_t::FAILED) {
        out_failed = true;
    }
    return false;
}

} // anonymous namespace

Series_view_plan plan_series_window(const series_window_plan_request_t& request)
{
    Series_view_plan plan;
    plan.series_id             = request.series_id;
    plan.view_kind             = request.view_kind;
    plan.source                = request.data_source;
    plan.access                = request.access;
    plan.lod_scale             = 1;
    plan.interpolation         = request.interpolation;
    plan.empty_window_behavior = request.empty_window_behavior;
    plan.nonfinite_policy      = request.nonfinite_policy;
    plan.style                 = request.style;

    if (request.planner_state) {
        request.planner_state->last_plan_reused_upload = false;
    }

    if (!request.planner_state ||
        !request.snapshot_cache ||
        !request.data_source ||
        !request.access ||
        !request.scales)
    {
        return plan;
    }

    auto&        state          = *request.planner_state;
    auto&        snapshot_cache = *request.snapshot_cache;
    Data_source& data_source    = *request.data_source;

    const Data_access_policy& access = *request.access;
    const erased_access_policy_t access_view =
        make_erased_access_policy_view(access);
    const access_policy_cache_key_t access_key =
        make_access_policy_cache_key(&access, access_view);
    state.last_access_dispatch_kind = access_view.dispatch_kind;
    const std::vector<std::size_t>& scales = *request.scales;
    const auto get_timestamp = [&access_view](const void* sample) {
        return access_view.timestamp(sample);
    };

    if (scales.empty() || request.t_max_ns <= request.t_min_ns || request.width_px <= 0.0) {
        return plan;
    }

    if (snapshot_cache.cached_snapshot_frame_id != request.frame_id) {
        reset_snapshot_cache(snapshot_cache);
    }

    const std::size_t level_count     = scales.size();
    const std::size_t max_level_index = level_count > 0 ? level_count - 1 : 0;
    std::size_t target_level = std::min<std::size_t>(
        state.has_last_lod_level ? state.last_lod_level : 0,
        max_level_index);

    constexpr std::size_t k_tried_stack_levels = 32;
    std::array<std::uint8_t, k_tried_stack_levels> tried_stack{};
    std::vector<std::uint8_t> tried_heap;
    std::uint8_t* tried = nullptr;
    if (level_count <= k_tried_stack_levels) {
        tried = tried_stack.data();
        std::fill(tried, tried + level_count, std::uint8_t{0});
    }
    else {
        tried_heap.assign(level_count, std::uint8_t{0});
        tried = tried_heap.data();
    }

    const auto acquire_frame_snapshot = [&](std::size_t level) {
        VNM_PLOT_PROFILE_SCOPE(request.profiler, "process_view.try_snapshot");
        snapshot_result_t snapshot_result;
        if (snapshot_cache.cached_snapshot_frame_id == request.frame_id &&
            snapshot_cache.cached_snapshot_level    == level            &&
            snapshot_cache.cached_snapshot_source   == &data_source     &&
            snapshot_cache.cached_snapshot)
        {
            snapshot_result.snapshot = snapshot_cache.cached_snapshot;
            snapshot_result.status   = snapshot_result_t::Snapshot_status::READY;
            return snapshot_result;
        }

        snapshot_result = data_source.try_snapshot(level);
        if (snapshot_result) {
            snapshot_cache.cached_snapshot_frame_id = request.frame_id;
            snapshot_cache.cached_snapshot_level    = level;
            snapshot_cache.cached_snapshot_source   = &data_source;
            snapshot_cache.cached_snapshot          = snapshot_result.snapshot;
            snapshot_cache.cached_snapshot_hold     = snapshot_result.snapshot.hold;
        }
        return snapshot_result;
    };

    const auto was_tried = [&](std::size_t level) -> bool {
        return tried && level < level_count && tried[level] != 0;
    };
    const auto mark_tried = [&](std::size_t level) {
        if (tried && level < level_count) {
            tried[level] = 1;
        }
    };

    const auto load_cached_plan = [&](Series_view_plan& p, std::size_t level) {
        p.source_first         = state.last_first;
        p.source_count         = state.last_source_count;
        p.synthetic_hold_count = state.last_synthetic_hold_count;
        p.gpu_count            = state.last_count;
        p.drawable_spans       = state.last_drawable_spans;
        p.lod_level            = level;
        p.lod_scale            = level < scales.size() ? scales[level] : std::size_t{1};
        p.pixels_per_sample    = state.last_applied_pps;
        p.snapshot.sequence    = state.last_sequence;
        p.t_min_ns             = request.t_min_ns;
        p.t_max_ns             = request.t_max_ns;
        p.t_origin_ns          = request.t_origin_ns;
        p.hold_last_forward    = state.last_hold_last_forward;
        p.hold_timestamp_ns    = state.last_hold_last_forward ? request.t_max_ns : 0;
        p.width_px             = static_cast<float>(request.width_px);
        p.interpolation        = request.interpolation;
        p.nonfinite_policy     = request.nonfinite_policy;
    };

    const auto try_stale_fallback =
        [&](Series_view_plan& p, std::size_t level) -> bool {
            const void* current_identity = data_source.identity();
            // The cached VBO holds samples rebased against uploaded_t_origin_ns;
            // reusing it under a moved origin would draw at the wrong x positions
            // because set_common_uniforms feeds the new view_origin_ns regardless.
            const bool identity_ok =
                (state.cached_data_identity != nullptr)                             &&
                (state.cached_data_identity == current_identity)                    &&
                request.has_uploaded_vbo                                            &&
                (state.last_count > 0)                                              &&
                state.has_last_lod_level                                            &&
                (state.last_lod_level == level)                                     &&
                (state.last_access_key == access_key)                               &&
                (state.last_t_min == request.t_min_ns)                              &&
                (state.last_t_max == request.t_max_ns)                              &&
                (state.last_width_px == request.width_px)                           &&
                (state.last_empty_window_behavior == request.empty_window_behavior) &&
                (state.last_nonfinite_policy == request.nonfinite_policy)           &&
                (state.last_interpolation == request.interpolation)                 &&
                (state.uploaded_t_origin_ns == request.t_origin_ns);
            if (!identity_ok) {
                return false;
            }
            load_cached_plan(p, state.last_lod_level);
            state.last_plan_reused_upload = true;
            return true;
        };

    const int max_attempts = static_cast<int>(level_count) + 2;

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const std::size_t applied_level =
            std::min<std::size_t>(target_level, max_level_index);
        if (was_tried(applied_level)) {
            break;
        }
        mark_tried(applied_level);
        const std::size_t applied_scale     = scales[applied_level];
        bool              hold_last_forward = false;

        const std::uint64_t current_seq = data_source.current_sequence(applied_level);
        if (current_seq                      != 0                             &&
            current_seq                      == state.last_sequence           &&
            applied_level                    == state.last_lod_level          &&
            request.has_uploaded_vbo                                          &&
            state.last_count                 >  0                             &&
            state.cached_data_identity       == data_source.identity()        &&
            state.last_access_key            == access_key                    &&
            state.last_t_min                 == request.t_min_ns              &&
            state.last_t_max                 == request.t_max_ns              &&
            state.last_width_px              == request.width_px              &&
            state.last_empty_window_behavior == request.empty_window_behavior &&
            state.last_nonfinite_policy      == request.nonfinite_policy      &&
            state.last_interpolation         == request.interpolation         &&
            state.uploaded_t_origin_ns       == request.t_origin_ns)
        {
            if (request.snapshot_requirement ==
                Snapshot_requirement::Frame_snapshot_required)
            {
                snapshot_result_t snapshot_result = acquire_frame_snapshot(applied_level);
                if (snapshot_result) {
                    if (snapshot_result.snapshot.sequence == current_seq) {
                        load_cached_plan(plan, applied_level);
                        plan.snapshot.snapshot        = snapshot_result.snapshot;
                        plan.snapshot.sequence        = snapshot_result.snapshot.sequence;
                        state.last_plan_reused_upload = true;
                        return plan;
                    }
                    // The source advanced between current_sequence() and
                    // try_snapshot(); fall through to replan against the
                    // newer frame-scoped snapshot instead of pairing cached
                    // window metadata with a different snapshot.
                }
                else {
                    load_cached_plan(plan, applied_level);
                    state.last_plan_reused_upload = true;
                    return plan;
                }
            }
            else {
                load_cached_plan(plan, applied_level);
                state.last_plan_reused_upload = true;
                return plan;
            }
        }

        snapshot_result_t snapshot_result = acquire_frame_snapshot(applied_level);

        if (!snapshot_result || !snapshot_result.snapshot || snapshot_result.snapshot.count == 0) {
            if (try_stale_fallback(plan, applied_level)) {
                break;
            }
            if (applied_level > 0) {
                target_level = applied_level - 1;
                continue;
            }
            break;
        }

        const auto& snapshot = snapshot_result.snapshot;

        bool timestamps_monotonic = true;
        state.last_timestamp_order_scan_performed = false;
        state.last_timestamp_order_scan_samples   = 0;
        state.last_timestamp_window_search        = Timestamp_window_search::NONE;
        if (access_view.has_timestamp()) {
            const void* current_identity = data_source.identity();
            const bool need_monotonicity_scan =
                state.last_timestamp_order_sequence   != snapshot.sequence ||
                state.last_timestamp_order_identity   != current_identity  ||
                state.last_timestamp_order_access_key != access_key;
            if (need_monotonicity_scan) {
                bool is_monotonic = true;
                state.last_timestamp_order_scan_performed = true;
                const void* first_sample = snapshot.at(0);
                ++state.last_timestamp_order_scan_samples;
                if (!first_sample) {
                    is_monotonic = false;
                }
                else {
                    std::int64_t prev_ts = get_timestamp(first_sample);
                    for (std::size_t i = 1; i < snapshot.count; ++i) {
                        const void* sample = snapshot.at(i);
                        ++state.last_timestamp_order_scan_samples;
                        if (!sample) {
                            is_monotonic = false;
                            break;
                        }
                        const std::int64_t ts = get_timestamp(sample);
                        if (ts < prev_ts) {
                            is_monotonic = false;
                            break;
                        }
                        prev_ts = ts;
                    }
                }
                state.last_timestamp_order_sequence   = snapshot.sequence;
                state.last_timestamp_order_identity   = current_identity;
                state.last_timestamp_order_access_key = access_key;
                state.last_timestamps_monotonic       = is_monotonic;
                if (request.profiler) {
                    request.profiler->record_counter(
                        "renderer.series_window.monotonicity_scan_count");
                    request.profiler->record_observation(
                        "renderer.series_window.monotonicity_scan_samples",
                        static_cast<double>(
                            state.last_timestamp_order_scan_samples));
                }
            }
            timestamps_monotonic = state.last_timestamps_monotonic;
        }

        std::size_t  first_idx    = 0;
        std::size_t  last_idx     = snapshot.count;
        std::int64_t last_ts      = 0;
        bool         have_last_ts = false;
        if (access_view.has_timestamp()) {
            const void* last_sample = snapshot.at(snapshot.count - 1);
            if (last_sample) {
                last_ts = get_timestamp(last_sample);
                have_last_ts = true;
            }
            if (!timestamps_monotonic) {
                // Non-monotonic timestamps invalidate binary-search assumptions.
                // Fall back to a linear scan for correctness.
                state.last_timestamp_window_search =
                    Timestamp_window_search::LINEAR;
                VNM_PLOT_PROFILE_SCOPE(
                    request.profiler,
                    "process_view.linear_fallback");
                const visible_sample_window_t window = select_visible_sample_window(
                    snapshot,
                    get_timestamp,
                    request.t_min_ns,
                    request.t_max_ns,
                    false);
                first_idx = window.first;
                last_idx = window.last_exclusive;
            }
            else {
                state.last_timestamp_window_search =
                    Timestamp_window_search::BINARY;
                VNM_PLOT_PROFILE_SCOPE(
                    request.profiler,
                    "process_view.binary_search");
                const visible_sample_window_t window = select_visible_sample_window(
                    snapshot,
                    get_timestamp,
                    request.t_min_ns,
                    request.t_max_ns,
                    true);
                first_idx = window.first;
                last_idx = window.last_exclusive;
            }
        }

        const bool can_hold_last_forward =
            request.empty_window_behavior ==
                Empty_window_behavior::HOLD_LAST_FORWARD &&
            timestamps_monotonic &&
            have_last_ts &&
            last_ts < request.t_max_ns;

        if (first_idx >= last_idx) {
            if (can_hold_last_forward) {
                std::size_t hold_source_index = 0;
                bool        hold_failed       = false;
                if (select_hold_source_index(
                        snapshot, access_view, request.nonfinite_policy,
                        snapshot.count - 1u, hold_source_index, hold_failed))
                {
                    first_idx         = hold_source_index;
                    last_idx          = hold_source_index + 1u;
                    hold_last_forward = true;
                }
                else
                if (hold_failed) {
                    break;
                }
                else
                if (applied_level > 0 && !was_tried(applied_level - 1)) {
                    target_level = applied_level - 1;
                    continue;
                }
                else {
                    break;
                }
            }
            else
            if (applied_level > 0 && !was_tried(applied_level - 1)) {
                target_level = applied_level - 1;
                continue;
            }
            else {
                break;
            }
        }
        else
        if (can_hold_last_forward && last_idx == snapshot.count) {
            hold_last_forward = true;
        }

        if (request.interpolation    == Series_interpolation::STEP_AFTER &&
            request.empty_window_behavior ==
                Empty_window_behavior::HOLD_LAST_FORWARD                 &&
            request.nonfinite_policy == Nonfinite_sample_policy::SKIP    &&
            timestamps_monotonic                                         &&
            first_idx                <  last_idx)
        {
            bool has_drawable_sample_in_requested_window = false;
            bool failed_sample_in_requested_window       = false;
            for (std::size_t index = first_idx; index < last_idx; ++index) {
                const void* sample = snapshot.at(index);
                if (!sample) {
                    failed_sample_in_requested_window = true;
                    break;
                }
                const std::int64_t ts_ns = get_timestamp(sample);
                if (ts_ns >= request.t_min_ns && ts_ns <= request.t_max_ns) {
                    sample_draw_value_t ignored;
                    const sample_draw_status_t status = read_sample_draw_value(
                        access_view,
                        sample,
                        request.nonfinite_policy,
                        ignored);
                    if (status == sample_draw_status_t::FAILED) {
                        failed_sample_in_requested_window = true;
                        break;
                    }
                    if (status == sample_draw_status_t::DRAWABLE) {
                        has_drawable_sample_in_requested_window = true;
                        break;
                    }
                }
            }
            if (failed_sample_in_requested_window) {
                break;
            }
            const void* first_sample = snapshot.at(first_idx);
            if (first_sample &&
                get_timestamp(first_sample) < request.t_min_ns)
            {
                std::size_t hold_source_index = 0;
                bool        hold_failed       = false;
                if (select_hold_source_index(
                        snapshot, access_view, request.nonfinite_policy, first_idx, hold_source_index, hold_failed))
                {
                    if (has_drawable_sample_in_requested_window) {
                        first_idx = std::min(first_idx, hold_source_index);
                    }
                    else {
                        first_idx         = hold_source_index;
                        last_idx          = hold_source_index + 1u;
                        hold_last_forward = true;
                    }
                }
                else
                if (hold_failed) {
                    break;
                }
                else
                if (!has_drawable_sample_in_requested_window) {
                    break;
                }
            }
        }

        const bool derive_drawable_timestamp_order =
            access_view.has_timestamp() && !timestamps_monotonic;
        drawable_window_result_t drawable_window = build_drawable_window(
            snapshot,
            access_view,
            request.nonfinite_policy,
            first_idx,
            last_idx,
            hold_last_forward,
            derive_drawable_timestamp_order);
        if (drawable_window.valid &&
            drawable_window.gpu_count == 0 &&
            hold_last_forward              &&
            request.nonfinite_policy  == Nonfinite_sample_policy::SKIP &&
            last_idx                  >  0)
        {
            std::size_t hold_source_index = 0;
            bool        hold_failed       = false;
            if (select_hold_source_index(
                    snapshot, access_view, request.nonfinite_policy, last_idx - 1u, hold_source_index, hold_failed))
            {
                first_idx       = hold_source_index;
                last_idx        = hold_source_index + 1u;
                drawable_window = build_drawable_window(
                    snapshot,
                    access_view,
                    request.nonfinite_policy,
                    first_idx,
                    last_idx,
                    hold_last_forward,
                    derive_drawable_timestamp_order);
            }
            else
            if (hold_failed) {
                drawable_window.valid = false;
            }
        }
        if (!drawable_window.valid || drawable_window.gpu_count == 0) {
            break;
        }

        const std::size_t source_count  = last_idx - first_idx;
        std::size_t       count_for_lod = 0;
        if (!checked_size_add(
                source_count, hold_last_forward ? 1u : 0u, count_for_lod))
        {
            break;
        }
        std::size_t base_samples = 0;
        if (count_for_lod > 0 &&
            !checked_size_product(count_for_lod, applied_scale, base_samples))
        {
            break;
        }
        const double base_pps = (base_samples > 0)
            ? request.width_px / static_cast<double>(base_samples) : 0.0;

        const std::size_t desired_level = choose_lod_level(scales, base_pps);
        if (desired_level != applied_level) {
            if (!was_tried(desired_level)) {
                target_level = desired_level;
                continue;
            }
        }

        const bool lod_switched =
            state.has_last_lod_level                             &&
            state.cached_data_identity == data_source.identity() &&
            state.last_lod_level != applied_level;

        state.last_sequence          = snapshot.sequence;
        state.cached_data_identity   = data_source.identity();
        state.last_access_key        = access_key;
        state.uploaded_t_origin_ns   = request.t_origin_ns;
        state.last_snapshot_elements = snapshot.count;
        state.last_first             = first_idx;
        state.last_count             = drawable_window.gpu_count;

        state.last_lod_level             = applied_level;
        state.has_last_lod_level         = true;
        state.last_t_min                 = request.t_min_ns;
        state.last_t_max                 = request.t_max_ns;
        state.last_width_px              = request.width_px;
        state.last_empty_window_behavior = request.empty_window_behavior;
        state.last_nonfinite_policy      = request.nonfinite_policy;
        state.last_interpolation         = request.interpolation;
        if (lod_switched && request.profiler) {
            request.profiler->record_counter(
                "renderer.series_window.lod_switch_count");
        }

        const bool effective_hold_last_forward =
            drawable_window.synthetic_hold_count > 0;
        Time_order selected_time_order = Time_order::UNKNOWN;
        if (access_view.has_timestamp()) {
            selected_time_order =
                timestamps_monotonic
                    ? Time_order::ASCENDING
                    : drawable_window.timestamp_order;
        }
        if (effective_hold_last_forward &&
            selected_time_order != Time_order::UNKNOWN &&
            !drawable_window.spans.empty())
        {
            const drawable_sample_span_t& final_span =
                drawable_window.spans.back();
            const std::size_t final_index =
                final_span.source_first + final_span.source_count - 1u;
            const void* final_sample = snapshot.at(final_index);
            if (!final_sample) {
                selected_time_order = Time_order::UNKNOWN;
            }
            else {
                const std::int64_t final_timestamp = get_timestamp(final_sample);
                const bool hold_preserves_order =
                    selected_time_order == Time_order::ASCENDING
                        ? request.t_max_ns >= final_timestamp
                        : request.t_max_ns <= final_timestamp;
                if (!hold_preserves_order) {
                    selected_time_order = Time_order::UNKNOWN;
                }
            }
        }

        plan.source_first            = state.last_first;
        plan.source_count            = drawable_window.source_count;
        plan.synthetic_hold_count    = drawable_window.synthetic_hold_count;
        plan.gpu_count               = drawable_window.gpu_count;
        plan.drawable_spans          = drawable_window.spans;
        plan.lod_level               = applied_level;
        plan.lod_scale               = applied_scale;
        plan.pixels_per_sample       = base_pps * static_cast<double>(applied_scale);
        state.last_applied_pps       = plan.pixels_per_sample;
        state.last_hold_last_forward = effective_hold_last_forward;
        plan.snapshot.snapshot       = snapshot;
        plan.snapshot.sequence       = snapshot.sequence;
        plan.t_min_ns                = request.t_min_ns;
        plan.t_max_ns                = request.t_max_ns;
        plan.t_origin_ns             = request.t_origin_ns;
        plan.hold_last_forward       = effective_hold_last_forward;
        plan.hold_timestamp_ns       = effective_hold_last_forward
            ? request.t_max_ns
            : 0;
        plan.width_px                = static_cast<float>(request.width_px);
        plan.interpolation           = request.interpolation;

        state.last_source_count         = plan.source_count;
        state.last_synthetic_hold_count = plan.synthetic_hold_count;
        state.last_drawable_spans       = plan.drawable_spans;
        state.last_selected_time_order  = selected_time_order;
        break;
    }

    return plan;
}

std::size_t stack_timestamp_budget(double width_px, std::size_t layer_count)
{
    if (layer_count == 0) {
        return 0;
    }
    const std::size_t output_limited =
        k_stack_max_output_samples_per_view / layer_count;
    if (output_limited == 0 || !(width_px > 0.0) || !std::isfinite(width_px)) {
        return output_limited;
    }

    const double ceil_width = std::ceil(width_px);
    if (ceil_width > static_cast<double>((output_limited - 1u) / 2u)) {
        return output_limited;
    }
    const auto  width         = static_cast<std::size_t>(ceil_width);
    std::size_t doubled       = 0;
    std::size_t pixel_limited = 0;
    if (!checked_size_product(width,   2u, doubled) ||
        !checked_size_add(doubled, 1u, pixel_limited))
    {
        return output_limited;
    }
    return std::min(pixel_limited, output_limited);
}

namespace {

struct stack_point_t
{
    std::int64_t   t             = 0;
    float          v             = 0.0f;
};

struct stack_source_view_t
{
    const Series_view_plan*    plan                = nullptr;
    erased_access_policy_t     access;
    std::size_t                source_first        = 0;
    std::size_t                source_count        = 0;
    bool                       reversed            = false;
    bool                       has_hold            = false;
    stack_point_t              hold;
    std::int64_t               first_t             = 0;
    std::int64_t               last_t              = 0;
    Time_order                 selected_time_order = Time_order::UNKNOWN;
    std::size_t*               source_reads        = nullptr;

    bool read(std::size_t offset, bool with_value, stack_point_t& out) const
    {
        if (source_reads &&
            *source_reads < std::numeric_limits<std::size_t>::max())
        {
            ++*source_reads;
        }
        const void* sample = plan->snapshot.snapshot.at(source_first + offset);
        if (!sample) {
            return false;
        }
        out.t = access.timestamp(sample);
        if (!with_value) {
            return true;
        }
        sample_draw_value_t value;
        if (read_sample_draw_value(
                access, sample, plan->nonfinite_policy, value) !=
            sample_draw_status_t::DRAWABLE)
        {
            return false;
        }
        out.v = value.y;
        return true;
    }

    std::size_t logical_count() const
    {
        return source_count + (has_hold ? 1u : 0u);
    }

    bool read_logical(
        std::size_t    offset,
        bool           with_value,
        stack_point_t& out) const
    {
        if (!reversed) {
            if (offset < source_count) {
                return read(offset, with_value, out);
            }
            if (has_hold && offset == source_count) {
                out = hold;
                return true;
            }
            return false;
        }

        if (has_hold) {
            if (offset == 0) {
                out = hold;
                return true;
            }
            --offset;
        }
        return offset < source_count
            ? read(source_count - 1u - offset, with_value, out)
            : false;
    }
};

Stack_rejection_reason make_stack_source_view(
    const Series_view_plan&    plan,
    Time_order                 selected_time_order,
    stack_source_view_t&       view,
    std::size_t*               source_reads)
{
    if (!plan.snapshot.snapshot || !plan.access || !plan.access->get_value) {
        return Stack_rejection_reason::NO_DRAWABLE_DATA;
    }
    if (plan.drawable_spans.size() != 1) {
        return Stack_rejection_reason::INCOMPATIBLE_DATA;
    }

    const drawable_sample_span_t& span = plan.drawable_spans.front();
    if (span.source_count == 0) {
        return Stack_rejection_reason::NO_DRAWABLE_DATA;
    }
    std::size_t source_end = 0;
    if (!checked_size_add(span.source_first, span.source_count, source_end) ||
        source_end > plan.snapshot.snapshot.count)
    {
        return Stack_rejection_reason::INCOMPATIBLE_DATA;
    }

    view.plan                = &plan;
    view.access              = make_erased_access_policy_view(*plan.access);
    view.source_first        = span.source_first;
    view.source_count        = span.source_count;
    view.selected_time_order = selected_time_order;
    view.source_reads        = source_reads;

    stack_point_t first;
    stack_point_t last;
    if (!view.read(0, false, first) ||
        !view.read(span.source_count - 1u, false, last))
    {
        return Stack_rejection_reason::INCOMPATIBLE_DATA;
    }

    if (span.gpu_count == span.source_count + 1u &&
        last.t         != plan.hold_timestamp_ns)
    {
        if (!view.read(span.source_count - 1u, true, last)) {
            return Stack_rejection_reason::INCOMPATIBLE_DATA;
        }
        view.has_hold = true;
        view.hold     = {plan.hold_timestamp_ns, last.v};
        last          = view.hold;
    }

    view.reversed = selected_time_order == Time_order::DESCENDING ||
        (selected_time_order == Time_order::UNKNOWN && first.t > last.t);
    view.first_t  = view.reversed ? last.t  : first.t;
    view.last_t   = view.reversed ? first.t : last.t;
    return Stack_rejection_reason::NONE;
}

std::size_t ceil_log2(std::size_t value)
{
    std::size_t result = 0;
    if (value > 0) {
        --value;
    }
    while (value > 0) {
        ++result;
        value >>= 1u;
    }
    return result;
}

bool lower_bound_timestamp(
    const stack_source_view_t& source,
    std::int64_t               timestamp,
    std::size_t&               offset)
{
    std::size_t first = 0;
    std::size_t last  = source.logical_count();
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2u;
        stack_point_t point;
        if (!source.read_logical(middle, false, point)) { return false;        }
        if (point.t < timestamp)                        { first = middle + 1u; }
        else {
            last = middle;
        }
    }
    offset = first;
    return true;
}

bool upper_bound_timestamp(
    const stack_source_view_t& source,
    std::int64_t               timestamp,
    std::size_t&               offset)
{
    std::size_t first = 0;
    std::size_t last  = source.logical_count();
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2u;
        stack_point_t point;
        if (!source.read_logical(middle, false, point)) { return false;  }
        if (timestamp < point.t)                        { last = middle; }
        else {
            first = middle + 1u;
        }
    }
    offset = first;
    return true;
}

bool binary_stack_value(
    const stack_source_view_t& source,
    Series_interpolation       interpolation,
    std::int64_t               timestamp,
    float&                     value)
{
    const std::size_t count       = source.logical_count();
    std::size_t       current_end = 0;
    if (!upper_bound_timestamp(source, timestamp, current_end) ||
        current_end == 0)
    {
        return false;
    }

    stack_point_t current_probe;
    if (!source.read_logical(current_end - 1u, false, current_probe)) {
        return false;
    }
    std::size_t current_offset = current_end - 1u;
    if (source.reversed &&
        !lower_bound_timestamp(source, current_probe.t, current_offset))
    {
        return false;
    }

    stack_point_t current;
    if (!source.read_logical(current_offset, true, current)) {
        return false;
    }
    value = current.v;
    if (interpolation != Series_interpolation::LINEAR || current_end >= count ||
        timestamp <= current.t)
    {
        return true;
    }

    stack_point_t next_probe;
    if (!source.read_logical(current_end, false, next_probe)) {
        return false;
    }
    std::size_t next_offset = current_end;
    if (!source.reversed) {
        std::size_t next_end = 0;
        if (!upper_bound_timestamp(source, next_probe.t, next_end) ||
            next_end == 0)
        {
            return false;
        }
        next_offset = next_end - 1u;
    }

    stack_point_t next;
    if (!source.read_logical(next_offset, true, next)) {
        return false;
    }
    const double position =
        normalized_time_position_ns(current.t, timestamp, next.t);
    value = static_cast<float>(
        current.v + position * (next.v - current.v));
    return true;
}

bool prefer_binary_stack_lookup(
    const stack_source_view_t& source,
    std::size_t                timestamp_count)
{
    if (source.selected_time_order == Time_order::UNKNOWN) {
        return false;
    }
    const std::size_t search_steps = std::max<std::size_t>(
        ceil_log2(source.logical_count()), 1u);
    std::size_t reads_per_timestamp = 0;
    std::size_t estimated_reads     = 0;
    return
        checked_size_product(search_steps, 2u, reads_per_timestamp)    &&
        checked_size_add(reads_per_timestamp, 4u, reads_per_timestamp) &&
        checked_size_product(
            timestamp_count, reads_per_timestamp, estimated_reads) &&
        estimated_reads < source.source_count;
}

struct stack_source_cursor_t
{
    const stack_source_view_t* source       = nullptr;
    std::size_t                raw_offset   = 0;
    bool                       hold_pending = false;
    bool                       with_value   = false;
    bool                       failed       = false;
    bool                       nonmonotonic = false;
    bool                       emitted      = false;
    bool                       has_pending  = false;
    std::int64_t               last_t       = 0;
    bool                       has_current  = false;
    bool                       has_next     = false;
    stack_point_t              current;
    stack_point_t              next;
    stack_point_t              pending;

    bool accept(stack_point_t& out)
    {
        if (emitted && out.t < last_t) {
            nonmonotonic = true;
        }
        emitted = true;
        last_t  = out.t;
        return true;
    }

    bool read_next(stack_point_t& out)
    {
        if (source->reversed && hold_pending) {
            hold_pending = false;
            out          = source->hold;
            return accept(out);
        }

        if (!source->reversed &&
            (has_pending || raw_offset < source->source_count))
        {
            if (has_pending) {
                out         = pending;
                has_pending = false;
            }
            else
            if (!source->read(raw_offset++, with_value, out)) {
                failed = true;
                return false;
            }
            while (raw_offset < source->source_count) {
                stack_point_t candidate;
                if (!source->read(raw_offset++, with_value, candidate)) {
                    failed = true;
                    return false;
                }
                if (candidate.t != out.t) {
                    pending     = candidate;
                    has_pending = true;
                    break;
                }
                if (with_value) {
                    out.v = candidate.v;
                }
            }
            return accept(out);
        }

        if (source->reversed && (has_pending || raw_offset > 0)) {
            if (has_pending) {
                out         = pending;
                has_pending = false;
            }
            else
            if (!source->read(--raw_offset, with_value, out)) {
                failed = true;
                return false;
            }
            while (raw_offset > 0) {
                stack_point_t candidate;
                if (!source->read(--raw_offset, with_value, candidate)) {
                    failed = true;
                    return false;
                }
                if (candidate.t != out.t) {
                    pending     = candidate;
                    has_pending = true;
                    break;
                }
            }
            return accept(out);
        }

        if (!source->reversed && hold_pending) {
            hold_pending = false;
            out          = source->hold;
            return accept(out);
        }
        return false;
    }

    bool reset(const stack_source_view_t& view, bool read_values)
    {
        source       = &view;
        raw_offset   = view.reversed ? view.source_count : 0;
        hold_pending = view.has_hold;
        with_value   = read_values;
        failed       = false;
        nonmonotonic = false;
        emitted      = false;
        has_pending  = false;
        last_t       = 0;
        has_current  = read_next(current);
        has_next     = has_current && read_next(next);
        return has_current && !failed;
    }

    void advance()
    {
        current     = next;
        has_current = has_next;
        has_next    = has_current && read_next(next);
    }
};

Stack_rejection_reason validate_stack_sources(
    const std::vector<stack_source_view_t>&    sources,
    std::size_t                                source_count)
{
    for (std::size_t i = 0; i < source_count; ++i) {
        const auto& source = sources[i];
        stack_source_cursor_t cursor;
        if (!cursor.reset(source, true)) {
            return Stack_rejection_reason::INCOMPATIBLE_DATA;
        }
        while (cursor.has_next) {
            cursor.advance();
        }
        if (cursor.failed) {
            return Stack_rejection_reason::INCOMPATIBLE_DATA;
        }
        if (cursor.nonmonotonic) {
            return Stack_rejection_reason::NONMONOTONIC_TIMESTAMPS;
        }
    }
    return Stack_rejection_reason::NONE;
}

Stack_rejection_reason validate_stack_sources(
    const std::vector<stack_source_view_t>& sources)
{
    return validate_stack_sources(sources, sources.size());
}

Stack_rejection_reason validated_rejection(
    const std::vector<stack_source_view_t>&        sources,
    Stack_rejection_reason                         fallback)
{
    const Stack_rejection_reason reason = validate_stack_sources(sources);
    return reason == Stack_rejection_reason::NONE ? fallback : reason;
}

Stack_rejection_reason compose_stacked_series_impl(
    const std::vector<const Series_view_plan*>&    plans,
    const std::vector<Time_order>*                 selected_time_orders,
    std::vector<std::vector<stacked_sample_t>>&    layers,
    std::size_t                                    timestamp_budget,
    stack_composition_stats_t*                     stats)
{
    layers.clear();
    if (timestamp_budget == 0) {
        return Stack_rejection_reason::OUTPUT_LIMIT;
    }
    if (plans.size() < 2) {
        return Stack_rejection_reason::NO_DRAWABLE_DATA;
    }
    if (std::any_of(plans.begin(), plans.end(), [](const auto* plan) { return !plan; })) {
        return Stack_rejection_reason::NO_DRAWABLE_DATA;
    }
    const Series_interpolation interpolation = plans.front()->interpolation;
    if (std::any_of(plans.begin(), plans.end(),
        [interpolation](const auto* plan) { return plan->interpolation != interpolation; }))
    {
        return Stack_rejection_reason::MIXED_INTERPOLATION;
    }

    std::size_t                      source_read_count = 0;
    std::vector<stack_source_view_t> sources(plans.size());
    for (std::size_t layer = 0; layer < plans.size(); ++layer) {
        const Time_order selected_time_order =
            selected_time_orders && layer < selected_time_orders->size()
                ? (*selected_time_orders)[layer]
                : Time_order::UNKNOWN;
        const Stack_rejection_reason reason = make_stack_source_view(
            *plans[layer],
            selected_time_order,
            sources[layer],
            &source_read_count);
        if (reason != Stack_rejection_reason::NONE) {
            const Stack_rejection_reason prior_reason =
                validate_stack_sources(sources, layer);
            return prior_reason == Stack_rejection_reason::NONE
                ? reason
                : prior_reason;
        }
    }

    std::vector<std::int64_t> timestamps;
    std::int64_t overlap_start = sources.front().first_t;
    std::int64_t overlap_end   = sources.front().last_t;
    for (const auto& source : sources) {
        overlap_start = std::max(overlap_start, source.first_t);
        overlap_end   = std::min(overlap_end, source.last_t);
    }
    if (overlap_end < overlap_start) {
        return validated_rejection(
            sources, Stack_rejection_reason::NO_COMMON_DOMAIN);
    }

    bool                     resampled = false;
    std::vector<stack_source_cursor_t> merge_cursors(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!merge_cursors[i].reset(sources[i], false)) {
            return Stack_rejection_reason::INCOMPATIBLE_DATA;
        }
        while (merge_cursors[i].has_current &&
            merge_cursors[i].current.t < overlap_start)
        {
            merge_cursors[i].advance();
        }
        if (merge_cursors[i].failed || merge_cursors[i].nonmonotonic) {
            return validated_rejection(
                sources, Stack_rejection_reason::INCOMPATIBLE_DATA);
        }
    }
    for (;;) {
        bool         found = false;
        std::int64_t next  = 0;
        for (const auto& cursor : merge_cursors) {
            if (!cursor.has_current) {
                continue;
            }
            const std::int64_t candidate = cursor.current.t;
            if (!found || candidate < next) {
                next  = candidate;
                found = true;
            }
        }
        if (!found)             { break; }
        if (next > overlap_end) { break; }
        if (next >= overlap_start) {
            if (timestamps.size() == timestamp_budget) {
                resampled = true;
                break;
            }
            timestamps.push_back(next);
        }
        for (auto& cursor : merge_cursors) {
            while (cursor.has_current && cursor.current.t == next) {
                cursor.advance();
            }
            if (cursor.failed || cursor.nonmonotonic) {
                return validated_rejection(
                    sources, Stack_rejection_reason::INCOMPATIBLE_DATA);
            }
        }
    }

    if (resampled) {
        timestamps.clear();
        if (overlap_start == overlap_end) {
            timestamps.push_back(overlap_start);
        }
        else {
            if (timestamp_budget < 2) {
                return validated_rejection(
                    sources, Stack_rejection_reason::OUTPUT_LIMIT);
            }
            const auto span = positive_span_ns(overlap_start, overlap_end);
            if (!span) {
                return validated_rejection(
                    sources, Stack_rejection_reason::OUTPUT_LIMIT);
            }
            const auto          denominator = static_cast<std::uint64_t>(timestamp_budget - 1u);
            const std::uint64_t step        = *span / denominator;
            const std::uint64_t remainder   = *span % denominator;
            std::uint64_t       offset      = 0;
            std::uint64_t       carry       = 0;
            timestamps.reserve(timestamp_budget);
            for (std::size_t i = 0; i < timestamp_budget; ++i) {
                timestamps.push_back(saturating_add_duration_ns(overlap_start, offset));
                if (i + 1u == timestamp_budget) {
                    break;
                }
                offset += step;
                carry  += remainder;
                if (carry >= denominator) {
                    ++offset;
                    carry -= denominator;
                }
            }
            if (timestamps.back() != overlap_end) {
                return validated_rejection(
                    sources, Stack_rejection_reason::OUTPUT_LIMIT);
            }
        }
    }

    std::size_t output_samples = 0;
    if (!checked_size_product(plans.size(), timestamps.size(), output_samples) ||
        output_samples > k_stack_max_output_samples_per_view)
    {
        return validated_rejection(
            sources, Stack_rejection_reason::OUTPUT_LIMIT);
    }
    if (stats) {
        stats->timestamp_count = timestamps.size();
        stats->resampled       = resampled;
    }

    layers.assign(plans.size(), {});
    for (auto& layer : layers) {
        layer.reserve(timestamps.size());
    }
    for (std::size_t layer = 0; layer < sources.size(); ++layer) {
        if (resampled &&
            prefer_binary_stack_lookup(sources[layer], timestamps.size()))
        {
            for (std::size_t sample = 0; sample < timestamps.size(); ++sample) {
                float value = 0.0f;
                if (!binary_stack_value(
                        sources[layer], plans[layer]->interpolation, timestamps[sample], value))
                {
                    layers.clear();
                    if (stats) {
                        *stats = {};
                    }
                    return Stack_rejection_reason::INCOMPATIBLE_DATA;
                }
                const float base = layer == 0
                    ? 0.0f
                    : layers[layer - 1u][sample].value;
                const float cumulative = base + value;
                if (!std::isfinite(cumulative)) {
                    layers.clear();
                    if (stats) {
                        *stats = {};
                    }
                    return Stack_rejection_reason::CUMULATIVE_OVERFLOW;
                }
                layers[layer].push_back({
                    timestamps[sample], cumulative, base});
            }
            continue;
        }

        stack_source_cursor_t cursor;
        if (!cursor.reset(sources[layer], true)) {
            layers.clear();
            if (stats) {
                *stats = {};
            }
            return validate_stack_sources(sources);
        }
        for (std::size_t sample = 0; sample < timestamps.size(); ++sample) {
            const std::int64_t timestamp = timestamps[sample];
            while (cursor.has_next && cursor.next.t <= timestamp) {
                cursor.advance();
            }
            if (cursor.failed || cursor.nonmonotonic || !cursor.has_current) {
                layers.clear();
                if (stats) {
                    *stats = {};
                }
                return validate_stack_sources(sources);
            }

            float value = cursor.current.v;
            if (plans[layer]->interpolation == Series_interpolation::LINEAR &&
                cursor.has_next && timestamp > cursor.current.t)
            {
                const stack_point_t& a = cursor.current;
                const stack_point_t& b = cursor.next;
                const double position  =
                    normalized_time_position_ns(a.t, timestamp, b.t);
                value = static_cast<float>(a.v + position * (b.v - a.v));
            }

            const float base = layer == 0
                ? 0.0f
                : layers[layer - 1u][sample].value;
            const float cumulative = base + value;
            if (!std::isfinite(cumulative)) {
                layers.clear();
                const Stack_rejection_reason input_reason =
                    validate_stack_sources(sources);
                if (input_reason != Stack_rejection_reason::NONE) {
                    if (stats) {
                        *stats = {};
                    }
                    return input_reason;
                }
                if (stats) {
                    *stats = {};
                }
                return Stack_rejection_reason::CUMULATIVE_OVERFLOW;
            }
            layers[layer].push_back({timestamp, cumulative, base});
        }
        while (cursor.has_next) {
            cursor.advance();
        }
        if (cursor.failed || cursor.nonmonotonic) {
            layers.clear();
            if (stats) {
                *stats = {};
            }
            return validate_stack_sources(sources);
        }
    }
    if (timestamps.empty()) {
        return Stack_rejection_reason::NO_DRAWABLE_DATA;
    }
    if (stats) {
        stats->source_read_count = source_read_count;
    }
    return Stack_rejection_reason::NONE;
}

Stack_rejection_reason compose_stacked_series_checked(
    const std::vector<const Series_view_plan*>&    plans,
    const std::vector<Time_order>*                 selected_time_orders,
    std::vector<std::vector<stacked_sample_t>>&    layers,
    std::size_t                                    timestamp_budget,
    stack_composition_stats_t*                     stats)
{
    if (stats) {
        *stats = {};
    }
    try {
        return compose_stacked_series_impl(
            plans, selected_time_orders, layers, timestamp_budget, stats);
    }
    catch (const std::bad_alloc&) {
        layers.clear();
        if (stats) {
            *stats = {};
        }
        return Stack_rejection_reason::OUTPUT_LIMIT;
    }
    catch (const std::length_error&) {
        layers.clear();
        if (stats) {
            *stats = {};
        }
        return Stack_rejection_reason::OUTPUT_LIMIT;
    }
}

} // anonymous namespace

Stack_rejection_reason compose_stacked_series(
    const std::vector<const Series_view_plan*>&    plans,
    std::vector<std::vector<stacked_sample_t>>&    layers,
    std::size_t                                    timestamp_budget,
    stack_composition_stats_t*                     stats)
{
    return compose_stacked_series_checked(
        plans, nullptr, layers, timestamp_budget, stats);
}

Stack_rejection_reason compose_stacked_series(
    const std::vector<const Series_view_plan*>&    plans,
    const std::vector<Time_order>&                 selected_time_orders,
    std::vector<std::vector<stacked_sample_t>>&    layers,
    std::size_t                                    timestamp_budget,
    stack_composition_stats_t*                     stats)
{
    return compose_stacked_series_checked(
        plans, &selected_time_orders, layers, timestamp_budget, stats);
}

const Data_access_policy& stacked_sample_access()
{
    static const Data_access_policy access = [] {
        Data_access_policy policy;
        policy.get_timestamp = [](const void* sample) {
            return static_cast<const stacked_sample_t*>(sample)->timestamp_ns;
        };
        policy.get_value = [](const void* sample) {
            return static_cast<const stacked_sample_t*>(sample)->value;
        };
        policy.get_range = [](const void* sample) {
            const auto& value = *static_cast<const stacked_sample_t*>(sample);
            return std::make_pair(value.base, value.value);
        };
        return policy;
    }();
    return access;
}

} // namespace vnm::plot::detail
