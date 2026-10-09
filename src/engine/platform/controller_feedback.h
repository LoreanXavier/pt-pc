#pragma once

#include <cstdint>

namespace pt {

enum class ControllerRumbleProfile : uint8_t { Original = 0, Enhanced = 1 };

struct ControllerFeedbackFeatures {
    bool trigger_rumble = false;
    bool dualsense_haptics = false;
};

ControllerFeedbackFeatures FeaturesForRumbleProfile(int profile, bool rumble_enabled);

struct TriggerRumbleLevels {
    uint16_t left = 0;
    uint16_t right = 0;

    bool Active() const { return left != 0 || right != 0; }
};

// SDL only defines trigger rumble for pads whose opened gamepad advertises this capability.
TriggerRumbleLevels BuildTriggerRumble(uint8_t left, uint8_t right, bool rumble_enabled, bool trigger_rumble_enabled,
                                       bool capability_reported);

}
