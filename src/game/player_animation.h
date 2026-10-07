#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/anim/rig.h"
#include "engine/anim/sim_bones.h"
#include "engine/anim/sim_physics.h"

namespace pt {
class Vfs;
}

namespace pt::anim {
struct GaniMotion;
class HelpBones;
}

namespace pt::game {

struct WalkFootstep {
    float frame = 0.0f;
    bool left = false;
};

struct WalkSound {
    float frame = 0.0f;
    uint64_t event = 0;
};

struct BodyClip {
    uint64_t path_code = 0;
    float frames = 0.0f;
    std::vector<float> path;
    std::vector<glm::vec2> root;
    std::vector<WalkFootstep> steps;
    std::vector<WalkSound> sounds;
    const anim::GaniMotion* motion = nullptr;

    bool Valid() const { return frames > 0.0f && path.size() >= 2 && root.size() == path.size() && motion; }
    float PathAt(float frame) const;
    glm::vec2 RootAt(float frame) const;
};

enum BodyClipId : int { kClipFront, kClipRight, kClipBack, kClipLeft, kClipStand, kClipCount };

struct PlayerLocomotion {
    bool loaded = false;
    std::array<BodyClip, kClipCount> clips{};
    std::array<BodyClip, 4> starts{};
    std::array<BodyClip, 4> stops{};
};

struct BodyAdvance {
    float distance = 0.0f;
    glm::vec2 root{0.0f};
    int footsteps = 0;
    std::vector<uint64_t> sounds;
};

class PlayerBody {
public:
    enum class Kind : uint8_t { Node, Start, Stop };
    static constexpr float kBlendFrames = 8.0f;

    void Reset(int clip);
    void Play(Kind kind, int clip);
    BodyAdvance Advance(float frames, float blend_frames, bool drawn_only = false);
    Kind ClipKind() const { return kind_; }
    int Clip() const { return clip_; }
    float Frame() const { return frame_; }
    float Blend() const { return blend_; }
    glm::vec3 Head();
    glm::quat HeadRotation();
    using HandReach = std::function<bool(const glm::mat4& hand_model, glm::mat4& target_model)>;
    bool Skin(const anim::HelpBones* help, std::vector<glm::mat4>& skin, bool light_arm = false, const anim::SimRig* sim = nullptr,
              const glm::mat4* body_world = nullptr, const HandReach* reach = nullptr);
    void LogHandBones(const glm::mat4& body_world) const;
    bool BoneModel(const char* name, glm::mat4& out) const;
    struct FootPlant {
        bool active[2] = {false, false};
        glm::vec3 target[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
        float yaw[2] = {0.0f, 0.0f};
        bool Any() const { return active[0] || active[1]; }
    };
    void SetFootPlant(const FootPlant& plant) { plant_ = plant; }
    bool FootTargets(glm::vec3 out[2]);

private:
    void ReachLeftHand(const glm::mat4& target);
    bool ApplyFootPlant(const FootPlant& plant, const anim::RigOutput& evaluated, anim::RigPose& pose) const;
    void TurnSubtree(int root, const glm::quat& turn, const glm::vec3& pivot);

public:

private:
    const BodyClip* Current() const;
    void Evaluate();

    Kind kind_ = Kind::Node;
    int clip_ = kClipStand;
    float frame_ = 0.0f;
    float blend_ = 1.0f;
    bool evaluated_ = false;
    anim::RigPose from_;
    anim::RigPose pose_;
    anim::RigOutput out_;
    anim::RigPose arm_pose_;
    anim::RigOutput arm_out_;
    FootPlant plant_;
    anim::RigPose plant_pose_;
    anim::RigOutput plant_out_;
    std::vector<glm::mat4> world_;
    glm::vec3 head_{0.0f, 1.586f, 0.04f};
    glm::quat head_rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    anim::SimBones sim_bones_;
    anim::SimPhysics sim_;
    float sim_seconds_ = 0.0f;
};

bool LoadPlayerAnimation(Vfs& vfs);
const PlayerLocomotion& PlayerLocomotionData();
int WalkDirection(float heading_delta);
bool LastFootLeft(bool fallback);

}
