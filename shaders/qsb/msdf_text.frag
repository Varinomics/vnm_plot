#version 440
#extension GL_GOOGLE_include_directive : require

#include "lcd_filter.glsl"

layout(std140, binding = 0) uniform Block
{
    mat4  pmv;
    vec4  color;
    vec4  shadow_color;
    float px_range;
    float target_width;
    float target_height;
    float shadow_radius;
    float lcd_subpixel_order;
    int   framebuffer_y_up;
    float padding_0;
    float padding_1;
    vec4  background_color;
} u;

layout(binding = 1) uniform sampler2D msdf_tex;

layout(location = 0) smooth in vec4 vs_tex_bounds;
layout(location = 1) smooth in vec4 vs_frame_rect;

layout(location = 0) out vec4 out_color;

float median(vec3 v)
{
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

float smootherstep(float edge0, float edge1, float x)
{
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

vec4 sample_glyph(vec2 uv, vec2 uv_min, vec2 uv_max)
{
    return texture(msdf_tex, clamp(uv, uv_min, uv_max));
}

float glyph_alpha_from_texel(vec4 texel)
{
    float sd = (median(texel.rgb) - 0.5) * u.px_range;
    float alpha = clamp(sd + 0.5, 0.0, 1.0);

    // The alpha channel is the true SDF from the MTSDF atlas. Keep it as an
    // artifact mask for the crisp foreground glyph, but not as the primary
    // edge source because MSDF keeps sharper corners.
    if (texel.a > 0.0) {
        float sd_sdf = (texel.a - 0.5) * u.px_range;
        alpha = min(alpha, clamp(sd_sdf + 0.5, 0.0, 1.0));
    }

    return alpha;
}

float signed_distance_from_texel(vec4 texel)
{
    if (texel.a > 0.0) {
        return (texel.a - 0.5) * u.px_range;
    }

    return (median(texel.rgb) - 0.5) * u.px_range;
}

float shadow_alpha_from_texel(vec4 texel)
{
    float radius = max(u.shadow_radius, 0.0);
    float sd = signed_distance_from_texel(texel);
    return smootherstep(-radius, 0.5, sd);
}

vec4 glyph_texel_at_ratio(vec2 glyph_ratio, vec2 uv_min, vec2 uv_max)
{
    vec2 glyph_span = max(vs_tex_bounds.zw - vs_tex_bounds.xy, vec2(0.000001));
    vec2 glyph_uv   = vs_tex_bounds.xy + glyph_ratio * glyph_span;
    return sample_glyph(glyph_uv, uv_min, uv_max);
}

float glyph_alpha_at_ratio(vec2 glyph_ratio, vec2 uv_min, vec2 uv_max)
{
    return glyph_alpha_from_texel(glyph_texel_at_ratio(glyph_ratio, uv_min, uv_max));
}

vec3 filtered_lcd_coverage(
    vec2 glyph_ratio,
    vec2 subpixel_step,
    bool forward_order,
    vec2 uv_min,
    vec2 uv_max)
{
    float sample_0 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step * 3.0, uv_min, uv_max);
    float sample_1 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step * 2.0, uv_min, uv_max);
    float sample_2 = glyph_alpha_at_ratio(glyph_ratio - subpixel_step, uv_min, uv_max);
    float sample_3 = glyph_alpha_at_ratio(glyph_ratio, uv_min, uv_max);
    float sample_4 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step, uv_min, uv_max);
    float sample_5 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step * 2.0, uv_min, uv_max);
    float sample_6 = glyph_alpha_at_ratio(glyph_ratio + subpixel_step * 3.0, uv_min, uv_max);

    return lcd_filter7(
        sample_0, sample_1, sample_2, sample_3,
        sample_4, sample_5, sample_6, forward_order);
}

void main()
{
    // Clamp the lookup inside the glyph box so neighbouring atlas glyphs
    // cannot bleed across the half-texel edge under bilinear filtering.
    vec2 atlas_size   = vec2(textureSize(msdf_tex, 0));
    vec2 glyph_span   = max(vs_tex_bounds.zw - vs_tex_bounds.xy, vec2(0.0));
    vec2 half_texel   = vec2(0.5) / atlas_size;
    vec2 clamp_margin = min(half_texel, glyph_span * 0.499);
    vec2 uv_min       = vs_tex_bounds.xy + clamp_margin;
    vec2 uv_max       = vs_tex_bounds.zw - clamp_margin;

    vec2 frame_origin = vs_frame_rect.xy;
    vec2 frame_size = max(vec2(1.0), vs_frame_rect.zw);
    float frag_y = (u.framebuffer_y_up != 0)
        ? (u.target_height - gl_FragCoord.y)
        : gl_FragCoord.y;
    vec2 glyph_pixel = vec2(gl_FragCoord.x, frag_y) - frame_origin;
    vec2 glyph_ratio = vec2(
        glyph_pixel.x / frame_size.x,
        1.0 - glyph_pixel.y / frame_size.y);

    bool lcd_rgb = u.lcd_subpixel_order > 0.5 && u.lcd_subpixel_order < 1.5;
    bool lcd_bgr = u.lcd_subpixel_order > 1.5 && u.lcd_subpixel_order < 2.5;
    bool lcd_vrgb = u.lcd_subpixel_order > 2.5 && u.lcd_subpixel_order < 3.5;
    bool lcd_vbgr = u.lcd_subpixel_order > 3.5 && u.lcd_subpixel_order < 4.5;
    bool lcd_horizontal = lcd_rgb || lcd_bgr;
    bool lcd_vertical = lcd_vrgb || lcd_vbgr;
    bool lcd_enabled =
        (lcd_horizontal || lcd_vertical) &&
        u.shadow_radius <= 0.0 &&
        u.color.a >= 0.999 &&
        u.background_color.a >= 0.999;
    if (lcd_enabled) {
        vec2 subpixel_step = lcd_horizontal
            ? vec2(1.0 / (3.0 * frame_size.x), 0.0)
            : vec2(0.0, -1.0 / (3.0 * frame_size.y));
        bool forward_order = lcd_rgb || lcd_vrgb;
        vec3 lcd_coverage =
            filtered_lcd_coverage(glyph_ratio, subpixel_step, forward_order, uv_min, uv_max);
        float alpha = max(lcd_coverage.r, max(lcd_coverage.g, lcd_coverage.b));
        if (alpha <= 0.0) {
            out_color = vec4(u.color.rgb, 0.0);
            return;
        }

        vec3 precomposed_rgb = mix(
            u.background_color.rgb,
            u.color.rgb,
            lcd_coverage);
        vec3 straight_rgb =
            (precomposed_rgb -
                u.background_color.rgb * (1.0 - alpha)) /
            alpha;
        out_color = vec4(clamp(straight_rgb, 0.0, 1.0), alpha);
        return;
    }

    vec4 texel = glyph_texel_at_ratio(glyph_ratio, uv_min, uv_max);
    float glyph_alpha = glyph_alpha_from_texel(texel);
    float shadow_alpha = 0.0;
    if (u.shadow_radius > 0.0 && u.shadow_color.a > 0.0) {
        shadow_alpha = shadow_alpha_from_texel(texel);
    }

    float glyph_a = u.color.a * glyph_alpha;
    float shadow_a = u.shadow_color.a * shadow_alpha;
    float out_a = glyph_a + shadow_a * (1.0 - glyph_a);
    if (out_a <= 0.0) {
        out_color = vec4(0.0);
        return;
    }

    vec3 out_rgb =
        (u.color.rgb * glyph_a + u.shadow_color.rgb * shadow_a * (1.0 - glyph_a)) /
        out_a;
    out_color = vec4(out_rgb, out_a);
}
