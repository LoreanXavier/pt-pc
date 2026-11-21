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

float Hash(float n) {
    return fract(sin(n * 12.9898) * 43758.5453);
}

vec3 BackdropHighlights(vec3 encoded) {
    vec3 light = pow(max(encoded, vec3(0.0)), vec3(2.2));
    float m = max(light.r, max(light.g, light.b));
    float gain = min(2.0 * frame.display.z, max(0.8 * frame.display.y, 1.0));
    float s = smoothstep(0.55, 1.0, m);
    light *= 1.0 + (gain - 1.0) * s * s;
    return pow(light, vec3(1.0 / 2.2));
}

float Covered(ivec2 p, ivec2 size) {
    return ImgFetch(IMG_DEPTH, clamp(p, ivec2(0), size - 1)).x > 1.0e-6 ? 1.0 : 0.0;
}

void main() {
    vec3 scene = Img(pass.ids.x, SMP_LINEAR_CLAMP, in_uv).rgb;
    ivec2 size = ivec2(ImgSize(IMG_DEPTH));
    ivec2 p = ivec2(in_uv * vec2(size));
    float covered = (Covered(p, size) * 2.0 + Covered(p + ivec2(1, 0), size) + Covered(p - ivec2(1, 0), size) + Covered(p + ivec2(0, 1), size) +
                     Covered(p - ivec2(0, 1), size)) / 6.0;
    if (covered >= 0.999) {
        out_color = vec4(scene, Luma601(scene));
        return;
    }
    float frame_aspect = pass.f2.x > 0.0 ? pass.f2.x : 16.0 / 9.0;
    float image_aspect = pass.f1.w > 0.0 ? pass.f1.w : 16.0 / 9.0;
    float band = clamp(image_aspect / frame_aspect, min(1.0, image_aspect / (16.0 / 9.0)), 1.0);
    float row = clamp(pass.m[0].w, band * 0.5, 1.0 - band * 0.5);
    float zoom = pass.m[0].x > 0.0 ? pass.m[0].x : 1.0;
    vec2 axis = vec2(0.5 + 0.5 * pass.m[0].y / frame_aspect, 0.5 - 0.5 * pass.m[0].z);
    vec2 shot = axis + (in_uv - 0.5) / zoom;
    vec2 uv = vec2(0.5 + (shot.x - 0.5) * band * frame_aspect / image_aspect, row + (shot.y - 0.5) * band);
    float tear = pass.f0.z;
    float split = 0.0;
    if (tear > 0.0) {
        float t = floor(pass.f0.w * 12.0);
        float line = floor(in_uv.y * 36.0) + t * 7.0;
        float pick = Hash(line + 0.37);
        if (pick > 1.0 - 0.45 * tear) uv.x += (Hash(line) - 0.5) * 0.12 * tear;
        split = 0.006 * tear;
    }
    uint tex = pass.ids.y & 0x3FFFFFFFu;
    vec3 bg;
    if (uv.x < 0.0 || uv.x > 1.0) {
        float past = uv.x < 0.0 ? -uv.x : uv.x - 1.0;
        vec2 m = vec2(uv.x < 0.0 ? -uv.x : 2.0 - uv.x, uv.y);
        bg = vec3(0.0);
        for (int i = -2; i <= 2; ++i) {
            for (int j = -2; j <= 2; ++j) {
                bg += texture(PT_TEX2D(tex), clamp(m + vec2(i, j) * (0.004 + 0.03 * past), 0.0, 1.0)).rgb;
            }
        }
        bg *= (1.0 / 25.0) * mix(0.8, 0.55, clamp(past * 4.0, 0.0, 1.0));
    } else {
        uv.y = clamp(uv.y, 0.0, 1.0);
        bg.r = texture(PT_TEX2D(tex), clamp(uv + vec2(split, 0.0), 0.0, 1.0)).r;
        bg.g = texture(PT_TEX2D(tex), uv).g;
        bg.b = texture(PT_TEX2D(tex), clamp(uv - vec2(split, 0.0), 0.0, 1.0)).b;
    }
    bg = clamp(bg, 0.0, 1.0) * pass.f1.rgb * (1.0 - clamp(pass.f0.y, 0.0, 1.0));
    if (HdrOutput()) {
        bg = BackdropHighlights(bg);
    }
    vec3 c = scene + bg * (1.0 - covered) * clamp(pass.f0.x, 0.0, 1.0);
    c = HdrOutput() ? max(c, vec3(0.0)) : clamp(c, 0.0, 1.0);
    out_color = vec4(c, Luma601(c));
}
