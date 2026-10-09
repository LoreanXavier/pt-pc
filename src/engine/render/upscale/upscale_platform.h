#pragma once

#include <string>

#include "engine/core/resource_path.h"

namespace pt {

std::string NarrowText(const wchar_t* text);
// hardware-accelerated GPU scheduling of the adapter with this LUID (D3DKMT WDDM 2.7 caps): 1 on, 0 off, -1 unknown
int HardwareGpuScheduling(const uint8_t (&luid)[8]);

}
