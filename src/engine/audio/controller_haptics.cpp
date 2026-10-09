#include "engine/audio/controller_haptics.h"

#include <algorithm>
#include <cmath>

namespace pt::audio {

bool ControllerHapticFilter::Process(std::span<const float> stereo_input, std::span<float> stereo_output, size_t frames) {
    if (frames > stereo_input.size() / 2 || frames > stereo_output.size() / 2) {
        return false;
    }

    constexpr float kSampleRate = 48000.0f;
    constexpr float kHighPassHz = 20.0f;
    constexpr float kLowPassHz = 250.0f;
    constexpr float kMaximumActuatorLevel = 0.25f;
    constexpr float kPi = 3.14159265358979323846f;
    const float high_pass_alpha = 1.0f / (1.0f + 2.0f * kPi * kHighPassHz / kSampleRate);
    const float low_pass_alpha = (2.0f * kPi * kLowPassHz / kSampleRate) / (1.0f + 2.0f * kPi * kLowPassHz / kSampleRate);

    for (size_t i = 0; i < frames; ++i) {
        for (size_t channel = 0; channel < 2; ++channel) {
            const size_t index = i * 2 + channel;
            const float input = std::isfinite(stereo_input[index]) ? stereo_input[index] : 0.0f;
            const float high = high_pass_alpha * (high_output_[channel] + input - high_input_[channel]);
            high_input_[channel] = input;
            high_output_[channel] = high;
            low_[channel] += low_pass_alpha * (high - low_[channel]);
            stereo_output[index] = std::clamp(low_[channel], -kMaximumActuatorLevel, kMaximumActuatorLevel);
        }
    }
    return true;
}

void ControllerHapticFilter::Reset() {
    for (size_t channel = 0; channel < 2; ++channel) {
        low_[channel] = 0.0f;
        high_input_[channel] = 0.0f;
        high_output_[channel] = 0.0f;
    }
}

}
