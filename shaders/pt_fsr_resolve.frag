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
layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec4 out_specular;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    View v = frame.views[pass.ids.x];
    vec3 diffuse = imageLoad(rt_gi_ping, p).rgb + imageLoad(rt_gi_pong, p).rgb;
    vec3 specular = imageLoad(rt_glossy_ping, p).rgb + imageLoad(rt_glossy_pong, p).rgb;
    float depth = ImgFetch(IMG_DEPTH, p).x;
    if (depth <= 0.0) {
        out_diffuse = out_specular = vec4(0.0);
        return;
    }
    vec4 g = ImgFetch(IMG_MATERIAL, p);
    vec4 mat0 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g.z, 0.25));
    vec4 mat1 = Img(RES_MATERIAL, SMP_POINT_CLAMP, vec2(g.z, 0.75));
    vec3 c = ViewPosition(v, PixelNdc(v, gl_FragCoord.xy), depth);
    vec3 n = DecodeNormal(ImgFetch(IMG_NORMAL, p).xyz);
    float code = imageLoad(pt_layer_raw, p).a;
    float kind = floor(code * 0.5);
    float amount = kind >= 1.0 ? Saturate(code - 2.0 * kind) : 0.0;
    out_diffuse = vec4(diffuse, 0.0);
    out_specular = vec4(specular * FsrSpecularAlbedo(v, p, c, n, g, mat0, mat1, amount), 0.0);
}
