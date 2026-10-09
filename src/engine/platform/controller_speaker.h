#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/controller_haptics.h"
#include "engine/audio/controller_pcm_capture.h"

struct SDL_AudioStream;
struct SDL_Gamepad;

namespace pt {

struct ControllerAudioEndpointName {
    uint32_t id = 0;
    std::string_view name;
    int channels = 0;
};

struct ControllerSpeakerEndpointName {
    std::string_view name;
    int channels = 0;
};

std::optional<uint32_t> MatchUniqueControllerAudioEndpoint(std::span<const ControllerSpeakerEndpointName> endpoints,
                                                            std::span<const ControllerAudioEndpointName> sdl_devices,
                                                            int expected_channels);

bool InterleaveDualSensePcm(std::span<const float> speaker_stereo, std::span<const float> actuator_stereo,
                            std::span<float> quad, size_t frames);
bool DownmixControllerSpeakerMono(std::span<const float> stereo, std::span<float> mono, size_t frames);
bool ScaleControllerPcm(std::span<const float> authored_stereo, std::span<float> speaker_stereo,
                        std::span<float> haptics_source_stereo, size_t frames, bool speaker_enabled, bool haptics_enabled,
                        float speaker_gain, float haptics_gain);

enum class ControllerPcmRoute : uint8_t { None, DualShock4Mono, DualSenseQuad };

class ControllerSpeakerOutput {
public:
    ControllerSpeakerOutput() = default;
    ~ControllerSpeakerOutput();
    ControllerSpeakerOutput(const ControllerSpeakerOutput&) = delete;
    ControllerSpeakerOutput& operator=(const ControllerSpeakerOutput&) = delete;

    bool OpenForGamepad(SDL_Gamepad* gamepad, std::string* reason = nullptr);
    void Close();
    void ClearPending();
    bool IsOpen() const;
    ControllerPcmRoute Route() const { return route_; }
    const std::string& DeviceName() const { return device_name_; }

    bool WriteMono(const float* speaker_mono, uint32_t frames);
    bool WriteDualSense(const float* speaker_stereo, const float* actuator_stereo, uint32_t frames);
    bool WriteCapturedBlock(const audio::ControllerPcmBlock& block, bool speaker_enabled, bool dualsense_haptics_enabled,
                            float speaker_gain = 1.0f, float haptics_gain = 1.0f);

private:
    SDL_AudioStream* stream_ = nullptr;
    ControllerPcmRoute route_ = ControllerPcmRoute::None;
    std::string device_name_;
    bool owns_audio_subsystem_ = false;
    audio::ControllerHapticFilter haptic_filter_;
    std::array<float, audio::ControllerPcmBlock::kMaxFrames> mono_scratch_{};
    std::array<float, audio::ControllerPcmBlock::kMaxFrames * 2> actuator_scratch_{};
    std::array<float, audio::ControllerPcmBlock::kMaxFrames * 2> speaker_scratch_{};
    std::array<float, audio::ControllerPcmBlock::kMaxFrames * 2> haptics_source_scratch_{};
};

}
