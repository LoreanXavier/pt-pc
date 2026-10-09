#include "engine/audio/audio_engine.h"

#include <SDL3/SDL.h>

#include <string>

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
        LogError("audio: SDL audio init failed (surround requested: {}): {} (audio drivers built in: {})", surround,
                 SDL_GetError(), SdlCompiledAudioDrivers());
        return false;
    }
    render_ = std::move(render);
    SDL_AudioSpec preferred_spec{};
    const bool queried = SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &preferred_spec, nullptr);
    const int preferred_channels = queried ? preferred_spec.channels : -1;
    if (!queried) {
        LogWarn("audio: could not query preferred playback format (surround requested: {}): {}", surround, SDL_GetError());
    }
    LogInfo("audio: opening playback (surround requested: {}, preferred channels: {})", surround, preferred_channels);

    std::string last_error;
    const auto probes = AudioOutputProbeOrder(preferred_channels, surround);
    for (const uint32_t probe_channels : probes) {
        if (probe_channels == 0) break;

        const SDL_AudioSpec requested_spec{SDL_AUDIO_F32, static_cast<int>(probe_channels), static_cast<int>(sample_rate)};
        const SDL_AudioDeviceID device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &requested_spec);
        if (!device) {
            last_error = SDL_GetError();
            LogWarn("audio: {}-channel device open failed (surround requested: {}, preferred channels: {}): {}",
                    probe_channels, surround, preferred_channels, last_error);
            continue;
        }

        SDL_AudioSpec opened_spec{};
        if (!SDL_GetAudioDeviceFormat(device, &opened_spec, nullptr)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not query opened {}-channel device (surround requested: {}): {}", probe_channels,
                    surround, last_error);
            SDL_CloseAudioDevice(device);
            continue;
        }

        const uint32_t app_channels = AudioOutputChannelsForDevice(opened_spec.channels, surround);
        if (surround && probe_channels > 2 && app_channels == 2) {
            LogInfo("audio: {}-channel request opened as {} channels; probing next layout", probe_channels,
                    opened_spec.channels);
            SDL_CloseAudioDevice(device);
            continue;
        }

        const SDL_AudioSpec app_spec{SDL_AUDIO_F32, static_cast<int>(app_channels), static_cast<int>(sample_rate)};
        SDL_AudioStream* stream = SDL_CreateAudioStream(&app_spec, nullptr);
        if (!stream) {
            last_error = SDL_GetError();
            LogWarn("audio: could not create {}-channel stream for {}-channel device: {}", app_channels,
                    opened_spec.channels, last_error);
            SDL_CloseAudioDevice(device);
            continue;
        }
        if (!SDL_BindAudioStream(device, stream)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not bind {}-channel stream to {}-channel device: {}", app_channels,
                    opened_spec.channels, last_error);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }

        SDL_AudioSpec bound_src{};
        SDL_AudioSpec bound_dst{};
        if (!SDL_GetAudioStreamFormat(stream, &bound_src, &bound_dst)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not query bound stream format: {}", last_error);
            SDL_UnbindAudioStream(stream);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }
        const uint32_t bound_app_channels = AudioOutputChannelsForDevice(bound_dst.channels, surround);
        if (surround && probe_channels > 2 && bound_app_channels == 2) {
            LogInfo("audio: bound {}-channel stream has {} output channels; probing next layout", app_channels,
                    bound_dst.channels);
            SDL_UnbindAudioStream(stream);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }
        if (bound_src.channels != static_cast<int>(bound_app_channels)) {
            const SDL_AudioSpec adjusted_src{SDL_AUDIO_F32, static_cast<int>(bound_app_channels), static_cast<int>(sample_rate)};
            if (!SDL_SetAudioStreamFormat(stream, &adjusted_src, nullptr)) {
                last_error = SDL_GetError();
                LogWarn("audio: could not match stream input to {} actual output channels: {}", bound_app_channels,
                        last_error);
                SDL_UnbindAudioStream(stream);
                SDL_DestroyAudioStream(stream);
                SDL_CloseAudioDevice(device);
                continue;
            }
        }
        if (!SDL_SetAudioStreamGetCallback(stream, Callback, this)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not install playback callback: {}", last_error);
            SDL_UnbindAudioStream(stream);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }

        device_id_ = device;
        stream_ = stream;
        channels_ = bound_app_channels;
        if (!SDL_ResumeAudioDevice(device_id_)) {
            last_error = SDL_GetError();
            LogError("audio: could not resume playback device (surround requested: {}, preferred channels: {}, requested channels: {}, actual channels: {}, renderer channels: {}): {}",
                     surround, preferred_channels, probe_channels, bound_dst.channels, channels_, last_error);
            Close();
            LogSdlAudioDevices(false);
            return false;
        }
        LogInfo("audio: playback at {} Hz (surround requested: {}, preferred channels: {}, requested channels: {}, actual channels: {}, renderer channels: {})",
                sample_rate, surround, preferred_channels, probe_channels, bound_dst.channels, channels_);
        LogSdlAudioDevices(false);
        return true;
    }

    LogError("audio: cannot open playback device (surround requested: {}, preferred channels: {}, last error: {})", surround,
             preferred_channels, last_error.empty() ? SDL_GetError() : last_error);
    render_ = {};
    channels_ = 2;
    LogSdlAudioDevices(false);
    return false;
}

void AudioOutput::Close() {
    if (stream_) {
        SDL_UnbindAudioStream(stream_);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (device_id_ != 0) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    render_ = {};
    channels_ = 2;
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
