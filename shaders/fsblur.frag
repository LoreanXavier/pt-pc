#version 460
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    uint current = pass.ids.x;
    uint history = pass.ids.y;
    vec2 texel = 1.0 / ImgSize(history);
    vec2 o = texel * pass.f0.x;
    vec2 lo = texel * 0.5;
    vec2 hi = vec2(1.0) - texel * 0.5;
    vec3 c = Img(history, SMP_LINEAR_CLAMP, clamp(in_uv, lo, hi)).rgb * 4.0;
    const vec2 edges[4] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
    const vec2 corners[4] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    for (int i = 0; i < 4; ++i) {
        c += Img(history, SMP_LINEAR_CLAMP, clamp(in_uv + edges[i] * o, lo, hi)).rgb * 2.0;
        c += Img(history, SMP_LINEAR_CLAMP, clamp(in_uv + corners[i] * o, lo, hi)).rgb;
    }
    c *= 0.0625;
    vec3 now = ImgFetch(current, ivec2(gl_FragCoord.xy)).rgb;
    vec3 result = mix(now, c, Saturate(pass.f0.y));
    out_color = vec4(result, Luma601(result));
}
