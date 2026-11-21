#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <deque>
#include <vector>

#include "engine/anim/sim_physics.h"
#include "engine/anim/skeleton.h"

namespace pt::anim {

class SimRigid {
public:
    void Reset() { warm_ = true; }
    void SetDelay(float seconds) { delay_ = seconds; }
    bool Step(const SimRig& rig, const Skeleton& skeleton, std::vector<glm::mat4>& world, const glm::mat4& model_world, float dt,
              const glm::vec3& wind = glm::vec3(0.0f));

    bool BodyFrame(size_t body, glm::vec3& position, glm::quat& rotation) const;
    bool SetBody(size_t body, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& velocity, const glm::vec3& spin);

private:
    struct State {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 velocity{0.0f};
        glm::vec3 spin{0.0f};
    };
    struct Joint {
        int child = -1;
        int parent = -1;
        glm::vec3 pivot_child{0.0f};
        glm::vec3 pivot_parent{0.0f};
        glm::vec3 axis_child{0.0f, -1.0f, 0.0f};
        glm::vec3 axis_parent{0.0f, -1.0f, 0.0f};
        float cos_limit = -1.0f;
        float limit = 0.0f;
        bool twist = false;
        glm::quat bind_relative{1.0f, 0.0f, 0.0f, 0.0f};
    };

    struct Contact {
        size_t a = 0;
        size_t b = 0;
        glm::vec3 ra{0.0f};
        glm::vec3 rb{0.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
        float depth = 0.0f;
        float permitted = 0.0f;
        float impulse = 0.0f;
    };

    void Bind(const SimRig& rig, const Skeleton& skeleton);
    bool Touch(size_t a, size_t b, Contact& out) const;
    void FindContacts();
    State Animated(const SimRig& rig, size_t body, const std::vector<glm::mat4>& world, const glm::mat4& model_world) const;
    float Twist(const Joint& j, const glm::quat& relative, const glm::vec3& axis) const;
    void SolveVelocities(float h);
    void SolvePositions();
    glm::mat3 InverseInertia(size_t body) const;

    const SimRig* rig_ = nullptr;
    const Skeleton* skeleton_ = nullptr;
    std::vector<int> bone_;
    std::vector<bool> active_;
    std::vector<bool> dynamic_;
    std::vector<float> inverse_mass_;
    std::vector<glm::vec3> inverse_inertia_;
    std::vector<float> max_speed_;
    std::vector<float> max_spin_;
    std::vector<Joint> joints_;
    std::vector<glm::vec3> segment_a_;
    std::vector<glm::vec3> segment_b_;
    std::vector<float> radius_;
    std::vector<float> permitted_;
    std::vector<int> collision_id_;
    std::vector<bool> wind_;
    float wind_coefficient_ = 1.0f;
    bool move_to_wind_ = false;
    glm::vec3 previous_model_{0.0f};
    glm::mat4 last_model_{1.0f};
    std::vector<std::pair<size_t, size_t>> pairs_;
    std::vector<Contact> contacts_;
    std::vector<int> bone_order_;
    std::vector<State> state_;
    std::vector<State> to_;
    struct Snapshot {
        double time = 0.0;
        std::vector<glm::vec3> position;
        std::vector<glm::quat> rotation;
    };
    void Remember(const glm::mat4& model_world);
    void Drawn(std::vector<glm::vec3>& position, std::vector<glm::quat>& rotation) const;
    std::deque<Snapshot> history_;
    double time_ = 0.0;
    float delay_ = 0.0f;
    bool warm_ = true;
};

}
