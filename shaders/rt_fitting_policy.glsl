const uint kLightNoFitting = 2u;
const float kRtFittingReach = 0.3;
bool RtFittingAllows(uint caster_flags, uint light_info, uint light_flags, float light_distance) {
    if ((caster_flags & 1u) == 0u) {
        return false;
    }
    uint fitting = (caster_flags >> 4u) & 63u;
    if (fitting != 0u) {
        return fitting == ((light_info >> 25u) & 63u);
    }
    return (light_flags & kLightNoFitting) == 0u && light_distance < kRtFittingReach;
}
