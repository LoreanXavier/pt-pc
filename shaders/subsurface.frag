#version 460
#include "common.glsl"

// The SUBSURFACE_SCATTER plugin (GrPluginSubSurfaceScatter, execute 0xDDBEA0) and its pixel shader SubSurfaceScattering
// (GrSystemShaders, hash f7436336fcf5793e). The plugin copies the diffuse light buffer (CopyBuffer), blurs the copy
// horizontally into a temporary target (m_localParam[0].x = 0), then blurs that vertically back into the light buffer
// and mixes it with the copy by the view angle (m_localParam[0].x = 1). Only pixels whose material index (G-buffer target 2.z,
// the shader's inSkinMaskBuffer) is 10 change. m_localParam[1] = (1 / tan(aspect fovy / 2), 1 / tan(fovy / 2), width, height)
// (0xDDBEA0, register 0xA5). pass.ids: x view, y mode (1 horizontal, 2 vertical and mix). The copy is a transfer
// (SubsurfacePass::Record); set 2 binding 0 is inLightBufferOriginal (the copy), binding 1 inLightBuffer (the copy in the
// first pass, the temporary target in the second).

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(set = 2, binding = 0) uniform sampler2D sss_original;
layout(set = 2, binding = 1) uniform sampler2D sss_light;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

const float kWeightR[7] = float[7](1.0, 0.986717, 0.947919, 0.886615, 0.807391, 0.715847, 0.617933);
const float kWeightG[7] = float[7](1.0, 0.964554, 0.865578, 0.722668, 0.561334, 0.405661, 0.272743);
const float kWeightB[7] = float[7](1.0, 0.943217, 0.791492, 0.590886, 0.392446, 0.231894, 0.121903);

vec3 LightAt(vec2 uv) {
    return texture(sss_light, uv).rgb;
}

bool Skin(vec2 uv) {
    return int(255.0 * Img(IMG_MATERIAL, SMP_POINT_CLAMP, uv).z) == 10;
}

void main() {
    View v = frame.views[pass.ids.x];
    vec2 size = vec2(textureSize(sss_light, 0));
    vec2 uv = gl_FragCoord.xy / size;
    vec3 light = LightAt(uv);
    if (!Skin(uv)) {
        out_color = vec4(light, 1.0);
        return;
    }
    bool horizontal = pass.ids.y == 1u;
    vec3 n = normalize(DecodeNormal(Img(IMG_NORMAL, SMP_POINT_CLAMP, uv).xyz));
    float depth = Img(IMG_DEPTH, SMP_POINT_CLAMP, uv).x;
    vec3 p = ViewPosition(v, PixelNdc(v, gl_FragCoord.xy), depth);
    float z = p.z;
    vec3 eye = -normalize(p);
    // kernel width: sqrt(0.000121 / (|V.z|^3 (|P|^2 + 1))) times the cosine between the normal and the view vector projected on the
    // blur's plane (xz for the horizontal pass, yz for the vertical one) times m_localParam[1].x or .y
    float base = sqrt(max(0.0, 0.000121 / (abs(eye.z) * eye.z * eye.z * (dot(p, p) + 1.0))));
    float cosine;
    float focal;
    float tan_y = v.projection_param.y;
    if (horizontal) {
        cosine = dot(normalize(n.xz), normalize(eye.xz));
        focal = 1.0 / tan(size.x / size.y * atan(tan_y));
    } else {
        cosine = dot(normalize(n.yz), normalize(eye.yz));
        focal = 1.0 / tan_y;
    }
    float half_width = abs(focal * abs(cosine) * base) * 0.5;
    vec3 sum = vec3(0.0);
    vec3 weight = vec3(0.0);
    for (int k = -6; k <= 6; ++k) {
        float offset = float(k) / 6.0 * half_width;
        vec2 tap = uv + (horizontal ? vec2(offset, 0.0) : vec2(0.0, offset));
        if (!Skin(tap)) {
            continue;
        }
        float dz = ViewZ(v, Img(IMG_DEPTH, SMP_POINT_CLAMP, tap).x) - z;
        float g = -dz * dz;
        vec3 w = vec3(kWeightR[abs(k)] * exp(217391.3 * g), kWeightG[abs(k)] * exp(357142.84 * g), kWeightB[abs(k)] * exp(454545.47 * g));
        sum += w * LightAt(tap);
        weight += w;
    }
    vec3 blurred = vec3(weight.x > 0.0 ? sum.x / weight.x : 0.0, weight.y > 0.0 ? sum.y / weight.y : 0.0,
                        weight.z > 0.0 ? sum.z / weight.z : 0.0);
    if (horizontal) {
        out_color = vec4(blurred, 1.0);
        return;
    }
    // the second pass mixes with the light before the blur by 1 - k (2 + 1 / N.V), k = 0.15, 0.18, 0.2
    vec3 original = texture(sss_original, uv).rgb;
    float t = 2.0 + 1.0 / dot(n, eye);
    vec3 mixw = clamp(1.0 - t * vec3(0.15, 0.18, 0.2), 0.0, 1.0);
    out_color = vec4(original + mixw * (blurred - original), 1.0);
}
