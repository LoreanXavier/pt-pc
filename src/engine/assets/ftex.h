#pragma once

#include <volk.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/fs/qar.h"

namespace pt {

struct FtexTexture {
    // 0..7 are Fox formats; 8 is the port's generated BC7 cache format.
    uint16_t pixel_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mip_count = 0;
    uint32_t faces = 1;
    uint32_t flags = 0;
    uint8_t filter = 0;
    uint16_t address_mode = 0;
    std::vector<std::vector<uint8_t>> mips;

    bool Srgb() const { return (flags & 2) != 0; }
    bool Cube() const { return (flags & 4) != 0; }
    bool NormalMap() const { return (flags & 8) != 0; }
    VkFormat Format() const;
    uint32_t MipWidth(uint32_t level) const { return std::max(1u, width >> level); }
    uint32_t MipHeight(uint32_t level) const { return std::max(1u, height >> level); }
};

std::string FtexStem(std::string_view path);
// a file of texture.qar by its name ("/Assets/.../x.ftex"), a mod's copy in its place (docs/modding.md)
std::optional<std::vector<uint8_t>> ReadTextureFile(const QarArchive& qar, const std::string& name);
bool LoadFtex(const QarArchive& qar, std::string_view path, FtexTexture& out);
// RGBA8 pixels of one level of a texture in the formats P.T. keeps pictures in (0 BGRA8, 2 BC1, 3 BC2, 4 BC3); false for others
bool DecodeFtexLevel(const FtexTexture& ftex, uint32_t level, std::vector<uint8_t>& rgba);
// One 4 x 4 block of RGBA8 pixels (row by row) as opaque BC1, or as the alpha half of BC3
void EncodeBc1Block(const uint8_t* rgba, uint8_t* out);
void EncodeBc3AlphaBlock(const uint8_t* rgba, uint8_t* out);

}
