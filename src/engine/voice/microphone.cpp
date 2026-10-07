#include "engine/voice/microphone.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>

#include "engine/core/log.h"

namespace pt {

bool Microphone::Open(int sample_rate, const std::string& device_name) {
    Close();
    sample_rate_ = sample_rate;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LogError("microphone: SDL audio init failed: {}", SDL_GetError());
        return false;
    }
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!device_name.empty()) {
        auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const std::string wanted = lower(device_name);
        int count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
        for (int i = 0; devices && i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            if (name && lower(name).find(wanted) != std::string::npos) {
                device = devices[i];
                LogInfo("microphone: using {}", name);
                break;
            }
        }
        SDL_free(devices);
        if (device == SDL_AUDIO_DEVICE_DEFAULT_RECORDING) {
            LogWarn("microphone: no device matches '{}', using default", device_name);
        }
    }
    SDL_AudioSpec spec{SDL_AUDIO_S16, 1, sample_rate};
    stream_ = SDL_OpenAudioDeviceStream(device, &spec, nullptr, nullptr);
    if (!stream_) {
        LogWarn("microphone: no recording device: {}", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    LogInfo("microphone: recording at {} Hz", sample_rate);
    return true;
}

void Microphone::Close() {
    SetMonitor(false);
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
}

bool Microphone::SetMonitor(bool enabled) {
    if (!enabled || !stream_) {
        if (monitor_) SDL_DestroyAudioStream(monitor_);
        monitor_ = nullptr;
        return !enabled;
    }
    if (monitor_) return true;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 1, sample_rate_};
    monitor_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!monitor_) {
        LogWarn("microphone: monitoring unavailable: {}", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(monitor_);
    return true;
}

size_t Microphone::Read(std::vector<int16_t>& out) {
    out.clear();
    if (!stream_) {
        return 0;
    }
    const int available = SDL_GetAudioStreamAvailable(stream_);
    if (available <= 0) {
        return 0;
    }
    out.resize(static_cast<size_t>(available) / sizeof(int16_t));
    const int got = SDL_GetAudioStreamData(stream_, out.data(), static_cast<int>(out.size() * sizeof(int16_t)));
    out.resize(got > 0 ? static_cast<size_t>(got) / sizeof(int16_t) : 0);
    if (monitor_ && !out.empty()) {
        if (SDL_GetAudioStreamQueued(monitor_) > sample_rate_) SDL_ClearAudioStream(monitor_);
        SDL_PutAudioStreamData(monitor_, out.data(), static_cast<int>(out.size() * sizeof(int16_t)));
    }
    return out.size();
}

}
