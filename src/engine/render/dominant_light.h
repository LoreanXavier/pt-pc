#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pt {

struct DominantLightState {
    glm::vec3 last{0.0f, -1.0f, 0.0f};
    glm::vec3 from{0.0f, -1.0f, 0.0f};
    float t = 1.0f;
    bool turn = false;
    glm::vec4 out{0.0f, -1.0f, 0.0f, 1.0f};

    glm::vec4 Step(const glm::vec3& found, float dt) {
        const float angle = 2.0f * std::acos(std::min(1.0f, std::sqrt(std::max(0.0f, 2.0f * (1.0f + glm::dot(found, last)))) * 0.5f));
        last = found;
        constexpr float kFade = 0.7853982f;
        constexpr float kTurn = 0.3926991f;
        if (angle > kFade) {
            t = 0.0f;
            turn = false;
        } else if (angle > kTurn) {
            t = 0.0f;
            turn = true;
        } else if (t == 1.0f) {
            from = found;
            out = glm::vec4(found, 1.0f);
            return out;
        }
        t += std::max(dt, 0.0f) * 4.0f;
        if (t >= 1.0f) {
            t = 1.0f;
            from = found;
        }
        if (turn) {
            const float c = std::clamp(glm::dot(from, found), -1.0f, 1.0f);
            const glm::vec3 axis = glm::cross(from, found);
            glm::vec3 dir = found;
            if (glm::dot(axis, axis) > 1.0e-12f) {
                dir = glm::angleAxis(t * std::acos(c), glm::normalize(axis)) * from;
            }
            out = glm::vec4(dir, 1.0f);
        } else if (t < 0.5f) {
            out = glm::vec4(from, 1.0f - 2.0f * t);
        } else {
            out = glm::vec4(found, 2.0f * (t - 0.5f));
        }
        return out;
    }
};

}
