#version 460
#include "common.glsl"
#include "reflection_mix.glsl"
#include "reflection_depth.glsl"

layout(push_constant) uniform PassPush {
    uvec4 ids;
    vec4 f0;
    vec4 f1;
    vec4 f2;
    mat4 m;
} pass;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

View v;

float SceneZ(vec2 uv) {
    return ViewZ(v, ReflectionFloorDepth(v, uv));
}

bool Outside(vec2 uv) {
    return uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;
}

vec2 Project(vec3 p) {
    return ReflectionProject(p, v.projection_param.xy, v.jitter.xy);
}

void main() {
    v = frame.views[pass.ids.x];
    g_reflection_depth_legacy = pass.f1.z > 0.5;
    vec3 plane = pass.f0.xyz;
    float far_limit = pass.f0.w;
    vec2 uv = (gl_FragCoord.xy - 0.00390625) * pass.f2.xy;
    vec3 N = DecodeNormal(ImgLod(IMG_NORMAL, SMP_POINT_CLAMP, uv, 0.0).xyz);
    if (!(abs(dot(plane, N)) >= 0.9)) {
        out_color = vec4(1.0, 1.0, 1.0, 0.0);
        return;
    }
    float z = SceneZ(uv);
    if (far_limit < z) {
        out_color = vec4(1.0, 1.0, 1.0, 0.0);
        return;
    }
    vec2 ndc = in_uv * 2.0 - 1.0;
    vec3 P = ReflectionViewPosition(ndc, v.projection_param.xy, v.jitter.xy, z);
    vec3 D = P * inversesqrt(dot(P, P));
    vec3 R = D - 2.0 * dot(plane, D) * plane;
    vec3 P2 = P + R;
    vec2 start = Project(P);
    vec2 delta = Project(P2) - start;
    float scale = 1.0 / length(0.4 * pass.f1.xy * delta);
    vec2 stride = delta * scale;
    float inv_z = 1.0 / P.z;
    float inv_z_step = (1.0 / P2.z - inv_z) * scale;

    // PC addition (12.16), only with the reflection accumulation of a temporal upscaler (f2.z): the coarse march starts at a
    // per pixel, per frame fraction of its 25 step stride (interleaved gradient noise, frame offset in f2.w), so a thin edge
    // (a frame, a door jamb) that the fixed samples catch on some pixels and step over on their neighbours is found at its
    // average over the frames instead of a length that snaps by a stride as the view moves; without it the march is the original's.
    // The first sample stays at s = 1 (1.25 pixels) as in the original: shifting it too stepped over the baseboard right above the
    // floor's edge, whose hit the original finds in the first interval, and ended those marches as "behind at the first sample";
    // the holes, a per pixel noise pattern along every floor-baseboard crease, put hit and no hit texels next to each other, and
    // the blend's bilinear map lookup between them took colours from far up the screen (thin bright dashes along the crease under
    // an upscaler, the user's white lines near the first hallway sconce)
    float offset = 0.0;
    if (pass.f2.z > 0.5) {
        vec2 noise_pixel = gl_FragCoord.xy + 5.588238 * pass.f2.w;
        offset = fract(52.9829189 * fract(dot(noise_pixel, vec2(0.06711056, 0.00583715))));
    }

    bool terminated = false;
    bool behind_previous = false;
    int hit = -1;
    for (int k = 0; k < 16; ++k) {
        float s = k == 0 && pass.f1.w < 0.5 ? 1.0 : 1.0 + 25.0 * (float(k) + offset);
        vec2 suv = 0.5 * (start + s * stride) + 0.5;
        float sz = SceneZ(suv);
        terminated = terminated || behind_previous || far_limit < sz || Outside(suv);
        bool behind = 1.0 / (inv_z_step * s + inv_z) > sz;
        if (k > 0 && behind && !terminated && hit < 0) {
            hit = k - 1;
        }
        behind_previous = behind;
    }
    if (hit < 0) {
        out_color = vec4(0.0);
        return;
    }

    float lo = hit == 0 && pass.f1.w < 0.5 ? 1.0 : 1.0 + 25.0 * (float(hit) + offset);
    float hi = 1.0 + 25.0 * (float(hit + 1) + offset);
    float best_error = 999.0;
    float best_step = 0.0;
    vec2 best_uv = vec2(0.0);
    bool invalid = false;
    for (int i = 0; i < 6; ++i) {
        float mid = (hi - lo) * 0.5 + lo;
        vec2 muv = 0.5 * (start + mid * stride) + 0.5;
        float mz = SceneZ(muv);
        float ray_z = 1.0 / (inv_z_step * mid + inv_z);
        invalid = invalid || Outside(muv) || far_limit < mz;
        float error = abs(ray_z - mz);
        if (best_error > error) {
            best_error = error;
            best_step = mid;
            best_uv = muv;
        }
        if (ray_z > mz) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    if (invalid) {
        out_color = vec4(0.0);
        return;
    }
    float travel = best_step * scale * length(R);
    float screen_fade = Saturate(best_step * 0.0026595744);
    screen_fade = 1.0 - screen_fade * screen_fade;
    float ray_fade = 1.0 - Saturate(travel * 0.02);
    float path_fade = 1.0 - Saturate((sqrt(P.z * P.z + 1.0 + P.y * P.y + P.x * P.x) + travel) * 0.002);
    float confidence = min(path_fade, min(screen_fade, ray_fade)) * (1.0 - Saturate(best_error * 10.0));
    out_color = vec4(clamp(0.5 * (best_uv - uv) + 0.5, 0.0, 1.0), confidence, 1.0);
}
