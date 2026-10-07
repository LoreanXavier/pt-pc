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
    View v = frame.views[pass.ids.w];
    vec2 velocity = 2.0 * Img(pass.ids.x, SMP_POINT_CLAMP, in_uv).zw - 1.0;
    float depth = Img(IMG_DEPTH, SMP_POINT_CLAMP, in_uv).x;
    vec2 tile = 2.0 * Img(pass.ids.y, SMP_LINEAR_CLAMP, in_uv).zw - 1.0;
    vec2 scaled = 128.0 * velocity * pass.f0.xy * pass.f0.w;
    vec2 dominant = dot(tile, tile) >= dot(velocity, velocity) ? tile : velocity;
    out_color = vec4(0.015625 * sqrt(1.0e-7 + dot(scaled, scaled)), sqrt(Saturate(abs(ViewZ(v, depth)) * 0.03125)), 0.5 * dominant + 0.5);
}
