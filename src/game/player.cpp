#include "game/player.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "game/player_animation.h"

namespace pt::game {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kMotionFps = 59.94006f;
/* The original steps the player once per 29.97 fps frame; the port ticks at 60 Hz and treats two ticks as one frame. */
constexpr float kOriginalFrame = 1.0f / 29.97003f;
constexpr float kTickToOriginal = 60.0f / kMotionFps;
constexpr glm::vec3 kHeadEyeOffset(0.0f, 0.06f, 0.015f);
/* Fitted against the PS4 captures: half the head bone's roll lands within 0.2 degrees over the walk cycle, the filtered head rotation alone does not. */
constexpr float kHeadRollShare = 0.5f;
constexpr float kLookDeadZone = 0.094117648f;
constexpr float kLookCrossRatio = 0.15f;
constexpr float kLookExponent = 2.0f;
constexpr float kLookAccelFrames = 12.0f;
constexpr float kLookDecelFrames = 1.0f;
constexpr float kLookFrame = 0.016683333f;
constexpr float kLookFocal = 21.0f;
constexpr float kBodyTurnDivisor = 12.0f;

float Wrap(float a) {
    const float t = a + kPi;
    return t >= 0.0f ? std::fmod(t, 2.0f * kPi) - kPi : kPi - std::fmod(-t, 2.0f * kPi);
}

float FoxYawOf(const glm::vec3& v) {
    if (std::abs(v.x) + std::abs(v.z) < 1e-6f) {
        return 0.0f;
    }
    return std::atan2(v.x, v.z);
}

glm::vec3 RotateYaw(float fox_yaw, const glm::vec3& v) {
    const float c = std::cos(fox_yaw);
    const float s = std::sin(fox_yaw);
    return glm::vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

float PadAxis(float v) {
    const int raw = std::clamp(static_cast<int>(std::lround(127.5f + 127.5f * v)), 0, 255);
    const float f = static_cast<float>(2 * raw - 256) / 255.0f;
    const int s = std::clamp(static_cast<int>(f * 32767.0f), -32768, 32767);
    return static_cast<float>(s) * 3.051851e-05f;
}

float LookVelocity(float v, float input, float max, float dt) {
    const float target = input * max;
    const float accel = dt * max / (kLookAccelFrames * kLookFrame);
    const float decel = dt * max / (kLookDecelFrames * kLookFrame);
    if (input > 0.0f) {
        if (v < 0.0f) {
            return std::min(v + decel, 0.0f);
        }
        return v <= target ? std::min(v + input * accel, target) : std::max(v - decel, target);
    }
    if (input < 0.0f) {
        if (v > 0.0f) {
            return std::max(v - decel, 0.0f);
        }
        return target <= v ? std::max(v + input * accel, target) : std::min(v + decel, target);
    }
    return v >= 0.0f ? std::max(v - decel, 0.0f) : std::min(v + decel, 0.0f);
}

}

void PadLocks::Add(char set, const std::string& key, uint32_t mask) {
    Set(set)[key] = mask;
}

void PadLocks::Remove(char set, const std::string& key) {
    Set(set).erase(key);
}

uint32_t PadLocks::Mask(char set) const {
    const auto& slots = set == 'A' ? a_ : set == 'B' ? b_ : c_;
    uint32_t mask = 0;
    for (const auto& [key, value] : slots) {
        mask |= value;
    }
    return mask;
}

std::string PadLocks::Describe() const {
    std::string text;
    for (const auto* slots : {&a_, &b_, &c_}) {
        for (const auto& [key, value] : *slots) {
            text += key + " ";
        }
    }
    return text;
}

void Player::Spawn(const glm::mat4& world) {
    controller.position = glm::vec3(world[3]);
    controller.Reset();
    const glm::vec3 forward = glm::normalize(glm::vec3(world[2]));
    body_yaw_ = body_yaw_target_ = FoxYawOf(forward);
    yaw = target_yaw = kPi;
    pitch = target_pitch = 0.0f;
    look_velocity_ = glm::vec2(0.0f);
    zoom = 1.0f;
    handy_light.enable = true;
    spawned = true;
    handoff_weight_ = 0.0f;
    ResetLocomotion();
    UpdateEye(0.0f);
    PublishNow();
    LogInfo("player: spawn at ({:.2f} {:.2f} {:.2f}) fox yaw {:.1f}", controller.position.x, controller.position.y, controller.position.z,
            glm::degrees(body_yaw_));
}

void Player::Warp(const glm::vec3& position, float fox_yaw) {
    controller.position = position;
    controller.Reset();
    body_yaw_ = body_yaw_target_ = Wrap(fox_yaw);
    yaw = target_yaw = fox_yaw + kPi;
    look_velocity_ = glm::vec2(0.0f);
    handoff_weight_ = 0.0f;
    ResetLocomotion();
    UpdateEye(0.0f);
    PublishNow();
}

void Player::ResetLocomotion() {
    standing_ = published_read_.standing = published_write_.standing = true;
    idle_timer_ = 0.0f;
    stick_heading_ = body_yaw_;
    motion_ = MotionFrame{1.0f, body_yaw_, false};
    frame_time_ = 1.0;
    frame_offset_ = drawn_offset_ = glm::vec3(0.0f);
    frame_dt_ = 0.0f;
    frame_gravity_ = false;
    body_.Reset(kClipStand);
    draw_sync_ = true;
}

void Player::HandCameraBack(const glm::vec3& position, float yaw_value, float pitch_value) {
    yaw = target_yaw = yaw_value;
    pitch = target_pitch = ClampPitch(pitch_value);
    look_velocity_ = glm::vec2(0.0f);
    handoff_local_ = WorldToBody(position);
    handoff_weight_ = 1.0f;
    eye_ = position;
}

void Player::FollowCamera(float yaw_value, float pitch_value) {
    yaw = target_yaw = yaw_value;
    pitch = target_pitch = ClampPitch(pitch_value);
    look_velocity_ = glm::vec2(0.0f);
}

glm::vec3 Player::LookDirection() const {
    return glm::normalize(glm::vec3(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)));
}

glm::vec3 Player::CameraForward() const {
    return glm::vec3(-std::sin(yaw), 0.0f, -std::cos(yaw));
}

float Player::CameraFoxYaw() const {
    return FoxYawOf(CameraForward());
}

glm::vec3 Player::BodyToWorld(const glm::vec3& local) const {
    return controller.BodyPosition() + RotateYaw(body_yaw_, local);
}

glm::mat4 Player::BodyTransform() const {
    return glm::rotate(glm::translate(glm::mat4(1.0f), controller.BodyPosition()), body_yaw_, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Player::DrawnBodyTransform() const {
    return glm::translate(glm::mat4(1.0f), drawn_offset_) * BodyTransform();
}

glm::vec3 Player::WorldToBody(const glm::vec3& world) const {
    return RotateYaw(-body_yaw_, world - controller.BodyPosition());
}

void Player::SetDisableLeftStick(bool disable) {
    if (disable) {
        locks.Add('B', "SetDisableLeftStick", 1);
        left_lock_timer_ = 0.0f;
    } else {
        locks.Remove('B', "SetDisableLeftStick");
    }
}

float Player::SpeedRate(float heading_delta, float magnitude, bool blur) const {
    if (magnitude < 0.1f) {
        return 1.0f;
    }
    const float d = heading_delta;
    float mx = 0.0f;
    float mn = 0.0f;
    const SpeedRange& side = d <= 0.0f ? params.right : params.left;
    const float a = std::abs(d) / kPi;
    if (a < 0.5f) {
        const float s = 2.0f * a;
        mx = params.front.max + s * (side.max - params.front.max);
        mn = params.front.min + s * (side.min - params.front.min);
    } else {
        const float s = 2.0f * (1.0f - a);
        mx = params.back.max + s * (side.max - params.back.max);
        mn = params.back.min + s * (side.min - params.back.min);
    }
    if (blur) {
        mx *= 3.5f;
    }
    const float t = std::clamp((magnitude - 0.1f) / (0.95f - 0.1f), 0.0f, 1.0f);
    return mn + t * (mx - mn);
}

void Player::UpdateLocomotion(const InputState& input) {
    const bool left_locked = LeftStickLocked();
    glm::vec2 stick = left_locked ? glm::vec2(0.0f) : input.left_stick;
    if (input.left_stick_from_pad) {
        stick = glm::vec2(PadAxis(stick.x), -PadAxis(-stick.y));
    }
    float m = glm::length(stick);
    if (m > 1.0f) {
        stick /= m;
        m = 1.0f;
    }
    if (m < 0.1f) {
        m = 0.0f;
    }
    stick_magnitude_ = m;
    const glm::vec3 forward = CameraForward();
    const glm::vec3 right(-forward.z, 0.0f, forward.x);
    if (m > 0.0f) {
        const glm::vec3 world = glm::normalize(forward * stick.y + right * stick.x);
        stick_heading_ = FoxYawOf(world);
    }
}

void Player::LocomotionFrame(const PlayerFrameContext& context) {
    const float m = stick_magnitude_;
    if (LeftStickLocked()) {
        idle_timer_ = 0.0f;
        standing_ = true;
    } else if (m >= 0.1f) {
        idle_timer_ = 0.0f;
        standing_ = false;
    } else {
        idle_timer_ += kOriginalFrame;
        if (idle_timer_ >= kStandDebounce) {
            standing_ = true;
        }
    }
    const float delta = Wrap(stick_heading_ - body_yaw_);
    const int direction = WalkDirection(delta);
    motion_.rate = standing_ ? 1.0f : SpeedRate(delta, m, context.full_screen_blur);
    motion_.heading = stick_heading_;
    motion_.steered = m >= 0.1f && !LeftStickLocked();
    const PlayerBody::Kind kind = body_.ClipKind();
    if (kind == PlayerBody::Kind::Start) {
        body_.Play(PlayerBody::Kind::Node, body_.Clip());
    } else if (kind == PlayerBody::Kind::Stop) {
        body_.Play(PlayerBody::Kind::Node, kClipStand);
    }
    const bool stand_node = body_.ClipKind() == PlayerBody::Kind::Node && body_.Clip() == kClipStand;
    if (standing_) {
        if (!stand_node && body_.ClipKind() != PlayerBody::Kind::Stop) {
            body_.Play(PlayerBody::Kind::Stop, body_.Clip());
        }
    } else if (stand_node || body_.ClipKind() == PlayerBody::Kind::Stop) {
        body_.Play(PlayerBody::Kind::Start, direction);
    } else if (body_.ClipKind() == PlayerBody::Kind::Node && body_.Clip() != direction) {
        body_.Play(PlayerBody::Kind::Node, direction);
    }
}

void Player::UpdateLook(float dt, const InputState& input, const PlayerFrameContext& context) {
    if (context.peephole_theater && !peephole_look_active_) {
        peephole_yaw_ = yaw;
        look_velocity_ = glm::vec2(0.0f);
    }
    peephole_look_active_ = context.peephole_theater;
    const uint32_t lock_b = locks.Mask('B');
    if (input.vr_look && !(lock_b & 2) && !context.peephole_theater) {
        if (vr_looked_) {
            vr_turn_ = Wrap(vr_turn_ + Wrap(yaw - vr_last_yaw_));
        }
        target_yaw = yaw = Wrap(input.vr_look_angles.x + vr_turn_);
        vr_last_yaw_ = yaw;
        vr_looked_ = true;
        target_pitch = pitch = ClampPitch(vr_script_pitch_ ? *vr_script_pitch_ : input.vr_look_angles.y);
        look_velocity_ = glm::vec2(0.0f);
        pc_turns_ = {};
        light_stick_ = light_stick_override_ ? *light_stick_override_ : glm::vec2(0.0f);
        return;
    }
    const float sx = context.invert_x ? -1.0f : 1.0f;
    const float sy = context.invert_y ? -1.0f : 1.0f;
    float x = (lock_b & 2) ? 0.0f : PadAxis(input.right_stick.x);
    float y = (lock_b & 2) ? 0.0f : PadAxis(input.right_stick.y);
    glm::vec2 light(x * sx, y * sy);
    if (!(lock_b & 1)) {
        light.x += 0.5f * PadAxis(input.left_stick.x);
    }
    const uint32_t light_held = HeldButtons();
    if (input.from_gamepad) {
        light.y += ((light_held & kPadLookDown) ? 0.25f : 0.0f) - ((light_held & kPadLookUp) ? 0.25f : 0.0f);
        light.x += ((light_held & kPadLookRight) ? 0.25f : 0.0f) - ((light_held & kPadLookLeft) ? 0.25f : 0.0f);
    }
    const float ax = std::abs(x);
    const float ay = std::abs(y);
    x = (ax >= kLookDeadZone && ay * kLookCrossRatio <= ax) ? x : 0.0f;
    y = (ay >= kLookDeadZone && std::abs(x) * kLookCrossRatio <= ay) ? y : 0.0f;
    const float k = std::pow(std::clamp(std::sqrt(x * x + y * y), 0.0f, 1.0f), kLookExponent);
    x *= k * sx;
    y *= k * sy;
    const float focal = params.focal_length * zoom;
    const float scale = focal > 0.0f ? kLookFocal / focal : 1.0f;
    const float step = std::min(dt, kLookFrame);
    const float samples = dt / kLookFrame;
    const int count = std::max(dt > 0.0f ? 1 : 0, static_cast<int>(std::floor(samples)) + (samples - std::floor(samples) > 0.9f ? 1 : 0));
    for (int i = 0; i < count; ++i) {
        look_velocity_.y = LookVelocity(look_velocity_.y, -x, glm::radians(params.rot_vel_max_y) * scale, step);
        look_velocity_.x = LookVelocity(look_velocity_.x, y, glm::radians(params.rot_vel_max_x) * scale, step);
        target_yaw += look_velocity_.y * step;
        const float next_pitch = target_pitch - look_velocity_.x * step;
        target_pitch = ClampPitch(next_pitch);
        if (target_pitch != next_pitch) {
            look_velocity_.x = 0.0f;
        }
    }
    const float pc_yaw_from = target_yaw;
    const float pc_pitch_from = target_pitch;
    if (!input.from_gamepad && !(lock_b & 2)) {
        const uint32_t held = HeldButtons();
        float kx = 0.0f;
        float ky = 0.0f;
        if (held & kPadLookUp) ky -= 0.25f * sy;
        if (held & kPadLookDown) ky += 0.25f * sy;
        if (held & kPadLookRight) kx += 0.25f * sx;
        if (held & kPadLookLeft) kx -= 0.25f * sx;
        target_yaw -= kx * glm::radians(params.rot_vel_max_y) * dt;
        target_pitch = ClampPitch(target_pitch - ky * glm::radians(params.rot_vel_max_x) * dt);
    }
    if (!(lock_b & 2)) {
        const glm::vec2 mouse = input.mouse_look * glm::vec2(sx, sy);
        target_yaw -= mouse.x;
        target_pitch = ClampPitch(target_pitch - mouse.y);
        yaw -= mouse.x;
        pitch = ClampPitch(pitch - mouse.y);
    }
    if (dt > 0.0f) {
        pc_turns_[pc_turn_next_] = glm::vec4(Wrap(pc_yaw_from - target_yaw), target_pitch - pc_pitch_from, dt, 0.0f);
        pc_turn_next_ = (pc_turn_next_ + 1) % pc_turns_.size();
    }
    glm::vec3 pc_sum(0.0f);
    for (const glm::vec4& t : pc_turns_) {
        pc_sum += glm::vec3(t);
    }
    if (pc_sum.z > 0.0f) {
        const glm::vec2 rate = glm::vec2(pc_sum) / pc_sum.z;
        const glm::vec2 max(glm::radians(params.rot_vel_max_y) * scale, glm::radians(params.rot_vel_max_x) * scale);
        const glm::vec2 u(rate.x / std::max(max.x, 1.0e-4f), -rate.y / std::max(max.y, 1.0e-4f));
        const float m = glm::length(u);
        if (m > 0.00083f) {
            light += u * (std::min(std::cbrt(m), 1.0f) / m);
        }
    }
    light_stick_ = light_stick_override_ ? *light_stick_override_ : glm::clamp(light, glm::vec2(-1.0f), glm::vec2(1.0f));
    if (peephole_look_active_) {
        float limited_yaw = target_yaw;
        float limited_pitch = target_pitch;
        ClampPeepholeLook(limited_yaw, limited_pitch);
        if (limited_yaw != target_yaw) look_velocity_.y = 0.0f;
        if (limited_pitch != target_pitch) look_velocity_.x = 0.0f;
        target_yaw = limited_yaw;
        target_pitch = limited_pitch;
        ClampPeepholeLook(yaw, pitch);
    }
    const float half_life = params.rot_interp_half_life;
    const float keep = half_life >= 0.001f ? std::pow(0.5f, dt / half_life) : 0.0f;
    yaw = target_yaw + (yaw - target_yaw) * keep;
    pitch = target_pitch + (pitch - target_pitch) * keep;
}

void Player::ClampPeepholeLook(float& yaw_value, float& pitch_value) const {
    if (!peephole_look_active_) {
        return;
    }
    constexpr float limit = 0.2617994f;
    const float offset = Wrap(yaw_value - peephole_yaw_);
    if (std::abs(offset) > limit) {
        yaw_value = peephole_yaw_ + std::clamp(offset, -limit, limit);
    }
    pitch_value = std::clamp(pitch_value, -limit, limit);
}

void Player::UpdateZoom(float dt, const PlayerFrameContext& context) {
    float rate_k = 0.0f;
    if (!context.nazo_action_armed && !context.big_zoom) {
        rate_k = zoom_target_ + 0.5f;
    } else {
        zoom_target_ = 2.0f;
        rate_k = 2.5f;
    }
    const float speed = std::max(1.5f, rate_k);
    const bool want = published_read_.standing && (HeldButtons() & kPadZoom);
    zooming = want;
    if (want) {
        zoom_active_ = true;
    }
    bool zoom_out = false;
    if (zoom == zoom_target_ && !published_read_.standing) {
        zooming = false;
        zoom_active_ = true;
        zoom_velocity_ = 1.0f;
        zoom_out = true;
    } else if (!zoom_active_) {
    } else if (want) {
        zoom_velocity_ = std::max(0.5f, zoom_velocity_ - dt);
        zoom += dt * speed * zoom_velocity_;
        if (zoom >= zoom_target_) {
            const float t2 = zoom - 2.0f * dt * speed * zoom_velocity_;
            zoom = zoom_target_;
            if (zoom_target_ < t2) {
                zoom = t2;
            }
        }
    } else {
        zoom_out = true;
    }
    if (zoom_out) {
        zoom_velocity_ = std::min(1.0f, zoom_velocity_ + dt);
        zoom -= dt * speed * zoom_velocity_ * 3.0f;
        if (zoom <= 1.0f) {
            zoom = 1.0f;
            zoom_active_ = false;
        }
    }
    zoom_falling_ = zoom_out;
    zoom_target_ = 1.35f;
}

void Player::UpdateBody(float dt, const CollisionWorld& world) {
    const float follow = 1.0f - std::pow(1.0f - 1.0f / kBodyTurnDivisor, dt * 299.7003f * 0.2f);
    body_yaw_ = Wrap(body_yaw_ + Wrap(body_yaw_target_ - body_yaw_) * follow);
    const BodyAdvance advance = body_.Advance(dt * kMotionFps * motion_.rate, dt * kMotionFps);
    glm::vec3 offset = RotateYaw(body_yaw_, glm::vec3(advance.root.x, 0.0f, advance.root.y));
    if (motion_.steered) {
        offset = glm::vec3(std::sin(motion_.heading), 0.0f, std::cos(motion_.heading)) * advance.distance;
    }
    static const bool per_tick = [] {
        const char* v = std::getenv("PT_CONTROLLER_TICK");
        return v && v[0] == '1';
    }();
    if (per_tick) {
        controller.Move(world, offset, dt, frame_start_ ? kOriginalFrame : 0.0f);
        drawn_offset_ = glm::vec3(0.0f);
    } else {
        frame_offset_ += offset;
        frame_dt_ += dt;
        frame_gravity_ = frame_gravity_ || frame_start_;
        if (FrameEnds()) {
            const glm::vec3 before = controller.BodyPosition();
            controller.Move(world, frame_offset_, frame_dt_, frame_gravity_ ? kOriginalFrame : 0.0f);
            const glm::vec3 step = controller.BodyPosition() - before;
            drawn_offset_ = glm::dot(step, step) < 0.25f ? -0.5f * step : glm::vec3(0.0f);
            frame_offset_ = glm::vec3(0.0f);
            frame_dt_ = 0.0f;
            frame_gravity_ = false;
        } else {
            drawn_offset_ = glm::vec3(0.0f);
        }
    }
    frame_start_ = false;
    if (stick_magnitude_ > 0.0f && !LeftStickLocked()) {
        foot_steps += advance.footsteps;
    }
    sounding_steps += advance.footsteps;
    anim_sounds.insert(anim_sounds.end(), advance.sounds.begin(), advance.sounds.end());
}

void Player::UpdateEye(float dt) {
    const glm::vec3 head_x = body_.HeadRotation() * glm::vec3(1.0f, 0.0f, 0.0f);
    roll_ = kHeadRollShare * camera_roll * std::atan2(head_x.y, std::sqrt(head_x.x * head_x.x + head_x.z * head_x.z));
    glm::vec3 eye = BodyToWorld(body_.Head()) + RotateYaw(Wrap(target_yaw + kPi), kHeadEyeOffset);
    if (handoff_weight_ > 0.0f) {
        handoff_weight_ -= dt * stick_magnitude_;
        if (handoff_weight_ > 0.0f) {
            const glm::vec3 local = WorldToBody(eye);
            eye = BodyToWorld(handoff_local_ + (local - handoff_local_) * (1.0f - handoff_weight_));
        }
    }
    eye_ = eye;
}

void Player::Update(float tick_dt, const InputState& input, const CollisionWorld& world, const PlayerFrameContext& context) {
    const float dt = tick_dt * kTickToOriginal;
    held_ = input.held;
    pressed_ = input.pressed;
    if (locks.Mask('B') & 1) {
        left_lock_timer_ += dt;
        if (left_lock_timer_ > 300.0f) {
            locks.Remove('B', "SetDisableLeftStick");
        }
    }
    body_yaw_target_ = CameraFoxYaw();
    UpdateLook(dt, input, context);
    UpdateLocomotion(input);
    frame_step_ = static_cast<double>(dt) / kOriginalFrame;
    frame_time_ += frame_step_;
    frame_pressed_ = (frame_time_ >= 1.0 - 1e-4 ? 0u : frame_pressed_) | input.pressed;
    if (frame_time_ >= 1.0 - 1e-4) {
        frame_time_ = std::min(frame_time_ - 1.0, 1.0);
        frame_time_ = frame_time_ < 1e-4 ? 0.0 : frame_time_;
        frame_start_ = true;
        LocomotionFrame(context);
    }
    if (!input.left_stick_from_pad) {
        motion_.heading = stick_heading_;
    }
    UpdateZoom(dt, context);
    if (PlayerLocomotionData().loaded) {
        UpdateBody(dt, world);
    }
    UpdateEye(dt);
    if (draw_pose_ && PlayerLocomotionData().loaded) {
        UpdateDrawPose(dt, world);
    }
}

void Player::UpdateDrawPose(float dt, const CollisionWorld& world) {
    constexpr float kStepAngle = 0.4363323f;
    constexpr float kFastStepAngle = 0.7853982f;
    constexpr float kMaxTwist = 1.3089969f;
    constexpr float kSettleAngle = 0.1047198f;
    constexpr float kSettleDistance = 0.04f;
    constexpr float kSettleSeconds = 0.35f;
    constexpr float kStepDistance = 0.12f;
    constexpr float kStopStepDistance = 0.4f;
    constexpr float kStepSeconds = 0.28f;
    constexpr float kFastStepSeconds = 0.2f;
    constexpr float kFollowSeconds = 0.2f;
    constexpr float kReleaseSeconds = 0.15f;
    constexpr float kStepLift = 0.07f;
    constexpr float kPlantHeight = 0.135f;
    constexpr float kOffsetReturn = 0.3f;
    constexpr float kBodyRadius = 0.3f;
    if (draw_sync_) {
        drawn_body_ = body_;
        draw_stopping_ = false;
        follow_kind_ = body_.ClipKind();
        follow_clip_ = body_.Clip();
        draw_offset_ = glm::vec3(0.0f);
        last_body_yaw_ = body_yaw_;
        feet_[0] = feet_[1] = FootState{};
        turn_still_ = 0.0f;
        plant_ = PlayerBody::FootPlant{};
        draw_sync_ = false;
    }
    const glm::vec3 offset_before = draw_offset_;
    const PlayerBody::Kind kind = body_.ClipKind();
    const int clip = body_.Clip();
    const bool changed = kind != follow_kind_ || clip != follow_clip_;
    follow_kind_ = kind;
    follow_clip_ = clip;
    const bool walking = kind == PlayerBody::Kind::Start || (kind == PlayerBody::Kind::Node && clip != kClipStand);
    if (draw_stopping_) {
        if (walking) {
            drawn_body_.Play(kind, clip);
            draw_stopping_ = false;
        }
    } else if (changed) {
        drawn_body_.Play(kind, clip);
        if (kind == PlayerBody::Kind::Stop) {
            draw_stopping_ = true;
            stop_clip_ = clip;
        }
    }
    const float before = drawn_body_.Frame();
    drawn_body_.Advance(dt * kMotionFps * (draw_stopping_ ? 1.0f : motion_.rate), dt * kMotionFps, true);
    glm::vec3 offset = draw_offset_;
    if (draw_stopping_) {
        const BodyClip& stop = PlayerLocomotionData().stops[static_cast<size_t>(stop_clip_)];
        if (kind != PlayerBody::Kind::Stop) {
            const glm::vec2 step = stop.RootAt(drawn_body_.Frame()) - stop.RootAt(before);
            offset += RotateYaw(body_yaw_, glm::vec3(step.x, 0.0f, step.y));
        }
        if (drawn_body_.Frame() >= stop.frames - 1.0e-3f) {
            drawn_body_.Play(PlayerBody::Kind::Node, kClipStand);
            draw_stopping_ = false;
        }
    } else if (walking) {
        const float length = glm::length(offset);
        const float back = kOffsetReturn * std::max(dt, 0.0f);
        offset = length > back ? offset * ((length - back) / length) : glm::vec3(0.0f);
    }
    if (const float length = glm::length(offset); length > 1.0e-4f) {
        const glm::vec3 direction = offset / length;
        RayHit hit;
        const glm::vec3 centre = controller.BodyPosition() + glm::vec3(0.0f, controller.shape.center_height, 0.0f);
        if (world.Raycast(centre, direction, length + kBodyRadius, hit)) {
            offset = direction * std::max(0.0f, std::min(length, hit.distance - kBodyRadius));
        }
    }
    draw_offset_ = offset;

    PlayerBody::FootPlant plant;
    glm::vec3 clip_model[2];
    const bool have_feet = drawn_body_.FootTargets(clip_model);
    const glm::mat4 pose = PoseTransform();
    const glm::mat4 to_model = glm::inverse(pose);
    const float turned = Wrap(body_yaw_ - last_body_yaw_);
    last_body_yaw_ = body_yaw_;
    const bool planting = have_feet && !walking;
    turn_still_ = planting && std::abs(turned) < 1.0e-4f && glm::length(offset - offset_before) < 1.0e-5f ? turn_still_ + dt : 0.0f;
    float off[2] = {0.0f, 0.0f};
    for (int side = 0; side < 2; ++side) {
        FootState& foot = feet_[side];
        const glm::vec3 clip_world = have_feet ? glm::vec3(pose * glm::vec4(clip_model[side], 1.0f)) : glm::vec3(0.0f);
        const bool down = clip_model[side].y < kPlantHeight;
        if (!planting) {
            if (foot.state == Foot::Locked && have_feet) {
                foot.state = Foot::Stepping;
                foot.from = foot.floor;
                foot.from_twist = Wrap(foot.yaw - body_yaw_);
                foot.time = 0.0f;
                foot.length = kReleaseSeconds;
                foot.arc = false;
            } else if (foot.state != Foot::Stepping) {
                foot.state = Foot::Free;
            }
        } else if (foot.state == Foot::Free && down) {
            foot.state = Foot::Locked;
            foot.floor = clip_world;
            foot.yaw = body_yaw_;
        }
        if (foot.state == Foot::Locked) {
            const glm::vec2 apart(foot.floor.x - clip_world.x, foot.floor.z - clip_world.z);
            const float twist = Wrap(foot.yaw - body_yaw_);
            off[side] = std::max(glm::length(apart) / (draw_stopping_ ? kStopStepDistance : kStepDistance), std::abs(twist) / kStepAngle);
            if (turn_still_ >= kSettleSeconds && (glm::length(apart) > kSettleDistance || std::abs(twist) > kSettleAngle)) {
                off[side] = std::max(off[side], 0.999f);
            }
            if (!down) {
                off[side] = 2.0f;
            }
        }
    }
    const int worse = off[0] >= off[1] ? 0 : 1;
    for (int pick : {worse, 1 - worse}) {
        FootState& foot = feet_[pick];
        const bool other_steps = feet_[1 - pick].state == Foot::Stepping;
        const bool lifted = off[pick] >= 2.0f;
        if (foot.state == Foot::Locked && (lifted || (off[pick] >= 0.999f && !other_steps))) {
            const float twist = Wrap(foot.yaw - body_yaw_);
            foot.state = Foot::Stepping;
            foot.from = foot.floor;
            foot.from_twist = twist;
            foot.time = 0.0f;
            foot.length = lifted ? kFollowSeconds : std::abs(twist) > kFastStepAngle ? kFastStepSeconds : kStepSeconds;
            foot.arc = !lifted;
        }
    }
    for (int side = 0; side < 2; ++side) {
        FootState& foot = feet_[side];
        if (!have_feet) {
            break;
        }
        const glm::vec3 clip_world(pose * glm::vec4(clip_model[side], 1.0f));
        if (foot.state == Foot::Locked) {
            float twist = Wrap(foot.yaw - body_yaw_);
            if (std::abs(twist) > kMaxTwist) {
                foot.yaw = body_yaw_ + std::copysign(kMaxTwist, twist);
                twist = std::copysign(kMaxTwist, twist);
            }
            plant.active[side] = true;
            plant.target[side] = glm::vec3(to_model * glm::vec4(foot.floor.x, clip_world.y, foot.floor.z, 1.0f));
            plant.yaw[side] = twist;
        } else if (foot.state == Foot::Stepping) {
            foot.time += dt;
            const float s = std::clamp(foot.time / std::max(foot.length, 1.0e-3f), 0.0f, 1.0f);
            const float eased = s * s * (3.0f - 2.0f * s);
            glm::vec3 at = glm::mix(foot.from, clip_world, eased);
            at.y = clip_world.y + (foot.arc ? kStepLift * std::sin(3.14159265f * s) : 0.0f);
            plant.active[side] = true;
            plant.target[side] = glm::vec3(to_model * glm::vec4(at, 1.0f));
            plant.yaw[side] = foot.from_twist * (1.0f - eased);
            if (s >= 1.0f) {
                foot.state = Foot::Free;
                plant.active[side] = false;
            }
        }
    }
    plant_ = plant;
    drawn_body_.SetFootPlant(plant);
}

std::string Player::DescribeDrawPose() const {
    const auto state = [](const FootState& f) { return f.state == Foot::Locked ? "locked" : f.state == Foot::Stepping ? "stepping" : "free"; };
    return std::format("pose {} clip {} frame {:.1f}{} offset ({:.3f} {:.3f}) feet {} {:.1f} {} {:.1f}", static_cast<int>(drawn_body_.ClipKind()),
                       drawn_body_.Clip(), drawn_body_.Frame(), draw_stopping_ ? " stopping" : "", draw_offset_.x, draw_offset_.z,
                       state(feet_[0]), glm::degrees(plant_.yaw[0]), state(feet_[1]), glm::degrees(plant_.yaw[1]));
}

void Player::EndFrame() {
    if (published_swap_) {
        published_read_ = published_write_;
        published_swap_ = false;
    }
    if (FrameEnds()) {
        published_write_ = {controller.position, body_yaw_, standing_, pitch};
        published_swap_ = true;
    }
}

void Player::PublishNow() {
    published_read_ = published_write_ = {controller.position, body_yaw_, standing_, pitch};
    published_swap_ = false;
}

Camera Player::MakeCamera() const {
    Camera camera;
    camera.position = eye_;
    camera.yaw = yaw;
    camera.pitch = pitch;
    camera.roll = roll_;
    const float focal = params.focal_length * zoom;
    camera.fov_y = 2.0f * std::atan(kFilmHeightMm * 0.5f / focal);
    camera.near_plane = 0.05f + 0.09f * std::max(zoom - 1.0f, 0.0f);
    return camera;
}

}
