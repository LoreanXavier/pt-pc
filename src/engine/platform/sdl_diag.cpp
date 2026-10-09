#include "engine/platform/sdl_diag.h"

#include <SDL3/SDL.h>

#include "engine/core/log.h"
#include "engine/platform/os.h"

namespace pt {

namespace {

std::string Join(int count, const char* (*driver)(int)) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        if (const char* name = driver(i)) {
            text += (text.empty() ? "" : ", ") + std::string(name);
        }
    }
    return text.empty() ? "none" : text;
}

std::string Variable(const char* name) {
    const std::string value = os::GetEnv(name);
    return std::string(name) + "=" + (value.empty() ? "(unset)" : value);
}

std::string DeviceNames(SDL_AudioDeviceID* devices, int count) {
    std::string text;
    for (int i = 0; devices && i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        text += (text.empty() ? "" : "; ") + std::string(name ? name : "?");
    }
    return text.empty() ? "none" : text;
}

}

std::string SdlCompiledVideoDrivers() {
    return Join(SDL_GetNumVideoDrivers(), SDL_GetVideoDriver);
}

std::string SdlCompiledAudioDrivers() {
    return Join(SDL_GetNumAudioDrivers(), SDL_GetAudioDriver);
}

std::string SdlVideoDiagnostics() {
    const char* hint = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    return std::format("SDL {}.{}.{}, video drivers built in: {}; SDL_VIDEO_DRIVER hint: {}; {}; {}; {}", SDL_MAJOR_VERSION,
                       SDL_MINOR_VERSION, SDL_MICRO_VERSION, SdlCompiledVideoDrivers(), hint && *hint ? hint : "(unset)",
                       Variable("DISPLAY"), Variable("WAYLAND_DISPLAY"), Variable("XDG_SESSION_TYPE"));
}

void LogSdlVideoInUse() {
    const char* driver = SDL_GetCurrentVideoDriver();
    LogInfo("video: SDL driver {} (built in: {})", driver ? driver : "none", SdlCompiledVideoDrivers());
}

void LogSdlAudioDevices(bool recording) {
    const char* driver = SDL_GetCurrentAudioDriver();
    int count = 0;
    SDL_AudioDeviceID* playback = SDL_GetAudioPlaybackDevices(&count);
    LogInfo("audio: SDL driver {} (built in: {}), playback devices: {}", driver ? driver : "none", SdlCompiledAudioDrivers(),
            DeviceNames(playback, count));
    SDL_free(playback);
    if (recording) {
        count = 0;
        SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
        LogInfo("audio: recording devices: {}", DeviceNames(devices, count));
        SDL_free(devices);
    }
}

}