#include "engine/assets/ftex.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

#include "engine/core/log.h"
#include "engine/core/pathcode.h"
#include "engine/fs/mods.h"

namespace pt {
namespace {

template <typename T>
T Read(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

bool ReadChunked(const std::vector<uint8_t>& file, size_t block, uint32_t unpacked, uint16_t chunk_count, std::vector<uint8_t>& out) {
    out.resize(unpacked);
    size_t written = 0;
    for (uint16_t c = 0; c < chunk_count; ++c) {
        const size_t entry = block + c * 8;
        const uint16_t stored_size = Read<uint16_t>(file, entry);
        const uint16_t chunk_unpacked = Read<uint16_t>(file, entry + 2);
        const uint32_t offset_field = Read<uint32_t>(file, entry + 4);
        const bool raw = (offset_field & 0x80000000u) != 0;
        const size_t chunk = block + (offset_field & 0x7FFFFFFFu);
        const size_t size = chunk_unpacked ? chunk_unpacked : 0x10000;
        if (written + size > out.size() || chunk + stored_size > file.size()) {
            return false;
        }
        if (raw) {
            std::memcpy(out.data() + written, file.data() + chunk, std::min<size_t>(stored_size, size));
        } else {
            uLongf length = static_cast<uLongf>(size);
            if (uncompress(out.data() + written, &length, file.data() + chunk, stored_size) != Z_OK) {
                return false;
            }
        }
        written += size;
    }
    return written == unpacked;
}

}

VkFormat FtexTexture::Format() const {
    switch (pixel_format) {
    case 0: return Srgb() ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    case 1: return VK_FORMAT_R8_UNORM;
    case 2: return Srgb() ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 3: return Srgb() ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
    case 4: return Srgb() ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
    case 5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 6: return VK_FORMAT_R32_SFLOAT;
    case 8: return Srgb() ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

std::string FtexStem(std::string_view path) {
    const size_t name_start = path.find_last_of('/') == std::string_view::npos ? 0 : path.find_last_of('/') + 1;
    const size_t dot = path.find('.', name_start);
    return std::string(dot == std::string_view::npos ? path : path.substr(0, dot));
}

std::optional<std::vector<uint8_t>> ReadTextureFile(const QarArchive& qar, const std::string& name) {
    if (auto data = mods::ReadOverride(name)) {
        return data;
    }
    return qar.Read(PathCode64(name));
}

bool LoadFtex(const QarArchive& qar, std::string_view path, FtexTexture& out) {
    const std::string stem = FtexStem(path);
    auto header = mods::ReadOverride(stem + ".ftex");
    const bool modded = header.has_value();
    if (!modded) {
        header = qar.Read(PathCode64(stem + ".ftex"));
    }
    if (!header || header->size() < 0x40 || std::memcmp(header->data(), "FTEX", 4) != 0) {
        return false;
    }
    out.pixel_format = Read<uint16_t>(*header, 0x08);
    out.width = Read<uint16_t>(*header, 0x0A);
    out.height = Read<uint16_t>(*header, 0x0C);
    out.depth = std::max<uint16_t>(1, Read<uint16_t>(*header, 0x0E));
    out.mip_count = Read<uint8_t>(*header, 0x10);
    out.filter = Read<uint8_t>(*header, 0x11);
    out.address_mode = Read<uint16_t>(*header, 0x12);
    out.flags = Read<uint32_t>(*header, 0x1C);
    out.faces = out.Cube() ? 6 : 1;
    const uint32_t entries = out.mip_count * out.faces;
    std::map<uint8_t, std::optional<std::vector<uint8_t>>> files;
    out.mips.assign(entries, {});
    for (uint32_t i = 0; i < entries; ++i) {
        const size_t e = 0x40 + size_t(i) * 16;
        const uint32_t offset = Read<uint32_t>(*header, e);
        const uint32_t unpacked = Read<uint32_t>(*header, e + 4);
        const uint8_t file_number = Read<uint8_t>(*header, e + 0x0D);
        const uint16_t chunk_count = Read<uint16_t>(*header, e + 0x0E);
        const std::vector<uint8_t>* source = nullptr;
        size_t block = offset;
        if (file_number == 0) {
            source = &*header;
        } else {
            auto& slot = files[file_number];
            if (!slot) {
                const std::string name = stem + "." + std::to_string(file_number) + ".ftexs";
                slot = modded ? mods::ReadOverride(name) : qar.Read(PathCode64(name));
                if (!slot) {
                    slot = std::vector<uint8_t>();
                }
            }
            source = &*slot;
        }
        if (source->empty()) {
            continue;
        }
        if (out.flags & 1) {
            if (!ReadChunked(*source, block, unpacked, chunk_count, out.mips[i])) {
                LogWarn("ftex: {} mip entry {} failed to inflate", stem, i);
                out.mips[i].clear();
            }
        } else if (block + unpacked <= source->size()) {
            out.mips[i].assign(source->begin() + block, source->begin() + block + unpacked);
        }
    }
    return true;
}

}

namespace pt {
namespace {

void Expand565(uint16_t c, uint8_t* out) {
    out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
    out[3] = 255;
}

void DecodeColorBlock(const uint8_t* b, bool four_colors, uint8_t out[16][4]) {
    const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    uint8_t palette[4][4];
    Expand565(c0, palette[0]);
    Expand565(c1, palette[1]);
    for (int i = 0; i < 3; ++i) {
        if (four_colors || c0 > c1) {
            palette[2][i] = static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
        } else {
            palette[2][i] = static_cast<uint8_t>((palette[0][i] + palette[1][i]) / 2);
            palette[3][i] = 0;
        }
    }
    palette[2][3] = 255;
    palette[3][3] = four_colors || c0 > c1 ? 255 : 0;
    const uint32_t indices = static_cast<uint32_t>(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
    for (int p = 0; p < 16; ++p) {
        std::copy_n(palette[(indices >> (2 * p)) & 3], 4, out[p]);
    }
}

void DecodeAlphaBlock(const uint8_t* b, uint8_t out[16][4]) {
    uint8_t a[8] = {b[0], b[1]};
    for (int i = 1; i < 7; ++i) {
        a[i + 1] = b[0] > b[1] ? static_cast<uint8_t>(((7 - i) * b[0] + i * b[1]) / 7) : static_cast<uint8_t>(((5 - i) * b[0] + i * b[1]) / 5);
    }
    if (b[0] <= b[1]) {
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    }
    for (int p = 0; p < 16; ++p) {
        out[p][3] = a[(bits >> (3 * p)) & 7];
    }
}

uint16_t To565(const float* rgb) {
    const int r = std::clamp(static_cast<int>(std::lround(rgb[0] * 31.0f / 255.0f)), 0, 31);
    const int g = std::clamp(static_cast<int>(std::lround(rgb[1] * 63.0f / 255.0f)), 0, 63);
    const int b = std::clamp(static_cast<int>(std::lround(rgb[2] * 31.0f / 255.0f)), 0, 31);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

}

bool DecodeFtexLevel(const FtexTexture& ftex, uint32_t level, std::vector<uint8_t>& rgba) {
    if (level >= ftex.mips.size() || ftex.mips[level].empty()) {
        return false;
    }
    const uint32_t width = ftex.MipWidth(level);
    const uint32_t height = ftex.MipHeight(level);
    const std::vector<uint8_t>& data = ftex.mips[level];
    rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    if (ftex.pixel_format == 0) {
        if (data.size() < rgba.size()) {
            return false;
        }
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i] = data[i + 2];
            rgba[i + 1] = data[i + 1];
            rgba[i + 2] = data[i];
            rgba[i + 3] = data[i + 3];
        }
        return true;
    }
    const int format = ftex.pixel_format;
    if (format < 2 || format > 4) {
        return false;
    }
    const size_t block_size = format == 2 ? 8 : 16;
    const uint32_t bw = (width + 3) / 4;
    const uint32_t bh = (height + 3) / 4;
    if (data.size() < block_size * bw * bh) {
        return false;
    }
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data.data() + block_size * (static_cast<size_t>(by) * bw + bx);
            uint8_t px[16][4];
            if (format == 2) {
                DecodeColorBlock(b, false, px);
            } else {
                DecodeColorBlock(b + 8, true, px);
                if (format == 3) {
                    for (int p = 0; p < 16; ++p) {
                        const int nibble = (b[p / 2] >> (4 * (p & 1))) & 15;
                        px[p][3] = static_cast<uint8_t>(nibble * 17);
                    }
                } else {
                    DecodeAlphaBlock(b, px);
                }
            }
            for (int p = 0; p < 16; ++p) {
                const uint32_t x = bx * 4 + (p & 3);
                const uint32_t y = by * 4 + (p >> 2);
                if (x < width && y < height) {
                    std::copy_n(px[p], 4, &rgba[(static_cast<size_t>(y) * width + x) * 4]);
                }
            }
        }
    }
    return true;
}

void EncodeBc1Block(const uint8_t* rgba, uint8_t* out) {
    float px[16][3];
    float mean[3] = {0.0f, 0.0f, 0.0f};
    for (int p = 0; p < 16; ++p) {
        for (int c = 0; c < 3; ++c) {
            px[p][c] = rgba[p * 4 + c];
            mean[c] += px[p][c] / 16.0f;
        }
    }
    float cov[6] = {};
    for (int p = 0; p < 16; ++p) {
        const float d[3] = {px[p][0] - mean[0], px[p][1] - mean[1], px[p][2] - mean[2]};
        cov[0] += d[0] * d[0];
        cov[1] += d[0] * d[1];
        cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1];
        cov[4] += d[1] * d[2];
        cov[5] += d[2] * d[2];
    }
    float axis[3] = {1.0f, 1.0f, 1.0f};
    for (int it = 0; it < 8; ++it) {
        const float n[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                            cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-6f) {
            break;
        }
        for (int c = 0; c < 3; ++c) {
            axis[c] = n[c] / len;
        }
    }
    float lo = 1e9f;
    float hi = -1e9f;
    for (int p = 0; p < 16; ++p) {
        const float t = (px[p][0] - mean[0]) * axis[0] + (px[p][1] - mean[1]) * axis[1] + (px[p][2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    float e0[3];
    float e1[3];
    for (int c = 0; c < 3; ++c) {
        e0[c] = mean[c] + axis[c] * hi;
        e1[c] = mean[c] + axis[c] * lo;
    }
    uint16_t c0 = 0;
    uint16_t c1 = 0;
    uint32_t indices = 0;
    for (int round = 0; round < 3; ++round) {
        c0 = To565(e0);
        c1 = To565(e1);
        if (c0 < c1) {
            std::swap(c0, c1);
        }
        uint8_t palette[4][4];
        Expand565(c0, palette[0]);
        Expand565(c1, palette[1]);
        for (int i = 0; i < 3; ++i) {
            palette[2][i] = static_cast<uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
            palette[3][i] = static_cast<uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
        }
        constexpr float kWeight[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
        float aa = 0.0f;
        float ab = 0.0f;
        float bb = 0.0f;
        float ax[3] = {};
        float bx[3] = {};
        indices = 0;
        for (int p = 0; p < 16; ++p) {
            int best = 0;
            float best_d = 1e30f;
            for (int k = 0; k < 4; ++k) {
                float d = 0.0f;
                for (int c = 0; c < 3; ++c) {
                    const float v = px[p][c] - palette[k][c];
                    d += v * v;
                }
                if (d < best_d) {
                    best_d = d;
                    best = k;
                }
            }
            indices |= static_cast<uint32_t>(best) << (2 * p);
            const float w0 = kWeight[best];
            const float w1 = 1.0f - w0;
            aa += w0 * w0;
            ab += w0 * w1;
            bb += w1 * w1;
            for (int c = 0; c < 3; ++c) {
                ax[c] += w0 * px[p][c];
                bx[c] += w1 * px[p][c];
            }
        }
        const float det = aa * bb - ab * ab;
        if (c0 == c1 || round == 2 || std::abs(det) < 1e-6f) {
            break;
        }
        for (int c = 0; c < 3; ++c) {
            e0[c] = std::clamp((ax[c] * bb - bx[c] * ab) / det, 0.0f, 255.0f);
            e1[c] = std::clamp((bx[c] * aa - ax[c] * ab) / det, 0.0f, 255.0f);
        }
    }
    if (c0 == c1) {
        indices = 0;
    }
    out[0] = static_cast<uint8_t>(c0 & 0xFF);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1 & 0xFF);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    for (int i = 0; i < 4; ++i) {
        out[4 + i] = static_cast<uint8_t>((indices >> (8 * i)) & 0xFF);
    }
}

void EncodeBc3AlphaBlock(const uint8_t* rgba, uint8_t* out) {
    uint8_t lo = 255;
    uint8_t hi = 0;
    for (int p = 0; p < 16; ++p) {
        lo = std::min(lo, rgba[p * 4 + 3]);
        hi = std::max(hi, rgba[p * 4 + 3]);
    }
    out[0] = hi;
    out[1] = lo;
    uint8_t a[8] = {hi, lo};
    for (int i = 1; i < 7; ++i) {
        a[i + 1] = static_cast<uint8_t>(((7 - i) * hi + i * lo) / 7);
    }
    uint64_t bits = 0;
    for (int p = 0; p < 16; ++p) {
        int best = 0;
        int best_d = 1 << 30;
        for (int k = 0; k < 8 && hi != lo; ++k) {
            const int d = std::abs(static_cast<int>(rgba[p * 4 + 3]) - a[k]);
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        bits |= static_cast<uint64_t>(best) << (3 * p);
    }
    for (int i = 0; i < 6; ++i) {
        out[2 + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
    }
}

}
