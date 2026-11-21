#version 460

#include "composite_output.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_layer;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D grain_noise;
layout(set = 0, binding = 2) uniform sampler2D ui_top;
layout(set = 0, binding = 3) uniform sampler2D hudless;

layout(push_constant) uniform CompositeParams {
    float exposure;
    float brightness;
    float mode;
    float pad;
    vec4 fade;
    vec4 grain;
    vec4 grain_offset;
    vec4 hdr;
    vec4 grade;
} params;

#include "film_grain.glsl"

void main() {
    vec4 sampled = texture(scene_color, in_uv);
    vec3 color = sampled.rgb;
    vec4 top = params.hdr.w > 0.5 ? texture(ui_top, in_uv) : vec4(0.0);
    if (params.grain.x > 0.0) {
        color = FilmGrain(color, in_uv, gl_FragCoord.xy, params.grain, params.grain_offset, params.grain_offset.w < 0.5);
    }
    color = color * (1.0 - top.a) + top.rgb;
    vec3 shown = CompositeDisplayColor(color, params.brightness, params.grain_offset.w, params.hdr.xy, params.grade.x);
    vec3 base = texelFetch(hudless, ivec2(gl_FragCoord.xy), 0).rgb;
    float tolerance = params.grain_offset.w > 1.5 ? 1.5 / 1023.0 : 1.5 / 255.0;
    out_layer = CompositeUiLayer(shown, base, tolerance);
}
