#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <glm/glm.hpp>

#include "engine/render/vk.h"

namespace pt::vk {

inline bool ChooseSwapchainSurfaceFormat(std::span<const VkSurfaceFormatKHR> formats, bool want_hdr, bool allow_hdr,
                                         VkSurfaceFormatKHR& chosen, bool& hdr_selected) {
    hdr_selected = false;
    if (formats.empty()) return false;
    const bool unrestricted = formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED;
    const auto find = [&](VkFormat format, VkColorSpaceKHR color_space) {
        for (const auto& candidate : formats) {
            if (candidate.format == format && candidate.colorSpace == color_space) {
                chosen = candidate;
                return true;
            }
        }
        if (unrestricted && formats[0].colorSpace == color_space) {
            chosen = {format, color_space};
            return true;
        }
        return false;
    };
    if (want_hdr && allow_hdr) {
        hdr_selected = find(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) ||
                       find(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT);
    }
    if (hdr_selected) return true;
    return find(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) ||
           find(VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
}

}

namespace pt::render {

struct PixelRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

inline bool MakePixelCrop(uint32_t source_width, uint32_t source_height, glm::vec4 crop, PixelRect& rect) {
    if (!source_width || !source_height || !std::isfinite(crop.x) || !std::isfinite(crop.y) || !std::isfinite(crop.z) ||
        !std::isfinite(crop.w)) return false;
    crop.x = std::clamp(crop.x, 0.0f, 1.0f);
    crop.y = std::clamp(crop.y, 0.0f, 1.0f);
    crop.z = std::clamp(crop.z, 0.0f, 1.0f - crop.x);
    crop.w = std::clamp(crop.w, 0.0f, 1.0f - crop.y);
    rect.x = static_cast<uint32_t>(std::floor(crop.x * source_width));
    rect.y = static_cast<uint32_t>(std::floor(crop.y * source_height));
    const uint32_t x1 = std::min(source_width, static_cast<uint32_t>(std::ceil((crop.x + crop.z) * source_width)));
    const uint32_t y1 = std::min(source_height, static_cast<uint32_t>(std::ceil((crop.y + crop.w) * source_height)));
    rect.width = x1 - rect.x;
    rect.height = y1 - rect.y;
    return rect.width > 0 && rect.height > 0;
}

inline float PqEncodeNits(float nits) {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

inline float PqDecodeNits(float code) {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const float p = std::pow(std::clamp(code, 0.0f, 1.0f), 1.0f / m2);
    const float numerator = std::max(p - c1, 0.0f);
    const float denominator = std::max(c2 - c3 * p, 1.0e-6f);
    return std::pow(numerator / denominator, 1.0f / m1) * 10000.0f;
}

inline uint8_t LinearToSrgb8(float linear) {
    linear = std::max(linear, 0.0f);
    const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
}

}
