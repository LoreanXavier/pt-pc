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

// NearFarUpScale2x2 (lisa_balcony_f070 3881, op 1544). Each half-resolution effect layer stores its
// sRGB-encoded premultiplied colour and transmittance. The depth range and the change in alpha between
// full and half samples choose how the two layers are reconstructed at this full-resolution pixel.
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (pass.ids.x == 1u) {
        vec4 scene = ImgFetch(IMG_HDR_COPY, pixel);
        vec4 particles = ImgFetch(IMG_PARTICLES, pixel);
        if (particles.a >= 1.0 && all(equal(particles.rgb, vec3(0.0)))) {
            out_color = scene;
            return;
        }
        vec3 encoded = particles.rgb + particles.a * SrgbEncode(max(scene.rgb, vec3(0.0)));
        out_color = vec4(SrgbDecode(max(encoded, vec3(0.0))), scene.a * particles.a);
        return;
    }
    ivec2 full_size = ivec2(ImgSize(IMG_HDR_COPY));
    ivec2 half_size = ivec2(ImgSize(IMG_NEAR_FAR_DEPTH));
    ivec2 half_pixel = clamp(ivec2(floor(0.5 * gl_FragCoord.xy - 0.25)), ivec2(0), half_size - 1);
    vec2 full_uv = gl_FragCoord.xy / vec2(full_size);
    vec2 half_uv = (vec2(half_pixel) + 0.5) / vec2(half_size);

    vec4 scene = ImgFetch(IMG_HDR_COPY, pixel);
    vec4 near_full = Img(IMG_PARTICLES_NEAR, SMP_LINEAR_WRAP, full_uv);
    vec4 near_half = Img(IMG_PARTICLES_NEAR, SMP_LINEAR_WRAP, half_uv);
    vec4 far_full = Img(IMG_PARTICLES_FAR, SMP_LINEAR_WRAP, full_uv);
    vec4 far_half = Img(IMG_PARTICLES_FAR, SMP_LINEAR_WRAP, half_uv);
    vec2 range = ImgFetch(IMG_NEAR_FAR_DEPTH, half_pixel).rg;
    float scene_reverse_z = ImgFetch(IMG_DEPTH, pixel).r;

    float near_change = clamp(abs(near_half.a - near_full.a) * 4.0, 0.0, 1.0);
    float far_change = clamp(abs(far_half.a - far_full.a) * 4.0, 0.0, 1.0);
    vec4 near_layer = mix(near_full, near_half, near_change);
    vec4 far_layer = mix(far_full, far_half, far_change);

    const float near_plane = max(pass.f0.x, 1.0e-7);
    float nearest_depth = near_plane / max(range.y, 1.0e-7);
    float farthest_depth = near_plane / max(range.x, 1.0e-7);
    float scene_depth = near_plane / max(scene_reverse_z, 1.0e-7);
    float depth_span = farthest_depth - nearest_depth;
    float depth_weight = abs(depth_span) <= 1.0e-7 ? 0.0 : clamp((scene_depth - nearest_depth) / depth_span, 0.0, 1.0);
    vec3 premultiplied = mix(far_layer.rgb, near_layer.rgb, depth_weight);
    float transmittance = mix(far_layer.a, near_layer.a, depth_weight);

    vec3 encoded_scene = SrgbEncode(max(scene.rgb, vec3(0.0)));
    out_color = vec4(SrgbDecode(max(premultiplied + transmittance * encoded_scene, vec3(0.0))), scene.a);
}
