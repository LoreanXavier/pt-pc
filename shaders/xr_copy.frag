#version 460

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D grain_noise;

layout(push_constant) uniform XrCopyParams {
    vec4 rect;
    vec4 params;
    vec4 grain;
    vec4 grain_offset;
} p;

vec3 Decode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

#include "film_grain.glsl"

void main() {
    vec2 uv = p.rect.xy + in_uv * p.rect.zw;
    vec4 c = texture(source, uv);
    if (p.params.x > 0.5) {
        float a = clamp(c.a, 0.0, 1.0);
        vec3 straight = a > 1.0e-4 ? c.rgb / a : vec3(0.0);
        out_color = vec4(Decode(straight) * a, a);
        return;
    }
    vec3 color = c.rgb;
    if (p.grain.x > 0.0) {
        color = FilmGrain(color, uv, gl_FragCoord.xy, p.grain, p.grain_offset, true);
    }
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / max(p.params.y, 0.1)));
    out_color = vec4(Decode(color), 1.0);
}
