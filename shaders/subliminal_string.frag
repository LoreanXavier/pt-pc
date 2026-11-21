#version 460
#include "common.glsl"
#include "tonemap_curve.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float across = pass.f0.w > 0.0 ? pass.f0.w : 1.0;
    vec2 uv = vec2((in_uv.x - 0.5) * across + 0.5, in_uv.y);
    float a = texture(PT_TEX2D(pass.ids.x & 0x3FFFFFFFu), pass.f0.xy + uv).a;
    float ink = (pass.f2.x > 0.0 ? pass.f2.x : 0.1) * clamp(pass.f0.z * a, 0.0, 1.0);

    vec3 hdr = ImgFetch(IMG_HDR, pixel).rgb * pass.f1.y;
    vec2 bloom_uv = in_uv + 0.5 / ImgSize(IMG_BLOOM_SUM);
    vec3 bloom = pass.f1.x > 0.0 ? Img(IMG_BLOOM_SUM, SMP_LINEAR_CLAMP, bloom_uv).rgb * pass.f1.x : vec3(0.0);
    bloom = HdrOutput() ? max(bloom, vec3(0.0)) : min(vec3(0.5), bloom);
    vec3 c;
    if (HdrOutput()) {
        vec3 encoded = SrgbEncode(clamp(hdr, vec3(0.0), vec3(6.0e4))) + max(ImgFetch(IMG_FLARE, pixel).rgb, vec3(0.0));
        vec3 ink_contribution = sqrt(max((encoded + ink) * (encoded + ink) + bloom, vec3(0.0))) - sqrt(max(encoded * encoded + bloom, vec3(0.0)));
        c = max(ImgFetch(pass.ids.y, pixel).rgb + ink_contribution, vec3(0.0));
    } else {
        vec3 encoded = SrgbEncode(clamp(hdr, 0.0, 1.0)) + max(ImgFetch(IMG_FLARE, pixel).rgb, vec3(0.0));
        vec3 ink_contribution = ScreenTonemap(min(encoded + ink, vec3(1.0)), bloom)
                             - ScreenTonemap(min(encoded, vec3(1.0)), bloom);
        c = clamp(ImgFetch(pass.ids.y, pixel).rgb + ink_contribution, 0.0, 1.0);
    }
    out_color = vec4(c, Luma601(c));
}
