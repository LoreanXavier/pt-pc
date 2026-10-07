#pragma once

#include <cstdint>
#include <functional>
#include <vector>

struct SDL_AudioStream;

namespace pt::audio {

class AudioOutput {
public:
    using RenderFunction = std::function<void(float* interleaved_stereo, uint32_t frames)>;

    AudioOutput() = default;
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Open(uint32_t sample_rate, RenderFunction render);
    void Close();
    bool IsOpen() const { return stream_ != nullptr; }

private:
    static void Callback(void* user, SDL_AudioStream* stream, int additional, int total);

    SDL_AudioStream* stream_ = nullptr;
    RenderFunction render_;
    std::vector<float> buffer_;
};

}
