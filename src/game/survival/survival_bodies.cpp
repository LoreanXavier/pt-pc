#include "game/survival/survival_bodies.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "game/player_animation.h"

namespace pt::game {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kCrawlStride = SurvivalPoser::kCrawlStride;
constexpr float kCrawlDuty = SurvivalPoser::kCrawlDuty;

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

glm::mat3 Basis(const glm::vec3& dir, const glm::vec3& up) {
    const glm::vec3 x = glm::normalize(dir);
    glm::vec3 z = glm::cross(x, up);
    z = glm::length(z) > 1e-6f ? glm::normalize(z) : glm::normalize(glm::cross(x, std::abs(x.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    return glm::mat3(x, glm::cross(z, x), z);
}

glm::quat Frame(const glm::vec3& from, const glm::vec3& from_up, const glm::vec3& to, const glm::vec3& to_up) {
    return glm::normalize(glm::quat_cast(Basis(to, to_up) * glm::transpose(Basis(from, from_up))));
}

glm::vec2 Gait(float phase, float stride, float lift) {
    const float p = phase - std::floor(phase);
    if (p < kCrawlDuty) return {stride * (0.5f - p / kCrawlDuty), 0.0f};
    const float u = (p - kCrawlDuty) / (1.0f - kCrawlDuty);
    return {stride * (-0.5f + Smooth(u)), lift * std::sin(kPi * u)};
}

}

SurvivalPoser::SurvivalPoser(const anim::Skeleton& skeleton) : skeleton_(skeleton) {
    auto find = [&](const char* name) { return skeleton_.FindName(name); };
    waist_ = find("SKL_000_WAIST");
    spine_ = find("SKL_001_SPINE");
    chest_ = find("SKL_002_CHEST");
    neck_ = find("SKL_003_NECK");
    head_ = find("SKL_004_HEAD");
    const char* kShoulder[] = {"SKL_010_LSHLD", "SKL_020_RSHLD"}, *kUpper[] = {"SKL_011_LUARM", "SKL_021_RUARM"};
    const char* kFore[] = {"SKL_012_LFARM", "SKL_022_RFARM"}, *kHand[] = {"SKL_013_LHAND", "SKL_023_RHAND"};
    const char* kThigh[] = {"SKL_030_LTHIGH", "SKL_040_RTHIGH"}, *kLeg[] = {"SKL_031_LLEG", "SKL_041_RLEG"};
    const char* kFoot[] = {"SKL_032_LFOOT", "SKL_042_RFOOT"}, *kToe[] = {"SKL_033_LTOE", "SKL_043_RTOE"};
    for (int s = 0; s < 2; ++s) {
        shoulder_[s] = find(kShoulder[s]);
        upper_arm_[s] = find(kUpper[s]);
        forearm_[s] = find(kFore[s]);
        hand_[s] = find(kHand[s]);
        thigh_[s] = find(kThigh[s]);
        leg_[s] = find(kLeg[s]);
        foot_[s] = find(kFoot[s]);
        toe_[s] = find(kToe[s]);
    }
    for (size_t i = 0; i < skeleton_.Size(); ++i) {
        const std::string& name = skeleton_.names[i];
        const size_t under = name.rfind('_');
        if (name.compare(0, 4, "SKL_") != 0 || under == std::string::npos || name.size() != under + 5 || name[under + 2] != 'F') continue;
        if (name[under + 1] != 'L' && name[under + 1] != 'R') continue;
        const int side = name[under + 1] == 'L' ? 0 : 1;
        const int joint = name[under + 4] - '0';
        if (joint >= 1 && joint <= 3) fingers_[side][joint - 1].push_back(static_cast<int>(i));
    }
    LogInfo("survival: poser fingers {} left, {} right", fingers_[0][0].size() + fingers_[0][1].size() + fingers_[0][2].size(),
            fingers_[1][0].size() + fingers_[1][1].size() + fingers_[1][2].size());
    valid_ = waist_ >= 0 && spine_ >= 0 && chest_ >= 0 && neck_ >= 0 && head_ >= 0;
    for (int s = 0; s < 2; ++s) {
        valid_ = valid_ && shoulder_[s] >= 0 && upper_arm_[s] >= 0 && forearm_[s] >= 0 && hand_[s] >= 0 && thigh_[s] >= 0 && leg_[s] >= 0 &&
                 foot_[s] >= 0 && toe_[s] >= 0;
    }
    pose_.Reset(skeleton_.Size());
    scale_.assign(skeleton_.Size(), glm::vec3(1.0f));
    rot_.assign(skeleton_.Size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    pos_.assign(skeleton_.Size(), glm::vec3(0.0f));
}

void SurvivalPoser::Solve() {
    const size_t n = skeleton_.Size();
    for (size_t i = 0; i < n; ++i) {
        const int16_t p = skeleton_.parents[i];
        const glm::vec3 local = skeleton_.bind_local[i] + pose_.offset[i];
        if (p >= 0 && static_cast<size_t>(p) < i) {
            const size_t q = static_cast<size_t>(p);
            rot_[i] = rot_[q] * pose_.rotation[i];
            pos_[i] = pos_[q] + rot_[q] * (local * scale_[q]);
        } else {
            rot_[i] = pose_.rotation[i];
            pos_[i] = local;
        }
    }
}

glm::quat SurvivalPoser::World(int bone) const {
    return bone >= 0 ? rot_[static_cast<size_t>(bone)] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

void SurvivalPoser::SetWorld(int bone, const glm::quat& rotation) {
    const int16_t p = skeleton_.parents[static_cast<size_t>(bone)];
    pose_.rotation[static_cast<size_t>(bone)] = glm::normalize(glm::inverse(World(p)) * rotation);
}

void SurvivalPoser::Orient(int bone, const glm::vec3& from, const glm::vec3& from_up, const glm::vec3& to, const glm::vec3& to_up) {
    SetWorld(bone, Frame(from, from_up, to, to_up));
    Solve();
}

void SurvivalPoser::TwoBone(int a, int b, int c, const glm::vec3& target, const glm::vec3& pole, const glm::vec3& bend) {
    const size_t ia = static_cast<size_t>(a), ib = static_cast<size_t>(b), ic = static_cast<size_t>(c);
    const glm::vec3 upper = (skeleton_.bind_local[ib] + pose_.offset[ib]) * scale_[ia];
    const glm::vec3 lower = (skeleton_.bind_local[ic] + pose_.offset[ic]) * scale_[ib];
    const float la = glm::length(upper), lb = glm::length(lower);
    const glm::vec3 root = pos_[ia];
    glm::vec3 to = target - root;
    float distance = glm::length(to);
    if (distance < 1e-4f || la < 1e-4f || lb < 1e-4f) return;
    const glm::vec3 dir = to / distance;
    distance = std::clamp(distance, std::abs(la - lb) + 1e-3f, (la + lb) * 0.999f);
    const float along = (la * la - lb * lb + distance * distance) / (2.0f * distance);
    const float h = std::sqrt(std::max(0.0f, la * la - along * along));
    glm::vec3 side = pole - dir * glm::dot(pole, dir);
    side = glm::length(side) > 1e-5f ? glm::normalize(side) : glm::normalize(glm::cross(dir, glm::vec3(1, 0, 0)));
    const glm::vec3 middle = root + dir * along + side * h;
    const glm::vec3 end = root + dir * distance;
    SetWorld(a, Frame(upper, bend, middle - root, side));
    Solve();
    SetWorld(b, Frame(lower, bend, end - middle, side));
    Solve();
}

void SurvivalPoser::Twitch(const glm::vec2& twitch) {
    if (twitch == glm::vec2(0.0f)) return;
    glm::quat& r = pose_.rotation[static_cast<size_t>(head_)];
    r = glm::normalize(r * glm::angleAxis(twitch.x, glm::vec3(0, 1, 0)) * glm::angleAxis(twitch.y, glm::vec3(0, 0, 1)));
}

void SurvivalPoser::WalkFrom(double frame, float leg_scale) {
    if (!PlayerClipPose(kClipFront, frame, skeleton_, pose_)) pose_.Reset(skeleton_.Size());
    if (leg_scale != 1.0f) {
        const float leg = skeleton_.bind_world[static_cast<size_t>(thigh_[0])].y - skeleton_.bind_world[static_cast<size_t>(foot_[0])].y;
        pose_.offset[static_cast<size_t>(waist_)].y += leg * (leg_scale - 1.0f);
        pose_.offset[static_cast<size_t>(waist_)] *= glm::vec3(leg_scale, 1.0f, leg_scale);
    }
}

bool SurvivalPoser::Doll(double frame, float reach, const glm::vec3& target, const glm::vec2& twitch, float raise, float slam, float quirk) {
    if (!valid_) return false;
    std::fill(scale_.begin(), scale_.end(), glm::vec3(1.0f));
    WalkFrom(frame, 1.0f);
    if (std::abs(quirk) > 1e-3f) {
        const float q = std::clamp(quirk, -1.0f, 1.0f);
        pose_.rotation[static_cast<size_t>(spine_)] =
            glm::normalize(pose_.rotation[static_cast<size_t>(spine_)] * glm::angleAxis(0.08f * std::abs(q), glm::vec3(1, 0, 0)) *
                           glm::angleAxis(0.06f * q, glm::vec3(0, 0, 1)));
        pose_.rotation[static_cast<size_t>(neck_)] =
            glm::normalize(pose_.rotation[static_cast<size_t>(neck_)] * glm::angleAxis(0.14f * q, glm::vec3(0, 0, 1)) *
                           glm::angleAxis(0.08f * q, glm::vec3(0, 1, 0)));
    }
    Solve();
    const float blow = Smooth(std::max(raise, slam));
    if (blow > 0.0f) {
        glm::quat& spine = pose_.rotation[static_cast<size_t>(spine_)];
        spine = glm::normalize(spine * glm::angleAxis(-0.25f * Smooth(raise) * (1.0f - Smooth(slam)) + 0.45f * Smooth(slam), glm::vec3(1, 0, 0)));
        Solve();
        for (int s = 0; s < 2; ++s) {
            const float side = s == 0 ? 1.0f : -1.0f;
            const glm::quat walked[] = {pose_.rotation[static_cast<size_t>(upper_arm_[s])], pose_.rotation[static_cast<size_t>(forearm_[s])]};
            const glm::vec3 shoulder = pos_[static_cast<size_t>(upper_arm_[s])];
            const glm::vec3 up = glm::vec3(side * 0.06f, shoulder.y + 0.62f, shoulder.z + 0.12f);
            const glm::vec3 down = target + glm::vec3(side * 0.08f, 0.0f, 0.0f);
            TwoBone(upper_arm_[s], forearm_[s], hand_[s], glm::mix(up, down, Smooth(slam)), glm::vec3(side * 1.0f, -0.3f, -0.3f), glm::vec3(0, 0, -1));
            pose_.rotation[static_cast<size_t>(upper_arm_[s])] = glm::normalize(glm::slerp(walked[0], pose_.rotation[static_cast<size_t>(upper_arm_[s])], blow));
            pose_.rotation[static_cast<size_t>(forearm_[s])] = glm::normalize(glm::slerp(walked[1], pose_.rotation[static_cast<size_t>(forearm_[s])], blow));
            Solve();
        }
        for (int s = 0; s < 2; ++s) {
            const float side = s == 0 ? 1.0f : -1.0f;
            for (int j = 0; j < 3; ++j) {
                for (const int f : fingers_[s][j]) {
                    glm::quat& r = pose_.rotation[static_cast<size_t>(f)];
                    r = glm::normalize(r * glm::angleAxis(-side * 0.9f * blow, glm::vec3(0, 0, 1)));
                }
            }
        }
        Twitch(twitch);
        Solve();
        return true;
    }
    if (reach > 0.0f) {
        for (int s = 0; s < 2; ++s) {
            const int chain[] = {upper_arm_[s], forearm_[s]};
            const glm::quat walked[] = {pose_.rotation[static_cast<size_t>(chain[0])], pose_.rotation[static_cast<size_t>(chain[1])]};
            const glm::vec3 shoulder = pos_[static_cast<size_t>(upper_arm_[s])];
            const float side = s == 0 ? 1.0f : -1.0f;
            const glm::vec3 aim = target + glm::vec3(side * 0.12f, 0.0f, 0.0f);
            const glm::vec3 dir = glm::normalize(aim - shoulder);
            TwoBone(upper_arm_[s], forearm_[s], hand_[s], shoulder + dir * 0.5f, glm::vec3(side * 0.4f, -1.0f, -0.2f), glm::vec3(0, 0, -1));
            const float w = Smooth(reach);
            for (int k = 0; k < 2; ++k) {
                glm::quat& r = pose_.rotation[static_cast<size_t>(chain[k])];
                r = glm::normalize(glm::slerp(walked[k], r, w));
            }
            Solve();
        }
        for (int s = 0; s < 2; ++s) {
            const float side = s == 0 ? 1.0f : -1.0f;
            for (int j = 0; j < 3; ++j) {
                for (const int f : fingers_[s][j]) {
                    glm::quat& r = pose_.rotation[static_cast<size_t>(f)];
                    r = glm::normalize(r * glm::angleAxis(-side * 0.3f * reach, glm::vec3(0, 0, 1)));
                }
            }
        }
    }
    Twitch(twitch);
    Solve();
    return true;
}

bool SurvivalPoser::Stretched(double frame, float raise, float slam, const glm::vec3& target, const glm::vec2& twitch) {
    if (!valid_) return false;
    std::fill(scale_.begin(), scale_.end(), glm::vec3(1.0f));
    auto scale = [&](int bone, glm::vec3 s) {
        if (bone >= 0) scale_[static_cast<size_t>(bone)] = s;
    };
    scale(spine_, {0.84f, 1.1f, 0.84f});
    scale(chest_, {0.86f, 1.08f, 0.84f});
    scale(neck_, {0.78f, 2.6f, 0.78f});
    for (int s = 0; s < 2; ++s) {
        scale(upper_arm_[s], {1.85f, 0.74f, 0.74f});
        scale(forearm_[s], {1.85f, 0.72f, 0.72f});
        scale(hand_[s], {1.2f, 0.85f, 0.9f});
        for (int j = 0; j < 3; ++j) {
            for (const int f : fingers_[s][j]) scale(f, {1.65f, 0.8f, 0.8f});
        }
        scale(thigh_[s], {0.84f, kStretchedLegs, 0.84f});
        scale(leg_[s], {0.84f, kStretchedLegs, 0.84f});
    }
    WalkFrom(frame, kStretchedLegs);
    auto turn = [&](int bone, float angle, const glm::vec3& axis) {
        glm::quat& r = pose_.rotation[static_cast<size_t>(bone)];
        r = glm::normalize(r * glm::angleAxis(angle, axis));
    };
    turn(spine_, 0.22f, {1, 0, 0});
    turn(chest_, 0.12f, {1, 0, 0});
    turn(neck_, 0.8f, {1, 0, 0});
    turn(neck_, 0.25f, {0, 0, 1});
    turn(head_, 1.05f, {0, 0, 1});
    turn(head_, -0.3f, {0, 1, 0});
    Solve();
    const float w = Smooth(std::max(raise, slam));
    if (w > 0.0f) {
        const int s = 1;
        const glm::quat walked[] = {pose_.rotation[static_cast<size_t>(upper_arm_[s])], pose_.rotation[static_cast<size_t>(forearm_[s])]};
        const glm::vec3 shoulder = pos_[static_cast<size_t>(upper_arm_[s])];
        const glm::vec3 up = shoulder + glm::vec3(-0.25f, 1.25f, 0.25f);
        const glm::vec3 aim = glm::mix(up, target, Smooth(slam));
        TwoBone(upper_arm_[s], forearm_[s], hand_[s], aim, glm::vec3(-0.6f, 0.2f, -1.0f), glm::vec3(0, 0, -1));
        pose_.rotation[static_cast<size_t>(upper_arm_[s])] = glm::normalize(glm::slerp(walked[0], pose_.rotation[static_cast<size_t>(upper_arm_[s])], w));
        pose_.rotation[static_cast<size_t>(forearm_[s])] = glm::normalize(glm::slerp(walked[1], pose_.rotation[static_cast<size_t>(forearm_[s])], w));
    }
    Twitch(twitch);
    Solve();
    return true;
}

void SurvivalPoser::Crawler(float phase, float crouch, float leap, float curl, const glm::vec2& twitch, float time, float claw) {
    if (!valid_) return;
    pose_.Reset(skeleton_.Size());
    std::fill(scale_.begin(), scale_.end(), glm::vec3(1.0f));
    for (int s = 0; s < 2; ++s) {
        scale_[static_cast<size_t>(upper_arm_[s])] = {1.32f, 0.86f, 0.86f};
        scale_[static_cast<size_t>(forearm_[s])] = {1.32f, 0.84f, 0.84f};
        for (int j = 0; j < 3; ++j) {
            for (const int f : fingers_[s][j]) scale_[static_cast<size_t>(f)] = {1.35f, 0.9f, 0.9f};
        }
    }
    scale_[static_cast<size_t>(spine_)] = {0.9f, 1.05f, 0.9f};
    const float alive = 1.0f - curl;
    const float sway = std::sin(2.0f * kPi * phase) * alive;
    float height = glm::mix(0.62f, 0.46f, Smooth(crouch));
    height = glm::mix(height, 0.2f, Smooth(curl));
    height += 0.025f * std::sin(4.0f * kPi * phase) * alive * (1.0f - leap);
    pose_.offset[static_cast<size_t>(waist_)] = glm::vec3(0.02f * sway, height, 0.0f) - skeleton_.bind_local[static_cast<size_t>(waist_)];
    const glm::quat over = glm::angleAxis(kPi, glm::vec3(0, 0, 1)) * glm::angleAxis(kPi * 0.5f, glm::vec3(1, 0, 0));
    pose_.rotation[static_cast<size_t>(waist_)] = glm::normalize(glm::angleAxis(0.06f * sway, glm::vec3(0, 0, 1)) * over);
    const float arch = 1.0f - 0.5f * leap;
    pose_.rotation[static_cast<size_t>(spine_)] = glm::angleAxis(-0.35f * arch, glm::vec3(1, 0, 0));
    pose_.rotation[static_cast<size_t>(chest_)] = glm::angleAxis(-0.45f * arch, glm::vec3(1, 0, 0));
    Solve();
    {
        const size_t n = static_cast<size_t>(neck_), h = static_cast<size_t>(head_);
        const glm::vec3 neck_dir = skeleton_.bind_local[h];
        Orient(neck_, neck_dir, {0, 0, 1}, {0.0f, -0.55f, 0.85f}, {0.0f, 0.85f, 0.55f});
        const glm::vec3 look = glm::angleAxis(twitch.x, glm::vec3(0, 1, 0)) * glm::vec3(0.0f, -0.12f, 1.0f);
        const glm::vec3 top = glm::angleAxis(twitch.y, glm::vec3(0, 0, 1)) * glm::vec3(0.0f, -1.0f, 0.0f);
        Orient(head_, {0, 0, 1}, {0, 1, 0}, look, top);
        (void)n;
    }
    for (int s = 0; s < 2; ++s) {
        const float side = s == 0 ? -1.0f : 1.0f;
        {
            const glm::vec2 g = Gait(phase + (s == 0 ? 0.5f : 0.0f), kCrawlStride, 0.13f) * alive * (1.0f - leap);
            glm::vec3 target(side * 0.22f, 0.09f + g.y, glm::mix(-0.52f, -0.44f, crouch) + g.x);
            const glm::vec3 hip = pos_[static_cast<size_t>(thigh_[s])];
            target = glm::mix(target, hip + glm::vec3(side * 0.1f, -0.25f, -0.78f), Smooth(leap));
            const float kick = std::sin(time * 23.0f + 1.7f * static_cast<float>(s)) * 0.06f * curl;
            target = glm::mix(target, glm::vec3(side * 0.2f, height + 0.5f + kick, -0.28f), Smooth(curl));
            TwoBone(thigh_[s], leg_[s], foot_[s], target, glm::vec3(side * 0.5f, 1.0f, 0.1f), glm::vec3(0, 0, 1));
            const glm::vec3 toe = skeleton_.bind_world[static_cast<size_t>(toe_[s])] - skeleton_.bind_world[static_cast<size_t>(foot_[s])];
            Orient(foot_[s], toe, {0, 1, 0}, glm::mix(glm::vec3(side * 0.35f, -0.05f, -1.0f), glm::vec3(side * 0.2f, 1.0f, -0.3f), curl), {0, 1, 0});
        }
        {
            const glm::vec2 g = Gait(phase + (s == 0 ? 0.0f : 0.5f), kCrawlStride, 0.1f) * alive * (1.0f - leap);
            glm::vec3 target(side * 0.36f, 0.035f + g.y, glm::mix(0.6f, 0.5f, crouch) + g.x);
            const glm::vec3 shoulder = pos_[static_cast<size_t>(upper_arm_[s])];
            const float rake = std::sin(time * 21.0f + kPi * static_cast<float>(s)) * claw;
            target = glm::mix(target, shoulder + glm::vec3(side * (0.34f + 0.06f * rake), 0.1f + 0.14f * rake, 0.55f - 0.08f * rake), Smooth(leap));
            const float kick = std::sin(time * 19.0f + 2.3f * static_cast<float>(s)) * 0.07f * curl;
            target = glm::mix(target, glm::vec3(side * 0.22f, height + 0.48f + kick, 0.3f), Smooth(curl));
            TwoBone(upper_arm_[s], forearm_[s], hand_[s], target, glm::vec3(side * 1.0f, 0.7f, 0.0f), glm::vec3(0, 0, -1));
            const glm::vec3 fingers(s == 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
            Orient(hand_[s], fingers, {0, 1, 0}, glm::vec3(side * 0.45f, -0.08f, 1.0f), glm::mix(glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), curl));
        }
    }
    for (int s = 0; s < 2; ++s) {
        const float side = s == 0 ? 1.0f : -1.0f;
        for (int j = 0; j < 3; ++j) {
            const float angle = (j == 2 ? 0.45f : 0.32f) * (1.0f + 1.2f * curl) * (1.0f - 0.6f * leap * (1.0f - claw) + 0.8f * claw);
            for (const int f : fingers_[s][j]) pose_.rotation[static_cast<size_t>(f)] = glm::angleAxis(-side * angle, glm::vec3(0, 0, 1));
        }
    }
    Solve();
}

void SurvivalPoser::Slacken(float weight) {
    if (!valid_ || weight <= 0.0f) return;
    const float w = Smooth(weight);
    auto blend = [&](int bone, const glm::quat& to) {
        glm::quat& r = pose_.rotation[static_cast<size_t>(bone)];
        r = glm::normalize(glm::slerp(r, to, w));
    };
    for (int s = 0; s < 2; ++s) {
        const float side = s == 0 ? 1.0f : -1.0f;
        blend(upper_arm_[s], glm::angleAxis(side * 1.0f, glm::vec3(0, 0, 1)) * glm::angleAxis(side * 0.4f, glm::vec3(0, 1, 0)));
        blend(forearm_[s], glm::angleAxis(side * 0.5f, glm::vec3(0, 1, 0)));
        blend(thigh_[s], glm::angleAxis(0.55f, glm::vec3(1, 0, 0)) * glm::angleAxis(side * 0.15f, glm::vec3(0, 0, 1)));
        blend(leg_[s], glm::angleAxis(-0.95f, glm::vec3(1, 0, 0)));
    }
    blend(spine_, glm::angleAxis(-0.15f, glm::vec3(1, 0, 0)));
    blend(neck_, glm::angleAxis(-0.35f, glm::vec3(1, 0, 0)));
    blend(head_, glm::angleAxis(-0.45f, glm::vec3(1, 0, 0)) * glm::angleAxis(0.4f, glm::vec3(0, 0, 1)));
    Solve();
}

void SurvivalPoser::Flinch(const glm::vec2& push, float twist) {
    if (!valid_) return;
    const float angle = glm::length(push);
    if (angle < 1e-4f && std::abs(twist) < 1e-4f) return;
    const glm::vec3 dir(push.x, 0.0f, push.y);
    const glm::vec3 axis = angle > 1e-4f ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), dir)) : glm::vec3(1, 0, 0);
    auto bend = [&](int bone, float share) {
        if (bone < 0) return;
        const glm::quat world = World(bone);
        SetWorld(bone, glm::angleAxis(angle * share, axis) * world);
        Solve();
    };
    bend(spine_, 0.35f);
    if (std::abs(twist) > 1e-4f) {
        SetWorld(chest_, glm::angleAxis(twist, glm::vec3(0, 1, 0)) * World(chest_));
        const int struck_arm = twist > 0.0f ? upper_arm_[0] : upper_arm_[1];
        if (struck_arm >= 0) {
            SetWorld(struck_arm, glm::angleAxis(twist * 0.65f, glm::vec3(0, 1, 0)) * World(struck_arm));
        }
        Solve();
    }
    bend(chest_, 0.45f);
    bend(neck_, 0.38f);
    bend(head_, 0.88f);
}

void SurvivalPoser::ApplyBody(const anim::PhysicalBody& body, const glm::mat4& to_model, float weight) {
    if (!valid_ || !body.Valid() || weight <= 0.0f) return;
    auto sim = [&](int bone, glm::vec3& out) {
        glm::vec3 w(0.0f);
        if (!body.Joint(bone, w)) return false;
        out = glm::vec3(to_model * glm::vec4(w, 1.0f));
        return true;
    };
    auto turn = [&](int bone, const glm::quat& q) {
        SetWorld(bone, glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), q, weight) * World(bone)));
        Solve();
    };
    auto arc = [](const glm::vec3& from, const glm::vec3& to) {
        const glm::vec3 a = glm::normalize(from), b = glm::normalize(to);
        const float c = glm::dot(a, b);
        if (c > 0.99999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (c < -0.9999f) {
            const glm::vec3 axis = glm::normalize(glm::cross(a, std::abs(a.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)));
            return glm::angleAxis(3.14159265f, axis);
        }
        const glm::vec3 axis = glm::cross(a, b);
        return glm::normalize(glm::quat(1.0f + c, axis.x, axis.y, axis.z));
    };
    auto aim_arc = [&](int bone, int next) {
        glm::vec3 a(0.0f), b(0.0f);
        if (!sim(bone, a) || !sim(next, b)) return;
        const glm::vec3 now = pos_[static_cast<size_t>(next)] - pos_[static_cast<size_t>(bone)];
        if (glm::length(now) < 1e-5f || glm::length(b - a) < 1e-5f) return;
        turn(bone, arc(now, b - a));
    };
    auto frame = [&](int bone, int next, int left, int right) {
        glm::vec3 a(0.0f), b(0.0f), l(0.0f), r(0.0f);
        if (!sim(bone, a) || !sim(next, b) || !sim(left, l) || !sim(right, r)) return;
        const glm::vec3 now = pos_[static_cast<size_t>(next)] - pos_[static_cast<size_t>(bone)];
        const glm::vec3 across = pos_[static_cast<size_t>(left)] - pos_[static_cast<size_t>(right)];
        if (glm::length(now) < 1e-5f || glm::length(across) < 1e-5f || glm::length(b - a) < 1e-5f || glm::length(l - r) < 1e-5f) return;
        turn(bone, Frame(now, across, b - a, l - r));
    };
    if (glm::vec3 w(0.0f); sim(waist_, w)) {
        pose_.offset[static_cast<size_t>(waist_)] += (w - pos_[static_cast<size_t>(waist_)]) * weight;
        Solve();
    }
    frame(waist_, spine_, thigh_[0], thigh_[1]);
    aim_arc(spine_, chest_);
    frame(chest_, neck_, upper_arm_[0], upper_arm_[1]);
    aim_arc(neck_, head_);
    {
        glm::vec3 h(0.0f);
        if (sim(head_, h)) {
            const glm::vec3 top = glm::vec3(to_model * glm::vec4(body.HeadTop(), 1.0f));
            const glm::vec3 now = World(head_) * glm::vec3(0.0f, 1.0f, 0.0f);
            if (glm::length(top - h) > 1e-5f) turn(head_, arc(now, top - h));
        }
    }
    for (int s = 0; s < 2; ++s) {
        aim_arc(upper_arm_[s], forearm_[s]);
        aim_arc(forearm_[s], hand_[s]);
        aim_arc(thigh_[s], leg_[s]);
        aim_arc(leg_[s], foot_[s]);
        aim_arc(foot_[s], toe_[s]);
    }
}

void SurvivalPoser::Skin(std::vector<glm::mat4>& out) const {
    anim::ComputeScaledSkin(skeleton_, pose_, scale_, out);
}

void SurvivalPoser::BoneWorld(std::vector<glm::mat4>& out) const {
    const size_t n = skeleton_.Size();
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = glm::translate(glm::mat4(1.0f), pos_[i]) * glm::mat4_cast(rot_[i]);
    }
}

void SurvivalPoser::ApplyRagdoll(const anim::Ragdoll& ragdoll, const glm::mat4& model_world) {
    if (!valid_ || !ragdoll.Active()) return;
    ragdoll.ApplyToPose(skeleton_, model_world, scale_, pose_);
    Solve();
}

}
