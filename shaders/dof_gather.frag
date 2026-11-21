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
layout(location = 0) out vec4 out_far;
layout(location = 1) out vec4 out_near;

const int kTaps = 48;

float FarRadius(float coc) {
    return pass.f0.x * Saturate((coc - pass.f0.z) / max(1.0 - pass.f0.z, 1.0e-3));
}

void main() {
    uint far_image = pass.ids.x;
    uint near_image = pass.ids.y;
    vec2 texel = 1.0 / vec2(ImgSize(far_image));
    vec2 uv = in_uv;
    vec4 far_center = Img(far_image, SMP_POINT_CLAMP, uv);
    vec4 near_center = Img(near_image, SMP_POINT_CLAMP, uv);
    float far_radius = FarRadius(far_center.a);
    float near_radius = pass.f0.y;
    vec4 far = far_center;
    vec4 near = near_center;
    float cover = near_center.a;
    for (int i = 0; i < kTaps; ++i) {
        float t = sqrt((float(i) + 0.5) / float(kTaps));
        float angle = 2.3999631 * float(i);
        vec2 ring = vec2(cos(angle), sin(angle)) * t;
        if (far_radius > 0.25) {
            vec4 s = Img(far_image, SMP_LINEAR_CLAMP, uv + ring * far_radius * texel);
            far += s * Saturate(FarRadius(s.a) - t * far_radius + 1.0);
        }
        if (near_radius > 0.25) {
            vec4 s = Img(near_image, SMP_LINEAR_CLAMP, uv + ring * near_radius * texel);
            near += s;
            cover = max(cover, s.a);
        }
    }
    out_far = vec4(far.a > 1.0e-5 ? far.rgb / far.a : vec3(0.0), far_center.a);
    out_near = vec4(near.a > 1.0e-5 ? near.rgb / near.a : vec3(0.0), Saturate(cover * pass.f0.w));
}
