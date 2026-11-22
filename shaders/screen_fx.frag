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

float LensChannel(uint source, vec2 uv, vec2 size, int channel) {
    vec2 p = uv * size - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    ivec2 last = ivec2(size) - 1;
    float sum = 0.0;
    float low = 1.0e30;
    float high = 0.0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            ivec2 at = clamp(ivec2(base) + ivec2(i - 1, j - 1), ivec2(0), last);
            float v = max(ImgFetch(source, at)[channel], 0.0);
            float light = v * v;
            sum += light * w[i].x * w[j].y;
            if ((i == 1 || i == 2) && (j == 1 || j == 2)) {
                low = min(low, light);
                high = max(high, light);
            }
        }
    }
    return sqrt(clamp(sum, low, high));
}

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
        if (HdrOutput()) {
            color.r = LensChannel(source, clamp(base + offset * 1.42, 0.0, 1.0), size, 0);
            color.g = LensChannel(source, clamp(base + offset * 1.275, 0.0, 1.0), size, 1);
            color.b = LensChannel(source, clamp(base + offset * 1.12, 0.0, 1.0), size, 2);
            return color;
        }
        color.r = Img(source, SMP_LINEAR_CLAMP, clamp(base + offset * 1.42, 0.0, 1.0)).r;
        color.g = Img(source, SMP_LINEAR_CLAMP, clamp(base + offset * 1.275, 0.0, 1.0)).g;
        color.b = Img(source, SMP_LINEAR_CLAMP, clamp(base + offset * 1.12, 0.0, 1.0)).b;
    } else {
        color = ImgFetch(source, clamp(ivec2(uv*ImgSize(source)), ivec2(0), ivec2(ImgSize(source))-1)).rgb;
    }
    return color;
}

vec3 HdrSharpen(uint source, ivec2 p, float amount) {
    ivec2 last = ivec2(ImgSize(source)) - 1;
    vec3 e = ImgFetch(source, p).rgb;
    float range = 1.0 / max(frame.display.y, 1.0);
    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    float le = dot(PostLight(e), luma) * range;
    float lb = dot(PostLight(ImgFetch(source, clamp(p + ivec2(0, -1), ivec2(0), last)).rgb), luma) * range;
    float ld = dot(PostLight(ImgFetch(source, clamp(p + ivec2(-1, 0), ivec2(0), last)).rgb), luma) * range;
    float lf = dot(PostLight(ImgFetch(source, clamp(p + ivec2(1, 0), ivec2(0), last)).rgb), luma) * range;
    float lh = dot(PostLight(ImgFetch(source, clamp(p + ivec2(0, 1), ivec2(0), last)).rgb), luma) * range;
    float low = min(min(lb, ld), min(lf, lh));
    float high = max(max(lb, ld), max(lf, lh));
    float hit_low = low / max(4.0 * high, 1.0e-6);
    float hit_high = (1.0 - high) / min(4.0 * low - 4.0, -1.0e-6);
    float lobe = clamp(max(-hit_low, hit_high), -0.1875, 0.0) * amount;
    float spread = max(high, le) - min(low, le);
    float alone = spread > 1.0e-6 ? Saturate(abs(0.25 * (lb + ld + lf + lh) - le) / spread) : 0.0;
    lobe *= 1.0 - 0.5 * alone;
    float sharpened = (lobe * (lb + ld + lf + lh) + le) / (4.0 * lobe + 1.0);
    float gain = le > 1.0e-7 ? max(sharpened, 0.0) / le : 1.0;
    float strongest = max(e.r, max(e.g, e.b));
    float room = strongest > 0.0 ? max(frame.display.y, 1.0) / pow(strongest, 2.2) : 1.0;
    return e * pow(min(gain, max(room, 1.0)), 1.0 / 2.2);
}

void main() {
    uint source=pass.ids.x;
    if(pass.f1.z>0.0) {
        out_color=vec4(HdrSharpen(source,ivec2(gl_FragCoord.xy),pass.f1.x),1.0);
        return;
    }
    vec3 color=SampleScreen(source,in_uv);
    if(pass.f1.x>0.0) {
        vec2 texel=1.0/ImgSize(source);
        vec3 a=SampleScreen(source,in_uv+vec2(-texel.x,0));
        vec3 b=SampleScreen(source,in_uv+vec2(texel.x,0));
        vec3 c=SampleScreen(source,in_uv+vec2(0,-texel.y));
        vec3 d=SampleScreen(source,in_uv+vec2(0,texel.y));
        vec3 lo=min(color,min(min(a,b),min(c,d))), mx=max(color,max(max(a,b),max(c,d)));
        vec3 sharpened=color+(color-(a+b+c+d)*.25)*pass.f1.x;
        vec3 ceiling=HdrOutput()?mx+(mx-lo)*.1:min(vec3(1),mx+(mx-lo)*.1);
        color=clamp(sharpened,max(vec3(0),lo-(mx-lo)*.1),ceiling);
    }
    if(pass.f1.y<=0.0||HdrOutput()) {
        out_color=vec4(color,1.0);
        return;
    }
    vec3 seed = fract(vec3(gl_FragCoord.xyx) * vec3(0.1031, 0.1030, 0.0973));
    seed += dot(seed, seed.yxz + 33.33);
    float noise = fract((seed.x + seed.y) * seed.z) - 0.5;
    out_color = vec4(clamp(color + noise / 255.0, 0.0, 1.0), 1.0);
}
