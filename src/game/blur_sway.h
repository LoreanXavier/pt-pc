#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <glm/glm.hpp>

namespace pt::game {

struct BlurSway {
    static constexpr double kGameFrame = 1.0 / 30.0;

    float phase_a = 0.0f;
    float phase_b = 0.0f;
    float pulse = 0.0f;
    float pulse_target = 0.0f;
    bool pulse_active = false;
    double frame_time = 0.0;
    glm::vec3 previous_position{0.0f};
    glm::vec3 previous_axis{0.0f, 0.0f, 1.0f};

    float Update(float dt, const glm::vec3& position, const glm::vec3& axis, const std::function<int(int, int)>& roll) {
        const float frames = dt * 30.0f;
        phase_a += 0.037f * frames;
        phase_b += 0.051f * frames;
        if (phase_a > 3.14159265f) {
            phase_a -= 6.2831853f;
        }
        if (phase_b > 3.14159265f) {
            phase_b -= 6.2831853f;
        }
        frame_time += dt;
        const bool game_frame = frame_time + 1e-6 >= kGameFrame;
        if (game_frame) {
            frame_time = std::fmod(frame_time + 1e-6, kGameFrame);
        }
        const float base = (std::sin(phase_a) + std::sin(phase_b)) * 0.5f * 0.9f;
        if (!pulse_active) {
            if (game_frame && (glm::length(position - previous_position) > 0.04f || glm::dot(axis, previous_axis) < 0.99f)) {
                if (roll(10001, 1) < 300) {
                    pulse_target = 0.9f;
                    pulse_active = true;
                }
            }
        } else if (std::abs(pulse - pulse_target) <= 1e-6f) {
            pulse = pulse_target;
            pulse_target = 0.0f;
            if (pulse < 1e-6f) {
                pulse_active = false;
            }
        } else {
            pulse += std::clamp(pulse_target - pulse, -0.03f * frames, 0.03f * frames);
        }
        if (game_frame) {
            previous_position = position;
            previous_axis = axis;
        }
        return std::min(std::max({base, pulse, 0.0f}), 0.9f);
    }
};

}
