#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/ui/text_layout.h"
#include "engine/ui/ui_batch.h"
#include "engine/ui/uif.h"
#include "game/ui/ui_assets.h"

namespace pt::game {

struct UiCanvas {
    static constexpr float kWidth = 1280.0f;
    static constexpr float kHeight = 720.0f;
    static constexpr float kUnit = 10.0f;

    glm::vec2 extent{kWidth, kHeight};
    glm::vec2 origin{0.0f};
    float scale = 1.0f;

    static UiCanvas Fit(VkExtent2D target);
    glm::vec2 ToTarget(glm::vec2 virtual_px) const { return origin + virtual_px * scale; }
    glm::vec2 FromUnits(glm::vec2 units) const { return ToTarget({kWidth * 0.5f + units.x * kUnit, kHeight * 0.5f - units.y * kUnit}); }
};

// A picture drawn inline in a text node where its text has U+E000 (a button prompt in a help line): the model's texture indices of the
// picture and its glow, the UV rectangle of its cell, the cell's width, and the width and height of what it shows (the disc or keycap
// without its shadow), all over the cell's height. The shown part is drawn one em high.
struct UifInlinePicture {
    int texture = -1;
    int glow = -1;
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{1.0f};
    float width = 1.0f;
    float body_width = 0.4f;
    float body_height = 0.4f;
};

struct UifNodeState {
    std::optional<bool> visible;
    std::optional<glm::vec4> color;
    glm::vec4 color_scale{1.0f};
    glm::vec2 offset{0.0f};
    glm::vec2 scale{1.0f};
    std::optional<std::string> text;
    std::optional<glm::vec4> text_box;
    // right-to-left text (Arabic) swaps its start and end alignment
    std::optional<float> text_line_pitch;
    bool mirror_rtl = false;
    // the end of a one-line text's room, in the node's own units (a row label ends before the row's value instead of running
    // under it): a wider text is shrunk to fit, and right-to-left text with mirror_rtl ends there
    std::optional<float> text_end;
    std::vector<std::string> description_samples;
    std::optional<std::array<float, 28>> params;
    std::optional<std::vector<glm::vec2>> positions;
    std::optional<int> texture;
    std::optional<std::vector<glm::vec2>> uvs;
    std::optional<UifInlinePicture> inline_picture;
    std::optional<UiFontType> font_type;
    std::optional<glm::vec3> anim_translate;
    std::optional<glm::vec3> anim_scale;
    std::optional<glm::vec4> anim_color;
    std::vector<std::pair<size_t, glm::vec2>> anim_points;
};

// Glyph quads of laid out text. Text shades them with the colour (Draw2D); Border is the subtitles' Draw2D_Border, white with a black rim.
void DrawText(ui::UiBatch& batch, const UiCanvas& canvas, const UiFont& font, const ui::TextLayout& layout, glm::vec4 color, ui::UiBlend blend,
              ui::UiShade shade = ui::UiShade::Text);

class UifView {
public:
    void Bind(const ui::UifModel* model, UiAssets* assets);
    bool Valid() const { return model_ != nullptr; }
    UifNodeState& State(uint16_t id);
    void ResetStates();
    const ui::UifNode* Node(uint16_t id) const { return model_ ? model_->FindById(id) : nullptr; }
    const ui::UifModel* Model() const { return model_; }
    UifNodeState& StateAt(size_t index) { return states_[index]; }
    const std::unordered_multimap<uint32_t, size_t>& HashIndex() const { return hash_index_; }
    const std::unordered_multimap<uint32_t, std::pair<size_t, size_t>>& PointIndex() const { return point_index_; }
    void ClearAnimation();
    glm::vec2 WorldPosition(uint16_t id) const;
    // the x extent of the text a node drew last, in the node's own units (left and right of its origin); none before it drew text
    std::optional<glm::vec2> TextSpan(uint16_t id) const;
    void Draw(ui::UiBatch& batch, const UiCanvas& canvas, int language, float root_alpha, glm::vec2 root_offset = glm::vec2(0.0f));

private:
    struct World {
        glm::mat3 transform{1.0f};
        glm::vec4 color{1.0f};
        bool visible = true;
        bool done = false;
    };

    const World& Resolve(int index, std::vector<World>& cache, float root_alpha, glm::vec2 root_offset) const;
    void DrawInlinePictures(ui::UiBatch& batch, const UiCanvas& canvas, const UiFont& font, const ui::TextStyle& style, const ui::TextLayout& layout,
                            const UifInlinePicture& picture, float height, glm::vec4 color) const;

    const ui::UifModel* model_ = nullptr;
    UiAssets* assets_ = nullptr;
    bool descriptions_audited_ = false;
    std::vector<UifNodeState> states_;
    std::vector<std::optional<glm::vec2>> text_spans_;
    std::unordered_multimap<uint32_t, size_t> hash_index_;
    std::unordered_multimap<uint32_t, std::pair<size_t, size_t>> point_index_;
};

}
