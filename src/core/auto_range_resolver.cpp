#include "auto_range_resolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vnm::plot::detail {

namespace {

constexpr time_range_t all_time_window()
{
    return {
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()
    };
}

sample_draw_status_t include_sample_range(
    const Data_access_policy&  access,
    const void*                sample,
    Nonfinite_sample_policy    nonfinite_policy,
    float&                     out_min,
    float&                     out_max,
    bool&                      have_any)
{
    sample_draw_value_t draw_value;
    const sample_draw_status_t status =
        read_sample_draw_value(access, sample, nonfinite_policy, draw_value);
    if (status != sample_draw_status_t::DRAWABLE) {
        return status;
    }

    if (!have_any) {
        out_min  = draw_value.y_min;
        out_max  = draw_value.y_max;
        have_any = true;
        return sample_draw_status_t::DRAWABLE;
    }

    out_min = std::min(out_min, draw_value.y_min);
    out_max = std::max(out_max, draw_value.y_max);
    return sample_draw_status_t::DRAWABLE;
}

bool scan_global_series_range(
    Data_source&               source,
    const Data_access_policy&  access,
    std::size_t                level,
    Nonfinite_sample_policy    nonfinite_policy,
    float&                     out_min,
    float&                     out_max)
{
    data_snapshot_t snapshot = source.snapshot(level);
    if (!snapshot.is_valid()) {
        return false;
    }

    bool have_any = false;
    for (std::size_t i = 0; i < snapshot.count; ++i) {
        const void* sample = snapshot.at(i);
        if (!sample || include_sample_range(
                access, sample, nonfinite_policy, out_min, out_max, have_any) ==
                sample_draw_status_t::FAILED)
        {
            return false;
        }
    }
    return have_any;
}

void apply_auto_v_range_padding(
    const Plot_config&                 config,
    bool                               data_range_nonnegative,
    float&                             v_min,
    float&                             v_max)
{
    if (v_max == v_min) {
        const float pad = std::max(std::abs(v_min) * 0.01f, 0.5f);
        v_min -= pad;
        v_max += pad;
    }

    if (config.auto_v_range_extra_scale > 0.0) {
        const double span = double(v_max) - double(v_min);
        if (span > 0.0) {
            const double center      = 0.5 * (double(v_min) + double(v_max));
            const double padded_span = span * (1.0 + config.auto_v_range_extra_scale);
            v_min = static_cast<float>(center - padded_span * 0.5);
            v_max = static_cast<float>(center + padded_span * 0.5);
        }
    }

    if (config.floor_nonnegative_auto_v_range_at_zero &&
        data_range_nonnegative &&
        v_min < 0.0f)
    {
        v_min = 0.0f;
    }
}

data_query_context_t make_query(
    const Data_access_policy&          access,
    time_range_t                       time_window,
    Series_interpolation               interpolation,
    Empty_window_behavior              empty_window_behavior,
    Nonfinite_sample_policy            nonfinite_policy)
{
    data_query_context_t query;
    query.access                = &access;
    query.semantics_key         = make_sample_semantics_key(&access);
    query.time_window           = time_window;
    query.interpolation         = interpolation;
    query.empty_window_behavior = empty_window_behavior;
    query.nonfinite_policy      = nonfinite_policy;
    return query;
}

bool same_cache_shape(
    const auto_range_cache_entry_t&    entry,
    const Data_source&                 source,
    const Data_access_policy&          access,
    std::size_t                        lod_level,
    const data_query_context_t&        query,
    std::uint64_t                      sequence)
{
    const erased_access_policy_t access_view =
        make_erased_access_policy_view(access);
    const access_policy_cache_key_t access_key =
        make_access_policy_cache_key(&access, access_view);
    return (
        entry.valid                                                      &&
        entry.source_identity == source.identity()                       &&
        entry.access_identity == &access                                 &&
        entry.access_key == access_key                                   &&
        entry.layout_key == access.layout_key                            &&
        entry.semantics_value == query.semantics_key.value               &&
        entry.semantics_revision == query.semantics_key.revision         &&
        entry.semantics_conservative == query.semantics_key.conservative &&
        entry.lod_level == lod_level                                     &&
        entry.t_min_ns == query.time_window.min_ns                       &&
        entry.t_max_ns              == query.time_window.max_ns          &&
        entry.interpolation         == query.interpolation               &&
        entry.empty_window_behavior == query.empty_window_behavior       &&
        entry.nonfinite_policy      == query.nonfinite_policy            &&
        entry.sequence              == sequence);
}

auto_range_cache_entry_t make_cache_entry(
    const Data_source&                 source,
    const Data_access_policy&          access,
    std::size_t                        lod_level,
    const data_query_context_t&        query,
    std::uint64_t                      sequence,
    Data_query_status                  status,
    value_range_t                      range)
{
    auto_range_cache_entry_t entry;
    const erased_access_policy_t access_view =
        make_erased_access_policy_view(access);
    entry.source_identity        = source.identity();
    entry.access_identity        = &access;
    entry.access_key             = make_access_policy_cache_key(&access, access_view);
    entry.layout_key             = access.layout_key;
    entry.semantics_value        = query.semantics_key.value;
    entry.semantics_revision     = query.semantics_key.revision;
    entry.semantics_conservative = query.semantics_key.conservative;
    entry.lod_level              = lod_level;
    entry.t_min_ns               = query.time_window.min_ns;
    entry.t_max_ns               = query.time_window.max_ns;
    entry.interpolation          = query.interpolation;
    entry.empty_window_behavior  = query.empty_window_behavior;
    entry.nonfinite_policy       = query.nonfinite_policy;
    entry.sequence               = sequence;
    entry.range                  = range;
    entry.status                 = status;
    entry.valid                  = true;
    return entry;
}

std::map<int, auto_range_cache_entry_t>* cache_entries(
    auto_range_cache_t*                cache,
    bool                               preview)
{
    if (!cache) {
        return nullptr;
    }
    return preview ? &cache->preview_entries : &cache->main_entries;
}

void prune_cache_entries(
    auto_range_cache_t*    cache,
    bool                   preview,
    const std::map<int, std::shared_ptr<const series_data_t>>&
                           series)
{
    std::map<int, auto_range_cache_entry_t>* entries = cache_entries(cache, preview);
    if (!entries) {
        return;
    }

    for (auto it = entries->begin(); it != entries->end();) {
        if (series.find(it->first) == series.end()) {
            it = entries->erase(it);
        }
        else {
            ++it;
        }
    }
}

bool valid_query_range(value_range_t range)
{
    return
        std::isfinite(range.min) &&
        std::isfinite(range.max) &&
        range.min <= range.max;
}

bool query_or_scan_series_range(
    int                        series_id,
    bool                       preview,
    Data_source&               source,
    const Data_access_policy&  access,
    std::size_t                level,
    Series_interpolation       interpolation,
    Empty_window_behavior      empty_window_behavior,
    Nonfinite_sample_policy    nonfinite_policy,
    bool                       visible_only,
    time_range_t               time_window,
    auto_range_cache_t*        cache,
    Profiler*                  profiler,
    float&                     out_min,
    float&                     out_max)
{
    data_query_context_t query = make_query(
        access,
        visible_only ? time_window : all_time_window(),
        interpolation,
        empty_window_behavior,
        nonfinite_policy);
    query.profiler = profiler;

    std::map<int, auto_range_cache_entry_t>* entries = cache_entries(cache, preview);
    const std::uint64_t current_sequence = source.current_sequence(level);

    const bool cacheable_query =
        entries && current_sequence != 0 && !query.semantics_key.conservative;
    if (cacheable_query) {
        const auto found = entries->find(series_id);
        if (found != entries->end() &&
            same_cache_shape(found->second, source, access, level, query, current_sequence))
        {
            if (found->second.status == Data_query_status::EMPTY) {
                return false;
            }
            out_min = found->second.range.min;
            out_max = found->second.range.max;
            return true;
        }
    }

    if (profiler) {
        profiler->record_counter("renderer.auto_range.query_count");
    }
    auto query_result = source.query_v_range(level, query);
    if (query_result.status == Data_query_status::UNSUPPORTED) {
        query_result = source.Data_source::query_v_range(level, query);
    }
    if (query_result.status == Data_query_status::READY) {
        if (!valid_query_range(query_result.value)) {
            return false;
        }
        out_min = query_result.value.min;
        out_max = query_result.value.max;
        if (!query.semantics_key.conservative && entries && query_result.sequence != 0) {
            (*entries)[series_id] = make_cache_entry(
                source,
                access,
                level,
                query,
                query_result.sequence,
                query_result.status,
                query_result.value);
        }
        return true;
    }

    if (query_result.status == Data_query_status::EMPTY) {
        if (!query.semantics_key.conservative && entries && query_result.sequence != 0) {
            (*entries)[series_id] = make_cache_entry(
                source,
                access,
                level,
                query,
                query_result.sequence,
                query_result.status,
                query_result.value);
        }
        return false;
    }

    if (query_result.status == Data_query_status::UNSUPPORTED && !visible_only) {
        if (profiler) {
            profiler->record_counter("renderer.auto_range.range_scan_count");
        }
        return scan_global_series_range(
            source, access, level, nonfinite_policy, out_min, out_max);
    }

    return false;
}

bool resolve_series_collection_range(
    const std::map<int, std::shared_ptr<const series_data_t>>&
                           series,
    const data_config_t&   data_cfg,
    const Plot_config&     config,
    bool                   preview,
    auto_range_cache_t*    cache,
    float&                 out_min,
    float&                 out_max)
{
    bool      have_any = false;
    Profiler* profiler = config.profiler.get();
    const bool visible_only =
        !preview && config.auto_v_range_mode == Auto_v_range_mode::VISIBLE;
    const time_range_t visible_window{data_cfg.t_min, data_cfg.t_max};
    std::map<int, std::pair<float, float>> stack_ranges;
    std::map<int, std::size_t> stack_members;
    for (const auto& [_, item] : series) {
        if (item && item->enabled && item->stack_group != 0 &&
            (preview ? item->preview_source() : item->main_source()))
        {
            ++stack_members[item->stack_group];
        }
    }
    prune_cache_entries(cache, preview, series);

    for (const auto& [id, item] : series) {
        if (!item || !item->enabled) {
            continue;
        }

        Data_source* source = preview ? item->preview_source() : item->main_source();
        if (!source) {
            continue;
        }

        const Data_access_policy& access =
            preview ? item->preview_access() : item->main_access();
        if (!access.get_value && !access.get_range) {
            continue;
        }

        const std::size_t levels = source->lod_levels();
        if (levels == 0) {
            continue;
        }
        const std::size_t level =
            config.auto_v_range_mode == Auto_v_range_mode::GLOBAL_LOD
                ? levels - 1
                : 0;

        float series_min = 0.0f;
        float series_max = 0.0f;
        const bool got_range = query_or_scan_series_range(
            id,
            preview,
            *source,
            access,
            level,
            preview ? item->effective_preview_interpolation() : item->interpolation,
            item->empty_window_behavior,
            item->nonfinite_policy,
            visible_only,
            visible_window,
            cache,
            profiler,
            series_min,
            series_max);
        if (!got_range) {
            continue;
        }

        if (item->stack_group != 0 && stack_members[item->stack_group] > 1) {
            auto& cumulative   = stack_ranges[item->stack_group];
            out_min            = std::min(out_min, std::min(cumulative.first,
                cumulative.first + series_min));
            out_max            = std::max(out_max, std::max(cumulative.second,
                cumulative.second + series_max));
            cumulative.first  += series_min;
            cumulative.second += series_max;
        }
        else {
            out_min = std::min(out_min, series_min);
            out_max = std::max(out_max, series_max);
        }
        have_any = true;
    }

    return have_any;
}

std::pair<float, float> fallback_range(const data_config_t& data_cfg)
{
    return {data_cfg.v_min, data_cfg.v_max};
}

std::pair<float, float> finalize_auto_range(
    const data_config_t&   data_cfg,
    const Plot_config&     config,
    float                  v_min,
    float                  v_max)
{
    if (!std::isfinite(v_min) || !std::isfinite(v_max) || v_max < v_min) {
        return fallback_range(data_cfg);
    }

    const bool data_range_nonnegative = v_min >= 0.0f;
    apply_auto_v_range_padding(config, data_range_nonnegative, v_min, v_max);
    return {v_min, v_max};
}

} // namespace

std::pair<float, float> resolve_main_v_range(
    const std::map<int, std::shared_ptr<const series_data_t>>&
                           series,
    const data_config_t&   data_cfg,
    const Plot_config&     config,
    bool                   v_auto,
    auto_range_cache_t*    cache)
{
    if (!v_auto) {
        return {data_cfg.v_manual_min, data_cfg.v_manual_max};
    }

    float v_min = std::numeric_limits<float>::max();
    float v_max = std::numeric_limits<float>::lowest();
    if (!resolve_series_collection_range(
            series, data_cfg, config, false, cache, v_min, v_max))
    {
        return fallback_range(data_cfg);
    }

    return finalize_auto_range(data_cfg, config, v_min, v_max);
}

std::pair<float, float> resolve_preview_v_range(
    const std::map<int, std::shared_ptr<const series_data_t>>&
                           series,
    const data_config_t&   data_cfg,
    const Plot_config&     config,
    auto_range_cache_t*    cache)
{
    float v_min = std::numeric_limits<float>::max();
    float v_max = std::numeric_limits<float>::lowest();
    if (!resolve_series_collection_range(
            series, data_cfg, config, true, cache, v_min, v_max))
    {
        return fallback_range(data_cfg);
    }

    return finalize_auto_range(data_cfg, config, v_min, v_max);
}

} // namespace vnm::plot::detail
