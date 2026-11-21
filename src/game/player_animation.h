#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "engine/anim/ragdoll.h"
#include "engine/anim/rig.h"
#include "engine/anim/sim_bones.h"
#include "engine/anim/sim_physics.h"
#include "engine/anim/sim_rigid.h"

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
    void SetStride(bool on, const glm::vec2& way) {
        stride_on_ = on;
        stride_ = way;
        evaluated_ = false;
    }
    void Steer(int clip);
    BodyAdvance Advance(float frames, float blend_frames, bool drawn_only = false);
    Kind ClipKind() const { return kind_; }
    int Clip() const { return clip_; }
    float Frame() const { return frame_; }
    float Blend() const { return blend_; }
    glm::vec3 Head();
    glm::quat HeadRotation();
    using HandReach = std::function<bool(const glm::mat4& hand_model, glm::mat4& target_model)>;
    bool Skin(const anim::HelpBones* help, std::vector<glm::mat4>& skin, bool light_arm = false, const anim::SimRig* sim = nullptr,
              const glm::mat4* body_world = nullptr, const HandReach* reach = nullptr, const glm::mat4* right_target = nullptr,
              float right_grip = 0.0f, float left_grip = 0.0f);
    void LogHandBones(const glm::mat4& body_world) const;
    bool BoneModel(const char* name, glm::mat4& out) const;
    const std::vector<glm::mat4>& WorldBones() const { return world_; }
    static const anim::Skeleton* BodySkeleton();
    const anim::Ragdoll* ragdoll = nullptr;
    std::optional<glm::vec3> elbow_hint[2];
    float right_hint_share = 1.0f;
    bool ease_reach = false;
    bool snap_reach = false;
    float straight_wrist[2] = {0.0f, 0.0f};
    std::optional<std::pair<glm::vec3, glm::vec3>> stock_line;
    float stock_deep = 0.0f;
    glm::vec3 stock_comb{0.0f};
    std::optional<std::pair<glm::vec3, glm::vec3>> fore_line;
    struct WristLimits {
        bool on = false;
        float back = 0.0f, palm = 0.0f, little = 0.0f, thumb = 0.0f;
    };
    WristLimits wrist_limits[2];
    glm::vec3 shoulder_moved[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    glm::mat4 natural_right_[3] = {glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};
    bool natural_known_ = false;
    float steady_height_ = 0.0f;
    bool steady_primed_ = false;
    float steady_now_ = 0.0f;
    glm::vec2 carry_now_{0.0f};
    glm::mat4 natural_chest_{1.0f};
    bool natural_chest_known_ = false;
    float run = 0.0f;
    float run_arms = 1.0f;
    float aim = 0.0f;
    float aim_pitch = 0.0f;
    float aim_yaw = 0.0f;
    float aim_long = 0.0f;
    float aim_recoil = 0.0f;
    float look_yaw = 0.0f;
    float look_pitch = 0.0f;
    float steady = 0.0f;
    float reach_rate = 12.0f;
    glm::vec2 carry{0.0f};
    bool NaturalBone(const char* name, glm::mat4& out) const;
    glm::vec3 KeepOffTorso(const glm::vec3& point, float margin) const { return TorsoPush(point, margin); }
    glm::vec3 melee{0.0f};
    std::optional<std::array<glm::vec4, 5>> finger_shape[2];
    struct FootPlant {
        bool active[2] = {false, false};
        glm::vec3 target[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
        float yaw[2] = {0.0f, 0.0f};
        bool Any() const { return active[0] || active[1]; }
    };
    void SetFootPlant(const FootPlant& plant) { plant_ = plant; }
    int hold_feet[2] = {0, 0};
    bool aim_knees = false;
    float hips_lag = 0.0f;
    int FootHeld(int side) const { return held_[side & 1].lift > 0.0f ? 2 : held_[side & 1].held ? 1 : 0; }
    float FootTwist(int side) const { return held_[side & 1].twist; }
    bool FootTargets(glm::vec3 out[2]);

private:
    void ReachLeftHand(const glm::mat4& target);
    void ReachHand(bool right, const glm::mat4& target, bool turn_hand);
    void ShapeFingers(bool right, const std::array<glm::vec4, 5>& shape, float amount);
    glm::vec3 TorsoPush(const glm::vec3& point, float margin, float hem = 0.0f, float hem_side = 0.0f) const;
    float StockRoom(const glm::vec3& off, float tall) const;
    glm::vec3 EasedPush(glm::vec3& state, const glm::vec3& push) const;
    void KeepArmOut(bool right);
    void HoldFeet(const glm::mat4& body_world);
    void ReachFoot(int side, const glm::vec3& target, float twist);
    void AimKnee(int side);
    float ArmRoom() const;
    void RunOverlay(float amount);
    static constexpr float kWalkLeftReach = 42.0f;
    void IdleOverlay();
    void IdleHeadOverlay();
    void AimOverlay(float amount, float pitch);
    void SteadyTorso(float amount);
    void CarryOverlay(const glm::vec2& stance);
    void MeleeOverlay(const glm::vec3& lean);
    bool ApplyFootPlant(const FootPlant& plant, const anim::RigOutput& evaluated, anim::RigPose& pose) const;
    void TurnSubtree(int root, const glm::quat& turn, const glm::vec3& pivot);
    void LagHips(float lag);

public:

private:
    const BodyClip* Current() const;
    void Evaluate();
    void EvaluateStride();
    float StridePhase() const;
    bool stride_on_ = false;
    glm::vec2 stride_{0.0f};
    anim::RigPose stride_pose_;
    anim::RigOutput stride_out_;

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
    glm::vec3 right_fore_[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    bool right_fore_seen_ = false;
    float left_arm_push_ = 0.0f;
    struct HeldFoot {
        bool held = false;
        bool primed = false;
        glm::vec3 before{0.0f};
        glm::vec3 floor{0.0f};
        glm::vec3 off{0.0f};
        float yaw = 0.0f;
        float twist = 0.0f;
        float lift = 0.0f;
        float lift_height = 0.0f;
        glm::vec3 back_from{0.0f};
        float back_twist = 0.0f;
        glm::vec3 shown{0.0f};
        float skim_time = 0.0f;
        float skim = 0.0f;
        bool planted = false;
        float side_fix = 0.0f;
        glm::vec3 shown_step{0.0f};
    };
    HeldFoot held_[2];
    float pose_knee_toe_[2] = {0.0f, 0.0f};
    float pose_toe_side_[2] = {0.0f, 0.0f};
    float pose_knee_out_[2] = {0.0f, 0.0f};
    float knee_turn_[2] = {0.0f, 0.0f};
    glm::vec3 drawn_pole_[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    float reach_age_[2] = {1.0f, 1.0f};
    bool KneeToToe(int side, float& angle, float& toe_side, float* knee_out = nullptr) const;
    glm::vec3 held_body_{0.0f};
    glm::vec2 held_way_{0.0f, 1.0f};
    anim::RigPose plant_pose_;
    anim::RigOutput plant_out_;
    std::vector<glm::mat4> world_;
    glm::vec3 head_{0.0f, 1.586f, 0.04f};
    glm::quat head_rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    anim::SimBones sim_bones_;
    anim::SimRigid sim_;
    float run_node_ = 0.0f;
    float run_phase_ = 0.0f;
    float run_rate_ = 0.0f;
    float run_frame_ = -1.0f;
    bool run_primed_ = false;
    glm::vec3 hand_push_[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    glm::vec3 elbow_push_[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    glm::vec3 left_way_push_{0.0f};
    float left_reach_ = 0.0f;
    float left_reach_speed_ = 0.0f;
    bool snap_now_ = false;
    glm::vec3 forearm_{0.0f};
    bool forearm_primed_ = false;
    float run_node_speed_ = 0.0f;
    float run_laid_ = 0.0f;
    glm::mat4 left_target_{1.0f};
    bool left_wanted_ = false;
    glm::vec3 left_shift_{0.0f};
    glm::quat left_turn_{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 pole_[2] = {glm::vec3(0.0f), glm::vec3(0.0f)};
    bool pole_primed_[2] = {false, false};
    float twist_[2] = {0.0f, 0.0f};
    bool twist_primed_[2] = {false, false};
    float pole_share_[2] = {1.0f, 1.0f};
    float right_reach_ = 0.0f;
    float right_reach_speed_ = 0.0f;
    glm::mat4 right_target_{1.0f};
    float sim_seconds_ = 0.0f;
    float idle_time_ = 0.0f;
    float idle_weight_ = 0.0f;
    float aim_yaw_now_ = 0.0f;
    float look_yaw_now_ = 0.0f;
    float look_pitch_now_ = 0.0f;
};

bool LoadPlayerAnimation(Vfs& vfs);
using PlayerPoseEdit = std::function<void(const anim::Skeleton& skeleton, std::vector<glm::mat4>& world)>;
bool PlayerPoseSkin(const anim::HelpBones* help, float frame, const PlayerPoseEdit& edit, std::vector<glm::mat4>& skin);
bool PlayerClipPose(int clip, double frame, const anim::Skeleton& target, anim::Pose& out);
bool PlayerClipTravel(int clip, float& frames, float& travel, const anim::Skeleton* target = nullptr);
const PlayerLocomotion& PlayerLocomotionData();
int WalkDirection(float heading_delta);
bool LastFootLeft(bool fallback);

}
