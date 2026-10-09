#version 460
#include "common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec2 out_depth_range;

// DownSampleDepth_NearFar (lisa_balcony_f070 3881): 2x2 reverse-Z min/max for the half-resolution
// effects pass, with the minimum also written to its GEQUAL depth attachment.
void main() {
    ivec2 source_size = ivec2(ImgSize(IMG_DEPTH));
    ivec2 half_pixel = ivec2(gl_FragCoord.xy);
    ivec2 source = half_pixel * 2;
    ivec2 hi = source_size - 1;
    float d0 = ImgFetch(IMG_DEPTH, clamp(source, ivec2(0), hi)).r;
    float d1 = ImgFetch(IMG_DEPTH, clamp(source + ivec2(0, 1), ivec2(0), hi)).r;
    float d2 = ImgFetch(IMG_DEPTH, clamp(source + ivec2(1, 0), ivec2(0), hi)).r;
    float d3 = ImgFetch(IMG_DEPTH, clamp(source + ivec2(1, 1), ivec2(0), hi)).r;
    float minimum_depth = min(min(d0, d1), min(d2, d3));
    float maximum_depth = max(max(d0, d1), max(d2, d3));
    out_depth_range = vec2(minimum_depth, maximum_depth);
    gl_FragDepth = minimum_depth;
}
