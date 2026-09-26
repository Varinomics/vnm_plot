#version 440
#extension GL_GOOGLE_include_directive : require

#include "uniform_blocks.glsl"

layout(std140, binding = 0) uniform Block
{
    Series_view_t view;
    float line_px;
    int   snap_to_pixels;
} u;

layout(location = 0) flat in vec4 fs_segment;
layout(location = 1) flat in vec4 fs_prev_segment;
layout(location = 2) flat in vec4 fs_next_segment;

layout(location = 0) out vec4 frag_color;

float dist_to_segment(vec2 p, vec2 a, vec2 b)
{
    vec2  ab   = b - a;
    float len2 = dot(ab, ab);
    if (len2 <= 1e-12) {
        return length(p - a);
    }
    float t = clamp(dot(p - a, ab) / len2, 0.0, 1.0);
    return length(p - (a + t * ab));
}

void main()
{
    float frag_y = (u.view.framebuffer_y_up != 0)
        ? (u.view.win_h - gl_FragCoord.y)
        : gl_FragCoord.y;
    vec2 frag = vec2(gl_FragCoord.x, frag_y);

    float d0     = dist_to_segment(frag, fs_segment.xy, fs_segment.zw);
    float d_prev = dist_to_segment(frag, fs_prev_segment.xy, fs_prev_segment.zw);
    float d_next = dist_to_segment(frag, fs_next_segment.xy, fs_next_segment.zw);
    float dist   = min(d0, min(d_prev, d_next));

    float half_px = max(u.line_px * 0.5, 0.5);
    float aa      = max(fwidth(dist), 0.75);
    float alpha   = 1.0 - smoothstep(half_px - aa, half_px + aa, dist);
    if (alpha <= 0.0) {
        discard;
    }

    frag_color = vec4(u.view.color.rgb, u.view.color.a * alpha);
}
