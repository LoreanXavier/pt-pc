#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/anim/gani.h"
#include "engine/anim/skeleton.h"

namespace pt::anim {

enum class RigUnitType : uint32_t {
    Root = 1,
    Rotation = 2,
    Leg = 3,
    LocalRotation = 4,
    Waist = 7,
    Arm = 8,
    Chain = 11,
};

struct RigUnit {
    uint32_t type = 0;
    int parent_joint = -1;
    int parent_unit = -1;
    std::vector<int> joints;
    std::vector<int> tracks;
    int end_joint = -1;
    glm::vec3 axis{0.0f};

    bool Is(RigUnitType t) const { return type == static_cast<uint32_t>(t); }
};

struct RigJoint {
    uint32_t unit = 0;
    uint32_t hash = 0;
};

struct RigMask {
    uint32_t hash = 0;
    std::string name;
    std::vector<float> weights;
};

class Rig {
public:
    static constexpr uint32_t kMagic = 0x21EA256C;

    bool Parse(std::span<const uint8_t> data, std::string* error);
    const std::string& Name() const { return name_; }
    uint32_t TrackCount() const { return track_count_; }
    const std::vector<RigUnit>& Units() const { return units_; }
    const std::vector<RigJoint>& Joints() const { return joints_; }
    const std::vector<RigMask>& Masks() const { return masks_; }
    bool Evaluable() const { return evaluable_; }
    bool ChannelIsRotation(size_t unit, size_t channel) const;

private:
    std::string name_;
    uint32_t track_count_ = 0;
    std::vector<RigUnit> units_;
    std::vector<RigJoint> joints_;
    std::vector<RigMask> masks_;
    bool evaluable_ = false;
};

struct RigBinding {
    std::vector<int> joint_bone;

    bool Bind(const Rig& rig, const Skeleton& skeleton);
    int Bone(int joint) const;
    bool Valid() const { return !joint_bone.empty(); }
};

struct RigPose {
    std::vector<glm::vec4> tracks;
    bool relative = false;
};

struct RigOutput {
    std::vector<glm::quat> rotation;
    std::vector<glm::vec3> position;
};

bool SampleRigPose(const Rig& rig, const GaniMotion& motion, double frame, RigPose& out, bool loop = true);
void EvaluateRig(const Rig& rig, const RigBinding& binding, const Skeleton& skeleton, const RigPose& pose, RigOutput& out);
void MakeRigPoseRelative(const Rig& rig, const RigBinding& binding, const Skeleton& skeleton, const RigOutput& evaluated, RigPose& pose);
void BlendRigPose(const Rig& rig, const RigPose& from, const RigPose& to, float weight, RigPose& out);
void RigOutputToPose(const Skeleton& skeleton, const RigOutput& output, Pose& out);
glm::vec3 RigRootTranslation(const Rig& rig, const RigPose& pose);
glm::quat RigRootRotation(const Rig& rig, const RigPose& pose);

}
