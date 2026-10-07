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

const float kOffsets[3] = float[](0.0, 1.3846153846, 3.2307692308);
const float kWeights[3] = float[](0.2270270270, 0.3162162162, 0.0702702703);

void main() {
    uint source = pass.ids.x;
    vec2 step = pass.f0.xy / ImgSize(source);
    vec4 c = Img(source, SMP_LINEAR_CLAMP, in_uv) * kWeights[0];
    for (int i = 1; i < 3; ++i) {
        c += Img(source, SMP_LINEAR_CLAMP, in_uv + step * kOffsets[i]) * kWeights[i];
        c += Img(source, SMP_LINEAR_CLAMP, in_uv - step * kOffsets[i]) * kWeights[i];
    }
    out_color = c;
}
