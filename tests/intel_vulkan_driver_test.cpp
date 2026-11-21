#ifdef NDEBUG
#undef NDEBUG
#endif
#include "engine/render/intel_vulkan_driver.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

#include "game/ui/pc_settings.h"

using namespace pt::vk;

namespace {
constexpr uint32_t Intel(uint32_t branch, uint32_t build) { return (branch << 14) | build; }
}

int main() {
    assert(Intel(101, 2127) == 0x19484F);
    assert(IntelWindowsDriverName(0x19484F) == "31.0.101.2127");
    assert(IntelWindowsDriverName(0x194857) == "31.0.101.2135");
    assert(IntelWindowsDriverName(kIntelFirstGoodDriver) == "31.0.101.2145");
    for (uint32_t build : {1994u, 2079u, 2111u, 2114u, 2115u, 2125u, 2127u, 2130u, 2134u, 2135u}) {
        assert(IntelDriverCrashesFirstFrame(0x8086, Intel(101, build), true));
    }
    assert(IntelDriverCrashesFirstFrame(0x8086, Intel(100, 9466), true));
    assert(!IntelDriverCrashesFirstFrame(0x8086, 0x194861, true));
    assert(!IntelDriverCrashesFirstFrame(0x8086, Intel(101, 5522), true));
    assert(!IntelDriverCrashesFirstFrame(0x8086, Intel(101, 7092), true));
    for (uint32_t vendor : {0x10DEu, 0x1002u, 0x1022u, 0x13B5u, 0x5143u, 0x106Bu}) {
        for (uint32_t version : {0x918D4000u, 0x9A438000u, 0x800161u, Intel(101, 2127), Intel(100, 9466), 0u}) {
            assert(!IntelDriverCrashesFirstFrame(vendor, version, true));
        }
    }
    assert(!IntelDriverCrashesFirstFrame(0x8086, (24u << 22) | (2u << 12) | 8u, true));
    assert(!IntelDriverCrashesFirstFrame(0x8086, Intel(101, 2127), false));
    assert(!IntelDriverCrashesFirstFrame(0x8086, 0, true));

    auto dxgi = [](uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
        WindowsDriverVersion v;
        v.field[0] = a;
        v.field[1] = b;
        v.field[2] = c;
        v.field[3] = d;
        v.valid = true;
        return v;
    };
    assert(!IntelDriverCrashesFirstFrame(0x8086, 0x194C27, true));
    assert(IntelDriverCrashesFirstFrame(0x8086, 0x194C27, true, dxgi(30, 0, 101, 3111)));
    assert(WindowsDriverName(dxgi(30, 0, 101, 3111)) == "30.0.101.3111");
    assert(IntelDriverCrashesFirstFrame(0x8086, Intel(101, 2135), true, dxgi(31, 0, 101, 2135)));
    assert(IntelDriverCrashesFirstFrame(0x8086, Intel(100, 9466), true, dxgi(27, 20, 100, 9466)));
    assert(!IntelDriverCrashesFirstFrame(0x8086, Intel(101, 2145), true, dxgi(31, 0, 101, 2145)));
    assert(!IntelDriverCrashesFirstFrame(0x8086, Intel(101, 5522), true, dxgi(31, 0, 101, 5522)));
    assert(!IntelDriverCrashesFirstFrame(0x8086, Intel(101, 8993), true, dxgi(32, 0, 101, 8993)));
    assert(!IntelDriverCrashesFirstFrame(0x10DE, 0x9B404000, true, dxgi(32, 0, 16, 2101)));

    const std::string_view english_title = pt::game::PcText("pc_intel_driver_title", 0);
    const std::string_view english = pt::game::PcText("pc_intel_driver_prompt", 0);
    assert(english_title == "Intel graphics driver");
    assert(english.find("31.0.101.2145") != std::string_view::npos);
    for (int language = 0; language < 14; ++language) {
        const std::string_view title = pt::game::PcText("pc_intel_driver_title", language);
        const std::string_view text = pt::game::PcText("pc_intel_driver_prompt", language);
        assert(!title.empty() && title != "pc_intel_driver_title");
        assert(!text.empty() && text != "pc_intel_driver_prompt");
        const size_t at = text.find("{1}");
        assert(at != std::string_view::npos && text.find("{1}", at + 1) == std::string_view::npos);
        assert(text.find("intel.com") != std::string_view::npos && text.find("31.0.101.2145") != std::string_view::npos);
        if (language > 0) {
            assert(text != english);
        }
        std::printf("%2d %.*s: %.*s\n", language, int(title.size()), title.data(), int(text.size()), text.data());
    }
    std::printf("intel_vulkan_driver_test: ok\n");
    return 0;
}
