#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "engine/anim/skeleton.h"

namespace pt::anim {

class Ragdoll {
public:
    using GroundFn = std::function<float(const glm::vec3&)>;
    using SweepFn = std::function<bool(const glm::vec3& from, const glm::vec3& to, glm::vec3& point, glm::vec3& normal)>;

    enum Part : int {
        kPelvis = 0,
        kChest,
        kHead,
        kLUpperArm,
        kLForearm,
        kRUpperArm,
        kRForearm,
        kLThigh,
        kLShin,
        kRThigh,
        kRShin,
        kPartCount,
    };

    bool Activate(const Skeleton& skeleton, const std::vector<glm::mat4>& bone_world, const glm::mat4& model_world,
                  float size = 1.0f, const glm::vec3& initial_vel = glm::vec3(0.0f));

    void Clear();
    bool Active() const { return active_; }
    bool Asleep() const { return asleep_; }
    float Age() const { return age_; }

    void AddImpulse(const glm::vec3& at, const glm::vec3& delta_v, float radius = 0.55f);
    void AddPartImpulse(Part part, const glm::vec3& delta_v, const glm::vec3& angular_kick = glm::vec3(0.0f));

    void SetCollapse(const glm::vec3& fall_dir, bool headshot = false);

    void Step(float dt, const GroundFn& ground, const SweepFn& sweep);

    void ApplyToBones(const Skeleton& skeleton, const glm::mat4& model_world, std::vector<glm::mat4>& bone_world) const;

    void ApplyToPose(const Skeleton& skeleton, const glm::mat4& model_world, const std::vector<glm::vec3>& scale,
                     Pose& pose) const;

    glm::vec3 PartPosition(Part part) const;
    glm::vec3 PelvisPosition() const { return PartPosition(kPelvis); }
    glm::vec3 HeadPosition() const { return PartPosition(kHead); }
    glm::vec3 ChestPosition() const { return PartPosition(kChest); }
    float TakeImpact();

private:
    struct Body {
        int bone = -1;
        int end_bone = -1;
        glm::vec3 pos{0.0f};
        glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 vel{0.0f};
        glm::vec3 ang{0.0f};
        float mass = 1.0f;
        float inv_mass = 1.0f;
        glm::vec3 inv_inertia_local{1.0f};
        glm::vec3 cap_a{0.0f};
        glm::vec3 cap_b{0.0f};
        float radius = 0.08f;
        glm::vec3 joint_in_body{0.0f};
        glm::vec3 prev_pos{0.0f};
    };

    struct Joint {
        int child = -1;
        int parent = -1;
        glm::vec3 pivot_child{0.0f};
        glm::vec3 pivot_parent{0.0f};
        bool is_hinge = false;
        glm::vec3 axis_child{0.0f, -1.0f, 0.0f};
        glm::vec3 axis_parent{0.0f, -1.0f, 0.0f};
        float cone_limit = 1.0f;
        float twist_min = -0.5f;
        float twist_max = 0.5f;
        glm::vec3 hinge_axis_child{1.0f, 0.0f, 0.0f};
        glm::vec3 hinge_axis_parent{1.0f, 0.0f, 0.0f};
        glm::vec3 hinge_ref_child{0.0f, -1.0f, 0.0f};
        glm::vec3 hinge_ref_parent{0.0f, -1.0f, 0.0f};
        float hinge_min = 0.0f;
        float hinge_max = 2.3f;
    };

    struct Contact {
        int body_a = -1;
        int body_b = -1;
        glm::vec3 ra{0.0f};
        glm::vec3 rb{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float depth = 0.0f;
        float normal_impulse = 0.0f;
        glm::vec3 tangent_impulse{0.0f};
    };

    glm::mat3 InvInertiaWorld(int idx) const;
    void ApplyCollapseDrive(float h);
    void GatherContacts(const std::vector<float>& floor_y);
    void SolveVelocities(float h);
    void SolvePositions(const std::vector<float>& floor_y);

    bool active_ = false;
    bool asleep_ = false;
    float age_ = 0.0f;
    float still_time_ = 0.0f;
    float size_ = 1.0f;
    float impact_ = 0.0f;
    float collapse_timer_ = 0.0f;
    bool collapse_headshot_ = false;
    glm::vec3 collapse_dir_{0.0f, 0.0f, -1.0f};

    std::array<Body, kPartCount> bodies_{};
    std::vector<Joint> joints_;
    std::vector<std::pair<int, int>> self_pairs_;
    std::vector<Contact> contacts_;

    std::vector<glm::mat4> saved_local_;
    std::vector<int> part_of_bone_;
    std::vector<int> bone_order_;
};

}
