#version 460
#include "common.glsl"

// The local reflections' temporal accumulation (rendering.md 12.16, a PC addition, only with a temporal upscaler): the
// reflection layer of reflect_layer.frag (premultiplied colour, amount; amount -1 off the floors) accumulated over the frames
// along the reflected image's own motion, then mixed into the scene as the original's blend mixes it. The upscaler reprojects
// the floor with the floor's motion, which the reflected image does not share, so it would show the raw jittered reflection.

#define IMG_REFLECT_LAYER 50
#define IMG_REFLECT_OFFSET 51

layout(push_constant) uniform PassPush {
    uvec4 ids;   // view, history valid, history image slot
    vec4 f0;
    vec4 f1;     // exposure now / exposure of the history, history weight, neighbourhood clip width (standard deviations),
                 // 1 when upscale_resolve.frag mixes the accumulated reflection in after the upscaler (the scene stays without it)
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_history;

void main() {
    View v = frame.views[pass.ids.x];
    ivec2 size = ivec2(v.viewport.xy);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    // the scene as reflect_blend.frag reads it
    vec4 base = ImgLod(IMG_HDR_COPY, SMP_LINEAR_WRAP, (gl_FragCoord.xy - 0.00390625) * v.viewport.zw, 0.0);
    vec4 current = ImgFetch(IMG_REFLECT_LAYER, pixel);
    if (current.a < 0.0) {
        out_color = base;
        out_history = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    // the floor neighbourhood of this frame's layer, and the offset of its strongest reflection (a floor pixel the march missed
    // this frame takes its history where its neighbours' reflection came from)
    vec4 lo = current;
    vec4 hi = current;
    vec4 sum = vec4(0.0);
    vec4 sum2 = vec4(0.0);
    float count = 0.0;
    vec4 offset = ImgFetch(IMG_REFLECT_OFFSET, pixel);
    float best = offset.z > 0.5 ? 1.0e30 : -1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 p = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
            vec4 s = ImgFetch(IMG_REFLECT_LAYER, p);
            if (s.a < 0.0) {
                continue;
            }
            lo = min(lo, s);
            hi = max(hi, s);
            sum += s;
            sum2 += s * s;
            count += 1.0;
            if (s.a > best) {
                vec4 o = ImgFetch(IMG_REFLECT_OFFSET, p);
                if (o.z > 0.5) {
                    best = s.a;
                    offset = o;
                }
            }
        }
    }

    vec4 result = current;
    if (pass.ids.y != 0u && offset.z >= 0.0) {
        vec2 history_pixel = (gl_FragCoord.xy * v.viewport.zw + offset.xy) * v.viewport.xy - 0.5;
        ivec2 p0 = ivec2(floor(history_pixel));
        vec2 f = history_pixel - vec2(p0);
        vec4 history = vec4(0.0);
        float weight = 0.0;
        for (int i = 0; i < 4; ++i) {
            ivec2 o = ivec2(i & 1, i >> 1);
            ivec2 p = p0 + o;
            if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) {
                continue;
            }
            vec4 h = ImgFetch(pass.ids.z, p);
            // history off the floors (amount -1) is not this reflection's
            if (h.a < 0.0) {
                continue;
            }
            float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
            history += h * w;
            weight += w;
        }
        if (weight > 0.05) {
            history /= weight;
            history.rgb *= pass.f1.x;
            vec4 mean = sum / count;
            vec4 deviation = sqrt(max(sum2 / count - mean * mean, vec4(0.0)));
            history = clamp(history, max(lo, mean - pass.f1.z * deviation), min(hi, mean + pass.f1.z * deviation));
            result = mix(current, history, pass.f1.y * clamp(weight, 0.0, 1.0));
        }
    }
    result = max(result, vec4(0.0));
    out_history = result;
    // the original's mix (reflect_blend.frag) with the accumulated reflection: mix(min(base, 1), min(hit, 1), amount)
    // on the sRGB-encoded values, as the original's 8-bit blend (reflect_blend_main.glsl)
    out_color = result.a > 0.0 && pass.f1.w < 0.5 ? vec4(SrgbDecode(SrgbEncode(min(base.rgb, vec3(1.0))) * (1.0 - result.a) + result.rgb), base.a) : base;
}
