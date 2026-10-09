float ReflectionSceneDepth(View view, vec2 uv) {
    float point_depth = ImgLod(IMG_DEPTH, SMP_POINT_CLAMP, uv, 0.0).x;
    if (all(equal(view.jitter.xy, vec2(0.0)))) return point_depth;
    ivec2 size = ivec2(ImgSize(IMG_DEPTH));
    vec2 sample_pixel = uv * vec2(size) - 0.5;
    ivec2 p = ivec2(floor(sample_pixel));
    vec4 footprint = vec4(
        ImgFetch(IMG_DEPTH, clamp(p, ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(1, 0), ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(0, 1), ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(1, 1), ivec2(0), size - 1)).x);
    return ReflectionContinuousDepth(footprint, fract(sample_pixel), point_depth, true);
}

// PT_REFLECT_DEPTH_LEGACY=1: the floor reflections take ReflectionSceneDepth's footprint test again (the passes set it from
// their push constants)
bool g_reflection_depth_legacy = false;

// The floor reflections' scene depth (reflect_make.frag, reflect_make_rt.frag, the reflection layer): under a jittered view the
// footprint interpolated only on a plane (ReflectionPlanarDepth), else the texel's own depth as without jitter. The mirror's
// capture keeps ReflectionSceneDepth.
float ReflectionFloorDepth(View view, vec2 uv) {
    if (g_reflection_depth_legacy) return ReflectionSceneDepth(view, uv);
    float point_depth = ImgLod(IMG_DEPTH, SMP_POINT_CLAMP, uv, 0.0).x;
    if (all(equal(view.jitter.xy, vec2(0.0)))) return point_depth;
    ivec2 size = ivec2(ImgSize(IMG_DEPTH));
    vec2 sample_pixel = uv * vec2(size) - 0.5;
    ivec2 p = ivec2(floor(sample_pixel));
    vec4 footprint = vec4(
        ImgFetch(IMG_DEPTH, clamp(p, ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(1, 0), ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(0, 1), ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(1, 1), ivec2(0), size - 1)).x);
    vec4 outer = vec4(
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(-1, 0), ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(2, 0), ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(0, -1), ivec2(0), size - 1)).x,
        ImgFetch(IMG_DEPTH, clamp(p + ivec2(0, 2), ivec2(0), size - 1)).x);
    return ReflectionPlanarDepth(footprint, outer, fract(sample_pixel), point_depth);
}
