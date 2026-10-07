#pragma once

#include "engine/assets/ftex.h"
#include <filesystem>
#include <span>

namespace pt {
constexpr uint32_t kEnhancedMaxSource = 2048;
bool EnhancedTextureEligible(std::string_view path, const FtexTexture& source);
bool EncodeEnhancedTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb, FtexTexture& out);
bool WriteTextureCache(const std::filesystem::path& path, uint64_t source, const FtexTexture& texture);
bool ReadTextureCache(const std::filesystem::path& path, uint64_t source, FtexTexture& texture);
}
