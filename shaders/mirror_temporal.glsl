bool MirrorHistoryDepthMatches(float history_z, float expected_z) {
    return history_z > 0.0 && expected_z > 0.0 && abs(history_z - expected_z) <= max(0.02, expected_z * 0.02);
}

vec3 MirrorTemporalColor(vec3 current, vec3 history, vec3 lo, vec3 hi, float exposure_ratio, float weight) {
    return mix(current, clamp(history * exposure_ratio, lo, hi), weight);
}
