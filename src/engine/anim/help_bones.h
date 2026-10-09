#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/anim/skeleton.h"

namespace pt::anim {

// Help-bone drivers (.frdv): the HelpBone anim plugin (0xAE6910; per frame 0xAE7410 -> 0xAE7420) places every driven
// bone from the model space pose once the rig and all other bones are placed. Types 1, 2, 7, 11, 12 and 13 are the
// ones in P.T.'s files (docs/formats/motion.md, section .frdv); other types leave their bone as it is.
enum class HelpBoneType : uint16_t {
    SlideByAngle = 1,
    Follow = 2,
    Twist = 7,
    SwingTurn = 11,
    TwistSlide = 12,
    SwingTurnSlide = 13,
};

struct HelpBoneEntry {
    uint16_t type = 0;
    int16_t driven = -1;
    int16_t source = -1;
    int16_t parent = -1;
    int16_t reference = -1;
    float weight = 0.0f;
    float min = 0.0f;
    float max = 0.0f;
    uint32_t axis = 0;
    float slide = 0.0f;
    float slide_min = 0.0f;
    float slide_max = 0.0f;
    uint32_t slide_axis = 0;
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};

    bool Is(HelpBoneType t) const { return type == static_cast<uint16_t>(t); }
};

class HelpBones {
public:
    static constexpr uint32_t kVersion = 0x0BFFB0A8;
    static constexpr size_t kEntrySize = 0x80;

    bool Parse(std::span<const uint8_t> data, std::string* error);
    const std::vector<HelpBoneEntry>& Entries() const { return entries_; }
    size_t UnknownEntries() const;
    // Model space rotations and positions of every bone, changed in place for the driven bones in entry order.
    void Evaluate(std::span<const glm::vec3> bind_local, std::span<glm::quat> rotation, std::span<glm::vec3> position) const;
    // The same on bone matrices without scale (ComputeBoneWorld output).
    void Apply(const Skeleton& skeleton, std::vector<glm::mat4>& world) const;

private:
    std::vector<HelpBoneEntry> entries_;
};

// PT_HELP_BONES_OFF=1 turns the evaluation off (for comparisons).
bool HelpBonesEnabled();

}
