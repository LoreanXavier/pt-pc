#include "engine/audio/audio_engine.h"

#include <SDL3/SDL.h>

#include "engine/core/log.h"

namespace pt::audio {

AudioOutput::~AudioOutput() {
    Close();
}

bool AudioOutput::Open(uint32_t sample_rate, RenderFunction render) {
    Close();
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LogError("audio: SDL audio init failed: {}", SDL_GetError());
        return false;
    }
    render_ = std::move(render);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(sample_rate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Callback, this);
    if (!stream_) {
        LogError("audio: cannot open playback device: {}", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    LogInfo("audio: playback at {} Hz stereo", sample_rate);
    return true;
}

void AudioOutput::Close() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

void AudioOutput::Callback(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* output = static_cast<AudioOutput*>(user);
    if (additional <= 0 || !output->render_) {
        return;
    }
    const uint32_t frames = static_cast<uint32_t>(additional) / (2 * sizeof(float));
    if (frames == 0) {
        return;
    }
    output->buffer_.resize(static_cast<size_t>(frames) * 2);
    output->render_(output->buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, output->buffer_.data(), static_cast<int>(output->buffer_.size() * sizeof(float)));
}

}
