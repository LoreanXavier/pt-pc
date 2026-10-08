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

vec3 SampleScreen(uint source, vec2 uv) {
    vec3 color;
    if (pass.f0.x > 0.0) {
        vec2 size = ImgSize(source);
        float across = (size.x / size.y) / (16.0 / 9.0);
        vec2 texel = vec2(1.0 / (1080.0 * size.x / size.y), 1.0 / 1080.0);
        vec2 p = (uv * 2.0 - 1.0) * vec2(across, 1.0);
        float r3 = Saturate(length(p) * 0.52240777);
        float r5 = 1.0 - sin(1.5707964 * r3);
        float r6 = inversesqrt(dot(p, p) + r5 * r5);
        float r7 = pow(r3, 1.75);
        vec2 offset = r7 * 84.0 * texel * p * r6;
        vec2 base = uv * 0.9 + 0.050000012;
        color.r = Img(source, SMP_LINEAR_CLAMP, clamp(base + offset * 1.42, 0.0, 1.0)).r;
        color.g = Img(source, SMP_LINEAR_CLAMP, clamp(base + offset * 1.275, 0.0, 1.0)).g;
        color.b = Img(source, SMP_LINEAR_CLAMP, clamp(base + offset * 1.12, 0.0, 1.0)).b;
    } else {
        color = ImgFetch(source, clamp(ivec2(uv*ImgSize(source)), ivec2(0), ivec2(ImgSize(source))-1)).rgb;
    }
    return color;
}

void main() {
    uint source=pass.ids.x;
    vec3 color=SampleScreen(source,in_uv);
    if(pass.f1.x>0.0) {
        vec2 texel=1.0/ImgSize(source);
        vec3 a=SampleScreen(source,in_uv+vec2(-texel.x,0));
        vec3 b=SampleScreen(source,in_uv+vec2(texel.x,0));
        vec3 c=SampleScreen(source,in_uv+vec2(0,-texel.y));
        vec3 d=SampleScreen(source,in_uv+vec2(0,texel.y));
        vec3 lo=min(color,min(min(a,b),min(c,d))), mx=max(color,max(max(a,b),max(c,d)));
        vec3 sharpened=color+(color-(a+b+c+d)*.25)*pass.f1.x;
        if (pass.f1.y > 0.5) {
            color=max(sharpened,max(vec3(0),lo-(mx-lo)*.1));
        } else {
            color=clamp(sharpened,max(vec3(0),lo-(mx-lo)*.1),min(vec3(1),mx+(mx-lo)*.1));
        }
    }
    if (pass.f1.y > 0.5) {
        out_color = vec4(SrgbDecode(max(color, vec3(0.0))), 1.0);
        return;
    }
    vec3 seed = fract(vec3(gl_FragCoord.xyx) * vec3(0.1031, 0.1030, 0.0973));
    seed += dot(seed, seed.yxz + 33.33);
    float noise = fract((seed.x + seed.y) * seed.z) - 0.5;
    out_color = vec4(clamp(color + noise / 255.0, 0.0, 1.0), 1.0);
}
