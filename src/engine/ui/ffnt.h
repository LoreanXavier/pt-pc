#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>

namespace pt::ui {

struct FfntGlyph {
    uint32_t code = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t layer = 0;
    uint16_t advance = 0;
    int16_t bearing = 0;
    int16_t top = 0;
    uint16_t flags = 0;
    uint16_t atlas_x = 0;
    uint16_t atlas_y = 0;
};

struct UnicodeFontState;
struct ShapedGlyph { const FfntGlyph* glyph = nullptr; float pen = 0, x = 0, y = 0, advance = 0; };

class FfntFont {
public:
    bool Parse(std::span<const uint8_t> data, std::string* error);

    const FfntGlyph* Find(uint32_t code) const;
    const FfntGlyph* FindOrFallback(uint32_t code) const;
    int EmSize() const { return em_size_; }
    int Pad() const { return pad_; }
    float LineFactor() const;
    float Advance(const FfntGlyph& glyph) const;
    bool Bit(const FfntGlyph& glyph, int x, int y) const;
    const std::vector<FfntGlyph>& Glyphs() const { return glyphs_; }

    bool BuildAtlas(std::vector<std::vector<uint8_t>>& mips, uint32_t mip_count, bool soften = true);
    void AddTurkishGlyphs();
    bool LoadUnicodeFont(std::string_view family, std::string_view characters, bool rtl, std::string* error);
    bool Unicode() const { return bool(unicode_); }
    std::vector<ShapedGlyph> ShapeLine(std::span<const uint32_t> codes, float inline_advance) const;
    uint8_t Coverage(const FfntGlyph& glyph, int x, int y) const;
    uint32_t AtlasWidth() const { return atlas_width_; }
    uint32_t AtlasHeight() const { return atlas_height_; }

private:
    std::shared_ptr<UnicodeFontState> unicode_;
    float unicode_line_factor_ = 1.0f;
    std::vector<FfntGlyph> glyphs_;
    std::vector<uint8_t> bitmap_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> composed_;
    uint32_t bitmap_width_ = 0;
    uint32_t bitmap_height_ = 0;
    int em_size_ = 0;
    int pad_ = 0;
    uint32_t atlas_width_ = 0;
    uint32_t atlas_height_ = 0;
};

std::vector<uint32_t> DecodeUtf8(std::string_view text);

}
