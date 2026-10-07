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

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 scene = ImgFetch(IMG_HDR_COPY, pixel);
    vec4 particles = ImgFetch(IMG_PARTICLES, pixel);
    if (particles.a >= 1.0 && all(equal(particles.rgb, vec3(0.0)))) {
        out_color = scene;
        return;
    }
    vec3 encoded = particles.rgb + particles.a * SrgbEncode(max(scene.rgb, vec3(0.0)));
    out_color = vec4(SrgbDecode(max(encoded, vec3(0.0))), pass.ids.x == 1u ? scene.a * particles.a : scene.a);
}
