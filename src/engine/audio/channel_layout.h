#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pt::audio {

enum class OutputLayout : uint8_t { Stereo = 2, Surround51 = 6, Surround71 = 8 };

constexpr OutputLayout ChooseOutputLayout(int preferred_device_channels, bool surround) {
    if (!surround) return OutputLayout::Stereo;
    if (preferred_device_channels >= 8) return OutputLayout::Surround71;
    if (preferred_device_channels >= 6) return OutputLayout::Surround51;
    return OutputLayout::Stereo;
}

constexpr uint32_t ChannelCount(OutputLayout layout) { return static_cast<uint32_t>(layout); }

constexpr std::array<uint32_t, 3> AudioOutputProbeOrder(int preferred_device_channels, bool surround) {
    if (!surround) return {2, 0, 0};
    if (preferred_device_channels >= 6 && preferred_device_channels < 8) return {6, 8, 2};
    return {8, 6, 2};
}

constexpr uint32_t AudioOutputChannelsForDevice(int actual_device_channels, bool surround) {
    if (!surround) return 2;
    if (actual_device_channels >= 8) return 8;
    if (actual_device_channels >= 6) return 6;
    return 2;
}

// SoundEngine order is FL, FR, FC, BL, BR, SL, SR, LFE; SDL uses FL, FR, FC, LFE, BL, BR, SL, SR.
constexpr std::array<uint8_t, 8> kWwiseToSdl71 = {0, 1, 2, 7, 3, 4, 5, 6};
constexpr float kSurroundFoldGain = 0.70710678f;

inline void WwiseToSdl51Frame(const std::array<const float*, 8>& source, uint32_t frame, float* destination) {
    destination[0] = source[0] ? source[0][frame] : 0.0f;
    destination[1] = source[1] ? source[1][frame] : 0.0f;
    destination[2] = source[2] ? source[2][frame] : 0.0f;
    destination[3] = 0.0f;
    const float back_left = source[3] ? source[3][frame] : 0.0f;
    const float back_right = source[4] ? source[4][frame] : 0.0f;
    const float side_left = source[5] ? source[5][frame] : 0.0f;
    const float side_right = source[6] ? source[6][frame] : 0.0f;
    destination[4] = (back_left + side_left) * kSurroundFoldGain;
    destination[5] = (back_right + side_right) * kSurroundFoldGain;
}

inline void DownmixWwiseToSdl51(const std::array<const float*, 8>& source, float* interleaved, uint32_t frames) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        WwiseToSdl51Frame(source, frame, interleaved + static_cast<size_t>(frame) * 6);
    }
}

template <size_t N>
void ReorderSpeakers(const std::array<const float*, 8>& source, float* interleaved, uint32_t frames,
                     const std::array<uint8_t, N>& order) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (size_t channel = 0; channel < N; ++channel) {
            const float* input = source[order[channel]];
            interleaved[static_cast<size_t>(frame) * N + channel] = input ? input[frame] : 0.0f;
        }
    }
}

}  // namespace pt::audio
