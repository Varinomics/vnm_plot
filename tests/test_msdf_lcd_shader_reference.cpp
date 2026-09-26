// vnm_plot MSDF LCD shader-reference drift checks

#include "test_macros.h"
#include "../src/core/lcd_policy.h"

#include <vnm_msdf_text/lcd_contract.h>
#include <vnm_msdf_text/lcd_shader_reference.h>

#include <cctype>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lcd = vnm::msdf_text::lcd;
namespace plot = vnm::plot;
namespace ref = vnm::msdf_text::lcd::shader_reference;

namespace {

std::string read_text_file(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

using glsl_token_list_t = std::vector<std::string>;

bool is_glsl_identifier_start(unsigned char ch)
{
    return std::isalpha(ch) || ch == '_';
}

bool is_glsl_identifier_char(unsigned char ch)
{
    return std::isalnum(ch) || ch == '_';
}

bool is_glsl_two_char_operator(std::string_view text)
{
    return
        text == "&&" ||
        text == "||" ||
        text == "<=" ||
        text == ">=" ||
        text == "==" ||
        text == "!=" ||
        text == "++" ||
        text == "--" ||
        text == "+=" ||
        text == "-=" ||
        text == "*=" ||
        text == "/=";
}

glsl_token_list_t tokenize_glsl(std::string_view text)
{
    glsl_token_list_t tokens;

    for (std::size_t pos = 0; pos < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[pos]);
        if (std::isspace(ch)) {
            ++pos;
            continue;
        }

        if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            pos += 2;
            while (pos < text.size() && text[pos] != '\n') {
                ++pos;
            }
            continue;
        }

        if (text[pos] == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
            pos += 2;
            while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/')) {
                ++pos;
            }
            pos = pos + 1 < text.size()
                ? pos + 2
                : text.size();
            continue;
        }

        if (is_glsl_identifier_start(ch)) {
            const std::size_t start = pos++;
            while (pos < text.size() &&
                is_glsl_identifier_char(static_cast<unsigned char>(text[pos])))
            {
                ++pos;
            }
            tokens.emplace_back(std::string(text.substr(start, pos - start)));
            continue;
        }

        if (std::isdigit(ch) ||
            (text[pos] == '.' && pos + 1 < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[pos + 1]))))
        {
            const std::size_t start = pos++;
            while (pos < text.size()) {
                const unsigned char next = static_cast<unsigned char>(text[pos]);
                if (std::isdigit(next) || text[pos] == '.') {
                    ++pos;
                    continue;
                }
                if (text[pos] == 'e' || text[pos] == 'E') {
                    ++pos;
                    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
                        ++pos;
                    }
                    continue;
                }
                break;
            }
            tokens.emplace_back(std::string(text.substr(start, pos - start)));
            continue;
        }

        if (pos + 1 < text.size() && is_glsl_two_char_operator(text.substr(pos, 2))) {
            tokens.emplace_back(std::string(text.substr(pos, 2)));
            pos += 2;
            continue;
        }

        tokens.emplace_back(1, static_cast<char>(ch));
        ++pos;
    }

    return tokens;
}

bool contains_token_sequence(
    const glsl_token_list_t&   tokens,
    const glsl_token_list_t&   expected)
{
    if (expected.empty() || tokens.size() < expected.size()) {
        return false;
    }

    for (std::size_t start = 0; start + expected.size() <= tokens.size(); ++start) {
        bool matches = true;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (tokens[start + i] != expected[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }

    return false;
}

bool contains_glsl_token_sequence(
    const glsl_token_list_t&   shader_tokens,
    std::string_view           expected_statement)
{
    return contains_token_sequence(shader_tokens, tokenize_glsl(expected_statement));
}

std::string sample_name_for_offset(float offset)
{
    const int sample_index = static_cast<int>(offset + 3.0f);
    return "sample_" + std::to_string(sample_index);
}

std::string weight_name_for_value(float weight)
{
    if (weight == ref::k_lcd_filter_edge)   { return "filter_edge";   }
    if (weight == ref::k_lcd_filter_side)   { return "filter_side";   }
    if (weight == ref::k_lcd_filter_center) { return "filter_center"; }
    return {};
}

std::string sample_expression_for_offset(float offset)
{
    if (offset == -3.0f) { return "glyph_ratio - subpixel_step * 3.0"; }
    if (offset == -2.0f) { return "glyph_ratio - subpixel_step * 2.0"; }
    if (offset == -1.0f) { return "glyph_ratio - subpixel_step";       }
    if (offset == 0.0f)  { return "glyph_ratio";                       }
    if (offset == 1.0f)  { return "glyph_ratio + subpixel_step";       }
    if (offset == 2.0f)  { return "glyph_ratio + subpixel_step * 2.0"; }
    if (offset == 3.0f)  { return "glyph_ratio + subpixel_step * 3.0"; }
    return {};
}

std::string lcd_order_bool_name(lcd::Resolved_lcd_subpixel_order order)
{
    switch (order) {
        case lcd::Resolved_lcd_subpixel_order::RGB:  return "lcd_rgb";
        case lcd::Resolved_lcd_subpixel_order::BGR:  return "lcd_bgr";
        case lcd::Resolved_lcd_subpixel_order::VRGB: return "lcd_vrgb";
        case lcd::Resolved_lcd_subpixel_order::VBGR: return "lcd_vbgr";
        case lcd::Resolved_lcd_subpixel_order::NONE: break;
    }

    return {};
}

std::string decode_threshold_statement(const ref::decode_threshold_t& threshold)
{
    const std::string condition(threshold.glsl_condition);
    const std::string separator     = " && ";
    const std::size_t separator_pos = condition.find(separator);
    if (separator_pos == std::string::npos) {
        return {};
    }

    const std::string bool_name = lcd_order_bool_name(threshold.order);
    if (bool_name.empty()) {
        return {};
    }

    return
        "bool "                                            +
        bool_name                                          +
        " = u.lcd_subpixel_order "                         +
        condition.substr(0, separator_pos)                 +
        " && u.lcd_subpixel_order "                        +
        condition.substr(separator_pos + separator.size()) +
        ";";
}

std::string sample_statement_for_offset(float offset)
{
    const std::string sample_name = sample_name_for_offset(offset);
    const std::string expression  = sample_expression_for_offset(offset);
    if (expression.empty()) {
        return {};
    }

    return
        "float "                   +
        sample_name                +
        " = glyph_alpha_at_ratio(" +
        expression                 +
        ", uv_min, uv_max);";
}

std::string filter_weight_statement(std::string_view name, std::string_view literal)
{
    return "float " + std::string(name) + " = " + std::string(literal) + ";";
}

std::string filter_window_statement(const ref::filter_window_t& window)
{
    std::string statement =
        "float " + std::string(window.channel_name) + "_coverage =\n";

    for (std::size_t i = 0; i < window.taps.size(); ++i) {
        const ref::filter_tap_t& tap         = window.taps[i];
        const std::string        sample_name = sample_name_for_offset(tap.offset);
        const std::string        weight_name = weight_name_for_value(tap.weight);
        if (weight_name.empty()) {
            return {};
        }

        statement += "        " + sample_name + " * " + weight_name;
        statement += (i + 1 == window.taps.size()) ? ";\n" : " +\n";
    }

    return statement;
}

std::string lcd_horizontal_group_statement()
{
    return "bool lcd_horizontal = lcd_rgb || lcd_bgr;";
}

std::string lcd_vertical_group_statement()
{
    return "bool lcd_vertical = lcd_vrgb || lcd_vbgr;";
}

std::string subpixel_step_statement()
{
    return
        "vec2 subpixel_step = lcd_horizontal\n" "            ? vec2(" +
        std::string(ref::k_lcd_horizontal_step_glsl)                  +
        ", 0.0)\n" "            : vec2(0.0, "                         +
        std::string(ref::k_lcd_vertical_step_glsl)                    +
        ");";
}

std::string lcd_enabled_statement()
{
    return
        std::string("bool lcd_enabled =\n")              +
        "        (lcd_horizontal || lcd_vertical) &&\n"  +
        "        u.shadow_radius <= 0.0 &&\n"            +
        "        u.color.a >= "                          +
        std::string(ref::k_lcd_opaque_alpha_cutoff_glsl) +
        " &&\n"                                          +
        "        u.background_color.a >= "               +
        std::string(ref::k_lcd_opaque_alpha_cutoff_glsl) +
        ";";
}

std::string forward_order_statement()
{
    return "bool forward_order = lcd_rgb || lcd_vrgb;";
}

std::string filtered_lcd_return_statement()
{
    return
        "return forward_order\n"
        "        ? vec3(first_coverage, center_coverage, last_coverage)\n"
        "        : vec3(last_coverage, center_coverage, first_coverage);";
}

bool test_cpp_order_values_match_shader_reference()
{
    struct case_t
    {
        lcd::Resolved_lcd_subpixel_order order;
        int                              value;
        float                            uniform;
    };

    constexpr case_t cases[] = {
        { lcd::Resolved_lcd_subpixel_order::NONE, ref::k_lcd_order_none_value, ref::k_lcd_order_none_uniform },
        { lcd::Resolved_lcd_subpixel_order::RGB,  ref::k_lcd_order_rgb_value,  ref::k_lcd_order_rgb_uniform  },
        { lcd::Resolved_lcd_subpixel_order::BGR,  ref::k_lcd_order_bgr_value,  ref::k_lcd_order_bgr_uniform  },
        { lcd::Resolved_lcd_subpixel_order::VRGB, ref::k_lcd_order_vrgb_value, ref::k_lcd_order_vrgb_uniform },
        { lcd::Resolved_lcd_subpixel_order::VBGR, ref::k_lcd_order_vbgr_value, ref::k_lcd_order_vbgr_uniform },
    };

    for (const case_t& item : cases) {
        TEST_ASSERT(lcd::resolved_order_value(item.order) == item.value,
            "resolved LCD order value must match shader reference");
        TEST_ASSERT(lcd::shader_uniform_value(item.order) == item.uniform,
            "resolved LCD shader uniform must match shader reference");
    }

    return true;
}

bool test_plot_shader_binds_lcd_reference_literals()
{
    const std::string shader = read_text_file(VNM_PLOT_MSDF_TEXT_FRAG_PATH);
    TEST_ASSERT(!shader.empty(), "plot MSDF fragment shader source must be readable");
    const glsl_token_list_t shader_tokens = tokenize_glsl(shader);
    const std::string filter = read_text_file(VNM_PLOT_LCD_FILTER_PATH);
    TEST_ASSERT(!filter.empty(), "shared LCD filter source must be readable");
    const glsl_token_list_t filter_tokens = tokenize_glsl(filter);
    const std::string grid = read_text_file(VNM_PLOT_GRID_FRAG_PATH);
    for (const std::string* source : {&shader, &grid}) {
        TEST_ASSERT(source->find("#include \"lcd_filter.glsl\"") != std::string::npos,
            "text and grid shaders must include the shared LCD filter");
        TEST_ASSERT(contains_glsl_token_sequence(tokenize_glsl(*source),
            "return lcd_filter7(sample_0, sample_1, sample_2, sample_3,"
            " sample_4, sample_5, sample_6, forward_order);"),
            "text and grid must send their seven samples to the shared filter");
    }

    for (const ref::decode_threshold_t& threshold : ref::k_lcd_decode_thresholds) {
        const std::string statement = decode_threshold_statement(threshold);
        TEST_ASSERT(!statement.empty(),
            "shared LCD decode threshold row must name a plot shader boolean");
        TEST_ASSERT(contains_glsl_token_sequence(shader_tokens, statement),
            "plot shader LCD decode threshold must bind the correct boolean name");
        TEST_ASSERT(lcd::shader_uniform_value(threshold.order) > threshold.min_exclusive,
            "decode threshold lower bound must contain its uniform value");
        TEST_ASSERT(lcd::shader_uniform_value(threshold.order) < threshold.max_exclusive,
            "decode threshold upper bound must contain its uniform value");
    }

    TEST_ASSERT(contains_glsl_token_sequence(
        filter_tokens, filter_weight_statement("filter_edge", ref::k_lcd_filter_edge_glsl)),
        "plot shader LCD edge filter literal must match shared reference");
    TEST_ASSERT(contains_glsl_token_sequence(
        filter_tokens, filter_weight_statement("filter_side", ref::k_lcd_filter_side_glsl)),
        "plot shader LCD side filter literal must match shared reference");
    TEST_ASSERT(contains_glsl_token_sequence(
        filter_tokens, filter_weight_statement("filter_center", ref::k_lcd_filter_center_glsl)),
        "plot shader LCD center filter literal must match shared reference");

    for (const ref::filter_window_t& window : ref::k_lcd_filter_windows) {
        const std::string statement = filter_window_statement(window);
        TEST_ASSERT(!statement.empty(),
            "shared LCD filter window taps must have plot shader symbols");
        TEST_ASSERT(contains_glsl_token_sequence(filter_tokens, statement),
            "plot shader LCD filter window statement must match shared reference");
    }

    for (float offset : ref::k_lcd_tap_offsets) {
        const std::string statement = sample_statement_for_offset(offset);
        TEST_ASSERT(!statement.empty(),
            "shared LCD sample tap offset must have a plot shader expression");
        TEST_ASSERT(contains_glsl_token_sequence(shader_tokens, statement),
            "plot shader sample tap statement must match shared reference");
    }

    TEST_ASSERT(contains_glsl_token_sequence(shader_tokens, subpixel_step_statement()),
        "plot shader LCD subpixel-step expression must match shared reference");
    TEST_ASSERT(contains_glsl_token_sequence(shader_tokens, lcd_horizontal_group_statement()),
        "plot shader horizontal LCD group must include RGB and BGR");
    TEST_ASSERT(contains_glsl_token_sequence(shader_tokens, lcd_vertical_group_statement()),
        "plot shader vertical LCD group must include VRGB and VBGR");
    TEST_ASSERT(contains_glsl_token_sequence(shader_tokens, forward_order_statement()),
        "plot shader LCD forward order must be RGB and VRGB");
    TEST_ASSERT(contains_glsl_token_sequence(filter_tokens, filtered_lcd_return_statement()),
        "plot shader LCD filtered coverage return order must preserve channel direction");
    TEST_ASSERT(contains_glsl_token_sequence(shader_tokens, lcd_enabled_statement()),
        "plot shader opacity cutoff expression must match shared reference");
    TEST_ASSERT(plot::detail::k_lcd_opaque_alpha_cutoff ==
        ref::k_lcd_opaque_alpha_cutoff,
        "plot CPU opacity cutoff must match shared shader reference");

    return true;
}

} // namespace

int main()
{
    std::cout << "MSDF LCD shader reference drift tests" << std::endl;

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_cpp_order_values_match_shader_reference);
    RUN_TEST(test_plot_shader_binds_lcd_reference_literals);

    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
