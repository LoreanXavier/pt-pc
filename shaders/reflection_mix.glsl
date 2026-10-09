vec3 ReflectionViewPosition(vec2 ndc, vec2 projection_scale, vec2 jitter, float z) {
    return vec3((ndc - jitter) * projection_scale * z, z);
}

vec2 ReflectionProject(vec3 p, vec2 projection_scale, vec2 jitter) {
    return p.xy / (projection_scale * p.z) + jitter;
}

float ReflectionContinuousDepth(vec4 footprint, vec2 fraction, float point_depth, bool temporal) {
    if (!temporal) return point_depth;
    float lo = min(min(footprint.x, footprint.y), min(footprint.z, footprint.w));
    float hi = max(max(footprint.x, footprint.y), max(footprint.z, footprint.w));
    // Reversed depth is affine across a projected plane. Keep point depth at silhouettes and sky boundaries.
    if (!(lo > 0.0) || hi - lo > lo * 0.05) return point_depth;
    return mix(mix(footprint.x, footprint.y, fraction.x), mix(footprint.z, footprint.w, fraction.x), fraction.y);
}

// The floor reflections' scene depth under a jittered view (12.16, PC addition): footprint is the 2x2 texels around the sample
// (x, y the upper row left and right, z, w the lower), outer the texels left of x, right of y, above x and below z. The footprint
// is interpolated only where all of them lie on one plane, on which reverse depth is affine in screen space: the footprint's
// cross difference and the second differences along its row and column vanish there, up to 5 % of the plane's own step along
// that direction (and 1e-5 of the depth for rounding).
// ReflectionContinuousDepth's test, the footprint within 5 % of its depth, holds across every edge far away (a 1.5 cm baseboard
// step at 10 m is 0.15 %): the march then took the floor-baseboard crease and the baseboard's top for surfaces that are not
// there (a crease's interpolation lies in front of both faces, an outer edge's behind them), and the far floor along the
// baseboards showed a line of hits on the lit baseboard and of misses that flickered with the jitter.
float ReflectionPlanarDepth(vec4 footprint, vec4 outer, vec2 fraction, float point_depth) {
    float lo = min(min(min(footprint.x, footprint.y), min(footprint.z, footprint.w)), min(min(outer.x, outer.y), min(outer.z, outer.w)));
    if (!(lo > 0.0)) return point_depth;
    float rounding = 1.0e-5 * footprint.x;
    float row_tolerance = 0.05 * abs(footprint.y - footprint.x) + rounding;
    float column_tolerance = 0.05 * abs(footprint.z - footprint.x) + rounding;
    float cross_difference = abs(footprint.x - footprint.y - footprint.z + footprint.w);
    float row = max(abs(outer.x - 2.0 * footprint.x + footprint.y), abs(footprint.x - 2.0 * footprint.y + outer.y));
    float column = max(abs(outer.z - 2.0 * footprint.x + footprint.z), abs(footprint.x - 2.0 * footprint.z + outer.w));
    if (row > row_tolerance || column > column_tolerance || cross_difference > min(row_tolerance, column_tolerance)) return point_depth;
    return mix(mix(footprint.x, footprint.y, fraction.x), mix(footprint.z, footprint.w, fraction.x), fraction.y);
}

// Screen coordinates and traced colour share a bilinear footprint at the edge of the visible scene.
vec3 RtMappedNormal(vec3 normal, vec3 tangent, vec3 bitangent, vec3 tangent_normal) {
    return normalize(tangent * tangent_normal.x + bitangent * tangent_normal.y + normal * tangent_normal.z);
}

// The map keeps the original's form (reflect_make_rt.frag): screen hits (offset, confidence, 1), floor texels without one 0 and
// off the floors (0, 0, 1, 0), so the screen weight is the original blend's confidence times coverage (refl.z * refl.w) and the
// traced colour (premultiplied by its weight) adds its own; the offset is the hits' average (xy / w, the texels without a hit
// holding 0) instead of one pulled toward the screen's corner where hit and no hit texels meet.
float RtReflectionCenterWeight(vec4 screen_map, vec4 traced) {
    return screen_map.z * screen_map.w + max(0.0, traced.a);
}

vec2 RtScreenCoordinate(vec4 screen_map) {
    return screen_map.w > 1.0e-4 ? screen_map.xy / screen_map.w : vec2(0.5);
}

vec4 RtReflectionContribution(vec4 screen_map, vec4 traced, vec3 screen_color) {
    vec2 coordinate = RtScreenCoordinate(screen_map);
    bool on_screen = screen_map.w > 1.0e-4 && all(greaterThanEqual(coordinate, vec2(0.0))) && all(lessThanEqual(coordinate, vec2(1.0)));
    float screen_weight = on_screen ? max(0.0, screen_map.z * screen_map.w) : 0.0;
    float traced_weight = max(0.0, traced.a);
    float weight = screen_weight + traced_weight;
    if (!(weight > 1.0e-4)) return vec4(0.0);
    return vec4((screen_color * screen_weight + traced.rgb) / weight, min(weight, 1.0));
}
