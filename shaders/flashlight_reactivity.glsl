vec2 FlashlightSurfaceNdc(vec2 uv, vec2 jitter) {
    return uv * 2.0 - 1.0 - jitter;
}
float FlashlightCone(vec3 world, vec3 origin, float cos_outer, vec3 direction, float cone_range) {
    if (!(cos_outer > -1.0 && dot(direction, direction) > 1.0e-8)) return 0.0;
    vec3 delta = world - origin;
    float distance = length(delta);
    if (!(distance > 1.0e-4 && distance < 12.0)) return 0.0;
    float cone = clamp((dot(delta / distance, direction) - cos_outer) * cone_range, 0.0, 1.0);
    return cone * clamp(1.0 - distance / 12.0, 0.0, 1.0);
}
float FlashlightReactive(vec3 world, vec3 origin, float cos_outer, vec3 direction, float cone_range) {
    return FlashlightCone(world, origin, cos_outer, direction, cone_range) * 0.75;
}
