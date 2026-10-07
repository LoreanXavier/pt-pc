#include "engine/anim/gani.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace pt::anim {
namespace {

constexpr uint32_t kGaniMagic = 0x0BFCA2D2;

template <typename T>
T At(std::span<const uint8_t> data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

}

const GaniUnit* GaniMotion::FindUnit(uint32_t hash) const {
    for (const GaniUnit& u : units) {
        if (u.hash == hash) {
            return &u;
        }
    }
    return nullptr;
}

std::string GaniMotion::BoneName(uint32_t hash) const {
    for (const NameEntry& e : bones) {
        if (e.hash == hash) {
            return e.name;
        }
    }
    return {};
}

double GaniMotion::WrapFrame(double frame, bool loop) const {
    if (frames == 0) {
        return 0.0;
    }
    const double length = static_cast<double>(frames);
    if (loop) {
        double f = std::fmod(frame, length);
        return f < 0.0 ? f + length : f;
    }
    return std::clamp(frame, 0.0, length);
}

glm::vec4 GaniMotion::Sample(const GaniUnit& unit, int track, double frame) const {
    if (track < 0 || static_cast<size_t>(track) >= unit.tracks.size()) {
        return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const GaniTrack& t = unit.tracks[static_cast<size_t>(track)];
    return SampleKeys(t.keys, t.kind, frame);
}

bool ParseGani(std::span<const uint8_t> data, size_t offset, GaniMotion& out, std::string* error) {
    if (At<uint32_t>(data, offset) != kGaniMagic) {
        if (error) {
            *error = std::format("no gani at {:#x}", offset);
        }
        return false;
    }
    const uint32_t header = At<uint32_t>(data, offset + 4);
    out.offset = offset;
    out.size = At<uint32_t>(data, offset + 8);
    std::vector<MotionNode> nodes;
    if (!ParseMotionTree(data, offset + header, nodes)) {
        if (error) {
            *error = std::format("bad gani tree at {:#x}", offset);
        }
        return false;
    }
    const size_t limit = std::min(data.size(), offset + out.size);
    for (const MotionNode& n : nodes) {
        if (n.hash == kHashSklList && !n.name_table.empty()) {
            out.bones = n.name_table;
        } else if (n.hash == kHashUnit && n.units) {
            const UnitsTable& table = *n.units;
            out.frames = table.frames;
            out.ticks_per_frame = table.ticks_per_frame;
            out.rig_word = table.rig_word;
            for (const UnitDesc& ud : table.units) {
                GaniUnit unit;
                unit.hash = ud.hash;
                unit.flags = ud.flags;
                for (const TrackDesc& td : ud.tracks) {
                    GaniTrack track;
                    track.index = td.index;
                    track.kind = td.kind;
                    track.bits = td.bits;
                    if (td.data_offset >= 0) {
                        ReadKeys(data.subspan(0, limit), static_cast<size_t>(td.data_offset), td.kind, td.bits, unit.Static(), table.frames, 0,
                                 track.keys);
                    }
                    const int slot = static_cast<int>(unit.tracks.size());
                    if (IsRotationKind(td.kind)) {
                        if (unit.rotation < 0) {
                            unit.rotation = slot;
                        }
                    } else if (unit.translation < 0) {
                        unit.translation = slot;
                    }
                    unit.tracks.push_back(std::move(track));
                }
                out.units.push_back(std::move(unit));
            }
        } else if (n.hash == kHashGaniEvents && n.raw_data) {
            const size_t top = n.raw_data;
            const uint32_t set_hash = At<uint32_t>(data, top);
            const uint32_t set_count = At<uint16_t>(data, top + 4);
            for (uint32_t i = 0; i < set_count; ++i) {
                const int32_t table_offset = At<int32_t>(data, top + 8 + 4 * static_cast<size_t>(i));
                std::vector<StreamEvent> events;
                uint32_t table_hash = 0;
                ParseEventTable(data.subspan(0, limit), top + static_cast<size_t>(static_cast<int64_t>(table_offset)), table_hash, events);
                for (StreamEvent& e : events) {
                    out.events.push_back({set_hash, table_hash, std::move(e)});
                }
            }
        } else if (n.name == "MOTION" && !n.params.empty()) {
            if (const NodeParam* p = n.Param(kHashSlopeDir)) {
                out.slope_dir = p->number;
            }
            if (const NodeParam* p = n.Param(kHashSlopeAngle)) {
                out.slope_angle = p->number;
            }
        }
    }
    return true;
}

bool MotionArchive::Parse(std::vector<uint8_t> bytes, std::string* error) {
    bytes_ = std::move(bytes);
    motions_.clear();
    const std::span<const uint8_t> data(bytes_);
    if (At<uint32_t>(data, 0) != kMagic) {
        if (error) {
            *error = "not a motion archive";
        }
        return false;
    }
    const uint32_t count = At<uint32_t>(data, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = 0x20 + 16 * static_cast<size_t>(i);
        GaniMotion motion;
        motion.path_code = At<uint64_t>(data, e);
        const uint32_t offset = At<uint32_t>(data, e + 8);
        if (!ParseGani(data, offset, motion, error)) {
            return false;
        }
        motions_.push_back(std::move(motion));
    }
    return true;
}

const GaniMotion* MotionArchive::Find(uint64_t path_code) const {
    for (const GaniMotion& m : motions_) {
        if (m.path_code == path_code) {
            return &m;
        }
    }
    return nullptr;
}

}
