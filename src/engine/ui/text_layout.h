#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

#include "engine/ui/ffnt.h"

namespace pt::ui {

// U+E000 in a text is a picture placed inline (a button prompt): it takes `inline_advance` of the line and is laid out as a glyph
// without a font glyph, so the caller draws it at its box
constexpr uint32_t kInlinePicture = 0xE000;

struct TextStyle {
    const FfntFont* font = nullptr;
    float font_width = 20.0f;
    float font_height = 20.0f;
    float text_space = 0.0f;
    float line_space = 0.0f;
    float inline_advance = 0.0f;
};

struct LaidGlyph {
    const FfntGlyph* glyph = nullptr;  // null for an inline picture, whose box is position and size (the advance and the line height)
    glm::vec2 position{0.0f};
    glm::vec2 size{0.0f};
};

struct TextLine {
    std::vector<LaidGlyph> glyphs;
    float width = 0.0f;
};

struct TextLayout {
    std::vector<TextLine> lines;
    float line_height = 0.0f;
    float line_pitch = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    // right-to-left text (Arabic in a Unicode font): its paragraphs start at the right; with mirror_rtl PlaceText and
    // LayoutTextInBox swap the start and end alignments for it
    bool rtl = false;
};

enum class TextAlign { Start, Center, End };

void NormalizeSubtitleStyle(TextStyle& style);

TextLayout LayoutText(std::string_view utf8, const TextStyle& style, float max_width);

TextLayout LayoutTextInBox(std::string_view utf8, TextStyle style, glm::vec2 box_min, glm::vec2 box_max, bool mirror_rtl = false);

void PlaceText(TextLayout& layout, glm::vec2 box_min, glm::vec2 box_max, TextAlign block_h, TextAlign line_h, TextAlign v,
               bool mirror_rtl = false);

}
