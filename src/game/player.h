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
};

class PadLocks {
public:
    void Add(char set, const std::string& key, uint32_t mask);
    void Remove(char set, const std::string& key);
    uint32_t Mask(char set) const;
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
    /* 0x98726E: the walk ends on the second frame after the stick drops under 0.1, which the stop timing in the captures confirms. */
    static constexpr float kStandDebounce = 0.06673333f;
    // 0x97B010 clamps the look pitch to the camera component's +0x1F8/+0x1FC, which 0x981130 (vfunc +0xD8) takes from +0x28/+0x2C
    // of the parameter block 0x127D580 builds at 0x1C97840 (0x127D1A0 writes -1.2217305 and 0.9599311 there): the original's pitch
    // is positive down, so the view goes 70 degrees up and 55 degrees down (ceillamp_f100 holds -55.00 from 3380 to 3500 while
    // its look target lies 56.7 degrees down). The port's pitch is positive up.
    static constexpr float kPitchUp = 1.2217305f;
    static constexpr float kPitchDown = 0.9599311f;
    static float ClampPitch(float value) { return value < -kPitchDown ? -kPitchDown : value > kPitchUp ? kPitchUp : value; }
    // the peephole theater's look aperture (UpdateLook), for a view the render turns between ticks; no change outside the theater
    void ClampPeepholeLook(float& yaw_value, float& pitch_value) const;

    // 0x963570: radius 0.4 (CharacterShape); only the sliding variant (PT_CONTROLLER_SLIDE=1) keeps its 0.3
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
    // 0xB010B0: the trap object sits at the controller sphere center
    glm::vec3 TrapPoint() const { return controller.position + glm::vec3(0.0f, controller.shape.center_height, 0.0f); }
    // the tick that completes a 29.97 fps frame (0xB010B0 tests traps once per frame, after the move)
    bool FrameEnds() const { return frame_time_ + frame_step_ >= 1.0 - 1e-4; }
    glm::vec3 Eye() const { return eye_; }
    // plr0_main0_def in the world: the body position (feet) turned by the body yaw, the frame of Head() and of BodySkin()
    glm::mat4 BodyTransform() const;
    // the drawn pose of the third person view when it is on (SetDrawPose), else the logic's
    bool BodySkin(const anim::HelpBones* help, std::vector<glm::mat4>& skin, bool light_arm = false, const anim::SimRig* sim = nullptr,
                  const PlayerBody::HandReach* reach = nullptr) {
        if (draw_pose_) {
            // the drawn transform, half a step behind the logic on a frame's last tick: the jacket's simulation followed the
            // logic's 30 Hz steps and its hem shook while walking
            const glm::mat4 world = PoseTransform();
            return drawn_body_.Skin(help, skin, light_arm, sim, &world, reach);
        }
        const glm::mat4 world = BodyTransform();
        return body_.Skin(help, skin, light_arm, sim, &world, reach);
    }
    // a bone of the last BodySkin in model space (PoseTransform places it in the world)
    bool BoneModel(const char* name, glm::mat4& out) const { return draw_pose_ ? drawn_body_.BoneModel(name, out) : body_.BoneModel(name, out); }
    // The third person view's own drawn body (Game::SetThirdPerson; not in the original, gameplay.md 10.6). It plays the logic's
    // clips as they change, but a stop clip to its end (the logic plays one frame of it) and walks out that clip's travel, which it
    // gives back while the player walks again, and while the player stands it keeps its feet planted on the floor as the body turns
    // over them and steps when they are turned too far (PlayerBody::FootPlant). The logic's body (the eye, the published state,
    // the footsteps) is not touched; with the view off nothing of it runs
    void SetDrawPose(bool on) {
        draw_sync_ = draw_sync_ || (on && !draw_pose_);
        draw_pose_ = on;
    }
    bool DrawPose() const { return draw_pose_; }
    // where the body is drawn: DrawnBodyTransform, moved by the drawn body's offset while SetDrawPose is on
    glm::mat4 PoseTransform() const {
        return draw_pose_ ? glm::translate(glm::mat4(1.0f), draw_offset_) * DrawnBodyTransform() : DrawnBodyTransform();
    }
    // the drawn pose's state for the logs (PT_THIRD_TRACE)
    std::string DescribeDrawPose() const;
    // 0x1282C50: the offset of the handy light's look target ray (x right, y down; RenderSceneBuilder::HandyTarget): the right
    // stick with the invert options (zero while the look is locked), half the left stick's x (zero while the left stick is
    // locked) and 0.25 per held digital look button, each axis clamped to [-1, 1]
    glm::vec2 LightStick() const { return light_stick_; }
    // input script `slightstick x y` (compare_ref's replays feed the capture's pad log; the look does not turn)
    // VR (docs/vr.md): the turns the game gave the player since the VR view last took them; the view turns its world by them
    float TakeVrTurn() { return std::exchange(vr_turn_, 0.0f); }
    // an input script's look pitch (its facings): in VR it stands in for the head's pitch in the logic camera from then on, as
    // the script stands in for the player's head (tests only; the eyes keep the headset's view)
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
    // 0x9406C0 publishes the camera yaw
    glm::vec3 BodyForward() const { return CameraForward(); }
    float FoxYaw() const { return CameraFoxYaw(); }
    float CameraFoxYaw() const;
    // the character's yaw, which follows the camera yaw (UpdateBody); 0x9406C0 builds the published rotation (+0x40) from it
    float BodyFoxYaw() const { return body_yaw_; }
    // the body yaw as the player service's double buffer (0x91E900) hands it to readers such as the trap check PlayerInputDir
    // (0x915850, vfunc +0x28 = 0x91F810, rotation +0x40): the value published at the end of the previous original frame
    float PublishedBodyFoxYaw() const { return published_read_.body_yaw; }
    // the position (feet) in the same read buffer: ShPlayer_PublishState (0x9406C0) writes position and rotation once per
    // original frame into the player service's write buffer, and the readers (traps, Lisa's sense step 0x1287F80) get the
    // read buffer, which the swap (0x91EB10) turns over at the next frame: the previous frame's values in either order
    glm::vec3 PublishedFeet() const { return published_read_.feet; }
    // the camera pitch (+0x50; the port's pitch, positive up) in the same read buffer: the Freezer's look-up test reads it
    float PublishedPitch() const { return published_read_.pitch; }

    // the zoom-out branch of 0x983B20 ran this tick (the zoom was released or the player walks), where the original
    // calls the nazo service's AbortPeephole (0x984A62)
    bool ZoomFalling() const { return zoom_falling_; }
    // flags 4 (STAND) and 5 (WALK) as 0x9406C0 publishes them: the read buffer, the previous frame's state
    bool Standing() const { return published_read_.standing; }
    bool Walking() const { return !published_read_.standing; }
    float StickMagnitude() const { return stick_magnitude_; }
    float StickHeading() const { return stick_heading_; }
    uint32_t HeldButtons() const { return held_ & ~locks.Mask('A'); }
    uint32_t PressedButtons() const { return pressed_ & ~locks.Mask('A'); }
    // pad+0x50 is read once per 29.97 fps frame: the presses of both ticks
    uint32_t FramePressedButtons() const { return frame_pressed_ & ~locks.Mask('A'); }
    bool LeftStickLocked() const { return (locks.Mask('B') & 1) != 0; }

    void SetDisableLeftStick(bool disable);
    void Warp(const glm::vec3& position, float fox_yaw);
    void HandCameraBack(const glm::vec3& position, float yaw_value, float pitch_value);
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
    // pt.ini [camera] roll: 1 takes the original's share of the head roll, 0 keeps the camera level
    float camera_roll = 1.0f;
    bool zooming = false;
    bool visible = true;
    bool spawned = false;
    int foot_steps = 0;
    int sounding_steps = 0;
    std::vector<uint64_t> anim_sounds;

private:
    void UpdateLocomotion(const InputState& input);
    void UpdateLook(float dt, const InputState& input, const PlayerFrameContext& context);
    void UpdateZoom(float dt, const PlayerFrameContext& context);
    void LocomotionFrame(const PlayerFrameContext& context);
    void UpdateBody(float dt, const CollisionWorld& world);
    void UpdateEye(float dt);
    void ResetLocomotion();
    float SpeedRate(float heading_delta, float magnitude, bool blur) const;
    glm::vec3 BodyToWorld(const glm::vec3& local) const;
public:
    // the drawn camera and body: the logic's, moved by the half step the drawing trails on a frame's last tick
    glm::vec3 DrawnOffset() const { return drawn_offset_; }
    glm::mat4 DrawnBodyTransform() const;
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
    // in the world, x and z
    glm::vec3 draw_offset_{0.0f};
    // each foot (left, right): free (the clip's), locked on the floor where it was put down (world position and the body yaw
    // then) or stepping from where it was to the clip's place
    enum class Foot : uint8_t { Free, Locked, Stepping };
    struct FootState {
        Foot state = Foot::Free;
        glm::vec3 floor{0.0f};
        float yaw = 0.0f;
        glm::vec3 from{0.0f};
        float from_twist = 0.0f;
        float time = 0.0f;
        float length = 0.0f;
        bool arc = false;
    };
    FootState feet_[2];
    float turn_still_ = 0.0f;
    float last_body_yaw_ = 0.0f;
    PlayerBody::FootPlant plant_;
    glm::vec3 eye_{0.0f};
    float roll_ = 0.0f;
    glm::vec3 handoff_local_{0.0f};
    float handoff_weight_ = 0.0f;
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
    // the view's turns by mouse and keyboard look in the last four ticks (to the right, up, the tick's dt; UpdateLook)
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
    // the frame's displacement, moved once at its end (0xB001E0 runs once per 29.97 fps frame), and the drawn body position,
    // half a step behind on the frame's last tick so the 60 Hz image moves every tick (render only)
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
    float left_lock_timer_ = 0.0f;
    float zoom_target_ = 1.35f;
    float zoom_velocity_ = 1.0f;
    bool zoom_active_ = false;
    bool zoom_falling_ = false;
};

}
