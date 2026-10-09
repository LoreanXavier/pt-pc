#include "engine/render/hdr_output.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };

    const std::array<VkSurfaceFormatKHR, 3> all_formats = {{{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
                                                            {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                            {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT}}};
    VkSurfaceFormatKHR selected{};
    bool hdr = false;
    check(pt::vk::ChooseSwapchainSurfaceFormat(all_formats, true, true, selected, hdr) && hdr &&
              selected.format == VK_FORMAT_R16G16B16A16_SFLOAT && selected.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
          "scRGB is preferred when both HDR formats are available");
    const std::array<VkSurfaceFormatKHR, 2> hdr10_formats = {all_formats[0], all_formats[1]};
    check(pt::vk::ChooseSwapchainSurfaceFormat(hdr10_formats, true, true, selected, hdr) && hdr &&
              selected.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32,
          "HDR10 is selected when scRGB is unavailable");
    check(pt::vk::ChooseSwapchainSurfaceFormat(hdr10_formats, true, false, selected, hdr) && !hdr &&
              selected.format == VK_FORMAT_B8G8R8A8_UNORM,
          "swapchain hooks fall back to SDR");
    const std::array<VkSurfaceFormatKHR, 1> unrestricted = {{{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}}};
    check(pt::vk::ChooseSwapchainSurfaceFormat(unrestricted, false, false, selected, hdr) &&
              selected.format == VK_FORMAT_B8G8R8A8_UNORM,
          "undefined surface format resolves to a concrete SDR format");
    check(!pt::vk::ChooseSwapchainSurfaceFormat({}, true, true, selected, hdr), "empty format list is rejected safely");

    bool transfer_ok = true;
    for (float nits : {0.0f, 0.1f, 80.0f, 203.0f, 1000.0f, 10000.0f}) {
        const float roundtrip = pt::render::PqDecodeNits(pt::render::PqEncodeNits(nits));
        transfer_ok &= std::abs(roundtrip - nits) <= std::max(0.02f, nits * 0.0001f);
    }
    check(transfer_ok, "PQ encode and decode round-trip from black through 10000 nits");
    check(pt::render::LinearToSrgb8(0.0f) == 0 && pt::render::LinearToSrgb8(1.0f) == 255,
          "linear-to-sRGB PNG transfer has exact black and white endpoints");

    pt::render::PixelRect rect;
    check(pt::render::MakePixelCrop(3840, 2160, {0.25f, 0.25f, 0.5f, 0.5f}, rect) && rect.x == 960 && rect.y == 540 &&
              rect.width == 1920 && rect.height == 1080,
          "normalized photo crop maps to the requested pixel rectangle");
    check(!pt::render::MakePixelCrop(3840, 2160, {0.5f, 0.5f, 0.0f, 0.5f}, rect), "empty screenshot crop is rejected");
    check(!pt::render::MakePixelCrop(3840, 2160, {0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f}, rect),
          "non-finite screenshot crop is rejected");
    return failures ? 1 : 0;
}
