
vec3 OriginalGrain(vec3 color, vec2 uv, vec4 grain, vec4 grain_offset) {
    float y = abs(dot(color, vec3(0.299, 0.587, 0.114)));
    float strength = grain.z;
    float alpha;
    if (grain.y > 0.0) {
        alpha = clamp(pow(y, 0.08) * (1.0 - pow(y, 0.12)) * strength * 0.45, 0.0, 1.0);
    } else {
        float p = pow(y, 0.7);
        alpha = clamp(p * (p * -strength * 0.45 + strength * 0.45), 0.0, 1.0);
    }
    float across = grain_offset.z > 0.0 ? grain_offset.z : 1.0;
    vec2 grain_uv = vec2((uv.x - 0.5) * across + 0.5, uv.y);
    vec3 noise = clamp(textureLod(grain_noise, 3.0 * (grain_offset.xy + grain_uv), log2(1536.0 / 1080.0)).rgb, 0.0, 1.0);
    return mix(color, noise, alpha);
}

float GrainHash(uvec2 p, uint seed) {
    uint h = p.x * 1664525u + p.y * 1013904223u + seed * 2654435761u;
    h ^= h >> 16;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return float(h >> 8) * (1.0 / 16777216.0);
}

vec3 FineGrain(vec3 color, vec2 pixel, vec4 grain, vec4 grain_offset, bool sdr) {
    vec3 light = pow(max(color, vec3(0.0)), vec3(2.2));
    float y = dot(light, vec3(0.2126, 0.7152, 0.0722));
    float density = 3.0 * y;
    float amount = pow(10.0, 0.880 - 0.736 * density - 0.003 * pow(density, 7.6));
    uint seed = floatBitsToUint(grain_offset.x) ^ (floatBitsToUint(grain_offset.y) * 747796405u);
    float a = (GrainHash(uvec2(pixel), seed) - 0.5) * amount * grain.z * 0.04;
    if (sdr) {
        float strongest = max(light.r, max(light.g, light.b));
        float room = strongest < 1.0 ? (1.0 - strongest) / max(strongest, 1.0e-4) : 0.0;
        a = clamp(a, -room, room);
    }
    return color * pow(max(1.0 + a, 0.0), 1.0 / 2.2);
}

vec3 FilmGrain(vec3 color, vec2 uv, vec2 pixel, vec4 grain, vec4 grain_offset, bool sdr) {
    return grain.w > 0.5 ? FineGrain(color, pixel, grain, grain_offset, sdr) : OriginalGrain(color, uv, grain, grain_offset);
}
