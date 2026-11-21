#version 460

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D grain_noise;
layout(set = 0, binding = 2) uniform sampler2D ui_top;

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
    vec4 look0;
    vec4 look1;
} params;

#include "composite_output.glsl"
#include "film_grain.glsl"
#include "hdr_grade.glsl"

const float kStripTop = 0.76;

bool CalibrationPattern(vec2 uv, float mask, float kind, float paper, float peak, float gamma, out vec3 nits) {
    if (kind < 0.5 || uv.y > kStripTop) {
        return false;
    }
    nits = vec3(0.0);
    if (kind < 1.5) {
        vec2 offset = abs(uv - vec2(0.5, kStripTop * 0.5));
        if (offset.x < 0.12 && offset.y < 0.22) {
            nits = vec3(mix(10000.0, peak, mask));
        }
        return true;
    }
    if (kind < 2.5 && uv.y > 0.61 && uv.y < 0.64 && abs(uv.x - 0.5) < 0.2) {
        float t = (uv.x - 0.3) / 0.4;
        float code = mix(PqEncode(paper), PqEncode(peak), t);
        float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
        float e = pow(code, 1.0 / m2);
        float level = 10000.0 * pow(max(e - c1, 0.0) / (c2 - c3 * e), 1.0 / m1);
        float doubling = log2(max(level / paper, 1.0));
        bool tick = doubling > 0.5 && abs(fract(doubling + 0.5) - 0.5) < 0.012;
        nits = vec3(tick ? 0.0 : level);
        return true;
    }
    float level = kind > 3.5 ? (uv.x < 0.5 ? 0.09 : 0.01) : 1.0;
    nits = vec3(paper * GammaDecode(level * mask, gamma));
    return true;
}

vec3 HdrExtendedColours(vec3 light) {
    float high = max(light.r, max(light.g, light.b));
    float low = min(light.r, min(light.g, light.b));
    float chroma = high > 1.0e-6 ? clamp((high - low) / high, 0.0, 1.0) : 0.0;
    const mat3 kP3To709 = mat3(1.2249401, -0.0420569, -0.0196376,
                               -0.2249404, 1.0420571, -0.0786361,
                               0.0, 0.0, 1.0982735);
    return mix(light, kP3To709 * light, chroma * chroma);
}

vec3 HdrPicture(vec3 color) {
    vec3 light = sign(color) * pow(abs(color), vec3(2.2));
    if (params.grade.z > 0.5) {
        float paper = params.hdr.x * pow(max(params.hdr.z, 1.0e-3), 2.2);
        light = HdrGrade(light, params.look0, params.look1, params.hdr.y / max(paper, 1.0));
    }
    if (params.grade.y > 1.5) {
        light = HdrExtendedColours(light);
    }
    light = HdrGamut(light, params.grade.y);
    return sign(light) * pow(abs(light), vec3(1.0 / 2.2));
}

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;
    if (params.mode > 0.5) {
        vec4 top = params.hdr.w > 0.5 ? texture(ui_top, in_uv) : vec4(0.0);
        if (params.grain_offset.w > 0.5) {
            float ui = 1.0 - clamp(texture(scene_color, in_uv).a, 0.0, 1.0);
            color = mix(color, min(color, vec3(1.0)), ui);
            top.rgb = min(top.rgb, vec3(1.0));
        }
        vec3 drawn = color * (1.0 - top.a) + top.rgb;
        if (params.grain.x > 0.0) {
            color = FilmGrain(color, in_uv, gl_FragCoord.xy, params.grain, params.grain_offset, params.grain_offset.w < 0.5);
        }
        color = color * (1.0 - top.a) + top.rgb;
        out_color = vec4(CompositeDisplayColor(color, params.brightness, params.grain_offset.w, params.hdr.xy, params.grade.x), 1.0);
        vec3 nits;
        if (params.grain_offset.w > 0.5 &&
            CalibrationPattern(in_uv, clamp(max(drawn.r, max(drawn.g, drawn.b)), 0.0, 1.0), params.exposure,
                               params.hdr.x * pow(max(params.hdr.z, 0.0), 2.2), params.hdr.y, params.grade.x, nits)) {
            out_color = vec4(OutputNits(nits, params.grain_offset.w), 1.0);
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
    if (params.grain_offset.w > 0.5 && params.grade.w > 0.5) {
        color = HdrPicture(color);
    }
    color *= params.hdr.z > 0.0 ? params.hdr.z : 1.0;
    out_color = vec4(mix(color, params.fade.rgb, params.fade.a), 1.0);
}
