#pragma once

#include <cstdint>
#include <functional>
#include <vector>

struct SDL_AudioStream;

namespace pt::audio {

class AudioOutput {
public:
    using RenderFunction = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

    AudioOutput() = default;
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Open(uint32_t sample_rate, bool surround, RenderFunction render);
    void Close();
    bool IsOpen() const { return stream_ != nullptr; }
    uint32_t Channels() const { return channels_; }

private:
    static void Callback(void* user, SDL_AudioStream* stream, int additional, int total);

    SDL_AudioStream* stream_ = nullptr;
    uint32_t device_id_ = 0;
    RenderFunction render_;
    std::vector<float> buffer_;
    uint32_t channels_ = 2;
};

}
