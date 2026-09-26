#include <vnm_plot/core/layout_calculator.h>
#include <vnm_plot/core/algo.h>
#include <vnm_plot/core/constants.h>
#include <vnm_plot/core/plot_config.h>
#include <vnm_plot/core/time_units.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vnm::plot {
using detail::any_fractional_at_precision;
using detail::build_time_steps_covering;
using detail::find_time_step_start_index;
using detail::get_shift;
using detail::k_text_margin_px;
using detail::trim_trailing_zero_decimals;

namespace {

int fixed_digits_for_step(double step)
{
    if (!(step > 0.0) || !std::isfinite(step)) {
        return 0;
    }
    return std::max(0, static_cast<int>(std::ceil(-std::log10(step))));
}

template<typename T>
static auto to_ieee_bits(T value)
{
    using Bits = std::conditional_t<std::is_same_v<T, float>, uint32_t, uint64_t>;
    static_assert(sizeof(Bits) == sizeof(T), "size mismatch");
    Bits bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::int64_t saturating_round_to_int64(long double value) noexcept
{
    constexpr long double k_min =
        static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    constexpr long double k_max =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());

    if (std::isnan(value)) {
        return 0;
    }
    if (!std::isfinite(value)) {
        return value < 0.0L
            ? std::numeric_limits<std::int64_t>::min()
            : std::numeric_limits<std::int64_t>::max();
    }

    const long double rounded = std::round(value);
    if (rounded <= k_min) { return std::numeric_limits<std::int64_t>::min(); }
    if (rounded >= k_max) { return std::numeric_limits<std::int64_t>::max(); }
    return static_cast<std::int64_t>(rounded);
}

std::int64_t saturating_seconds_to_ns(double seconds) noexcept
{
    constexpr long double k_ns_per_second = 1.0e9L;
    return saturating_round_to_int64(
        static_cast<long double>(seconds) * k_ns_per_second);
}

struct Cached_label
{
    std::string    bytes;
    float          width = 0.0f;
};

// Thread-local timestamp label cache
class Timestamp_label_cache
{
public:
    void set_context(
        double     step,
        double     range,
        uint64_t   measure_key,
        float      monospace_advance,
        bool       monospace_advance_reliable,
        double     adjusted_font_size,
        size_t     format_signature)
    {
        if (!std::isfinite(step))               { step               = 0.0;  }
        if (!std::isfinite(range))              { range              = 0.0;  }
        if (!std::isfinite(monospace_advance))  { monospace_advance  = 0.0f; }
        if (!std::isfinite(adjusted_font_size)) { adjusted_font_size = 0.0;  }

        const auto step_bits    = to_ieee_bits(step);
        const auto range_bits   = to_ieee_bits(range);
        const auto advance_bits = to_ieee_bits(monospace_advance);
        const auto font_bits    = to_ieee_bits(adjusted_font_size);

        const Context_key key{
            step_bits,
            range_bits,
            advance_bits,
            static_cast<uint8_t>(monospace_advance_reliable ? 1u : 0u),
            measure_key,
            font_bits,
            format_signature
        };

        auto it = m_contexts.find(key);
        if (it == m_contexts.end()) {
            if (m_contexts.size() >= k_max_contexts && !m_lru.empty()) {
                m_contexts.erase(m_lru.front());
                m_lru.erase(m_lru.begin());
            }
            it = m_contexts.emplace(key, Context_data{}).first;
            m_lru.push_back(key);
        }
        else {
            auto pos = std::find(m_lru.begin(), m_lru.end(), key);
            if (pos != m_lru.end() && std::next(pos) != m_lru.end()) {
                Context_key moved = *pos;
                m_lru.erase(pos);
                m_lru.push_back(moved);
            }
        }

        m_active = &it->second;
        if (m_active->labels.size() > k_max_entries) {
            m_active->labels.clear();
        }
    }

    bool try_get(std::int64_t t, const Cached_label*& out) const
    {
        const Context_data* active = m_active;
        if (!active) {
            return false;
        }
        const auto it = active->labels.find(t);
        if (it == active->labels.end()) {
            return false;
        }
        out = &it->second;
        return true;
    }

    void store(std::int64_t t, std::string&& bytes, float width)
    {
        if (!m_active) {
            return;
        }
        auto& labels = m_active->labels;
        if (labels.size() >= k_max_entries) {
            labels.clear();
        }
        labels.insert_or_assign(t, Cached_label{std::move(bytes), width});
    }

private:
    struct Context_key
    {
        uint64_t   step_bits          = 0;
        uint64_t   range_bits         = 0;
        uint32_t   monospace_bits     = 0;
        uint8_t    monospace_reliable = 0;
        uint64_t   measure_key        = 0;
        uint64_t   font_size_bits     = 0;
        size_t     format_signature   = 0;

        friend bool operator==(const Context_key& lhs, const Context_key& rhs) noexcept
        {
            return
                lhs.step_bits          == rhs.step_bits          &&
                lhs.range_bits         == rhs.range_bits         &&
                lhs.monospace_bits     == rhs.monospace_bits     &&
                lhs.monospace_reliable == rhs.monospace_reliable &&
                lhs.measure_key        == rhs.measure_key        &&
                lhs.font_size_bits     == rhs.font_size_bits     &&
                lhs.format_signature   == rhs.format_signature;
        }
    };

    struct Context_key_hash
    {
        size_t operator()(const Context_key& key) const noexcept
        {
            auto combine = [](size_t seed, size_t value) {
                seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 22);
                return seed;
            };
            size_t seed = std::hash<uint64_t>{}(key.step_bits);
            seed = combine(seed, std::hash<uint64_t>{}(key.range_bits));
            seed = combine(seed, std::hash<uint32_t>{}(key.monospace_bits));
            seed = combine(seed, std::hash<uint8_t>{}(key.monospace_reliable));
            seed = combine(seed, std::hash<uint64_t>{}(key.measure_key));
            seed = combine(seed, std::hash<uint64_t>{}(key.font_size_bits));
            seed = combine(seed, std::hash<size_t>{}(key.format_signature));
            return seed;
        }
    };

    struct Context_data
    {
        std::unordered_map<std::int64_t, Cached_label> labels;
    };

    static constexpr size_t                                         k_max_entries  = 4096;
    static constexpr size_t                                         k_max_contexts = 8;

    std::unordered_map<Context_key, Context_data, Context_key_hash> m_contexts;
    std::vector<Context_key>                                        m_lru;
    Context_data*                                                   m_active       = nullptr;
};

// A std::function's target type cannot identify a callable's captured state.
// Probe each layout calculation so replacing a formatter refreshes label widths.
size_t format_signature(double step, const Layout_calculator::parameters_t& params)
{
    if (!params.format_timestamp_func) {
        return 0;
    }

    std::string signature_text;
    signature_text.reserve(96);

    // Probe the formatter at a handful of synthetic timestamps. The set
    // crosses second/sub-second boundaries so the resulting schema covers
    // both regimes the calculator may pick. Step is in seconds; convert to
    // the formatter's int64-nanosecond contract.
    static constexpr std::array<std::int64_t, 3> k_sample_ns = {
        std::int64_t{0},
        std::int64_t{123'456'789},          // 0.123456789 s
        std::int64_t{12'345'678'900'000}    // 12345.6789 s
    };
    const std::int64_t step_ns = saturating_seconds_to_ns(step);

    bool first = true;
    for (std::int64_t sample_ns : k_sample_ns) {
        std::string text = params.format_timestamp_func(sample_ns, step_ns);
        for (char& ch : text) {
            if (ch >= '0' && ch <= '9') {
                ch = '0';
            }
        }
        if (!first) {
            signature_text.push_back('|');
        }
        signature_text += text;
        first = false;
    }

    const size_t signature = std::hash<std::string>{}(signature_text);
    return signature ^ (params.format_timestamp_revision +
        0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 22));
}

Timestamp_label_cache& timestamp_label_cache()
{
    thread_local Timestamp_label_cache cache;
    return cache;
}

bool has_anchor_within(
    const std::vector<std::pair<float, float>>&    intervals,
    float                                          anchor,
    float                                          tolerance)
{
    if (intervals.empty()) {
        return false;
    }

    const auto lower = std::lower_bound(
        intervals.begin(),
        intervals.end(),
        anchor,
        [](const auto& interval, float value) { return interval.first < value; });

    if (lower != intervals.end() && std::fabs(anchor - lower->first) < tolerance) {
        return true;
    }

    if (lower != intervals.begin()) {
        const auto prev = std::prev(lower);
        if (std::fabs(anchor - prev->first) < tolerance) {
            return true;
        }
    }

    return false;
}

} // namespace

bool Layout_calculator::fits_with_gap(
    const std::vector<std::pair<float, float>>&    level,
    const std::vector<std::pair<float, float>>&    accepted,
    float                                          min_gap) const
{
    if (level.empty()) {
        return true;
    }
    for (size_t i = 1; i < level.size(); ++i) {
        if (level[i].first - level[i - 1].second < min_gap) {
            return false;
        }
    }

    size_t j = 0;
    for (const auto& iv : level) {
        while (j < accepted.size() && accepted[j].second + min_gap <= iv.first) {
            ++j;
        }
        if (j                                                                               < accepted.size() &&
            std::max(iv.first, accepted[j].first) - std::min(iv.second, accepted[j].second) < min_gap)
        {
            return false;
        }
        if (j                                                                                       > 0 &&
            std::max(iv.first, accepted[j - 1].first) - std::min(iv.second, accepted[j - 1].second) < min_gap)
        {
            return false;
        }
    }
    return true;
}

Layout_calculator::result_t Layout_calculator::calculate(const parameters_t& params) const
{
    result_t res;

    vnm::plot::Profiler* profiler = params.profiler;
    VNM_PLOT_PROFILE_SCOPE(
        profiler,
        "renderer.frame.calculate_layout.impl.cache_miss.pass1");

    // --- Vertical (V) Axis Label Selection ---
    double v_span = 0.0;
    {
        VNM_PLOT_PROFILE_SCOPE(
            profiler,
            "renderer.frame.calculate_layout.impl.cache_miss.pass1.v_span");
        v_span = double(params.v_max) - double(params.v_min);
    }
    if (v_span > 0.0 && params.usable_height > 0.0) {
        VNM_PLOT_PROFILE_SCOPE(
            profiler,
            "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis");

        const int divs[2] = {5, 2};
        double step                = 1.0;
        double test                = 0.0;
        int    i                   = 16;
        int    initial_level_index = 0;

        double initial_level_step   = 0.0;
        double px_per_unit          = 0.0;
        float  label_box_height_px  = 0.0f;
        float  label_gap_px         = 0.0f;
        float  min_label_spacing_px = 0.0f;
        double finest_step_accepted = 0.0;

        auto& accepted_boxes = m_scratch_accepted_boxes;
        auto& level          = m_scratch_level;
        auto& accepted_y     = m_scratch_accepted_y;

        constexpr float k_coincide = 1.0f;

        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.setup");
            const auto validate_seed = [&](double seed_step, int seed_index) {
                if (!(seed_step > 0.0)) {
                    return false;
                }
                const double upper = seed_step * divs[seed_index & 1];
                const double lower = seed_step / divs[(seed_index - 1) & 1];
                if (!std::isfinite(upper) || !std::isfinite(lower)) {
                    return false;
                }
                const double tol = std::max(1e-6, std::abs(v_span) * 1e-6);
                return (lower - tol) <= v_span && v_span <= (upper + tol);
            };

            bool used_seed = false;
            if (params.has_vertical_seed && params.vertical_seed_index >= 0) {
                if (validate_seed(params.vertical_seed_step, params.vertical_seed_index)) {
                    step      = params.vertical_seed_step;
                    i         = params.vertical_seed_index;
                    used_seed = true;
                }
            }

            if (!used_seed) {
                for (; (test = step * divs[i & 1]) < v_span; ++i) {
                    step = test;
                }
                for (; (test = step / divs[(i - 1) & 1]) > v_span; --i) {
                    step = test;
                }
            }

            initial_level_index = i;
            initial_level_step  = step;
            px_per_unit         = params.usable_height / v_span;

            accepted_boxes.clear();
            level.clear();
            accepted_y.clear();

            label_box_height_px  = std::max(
                1.0f,
                static_cast<float>(params.adjusted_font_size_in_pixels));
            label_gap_px         = 10.0f;
            min_label_spacing_px = label_box_height_px + label_gap_px;
            finest_step_accepted = step;
        }

        const auto y_of = [&](double v) {
            return float(params.usable_height - (v - double(params.v_min)) * px_per_unit);
        };

        for (int guard = 0; guard < 64; ++guard) {
            double shift     = 0.0;
            double extend    = 0.0;
            int    j_min     = 0;
            int    j_max     = 0;
            auto&  this_vals = m_scratch_vals;

            bool skip_level_due_to_conflict = false;

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.iter_prep");
                level.clear();
                shift  = get_shift(step, double(params.v_min));
                extend = step;
                j_min  = static_cast<int>(std::ceil((-extend - shift) / step - 1e-9));
                j_max  = static_cast<int>(std::floor((v_span + extend - shift) / step + 1e-9));
                this_vals.clear();
                this_vals.reserve(j_max - j_min + 1);
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.scan");
                for (int j = j_min; j <= j_max; ++j) {
                    const double v = double(params.v_min) + shift + j * step;
                    const float  y = y_of(v);

                    if (y <= 0.f || y >= float(params.label_visible_height)) {
                        continue;
                    }

                    bool coincides = false;
                    for (float ay : accepted_y) {
                        if (std::fabs(ay - y) < k_coincide) {
                            coincides = true;
                            break;
                        }
                    }

                    if (!coincides) {
                        if (!this_vals.empty()) {
                            const float prev_y = this_vals.back().second;
                            if (std::fabs(prev_y - y) < min_label_spacing_px) {
                                skip_level_due_to_conflict = true;
                                break;
                            }
                        }
                        this_vals.emplace_back(v, y);
                    }
                }
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.resolve");
                if (skip_level_due_to_conflict) {
                    this_vals.clear();
                }

                if (!skip_level_due_to_conflict) {
                    bool level_fits = false;
                    {
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.level_build");
                        for (const auto& vy : this_vals) {
                            level.emplace_back(
                                vy.second - 0.5f * label_box_height_px,
                                vy.second + 0.5f * label_box_height_px);
                        }
                        std::sort(level.begin(), level.end());
                    }

                    if (!level.empty()) {
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.fits_with_gap");
                        level_fits = fits_with_gap(level, accepted_boxes, label_gap_px);
                    }

                    if (level_fits) {
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.accept");
                        finest_step_accepted = std::min(finest_step_accepted, step);
                        for (size_t k = 0; k < this_vals.size(); ++k) {
                            res.v_labels.push_back({this_vals[k].first, this_vals[k].second, {}});
                            accepted_boxes.push_back(level[k]);
                            accepted_y.push_back(this_vals[k].second);
                        }
                        std::inplace_merge(
                            accepted_boxes.begin(),
                            accepted_boxes.end() - level.size(),
                            accepted_boxes.end());
                    }
                }
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.advance");
                --i;
                step /= divs[i & 1];
                if (step * px_per_unit < 2.0) {
                    break;
                }
            }
        }

        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.finalize");
            res.vertical_seed_index  = initial_level_index;
            res.vertical_seed_step   = initial_level_step;
            res.vertical_finest_step = finest_step_accepted;
        }

        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.fixed_digits");
            res.v_label_fixed_digits = params.get_required_fixed_digits_func
                ? std::max(0, params.get_required_fixed_digits_func(finest_step_accepted))
                : fixed_digits_for_step(finest_step_accepted);

            auto& vals = m_scratch_vals_d;
            vals.clear();
            vals.reserve(res.v_labels.size());
            for (const auto& e : res.v_labels) {
                vals.push_back(e.value);
            }

            if (!any_fractional_at_precision(vals, res.v_label_fixed_digits)) {
                res.v_label_fixed_digits = 0;
            }
            else {
                res.v_label_fixed_digits = trim_trailing_zero_decimals(vals, res.v_label_fixed_digits);
            }
        }

        // Format and measure labels
        float advance       = 0.f;
        bool  use_monospace = false;
        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.label_prep");
            res.max_v_label_text_width = 0.f;
            advance                    = std::max(params.monospace_char_advance_px, 0.f);
            use_monospace              = params.monospace_advance_is_reliable && advance > 0.f;
        }

        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.vertical_axis.measure_text");
            for (auto& e : res.v_labels) {
                value_format_context_t context;
                context.role                   = Value_format_role::AXIS_LABEL;
                context.suggested_fixed_digits = res.v_label_fixed_digits;

                std::string text = params.format_value_func
                    ? params.format_value_func(e.value, context)
                    : std::string{};
                if (text.empty()) {
                    text = format_axis_fixed_or_int(e.value, res.v_label_fixed_digits);
                }
                if (text.empty() || text[0] != '-') {
                    text.insert(text.begin(), ' ');
                }

                float width = 0.f;
                if (use_monospace) {
                    width = advance * float(text.size());
                }
                else
                if (params.measure_text_func) {
                    width = params.measure_text_func(text.c_str());
                    if (width <= 0.f && advance > 0.f) {
                        width = advance * float(text.size());
                    }
                }
                else {
                    width = advance * float(text.size());
                }

                res.max_v_label_text_width = std::max(res.max_v_label_text_width, width);
                e.text = std::move(text);
            }

            std::sort(
                res.v_labels.begin(),
                res.v_labels.end(),
                [](const v_label_t& lhs, const v_label_t& rhs) {
                    return lhs.value < rhs.value;
                });
            res.v_labels.erase(
                std::unique(
                    res.v_labels.begin(),
                    res.v_labels.end(),
                    [](const v_label_t& lhs, const v_label_t& rhs) {
                        return lhs.text == rhs.text;
                    }),
                res.v_labels.end());
        }
    }

    // --- Horizontal (T) Axis Label Selection ---
    // Keep absolute ticks in integer nanoseconds. Floating point is used only
    // for the step ladder and differences mapped to screen coordinates.
    constexpr double k_ns_per_second_d = 1.0e9;
    constexpr double k_seconds_per_ns  = 1.0 / k_ns_per_second_d;
    double           t_range           = 0.0;
    {
        VNM_PLOT_PROFILE_SCOPE(
            profiler,
            "renderer.frame.calculate_layout.impl.cache_miss.pass1.t_range");
        const auto t_range_ns = positive_span_ns_as_long_double(params.t_min, params.t_max);
        if (t_range_ns) {
            t_range = static_cast<double>(
                *t_range_ns * static_cast<long double>(k_seconds_per_ns));
        }
    }
    if (t_range > 0.0 && params.usable_width > 0.0f) {
        VNM_PLOT_PROFILE_SCOPE(
            profiler,
            "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis");

        constexpr float k_coincide = 1.0f;
        const float     min_gap    = 10.0f;

        double px_per_t      = 0.0;
        float  advance       = 0.f;
        bool   use_monospace = false;
        std::vector<double> steps;
        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.prep");
            px_per_t      = params.usable_width / t_range;
            advance       = std::max(params.monospace_char_advance_px, 0.f);
            use_monospace = params.monospace_advance_is_reliable && advance > 0.f;
            steps         = build_time_steps_covering(t_range);
        }

        const auto x_of_t = [&](std::int64_t timestamp_ns) -> float {
            return float(span_ns_as_long_double(params.t_min, timestamp_ns) *
                static_cast<long double>(k_seconds_per_ns) * px_per_t);
        };

        const auto seconds_to_ns = [](double seconds) -> std::int64_t {
            return saturating_seconds_to_ns(seconds);
        };

        const auto label_text = [&](std::int64_t timestamp_ns, double step_seconds) -> std::string {
            if (!params.format_timestamp_func) {
                return {};
            }
            return params.format_timestamp_func(timestamp_ns, seconds_to_ns(step_seconds));
        };

        std::vector<std::pair<float, float>> accepted;
        std::vector<std::pair<float, float>> level;

        int si = -1;
        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.setup");
            if (params.has_horizontal_seed &&
                params.horizontal_seed_index >= 0 &&
                params.horizontal_seed_index <  static_cast<int>(steps.size()))
            {
                const double seeded_step = steps[params.horizontal_seed_index];
                const double ref = std::max(
                    1e-6,
                    std::max(std::abs(seeded_step), std::abs(params.horizontal_seed_step)));
                if (std::abs(seeded_step - params.horizontal_seed_step) <= ref * 1e-6) {
                    si = params.horizontal_seed_index;
                }
            }
            if (si < 0) {
                si = std::max(0, find_time_step_start_index(steps, t_range));
            }
            level.reserve(32);
        }

        const int    start_si   = si;
        const double start_step = (si >= 0 && si < static_cast<int>(steps.size())) ? steps[si] : 0.0;

        bool   any_level   = false;
        bool   any_subsec  = false;
        double finest_step = 0.0;

        for (; si >= 0; --si) {
            const double step = steps[si];
            if (step > static_cast<double>(std::numeric_limits<std::int64_t>::max()) * k_seconds_per_ns) {
                continue;
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.step_guard");
                if (step * px_per_t < min_gap) {
                    break;
                }
            }

            struct cand
            {
                std::int64_t t;
                float        x0;
                float        x1;
                float        x_anchor;
            };
            std::vector<cand> candidates;
            float right_vis             = 0.0f;
            float pixel_step            = 0.0f;
            float optimistic_width      = 0.0f;
            float estimated_label_width = 0.0f;
            std::int64_t t_start = 0;
            const std::int64_t step_ns = seconds_to_ns(step);
            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.step_prep");
                candidates.reserve(64);

                right_vis = float(params.usable_width + params.vbar_width);
                pixel_step = static_cast<float>(step * px_per_t);
                if (pixel_step <= 0.f) {
                    continue;
                }

                optimistic_width = use_monospace ? advance * 4.f : min_gap;
                if (pixel_step < (optimistic_width + min_gap)) {
                    break;
                }

                // Estimate timestamp label width (e.g. "1970-01-01 02:30:00" = 19 chars).
                estimated_label_width = (advance > 0.f) ? advance * 20.f : (min_gap * 10.f);

                // Start tick generation early enough that labels whose right edge is still visible
                // are included. A label at anchor x extends visually to x + k_text_margin_px + width.
                // Using floor (not ceil) plus a width-based margin ensures we don't skip visible labels
                // when t_min crosses a step boundary during panning.
                const float label_extent_px = estimated_label_width + k_text_margin_px;
                const double left_steps =
                    static_cast<double>(label_extent_px) / static_cast<double>(pixel_step);
                const std::int64_t remainder = params.t_min % step_ns;
                t_start = params.t_min - remainder;
                const int preceding_steps = int(std::ceil(1.0 + left_steps)) + (remainder < 0 ? 1 : 0);
                for (int index = 0; index < preceding_steps; ++index) {
                    const auto previous = checked_sub_ns(t_start, step_ns);
                    if (!previous) {
                        break;
                    }
                    t_start = *previous;
                }
            }

            size_t signature = 0;
            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                    "format_labels.signature_build");
                signature = format_signature(step, params);
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.cache_context");
                timestamp_label_cache().set_context(
                    step,
                    t_range,
                    params.measure_text_cache_key,
                    advance,
                    params.monospace_advance_is_reliable,
                    params.adjusted_font_size_in_pixels,
                    signature);
            }

            std::int64_t t = t_start;
            const std::int64_t last_tick = saturating_add_ns(params.t_max, step_ns);
            const auto advance_tick = [&](int count) {
                const auto step_count = static_cast<std::uint64_t>(count);
                const auto unsigned_step = static_cast<std::uint64_t>(step_ns);
                if (step_count > std::numeric_limits<std::uint64_t>::max() / unsigned_step) {
                    return false;
                }
                const auto distance = step_count * unsigned_step;
                const auto remaining = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                    static_cast<std::uint64_t>(t);
                if (distance > remaining) {
                    return false;
                }
                t = saturating_add_duration_ns(t, distance);
                return true;
            };
            float        last_width          = estimated_label_width;
            bool         have_prev_candidate = false;
            float        prev_candidate_x1   = std::numeric_limits<float>::lowest();
            bool         step_invalid        = false;

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.format_labels");
                while (t <= last_tick) {
                    VNM_PLOT_PROFILE_SCOPE(
                        profiler,
                        "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                        "format_labels.candidate_loop");
                    const float x = x_of_t(t);
                    if (x >= right_vis) {
                        break;
                    }

                    cand candidate{t, x, x, x};
                    // Use conservative width estimate for skip optimization.
                    // Must be at least as large as actual label width to avoid skipping visible labels.
                    const float skip_width = std::max(last_width, estimated_label_width);
                    // Account for text margin: visual right edge is at x + k_text_margin_px + width.
                    // Skip only when visual right edge is fully offscreen (< 0).
                    if (x + k_text_margin_px + skip_width <= 0.f) {
                        const int skip  = std::max(1, int(std::ceil(skip_width / pixel_step)));
                        if (!advance_tick(skip)) {
                            break;
                        }
                        continue;
                    }

                    bool anchor_taken = false;
                    {
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                            "format_labels.anchor_check");
                        anchor_taken = has_anchor_within(accepted, x, k_coincide);
                    }

                    if (anchor_taken) {
                        const int skip  = std::max(1, int(std::ceil(min_gap / pixel_step)));
                        if (!advance_tick(skip)) {
                            break;
                        }
                        continue;
                    }

                    if (have_prev_candidate && x < prev_candidate_x1 + min_gap) {
                        step_invalid = true;
                        candidates.clear();
                        break;
                    }

                    float               w         = 0.0f;
                    const Cached_label* cached    = nullptr;
                    const bool          cache_hit = timestamp_label_cache().try_get(t, cached);

                    if (cache_hit) {
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                            "format_labels.cache_hit_lookup");
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                            "format_labels.cache_hit_count");
                        w = cached->width;
                    }
                    else {
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                            "format_labels.cache_miss_lookup");
                        VNM_PLOT_PROFILE_SCOPE(
                            profiler,
                            "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                            "format_labels.cache_miss_count");
                        std::string label;
                        {
                            VNM_PLOT_PROFILE_SCOPE(
                                profiler,
                                "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                                "format_labels.format_timestamp");
                            label = label_text(t, step);
                        }

                        if (use_monospace) {
                            w = advance * float(label.size());
                        }
                        else
                        if (params.measure_text_func) {
                            VNM_PLOT_PROFILE_SCOPE(
                                profiler,
                                "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                                "format_labels.measure_text");
                            w = params.measure_text_func(label.c_str());
                            if (w <= 0.f && advance > 0.f) {
                                w = advance * float(label.size());
                            }
                        }
                        else {
                            w = advance * float(label.size());
                        }

                        timestamp_label_cache().store(t, std::move(label), w);
                    }

                    last_width = std::max(w, optimistic_width);
                    if (w + min_gap > pixel_step) {
                        step_invalid = true;
                        candidates.clear();
                        break;
                    }
                    // Account for text margin: visual right edge is at x + k_text_margin_px + w.
                    // Cull only when visual right edge is fully offscreen (< 0).
                    if (x + k_text_margin_px + w <= 0.f) {
                        const int skip  = std::max(1, int(std::ceil(w / pixel_step)));
                        if (!advance_tick(skip)) {
                            break;
                        }
                        continue;
                    }
                    if (have_prev_candidate && x < prev_candidate_x1 + min_gap) {
                        step_invalid = true;
                        candidates.clear();
                        break;
                    }

                    candidate.x1 = x + w;
                    candidates.push_back(std::move(candidate));
                    have_prev_candidate = true;
                    prev_candidate_x1 = x + w;

                    const float required_spacing = w + min_gap;
                    const int   skip             = std::max(1, int(std::ceil(required_spacing / pixel_step)));
                    if (!advance_tick(skip)) {
                        break;
                    }
                }
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.step_checks");
                if (step_invalid) {
                    continue;
                }

                if (candidates.empty()) {
                    continue;
                }
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.arrange_labels");

                // Filter out anchors already taken
                {
                    VNM_PLOT_PROFILE_SCOPE(
                        profiler,
                        "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis."
                        "arrange_labels.anchor_filter");
                    auto write_it = candidates.begin();
                    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
                        const bool anchor_taken = has_anchor_within(accepted, it->x_anchor, k_coincide);
                        if (anchor_taken)   { continue;                   }
                        if (write_it != it) { *write_it = std::move(*it); }
                        ++write_it;
                    }
                    candidates.erase(write_it, candidates.end());
                }
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.arrange_checks");
                if (candidates.empty()) {
                    continue;
                }
            }

            {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.level_build");
                level.clear();
                level.reserve(candidates.size());
                for (const auto& c : candidates) {
                    level.emplace_back(c.x0, c.x1);
                }
            }

            bool level_fits = false;
            if (!level.empty()) {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.fits_with_gap");
                level_fits = fits_with_gap(level, accepted, min_gap);
            }

            if (level_fits) {
                VNM_PLOT_PROFILE_SCOPE(
                    profiler,
                    "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.emit_labels");
                any_level = true;
                finest_step = step;
                if (step < 1.0) {
                    any_subsec = true;
                }

                for (auto& candidate : candidates) {
                    std::string label_bytes;
                    const Cached_label* cached_label = nullptr;
                    if (timestamp_label_cache().try_get(candidate.t, cached_label) && cached_label) {
                        label_bytes = cached_label->bytes;
                    }
                    else {
                        std::string label = label_text(candidate.t, step);
                        label_bytes = label;
                        timestamp_label_cache().store(candidate.t, std::move(label), candidate.x1 - candidate.x0);
                    }

                    if (label_bytes.empty()) {
                        continue;
                    }

                    res.h_labels.push_back({
                        candidate.t,
                        glm::vec2(
                            candidate.x_anchor,
                            float(params.usable_height + params.h_label_vertical_nudge_factor *
                                params.adjusted_font_size_in_pixels)),
                        std::move(label_bytes)
                    });
                }

                accepted.insert(accepted.end(), level.begin(), level.end());
                std::inplace_merge(accepted.begin(), accepted.end() - level.size(), accepted.end());
            }
        }

        {
            VNM_PLOT_PROFILE_SCOPE(
                profiler,
                "renderer.frame.calculate_layout.impl.cache_miss.pass1.horizontal_axis.finalize");
            if (any_level) {
                res.h_labels_subsecond = any_subsec;
            }

            res.horizontal_seed_index = start_si;
            res.horizontal_seed_step  = start_step;
        }

        // Ensure uniform label format using the finest accepted step.
        // After the multi-level loop, labels from coarser steps may have
        // narrower formats. Re-format all with finest_step, then remove
        // any labels that now overlap due to wider text.
        if (any_level && finest_step > 0.0 && params.format_timestamp_func &&
            !res.h_labels.empty())
        {
            const std::int64_t finest_step_ns = seconds_to_ns(finest_step);
            for (auto& label : res.h_labels) {
                label.text = params.format_timestamp_func(label.value, finest_step_ns);
            }

            std::sort(res.h_labels.begin(), res.h_labels.end(),
                [](const h_label_t& a, const h_label_t& b) {
                    return a.position.x < b.position.x;
                });

            std::string previous_text;
            std::string following_text;
            bool        have_previous_text  = false;
            bool        have_following_text = false;
            // Include the adjacent grid tick so culling cannot promote a
            // later duplicate to the retained position.
            if (params.horizontal_axis_left_to_right) {
                const auto previous = checked_sub_ns(
                    res.h_labels.front().value,
                    finest_step_ns);
                if (previous) {
                    previous_text = params.format_timestamp_func(*previous, finest_step_ns);
                    have_previous_text = true;
                }
            }
            else {
                const auto following = checked_add_ns(
                    res.h_labels.back().value,
                    finest_step_ns);
                if (following) {
                    following_text = params.format_timestamp_func(*following, finest_step_ns);
                    have_following_text = true;
                }
            }

            float prev_right = -std::numeric_limits<float>::max();
            auto  write      = res.h_labels.begin();
            for (auto it = res.h_labels.begin(); it != res.h_labels.end(); ++it) {
                const auto next = std::next(it);
                const bool duplicate = params.horizontal_axis_left_to_right
                    ? have_previous_text && it->text == previous_text
                    : (next != res.h_labels.end() && it->text == next->text) ||
                        (next == res.h_labels.end() &&
                            have_following_text && it->text == following_text);
                previous_text      = it->text;
                have_previous_text = true;
                if (duplicate) {
                    continue;
                }

                float w = 0.f;
                if (use_monospace && advance > 0.f) { w = advance * float(it->text.size());           } else
                if (params.measure_text_func)       { w = params.measure_text_func(it->text.c_str()); } else
                if (advance > 0.f)                  { w = advance * float(it->text.size()); }

                if (it->position.x >= prev_right + min_gap) {
                    prev_right = it->position.x + w;
                    if (write != it) {
                        *write = std::move(*it);
                    }
                    ++write;
                }
            }
            res.h_labels.erase(write, res.h_labels.end());
        }
    }

    return res;
}

} // namespace vnm::plot
