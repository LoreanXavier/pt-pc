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
struct SDL_hid_device;
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

// The native platform adapter first narrows endpoint candidates by the HID device's PnP container ID. This helper then
// requires one exact, unique friendly-name match to SDL's physical playback-device list.
std::optional<uint32_t> MatchUniqueControllerAudioEndpoint(std::span<const ControllerSpeakerEndpointName> endpoints,
                                                            std::span<const ControllerAudioEndpointName> sdl_devices,
                                                            int expected_channels);

bool InterleaveDualSensePcm(std::span<const float> speaker_stereo, std::span<const float> actuator_stereo,
                            std::span<float> quad, size_t frames);
bool DownmixControllerSpeakerMono(std::span<const float> stereo, std::span<float> mono, size_t frames);
bool ScaleControllerPcm(std::span<const float> authored_stereo, std::span<float> speaker_stereo,
                        std::span<float> haptics_source_stereo, size_t frames, bool speaker_enabled, bool haptics_enabled,
                        float speaker_gain, float haptics_gain);

// The DualSense's haptics over Bluetooth (no audio endpoint there): HID output report 0x32, 142 bytes with its id, carrying a
// 0x11 control packet (7 bytes, the last a running counter) and a 0x12 packet of 32 interleaved stereo frames of signed 8-bit
// actuator PCM at 3 kHz, sealed with the Bluetooth output CRC-32 (seed byte 0xA2). Layout after SAxense (egormanga/SAxense).
constexpr size_t kDualSenseBtHapticsReportSize = 142;
constexpr size_t kDualSenseBtHapticsFrames = 32;
constexpr int kDualSenseBtHapticsRate = 3000;
void BuildDualSenseBtHapticsReport(std::span<const int8_t> stereo_s8, uint8_t counter, std::span<uint8_t> report);

enum class ControllerPcmRoute : uint8_t { None, DualShock4Mono, DualSenseQuad, DualSenseBluetoothHaptics };

class ControllerSpeakerOutput {
public:
    ControllerSpeakerOutput() = default;
    ~ControllerSpeakerOutput();
    ControllerSpeakerOutput(const ControllerSpeakerOutput&) = delete;
    ControllerSpeakerOutput& operator=(const ControllerSpeakerOutput&) = delete;

    // Opens only a wired Sony pad's render endpoint after resolving its exact PnP container. No system-default device is used.
    bool OpenForGamepad(SDL_Gamepad* gamepad, std::string* reason = nullptr);
    void Close();
    void ClearPending();
    bool IsOpen() const;
    ControllerPcmRoute Route() const { return route_; }
    const std::string& DeviceName() const { return device_name_; }

    // DS4: mono speaker PCM. DualSense: stereo speaker PCM in channels 1-2 and authored actuator PCM in channels 3-4.
    // Speaker PCM is required so opening the shared DualSense endpoint cannot mute its intended speaker/headphone audio.
    bool WriteMono(const float* speaker_mono, uint32_t frames);
    bool WriteDualSense(const float* speaker_stereo, const float* actuator_stereo, uint32_t frames);
    bool WriteCapturedBlock(const audio::ControllerPcmBlock& block, bool speaker_enabled, bool dualsense_haptics_enabled,
                            float speaker_gain = 1.0f, float haptics_gain = 1.0f);

private:
    bool WriteBluetoothHaptics(const float* actuator_stereo, uint32_t frames);

    SDL_AudioStream* stream_ = nullptr;
    SDL_hid_device* hid_ = nullptr;
    // 48 kHz actuator frames averaged 16 to one 3 kHz frame, then 32 of those to a report
    std::array<float, 2> bt_sum_{};
    uint32_t bt_summed_ = 0;
    std::array<int8_t, kDualSenseBtHapticsFrames * 2> bt_frames_{};
    uint32_t bt_frame_count_ = 0;
    uint8_t bt_counter_ = 0;
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
