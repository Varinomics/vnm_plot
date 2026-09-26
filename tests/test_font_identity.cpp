// vnm_plot font identity tests
//
// The atlas a Font_renderer produces is decided by the asset loader it was
// given. Loaders that register different font bytes under the same asset name
// must not share one atlas, and a forced rebuild must regenerate it.

#include "test_macros.h"

#include <vnm_plot/rhi/asset_loader.h>
#include <vnm_plot/rhi/font_renderer.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace plot = vnm::plot;

namespace {

constexpr int k_test_font_px = 18;

struct Scoped_font_disk_cache_setting
{
    bool previous = plot::font_disk_cache_enabled();

    explicit Scoped_font_disk_cache_setting(bool enabled)
    {
        plot::set_font_disk_cache_enabled(enabled);
    }

    ~Scoped_font_disk_cache_setting()
    {
        plot::set_font_disk_cache_enabled(previous);
    }

    Scoped_font_disk_cache_setting(const Scoped_font_disk_cache_setting&)            = delete;
    Scoped_font_disk_cache_setting& operator=(const Scoped_font_disk_cache_setting&) = delete;
};

bool test_distinct_loader_fonts_do_not_share_an_atlas()
{
    Scoped_font_disk_cache_setting cache_setting(false);

    plot::Asset_loader bundled_loader;
    plot::init_embedded_assets(bundled_loader);
    const auto bundled_font = bundled_loader.load("fonts/monospace.ttf");
    TEST_ASSERT(bundled_font && !bundled_font->empty(),
        "the bundled monospace font asset must be available");

    // A second loader supplying its own font bytes under the same asset name is
    // exactly what a consumer that wants its own typeface does; the bytes here
    // are the bundled font plus ignored trailing padding, so the only thing
    // that differs between the two loaders is the font identity.
    const std::string overridden_font = *bundled_font + std::string(64, '\0');
    plot::Asset_loader overriding_loader;
    plot::init_embedded_assets(overriding_loader);
    overriding_loader.register_embedded("fonts/monospace.ttf", overridden_font);

    plot::Font_renderer bundled_renderer(bundled_loader);
    bundled_renderer.initialize_metrics(k_test_font_px);
    const std::uint64_t bundled_key = bundled_renderer.text_measure_cache_key();
    TEST_ASSERT(bundled_key != 0, "the bundled loader must produce an atlas");

    plot::Font_renderer overriding_renderer(overriding_loader);
    overriding_renderer.initialize_metrics(k_test_font_px);
    const std::uint64_t overriding_key = overriding_renderer.text_measure_cache_key();
    TEST_ASSERT(overriding_key != 0, "the overriding loader must produce an atlas");

    TEST_ASSERT(bundled_key != overriding_key,
        "a loader that supplies its own font must not be served the other loader's atlas");

    plot::Font_renderer second_bundled_renderer(bundled_loader);
    second_bundled_renderer.initialize_metrics(k_test_font_px);
    TEST_ASSERT(second_bundled_renderer.text_measure_cache_key() == bundled_key,
        "a loader with unchanged font bytes must keep reusing its own atlas");

    return true;
}

bool test_forced_rebuild_regenerates_the_atlas()
{
    Scoped_font_disk_cache_setting cache_setting(false);

    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);

    plot::Font_renderer renderer(loader);
    renderer.initialize_metrics(k_test_font_px);
    const std::uint64_t initial_key = renderer.text_measure_cache_key();
    TEST_ASSERT(initial_key != 0, "the first initialization must produce an atlas");

    renderer.initialize_metrics(k_test_font_px);
    TEST_ASSERT(renderer.text_measure_cache_key() == initial_key,
        "an unforced initialization at the same height must reuse the atlas");

    const float advance_before = renderer.measure_text_px("0123456789");
    renderer.initialize_metrics(k_test_font_px, true);
    const std::uint64_t rebuilt_key = renderer.text_measure_cache_key();

    TEST_ASSERT(rebuilt_key != 0, "the forced rebuild must produce an atlas");
    TEST_ASSERT(rebuilt_key != initial_key,
        "force_rebuild must regenerate the atlas instead of returning the cached one");
    TEST_ASSERT(renderer.measure_text_px("0123456789") == advance_before,
        "regenerating the same font at the same height must not change its metrics");

    return true;
}

bool test_concurrent_renderers_share_one_atlas_build()
{
    Scoped_font_disk_cache_setting cache_setting(false);

    // register_embedded borrows its bytes, including while both threads bake.
    std::string concurrent_font;
    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);

    // A font identity no earlier case in this process has built, so both threads
    // miss the memo and the disk cache is off, which keeps the bake long enough for
    // the second thread to arrive while the first is still inside it. This is
    // the shape Logonomic and Phylax produce: two QQuickRhiItems, and therefore
    // two Qt scene-graph render threads, initializing font metrics at once.
    constexpr int k_concurrent_font_px = k_test_font_px + 1;
    const auto bundled_font = loader.load("fonts/monospace.ttf");
    TEST_ASSERT(bundled_font && !bundled_font->empty(), "the bundled font must be available");
    concurrent_font = *bundled_font + std::string(128, '\0');
    loader.register_embedded("fonts/monospace.ttf", concurrent_font);

    plot::Font_renderer   first_renderer(loader);
    plot::Font_renderer   second_renderer(loader);
    std::atomic<int>      ready{0};

    const auto initialize = [&](plot::Font_renderer& renderer) {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        renderer.initialize_metrics(k_concurrent_font_px);
    };

    std::thread first([&] { initialize(first_renderer); });
    std::thread second([&] { initialize(second_renderer); });
    first.join();
    second.join();

    const std::uint64_t first_key  = first_renderer.text_measure_cache_key();
    const std::uint64_t second_key = second_renderer.text_measure_cache_key();

    TEST_ASSERT(first_key != 0 && second_key != 0,
        "both concurrent renderers must end up with an atlas");
    // The key includes the atlas' cache epoch, stamped once per production,
    // so two distinct keys mean the two threads each ran their own multi-second
    // bake of the same font at the same height.
    TEST_ASSERT(first_key == second_key,
        "concurrent renderers at one height must share a single atlas build");

    return true;
}

} // namespace

int main()
{
    std::cout << "Font identity tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_distinct_loader_fonts_do_not_share_an_atlas);
    RUN_TEST(test_forced_rebuild_regenerates_the_atlas);
    RUN_TEST(test_concurrent_renderers_share_one_atlas_build);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
