#include "engine/assets/texture_cache.h"
#include "engine/assets/enhanced_textures.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } };
    pt::FtexTexture source;
    source.width = 8; source.height = 4; source.depth = 1; source.faces = 1; source.pixel_format = 2; source.flags = 2;
    check(pt::EnhancedTextureEligible("/Assets/sh/texture/wall_bsm", source), "opaque albedo eligible");
    check(!pt::EnhancedTextureEligible("/Assets/sh/texture/wall_nrm", source), "normal name excluded");
    source.flags |= 8;
    check(!pt::EnhancedTextureEligible("/Assets/sh/texture/wall_bsm", source), "normal flag excluded");
    source.flags = 2; source.pixel_format = 4;
    check(!pt::EnhancedTextureEligible("/Assets/sh/texture/wall_bsm", source), "alpha texture excluded");
    source.pixel_format = 2;
    check(!pt::EnhancedTextureEligible("/Assets/sh/ui/texture/wall_bsm", source), "UI excluded");
    source.faces = 6;
    check(!pt::EnhancedTextureEligible("/Assets/sh/texture/wall_bsm", source), "cube excluded");
    source.faces = 1;
    source.width = source.height = 2048;
    check(pt::EnhancedTextureEligible("/Assets/sh/texture/wall_bsm", source), "2048 colour map eligible (the hallway's walls)");
    source.width = source.height = 4096;
    check(!pt::EnhancedTextureEligible("/Assets/sh/texture/wall_bsm", source), "4096 excluded");
    source.width = 8; source.height = 4;
    source.mip_count = 1;
    source.mips = {{1, 2, 3, 4}};
    const uint64_t source_key = pt::EnhancedTextureKey(source, 1);
    check(source_key != pt::EnhancedTextureKey(source, 2), "model changes invalidate cache");
    source.mips[0][0] = 9;
    check(source_key != pt::EnhancedTextureKey(source, 1), "source bytes invalidate cache");
    const auto root = std::filesystem::temp_directory_path() / ("pt-cache-test-" + std::to_string(SDL_GetTicksNS()));
    std::filesystem::create_directory(root);
    const auto file = root / "texture.pttex";
    std::vector<uint8_t> pixels(8 * 4 * 4, 255);
    pt::FtexTexture enhanced;
    check(pt::EncodeEnhancedTexture(8, 4, pixels, true, enhanced), "encode BC7 chain");
    check(enhanced.mips.size() == 4 && enhanced.mips.back().size() == 16, "chain through 1x1");
    check(pt::WriteTextureCache(file, 42, enhanced), "cache write");
    pt::FtexTexture read;
    check(pt::ReadTextureCache(file, 42, read), "cache roundtrip");
    check(read.mips == enhanced.mips && read.width == 8 && read.Srgb(), "roundtrip contents");
    check(read.Format() == VK_FORMAT_BC7_SRGB_BLOCK, "BC7 sRGB upload format");
    check(!pt::ReadTextureCache(file, 43, read), "different source rejected");
    {
        std::fstream corrupt(file, std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(40); corrupt.put('\0');
    }
    check(!pt::ReadTextureCache(file, 42, read), "corrupt block rejected");
    std::filesystem::resize_file(file, 31);
    check(!pt::ReadTextureCache(file, 42, read), "truncated cache rejected");
    std::filesystem::remove(file);
    std::filesystem::remove(root);
    std::printf("texture cache: %d failures\n", failures);
    return failures ? 1 : 0;
}
