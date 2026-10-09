#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

#include "engine/platform/input.h"

namespace pt::game {

// cross (left half) and circle (right half) cells drawn from the option screen's icon atlas and its glow
constexpr const char* kPcIconAtlas = "ui:pc:button_icons";
constexpr const char* kPcIconGlow = "ui:pc:button_glow";

// The PS4 buttons the option screen shows (cross and circle for the PC additions)
enum class PromptButton : uint8_t { Cross, Circle, Square, Triangle, Options, DpadUpDown, DpadLeftRight, L1, R1 };

// A button prompt: the PS4 button of the screen and the keyboard bindings it stands for (the D-pad pairs name two)
struct Prompt {
    PromptButton button = PromptButton::Cross;
    KeyAction key = KeyAction::Confirm;
    KeyAction key2 = KeyAction::Confirm;
};

// A prompt's picture: the texture (a UIF texture path or a generated name) and its glow, the cell's UV rectangle, the cell's width, and
// the width and height of what it shows (the disc, D-pad or keycap without the shadow around it), over the cell's height. `original`:
// the option screen's own picture of that button (the PlayStation glyphs, and the D-pad for every pad), which its nodes keep.
struct PromptGlyph {
    std::string icon;
    std::string glow;
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{1.0f};
    float width = 1.0f;
    float body_width = 0.4f;
    float body_height = 0.4f;
    bool original = false;
};

// Stable procedural glyph keys for Steam controller prompts; also keeps the hybrid face/shoulder layout testable without a renderer.
std::string SteamPromptGlyphName(const Prompt& prompt, const PromptStyle& style);

}
