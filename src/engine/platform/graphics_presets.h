#pragma once
#include "engine/platform/settings.h"
namespace pt {
enum class GraphicsPreset { Low, Original, High, Ultra, Custom };
void ApplyGraphicsPreset(AppSettings& settings, GraphicsPreset preset, bool ray_tracing_supported);
GraphicsPreset DetectGraphicsPreset(const AppSettings& settings, bool ray_tracing_supported);
}
