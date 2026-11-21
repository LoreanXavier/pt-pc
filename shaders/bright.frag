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

float ShoulderInverse(float y, float t, float c) {
    if (y <= t) {
        return y;
    }
    float s = t - c + c * c / max(t + c - y, 1.0e-6);
    float k = frame.display.z;
    if (k > 1.001) {
        float big_s = s - t;
        float b = 1.0 - big_s;
        s = t + (-b + sqrt(b * b + 4.0 * k * big_s)) / (2.0 * k);
    }
    return s;
}

vec3 SdrShoulder(vec3 x, float t, float c) {
    vec3 curve = t + c - c * c / (max(x, vec3(t)) - t + c);
    return mix(curve, x, vec3(lessThanEqual(x, vec3(t))));
}

vec3 BloomSource(vec3 y) {
    if (!HdrOutput() || pass.f2.w < 0.5) {
        return y;
    }
    float t = pass.f2.z;
    float c = pass.f2.x;
    vec3 x;
    if (frame.grade.y > 0.5) {
        float m = max(y.r, max(y.g, y.b));
        x = m > t ? y * (ShoulderInverse(m, t, c) / m) : y;
    } else {
        x = vec3(ShoulderInverse(y.r, t, c), ShoulderInverse(y.g, t, c), ShoulderInverse(y.b, t, c));
    }
    return SdrShoulder(x, t, pass.f2.y);
}

vec3 Bright(vec3 scene) {
    vec3 x = clamp(BloomSource(max(scene, vec3(0.0))) * pass.f1.x, 0.0, 1.0);
    float l1 = Luma709(x);
    float l2 = l1 * l1;
    float l3 = l2 * l1;
    float w = pass.f0.x * min(l1, 32.0) + pass.f0.y * min(l2, 32.0) + pass.f0.z * min(l3, 32.0) + pass.f0.w * min(l3 * l1, 32.0);
    return clamp(x * w, 0.0, 1.0);
}

void main() {
    vec2 block = ImgSize(IMG_HDR) / ImgSize(IMG_BLOOM_A);
    if (HdrOutput()) {
        vec2 low = floor(gl_FragCoord.xy) * block;
        vec2 high = low + block;
        ivec2 size = ivec2(ImgSize(IMG_HDR));
        vec3 sum = vec3(0.0);
        float total = 0.0;
        for (int y = int(floor(low.y)); y < int(ceil(high.y)); ++y) {
            float wy = min(float(y + 1), high.y) - max(float(y), low.y);
            for (int x = int(floor(low.x)); x < int(ceil(high.x)); ++x) {
                float w = wy * (min(float(x + 1), high.x) - max(float(x), low.x));
                sum += Bright(ImgFetch(IMG_HDR, min(ivec2(x, y), size - 1)).rgb) * w;
                total += w;
            }
        }
        out_color = vec4(sum / max(total, 1.0e-6), 1.0);
        return;
    }
    vec4 t = ImgFetch(IMG_HDR, ivec2(floor(floor(gl_FragCoord.xy) * block + 0.5 * block)));
    out_color = vec4(Bright(t.rgb), 1.0);
}
