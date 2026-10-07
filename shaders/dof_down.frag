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
    vec2 t = pass.f2.zw;
    vec2 p = gl_FragCoord.xy;
    if (pass.ids.z == 0u) {
        vec4 g = textureGather(sampler2D(images[pass.ids.x], samplers[SMP_LINEAR_CLAMP]), p * t, 3);
        vec4 c = Tap(p * t);
        float near = max(Saturate(0.5 - g.x) * 2.0, max(Saturate(0.5 - g.w) * 2.0, max(Saturate(0.5 - g.y) * 2.0, Saturate(0.5 - g.z) * 2.0)));
        out_color = vec4(c.rgb, near);
        return;
    }
    if (pass.ids.z == 2u) {
        vec2 q = p - 0.00390625;
        vec4 s = Tap(vec2(t.x * q.x - 0.5 * t.x, t.y * q.y + 0.5 * t.y)) + Tap(t * q + 0.5 * t) + Tap(vec2(t.x * q.x + 0.5 * t.x, t.y * q.y - 0.5 * t.y)) +
                 Tap(t * q - 0.5 * t) + Tap(t * q);
        out_color = 0.2 * s;
        return;
    }
    float a = 0.0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            a = max(a, Tap((p + vec2(0.5 * float(i) - 0.75, 0.5 * float(j) - 0.75)) * t).w);
        }
    }
    vec3 c = Tap((p + vec2(-0.5, -0.5)) * t).rgb + Tap((p + vec2(0.5, -0.5)) * t).rgb + Tap((p + vec2(-0.5, 0.5)) * t).rgb +
             Tap((p + vec2(0.5, 0.5)) * t).rgb;
    out_color = vec4(0.25 * c, a);
}
