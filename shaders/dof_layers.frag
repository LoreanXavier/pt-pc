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

void main() {
    uint source = pass.ids.x;
    ivec2 last = ivec2(ImgSize(source)) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy) * 2;
    vec4 far = vec4(0.0);
    vec4 near = vec4(0.0);
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            vec4 t = ImgFetch(source, min(p + ivec2(i, j), last));
            float coc = 2.0 * t.a - 1.0;
            vec4 light = vec4(PostLight(t.rgb), 1.0);
            far += light * max(coc, 0.0);
            near += light * max(-coc, 0.0);
        }
    }
    out_far = 0.25 * far;
    out_near = 0.25 * near;
}
