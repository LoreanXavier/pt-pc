#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>
#include <glm/glm.hpp>

#include "engine/render/vk.h"

namespace pt::vk {

inline bool ChooseSwapchainSurfaceFormat(std::span<const VkSurfaceFormatKHR> formats, bool want_hdr, bool allow_hdr,
                                         VkSurfaceFormatKHR& chosen, bool& hdr_selected, bool allow_scrgb = true) {
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
        hdr_selected = (allow_scrgb && find(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)) ||
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

constexpr int kHdrPaperWhiteDefault = 200;
constexpr int kHdrPaperWhiteMin = 80;
constexpr int kHdrPaperWhiteMax = 500;
constexpr int kHdrPeakFallback = 1000;
constexpr int kHdrPeakMin = 400;
constexpr int kHdrPeakMax = 4000;

inline float ClampHdrPaperWhite(float nits) {
    return std::isfinite(nits) ? std::clamp(nits, float(kHdrPaperWhiteMin), float(kHdrPaperWhiteMax)) : float(kHdrPaperWhiteDefault);
}

inline float ResolveHdrPeak(float setting, float display) {
    const float chosen = setting > 0.0f ? setting : display > 0.0f ? display : float(kHdrPeakFallback);
    return std::isfinite(chosen) ? std::clamp(chosen, float(kHdrPeakMin), float(kHdrPeakMax)) : float(kHdrPeakFallback);
}

inline float SrgbEncodeLinear(float linear) {
    linear = std::max(linear, 0.0f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

inline float SrgbDecodeLinear(float encoded) {
    encoded = std::max(encoded, 0.0f);
    return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

inline float HdrShoulderAsymptote(float ratio) {
    ratio = std::isfinite(ratio) ? std::clamp(ratio, 1.0f, 64.0f) : 1.0f;
    return SrgbDecodeLinear(std::pow(ratio, 1.0f / 2.2f));
}

constexpr float kHdrRollKnee = 0.8f;
inline float HdrPeakRoll(float light, float ratio) {
    const float knee = kHdrRollKnee * ratio;
    if (!(light > knee)) return std::max(light, 0.0f);
    const float room = ratio - knee;
    return knee + room * (1.0f - std::exp(-(light - knee) / room));
}

inline float MeterWeighted(const glm::vec2* cells, uint32_t count) {
    struct Cell {
        float value;
        float weight;
    };
    std::vector<Cell> list;
    list.reserve(count);
    double total = 0.0;
    const int side = 32;
    for (uint32_t i = 0; i < count; ++i) {
        const float value = cells[i].x;
        if (cells[i].y <= 0.0f || !std::isfinite(value)) {
            continue;
        }
        const float x = (static_cast<float>(i % side) + 0.5f) / side - 0.5f;
        const float y = (static_cast<float>(i / side) + 0.5f) / side - 0.5f;
        const float weight = cells[i].y * (0.6f + 0.4f * std::exp(-(x * x + y * y) / (2.0f * 0.28f * 0.28f)));
        list.push_back({std::max(value, 0.0f), weight});
        total += weight;
    }
    if (list.empty() || total <= 0.0) {
        return -1.0f;
    }
    std::sort(list.begin(), list.end(), [](const Cell& a, const Cell& b) { return a.value < b.value; });
    const double low = 0.02 * total;
    const double high = 0.98 * total;
    double at = 0.0;
    double sum = 0.0;
    double kept = 0.0;
    for (const Cell& c : list) {
        const double next = at + c.weight;
        const double share = std::max(0.0, std::min(next, high) - std::max(at, low));
        sum += share * c.value;
        kept += share;
        at = next;
    }
    return kept > 0.0 ? static_cast<float>(sum / kept) : -1.0f;
}

inline float AdaptEv(float ev, float target, float dt, float speed) {
    constexpr float kAdaptJoin = 1.5f;
    if (!(dt > 0.0f)) {
        return ev;
    }
    dt = std::min(dt, 0.1f);
    const float scale = std::max(speed, 0.0f) / 1.6f;
    const float rate = (target < ev ? 3.0f : 1.5f) * scale;
    if (rate <= 0.0f) {
        return ev;
    }
    const float side = target < ev ? -1.0f : 1.0f;
    float gap = std::fabs(target - ev);
    if (gap > kAdaptJoin) {
        const float steady = (gap - kAdaptJoin) / rate;
        if (dt <= steady) {
            return target - side * (gap - rate * dt);
        }
        dt -= steady;
        gap = kAdaptJoin;
    }
    return target - side * gap * std::exp(-dt * rate / kAdaptJoin);
}

struct HdrLook {
    int gamma = 0;
    bool keep_hue = false;
    int colour_range = 0;
    float exposure = 0.0f;
    float contrast = 1.0f;
    float highlights = 1.0f;
    float shadows = 1.0f;
    float saturation = 1.0f;
    float highlight_saturation = 1.0f;
    float blowout = 0.0f;
    bool Graded() const {
        return exposure != 0.0f || contrast != 1.0f || highlights != 1.0f || shadows != 1.0f || saturation != 1.0f ||
               highlight_saturation != 1.0f || blowout != 0.0f;
    }
    bool operator==(const HdrLook&) const = default;
};

inline uint8_t LinearToSrgb8(float linear) {
    linear = std::max(linear, 0.0f);
    const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
}

}
