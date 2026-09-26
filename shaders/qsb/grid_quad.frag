#version 440
#extension GL_GOOGLE_include_directive : require

#include "lcd_filter.glsl"

const int GRID_LEVEL_MAX = 32;

// Per-level grid parameters packed into one vec4 (.x = spacing_px,
// .y = start_px, .z = alpha, .w = thickness_px). std140 stores arrays
// of scalars at vec4 stride; packing four scalars per element keeps the
// UBO compact.
layout(std140, binding = 0) uniform Block
{
    vec2  plot_size_px;
    vec2  region_origin_px;
    vec4  grid_color;
    int   v_count;
    int   t_count;
    int   framebuffer_y_up;
    float win_h;
    vec4  v_levels[GRID_LEVEL_MAX];
    vec4  t_levels[GRID_LEVEL_MAX];
    float lcd_subpixel_order;
    vec4  background_color;
} u;

layout(location = 0) out vec4 out_color;

float line_mask(float coord, float spacing, float start, float thickness_px)
{
    if (spacing <= 0.0) {
        return 0.0;
    }
    float wrapped = mod(coord - start, spacing);
    float dist    = min(wrapped, spacing - wrapped);
    float half_t  = thickness_px * 0.1;
    return 1.0 - smoothstep(half_t, half_t + 1.0, dist);
}

float accumulate_lines(int count, float coord, vec4 levels[GRID_LEVEL_MAX])
{
    float mask = 0.0;
    for (int i = 0; i < count && i < GRID_LEVEL_MAX; ++i) {
        float contrib = line_mask(coord, levels[i].x, levels[i].y, levels[i].w);
        mask = max(mask, contrib * levels[i].z);
    }
    return mask;
}

vec3 filtered_lcd_coverage(
    int count,
    float coord,
    vec4 levels[GRID_LEVEL_MAX],
    bool forward_order)
{
    float sample_0 = 0.0;
    float sample_1 = 0.0;
    float sample_2 = 0.0;
    float sample_3 = 0.0;
    float sample_4 = 0.0;
    float sample_5 = 0.0;
    float sample_6 = 0.0;

    for (int i = 0; i < count && i < GRID_LEVEL_MAX; ++i) {
        vec4 level = levels[i];
        float spacing = level.x;
        if (spacing <= 0.0) {
            continue;
        }

        float wrapped = mod(coord - level.y, spacing);
        float dist = min(wrapped, spacing - wrapped);
        if (dist >= level.w * 0.1 + 2.0) {
            continue;
        }

        sample_0 = max(sample_0, line_mask(coord - 1.0,       spacing, level.y, level.w) * level.z);
        sample_1 = max(sample_1, line_mask(coord - 2.0 / 3.0, spacing, level.y, level.w) * level.z);
        sample_2 = max(sample_2, line_mask(coord - 1.0 / 3.0, spacing, level.y, level.w) * level.z);
        sample_3 = max(sample_3, line_mask(coord,             spacing, level.y, level.w) * level.z);
        sample_4 = max(sample_4, line_mask(coord + 1.0 / 3.0, spacing, level.y, level.w) * level.z);
        sample_5 = max(sample_5, line_mask(coord + 2.0 / 3.0, spacing, level.y, level.w) * level.z);
        sample_6 = max(sample_6, line_mask(coord + 1.0,       spacing, level.y, level.w) * level.z);
    }

    return lcd_filter7(
        sample_0, sample_1, sample_2, sample_3,
        sample_4, sample_5, sample_6, forward_order);
}

void main()
{
    float frag_y = (u.framebuffer_y_up != 0)
        ? (u.win_h - gl_FragCoord.y)
        : gl_FragCoord.y;
    vec2 frag = vec2(gl_FragCoord.x, frag_y) - u.region_origin_px;

    if (frag.x < 0.0              || frag.y < 0.0 ||
        frag.x > u.plot_size_px.x || frag.y > u.plot_size_px.y)
    {
        discard;
    }

    float v_mask = accumulate_lines(u.v_count, frag.y, u.v_levels);
    bool lcd_rgb = u.lcd_subpixel_order > 0.5 && u.lcd_subpixel_order < 1.5;
    bool lcd_bgr = u.lcd_subpixel_order > 1.5 && u.lcd_subpixel_order < 2.5;
    bool lcd_enabled =
        (lcd_rgb || lcd_bgr) &&
        u.background_color.a >= 0.999;

    if (!lcd_enabled) {
        float t_mask = accumulate_lines(u.t_count, frag.x, u.t_levels);
        float mask = max(v_mask, t_mask);
        if (mask <= 0.001) {
            discard;
        }
        out_color = vec4(u.grid_color.rgb, u.grid_color.a * clamp(mask, 0.0, 1.0));
        return;
    }

    vec3 t_coverage = filtered_lcd_coverage(
        u.t_count,
        frag.x,
        u.t_levels,
        lcd_rgb);
    vec3 coverage = u.grid_color.a * clamp(
        max(vec3(v_mask), t_coverage),
        vec3(0.0),
        vec3(1.0));
    float alpha = max(coverage.r, max(coverage.g, coverage.b));
    if (alpha <= 0.001) {
        discard;
    }

    vec3 precomposed_rgb = mix(
        u.background_color.rgb,
        u.grid_color.rgb,
        coverage);
    vec3 straight_rgb =
        (precomposed_rgb -
            u.background_color.rgb * (1.0 - alpha)) /
        alpha;
    out_color = vec4(clamp(straight_rgb, 0.0, 1.0), alpha);
}
