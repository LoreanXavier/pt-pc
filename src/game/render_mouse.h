#pragma once
#include "engine/render/camera.h"
#include "game/player.h"
namespace pt::game {
inline Camera RenderMouseLook(Camera rendered, const Camera& current, glm::vec2 pending, bool invert_x, bool invert_y) {
    // Mouse deltas are already integrated in current, unlike smoothed pad look.
    // Blending these angles again makes the camera rewind whenever a tick consumes pending input.
    rendered.yaw = current.yaw - pending.x * (invert_x ? -1.0f : 1.0f);
    rendered.pitch = Player::ClampPitch(current.pitch - pending.y * (invert_y ? -1.0f : 1.0f));
    return rendered;
}
}
