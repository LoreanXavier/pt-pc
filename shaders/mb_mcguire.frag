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

float Inverse(float x) {
    return x != 0.0 ? 1.0 / x : 1.0e30;
}

void main() {
    uint image = pass.ids.x;
    uint vd = pass.ids.y;
    vec4 lp = pass.f0;
    vec2 uv = in_uv;
    vec4 center_vd = ImgLod(vd, SMP_POINT_CLAMP, uv, 0.0);
    vec2 dir = vec2(0.06666667 * lp.x * lp.w, 0.11851852 * lp.y * lp.w) * (2.0 * center_vd.zw - 1.0) * lp.z;
    if (1.1286062e-06 > dot(dir, dir)) {
        vec4 c = ImgLod(image, SMP_POINT_CLAMP, uv, 0.0);
        out_color = vec4(c.rgb, lp.z >= 0.99 ? 0.0 : c.a);
        return;
    }
    vec4 center = ImgLod(image, SMP_POINT_CLAMP, uv, 0.0);
    vec2 step = 0.16666667 * dir;
    vec2 start = step * -3.0 + uv;
    float d0 = 0.015625 * length((start - uv) * vec2(1920.0, 1080.0));
    float dstep = -0.015625 * length(step * vec2(1920.0, 1080.0));
    float zc = center_vd.y * center_vd.y;
    float inv_c = Inverse(center_vd.x);
    vec3 sum = 4.0 * center.rgb;
    float weight = 4.0;
    for (int k = 0; k < 7; ++k) {
        if (k == 3) {
            continue;
        }
        float fk = float(k);
        vec2 suv = fk * step + start;
        vec4 s_vd = ImgLod(vd, SMP_POINT_CLAMP, suv, 0.0);
        vec3 s = ImgLod(image, SMP_POINT_CLAMP, suv, 0.0).rgb;
        float d = abs(fk * dstep + d0);
        float zs = s_vd.y * s_vd.y;
        float cone_s = 1.0 - d * Inverse(s_vd.x);
        float cone_c = 1.0 - d * inv_c;
        float front = Saturate((zc - zs) * 128.0 + 1.0);
        float back = Saturate((zs - zc) * 128.0 + 1.0);
        float w = 2.0 * Saturate(cone_s + 0.95) * Saturate(cone_c + 0.95) + front * Saturate(cone_s) + back * Saturate(cone_c);
        sum += s * w;
        weight += w;
    }
    vec3 c = sum * Inverse(weight);
    out_color = vec4(c, lp.z >= 0.99 ? Saturate(0.25 * weight - 1.2) : center.a);
}
