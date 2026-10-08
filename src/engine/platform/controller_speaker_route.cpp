#include "engine/platform/controller_speaker.h"

#include <algorithm>
#include <cmath>

namespace pt {

std::optional<uint32_t> MatchUniqueControllerAudioEndpoint(std::span<const ControllerSpeakerEndpointName> endpoints,
                                                           std::span<const ControllerAudioEndpointName> sdl_devices,
                                                           int expected_channels) {
    std::optional<uint32_t> match;
    for (const ControllerSpeakerEndpointName& endpoint : endpoints) {
        if (endpoint.channels != expected_channels) {
            continue;
        }
        for (const ControllerAudioEndpointName& device : sdl_devices) {
            if (device.channels != expected_channels || device.name != endpoint.name) {
                continue;
            }
            if (match) {
                return std::nullopt;
            }
            match = device.id;
        }
    }
    return match;
}

bool InterleaveDualSensePcm(std::span<const float> speaker_stereo, std::span<const float> actuator_stereo,
                            std::span<float> quad, size_t frames) {
    if (frames == 0 || frames > SIZE_MAX / 4 || speaker_stereo.size() != frames * 2 ||
        (!actuator_stereo.empty() && actuator_stereo.size() != frames * 2) || quad.size() != frames * 4) {
        return false;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        quad[frame * 4] = speaker_stereo[frame * 2];
        quad[frame * 4 + 1] = speaker_stereo[frame * 2 + 1];
        quad[frame * 4 + 2] = actuator_stereo.empty() ? 0.0f : actuator_stereo[frame * 2];
        quad[frame * 4 + 3] = actuator_stereo.empty() ? 0.0f : actuator_stereo[frame * 2 + 1];
    }
    return true;
}

bool DownmixControllerSpeakerMono(std::span<const float> stereo, std::span<float> mono, size_t frames) {
    if (frames == 0 || frames > SIZE_MAX / 2 || stereo.size() != frames * 2 || mono.size() != frames) {
        return false;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        mono[frame] = (stereo[frame * 2] + stereo[frame * 2 + 1]) * 0.5f;
    }
    return true;
}

bool ScaleControllerPcm(std::span<const float> authored_stereo, std::span<float> speaker_stereo,
                       std::span<float> haptics_source_stereo, size_t frames, bool speaker_enabled, bool haptics_enabled,
                       float speaker_gain, float haptics_gain) {
    if (frames == 0 || frames > SIZE_MAX / 2 || authored_stereo.size() != frames * 2 || speaker_stereo.size() != frames * 2 ||
        haptics_source_stereo.size() != frames * 2) {
        return false;
    }
    speaker_gain = std::isfinite(speaker_gain) ? std::clamp(speaker_gain, 0.0f, 2.0f) : 0.0f;
    haptics_gain = std::isfinite(haptics_gain) ? std::clamp(haptics_gain, 0.0f, 1.0f) : 0.0f;
    const float speaker_level = speaker_enabled ? speaker_gain : 0.0f;
    const float haptics_level = haptics_enabled ? haptics_gain : 0.0f;
    for (size_t i = 0; i < frames * 2; ++i) {
        speaker_stereo[i] = authored_stereo[i] * speaker_level;
        haptics_source_stereo[i] = authored_stereo[i] * haptics_level;
    }
    return true;
}

}
