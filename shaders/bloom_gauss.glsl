vec4 BloomGaussian(uint source, vec2 uv, vec2 axis, float sigma) {
    float spacing = max(0.5 * sigma, 1.0);
    int reach = int(ceil(3.0 * sigma / spacing));
    vec2 step = axis * spacing / vec2(ImgSize(source));
    vec4 sum = Img(source, SMP_LINEAR_CLAMP, uv);
    float total = 1.0;
    for (int i = 1; i <= reach; ++i) {
        float d = float(i) * spacing;
        float w = exp(-0.5 * d * d / (sigma * sigma));
        sum += (Img(source, SMP_LINEAR_CLAMP, uv + step * float(i)) + Img(source, SMP_LINEAR_CLAMP, uv - step * float(i))) * w;
        total += 2.0 * w;
    }
    return sum / total;
}
