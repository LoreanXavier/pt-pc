#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/anim/anim_codec.h"
#include "engine/anim/motion_data.h"

namespace pt::anim {

struct GaniTrack {
    uint16_t index = 0;
    uint8_t kind = 0;
    uint8_t bits = 0;
    std::vector<Key> keys;
};

struct GaniUnit {
    uint32_t hash = 0;
    uint8_t flags = 0;
    std::vector<GaniTrack> tracks;
    int rotation = -1;
    int translation = -1;

    bool Loop() const { return (flags & 1) != 0; }
    bool Static() const { return (flags & 4) != 0; }
};

struct GaniEvent {
    uint32_t set_hash = 0;
    uint32_t table_hash = 0;
    StreamEvent event;
};

struct GaniMotion {
    uint64_t path_code = 0;
    size_t offset = 0;
    uint32_t size = 0;
    uint32_t frames = 0;
    uint32_t ticks_per_frame = 5;
    uint32_t rig_word = 0;
    float slope_dir = 0.0f;
    float slope_angle = 0.0f;
    std::vector<NameEntry> bones;
    std::vector<GaniUnit> units;
    std::vector<GaniEvent> events;

    bool RigDriven() const { return (rig_word & 1) != 0; }
    bool Loops() const { return !units.empty() && units.front().Loop(); }
    double Seconds() const { return frames / kMotionFramesPerSecond; }
    const GaniUnit* FindUnit(uint32_t hash) const;
    std::string BoneName(uint32_t hash) const;
    double WrapFrame(double frame, bool loop) const;
    glm::vec4 Sample(const GaniUnit& unit, int track, double frame) const;
};

bool ParseGani(std::span<const uint8_t> data, size_t offset, GaniMotion& out, std::string* error);

class MotionArchive {
public:
    static constexpr uint32_t kMagic = 0x0C012B72;

    bool Parse(std::vector<uint8_t> bytes, std::string* error);
    const GaniMotion* Find(uint64_t path_code) const;
    const std::vector<GaniMotion>& Motions() const { return motions_; }
    bool Empty() const { return motions_.empty(); }

private:
    std::vector<uint8_t> bytes_;
    std::vector<GaniMotion> motions_;
};

}
