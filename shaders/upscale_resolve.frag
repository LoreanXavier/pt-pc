#version 460
#include "common.glsl"

#define IMG_UPSCALED 43
#define IMG_MOTION 45
#define IMG_HANDY_FACTOR 55

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_depth;
layout(location = 2) out vec4 out_velocity;

// The accumulated floor reflection (reflect_temporal.frag's history, slot ids.z; 0 none) at a render position: bilinear over the
// floor texels (amount -1 off the floors counts as no reflection)
vec4 Reflection(vec2 render_uv) {
    ivec2 size = ivec2(pass.f1.xy);
    vec2 texel = render_uv * pass.f1.xy - 0.5;
    ivec2 p0 = ivec2(floor(texel));
    vec2 f = texel - vec2(p0);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 4; ++i) {
        ivec2 o = ivec2(i & 1, i >> 1);
        vec4 s = ImgFetch(pass.ids.z, clamp(p0 + o, ivec2(0), size - 1));
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        sum += s.a > 0.0 ? s * w : vec4(0.0);
    }
    return sum;
}

void main() {
    vec2 uv = gl_FragCoord.xy * pass.f0.zw;
    vec2 render_uv = uv + pass.f0.xy;
    vec4 source = Img(IMG_HDR, SMP_LINEAR_CLAMP, render_uv);
    vec3 color = source.rgb;
    if (pass.ids.x == 1u) {
        color = ImgFetch(IMG_UPSCALED, ivec2(gl_FragCoord.xy)).rgb;
        // the handy light back on the upscaled colour (upscale_demod.frag), its factor at this output pixel's render position
        if (pass.f1.z > 0.5) {
            color *= Img(IMG_HANDY_FACTOR, SMP_LINEAR_CLAMP, render_uv).x;
        }
    } else if (pass.ids.x == 2u) {
        vec2 motion = Img(IMG_MOTION, SMP_POINT_CLAMP, render_uv).xy;
        color = Img(IMG_UPSCALED, SMP_LINEAR_CLAMP, uv + motion).rgb;
    }
    // PC addition (12.16): the floor reflection mixed in after the upscaler, as reflect_blend.frag mixes it, so the upscaler
    // reprojects the floor alone (its motion is the floor's; the reflected image's is not) and the reflection keeps its own history
    if (pass.ids.z != 0u) {
        // at the output pixel itself, not the jittered render position: the accumulation averages the jitter, and reading
        // it through this frame's jitter moved a thin bright reflection (a lit baseboard) by up to half a texel every frame
        vec4 reflection = Reflection(uv);
        if (reflection.a > 0.0) {
            color = SrgbDecode(SrgbEncode(min(color, vec3(1.0))) * (1.0 - reflection.a) + reflection.rgb);
        }
    }
    ivec2 size = ivec2(pass.f1.xy);
    ivec2 nearest = clamp(ivec2(floor(render_uv * pass.f1.xy)), ivec2(0), size - 1);
    out_color = vec4(color, source.a);
    out_depth = vec4(ImgFetch(IMG_DEPTH, nearest).x, 0.0, 0.0, 0.0);
    out_velocity = ImgFetch(pass.ids.y, nearest);
}
