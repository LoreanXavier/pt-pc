#include "game/vr_play.h"

#include <algorithm>
#include <cmath>

#include "engine/core/log.h"
#include "engine/physics/collision_world.h"
#include "game/game.h"

namespace pt::game {

namespace {

constexpr float kHudDistance = 1.6f;
constexpr float kHudWidth = 1.4f;
constexpr float kHudDrop = 0.08f;
constexpr float kHudDeadZone = 0.349f;
constexpr float kScreenDistance = 2.5f;
constexpr float kScreenWidth = 2.8f;
constexpr float kHeadReach = 0.5f;
constexpr float kHeadRise = 0.4f;
constexpr float kHeadDrop = 0.8f;
constexpr float kWallMargin = 0.12f;
constexpr float kNearPlane = 0.05f;

}

void VrPlay::BeginLoop(bool& running) {
    if (host_.FrameOpen()) {
        host_.EndFrame({});
    }
    host_.PollEvents();
    if (host_.ExitRequested()) {
        LogInfo("vr: session ended by the runtime, quitting");
        running = false;
    }
    waited_ = host_.WaitFrame();
    if (!waited_) {
        return;
    }
    if (!host_.BeginFrame()) {
        waited_ = false;
        return;
    }
    host_.LocateViews();
    host_.SyncActions();
}

bool VrPlay::ScreenMode(Game& game) {
    glm::vec3 position;
    glm::quat rotation;
    float fov = 0.0f;
    const bool screen = game.Demos().CameraWorld(position, rotation, fov) || game.Nazo().IsPeepholeTheaterActive();
    if (screen != screen_) {
        LogInfo("vr: {}", screen ? "virtual screen (a camera the game frames)" : "stereo view");
    }
    screen_ = screen;
    if (!screen_) {
        screen_placed_ = false;
    }
    return screen_;
}

void VrPlay::ApplyControls(Game& game, InputState& state, bool menu_open, bool screen, float dt) {
    const xr::ControllerState& c = host_.Controllers();
    const xr::ViewPose& head = host_.Head();
    if (waited_ && host_.ViewsValid() && !centered_) {
        rig_.Recenter(head.position, head.orientation, game.GetCamera().yaw);
        centered_ = true;
        LogInfo("vr: centred on the head at ({:.3f} {:.3f} {:.3f}), world yaw {:.1f}", head.position.x, head.position.y, head.position.z,
                glm::degrees(rig_.BaseYaw()));
    }
    uint32_t raw = 0;
    if (c.active) {
        glm::vec2 move = c.move;
        if (glm::length(move) > 1.0f) move = glm::normalize(move);
        if (menu_open) {
            if (move.y > 0.6f) raw |= kRawUp;
            if (move.y < -0.6f) raw |= kRawDown;
            if (move.x > 0.6f) raw |= kRawRight;
            if (move.x < -0.6f) raw |= kRawLeft;
        } else if (glm::length(move) > glm::length(state.left_stick)) {
            state.left_stick = move;
            state.left_stick_from_pad = true;
        }
        if (c.interact) raw |= kRawCross;
        if (c.back) raw |= kRawCircle;
        if (c.menu) raw |= kRawStart;
        if (c.zoom) raw |= kRawR3;
        if (c.gouge) raw |= kRawSquare;
        if (c.triangle) raw |= kRawTriangle;
        if (!menu_open && screen) {
            const glm::vec2 look(c.turn.x, -c.turn.y);
            if (glm::length(look) > glm::length(state.right_stick)) state.right_stick = look;
        } else if (!menu_open && centered_) {
            const float x = c.turn.x;
            if (settings_.turn == 1) {
                if (std::abs(x) > 0.15f) rig_.Turn(-x * glm::radians(settings_.smooth_speed) * dt, head.position);
            } else if (snap_armed_ && std::abs(x) > 0.7f) {
                rig_.Turn(glm::radians(x > 0.0f ? -settings_.snap_degrees : settings_.snap_degrees), head.position);
                snap_armed_ = false;
            } else if (std::abs(x) < 0.3f) {
                snap_armed_ = true;
            }
        }
        if (raw != 0 || glm::length(c.move) > 0.2f || glm::length(c.turn) > 0.2f) {
            state.from_gamepad = true;
        }
        state.prompts = PromptStyle{PromptDevice::Xbox, {'A', 'B', 'X', 'Y'}};
    }
    const uint32_t pressed = raw & ~raw_previous_;
    raw_previous_ = raw;
    state.raw_held |= raw;
    state.raw_pressed |= pressed;
    state.held |= PlayerButtonsFromRaw(raw);
    state.pressed |= PlayerButtonsFromRaw(pressed);
    if (pressed & kRawSquare) {
        state.gouge_pressed = true;
        state.pressed |= kPadGouge;
    }
    if (pressed & kRawStart) state.pause = true;
    if (c.settings && !settings_previous_) state.pc_settings = true;
    settings_previous_ = c.settings;
    if (pressed) state.any_button = true;
    Player& player = game.GetPlayer();
    const bool look_free = !(player.locks.Mask('B') & 2) && !game.Demos().ControlsPlayer();
    if (!screen && centered_ && host_.ViewsValid() && look_free) {
        if (const float turn = player.TakeVrTurn(); std::abs(turn) > 1.0e-5f) {
            rig_.Turn(turn, head.position);
            if (std::abs(turn) > 0.0175f && ++game_turns_ <= 20) {
                LogInfo("vr: game turned the player {:.1f} degrees, view follows", glm::degrees(turn));
            }
        }
        const xr::Angles a = xr::AnglesOf(rig_.ToWorld(head.orientation));
        state.vr_look = true;
        state.vr_look_angles = glm::vec2(a.yaw, a.pitch);
    }
    std::optional<std::pair<glm::vec3, glm::vec3>> light;
    const int hand = std::clamp(settings_.flashlight_hand, 0, 1);
    if (settings_.flashlight == 1 && !screen && centered_ && eye_height_ >= 0.0f && c.aim[hand].valid) {
        const glm::vec3 position = last_anchor_ + rig_.Offset(c.aim[hand].position) + last_correction_;
        const glm::vec3 direction = glm::normalize(rig_.ToWorld(c.aim[hand].orientation) * glm::vec3(0.0f, 0.0f, -1.0f));
        light = std::make_pair(position, direction);
    }
    game.SetHandyPoseOverride(light);
}

bool VrPlay::PrepareStereo(Game& game, const Camera& logic, float dt, bool menu_open, Stereo& out) {
    if (!waited_ || !host_.ShouldRender() || !host_.ViewsValid() || !centered_) {
        return false;
    }
    const glm::vec4 tangents[2] = {host_.Eye(0).tangents, host_.Eye(1).tangents};
    if (render_size_.x == 0) {
        const VkExtent2D swapchain = host_.EyeExtent();
        render_size_ = xr::StereoRenderSize(tangents, glm::uvec2(swapchain.width, swapchain.height));
        LogInfo("vr: eyes drawn at {}x{} for {}x{} images (fields L {:.1f} R {:.1f} U {:.1f} D {:.1f} degrees, left eye)", render_size_.x,
                render_size_.y, swapchain.width, swapchain.height, glm::degrees(host_.Eye(0).angles.x), glm::degrees(host_.Eye(0).angles.y),
                glm::degrees(host_.Eye(0).angles.z), glm::degrees(host_.Eye(0).angles.w));
    }
    for (int i = 0; i < 2; ++i) {
        frusta_[i] = xr::FrustumFor(tangents[i], render_size_, tangents);
    }
    const Player& player = game.GetPlayer();
    const glm::vec3 feet = player.Feet();
    const glm::vec3 eye = player.Eye();
    const float height = eye.y - feet.y;
    eye_height_ = eye_height_ < 0.0f ? height : eye_height_ + (height - eye_height_) * (1.0f - std::exp(-std::max(dt, 0.0f) / 0.6f));
    const glm::vec3 anchor = logic.position + (feet - eye) + glm::vec3(0.0f, eye_height_, 0.0f);
    const xr::ViewPose& head = host_.Head();
    const glm::vec3 offset = rig_.Offset(head.position);
    glm::vec3 kept = offset;
    const float reach = glm::length(glm::vec2(kept.x, kept.z));
    if (reach > kHeadReach) {
        kept.x *= kHeadReach / reach;
        kept.z *= kHeadReach / reach;
    }
    kept.y = std::clamp(kept.y, -kHeadDrop, kHeadRise);
    if (const float length = glm::length(kept); length > 1.0e-3f) {
        const glm::vec3 direction = kept / length;
        RayHit hit;
        if (game.Collision().Raycast(anchor, direction, length + kWallMargin, hit)) {
            kept = direction * std::max(0.0f, hit.distance - kWallMargin);
        }
    }
    const glm::vec3 correction = kept - offset;
    last_anchor_ = anchor;
    last_correction_ = correction;
    out.head = xr::EyeCamera(anchor + kept, rig_.ToWorld(head.orientation), frusta_[0], kNearPlane);
    for (int i = 0; i < 2; ++i) {
        const xr::ViewPose& pose = host_.Eye(i);
        out.poses[i] = pose;
        out.eyes[i] = xr::EyeCamera(anchor + rig_.Offset(pose.position) + correction, rig_.ToWorld(pose.orientation), frusta_[i], kNearPlane);
    }
    out.render = {render_size_.x, render_size_.y};
    Place(head.position, xr::AnglesOf(head.orientation).yaw, menu_open, dt);
    auto target = [&](xr::Swapchain& sc, const glm::vec4& rect, XrTarget& t) {
        if (!host_.Acquire(sc)) return false;
        t.image = sc.images[sc.index];
        t.view = sc.views[sc.index];
        t.format = sc.format;
        t.extent = sc.extent;
        t.rect = rect;
        return true;
    };
    if (!target(host_.EyeSwapchain(0), frusta_[0].rect, out.targets[0]) || !target(host_.EyeSwapchain(1), frusta_[1].rect, out.targets[1]) ||
        !target(host_.HudSwapchain(), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), out.hud)) {
        for (xr::Swapchain* sc : {&host_.EyeSwapchain(0), &host_.EyeSwapchain(1), &host_.HudSwapchain()}) host_.Release(*sc);
        return false;
    }
    return true;
}

void VrPlay::FinishStereo(const Stereo& stereo) {
    host_.Release(host_.EyeSwapchain(0));
    host_.Release(host_.EyeSwapchain(1));
    host_.Release(host_.HudSwapchain());
    xr::FrameLayers layers;
    layers.projection = true;
    layers.eyes[0] = stereo.poses[0];
    layers.eyes[1] = stereo.poses[1];
    layers.hud = true;
    layers.hud_orientation = xr::YawRotation(hud_yaw_);
    layers.hud_position = hud_position_;
    layers.hud_size = glm::vec2(kHudWidth, kHudWidth * 9.0f / 16.0f);
    host_.EndFrame(layers);
    if (++frames_ == 1) {
        LogInfo("vr: first stereo frame submitted");
    }
}

bool VrPlay::PrepareScreen(XrTarget& out) {
    if (!waited_ || !host_.ShouldRender()) {
        return false;
    }
    const xr::ViewPose& head = host_.Head();
    if (!screen_placed_) {
        const float yaw = host_.ViewsValid() ? xr::AnglesOf(head.orientation).yaw : 0.0f;
        screen_orientation_ = xr::YawRotation(yaw);
        screen_position_ = head.position + screen_orientation_ * glm::vec3(0.0f, 0.0f, -kScreenDistance);
        screen_placed_ = true;
    }
    xr::Swapchain& sc = host_.ScreenSwapchain();
    if (!host_.Acquire(sc)) {
        return false;
    }
    out.image = sc.images[sc.index];
    out.view = sc.views[sc.index];
    out.format = sc.format;
    out.extent = sc.extent;
    out.rect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    return true;
}

void VrPlay::FinishScreen() {
    host_.Release(host_.ScreenSwapchain());
    xr::FrameLayers layers;
    layers.screen = true;
    layers.screen_orientation = screen_orientation_;
    layers.screen_position = screen_position_;
    layers.screen_size = glm::vec2(kScreenWidth, kScreenWidth * 9.0f / 16.0f);
    host_.EndFrame(layers);
}

void VrPlay::EndEmpty() {
    if (host_.FrameOpen()) {
        host_.EndFrame({});
    }
}

void VrPlay::Rumble(uint8_t large_motor, uint8_t small_motor) {
    const uint8_t strongest = std::max(large_motor, small_motor);
    if (strongest == 0) {
        return;
    }
    const float amplitude = static_cast<float>(strongest) / 255.0f;
    host_.Haptic(0, amplitude, 0.05f);
    host_.Haptic(1, amplitude, 0.05f);
}

void VrPlay::Place(const glm::vec3& head_local, float head_yaw_local, bool menu_open, float dt) {
    if (!hud_placed_ || (menu_open && !menu_was_open_)) {
        hud_yaw_ = head_yaw_local;
        hud_placed_ = true;
    } else {
        const float d = xr::WrapAngle(head_yaw_local - hud_yaw_);
        if (std::abs(d) > kHudDeadZone) {
            const float beyond = d - std::copysign(kHudDeadZone, d);
            hud_yaw_ = xr::WrapAngle(hud_yaw_ + beyond * std::min(1.0f, std::max(dt, 0.0f) * 3.0f));
        }
    }
    menu_was_open_ = menu_open;
    hud_position_ = head_local + xr::YawRotation(hud_yaw_) * glm::vec3(0.0f, -kHudDrop, -kHudDistance);
}

}
