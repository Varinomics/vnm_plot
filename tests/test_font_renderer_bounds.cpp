// vnm_plot Font_renderer visual bounds tests

#include "test_macros.h"

#include <vnm_plot/rhi/asset_loader.h>
#include <vnm_plot/rhi/font_renderer.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <string>

namespace plot = vnm::plot;

namespace {

constexpr int   k_test_font_px   = 18;
constexpr float k_bounds_epsilon = 1.0e-4f;

bool finite_ordered(const glm::vec4& bounds)
{
    return
        std::isfinite(bounds.x) &&
        std::isfinite(bounds.y) &&
        std::isfinite(bounds.z) &&
        std::isfinite(bounds.w) &&
        bounds.z > bounds.x     &&
        bounds.w > bounds.y;
}

bool nearly_equal(float lhs, float rhs)
{
    return std::abs(lhs - rhs) <= k_bounds_epsilon;
}

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
};

bool test_bounds_fail_closed_without_atlas()
{
    // Bound to a font source but never initialized, which is the state the
    // measurement accessors have to fail closed in.
    plot::Asset_loader  loader;
    plot::Font_renderer renderer(loader);
    glm::vec4           bounds(1.0f);

    TEST_ASSERT(!renderer.text_visual_bounds_px(nullptr, 0.0f, 0.0f, bounds),
        "null text must fail closed before font initialization");
    TEST_ASSERT(!renderer.text_visual_bounds_px("Axis", 0.0f, 0.0f, bounds),
        "visible text must fail closed before font initialization");
    TEST_ASSERT(!renderer.text_ink_bounds_px(nullptr, 0.0f, 0.0f, bounds),
        "null text must fail closed for ink bounds before font initialization");
    TEST_ASSERT(!renderer.text_ink_bounds_px("Axis", 0.0f, 0.0f, bounds),
        "visible text must fail closed for ink bounds before font initialization");
    return true;
}

bool test_bounds_fail_closed_for_no_visible_glyphs()
{
    Scoped_font_disk_cache_setting cache_setting(false);

    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);

    plot::Font_renderer renderer(loader);
    renderer.initialize_metrics(k_test_font_px);

    glm::vec4 bounds(1.0f);
    TEST_ASSERT(!renderer.text_visual_bounds_px(nullptr, 10.0f, 20.0f, bounds),
        "null text must fail closed after font initialization");
    TEST_ASSERT(!renderer.text_visual_bounds_px("", 10.0f, 20.0f, bounds),
        "empty text must fail closed as no visible glyphs");
    TEST_ASSERT(!renderer.text_visual_bounds_px("   ", 10.0f, 20.0f, bounds),
        "space-only text must fail closed as no visible glyphs");
    TEST_ASSERT(!renderer.text_ink_bounds_px(nullptr, 10.0f, 20.0f, bounds),
        "null text must fail closed for ink bounds after font initialization");
    TEST_ASSERT(!renderer.text_ink_bounds_px("", 10.0f, 20.0f, bounds),
        "empty text must fail closed for ink bounds as no visible glyphs");
    TEST_ASSERT(!renderer.text_ink_bounds_px("   ", 10.0f, 20.0f, bounds),
        "space-only text must fail closed for ink bounds as no visible glyphs");
    return true;
}

bool test_visible_bounds_are_finite_ordered_and_translation_invariant()
{
    Scoped_font_disk_cache_setting cache_setting(false);

    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);

    plot::Font_renderer renderer(loader);
    renderer.initialize_metrics(k_test_font_px);

    constexpr const char* k_text = "Axis 123";
    constexpr float       x      = 12.25f;
    constexpr float       y      = 34.5f;
    constexpr float       dx     = 7.75f;
    constexpr float       dy     = -3.5f;

    glm::vec4 first;
    glm::vec4 translated;
    TEST_ASSERT(renderer.text_visual_bounds_px(k_text, x, y, first),
        "visible text must produce visual bounds after font initialization");
    TEST_ASSERT(finite_ordered(first),
        "visible text bounds must be finite and ordered");

    TEST_ASSERT(renderer.text_visual_bounds_px(k_text, x + dx, y + dy, translated),
        "translated visible text must produce visual bounds");
    TEST_ASSERT(finite_ordered(translated),
        "translated visible text bounds must be finite and ordered");

    TEST_ASSERT(nearly_equal(translated.x - first.x, dx),
        "translated left bound must shift by dx");
    TEST_ASSERT(nearly_equal(translated.z - first.z, dx),
        "translated right bound must shift by dx");
    TEST_ASSERT(nearly_equal(translated.y - first.y, dy),
        "translated top bound must shift by dy");
    TEST_ASSERT(nearly_equal(translated.w - first.w, dy),
        "translated bottom bound must shift by dy");
    TEST_ASSERT(nearly_equal(translated.z - translated.x, first.z - first.x),
        "translated width must remain unchanged");
    TEST_ASSERT(nearly_equal(translated.w - translated.y, first.w - first.y),
        "translated height must remain unchanged");

    const float first_width  = first.z - first.x;
    const float first_height = first.w - first.y;
    const float advance      = renderer.measure_text_px(k_text);
    TEST_ASSERT(first_width > advance * 0.45f,
        "visible text bounds width must be plausible relative to text advance");
    TEST_ASSERT(first_width < advance + static_cast<float>(k_test_font_px),
        "visible text bounds width must not greatly exceed text advance");
    TEST_ASSERT(first_height > static_cast<float>(k_test_font_px) * 0.25f,
        "visible text bounds height must be plausible relative to font size");
    TEST_ASSERT(first_height < static_cast<float>(k_test_font_px) * 1.5f,
        "visible text bounds height must not greatly exceed font size");

    constexpr const char* k_single_glyph   = "0";
    constexpr const char* k_repeated_glyph = "0000";
    glm::vec4 single_bounds;
    glm::vec4 repeated_bounds;
    TEST_ASSERT(renderer.text_visual_bounds_px(k_single_glyph, x, y, single_bounds),
        "single visible glyph must produce visual bounds");
    TEST_ASSERT(renderer.text_visual_bounds_px(k_repeated_glyph, x, y, repeated_bounds),
        "repeated visible glyphs must produce visual bounds");

    const float single_width     = single_bounds.z - single_bounds.x;
    const float repeated_width   = repeated_bounds.z - repeated_bounds.x;
    const float single_advance   = renderer.measure_text_px(k_single_glyph);
    const float repeated_advance = renderer.measure_text_px(k_repeated_glyph);
    TEST_ASSERT(repeated_advance > single_advance * 3.5f,
        "repeated glyph advance must grow monotonically");
    TEST_ASSERT(repeated_width > single_width * 2.5f,
        "repeated glyph bounds must be wider than one glyph");
    TEST_ASSERT(repeated_width < repeated_advance + static_cast<float>(k_test_font_px),
        "repeated glyph bounds width must stay plausible relative to advance");
    return true;
}

bool test_ink_bounds_strip_the_padding_the_quad_carries()
{
    Scoped_font_disk_cache_setting cache_setting(false);

    plot::Asset_loader loader;
    plot::init_embedded_assets(loader);

    plot::Font_renderer renderer(loader);
    renderer.initialize_metrics(k_test_font_px);

    constexpr float x = 12.25f;
    constexpr float y = 34.5f;

    // The padding is the distance field's, so it is the same all round every
    // glyph, and the text bound takes it from whichever glyph reaches each
    // side. Two strings sharing one atlas therefore lose the same margin.
    bool  margin_known = false;
    float first_margin = 0.0f;
    for (const char* text : {"Axis 123", "0", "0000", "|"}) {
        glm::vec4 quad;
        glm::vec4 ink;
        TEST_ASSERT(renderer.text_visual_bounds_px(text, x, y, quad),
            "visible text must produce visual bounds");
        TEST_ASSERT(renderer.text_ink_bounds_px(text, x, y, ink),
            "visible text must produce ink bounds");
        TEST_ASSERT(finite_ordered(ink),
            "ink bounds must be finite and ordered");
        TEST_ASSERT(ink.x > quad.x && ink.y > quad.y && ink.z < quad.z && ink.w < quad.w,
            "ink bounds must sit strictly inside the visual quad on every side");

        const float margin = ink.x - quad.x;
        TEST_ASSERT(nearly_equal(ink.y - quad.y, margin) &&
                    nearly_equal(quad.z - ink.z, margin) &&
                    nearly_equal(quad.w - ink.w, margin),
            "the quad must exceed the ink by one margin on all four sides");
        if (!margin_known) {
            margin_known = true;
            first_margin = margin;
        }
        TEST_ASSERT(nearly_equal(margin, first_margin),
            "every string of one atlas must lose the same margin");

        const float ink_height = ink.w - ink.y;
        TEST_ASSERT(ink_height > static_cast<float>(k_test_font_px) * 0.25f,
            "ink bounds must stay a plausible height for the font size");
        TEST_ASSERT(ink_height < static_cast<float>(k_test_font_px) * 1.5f,
            "ink bounds must not greatly exceed the font size");
    }

    // Widening the text widens its ink, so the margin is not swallowing it.
    glm::vec4 single;
    glm::vec4 repeated;
    TEST_ASSERT(renderer.text_ink_bounds_px("0", x, y, single),
        "single visible glyph must produce ink bounds");
    TEST_ASSERT(renderer.text_ink_bounds_px("0000", x, y, repeated),
        "repeated visible glyphs must produce ink bounds");
    TEST_ASSERT((repeated.z - repeated.x) > (single.z - single.x) * 2.5f,
        "repeated glyph ink must be wider than one glyph's");

    constexpr float dx = 7.75f;
    constexpr float dy = -3.5f;
    glm::vec4 translated;
    TEST_ASSERT(renderer.text_ink_bounds_px("0", x + dx, y + dy, translated),
        "translated visible text must produce ink bounds");
    TEST_ASSERT(nearly_equal(translated.x - single.x, dx) &&
                nearly_equal(translated.z - single.z, dx) &&
                nearly_equal(translated.y - single.y, dy) &&
                nearly_equal(translated.w - single.w, dy),
        "ink bounds must shift with the baseline they are measured at");
    return true;
}

} // namespace

int main()
{
    std::cout << "Font renderer bounds tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_bounds_fail_closed_without_atlas);
    RUN_TEST(test_bounds_fail_closed_for_no_visible_glyphs);
    RUN_TEST(test_visible_bounds_are_finite_ordered_and_translation_invariant);
    RUN_TEST(test_ink_bounds_strip_the_padding_the_quad_carries);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
