#include "engine/audio/audio_engine.h"

#include <SDL3/SDL.h>

#include "engine/audio/channel_layout.h"
#include "engine/core/log.h"
#include "engine/platform/sdl_diag.h"

namespace pt::audio {

AudioOutput::~AudioOutput() {
    Close();
}

bool AudioOutput::Open(uint32_t sample_rate, bool surround, RenderFunction render) {
    Close();
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LogError("audio: SDL audio init failed: {} (audio drivers built in: {})", SDL_GetError(), SdlCompiledAudioDrivers());
        return false;
    }
    render_ = std::move(render);
    SDL_AudioSpec device_spec{};
    const bool queried = SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device_spec, nullptr);
    channels_ = queried ? ChannelCount(ChooseOutputLayout(device_spec.channels, surround)) : 2;
    SDL_AudioSpec spec{SDL_AUDIO_F32, static_cast<int>(channels_), static_cast<int>(sample_rate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Callback, this);
    if (!stream_ && channels_ != 2) {
        LogWarn("audio: {}-channel playback unavailable ({}), retrying stereo", channels_, SDL_GetError());
        channels_ = 2;
        spec.channels = 2;
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Callback, this);
    }
    if (!stream_) {
        LogError("audio: cannot open playback device: {}", SDL_GetError());
        LogSdlAudioDevices(false);
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    LogInfo("audio: playback at {} Hz, {} channels{}", sample_rate, channels_,
            channels_ == 8 ? " (7.1)" : channels_ == 6 ? " (5.1)" : " (stereo)");
    LogSdlAudioDevices(false);
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
    const uint32_t channels = output->channels_;
    const uint32_t frames = static_cast<uint32_t>(additional) / (channels * sizeof(float));
    if (frames == 0) {
        return;
    }
    output->buffer_.resize(static_cast<size_t>(frames) * channels);
    output->render_(output->buffer_.data(), frames, channels);
    SDL_PutAudioStreamData(stream, output->buffer_.data(), static_cast<int>(output->buffer_.size() * sizeof(float)));
}

}
