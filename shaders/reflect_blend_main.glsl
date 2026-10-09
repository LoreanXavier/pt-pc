// The local reflection blend (rendering.md 12.16): reflect_blend.frag, and reflect_blend_rt.frag with PT_RT_REFLECTIONS (12.21).
// With PT_REFLECT_LAYER (reflect_layer.frag, reflect_layer_rt.frag; a PC addition for the temporal upscalers, 12.16) the pass
// writes the reflection on its own instead of mixing it into the scene: premultiplied colour and amount, and where its history
// lies, for reflect_temporal.frag, which accumulates it and does the mix.
#include "common.glsl"
#ifdef PT_RT_REFLECTIONS
// the traced hits' colour (reflect_make_rt.frag's second target), through the ray tracing set
layout(set = 2, binding = 2) uniform sampler2D rt_reflection_color;
#include "reflection_mix.glsl"
#endif
#ifdef PT_REFLECT_LAYER
#ifndef PT_RT_REFLECTIONS
#include "reflection_mix.glsl"
#endif
#include "reflection_depth.glsl"
#endif

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
#ifdef PT_REFLECT_LAYER
layout(location = 1) out vec4 out_history;

// out_color with no reflection: amount 0 on a floor (the temporal pass may keep its history there), -1 on anything else
vec4 NoReflection(vec3 N, vec3 plane) {
    return vec4(0.0, 0.0, 0.0, dot(N, plane) >= 0.9 ? 0.0 : -1.0);
}

// Where the reflection at uv was last frame, as an offset from uv: the reflected point's virtual image (the mirror image of the
// hit across the horizontal floor) lies on the pixel's own ray, as far as the floor plus the hit's distance from the floor point,
// and the previous unjittered view projection (pass.m) puts it on last frame's screen. The offset follows the upscaler's motion
// convention (unjittered now to unjittered then). z: 1 for a reflection's own motion, 0 for the floor's (no hit to go by).
vec4 HistoryOffset(View v, vec2 uv, vec2 hit_uv, bool has_hit) {
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 P = ReflectionViewPosition(ndc, v.projection_param.xy, v.jitter.xy, ViewZ(v, ReflectionFloorDepth(v, uv)));
    float distance = length(P);
    float hit_depth = has_hit ? ReflectionFloorDepth(v, hit_uv) : 0.0;
    has_hit = hit_depth > 0.0;
    if (has_hit) {
        vec3 H = ReflectionViewPosition(hit_uv * 2.0 - 1.0, v.projection_param.xy, v.jitter.xy, ViewZ(v, hit_depth));
        distance += length(H - P);
    }
    vec3 virtual_image = P * (distance / max(length(P), 1.0e-6));
    vec4 previous = pass.m * vec4((v.inv_view * vec4(virtual_image, 1.0)).xyz, 1.0);
    if (!(previous.w > 1.0e-6)) {
        return vec4(0.0, 0.0, -1.0, 0.0);
    }
    return vec4((previous.xy / previous.w - (ndc - v.jitter.xy)) * 0.5, has_hit ? 1.0 : 0.0, 0.0);
}
#endif

void main() {
    View v = frame.views[pass.ids.x];
#ifdef PT_REFLECT_LAYER
    g_reflection_depth_legacy = pass.f0.w > 0.5;
#endif
    vec2 uv = (gl_FragCoord.xy - 0.00390625) * v.viewport.zw;
    vec2 map_scale = pass.f1.xy;
    vec4 base = ImgLod(IMG_HDR_COPY, SMP_LINEAR_WRAP, uv, 0.0);
    vec3 N = DecodeNormal(ImgLod(IMG_NORMAL, SMP_POINT_CLAMP, uv, 0.0).xyz);
    vec4 material = ImgLod(IMG_MATERIAL, SMP_LINEAR_WRAP, uv, 0.0);
    vec4 center = ImgLod(IMG_REFMAP, SMP_LINEAR_WRAP, uv / map_scale, 0.0);
    vec3 plane = pass.f0.xyz;
    float weight = center.z * center.w;
#ifdef PT_RT_REFLECTIONS
    weight = RtReflectionCenterWeight(center, textureLod(rt_reflection_color, uv / map_scale, 0.0));
#endif
    vec2 tilt = (N.xy - plane.xy) * weight;
    vec4 refl = ImgLod(IMG_REFMAP, SMP_LINEAR_WRAP, (uv + 0.15 * vec2(-tilt.x, tilt.y)) / map_scale, 0.0);
    float valid = refl.y < 0.0 || refl.y > 1.0 || refl.x > 1.0 || refl.x < 0.0 ? 0.0 : refl.w;
#ifdef PT_RT_REFLECTIONS
    // traced hits off screen: their colour, premultiplied by their weight (reflect_make_rt.frag)
    vec4 traced = textureLod(rt_reflection_color, (uv + 0.15 * vec2(-tilt.x, tilt.y)) / map_scale, 0.0);
    if (!(valid > 0.0) && !(traced.a > 1.0e-4)) {
#ifdef PT_REFLECT_LAYER
        out_color = NoReflection(N, plane);
        out_history = HistoryOffset(v, uv, uv, false);
#else
        out_color = base;
#endif
        return;
    }
#else
    if (!(valid > 0.0)) {
#ifdef PT_REFLECT_LAYER
        out_color = NoReflection(N, plane);
        out_history = HistoryOffset(v, uv, uv, false);
#else
        out_color = base;
#endif
        return;
    }
#endif
    float facing = Saturate(dot(N, plane));
    float lean = dot(N, pass.f2.xyz);
    float smooth_term = pow(abs(1.0 - material.x), 4.0);
    float strength = Saturate(pass.f1.z * smooth_term * max(1.0, min(4.0, lean * 3.0 + 2.0)) + pass.f1.w);
#ifdef PT_RT_REFLECTIONS
    vec2 coordinate = RtScreenCoordinate(refl);
    vec3 screen_hit = ImgLod(IMG_HDR_COPY, SMP_LINEAR_WRAP, uv + map_scale * (2.0 * coordinate - 1.0), 0.0).rgb;
    vec4 contribution = RtReflectionContribution(refl, traced, screen_hit);
    float amount = strength * facing * contribution.a;
#else
    float amount = strength * facing * refl.z * valid;
#endif
    if (pass.f2.w < 1.0) {
        amount -= abs(2.0 * uv.x - 1.0) * amount;
    }
    vec3 hit = ImgLod(IMG_HDR_COPY, SMP_LINEAR_WRAP, uv + map_scale * (2.0 * refl.xy - 1.0), 0.0).rgb;
#ifdef PT_RT_REFLECTIONS
    hit = contribution.rgb;
#endif
#ifdef PT_REFLECT_LAYER
    amount = max(amount, 0.0);
    out_color = vec4(min(hit, vec3(1.0)) * amount, amount);
#ifdef PT_RT_REFLECTIONS
    // a traced hit off screen has no screen position: its history follows the floor
    bool on_screen = refl.w > 1.0e-4 && refl.z > 0.0 && all(greaterThanEqual(coordinate, vec2(0.0))) && all(lessThanEqual(coordinate, vec2(1.0)));
    out_history = HistoryOffset(v, uv, uv + map_scale * (2.0 * coordinate - 1.0), on_screen);
#else
    out_history = HistoryOffset(v, uv, uv + map_scale * (2.0 * refl.xy - 1.0), true);
#endif
#else
    out_color = vec4(mix(min(base.rgb, vec3(1.0)), min(hit, vec3(1.0)), amount), base.a);
#endif
}
