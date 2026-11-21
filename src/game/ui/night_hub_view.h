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

class NightHubView {
public:
    enum class Result { Stay, Back, Activated };

    void Reset();
    Result Update(Game& game, PcSettingsSource& source, const MenuInput& input, float dt);
    void Draw(ui::UiBatch& batch, const UiCanvas& canvas, UiAssets& assets, const PcSettingsSource& source,
              const PromptStyle& prompts, int language);

private:
    int focus_ = 0;
    int hovered_ = -1;
    float flash_ = 0.0f;
    float repeat_timer_ = 0.0f;
    uint32_t repeat_dir_ = 0;
};

}
