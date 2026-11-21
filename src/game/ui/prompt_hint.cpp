#include "game/ui/prompt_hint.h"

#include <algorithm>
#include <array>

#include "engine/render/texture_manager.h"
#include "game/ui/ui_assets.h"
#include "game/ui/uif_view.h"

namespace pt::game {
namespace {

constexpr std::string_view kPictureCode = "\xEE\x80\x80";

struct Token {
    std::string_view name;
    std::array<Prompt, 4> keyboard{};
    int keyboard_count = 0;
    std::array<Prompt, 4> pad{};
    int pad_count = 0;
};

constexpr Prompt Key(KeyAction action) { return {PromptButton::Cross, action, action}; }
constexpr Prompt Both(PromptButton button, KeyAction action) { return {button, action, action}; }
constexpr Prompt Pair(PromptButton button, KeyAction a, KeyAction b) { return {button, a, b}; }

const std::array<Token, 36>& Tokens() {
    static const std::array<Token, 36> tokens = {{
        {"accept", {Both(PromptButton::Cross, KeyAction::Confirm)}, 1, {Both(PromptButton::Cross, KeyAction::Confirm)}, 1},
        {"back", {Both(PromptButton::Circle, KeyAction::Menu)}, 1, {Both(PromptButton::Circle, KeyAction::Menu)}, 1},
        {"pause", {Both(PromptButton::Options, KeyAction::Menu)}, 1, {Both(PromptButton::Options, KeyAction::Menu)}, 1},
        {"columns", {Both(PromptButton::R1, KeyAction::MenuColumn)}, 1, {Both(PromptButton::L1, KeyAction::MenuHouse), Both(PromptButton::R1, KeyAction::MenuColumn)}, 2},
        {"updown", {Pair(PromptButton::DpadUpDown, KeyAction::PadUp, KeyAction::PadDown)}, 1, {Pair(PromptButton::DpadUpDown, KeyAction::PadUp, KeyAction::PadDown)}, 1},
        {"leftright", {Pair(PromptButton::DpadLeftRight, KeyAction::PadLeft, KeyAction::PadRight)}, 1, {Pair(PromptButton::DpadLeftRight, KeyAction::PadLeft, KeyAction::PadRight)}, 1},
        {"dpad",
         {Pair(PromptButton::DpadUpDown, KeyAction::PadUp, KeyAction::PadDown), Pair(PromptButton::DpadLeftRight, KeyAction::PadLeft, KeyAction::PadRight)}, 2,
         {Pair(PromptButton::DpadUpDown, KeyAction::PadUp, KeyAction::PadDown), Pair(PromptButton::DpadLeftRight, KeyAction::PadLeft, KeyAction::PadRight)}, 2},
        {"photo", {Both(PromptButton::Square, KeyAction::PhotoSave)}, 1, {Both(PromptButton::Square, KeyAction::PhotoSave)}, 1},
        {"hide", {Both(PromptButton::Triangle, KeyAction::PhotoHide)}, 1, {Both(PromptButton::Triangle, KeyAction::PhotoHide)}, 1},
        {"walk", {Key(KeyAction::WalkForward), Key(KeyAction::WalkLeft), Key(KeyAction::WalkBack), Key(KeyAction::WalkRight)}, 4, {}, 0},
        {"look", {Key(KeyAction::Zoom)}, 1, {}, 0},
        {"rise", {Key(KeyAction::FreeRise)}, 1, {}, 0},
        {"sink", {Key(KeyAction::FreeSink)}, 1, {}, 0},
        {"fast", {Key(KeyAction::FreeFast)}, 1, {}, 0},
        {"seek", {Key(KeyAction::MusicBack), Key(KeyAction::MusicForward)}, 2,
         {Both(PromptButton::L1, KeyAction::MusicBack), Both(PromptButton::R1, KeyAction::MusicForward)}, 2},
        {"restart", {Key(KeyAction::MusicRestart)}, 1, {Both(PromptButton::Square, KeyAction::MusicRestart)}, 1},
        {"loop", {Key(KeyAction::MusicLoop)}, 1, {Both(PromptButton::Triangle, KeyAction::MusicLoop)}, 1},
        {"tabprev", {Key(KeyAction::MenuTabPrev)}, 1, {Both(PromptButton::L1, KeyAction::MenuTabPrev)}, 1},
        {"tabnext", {Key(KeyAction::MenuTabNext)}, 1, {Both(PromptButton::R1, KeyAction::MenuTabNext)}, 1},
        {"donate", {Key(KeyAction::MenuDonate)}, 1, {Both(PromptButton::Select, KeyAction::MenuDonate)}, 1},
        {"update", {Key(KeyAction::MenuUpdate)}, 1, {Both(PromptButton::Triangle, KeyAction::MenuUpdate)}, 1},
        {"support", {Key(KeyAction::MenuDonate)}, 1, {Both(PromptButton::L3, KeyAction::MenuDonate)}, 1},
        {"swap", {Key(KeyAction::SurvivalSwap)}, 1, {Both(PromptButton::Triangle, KeyAction::SurvivalSwap)}, 1},
        {"melee", {Both(PromptButton::R1, KeyAction::SurvivalMelee)}, 1, {Both(PromptButton::R1, KeyAction::SurvivalMelee)}, 1},
        {"flare", {Both(PromptButton::L1, KeyAction::SurvivalFlare)}, 1, {Both(PromptButton::L1, KeyAction::SurvivalFlare)}, 1},
        {"fire", {Both(PromptButton::R2, KeyAction::SurvivalFire)}, 1, {Both(PromptButton::R2, KeyAction::SurvivalFire)}, 1},
        {"aim", {Both(PromptButton::L2, KeyAction::Zoom)}, 1, {Both(PromptButton::L2, KeyAction::Zoom)}, 1},
        {"reload", {Both(PromptButton::Square, KeyAction::SurvivalReload)}, 1, {Both(PromptButton::Square, KeyAction::SurvivalReload)}, 1},
        {"sprint", {Both(PromptButton::L3, KeyAction::SurvivalSprint)}, 1, {Both(PromptButton::L3, KeyAction::SurvivalSprint)}, 1},
        {"use", {Both(PromptButton::Cross, KeyAction::SurvivalUse)}, 1, {Both(PromptButton::Cross, KeyAction::SurvivalUse)}, 1},
        {"picks", {Key(KeyAction::SurvivalPick1), Key(KeyAction::SurvivalPick2), Key(KeyAction::SurvivalPick3)}, 3,
         {Pair(PromptButton::DpadLeftRight, KeyAction::PadLeft, KeyAction::PadRight), Pair(PromptButton::DpadUpDown, KeyAction::PadUp, KeyAction::PadDown)}, 2},
        {"task", {Both(PromptButton::Select, KeyAction::SurvivalInfo)}, 1, {Both(PromptButton::Select, KeyAction::SurvivalInfo)}, 1},
        {"survival", {Key(KeyAction::MenuSurvival)}, 1, {Both(PromptButton::R2, KeyAction::MenuSurvival)}, 1},
        {"armorytab", {Key(KeyAction::MenuColumn)}, 1, {Both(PromptButton::L2, KeyAction::MenuColumn), Both(PromptButton::R2, KeyAction::MenuColumn)}, 2},
        {"armoryall", {Key(KeyAction::SurvivalMelee)}, 1, {Both(PromptButton::Triangle, KeyAction::SurvivalMelee)}, 1},
        {"armoryorbit", {Both(PromptButton::R3, KeyAction::SurvivalFire)}, 1, {Both(PromptButton::R3, KeyAction::SurvivalFire)}, 1},
    }};
    return tokens;
}

const Token* FindToken(std::string_view name) {
    for (const Token& token : Tokens()) {
        if (token.name == name) return &token;
    }
    return nullptr;
}

}

bool HasPromptTokens(std::string_view text) {
    for (size_t at = text.find('{'); at != std::string_view::npos; at = text.find('{', at + 1)) {
        const size_t end = text.find('}', at);
        if (end == std::string_view::npos) break;
        std::string_view name = text.substr(at + 1, end - at - 1);
        if (name == "kb" || name == "pad" || name == "/kb" || name == "/pad" || FindToken(name)) return true;
    }
    return false;
}

PromptLine ExpandPromptHint(std::string_view hint, bool keyboard) {
    PromptLine out;
    out.text.reserve(hint.size());
    int block = 0;
    const auto shown = [&] { return block == 0 || (block == 1 && keyboard) || (block == 2 && !keyboard); };
    for (size_t i = 0; i < hint.size();) {
        if (hint[i] == '{') {
            const size_t end = hint.find('}', i);
            if (end != std::string_view::npos) {
                const std::string_view name = hint.substr(i + 1, end - i - 1);
                if (name == "kb" || name == "pad") {
                    block = name == "kb" ? 1 : 2;
                    i = end + 1;
                    continue;
                }
                if (name == "/kb" || name == "/pad") {
                    block = 0;
                    i = end + 1;
                    continue;
                }
                if (const Token* token = FindToken(name)) {
                    if (shown()) {
                        const int count = keyboard ? token->keyboard_count : token->pad_count;
                        const auto& prompts = keyboard ? token->keyboard : token->pad;
                        for (int n = 0; n < count; ++n) {
                            out.text += kPictureCode;
                            out.pictures.push_back(prompts[static_cast<size_t>(n)]);
                        }
                    }
                    i = end + 1;
                    continue;
                }
            }
        }
        if (shown()) out.text += hint[i];
        ++i;
    }
    return out;
}

std::vector<float> PromptAdvances(UiAssets& assets, const PromptLine& line, const PromptStyle& style, float font_height) {
    std::vector<float> advances;
    for (const Prompt& prompt : line.pictures) {
        const PromptGlyph glyph = assets.PromptPicture(prompt, style);
        advances.push_back(font_height / std::max(0.05f, glyph.body_height) * glyph.body_width + 0.5f * font_height);
    }
    return advances;
}

void DrawPromptPictures(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const UiFont& font, const ui::TextLayout& layout,
                        const PromptLine& line, const PromptStyle& prompts, float alpha) {
    const float font_height = layout.line_height / std::max(0.01f, font.font.LineFactor());
    const float em = static_cast<float>(std::max(1, font.font.EmSize()));
    const ui::FfntGlyph* cap = font.font.Find('H');
    const float cap_middle = cap ? (static_cast<float>(font.font.Pad()) + cap->top + cap->height * 0.5f) / em : 0.5f * font.font.LineFactor();
    auto texture = [&](const std::string& name) {
        const uint32_t t = assets.Texture(name);
        return t | assets.TextureAddressBits(t);
    };
    for (const ui::TextLine& text_line : layout.lines) {
        for (const ui::LaidGlyph& g : text_line.glyphs) {
            if (g.glyph || g.picture < 0 || static_cast<size_t>(g.picture) >= line.pictures.size()) continue;
            const PromptGlyph glyph = assets.PromptPicture(line.pictures[static_cast<size_t>(g.picture)], prompts);
            if (glyph.icon.empty()) continue;
            const float height = font_height / std::max(0.05f, glyph.body_height);
            const glm::vec2 centre(g.position.x + g.size.x * 0.5f, g.position.y + font_height * cap_middle);
            const glm::vec2 half(height * glyph.width * 0.5f, height * 0.5f);
            const glm::vec2 a = canvas.ToTarget(centre - half), b = canvas.ToTarget(centre + half);
            if (!glyph.glow.empty()) {
                batch.Quad(a, b, glyph.uv0, glyph.uv1, glm::vec4(1.0f, 1.0f, 1.0f, 0.4f * alpha), ui::UiDrawParams::Plain(texture(glyph.glow)),
                           ui::UiShade::Material, ui::UiBlend::Additive);
            }
            batch.Quad(a, b, glyph.uv0, glyph.uv1, glm::vec4(1.0f, 1.0f, 1.0f, alpha), ui::UiDrawParams::Plain(texture(glyph.icon)), ui::UiShade::Material,
                       ui::UiBlend::Alpha);
        }
    }
}

void DrawPromptText(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, UiFont& font, std::string_view hint, const PromptStyle& prompts,
                    float size, glm::vec2 lo, glm::vec2 hi, ui::TextAlign align, ui::TextAlign vertical, glm::vec4 color, bool fit, float line_space) {
    const PromptLine line = ExpandPromptHint(hint, prompts);
    if (line.text.empty()) return;
    ui::TextStyle style;
    style.font = &font.font;
    style.font_width = style.font_height = size;
    style.line_space = line_space;
    style.inline_advances = PromptAdvances(assets, line, prompts, size);
    style.inline_advance = style.inline_advances.empty() ? 0.0f : style.inline_advances.front();
    ui::TextLayout layout = fit ? ui::LayoutTextInBox(line.text, style, glm::vec2(0.0f), hi - lo) : ui::LayoutText(line.text, style, hi.x - lo.x);
    ui::PlaceText(layout, lo, hi, align, align, vertical, true);
    DrawText(batch, canvas, font, layout, color, ui::UiBlend::Alpha);
    DrawPromptPictures(batch, canvas, assets, font, layout, line, prompts, color.a);
}

}
