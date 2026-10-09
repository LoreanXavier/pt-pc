#include "engine/platform/controller_speaker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };

    const std::array endpoints{pt::ControllerSpeakerEndpointName{"DualSense USB", 4}};
    const std::array devices{pt::ControllerAudioEndpointName{10, "Speakers", 2},
                             pt::ControllerAudioEndpointName{11, "DualSense USB", 2},
                             pt::ControllerAudioEndpointName{12, "DualSense USB", 4}};
    check(pt::MatchUniqueControllerAudioEndpoint(endpoints, devices, 4) == 12,
          "exact endpoint name and channel count select the SDL device associated by native identity");

    const std::array unrelated{pt::ControllerSpeakerEndpointName{"Speakers", 4}};
    check(!pt::MatchUniqueControllerAudioEndpoint(unrelated, devices, 4), "a name without a native identity match is rejected");

    const std::array ambiguous_devices{pt::ControllerAudioEndpointName{12, "DualSense USB", 4},
                                       pt::ControllerAudioEndpointName{13, "DualSense USB", 4}};
    check(!pt::MatchUniqueControllerAudioEndpoint(endpoints, ambiguous_devices, 4), "duplicate SDL names fail closed");

    const std::array ambiguous_endpoints{pt::ControllerSpeakerEndpointName{"DualSense USB", 4},
                                         pt::ControllerSpeakerEndpointName{"DualSense USB", 4}};
    check(!pt::MatchUniqueControllerAudioEndpoint(ambiguous_endpoints, devices, 4), "multiple endpoints in one container fail closed");

    check(!pt::MatchUniqueControllerAudioEndpoint(endpoints, devices, 1), "incorrect channel layout is rejected");

    const std::array speaker{0.1f, 0.2f, 0.3f, 0.4f};
    const std::array actuators{0.5f, 0.6f, 0.7f, 0.8f};
    std::array<float, 8> quad{};
    check(pt::InterleaveDualSensePcm(speaker, actuators, quad, 2) && quad == std::array{0.1f, 0.2f, 0.5f, 0.6f,
                                                                                       0.3f, 0.4f, 0.7f, 0.8f},
          "speaker and actuator PCM occupy separate DualSense channel pairs");
    std::array<float, 8> speaker_only{};
    check(pt::InterleaveDualSensePcm(speaker, {}, speaker_only, 2) &&
              speaker_only == std::array{0.1f, 0.2f, 0.0f, 0.0f, 0.3f, 0.4f, 0.0f, 0.0f},
          "missing actuator PCM mutes only the haptic channel pair");
    check(!pt::InterleaveDualSensePcm(speaker, actuators, quad, 1), "mismatched PCM frame counts are rejected");
    std::array<float, 2> mono{};
    check(pt::DownmixControllerSpeakerMono(speaker, mono, 2) && std::abs(mono[0] - 0.15f) < 1e-6f && std::abs(mono[1] - 0.35f) < 1e-6f,
          "DS4 speaker downmix averages authored stereo channels per frame");
    check(!pt::DownmixControllerSpeakerMono(speaker, std::span<float>(mono).first(1), 2),
          "DS4 downmix rejects an undersized output buffer");
    const std::array authored{0.4f, -0.3f, 0.2f, 0.1f};
    std::array<float, 4> speaker_scaled{};
    std::array<float, 4> haptics_source{};
    check(pt::ScaleControllerPcm(authored, speaker_scaled, haptics_source, 2, true, true, 0.2f, 0.8f) &&
              std::abs(speaker_scaled[0] - 0.08f) < 1e-6f && std::abs(speaker_scaled[1] + 0.06f) < 1e-6f &&
              std::abs(haptics_source[0] - 0.32f) < 1e-6f && std::abs(haptics_source[1] + 0.24f) < 1e-6f,
          "independent controller speaker and haptics gains scale their own channels");
    std::array<float, 4> speaker_louder{};
    std::array<float, 4> haptics_unchanged{};
    check(pt::ScaleControllerPcm(authored, speaker_louder, haptics_unchanged, 2, true, true, 1.0f, 0.8f) &&
              speaker_louder[0] == authored[0] && haptics_unchanged == haptics_source,
          "changing speaker volume leaves the actuator source unchanged");
    check(pt::ScaleControllerPcm(authored, speaker_scaled, haptics_source, 2, false, true, 1.0f, 0.8f) &&
              std::all_of(speaker_scaled.begin(), speaker_scaled.end(), [](float x) { return x == 0.0f; }) &&
              haptics_source == haptics_unchanged,
          "haptics-only mode silences DualSense speaker channels");
    check(pt::ScaleControllerPcm(authored, speaker_scaled, haptics_source, 2, true, false, 0.5f, 1.0f) &&
              std::all_of(haptics_source.begin(), haptics_source.end(), [](float x) { return x == 0.0f; }),
          "speaker-only mode silences DualSense actuator channels");
    check(!pt::ScaleControllerPcm(authored, std::span<float>(speaker_scaled).first(2), haptics_source, 2, true, true, 1.0f, 1.0f),
          "channel scaling rejects mismatched output spans");
    return failures ? 1 : 0;
}
