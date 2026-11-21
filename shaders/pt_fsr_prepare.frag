#version 460
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

#define RT_FULL_IMAGES_READONLY 1
#include "rt_full_images.glsl"
#define PT_IMAGES_READONLY 1
#include "pt_images.glsl"

#include "pt_fsr_albedo.glsl"

layout(location = 0) out vec4 out_direct_diffuse;
layout(location = 1) out vec4 out_direct_specular;
layout(location = 2) out vec4 out_indirect_diffuse;
layout(location = 3) out vec4 out_indirect_specular;
layout(location = 4) out float out_depth;
layout(location = 5) out vec4 out_normal;

vec2 OctNormal(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 xy = n.z < 0.0 ? (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0) : n.xy;
    return xy * 0.5 + 0.5;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    View v = frame.views[pass.ids.x];
    float depth = ImgFetch(IMG_DEPTH, p).x;
    out_direct_diffuse = out_direct_specular = out_indirect_diffuse = out_indirect_specular = vec4(0.0);
    out_depth = -65504.0;
    out_normal = vec4(0.5, 0.5, 1.0, 0.0);
    if (depth <= 0.0) return;
    vec2 ndc = PixelNdc(v, gl_FragCoord.xy);
    vec3 c = ViewPosition(v, ndc, depth);
    out_depth = -c.z;
    vec4 g = ImgFetch(IMG_MATERIAL, p);
    vec4 mat0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g.z, 0.25));
    vec4 mat1 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g.z, 0.75));
    vec3 n = DecodeNormal(ImgFetch(IMG_NORMAL, p).xyz);
    vec3 world_n = normalize(mat3(v.inv_view) * n);
    vec3 albedo = SrgbDecode(ImgFetch(IMG_ALBEDO, p).rgb);
    vec4 layer = imageLoad(pt_layer_raw, p);
    float kind = floor(layer.a * 0.5);
    float amount = kind >= 1.0 ? Saturate(layer.a - 2.0 * kind) : 0.0;
    float remaining = 1.0 - amount;
    out_normal = vec4(OctNormal(world_n), kind >= 3.0 ? 0.0 : Saturate(max(g.x, mat1.w)), 0.0);
    float inverse_exposure = 1.0 / max(v.exposure.x, 1.0e-12);
    vec4 diffuse = imageLoad(rt_gi_raw, p);
    vec4 specular = imageLoad(rt_glossy_raw, p);
    vec3 indirect_diffuse = max(diffuse.rgb * remaining, vec3(0.0));
    vec3 reflected = specular.a >= 0.0 ? specular.rgb * g.y * mat0.rgb * remaining : vec3(0.0);
    vec3 indirect_specular = max(reflected + layer.rgb * amount, vec3(0.0));
    if (pass.ids.y == 0u) {
        indirect_diffuse = vec3(0.0);
        indirect_specular = max(layer.rgb * amount, vec3(0.0));
    }
    vec3 diffuse_light = ImgFetch(IMG_DIFFUSE, p).rgb * inverse_exposure;
    vec3 specular_light = ImgFetch(IMG_SPECULAR, p).rgb * inverse_exposure;
    vec3 direct_diffuse = max(diffuse_light - indirect_diffuse, vec3(0.0));
    vec3 direct_specular = max(specular_light - indirect_specular, vec3(0.0));
    vec3 specular_albedo = FsrSpecularAlbedo(v, p, c, n, g, mat0, mat1, amount);
    out_indirect_diffuse = vec4(FsrSignal(indirect_diffuse, v), max(diffuse.a, 0.0));
    out_indirect_specular = vec4(FsrSignal(indirect_specular / specular_albedo, v), max(specular.a, 0.0));
    out_direct_diffuse = vec4(FsrSignal(direct_diffuse, v), 0.0);
    out_direct_specular = vec4(FsrSignal(direct_specular / specular_albedo, v), 0.0);
}
