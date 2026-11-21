#version 460
#include "common.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) out vec4 out_diffuse_albedo;
layout(location = 1) out vec4 out_specular_albedo;
layout(location = 2) out vec4 out_normal_roughness;
layout(location = 3) out vec4 out_hit_distance;

#define RT_FULL_IMAGES_READONLY 1
#include "rt_full_images.glsl"
#define PT_IMAGES_READONLY 1
#include "pt_images.glsl"
#include "pt_fsr_albedo.glsl"

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = ImgFetch(IMG_DEPTH, pixel).x;
    if (depth <= 0.0) {
        out_diffuse_albedo = vec4(0.0);
        out_specular_albedo = vec4(0.0);
        out_normal_roughness = vec4(0.0, 0.0, 0.0, 1.0);
        out_hit_distance = vec4(0.0);
        return;
    }
    View v = frame.views[pass.ids.x];
    vec3 c = ViewPosition(v, PixelNdc(v, vec2(pixel) + 0.5), depth);
    vec3 n = DecodeNormal(ImgFetch(IMG_NORMAL, pixel).xyz);
    vec3 V = normalize(-c);
    vec4 g = ImgFetch(IMG_MATERIAL, pixel);
    vec4 mat0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g.z, 0.25));
    vec4 mat1 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g.z, 0.75));
    float F = Img(RES_LUT1, SMP_LINEAR_CLAMP, vec2(Saturate(dot(n, V)), mat1.x)).x;
    vec3 specular = max(g.y * mat0.rgb * F, vec3(0.0));
    vec3 albedo = SrgbDecode(ImgFetch(IMG_ALBEDO, pixel).rgb);
    float code = imageLoad(pt_layer_raw, pixel).a;
    float kind = floor(code * 0.5);
    float amount = kind >= 1.0 ? Saturate(code - 2.0 * kind) : 0.0;
    out_diffuse_albedo = vec4(albedo * (1.0 - amount), 1.0);
    out_specular_albedo = vec4(specular * (1.0 - amount) + amount, 1.0);
    if (pass.ids.y == 0u) {
        out_diffuse_albedo = vec4(albedo, 1.0);
        out_specular_albedo = vec4(FsrSpecularAlbedo(v, pixel, c, n, g, mat0, mat1, amount), 1.0);
    }
    vec3 world_n = normalize(mat3(v.inv_view) * n);
    float roughness = Saturate(max(g.x, mat1.w));
    if (pass.ids.y != 0u && kind == 1.0 && amount > 0.0) {
        vec3 coat_n = imageLoad(pt_layer_geometry, pixel).xyz;
        float coat_share = amount / max(Luma709(specular * (1.0 - amount)) + amount, 1.0e-6);
        world_n = normalize(mix(world_n, coat_n, coat_share));
        roughness *= 1.0 - coat_share;
    }
    out_normal_roughness = vec4(world_n, roughness);
    out_hit_distance = vec4(max(imageLoad(rt_glossy_raw, pixel).a, 0.0), 0.0, 0.0, 0.0);
}
