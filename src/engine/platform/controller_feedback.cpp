#include "engine/platform/controller_feedback.h"

namespace pt {

ControllerFeedbackFeatures FeaturesForRumbleProfile(int profile, bool rumble_enabled) {
    const bool enhanced = profile == static_cast<int>(ControllerRumbleProfile::Enhanced) && rumble_enabled;
    return {enhanced, enhanced};
}

TriggerRumbleLevels BuildTriggerRumble(uint8_t left, uint8_t right, bool rumble_enabled, bool trigger_rumble_enabled,
                                      bool capability_reported) {
    if (!rumble_enabled || !trigger_rumble_enabled || !capability_reported) {
        return {};
    }
    return {static_cast<uint16_t>(left * 257u), static_cast<uint16_t>(right * 257u)};
}

}
