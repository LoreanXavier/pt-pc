#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace pt::render {

struct HdrPngStats {
    float max_cll = 0.0f;
    float max_fall = 0.0f;
};
HdrPngStats MeasureHdrLevels(std::span<const float> rgb709_nits);
std::vector<uint8_t> EncodeHdrPng(uint32_t width, uint32_t height, std::span<const float> rgb709_nits);
bool WriteHdrPng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgb709_nits);

}
