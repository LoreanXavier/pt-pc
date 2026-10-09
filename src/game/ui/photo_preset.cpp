#include "game/ui/photo_preset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace pt::game {
namespace {
constexpr std::array<float, 12> kFocalLengthsMm{8.0f, 10.0f, 12.0f, 14.0f, 16.0f, 18.0f, 21.0f, 24.0f, 28.0f, 35.0f, 50.0f, 70.0f};
constexpr std::array<float, static_cast<size_t>(PhotoAspectPreset::Count)> kAspectRatios{0.0f, 2.39f, 1.85f, 4.0f / 3.0f,
                                                                                       3.0f / 2.0f, 1.0f};
}

int PhotoFocalLengthStepCount() {
    return static_cast<int>(kFocalLengthsMm.size());
}

float PhotoFocalLengthForStep(int step) {
    const int index = std::clamp(step, 0, static_cast<int>(kFocalLengthsMm.size()) - 1);
    return kFocalLengthsMm[static_cast<size_t>(index)];
}

int PhotoFocalLengthStep(float focal_length_mm) {
    const auto it = std::min_element(kFocalLengthsMm.begin(), kFocalLengthsMm.end(), [focal_length_mm](float a, float b) {
        return std::abs(a - focal_length_mm) < std::abs(b - focal_length_mm);
    });
    return static_cast<int>(std::distance(kFocalLengthsMm.begin(), it));
}

float PhotoFovYDegrees(float focal_length_mm, float sensor_height_mm) {
    if (!(focal_length_mm > 0.0f) || !(sensor_height_mm > 0.0f)) return 0.0f;
    return 2.0f * std::atan(sensor_height_mm / (2.0f * focal_length_mm)) * 180.0f / 3.14159265358979323846f;
}

float PhotoFocalLengthFromFovYDegrees(float fov_y_degrees, float sensor_height_mm) {
    if (!(fov_y_degrees > 0.0f && fov_y_degrees < 180.0f) || !(sensor_height_mm > 0.0f)) return 0.0f;
    return sensor_height_mm * 0.5f / std::tan(fov_y_degrees * 3.14159265358979323846f / 360.0f);
}

float PhotoAspectRatio(PhotoAspectPreset preset, float source_aspect) {
    const size_t index = static_cast<size_t>(preset);
    if (index == 0 || index >= kAspectRatios.size()) return source_aspect;
    return kAspectRatios[index];
}

PhotoCropRect PhotoCropForAspect(float source_aspect, float target_aspect) {
    if (!(source_aspect > 0.0f) || !(target_aspect > 0.0f)) return {};
    if (target_aspect < source_aspect) {
        const float width = target_aspect / source_aspect;
        return {(1.0f - width) * 0.5f, 0.0f, width, 1.0f};
    }
    const float height = source_aspect / target_aspect;
    return {0.0f, (1.0f - height) * 0.5f, 1.0f, height};
}

float PhotoCropFovYDegrees(float fov_y_degrees, PhotoCropRect crop) {
    if (!(fov_y_degrees > 0.0f && fov_y_degrees < 180.0f) || !(crop.height > 0.0f)) return 0.0f;
    return 2.0f * std::atan(std::tan(fov_y_degrees * 3.14159265358979323846f / 360.0f) * crop.height) *
           180.0f / 3.14159265358979323846f;
}

PhotoExtent PhotoCaptureExtent(uint32_t source_width, uint32_t source_height, PhotoAspectPreset aspect, PhotoResolution resolution) {
    if (source_width == 0 || source_height == 0) return {};
    const float source_aspect = static_cast<float>(source_width) / static_cast<float>(source_height);
    const PhotoCropRect crop = PhotoCropForAspect(source_aspect, PhotoAspectRatio(aspect, source_aspect));
    const float crop_width = static_cast<float>(source_width) * crop.width;
    const float crop_height = static_cast<float>(source_height) * crop.height;
    const float long_edge = std::max(crop_width, crop_height);
    if (!(long_edge > 0.0f)) return {};
    const float scale = resolution == PhotoResolution::FourK ? 3840.0f / long_edge : 1.0f;
    return {static_cast<uint32_t>(std::lround(crop_width * scale)), static_cast<uint32_t>(std::lround(crop_height * scale))};
}

}  // namespace pt::game
