#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

#include "engine/anim/physical_body.h"
#include "engine/anim/ragdoll.h"
#include "engine/anim/skeleton.h"

namespace pt::game {

class SurvivalPoser {
public:
    explicit SurvivalPoser(const anim::Skeleton& skeleton);
    bool Valid() const { return valid_; }

    bool Doll(double frame, float reach, const glm::vec3& target, const glm::vec2& twitch, float raise = 0.0f, float slam = 0.0f, float quirk = 0.0f);
    bool Stretched(double frame, float raise, float slam, const glm::vec3& target, const glm::vec2& twitch);
    void Crawler(float phase, float crouch, float leap, float curl, const glm::vec2& twitch, float time, float claw = 0.0f);
    void Slacken(float weight);
    void Flinch(const glm::vec2& push, float twist);
    void ApplyBody(const anim::PhysicalBody& body, const glm::mat4& to_model, float weight);
    void ApplyRagdoll(const anim::Ragdoll& ragdoll, const glm::mat4& model_world);
    void BoneWorld(std::vector<glm::mat4>& out) const;
    void Skin(std::vector<glm::mat4>& out) const;
    glm::quat Turn(int bone) const { return World(bone); }
    glm::vec3 Joint(int bone) const { return bone >= 0 && static_cast<size_t>(bone) < pos_.size() ? pos_[static_cast<size_t>(bone)] : glm::vec3(0.0f); }
    int head() const { return head_; }
    int waist() const { return waist_; }
    static constexpr float kStretchedLegs = 1.22f;
    static constexpr float kCrawlStride = 0.6f;
    static constexpr float kCrawlDuty = 0.5f;

private:
    void Solve();
    void SetWorld(int bone, const glm::quat& rotation);
    glm::quat World(int bone) const;
    void TwoBone(int a, int b, int c, const glm::vec3& target, const glm::vec3& pole, const glm::vec3& bend);
    void Orient(int bone, const glm::vec3& from, const glm::vec3& from_up, const glm::vec3& to, const glm::vec3& to_up);
    void Twitch(const glm::vec2& twitch);
    void WalkFrom(double frame, float leg_scale);

    const anim::Skeleton& skeleton_;
    bool valid_ = false;
    anim::Pose pose_;
    std::vector<glm::vec3> scale_;
    std::vector<glm::quat> rot_;
    std::vector<glm::vec3> pos_;
    int waist_ = -1, spine_ = -1, chest_ = -1, neck_ = -1, head_ = -1;
    int shoulder_[2] = {-1, -1}, upper_arm_[2] = {-1, -1}, forearm_[2] = {-1, -1}, hand_[2] = {-1, -1};
    int thigh_[2] = {-1, -1}, leg_[2] = {-1, -1}, foot_[2] = {-1, -1}, toe_[2] = {-1, -1};
    std::vector<int> fingers_[2][3];
};

}
