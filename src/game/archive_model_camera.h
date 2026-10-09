#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace pt::game {

// Move the model's look point in the camera plane. The limit is relative to the framing chosen at StartModel,
// so panning can reveal the face or other details without losing the model altogether.
inline glm::vec3 PanArchiveModel(const glm::vec3& target, const glm::vec3& home, const glm::vec3& right,
                                 const glm::vec3& up, glm::vec2 input, float dt, float distance, float radius) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return target;
    const float extent = std::max(radius, 0.05f);
    const glm::vec3 direction = right * input.x - up * input.y;
    const glm::vec3 moved = target + direction * (std::max(distance, extent) * 0.8f * dt);
    const glm::vec3 offset = moved - home;
    const float limit = extent * 1.5f;
    const float length = glm::length(offset);
    return length > limit ? home + offset * (limit / length) : moved;
}

}
