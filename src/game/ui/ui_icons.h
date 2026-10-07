#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

#include "engine/platform/input.h"

namespace pt::game {

constexpr const char* kPcIconAtlas = "ui:pc:button_icons";
constexpr const char* kPcIconGlow = "ui:pc:button_glow";

enum class PromptButton : uint8_t { Cross, Circle, Square, Triangle, Options, DpadUpDown, DpadLeftRight, L1, R1 };

struct Prompt {
    PromptButton button = PromptButton::Cross;
    KeyAction key = KeyAction::Confirm;
    KeyAction key2 = KeyAction::Confirm;
};

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

}
