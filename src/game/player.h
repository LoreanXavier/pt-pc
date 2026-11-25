#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/physics/collision_world.h"
#include "engine/platform/input.h"
#include "engine/render/camera.h"
#include "game/parameters.h"
#include "game/player_animation.h"

namespace pt::game {

struct HandyLight {
    bool enable = false;
    float lumen = 0.1f;
};

struct PlayerFrameContext {
    bool nazo_action_armed = false;
    bool big_zoom = false;
    bool full_screen_blur = false;
    bool invert_x = false;
    bool invert_y = false;
    bool peephole_theater = false;
    bool peephole_center_valid = false;
    float peephole_center_yaw = 0.0f;
};

class PadLocks {
public:
    void Add(char set, const std::string& key, uint32_t mask);
    void Remove(char set, const std::string& key);
    uint32_t Mask(char set) const;
    uint32_t MaskExcept(char set, const std::string& key) const;
    bool Any() const { return !a_.empty() || !b_.empty() || !c_.empty(); }
    std::string Describe() const;

private:
    std::map<std::string, uint32_t>& Set(char set) { return set == 'A' ? a_ : set == 'B' ? b_ : c_; }
    std::map<std::string, uint32_t> a_;
    std::map<std::string, uint32_t> b_;
    std::map<std::string, uint32_t> c_;
};

class Player {
public:
    static constexpr float kFilmHeightMm = 13.5f;
    static constexpr float kZoomHeld = 1.35f;
    /* 0x98726E: the walk ends on the second frame after the stick drops under 0.1, which the stop timing in the captures confirms. */
    static constexpr float kStandDebounce = 0.06673333f;
    static constexpr float kPitchUp = 1.2217305f;
    static constexpr float kPitchDown = 0.9599311f;
    static float ClampPitch(float value) { return value < -kPitchDown ? -kPitchDown : value > kPitchUp ? kPitchUp : value; }
    void ClampPeepholeLook(float& yaw_value, float& pitch_value) const;
    static void ClampPeepholeLook(float& yaw_value, float& pitch_value, float center_yaw);
    glm::vec2 PeepholeTurnDelta(glm::vec2 delta) const;

    Player() {
        if (!OriginalController()) {
            controller.shape.radius = 0.3f;
        }
    }
    void Spawn(const glm::mat4& world);
    void Update(float dt, const InputState& input, const CollisionWorld& world, const PlayerFrameContext& context);
    void EndFrame();
    Camera MakeCamera() const;

    glm::vec3 Feet() const { return controller.position; }
    glm::vec3 TrapPoint() const { return controller.position + glm::vec3(0.0f, controller.shape.center_height, 0.0f); }
    bool FrameEnds() const { return frame_time_ + frame_step_ >= 1.0 - 1e-4; }
    glm::vec3 Eye() const { return eye_; }
    const PlayerBody& LogicBody() const { return body_; }
    glm::mat4 BodyTransform() const;
    bool BodySkin(const anim::HelpBones* help, std::vector<glm::mat4>& skin, bool light_arm = false, const anim::SimRig* sim = nullptr,
                  const PlayerBody::HandReach* reach = nullptr) {
        const glm::mat4* right = right_hand_target ? &*right_hand_target : nullptr;
        if (draw_pose_) {
            const glm::mat4 world = PoseTransform();
            return drawn_body_.Skin(help, skin, light_arm, sim, &world, reach, right, right_grip, left_grip);
        }
        const glm::mat4 world = BodyTransform();
        return body_.Skin(help, skin, light_arm, sim, &world, reach, right, right_grip, left_grip);
    }
    void SetRun(float amount, float arms = 1.0f) {
        body_.run = drawn_body_.run = amount;
        body_.run_arms = drawn_body_.run_arms = arms;
    }
    glm::vec3 stop_carry_{0.0f};
    float stop_share_ = 0.0f;
    void SetReachEase(bool on) { body_.ease_reach = drawn_body_.ease_reach = on; }
    void SnapReach() { body_.snap_reach = drawn_body_.snap_reach = true; }
    void SetFingerShapes(const std::optional<std::array<glm::vec4, 5>>& left, const std::optional<std::array<glm::vec4, 5>>& right) {
        body_.finger_shape[0] = drawn_body_.finger_shape[0] = left;
        body_.finger_shape[1] = drawn_body_.finger_shape[1] = right;
    }
    void SetAim(float amount, float pitch, float yaw_offset = 0.0f) {
        body_.aim = drawn_body_.aim = amount;
        body_.aim_pitch = drawn_body_.aim_pitch = pitch;
        body_.aim_yaw = drawn_body_.aim_yaw = yaw_offset;
    }
    void SetHeadLook(float yaw_offset, float pitch_val) {
        body_.look_yaw = drawn_body_.look_yaw = yaw_offset;
        body_.look_pitch = drawn_body_.look_pitch = pitch_val;
    }
    void SetAimStance(float long_gun, float recoil) {
        body_.aim_long = drawn_body_.aim_long = long_gun;
        body_.aim_recoil = drawn_body_.aim_recoil = recoil;
    }
    void SetMeleeBody(const glm::vec3& lean) { body_.melee = drawn_body_.melee = lean; }
    void SetStockLine(const std::optional<std::pair<glm::vec3, glm::vec3>>& line, float deep = 0.0f, const glm::vec3& comb = glm::vec3(0.0f), const std::optional<std::pair<glm::vec3, glm::vec3>>& fore = std::nullopt) {
        body_.stock_line = drawn_body_.stock_line = line;
        body_.stock_deep = drawn_body_.stock_deep = deep;
        body_.stock_comb = drawn_body_.stock_comb = comb;
        body_.fore_line = drawn_body_.fore_line = fore;
    }
    void SetStraightWrists(float left, float right) {
        body_.straight_wrist[0] = drawn_body_.straight_wrist[0] = left;
        body_.straight_wrist[1] = drawn_body_.straight_wrist[1] = right;
    }
    void SetRagdoll(const anim::Ragdoll* ragdoll) { body_.ragdoll = drawn_body_.ragdoll = ragdoll; }
    const std::vector<glm::mat4>& WorldBones() const { return (draw_pose_ ? drawn_body_ : body_).WorldBones(); }
    void SetWristLimits(const PlayerBody::WristLimits& left, const PlayerBody::WristLimits& right) {
        for (PlayerBody* body : {&body_, &drawn_body_}) {
            body->wrist_limits[0] = left;
            body->wrist_limits[1] = right;
        }
    }
    glm::vec3 ShoulderMoved(bool right) const { return (draw_pose_ ? drawn_body_ : body_).shoulder_moved[right ? 1 : 0]; }
    void SetCarryPose(float weight, const glm::mat4& hand, const glm::vec3& left_pole, const glm::vec3& right_pole) {
        for (PlayerBody* body : {&body_, &drawn_body_}) {
            body->carry_weight = weight;
            body->carry_hand = hand;
            body->carry_pole[0] = left_pole;
            body->carry_pole[1] = right_pole;
        }
    }
    void SetElbowHints(const std::optional<glm::vec3>& left, const std::optional<glm::vec3>& right, float right_share = 1.0f) {
        body_.elbow_hint[0] = drawn_body_.elbow_hint[0] = left;
        body_.elbow_hint[1] = drawn_body_.elbow_hint[1] = right;
        body_.right_hint_share = drawn_body_.right_hint_share = right_share;
    }
    bool BoneModel(const char* name, glm::mat4& out) const { return draw_pose_ ? drawn_body_.BoneModel(name, out) : body_.BoneModel(name, out); }
    bool NaturalBone(const char* name, glm::mat4& out) const { return draw_pose_ ? drawn_body_.NaturalBone(name, out) : body_.NaturalBone(name, out); }
    glm::vec3 KeepOffTorso(const glm::vec3& point, float margin) const { return draw_pose_ ? drawn_body_.KeepOffTorso(point, margin) : body_.KeepOffTorso(point, margin); }
    void SetReachRate(float rate) { body_.reach_rate = drawn_body_.reach_rate = rate; }
    void SetLunge(float metres) { lunge_ = metres; }
    void SetSteady(float amount, const glm::vec2& carry = glm::vec2(0.0f)) {
        body_.steady = drawn_body_.steady = amount;
        body_.carry = drawn_body_.carry = carry;
    }
    void SetDrawPose(bool on) {
        draw_sync_ = draw_sync_ || (on && !draw_pose_);
        draw_pose_ = on;
    }
    bool DrawPose() const { return draw_pose_; }
    int DrawnFootState(int side) const {
        return feet_[side & 1].state != Foot::Free ? static_cast<int>(feet_[side & 1].state) : drawn_body_.FootHeld(side & 1);
    }
    void SetStrideBlend(bool on) { stride_blend_ = on; }
    glm::mat4 PoseTransform() const {
        const glm::mat4 body = draw_pose_ ? glm::translate(glm::mat4(1.0f), draw_offset_) * DrawnBodyTransform() : DrawnBodyTransform();
        return sprint_lean != 0.0f ? glm::rotate(body, sprint_lean, glm::vec3(1.0f, 0.0f, 0.0f)) : body;
    }
    std::string DescribeDrawPose() const;
    glm::vec2 LightStick() const { return light_stick_; }
    glm::vec2 DemoLightStick(const InputState& input, bool invert_x, bool invert_y) const;
    float TakeVrTurn() { return std::exchange(vr_turn_, 0.0f); }
    void SetScriptPitch(float value) {
        pitch = target_pitch = ClampPitch(value);
        vr_script_pitch_ = pitch;
    }
    void SetLightStickOverride(const std::optional<glm::vec2>& stick) {
        light_stick_override_ = stick;
        if (stick) {
            light_stick_ = *stick;
        }
    }
    glm::vec3 LookDirection() const;
    glm::vec3 CameraForward() const;
    glm::vec3 BodyForward() const { return CameraForward(); }
    float FoxYaw() const { return CameraFoxYaw(); }
    float CameraFoxYaw() const;
    float BodyFoxYaw() const { return body_yaw_; }
    float PublishedBodyFoxYaw() const { return published_read_.body_yaw; }
    glm::vec3 PublishedFeet() const { return published_read_.feet; }
    float PublishedPitch() const { return published_read_.pitch; }

    bool ZoomFalling() const { return zoom_falling_; }
    bool Standing() const { return published_read_.standing; }
    bool Walking() const { return !published_read_.standing; }
    float StickMagnitude() const { return stick_magnitude_; }
    float StickHeading() const { return stick_heading_; }
    float HipsLag() const { return hips_lag_; }
    int DrawnHold(int side) const { return drawn_body_.hold_feet[side & 1]; }
    bool DrawnHaveFeet() const { return drawn_body_.aim_knees; }
    uint32_t HeldButtons() const { return held_ & ~locks.Mask('A'); }
    uint32_t PressedButtons() const { return pressed_ & ~locks.Mask('A'); }
    uint32_t FramePressedButtons() const { return frame_pressed_ & ~locks.Mask('A'); }
    bool LeftStickLocked() const { return (locks.Mask('B') & 1) != 0; }

    void SetDisableLeftStick(bool disable);
    void Warp(const glm::vec3& position, float fox_yaw);
    void DemoWarp(const glm::vec3& position, float fox_yaw);
    void DemoHeldFrame(const InputState& input);
    glm::vec3 FollowHead(const CollisionWorld& world, const glm::vec3& displacement);
    void HandCameraBack(const glm::vec3& position, float yaw_value, float pitch_value, bool anchor_after_move = false);
    void FollowCamera(float yaw_value, float pitch_value);

    CharacterController controller;
    PlayerParameters params;
    HandyLight handy_light;
    PadLocks locks;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float target_yaw = 0.0f;
    float target_pitch = 0.0f;
    float zoom = 1.0f;
    float sprint_rate = 1.0f;
    float sprint_stride = 1.0f;
    float sprint_lean = 0.0f;
    std::optional<glm::mat4> right_hand_target;
    float right_grip = 0.0f;
    float left_grip = 0.0f;
    float camera_roll = 1.0f;
    bool zooming = false;
    bool visible = true;
    bool spawned = false;
    int foot_steps = 0;
    int sounding_steps = 0;
    std::vector<uint64_t> anim_sounds;

private:
    void UpdateLocomotion(const InputState& input, float dt);
    void UpdateLook(float dt, const InputState& input, const PlayerFrameContext& context);
    void UpdateZoom(float dt, const PlayerFrameContext& context);
    void LocomotionFrame(const PlayerFrameContext& context);
    void StepStandWalk(float magnitude, bool left_locked);
    void UpdateBody(float dt, const CollisionWorld& world);
    void UpdateEye(float dt);
    void ResetLocomotion();
    float SpeedRate(float heading_delta, float magnitude, bool blur) const;
    glm::vec3 BodyToWorld(const glm::vec3& local) const;
public:
    glm::vec3 DrawnOffset() const { return drawn_offset_; }
    glm::mat4 DrawnBodyTransform() const;
    bool stride_blend_ = false;
    glm::vec2 stride_way_{0.0f};
    glm::vec2 stride_way_speed_{0.0f};
    float strafe_weight_ = 0.0f;
    float strafe_weight_speed_ = 0.0f;
    float strafe_phase_ = 0.0f;
    bool strafe_left_ = false;
    glm::vec3 strafe_last_{0.0f};
    bool strafe_primed_ = false;
    float draw_turn_ = 0.0f;
    float draw_turn_speed_ = 0.0f;
    glm::vec2 stick_soft_{0.0f};
    bool stick_eased_ = false;
    glm::vec2 stick_soft_speed_{0.0f};
private:
    glm::vec3 WorldToBody(const glm::vec3& world) const;

    PlayerBody body_;
    void UpdateDrawPose(float dt, const CollisionWorld& world);
    PlayerBody drawn_body_;
    bool draw_pose_ = false;
    bool draw_sync_ = false;
    bool draw_stopping_ = false;
    PlayerBody::Kind follow_kind_ = PlayerBody::Kind::Node;
    int follow_clip_ = 0;
    int stop_clip_ = 0;
    glm::vec3 draw_offset_{0.0f};
    float lunge_ = 0.0f;
    glm::vec3 lunge_offset_{0.0f};
    enum class Foot : uint8_t { Free, Locked, Stepping };
    struct FootState {
        Foot state = Foot::Free;
        glm::vec3 floor{0.0f};
        float yaw = 0.0f;
        glm::vec3 from{0.0f};
        float from_yaw = 0.0f;
        float time = 0.0f;
        float length = 0.0f;
        bool arc = false;
    };
    FootState feet_[2];
    float turn_still_ = 0.0f;
    float last_body_yaw_ = 0.0f;
    float hips_yaw_ = 0.0f;
    float hips_rate_ = 0.0f;
    float hips_lag_ = 0.0f;
    PlayerBody::FootPlant plant_;
    glm::vec3 eye_{0.0f};
    float roll_ = 0.0f;
    float look_roll_ = 0.0f;
    glm::vec3 handoff_local_{0.0f};
    float handoff_weight_ = 0.0f;
    glm::vec3 handoff_world_{0.0f};
    bool handoff_anchor_pending_ = false;
    float body_yaw_ = 0.0f;
    float body_yaw_target_ = 0.0f;
    struct Published {
        glm::vec3 feet{0.0f};
        float body_yaw = 0.0f;
        bool standing = true;
        float pitch = 0.0f;
    };
    Published published_read_;
    Published published_write_;
    bool published_swap_ = false;
    void PublishNow();
    glm::vec2 look_velocity_{0.0f};
    bool peephole_look_active_ = false;
    float peephole_yaw_ = 0.0f;
    glm::vec2 light_stick_{0.0f};
    std::array<glm::vec4, 4> pc_turns_{};
    size_t pc_turn_next_ = 0;
    std::optional<glm::vec2> light_stick_override_;
    float vr_turn_ = 0.0f;
    std::optional<float> vr_script_pitch_;
    float vr_last_yaw_ = 0.0f;
    bool vr_looked_ = false;
    struct MotionFrame {
        float rate = 1.0f;
        float heading = 0.0f;
        bool steered = false;
    };
    double frame_time_ = 1.0;
    double frame_step_ = 0.5;
    bool frame_start_ = false;
    glm::vec3 frame_offset_{0.0f};
    float frame_dt_ = 0.0f;
    bool frame_gravity_ = false;
    glm::vec3 drawn_offset_{0.0f};
    MotionFrame motion_;
    float stick_magnitude_ = 0.0f;
    float stick_heading_ = 0.0f;
    uint32_t held_ = 0;
    uint32_t pressed_ = 0;
    uint32_t frame_pressed_ = 0;
    bool standing_ = true;
    float idle_timer_ = 0.0f;
    bool carried_walk_logged_ = false;
    float left_lock_timer_ = 0.0f;
    float zoom_target_ = kZoomHeld;
    float zoom_velocity_ = 1.0f;
    bool zoom_active_ = false;
    bool zoom_falling_ = false;
};

}
