#pragma once

#include <cstddef>
#include <span>

namespace pt::audio {

class ControllerHapticFilter {
public:
    bool Process(std::span<const float> stereo_input, std::span<float> stereo_output, size_t frames);
    void Reset();

private:
    float low_[2] = {};
    float high_input_[2] = {};
    float high_output_[2] = {};
};

}
