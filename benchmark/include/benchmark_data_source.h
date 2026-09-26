// vnm_plot Benchmark - Data Source
// Bridges Ring_buffer to vnm::plot::Data_source interface with zero-copy snapshots

#ifndef VNM_PLOT_BENCHMARK_DATA_SOURCE_H
#define VNM_PLOT_BENCHMARK_DATA_SOURCE_H

#include "ring_buffer.h"
#include "sample_types.h"

#include <vnm_plot/core/types.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace vnm::benchmark {

/// Unique layout keys for benchmark sample types
constexpr uint64_t k_bar_sample_layout_key = 0x424152;      // "BAR" in ASCII
constexpr uint64_t k_trade_sample_layout_key = 0x545244;    // "TRD" in ASCII

/// Data source that wraps a Ring_buffer and implements vnm::plot::Data_source.
/// Provides a zero-copy snapshot view into the ring buffer while holding a
/// shared lock, so consumers can read safely without an intermediate copy.
template<typename T>
class Benchmark_data_source : public vnm::plot::Data_source {
public:
    /// Construct with reference to a ring buffer.
    /// @param buffer Ring buffer to read from (must outlive this data source)
    explicit Benchmark_data_source(Ring_buffer<T>& buffer)
        : m_buffer(buffer)
    {}

    ~Benchmark_data_source() override = default;

    /// Take a snapshot view of the ring buffer.
    /// Thread-safe: holds a shared lock for the snapshot lifetime.
    vnm::plot::snapshot_result_t try_snapshot(size_t lod_level = 0) override {
        using Status = vnm::plot::snapshot_result_t::Snapshot_status;

        // Only LOD level 0 is supported
        if (lod_level > 0) {
            return {vnm::plot::data_snapshot_t{}, Status::FAILED};
        }

        const auto started = std::chrono::steady_clock::now();
        auto view = m_buffer.view();
        m_snapshot_sequence.store(view.sequence, std::memory_order_release);

        if (m_profiler) {
            const double snapshot_ns = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - started).count();
            m_profiler->record_observation("benchmark.snapshot.time_ns", snapshot_ns);
            m_profiler->record_counter("benchmark.snapshot.count");
            m_profiler->record_observation(
                "benchmark.snapshot.view_bytes",
                static_cast<double>(view.count * sizeof(T)));
            m_profiler->record_observation("benchmark.snapshot.copied_bytes", 0.0);
        }

        vnm::plot::data_snapshot_t snapshot;
        snapshot.data = view.data;
        snapshot.count = view.count;
        snapshot.stride = sizeof(T);
        snapshot.sequence = view.sequence;
        snapshot.data2 = view.data2;
        snapshot.count2 = view.count2;

        if (view.count == 0) {
            return {snapshot, Status::EMPTY};
        }

        snapshot.hold = view.lock;

        return {
            snapshot,
            Status::READY
        };
    }

    size_t sample_stride() const override {
        return sizeof(T);
    }

    size_t lod_levels() const override {
        return 1;
    }

    size_t lod_scale(size_t level) const override {
        return level == 0 ? 1 : 0;
    }

    uint64_t current_sequence(size_t lod_level = 0) const override {
        return lod_level == 0 ? m_buffer.sequence() : 0;
    }

    /// Get current sequence for change detection
    uint64_t sequence() const {
        return m_snapshot_sequence.load(std::memory_order_acquire);
    }

    void set_profiler(vnm::plot::Profiler* profiler) noexcept {
        m_profiler = profiler;
    }

private:
    Ring_buffer<T>& m_buffer;
    std::atomic<uint64_t> m_snapshot_sequence{0};
    vnm::plot::Profiler* m_profiler = nullptr;
};

/// Create a Data_access_policy for Bar_sample. The renderer owns the GPU-side
/// sample layout (gpu_sample_t) and rebases timestamps on upload, so the
/// policy only exposes value extractors plus a stable layout_key for caching.
inline vnm::plot::Data_access_policy make_bar_access_policy() {
    vnm::plot::Data_access_policy policy;

    policy.get_timestamp = [](const void* sample) -> std::int64_t {
        return static_cast<const Bar_sample*>(sample)->timestamp;
    };

    policy.get_value = [](const void* sample) -> float {
        return static_cast<const Bar_sample*>(sample)->close;
    };

    policy.get_range = [](const void* sample) -> std::pair<float, float> {
        const auto* bar = static_cast<const Bar_sample*>(sample);
        return {bar->low, bar->high};
    };

    policy.layout_key = k_bar_sample_layout_key;

    return policy;
}

/// Create a Data_access_policy for Trade_sample. The renderer owns the GPU-side
/// sample layout (gpu_sample_t) and rebases timestamps on upload, so the
/// policy only exposes value extractors plus a stable layout_key for caching.
inline vnm::plot::Data_access_policy make_trade_access_policy() {
    vnm::plot::Data_access_policy policy;

    policy.get_timestamp = [](const void* sample) -> std::int64_t {
        return static_cast<const Trade_sample*>(sample)->timestamp;
    };

    policy.get_value = [](const void* sample) -> float {
        return static_cast<const Trade_sample*>(sample)->price;
    };

    policy.get_range = [](const void* sample) -> std::pair<float, float> {
        const auto* trade = static_cast<const Trade_sample*>(sample);
        return {trade->price, trade->price};
    };

    policy.layout_key = k_trade_sample_layout_key;

    return policy;
}

}  // namespace vnm::benchmark

#endif  // VNM_PLOT_BENCHMARK_DATA_SOURCE_H
