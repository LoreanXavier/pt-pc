#version 460
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

const uint kCubeIndices[36] = uint[](0, 4, 6, 0, 6, 2, 5, 1, 3, 5, 3, 7, 0, 1, 5, 0, 5, 4, 6, 7, 3, 6, 3, 2, 1, 0, 2, 1, 2, 3, 4, 5, 7, 4, 7, 6);

void main() {
    uint corner = kCubeIndices[gl_VertexIndex];
    vec3 local = vec3((corner & 1u) != 0u ? 1.0 : -1.0, (corner & 2u) != 0u ? 1.0 : -1.0, (corner & 4u) != 0u ? 1.0 : -1.0);
    View v = frame.views[pass.ids.x];
    gl_Position = v.view_projection * (pass.m * vec4(local, 1.0));
}
