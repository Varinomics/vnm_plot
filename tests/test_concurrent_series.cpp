// vnm_plot concurrent data-source tests

#include "test_macros.h"

#include <vnm_plot/core/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace plot = vnm::plot;

namespace {

struct sample_t
{
    double             t = 0.0;
    float              v = 0.0f;
};

struct Blocking_publication_probe
{
    std::atomic<bool>  block_next_destroy{false};
    std::atomic<bool>  destroy_entered{false};
    std::atomic<bool>  release_destroy{false};
};

struct Blocking_sample
{
    double t = 0.0;
    float  v = 0.0f;
    std::shared_ptr<Blocking_publication_probe>
           probe;

    ~Blocking_sample()
    {
        if (!probe ||
            !probe->block_next_destroy.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        probe->destroy_entered.store(true, std::memory_order_release);
        while (!probe->release_destroy.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

const sample_t* sample_at(const plot::Data_snapshot& snapshot, std::size_t index)
{
    return reinterpret_cast<const sample_t*>(snapshot.at(index));
}

const Blocking_sample* blocking_sample_at(
    const plot::Data_snapshot&     snapshot,
    std::size_t                    index)
{
    return reinterpret_cast<const Blocking_sample*>(snapshot.at(index));
}

std::vector<Blocking_sample> make_blocking_samples(
    float                                          generation,
    std::shared_ptr<Blocking_publication_probe>    probe = {})
{
    std::vector<Blocking_sample> samples;
    samples.reserve(16);
    for (std::size_t i = 0; i < 16; ++i) {
        Blocking_sample sample;
        sample.t     = static_cast<double>(i);
        sample.v     = generation;
        sample.probe = probe;
        samples.push_back(std::move(sample));
    }
    return samples;
}

bool test_vector_source_set_data_bumps_sequence()
{
    plot::Vector_data_source<sample_t> source;
    TEST_ASSERT(source.current_sequence(0) == 0, "initial sequence should be 0");

    source.set_data({{0.0, 0.0f}, {1.0, 1.0f}});
    TEST_ASSERT(source.current_sequence(0) == 1, "set_data should bump sequence once");

    source.set_data({{0.0, 0.0f}, {1.0, 1.0f}, {2.0, 2.0f}});
    TEST_ASSERT(source.current_sequence(0) == 2,
        "a second set_data should bump sequence again");

    auto snap = source.try_snapshot(0);
    TEST_ASSERT(static_cast<bool>(snap), "snapshot should be READY when data is present");
    TEST_ASSERT(snap.snapshot.sequence == 2,
        "snapshot sequence should match current_sequence()");
    TEST_ASSERT(snap.snapshot.count == 3, "snapshot count should reflect stored samples");
    TEST_ASSERT(snap.snapshot.stride == sizeof(sample_t), "stride should be sizeof(sample_t)");
    TEST_ASSERT(snap.snapshot.hold, "snapshot should retain the immutable vector payload");
    return true;
}

bool test_vector_source_constructor_data_is_published()
{
    std::vector<sample_t> samples = {
        { 0.0, 4.0f },
        { 1.0, 5.0f },
    };
    plot::Vector_data_source<sample_t> source(std::move(samples));

    TEST_ASSERT(source.current_sequence(0) == 1,
        "constructor-provided data should publish a nonzero sequence");
    auto snap = source.try_snapshot(0);
    TEST_ASSERT(static_cast<bool>(snap),
        "constructor-provided data should produce a ready snapshot");
    TEST_ASSERT(snap.snapshot.sequence == source.current_sequence(0),
        "constructor snapshot sequence should match current_sequence");
    TEST_ASSERT(snap.snapshot.count == 2,
        "constructor snapshot should expose the provided samples");

    return true;
}

bool test_vector_source_empty_snapshot_reports_empty_status()
{
    plot::Vector_data_source<sample_t> source;
    auto snap = source.try_snapshot(0);
    TEST_ASSERT(snap.status == plot::snapshot_result_t::Snapshot_status::EMPTY,
        "empty source should yield EMPTY status");
    TEST_ASSERT(!static_cast<bool>(snap), "snapshot_result_t with EMPTY status should be falsey");

    source.set_data({});
    snap = source.try_snapshot(0);
    TEST_ASSERT(snap.status == plot::snapshot_result_t::Snapshot_status::EMPTY,
        "empty published data should yield EMPTY status");
    TEST_ASSERT(snap.snapshot.sequence == source.current_sequence(0),
        "empty snapshot should preserve the published sequence");
    return true;
}

bool test_vector_source_snapshot_hold_keeps_old_payload_after_set_data()
{
    plot::Vector_data_source<sample_t> source;
    source.set_data({{1.0, 10.0f}});

    auto old_snapshot = source.try_snapshot(0);
    TEST_ASSERT(static_cast<bool>(old_snapshot), "initial vector snapshot should be READY");
    TEST_ASSERT(old_snapshot.snapshot.hold,
        "vector snapshot should keep the published payload alive");
    TEST_ASSERT(old_snapshot.snapshot.sequence == 1,
        "initial publication should have sequence 1");

    source.set_data({{2.0, 20.0f}, {3.0, 30.0f}});

    const auto* old_sample = sample_at(old_snapshot.snapshot, 0);
    TEST_ASSERT(old_sample != nullptr, "old snapshot sample should remain readable");
    TEST_ASSERT(old_snapshot.snapshot.count == 1,
        "old snapshot should keep its original sample count");
    TEST_ASSERT(old_sample->t == 1.0 && old_sample->v == 10.0f,
        "old snapshot should still point at the old immutable payload");

    auto new_snapshot = source.try_snapshot(0);
    TEST_ASSERT(static_cast<bool>(new_snapshot), "new vector snapshot should be READY");
    TEST_ASSERT(new_snapshot.snapshot.sequence == 2,
        "new publication should advance the sequence");
    TEST_ASSERT(new_snapshot.snapshot.count == 2,
        "new snapshot should reflect the replacement payload");
    const auto* new_sample = sample_at(new_snapshot.snapshot, 0);
    TEST_ASSERT(new_sample != nullptr, "new snapshot sample should be readable");
    TEST_ASSERT(new_sample->t == 2.0 && new_sample->v == 20.0f,
        "new snapshot should point at the replacement payload");

    return true;
}

bool test_vector_source_snapshots_are_consistent_under_concurrent_set_data()
{
    plot::Vector_data_source<sample_t> source;
    source.set_data({{0.0, 0.0f}});

    std::atomic<bool> writer_done{false};
    std::atomic<bool> readers_started{false};
    std::atomic<std::uint64_t> ready_readers{0};
    std::atomic<std::uint64_t> reader_errors{0};
    std::atomic<std::uint64_t> observed_max_sequence{0};

    auto make_samples = [](std::uint64_t generation) {
        std::vector<sample_t> samples;
        samples.reserve(16);
        for (std::size_t i = 0; i < 16; ++i) {
            samples.push_back({static_cast<double>(i), static_cast<float>(generation)});
        }
        return samples;
    };

    auto reader_fn = [&] {
        std::uint64_t previous_sequence = 0;
        auto inspect_snapshot = [&] {
            auto result = source.try_snapshot(0);
            if (!result) {
                return;
            }

            const auto* first = sample_at(result.snapshot, 0);
            if (!first) {
                reader_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const float generation = first->v;
            for (std::size_t i = 0; i < result.snapshot.count; ++i) {
                const auto* current = sample_at(result.snapshot, i);
                if (!current                             ||
                    current->t != static_cast<double>(i) ||
                    current->v != generation)
                {
                    reader_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }

            if (result.snapshot.sequence < previous_sequence) {
                reader_errors.fetch_add(1, std::memory_order_relaxed);
            }
            previous_sequence = result.snapshot.sequence;

            std::uint64_t observed = observed_max_sequence.load(std::memory_order_relaxed);
            while (result.snapshot.sequence > observed
                && !observed_max_sequence.compare_exchange_weak(
                    observed,
                    result.snapshot.sequence,
                    std::memory_order_relaxed))
            {
            }
        };

        ready_readers.fetch_add(1, std::memory_order_acq_rel);
        while (!readers_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (!writer_done.load(std::memory_order_acquire)) {
            inspect_snapshot();
        }
        inspect_snapshot();
    };

    std::thread reader1(reader_fn);
    std::thread reader2(reader_fn);
    while (ready_readers.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    readers_started.store(true, std::memory_order_release);

    std::thread writer([&] {
        for (std::uint64_t generation = 1; generation <= 2000; ++generation) {
            source.set_data(make_samples(generation));
        }
        writer_done.store(true, std::memory_order_release);
    });

    writer.join();
    reader1.join();
    reader2.join();

    TEST_ASSERT(reader_errors.load() == 0,
        "readers should only observe whole immutable vector payloads");
    TEST_ASSERT(observed_max_sequence.load() > 1,
        "readers should observe at least one post-initial vector sequence");
    return true;
}

bool test_vector_source_snapshots_progress_while_set_data_is_active()
{
    plot::Vector_data_source<Blocking_sample> source;
    auto probe = std::make_shared<Blocking_publication_probe>();
    source.set_data(make_blocking_samples(1.0f, probe));

    std::atomic<bool> readers_started{false};
    std::atomic<bool> readers_stop{false};
    std::atomic<std::uint64_t> active_reads{0};
    std::atomic<std::uint64_t> reader_errors{0};
    constexpr std::uint64_t required_active_reads = 100;

    auto reader_fn = [&] {
        while (!readers_started.load(std::memory_order_acquire) &&
            !readers_stop.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        while (!readers_stop.load(std::memory_order_acquire)) {
            auto result = source.try_snapshot(0);
            if (!result || result.snapshot.sequence != 2 || result.snapshot.count != 16) {
                reader_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const auto* first = blocking_sample_at(result.snapshot, 0);
            if (!first) {
                reader_errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const float generation = first->v;
            for (std::size_t i = 0; i < result.snapshot.count; ++i) {
                const auto* current = blocking_sample_at(result.snapshot, i);
                if (!current                             ||
                    current->t != static_cast<double>(i) ||
                    current->v != generation)
                {
                    reader_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            active_reads.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread reader1(reader_fn);
    std::thread reader2(reader_fn);
    std::thread writer([&] {
        probe->block_next_destroy.store(true, std::memory_order_release);
        source.set_data(make_blocking_samples(2.0f));
    });

    const auto destroy_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!probe->destroy_entered.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < destroy_deadline)
    {
        std::this_thread::yield();
    }
    const bool destroy_entered = probe->destroy_entered.load(std::memory_order_acquire);

    if (destroy_entered) {
        readers_started.store(true, std::memory_order_release);
        const auto read_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (active_reads.load(std::memory_order_acquire) < required_active_reads &&
            std::chrono::steady_clock::now()                < read_deadline)
        {
            std::this_thread::yield();
        }
    }

    readers_stop.store(true, std::memory_order_release);
    probe->release_destroy.store(true, std::memory_order_release);
    reader1.join();
    reader2.join();
    writer.join();

    TEST_ASSERT(destroy_entered,
        "old payload destruction should be reached inside set_data");
    TEST_ASSERT(active_reads.load() >= required_active_reads,
        "readers should inspect snapshots while set_data has not returned");
    TEST_ASSERT(reader_errors.load() == 0,
        "active set_data readers should observe the replacement payload whole");
    return true;
}

} // namespace

int main()
{
    std::cout << "Concurrent series / Data_source tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_vector_source_set_data_bumps_sequence);
    RUN_TEST(test_vector_source_constructor_data_is_published);
    RUN_TEST(test_vector_source_empty_snapshot_reports_empty_status);
    RUN_TEST(test_vector_source_snapshot_hold_keeps_old_payload_after_set_data);
    RUN_TEST(test_vector_source_snapshots_are_consistent_under_concurrent_set_data);
    RUN_TEST(test_vector_source_snapshots_progress_while_set_data_is_active);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
