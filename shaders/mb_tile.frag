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

vec2 best = vec2(0.0);
float best_len = 0.0;

void Take(vec2 uv) {
    vec2 v = 2.0 * Img(pass.ids.x, SMP_POINT_CLAMP, uv).zw - 1.0;
    float len = dot(v, v);
    if (len > best_len) {
        best_len = len;
        best = v;
    }
}

void main() {
    vec2 t = pass.f0.xy;
    vec2 p = gl_FragCoord.xy;
    if (pass.ids.z == 0u) {
        Take(p * t - 0.25 * t);
        Take(vec2(p.x * t.x + 0.25 * t.x, p.y * t.y - 0.25 * t.y));
        Take(vec2(p.x * t.x - 0.25 * t.x, p.y * t.y + 0.25 * t.y));
        Take(p * t + 0.25 * t);
    } else {
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) {
                Take((p + vec2(float(i), float(j))) * t);
            }
        }
    }
    out_color = vec4(0.0, 0.0, 0.5 * best + 0.5);
}
