#include <cmath>
#include <cstdio>
#include "game/player.h"

int main() {
    using namespace pt;
    using namespace pt::game;
    int failures = 0;
    const auto check = [&](const char* name, bool ok) {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    Player player;
    CollisionWorld world;
    PlayerFrameContext context;
    InputState input;
    player.FollowCamera(3.1f, 0.0f);
    context.peephole_theater = true;
    player.Update(1.0f / 60, input, world, context);
    const float center = player.yaw;
    const auto bounded = [&] {
        return std::abs(std::remainder(player.yaw - center, 6.2831853f)) <= .261801f &&
               std::abs(player.pitch) <= .261801f &&
               std::abs(std::remainder(player.target_yaw - center, 6.2831853f)) <= .261801f &&
               std::abs(player.target_pitch) <= .261801f;
    };
    for (float delta : {1000.0f, -1000.0f, 3.1415927f, -3.1415927f}) {
        input.mouse_look = glm::vec2(delta, delta);
        player.Update(1.0f / 60, input, world, context);
        check("large mouse turn stays inside aperture, including yaw seam", bounded());
    }
    input = InputState{};
    input.held = kPadLookRight | kPadLookUp;
    for (int i = 0; i < 240; ++i) player.Update(1.0f / 60, input, world, context);
    check("held keyboard look stays inside aperture", bounded());
    {
        // the render's turn between ticks (Game::PeepholeTurn) takes the same aperture as the tick
        float turned_yaw = center + 1.0f;
        float turned_pitch = -1.0f;
        player.ClampPeepholeLook(turned_yaw, turned_pitch);
        check("render turn beyond the aperture stops at its edge",
              std::abs(std::remainder(turned_yaw - center, 6.2831853f) - .2617994f) < 1e-4f && std::abs(turned_pitch + .2617994f) < 1e-6f);
        float inside_yaw = center + 0.1f;
        float inside_pitch = 0.05f;
        player.ClampPeepholeLook(inside_yaw, inside_pitch);
        check("render turn inside the aperture is kept", inside_yaw == center + 0.1f && inside_pitch == 0.05f);
    }
    context.peephole_theater = false;
    input = InputState{};
    input.mouse_look.x = 1.0f;
    player.Update(1.0f / 60, input, world, context);
    check("ending theater restores ordinary free look", !bounded());
    input = InputState{};
    context.peephole_theater = true;
    const float reentry = player.yaw;
    player.Update(1.0f / 60, input, world, context);
    check("re-entering does not reuse old aperture heading", std::abs(player.yaw - reentry) < .0001f);
    return failures ? 1 : 0;
}
