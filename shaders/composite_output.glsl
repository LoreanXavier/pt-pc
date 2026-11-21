float PqEncode(float nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

#include "hdr_shoulder.glsl"

const mat3 kBt709ToBt2020 = mat3(0.627404, 0.069097, 0.0163916,
                                 0.329283, 0.919540, 0.0880132,
                                 0.0433136, 0.0113612, 0.895595);
const mat3 kBt2020ToBt709 = mat3(1.660491, -0.124550, -0.018151,
                                 -0.587641, 1.132900, -0.100579,
                                 -0.072850, -0.008349, 1.118730);

float GammaDecode(float e, float gamma) {
    if (e >= 1.0 || gamma < 0.5) {
        return pow(e, 2.2);
    }
    if (gamma < 1.5) {
        return e <= 0.04045 ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4);
    }
    return pow(e, 2.4);
}

vec3 CompositeDisplayColor(vec3 color, float brightness, float output_mode, vec2 hdr, float gamma) {
    if (output_mode < 0.5) {
        return pow(clamp(color, 0.0, 1.0), vec3(1.0 / brightness));
    }
    vec3 e = sign(color) * pow(abs(color), vec3(1.0 / brightness));
    vec3 light = sign(e) * vec3(GammaDecode(abs(e.r), gamma), GammaDecode(abs(e.g), gamma), GammaDecode(abs(e.b), gamma));
    vec3 wide = HdrPeakRollRgb(kBt709ToBt2020 * light, hdr.y / max(hdr.x, 1.0));
    if (output_mode < 1.5) {
        return (kBt2020ToBt709 * wide) * (hdr.x / 80.0);
    }
    wide *= hdr.x;
    return vec3(PqEncode(wide.r), PqEncode(wide.g), PqEncode(wide.b));
}

vec3 OutputNits(vec3 nits, float output_mode) {
    if (output_mode < 1.5) {
        return nits * (1.0 / 80.0);
    }
    vec3 bt2020 = kBt709ToBt2020 * nits;
    return vec3(PqEncode(bt2020.r), PqEncode(bt2020.g), PqEncode(bt2020.b));
}

vec4 CompositeUiLayer(vec3 shown, vec3 base, float tolerance) {
    vec3 change = shown - base;
    if (all(lessThanEqual(abs(change), vec3(tolerance)))) {
        return vec4(max(change, vec3(0.0)), 0.0);
    }
    float a = max(abs(change.r), max(abs(change.g), abs(change.b)));
    for (int c = 0; c < 3; ++c) {
        if (base[c] > 0.0) {
            a = max(a, 1.0 - shown[c] / base[c]);
        }
    }
    a = clamp(a, 0.0, 1.0);
    return vec4(max(shown - (1.0 - a) * base, vec3(0.0)), a);
}
