// vnm_plot Asset_loader tests

#include "test_macros.h"

#include <vnm_plot/rhi/asset_loader.h>

#include <iostream>
#include <string>
#include <vector>

namespace plot = vnm::plot;

namespace {

bool test_missing_asset_logs_and_returns_nullopt()
{
    plot::Asset_loader loader;
    std::vector<std::string> messages;
    loader.set_log_callback([&](const std::string& msg) { messages.push_back(msg); });

    auto result = loader.load("does/not/exist.vert");
    TEST_ASSERT(!result.has_value(), "missing asset should return nullopt");
    TEST_ASSERT(!messages.empty(),   "missing asset should surface a log message");
    TEST_ASSERT(messages.front().find("does/not/exist.vert") != std::string::npos,
        "log message should reference the missing asset name");
    return true;
}

bool test_embedded_asset_returns_registered_bytes()
{
    plot::Asset_loader loader;
    const std::string payload = "hello-shader-bytes";
    loader.register_embedded("example.vert", payload);

    auto result = loader.load("example.vert");
    TEST_ASSERT(result.has_value(), "registered embedded asset should be loadable");
    TEST_ASSERT(*result == payload, "embedded asset bytes should match what was registered");
    return true;
}

bool test_bundled_font_can_be_replaced()
{
    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);
#if defined(VNM_PLOT_ENABLE_TEXT)
    const auto bundled = loader.load("fonts/monospace.ttf");
    TEST_ASSERT(bundled && !bundled->empty(), "Qt resource must provide the bundled font");
#endif
    loader.register_embedded("fonts/monospace.ttf", "custom-font");
    const auto replacement = loader.load("fonts/monospace.ttf");
    TEST_ASSERT(replacement && *replacement == "custom-font",
        "hosts must still be able to replace the default font bytes");
    return true;
}

} // namespace

int main()
{
    std::cout << "Asset loader tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_missing_asset_logs_and_returns_nullopt);
    RUN_TEST(test_embedded_asset_returns_registered_bytes);
    RUN_TEST(test_bundled_font_can_be_replaced);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
