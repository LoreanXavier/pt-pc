#include "engine/render/hdr_output.h"
#include "engine/render/hdr_png.h"

#include <array>
#include <cmath>
#include <vector>
#include <cstring>
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
    check(pt::vk::ChooseSwapchainSurfaceFormat(all_formats, true, true, selected, hdr, false) && hdr &&
              selected.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && selected.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT,
          "DLSS frame generation uses HDR10 instead of unsupported scRGB");
    const std::array<VkSurfaceFormatKHR, 2> scrgb_only = {all_formats[1], all_formats[2]};
    check(pt::vk::ChooseSwapchainSurfaceFormat(scrgb_only, true, true, selected, hdr, false) && !hdr &&
              selected.format == VK_FORMAT_B8G8R8A8_UNORM,
          "DLSS frame generation falls back to SDR when HDR10 is unavailable");
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

    {
        bool asymptote = true;
        for (float ratio : {1.0f, 2.0f, 5.0f, 12.5f, 50.0f}) {
            const float a = pt::render::HdrShoulderAsymptote(ratio);
            const float shown = std::pow(pt::render::SrgbEncodeLinear(a), 2.2f);
            asymptote &= std::abs(shown - ratio) <= ratio * 1.0e-4f;
        }
        check(asymptote, "shoulder asymptote shows at the peak");
        check(pt::render::HdrShoulderAsymptote(1.0f) == 1.0f, "no headroom: the asymptote is paper white");
        bool identity = true, monotonic = true, bounded = true, smooth = true;
        for (float ratio : {1.2f, 2.0f, 5.0f, 10.0f, 50.0f}) {
            float previous = 0.0f;
            for (float m = 0.0f; m <= 200.0f; m += 0.01f) {
                const float s = pt::render::HdrPeakRoll(m, ratio);
                if (m <= pt::render::kHdrRollKnee * ratio) identity &= s == m;
                monotonic &= s >= previous - 1.0e-5f;
                bounded &= s <= ratio;
                smooth &= s - previous <= 0.01f + 1.0e-4f;
                previous = s;
            }
        }
        check(identity, "peak roll leaves the light below its knee untouched");
        check(monotonic, "peak roll never falls");
        check(bounded, "peak roll never passes the peak");
        check(smooth, "peak roll is never steeper than the light itself");
        check(pt::render::ResolveHdrPeak(0.0f, 0.0f) == 1000.0f && pt::render::ResolveHdrPeak(0.0f, 800.0f) == 800.0f &&
                  pt::render::ResolveHdrPeak(1500.0f, 800.0f) == 1500.0f && pt::render::ResolveHdrPeak(0.0f, 20000.0f) == 4000.0f &&
                  pt::render::ResolveHdrPeak(100.0f, 0.0f) == 400.0f,
              "peak: the setting, else the display's, else 1000, within 400 to 4000");
        check(pt::render::ClampHdrPaperWhite(10.0f) == 80.0f && pt::render::ClampHdrPaperWhite(900.0f) == 500.0f &&
                  pt::render::ClampHdrPaperWhite(200.0f) == 200.0f,
              "paper white is kept within 80 to 500");
    }

    {
        const std::vector<float> pixels = {203.0f, 203.0f, 203.0f, 1000.0f, 0.0f, 0.0f};
        const std::vector<uint8_t> png = pt::render::EncodeHdrPng(2, 1, pixels);
        auto find = [&](const char* type) {
            for (size_t i = 8; i + 8 <= png.size();) {
                const uint32_t length = (uint32_t(png[i]) << 24) | (uint32_t(png[i + 1]) << 16) | (uint32_t(png[i + 2]) << 8) | png[i + 3];
                if (std::memcmp(png.data() + i + 4, type, 4) == 0) return i;
                i += 12 + length;
            }
            return size_t(0);
        };
        const size_t cicp = find("cICP");
        const size_t clli = find("cLLi");
        const size_t idat = find("IDAT");
        check(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && cicp && clli && idat && cicp < idat && clli < idat,
              "HDR photo: PNG with cICP and cLLi before the image data");
        check(cicp && png[cicp + 8] == 9 && png[cicp + 9] == 16 && png[cicp + 10] == 0 && png[cicp + 11] == 1,
              "HDR photo: cICP is BT.2020 primaries, PQ transfer, RGB, full range");
        const uint32_t max_cll = clli ? (uint32_t(png[clli + 8]) << 24) | (uint32_t(png[clli + 9]) << 16) | (uint32_t(png[clli + 10]) << 8) | png[clli + 11] : 0;
        check(max_cll == 10000000u, "HDR photo: cLLi MaxCLL is the brightest channel (1000 nits)");
        const pt::render::HdrPngStats stats = pt::render::MeasureHdrLevels(pixels);
        check(std::abs(stats.max_fall - 601.5f) < 0.01f, "HDR photo: MaxFALL is the mean of the brightest channels");
        check(pt::render::EncodeHdrPng(3, 1, pixels).empty(), "HDR photo: a pixel count that does not match is refused");
    }

    {
        const auto run = [](float fps, float seconds, float from, float to) {
            float ev = from;
            for (int i = 0; i < static_cast<int>(seconds * fps); ++i) ev = pt::render::AdaptEv(ev, to, 1.0f / fps, 1.6f);
            return ev;
        };
        for (const float to : {-6.0f, 2.0f}) {
            const float reference = run(240.0f, 1.0f, -2.0f, to);
            check(std::fabs(run(30.0f, 1.0f, -2.0f, to) - reference) < 2.0e-3f && std::fabs(run(60.0f, 1.0f, -2.0f, to) - reference) < 2.0e-3f,
                  "HDR adaptation: the frame rate does not change the path");
            check(std::fabs(run(60.0f, 20.0f, -2.0f, to) - to) < 1.0e-3f, "HDR adaptation: the target is reached");
        }
        check(pt::render::AdaptEv(-3.0f, 0.0f, 0.0f, 1.6f) == -3.0f, "HDR adaptation: no time, no step");
        check(pt::render::AdaptEv(-3.0f, 0.0f, 100.0f, 1.6f) < -2.0f, "HDR adaptation: a stall counts as 100 ms");
        std::vector<glm::vec2> cells(1024, glm::vec2(0.5f, 1.0f));
        check(std::fabs(pt::render::MeterWeighted(cells.data(), 1024) - 0.5f) < 1.0e-5f, "HDR meter: an even view reads its level");
        for (int i = 0; i < 10; ++i) cells[500 + i].x = 1.0e5f;
        check(std::fabs(pt::render::MeterWeighted(cells.data(), 1024) - 0.5f) < 1.0e-5f, "HDR meter: a few bright cells are left out");
        for (auto& c : cells) c.y = 0.0f;
        check(pt::render::MeterWeighted(cells.data(), 1024) < 0.0f, "HDR meter: no surface, no reading");
    }

    pt::render::PixelRect rect;
    check(pt::render::MakePixelCrop(3840, 2160, {0.25f, 0.25f, 0.5f, 0.5f}, rect) && rect.x == 960 && rect.y == 540 &&
              rect.width == 1920 && rect.height == 1080,
          "normalized photo crop maps to the requested pixel rectangle");
    check(!pt::render::MakePixelCrop(3840, 2160, {0.5f, 0.5f, 0.0f, 0.5f}, rect), "empty screenshot crop is rejected");
    check(!pt::render::MakePixelCrop(3840, 2160, {0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f}, rect),
          "non-finite screenshot crop is rejected");
    return failures ? 1 : 0;
}
