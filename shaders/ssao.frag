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

float LinearDepth(View v, vec2 uv) {
    return ViewZ(v, max(Img(IMG_DEPTH, SMP_POINT_CLAMP, uv).x, 1.0e-7));
}

float Pair(float z, float za, float zb, float k, float max_distance, float falloff) {
    float da = z - za;
    float db = z - zb;
    float a = Saturate(da * k + 0.5);
    float b = Saturate(db * k + 0.5);
    float wa = Saturate(falloff * (max_distance - da));
    float wb = Saturate(falloff * (max_distance - db));
    float fa = wa * (0.5 - a) + 0.5;
    float fb = wb * (0.5 - b) + 0.5;
    return (wa * (a - fb) + fb) + (wb * (b - fa) + fa);
}

void main() {
    View v = frame.views[pass.ids.x];
    float depth = Img(IMG_DEPTH, SMP_POINT_CLAMP, in_uv).x;
    if (depth <= 0.0) {
        out_color = vec4(1.0);
        return;
    }
    float z = ViewZ(v, depth);
    uint hx = uint(gl_FragCoord.x) & 3u;
    uint hy = uint(gl_FragCoord.y) & 3u;
    float angle = float((hx * 7u + hy * 13u) & 15u) * (6.2831853 / 16.0) + float(hy) * 0.37;
    vec2 rot = vec2(cos(angle), sin(angle));
    float aspect = v.viewport.y / v.viewport.x;
    vec2 radius_a = vec2(min(0.07, pass.f0.x / z) * aspect, min(0.07, pass.f0.x / z));
    vec2 radius_b = vec2(min(0.07, pass.f0.y / z) * aspect, min(0.07, pass.f0.y / z));
    vec2 d1 = vec2(0.216506 * rot.x - 0.125 * rot.y, 0.125 * rot.x + 0.216506 * rot.y);
    vec2 d2 = vec2(-0.433013 * rot.x - 0.25 * rot.y, 0.25 * rot.x - 0.433013 * rot.y);
    vec2 d3 = vec2(rot.y, -rot.x) * 0.75;
    float inner = 0.0338425;
    float outer = 0.0338425;
    inner += 0.156523 * Pair(z, LinearDepth(v, in_uv + d1 * radius_a), LinearDepth(v, in_uv - d1 * radius_a), 0.5164734, pass.f1.x, pass.f1.z);
    outer += 0.156523 * Pair(z, LinearDepth(v, in_uv + d1 * radius_b), LinearDepth(v, in_uv - d1 * radius_b), 0.5164734, pass.f1.y, pass.f1.w);
    inner += 0.176937 * Pair(z, LinearDepth(v, in_uv + d2 * radius_a), LinearDepth(v, in_uv - d2 * radius_a), 0.5775443, pass.f1.x, pass.f1.z);
    outer += 0.176937 * Pair(z, LinearDepth(v, in_uv + d2 * radius_b), LinearDepth(v, in_uv - d2 * radius_b), 0.5775443, pass.f1.y, pass.f1.w);
    inner += 0.132698 * Pair(z, LinearDepth(v, in_uv + d3 * radius_a), LinearDepth(v, in_uv - d3 * radius_a), 0.75592875, pass.f1.x, pass.f1.z);
    outer += 0.132698 * Pair(z, LinearDepth(v, in_uv + d3 * radius_b), LinearDepth(v, in_uv - d3 * radius_b), 0.75592875, pass.f1.y, pass.f1.w);
    float occlusion = Saturate(Saturate(2.0 * inner - 1.0) + Saturate(2.0 * outer - 1.0));
    float strength = Saturate(pass.f2.x * (z - pass.f2.y)) * Saturate(pass.f2.w * (pass.f2.z - z));
    out_color = vec4(1.0 - Saturate(strength) * occlusion);
}
