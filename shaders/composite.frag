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

float PqEncode(float nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;
    if (params.mode > 0.5) {
        if (params.grain.x > 0.0) {
            // Draw2D_ShFilmGrain, Draw2D layer 200 (0x9398E0)
            float y = abs(dot(color, vec3(0.299, 0.587, 0.114)));
            float strength = params.grain.z;
            float alpha;
            if (params.grain.y > 0.0) {
                alpha = clamp(pow(y, 0.08) * (1.0 - pow(y, 0.12)) * strength * 0.45, 0.0, 1.0);
            } else {
                float p = pow(y, 0.7);
                alpha = clamp(p * (p * -strength * 0.45 + strength * 0.45), 0.0, 1.0);
            }
            // the noise texture has 8 mip levels and Draw2D_ShFilmGrain filters between them (f010_grain_ops 1705, op 1181); 3 tiles of
            // 512 texels down the 1080 rows of the PS4's frame put its level at log2(1536 / 1080), which the port keeps at any size.
            // The tiles span the original's 16:9 frame: a wider window (grain_offset.z, its width in 16:9 frames) shows more of
            // them across instead of stretching them
            float across = params.grain_offset.z > 0.0 ? params.grain_offset.z : 1.0;
            vec2 grain_uv = vec2((in_uv.x - 0.5) * across + 0.5, in_uv.y);
            vec3 noise = clamp(textureLod(grain_noise, 3.0 * (params.grain_offset.xy + grain_uv), log2(1536.0 / 1080.0)).rgb, 0.0, 1.0);
            color = mix(color, noise, alpha);
        }
        if (params.grain_offset.w > 0.5) {
            color = pow(max(color, vec3(0.0)), vec3(1.0 / params.brightness));
        } else {
            color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / params.brightness));
        }
        if (params.grain_offset.w > 0.5) {
            if (params.grain_offset.w < 1.5) {
                out_color = vec4(color * (203.0 / 80.0), 1.0);
            } else {
                vec3 bt2020 = mat3(0.627404, 0.069097, 0.0163916,
                                   0.329283, 0.919540, 0.0880132,
                                   0.0433136, 0.0113612, 0.895595) * (color * 203.0);
                out_color = vec4(vec3(PqEncode(bt2020.r), PqEncode(bt2020.g), PqEncode(bt2020.b)), 1.0);
            }
        } else {
            out_color = vec4(color, 1.0);
        }
        return;
    }
    if (params.pad > 0.5 && params.pad < 1.5) {
        color = vec3(dot(color, vec3(0.2126,0.7152,0.0722)));
    } else if (params.pad < 2.5 && params.pad > 1.5) {
        color *= vec3(1.06,1.0,0.92);
    } else if (params.pad < 3.5 && params.pad > 2.5) {
        color *= vec3(0.92,1.0,1.06);
    } else if (params.pad > 3.5) {
        color = mix(vec3(dot(color,vec3(0.2126,0.7152,0.0722))),color,0.35);
    }
    out_color = vec4(mix(color, params.fade.rgb, params.fade.a), 1.0);
}
