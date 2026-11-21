#pragma once

#include <glm/glm.hpp>

#include "engine/platform/input.h"
#include "engine/ui/ui_batch.h"
#include "game/ui/pc_settings.h"

namespace pt::game {

class Game;
class UiAssets;
struct MenuInput;
struct UiCanvas;

class ArmoryView {
public:
    enum class Result { Stay, Back };

    void Reset();
    Result Update(Game& game, PcSettingsSource& source, const MenuInput& input, float dt);
    void Draw(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcSettingsSource& source, const PromptStyle& prompts, int language);

private:
    int scroll_ = 0;
    bool dragging_ = false;
    glm::vec2 drag_last_{0.0f};
    float repeat_timer_ = 0.0f;
    uint32_t repeat_dir_ = 0;
    float pulse_ = 0.0f;
    int hovered_weapon_ = -1;
    int hovered_mode_ = -1;
    int hovered_filter_ = -1;
    int hovered_card_ = -1;
    int hovered_action_ = -1;
};

}
