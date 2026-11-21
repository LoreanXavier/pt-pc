vec3 FsrSpecularAlbedo(View v, ivec2 p, vec3 c, vec3 n, vec4 g, vec4 mat0, vec4 mat1, float amount) {
    vec3 V = normalize(-c);
    float F = Img(RES_LUT1, SMP_LINEAR_CLAMP, vec2(Saturate(dot(n, V)), mat1.x)).x;
    return max(max(g.y * mat0.rgb * F, vec3(0.0)) * (1.0 - amount) + amount, vec3(0.002));
}

vec3 FsrSignal(vec3 light, View v) {
    return min(light * v.exposure.x, vec3(60000.0));
}
