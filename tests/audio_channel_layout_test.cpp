#include "engine/audio/channel_layout.h"

#include <array>
#include <cstdio>

int main() {
    using namespace pt::audio;
    if (ChooseOutputLayout(8, false) != OutputLayout::Stereo || ChooseOutputLayout(8, true) != OutputLayout::Surround71 ||
        ChooseOutputLayout(6, true) != OutputLayout::Surround51 || ChooseOutputLayout(2, true) != OutputLayout::Stereo ||
        ChooseOutputLayout(1, true) != OutputLayout::Stereo) {
        std::puts("surround layout selection failed");
        return 1;
    }

    std::array<std::array<float, 2>, 8> channels{};
    std::array<const float*, 8> source{};
    for (size_t channel = 0; channel < channels.size(); ++channel) {
        channels[channel] = {static_cast<float>(channel + 10), static_cast<float>(channel + 20)};
        source[channel] = channels[channel].data();
    }
    float out51[12]{};
    DownmixWwiseToSdl51(source, out51, 2);
    constexpr float kRearFold = 0.70710678f;
    const float expected51[] = {10, 11, 12, 0, kRearFold * (13 + 15), kRearFold * (14 + 16),
                                20, 21, 22, 0, kRearFold * (23 + 25), kRearFold * (24 + 26)};
    for (size_t i = 0; i < std::size(expected51); ++i) {
        if (std::abs(out51[i] - expected51[i]) > 1e-6f) {
            std::puts("5.1 speaker order failed");
            return 1;
        }
    }
    float out71[16]{};
    ReorderSpeakers(source, out71, 2, kWwiseToSdl71);
    constexpr float expected71[] = {10, 11, 12, 17, 13, 14, 15, 16, 20, 21, 22, 27, 23, 24, 25, 26};
    for (size_t i = 0; i < std::size(expected71); ++i) {
        if (out71[i] != expected71[i]) {
            std::puts("7.1 speaker order failed");
            return 1;
        }
    }
    return 0;
}
