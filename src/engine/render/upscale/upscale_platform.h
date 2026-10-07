#pragma once

#include <string>

#include "engine/core/resource_path.h"

namespace pt {

std::string NarrowText(const wchar_t* text);
int HardwareGpuScheduling(const uint8_t (&luid)[8]);

}
