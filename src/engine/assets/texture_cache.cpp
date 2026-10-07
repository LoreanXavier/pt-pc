#include "engine/assets/texture_cache.h"

#include <bc7enc.h>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>

namespace pt {
namespace {
constexpr uint32_t kMagic = 0x31585450;
constexpr uint32_t kMaxSide = 4096;
uint32_t Levels(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while (w > 1 || h > 1) { w = std::max(1u, w / 2); h = std::max(1u, h / 2); ++n; }
    return n;
}
size_t Bytes(uint32_t w, uint32_t h) { return size_t((w + 3) / 4) * ((h + 3) / 4) * 16; }
float Linear(uint8_t v) {
    const float f = v / 255.0f;
    return f <= 0.04045f ? f / 12.92f : std::pow((f + 0.055f) / 1.055f, 2.4f);
}
uint8_t Gamma(float f) {
    f = f <= 0.0031308f ? f * 12.92f : 1.055f * std::pow(f, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(std::lround(f * 255), 0l, 255l));
}
}

bool EnhancedTextureEligible(std::string_view path, const FtexTexture& t) {
    if (t.NormalMap() || t.Cube() || t.faces != 1 || t.depth != 1 || t.pixel_format != 2 || !t.Srgb() ||
        !t.width || !t.height || t.width > kEnhancedMaxSource || t.height > kEnhancedMaxSource) return false;
    if (path.find("/ui/") != path.npos || path.find("/effect/") != path.npos || path.find("/decal/") != path.npos) return false;
    return path.ends_with("_bsm") || path.ends_with("_lym") || path.ends_with("_ils");
}

bool EncodeEnhancedTexture(uint32_t w, uint32_t h, std::span<const uint8_t> rgba, bool srgb, FtexTexture& out) {
    if (!w || !h || w > kMaxSide || h > kMaxSide || rgba.size() != size_t(w) * h * 4) return false;
    static std::once_flag init;
    std::call_once(init, bc7enc_compress_block_init);
    out = {};
    out.width = w; out.height = h; out.faces = 1; out.depth = 1; out.flags = srgb ? 2 : 0;
    out.pixel_format = 8; out.mip_count = Levels(w, h);
    std::vector<uint8_t> pixels(rgba.begin(), rgba.end());
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_max_partitions = 8;
    for (uint32_t level = 0; level < out.mip_count; ++level) {
        auto& mip = out.mips.emplace_back(Bytes(w, h));
        for (uint32_t by = 0; by < (h + 3) / 4; ++by) for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx) {
            uint8_t block[64];
            for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
                const size_t src = (size_t(std::min(by * 4 + y, h - 1)) * w + std::min(bx * 4 + x, w - 1)) * 4;
                std::copy_n(pixels.data() + src, 4, block + (y * 4 + x) * 4);
            }
            bc7enc_compress_block(mip.data() + (size_t(by) * ((w + 3) / 4) + bx) * 16, block, &params);
        }
        if (w == 1 && h == 1) break;
        const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<uint8_t> next(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; ++y) for (uint32_t x = 0; x < nw; ++x) for (uint32_t c = 0; c < 4; ++c) {
            float sum = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) for (uint32_t dx = 0; dx < 2; ++dx) {
                const uint8_t v = pixels[(size_t(std::min(y * 2 + dy, h - 1)) * w + std::min(x * 2 + dx, w - 1)) * 4 + c];
                sum += srgb && c < 3 ? Linear(v) : v / 255.0f;
            }
            next[(size_t(y) * nw + x) * 4 + c] = srgb && c < 3 ? Gamma(sum * 0.25f) : static_cast<uint8_t>(std::lround(sum * 63.75f));
        }
        pixels = std::move(next); w = nw; h = nh;
    }
    return true;
}

bool WriteTextureCache(const std::filesystem::path& path, uint64_t source, const FtexTexture& t) {
    if (!t.width || !t.height || t.width > kMaxSide || t.height > kMaxSide || t.pixel_format != 8 ||
        t.faces != 1 || t.depth != 1 || t.mips.size() != Levels(t.width, t.height)) return false;
    const auto temp = std::filesystem::path(path.wstring() + L".partial");
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    const uint32_t header[] = {kMagic, 1, t.width, t.height, t.flags & 2, static_cast<uint32_t>(t.mips.size())};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&source), sizeof(source));
    uint32_t w = t.width, h = t.height;
    for (const auto& mip : t.mips) {
        if (mip.size() != Bytes(w, h)) { file.close(); std::filesystem::remove(temp); return false; }
        const uint32_t crc = crc32(0, mip.data(), static_cast<uInt>(mip.size()));
        file.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
        file.write(reinterpret_cast<const char*>(mip.data()), mip.size());
        w = std::max(1u, w / 2); h = std::max(1u, h / 2);
    }
    file.flush();
    const bool ok = file.good();
    file.close();
    std::error_code ec;
    if (!ok) { std::filesystem::remove(temp, ec); return false; }
    std::filesystem::remove(path, ec);
    ec.clear(); std::filesystem::rename(temp, path, ec);
    if (ec) { std::filesystem::remove(temp, ec); return false; }
    return true;
}

bool ReadTextureCache(const std::filesystem::path& path, uint64_t source, FtexTexture& t) {
    std::ifstream file(path, std::ios::binary);
    uint32_t header[6]{}; uint64_t stored_source = 0;
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    file.read(reinterpret_cast<char*>(&stored_source), sizeof(stored_source));
    const uint32_t w0 = header[2], h0 = header[3];
    if (!file || header[0] != kMagic || header[1] != 1 || stored_source != source || !w0 || !h0 ||
        w0 > kMaxSide || h0 > kMaxSide || (header[4] & ~2u) || header[5] != Levels(w0, h0)) return false;
    FtexTexture result;
    result.width = w0; result.height = h0; result.flags = header[4]; result.pixel_format = 8; result.mip_count = header[5];
    uint32_t w = w0, h = h0;
    for (uint32_t i = 0; i < result.mip_count; ++i) {
        uint32_t crc = 0;
        file.read(reinterpret_cast<char*>(&crc), sizeof(crc));
        auto& mip = result.mips.emplace_back(Bytes(w, h));
        file.read(reinterpret_cast<char*>(mip.data()), mip.size());
        if (!file || crc != crc32(0, mip.data(), static_cast<uInt>(mip.size()))) return false;
        w = std::max(1u, w / 2); h = std::max(1u, h / 2);
    }
    if (file.peek() != std::char_traits<char>::eof()) return false;
    t = std::move(result);
    return true;
}
}
