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

float BloomAlpha(vec3 color) {
    float r = Luma709(color) / 32.0;
    return r * inversesqrt(max(1.0 / 512.0, r));
}

void TppFog(vec3 eye, vec3 point, out vec3 inscatter, out float transmittance) {
    vec4 g0 = frame.fog[0];
    vec4 g1 = frame.fog[1];
    vec4 mie = frame.fog[2];
    vec4 ray = frame.fog[3];
    vec3 d = point - eye;
    vec3 dh = 0.01 * d;
    float dist = length(dh);
    float cos_theta = dot(frame.fog[7].xyz, dh / max(0.0001, dist));
    float height = max(1.0e-6, 100.0 * g0.y * dh.y);
    float amount = g0.x * pow(max(0.0, dist - g0.z * 0.01), g1.w);
    float depth = (amount * 100.0 - 100.0 * amount * exp(-height)) / height;
    float rayleigh_phase = 0.059683103 * cos_theta * cos_theta + 0.059683103;
    float m = 1.0 - cos_theta * mie.w;
    float mie_phase = 0.07957747 * (1.0 - mie.w * mie.w) / (m * m);
    vec3 sigma = mie.rgb + ray.rgb;
    vec3 directional = ray.w * (mie.rgb * mie_phase + ray.rgb * rayleigh_phase) / max(vec3(1.0e-6), sigma);
    float opacity = 1.0 - exp(-depth);
    vec3 color = g1.rgb + directional * (1.0 - exp(-depth * sigma));
    // No area box term: VolFog_TppVolFog_Area (ps adf9aadf6beb2a36) runs its box code only for the froxels whose ray misses the
    // box (0x048c to 0x0498: exec = all & ~hit), where the segment in the box is empty, so the volume holds the global fog alone
    // (ending_fog_spot 3560: every texel within the global fog's own quantization, rgb within 0.0027)
    inscatter = clamp(color * opacity, 0.0, 1.0);
    transmittance = 1.0 - opacity;
}

vec3 TppShoulder(vec3 x) {
    float a = pass.f0.x;
    float t = pass.f0.y;
    float c = pass.f0.z;
    vec3 curve = t + c - 1.0 / (a * (max(x, vec3(t)) - t + c));
    return mix(curve, x, vec3(lessThanEqual(x, vec3(t))));
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    uint tpp = pass.ids.z;
    View v = frame.views[pass.ids.x];
    vec3 color = vec3(0.0);
    float z = 1.0e9;
    if (depth <= 0.0) {
        if (tpp == 0u) {
            out_color = vec4(0.0);
            return;
        }
    } else {
        vec3 albedo = SrgbDecode(ImgFetch(IMG_ALBEDO, pixel).rgb);
        vec3 diffuse = ImgFetch(IMG_DIFFUSE, pixel).rgb;
        vec3 specular = ImgFetch(IMG_SPECULAR, pixel).rgb;
        float occlusion = pass.ids.y != 0u ? ImgFetch(IMG_AO_BLUR, pixel).x : 1.0;
        color = (diffuse * albedo + specular) * occlusion;
        z = ViewZ(v, depth);
    }
    if ((tpp & 1u) != 0u && depth <= 0.0 && (tpp & 4u) != 0u) {
        // The sky (Sky_Draw_TppBaked, ps f1bec4c4b2912602, drawn after the composite where the depth is still clear; rendering.md
        // 12.7): with P.T.'s constants (ending_sky_trace: m2.z = 0 no moon, m3.w = 0 no scattering, m7.x = 0 no stars,
        // m5.w = 1 and m6 = (1000, -986895) for the fog weight 1) it is the fog volume's far slice, averaged over the pixel and
        // two points 0.3 NDC away along the diagonal (weights 1/2, 1/4, 1/4), plus the alpha mode 4 Bayer value
        // (2 b + 1) / 255 / 128 of its inMesh texture before the shoulder (the 3560 and 4080 target dumps: 92 % and 89 % of
        // the sky pixels exact). tpp3dfw_constant_sky_ed's dome on top keeps the target within 1/32768 (cloudCover and the
        // cylinder density are 0), so it is left out.
        vec2 ndc = PixelNdc(v, gl_FragCoord.xy);
        vec2 taps[3] = vec2[3](ndc, clamp(ndc + vec2(0.3, -0.3), -1.0, 1.0), clamp(ndc - vec2(0.3, -0.3), -1.0, 1.0));
        float weights[3] = float[3](0.5, 0.25, 0.25);
        float fog_z = frame.fog[0].w;
        for (int i = 0; i < 3; ++i) {
            vec3 world_point = (v.inv_view * vec4(taps[i] * v.projection_param.xy * fog_z, fog_z, 1.0)).xyz;
            vec3 inscatter;
            float transmittance;
            TppFog(v.eye.xyz, world_point, inscatter, transmittance);
            color += weights[i] * inscatter;
        }
        color += MeshDither(gl_FragCoord.xy, v.temporal).y / 128.0;
    } else if ((tpp & 1u) != 0u) {
        // DR_VolFog_TppTonemap reads the froxel volume at the clamped slice of the pixel's depth: the fog of view depth
        // clamp(z, 1, far) (SceneRenderer::SetTppFog)
        float fog_z = clamp(z, 1.0, frame.fog[0].w);
        vec3 view_point = vec3(PixelNdc(v, gl_FragCoord.xy) * v.projection_param.xy * fog_z, fog_z);
        vec3 world_point = (v.inv_view * vec4(view_point, 1.0)).xyz;
        vec3 inscatter;
        float transmittance;
        TppFog(v.eye.xyz, world_point, inscatter, transmittance);
        color = color * transmittance + inscatter;
    }
    float bloom_alpha = BloomAlpha(color);
    if ((tpp & 2u) != 0u) {
        color = TppShoulder(color);
    }
    out_color = vec4(color, bloom_alpha);
}
