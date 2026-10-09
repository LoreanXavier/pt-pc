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

// ShPlayer_layers.mtar: walk loops, stand idle, start clips (1.0 m), stop clips (0.2 m)
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

// RIG_ROOT path length and displacement (model xz) over the advanced frames
struct BodyAdvance {
    float distance = 0.0f;
    glm::vec2 root{0.0f};
    int footsteps = 0;
    std::vector<uint64_t> sounds;
};

// one clip at a time, blended in from the held pose over 8 motion frames (0x986420, 0x9874F0 interpolation 8.0)
class PlayerBody {
public:
    enum class Kind : uint8_t { Node, Start, Stop };
    static constexpr float kBlendFrames = 8.0f;

    void Reset(int clip);
    void Play(Kind kind, int clip);
    // `drawn_only`: a pose for the drawing alone (the third person view's own body, Player::UpdateDrawPose), whose foot contacts
    // must not change the side the logic's footsteps sound on (LastFootLeft)
    BodyAdvance Advance(float frames, float blend_frames, bool drawn_only = false);
    Kind ClipKind() const { return kind_; }
    int Clip() const { return clip_; }
    float Frame() const { return frame_; }
    float Blend() const { return blend_; }
    glm::vec3 Head();
    glm::quat HeadRotation();
    // Skin matrices of plr0_main0_def.fmdl in the pose (model space), with the help bones of `help` placed as in demos;
    // `light_arm` lays the arm motion of the lit flashlight over the left arm (kLightArmMotion). With `sim` (the parts'
    // plr0_main0_def_s00.sim) and the body's world transform the jacket, hood and hair are simulated in world space from
    // it, else by the hand-made SimBones chains.
    // `reach` gets the left hand's transform in model space (SKL_013_LHAND) and returns where it should be: the upper arm and the
    // forearm then turn (two bone IK, the elbow kept in its plane) to put the wrist there and the hand takes that rotation
    using HandReach = std::function<bool(const glm::mat4& hand_model, glm::mat4& target_model)>;
    bool Skin(const anim::HelpBones* help, std::vector<glm::mat4>& skin, bool light_arm = false, const anim::SimRig* sim = nullptr,
              const glm::mat4* body_world = nullptr, const HandReach* reach = nullptr);
    // PT_HANDY_BONES=1: log the world position of both hand bones after skinning (model space world_ via body_world).
    // Diagnostic only; no pose or draw changes.
    void LogHandBones(const glm::mat4& body_world) const;
    // a bone's transform in model space after the last Skin (false before one, or for a name the skeleton lacks)
    bool BoneModel(const char* name, glm::mat4& out) const;
    // Planted feet for the drawn pose (third person, Player::UpdateDrawPose): for the left (0) and right (1) leg set `active`,
    // its IK target (the foot) goes to `target` (model space) and its knee swivel and the foot's rotation turn about the model's
    // vertical axis by `yaw` before the rig is evaluated, so a foot keeps its place on the floor while the body moves or turns
    // over it. Inactive legs keep the clip's pose
    struct FootPlant {
        bool active[2] = {false, false};
        glm::vec3 target[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
        float yaw[2] = {0.0f, 0.0f};
        bool Any() const { return active[0] || active[1]; }
    };
    void SetFootPlant(const FootPlant& plant) { plant_ = plant; }
    // the clip's foot targets (the legs' IK targets) in model space for the current pose, left first; false without the rig
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
    // game time since the last skinning, the simulation step
    float sim_seconds_ = 0.0f;
};

bool LoadPlayerAnimation(Vfs& vfs);
const PlayerLocomotion& PlayerLocomotionData();
int WalkDirection(float heading_delta);
bool LastFootLeft(bool fallback);

}

