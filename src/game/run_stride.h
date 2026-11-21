#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace pt::game {

namespace run_stride {

constexpr float kSprintSpeed = 1.85f;
constexpr float kNightSprintSpeed = 2.65f;
constexpr float kNightPaceSpeed = 1.5f;
constexpr float kWalkStepsPerFrame = 2.0f / 72.0f;
constexpr float kMotionFps = 59.94f;

inline float Cadence(float speed) {
    const float clamped = std::max(speed, 0.05f);
    return clamped > 1.0f ? std::pow(clamped, 0.58f) : std::pow(clamped, 0.75f);
}

inline float Weight(float speed_scale, float sprint_speed = kSprintSpeed, float walk_speed = 1.0f) {
    return std::clamp((speed_scale - walk_speed) / (sprint_speed - walk_speed), 0.0f, 1.0f);
}

inline float AdvanceBob(float& phase, int& steps, int sounding_steps, float stick, float cadence, float dt) {
    constexpr float kPi = 3.14159265358979f;
    const float rate = stick > 0.1f ? cadence * std::clamp(0.5f + 0.6f * stick, 0.5f, 1.0f) : 0.0f;
    phase += dt * kMotionFps * rate * kWalkStepsPerFrame * kPi;
    float found = NAN;
    if (sounding_steps != steps) {
        if (sounding_steps > steps) {
            const float low = std::floor(phase / kPi) * kPi + 0.5f * kPi;
            float error = low - phase;
            if (error > 0.5f * kPi) error -= kPi;
            if (error < -0.5f * kPi) error += kPi;
            phase += error / 3.0f;
            found = error;
        }
        steps = sounding_steps;
    }
    phase = std::fmod(phase, 4.0f * kPi);
    return found;
}

inline glm::vec4 Bob(float phase, float weight) {
    if (weight <= 0.0f) return glm::vec4(0.0f);
    return glm::vec4(std::cos(phase) * 0.007f, std::cos(2.0f * phase) * 0.011f, std::sin(phase) * 0.0045f, std::sin(2.0f * phase) * 0.0022f) * weight;
}

struct SteadyEye {
    static constexpr float kShare = 0.8f;
    float height = 0.0f;
    bool valid = false;
    void Advance(float eye_height, float dt) {
        if (!valid || std::abs(eye_height - height) > 0.3f) {
            height = eye_height;
            valid = true;
            return;
        }
        height += (eye_height - height) * (1.0f - std::exp(-dt / 0.35f));
    }
    float Height(float eye_height, float weight) const {
        return valid ? eye_height + (height - eye_height) * kShare * std::clamp(weight, 0.0f, 1.0f) : eye_height;
    }
    static float Roll(float roll, float weight) { return roll * (1.0f - kShare * std::clamp(weight, 0.0f, 1.0f)); }
};

}

}
