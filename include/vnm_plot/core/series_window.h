#pragma once

// VNM Plot Library - Core Series Window Planning Types
// Renderer-independent view/window metadata shared by planning APIs.

#include <vnm_plot/core/types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vnm::plot {

enum class Series_view_kind
{
    MAIN,
    PREVIEW
};

enum class Stack_view_state
{
    // No renderer result matches the current series data and view range.
    PENDING,
    // The current view was accepted. A singleton group is active but is not
    // composed with another series.
    ACTIVE,
    // The current view was omitted for the accompanying rejection reason.
    SUPPRESSED,
};

enum class Stack_rejection_reason
{
    NONE,
    MIXED_INTERPOLATION,
    NO_DRAWABLE_DATA,
    INCOMPATIBLE_DATA,
    NONMONOTONIC_TIMESTAMPS,
    NO_COMMON_DOMAIN,
    CUMULATIVE_OVERFLOW,
    OUTPUT_LIMIT,
};

struct Stack_view_status
{
    int                        group     = 0;
    Series_view_kind           view_kind = Series_view_kind::MAIN;
    Stack_view_state           state     = Stack_view_state::PENDING;
    Stack_rejection_reason     reason    = Stack_rejection_reason::NONE;
    std::vector<int>           affected_series_ids;
};

namespace detail {

// Revision and cumulative geometry of one source in a completed stack pass.
// The renderer, feedback channel, and GUI mirror share this representation.
struct Rendered_stack_source
{
    int                    series_id     = 0;
    const Data_source*     source        = nullptr;
    std::size_t            lod           = 0;
    std::uint64_t          sequence      = 0;
    Series_interpolation   interpolation = Series_interpolation::LINEAR;
    data_snapshot_t        cumulative;
};

struct Rendered_stack_status
{
    Stack_view_status                 status;
    std::vector<Rendered_stack_source> sources;
};

} // namespace detail

struct drawable_sample_span_t
{
    std::size_t                source_first = 0;
    std::size_t                source_count = 0;
    std::size_t                gpu_first    = 0;
    std::size_t                gpu_count    = 0;
};

struct sample_window_t
{
    Series_view_kind                       view_kind         = Series_view_kind::MAIN;

    // Consumers receive a valid frame-scoped snapshot for drawable sample
    // windows. If the renderer cannot acquire one, it skips drawable windows
    // instead of passing empty data as drawable.
    data_snapshot_t                        snapshot;
    const Data_access_policy*              access            = nullptr;

    // Source window in `snapshot`, before synthetic draw-only samples.
    // `source_count == 0` means there are no real source samples to draw.
    std::size_t                            source_first      = 0;
    std::size_t                            source_count      = 0;

    // Draw-only hold-forward samples appended after the source window. This is
    // currently zero or one. A synthetic hold sample copies the last source
    // sample's value/range and uses `hold_timestamp_ns` as its GPU timestamp.
    std::size_t                            synthetic_hold_count = 0;

    // Number of compact GPU samples staged by built-in rendering. Non-drawable
    // samples in the source window are omitted unless policy replaces them.
    std::size_t                            gpu_count         = 0;
    std::vector<drawable_sample_span_t>    drawable_spans;
    std::size_t                            lod_level         = 0;
    double                                 pixels_per_sample = 0.0;
    std::uint64_t                          sample_sequence   = 0;
    Series_interpolation                   interpolation     = Series_interpolation::LINEAR;
    bool                                   stacked           = false;
    Nonfinite_sample_policy nonfinite_policy =
        Nonfinite_sample_policy::BREAK_SEGMENT;

    std::int64_t                           t_min_ns     = 0;
    std::int64_t                           t_max_ns     = 0;
    std::int64_t                           t_origin_ns  = 0;

    bool                                   hold_last_forward = false;
    std::int64_t                           hold_timestamp_ns = 0;

    float                                  v_min        = 0.0f;
    float                                  v_max        = 1.0f;
    float                                  width_px     = 0.0f;
    float                                  height_px    = 0.0f;
    float                                  y_offset_px  = 0.0f;
    float                                  window_alpha = 1.0f;
};

struct value_range_plan_t
{
    float                      min   = 0.0f;
    float                      max   = 1.0f;
    bool                       valid = false;
};

struct Planned_snapshot
{
    data_snapshot_t            snapshot;
    std::uint64_t              sequence  = 0;
};

struct Series_view_plan
{
    int                        series_id = 0;
    Series_view_kind           view_kind = Series_view_kind::MAIN;
    Data_source*               source    = nullptr;
    const Data_access_policy*  access    = nullptr;

    std::size_t                lod_level = 0;
    std::size_t                lod_scale = 1;
    Planned_snapshot           snapshot;

    std::size_t                source_first         = 0;
    std::size_t                source_count         = 0;
    std::size_t                synthetic_hold_count = 0;
    std::size_t                gpu_count            = 0;
    std::vector<drawable_sample_span_t>
                               drawable_spans;

    std::int64_t               t_min_ns    = 0;
    std::int64_t               t_max_ns    = 0;
    std::int64_t               t_origin_ns = 0;

    bool                       hold_last_forward = false;
    std::int64_t               hold_timestamp_ns = 0;

    Series_interpolation       interpolation         = Series_interpolation::LINEAR;
    bool                       stacked               = false;
    Empty_window_behavior      empty_window_behavior = Empty_window_behavior::DRAW_NOTHING;
    Nonfinite_sample_policy nonfinite_policy =
        Nonfinite_sample_policy::BREAK_SEGMENT;
    Display_style              style             = Display_style::NONE;

    float                      v_min             = 0.0f;
    float                      v_max             = 1.0f;
    float                      width_px          = 0.0f;
    float                      height_px         = 0.0f;
    float                      y_offset_px       = 0.0f;
    float                      window_alpha      = 1.0f;
    double                     pixels_per_sample = 0.0;
};

struct Frame_range_plan
{
    value_range_plan_t         main_v_range;
    value_range_plan_t         preview_v_range;
};

} // namespace vnm::plot
