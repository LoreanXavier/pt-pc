#version 460

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) flat out uint out_draw;

layout(push_constant) uniform UiPush {
    vec2 inverse_extent;
    uint draw;
} push;

void main() {
    out_uv = in_uv;
    out_color = in_color;
    out_draw = push.draw;
    gl_Position = vec4(in_position * push.inverse_extent * 2.0 - 1.0, 0.0, 1.0);
}
