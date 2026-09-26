// vnm_plot Benchmark - Data Source Tests

#include "benchmark_data_source.h"
#include "brownian_generator.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <future>
#include <limits>
#include <vector>

using vnm::benchmark::Bar_sample;
using vnm::benchmark::Benchmark_data_source;
using vnm::benchmark::Brownian_generator;
using vnm::benchmark::Ring_buffer;
using vnm::benchmark::Trade_sample;
using vnm::benchmark::k_bar_sample_layout_key;
using vnm::benchmark::k_trade_sample_layout_key;
using vnm::benchmark::make_bar_access_policy;
using vnm::benchmark::make_trade_access_policy;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << " at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while(0)

#define RUN_TEST(test_fn) \
    do { \
        std::cout << "Running " << #test_fn << "... "; \
        if (test_fn()) { \
            std::cout << "PASS" << std::endl; \
            ++passed; \
        } \
        else { \
            std::cout << "FAIL" << std::endl; \
            ++failed; \
        } \
    } while(0)

template<typename Source>
vnm::plot::data_query_result_t<vnm::plot::value_range_t> query_full_range(
    Source& source,
    const vnm::plot::Data_access_policy& access)
{
    vnm::plot::data_query_context_t query;
    query.access = &access;
    query.time_window = {
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()
    };
    return source.query_v_range(0, query);
}

// Test: Empty buffer returns EMPTY status and empty query result
bool test_empty_buffer() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    auto result = source.try_snapshot();

    TEST_ASSERT(result.status == vnm::plot::snapshot_result_t::Snapshot_status::EMPTY,
                "empty buffer should return EMPTY status");
    TEST_ASSERT(result.snapshot.count == 0, "snapshot count should be 0");

    const auto query_result = query_full_range(source, make_bar_access_policy());
    TEST_ASSERT(query_result.status == vnm::plot::Data_query_status::EMPTY,
                "empty buffer range query should return EMPTY");

    return true;
}

// Test: Snapshot contains data after push
bool test_snapshot_with_data() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    // Push some data. Timestamps are int64 nanoseconds (API convention).
    Bar_sample bar{};
    bar.timestamp = 1'000'000'000;  // 1.0 s
    bar.open = 100.0f;
    bar.high = 105.0f;
    bar.low = 95.0f;
    bar.close = 102.0f;
    bar.volume = 1000.0f;
    buffer.push(bar);

    auto result = source.try_snapshot();

    TEST_ASSERT(result.status == vnm::plot::snapshot_result_t::Snapshot_status::READY,
                "should return OK status");
    TEST_ASSERT(result.snapshot.count == 1, "snapshot count should be 1");
    TEST_ASSERT(result.snapshot.stride == sizeof(Bar_sample), "stride should match");
    TEST_ASSERT(result.snapshot.data != nullptr, "data should not be null");

    return true;
}

// Test: Snapshot data is correct
bool test_snapshot_data_correctness() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    // Push specific data. Timestamps are int64 nanoseconds (API convention).
    constexpr int64_t k_test_ts_ns = 123'456'000'000;  // 123.456 s in ns
    Bar_sample bar{};
    bar.timestamp = k_test_ts_ns;
    bar.open = 100.0f;
    bar.high = 110.0f;
    bar.low = 90.0f;
    bar.close = 105.0f;
    bar.volume = 5000.0f;
    buffer.push(bar);

    auto result = source.try_snapshot();
    const auto* sample = static_cast<const Bar_sample*>(result.snapshot.at(0));
    TEST_ASSERT(sample != nullptr, "sample should be available");
    TEST_ASSERT(sample->timestamp == k_test_ts_ns, "timestamp should match");
    TEST_ASSERT(sample->close == 105.0f, "close should match");

    return true;
}

// Test: Value range computation for bars
bool test_bar_value_range() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    // Push bars with known ranges
    Bar_sample bar1{};
    bar1.low = 90.0f;
    bar1.high = 100.0f;
    buffer.push(bar1);

    Bar_sample bar2{};
    bar2.low = 85.0f;  // New minimum
    bar2.high = 95.0f;
    buffer.push(bar2);

    Bar_sample bar3{};
    bar3.low = 92.0f;
    bar3.high = 110.0f;  // New maximum
    buffer.push(bar3);

    const auto query_result = query_full_range(source, make_bar_access_policy());
    TEST_ASSERT(query_result.status == vnm::plot::Data_query_status::READY,
                "bar range query should be READY");
    TEST_ASSERT(query_result.value.min == 85.0f, "min should be 85.0");
    TEST_ASSERT(query_result.value.max == 110.0f, "max should be 110.0");

    return true;
}

// Test: Value range for trades
bool test_trade_value_range() {
    Ring_buffer<Trade_sample> buffer(100);
    Benchmark_data_source<Trade_sample> source(buffer);

    Trade_sample t1{};
    t1.price = 100.0f;
    buffer.push(t1);

    Trade_sample t2{};
    t2.price = 90.0f;
    buffer.push(t2);

    Trade_sample t3{};
    t3.price = 110.0f;
    buffer.push(t3);

    const auto query_result = query_full_range(source, make_trade_access_policy());
    TEST_ASSERT(query_result.status == vnm::plot::Data_query_status::READY,
                "trade range query should be READY");
    TEST_ASSERT(query_result.value.min == 90.0f, "min should be 90.0");
    TEST_ASSERT(query_result.value.max == 110.0f, "max should be 110.0");

    return true;
}

// Test: Sample stride
bool test_sample_stride() {
    Ring_buffer<Bar_sample> bar_buffer(100);
    Benchmark_data_source<Bar_sample> bar_source(bar_buffer);

    Ring_buffer<Trade_sample> trade_buffer(100);
    Benchmark_data_source<Trade_sample> trade_source(trade_buffer);

    TEST_ASSERT(bar_source.sample_stride() == 28, "Bar stride should be 28");
    TEST_ASSERT(trade_source.sample_stride() == 16, "Trade stride should be 16");

    return true;
}

// Test: Sequence tracking
bool test_sequence_tracking() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    TEST_ASSERT(source.sequence() == 0, "initial sequence should be 0");

    Bar_sample bar{};
    buffer.push(bar);
    source.try_snapshot();

    TEST_ASSERT(source.sequence() == 2, "snapshot should expose revision 2 after one push");

    buffer.push(bar);
    buffer.push(bar);
    source.try_snapshot();

    TEST_ASSERT(source.sequence() == 4, "snapshot should expose revision 4 after three pushes");

    return true;
}

// Test: One LOD preserves insertion order across a wrapped ring snapshot
bool test_single_lod_preserves_insertion_order() {
    Ring_buffer<Bar_sample> buffer(3);
    Benchmark_data_source<Bar_sample> source(buffer);

    TEST_ASSERT(source.lod_levels() == 1, "benchmark source should expose one LOD");
    TEST_ASSERT(source.lod_scale(0) == 1, "LOD 0 scale should be 1");
    TEST_ASSERT(source.lod_scale(1) == 0, "unsupported LOD should not expose a scale");

    const std::vector<std::size_t> scales = source.lod_scales();
    TEST_ASSERT(scales.size() == 1, "lod_scales should expose one scale");
    TEST_ASSERT(scales[0] == 1, "lod_scales should pin scale 1 for LOD 0");

    Bar_sample first{};
    first.timestamp = 30;
    Bar_sample second{};
    second.timestamp = 10;
    Bar_sample third{};
    third.timestamp = 20;
    Bar_sample fourth{};
    fourth.timestamp = 5;
    buffer.push(first);
    buffer.push(second);
    buffer.push(third);
    buffer.push(fourth);

    auto result = source.try_snapshot();
    TEST_ASSERT(result.status == vnm::plot::snapshot_result_t::Snapshot_status::READY,
                "wrapped snapshot should be READY");
    TEST_ASSERT(result.snapshot.is_segmented(), "snapshot should be segmented after wrap");
    const auto* sample0 = static_cast<const Bar_sample*>(result.snapshot.at(0));
    const auto* sample2 = static_cast<const Bar_sample*>(result.snapshot.at(2));
    TEST_ASSERT(sample0 != nullptr && sample2 != nullptr,
                "wrapped snapshot samples should be available");
    TEST_ASSERT(sample0->timestamp == 10 && sample2->timestamp == 5,
                "ring view should preserve insertion order after wrap");

    return true;
}

// Test: current_sequence exposes the same stable nonzero ring revision
bool test_current_sequence_metadata() {
    Ring_buffer<Trade_sample> buffer(4);
    Benchmark_data_source<Trade_sample> source(buffer);

    TEST_ASSERT(source.current_sequence(0) == 1,
                "initial LOD 0 current_sequence should expose stable revision 1");
    TEST_ASSERT(source.current_sequence(1) == 0,
                "unsupported LOD current_sequence should be 0");
    TEST_ASSERT(source.sequence() == 0,
                "last snapshot sequence should start at 0");

    auto empty_result = source.try_snapshot();
    TEST_ASSERT(empty_result.status == vnm::plot::snapshot_result_t::Snapshot_status::EMPTY,
                "initial snapshot should be EMPTY");
    TEST_ASSERT(empty_result.snapshot.sequence == source.current_sequence(0),
                "EMPTY snapshot and current_sequence should expose the same revision");

    Trade_sample trade{};
    buffer.push(trade);
    buffer.push(trade);

    TEST_ASSERT(source.current_sequence(0) == 3,
                "current_sequence should advance independently of snapshots");
    TEST_ASSERT(source.current_sequence(1) == 0,
                "unsupported LOD current_sequence should stay 0");
    TEST_ASSERT(source.sequence() == 1,
                "last snapshot sequence should retain the initial EMPTY revision");

    auto result = source.try_snapshot();
    TEST_ASSERT(result.status == vnm::plot::snapshot_result_t::Snapshot_status::READY,
                "snapshot should be READY after pushes");
    TEST_ASSERT(result.snapshot.sequence == 3,
                "snapshot sequence should match ring sequence");
    TEST_ASSERT(source.sequence() == 3,
                "last snapshot sequence should update after try_snapshot");
    result = {};

    buffer.clear();
    TEST_ASSERT(source.current_sequence(0) == 4,
                "clear should advance current_sequence without resetting it");
    empty_result = source.try_snapshot();
    TEST_ASSERT(empty_result.status == vnm::plot::snapshot_result_t::Snapshot_status::EMPTY,
                "snapshot after clear should be EMPTY");
    TEST_ASSERT(empty_result.snapshot.sequence == source.current_sequence(0),
                "cleared snapshot and current_sequence should expose the same revision");
    buffer.push(trade);
    TEST_ASSERT(source.current_sequence(0) == 5,
                "push after clear should continue the stable revision");
    TEST_ASSERT(source.current_sequence(1) == 0,
                "unsupported LOD current_sequence should stay 0 after reset");

    return true;
}

// Test: Bar access policy
bool test_bar_access_policy() {
    auto policy = make_bar_access_policy();

    TEST_ASSERT(policy.is_valid(), "policy should be valid");

    constexpr int64_t k_bar_ts_ns = 1'000'000'000'000;  // 1000.0 s in ns
    Bar_sample bar{};
    bar.timestamp = k_bar_ts_ns;
    bar.close = 105.5f;
    bar.low = 100.0f;
    bar.high = 110.0f;
    bar.volume = 5000.0f;

    TEST_ASSERT(policy.get_timestamp(&bar) == k_bar_ts_ns, "timestamp extraction");
    TEST_ASSERT(policy.get_value(&bar) == 105.5f, "value extraction (close)");

    auto [lo, hi] = policy.get_range(&bar);
    TEST_ASSERT(lo == 100.0f, "range low");
    TEST_ASSERT(hi == 110.0f, "range high");

    return true;
}

// Test: Trade access policy
bool test_trade_access_policy() {
    auto policy = make_trade_access_policy();

    TEST_ASSERT(policy.is_valid(), "policy should be valid");

    constexpr int64_t k_trade_ts_ns = 2'000'000'000'000;  // 2000.0 s in ns
    Trade_sample trade{};
    trade.timestamp = k_trade_ts_ns;
    trade.price = 99.5f;
    trade.size = 100.0f;

    TEST_ASSERT(policy.get_timestamp(&trade) == k_trade_ts_ns, "timestamp extraction");
    TEST_ASSERT(policy.get_value(&trade) == 99.5f, "value extraction (price)");

    auto [lo, hi] = policy.get_range(&trade);
    TEST_ASSERT(lo == 99.5f, "range low == price");
    TEST_ASSERT(hi == 99.5f, "range high == price");

    return true;
}

// Test: Snapshot view refresh reflects new data
bool test_snapshot_live_view() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    // Push initial data
    Bar_sample bar1{};
    bar1.close = 100.0f;
    buffer.push(bar1);

    // Take snapshot
    auto result1 = source.try_snapshot();
    const auto* sample1 = static_cast<const Bar_sample*>(result1.snapshot.at(0));
    TEST_ASSERT(sample1 != nullptr, "initial sample should be available");
    float initial_close = sample1->close;
    result1 = {};

    // Modify buffer after snapshot
    Bar_sample bar2{};
    bar2.close = 200.0f;
    buffer.push(bar2);

    // Now take new snapshot
    auto result2 = source.try_snapshot();
    TEST_ASSERT(result2.snapshot.count == 2, "new snapshot should have 2 samples");
    const auto* sample2 = static_cast<const Bar_sample*>(result2.snapshot.at(0));
    TEST_ASSERT(sample2 != nullptr, "updated sample should be available");
    TEST_ASSERT(sample2->close == initial_close,
                "existing sample should match after refresh");

    return true;
}

// Test: Integration with Brownian generator
bool test_brownian_integration() {
    Brownian_generator::Config config;
    config.seed = 42;
    Brownian_generator gen(config);

    Ring_buffer<Bar_sample> buffer(1000);
    Benchmark_data_source<Bar_sample> source(buffer);

    // Generate some bars
    for (int i = 0; i < 100; ++i) {
        buffer.push(gen.next_bar());
    }

    auto result = source.try_snapshot();
    TEST_ASSERT(result.status == vnm::plot::snapshot_result_t::Snapshot_status::READY, "should be READY");
    TEST_ASSERT(result.snapshot.count == 100, "should have 100 samples");

    // Value range should be valid
    const auto query_result = query_full_range(source, make_bar_access_policy());
    TEST_ASSERT(query_result.status == vnm::plot::Data_query_status::READY,
                "generated bar range query should be READY");
    TEST_ASSERT(query_result.value.min < query_result.value.max,
                "min should be less than max");
    TEST_ASSERT(query_result.value.min > 0.0f,
                "min should be positive (price data)");

    return true;
}

// Test: Unsupported LOD levels return FAILED
bool test_unsupported_lod() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    // Push some data first
    Bar_sample bar{};
    buffer.push(bar);

    // LOD 0 should work
    auto result0 = source.try_snapshot(0);
    TEST_ASSERT(result0.status == vnm::plot::snapshot_result_t::Snapshot_status::READY,
                "LOD 0 should return OK");

    // LOD 1 should fail
    auto result1 = source.try_snapshot(1);
    TEST_ASSERT(result1.status == vnm::plot::snapshot_result_t::Snapshot_status::FAILED,
                "LOD 1 should return FAILED");

    // LOD 100 should fail
    auto result100 = source.try_snapshot(100);
    TEST_ASSERT(result100.status == vnm::plot::snapshot_result_t::Snapshot_status::FAILED,
                "LOD 100 should return FAILED");

    return true;
}

// Test: Stable sequence preserves the zero-copy view identity.
bool test_sequence_short_circuit_preserves_view_identity() {
    Ring_buffer<Bar_sample> buffer(100);
    Benchmark_data_source<Bar_sample> source(buffer);

    // Push some data
    Bar_sample bar{};
    bar.close = 100.0f;
    buffer.push(bar);

    // First and second snapshots: same data, same cached pointer expected.
    // Each snapshot holds a shared_lock on the ring buffer for its
    // lifetime, so a subsequent push() would block on the writer lock
    // until every outstanding snapshot is released. Capture the
    // identity bits we need to assert on, then drop the snapshots
    // before pushing further data.
    std::size_t result1_count = 0;
    const void* result1_data = nullptr;
    uint64_t seq1 = 0;
    {
        auto result1 = source.try_snapshot();
        TEST_ASSERT(result1.status == vnm::plot::snapshot_result_t::Snapshot_status::READY,
                    "first snapshot should be READY");
        result1_count = result1.snapshot.count;
        result1_data = result1.snapshot.data;
        seq1 = source.sequence();

        auto result2 = source.try_snapshot();
        TEST_ASSERT(result2.status == vnm::plot::snapshot_result_t::Snapshot_status::READY,
                    "cached snapshot should be READY");
        TEST_ASSERT(result2.snapshot.count == result1_count,
                    "cached snapshot should have same count");
        TEST_ASSERT(result2.snapshot.data == result1_data,
                    "cached snapshot should return same pointer");
    }

    // Push new data. Snapshot locks released above; push acquires the
    // writer lock cleanly.
    Bar_sample bar2{};
    bar2.close = 200.0f;
    buffer.push(bar2);

    // Third snapshot should detect the new revision and expose the expanded view.
    auto result3 = source.try_snapshot();
    TEST_ASSERT(result3.status == vnm::plot::snapshot_result_t::Snapshot_status::READY,
                "new snapshot should be READY");
    TEST_ASSERT(result3.snapshot.count == 2, "new snapshot should have 2 samples");
    TEST_ASSERT(source.sequence() > seq1, "sequence should have increased");

    return true;
}

// Test: Access policies expose stable, distinct layout_keys for cache identity
bool test_access_policy_layout_keys() {
    auto bar_policy = make_bar_access_policy();
    auto trade_policy = make_trade_access_policy();

    TEST_ASSERT(bar_policy.layout_key != 0, "bar policy should have non-zero layout_key");
    TEST_ASSERT(trade_policy.layout_key != 0, "trade policy should have non-zero layout_key");
    TEST_ASSERT(bar_policy.layout_key != trade_policy.layout_key,
                "bar and trade policies should have different layout_keys");

    TEST_ASSERT(bar_policy.layout_key == k_bar_sample_layout_key,
                "bar policy layout_key should match constant");
    TEST_ASSERT(trade_policy.layout_key == k_trade_sample_layout_key,
                "trade policy layout_key should match constant");

    return true;
}

// Test: release a direct-view snapshot before a second snapshot-backed query
bool test_release_snapshot_before_nested_query() {
    Ring_buffer<Bar_sample> buffer(8);
    Benchmark_data_source<Bar_sample> source(buffer);

    Bar_sample first{};
    first.timestamp = 1;
    first.low = 9.0f;
    first.high = 11.0f;
    buffer.push(first);

    auto snapshot_result = source.try_snapshot();
    TEST_ASSERT(snapshot_result.status ==
            vnm::plot::snapshot_result_t::Snapshot_status::READY,
        "initial direct-view snapshot should be READY");

    std::promise<void> writer_started;
    auto writer_started_future = writer_started.get_future();
    auto writer = std::async(std::launch::async, [&]() {
        writer_started.set_value();
        Bar_sample second{};
        second.timestamp = 2;
        second.low = 19.0f;
        second.high = 21.0f;
        buffer.push(second);
    });
    writer_started_future.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    snapshot_result = {};
    TEST_ASSERT(writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
        "writer should complete after the direct-view snapshot is released");
    writer.get();

    auto access = make_bar_access_policy();
    vnm::plot::data_query_context_t query;
    query.access = &access;
    query.time_window = {0, 10};
    const auto range = source.query_v_range(0, query);
    TEST_ASSERT(range.status == vnm::plot::Data_query_status::READY,
        "snapshot-backed range query should complete after writer progress");
    TEST_ASSERT(range.value.min == 9.0f && range.value.max == 21.0f,
        "range query should include both published bars");
    return true;
}

int main() {
    std::cout << "Benchmark Data Source Test Suite\n";
    std::cout << "=================================\n\n";

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_empty_buffer);
    RUN_TEST(test_snapshot_with_data);
    RUN_TEST(test_snapshot_data_correctness);
    RUN_TEST(test_bar_value_range);
    RUN_TEST(test_trade_value_range);
    RUN_TEST(test_sample_stride);
    RUN_TEST(test_sequence_tracking);
    RUN_TEST(test_single_lod_preserves_insertion_order);
    RUN_TEST(test_current_sequence_metadata);
    RUN_TEST(test_bar_access_policy);
    RUN_TEST(test_trade_access_policy);
    RUN_TEST(test_snapshot_live_view);
    RUN_TEST(test_brownian_integration);
    RUN_TEST(test_unsupported_lod);
    RUN_TEST(test_sequence_short_circuit_preserves_view_identity);
    RUN_TEST(test_access_policy_layout_keys);
    RUN_TEST(test_release_snapshot_before_nested_query);

    std::cout << "\n=================================\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";

    return failed > 0 ? 1 : 0;
}
