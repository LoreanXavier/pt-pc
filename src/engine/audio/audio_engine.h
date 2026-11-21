#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/audio/audio_objects.h"
#include "engine/audio/output_device.h"

struct SDL_AudioStream;

namespace pt::audio {

class SpatialOutput;

class AudioOutput {
public:
    static void SetPreferSpatial(bool prefer) { prefer_spatial_ = prefer; }
    static void SetPreferredDevice(std::string name) { preferred_device_ = std::move(name); }
    static const std::string& PreferredDevice() { return preferred_device_; }

    using RenderFunction = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    using ObjectRenderFunction = std::function<void(float* bed, uint32_t frames, AudioObjectMix& mix)>;
    bool Open(uint32_t sample_rate, bool surround, RenderFunction render, ObjectRenderFunction object_render = nullptr);
    void Close();
    bool IsOpen() const { return stream_ != nullptr || spatial_ != nullptr; }
    uint32_t Channels() const { return channels_; }
    const std::string& DeviceName() const { return device_name_; }
    OutputPick Pick() const { return pick_; }
    bool Poll(uint64_t now_ns);

private:
    static void Callback(void* user, SDL_AudioStream* stream, int additional, int total);
    void LogOutput(const char* path) const;

    SDL_AudioStream* stream_ = nullptr;
    uint32_t device_id_ = 0;
    RenderFunction render_;
    std::vector<float> buffer_;
    uint32_t channels_ = 2;
    std::unique_ptr<SpatialOutput> spatial_;
    std::string device_name_;
    std::string device_endpoint_id_;
    OutputPick pick_ = OutputPick::SystemDefault;
    uint32_t chosen_physical_ = 0;
    std::string default_at_open_;
    uint64_t next_poll_ns_ = 0;
    static inline bool prefer_spatial_ = false;
    static inline std::string preferred_device_;
};

}
