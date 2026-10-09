#pragma once

#include <string>

namespace pt {

// "x11, wayland, offscreen, dummy": the SDL drivers compiled into this binary, in SDL's try order
std::string SdlCompiledVideoDrivers();
std::string SdlCompiledAudioDrivers();

// One line each for the log: why SDL_Init(VIDEO) can fail (compiled drivers, SDL_VIDEO_DRIVER, DISPLAY, WAYLAND_DISPLAY, XDG_SESSION_TYPE)
// and what the running audio driver sees (name, playback and recording devices; recording only when asked)
std::string SdlVideoDiagnostics();
void LogSdlVideoInUse();
void LogSdlAudioDevices(bool recording);

}