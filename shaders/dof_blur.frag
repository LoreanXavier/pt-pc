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

vec4 Tap(vec2 uv) {
    return Img(pass.ids.x, SMP_LINEAR_CLAMP, uv);
}

void main() {
    vec2 p = gl_FragCoord.xy - 0.5;
    float coc = Img(pass.ids.y, SMP_POINT_CLAMP, pass.f2.zw * p + pass.f2.zw * 0.5).w;
    float amount = pass.ids.z == 0u ? Saturate(2.0 * coc - 1.0) : Saturate(0.5 - coc) * 2.0;
    float r = pass.f1.z * pass.f1.x * Saturate(amount - pass.f1.y);
    vec2 o = pass.f0.xy * r;
    vec2 c = pass.f0.xy * (p + 0.5);
    vec4 sum = Tap(c) * 0.25;
    sum += Tap(c + vec2(-0.8660254 * o.x, 0.5 * o.y)) * 0.125;
    sum += Tap(c + vec2(0.0, o.y)) * 0.125;
    sum += Tap(c + vec2(-0.8660254 * o.x, -0.5 * o.y)) * 0.125;
    sum += Tap(c + vec2(0.0, -o.y)) * 0.125;
    sum += Tap(c + vec2(0.8660254 * o.x, -0.5 * o.y)) * 0.125;
    sum += Tap(c + vec2(0.8660254 * o.x, 0.5 * o.y)) * 0.125;
    out_color = sum;
}
