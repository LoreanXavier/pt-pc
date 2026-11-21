
const float kHdrRollKnee = 0.8;

float HdrPeakRoll(float light, float ratio) {
    float knee = kHdrRollKnee * ratio;
    if (!(light > knee)) {
        return max(light, 0.0);
    }
    float room = ratio - knee;
    return knee + room * (1.0 - exp(-(light - knee) / room));
}

vec3 HdrPeakRollRgb(vec3 light, float ratio) {
    light = max(light, vec3(0.0));
    float strongest = max(light.r, max(light.g, light.b));
    if (!(strongest > kHdrRollKnee * ratio)) {
        return light;
    }
    return light * (HdrPeakRoll(strongest, ratio) / strongest);
}
