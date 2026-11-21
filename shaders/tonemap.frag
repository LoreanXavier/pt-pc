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

vec3 Lut16(uint lut, vec3 c) {
    float slice = floor(15.0 * c.b);
    float u = c.r * (15.0 / 256.0) + 1.0 / 512.0 + slice / 16.0;
    float v = c.g * (15.0 / 16.0) + 1.0 / 32.0;
    vec3 a = Img(lut, SMP_LINEAR_CLAMP, vec2(u, v)).rgb;
    vec3 b = Img(lut, SMP_LINEAR_CLAMP, vec2(u + 1.0 / 16.0, v)).rgb;
    return mix(a, b, 15.0 * c.b - slice);
}

vec3 QuantizeDither(vec3 c, float salt) {
    vec3 seed = fract((vec3(gl_FragCoord.xyx) + salt) * vec3(0.1031, 0.1030, 0.0973));
    seed += dot(seed, seed.yxz + 33.33);
    return c + (fract((seed.x + seed.y) * seed.z) - 0.5) / 255.0;
}

vec3 ScreenTonemapHdr(vec3 x, vec3 bloom) {
    vec3 r = x * x + bloom;
    return r * inversesqrt(max(vec3(1.0 / 512.0), r));
}

vec3 LutTexel(uint lut, ivec3 i) {
    return ImgFetch(lut, ivec2(i.z * 64 + i.x, i.y)).rgb;
}

vec3 Lut64(uint lut, vec3 c) {
    vec3 p = clamp(c, 0.0, 1.0) * 63.0;
    ivec3 b = ivec3(min(floor(p), vec3(62.0)));
    vec3 f = p - vec3(b);
    ivec3 first;
    ivec3 second;
    vec4 w;
    if (f.x >= f.y) {
        if (f.y >= f.z) {
            first = ivec3(1, 0, 0);
            second = ivec3(1, 1, 0);
            w = vec4(1.0 - f.x, f.x - f.y, f.y - f.z, f.z);
        } else if (f.x >= f.z) {
            first = ivec3(1, 0, 0);
            second = ivec3(1, 0, 1);
            w = vec4(1.0 - f.x, f.x - f.z, f.z - f.y, f.y);
        } else {
            first = ivec3(0, 0, 1);
            second = ivec3(1, 0, 1);
            w = vec4(1.0 - f.z, f.z - f.x, f.x - f.y, f.y);
        }
    } else if (f.z >= f.y) {
        first = ivec3(0, 0, 1);
        second = ivec3(0, 1, 1);
        w = vec4(1.0 - f.z, f.z - f.y, f.y - f.x, f.x);
    } else if (f.z >= f.x) {
        first = ivec3(0, 1, 0);
        second = ivec3(0, 1, 1);
        w = vec4(1.0 - f.y, f.y - f.z, f.z - f.x, f.x);
    } else {
        first = ivec3(0, 1, 0);
        second = ivec3(1, 1, 0);
        w = vec4(1.0 - f.y, f.y - f.x, f.x - f.z, f.z);
    }
    return w.x * LutTexel(lut, b) + w.y * LutTexel(lut, b + first) + w.z * LutTexel(lut, b + second) + w.w * LutTexel(lut, b + 1);
}

vec3 LutHdr(uint lut, vec3 c) {
    float strongest = max(c.r, max(c.g, c.b));
    if (strongest <= 1.0) {
        vec3 g = Lut64(lut, c);
        return g + pass.f2.xyz * (1.0 - g);
    }
    vec3 g = Lut64(lut, c / strongest);
    return (g + pass.f2.xyz * (1.0 - g)) * strongest;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    bool hdr_output = HdrOutput();
    vec3 c;
    vec3 hdr = vec3(0.0);
    if (pass.ids.y == 1u) {
        c = ImgFetch(pass.ids.x, pixel).rgb;
    } else {
        hdr = ImgFetch(IMG_HDR, pixel).rgb * pass.f0.z;
        vec2 bloom_uv = hdr_output ? in_uv : in_uv + 0.5 / ImgSize(IMG_BLOOM_SUM);
        vec3 b = vec3(0.0);
        if (pass.f0.x > 0.0) {
            b = Img(IMG_BLOOM_SUM, SMP_LINEAR_CLAMP, bloom_uv).rgb;
            b = hdr_output ? max(b, vec3(0.0)) * pass.f0.x : min(vec3(0.5), b * pass.f0.x);
        }
        if (hdr_output) {
            hdr = mix(hdr, vec3(0.0), isnan(hdr));
            hdr = clamp(hdr, vec3(0.0), vec3(6.0e4));
            c = ScreenTonemapHdr(SrgbEncode(hdr) + max(ImgFetch(IMG_FLARE, pixel).rgb, vec3(0.0)), b);
        } else {
            vec3 x = min(SrgbEncode(clamp(hdr, 0.0, 1.0)) + max(ImgFetch(IMG_FLARE, pixel).rgb, vec3(0.0)), vec3(1.0));
            c = ScreenTonemap(x, b);
        }
    }
    vec3 graded = c;
    if (pass.f0.w > 0.0) {
        if (hdr_output) {
            graded = LutHdr(RES_COLOR_LUT, c);
            if (pass.f0.y < 1.0) {
                graded = mix(LutHdr(RES_COLOR_LUT_PREV, c), graded, pass.f0.y);
            }
        } else {
            vec3 within = clamp(c, 0.0, 1.0);
            graded = Lut16(RES_COLOR_LUT, within);
            if (pass.f0.y < 1.0) {
                graded = mix(Lut16(RES_COLOR_LUT_PREV, within), graded, pass.f0.y);
            }
        }
    }
    out_color = vec4(pass.f1.x > 0.0 && !hdr_output ? QuantizeDither(graded, pass.ids.y == 1u ? 17.0 : 0.0) : graded, Luma601(graded));
}
