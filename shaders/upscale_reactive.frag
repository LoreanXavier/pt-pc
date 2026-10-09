#version 460
#include "common.glsl"
#include "flashlight_reactivity.glsl"

#define IMG_OPAQUE 44

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_reactive;

vec3 Compress(vec3 c) {
    c = max(c, vec3(0.0));
    return c / (1.0 + max(c.r, max(c.g, c.b)));
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 before = Compress(ImgFetch(IMG_OPAQUE, pixel).rgb);
    vec3 after = Compress(ImgFetch(IMG_HDR, pixel).rgb);
    vec3 d = abs(after - before);
    float delta = max(d.r, max(d.g, d.b));
    float reactive = min(pass.f0.y, delta * pass.f0.x);

    // The handy light sweeping a still surface: the wall's motion vectors are zero while its illumination moves, so the
    // temporal filter would keep reprojecting history that no longer matches the beam. The comparison above cannot see it:
    // the opaque target is copied after the lighting and forward passes (upscaling.md, Frame 4), so both images carry the same
    // light and the delta is 0 on the beam. pass.f2 carries the light's cone (xyz direction, w the falloff
    // 1 / (cos penumbra - cos umbra)), so the mask is built from the light itself: it rises with the cone the frame shows,
    // which is where the stale history has to go, and stays out of the pixels the beam does not reach.
    // A constant mask over the whole cone kept the history from settling anywhere in the beam (shimmer on still walls), so the
    // mask follows the change instead: the beam of this frame against the beam of the last (pass.m columns 0 and 1), which is
    // the trail the history still carries when the light moves, and the whole cone the frame it turns on or off.
    if ((pass.f1.w > -1.0 || pass.m[0].w > -1.0) && pass.f0.z > 0.0) {
        float depth = ImgFetch(IMG_DEPTH, pixel).x;
        if (depth > 0.0) {
            View v = frame.views[pass.ids.w];
            vec2 uv = gl_FragCoord.xy * v.viewport.zw;
            vec3 surface = ViewPosition(v, FlashlightSurfaceNdc(uv, v.jitter.xy), depth);
            vec3 world = (v.inv_view * vec4(surface, 1.0)).xyz;
            float now = FlashlightCone(world, pass.f1.xyz, pass.f1.w, pass.f2.xyz, pass.f2.w);
            float before = FlashlightCone(world, pass.m[0].xyz, pass.m[0].w, pass.m[1].xyz, pass.m[1].w);
            reactive = max(reactive, min(pass.f0.w, abs(now - before) * pass.f0.z));
        }
    }

    // PT_REACTIVE_ALL=1 (ids.x): the whole frame fully reactive, to see whether the upscaler takes the mask at all
    out_reactive = vec4(pass.ids.x != 0u ? 1.0 : reactive, 0.0, 0.0, 0.0);
}
