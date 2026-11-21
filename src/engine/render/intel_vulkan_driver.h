#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pt::vk {

constexpr uint32_t kIntelVendorId = 0x8086;
constexpr uint32_t kIntelFirstGoodDriver = (101u << 14) | 2145u;

std::string IntelWindowsDriverName(uint32_t driver_version);

struct WindowsDriverVersion {
    uint16_t field[4] = {};
    bool valid = false;
};
std::string WindowsDriverName(const WindowsDriverVersion& version);
#ifdef _WIN32
WindowsDriverVersion DxgiDriverVersion(const uint8_t (&luid)[8]);
struct DxgiAdapter {
    uint8_t luid[8] = {};
    uint32_t vendor_id = 0;
    std::string name;
};
std::vector<DxgiAdapter> DxgiHardwareAdapters();
#endif

bool IntelDriverCrashesFirstFrame(uint32_t vendor_id, uint32_t driver_version, bool windows, const WindowsDriverVersion& dxgi = {});

}
