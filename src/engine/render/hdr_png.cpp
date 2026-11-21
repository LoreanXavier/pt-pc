#include "engine/render/hdr_png.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <fstream>

#include "engine/render/hdr_output.h"

namespace pt::render {
namespace {

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void Chunk(std::vector<uint8_t>& out, const char (&type)[5], const std::vector<uint8_t>& data) {
    Put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uLong crc = crc32(0L, out.data() + start, static_cast<uInt>(out.size() - start));
    Put32(out, static_cast<uint32_t>(crc));
}

}

HdrPngStats MeasureHdrLevels(std::span<const float> rgb709_nits) {
    HdrPngStats stats;
    const size_t pixels = rgb709_nits.size() / 3;
    double sum = 0.0;
    for (size_t i = 0; i < pixels; ++i) {
        const float strongest = std::max({rgb709_nits[i * 3], rgb709_nits[i * 3 + 1], rgb709_nits[i * 3 + 2], 0.0f});
        if (!std::isfinite(strongest)) continue;
        stats.max_cll = std::max(stats.max_cll, strongest);
        sum += strongest;
    }
    stats.max_fall = pixels ? static_cast<float>(sum / static_cast<double>(pixels)) : 0.0f;
    return stats;
}

std::vector<uint8_t> EncodeHdrPng(uint32_t width, uint32_t height, std::span<const float> rgb709_nits) {
    if (!width || !height || rgb709_nits.size() != size_t(width) * height * 3) return {};
    std::vector<uint8_t> raw;
    raw.reserve((size_t(width) * 6 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x) {
            const float* p = rgb709_nits.data() + (size_t(y) * width + x) * 3;
            const float r = std::isfinite(p[0]) ? std::max(p[0], 0.0f) : 0.0f;
            const float g = std::isfinite(p[1]) ? std::max(p[1], 0.0f) : 0.0f;
            const float b = std::isfinite(p[2]) ? std::max(p[2], 0.0f) : 0.0f;
            const float rgb2020[3] = {0.627404f * r + 0.329283f * g + 0.0433136f * b, 0.069097f * r + 0.919540f * g + 0.0113612f * b,
                                      0.0163916f * r + 0.0880132f * g + 0.895595f * b};
            for (float channel : rgb2020) {
                const uint16_t code = static_cast<uint16_t>(std::lround(std::clamp(PqEncodeNits(channel), 0.0f, 1.0f) * 65535.0f));
                raw.push_back(static_cast<uint8_t>(code >> 8));
                raw.push_back(static_cast<uint8_t>(code));
            }
        }
    }
    uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return {};
    packed.resize(packed_size);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> header;
    Put32(header, width);
    Put32(header, height);
    header.insert(header.end(), {16, 2, 0, 0, 0});
    Chunk(out, "IHDR", header);
    Chunk(out, "cICP", {9, 16, 0, 1});
    const HdrPngStats stats = MeasureHdrLevels(rgb709_nits);
    std::vector<uint8_t> levels;
    Put32(levels, static_cast<uint32_t>(std::lround(std::min(stats.max_cll, 10000.0f) * 10000.0f)));
    Put32(levels, static_cast<uint32_t>(std::lround(std::min(stats.max_fall, 10000.0f) * 10000.0f)));
    Chunk(out, "cLLi", levels);
    Chunk(out, "IDAT", packed);
    Chunk(out, "IEND", {});
    return out;
}

bool WriteHdrPng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const float> rgb709_nits) {
    const std::vector<uint8_t> png = EncodeHdrPng(width, height, rgb709_nits);
    if (png.empty()) return false;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(file);
}

}
