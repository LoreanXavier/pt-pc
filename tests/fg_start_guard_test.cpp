#include <cstdio>
#include <sstream>

#include "engine/render/upscale/fg_start_guard.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
}

int main() {
    {
        std::istringstream log(
            "[    8.822] info  frame generation: FSR 3 frame generation on DirectX 12, the swapchain is made again\n"
            "[    9.522] info  fsr fg d3d12: FSR 3 frame generation swapchain 1920x1080, 3 buffers\n"
            "[    9.674] error d3d12 backend: device lost at an image creation: the DirectX 12 device was removed (0x887a002b)\n");
        const pt::FgStartLoss loss = pt::FindFrameGenStartLoss(log);
        Check(loss.found && loss.owner == "FSR 3 frame generation" && loss.seconds > 0.8 && loss.seconds < 0.9, "FSR start loss");
    }
    {
        std::istringstream log(
            "[  165.493] info  frame generation: XeSS Frame Generation on DirectX 12, the swapchain is made again\n"
            "[  166.581] error d3d12 backend: device lost at present: the DirectX 12 device was removed (0x887a002b)\n");
        Check(pt::FindFrameGenStartLoss(log).owner == "XeSS Frame Generation", "XeSS start loss");
    }
    {
        std::istringstream log(
            "[   10.000] info  frame generation: DLSS Frame Generation on DirectX 12, the swapchain is made again\n"
            "[  900.000] error d3d12 backend: device lost at present: the DirectX 12 device was removed (0x887a0006)\n");
        Check(!pt::FindFrameGenStartLoss(log).found, "a late loss");
    }
    {
        std::istringstream log(
            "[   10.000] info  frame generation: DLSS Frame Generation on DirectX 12, the swapchain is made again\n"
            "[   12.000] info  frame generation: off on DirectX 12, the swapchain is made again\n"
            "[   13.000] error d3d12 backend: device lost at present: the DirectX 12 device was removed (0x887a0006)\n");
        Check(!pt::FindFrameGenStartLoss(log).found, "off before the loss");
    }
    {
        std::istringstream log("[    1.000] info  boot\n[    2.000] error d3d12 backend: device lost at present\n");
        Check(!pt::FindFrameGenStartLoss(log).found, "a loss without frame generation");
    }
    if (failures) return 1;
    std::printf("fg start guard test passed\n");
    return 0;
}
