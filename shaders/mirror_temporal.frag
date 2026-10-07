#version 460
#include "common.glsl"
#include "reflection_mix.glsl"
#include "reflection_depth.glsl"
#include "mirror_temporal.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec3 CurrentColor(vec2 pixel) {
    vec2 sample_pixel = clamp(pixel, vec2(0.5), pass.f0.xy - 0.5);
    return Img(IMG_MIRROR, SMP_LINEAR_CLAMP, sample_pixel * pass.f0.zw).rgb;
}

void main() {
    View capture = frame.views[pass.ids.w];
    vec2 pixel = gl_FragCoord.xy;
    vec2 uv = pixel / pass.f0.xy;
    vec2 sample_pixel = pixel + capture.jitter.xy * pass.f0.xy * 0.5;
    vec3 current = CurrentColor(sample_pixel);
    float depth = ReflectionSceneDepth(capture, clamp(sample_pixel, vec2(0.5), pass.f0.xy - 0.5) * pass.f0.zw);
    float z = depth > 0.0 ? ViewZ(capture, depth) : 0.0;
    out_color = vec4(current, z);
    if (pass.ids.x == 0u || !(z > 0.0)) return;

    vec2 ndc = uv * 2.0 - 1.0;
    vec3 world = (capture.inv_view * vec4(ndc * capture.projection_param.xy * z, z, 1.0)).xyz;
    vec4 previous = pass.m * vec4(world, 1.0);
    if (!(previous.w > 0.0)) return;
    vec2 previous_uv = previous.xy / previous.w * 0.5 + 0.5;
    if (any(lessThan(previous_uv, vec2(0.0))) || any(greaterThan(previous_uv, vec2(1.0)))) return;
    vec2 previous_pixel = clamp(previous_uv * pass.f0.xy, vec2(0.5), pass.f0.xy - 0.5);
    vec4 history = Img(48u, SMP_LINEAR_CLAMP, previous_pixel * pass.f0.zw);
    float depth_diff = abs(history.a - previous.w);
    float tol = max(0.04, previous.w * 0.05);
    if (!(history.a > 0.0) || !(previous.w > 0.0) || depth_diff > tol * 2.0) return;
    float depth_weight = clamp(1.0 - depth_diff / (tol * 2.0), 0.0, 1.0);

    vec3 lo = current, hi = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 neighbor = CurrentColor(sample_pixel + vec2(x, y));
            lo = min(lo, neighbor);
            hi = max(hi, neighbor);
        }
    }
    out_color.rgb = MirrorTemporalColor(current, history.rgb, lo, hi, pass.f1.x, pass.f1.y * depth_weight);
}
