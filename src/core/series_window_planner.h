#pragma once

// Internal QRhi-independent visible-window and LOD planning for series views.

#include <vnm_plot/core/series_window.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace vnm::plot {

class Profiler;

namespace detail {

// Timestamp ordering observed in snapshots and selected stack inputs.
enum class Time_order
{
    UNKNOWN,
    ASCENDING,
    DESCENDING,
};

enum class Snapshot_requirement
{
    Optional,
    Frame_snapshot_required
};

enum class Timestamp_window_search
{
    NONE,
    BINARY,
    LINEAR,
};

struct series_window_planner_state_t
{
    static constexpr std::int64_t k_no_timestamp =
        std::numeric_limits<std::int64_t>::min();

    std::size_t                last_snapshot_elements              = 0;
    std::uint64_t              last_sequence                       = 0;
    const void*                cached_data_identity                = nullptr;
    std::uint64_t              last_timestamp_order_sequence       = 0;
    const void*                last_timestamp_order_identity       = nullptr;
    access_policy_cache_key_t  last_timestamp_order_access_key;
    bool                       last_timestamp_order_scan_performed = false;
    std::size_t                last_timestamp_order_scan_samples   = 0;
    bool                       last_timestamps_monotonic           = true;
    Time_order                 last_selected_time_order            = Time_order::UNKNOWN;
    Timestamp_window_search last_timestamp_window_search =
        Timestamp_window_search::NONE;
    access_dispatch_kind_t last_access_dispatch_kind =
        access_dispatch_kind_t::NONE;
    access_policy_cache_key_t  last_access_key;

    std::size_t                last_first         = 0;
    std::size_t                last_count         = 0;
    bool                       has_last_lod_level = false;
    std::size_t                last_lod_level     = 0;
    std::int64_t               last_t_min         = k_no_timestamp;
    std::int64_t               last_t_max         = k_no_timestamp;
    double                     last_width_px      = std::numeric_limits<double>::quiet_NaN();
    Empty_window_behavior last_empty_window_behavior =
        Empty_window_behavior::DRAW_NOTHING;
    Nonfinite_sample_policy last_nonfinite_policy =
        Nonfinite_sample_policy::BREAK_SEGMENT;
    double                     last_applied_pps          = 0.0;
    bool                       last_hold_last_forward    = false;
    Series_interpolation       last_interpolation        = Series_interpolation::LINEAR;
    std::size_t                last_source_count         = 0;
    std::size_t                last_synthetic_hold_count = 0;
    std::vector<drawable_sample_span_t>
                               last_drawable_spans;
    std::int64_t               uploaded_t_origin_ns      = k_no_timestamp;
    bool                       last_plan_reused_upload   = false;
};

struct Series_window_snapshot_cache
{
    std::uint64_t              cached_snapshot_frame_id = 0;
    std::size_t                cached_snapshot_level    = std::numeric_limits<std::size_t>::max();
    const Data_source*         cached_snapshot_source   = nullptr;
    data_snapshot_t            cached_snapshot;
    std::shared_ptr<void>      cached_snapshot_hold;
};

struct series_window_plan_request_t
{
    int                        series_id                = 0;
    Series_view_kind           view_kind                = Series_view_kind::MAIN;
    series_window_planner_state_t*
                               planner_state            = nullptr;
    Series_window_snapshot_cache*
                               snapshot_cache           = nullptr;
    std::uint64_t              frame_id                 = 0;
    Data_source*               data_source              = nullptr;
    const Data_access_policy*  access                   = nullptr;
    const std::vector<std::size_t>*
                               scales                   = nullptr;
    std::int64_t               t_min_ns                 = 0;
    std::int64_t               t_max_ns                 = 0;
    std::int64_t               t_origin_ns              = 0;
    double                     width_px                 = 0.0;
    Empty_window_behavior empty_window_behavior =
        Empty_window_behavior::DRAW_NOTHING;
    Nonfinite_sample_policy nonfinite_policy =
        Nonfinite_sample_policy::BREAK_SEGMENT;
    Display_style              style                = Display_style::NONE;
    Series_interpolation       interpolation        = Series_interpolation::LINEAR;
    Snapshot_requirement       snapshot_requirement = Snapshot_requirement::Optional;
    bool                       has_uploaded_vbo     = false;
    Profiler*                  profiler             = nullptr;
};

Series_view_plan plan_series_window(const series_window_plan_request_t& request);

struct stacked_sample_t
{
    std::int64_t   timestamp_ns = 0;
    float          value        = 0.0f;
    float          base         = 0.0f;
};

inline constexpr std::size_t k_stack_max_output_samples_per_view = 1'048'576;

struct stack_composition_stats_t
{
    std::size_t    timestamp_count   = 0;
    std::size_t    source_read_count = 0;
    bool           resampled         = false;
};

std::size_t stack_timestamp_budget(
    double                                         width_px,
    std::size_t                                    layer_count);

// Compose independently planned LOD windows onto their exact timestamp union
// when it fits, otherwise onto a bounded shared grid. Output layers match input
// order and contain cumulative tops plus bases.
Stack_rejection_reason compose_stacked_series(
    const std::vector<const Series_view_plan*>&    plans,
    std::vector<std::vector<stacked_sample_t>>&    layers,
    std::size_t                                    timestamp_budget,
    stack_composition_stats_t*                     stats = nullptr);

Stack_rejection_reason compose_stacked_series(
    const std::vector<const Series_view_plan*>&    plans,
    const std::vector<Time_order>&                 selected_time_orders,
    std::vector<std::vector<stacked_sample_t>>&    layers,
    std::size_t                                    timestamp_budget,
    stack_composition_stats_t*                     stats = nullptr);

const Data_access_policy& stacked_sample_access();

} // namespace detail
} // namespace vnm::plot
