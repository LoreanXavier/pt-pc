#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/anim/skeleton.h"

namespace pt::anim {

struct SimShape {
    int32_t type = 0;
    glm::vec3 size{0.0f};
    glm::vec3 offset{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct SimBody {
    std::string bone;
    bool dynamic = false;
    bool hit = false;
    bool wind = false;
    bool no_gravity = false;
    float mass = 1.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    float max_linear_velocity = 100.0f;
    glm::vec3 offset_position{0.0f};
    glm::quat offset_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 bind_position{0.0f};
    glm::quat bind_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    SimShape shape;

    int parent = -1;
    int constraint = -1;
    glm::vec3 pivot_local{0.0f};
    glm::vec3 pivot_parent{0.0f};
    glm::vec3 end_local{0.0f};
    glm::vec3 direction_local{0.0f, -1.0f, 0.0f};
    float length = 0.0f;
    glm::quat relative_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 cone_axis_parent{0.0f, -1.0f, 0.0f};
    float cone_cos = -1.0f;
    bool linked_to_parent_end = false;
    float radius = 0.0f;
};

struct SimConstraint {
    int a = -1;
    int b = -1;
    glm::vec3 pivot{0.0f};
    glm::vec3 ref_a{0.0f, -1.0f, 0.0f};
    glm::vec3 ref_b{0.0f, -1.0f, 0.0f};
    bool limited = false;
    float limit_degrees = 0.0f;
    bool stop_twist = true;
};

class SimRig {
public:
    bool Load(std::string_view name, std::span<const uint8_t> data, std::string* error);

    const std::string& Name() const { return name_; }
    const std::vector<SimBody>& Bodies() const { return bodies_; }
    const std::vector<SimConstraint>& Constraints() const { return constraints_; }
    const std::vector<int>& Order() const { return order_; }
    const std::vector<int>& Hits() const { return hits_; }
    float HitDistance(size_t order_index, size_t hit_index) const { return hit_distance_[order_index * hits_.size() + hit_index]; }
    float WindCoefficient() const { return wind_coefficient_; }
    bool ConvertMoveToWind() const { return convert_move_to_wind_; }
    size_t DynamicCount() const { return order_.size(); }

private:
    std::string name_;
    std::vector<SimBody> bodies_;
    std::vector<SimConstraint> constraints_;
    std::vector<int> order_;
    std::vector<int> hits_;
    std::vector<float> hit_distance_;
    float wind_coefficient_ = 1.0f;
    bool convert_move_to_wind_ = false;
};

class SimPhysics {
public:
    void Reset() { warm_ = true; }
    bool Step(const SimRig& rig, const Skeleton& skeleton, std::vector<glm::mat4>& world, const glm::mat4& model_world, float dt,
              const glm::vec3& wind);

private:
    struct Frame {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    void Bind(const SimRig& rig, const Skeleton& skeleton);
    Frame Animated(const SimRig& rig, size_t body, const std::vector<glm::mat4>& world, const glm::mat4& model_world) const;
    void Snap(const SimRig& rig, const std::vector<glm::mat4>& world, const glm::mat4& model_world);
    void Solve(const SimRig& rig, size_t order_index, const std::vector<int>& slot, bool constrain);

    const SimRig* rig_ = nullptr;
    const Skeleton* skeleton_ = nullptr;
    std::vector<int> bone_;
    std::vector<bool> active_;
    std::vector<int> bone_order_;
    std::vector<Frame> frame_;
    std::vector<Frame> previous_kinematic_;
    std::vector<glm::vec3> end_;
    std::vector<glm::vec3> previous_end_;
    std::vector<glm::vec3> velocity_;
    bool warm_ = true;
};

bool SimPhysicsEnabled();
void SetSimWind(const glm::vec3& wind);
glm::vec3 SimWind();

}
