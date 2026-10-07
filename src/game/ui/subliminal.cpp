#include "game/ui/subliminal.h"

#include <algorithm>

#include "game/game.h"

namespace pt::game {
namespace {

constexpr float kOriginalStep = 1.0f / Game::kOriginalFrameRate;

void StepEnvelope(Envelope& e, float dt, bool& hold_ended) {
    hold_ended = false;
    if (e.in_duration > 0.0f) {
        e.in_time = std::min(e.in_duration, dt + e.in_time);
        e.value = e.in_time / e.in_duration;
        if (e.value >= 1.0f) {
            e.in_duration = 0.0f;
        }
    } else if (e.hold_duration > 0.0f) {
        e.hold_time += dt;
        if (e.hold_duration < e.hold_time) {
            e.hold_duration = 0.0f;
            hold_ended = true;
        }
    } else if (e.out_duration > 0.0f) {
        e.out_time = std::min(e.out_duration, dt + e.out_time);
        e.value = 1.0f - e.out_time / e.out_duration;
        if (e.value <= 0.0f) {
            e.out_duration = 0.0f;
        }
    }
}

}

void Envelope::Start(float in, float hold, float out) {
    value = 0.0f;
    in_time = 0.0f;
    in_duration = in;
    hold_time = 0.0f;
    hold_duration = hold;
    out_time = 0.0f;
    out_duration = out;
}

void SubliminalEffect::Trigger(int index, bool flag, bool no_string, glm::vec2 position) {
    if (index < 0 || index >= kImageCount) {
        return;
    }
    noise_a_.Start(0.03f, 0.001f, 0.03f);
    string_.in_time = 0.0f;
    if (!no_string) {
        string_.Start(0.03f, 3.0f, 0.03f);
        position_ = position;
        image_ = index;
    } else {
        string_ = Envelope{};
    }
    flag_ = flag;
    step_time_ = kOriginalStep;
}

void SubliminalEffect::Update(float dt) {
    if (dt <= 0.0f) {
        return;
    }
    step_time_ += dt;
    while (step_time_ >= kOriginalStep * 0.999f) {
        step_time_ = std::max(0.0f, step_time_ - kOriginalStep);
        Step(kOriginalStep);
    }
}

void SubliminalEffect::Step(float dt) {
    bool hold_ended = false;
    StepEnvelope(noise_a_, dt, hold_ended);
    if (hold_ended && flag_) {
        noise_b_.Start(0.01f, 0.0f, 0.01f);
        flag_ = false;
    }
    StepEnvelope(noise_b_, dt, hold_ended);
    StepEnvelope(string_, dt, hold_ended);
    if (hold_ended) {
        noise_a_.Start(0.03f, 0.0f, 0.03f);
    }
    phase_ += 0.7f;
    if (phase_ < 0.0f) {
        phase_ += 1.0f;
    }
    if (phase_ > 1.0f) {
        phase_ -= 1.0f;
    }
}

}
