#pragma once
// VNM Plot Library - Core Types
// Qt-free types used by the data and layout interfaces.
#include <vnm_plot/core/time_units.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace vnm::plot {
struct Plot_config;
class Profiler;
struct Data_access_policy;
template<typename Sample>
struct Data_access_policy_typed;

struct sample_semantics_key_t
{
    std::uint64_t  value        = 0;
    std::uint64_t  revision     = 0;
    bool           conservative = true;
};

namespace detail {

enum class access_dispatch_kind_t : std::uint8_t
{
    NONE,
    MEMBER_POINTER,
    STD_FUNCTION,
    MIXED
};

struct erased_access_policy_t
{
    using timestamp_fn_t =
        std::int64_t (*)(const erased_access_policy_t&, const void*);
    using value_fn_t     =
        float (*)(const erased_access_policy_t&, const void*);
    using range_fn_t     =
        std::pair<float, float> (*)(const erased_access_policy_t&, const void*);

    const void*            ctx              = nullptr;
    timestamp_fn_t         get_timestamp    = nullptr;
    value_fn_t             get_value        = nullptr;
    range_fn_t             get_range        = nullptr;

    access_dispatch_kind_t dispatch_kind    = access_dispatch_kind_t::NONE;
    std::size_t            timestamp_offset = 0;
    std::size_t            value_offset     = 0;
    std::size_t            range_min_offset = 0;
    std::size_t            range_max_offset = 0;

    [[nodiscard]] bool has_timestamp() const noexcept
    {
        return get_timestamp != nullptr;
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return get_value != nullptr;
    }

    [[nodiscard]] bool has_range() const noexcept
    {
        return get_range != nullptr;
    }

    [[nodiscard]] bool is_valid() const noexcept
    {
        return has_timestamp() && (has_value() || has_range());
    }

    std::int64_t timestamp(const void* sample) const
    {
        return get_timestamp ? get_timestamp(*this, sample) : std::int64_t{0};
    }

    float value(const void* sample) const
    {
        return get_value ? get_value(*this, sample) : 0.0f;
    }

    std::pair<float, float> range(const void* sample) const
    {
        return get_range
            ? get_range(*this, sample)
            : std::make_pair(0.0f, 0.0f);
    }
};

struct access_policy_cache_key_t
{
    const Data_access_policy*  identity      = nullptr;
    std::uint64_t              layout_key    = 0;
    std::uint64_t              revision      = 0;
    access_dispatch_kind_t     dispatch_kind = access_dispatch_kind_t::NONE;
    bool                       has_timestamp = false;
    bool                       has_value     = false;
    bool                       has_range     = false;

    [[nodiscard]] bool operator==(
        const access_policy_cache_key_t& other) const noexcept
    {
        return
            identity      == other.identity      &&
            layout_key    == other.layout_key    &&
            revision      == other.revision      &&
            dispatch_kind == other.dispatch_kind &&
            has_timestamp == other.has_timestamp &&
            has_value     == other.has_value     &&
            has_range     == other.has_range;
    }

    [[nodiscard]] bool operator!=(
        const access_policy_cache_key_t& other) const noexcept
    {
        return !(*this == other);
    }
};

erased_access_policy_t make_erased_access_policy_view(
    const Data_access_policy&      policy);

access_policy_cache_key_t make_access_policy_cache_key(
    const Data_access_policy*      policy,
    const erased_access_policy_t&  view);

sample_semantics_key_t make_sample_semantics_key(
    const Data_access_policy*      policy);

template<typename Arg, typename Policy>
class Access_policy_base;

template<typename Signature>
class access_function_slot_t;

template<typename R, typename... Args>
class access_function_slot_t<R(Args...)>
{
public:
    using function_t = std::function<R(Args...)>;

    access_function_slot_t() = default;
    access_function_slot_t(const access_function_slot_t& other)
    :
        m_function(other.m_function)
    {}

    access_function_slot_t(access_function_slot_t&& other)
    :
        m_function(std::move(other.m_function))
    {
        other.clear_internal_access();
    }

    access_function_slot_t& operator=(const access_function_slot_t& other)
    {
        if (this != &other) {
            m_function = other.m_function;
            clear_internal_access();
        }
        return *this;
    }

    access_function_slot_t& operator=(access_function_slot_t&& other)
    {
        if (this != &other) {
            m_function = std::move(other.m_function);
            clear_internal_access();
            other.clear_internal_access();
        }
        return *this;
    }

    template<
        typename Callable,
        typename = std::enable_if_t<
            !std::is_same_v<std::decay_t<Callable>, access_function_slot_t>>>
    access_function_slot_t& operator=(Callable&& callable)
    {
        m_function = std::forward<Callable>(callable);
        clear_internal_access();
        return *this;
    }

    access_function_slot_t& operator=(std::nullptr_t)
    {
        m_function = nullptr;
        clear_internal_access();
        return *this;
    }

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(m_function);
    }

    R operator()(Args... args) const
    {
        return m_function(std::forward<Args>(args)...);
    }

    [[nodiscard]] const function_t& function() const noexcept
    {
        return m_function;
    }

private:
    template<typename, typename>
    friend class Access_policy_base;

    void bind_internal_access(
        erased_access_policy_t*    access,
        std::uint64_t*             revision,
        sample_semantics_key_t*    semantics_key = nullptr) noexcept
    {
        m_internal_access = access;
        m_revision        = revision;
        m_semantics_key   = semantics_key;
    }

    void clear_internal_access() noexcept
    {
        if (m_internal_access) {
            *m_internal_access = erased_access_policy_t{};
        }
        if (m_semantics_key) {
            *m_semantics_key = sample_semantics_key_t{};
        }
        if (m_revision) {
            ++(*m_revision);
        }
    }

    function_t                 m_function;
    erased_access_policy_t*    m_internal_access = nullptr;
    std::uint64_t*             m_revision        = nullptr;
    sample_semantics_key_t*    m_semantics_key   = nullptr;
};

inline sample_semantics_key_t make_explicit_sample_semantics_key(
    std::uint64_t  value,
    std::uint64_t  revision) noexcept
{
    sample_semantics_key_t key;
    if (value != 0) {
        key.value        = value;
        key.revision     = revision;
        key.conservative = false;
    }
    return key;
}

// Each slot points into its owning policy. Whole-policy moves use the copy
// operations too, retaining the source's bindings and rebinding the destination.
template<typename Arg, typename Policy>
class Access_policy_base
{
public:
    using timestamp_accessor_t = access_function_slot_t<std::int64_t(Arg)>;
    using value_accessor_t     = access_function_slot_t<float(Arg)>;
    using range_accessor_t     = access_function_slot_t<std::pair<float, float>(Arg)>;

    // --- Sample value extraction ---
    // Values narrow to float before upload. For large-biased signals, subtract
    // the bias inside the accessor so the remaining dynamic range survives.
    // Timestamps are int64_t nanoseconds (by API convention; the unit is the
    // accessor's contract with vnm_plot).
    timestamp_accessor_t   get_timestamp; ///< Extract timestamp (ns)
    value_accessor_t       get_value;     ///< Extract primary value
    range_accessor_t       get_range;     ///< Extract min/max range

    ///< Byte-layout cache key for renderer-internal caches. It is not a
    ///< unique sample type identity and does not describe accessor semantics.
    uint64_t               layout_key = 0;

    ///< Accessor-transform identity for source query caches. Member-pointer
    ///< typed policies populate a stable key; callable policies stay
    ///< conservative unless the caller supplies a non-zero value and maintains
    ///< revision when the callable semantics change.
    sample_semantics_key_t semantics_key;

    bool is_valid() const
    {
        return get_timestamp && (get_value || get_range);
    }

    Policy& set_semantics_key(
        std::uint64_t  value,
        std::uint64_t  revision = 0) noexcept
    {
        semantics_key =
            make_explicit_sample_semantics_key(value, revision);
        return static_cast<Policy&>(*this);
    }

protected:
    Access_policy_base()
    {
        bind_accessor_slots();
    }

    Access_policy_base(const Access_policy_base& other)
    :
        get_timestamp(other.get_timestamp),
        get_value(other.get_value),
        get_range(other.get_range),
        layout_key(other.layout_key),
        semantics_key(other.semantics_key),
        m_internal_access(other.m_internal_access),
        m_access_revision(other.m_access_revision)
    {
        bind_accessor_slots();
    }

    Access_policy_base& operator=(const Access_policy_base& other)
    {
        if (this != &other) {
            get_timestamp     = other.get_timestamp;
            get_value         = other.get_value;
            get_range         = other.get_range;
            layout_key        = other.layout_key;
            semantics_key     = other.semantics_key;
            m_internal_access = other.m_internal_access;
            ++m_access_revision;
            bind_accessor_slots();
        }
        return *this;
    }

    void set_internal_access(erased_access_policy_t access)
    {
        m_internal_access = access;
        ++m_access_revision;
        bind_accessor_slots();
    }

    erased_access_policy_t m_internal_access;
    std::uint64_t          m_access_revision = 1;

private:
    void bind_accessor_slots() noexcept
    {
        get_timestamp.bind_internal_access(
            &m_internal_access,
            &m_access_revision,
            &semantics_key);
        get_value.bind_internal_access(
            &m_internal_access,
            &m_access_revision,
            &semantics_key);
        get_range.bind_internal_access(
            &m_internal_access,
            &m_access_revision,
            &semantics_key);
    }
};

} // namespace detail

// -----------------------------------------------------------------------------
// Size_2i - Replacement for QSize
// -----------------------------------------------------------------------------
struct Size_2i
{
    int width  = 0;
    int height = 0;

    constexpr Size_2i() = default;
    constexpr Size_2i(int w, int h) : width(w), height(h) {}

    [[nodiscard]] constexpr bool is_valid() const noexcept
    {
        return width > 0 && height > 0;
    }

    [[nodiscard]] constexpr bool operator==(const Size_2i& other) const noexcept
    {
        return width == other.width && height == other.height;
    }
};

// -----------------------------------------------------------------------------
// Byte Buffer - Lightweight replacement for QByteArray
// -----------------------------------------------------------------------------
// Using std::string as byte buffer: has SSO, move semantics, and works well
// with file I/O. std::vector<char> is an alternative but std::string is more
// convenient for text-based asset formats (shaders, JSON).
using Byte_buffer = std::string;
using Byte_view   = std::string_view;

// -----------------------------------------------------------------------------
// data_snapshot_t: A view of sample data (optionally split into two segments)
// -----------------------------------------------------------------------------
// Represents a snapshot of data that can be safely read. The data pointer
// points to a contiguous array of samples, each `stride` bytes apart.
// If data2/count2 is set, the logical snapshot is split into two contiguous
// segments (e.g., ring buffer wrap). The total count is `count`.
// `sequence` is a monotonic counter that increments on data changes.
//
// Lifetime contract:
// - The Data_source implementation decides whether the pointers refer to a
//   copy it owns, or a direct view into its live storage.
// - Whatever guarantees the view's validity (an internal buffer, a lock, a
//   reference count) must be kept alive via `hold`; the snapshot is safe to
//   read for exactly as long as the caller keeps `hold` alive.
// - Consumers must not cache `data`/`data2` beyond the lifetime of the
//   snapshot unless they also retain `hold`.
struct data_snapshot_t
{
    const void*            data     = nullptr; ///< Pointer to first sample
    size_t                 count    = 0;       ///< Number of samples
    size_t                 stride   = 0;       ///< Bytes between consecutive samples
    uint64_t               sequence = 0;       ///< Change counter for cache invalidation
    const void*            data2    = nullptr; ///< Optional second segment (wrap)
    size_t                 count2   = 0;       ///< Samples in second segment
    std::shared_ptr<void>  hold;               ///< Optional ownership/lock guard

    explicit operator bool() const { return is_valid(); }

    bool is_valid() const noexcept
    {
        return
             data != nullptr &&
             count > 0       &&
             stride > 0      &&
             count2 <= count &&
            (count2 == 0 || data2 != nullptr);
    }

    const void* at(size_t index) const
    {
        if (!data || index >= count || stride == 0) {
            return nullptr;
        }
        if (count2 > count || (count2 > 0 && !data2)) {
            return nullptr;
        }
        const size_t count1 = count - count2;
        if (index < count1)        { return static_cast<const char*>(data) + index * stride; }
        if (!data2 || count2 == 0) { return nullptr;                                         }
        return static_cast<const char*>(data2) + (index - count1) * stride;
    }

    size_t count1() const { return count2 <= count ? count - count2 : 0; }

    bool is_segmented() const { return data2 != nullptr && count2 > 0; }
};

struct snapshot_result_t
{
    data_snapshot_t snapshot;
    enum class Snapshot_status { READY, EMPTY, BUSY, FAILED } status = Snapshot_status::READY;

    explicit operator bool() const { return status == Snapshot_status::READY && snapshot; }
};

// -----------------------------------------------------------------------------
// Data_access_policy: How to extract values from samples
// -----------------------------------------------------------------------------
// Defines how the renderer extracts meaningful values from opaque sample data.
// This enables rendering of arbitrary sample types without template explosion.
struct Data_access_policy : detail::Access_policy_base<const void*, Data_access_policy>
{
    Data_access_policy() {}

private:
    friend detail::erased_access_policy_t detail::make_erased_access_policy_view(
        const Data_access_policy&      policy);

    friend detail::access_policy_cache_key_t detail::make_access_policy_cache_key(
        const Data_access_policy*              policy,
        const detail::erased_access_policy_t&  view);

    friend sample_semantics_key_t detail::make_sample_semantics_key(
        const Data_access_policy*      policy);
    template<typename>
    friend struct Data_access_policy_typed;
};

namespace detail {

inline std::int64_t std_function_access_timestamp(
    const erased_access_policy_t&  view,
    const void*                    sample)
{
    const auto* policy = static_cast<const Data_access_policy*>(view.ctx);
    return
        policy &&
        policy->get_timestamp
            ? policy->get_timestamp(sample)
            : std::int64_t{0};
}

inline float std_function_access_value(
    const erased_access_policy_t&  view,
    const void*                    sample)
{
    const auto* policy = static_cast<const Data_access_policy*>(view.ctx);
    return policy && policy->get_value ? policy->get_value(sample) : 0.0f;
}

inline std::pair<float, float> std_function_access_range(
    const erased_access_policy_t&  view,
    const void*                    sample)
{
    const auto* policy = static_cast<const Data_access_policy*>(view.ctx);
    return
        policy &&
        policy->get_range
            ? policy->get_range(sample)
            : std::make_pair(0.0f, 0.0f);
}

inline erased_access_policy_t make_erased_access_policy_view(
    const Data_access_policy& policy)
{
    erased_access_policy_t view              = policy.m_internal_access;
    bool                   uses_std_function = false;

    if (!view.get_timestamp && policy.get_timestamp) {
        view.get_timestamp = &std_function_access_timestamp;
        uses_std_function = true;
    }
    if (!view.get_value && policy.get_value) {
        view.get_value = &std_function_access_value;
        uses_std_function = true;
    }
    if (!view.get_range && policy.get_range) {
        view.get_range = &std_function_access_range;
        uses_std_function = true;
    }

    if (uses_std_function) {
        view.ctx = &policy;
        if (view.dispatch_kind == access_dispatch_kind_t::NONE) {
            view.dispatch_kind = access_dispatch_kind_t::STD_FUNCTION;
        }
        else
        if (view.dispatch_kind == access_dispatch_kind_t::MEMBER_POINTER) {
            view.dispatch_kind = access_dispatch_kind_t::MIXED;
        }
    }
    return view;
}

inline access_policy_cache_key_t make_access_policy_cache_key(
    const Data_access_policy*      policy,
    const erased_access_policy_t&  view)
{
    access_policy_cache_key_t key;
    key.identity      = policy;
    key.layout_key    = policy ? policy->layout_key : 0;
    key.revision      = policy ? policy->m_access_revision : 0;
    key.dispatch_kind = view.dispatch_kind;
    key.has_timestamp = view.has_timestamp();
    key.has_value     = view.has_value();
    key.has_range     = view.has_range();
    return key;
}

inline sample_semantics_key_t make_sample_semantics_key(
    const Data_access_policy* policy)
{
    if (!policy) {
        return {};
    }

    sample_semantics_key_t key = policy->semantics_key;
    if (key.conservative || key.value == 0) {
        key.value = 0;
        if (key.revision < policy->m_access_revision) {
            key.revision = policy->m_access_revision;
        }
        key.conservative = true;
    }
    return key;
}

} // namespace detail

enum class Series_interpolation
{
    LINEAR,
    STEP_AFTER
};

enum class Empty_window_behavior
{
    DRAW_NOTHING,
    /// Hold the most recent sample forward across an empty visible window.
    /// The renderer synthesizes this only after observing ascending timestamps,
    /// holding the window's last sample at `t_max`.
    HOLD_LAST_FORWARD
};

enum class Nonfinite_sample_policy
{
    BREAK_SEGMENT,
    SKIP,
    REPLACE_WITH_ZERO,
    REJECT_WINDOW,
};

enum class Data_query_status
{
    READY,
    EMPTY,
    BUSY,
    UNSUPPORTED,
    FAILED,
};

struct value_range_t
{
    float          min = 0.0f;
    float          max = 0.0f;
};

namespace detail {

enum class sample_draw_status_t
{
    DRAWABLE,
    SKIPPED,
    FAILED,
};

struct sample_draw_value_t
{
    float y     = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
};

sample_draw_status_t read_sample_draw_value(
    const erased_access_policy_t&  access,
    const void*                    sample,
    Nonfinite_sample_policy        policy,
    sample_draw_value_t&           out);

sample_draw_status_t read_sample_draw_value(
    const Data_access_policy&      access,
    const void*                    sample,
    Nonfinite_sample_policy        policy,
    sample_draw_value_t&           out);

} // namespace detail

template<typename T>
struct data_query_result_t
{
    Data_query_status          status                = Data_query_status::UNSUPPORTED;
    T                 value{};
    std::uint64_t              sequence              = 0;
};

struct data_query_context_t
{
    const Data_access_policy*  access                = nullptr;
    // Optional caller-owned profiler for query-internal work such as fallback scans.
    Profiler*                  profiler              = nullptr;
    sample_semantics_key_t     semantics_key;
    time_range_t               time_window{};
    Series_interpolation       interpolation         = Series_interpolation::LINEAR;
    Empty_window_behavior      empty_window_behavior = Empty_window_behavior::HOLD_LAST_FORWARD;
    Nonfinite_sample_policy    nonfinite_policy      = Nonfinite_sample_policy::BREAK_SEGMENT;
};

// -----------------------------------------------------------------------------
// Data_source: Abstract interface for data sources
// -----------------------------------------------------------------------------
class Data_source
{
public:
    virtual ~Data_source() = default;

    // Data_source decides whether snapshots are copied or direct views.
    // Callers that want buffering must implement it in their Data_source.
    virtual snapshot_result_t try_snapshot(size_t lod_level = 0) = 0;

    data_snapshot_t snapshot(size_t lod_level = 0)
    {
        auto result = try_snapshot(lod_level);
        return result ? result.snapshot : data_snapshot_t{};
    }

    virtual size_t lod_levels() const { return 1; }
    virtual size_t lod_scale(size_t level) const { (void)level; return 1; }
    virtual size_t sample_stride() const = 0;
    virtual const void* identity() const { return this; }

    /// Returns the current content revision for the given LOD level without
    /// taking a snapshot. A supported revision must remain nonzero, change
    /// whenever the logical snapshot content changes (including clear/reset),
    /// and equal the sequence returned by try_snapshot() while that content is
    /// current. Returning 0, or a revision that cannot satisfy this contract,
    /// disables sequence-based reuse.
    virtual uint64_t current_sequence(size_t lod_level = 0) const { (void)lod_level; return 0; }

    virtual std::vector<std::size_t> lod_scales() const;

    /// Resolve drawable values in the time window. STEP_AFTER also includes
    /// the drawable value held in from before its left edge when a following
    /// sample exists, even if that sample is beyond the right edge. With no
    /// following sample, only HOLD_LAST_FORWARD extends the last value.
    /// Overrides must apply the query's nonfinite policy to held values too.
    virtual data_query_result_t<value_range_t> query_v_range(
        std::size_t                    lod,
        const data_query_context_t&    query);

};

// -----------------------------------------------------------------------------
// Data_source_ref: Explicit owning/non-owning data source handle
// -----------------------------------------------------------------------------
struct Data_source_ref
{
    std::shared_ptr<Data_source> owner;
    Data_source* ptr = nullptr;

    Data_source_ref() = default;
    explicit Data_source_ref(std::shared_ptr<Data_source> source)
    {
        set(std::move(source));
    }
    explicit Data_source_ref(Data_source& source)
    {
        set_ref(source);
    }

    void set(std::shared_ptr<Data_source> source)
    {
        owner = std::move(source);
        ptr = owner.get();
    }

    void set_ref(Data_source& source)
    {
        owner.reset();
        ptr = &source;
    }

    void reset()
    {
        owner.reset();
        ptr = nullptr;
    }

    Data_source* get() const { return ptr; }

    explicit operator bool() const { return ptr != nullptr; }

    Data_source_ref& operator=(std::shared_ptr<Data_source> source)
    {
        set(std::move(source));
        return *this;
    }
};

// -----------------------------------------------------------------------------
// Vector_data_source: Simple vector-backed data source
// -----------------------------------------------------------------------------
template<typename T>
class Vector_data_source : public Data_source
{
public:
    Vector_data_source()
        : m_payload(std::make_shared<Payload>())
    {}

    explicit Vector_data_source(std::vector<T> data)
        : m_payload(std::make_shared<Payload>(std::move(data), 1))
    {}

    snapshot_result_t try_snapshot(size_t /*lod_level*/ = 0) override
    {
        std::shared_ptr<Payload> payload;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            payload = m_payload;
        }
        if (payload->data.empty()) {
            data_snapshot_t snapshot;
            snapshot.stride   = sizeof(T);
            snapshot.sequence = payload->sequence;
            snapshot.hold     = payload;
            return {
                snapshot,
                snapshot_result_t::Snapshot_status::EMPTY
            };
        }
        return {
            data_snapshot_t{
                payload->data.data(),
                payload->data.size(),
                sizeof(T),
                payload->sequence,
                nullptr,
                0,
                payload
            },
            snapshot_result_t::Snapshot_status::READY
        };
    }

    uint64_t current_sequence(size_t /*lod_level*/ = 0) const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_payload->sequence;
    }

    size_t sample_stride() const override { return sizeof(T); }

    void set_data(std::vector<T> data)
    {
        auto new_payload = std::make_shared<Payload>(std::move(data), 0);
        std::shared_ptr<Payload> old_payload;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            new_payload->sequence = m_payload->sequence + 1;
            old_payload           = std::move(m_payload);
            m_payload             = std::move(new_payload);
        }
    }

private:
    struct Payload
    {
        Payload() = default;

        Payload(std::vector<T> payload_data, std::uint64_t payload_sequence)
        :
            data(std::move(payload_data)),
            sequence(payload_sequence)
        {}

        std::vector<T> data;
        std::uint64_t  sequence = 0;
    };

    mutable std::mutex         m_mutex;
    std::shared_ptr<Payload>   m_payload;
};

// -----------------------------------------------------------------------------
// Display Styles (bit flags for combination)
// -----------------------------------------------------------------------------
enum class Display_style : int
{
    NONE           = 0x0,
    DOTS           = 0x1,
    LINE           = 0x2,
    AREA           = 0x4,
    DOTS_LINE      = DOTS | LINE,
    DOTS_AREA      = DOTS | AREA,
    LINE_AREA      = LINE | AREA,
    DOTS_LINE_AREA = DOTS | LINE | AREA
};

inline Display_style operator|(Display_style a, Display_style b)
{
    return static_cast<Display_style>(static_cast<int>(a) | static_cast<int>(b));
}

inline Display_style operator&(Display_style a, Display_style b)
{
    return static_cast<Display_style>(static_cast<int>(a) & static_cast<int>(b));
}

inline bool operator!(Display_style s)
{
    return static_cast<int>(s) == 0;
}

constexpr int k_visible_info_none        = 0x0;
constexpr int k_visible_info_value_range = 0x1;
constexpr int k_visible_info_time_range  = 0x2;
constexpr int k_visible_info_all         = k_visible_info_value_range | k_visible_info_time_range;

// -----------------------------------------------------------------------------
// Preview Configuration
// -----------------------------------------------------------------------------
struct preview_config_t
{
    Data_source_ref    data_source;   // required when preview_config is set
    Data_access_policy access;        // optional; if invalid, fall back to main access
    std::optional<Display_style>
                       style;         // nullopt means use main style
    std::optional<Series_interpolation>
                       interpolation; // nullopt means use main interpolation
};

// -----------------------------------------------------------------------------
// Data Configuration
// -----------------------------------------------------------------------------
struct data_config_t
{
    float          v_min        = -1.f;
    float          v_max        = 1.f;
    float          v_manual_min = 0.f;
    float          v_manual_max = 5.f;

    // Timestamps are int64_t nanoseconds (API convention). The defaults
    // describe a 10-second view starting at 0 ns; every Plot_widget user
    // is expected to call set_view (or attach a configured Plot_time_axis)
    // before the first paint, but if neither happens the widget renders a
    // sane 10-second window instead of a 5-microsecond one. The previous
    // 5000 / 10000 / 0 / 10000 literals were carried over from a pre-int64
    // era when the unit was seconds and described a 10000-second view.
    std::int64_t   t_min           = 0;
    std::int64_t   t_max           = std::int64_t{10} * 1'000'000'000;
    std::int64_t   t_available_min = 0;
    std::int64_t   t_available_max = std::int64_t{10} * 1'000'000'000;

    // Deprecated: this field is ignored by rendering. Use the widget's
    // vbar_width_qml()/vbar_width_pixels() or explicit layout parameters.
    double vbar_width = 150.;
};

// -----------------------------------------------------------------------------
// series_data_t: Unified series descriptor using Data_access_policy
// -----------------------------------------------------------------------------
struct series_data_t
{
    virtual ~series_data_t() = default;

    bool                               enabled               = true;
    Display_style                      style                 = Display_style::LINE;
    Series_interpolation               interpolation         = Series_interpolation::LINEAR;
    Empty_window_behavior              empty_window_behavior = Empty_window_behavior::DRAW_NOTHING;
    Nonfinite_sample_policy            nonfinite_policy      = Nonfinite_sample_policy::BREAK_SEGMENT;
    glm::vec4                          color                 = glm::vec4(0.16f, 0.45f, 0.64f, 1.0f);
    std::string                        series_label;
    // Non-zero values opt ordinary series into one cumulative stack. Zero
    // preserves independent rendering. Members are ordered bottom-to-top by
    // ascending plot ID, so reordering also requires updating plot IDs.
    int                                stack_group           = 0;

    Data_source_ref                    data_source;
    Data_access_policy                 access;
    // Optional per-series preview configuration. When set, preview rendering can
    // use a distinct data source, access policy, and style.
    std::optional<preview_config_t>    preview_config;

    [[nodiscard]] virtual std::shared_ptr<series_data_t> clone() const
    {
        return std::make_shared<series_data_t>(*this);
    }

    std::int64_t get_timestamp(const void* sample) const
    {
        return access.get_timestamp ? access.get_timestamp(sample) : std::int64_t{0};
    }

    float get_value(const void* sample) const
    {
        return access.get_value ? access.get_value(sample) : 0.0f;
    }

    std::pair<float, float> get_range(const void* sample) const
    {
        return access.get_range ? access.get_range(sample) : std::make_pair(0.0f, 0.0f);
    }

    void set_data_source(std::shared_ptr<Data_source> source)
    {
        data_source.set(std::move(source));
    }

    void set_data_source_ref(Data_source& source)
    {
        data_source.set_ref(source);
    }

    Data_source* main_source() const
    {
        return data_source.get();
    }

    // Returns preview source; null means preview is skipped.
    Data_source* preview_source() const
    {
        if (!preview_config) {
            return data_source.get();
        }
        return preview_config->data_source.get();
    }

    // Main access policy.
    const Data_access_policy& main_access() const
    {
        return access;
    }

    // Preview access policy (falls back to main when invalid).
    const Data_access_policy& preview_access() const
    {
        if (preview_config && preview_config->access.is_valid()) {
            return preview_config->access;
        }
        return access;
    }

    // Preview style (falls back to main when unset).
    Display_style effective_preview_style() const
    {
        if (preview_config && preview_config->style) {
            return *preview_config->style;
        }
        return style;
    }

    Series_interpolation effective_preview_interpolation() const
    {
        if (preview_config && preview_config->interpolation) {
            return *preview_config->interpolation;
        }
        return interpolation;
    }

    bool has_preview_config() const { return preview_config.has_value(); }

    bool preview_matches_main() const
    {
        if (!preview_config) {
            return true;
        }
        Data_source* prev = preview_source();
        if (!data_source || !prev || data_source.get() != prev) {
            return false;
        }
        return
            preview_access().layout_key       == access.layout_key &&
            effective_preview_style()         == style             &&
            effective_preview_interpolation() == interpolation;
    }

};

// -----------------------------------------------------------------------------
// Grid and Layout Types
// -----------------------------------------------------------------------------
struct grid_layer_params_t
{
    static constexpr int   k_max_levels               = 32;
    int                    count                      = 0;
    float                  spacing_px[k_max_levels]   = {};
    float                  start_px[k_max_levels]     = {};
    float                  alpha[k_max_levels]        = {};
    float                  thickness_px[k_max_levels] = {};
};

/// Vertical axis label (value bar on the right side of plot)
struct v_label_t
{
    double         value; ///< Data value this label represents
    float          y;     ///< Y position in pixels (from bottom)
    std::string    text;  ///< Formatted label text
};

/// Horizontal axis label (time bar below plot)
struct h_label_t
{
    std::int64_t   value;    ///< Timestamp this label represents (nanoseconds)
    glm::vec2      position; ///< Position in pixels (x, y from bottom-left)
    std::string    text;     ///< Formatted label text
};

/// Result of layout calculation for a single frame.
/// Contains computed dimensions and pre-positioned labels.
struct frame_layout_result_t
{
    double                 usable_width = 0.0;  ///< Plot area width in pixels
    double                 usable_height = 0.0; ///< Plot area height in pixels
    double                 v_bar_width = 0.0;
    float                  max_v_label_text_width = 0.f;

    std::vector<h_label_t> h_labels;
    std::vector<v_label_t> v_labels;

    int                    v_label_fixed_digits = 0;
    bool                   h_labels_subsecond = false;

};

/// Key for layout caching. Layout is recomputed only when this key changes.
struct layout_cache_key_t
{
    float          v0                           = 0.0f;
    float          v1                           = 0.0f;
    std::int64_t   t0                           = 0; // nanoseconds
    std::int64_t   t1                           = 0; // nanoseconds
    Size_2i        viewport_size;
    double         adjusted_reserved_height     = 0.0;
    double         adjusted_preview_height      = 0.0;
    double         adjusted_font_size_in_pixels = 0.0;
    double         vbar_width_pixels            = 0.0;
    uint64_t       font_metrics_key             = 0;
    uint64_t       config_revision              = 0;
    uint64_t       format_timestamp_revision    = 0;
    uint64_t       format_value_revision        = 0;

    [[nodiscard]] bool operator==(const layout_cache_key_t& other) const noexcept
    {
        const bool layout_fields_match =
            v0                           == other.v0                           &&
            v1                           == other.v1                           &&
            t0                           == other.t0                           &&
            t1                           == other.t1                           &&
            viewport_size                == other.viewport_size                &&
            adjusted_reserved_height     == other.adjusted_reserved_height     &&
            adjusted_preview_height      == other.adjusted_preview_height      &&
            adjusted_font_size_in_pixels == other.adjusted_font_size_in_pixels &&
            vbar_width_pixels            == other.vbar_width_pixels            &&
            font_metrics_key             == other.font_metrics_key;

        return
            layout_fields_match                                          &&
            config_revision           == other.config_revision           &&
            format_timestamp_revision == other.format_timestamp_revision &&
            format_value_revision     == other.format_value_revision;
    }

};

class Layout_cache
{
public:
    using key_t      = layout_cache_key_t;
    using value_type = frame_layout_result_t;

    [[nodiscard]] const value_type* try_get(const key_t& query) const noexcept
    {
        if (!m_valid || m_key != query) {
            return nullptr;
        }
        return &m_value;
    }

    [[nodiscard]] const value_type& store(const key_t& new_key, value_type&& new_value) noexcept
    {
        m_key   = new_key;
        m_value = std::move(new_value);
        m_valid = true;
        return m_value;
    }

    void invalidate() noexcept { m_valid = false; }

private:
    bool       m_valid = false;
    key_t      m_key{};
    value_type m_value{};
};

} // namespace vnm::plot
