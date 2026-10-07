#version 460

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D grain_noise;

layout(push_constant) uniform CompositeParams {
    float exposure;
    float brightness;
    float mode;
    float pad;
    vec4 fade;
    vec4 grain;
    vec4 grain_offset;
} params;

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;
    if (params.mode > 0.5) {
        if (params.grain.x > 0.0) {
            float y = abs(dot(color, vec3(0.299, 0.587, 0.114)));
            float strength = params.grain.z;
            float alpha;
            if (params.grain.y > 0.0) {
                alpha = clamp(pow(y, 0.08) * (1.0 - pow(y, 0.12)) * strength * 0.45, 0.0, 1.0);
            } else {
                float p = pow(y, 0.7);
                alpha = clamp(p * (p * -strength * 0.45 + strength * 0.45), 0.0, 1.0);
            }
            float across = params.grain_offset.z > 0.0 ? params.grain_offset.z : 1.0;
            vec2 grain_uv = vec2((in_uv.x - 0.5) * across + 0.5, in_uv.y);
            vec3 noise = clamp(textureLod(grain_noise, 3.0 * (params.grain_offset.xy + grain_uv), log2(1536.0 / 1080.0)).rgb, 0.0, 1.0);
            color = mix(color, noise, alpha);
        }
        out_color = vec4(pow(clamp(color, 0.0, 1.0), vec3(1.0 / params.brightness)), 1.0);
        return;
    }
    out_color = vec4(mix(color, params.fade.rgb, params.fade.a), 1.0);
}
