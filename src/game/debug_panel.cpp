#include "game/debug_panel.h"

#include <imgui.h>

#include "game/game.h"

namespace pt::game {

void DrawGameDebugPanel(Game& game) {
    if (!ImGui::Begin("Game")) {
        ImGui::End();
        return;
    }
    GameController& controller = game.Controller();
    FloorLevel& floor = game.Floor();
    Player& player = game.GetPlayer();
    ImGui::Text("step %d (requested %d)", controller.Step(), controller.RequestedStep());
    ImGui::Text("floor %s (index %d) loop %d, save floor %s", floor.CurrentFloorName().c_str(), floor.Index(), floor.LoopCount(),
                floor.SaveFloorName().c_str());
    const glm::vec3 feet = player.Feet();
    ImGui::Text("feet %.2f %.2f %.2f yaw %.2f pitch %.2f grounded %d", feet.x, feet.y, feet.z, player.yaw, player.pitch,
                player.controller.grounded ? 1 : 0);
    ImGui::Text("locks: %s handy light %s zoom %.2f %s", player.locks.Describe().c_str(), player.handy_light.enable ? "on" : "off", player.zoom,
                player.Standing() ? "standing" : "walking");
    ImGui::Text("fade %.2f state %d, lut %s, blur %d, mirror %d", game.Effects().FadeAlpha(), game.Effects().fade_state, game.Effects().lut.c_str(),
                game.Effects().full_screen_blur ? 1 : 0, game.Effects().mirror_capture ? 1 : 0);
    if (ImGui::CollapsingHeader("Stages", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& [label, stage] : game.Stages().Stages()) {
            ImGui::Text("%s: id %u %s %s", label.c_str(), stage->id, stage->active ? "active" : "inactive", stage->package_path.c_str());
        }
    }
    if (ImGui::CollapsingHeader("Demos", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const PlayingDemo& demo : game.Demos().Playing()) {
            ImGui::Text("%s %.2f / %.2f", demo.demo_id.c_str(), demo.time, demo.length);
        }
        if (ImGui::Button("skip demos")) {
            game.Demos().Skip();
        }
    }
    if (ImGui::CollapsingHeader("Nazo")) {
        for (int i = 0; i < NazoManager::kCount; ++i) {
            const NazoId id = static_cast<NazoId>(i);
            ImGui::Text("%s: %d word %#x", std::string(NazoName(id)).c_str(), static_cast<int>(game.Nazo().State(id)), game.Nazo().Word(id));
        }
    }
    if (ImGui::CollapsingHeader("Gimmicks")) {
        for (const Gimmick& g : game.Objects().Gimmicks()) {
            ImGui::Text("%s: %s %s logic %s motion %s", g.name.c_str(), g.enabled ? "on" : "off", g.placed ? "placed" : "-", g.logic_state.c_str(),
                        g.motion.c_str());
        }
    }
    if (ImGui::Button("next floor")) {
        floor.GoNextFloor();
    }
    ImGui::SameLine();
    if (ImGui::Button("game over")) {
        controller.SetStep(16);
    }
    ImGui::End();
}

}
