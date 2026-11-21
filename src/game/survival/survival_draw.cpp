#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/assets/fmdl.h"
#include "engine/fs/vfs.h"
#include "engine/physics/collision_world.h"
#include "engine/render/camera.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_lighting.h"
#include "game/demo_system.h"
#include "game/game.h"
#include "game/player.h"
#include "game/run_stride.h"
#include "game/survival/survival.h"
#include "game/survival/survival_models.h"

namespace pt::game {

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint64_t kSourceBase = 0x5355525600000000ull;
constexpr std::array<glm::vec3, 5> kStreetLamps{{{4.28f, 7.1f, 99.75f}, {-8.88f, 7.1f, 109.71f}, {4.28f, 7.1f, 143.9f}, {-8.88f, 7.1f, 62.69f},
                                                 {4.28f, 7.1f, 53.7f}}};
static_assert(kStreetLamps.size() == kSurvivalLamps);
constexpr std::array<std::pair<glm::vec3, glm::vec3>, 8> kLevelCars{{{{-20.45f, 0.0f, 12.57f}, {1.0f, 0.0f, 0.04f}},
                                                                     {{-20.46f, 0.0f, 16.39f}, {1.0f, 0.0f, 0.05f}},
                                                                     {{-20.57f, 0.0f, 24.08f}, {-1.0f, 0.0f, -0.04f}},
                                                                     {{-31.01f, 0.0f, 25.28f}, {-1.0f, 0.0f, -0.01f}},
                                                                     {{-6.81f, 0.0f, 27.28f}, {0.05f, 0.0f, 1.0f}},
                                                                     {{-0.19f, 0.0f, 94.8f}, {-0.02f, 0.0f, 1.0f}},
                                                                     {{8.04f, 0.0f, 140.13f}, {-1.0f, 0.0f, 0.03f}},
                                                                     {{-14.7f, 0.0f, 209.56f}, {0.71f, 0.0f, -0.71f}}}};
constexpr std::array<size_t, kSurvivalLamps> kLampsOut{4, 2, 3, 1, 0};

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

glm::mat4 Frame(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f)) {
    const glm::vec3 z = glm::normalize(forward);
    glm::vec3 x = glm::cross(up, z);
    x = glm::length(x) > 1e-4f ? glm::normalize(x) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 y = glm::cross(z, x);
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(x, 0.0f);
    m[1] = glm::vec4(y, 0.0f);
    m[2] = glm::vec4(z, 0.0f);
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

glm::mat4 GripHand(const SurvivalGunModel& gun);
glm::mat4 TurnXyz(const glm::vec3& r);

glm::mat4 Rot(float angle, const glm::vec3& axis) {
    return glm::rotate(glm::mat4(1.0f), angle, axis);
}

constexpr PlayerBody::WristLimits kRightWrist{true, 0.8f, 0.7f, 0.75f, 0.25f};
constexpr PlayerBody::WristLimits kLeftWrist{true, 0.8f, 0.7f, 0.85f, 0.3f};
constexpr float kRightWristHeld = 0.15f;

struct Sights {
    float relief;
    glm::vec3 hip;
    float straight_wrist = 0.0f;
    glm::vec3 hip_turn{0.035f, 0.075f, -0.1f};
    float eye_over = 0.0f;
};
const Sights& SightsOf(SurvivalWeapon w) {
    static const Sights kSights[] = {
        {0.3f, {0.16f, -0.15f, 0.36f}},
        {0.47f, {0.105f, -0.115f, 0.38f}},
        {0.34f, {0.12f, -0.16f, 0.17f}, 1.0f, {0.035f, 0.03f, -0.1f}, 0.03f},
        {0.49f, {0.115f, -0.135f, 0.41f}, 0.15f, {-0.05f, 0.075f, -0.10f}},
        {0.36f, {0.12f, -0.16f, 0.17f}, 1.0f, {0.035f, 0.03f, -0.1f}},
        {0.34f, {0.12f, -0.16f, 0.17f}, 1.0f, {0.035f, 0.03f, -0.1f}},
    };
    return kSights[static_cast<size_t>(w)];
}

}

glm::mat4 SurvivalNight::FirstPersonWeapon(const Camera& camera) const {
    const glm::vec3 f = camera.Forward(), r = camera.Right(), u = camera.Up();
    glm::mat4 view(1.0f);
    view[0] = glm::vec4(-r, 0.0f);
    view[1] = glm::vec4(u, 0.0f);
    view[2] = glm::vec4(f, 0.0f);
    view[3] = glm::vec4(camera.position, 1.0f);
    const Sights& s = SightsOf(weapon_);
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    const float sprint = SprintWeight();
    const glm::vec3 rear = model.sight_rear + glm::vec3(0.0f, s.eye_over, 0.0f);
    const glm::vec3 line = glm::normalize(model.sight_front - rear);
    const float drop = std::atan2(-line.y, line.z);
    const glm::quat aimed_turn = glm::angleAxis(-drop, glm::vec3(1, 0, 0));
    const glm::vec3 aimed_at = glm::vec3(0.0f, 0.0f, s.relief) - aimed_turn * rear;
    const glm::quat hip_turn = glm::angleAxis(s.hip_turn.y, glm::vec3(0, 1, 0)) * glm::angleAxis(s.hip_turn.x, glm::vec3(1, 0, 0)) * glm::angleAxis(s.hip_turn.z, glm::vec3(0, 0, 1));
    const glm::vec3 hip_at(-s.hip.x, s.hip.y, s.hip.z);
    const float a = Smooth(aim_);
    glm::vec3 at = glm::mix(hip_at, aimed_at, a);
    const float hold = SurvivalLongGun(weapon_) ? a * (1.0f - Smooth(sights_w_)) : 0.0f;
    at += glm::vec3(-0.12f, -0.1f, -0.02f) * hold;
    glm::quat turn = glm::slerp(hip_turn, aimed_turn, a);
    turn = glm::angleAxis(0.025f * hold, glm::vec3(0, 1, 0)) * turn * glm::angleAxis(-0.28f * hold, glm::vec3(0, 0, 1));
    const float bob = bob_weight_ * (0.007f + 0.015f * sprint) * (1.0f - 0.8f * aim_);
    at += glm::vec3(-std::cos(bob_phase_) * bob * 1.25f, -std::abs(std::sin(bob_phase_)) * bob, 0.0f);
    at.y += std::sin(time_ * 1.7f) * 0.0012f * (1.0f - 0.6f * aim_);
    at += glm::vec3(sway_.x * 0.05f, sway_.y * 0.04f - std::abs(sway_.x) * 0.012f, 0.0f);
    turn = glm::angleAxis(-sway_.x * 0.9f, glm::vec3(0, 1, 0)) * glm::angleAxis(sway_.y * 0.7f, glm::vec3(1, 0, 0)) *
           glm::angleAxis(-sway_.x * 0.45f - side_lean_, glm::vec3(0, 0, 1)) * turn;
    at += glm::vec3(0.05f, -0.07f, -0.04f) * sprint;
    turn = glm::angleAxis(0.55f * sprint, glm::vec3(0, 1, 0)) * glm::angleAxis(0.45f * sprint, glm::vec3(1, 0, 0)) * turn;
    at.y -= 0.26f * Smooth(swap_);
    turn = glm::angleAxis(0.6f * Smooth(swap_), glm::vec3(1, 0, 0)) * turn;
    if (hud_.dead) at.y -= 0.5f * Smooth(dead_fall_);
    if (ReloadPose pose; ReloadNow(pose)) {
        at += pose.at;
        turn = turn * glm::quat_cast(glm::mat3(TurnXyz(pose.turn)));
    }
    if (weapon_ == SurvivalWeapon::Handgun && reload_timer_ > 0.0f && reload_from_empty_) {
        const float t = reload_length_ - reload_timer_;
        if (t >= 1.1f) turn = turn * glm::angleAxis(-0.12f * std::exp(-(t - 1.1f) * 14.0f), glm::vec3(1, 0, 0));
    }
    if (reload_timer_ > 0.0f && reload_length_ > 0.05f) {
        const ReloadStyle rstyle = ActiveReloadStyle();
        const float p = std::clamp(1.0f - reload_timer_ / reload_length_, 0.0f, 1.0f);
        if (rstyle == ReloadStyle::Tactical) {
            const float env = Smooth(p / 0.14f) * Smooth((1.0f - p) / 0.16f);
            const float snap = p > 0.76f && p < 0.96f ? std::sin(((p - 0.76f) / 0.20f) * kPi) : 0.0f;
            at += glm::vec3(-0.025f, 0.024f, 0.025f) * env + glm::vec3(0.0f, 0.015f, -0.025f) * snap;
            turn = turn * glm::angleAxis(-0.14f * env - 0.18f * snap, glm::vec3(1, 0, 0))
                        * glm::angleAxis(0.48f * env, glm::vec3(0, 0, 1));
        } else if (rstyle == ReloadStyle::Gunslinger) {
            const float entry = p < 0.24f ? std::sin((p / 0.24f) * kPi) : 0.0f;
            const float spin_u = Smooth(std::clamp((p - 0.64f) / 0.32f, 0.0f, 1.0f));
            const float lift = std::sin(spin_u * kPi);
            const float angle = spin_u * 2.0f * kPi;
            if (SurvivalLongGun(weapon_)) {
                at += glm::vec3(0.035f * lift, 0.052f * lift + 0.015f * entry, 0.115f * lift);
                turn = turn * glm::angleAxis(angle, glm::vec3(1, 0, 0))
                            * glm::angleAxis(0.16f * lift + 0.20f * entry, glm::vec3(0, 0, 1));
            } else {
                at += glm::vec3(-0.015f * lift, 0.048f * lift + 0.016f * entry, 0.045f * lift);
                turn = turn * glm::angleAxis(angle, glm::vec3(1, 0, 0))
                            * glm::angleAxis(0.22f * lift + 0.26f * entry, glm::vec3(0, 0, 1));
            }
        } else if (rstyle == ReloadStyle::Telekinetic) {
            const float hover = Smooth((p - 0.06f) / 0.14f) * Smooth((0.94f - p) / 0.14f);
            const float bob = std::sin(p * 4.0f * kPi) * 0.012f;
            const float tremor = std::sin(time_ * 42.0f) * 0.002f;
            if (SurvivalLongGun(weapon_)) {
                at += glm::vec3(0.055f + tremor, 0.050f + bob, 0.085f) * hover;
                turn = turn * glm::angleAxis(0.24f * std::sin(p * kPi) * hover, glm::vec3(0, 1, 0))
                            * glm::angleAxis(-0.10f * std::cos(p * 2.0f * kPi) * hover, glm::vec3(1, 0, 0))
                            * glm::angleAxis(0.18f * std::sin(p * 2.0f * kPi) * hover, glm::vec3(0, 0, 1));
            } else {
                at += glm::vec3(-0.018f + tremor, 0.062f + bob, 0.105f) * hover;
                turn = turn * glm::angleAxis(0.42f * std::sin(p * kPi) * hover, glm::vec3(0, 1, 0))
                            * glm::angleAxis(-0.16f * std::cos(p * 2.0f * kPi) * hover, glm::vec3(1, 0, 0))
                            * glm::angleAxis(0.32f * std::sin(p * 2.0f * kPi) * hover, glm::vec3(0, 0, 1));
            }
        }
    }
    {
        const float cycle = weapon_ == SurvivalWeapon::Lever ? Smooth(gun_.lever) : weapon_ == SurvivalWeapon::Shotgun && gun_.pumping ? 0.5f * Smooth(gun_.slide) : 0.0f;
        if (cycle > 0.0f) {
            const float k = cycle * (1.0f - 0.55f * Smooth(sights_w_));
            at += glm::vec3(0.03f, 0.07f, 0.02f) * k;
            turn = turn * glm::angleAxis(0.05f * k, glm::vec3(1, 0, 0)) * glm::angleAxis(0.3f * k, glm::vec3(0, 0, 1));
        }
    }
    if (const float jab = ShoveOut(); jab > 0.0f) {
        const bool long_gun = SurvivalLongGun(weapon_);
        at += (long_gun ? glm::vec3(0.08f, 0.03f, 0.04f) : glm::vec3(0.06f, 0.02f, 0.08f)) * jab;
        turn = glm::angleAxis((long_gun ? 0.5f : 0.4f) * jab, glm::vec3(0, 1, 0)) * turn * glm::angleAxis(-(long_gun ? 0.45f : 0.35f) * jab, glm::vec3(0, 0, 1));
    }
    if (const float away = SwingAway(); away > 0.0f) {
        at += glm::vec3(0.1f, -0.36f, -0.06f) * away;
        turn = glm::angleAxis(0.7f * away, glm::vec3(1, 0, 0)) * turn;
    }
    if (const float off = ThrowAway(); off > 0.0f) {
        at += glm::vec3(-0.02f, -0.03f, 0.0f) * off;
        turn = turn * glm::angleAxis(0.12f * off, glm::vec3(0, 0, 1));
    }
    if (const float fc = FlashClickWeight(); fc > 0.0f && weapon_ != SurvivalWeapon::Flashlight) {
        const bool long_gun = SurvivalLongGun(weapon_);
        const float press = FlashClickPress();
        at += (long_gun ? glm::vec3(-0.012f, -0.018f - 0.005f * press, -0.008f) : glm::vec3(-0.006f, -0.011f - 0.003f * press, -0.004f)) * fc;
        turn = turn * glm::angleAxis((long_gun ? 0.065f : 0.035f) * fc, glm::vec3(1, 0, 0)) *
               glm::angleAxis(-(long_gun ? 0.085f : 0.055f) * fc, glm::vec3(0, 0, 1));
    }
    at -= turn * glm::vec3(0.0f, 0.0f, gun_kick_.x);
    const glm::quat climb = glm::angleAxis(-gun_kick_.y, glm::vec3(1, 0, 0));
    const glm::vec3 about = weapon_ == SurvivalWeapon::Magnum ? model.wrist
                            : SurvivalLongGun(weapon_) && model.body ? glm::vec3(0.0f, model.sight_rear.y - 0.08f, model.body->bounds_min.z + 0.015f)
                                                                     : glm::vec3(0.0f);
    at += turn * (about - climb * about);
    turn = turn * climb;
    return view * glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(turn);
}

float SurvivalNight::ViewPush() const {
    return (SurvivalLongGun(weapon_) ? 0.6f : weapon_ == SurvivalWeapon::Magnum ? 0.25f : 0.0f) * gun_kick_.x;
}

float SurvivalNight::ThirdPersonRaise() const {
    return raise_;
}

bool SurvivalNight::BodyHoldsGun() const {
    return game_.FreeView() || (game_.ThirdPersonCameraActive() && game_.ThirdPersonShown());
}

uint8_t SurvivalNight::BodyViews() const {
    return game_.FreeView() && !game_.BodyShown() ? 1 : 0;
}

glm::mat4 SurvivalNight::WeaponTransform(const Camera& camera) const {
    if (!BodyHoldsGun()) return FirstPersonWeapon(camera);
    const Player& player = game_.GetPlayer();
    const glm::mat4 body = player.PoseTransform();
    const float sprint = SprintWeight();
    const glm::vec3 shoulder = glm::vec3(body * glm::vec4(-0.19f, 1.42f, 0.02f, 1.0f));
    Camera own = player.MakeCamera();
    own.position += player.DrawnOffset();
    const glm::vec3 own_fwd = own.Forward();
    glm::vec3 point(0.0f);
    if (aim_cache_time_ == time_ && glm::distance(own.position, aim_cache_pos_) < 1e-4f && glm::dot(own_fwd, aim_cache_dir_) > 0.99999f) {
        point = aim_cache_point_;
    } else {
        RayHit hit;
        const float reach = game_.Collision().Raycast(own.position, own_fwd, 60.0f, hit) ? hit.distance : 60.0f;
        point = own.position + own_fwd * std::max(reach, 2.0f);
        aim_cache_time_ = time_;
        aim_cache_pos_ = own.position;
        aim_cache_dir_ = own_fwd;
        aim_cache_point_ = point;
    }
    (void)camera;
    const glm::vec3 body_forward = glm::normalize(glm::vec3(body * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
    const glm::vec3 body_left = glm::normalize(glm::vec3(body * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
    const float raise = ThirdPersonRaise();
    glm::vec3 flat = glm::vec3(body_forward.x, 0.0f, body_forward.z);
    flat = glm::length(flat) > 1e-3f ? glm::normalize(flat) : glm::vec3(0.0f, 0.0f, 1.0f);
    const bool across = SurvivalLongGun(weapon_);
    constexpr float kCarryAhead = 0.165f, kCarryDown = -0.10f, kCarryIn = 0.035f;
    constexpr glm::vec3 kPocket(-0.105f, 0.18f, 0.205f);
    constexpr glm::vec3 kCarryPocket(-0.155f, 0.138f, 0.168f);
    constexpr glm::vec3 kReloadPocket(-0.112f, 0.176f, 0.215f);
    const SurvivalGunModel& held_model = models_.guns[static_cast<size_t>(weapon_)];
    const glm::vec3 butt_of = held_model.body ? glm::vec3(0.0f, held_model.sight_rear.y - 0.08f, held_model.body->bounds_min.z + 0.015f) : glm::vec3(0.0f, 0.0f, -0.35f);
    const auto across_dir = [&](const glm::vec3& ahead) { return glm::normalize(ahead * 0.68f + glm::vec3(0.0f, -0.40f, 0.0f) + body_left * 0.61f); };
    const auto run_dir = [&](const glm::vec3& ahead) { return glm::normalize(ahead * 0.50f + glm::vec3(0.0f, -0.44f, 0.0f) + body_left * 0.74f); };
    const auto reload_long_dir = [&](const glm::vec3& ahead) { return glm::normalize(ahead * 0.85f + glm::vec3(0.0f, -0.06f, 0.0f) + body_left * 0.28f); };
    const float rf = Smooth(reload_front_);
    glm::vec3 ready_dir_now(0.0f);
    glm::mat4 ride(1.0f);
    if (glm::mat4 chest_bone(1.0f); player.NaturalBone("SKL_002_CHEST", chest_bone)) {
        constexpr glm::vec3 kChestStand(0.002f, 1.207f, 0.002f);
        const glm::vec3 chest_ahead(chest_bone * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        ride = body * glm::translate(glm::mat4(1.0f), glm::vec3(chest_bone[3])) * glm::rotate(glm::mat4(1.0f), std::atan2(chest_ahead.x, chest_ahead.z), glm::vec3(0.0f, 1.0f, 0.0f)) *
               glm::translate(glm::mat4(1.0f), -kChestStand) * glm::inverse(body);
    }
    const auto ridden = [&](const glm::vec3& p) { return glm::vec3(ride * glm::vec4(p, 1.0f)); };
    const auto ridden_dir = [&](const glm::vec3& d) { return glm::normalize(glm::vec3(ride * glm::vec4(d, 0.0f))); };
    glm::vec3 ready_dir = across ? glm::normalize(glm::mix(across_dir(flat), reload_long_dir(flat), rf))
                                       : weapon_ == SurvivalWeapon::Magnum ? glm::normalize(flat * 0.79f + glm::vec3(0.0f, -0.56f, 0.0f) + body_left * 0.24f)
                                                                           : glm::normalize(flat * 0.81f + glm::vec3(0.0f, -0.56f, 0.0f) + body_left * 0.17f);
    const float breath_y = 0.0035f * std::sin(time_ * 1.75f);
    glm::vec3 ready_wrist = ridden(shoulder + body_left * (weapon_ == SurvivalWeapon::Magnum ? 0.095f : 0.090f) +
                                   glm::vec3(0.0f, (weapon_ == SurvivalWeapon::Magnum ? -0.270f : -0.275f) + breath_y, 0.0f) +
                                   flat * (weapon_ == SurvivalWeapon::Magnum ? 0.315f : 0.305f));
    ready_wrist += (flat * 0.016f * std::sin(bob_phase_) + glm::vec3(0.0f, 0.007f, 0.0f) * std::sin(2.0f * bob_phase_)) * bob_weight_;
    ready_dir = ridden_dir(ready_dir);
    glm::vec3 carry_butt = ridden(shoulder + flat * kCarryAhead + glm::vec3(0.0f, glm::mix(kCarryDown, -0.02f, rf), 0.0f) + body_left * kCarryIn);
    if (glm::mat4 chest(1.0f); across && player.BoneModel("SKL_002_CHEST", chest)) {
        const float in = weapon_ == SurvivalWeapon::Double ? 0.035f : 0.0f;
        const glm::vec3 pocket_now = glm::mix(kCarryPocket, kReloadPocket, rf);
        carry_butt = glm::vec3(body * chest * glm::vec4(pocket_now.x + in * (1.0f - rf), pocket_now.y, pocket_now.z, 1.0f));
    }
    glm::vec3 ready_at = across ? glm::vec3(Frame(carry_butt, ready_dir) * glm::vec4(-butt_of, 1.0f))
                                : glm::vec3(Frame(ready_wrist, ready_dir) * glm::vec4(-held_model.wrist, 1.0f));
    {
        const glm::vec3 grip(Frame(ready_at, ready_dir) * glm::vec4(held_model.wrist, 1.0f));
        ready_at += glm::vec3(body * glm::vec4(player.KeepOffTorso(glm::vec3(glm::inverse(body) * glm::vec4(grip, 1.0f)), across ? 0.068f : 0.110f), 0.0f));
    }
    if (!across && reload_front_ > 0.0f) {
        const float k = reload_front_;
        const glm::vec3 reload_dir = weapon_ == SurvivalWeapon::Magnum ? glm::normalize(flat * 0.6f + glm::vec3(0.0f, -0.72f, 0.0f) + body_left * 0.36f)
                                                                       : glm::normalize(flat * 0.82f + glm::vec3(0.0f, -0.3f, 0.0f) + body_left * 0.2f);
        const glm::vec3 reload_at = glm::vec3(Frame(ridden(shoulder + body_left * 0.15f + flat * 0.35f + glm::vec3(0.0f, -0.27f, 0.0f)), reload_dir) * glm::vec4(-held_model.wrist, 1.0f));
        ready_dir_now = glm::normalize(glm::mix(ready_dir, reload_dir, k));
        ready_at = glm::mix(ready_at, reload_at, k);
        const glm::vec3 wrist_now(Frame(ready_at, ready_dir_now) * glm::vec4(held_model.wrist, 1.0f));
        ready_at += glm::vec3(body * glm::vec4(player.KeepOffTorso(glm::vec3(glm::inverse(body) * glm::vec4(wrist_now, 1.0f)), weapon_ == SurvivalWeapon::Magnum ? 0.13f : 0.1f), 0.0f));
    }
    glm::vec3 eye = glm::vec3(body * glm::vec4(-0.032f, 1.66f, 0.07f, 1.0f));
    glm::mat4 head(1.0f);
    const bool head_known = player.BoneModel("SKL_004_HEAD", head);
    if (head_known) eye = glm::vec3(body * head * glm::vec4(-0.032f, 0.06f, 0.06f, 1.0f));
    const glm::vec3 sight_line = glm::normalize(point - eye);
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    glm::vec3 up_at(0.0f);
    glm::vec3 up_line = sight_line;
    if (SurvivalLongGun(weapon_) && model.body) {
        glm::vec3 pocket = shoulder + body_forward * 0.05f + glm::vec3(0.0f, -0.17f, 0.0f);
        if (glm::mat4 chest(1.0f); player.BoneModel("SKL_002_CHEST", chest)) pocket = glm::vec3(body * chest * glm::vec4(kPocket.x, kPocket.y, kPocket.z, 1.0f));
        const glm::vec3 butt(0.0f, model.sight_rear.y - 0.08f, model.body->bounds_min.z + 0.015f);
        up_line = glm::normalize(point - pocket);
        up_at = glm::vec3(Frame(pocket, up_line) * glm::vec4(-butt, 1.0f));
    } else {
        const glm::vec3 eyes = head_known ? glm::vec3(body * head * glm::vec4(0.0f, 0.06f, 0.06f, 1.0f)) : eye + body_left * 0.032f;
        float out = 0.4f;
        const auto gun_at = [&](float by) { return glm::vec3(Frame(eyes + sight_line * by, sight_line) * glm::vec4(-model.sight_rear - glm::vec3(0.0f, 0.03f, 0.0f), 1.0f)); };
        if (glm::mat4 upper(1.0f), fore(1.0f), hand_bone(1.0f);
            player.NaturalBone("SKL_021_RUARM", upper) && player.NaturalBone("SKL_022_RFARM", fore) && player.NaturalBone("SKL_023_RHAND", hand_bone)) {
            const float arm = glm::distance(glm::vec3(upper[3]), glm::vec3(fore[3])) + glm::distance(glm::vec3(fore[3]), glm::vec3(hand_bone[3]));
            const glm::vec3 from = glm::vec3(body * upper[3]);
            for (int i = 0; i < 3; ++i) {
                const glm::vec3 wrist = glm::vec3(Frame(gun_at(out), sight_line) * glm::vec4(model.wrist, 1.0f)) - from;
                const float reach = glm::length(wrist);
                const float along = reach > 1.0e-4f ? glm::dot(wrist / reach, sight_line) : 1.0f;
                if (along < 0.3f) break;
                out = std::clamp(out - (reach - 0.915f * arm) / along, 0.25f, 0.44f);
            }
        }
        up_at = gun_at(out);
    }
    glm::vec3 aim = glm::normalize(glm::mix(glm::length(ready_dir_now) > 0.5f ? ready_dir_now : ready_dir, up_line, raise));
    glm::vec3 at = glm::mix(ready_at, up_at, raise);
    if (!across) {
        const auto arc = [](float w) { return 4.0f * w * (1.0f - w); };
        at += (-body_left * 0.07f + body_forward * 0.07f) * std::max({arc(raise), arc(reload_front_), arc(Smooth(swap_))});
    } else {
        const auto arc = [](float w) { return 4.0f * w * (1.0f - w); };
        at += body_forward * (0.03f * arc(rf));
        at -= body_left * (0.08f * rf * Smooth(raise / 0.25f) * (1.0f - raise));
    }
    if (!across && raise > 0.0f && raise < 1.0f) {
        const glm::vec3 wrist_now(Frame(at, aim) * glm::vec4(held_model.wrist, 1.0f));
        at += glm::vec3(body * glm::vec4(player.KeepOffTorso(glm::vec3(glm::inverse(body) * glm::vec4(wrist_now, 1.0f)), weapon_ == SurvivalWeapon::Magnum ? 0.13f : 0.1f), 0.0f));
    }
    if (!across && raise > 0.01f && raise < 0.99f) {
        if (glm::mat4 upper(1.0f), fore(1.0f), hand_bone(1.0f);
            player.NaturalBone("SKL_021_RUARM", upper) && player.NaturalBone("SKL_022_RFARM", fore) && player.NaturalBone("SKL_023_RHAND", hand_bone)) {
            const float a = glm::distance(glm::vec3(upper[3]), glm::vec3(fore[3])), b = glm::distance(glm::vec3(fore[3]), glm::vec3(hand_bone[3]));
            const glm::vec3 from(body * upper[3]);
            const glm::vec3 aim_set = aim;
            const float share = 4.0f * raise * (1.0f - raise);
            for (int i = 0; i < 2 && a > 1.0e-3f && b > 1.0e-3f; ++i) {
                const glm::vec3 wrist(Frame(at, aim) * glm::vec4(held_model.wrist, 1.0f));
                const glm::vec3 to = wrist - from;
                const float reach = glm::length(to);
                if (reach < 1.0e-3f) break;
                const glm::vec3 n = to / reach;
                const float d = std::clamp(reach, std::abs(a - b) + 1.0e-4f, a + b - 1.0e-4f);
                glm::vec3 pole = glm::vec3(0.0f, -1.0f, 0.0f) - n * glm::dot(glm::vec3(0.0f, -1.0f, 0.0f), n);
                if (glm::length(pole) < 1.0e-3f) pole = -body_forward;
                pole = glm::normalize(pole);
                const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
                const glm::vec3 elbow = from + n * (a * cos_a) + pole * (a * std::sqrt(1.0f - cos_a * cos_a));
                if (glm::distance(wrist, elbow) < 1.0e-3f) break;
                const glm::vec3 mixed = glm::mix(aim_set, glm::normalize(wrist - elbow), share);
                if (glm::length(mixed) > 1.0e-3f) aim = glm::normalize(mixed);
            }
        }
    }
    const float away = SwingAway();
    const float low = std::max({across ? 0.0f : sprint, hud_.dead ? 1.0f : 0.0f, across ? 0.0f : away});
    const glm::vec3 low_dir = glm::normalize(body_forward * 0.5f + glm::vec3(0.0f, -0.75f, 0.0f));
    aim = glm::normalize(glm::mix(aim, low_dir, low * 0.8f));
    at = glm::mix(at, shoulder + low_dir * 0.3f + body_forward * 0.12f + glm::vec3(0.0f, -0.12f, 0.0f) + body_left * (0.16f + 0.21f * away), low * 0.8f);
    if (across && away > 0.0f) {
        at += (glm::vec3(0.0f, -0.17f, 0.0f) + body_left * 0.07f - body_forward * 0.05f) * away;
        aim = glm::normalize(aim + glm::vec3(0.0f, -0.4f, 0.0f) * away);
    }
    if (!across && away > 0.0f) {
        const bool magnum = weapon_ == SurvivalWeapon::Magnum;
        const glm::vec3 left_wrist = ridden(glm::vec3(body * glm::vec4(magnum ? 0.33f : 0.31f, 0.95f, magnum ? 0.14f : 0.1f, 1.0f)));
        const glm::vec3 hang = glm::normalize(body_forward * 0.35f + glm::vec3(0.0f, -0.94f, 0.0f));
        glm::mat4 on_gun(1.0f);
        LeftHandOnGun(on_gun);
        at = glm::mix(at, left_wrist - glm::vec3(Frame(glm::vec3(0.0f), hang) * glm::vec4(glm::vec3(on_gun[3]), 0.0f)), away) + body_forward * (0.2f * 4.0f * away * (1.0f - away));
        aim = glm::normalize(glm::mix(aim, hang, away));
    }
    const float run = Smooth((run_stride::Weight(1.0f + BodyRunWeight() * (run_stride::kNightSprintSpeed - 1.0f), run_stride::kNightSprintSpeed, run_stride::kNightPaceSpeed) - 0.1f) / 0.45f);
    if (glm::mat4 chest_bone(1.0f); run > 0.0f && (player.NaturalBone("SKL_002_CHEST", chest_bone) || player.BoneModel("SKL_002_CHEST", chest_bone))) {
        const glm::vec3 chest_at(body * chest_bone[3]);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 right = -body_left;
        const bool long_gun = SurvivalLongGun(weapon_);
        const glm::vec3 chest_ahead(chest_bone * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        const float shoulders = std::clamp(std::atan2(chest_ahead.x, chest_ahead.z), -0.35f, 0.35f);
        const glm::quat swing = glm::angleAxis((long_gun ? 0.18f : 0.08f) * shoulders, glm::vec3(body * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));
        const glm::vec3 turned = swing * run_dir(body_forward);
        const glm::vec3 carry_dir = long_gun ? turned
                                             : weapon_ == SurvivalWeapon::Magnum ? glm::normalize(body_forward * 0.72f - up * 0.65f + body_left * 0.22f)
                                                                                 : glm::normalize(body_forward * 0.74f - up * 0.64f + body_left * 0.18f);
        glm::vec3 carry_at(0.0f);
        if (long_gun) {
            const glm::vec3 stride_pump = up * (0.011f * std::cos(2.0f * bob_phase_)) + body_forward * (0.006f * std::sin(bob_phase_));
            const float in = weapon_ == SurvivalWeapon::Double ? 0.016f : 0.0f;
            const glm::vec3 butt_at = glm::vec3(body * chest_bone * glm::vec4(-0.165f + in, 0.172f, 0.132f, 1.0f)) + stride_pump;
            carry_at = glm::vec3(Frame(butt_at, turned) * glm::vec4(-butt_of, 1.0f));
            const glm::vec3 grip(Frame(carry_at, turned) * glm::vec4(held_model.wrist, 1.0f));
            carry_at += glm::vec3(body * glm::vec4(player.KeepOffTorso(glm::vec3(glm::inverse(body) * glm::vec4(grip, 1.0f)), 0.068f), 0.0f));
        } else {
            const glm::vec3 pump = body_forward * (0.010f * std::sin(bob_phase_)) + up * (0.010f * std::cos(2.0f * bob_phase_));
            glm::vec3 sprint_wrist = chest_at + body_forward * 0.275f + right * 0.045f - up * 0.065f + pump;
            sprint_wrist += glm::vec3(body * glm::vec4(player.KeepOffTorso(glm::vec3(glm::inverse(body) * glm::vec4(sprint_wrist, 1.0f)), 0.115f), 0.0f));
            carry_at = glm::vec3(Frame(sprint_wrist, carry_dir) * glm::vec4(-held_model.wrist, 1.0f));
        }
        aim = glm::normalize(glm::mix(aim, carry_dir, run));
        at = glm::mix(at, carry_at, run);
        if (static const bool log = std::getenv("PT_SURVIVAL_CARRY_LOG") != nullptr; log && game_.Frame() % 20 == 0) {
            LogInfo("survival: carry run {:.2f} chest ({:.2f} {:.2f} {:.2f}) feet y {:.2f} at ({:.2f} {:.2f} {:.2f}) aim ({:.2f} {:.2f} {:.2f})", run, chest_at.x, chest_at.y,
                    chest_at.z, player.Feet().y, at.x, at.y, at.z, aim.x, aim.y, aim.z);
        }
    }
    if (const float drawn = Smooth(swap_); drawn > 0.0f) {
        const bool long_gun_now = SurvivalLongGun(weapon_);
        const glm::vec3 from_dir = glm::normalize(body_forward * 0.78f + glm::vec3(0.0f, long_gun_now ? -0.3f : -0.6f, 0.0f) + body_left * (long_gun_now ? -0.32f : 0.18f));
        glm::vec3 wrist = glm::vec3(body * glm::vec4(-0.30f, glm::mix(0.95f, 0.84f, draw_pick_) + (long_gun_now ? 0.12f * (1.0f - draw_pick_) : 0.0f), glm::mix(0.18f, 0.22f, draw_pick_), 1.0f));
        wrist += glm::vec3(body * glm::vec4(player.KeepOffTorso(glm::vec3(glm::inverse(body) * glm::vec4(wrist, 1.0f)), 0.13f), 0.0f));
        const glm::vec3 from_at = wrist - glm::vec3(Frame(glm::vec3(0.0f), from_dir) * glm::vec4(model.wrist, 0.0f));
        aim = glm::normalize(glm::mix(aim, from_dir, drawn));
        at = glm::mix(at, from_at, drawn) + (-body_left * 0.06f + body_forward * 0.07f) * (4.0f * drawn * (1.0f - drawn));
    }
    if (const float wind = ShoveWind(); wind > 0.0f) {
        if (SurvivalLongGun(weapon_)) at += (-aim * 0.07f + glm::vec3(0.0f, 0.03f, 0.0f) + body_forward * 0.02f) * wind;
    }
    const float jab = ShoveOut();
    if (jab > 0.0f) {
        const glm::vec3 chest = shoulder + body_left * (SurvivalLongGun(weapon_) ? 0.1f : 0.19f) + glm::vec3(0.0f, -0.16f, 0.0f);
        if (SurvivalLongGun(weapon_)) {
            const glm::vec3 stroke = glm::normalize(body_left * 0.86f - body_forward * 0.2f + glm::vec3(0.0f, 0.12f, 0.0f));
            const glm::vec3 grip = shoulder + body_left * 0.14f + glm::vec3(0.0f, -0.1f, 0.0f) + body_forward * 0.5f;
            aim = glm::normalize(glm::mix(aim, stroke, jab));
            at = glm::mix(at, glm::vec3(Frame(grip, stroke) * glm::vec4(-held_model.wrist, 1.0f)), jab);
        } else {
            at = glm::mix(at, shoulder - body_left * (weapon_ == SurvivalWeapon::Magnum ? 0.16f : 0.07f) + glm::vec3(0.0f, -0.2f, 0.0f) + body_forward * (weapon_ == SurvivalWeapon::Magnum ? 0.42f : 0.38f), jab);
            aim = glm::normalize(glm::mix(aim, glm::normalize(body_forward + glm::vec3(0.0f, -0.12f, 0.0f)), jab));
            at += (-body_left * 0.09f + body_forward * 0.05f) * (4.0f * jab * (1.0f - jab));
            const glm::vec3 kept(Frame(at, aim) * glm::vec4(held_model.wrist, 1.0f));
            at += glm::vec3(body * glm::vec4(player.KeepOffTorso(glm::vec3(glm::inverse(body) * glm::vec4(kept, 1.0f)), 0.14f), 0.0f));
        }
    }
    glm::mat4 m = Frame(at, aim);
    m = m * glm::rotate(glm::mat4(1.0f), -0.06f * (1.0f - aim_) - (across ? 0.25f : 0.14f) * (1.0f - raise) * (1.0f - rf), glm::vec3(0, 0, 1));
    if (ReloadPose pose; ReloadNow(pose)) {
        if (weapon_ == SurvivalWeapon::Shotgun || weapon_ == SurvivalWeapon::Lever) {
            m = m * glm::translate(glm::mat4(1.0f), butt_of) * Rot(pose.turn.z * 0.32f, {0, 0, 1}) * glm::translate(glm::mat4(1.0f), -butt_of);
        } else if (weapon_ == SurvivalWeapon::Double) {
            m = m * glm::translate(glm::mat4(1.0f), butt_of) * TurnXyz(pose.turn * glm::vec3(0.12f, 0.14f, 0.28f)) * glm::translate(glm::mat4(1.0f), -butt_of);
        } else {
            m = m * TurnXyz(pose.turn);
        }
    }
    m = glm::translate(m, glm::vec3(0.0f, 0.0f, -gun_kick_.x)) * Rot(-gun_kick_.y, {1, 0, 0});
    if (jab > 0.0f) {
        const bool long_gun = SurvivalLongGun(weapon_);
        if (!long_gun) m = m * Rot(-0.5f * jab, {0, 0, 1});
    }
    if (const float fc = FlashClickWeight(); fc > 0.0f && weapon_ != SurvivalWeapon::Flashlight) {
        const bool long_gun = SurvivalLongGun(weapon_);
        m[3] += glm::vec4(0.0f, -(long_gun ? 0.018f : 0.008f) * fc, 0.0f, 0.0f);
        m = m * Rot((long_gun ? 0.08f : 0.03f) * fc, {1, 0, 0}) * Rot(-(long_gun ? 0.075f : 0.055f) * fc, {0, 0, 1});
    }
    if (SurvivalLongGun(weapon_)) {
        if (glm::mat4 bone(1.0f); player.NaturalBone("SKL_021_RUARM", bone) || player.BoneModel("SKL_021_RUARM", bone)) {
            constexpr float kReach = 0.485f;
            const glm::vec3 from_shoulder = glm::vec3(m * glm::vec4(held_model.wrist, 1.0f)) - glm::vec3(body * bone[3]);
            if (const float length = glm::length(from_shoulder); length > kReach) {
                const float side = glm::dot(from_shoulder, body_left);
                const glm::vec3 way = glm::normalize(from_shoulder / length + body_left * (1.6f * std::clamp(side / 0.1f, -1.0f, 1.0f)));
                const float along = glm::dot(from_shoulder, way);
                m[3] -= glm::vec4(way * (along - std::sqrt(std::max(along * along - (length * length - kReach * kReach), 0.0f))), 0.0f);
            }
        }
        if (glm::mat4 lsh(1.0f); rf < 0.05f && away <= 0.0f && jab <= 0.0f && FlashClickWeight() < 0.05f && player.BoneModel("SKL_011_LUARM", lsh)) {
            constexpr float kLeftReach = 0.485f;
            const glm::vec3 pump_world = glm::vec3(m * glm::vec4(held_model.pump_grip, 1.0f));
            const glm::vec3 from_left = pump_world - glm::vec3(body * lsh[3]);
            if (const float length = glm::length(from_left); length > kLeftReach) {
                const float pull = std::min(length - kLeftReach, 0.095f);
                m[3] -= glm::vec4(body_forward * pull, 0.0f);
            }
        }
    }
    if (SurvivalLongGun(weapon_)) {
        const glm::mat4 inv_body = glm::inverse(body);
        const std::array<std::pair<glm::vec3, float>, 4> probes = {{
            {butt_of, 0.032f},
            {glm::mix(butt_of, held_model.wrist, 0.5f), 0.036f},
            {held_model.wrist, rf > 0.0f ? glm::mix(0.068f, 0.115f, rf) : 0.068f},
            {glm::vec3(0.0f, 0.0f, 0.06f), 0.036f},
        }};
        glm::vec3 push(0.0f);
        for (const auto& [local_pt, margin] : probes) {
            const glm::vec3 world_pt(m * glm::vec4(local_pt, 1.0f));
            const glm::vec3 delta = player.KeepOffTorso(glm::vec3(inv_body * glm::vec4(world_pt, 1.0f)), margin);
            if (std::abs(delta.x) > std::abs(push.x)) push.x = delta.x;
            if (std::abs(delta.y) > std::abs(push.y)) push.y = delta.y;
            if (std::abs(delta.z) > std::abs(push.z)) push.z = delta.z;
        }
        m[3] += glm::vec4(glm::vec3(body * glm::vec4(push, 0.0f)), 0.0f);
    }
    return m;
}

namespace {

glm::mat4 TurnXyz(const glm::vec3& r) {
    return glm::rotate(glm::mat4(1.0f), r.x, glm::vec3(1, 0, 0)) * glm::rotate(glm::mat4(1.0f), r.y, glm::vec3(0, 1, 0)) *
           glm::rotate(glm::mat4(1.0f), r.z, glm::vec3(0, 0, 1));
}

glm::mat4 GripHand(const SurvivalGunModel& gun) {
    glm::mat4 hand(1.0f);
    hand[0] = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    hand[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    hand[2] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    return glm::translate(glm::mat4(1.0f), gun.wrist) * glm::rotate(glm::mat4(1.0f), gun.rake, glm::vec3(1, 0, 0)) * TurnXyz(gun.grip_turn) * hand;
}

}

bool SurvivalNight::ThirdPersonSupport(const Camera& camera, glm::vec3& point, glm::vec3& bore) const {
    if (!active_ || weapon_ == SurvivalWeapon::Flashlight || hud_.dead || !BodyHoldsGun()) {
        return false;
    }
    const glm::mat4 gun = WeaponTransform(camera);
    bore = glm::normalize(glm::vec3(gun[2]));
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    if (SurvivalLongGun(weapon_)) {
        point = glm::vec3(gun * BarrelsTurn() * glm::vec4(model.pump_grip + glm::vec3(0.0f, -0.046f, -0.02f - model.pump_travel * gun_.slide), 1.0f));
        return true;
    }
    if (reload_timer_ > 0.0f) {
        const float t = reload_length_ - reload_timer_;
        if (weapon_ == SurvivalWeapon::Handgun) {
            const float seat = reload_from_empty_ ? 0.62f : 0.6f;
            if (t < 0.22f || t > seat + 0.12f) return false;
            const float out = t < seat - 0.18f ? 1.0f : gun_.magazine;
            point = glm::vec3(gun * glm::vec4(model.magazine_base + glm::vec3(0.0f, -0.024f, -0.006f) + model.magazine_drop * (0.16f * out), 1.0f));
            return true;
        }
        if (t < 0.65f || t > 1.35f) return false;
        const float in = Smooth((t - 0.75f) / 0.45f);
        point = glm::vec3(gun * glm::vec4(model.cylinder_axis + glm::vec3(model.cylinder_swing.x * gun_.swing, -0.03f, -0.07f - 0.09f * (1.0f - in)), 1.0f));
        return true;
    }
    if (ThirdPersonRaise() < 0.6f) return false;
    point = glm::vec3(gun * glm::vec4(0.006f, -0.105f, -0.055f, 1.0f));
    return true;
}

std::optional<std::array<glm::vec4, 5>> SurvivalNight::GripFingers() const {
    if (active_ && PipeHold() > 0.0f) return PipeFist();
    if (!active_ || weapon_ == SurvivalWeapon::Flashlight) return std::nullopt;
    return HeldFingers(models_.guns[static_cast<size_t>(weapon_)]);
}

std::array<glm::vec4, 5> SurvivalNight::HeldFingers(const SurvivalGunModel& model) const {
    std::array<glm::vec4, 5> shape = model.fingers;
    if (model.trigger_index) shape[1] = glm::mix(shape[1], *model.trigger_index, Smooth(trigger_finger_));
    if (ReloadPose pose; ReloadNow(pose)) shape[0] = glm::mix(shape[0], pose.right_thumb, pose.thumb);
    if (SurvivalLongGun(weapon_)) shape[0] = glm::mix(shape[0], glm::vec4(0.0f), Smooth(sights_w_));
    if (BodyHoldsGun()) {
        if (!SurvivalLongGun(weapon_)) {
            shape[0] = glm::vec4(shape[0].x, std::max(shape[0].y, 0.58f), std::clamp(shape[0].z, -0.35f, 0.25f), std::clamp(shape[0].w, -0.15f, 0.25f));
            for (size_t f = 2; f < shape.size(); ++f) {
                shape[f].y = std::max(shape[f].y, 0.52f);
                shape[f].z = std::max(shape[f].z, 0.36f);
                shape[f].w = std::max(shape[f].w, 0.24f);
            }
        }
        if (SurvivalLongGun(weapon_)) {
            glm::vec4 over(0.22f, 1.93f, 1.62f, 1.4f);
            if (ReloadPose pose; ReloadNow(pose)) over = glm::mix(over, pose.right_thumb, pose.thumb);
            shape[0] = over;
            if (weapon_ != SurvivalWeapon::Shotgun) shape[1] += glm::vec4(0.0f, 0.5f, 0.7f, 0.5f) * (0.5f + 0.5f * Smooth(trigger_finger_));
            for (size_t f = 2; f < shape.size(); ++f) {
                shape[f].y = std::max(shape[f].y, 0.50f);
                shape[f].z = std::max(shape[f].z, 0.32f);
                shape[f].w = std::max(shape[f].w, 0.20f);
            }
        }
    }
    return shape;
}

std::optional<std::array<glm::vec4, 5>> SurvivalNight::SupportFingers() const {
    if (active_ && FlashClickWeight() > 0.02f) return FlashClickFingers();
    if (active_ && BodyHoldsGun() && (weapon_ == SurvivalWeapon::Handgun || weapon_ == SurvivalWeapon::Magnum) && std::max(ShoveOut(), ShoveWind()) > 0.02f) {
        return std::array<glm::vec4, 5>{glm::vec4(0.3f, 0.1f, 0.1f, 0.0f), glm::vec4(-0.05f, 0.12f, 0.12f, 0.06f), glm::vec4(0.0f, 0.12f, 0.12f, 0.06f),
                                        glm::vec4(0.03f, 0.14f, 0.14f, 0.08f), glm::vec4(0.08f, 0.18f, 0.16f, 0.1f)};
    }
    if (active_ && ThrowAway() > 0.0f) return ThrowFingers();
    if (!active_ || weapon_ == SurvivalWeapon::Flashlight) return std::nullopt;
    return LeftFingers(models_.guns[static_cast<size_t>(weapon_)]);
}

std::array<glm::vec4, 5> SurvivalNight::LeftFingers(const SurvivalGunModel& model) const {
    std::array<glm::vec4, 5> shape = model.support_fingers;
    if (BodyHoldsGun() && !SurvivalLongGun(weapon_)) {
        shape[0] = glm::vec4(-0.04f, -0.10f, 0.14f, 0.04f);
        shape[1] = glm::vec4(-0.05f, 0.56f, 0.78f, 0.44f);
        shape[2] = glm::vec4(0.02f, 0.66f, 0.86f, 0.52f);
        shape[3] = glm::vec4(0.05f, 0.68f, 0.80f, 0.48f);
        shape[4] = glm::vec4(0.08f, 0.70f, 0.72f, 0.44f);
    }
    if (BodyHoldsGun() && SurvivalLongGun(weapon_)) {
        shape[0] = glm::vec4(0.08f, 0.28f, 0.36f, 0.22f);
        constexpr float kFan[4] = {-0.06f, -0.01f, 0.03f, 0.08f};
        for (size_t f = 1; f < shape.size(); ++f) {
            const float ff = static_cast<float>(f);
            shape[f] = glm::vec4(kFan[f - 1], 0.70f + 0.035f * ff, 0.60f + 0.03f * ff, 0.42f + 0.025f * ff);
        }
    }
    if (ReloadPose pose; ReloadNow(pose)) {
        for (size_t f = 0; f < shape.size(); ++f) shape[f] = glm::mix(shape[f], pose.fingers[f], pose.hand);
        if (BodyHoldsGun()) shape[1] = glm::mix(shape[1], glm::vec4(shape[1].x, shape[2].y, shape[2].z, shape[2].w), 0.85f * pose.hand);
    }
    return shape;
}

bool SurvivalNight::ChestLight(glm::mat4& world) const {
    if (!active_ || !models_.chest_light) return false;
    if (waking_ && wake_third_) {
        glm::mat4 chest(1.0f);
        if (!game_.Demos().PlayerBoneWorld("gc_p00_020", "SKL_002_CHEST", chest)) return false;
        world = chest * glm::translate(glm::mat4(1.0f), models_.chest_mount) * TurnXyz(models_.chest_turn) *
                glm::translate(glm::mat4(1.0f), -models_.chest_clip);
        return true;
    }
    const Player& player = game_.GetPlayer();
    glm::mat4 chest(1.0f);
    if (!player.BoneModel("SKL_002_CHEST", chest)) return false;
    world = player.PoseTransform() * chest * glm::translate(glm::mat4(1.0f), models_.chest_mount) * TurnXyz(models_.chest_turn) *
            glm::translate(glm::mat4(1.0f), -models_.chest_clip);
    return true;
}

namespace {
constexpr float kFlashClickDuration = 0.52f;
constexpr float kFlashClickToggleAt = 0.22f;
}

void SurvivalNight::TriggerFlashlightClick() {
    if (!active_ || hud_.dead || waking_) {
        game_.ToggleStreetFlashlightImmediate();
        return;
    }
    if (reload_timer_ > 0.0f || throw_timer_ >= 0.0f || melee_timer_ >= 0.0f || stowing_ || swap_ > 0.3f) {
        game_.ToggleStreetFlashlightImmediate();
        return;
    }
    if (flash_click_timer_ >= 0.0f) {
        if (!flash_click_toggled_) game_.ToggleStreetFlashlightImmediate();
        flash_click_timer_ = 0.09f;
        flash_click_toggled_ = false;
        return;
    }
    flash_click_timer_ = 0.0f;
    flash_click_toggled_ = false;
}

void SurvivalNight::UpdateFlashClick(float dt) {
    if (flash_click_timer_ < 0.0f) return;
    if (hud_.dead || reload_timer_ > 0.0f || throw_timer_ >= 0.0f || melee_timer_ >= 0.0f || stowing_) {
        if (!flash_click_toggled_ && !hud_.dead) game_.ToggleStreetFlashlightImmediate();
        flash_click_timer_ = -1.0f;
        flash_click_toggled_ = false;
        return;
    }
    flash_click_timer_ += dt;
    if (!flash_click_toggled_ && flash_click_timer_ >= kFlashClickToggleAt) {
        flash_click_toggled_ = true;
        game_.ToggleStreetFlashlightImmediate();
    }
    if (flash_click_timer_ >= kFlashClickDuration) {
        flash_click_timer_ = -1.0f;
        flash_click_toggled_ = false;
    }
}

float SurvivalNight::FlashClickWeight() const {
    if (flash_click_timer_ < 0.0f || hud_.dead) return 0.0f;
    const float t = flash_click_timer_;
    if (t < 0.17f) return Smooth(t / 0.17f);
    if (t < 0.30f) return 1.0f;
    return 1.0f - Smooth((t - 0.30f) / (kFlashClickDuration - 0.30f));
}

float SurvivalNight::FlashClickPress() const {
    if (flash_click_timer_ < 0.16f || flash_click_timer_ > 0.32f || hud_.dead) return 0.0f;
    const float t = flash_click_timer_;
    return t < 0.23f ? Smooth((t - 0.16f) / 0.07f) : 1.0f - Smooth((t - 0.23f) / 0.09f);
}

float SurvivalNight::FlashClickCameraDip() const {
    return 0.35f * FlashClickWeight() + 0.65f * FlashClickPress();
}

std::array<glm::vec4, 5> SurvivalNight::FlashClickFingers() {
    return {glm::vec4(0.16f, 0.22f, 0.28f, 0.14f), glm::vec4(-0.04f, 0.18f, 0.24f, 0.16f),
            glm::vec4(0.02f, 0.24f, 0.30f, 0.20f), glm::vec4(0.06f, 0.66f, 0.74f, 0.48f),
            glm::vec4(0.10f, 0.74f, 0.80f, 0.52f)};
}

float SurvivalNight::FlashClickHand3P(glm::mat4& hand) const {
    const float w = FlashClickWeight();
    if (w <= 0.0f) return 0.0f;
    glm::mat4 fl(1.0f);
    if (!ChestLight(fl)) return 0.0f;
    const float press = FlashClickPress();
    const glm::vec3 hx = glm::normalize(glm::vec3(-0.56f, 0.77f, -0.30f));
    glm::vec3 hy = glm::normalize(glm::vec3(0.24f, 0.38f, 0.89f));
    const glm::vec3 hz = glm::normalize(glm::cross(hx, hy));
    hy = glm::cross(hz, hx);
    const glm::vec3 tip_local(-0.016f, 0.012f, 0.024f - 0.012f * press);
    const glm::vec3 wrist_local = tip_local - hx * 0.108f + hy * 0.014f;
    glm::mat4 local(1.0f);
    local[0] = glm::vec4(hx, 0.0f);
    local[1] = glm::vec4(hy, 0.0f);
    local[2] = glm::vec4(hz, 0.0f);
    local[3] = glm::vec4(wrist_local, 1.0f);
    hand = fl * local;
    return w;
}

float SurvivalNight::FlashClickHand1P(const Camera& camera, glm::mat4& hand) const {
    const float w = FlashClickWeight();
    if (w <= 0.0f) return 0.0f;
    const float press = FlashClickPress();
    glm::mat4 view(1.0f);
    view[0] = glm::vec4(-camera.Right(), 0.0f);
    view[1] = glm::vec4(camera.Up(), 0.0f);
    view[2] = glm::vec4(camera.Forward(), 0.0f);
    view[3] = glm::vec4(camera.position, 1.0f);
    const glm::vec3 hx = glm::normalize(glm::vec3(-0.30f, 0.56f, -0.77f));
    glm::vec3 hy = glm::normalize(glm::vec3(0.42f, 0.72f, 0.55f));
    const glm::vec3 hz = glm::normalize(glm::cross(hx, hy));
    hy = glm::cross(hz, hx);
    const glm::vec3 wrist_view(0.195f - 0.018f * press, -0.135f - 0.016f * press, 0.345f - 0.052f * press);
    glm::mat4 local(1.0f);
    local[0] = glm::vec4(hx, 0.0f);
    local[1] = glm::vec4(hy, 0.0f);
    local[2] = glm::vec4(hz, 0.0f);
    local[3] = glm::vec4(wrist_view, 1.0f);
    hand = view * local;
    return w;
}

bool SurvivalNight::ThirdPersonLeftHand(const Camera& camera, glm::mat4& world) const {
    if (!active_ || hud_.dead || !models_.chest_light || !BodyHoldsGun()) return false;
    glm::mat4 thrown(1.0f);
    const float off = ThrowHand(SwingCamera(camera), thrown);
    glm::mat4 click_hand(1.0f);
    const float click = off <= 0.0f ? FlashClickHand3P(click_hand) : 0.0f;
    glm::mat4 on_gun(1.0f);
    const bool held = weapon_ != SurvivalWeapon::Flashlight && !stowing_ && swap_ <= 0.15f && LeftHandOnGun(on_gun) >= 0.5f;
    const bool hand_shoves = weapon_ == SurvivalWeapon::Handgun || weapon_ == SurvivalWeapon::Magnum;
    const float jab = hand_shoves ? ShoveOut() : 0.0f;
    const float cocked = hand_shoves ? std::max(jab, ShoveWind()) : 0.0f;
    glm::mat4 push(1.0f);
    if (cocked > 0.0f) {
        push[0] = glm::vec4(0.15f, 0.97f, 0.2f, 0.0f);
        push[1] = glm::vec4(0.0f, 0.2f, -0.98f, 0.0f);
        push[2] = glm::vec4(glm::cross(glm::vec3(push[0]), glm::vec3(push[1])), 0.0f);
        push[3] = glm::vec4(glm::mix(glm::vec3(0.31f, 1.2f, 0.36f), glm::vec3(0.03f, 1.36f, 0.6f), jab), 1.0f);
        push = game_.GetPlayer().PoseTransform() * push;
    }
    if (!held && off <= 0.0f && click <= 0.0f && cocked <= 0.02f) return false;
    const glm::mat4 gun_world = held ? DrawnWeaponTransform(camera) : glm::mat4(1.0f);
    glm::mat4 natural_left = click_hand;
    if (!held && click > 0.0f) {
        if (glm::mat4 nat(1.0f); game_.GetPlayer().NaturalBone("SKL_013_LHAND", nat)) {
            natural_left = game_.GetPlayer().PoseTransform() * nat;
        }
    }
    world = held ? gun_world * on_gun : off > 0.0f ? thrown : click > 0.0f ? natural_left : push;
    if (held) {
        const bool long_gun = SurvivalLongGun(weapon_);
        ReloadPose pose;
        const float fit = ReloadNow(pose) ? 1.0f - pose.hand : 1.0f;
        if (!long_gun) {
            const glm::vec3 clear_offset = (weapon_ == SurvivalWeapon::Magnum ? glm::vec3(0.002f, 0.008f, 0.005f)
                                                                              : glm::vec3(0.003f, 0.001f, 0.003f)) *
                                           fit;
            world = gun_world * glm::translate(glm::mat4(1.0f), clear_offset) * on_gun;
            constexpr glm::vec3 kPalm(0.03f, 0.0f, 0.0f);
            const float cant_y = (weapon_ == SurvivalWeapon::Magnum ? -0.04f : -0.03f) * fit;
            const float cant_z = -0.02f * fit;
            world = world * glm::translate(glm::mat4(1.0f), kPalm) * glm::rotate(glm::mat4(1.0f), cant_y, glm::vec3(0.0f, 1.0f, 0.0f)) *
                    glm::rotate(glm::mat4(1.0f), cant_z, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::translate(glm::mat4(1.0f), -kPalm);
        } else {
            const glm::vec3 knuckles(0.08f, 0.0f, 0.0f);
            const float turn_y = 0.14f * fit;
            const float turn_z = 0.05f * fit;
            const float lift_y = weapon_ == SurvivalWeapon::Shotgun ? 0.004f
                               : weapon_ == SurvivalWeapon::Lever   ? -0.012f
                                                                    : -0.008f;
            world = world * glm::translate(glm::mat4(1.0f), knuckles + glm::vec3(0.0f, lift_y, 0.0f) * fit) *
                    glm::rotate(glm::mat4(1.0f), turn_y, glm::vec3(0.0f, 1.0f, 0.0f)) *
                    glm::rotate(glm::mat4(1.0f), turn_z, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::translate(glm::mat4(1.0f), -knuckles);
        }
    }
    if (off > 0.0f) {
        const float k = Smooth(off);
        glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(world)), glm::quat_cast(glm::mat3(thrown)), k));
        m[3] = glm::vec4(glm::mix(glm::vec3(world[3]), glm::vec3(thrown[3]), k), 1.0f);
        world = m;
    } else if (click > 0.0f) {
        const glm::mat4 body = game_.GetPlayer().PoseTransform();
        const glm::vec3 body_fwd = glm::normalize(glm::vec3(body * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
        const glm::vec3 body_left = glm::normalize(glm::vec3(body * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
        const float arc = 4.0f * click * (1.0f - click);
        glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(world)), glm::quat_cast(glm::mat3(click_hand)), click));
        m[3] = glm::vec4(glm::mix(glm::vec3(world[3]), glm::vec3(click_hand[3]), click) + (body_fwd * 0.055f + body_left * 0.045f) * arc, 1.0f);
        world = m;
    }
    if (cocked > 0.02f && off <= 0.0f && click <= 0.0f) world = push;
    if (off > 0.0f || (click > 0.0f && click < 0.85f)) {
        const glm::mat4 body = game_.GetPlayer().PoseTransform();
        const float margin = off > 0.0f ? 0.09f : 0.045f * (1.0f - click);
        world[3] += body * glm::vec4(game_.GetPlayer().KeepOffTorso(glm::vec3(glm::inverse(body) * world[3]), margin), 0.0f);
    }
    return true;
}

float SurvivalNight::ThirdPersonAim() const {
    if (!active_ || weapon_ == SurvivalWeapon::Flashlight || hud_.dead) return 0.0f;
    return ThirdPersonRaise() * (1.0f - Smooth((SprintWeight() - 0.1f) / 0.45f));
}

float SurvivalNight::ThirdPersonSteady() const {
    if (!active_ || weapon_ == SurvivalWeapon::Flashlight || hud_.dead) return 0.0f;
    const float carried = SurvivalLongGun(weapon_) ? 0.48f : std::max(0.38f, 0.6f * reload_front_);
    const float sprint = Smooth((SprintWeight() - 0.1f) / 0.45f);
    return glm::mix(carried, 0.9f, ThirdPersonRaise()) * (1.0f - sprint) + 0.34f * sprint;
}

glm::vec2 SurvivalNight::ThirdPersonCarry() const {
    if (!active_ || weapon_ == SurvivalWeapon::Flashlight || hud_.dead) return glm::vec2(0.0f);
    const float sprint = Smooth((SprintWeight() - 0.1f) / 0.45f);
    const glm::vec2 stance = SurvivalLongGun(weapon_) ? glm::vec2(0.30f, 0.05f) : glm::vec2(0.16f, 0.04f);
    const float sprint_keep = SurvivalLongGun(weapon_) ? glm::mix(1.0f, 0.65f, sprint) : glm::mix(1.0f, 0.25f, sprint);
    const float k = (1.0f - ThirdPersonRaise()) * sprint_keep * (1.0f - Smooth(swap_)) * (1.0f - std::min(1.0f, ShoveOut() + ShoveWind() + SwingAway() + ThrowAway())) * (1.0f - reload_front_);
    return stance * k;
}

void SurvivalNight::ThirdPersonStance(float& long_gun, float& recoil, std::optional<glm::vec3>& left_elbow, std::optional<glm::vec3>& right_elbow, float& right_share) const {
    long_gun = SurvivalLongGun(weapon_) ? 1.0f : 0.0f;
    recoil = body_recoil_;
    right_share = 1.0f;
    left_elbow.reset();
    right_elbow.reset();
    if (const float hold = PipeHold(); hold > 0.0f) {
        const float p = Smooth((melee_timer_ - 0.15f) / 0.15f);
        right_elbow = glm::vec3(-0.85f, 0.25f - 1.15f * p, -0.15f);
        return;
    }
    if (const float jab = ShoveOut(); jab > (SurvivalLongGun(weapon_) ? 0.3f : 0.0f)) {
        if (SurvivalLongGun(weapon_)) {
            right_elbow = glm::vec3(-0.85f, -0.35f, -0.1f);
            left_elbow = glm::vec3(0.85f, -0.35f, -0.1f);
        } else {
            right_elbow = glm::vec3(-0.5f, -0.4f, -0.8f);
        }
        return;
    }
    const float up = ThirdPersonAim();
    const float sprint_w = Smooth((SprintWeight() - 0.1f) / 0.45f);
    constexpr glm::vec3 across_elbow(-0.36f, -0.95f, -0.16f);
    const float across = SurvivalLongGun(weapon_) && !hud_.dead ? Smooth(reload_front_) : 0.0f;
    if (SurvivalLongGun(weapon_) && !hud_.dead) {
        if (reload_timer_ > 0.0f) {
            right_elbow = glm::mix(glm::vec3(-0.64f, -0.82f, -0.22f), across_elbow, across);
            left_elbow = glm::vec3(0.65f, -0.80f, -0.18f);
            return;
        }
        const glm::vec3 base_r = up < 0.3f ? glm::mix(glm::vec3(-0.42f, -0.92f, -0.18f), across_elbow, across)
                                           : glm::mix(glm::vec3(-0.45f, -0.92f, -0.18f), across_elbow, across);
        const glm::vec3 base_l = up < 0.3f ? glm::vec3(0.40f, -0.94f, -0.18f) : glm::vec3(0.36f, -0.98f, 0.02f);
        right_elbow = glm::mix(base_r, glm::vec3(-0.48f, -0.90f, -0.18f), sprint_w * (1.0f - Smooth(up)));
        left_elbow = glm::mix(base_l, glm::vec3(0.44f, -0.92f, -0.16f), sprint_w * (1.0f - Smooth(up)));
        if (const float fc = FlashClickWeight(); fc > 0.0f) {
            left_elbow = glm::mix(*left_elbow, glm::vec3(0.75f, -0.78f, 0.12f), fc);
        }
        return;
    }
    if ((weapon_ == SurvivalWeapon::Handgun || weapon_ == SurvivalWeapon::Magnum) && !hud_.dead) {
        if (reload_front_ > 0.02f && up < 0.1f) {
            const float k = reload_front_;
            right_elbow = glm::mix(glm::vec3(-0.46f, -0.90f, -0.20f), glm::vec3(-0.8f, -0.6f, -0.1f), k);
            left_elbow = glm::mix(glm::vec3(0.50f, -0.90f, -0.20f), glm::vec3(0.8f, -0.6f, -0.1f), k);
            return;
        }
        const float a = Smooth(up);
        const glm::vec3 carry_r = glm::mix(glm::vec3(-0.28f, -0.95f, -0.18f), glm::vec3(-0.12f, -0.98f, -0.08f), a);
        const glm::vec3 carry_l = glm::mix(glm::vec3(0.28f, -0.95f, -0.18f), glm::vec3(weapon_ == SurvivalWeapon::Magnum ? 0.55f : 0.45f, -0.95f, -0.08f), a);
        right_elbow = glm::mix(carry_r, glm::vec3(-0.30f, -0.94f, -0.18f), sprint_w * (1.0f - a));
        left_elbow = glm::mix(carry_l, glm::vec3(0.32f, -0.94f, -0.18f), sprint_w * (1.0f - a));
        if (const float fc = FlashClickWeight(); fc > 0.0f) {
            left_elbow = glm::mix(*left_elbow, glm::vec3(0.75f, -0.78f, 0.12f), fc);
        }
        return;
    }
    if (!hud_.dead && FlashClickWeight() > 0.0f) {
        left_elbow = glm::vec3(0.75f, -0.78f, 0.12f);
    }
}

std::optional<SurvivalNight::StockLine> SurvivalNight::ThirdPersonStock(const glm::mat4& body, const Camera& camera) const {
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    if (!active_ || hud_.dead || !SurvivalLongGun(weapon_) || !model.body) return std::nullopt;
    const glm::mat4 gun = glm::inverse(body) * WeaponTransform(camera);
    const float y = model.sight_rear.y - 0.1f;
    const float butt_y = std::min(y, model.body->bounds_min.y + 0.063f);
    return StockLine{glm::vec3(gun * glm::vec4(0.0f, butt_y, model.body->bounds_min.z + 0.015f, 1.0f)), glm::vec3(gun * glm::vec4(0.0f, y, model.wrist.z, 1.0f)),
                     glm::vec3(gun * glm::vec4(0.0f, 0.055f, 0.0f, 0.0f)), glm::vec3(gun * glm::vec4(0.0f, model.pump_grip.y, model.wrist.z, 1.0f)),
                     glm::vec3(gun * glm::vec4(0.0f, model.pump_grip.y, model.body->bounds_max.z, 1.0f))};
}

glm::mat4 SurvivalNight::ThirdPersonRightHandLocal() const {
    glm::mat4 local = GripOnGun();
    {
        const glm::vec2 kTune = weapon_ == SurvivalWeapon::Handgun ? glm::vec2(-0.4f, 0.0f)
                                : weapon_ == SurvivalWeapon::Magnum ? glm::vec2(-0.5f, -0.5f)
                                : weapon_ == SurvivalWeapon::Lever ? glm::vec2(-0.12f, 0.04f)
                                : weapon_ == SurvivalWeapon::Double ? glm::vec2(-0.45f, -0.45f)
                                                                    : glm::vec2(0.0f);
        ReloadPose pose;
        const float k = ReloadNow(pose) ? Smooth(reload_front_) : 0.0f;
        if (weapon_ == SurvivalWeapon::Magnum || weapon_ == SurvivalWeapon::Lever || weapon_ == SurvivalWeapon::Double) {
            const glm::vec2 kAim = weapon_ == SurvivalWeapon::Magnum ? glm::vec2(0.32f, 0.08f) : glm::vec2(-0.16f, -0.14f);
            const glm::vec3 pivot(-0.08f, 0.0f, 0.0f);
            local = local * glm::translate(glm::mat4(1.0f), pivot) * glm::rotate(glm::mat4(1.0f), kAim.x * raise_, glm::vec3(0.0f, 1.0f, 0.0f)) *
                    glm::rotate(glm::mat4(1.0f), kAim.y * raise_, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::translate(glm::mat4(1.0f), -pivot);
        }
        const glm::vec3 knuckles(-0.08f, 0.0f, 0.0f);
        local = local * glm::translate(glm::mat4(1.0f), knuckles) * glm::rotate(glm::mat4(1.0f), kTune.x * k, glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::rotate(glm::mat4(1.0f), kTune.y * k, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::translate(glm::mat4(1.0f), -knuckles);
    }
    if (SurvivalLongGun(weapon_)) {
        const float raised = std::max(ThirdPersonRaise(), Smooth(reload_front_));
        const glm::vec3 knuckles(-0.08f, 0.0f, 0.0f);
        local = local * glm::translate(glm::mat4(1.0f), knuckles) * glm::rotate(glm::mat4(1.0f), glm::mix(0.10f, -0.24f, raised), glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::rotate(glm::mat4(1.0f), glm::mix(-0.20f, 0.0f, raised), glm::vec3(0.0f, 0.0f, 1.0f)) * glm::translate(glm::mat4(1.0f), -knuckles);
    }
    return local;
}

glm::mat4 SurvivalNight::DrawnWeaponTransform(const Camera& camera) const {
    const glm::mat4 desired = WeaponTransform(camera);
    if (!BodyHoldsGun() || weapon_ == SurvivalWeapon::Flashlight || hud_.dead || SwingAway() > 0.02f || PipeHold() > 0.02f) {
        return desired;
    }
    const Player& player = game_.GetPlayer();
    if (glm::mat4 rhand_model(1.0f); player.BoneModel("SKL_023_RHAND", rhand_model)) {
        glm::mat4 welded = player.PoseTransform() * rhand_model * glm::inverse(ThirdPersonRightHandLocal());
        if (reload_timer_ > 0.0f && reload_length_ > 0.05f) {
            const ReloadStyle rstyle = ActiveReloadStyle();
            const float p = std::clamp(1.0f - reload_timer_ / reload_length_, 0.0f, 1.0f);
            if (rstyle == ReloadStyle::Tactical) {
                const float env = Smooth(p / 0.14f) * Smooth((1.0f - p) / 0.16f);
                welded = welded * glm::rotate(glm::mat4(1.0f), 0.38f * env, glm::vec3(0, 0, 1));
            } else if (rstyle == ReloadStyle::Gunslinger) {
                const float spin_u = Smooth(std::clamp((p - 0.64f) / 0.32f, 0.0f, 1.0f));
                const float lift = std::sin(spin_u * kPi);
                const float fwd = SurvivalLongGun(weapon_) ? 0.10f : 0.04f;
                welded = welded * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.055f * lift, fwd * lift))
                                * glm::rotate(glm::mat4(1.0f), spin_u * 2.0f * kPi, glm::vec3(1, 0, 0));
            } else if (rstyle == ReloadStyle::Telekinetic) {
                const float hover = Smooth((p - 0.05f) / 0.14f) * Smooth((0.95f - p) / 0.14f);
                const float bob = std::sin(p * 4.0f * kPi) * 0.02f;
                const glm::vec3 pivot(0.0f, 0.11f + bob, 0.22f);
                welded = welded * glm::translate(glm::mat4(1.0f), pivot * hover)
                                * glm::rotate(glm::mat4(1.0f), 0.72f * std::sin(p * kPi) * hover, glm::vec3(0, 1, 0))
                                * glm::rotate(glm::mat4(1.0f), -0.25f * std::cos(p * 2.0f * kPi) * hover, glm::vec3(1, 0, 0))
                                * glm::rotate(glm::mat4(1.0f), 0.48f * std::sin(p * 2.0f * kPi) * hover, glm::vec3(0, 0, 1));
            }
        }
        return welded;
    }
    return desired;
}

bool SurvivalNight::ThirdPersonGrip(const glm::mat4& body, const Camera& camera, glm::mat4& right_hand) {
    if (glm::mat4 swing(1.0f); active_ && !hud_.dead && PipeSwingFrame(SwingCamera(camera), swing)) {
        const glm::mat4 to = swing * GripHand(models_.guns[static_cast<size_t>(SurvivalWeapon::Handgun)]);
        glm::mat4 from = weapon_ == SurvivalWeapon::Flashlight ? to : WeaponTransform(camera) * GripOnGun();
        const float k = Smooth(PipeHold());
        glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(from)), glm::quat_cast(glm::mat3(to)), k));
        m[3] = glm::vec4(glm::mix(glm::vec3(from[3]), glm::vec3(to[3]), k), 1.0f);
        right_hand = glm::inverse(body) * m;
        return true;
    }
    if (!active_ || weapon_ == SurvivalWeapon::Flashlight || hud_.dead) return false;
    right_hand = glm::inverse(body) * WeaponTransform(camera) * ThirdPersonRightHandLocal();
    return true;
}

void SurvivalNight::MakeArms() {
    if (arm_tried_) return;
    {
        arm_tried_ = true;
        const std::string& file = game_.Demos().PlayerModelFile();
        const ModelEntry* full = file.empty() ? nullptr : game_.Models().Get(file);
        if (full) {
            std::vector<bool> arm(full->bone_names.size(), false);
            for (size_t i = 0; i < arm.size(); ++i) {
                for (int b = static_cast<int>(i); b >= 0 && static_cast<size_t>(b) < full->bone_parents.size(); b = full->bone_parents[static_cast<size_t>(b)]) {
                    if (full->bone_names[static_cast<size_t>(b)] == "SKL_021_RUARM") {
                        arm[i] = true;
                        break;
                    }
                }
            }
            std::vector<bool> left(full->bone_names.size(), false);
            for (size_t i = 0; i < left.size(); ++i) {
                for (int b = static_cast<int>(i); b >= 0 && static_cast<size_t>(b) < full->bone_parents.size(); b = full->bone_parents[static_cast<size_t>(b)]) {
                    if (full->bone_names[static_cast<size_t>(b)] == "SKL_011_LUARM") {
                        left[i] = true;
                        break;
                    }
                }
            }
            auto keep = [](std::vector<bool> bones) {
                return [bones](const Vertex& v) {
                    int best = 0;
                    for (int k = 1; k < 4; ++k) {
                        if (v.weights[k] > v.weights[best]) best = k;
                    }
                    const size_t bone = v.joints[best];
                    return bone < bones.size() && bones[bone];
                };
            };
            auto bone = [&](std::string_view name) {
                for (size_t i = 0; i < full->bone_names.size(); ++i) {
                    if (full->bone_names[i] == name) return static_cast<int>(i);
                }
                return -1;
            };
            struct Limb {
                glm::vec3 shoulder, elbow, wrist;
                bool valid = false;
            };
            std::array<Limb, 2> limbs{};
            const std::array<std::array<const char*, 3>, 2> names = {std::array<const char*, 3>{"SKL_021_RUARM", "SKL_022_RFARM", "SKL_023_RHAND"},
                                                                     std::array<const char*, 3>{"SKL_011_LUARM", "SKL_012_LFARM", "SKL_013_LHAND"}};
            for (size_t side = 0; side < 2; ++side) {
                const int a = bone(names[side][0]), b = bone(names[side][1]), c = bone(names[side][2]);
                if (a < 0 || b < 0 || c < 0 || static_cast<size_t>(std::max({a, b, c})) >= full->bone_bind_world.size()) continue;
                limbs[side] = {full->bone_bind_world[static_cast<size_t>(a)], full->bone_bind_world[static_cast<size_t>(b)],
                               full->bone_bind_world[static_cast<size_t>(c)], true};
            }
            auto sleeves = [limbs](Vertex& v) {
                auto along = [&](const glm::vec3& from, const glm::vec3& to, float& t, glm::vec3& on_axis) {
                    const glm::vec3 axis = to - from;
                    t = glm::dot(v.position - from, axis) / std::max(glm::dot(axis, axis), 1e-6f);
                    on_axis = from + axis * t;
                    return glm::length(v.position - (from + axis * std::clamp(t, 0.0f, 1.0f)));
                };
                const Limb* best = nullptr;
                float best_distance = 1e9f;
                for (const Limb& limb : limbs) {
                    if (!limb.valid) continue;
                    float t = 0.0f;
                    glm::vec3 on;
                    const float d = std::min(along(limb.elbow, limb.wrist, t, on), along(limb.shoulder, limb.elbow, t, on));
                    if (d < best_distance) {
                        best_distance = d;
                        best = &limb;
                    }
                }
                if (!best) return;
                auto ease = [](float x) {
                    x = std::clamp(x, 0.0f, 1.0f);
                    return x * x * (3.0f - 2.0f * x);
                };
                float t = 0.0f;
                glm::vec3 on;
                along(best->elbow, best->wrist, t, on);
                if (t >= 0.0f && t < 1.12f) {
                    const float keep = (0.8f + (0.45f - 0.8f) * ease((t - 0.6f) / 0.35f)) * (1.0f - ease((t - 1.0f) / 0.12f)) + ease((t - 1.0f) / 0.12f);
                    const glm::vec3 out = v.position - on;
                    const float r = glm::length(out);
                    if (r > 0.028f) v.position = on + out / r * (0.028f + (r - 0.028f) * keep);
                    return;
                }
                along(best->shoulder, best->elbow, t, on);
                if (t >= 0.1f && t <= 1.0f) {
                    const glm::vec3 out = v.position - on;
                    const float r = glm::length(out);
                    if (r > 0.04f) v.position = on + out / r * (0.04f + (r - 0.04f) * 0.82f);
                }
            };
            struct Segment {
                glm::vec3 from{0.0f}, axis{0.0f}, u{0.0f}, w{0.0f};
                bool fore = false;
                std::array<float, 36 * 16> least{};
            };
            constexpr float kTuckT0 = -0.3f, kTuckStep = 0.05f;
            constexpr int kTuckBins = 36, kTuckAngles = 16;
            auto segment_polar = [](const Segment& s, const glm::vec3& p, float& t, float& r, int& angle) {
                t = glm::dot(p - s.from, s.axis) / std::max(glm::dot(s.axis, s.axis), 1e-6f);
                const glm::vec3 d = p - (s.from + s.axis * t);
                r = glm::length(d);
                const float a = std::atan2(glm::dot(d, s.w), glm::dot(d, s.u));
                angle = (static_cast<int>(std::floor((a + glm::pi<float>()) / glm::two_pi<float>() * kTuckAngles)) % kTuckAngles + kTuckAngles) % kTuckAngles;
            };
            std::array<std::array<Segment, 2>, 2> segments{};
            for (size_t side = 0; side < 2; ++side) {
                const Limb& limb = limbs[side];
                for (size_t k = 0; k < 2; ++k) {
                    Segment& s = segments[side][k];
                    s.from = k == 0 ? limb.shoulder : limb.elbow;
                    s.axis = (k == 0 ? limb.elbow : limb.wrist) - s.from;
                    s.fore = k == 1;
                    const glm::vec3 along = glm::normalize(s.axis + glm::vec3(1e-6f));
                    glm::vec3 up(0.0f, 1.0f, 0.0f);
                    if (std::abs(along.y) > 0.9f) up = glm::vec3(0.0f, 0.0f, 1.0f);
                    s.u = glm::normalize(up - along * glm::dot(up, along));
                    s.w = glm::cross(along, s.u);
                    s.least.fill(1e9f);
                }
            }
            if (MeshData cloth; limbs[0].valid && limbs[1].valid && game_.Models().ReadMesh(file, cloth)) {
                for (const SubMesh& sub : cloth.submeshes) {
                    if (sub.material >= full->materials.size() || full->materials[sub.material].base_color.find("plr_jaket0") == std::string::npos) continue;
                    for (uint32_t i = 0; i < sub.index_count; ++i) {
                        const size_t at = static_cast<size_t>(static_cast<int64_t>(cloth.indices[sub.first_index + i]) + sub.vertex_offset);
                        if (at >= cloth.vertices.size()) continue;
                        for (auto& pair : segments) {
                            for (Segment& s : pair) {
                                float t = 0.0f, r = 0.0f;
                                int angle = 0;
                                segment_polar(s, cloth.vertices[at].position, t, r, angle);
                                const int bin = static_cast<int>(std::floor((t - kTuckT0) / kTuckStep));
                                if (r < 0.1f && bin >= 0 && bin < kTuckBins) {
                                    float& least = s.least[static_cast<size_t>(bin * kTuckAngles + angle)];
                                    least = std::min(least, r);
                                }
                            }
                        }
                    }
                }
            }
            auto tucked = [segments, segment_polar](Vertex& v, const FmdlMaterial& material) {
                if (material.base_color.find("plr_hdam0") == std::string::npos) return;
                const Segment* best = nullptr;
                float best_distance = 1e9f;
                for (const auto& pair : segments) {
                    for (const Segment& s : pair) {
                        const float t = std::clamp(glm::dot(v.position - s.from, s.axis) / std::max(glm::dot(s.axis, s.axis), 1e-6f), 0.0f, 1.0f);
                        const float d = glm::length(v.position - (s.from + s.axis * t));
                        if (d < best_distance) {
                            best_distance = d;
                            best = &s;
                        }
                    }
                }
                if (!best || best_distance > 0.1f) return;
                float t = 0.0f, r = 0.0f;
                int angle = 0;
                segment_polar(*best, v.position, t, r, angle);
                const float x = std::clamp((t - 0.85f) / 0.1f, 0.0f, 1.0f);
                const float k = best->fore ? 1.0f - x * x * (3.0f - 2.0f * x) : 1.0f;
                const int bin = static_cast<int>(std::floor((t - kTuckT0) / kTuckStep));
                if (k <= 0.0f || bin < 0 || bin >= kTuckBins || r < 1e-4f) return;
                float least = 1e9f;
                for (int b = std::max(0, bin - 1); b <= std::min(kTuckBins - 1, bin + 1); ++b) {
                    for (int a = angle - 1; a <= angle + 1; ++a) {
                        least = std::min(least, best->least[static_cast<size_t>(b * kTuckAngles + (a + kTuckAngles) % kTuckAngles)]);
                    }
                }
                const float limit = std::max(0.03f, least - 0.009f);
                if (least > 1.0f || r <= limit) return;
                const glm::vec3 on = best->from + best->axis * t;
                v.position = on + (v.position - on) * ((r + (limit - r) * k) / r);
            };
            auto short_sleeves = [limbs](const Vertex& v, const FmdlMaterial& material) {
                if (material.base_color.find("plr_hand") != std::string::npos) return true;
                for (const Limb& limb : limbs) {
                    if (!limb.valid) continue;
                    const glm::vec3 axis = limb.wrist - limb.elbow;
                    const float t = glm::dot(v.position - limb.elbow, axis) / std::max(glm::dot(axis, axis), 1e-6f);
                    if (t > 0.95f && glm::length(v.position - (limb.elbow + axis * t)) < 0.1f) return false;
                }
                return true;
            };
            auto kept = [short_sleeves](std::function<bool(const Vertex&)> bones) {
                return [bones, short_sleeves](const Vertex& v, const FmdlMaterial& material) { return bones(v) && short_sleeves(v, material); };
            };
            auto cuffs = [limbs](Vertex& v, const FmdlMaterial& material) {
                if (material.base_color.find("plr_hand") != std::string::npos) return;
                const float end = material.base_color.find("plr_jaket0") != std::string::npos ? 0.84f : 0.89f;
                for (const Limb& limb : limbs) {
                    if (!limb.valid) continue;
                    const glm::vec3 axis = limb.wrist - limb.elbow;
                    const float t = glm::dot(v.position - limb.elbow, axis) / std::max(glm::dot(axis, axis), 1e-6f);
                    if (t > end && t < 1.2f && glm::length(v.position - (limb.elbow + axis * t)) < 0.1f) v.position -= axis * (t - end);
                }
            };
            auto fitted = [sleeves, tucked, cuffs](Vertex& v, const FmdlMaterial& material) {
                tucked(v, material);
                sleeves(v);
                cuffs(v, material);
            };
            const ModelEntry* subset = game_.Models().GetSubsetByMaterial(file, file + "#survival_right_arm", kept(keep(arm)), fitted);
            arm_mesh_ = subset ? subset->mesh.get() : nullptr;
            const ModelEntry* left_subset = game_.Models().GetSubsetByMaterial(file, file + "#survival_left_arm", kept(keep(left)), fitted);
            left_arm_mesh_ = left_subset ? left_subset->mesh.get() : nullptr;
            for (size_t w = 1; w < models_.guns.size(); ++w) {
                for (size_t side = 0; side < 2; ++side) {
                    const SurvivalGunModel& gun = models_.guns[w];
                    if (gun.press[side].empty()) continue;
                    const auto pressed = [fitted, &gun, side](Vertex& v, const FmdlMaterial& material) {
                        fitted(v, material);
                        v.position += gun.PressAt(side, v.position);
                    };
                    const ModelEntry* held = game_.Models().GetSubsetByMaterial(file, file + "#survival_arm_" + std::to_string(w) + "_" + std::to_string(side), kept(keep(side == 0 ? arm : left)), pressed);
                    held_arm_mesh_[w][side] = held ? held->mesh.get() : nullptr;
                }
            }
            if (MeshData mesh; game_.Models().ReadMesh(file, mesh)) {
                int hoodie_group = -1;
                for (const SubMesh& m : mesh.submeshes) {
                    if (m.material < full->materials.size() && full->materials[m.material].name == "fabric_no_glossy_hood_new") hoodie_group = static_cast<int>(m.group);
                }
                for (const SubMesh& m : mesh.submeshes) {
                    if (m.material >= full->materials.size() || full->materials[m.material].name != "fabric_hi_glossy_jaket" || static_cast<int>(m.group) != hoodie_group) continue;
                    for (uint32_t i = 0; i < m.index_count; ++i) {
                        const Vertex& v = mesh.vertices[static_cast<size_t>(mesh.indices[m.first_index + i]) + static_cast<size_t>(m.vertex_offset)];
                        jacket_skin_.push_back({v.position, v.joints, v.weights});
                    }
                }
                for (const SubMesh& m : mesh.submeshes) {
                    if (m.material >= full->materials.size() || static_cast<int>(m.group) != hoodie_group) continue;
                    const std::string& name = full->materials[m.material].name;
                    std::vector<glm::vec3>* list = name == "fabric_no_glossy_hood_new" ? &hoodie_lip_ : name == "fabric_low_glossy5" ? &shirt_lip_ : nullptr;
                    if (!list) continue;
                    for (uint32_t i = 0; i < m.index_count; ++i) {
                        const glm::vec3& at = mesh.vertices[static_cast<size_t>(mesh.indices[m.first_index + i]) + static_cast<size_t>(m.vertex_offset)].position;
                        if (at.y < -0.04f) list->push_back(at);
                    }
                }
            }
            const auto fit_body = [this, tucked](Vertex& v, const FmdlMaterial& material) {
                tucked(v, material);
                FitBodyVertex(v, material);
            };
            const ModelEntry* body = game_.Models().GetSubsetByMaterial(file, file + "#survival_body", [](const Vertex&, const FmdlMaterial& material) { return material.name != "fabric_low_glossy5"; }, fit_body);
            body_mesh_ = body ? body->mesh.get() : nullptr;
            const ModelEntry* shirt = game_.Models().GetSubsetByMaterial(file, file + "#survival_shirt", [](const Vertex&, const FmdlMaterial& material) { return material.name == "fabric_low_glossy5"; }, fit_body);
            shirt_mesh_ = shirt ? shirt->mesh.get() : nullptr;
            if (MeshData mesh; std::getenv("PT_SURVIVAL_HAND_DUMP") && game_.Models().ReadMesh(file, mesh)) {
                const std::array<ModelCache::MaterialKeep, 2> sides = {kept(keep(arm)), kept(keep(left))};
                const FmdlMaterial none;
                const std::set<uint64_t>& hidden_codes = game_.Demos().PlayerDefaultHidden();
                std::vector<uint8_t> hidden_group(mesh.groups.size(), 0);
                for (size_t g = 0; g < mesh.groups.size(); ++g) {
                    for (int at = static_cast<int>(g), depth = 0; at >= 0 && at < static_cast<int>(mesh.groups.size()) && depth < 64; ++depth) {
                        if (hidden_codes.contains(mesh.groups[static_cast<size_t>(at)].code)) {
                            hidden_group[g] = 1;
                            break;
                        }
                        at = mesh.groups[static_cast<size_t>(at)].parent;
                    }
                }
                for (size_t side = 0; side < 2; ++side) {
                    MeshData& dump = arm_dump_[side];
                    std::vector<int64_t> index_of(mesh.vertices.size(), -1);
                    for (const SubMesh& sub : mesh.submeshes) {
                        if (sub.group < hidden_group.size() && hidden_group[sub.group] != 0) continue;
                        const FmdlMaterial& material = sub.material < full->materials.size() ? full->materials[sub.material] : none;
                        for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                            std::array<size_t, 3> tri{};
                            bool all = true;
                            for (int k = 0; k < 3 && all; ++k) {
                                tri[static_cast<size_t>(k)] = static_cast<size_t>(static_cast<int64_t>(mesh.indices[sub.first_index + i + static_cast<uint32_t>(k)]) + sub.vertex_offset);
                                all = sides[side](mesh.vertices[tri[static_cast<size_t>(k)]], material);
                            }
                            if (!all) continue;
                            for (size_t v : tri) {
                                if (index_of[v] < 0) {
                                    index_of[v] = static_cast<int64_t>(dump.vertices.size());
                                    Vertex moved = mesh.vertices[v];
                                    fitted(moved, material);
                                    dump.vertices.push_back(moved);
                                }
                                dump.indices.push_back(static_cast<uint32_t>(index_of[v]));
                            }
                        }
                    }
                }
            }
        }
        LogInfo("survival: the first person arm {}", arm_mesh_ ? "made from the player's model" : "could not be made");
    }
}

void SurvivalNight::FitBodyVertex(Vertex& v, const FmdlMaterial& material) const {
    glm::vec3& p = v.position;
    if (static const bool plain = std::getenv("PT_BODY_NOFIT") != nullptr; plain) return;
    const bool hoodie = material.name == "fabric_no_glossy_hood_new" || material.name == "fabric_no_glossy_hdam_new";
    if (material.name == "new_hand" && std::abs(p.x) < 0.74f) {
        const float k = 0.4f * std::clamp((0.74f - std::abs(p.x)) / 0.05f, 0.0f, 1.0f);
        p.y += (0.402f - p.y) * k;
        p.z += (-0.031f - p.z) * k;
        return;
    }
    if (material.name == "skin_white_male1" && p.y < 0.455f) {
        const float k = 0.3f * std::clamp((0.455f - p.y) / 0.02f, 0.0f, 1.0f);
        p.x -= p.x * k;
        p.z += (-0.015f - p.z) * k;
        return;
    }
    const bool shirt = material.name == "fabric_low_glossy5";
    if (material.name == "fabric_no_glossy_hdam_new" && std::abs(p.x) > 0.44f && std::abs(p.x) < 0.755f) {
        const float grow = std::clamp((std::abs(p.x) - 0.44f) / 0.04f, 0.0f, 1.0f) * (1.0f - std::clamp((std::abs(p.x) - 0.74f) / 0.015f, 0.0f, 1.0f));
        const float in = 1.0f - 0.2f * grow;
        p.y = 0.402f + (p.y - 0.402f) * in;
        p.z = -0.031f + (p.z + 0.031f) * in;
    }
    if ((material.name == "fabric_hi_glossy_jaket" || material.name == "alminium_a" || material.name == "fabric_no_glossy_hdam_new") && std::abs(p.x) > 0.6f) {
        const bool left_arm = p.x > 0.0f;
        const bool gusset = material.name == "fabric_no_glossy_hdam_new";
        const float along_arm = std::min(std::abs(p.x), 0.755f);
        const float middle = glm::radians(left_arm ? -53.5f : -29.5f + 84.0f * std::max(0.0f, along_arm - 0.67f));
        const float half = std::max(0.0f, along_arm - (left_arm ? 0.667f : 0.617f)) * glm::radians(left_arm ? 325.0f : 340.0f) * 1.1f;
        if (half > 0.0f) {
            const glm::vec2 round(p.y - 0.402f, p.z + 0.031f);
            const float angle = std::atan2(round.y, round.x);
            const float off = std::remainder(angle - middle, 2.0f * kPi);
            if (gusset) {
                return;
            }
            const float closed = std::abs(off) >= half ? std::copysign((std::abs(off) - half) * kPi / (kPi - half), off) : off * 0.04f;
            const float turn = closed - off;
            const float c = std::cos(turn), s = std::sin(turn);
            const auto turned = [c, s](float& y, float& z) {
                const float ny = y * c - z * s;
                z = y * s + z * c;
                y = ny;
            };
            float y = round.x, z = round.y;
            turned(y, z);
            p.y = y + 0.402f;
            p.z = z - 0.031f;
            turned(v.normal.y, v.normal.z);
            turned(v.tangent.y, v.tangent.z);
        }
        if (material.name == "alminium_a" || gusset) return;
    }
    const auto inside = [&p](const std::vector<glm::vec3>& over, float margin, float reach) {
        const glm::vec2 from(p.x, p.z - 0.01f);
        const float at = glm::length(from);
        if (at < 0.05f) return;
        float limit = 1.0e9f;
        for (const glm::vec3& q : over) {
            if (std::abs(q.y - p.y) > reach) continue;
            const glm::vec2 other(q.x, q.z - 0.01f);
            const float r = glm::length(other);
            if (r > 0.05f && glm::dot(other / r, from / at) > 0.985f) limit = std::min(limit, r);
        }
        if (limit < 1.0e8f && at > limit - margin) {
            const glm::vec2 moved = from * ((limit - margin) / at);
            p.x = moved.x;
            p.z = moved.y + 0.01f;
        }
    };
    if (shirt && p.y < -0.08f) inside(hoodie_lip_, 0.006f, 0.02f);
    if (material.name == "fabric_no_glossy" && p.y > -0.18f) inside(shirt_lip_, 0.012f, 0.02f);
    if (!hoodie && !shirt && material.name != "fabric_hi_glossy_jaket") return;
    if (const float y = p.y - 0.44f; y > 0.0f) {
        const float across = std::clamp((0.16f - std::abs(p.x)) / 0.06f, 0.0f, 1.0f);
        const float behind = std::clamp((-0.065f - p.z) / 0.02f, 0.0f, 1.0f);
        p.y -= 0.55f * y * across * across * (3.0f - 2.0f * across) * behind * behind * (3.0f - 2.0f * behind);
    }
    if (!hoodie && !shirt && p.y < -0.03f) {
        const float flare = std::clamp((-0.03f - p.y) / 0.05f, 0.0f, 1.0f);
        const glm::vec2 from(p.x, p.z - 0.01f);
        if (const float at = glm::length(from); at > 0.05f) {
            const glm::vec2 wider = from * (1.0f + 0.012f * flare * flare * (3.0f - 2.0f * flare) / at);
            p.x = wider.x;
            p.z = wider.y + 0.01f;
        }
    }
    if ((!hoodie && !shirt) || p.y > 0.34f) return;
    const JacketSkin* over = nullptr;
    float nearest = 0.05f * 0.05f;
    for (const JacketSkin& leather : jacket_skin_) {
        const glm::vec3 off = leather.place - p;
        if (const float d = glm::dot(off, off); d < nearest) {
            nearest = d;
            over = &leather;
        }
    }
    if (!over) return;
    const auto smooth = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    };
    const float covered = smooth((0.05f - std::sqrt(nearest)) / 0.03f) * smooth((0.34f - p.y) / 0.06f);
    const float share = smooth((p.y + (shirt ? 0.16f : 0.15f)) / 0.06f) * covered;
    std::array<float, 256> sum{};
    for (int i = 0; i < 4; ++i) {
        sum[v.joints[i]] += static_cast<float>(v.weights[i]) * (1.0f - share);
        sum[over->joints[i]] += static_cast<float>(over->weights[i]) * share;
    }
    glm::u8vec4 joints(0);
    glm::vec4 weights(0.0f);
    for (int i = 0; i < 4; ++i) {
        const size_t top = static_cast<size_t>(std::max_element(sum.begin(), sum.end()) - sum.begin());
        joints[i] = static_cast<uint8_t>(top);
        weights[i] = sum[top];
        sum[top] = 0.0f;
    }
    const float total = weights.x + weights.y + weights.z + weights.w;
    int left = 255;
    for (int i = 0; i < 4; ++i) {
        const int w = i < 3 ? static_cast<int>(weights[i] * 255.0f / total + 0.5f) : left;
        v.joints[i] = joints[i];
        v.weights[i] = static_cast<uint8_t>(std::clamp(w, 0, left));
        left -= v.weights[i];
    }
    const glm::vec2 out(p.x, p.z - 0.01f);
    const float l = glm::length(out);
    if (l < 0.05f) return;
    const float in = 0.012f * (shirt ? smooth((p.y + 0.145f) / 0.03f) : smooth((p.y + 0.09f) / 0.04f)) * covered;
    const glm::vec2 moved = out * (1.0f - in / l);
    p.x = moved.x;
    p.z = moved.y + 0.01f;
}

const GpuMesh* SurvivalNight::FittedBody() {
    if (!active_) return nullptr;
    MakeArms();
    return body_mesh_;
}

void SurvivalNight::AddArmDraws(std::vector<DrawItem>& out, const Camera& camera, const glm::mat4& gun) {
    if (!arm_mesh_ || hud_.dead) return;
    Player& player = game_.GetPlayer();
    const glm::mat4 body = player.BodyTransform();
    const glm::vec3 eye_model(glm::inverse(body) * glm::vec4(player.Eye(), 1.0f));
    constexpr glm::vec3 kShoulders(-0.003f, -0.216f, -0.034f);
    const float stance = SurvivalLongGun(weapon_) ? Smooth(aim_) : 0.0f;
    const glm::vec3 shoulders = kShoulders + glm::vec3(0.0f, 0.0f, 0.08f * stance);
    const float stance_twist = 0.45f * stance;
    const glm::vec3 shift = camera.Right() * arm_shift_.x + camera.Up() * arm_shift_.y + camera.Forward() * arm_shift_.z;
    const glm::vec3 pivot = camera.position + camera.Right() * kShoulders.x + camera.Up() * kShoulders.y + camera.Forward() * kShoulders.z;
    const glm::mat4 draw = glm::translate(glm::mat4(1.0f), pivot) * glm::rotate(glm::mat4(1.0f), arm_twist_, glm::vec3(0, 1, 0)) *
                           glm::translate(glm::mat4(1.0f), camera.position - shift - pivot) *
                           glm::rotate(glm::mat4(1.0f), camera.yaw + kPi, glm::vec3(0, 1, 0)) * glm::translate(glm::mat4(1.0f), -eye_model);
    const std::optional<glm::mat4> keep_target = player.right_hand_target;
    const float keep_grip = player.right_grip;
    glm::mat4 hand_gun = gun;
    float stylized_release = 0.0f;
    if (reload_timer_ > 0.0f && reload_length_ > 0.05f) {
        const ReloadStyle rstyle = ActiveReloadStyle();
        const float p = std::clamp(1.0f - reload_timer_ / reload_length_, 0.0f, 1.0f);
        if (rstyle == ReloadStyle::Gunslinger) {
            const float spin_u = Smooth(std::clamp((p - 0.64f) / 0.32f, 0.0f, 1.0f));
            const float lift = std::sin(spin_u * kPi);
            stylized_release = lift;
            if (lift > 0.001f) {
                const float angle = spin_u * 2.0f * kPi;
                const bool lg = SurvivalLongGun(weapon_);
                const glm::mat4 undo_rot = glm::mat4_cast(glm::angleAxis(-(lg ? 0.16f : 0.22f) * lift, glm::vec3(0, 0, 1)) * glm::angleAxis(-angle, glm::vec3(1, 0, 0)));
                hand_gun = gun * undo_rot;
                hand_gun[3] -= glm::vec4(camera.Up() * ((lg ? 0.065f : 0.055f) * lift) + camera.Forward() * ((lg ? 0.115f : 0.045f) * lift), 0.0f);
            }
        } else if (rstyle == ReloadStyle::Telekinetic) {
            const float hover = Smooth((p - 0.06f) / 0.14f) * Smooth((0.94f - p) / 0.14f);
            stylized_release = hover;
            if (hover > 0.001f) {
                const bool lg = SurvivalLongGun(weapon_);
                const glm::mat4 undo_rot = glm::mat4_cast(glm::angleAxis(-(lg ? 0.18f : 0.32f) * std::sin(p * 2.0f * kPi) * hover, glm::vec3(0, 0, 1)) *
                                                          glm::angleAxis((lg ? 0.10f : 0.16f) * std::cos(p * 2.0f * kPi) * hover, glm::vec3(1, 0, 0)) *
                                                          glm::angleAxis(-(lg ? 0.24f : 0.42f) * std::sin(p * kPi) * hover, glm::vec3(0, 1, 0)));
                hand_gun = gun * undo_rot;
                hand_gun[3] -= glm::vec4(camera.Up() * ((lg ? 0.19f : 0.14f) * hover) + camera.Forward() * ((lg ? 0.085f : 0.105f) * hover), 0.0f);
            }
        }
    }
    player.right_hand_target = glm::inverse(draw) * hand_gun * GripOnGun();
    player.right_grip = 1.0f;
    glm::mat4 swing(1.0f);
    const float on_pipe = PipeSwingFrame(camera, swing) ? PipeHold() : 0.0f;
    if (on_pipe > 0.0f) {
        const glm::mat4 to = swing * GripHand(models_.guns[static_cast<size_t>(SurvivalWeapon::Handgun)]);
        const glm::mat4 from = weapon_ == SurvivalWeapon::Flashlight ? to : gun * GripOnGun();
        const float k = Smooth(on_pipe);
        glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(from)), glm::quat_cast(glm::mat3(to)), k));
        m[3] = glm::vec4(glm::mix(glm::vec3(from[3]), glm::vec3(to[3]), k), 1.0f);
        player.right_hand_target = glm::inverse(draw) * m;
    }
    const bool right_shown = weapon_ != SurvivalWeapon::Flashlight || on_pipe > 0.0f;
    glm::mat4 thrown(1.0f);
    const float off = left_arm_mesh_ ? ThrowHand(camera, thrown) : 0.0f;
    glm::mat4 click_hand(1.0f);
    const float click = (left_arm_mesh_ && off <= 0.0f) ? FlashClickHand1P(camera, click_hand) : 0.0f;
    glm::mat4 on_gun(1.0f);
    const float left = left_arm_mesh_ ? LeftHandOnGun(on_gun) : 0.0f;
    const PlayerBody::HandReach reach = [&](const glm::mat4&, glm::mat4& target_model) {
        glm::mat4 rest(1.0f);
        rest[0] = glm::vec4(camera.Forward(), 0.0f);
        rest[1] = glm::vec4(-camera.Up(), 0.0f);
        rest[2] = glm::vec4(glm::cross(camera.Forward(), -camera.Up()), 0.0f);
        rest[3] = glm::vec4(camera.position - camera.Right() * 0.3f - camera.Up() * 0.5f + camera.Forward() * 0.1f, 1.0f);
        const glm::mat4 held = hand_gun * on_gun;
        const glm::quat turn = glm::slerp(glm::quat_cast(glm::mat3(rest)), glm::quat_cast(glm::mat3(held)), Smooth(left));
        glm::mat4 world = glm::mat4_cast(turn);
        world[3] = glm::vec4(glm::mix(glm::vec3(rest[3]), glm::vec3(held[3]), Smooth(left)), 1.0f);
        if (off > 0.0f) {
            const float k = Smooth(off);
            glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(world)), glm::quat_cast(glm::mat3(thrown)), k));
            m[3] = glm::vec4(glm::mix(glm::vec3(world[3]), glm::vec3(thrown[3]), k), 1.0f);
            world = m;
        } else if (click > 0.0f) {
            const float arc = 4.0f * click * (1.0f - click);
            glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(world)), glm::quat_cast(glm::mat3(click_hand)), click));
            m[3] = glm::vec4(glm::mix(glm::vec3(world[3]), glm::vec3(click_hand[3]), click) + (-camera.Right() * 0.035f + camera.Forward() * 0.035f) * arc, 1.0f);
            world = m;
        }
        target_model = glm::inverse(draw) * world;
        return true;
    };
    const float keep_left = player.left_grip;
    const bool left_used = left > 0.0f || off > 0.0f || click > 0.0f;
    player.left_grip = left_used ? 1.0f : 0.0f;
    const SurvivalGunModel& held = models_.guns[static_cast<size_t>(weapon_)];
    std::array<glm::vec4, 5> right_fingers = on_pipe > 0.0f ? PipeFist() : HeldFingers(held);
    if (stylized_release > 0.01f && on_pipe <= 0.0f) {
        for (auto& f : right_fingers) f = glm::mix(f, glm::vec4(0.0f, 0.12f, 0.15f, 0.08f), stylized_release);
    }
    player.SetFingerShapes(off > 0.5f || (off > 0.0f && left <= 0.0f) ? std::optional(ThrowFingers())
                           : click > 0.15f || (click > 0.0f && left <= 0.0f) ? std::optional(FlashClickFingers())
                           : left > 0.0f                                     ? std::optional(LeftFingers(held))
                                                                             : std::nullopt,
                           right_fingers);
    const glm::mat3 to_model = glm::inverse(glm::mat3(draw));
    const float spread = 0.6f + 0.6f * aim_;
    const float down = 1.0f + 0.8f * aim_;
    ReloadPose reload_pose;
    const float left_spread = spread * (ReloadNow(reload_pose) ? glm::mix(1.0f, reload_pose.elbow, reload_pose.hand) : 1.0f);
    glm::vec3 right_elbow = camera.Right() * spread - camera.Up() * down;
    if (on_pipe > 0.0f) {
        const float p = Smooth((melee_timer_ - 0.12f) / 0.2f);
        right_elbow = glm::mix(right_elbow, camera.Right() * 0.9f + camera.Up() * (0.25f - 1.25f * p) - camera.Forward() * 0.2f, Smooth(on_pipe));
    }
    glm::vec3 left_elbow_view = -camera.Right() * left_spread - camera.Up() * down;
    if (click > 0.0f) {
        left_elbow_view = glm::mix(left_elbow_view, -camera.Right() * 0.92f - camera.Up() * 1.15f + camera.Forward() * 0.15f, click);
    }
    player.SetElbowHints(to_model * left_elbow_view, to_model * right_elbow);
    const float straight = SightsOf(weapon_).straight_wrist * (SurvivalLongGun(weapon_) ? 1.0f : 1.0f - Smooth(aim_));
    player.SetStraightWrists(click > 0.0f ? 0.0f : straight, on_pipe > 0.0f ? 0.0f : straight);
    if (weapon_ != SurvivalWeapon::Handgun && weapon_ != SurvivalWeapon::Flashlight) {
        PlayerBody::WristLimits left_lim = kLeftWrist;
        left_lim.on = click <= 0.0f;
        PlayerBody::WristLimits right = kRightWrist;
        right.little = glm::mix(kRightWristHeld, right.little, Smooth(aim_));
        right.on = on_pipe <= 0.0f;
        player.SetWristLimits(left_lim, right);
    }
    const std::shared_ptr<const anim::HelpBones> help = game_.Demos().PlayerHelpBones();
    player.SetReachEase(false);
    const bool posed = player.BodySkin(help.get(), arm_skin_, false, nullptr, left_used ? &reach : nullptr);
    player.SetStraightWrists(0.0f, 0.0f);
    player.SetWristLimits({}, {});
    player.SetElbowHints(std::nullopt, std::nullopt);
    player.SetFingerShapes(std::nullopt, GripFingers());
    player.right_hand_target = keep_target;
    player.right_grip = keep_grip;
    player.left_grip = keep_left;
    if (!posed) return;
    if (glm::mat4 l(1.0f), r(1.0f); player.BoneModel("SKL_011_LUARM", l) && player.BoneModel("SKL_021_RUARM", r)) {
        const glm::vec3 lw(draw * (l[3] - glm::vec4(player.ShoulderMoved(false), 0.0f))), rw(draw * (r[3] - glm::vec4(player.ShoulderMoved(true), 0.0f)));
        const glm::vec3 mid = 0.5f * (lw + rw) - camera.position;
        const glm::vec3 seen(glm::dot(mid, camera.Right()), glm::dot(mid, camera.Up()), glm::dot(mid, camera.Forward()));
        arm_shift_ = glm::clamp(arm_shift_ + seen - shoulders, glm::vec3(-0.15f), glm::vec3(0.15f));
        const glm::vec3 across = rw - lw;
        const glm::vec3 right = glm::normalize(glm::vec3(camera.Right().x, 0.0f, camera.Right().z));
        const glm::vec3 ahead = glm::normalize(glm::vec3(camera.Forward().x, 0.0f, camera.Forward().z));
        arm_twist_ = std::clamp(arm_twist_ + std::atan2(-glm::dot(across, ahead), glm::dot(across, right)) - stance_twist, -0.8f, 0.8f);
    }
    static const bool arm_log = std::getenv("PT_SURVIVAL_ARM_LOG") != nullptr;
    static const bool arm_log_every = arm_log && std::string_view(std::getenv("PT_SURVIVAL_ARM_LOG")) == "every";
    if (arm_log && (arm_log_every || game_.Frame() % 30 == 0)) {
        auto view_space = [&](const glm::vec3& w) {
            const glm::vec3 d = w - camera.position;
            return glm::vec3(glm::dot(d, camera.Right()), glm::dot(d, camera.Up()), glm::dot(d, camera.Forward()));
        };
        glm::mat4 bone(1.0f);
        const glm::vec3 target = view_space(glm::vec3(gun * GripOnGun()[3]));
        glm::vec3 hand(0.0f), elbow(0.0f), shoulder(0.0f);
        if (player.BoneModel("SKL_023_RHAND", bone)) hand = view_space(glm::vec3(draw * bone[3]));
        if (player.BoneModel("SKL_022_RFARM", bone)) elbow = view_space(glm::vec3(draw * bone[3]));
        if (player.BoneModel("SKL_021_RUARM", bone)) shoulder = view_space(glm::vec3(draw * bone[3]));
        glm::vec3 flash, beam;
        game_.HandyBeam(flash, beam);
        const glm::vec3 f = view_space(flash);
        const glm::vec3 b(glm::dot(beam, camera.Right()), glm::dot(beam, camera.Up()), glm::dot(beam, camera.Forward()));
        if (glm::mat4 fore(1.0f), wrist(1.0f); player.BoneModel("SKL_022_RFARM", fore) && player.BoneModel("SKL_023_RHAND", wrist)) {
            const glm::vec3 along = glm::normalize(glm::vec3(wrist[3] - fore[3]));
            const glm::vec3 in_hand = glm::normalize(glm::transpose(glm::mat3(wrist)) * along);
            LogInfo("survival wrist: {:.1f} deg palm {:.1f} side {:.1f} aim {:.2f} reload {:.2f} tilt {:.2f}", glm::degrees(std::acos(std::clamp(glm::dot(along, -glm::normalize(glm::vec3(wrist[0]))), -1.0f, 1.0f))),
                    glm::degrees(std::atan2(in_hand.y, -in_hand.x)), glm::degrees(std::atan2(in_hand.z, -in_hand.x)), aim_, reload_timer_, gun_.tilt);
        }
        if (glm::mat4 fore(1.0f), wrist(1.0f); left_used && player.BoneModel("SKL_012_LFARM", fore) && player.BoneModel("SKL_013_LHAND", wrist)) {
            const glm::vec3 in_hand = glm::normalize(glm::transpose(glm::mat3(wrist)) * glm::normalize(glm::vec3(wrist[3] - fore[3])));
            LogInfo("survival left wrist: palm {:.1f} side {:.1f} aim {:.2f} sights {:.2f} reload {:.2f}", glm::degrees(std::atan2(in_hand.y, in_hand.x)), glm::degrees(std::atan2(in_hand.z, in_hand.x)), aim_, sights_w_,
                    reload_timer_);
        }
        LogInfo("survival arm: target ({:.3f} {:.3f} {:.3f}) hand ({:.3f} {:.3f} {:.3f}) elbow ({:.3f} {:.3f} {:.3f}) shoulder ({:.3f} {:.3f} {:.3f}) "
                "eye model ({:.3f} {:.3f} {:.3f}) flash ({:.3f} {:.3f} {:.3f}) beam ({:.3f} {:.3f} {:.3f})",
                target.x, target.y, target.z, hand.x, hand.y, hand.z, elbow.x, elbow.y, elbow.z, shoulder.x, shoulder.y, shoulder.z, eye_model.x,
                eye_model.y, eye_model.z, f.x, f.y, f.z, b.x, b.y, b.z);
        glm::vec3 left_hand(0.0f), left_shoulder(0.0f);
        if (player.BoneModel("SKL_013_LHAND", bone)) left_hand = view_space(glm::vec3(draw * bone[3]));
        if (player.BoneModel("SKL_011_LUARM", bone)) left_shoulder = view_space(glm::vec3(draw * bone[3]));
        const glm::vec3 left_target = view_space(glm::vec3((gun * on_gun)[3]));
        LogInfo("survival arm: left {:.2f} target ({:.3f} {:.3f} {:.3f}) hand ({:.3f} {:.3f} {:.3f}) off {:.3f} shoulder ({:.3f} {:.3f} {:.3f}) reach {:.3f} "
                "aim {:.2f} speed {:.2f} shift ({:.3f} {:.3f} {:.3f}) twist {:.3f} right shoulder z {:.3f}",
                left, left_target.x, left_target.y, left_target.z, left_hand.x, left_hand.y, left_hand.z, glm::distance(left_hand, left_target),
                left_shoulder.x, left_shoulder.y, left_shoulder.z, glm::distance(left_shoulder, left_target), aim_, speed_scale_, arm_shift_.x,
                arm_shift_.y, arm_shift_.z, arm_twist_, shoulder.z);
    }
    static const char* const dump_folder = std::getenv("PT_SURVIVAL_HAND_DUMP");
    const std::string reload_state = reload_timer_ > 0.0f    ? "r" + std::to_string(static_cast<int>(gun_.reload_at * 1000.0f))
                                     : melee_timer_ >= 0.0f ? "r" + std::to_string(2000 + static_cast<int>(melee_timer_ * 1000.0f))
                                                            : std::string();
    if (const char* state = !reload_state.empty() ? reload_state.c_str() : aim_ > 0.98f ? (sights_w_ > 0.98f ? "sights" : "ads") : aim_ < 0.02f ? "hip" : nullptr;
        dump_folder && state && game_.Frame() % (reload_state.empty() ? 30 : 2) == 0) {
        const glm::mat4 to_gun = glm::inverse(gun) * draw;
        const std::string stem = std::string(dump_folder) + "/hand_" + std::to_string(static_cast<int>(weapon_)) + "_" + state;
        if (std::FILE* parts = std::fopen((stem + ".parts").c_str(), "w")) {
            ReloadPose pose;
            const bool keyed = ReloadNow(pose);
            std::fprintf(parts, "at %.4f swing %.4f open %.4f left %.4f hand %.4f away %.4f item %.4f seat %.4f spent %.4f\n", gun_.reload_at, gun_.swing, gun_.open, left,
                         keyed ? pose.hand : 0.0f, keyed ? pose.away : 0.0f, keyed ? pose.item : 0.0f, keyed ? pose.seat : 0.0f, keyed ? pose.spent : 0.0f);
            const glm::mat4 from_gun = glm::inverse(gun);
            const glm::vec3 eye(from_gun * glm::vec4(camera.position, 1.0f));
            const glm::vec3 ahead = glm::mat3(from_gun) * camera.Forward(), above = glm::mat3(from_gun) * camera.Up();
            std::fprintf(parts, "eye %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f\n", eye.x, eye.y, eye.z, ahead.x, ahead.y, ahead.z, above.x, above.y, above.z, camera.fov_y);
            std::vector<DrawItem> items;
            if (glm::mat4 bone(1.0f); player.BoneModel("SKL_013_LHAND", bone)) {
                AddReloadItems(items, gun, draw * bone, true);
                const glm::mat4 h = to_gun * bone;
                std::fprintf(parts, "hand");
                for (int c = 0; c < 4; ++c) std::fprintf(parts, " %.6f %.6f %.6f %.6f", h[c].x, h[c].y, h[c].z, h[c].w);
                std::fprintf(parts, "\n");
            }
            for (const DrawItem& carried : items) {
                const glm::mat4 m = glm::inverse(gun) * carried.transform;
                std::fprintf(parts, "item");
                for (int c = 0; c < 4; ++c) std::fprintf(parts, " %.6f %.6f %.6f %.6f", m[c].x, m[c].y, m[c].z, m[c].w);
                std::fprintf(parts, "\n");
            }
            std::fclose(parts);
        }
        std::FILE* obj = std::fopen((stem + ".obj").c_str(), "w");
        std::FILE* bones = std::fopen((stem + ".bones").c_str(), "w");
        const std::vector<std::string>& names = game_.Models().Get(game_.Demos().PlayerModelFile())->bone_names;
        size_t base = 1;
        for (size_t side = 0; side < (left > 0.02f ? 2u : 1u) && obj && bones; ++side) {
            std::fprintf(obj, "o %s\n", side == 0 ? "right" : "left");
            for (const Vertex& v : arm_dump_[side].vertices) {
                glm::mat4 skin(0.0f);
                float total = 0.0f;
                int best = 0;
                for (int k = 0; k < 4; ++k) {
                    const float w = v.weights[k] / 255.0f;
                    if (w <= 0.0f || v.joints[k] >= arm_skin_.size()) continue;
                    skin += arm_skin_[v.joints[k]] * w;
                    total += w;
                    if (v.weights[k] > v.weights[best]) best = k;
                }
                const glm::vec3 place = v.position + models_.guns[static_cast<size_t>(weapon_)].PressAt(side, v.position);
                const glm::vec3 p = total > 0.0f ? glm::vec3(to_gun * (skin / total) * glm::vec4(place, 1.0f)) : glm::vec3(0.0f);
                std::fprintf(obj, "v %.5f %.5f %.5f\n", p.x, p.y, p.z);
                std::fprintf(bones, "%s\n", v.joints[best] < names.size() ? names[v.joints[best]].c_str() : "?");
            }
            for (size_t i = 0; i + 2 < arm_dump_[side].indices.size(); i += 3) {
                std::fprintf(obj, "f %zu %zu %zu\n", base + arm_dump_[side].indices[i], base + arm_dump_[side].indices[i + 1], base + arm_dump_[side].indices[i + 2]);
            }
            base += arm_dump_[side].vertices.size();
        }
        if (obj) std::fclose(obj);
        if (bones) std::fclose(bones);
        const ModelEntry* rig = game_.Models().Get(game_.Demos().PlayerModelFile());
        if (std::FILE* f = std::fopen((stem + ".rig").c_str(), "w"); f && rig) {
            for (size_t b = 0; b < rig->bone_names.size() && b < arm_skin_.size(); ++b) {
                glm::mat4 bone(1.0f);
                if (!player.BoneModel(rig->bone_names[b].c_str(), bone)) continue;
                const glm::mat4 world = to_gun * bone;
                const glm::mat4 inverse_bind = glm::inverse(bone) * arm_skin_[b];
                std::fprintf(f, "bone %zu %s %d", b, rig->bone_names[b].c_str(), b < rig->bone_parents.size() ? rig->bone_parents[b] : -1);
                for (int c = 0; c < 4; ++c) std::fprintf(f, " %.6f %.6f %.6f %.6f", world[c].x, world[c].y, world[c].z, world[c].w);
                for (int c = 0; c < 4; ++c) std::fprintf(f, " %.6f %.6f %.6f %.6f", inverse_bind[c].x, inverse_bind[c].y, inverse_bind[c].z, inverse_bind[c].w);
                std::fprintf(f, "\n");
            }
            for (size_t side = 0; side < 2; ++side) {
                for (const Vertex& v : arm_dump_[side].vertices) {
                    std::fprintf(f, "vertex %zu %.5f %.5f %.5f %d %d %d %d %d %d %d %d\n", side, v.position.x, v.position.y, v.position.z, v.joints[0],
                                 v.joints[1], v.joints[2], v.joints[3], v.weights[0], v.weights[1], v.weights[2], v.weights[3]);
                }
            }
            for (size_t side = 0; side < 2; ++side) {
                for (size_t i = 0; i + 2 < arm_dump_[side].indices.size(); i += 3) {
                    std::fprintf(f, "face %zu %u %u %u\n", side, arm_dump_[side].indices[i], arm_dump_[side].indices[i + 1], arm_dump_[side].indices[i + 2]);
                }
            }
            std::fclose(f);
        }
        if (std::FILE* joints = std::fopen((stem + ".joints").c_str(), "w")) {
            for (size_t b = 0; b < names.size(); ++b) {
                const std::string& n = names[b];
                const bool finger = n.size() >= 12 && (n[8] == 'R' || n[8] == 'L') && n[9] == 'F';
                if (!finger && n.find("HAND") == std::string::npos && n.find("FARM") == std::string::npos) continue;
                glm::mat4 bone(1.0f);
                if (!player.BoneModel(n.c_str(), bone)) continue;
                const glm::mat4 g = to_gun * bone;
                std::fprintf(joints, "%s %.5f %.5f %.5f  x %.3f %.3f %.3f  y %.3f %.3f %.3f  z %.3f %.3f %.3f\n", n.c_str(), g[3].x, g[3].y, g[3].z,
                             g[0].x, g[0].y, g[0].z, g[1].x, g[1].y, g[1].z, g[2].x, g[2].y, g[2].z);
            }
            std::fclose(joints);
        }
    }
    if (glm::mat4 bone(1.0f); player.BoneModel("SKL_013_LHAND", bone)) AddReloadItems(out, gun, draw * bone, true);
    hand_flare_lit_ = false;
    if (glm::mat4 bone(1.0f); off > 0.0f && !thrown_ && models_.drawn.flare && player.BoneModel("SKL_013_LHAND", bone)) {
        const glm::mat4 flare = draw * bone * FlareInHand();
        DrawItem f;
        f.mesh = models_.drawn.flare;
        f.transform = flare;
        f.hidden_views = 2;
        f.handy_shadowless = true;
        f.light_fitting = kHandyLightFitting;
        f.view_attached = true;
        f.source = kSourceBase + 0x50008u;
        out.push_back(f);
        if (throw_timer_ >= 0.1f && models_.flare_tip) {
            f.mesh = models_.flare_tip.get();
            f.source = kSourceBase + 0x50009u;
            out.push_back(f);
            hand_flare_at_ = glm::vec3(flare * glm::vec4(0.0f, 0.0f, 0.16f, 1.0f));
            hand_flare_lit_ = true;
        }
    }
    const std::array<const GpuMesh*, 2>& held_arms = held_arm_mesh_[static_cast<size_t>(weapon_)];
    DrawItem item;
    item.mesh = held_arms[0] ? held_arms[0] : arm_mesh_;
    item.transform = draw;
    item.skin = arm_skin_;
    item.hidden_views = 2;
    item.handy_shadowless = true;
    item.light_fitting = kHandyLightFitting;
    item.view_attached = true;
    item.source = kSourceBase + 0x50000u;
    item.hidden_meshes = HiddenMeshes(*item.mesh, game_.Demos().PlayerDefaultHidden());
    if (right_shown) out.push_back(item);
    if (left > 0.02f || off > 0.02f || click > 0.02f) {
        item.mesh = (click > 0.02f || off > 0.02f || !held_arms[1]) ? left_arm_mesh_ : held_arms[1];
        item.hidden_meshes = HiddenMeshes(*item.mesh, game_.Demos().PlayerDefaultHidden());
        item.source = kSourceBase + 0x50001u;
        out.push_back(item);
    }
}

glm::mat4 SurvivalNight::GripOnGun() const {
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    if (weapon_ != SurvivalWeapon::Lever || gun_.lever <= 0.0f) return GripHand(model);
    return glm::translate(glm::mat4(1.0f), model.lever_pivot) * Rot(-0.9f * gun_.lever, {1, 0, 0}) * glm::translate(glm::mat4(1.0f), -model.lever_pivot) *
           GripHand(model);
}

glm::mat4 SurvivalNight::BarrelsTurn() const {
    if (weapon_ != SurvivalWeapon::Double || gun_.open <= 0.0f) return glm::mat4(1.0f);
    const glm::vec3 hinge = models_.guns[static_cast<size_t>(weapon_)].hinge;
    return glm::translate(glm::mat4(1.0f), hinge) * Rot(0.6f * gun_.open, {1, 0, 0}) * glm::translate(glm::mat4(1.0f), -hinge);
}

float SurvivalNight::LeftHandOnGun(glm::mat4& hand) const {
    glm::mat4 under(1.0f);
    under[0] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    under[1] = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
    under[2] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    const float away = std::max({BodyHoldsGun() ? 0.0f : SprintWeight(), Smooth(swap_), hud_.dead ? 1.0f : 0.0f});
    glm::mat4 support(1.0f);
    support[0] = glm::vec4(0.0f, -0.42f, 0.91f, 0.0f);
    support[1] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    support[2] = glm::vec4(glm::cross(glm::vec3(support[0]), glm::vec3(support[1])), 0.0f);
    const float both = 1.0f - away;
    ReloadPose pose;
    const bool keyed = ReloadNow(pose);
    const float raised = 1.0f;
    auto reload_hand = [&](const glm::mat4& hold) {
        if (!keyed || pose.hand <= 0.0f) return hold;
        glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(hold)), pose.left_turn, pose.hand));
        m[3] = glm::vec4(glm::mix(glm::vec3(hold[3]), pose.left, pose.hand), 1.0f);
        return m;
    };
    switch (weapon_) {
    case SurvivalWeapon::Handgun: {
        hand = reload_hand(glm::translate(glm::mat4(1.0f), model.support) * TurnXyz(model.support_turn) * support);
        return both * (keyed ? (1.0f - pose.away) * std::max(raised, reload_front_ > 0.6f ? 1.0f : 0.0f) : raised);
    }
    case SurvivalWeapon::Shotgun:
    case SurvivalWeapon::Lever:
    case SurvivalWeapon::Double: {
        const glm::vec3 pump = model.pump_grip - glm::vec3(0.0f, 0.0f, model.pump_travel * gun_.slide);
        hand = reload_hand(BarrelsTurn() * glm::translate(glm::mat4(1.0f), pump + model.support) * TurnXyz(model.support_turn) * under);
        return both * (keyed ? 1.0f - pose.away : 1.0f);
    }
    case SurvivalWeapon::Magnum:
        hand = reload_hand(glm::translate(glm::mat4(1.0f), model.support) * TurnXyz(model.support_turn) * support);
        return both * (keyed ? (1.0f - pose.away) * std::max(raised, reload_front_ > 0.6f ? 1.0f : 0.0f) : raised);
    default: return 0.0f;
    }
}

glm::vec4 SurvivalNight::RunBob() const {
    return run_stride::Bob(bob_phase_, RunWeight() * bob_weight_);
}

void SurvivalNight::DeathCamera(Camera& camera) const {
    if (!active_ || !hud_.dead) return;
    if (game_.ThirdPersonCameraActive()) {
        if (player_ragdoll_.Active()) {
            const float k = Smooth(hud_.dead_time / 1.35f);
            const glm::vec3 chest = player_ragdoll_.ChestPosition();
            const glm::vec3 back = -camera.Forward();
            const glm::vec3 flat_back = glm::length(glm::vec2(back.x, back.z)) > 1e-3f
                                            ? glm::normalize(glm::vec3(back.x, 0.0f, back.z))
                                            : glm::vec3(0.0f, 0.0f, -1.0f);
            glm::vec3 target_pos = chest + flat_back * 2.15f + camera.Right() * 0.45f + glm::vec3(0.0f, 1.05f, 0.0f);
            const glm::vec3 from = chest + glm::vec3(0.0f, 0.25f, 0.0f);
            const glm::vec3 delta = target_pos - from;
            const float dist = glm::length(delta);
            if (dist > 0.25f) {
                RayHit hit;
                if (game_.Collision().Raycast(from, delta / dist, dist + 0.15f, hit)) {
                    target_pos = from + (delta / dist) * std::max(0.45f, hit.distance - 0.18f);
                }
            }
            camera.position = glm::mix(camera.position, target_pos, k * 0.72f);
            const glm::vec3 look = glm::normalize((chest + glm::vec3(0.0f, 0.08f, 0.0f)) - camera.position);
            const float want_yaw = std::atan2(-look.x, -look.z);
            const float want_pitch = std::asin(std::clamp(look.y, -1.0f, 1.0f));
            float dyaw = want_yaw - camera.yaw;
            while (dyaw > kPi) dyaw -= 2.0f * kPi;
            while (dyaw < -kPi) dyaw += 2.0f * kPi;
            camera.yaw += dyaw * k * 0.65f;
            camera.pitch = glm::mix(camera.pitch, want_pitch, k * 0.75f);
        }
        return;
    }
    const float k = dead_fall_ * dead_fall_;
    const float bounce = dead_fall_ >= 1.0f ? std::exp(-(hud_.dead_time - 0.9f) * 6.0f) * std::sin((hud_.dead_time - 0.9f) * 22.0f) * 0.03f : 0.0f;
    const glm::vec3 fall_dir(-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw));
    float slide = 0.35f * k;
    if (slide > 0.01f) {
        RayHit hit;
        if (game_.Collision().Raycast(dead_eye_, fall_dir, slide + 0.24f, hit)) {
            slide = std::max(0.0f, std::min(slide, hit.distance - 0.24f));
        }
    }
    camera.position.x = dead_eye_.x + fall_dir.x * slide;
    camera.position.z = dead_eye_.z + fall_dir.z * slide;
    const float local_ground = GroundAt(camera.position, dead_ground_);
    const float ground = std::max(dead_ground_, local_ground) + 0.20f;
    camera.position.y = glm::mix(dead_eye_.y, ground, k) + std::max(0.0f, bounce);
    camera.roll += 1.3f * k + bounce * 2.0f;
    camera.pitch = glm::mix(camera.pitch, 0.08f, k);
}

void SurvivalNight::ClampCameraClear(Camera& camera) const {
    if (!active_) return;
    const CollisionWorld& col = game_.Collision();
    auto enforce_floor = [&]() {
        const glm::vec3 fwd = camera.Forward();
        glm::vec3 fwd_xz(fwd.x, 0.0f, fwd.z);
        fwd_xz = glm::length(fwd_xz) > 1e-3f ? glm::normalize(fwd_xz) : glm::vec3(0.0f, 0.0f, 1.0f);
        const std::array<glm::vec2, 6> offsets = {{
            {0.0f, 0.0f}, {0.18f, 0.0f}, {-0.18f, 0.0f}, {0.0f, 0.18f}, {0.0f, -0.18f}, {fwd_xz.x * 0.22f, fwd_xz.z * 0.22f},
        }};
        float highest_floor = -1e9f;
        const float ceiling_cap = camera.position.y + 0.48f;
        for (const glm::vec2& o : offsets) {
            RayHit hit;
            const glm::vec3 top(camera.position.x + o.x, ceiling_cap, camera.position.z + o.y);
            if (col.Raycast(top, glm::vec3(0.0f, -1.0f, 0.0f), 2.0f, hit) && hit.normal.y > 0.35f) {
                highest_floor = std::max(highest_floor, hit.point.y);
            }
        }
        if (highest_floor > -1e8f) {
            constexpr float kMinFloorClear = 0.165f;
            if (camera.position.y < highest_floor + kMinFloorClear) {
                camera.position.y = highest_floor + kMinFloorClear;
            }
        }
    };
    enforce_floor();
    constexpr float kWallClear = 0.22f;
    for (int iter = 0; iter < 2; ++iter) {
        glm::vec3 push(0.0f);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * (kPi * 0.25f);
            const glm::vec3 dir(std::cos(a), 0.0f, std::sin(a));
            RayHit hit;
            if (col.Raycast(camera.position, dir, kWallClear, hit) && hit.distance < kWallClear) {
                push += hit.normal * (kWallClear - hit.distance);
            }
        }
        {
            const glm::vec3 fwd = camera.Forward();
            RayHit hit;
            if (col.Raycast(camera.position, fwd, kWallClear, hit) && hit.distance < kWallClear) {
                push += hit.normal * (kWallClear - hit.distance);
            }
        }
        if (glm::length(push) < 1e-4f) break;
        const float len = glm::length(push);
        if (len > 0.35f) push *= 0.35f / len;
        camera.position += push;
        enforce_floor();
    }
}

void SurvivalNight::WakeCamera(Camera& camera) const {
    if (!active_ || !waking_ || !wake_third_) return;
    const double frame = std::max(0.0, game_.Demos().PlayFrame("gc_p00_020"));
    const float rise = Smooth(static_cast<float>((frame - 160.0) / 560.0));
    const float settle = Smooth(static_cast<float>((frame - 620.0) / 235.0));
    const glm::vec3 forward(std::sin(wake_yaw_), 0.0f, std::cos(wake_yaw_));
    const glm::vec3 right(-std::cos(wake_yaw_), 0.0f, std::sin(wake_yaw_));
    glm::vec3 body_focus = wake_at_ + glm::vec3(0.0f, glm::mix(0.32f, 1.32f, rise), 0.0f);
    if (glm::mat4 chest(1.0f); game_.Demos().PlayerBoneWorld("gc_p00_020", "SKL_002_CHEST", chest)) {
        body_focus = glm::vec3(chest[3]);
    }
    if (glm::mat4 head(1.0f); game_.Demos().PlayerBoneWorld("gc_p00_020", "SKL_004_HEAD", head)) {
        body_focus = glm::mix(body_focus, glm::vec3(head[3]), 0.45f);
    }
    const glm::vec3 ots_head = wake_at_ + glm::vec3(0.0f, 1.60f, 0.0f);
    const glm::vec3 ots_pos = ots_head + right * 0.36f + glm::vec3(0.0f, 0.10f, 0.0f) - forward * 1.60f;
    const glm::vec3 wake_pos = glm::vec3(body_focus.x, wake_at_.y + glm::mix(0.68f, 1.56f, rise), body_focus.z) +
                               right * glm::mix(0.78f, 0.44f, rise) - forward * glm::mix(2.05f, 1.72f, rise);
    glm::vec3 pos = glm::mix(wake_pos, ots_pos, settle);
    const glm::vec3 look_at = glm::mix(body_focus + forward * 0.25f, ots_head + forward * 14.0f, settle);
    const glm::vec3 from = body_focus + glm::vec3(0.0f, 0.15f, 0.0f);
    const glm::vec3 delta = pos - from;
    const float dist = glm::length(delta);
    if (dist > 0.2f) {
        RayHit hit;
        if (game_.Collision().Raycast(from, delta / dist, dist + 0.15f, hit)) {
            pos = from + (delta / dist) * std::max(0.35f, hit.distance - 0.18f);
        }
    }
    camera.position = pos;
    const glm::vec3 dir = glm::normalize(look_at - camera.position);
    camera.yaw = std::atan2(-dir.x, -dir.z);
    camera.pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    camera.roll = 0.0f;
    camera.fov_y = glm::radians(50.0f);
}

bool SurvivalNight::ReloadNow(ReloadPose& pose) const {
    const std::vector<SurvivalReloadKey>& keys = models_.guns[static_cast<size_t>(weapon_)].reload.keys;
    if (keys.empty() || gun_.tilt <= 0.0f) return false;
    size_t next = 1;
    while (next + 1 < keys.size() && keys[next].t < gun_.reload_at) ++next;
    const SurvivalReloadKey& a = keys[next > 0 ? next - 1 : 0];
    const SurvivalReloadKey& b = keys[std::min(next, keys.size() - 1)];
    const float x = b.t > a.t ? Smooth((gun_.reload_at - a.t) / (b.t - a.t)) : 1.0f;
    const float k = Smooth(gun_.tilt);
    pose.at = glm::mix(a.at, b.at, x) * k;
    pose.turn = glm::mix(a.turn, b.turn, x) * k;
    pose.hand = glm::mix(a.hand, b.hand, x) * k;
    pose.left = glm::mix(a.left, b.left, x);
    pose.left_turn = glm::slerp(a.left_turn, b.left_turn, x);
    pose.away = glm::mix(a.away, b.away, x);
    pose.elbow = glm::mix(a.elbow, b.elbow, x);
    pose.thumb = glm::mix(a.thumb, b.thumb, x) * k;
    pose.right_thumb = glm::mix(a.right_thumb, b.right_thumb, x);
    for (size_t f = 0; f < pose.fingers.size(); ++f) pose.fingers[f] = glm::mix(a.fingers[f], b.fingers[f], x);
    pose.item = glm::mix(a.item, b.item, x);
    pose.seat = glm::mix(a.seat, b.seat, x);
    pose.spent = glm::mix(a.spent, b.spent, x);
    pose.open = glm::mix(a.open, b.open, x) * k;
    return true;
}

void SurvivalNight::AddReloadItems(std::vector<DrawItem>& out, const glm::mat4& gun, const glm::mat4& hand, bool first_person) const {
    ReloadPose pose;
    if (reload_timer_ <= 0.0f || !ReloadNow(pose) || pose.item < 0.5f) return;
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    const glm::mat4 held = hand * glm::translate(glm::mat4(1.0f), model.reload.item_at) * TurnXyz(model.reload.item_turn);
    const float seat = Smooth(pose.seat);
    auto add = [&](const GpuMesh* mesh, const glm::mat4& in_hand, const glm::mat4& home, const glm::vec3& scale, uint32_t part) {
        if (!mesh) return;
        glm::mat4 m = glm::mat4_cast(glm::slerp(glm::quat_cast(glm::mat3(in_hand)), glm::quat_cast(glm::mat3(home)), seat));
        m[3] = glm::vec4(glm::mix(glm::vec3(in_hand[3]), glm::vec3(home[3]), seat), 1.0f);
        DrawItem item;
        item.mesh = mesh;
        item.transform = glm::scale(m, scale);
        item.hidden_views = first_person ? 2 : 0;
        item.handy_shadowless = first_person;
        item.light_fitting = kHandyLightFitting;
        item.view_attached = first_person;
        item.turns_with_view = !first_person;
        item.source = kSourceBase + 0x50020u + part;
        out.push_back(item);
    };
    switch (weapon_) {
    case SurvivalWeapon::Handgun:
        add(model.magazine, held, gun, glm::vec3(1.0f), 0);
        break;
    case SurvivalWeapon::Shotgun:
        add(models_.drawn.shell, held, glm::translate(gun, model.loading_port + glm::vec3(0.0f, 0.0f, 0.05f)), glm::vec3(1.0f), 0);
        break;
    case SurvivalWeapon::Lever:
        add(models_.drawn.casing, held, glm::translate(gun, model.loading_port + glm::vec3(0.0f, 0.0f, 0.04f)), glm::vec3(1.25f, 1.25f, 2.6f), 0);
        break;
    case SurvivalWeapon::Double:
        for (const float side : {1.0f, -1.0f}) {
            add(models_.drawn.shell, held * glm::translate(glm::mat4(1.0f), glm::vec3(side * 0.0115f, 0.0f, 0.0f)),
                glm::translate(gun * BarrelsTurn(), model.ejection + glm::vec3(side * model.barrel_side, 0.0f, 0.0f)), glm::vec3(1.0f), side > 0.0f ? 0 : 1);
        }
        break;
    case SurvivalWeapon::Magnum:
        add(pose.spent > 0.5f ? models_.loader_empty.get() : models_.loader.get(), held,
            glm::translate(gun, model.cylinder_axis + model.cylinder_swing * gun_.swing + glm::vec3(0.0f, 0.0f, -0.0105f)), glm::vec3(1.0f), 0);
        break;
    default: break;
    }
}

void SurvivalNight::AddGunDraws(std::vector<DrawItem>& out, const glm::mat4& gun, uint8_t hidden_views) const {
    const size_t w = static_cast<size_t>(weapon_);
    const SurvivalGunModel& model = models_.guns[w];
    auto draw = [&](const GpuMesh* mesh, const glm::mat4& m, uint64_t part) {
        if (!mesh) return;
        DrawItem item;
        item.mesh = mesh;
        item.transform = m;
        item.hidden_views = hidden_views;
        item.handy_shadowless = hidden_views == 2;
        item.light_fitting = kHandyLightFitting;
        item.view_attached = hidden_views == 2;
        item.turns_with_view = hidden_views != 2;
        item.source = kSourceBase + 0x20000u + w * 16u + part;
        out.push_back(item);
    };
    draw(model.body, gun, 0);
    const bool reloading = reload_timer_ > 0.0f;
    switch (weapon_) {
    case SurvivalWeapon::Handgun: {
        draw(model.moving, glm::translate(gun, glm::vec3(0.0f, 0.0f, -model.slide_travel * gun_.slide)), 1);
        if (model.hammer) {
            const glm::vec3 pivot = model.hammer_pivot;
            draw(model.hammer, glm::translate(gun, pivot) * Rot(-0.25f * gun_.slide, {1, 0, 0}) * glm::translate(glm::mat4(1.0f), -pivot), 3);
        }
        if (gun_.magazine < 0.999f) {
            draw(model.magazine, glm::translate(gun, model.magazine_drop * (0.16f * gun_.magazine)), 2);
        }
        break;
    }
    case SurvivalWeapon::Shotgun: {
        draw(model.moving, glm::translate(gun, glm::vec3(0.0f, 0.0f, -model.pump_travel * gun_.slide)), 1);
        break;
    }
    case SurvivalWeapon::Lever: {
        const glm::vec3 pivot = model.lever_pivot;
        draw(model.moving, glm::translate(gun, pivot) * Rot(-0.9f * gun_.lever, {1, 0, 0}) * glm::translate(glm::mat4(1.0f), -pivot), 1);
        const glm::vec3 hp = model.hammer_pivot;
        draw(model.hammer, glm::translate(gun, hp) * Rot(-0.5f * (1.0f - gun_.hammer_fall), {1, 0, 0}) * glm::translate(glm::mat4(1.0f), -hp), 2);
        break;
    }
    case SurvivalWeapon::Double: {
        draw(model.moving, gun * BarrelsTurn(), 1);
        break;
    }
    case SurvivalWeapon::Magnum: {
        const glm::vec3 axis = model.cylinder_axis;
        const glm::vec3 swing = model.cylinder_swing * gun_.swing;
        const glm::mat4 cylinder = glm::translate(gun, axis + swing) * Rot(gun_.cylinder, {0, 0, 1}) * glm::translate(glm::mat4(1.0f), -axis);
        draw(model.moving, cylinder, 1);
        const glm::vec3 pivot = model.hammer_pivot;
        draw(model.hammer, glm::translate(gun, pivot) * Rot(-0.75f * gun_.hammer, {1, 0, 0}) * glm::translate(glm::mat4(1.0f), -pivot), 2);
        if (ReloadPose pose; reloading && ReloadNow(pose) && (gun_.reload_at < model.reload.eject || pose.seat > 0.98f || pose.spent > 0.5f)) {
            draw(models_.rounds.get(), glm::translate(gun, axis + swing) * Rot(gun_.cylinder, {0, 0, 1}), 3);
        }
        break;
    }
    default: break;
    }
    const int equipped_skin = game_.SurvivalProfileData().EquippedSkin(static_cast<int>(weapon_));
    if (SurvivalSkin(equipped_skin).rarity == SkinRarity::Mythic) {
        TextureManager& tm = game_.Models().Textures();
        ApplyWeaponSkinAnimated(tm, model, equipped_skin, time_);
        EmitMythicWeaponVfx(tm, models_, model, gun, equipped_skin, time_, hidden_views, hidden_views == 2, hidden_views != 2, 190.0f, out);
    }
    if (muzzle_ > 0.0f && models_.flash) {
        const float k = muzzle_size_ * (weapon_ == SurvivalWeapon::Handgun ? 0.8f : 1.25f) * (0.6f + 0.4f * muzzle_ / 0.05f);
        draw(models_.flash.get(), glm::scale(glm::translate(gun, model.muzzle) * Rot(muzzle_roll_, {0, 0, 1}), glm::vec3(k, k, k * (0.8f + 0.4f * muzzle_size_))), 4);
    }
}

void SurvivalNight::AddContactQuads(vfx::RenderList& list, const Camera& camera) const {
    if (models_.contact_texture == 0) return;
    const uint32_t first = static_cast<uint32_t>(list.quads.size());
    float depth = 0.0f;
    auto under_car = [&](const glm::vec3& middle, const glm::vec3& along) {
        const glm::vec3 ahead = glm::normalize(glm::vec3(along.x, 0.0f, along.z));
        const glm::vec3 side(-ahead.z, 0.0f, ahead.x);
        const glm::vec3 centre(middle.x, 0.02f, middle.z);
        vfx::Quad q;
        for (int corner = 0; corner < 4; ++corner) {
            const float u = (corner & 1) != 0 ? 1.0f : -1.0f, v = (corner & 2) != 0 ? -1.0f : 1.0f;
            q.corner[corner] = glm::vec4(centre + side * (1.3f * u) + ahead * (2.85f * v), 1.0f);
        }
        q.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        q.uv_next = q.uv;
        q.color = glm::vec4(0.0f, 0.0f, 0.0f, 0.92f);
        q.params = glm::vec4(1.0f / 0.02f, 0.0f, 0.0f, 0.0f);
        q.info = glm::uvec4(models_.car_contact_texture, vfx::kQuadSoft, vfx::kNoTexture, 0u);
        list.quads.push_back(q);
        depth = std::max(depth, glm::dot(middle - camera.position, camera.Forward()));
    };
    for (const auto& [middle, along] : kLevelCars) under_car(middle, along);
    for (const glm::mat4& car : game_.Demos().ModelsDrawn("shsb_carr")) under_car(glm::vec3(car[3]), glm::vec3(car[2]));
    if (BodyHoldsGun() && !hud_.dead && !waking_) {
        const Player& player = game_.GetPlayer();
        const glm::mat4 body = player.PoseTransform();
        glm::vec3 foot_mid = glm::vec3(body[3]);
        if (glm::mat4 lf(1.0f), rf(1.0f); player.BoneModel("SKL_032_LFOOT", lf) && player.BoneModel("SKL_042_RFOOT", rf)) {
            const glm::vec3 lw(body * lf[3]);
            const glm::vec3 rw(body * rf[3]);
            foot_mid.x = 0.5f * (lw.x + rw.x);
            foot_mid.z = 0.5f * (lw.z + rw.z);
        }
        const glm::vec3 ahead = glm::normalize(glm::vec3(body[2].x, 0.0f, body[2].z) + glm::vec3(0.0f, 0.0f, 1.0e-5f));
        const glm::vec3 side(-ahead.z, 0.0f, ahead.x);
        const glm::vec3 centre(foot_mid.x, foot_mid.y + 0.02f, foot_mid.z);
        vfx::Quad q;
        for (int corner = 0; corner < 4; ++corner) {
            const float u = (corner & 1) != 0 ? 1.0f : -1.0f, v = (corner & 2) != 0 ? -1.0f : 1.0f;
            q.corner[corner] = glm::vec4(centre + side * (0.44f * u) + ahead * (0.38f * v), 1.0f);
        }
        q.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        q.uv_next = q.uv;
        q.color = glm::vec4(0.0f, 0.0f, 0.0f, 0.62f);
        q.params = glm::vec4(1.0f / 0.02f, 0.0f, 0.0f, 0.0f);
        q.info = glm::uvec4(models_.contact_texture, vfx::kQuadSoft, vfx::kNoTexture, 0u);
        list.quads.push_back(q);
        depth = std::max(depth, glm::dot(foot_mid - camera.position, camera.Forward()));
    }
    for (const Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dying) continue;
        const float up = 1.0f - std::clamp(e.sink / 0.4f, 0.0f, 1.0f);
        if (up <= 0.0f) continue;
        glm::vec2 reach(0.42f, 0.36f);
        if (e.kind == SurvivalEnemyKind::Crawler) reach = {0.55f, 0.95f};
        else if (e.kind == SurvivalEnemyKind::Giant) reach = {0.8f, 0.68f};
        const glm::vec3 ahead(std::sin(e.yaw), 0.0f, std::cos(e.yaw));
        const glm::vec3 side(-ahead.z, 0.0f, ahead.x);
        const glm::vec3 centre = e.position + glm::vec3(0.0f, 0.02f, 0.0f);
        vfx::Quad q;
        for (int corner = 0; corner < 4; ++corner) {
            const float u = (corner & 1) != 0 ? 1.0f : -1.0f, v = (corner & 2) != 0 ? -1.0f : 1.0f;
            q.corner[corner] = glm::vec4(centre + side * (reach.x * u) + ahead * (reach.y * v), 1.0f);
        }
        q.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        q.uv_next = q.uv;
        q.color = glm::vec4(0.0f, 0.0f, 0.0f, 0.55f * up);
        q.params = glm::vec4(1.0f / 0.02f, 0.0f, 0.0f, 0.0f);
        q.info = glm::uvec4(models_.contact_texture, vfx::kQuadSoft, vfx::kNoTexture, 0u);
        list.quads.push_back(q);
        depth = std::max(depth, glm::dot(e.position - camera.position, camera.Forward()));
    }
    const uint32_t count = static_cast<uint32_t>(list.quads.size()) - first;
    if (count == 0) return;
    vfx::Draw draw;
    draw.layer = vfx::Layer::World;
    draw.blend = vfx::BlendMode::Mul;
    draw.first = first;
    draw.count = count;
    draw.depth = depth;
    list.draws.push_back(draw);
}

void SurvivalNight::AddEffectQuads(vfx::RenderList& list, const Camera& camera) const {
    if (!active_) return;
    AddContactQuads(list, camera);
    if (models_.mist_texture == 0) return;
    const uint32_t first = static_cast<uint32_t>(list.quads.size());
    glm::vec3 lens(0.0f), beam(0.0f);
    if (glm::mat4 chest(1.0f); ChestLight(chest)) {
        const Camera own = game_.GetPlayer().MakeCamera();
        lens = glm::vec3(chest * glm::vec4(models_.chest_lens, 1.0f));
        beam = glm::normalize(own.position + own.Forward() * std::max(beam_reach_, 3.0f) - lens);
    }
    float depth = 0.0f;
    for (const BurningFlare& f : flares_out_) {
        if (f.age >= f.life) continue;
        const glm::vec3 tip = f.position + glm::vec3(f.rotation * glm::vec4(0.0f, 0.0f, 0.1f, 0.0f));
        const float burning = std::clamp(f.life - 1.0f - f.age, 0.0f, 1.0f);
        const float phase = static_cast<float>(f.id) * 1.7f;
        const float pulse = 0.86f + 0.14f * std::sin(time_ * 27.0f + phase) + 0.08f * std::sin(time_ * 43.0f + phase * 1.9f);
        constexpr int kPuffs = 24;
        constexpr float kSmokeLife = 3.0f;
        for (int i = 0; i < kPuffs; ++i) {
            const float t = std::fmod(f.age / kSmokeLife + static_cast<float>(i) / kPuffs, 1.0f);
            if (f.age < t * kSmokeLife) continue;
            const float seed = static_cast<float>(f.id * 7u + static_cast<uint32_t>(i) * 19u);
            const float swirl_x = 0.50f * t * std::sin(seed + t * 2.3f) + 0.48f * t * t;
            const float swirl_z = 0.45f * t * std::cos(seed * 1.3f + t * 1.9f) - 0.22f * t * t;
            const glm::vec3 at = tip + glm::vec3(swirl_x, 0.14f + 3.65f * std::pow(t, 0.85f), swirl_z);
            const float width = 0.28f + 1.65f * std::pow(t, 0.72f);
            const float rot = seed * 0.73f + t * (i % 2 == 0 ? 1.15f : -1.15f);
            const float cr = std::cos(rot), sr = std::sin(rot);
            const glm::vec3 r_axis = (camera.Right() * cr + camera.Up() * sr) * width;
            const glm::vec3 u_axis = (camera.Up() * cr - camera.Right() * sr) * width;
            vfx::Quad q;
            for (int corner = 0; corner < 4; ++corner) {
                const float u = (corner & 1) != 0 ? 0.5f : -0.5f, v = (corner & 2) != 0 ? -0.5f : 0.5f;
                q.corner[corner] = glm::vec4(at + r_axis * u + u_axis * v, 1.0f);
            }
            q.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            q.uv_next = q.uv;
            const glm::vec3 base_red = glm::vec3(3.4f, 0.26f, 0.11f) * pulse;
            const glm::vec3 mid_red = glm::vec3(1.10f, 0.12f, 0.065f) * (0.85f + 0.15f * pulse);
            const glm::vec3 top_smoke = glm::vec3(0.22f, 0.065f, 0.055f);
            const glm::vec3 lit = t < 0.32f ? glm::mix(base_red, mid_red, Smooth(t / 0.32f))
                                            : glm::mix(mid_red, top_smoke, Smooth((t - 0.32f) / 0.68f));
            const float alpha = 0.88f * Smooth(t / 0.06f) * std::pow(1.0f - t, 0.72f) * (0.25f + 0.75f * burning);
            q.color = glm::vec4(lit, alpha);
            q.params = glm::vec4(1.0f / 0.18f, 0.0f, 0.0f, 0.0f);
            q.info = glm::uvec4(models_.mist_texture, vfx::kQuadSoft, vfx::kNoTexture, 0u);
            list.quads.push_back(q);
            depth = std::max(depth, glm::dot(at - camera.position, camera.Forward()));
        }
    }
    for (const Debris& d : debris_) {
        if (d.kind != Debris::Kind::Mist || d.age >= d.life) continue;
        const float t = std::clamp(d.age / std::max(d.life, 1e-3f), 0.0f, 1.0f);
        const float width = d.size * (0.55f + 0.45f * std::sqrt(t));
        const glm::vec2 flight(glm::dot(d.spin, camera.Right()), glm::dot(d.spin, camera.Up()));
        const float on_screen = glm::length(flight);
        const glm::vec2 axis = on_screen > 0.15f ? flight / on_screen : glm::vec2(std::cos(d.spin.z), std::sin(d.spin.z));
        const float stretch = 1.0f + 0.5f * std::min(on_screen, 1.0f) * (1.0f - t);
        const glm::vec3 along = (camera.Right() * axis.x + camera.Up() * axis.y) * (width * stretch);
        const glm::vec3 across = (camera.Up() * axis.x - camera.Right() * axis.y) * (width * 0.85f);
        const glm::vec3 centre = d.position + along * 0.32f;
        vfx::Quad q;
        for (int corner = 0; corner < 4; ++corner) {
            const float u = (corner & 1) != 0 ? 0.5f : -0.5f, v = (corner & 2) != 0 ? -0.5f : 0.5f;
            q.corner[corner] = glm::vec4(centre + along * u + across * v, 1.0f);
        }
        q.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        q.uv_next = q.uv;
        float lit = 0.7f;
        if (glm::dot(beam, beam) > 0.5f) {
            const glm::vec3 to = d.position - lens;
            const float distance = glm::length(to);
            const float cone = Smooth((glm::dot(to / std::max(distance, 1e-3f), beam) - 0.88f) / 0.09f);
            lit += 0.5f * cone / (1.0f + distance * distance / 64.0f);
        }
        q.color = glm::vec4(glm::vec3(0.42f, 0.03f, 0.022f) * lit, 0.8f * std::pow(1.0f - t, 1.7f));
        q.params = glm::vec4(1.0f / 0.04f, 0.0f, 0.0f, 0.0f);
        q.info = glm::uvec4(models_.mist_texture, vfx::kQuadSoft, vfx::kNoTexture, 0u);
        list.quads.push_back(q);
        depth = std::max(depth, glm::dot(d.position - camera.position, camera.Forward()));
    }
    const uint32_t count = static_cast<uint32_t>(list.quads.size()) - first;
    if (static const bool log = std::getenv("PT_SURVIVAL_POSE_LOG") != nullptr; log && count > 0) {
        LogInfo("survival: {} mist quads, texture {}, the first at ({:.2f} {:.2f} {:.2f}) alpha {:.2f}", count, models_.mist_texture,
                list.quads[first].corner[0].x, list.quads[first].corner[0].y, list.quads[first].corner[0].z, list.quads[first].color.a);
    }
    if (count > 0) {
        vfx::Draw draw;
        draw.layer = vfx::Layer::World;
        draw.blend = vfx::BlendMode::Alpha;
        draw.first = first;
        draw.count = count;
        draw.depth = depth;
        list.draws.push_back(draw);
    }
    if (models_.flare_glow_texture != 0) {
        const uint32_t glow_first = static_cast<uint32_t>(list.quads.size());
        float glow_depth = 0.0f;
        auto add_halo = [&](const glm::vec3& at, float width, const glm::vec4& color, float soft_m) {
            vfx::Quad q;
            for (int corner = 0; corner < 4; ++corner) {
                const float u = (corner & 1) != 0 ? 0.5f : -0.5f, v = (corner & 2) != 0 ? -0.5f : 0.5f;
                q.corner[corner] = glm::vec4(at + camera.Right() * (width * u) + camera.Up() * (width * v), 1.0f);
            }
            q.uv = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            q.uv_next = q.uv;
            q.color = color;
            q.params = glm::vec4(1.0f / soft_m, 0.0f, 0.0f, 0.0f);
            q.info = glm::uvec4(models_.flare_glow_texture, vfx::kQuadSoft, vfx::kNoTexture, 0u);
            list.quads.push_back(q);
            glow_depth = std::max(glow_depth, glm::dot(at - camera.position, camera.Forward()));
        };
        for (const BurningFlare& f : flares_out_) {
            const float alive = std::clamp(f.life - 1.0f - f.age, 0.0f, 1.0f) * Smooth(f.age / 0.15f);
            if (alive <= 0.0f) continue;
            const float phase = static_cast<float>(f.id) * 1.7f;
            const float flicker = std::clamp(0.84f + 0.14f * std::sin(time_ * 23.0f + phase) + 0.10f * std::sin(time_ * 41.0f + phase * 2.3f) +
                                             0.08f * std::sin(time_ * 7.3f + phase * 0.7f), 0.6f, 1.25f);
            const glm::vec3 tip = f.position + glm::vec3(f.rotation * glm::vec4(0.0f, 0.0f, 0.105f, 0.0f)) + glm::vec3(0.0f, 0.09f, 0.0f);
            add_halo(tip, 0.44f * (0.9f + 0.16f * flicker), glm::vec4(5.5f, 1.85f, 1.35f, 0.95f * alive), 0.12f);
            add_halo(tip + glm::vec3(0.0f, 0.10f, 0.0f), 1.85f * (0.92f + 0.14f * flicker),
                     glm::vec4(2.4f * flicker, 0.17f * flicker, 0.08f * flicker, 0.80f * alive), 0.28f);
            add_halo(tip + glm::vec3(0.0f, 0.36f, 0.0f), 3.8f, glm::vec4(0.75f * flicker, 0.045f * flicker, 0.022f * flicker, 0.42f * alive), 0.45f);
        }
        if (hand_flare_lit_ && throw_timer_ >= 0.1f && !thrown_) {
            const float k = Smooth((throw_timer_ - 0.1f) / 0.08f);
            add_halo(hand_flare_at_, 0.22f, glm::vec4(3.6f, 1.2f, 0.85f, 0.85f * k), 0.04f);
            add_halo(hand_flare_at_, 0.75f, glm::vec4(1.4f, 0.12f, 0.06f, 0.55f * k), 0.12f);
        }
        for (const Debris& d : debris_) {
            if (d.kind != Debris::Kind::Heal || d.age >= d.life) continue;
            const float t = std::clamp(d.age / std::max(d.life, 1e-3f), 0.0f, 1.0f);
            const float fade = Smooth(t / 0.14f) * std::pow(1.0f - t, 1.35f);
            add_halo(d.position, d.size * (0.75f + 0.55f * t), glm::vec4(0.35f, 2.35f, 0.95f, 0.72f * fade), 0.08f);
            add_halo(d.position, d.size * 2.2f, glm::vec4(0.14f, 0.95f, 0.38f, 0.26f * fade), 0.16f);
        }
        const uint32_t glow_count = static_cast<uint32_t>(list.quads.size()) - glow_first;
        if (glow_count > 0) {
            vfx::Draw draw;
            draw.layer = vfx::Layer::World;
            draw.blend = vfx::BlendMode::Add;
            draw.first = glow_first;
            draw.count = glow_count;
            draw.depth = glow_depth;
            list.draws.push_back(draw);
        }
    }
}

void SurvivalNight::AddDraws(std::vector<DrawItem>& out, const Camera& camera) {
    if (!active_) return;
    city_.AddDraws(out);
    for (const Enemy& e : enemies_) {
        const bool lisa = e.kind == SurvivalEnemyKind::Mother || e.kind == SurvivalEnemyKind::Lisa;
        const GpuMesh* mesh = lisa ? lisa_mesh_ : doll_mesh_;
        if (!mesh || skins_[e.slot].empty()) continue;
        if (e.state == Enemy::State::Dying && !BodyHoldsGun() && glm::distance(e.head, camera.position) < 0.45f) continue;
        if (caught_ && e.id == caught_by_) continue;
        DrawItem item;
        item.mesh = mesh;
        const glm::mat4 m = EnemyTransform(e);
        item.transform = m;
        for (const Enemy::Wound& w : e.wounds) {
            if (w.bone < 0 || static_cast<size_t>(w.bone) >= skins_[e.slot].size() || !models_.wound) continue;
            const glm::mat4 bone = m * skins_[e.slot][static_cast<size_t>(w.bone)];
            const glm::vec3 p(bone * glm::vec4(w.point, 1.0f));
            const glm::vec3 n = glm::normalize(glm::vec3(bone * glm::vec4(w.normal, 0.0f)));
            const glm::vec3 side = std::abs(n.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            const float s = w.size * (e.kind == SurvivalEnemyKind::Giant ? 1.9f : 1.0f) * (0.55f + 0.45f * Smooth(w.age / 0.4f));
            DrawItem wound;
            wound.mesh = models_.wound.get();
            wound.transform = glm::scale(Frame(p, n, side), glm::vec3(s));
            wound.source = kSourceBase + 0x50000u + e.id * 16u + static_cast<uint32_t>(&w - e.wounds.data());
            out.push_back(wound);
        }
        item.skin = skins_[e.slot];
        if (lisa) item.hidden_meshes = lisa_hidden_;
        item.original_look = e.kind == SurvivalEnemyKind::Lisa;
        const glm::vec3 base(1.0f);
        glm::vec3 tint = glm::mix(base, glm::vec3(1.7f, 0.55f, 0.48f), e.hurt * 0.48f);
        const float hp_share = std::clamp(e.health / std::max(e.max_health, 1.0f), 0.0f, 1.0f);
        if (hp_share < 0.6f && e.kind != SurvivalEnemyKind::Lisa) {
            const float battered = 1.0f - hp_share / 0.6f;
            tint *= glm::mix(glm::vec3(1.0f), glm::vec3(0.84f, 0.66f, 0.62f), battered * 0.75f);
        }
        if (e.kind == SurvivalEnemyKind::Doll) {
            const float tone = 0.90f + 0.14f * std::sin(static_cast<float>(e.id) * 1.91f) * 0.5f;
            tint *= glm::vec3(tone, tone * 0.97f, tone * 0.94f);
        } else if (e.kind == SurvivalEnemyKind::Crawler) {
            tint *= glm::vec3(0.68f, 0.57f, 0.55f);
        } else if (e.kind == SurvivalEnemyKind::Stretched) {
            tint *= glm::vec3(0.76f, 0.83f, 0.92f);
        } else if (e.kind == SurvivalEnemyKind::Mother) {
            tint *= glm::vec3(0.92f, 0.84f, 0.72f);
        } else if (e.kind == SurvivalEnemyKind::Giant) {
            tint *= glm::vec3(0.56f, 0.55f, 0.58f);
        }
        if (e.affix & kAffixArmored) tint *= glm::vec3(0.42f, 0.45f, 0.5f);
        if (e.affix & kAffixShy) tint *= glm::vec3(1.35f, 1.35f, 1.4f);
        if (e.affix & kAffixScreamer) tint *= glm::vec3(1.15f, 0.62f, 0.56f);
        if (e.affix & kAffixSplitter) tint *= glm::vec3(0.98f, 0.9f, 0.55f);
        if (e.state == Enemy::State::Dying) tint *= 1.0f - 0.45f * Smooth(e.timer / 1.5f);
        item.tint = glm::vec4(tint, 1.0f);
        item.source = kSourceBase + e.id;
        out.push_back(item);
    }
    AddPhantomDraws(out);
    AddTaskDraws(out);
    for (const Pickup& p : pickups_) {
        glm::mat4 m = glm::translate(glm::mat4(1.0f), p.position) * Rot(static_cast<float>(p.id) * 2.39996f, {0, 1, 0});
        const GpuMesh* mesh = nullptr;
        if (p.kind == SurvivalPickupKind::Ammo) {
            mesh = models_.drawn.ammo[static_cast<size_t>(p.weapon)];
            m = glm::scale(m, glm::vec3(1.2f));
        } else if (p.kind == SurvivalPickupKind::Health) {
            mesh = models_.drawn.medkit;
        } else if (p.kind == SurvivalPickupKind::Pipe) {
            mesh = models_.drawn.pipe;
            m = m * Rot(kPi * 0.5f, {0, 1, 0}) * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0228f, -0.27f)) * Rot(kPi * 0.5f, {0, 0, 1});
        } else if (p.kind == SurvivalPickupKind::Flare) {
            mesh = models_.drawn.flare;
            m = m * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0158f, 0.0f));
        } else {
            mesh = models_.guns[static_cast<size_t>(p.weapon)].body;
            m = glm::translate(m, glm::vec3(0.0f, 0.03f, 0.0f)) * Rot(kPi * 0.5f, {0, 0, 1});
        }
        if (mesh) {
            DrawItem item;
            item.mesh = mesh;
            item.transform = m;
            item.source = kSourceBase + 0x10000u + p.id * 4u;
            out.push_back(item);
            if (p.kind == SurvivalPickupKind::Weapon) {
                const SurvivalGunModel& g = models_.guns[static_cast<size_t>(p.weapon)];
                for (const GpuMesh* part : {g.moving, g.hammer, g.magazine}) {
                    if (!part) continue;
                    DrawItem piece = item;
                    piece.mesh = part;
                    piece.source += 1;
                    out.push_back(piece);
                }
            }
        }
    }
    for (const Debris& d : debris_) {
        const GpuMesh* mesh = nullptr;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), d.position) * glm::mat4_cast(d.rotation);
        switch (d.kind) {
        case Debris::Kind::Casing: mesh = models_.drawn.casing; break;
        case Debris::Kind::Shell: mesh = models_.drawn.shell; break;
        case Debris::Kind::Magazine: mesh = models_.guns[1].magazine; break;
        case Debris::Kind::PipeHead: mesh = models_.drawn.pipe_halves[0]; break;
        case Debris::Kind::PipeTail: mesh = models_.drawn.pipe_halves[1]; break;
        case Debris::Kind::Blood: {
            mesh = models_.droplet.get();
            const float speed = glm::length(d.velocity);
            if (speed > 0.05f) {
                m = glm::scale(Frame(d.position, d.velocity / speed, std::abs(d.velocity.y) / speed > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)),
                               glm::vec3(d.size, d.size, d.size * std::min(4.0f, 1.0f + 0.35f * speed)));
            } else {
                m = glm::scale(glm::translate(glm::mat4(1.0f), d.position), glm::vec3(d.size));
            }
            break;
        }
        case Debris::Kind::Mist:
        case Debris::Kind::Heal: continue;
        case Debris::Kind::Spark: {
            mesh = models_.spark.get();
            const float l = glm::length(d.velocity);
            if (l > 1e-3f) m = Frame(d.position, d.velocity / l, std::abs(d.velocity.y) / l > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
            break;
        }
        }
        if (!mesh) continue;
        DrawItem item;
        item.mesh = mesh;
        item.transform = m;
        item.source = kSourceBase + 0x30000u + d.id;
        out.push_back(item);
    }
    for (const Mark& mk : marks_) {
        DrawItem item;
        if (mk.kind == Mark::Kind::Blood) {
            item.mesh = models_.blood[static_cast<size_t>(mk.variant)].get();
            const float s = mk.size * (0.35f + 0.65f * Smooth(mk.grow));
            item.transform = glm::scale(glm::translate(glm::mat4(1.0f), mk.position) * Rot(mk.spin, {0, 1, 0}), glm::vec3(s, 1.0f, s));
        } else if (mk.kind == Mark::Kind::Splat) {
            item.mesh = models_.blood[static_cast<size_t>(mk.variant)].get();
            const float s = mk.size * (0.6f + 0.4f * Smooth(mk.grow));
            const glm::vec3 along = glm::normalize(glm::cross(mk.normal, std::abs(mk.normal.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)));
            item.transform = glm::scale(Frame(mk.position, along, mk.normal) * Rot(mk.spin, {0, 1, 0}), glm::vec3(s, 1.0f, s));
        } else {
            item.mesh = models_.hole.get();
            const glm::vec3 up = std::abs(mk.normal.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
            item.transform = glm::scale(Frame(mk.position, mk.normal, up) * Rot(mk.spin, {0, 0, 1}), glm::vec3(mk.size));
        }
        if (!item.mesh) continue;
        item.source = kSourceBase + 0x40000u + mk.id;
        out.push_back(item);
    }
    for (const Tracer& t : tracers_) {
        const glm::vec3 d = t.to - t.from;
        const float length = glm::length(d);
        if (length < 0.05f || !models_.tracer) continue;
        const float head = std::min(length, 1.5f + 3.0f + 400.0f * t.age);
        const float tail = std::max(1.5f, head - 3.0f);
        if (head - tail < 0.1f) continue;
        DrawItem item;
        item.mesh = models_.tracer.get();
        item.transform = glm::scale(Frame(t.from + d / length * tail, d / length), glm::vec3(1.0f, 1.0f, head - tail));
        item.hidden_views = 2;
        out.push_back(item);
    }
    if (glm::mat4 chest(1.0f); ChestLight(chest)) {
        DrawItem item;
        item.mesh = models_.chest_light;
        item.transform = chest;
        item.hidden_views = game_.BodyShown() ? 0 : 1;
        item.handy_shadowless = true;
        item.light_fitting = kHandyLightFitting;
        item.turns_with_view = true;
        out.push_back(item);
        if (game_.GetPlayer().handy_light.enable && models_.lens_glow) {
            item.mesh = models_.lens_glow.get();
            item.transform = glm::translate(chest, models_.chest_lens + glm::vec3(0.0f, 0.0f, 0.0004f));
            out.push_back(item);
        }
    }
    const bool fallen = hud_.dead && (game_.ThirdPersonCameraActive() || dead_fall_ >= 1.0f);
    if ((weapon_ != SurvivalWeapon::Flashlight || PipeHold() > 0.0f || ThrowAway() > 0.0f) && !fallen && !caught_ && !waking_) {
        const bool held = BodyHoldsGun();
        const glm::mat4 gun = held ? DrawnWeaponTransform(camera) : WeaponTransform(camera);
        if (weapon_ != SurvivalWeapon::Flashlight) AddGunDraws(out, gun, held ? BodyViews() : 2);
        if (!held) AddArmDraws(out, camera, gun);
        AddCarriedDraws(out, camera, !held);
    }
    if (dropped_gun_.active && dropped_gun_.weapon != SurvivalWeapon::Flashlight) {
        const SurvivalGunModel& g = models_.guns[static_cast<size_t>(dropped_gun_.weapon)];
        const glm::mat4 m = glm::translate(glm::mat4(1.0f), dropped_gun_.pos) * glm::mat4_cast(dropped_gun_.rot);
        uint32_t part_idx = 0;
        for (const GpuMesh* mesh : {g.body, g.moving, g.hammer, g.magazine}) {
            if (!mesh) continue;
            DrawItem item;
            item.mesh = mesh;
            item.transform = m;
            item.source = kSourceBase + 0x59000u + part_idx++;
            out.push_back(item);
        }
    }
    for (const BurningFlare& f : flares_out_) {
        if (!models_.drawn.flare) break;
        DrawItem item;
        item.mesh = models_.drawn.flare;
        item.transform = glm::translate(glm::mat4(1.0f), f.position) * glm::mat4_cast(f.rotation);
        item.source = kSourceBase + 0x60000u + f.id * 2u;
        out.push_back(item);
        if (f.age < f.life - 1.0f && models_.flare_tip) {
            item.mesh = models_.flare_tip.get();
            item.source += 1;
            out.push_back(item);
        }
    }
}

void SurvivalNight::AddCarriedDraws(std::vector<DrawItem>& out, const Camera& camera, bool first_person) {
    auto add = [&](const GpuMesh* mesh, const glm::mat4& m, uint32_t part) {
        if (!mesh) return;
        DrawItem item;
        item.mesh = mesh;
        item.transform = m;
        item.hidden_views = first_person ? 2 : BodyViews();
        item.handy_shadowless = first_person;
        item.light_fitting = kHandyLightFitting;
        item.view_attached = first_person;
        item.turns_with_view = !first_person;
        item.source = kSourceBase + 0x50010u + part;
        out.push_back(item);
    };
    if (glm::mat4 swing(1.0f); PipeHold() > 0.5f && PipeSwingFrame(SwingCamera(camera), swing)) {
        glm::mat4 pipe_world = swing * PipeInGrip();
        if (!first_person) {
            const Player& player = game_.GetPlayer();
            if (glm::mat4 rhand(1.0f); player.BoneModel("SKL_023_RHAND", rhand)) {
                const SurvivalGunModel& hg = models_.guns[static_cast<size_t>(SurvivalWeapon::Handgun)];
                pipe_world = player.PoseTransform() * rhand * glm::inverse(GripHand(hg)) * PipeInGrip();
            }
        }
        add(models_.drawn.pipe, pipe_world, 0);
    }
    if (first_person) return;
    hand_flare_lit_ = false;
    const Player& player = game_.GetPlayer();
    if (glm::mat4 bone(1.0f); player.BoneModel("SKL_013_LHAND", bone)) AddReloadItems(out, DrawnWeaponTransform(camera), player.PoseTransform() * bone, false);
    if (glm::mat4 bone(1.0f); ThrowAway() > 0.0f && !thrown_ && player.BoneModel("SKL_013_LHAND", bone)) {
        const glm::mat4 flare = player.PoseTransform() * bone * FlareInHand();
        add(models_.drawn.flare, flare, 1);
        if (throw_timer_ >= 0.1f) {
            add(models_.flare_tip.get(), flare, 2);
            hand_flare_at_ = glm::vec3(flare * glm::vec4(0.0f, 0.0f, 0.1f, 1.0f));
            hand_flare_lit_ = true;
        }
    }
}

bool SurvivalNight::NearLitLamp(const glm::vec3& p, float radius) const {
    if (StreetLightLevel() < 0.5f) return false;
    for (size_t i = 0; i < kStreetLamps.size(); ++i) {
        const glm::vec3& lamp = kStreetLamps[i];
        if (LampAlive(i) > 0.5f && glm::distance(glm::vec2(lamp.x, lamp.z), glm::vec2(p.x, p.z)) < radius) return true;
    }
    return false;
}

std::vector<glm::vec2> SurvivalNight::LitLamps() const {
    std::vector<glm::vec2> lamps;
    if (StreetLightLevel() < 0.5f) return lamps;
    for (size_t i = 0; i < kStreetLamps.size(); ++i) {
        if (LampAlive(i) > 0.5f) lamps.emplace_back(kStreetLamps[i].x, kStreetLamps[i].z);
    }
    lamps.emplace_back(4.28f, 26.57f);
    lamps.emplace_back(-8.88f, 164.76f);
    std::erase_if(lamps, [this](const glm::vec2& lamp) { return lamp.y > north_end_ - 1.0f; });
    return lamps;
}

float SurvivalNight::ToTheLight(const glm::vec3& p) const {
    if (InFlareLight(p)) return 0.0f;
    constexpr float kLit = 12.0f;
    float best = 1e9f;
    for (const glm::vec2& lamp : LitLamps()) best = std::min(best, std::max(0.0f, glm::distance(lamp, glm::vec2(p.x, p.z)) - kLit));
    return best;
}

float SurvivalNight::StreetLightLevel() const {
    return std::min(BlackoutLevel(), LampsLevel());
}

float SurvivalNight::LampsLevel() const {
    const float out = static_cast<float>(lamps_out_) + (lamp_failing_ >= 0.0f ? Smooth(lamp_failing_ / 1.5f) : 0.0f);
    return 1.0f - 0.17f * out;
}

float SurvivalNight::GlowLevel() const {
    const float out = static_cast<float>(lamps_out_) + (lamp_failing_ >= 0.0f ? Smooth(lamp_failing_ / 1.5f) : 0.0f);
    return std::max(0.0f, 1.0f - out / static_cast<float>(kSurvivalLamps));
}

float SurvivalNight::LampAlive(size_t i) const {
    const size_t rank = static_cast<size_t>(std::find(kLampsOut.begin(), kLampsOut.end(), i) - kLampsOut.begin());
    if (rank < static_cast<size_t>(lamps_out_)) return 0.0f;
    if (rank == static_cast<size_t>(lamps_out_) && lamp_failing_ >= 0.0f) {
        const float h = std::sin(lamp_failing_ * 41.0f) * 43758.5453f;
        return (h - std::floor(h) > 0.4f ? 1.0f : 0.1f) * (1.0f - 0.6f * lamp_failing_ / 1.5f);
    }
    return 1.0f;
}

void SurvivalNight::UpdateLamps(float dt) {
    if (lamp_failing_ >= 0.0f) {
        lamp_failing_ += dt;
        if (lamp_failing_ >= 1.5f) {
            lamp_failing_ = -1.0f;
            const glm::vec3 at = kStreetLamps[kLampsOut[static_cast<size_t>(std::min(lamps_out_, kSurvivalLamps - 1))]];
            lamps_out_ = std::min(lamps_out_ + 1, kSurvivalLamps);
            Play(lights_down_, &at);
        }
    } else if (lamp_fail_at_ >= 0.0f && phase_ == SurvivalPhase::Wave && wave_time_ >= lamp_fail_at_ && lamps_out_ < kSurvivalLamps) {
        lamp_fail_at_ = -1.0f;
        lamp_failing_ = 0.0f;
        const glm::vec3 at = kStreetLamps[kLampsOut[static_cast<size_t>(lamps_out_)]];
        Play(lights_fail_, &at);
        LogInfo("survival: the street lamp at ({:.1f} {:.1f}) goes out ({} of {})", at.x, at.z, lamps_out_ + 1, kSurvivalLamps);
    }
}

float SurvivalNight::BlackoutLevel() const {
    if (blackout_time_ < 0.0f) return 1.0f;
    if (boss_flash_ > 0.0f) return 0.75f * std::min(1.0f, boss_flash_ / 0.1f);
    const float t = blackout_time_;
    auto flicker = [](float x, float rate) {
        const float h = std::sin(x * rate) * 43758.5453f;
        return h - std::floor(h) > 0.45f ? 1.0f : 0.15f;
    };
    if (t < 1.2f) return flicker(t, 37.0f) * (1.0f - 0.5f * t / 1.2f);
    const float back = t - 1.2f - blackout_length_;
    if (back < 0.0f) return 0.02f;
    if (back < 1.5f) return flicker(back, 29.0f) * std::min(1.0f, 0.3f + back / 1.5f);
    return 1.0f;
}

glm::vec3 SurvivalNight::HellTint() const {
    return glm::mix(glm::vec3(1.0f), glm::vec3(1.9f, 0.1f, 0.06f), Smooth(hell_level_));
}

void SurvivalNight::DimStreetGlow(float level, const glm::vec3& tint) {
    TextureManager& table = game_.Models().Textures();
    const uint32_t count = table.MaterialCount();
    const bool tinted = glm::distance(tint, glm::vec3(1.0f)) > 1.0e-3f;
    const bool changed = level < 0.999f || tinted;
    if (std::abs(level - dimmed_level_) < 1.0e-3f && glm::distance(tint, dimmed_tint_) < 1.0e-3f &&
        (!changed ? dimmed_.empty() || blackout_time_ >= 0.0f : dimmed_scanned_ == count)) return;
    if (changed) {
        const int32_t lens = game_.HandyLensMaterial();
        for (uint32_t i = dimmed_scanned_; i < count; ++i) {
            const MaterialGpu* m = table.Material(i);
            const bool own = (i >= own_materials_begin_ && i < own_materials_end_) || static_cast<int32_t>(i) == lens;
            if (!own && m->kind == gpu::kKindConstant && m->params.y > 0.0f) dimmed_.push_back({i, m->params.y, m->albedo_factor});
        }
        dimmed_scanned_ = count;
    }
    for (const Dimmed& d : dimmed_) {
        MaterialGpu m = *table.Material(d.index);
        m.params.y = d.strength * std::min(level, 1.0f);
        m.albedo_factor = glm::vec4(glm::vec3(d.colour) * tint, d.colour.a);
        table.UpdateMaterial(d.index, m);
    }
    if (!changed && blackout_time_ < 0.0f) {
        if (!dimmed_.empty()) LogInfo("survival: the street's {} glowing materials lit again", dimmed_.size());
        dimmed_.clear();
        dimmed_scanned_ = 0;
    } else if (level < 0.999f && dimmed_level_ >= 0.999f && blackout_time_ < 0.1f) {
        LogInfo("survival: the street's {} glowing materials dimmed", dimmed_.size());
    }
    dimmed_level_ = level;
    dimmed_tint_ = tint;
}

void SurvivalNight::MatchCrossStreet(bool on) {
    constexpr const char* kMap = "/Assets/sh/environ/object/shsb/map/shsb_mapp001/scenes/shsb_mapp001.fmdl";
    TextureManager& table = game_.Models().Textures();
    if (!on) {
        if (cross_street_material_ != kNoCrossStreet) table.UpdateMaterial(cross_street_material_, cross_street_own_);
        cross_street_material_ = kNoCrossStreet;
        if (street_material_ != kNoCrossStreet) table.UpdateMaterial(street_material_, street_own_);
        street_material_ = kNoCrossStreet;
        for (const auto& [index, own] : street_fine_) table.UpdateMaterial(index, own);
        street_fine_.clear();
        if (!road_own_.empty()) {
            if (const ModelEntry* map = game_.Models().Get(kMap); map && map->mesh) {
                gpu::UploadBuffer(game_.Models().Scene().Gpu(), map->mesh->vertices, road_own_.data(), road_own_.size() * sizeof(Vertex));
            }
            road_own_.clear();
        }
        return;
    }
    if (cross_street_material_ != kNoCrossStreet) return;
    constexpr size_t kCrossStreet = 4, kStreet = 15;
    const ModelEntry* map = game_.Models().Get(kMap);
    if (!map || !map->mesh || map->mesh->submeshes.size() <= kStreet) return;
    const uint32_t cross = map->mesh->submeshes[kCrossStreet].material;
    const MaterialGpu* own = table.Material(cross);
    const MaterialGpu* street = table.Material(map->mesh->submeshes[kStreet].material);
    if (!own || !street || street->aux1 != own->normal || own == street) {
        LogWarn("survival: the street map's cross street and street are not its meshes {} and {}; the cross street keeps its asphalt", kCrossStreet, kStreet);
        return;
    }
    cross_street_own_ = *own;
    cross_street_material_ = cross;
    table.UpdateMaterial(cross, *street);
    LogInfo("survival: the cross street (material {}) shows the street's asphalt", cross);
    FmdlModel model;
    const auto file = game_.GetVfs().ReadFile(kMap);
    if (!file || !LoadFmdl(std::span<const uint8_t>(file->data(), file->size()), "survival road", model) ||
        model.mesh.submeshes.size() != map->mesh->submeshes.size()) {
        LogWarn("survival: the street map could not be read again, its road keeps the joints of its strips");
        return;
    }
    road_own_ = model.mesh.vertices;
    constexpr float kPlacements = 107.908722f;
    constexpr float kTile = kStreetTile;
    constexpr float kRise = 0.001f / kPlacements;
    size_t mapped = 0;
    std::vector<bool> done(model.mesh.vertices.size(), false);
    for (const size_t part : {kCrossStreet, kStreet}) {
        const SubMesh& sub = model.mesh.submeshes[part];
        for (uint32_t i = sub.first_index; i < sub.first_index + sub.index_count; ++i) {
            const size_t at = static_cast<size_t>(static_cast<int64_t>(model.mesh.indices[i]) + sub.vertex_offset);
            if (at >= done.size() || done[at]) continue;
            done[at] = true;
            Vertex& v = model.mesh.vertices[at];
            v.uv0 = glm::vec2(v.position.x, v.position.z) / kTile;
            v.position.y += kRise * (v.position.z + 14.0f) - 0.001f;
            v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            ++mapped;
        }
    }
    gpu::UploadBuffer(game_.Models().Scene().Gpu(), map->mesh->vertices, model.mesh.vertices.data(), model.mesh.vertices.size() * sizeof(Vertex));
    street_material_ = map->mesh->submeshes[kStreet].material;
    street_own_ = *table.Material(street_material_);
    MaterialGpu road = street_own_;
    road.flags |= gpu::kMatBlendTile;
    table.UpdateMaterial(street_material_, road);
    table.UpdateMaterial(cross, road);
    LogInfo("survival: the road's {} vertices mapped from above, one tile every {:.1f} m", mapped, kTile);
    auto keep_fine = [&](uint32_t index) {
        if (index == cross || index == street_material_) return;
        if (std::any_of(street_fine_.begin(), street_fine_.end(), [&](const auto& f) { return f.first == index; })) return;
        const MaterialGpu* stone = table.Material(index);
        if (!stone) return;
        street_fine_.emplace_back(index, *stone);
        MaterialGpu fine = *stone;
        fine.flags |= gpu::kMatFineSpecular;
        table.UpdateMaterial(index, fine);
    };
    for (const SubMesh& sub : map->mesh->submeshes) keep_fine(sub.material);
    for (const char* car : {"/Assets/sh/environ/object/shsb/car/shsb_carr001/scenes/shsb_carr001.fmdl",
                            "/Assets/sh/environ/object/shsb/car/shsb_carr001/scenes/shsb_carr001_vrtn001.fmdl"}) {
        if (!game_.GetVfs().ReadFile(car)) continue;
        if (const ModelEntry* model = game_.Models().Get(car)) {
            for (const uint32_t index : model->material_indices) keep_fine(index);
        }
    }
    LogInfo("survival: {} of the street's materials keep their highlights to the pixel", street_fine_.size());
}

void SurvivalNight::SettleStreetLayers(bool on) {
    gpu::Device& device = game_.Models().Scene().Gpu();
    if (!on) {
        for (const auto& [path, own] : settled_own_) {
            if (const ModelEntry* entry = game_.Models().Get(path); entry && entry->mesh) gpu::UploadBuffer(device, entry->mesh->vertices, own.data(), own.size() * sizeof(Vertex));
        }
        settled_own_.clear();
        return;
    }
    if (!settled_own_.empty()) return;
    const std::pair<const char*, glm::vec3> kMoves[] = {
        {"/Assets/sh/environ/object/shsb/garbage/shsb_grbg004/scenes/shsb_grbg004.fmdl", {0.0f, 0.003f, 0.0f}},
        {"/Assets/sh/environ/object/shsb/wall/shsb_wall001/scenes/shsb_wall001_vrtn001.fmdl", {0.0f, 0.0f, -0.003f}},
    };
    for (const auto& [path, by] : kMoves) {
        const ModelEntry* entry = game_.Models().Get(path);
        const auto file = game_.GetVfs().ReadFile(path);
        FmdlModel model;
        if (!entry || !entry->mesh || !file || !LoadFmdl(std::span<const uint8_t>(file->data(), file->size()), "survival street", model)) {
            LogWarn("survival: {} could not be moved off what it stands on", path);
            continue;
        }
        settled_own_.emplace_back(path, model.mesh.vertices);
        for (Vertex& v : model.mesh.vertices) v.position += by;
        gpu::UploadBuffer(device, entry->mesh->vertices, model.mesh.vertices.data(), model.mesh.vertices.size() * sizeof(Vertex));
    }
}

void SurvivalNight::DressBareThings(bool on) {
    TextureManager& table = game_.Models().Textures();
    if (!on) {
        for (const auto& [index, own] : bare_materials_) table.UpdateMaterial(index, own);
        bare_materials_.clear();
        return;
    }
    if (!bare_materials_.empty()) return;
    struct Dress {
        const char* model;
        const char* material;
        const char* stem;
        const char* base;
    };
    static constexpr const char* kMetal = "/Assets/sh/environ/object/shsb/can/shsb_cann001/sourceimages/shsb_cann001";
    static constexpr Dress kDress[] = {
        {"/Assets/sh/environ/object/shsb/case/shsb_case001/scenes/shsb_case001_vrtn001.fmdl", "", kMetal, "_bsm"},
        {"/Assets/sh/environ/object/shsb/cat_walk/shsb_ctwk001/scenes/shsb_ctwk001.fmdl", "", kMetal, "_bsm"},
        {"/Assets/sh/environ/object/shsb/foliage_plant/shsb_flpl001/scenes/shsb_flpl001.fmdl", "leaf",
         "/Assets/sh/environ/object/shsb/tree/shsb_tree001/sourceimages/shsb_tree001_le_02", "_bsm_alp"},
        {"/Assets/sh/environ/object/shsb/foliage_plant/shsb_flpl001/scenes/shsb_flpl001.fmdl", "adobe",
         "/Assets/sh/environ/object/shsb/carton/shsb_crto001/sourceimages/shsb_crto001_002", "_bsm"},
        {"/Assets/sh/environ/object/shsb/foliage_plant/shsb_flpl001/scenes/shsb_flpl001.fmdl", "iron", kMetal, "_bsm"},
    };
    const QarArchive& qar = game_.GetVfs().Textures();
    auto load = [&](const std::string& path, uint32_t fallback) {
        bool ok = false;
        const uint32_t index = table.LoadFox(qar, path, &ok);
        return ok ? index : fallback;
    };
    for (const Dress& d : kDress) {
        if (!game_.GetVfs().ReadFile(d.model)) continue;
        const ModelEntry* model = game_.Models().Get(d.model);
        if (!model) continue;
        for (size_t k = 0; k < model->materials.size() && k < model->material_indices.size(); ++k) {
            if (*d.material && !model->materials[k].name.starts_with(d.material)) continue;
            const uint32_t index = model->material_indices[k];
            const MaterialGpu* own = table.Material(index);
            if (!own || own->albedo != TextureManager::kGreySrgb) continue;
            if (std::any_of(bare_materials_.begin(), bare_materials_.end(), [&](const auto& b) { return b.first == index; })) continue;
            MaterialGpu dressed = *own;
            const std::string stem = d.stem;
            dressed.albedo = load(stem + d.base, own->albedo);
            dressed.normal = load(stem + (stem == kMetal ? "_hnm" : "_nrm"), own->normal);
            if (dressed.normal != TextureManager::kFlatNormal && dressed.normal != TextureManager::kGrey) dressed.flags |= gpu::kMatNormalMap;
            dressed.specular = load(stem + "_srm", own->specular);
            bare_materials_.emplace_back(index, *own);
            table.UpdateMaterial(index, dressed);
            LogInfo("survival: {}'s {} dressed in {}", d.model, model->materials[k].name, stem);
        }
    }
}

void SurvivalNight::ApplyStreetEvents(SceneLighting& lighting) const {
    if (!active_) return;
    lighting.screen.wide_shadow_limit = true;
    const glm::vec3 fog_own = lighting.tpp.fog_self;
    auto street_light = [](const SceneLight& l) {
        return !(l.name.empty() || l.name == "LisaHintGlint" || l.name == "IntercomLight" || l.id == kMirrorLightId ||
                 l.id == kHandyLightId || (l.id >= kHandyReflectionId && l.id < kHandyReflectionId + 4) ||
                 l.name.find("ChrSpot") != std::string::npos);
    };
    if (hud_.offer_count > 0) {
        const float k = hud_.offer_taken < 0 ? Smooth(hud_.offer_age / 0.35f) : 1.0f - Smooth(hud_.taken_age / 0.4f);
        if (k > 0.01f) {
            lighting.screen.depth_of_field = true;
            if (BodyHoldsGun()) {
                const Camera eye = game_.ViewCamera(true);
                const glm::mat4 body = game_.GetPlayer().PoseTransform();
                const float along = std::max(0.5f, glm::dot(glm::vec3(body * glm::vec4(0.0f, 1.2f, 0.0f, 1.0f)) - eye.position, eye.Forward()));
                lighting.screen.focus_distance = along;
                lighting.screen.dof_flags |= 1u;
            } else {
                lighting.screen.focus_distance = glm::mix(lighting.screen.focus_distance, 0.5f, k);
            }
            lighting.screen.focal_length = glm::mix(lighting.screen.focal_length, 85.0f, k);
            lighting.screen.aperture = glm::mix(lighting.screen.aperture, 1.0f, k);
        }
    }
    lighting.screen.black_lift = glm::mix(glm::vec3(0.012f, 0.014f, 0.019f), glm::vec3(0.04f, 0.009f, 0.009f), Smooth(hell_level_));
    if (close_body_ > 0.01f) lighting.screen.focus_distance = glm::mix(lighting.screen.focus_distance, close_body_at_, close_body_);
    if (studio_) {
        for (SceneLight& l : lighting.lights) {
            if (street_light(l)) l.intensity = glm::vec3(0.0f);
        }
        lighting.tpp.fog_self = glm::vec3(0.0f);
    }
    if (fog_level_ > 0.0f) {
        const float f = Smooth(fog_level_);
        lighting.tpp.fog_density *= 1.0f + 3.0f * f;
        lighting.tpp.fog_self *= 1.0f + 2.0f * f;
        lighting.tpp.fog_far = glm::mix(lighting.tpp.fog_far, std::min(lighting.tpp.fog_far, 25.0f), f);
    }
    if (siren_level_ > 0.0f) {
        const float s = Smooth(siren_level_);
        const glm::vec3 rust = glm::mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.5f, 0.36f), s);
        for (SceneLight& l : lighting.lights) {
            if (street_light(l)) l.intensity *= rust;
        }
        lighting.tpp.fog_self *= rust;
        lighting.screen.color_scale *= glm::mix(glm::vec3(1.0f), glm::vec3(1.12f, 0.88f, 0.78f), s);
    }
    for (SceneLight& l : lighting.lights) {
        if (l.type == LightType::Spot && l.cast_shadow && street_light(l) && l.position.y < 5.0f && std::abs(l.direction.y) < 0.35f) {
            static const float film = [] { const char* g = std::getenv("PT_SURVIVAL_FILM"); return g ? static_cast<float>(std::atof(g)) : 0.4f; }();
            l.cast_shadow = false;
            l.fill = true;
            l.intensity *= film;
            l.specular_scale = 0.0f;
        }
        if (l.type == LightType::Point && street_light(l) && l.position.y < 3.0f && l.outer_range <= 12.0f) {
            l.specular_scale = 0.0f;
            l.fill = true;
        }
    }
    if (hell_level_ > 0.0f) {
        const glm::vec3 blood = HellTint();
        for (SceneLight& l : lighting.lights) {
            if (street_light(l)) l.intensity *= blood;
        }
        for (SceneProbe& p : lighting.probes) {
            for (glm::vec3& c : p.sh) c *= blood;
        }
        lighting.tpp.fog_self *= blood;
        lighting.tpp.dir_color *= blood;
        lighting.stage_glow *= blood;
        lighting.screen.color_scale *= glm::mix(glm::vec3(1.0f), glm::vec3(1.1f, 0.86f, 0.84f), Smooth(hell_level_));
    }
    for (SceneLight& l : lighting.lights) {
        if (l.id != kHandyLightId) continue;
        l.outer_range = std::max(l.outer_range, 14.0f) * 1.4f;
        float crit_flicker = 1.0f;
        if (!hud_.dead && !waking_ && max_health_ > 0.0f && health_ < max_health_ * 0.25f) {
            const float low = 1.0f - std::clamp(health_ / (max_health_ * 0.25f), 0.0f, 1.0f);
            const float wave_a = std::sin(time_ * 27.0f) * std::sin(time_ * 43.0f);
            const float dip = wave_a > 0.72f ? 0.55f : 0.90f + 0.10f * std::sin(time_ * 19.0f);
            crit_flicker = glm::mix(1.0f, dip, 0.45f + 0.55f * low);
        }
        l.intensity *= 1.25f * torch_tint_ * crit_flicker;
        l.specular_scale *= 1.0f - 0.75f * Smooth(hell_level_);
        l.inner_range = std::max(l.inner_range, 1.1f);
    }
    const float level = StreetLightLevel();
    if (level >= 0.999f) return;
    static const float torch = [] { const char* g = std::getenv("PT_SURVIVAL_BLACKOUT_TORCH"); return g ? static_cast<float>(std::atof(g)) : 32.0f; }();
    static const glm::vec2 reach = [] {
        glm::vec2 r(5.0f, 45.0f);
        if (const char* g = std::getenv("PT_SURVIVAL_BLACKOUT_REACH")) std::sscanf(g, "%f %f", &r.x, &r.y);
        return r;
    }();
    const float dark = 1.0f - level;
    for (SceneLight& l : lighting.lights) {
        if (l.id == kHandyLightId) {
            l.inner_range = glm::mix(l.inner_range, std::max(l.inner_range, reach.x), dark);
            l.outer_range = glm::mix(l.outer_range, std::max(l.outer_range, reach.y), dark);
        }
        if (l.id == kHandyLightId || (l.id >= kHandyReflectionId && l.id < kHandyReflectionId + 4)) {
            l.intensity *= 1.0f + (torch - 1.0f) * dark;
        } else if (street_light(l)) {
            l.intensity *= l.name == "SurvivalLamp" ? BlackoutLevel() : level;
        }
    }
    lighting.stage_glow *= std::min(level, GlowLevel());
    const float baked = 0.12f + 0.88f * level;
    for (SceneProbe& p : lighting.probes) {
        for (glm::vec3& c : p.sh) c *= baked;
    }
    lighting.tpp.fog_self *= baked;
    lighting.tpp.dir_color *= 0.5f + 0.5f * level;
    if (lamps_out_ >= kSurvivalLamps) {
        static const float cold = [] { const char* g = std::getenv("PT_SURVIVAL_COLD_FOG"); return g ? static_cast<float>(std::atof(g)) : 6.0f; }();
        const float own = glm::dot(fog_own, glm::vec3(0.3f, 0.59f, 0.11f));
        lighting.tpp.fog_self = glm::max(lighting.tpp.fog_self, glm::vec3(0.62f, 0.74f, 0.92f) * (cold * own));
    }
}

void SurvivalNight::AddLights(SceneLighting& lighting) const {
    if (!active_) return;
    auto point = [&](uint64_t id, const glm::vec3& at, const glm::vec3& intensity, float range, float diffuse) {
        SceneLight l;
        l.id = id;
        l.type = LightType::Point;
        l.position = at;
        l.intensity = intensity;
        l.inner_range = 0.0f;
        l.outer_range = range;
        l.dimmer = 0.05f;
        l.diffuse_scale = diffuse;
        l.cast_shadow = false;
        lighting.lights.push_back(l);
    };
    if (muzzle_ > 0.0f) {
        const float k = muzzle_ / 0.05f;
        static const float gain = [] { const char* g = std::getenv("PT_SURVIVAL_MUZZLE"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
        const float strength = (weapon_ == SurvivalWeapon::Handgun ? 220.0f : 380.0f) * gain;
        point(kSourceBase + 1, muzzle_at_, glm::vec3(1.0f, 0.6f, 0.28f) * (strength * k), 7.0f, 1.0f);
    }
    if (weapon_ != SurvivalWeapon::Flashlight && !hud_.dead && !caught_ && !waking_) {
        const int skin_id = game_.SurvivalProfileData().EquippedSkin(static_cast<int>(weapon_));
        const SurvivalSkinDef& skin = SurvivalSkin(skin_id);
        float hover = 0.0f;
        if (reload_timer_ > 0.0f && reload_length_ > 0.05f && ActiveReloadStyle() == ReloadStyle::Telekinetic) {
            const float p = std::clamp(1.0f - reload_timer_ / reload_length_, 0.0f, 1.0f);
            hover = Smooth((p - 0.05f) / 0.14f) * Smooth((0.95f - p) / 0.14f);
        }
        const bool is_mythic = skin.rarity == SkinRarity::Mythic;
        if (is_mythic || hover > 0.01f) {
            const Camera cam = game_.ViewCamera(true);
            const glm::mat4 gw = BodyHoldsGun() ? DrawnWeaponTransform(cam) : WeaponTransform(cam);
            const glm::vec3 pos = glm::vec3(gw[3]) + glm::vec3(gw[2]) * 0.14f + glm::vec3(gw[1]) * 0.05f;
            const glm::vec3 col = is_mythic ? MythicLiveColor(skin_id, time_) : skin.aura_color;
            const float idle_pulse = 0.72f + 0.28f * std::sin(time_ * (skin_id == 54 ? 17.0f : 3.6f));
            const float gain = (is_mythic ? 36.0f * idle_pulse : 0.0f) + 145.0f * hover * (0.80f + 0.20f * std::sin(time_ * 26.0f));
            point(kSourceBase + 3, pos, col * gain, is_mythic ? 2.4f + 1.4f * hover : 3.6f, 1.0f);
        }
    }
    if (burst_light_ > 0.0f) {
        const float k = Smooth(burst_light_ / 0.35f);
        SceneLight l;
        l.id = kSourceBase + 2;
        l.type = LightType::Spot;
        l.position = burst_at_;
        l.direction = burst_dir_;
        const glm::vec3 side = std::abs(burst_dir_.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        l.up = glm::normalize(glm::cross(glm::cross(burst_dir_, side), burst_dir_));
        l.intensity = glm::vec3(0.85f, 0.92f, 1.0f) * (260000.0f * k);
        l.inner_range = 0.05f;
        l.outer_range = 16.0f;
        l.cos_outer = std::cos(0.55f);
        l.inv_cone_range = 1.0f / std::max(1e-4f, std::cos(0.2f) - l.cos_outer);
        l.cone_exponent = 1.0f;
        l.cast_shadow = false;
        lighting.lights.push_back(l);
    }
    static const float lamp_gain = [] { const char* g = std::getenv("PT_SURVIVAL_LAMP"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
    const glm::vec3 feet = game_.GetPlayer().Feet();
    for (size_t i = 0; i < kStreetLamps.size(); ++i) {
        const glm::vec3 head = kStreetLamps[i];
        if (glm::distance(glm::vec2(head.x, head.z), glm::vec2(feet.x, feet.z)) > 40.0f) continue;
        const float alive = LampAlive(i);
        if (alive <= 0.0f) continue;
        SceneLight l;
        l.id = kSourceBase + 32 + i;
        l.name = "SurvivalLamp";
        l.type = LightType::Spot;
        l.position = head - glm::vec3(0.0f, 0.2f, 0.0f);
        l.direction = glm::vec3(0.0f, -1.0f, 0.0f);
        l.up = glm::vec3(0.0f, 0.0f, 1.0f);
        const bool mercury = i == 0;
        l.intensity = mercury ? glm::vec3(0.62f, 0.86f, 1.0f) * (27000.0f * lamp_gain) : glm::vec3(1.0f, 0.6f, 0.26f) * (38000.0f * lamp_gain);
        l.intensity *= alive;
        l.inner_range = 0.05f;
        l.outer_range = 22.0f;
        l.dimmer = 1.0f;
        constexpr float kSpread = 1.19f, kFull = 0.7f;
        l.cos_outer = std::cos(kSpread);
        l.inv_cone_range = 1.0f / (std::cos(kFull) - l.cos_outer);
        l.cone_exponent = 1.0f;
        l.shadow_cos_outer = l.cos_outer;
        l.shadow_inv_cone_range = l.inv_cone_range;
        l.shadow_fov = 2.0f * kSpread;
        l.view_bias = -0.01f;
        l.source_radius = 0.12f;
        l.cast_shadow = true;
        l.priority = 0x80;
        lighting.lights.push_back(l);
    }
    AddTaskLights(lighting);
    {
        std::array<std::pair<float, size_t>, 6> order{};
        size_t flare_count = 0;
        for (size_t i = 0; i < flares_out_.size() && flare_count < order.size(); ++i) {
            const glm::vec3 off = flares_out_[i].position - feet;
            order[flare_count++] = {glm::dot(off, off), i};
        }
        std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(flare_count));
        for (size_t rank = 0; rank < flare_count; ++rank) {
            const size_t i = order[rank].second;
            const BurningFlare& f = flares_out_[i];
            const float phase = static_cast<float>(f.id) * 1.7f;
            const float flicker = std::clamp(0.84f + 0.14f * std::sin(time_ * 23.0f + phase) + 0.10f * std::sin(time_ * 41.0f + phase * 2.3f) +
                                             0.08f * std::sin(time_ * 7.3f + phase * 0.7f) + 0.06f * std::sin(time_ * 67.0f + phase * 4.1f),
                                             0.58f, 1.25f);
            const float alive = std::clamp(f.life - 1.0f - f.age, 0.0f, 1.0f) * Smooth(f.age / 0.15f);
            if (alive <= 0.0f) continue;
            const glm::vec3 jitter(0.028f * std::sin(time_ * 31.0f + phase), 0.015f * std::sin(time_ * 47.0f + phase * 1.4f),
                                   0.028f * std::cos(time_ * 29.0f + phase * 2.1f));
            const glm::vec3 tip = f.position + glm::vec3(f.rotation * glm::vec4(0.0f, 0.0f, 0.1f, 0.0f)) + glm::vec3(0.0f, 0.38f, 0.0f) + jitter;
            SceneLight l;
            l.id = kSourceBase + 48 + (f.id & 0xFFu);
            l.type = LightType::Point;
            l.position = tip;
            l.intensity = glm::vec3(1.0f, 0.082f, 0.035f) * (105000.0f * flicker * alive);
            l.inner_range = 0.55f;
            l.outer_range = 28.0f;
            l.dimmer = 0.85f;
            l.diffuse_scale = 1.0f;
            l.specular_scale = 0.45f;
            l.source_radius = 0.08f;
            l.cast_shadow = rank < 2;
            l.shadow_strength = 1.0f;
            l.shadow_bias = 0.04f;
            l.view_bias = -0.01f;
            l.priority = 0x50;
            l.fill = false;
            lighting.lights.push_back(l);
            SceneLight bounce;
            bounce.id = kSourceBase + 320 + (f.id & 0xFFu);
            bounce.type = LightType::Point;
            bounce.position = tip + glm::vec3(0.0f, 1.55f, 0.0f);
            bounce.intensity = glm::vec3(1.0f, 0.08f, 0.04f) * (26000.0f * (0.85f + 0.15f * flicker) * alive);
            bounce.inner_range = 1.6f;
            bounce.outer_range = 24.0f;
            bounce.dimmer = 1.0f;
            bounce.diffuse_scale = 1.0f;
            bounce.specular_scale = 0.15f;
            bounce.cast_shadow = false;
            bounce.fill = true;
            lighting.lights.push_back(bounce);
        }
    }
    if (hand_flare_lit_ && throw_timer_ >= 0.1f && !thrown_) {
        const float k = Smooth((throw_timer_ - 0.1f) / 0.08f);
        const float hand_flicker = 0.88f + 0.12f * std::sin(time_ * 33.0f);
        SceneLight l;
        l.id = kSourceBase + 47;
        l.type = LightType::Point;
        l.position = hand_flare_at_ + glm::vec3(0.0f, 0.06f, 0.0f);
        l.intensity = glm::vec3(1.0f, 0.13f, 0.065f) * (4500.0f * hand_flicker * k);
        l.inner_range = 0.12f;
        l.outer_range = 10.0f;
        l.dimmer = 0.30f;
        l.diffuse_scale = 1.0f;
        l.specular_scale = 1.0f;
        l.cast_shadow = false;
        lighting.lights.push_back(l);
    }
    {
        const glm::vec3 feet = game_.GetPlayer().Feet();
        std::array<std::pair<float, size_t>, 8> closest{};
        size_t count = 0;
        for (size_t i = 0; i < pickups_.size(); ++i) {
            const glm::vec3 off = pickups_[i].position - feet;
            const std::pair<float, size_t> one(glm::dot(off, off), i);
            if (count < closest.size()) {
                closest[count++] = one;
            } else if (const auto last = std::max_element(closest.begin(), closest.end()); one < *last) {
                *last = one;
            }
        }
        std::sort(closest.begin(), closest.begin() + static_cast<std::ptrdiff_t>(count), [](const auto& a, const auto& b) { return a.second < b.second; });
        for (size_t i = 0; i < count; ++i) {
            point(kSourceBase + 16 + i, pickups_[closest[i].second].position + glm::vec3(0.0f, 0.3f, 0.0f), glm::vec3(0.8f, 0.86f, 0.95f) * 30.0f, 1.8f, 1.0f);
        }
    }
    if (weapon_ != SurvivalWeapon::Flashlight && !hud_.dead && !BodyHoldsGun()) {
        const float street = 0.2f + 0.8f * StreetLightLevel();
        const float strength = (0.6f - 0.3f * Smooth(aim_)) * street;
        const glm::mat4 gun = WeaponTransform(game_.ViewCamera(true));
        static const float fill = [] { const char* g = std::getenv("PT_SURVIVAL_FILL"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
        static const glm::vec3 fill_at = [] {
            glm::vec3 at(0.0f, 0.35f, 0.6f);
            if (const char* g = std::getenv("PT_SURVIVAL_FILL_AT")) std::sscanf(g, "%f %f %f", &at.x, &at.y, &at.z);
            return at;
        }();
        point(kSourceBase + 3, glm::vec3(gun * glm::vec4(fill_at, 1.0f)), glm::vec3(0.9f, 0.86f, 0.8f) * (60.0f * fill * strength), 1.2f, 1.0f);
        static const float glint = [] { const char* g = std::getenv("PT_SURVIVAL_GLINT"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
        static const glm::vec3 glint_at = [] {
            glm::vec3 at(0.25f, 0.05f, 0.6f);
            if (const char* g = std::getenv("PT_SURVIVAL_GLINT_AT")) std::sscanf(g, "%f %f %f", &at.x, &at.y, &at.z);
            return at;
        }();
        const float sheen = (1.0f - 0.6f * Smooth(aim_)) * (1.0f - Smooth(gun_.tilt)) * (1.0f - Smooth(gun_.lever)) * street;
        point(kSourceBase + 4, glm::vec3(gun * glm::vec4(glint_at, 1.0f)), glm::vec3(0.75f, 0.8f, 0.9f) * (90.0f * glint * sheen), 1.0f, 1.0f);
        static const float edge = [] { const char* g = std::getenv("PT_SURVIVAL_EDGE"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
        static const glm::vec3 edge_at = [] {
            glm::vec3 at(0.3f, 0.3f, 0.05f);
            if (const char* g = std::getenv("PT_SURVIVAL_EDGE_AT")) std::sscanf(g, "%f %f %f", &at.x, &at.y, &at.z);
            return at;
        }();
        const Camera eye = game_.ViewCamera(true);
        point(kSourceBase + 6, eye.position - eye.Right() * edge_at.x + eye.Up() * edge_at.y + eye.Forward() * edge_at.z, glm::vec3(0.85f, 0.9f, 1.0f) * (140.0f * edge * (0.3f + 0.7f * street)),
              1.6f, 0.08f);
    }
    if (!studio_) {
        static const float near_gain = [] { const char* g = std::getenv("PT_SURVIVAL_NEAR"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
        const Camera view = game_.ViewCamera(true);
        const float street = StreetLightLevel();
        glm::vec3 flat_fwd(view.Forward().x, 0.0f, view.Forward().z);
        flat_fwd = glm::length(flat_fwd) > 1e-4f ? glm::normalize(flat_fwd) : glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 flat_right = glm::normalize(glm::cross(flat_fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 anchor(view.position.x, game_.GetPlayer().Feet().y + 1.65f, view.position.z);
        const float strength = glm::mix(2000.0f - 1100.0f * street, 2800.0f * (0.7f + 0.3f * street), Smooth(game_.ThirdPersonWeight()));
        const glm::vec3 tint = hell_level_ > 0.0f ? HellTint() * (1.0f - 0.45f * Smooth(hell_level_)) : glm::vec3(1.0f);
        point(kSourceBase + 5, anchor - flat_fwd * 2.5f + glm::vec3(0.0f, 1.2f, 0.0f), glm::vec3(1.0f, 0.84f, 0.68f) * tint * (strength * near_gain),
              glm::mix(7.0f, 16.0f, street), 1.0f);
        lighting.lights.back().fill = true;
        lighting.lights.back().specular_scale = 0.0f;
        if (const float third = Smooth(game_.ThirdPersonWeight()); third > 0.0f) {
            static const float sheen_gain = [] { const char* g = std::getenv("PT_SURVIVAL_SHEEN"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
            point(kSourceBase + 7, anchor + flat_right * 1.1f + glm::vec3(0.0f, 1.0f, 0.0f) + flat_fwd * 0.3f,
                  glm::vec3(0.9f, 0.93f, 1.0f) * tint * (3600.0f * third * sheen_gain * (0.3f + 0.7f * street) * (1.0f - 0.6f * Smooth(hell_level_))), 3.2f, 0.06f);
            glm::mat4 skull(1.0f);
            const Player& body = game_.GetPlayer();
            if (body.BoneModel("SKL_004_HEAD", skull)) {
                static const float hair_gain = [] { const char* g = std::getenv("PT_SURVIVAL_HAIR"); return g ? static_cast<float>(std::atof(g)) : 1.0f; }();
                const glm::vec3 crown = glm::vec3(body.PoseTransform() * skull * glm::vec4(0.0f, 0.1f, 0.0f, 1.0f));
                const glm::vec3 back = glm::normalize(view.position - crown);
                point(kSourceBase + 8, crown + back * 0.28f + glm::vec3(0.0f, 0.32f, 0.0f), glm::vec3(1.0f, 0.93f, 0.84f) * tint * (100.0f * third * hair_gain * (0.35f + 0.65f * street)), 0.75f, 1.0f);
            }
        }
    }
}

}
