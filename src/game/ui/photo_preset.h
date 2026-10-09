#pragma once

#include <cstdint>

namespace pt::game {

enum class PhotoAspectPreset : uint8_t { Off, Cinema239, Cinema185, FourByThree, ThreeByTwo, Square, Count };
enum class PhotoResolution : uint8_t { Native, FourK, Count };
enum class PhotoColorFilter : uint8_t { Off, BlackAndWhite, Warm, Cool, Muted, Count };

struct PhotoExtent {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct PhotoCropRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

int PhotoFocalLengthStepCount();
float PhotoFocalLengthForStep(int step);
int PhotoFocalLengthStep(float focal_length_mm);
float PhotoFovYDegrees(float focal_length_mm, float sensor_height_mm = 13.5f);
float PhotoFocalLengthFromFovYDegrees(float fov_y_degrees, float sensor_height_mm = 13.5f);

float PhotoAspectRatio(PhotoAspectPreset preset, float source_aspect);
PhotoCropRect PhotoCropForAspect(float source_aspect, float target_aspect);
float PhotoCropFovYDegrees(float fov_y_degrees, PhotoCropRect crop);
PhotoExtent PhotoCaptureExtent(uint32_t source_width, uint32_t source_height, PhotoAspectPreset aspect, PhotoResolution resolution);

}
