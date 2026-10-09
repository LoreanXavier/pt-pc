#version 460

// VR (docs/vr.md, Renderer::CopyToXr): a frame or the HUD into an OpenXR swapchain image. The port's images hold sRGB-encoded
// values in UNORM formats; OpenXR reads an sRGB swapchain format as encoded and a UNORM one as linear, so this writes linear
// values and an sRGB target encodes them again on write. The frame gets what composite.frag's mode 1 gives the window (film
// grain, brightness); the HUD is premultiplied by its coverage (the UI's blend on a transparent image), unpremultiplied, decoded
// and premultiplied again in linear light.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D grain_noise;

layout(push_constant) uniform XrCopyParams {
    vec4 rect;
    vec4 params;  // x 1 for the premultiplied HUD, y brightness
    vec4 grain;
    vec4 grain_offset;
} p;

vec3 Decode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    vec2 uv = p.rect.xy + in_uv * p.rect.zw;
    vec4 c = texture(source, uv);
    if (p.params.x > 0.5) {
        float a = clamp(c.a, 0.0, 1.0);
        vec3 straight = a > 1.0e-4 ? c.rgb / a : vec3(0.0);
        out_color = vec4(Decode(straight) * a, a);
        return;
    }
    vec3 color = c.rgb;
    if (p.grain.x > 0.0) {
        // as composite.frag (Draw2D_ShFilmGrain)
        float y = abs(dot(color, vec3(0.299, 0.587, 0.114)));
        float strength = p.grain.z;
        float alpha;
        if (p.grain.y > 0.0) {
            alpha = clamp(pow(y, 0.08) * (1.0 - pow(y, 0.12)) * strength * 0.45, 0.0, 1.0);
        } else {
            float q = pow(y, 0.7);
            alpha = clamp(q * (q * -strength * 0.45 + strength * 0.45), 0.0, 1.0);
        }
        float across = p.grain_offset.z > 0.0 ? p.grain_offset.z : 1.0;
        vec2 grain_uv = vec2((uv.x - 0.5) * across + 0.5, uv.y);
        vec3 noise = clamp(textureLod(grain_noise, 3.0 * (p.grain_offset.xy + grain_uv), log2(1536.0 / 1080.0)).rgb, 0.0, 1.0);
        color = mix(color, noise, alpha);
    }
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / max(p.params.y, 0.1)));
    out_color = vec4(Decode(color), 1.0);
}
