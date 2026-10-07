#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace pt::game {

struct Envelope {
    float value = 0.0f;
    float in_time = 0.0f;
    float in_duration = 0.0f;
    float hold_time = 0.0f;
    float hold_duration = 0.0f;
    float out_time = 0.0f;
    float out_duration = 0.0f;

    void Start(float in, float hold, float out);
    bool Running() const { return in_duration > 0.0f || hold_duration > 0.0f || out_duration > 0.0f; }
};

class SubliminalEffect {
public:
    static constexpr int kImageCount = 10;

    void Trigger(int index, bool flag, bool no_string, glm::vec2 position);
    void Update(float dt);
    bool Active() const { return noise_a_.Running() || noise_b_.Running() || string_.Running() || noise_a_.value > 0.0f || string_.value > 0.0f; }

    float Phase() const { return phase_; }
    float NoiseA() const { return noise_a_.value; }
    float NoiseB() const { return noise_b_.value; }
    float StringValue() const { return string_.value; }
    int Image() const { return image_; }
    glm::vec2 Position() const { return position_; }

private:
    void Step(float dt);

    Envelope noise_a_;
    Envelope noise_b_;
    Envelope string_;
    glm::vec2 position_{0.0f};
    float phase_ = 0.0f;
    float step_time_ = 0.0f;
    int image_ = -1;
    bool flag_ = false;
};

}
