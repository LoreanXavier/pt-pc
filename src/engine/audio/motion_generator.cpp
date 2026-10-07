#include "engine/audio/motion_generator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pt::audio {
namespace {

template <typename T>
T Read(std::span<const uint8_t> block, size_t offset) {
    T value{};
    std::memcpy(&value, block.data() + offset, sizeof(T));
    return value;
}

}

bool MotionGeneratorParams::Parse(std::span<const uint8_t> block) {
    if (block.size() < 0x24) {
        return false;
    }
    period = Read<float>(block, 0x00);
    period_multiplier = Read<float>(block, 0x04);
    duration = Read<float>(block, 0x08);
    attack = Read<float>(block, 0x0C);
    decay = Read<float>(block, 0x10);
    sustain_time = Read<float>(block, 0x14);
    release = Read<float>(block, 0x18);
    sustain_level = std::pow(10.0f, Read<float>(block, 0x1C) * 0.05f);
    duration_type = Read<uint16_t>(block, 0x20);
    curves.clear();
    const uint16_t count = Read<uint16_t>(block, 0x22);
    size_t pos = 0x24;
    for (uint16_t c = 0; c < count; ++c) {
        if (pos + 2 > block.size()) {
            return false;
        }
        const uint16_t points = Read<uint16_t>(block, pos);
        pos += 2;
        if (pos + static_cast<size_t>(points) * 12 > block.size()) {
            return false;
        }
        Curve curve;
        for (uint16_t i = 0; i < points; ++i) {
            CurvePoint point;
            point.x = Read<float>(block, pos);
            point.y = Read<float>(block, pos + 4);
            point.interp = static_cast<Interp>(std::min<uint32_t>(Read<uint32_t>(block, pos + 8), static_cast<uint32_t>(Interp::Constant)));
            curve.points.push_back(point);
            pos += 12;
        }
        curves.push_back(std::move(curve));
    }
    return true;
}

float MotionGeneratorParams::Duration() const {
    switch (duration_type) {
        case 0: return period * period_multiplier;
        case 1: return duration;
        case 2: return attack + decay + sustain_time + release;
        default: return 0.0f;
    }
}

float MotionGeneratorParams::Envelope(double seconds) const {
    if (duration_type != 2) {
        return 1.0f;
    }
    double t = seconds;
    if (t < attack) {
        return static_cast<float>(t / attack);
    }
    t -= attack;
    if (t < decay) {
        return static_cast<float>(1.0 - (1.0 - sustain_level) * t / decay);
    }
    t -= decay;
    if (t < sustain_time) {
        return sustain_level;
    }
    t -= sustain_time;
    if (t < release) {
        return static_cast<float>(sustain_level * (1.0 - t / release));
    }
    return 0.0f;
}

float MotionGeneratorParams::CurveTime(double seconds) const {
    const double t = seconds / std::max(period_multiplier, 1.0e-6f);
    return static_cast<float>(period > 0.0f ? std::fmod(t, static_cast<double>(period)) : t);
}

float MotionGeneratorParams::Sample(size_t curve, double seconds) const {
    if (curve >= curves.size()) {
        return 0.0f;
    }
    return curves[curve].EvaluateRaw(CurveTime(seconds)) * Envelope(seconds);
}

uint8_t MotionToPadByte(float value) {
    if (value > 1.0f) {
        return 255;
    }
    return value > 0.0f ? static_cast<uint8_t>(static_cast<int>(value * 255.0f)) : 0;
}

}
