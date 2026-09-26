// vnm_plot MSDF font atlas cache tests

#include "test_macros.h"

#include "../src/core/font_atlas_cache.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace detail = vnm::plot::detail;

namespace {

constexpr std::size_t k_atlas_bytes = 4096;

detail::font_atlas_key_t make_key(std::uint8_t digest_fill, int pixel_height)
{
    detail::font_atlas_key_t key;
    key.font_digest.fill(digest_fill);
    key.baked_pixel_height = pixel_height;
    return key;
}

std::shared_ptr<detail::cached_font_data_t> make_font(const detail::font_atlas_key_t& key)
{
    auto font = std::make_shared<detail::cached_font_data_t>();
    font->atlas.baked_pixel_height = key.baked_pixel_height;
    font->font_digest       = key.font_digest;
    font->atlas.rgba.resize(k_atlas_bytes);
    return font;
}

bool test_repeated_requests_reuse_one_production()
{
    detail::Font_atlas_cache cache(4u * k_atlas_bytes);
    const auto key = make_key(0x11u, 18);

    int productions = 0;
    const auto produce = [&] {
        ++productions;
        return make_font(key);
    };

    const auto first  = cache.get_or_build(key, false, produce);
    const auto second = cache.get_or_build(key, false, produce);

    TEST_ASSERT(first && second, "the cache should return the produced atlas");
    TEST_ASSERT(first == second, "a repeated request should return the memoized atlas");
    TEST_ASSERT(productions == 1, "a memoized key must not be produced twice");
    return true;
}

bool test_distinct_fonts_at_one_height_get_distinct_atlases()
{
    detail::Font_atlas_cache cache(4u * k_atlas_bytes);
    const auto first_font  = make_key(0x21u, 18);
    const auto second_font = make_key(0x22u, 18);

    const auto first  = cache.get_or_build(first_font,  false, [&] { return make_font(first_font); });
    const auto second = cache.get_or_build(second_font, false, [&] { return make_font(second_font); });

    TEST_ASSERT(first && second, "both fonts should produce an atlas");
    TEST_ASSERT(first != second,
        "two fonts at the same draw height must not share one atlas");
    TEST_ASSERT(first->font_digest == first_font.font_digest &&
        second->font_digest == second_font.font_digest,
                "each atlas must keep the identity of the font it was built from");
    return true;
}

bool test_concurrent_requests_share_one_production()
{
    detail::Font_atlas_cache cache(4u * k_atlas_bytes);
    const auto key = make_key(0x31u, 18);

    std::atomic<int> productions{0};
    const auto produce = [&] {
        productions.fetch_add(1, std::memory_order_relaxed);
        // Hold the single flight open long enough for the other threads to
        // reach the cache while this production is still running.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return make_font(key);
    };

    constexpr std::size_t k_thread_count = 8;
    std::vector<std::shared_ptr<detail::cached_font_data_t>> results(k_thread_count);
    std::vector<std::thread>                                 threads;
    threads.reserve(k_thread_count);
    for (std::size_t i = 0; i < k_thread_count; ++i) {
        threads.emplace_back([&, i] {
            results[i] = cache.get_or_build(key, false, produce);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    TEST_ASSERT(productions.load(std::memory_order_relaxed) == 1,
        "concurrent requests for one key must run a single production");
    for (const auto& result : results) {
        TEST_ASSERT(result == results.front(),
            "every concurrent caller must receive the same atlas");
    }
    return true;
}

bool test_forced_rebuild_replaces_the_memoized_atlas()
{
    detail::Font_atlas_cache cache(4u * k_atlas_bytes);
    const auto key = make_key(0x41u, 18);

    int productions = 0;
    const auto produce = [&] {
        ++productions;
        return make_font(key);
    };

    const auto first    = cache.get_or_build(key, false, produce);
    const auto rebuilt  = cache.get_or_build(key, true,  produce);
    const auto memoized = cache.get_or_build(key, false, produce);

    TEST_ASSERT(productions == 2, "a forced rebuild must run a fresh production");
    TEST_ASSERT(first != rebuilt, "a forced rebuild must not return the memoized atlas");
    TEST_ASSERT(memoized == rebuilt, "later requests must see the rebuilt atlas");
    return true;
}

bool test_eviction_drops_the_least_recently_used_atlas()
{
    detail::Font_atlas_cache cache(2u * k_atlas_bytes);
    const auto first  = make_key(0x51u, 18);
    const auto second = make_key(0x51u, 20);
    const auto third  = make_key(0x51u, 22);

    int productions = 0;
    const auto produce = [&](const detail::font_atlas_key_t& key) {
        return cache.get_or_build(key, false, [&] {
            ++productions;
            return make_font(key);
        });
    };

    (void)produce(first);
    (void)produce(second);
    (void)produce(first);
    TEST_ASSERT(productions == 2, "both keys should be memoized within the budget");

    (void)produce(third);
    TEST_ASSERT(productions == 3, "a third atlas exceeds the budget and is produced");

    (void)produce(first);
    TEST_ASSERT(productions == 3, "the recently used atlas must survive eviction");

    (void)produce(second);
    TEST_ASSERT(productions == 4, "the least recently used atlas must be the evicted one");
    return true;
}

bool test_failed_production_is_not_memoized()
{
    detail::Font_atlas_cache cache(4u * k_atlas_bytes);
    const auto key = make_key(0x61u, 18);

    const auto failed = cache.get_or_build(
        key,
        false,
        [] { return std::shared_ptr<detail::cached_font_data_t>(); });
    TEST_ASSERT(!failed, "a failed production must be reported to the caller");

    const auto retried = cache.get_or_build(key, false, [&] { return make_font(key); });
    TEST_ASSERT(retried, "a later request must be able to produce the atlas again");
    return true;
}

} // namespace

int main()
{
    std::cout << "Font atlas cache tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_repeated_requests_reuse_one_production);
    RUN_TEST(test_distinct_fonts_at_one_height_get_distinct_atlases);
    RUN_TEST(test_concurrent_requests_share_one_production);
    RUN_TEST(test_forced_rebuild_replaces_the_memoized_atlas);
    RUN_TEST(test_eviction_drops_the_least_recently_used_atlas);
    RUN_TEST(test_failed_production_is_not_memoized);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
