#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/audio/wwise_bank.h"

namespace pt::audio {

// Wwise Motion Generator, 0x5E2950 0x5E2F60 0x5E31D0
struct MotionGeneratorParams {
    float period = 1.0f;
    float period_multiplier = 1.0f;
    float duration = 1.0f;
    float attack = 0.0f;
    float decay = 0.0f;
    float sustain_time = 0.0f;
    float release = 0.0f;
    float sustain_level = 1.0f;
    uint16_t duration_type = 0;
    std::vector<Curve> curves;

    bool Parse(std::span<const uint8_t> block);
    float Duration() const;
    float Envelope(double seconds) const;
    float CurveTime(double seconds) const;
    float Sample(size_t curve, double seconds) const;
};

struct MotionLevels {
    uint8_t large_motor = 0;
    uint8_t small_motor = 0;
};

uint8_t MotionToPadByte(float value);

}
