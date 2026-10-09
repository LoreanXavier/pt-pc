#pragma once

#include <string>

namespace pt {

std::string SdlCompiledVideoDrivers();
std::string SdlCompiledAudioDrivers();

std::string SdlVideoDiagnostics();
void LogSdlVideoInUse();
void LogSdlAudioDevices(bool recording);

}
