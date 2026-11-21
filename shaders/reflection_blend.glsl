vec3 ReflectionBlendBase(vec3 base, bool hdr) {
    return SrgbEncode(hdr || HdrOutput() ? max(base, vec3(0.0)) : min(base, vec3(1.0)));
}
