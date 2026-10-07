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
    ivec2 size = ivec2(ImgSize(IMG_AO));
    float sum = 0.0;
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) {
            sum += ImgFetch(IMG_AO, clamp(pixel + ivec2(x, y), ivec2(0), size - 1)).x;
        }
    }
    float visibility = sum / 16.0;
    out_color = vec4(pow(Saturate(visibility), 1.0 / max(pass.f0.x, 1.0e-3)));
}
