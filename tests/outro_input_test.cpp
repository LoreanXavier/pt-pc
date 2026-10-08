#include "game/outro_input.h"
#include "game/player.h"

#include <cstdio>

namespace {

bool Empty(const pt::InputState& input) {
    return input.left_stick == glm::vec2(0.0f) && input.right_stick == glm::vec2(0.0f) && input.mouse_look == glm::vec2(0.0f) &&
           input.held == 0 && input.pressed == 0 && input.raw_held == 0 && input.raw_pressed == 0 && !input.pause && !input.confirm &&
           !input.cancel && !input.pc_settings && !input.any_button && !input.from_gamepad && !input.left_stick_from_pad && !input.click &&
           !input.right_click && !input.gouge_pressed && !input.voice_keyword_pressed && !input.house_pressed && !input.pointer_valid &&
           input.pointer == glm::vec2(0.0f) && !input.vr_look && input.vr_look_angles == glm::vec2(0.0f) && input.gamepad_sensitivity == 1.0f;
}

}

int main() {
    int failures = 0;
    const auto check = [&](const char* name, bool ok) {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    pt::InputState input;
    input.left_stick = {0.7f, -0.4f};
    input.right_stick = {-0.2f, 0.9f};
    input.mouse_look = {1.2f, -0.5f};
    input.held = 0x1234;
    input.pressed = 0x5678;
    input.raw_held = 0x9abc;
    input.raw_pressed = 0xdef0;
    input.pause = input.confirm = input.cancel = input.pc_settings = input.any_button = true;
    input.from_gamepad = input.left_stick_from_pad = input.click = input.right_click = input.gouge_pressed = true;
    input.voice_keyword_pressed = input.house_pressed = input.pointer_valid = true;
    input.pointer = {320.0f, 180.0f};
    input.vr_look = true;
    input.vr_look_angles = {0.3f, 0.4f};
    input.gamepad_sensitivity = 1.5f;

    check("step 28 blocks outro gameplay input", pt::game::EndingOutroInputBlocked(28));
    check("step 28 clears look, sticks, buttons, menu, and VR input", Empty(pt::game::GateEndingOutroInput(28, input)));

    check("step 31 street controls remain enabled", !pt::game::EndingOutroInputBlocked(31));
    const pt::InputState street = pt::game::GateEndingOutroInput(31, input);
    check("step 31 preserves movement, look, buttons, and pause", street.left_stick == input.left_stick && street.right_stick == input.right_stick &&
                                                                   street.mouse_look == input.mouse_look && street.held == input.held &&
                                                                   street.pressed == input.pressed && street.raw_held == input.raw_held &&
                                                                   street.raw_pressed == input.raw_pressed && street.pause && street.confirm &&
                                                                   street.cancel && street.pc_settings && street.click && street.vr_look);

    check("step 33 credits controls remain enabled", !pt::game::EndingOutroInputBlocked(33));
    const pt::InputState credits = pt::game::GateEndingOutroInput(33, input);
    check("step 33 preserves confirm and back input", credits.confirm && credits.cancel && credits.mouse_look == input.mouse_look &&
                                                        credits.held == input.held);

    pt::InputState look;
    look.mouse_look.x = 0.1f;
    pt::game::Player baseline;
    pt::game::Player outro;
    pt::game::Player street_player;
    pt::game::Player credits_player;
    pt::CollisionWorld world;
    pt::game::PlayerFrameContext context;
    for (pt::game::Player* player : {&baseline, &outro, &street_player, &credits_player}) player->FollowCamera(0.0f, 0.0f);
    baseline.Update(1.0f / 60.0f, look, world, context);
    outro.Update(1.0f / 60.0f, pt::game::GateEndingOutroInput(28, look), world, context);
    street_player.Update(1.0f / 60.0f, pt::game::GateEndingOutroInput(31, look), world, context);
    credits_player.Update(1.0f / 60.0f, pt::game::GateEndingOutroInput(33, look), world, context);
    check("outro mouse movement no longer turns the player camera", baseline.yaw != 0.0f && outro.yaw == 0.0f);
    check("street and credits preserve ordinary player look", street_player.yaw == baseline.yaw && credits_player.yaw == baseline.yaw);
    return failures ? 1 : 0;
}
