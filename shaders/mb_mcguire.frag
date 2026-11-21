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

const float kSwitchOn = 1.0623588e-03;

void main() {
    uint image = pass.ids.x;
    uint vd = pass.ids.y;
    vec4 lp = pass.f0;
    vec2 uv = in_uv;
    vec4 center_vd = ImgLod(vd, SMP_POINT_CLAMP, uv, 0.0);
    vec2 dir = vec2(0.06666667 * lp.x * lp.w, 0.11851852 * lp.y * lp.w) * MbLoad(center_vd.zw) * lp.z;
    bool street = pass.ids.z != 0u;
    bool fine = pass.ids.z == 2u;
    float full = length(dir) / max(lp.z, 1.0e-6);
    float kept = street ? full * smoothstep(0.5 * kSwitchOn, 1.5 * kSwitchOn, full) : full;
    dir *= full > 0.0 ? kept / full : 0.0;
    if ((street && kept <= 0.0) || (!street && dot(dir, dir) < 1.1286062e-06)) {
        vec4 c = ImgLod(image, SMP_POINT_CLAMP, uv, 0.0);
        out_color = vec4(c.rgb, lp.z >= 0.99 ? 0.0 : c.a);
        return;
    }
    vec4 center = ImgLod(image, SMP_POINT_CLAMP, uv, 0.0);
    int kTaps = fine ? clamp(int(0.5 * length(dir * vec2(1920.0, 1080.0))) | 1, 17, 49) : street ? 15 : 7;
    int kCentre = kTaps / 2;
    float jitter = street ? fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y)) - 0.5 : 0.0;
    vec2 step = street ? dir / float(kTaps - 1) : 0.16666667 * dir;
    vec2 start = step * (jitter - float(kCentre)) + uv;
    float d0 = 0.015625 * length((start - uv) * vec2(1920.0, 1080.0));
    float dstep = -0.015625 * length(step * vec2(1920.0, 1080.0));
    float zc = fine ? center_vd.y : center_vd.y * center_vd.y;
    float inv_c = Inverse(center_vd.x);
    float kCentreWeight = 4.0 * float(kTaps - 1) / 6.0;
    vec3 sum = kCentreWeight * center.rgb;
    float weight = kCentreWeight;
    for (int k = 0; k < kTaps; ++k) {
        if (k == kCentre) {
            continue;
        }
        float fk = float(k);
        vec2 suv = fk * step + start;
        vec4 s_vd = ImgLod(vd, SMP_POINT_CLAMP, suv, 0.0);
        vec3 s = ImgLod(image, street ? SMP_LINEAR_CLAMP : SMP_POINT_CLAMP, suv, 0.0).rgb;
        float d = abs(fk * dstep + d0);
        float zs = fine ? s_vd.y : s_vd.y * s_vd.y;
        float cone_s = 1.0 - d * Inverse(s_vd.x);
        float cone_c = 1.0 - d * inv_c;
        float apart = fine ? (zc - zs) / max(0.01 * min(zc, zs), 1.0e-4) : (zc - zs) * 128.0;
        float front = Saturate(apart + 1.0);
        float back = Saturate(1.0 - apart);
        float w = 2.0 * Saturate(cone_s + 0.95) * Saturate(cone_c + 0.95) + front * Saturate(cone_s) + back * Saturate(cone_c);
        sum += s * w;
        weight += w;
    }
    vec3 c = sum * Inverse(weight);
    float alpha = street ? Saturate(0.25 * weight * 6.0 / float(kTaps - 1) - 1.2) : Saturate(0.25 * weight - 1.2);
    out_color = vec4(c, lp.z >= 0.99 ? alpha : center.a);
}
