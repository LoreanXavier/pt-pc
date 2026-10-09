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

// The bone simulation of a model (`.sim`, a fox2 DataSet: SimOnPhysics with a PhObjectDesc), as plr0_main0_def_s00.sim
// describes the player's jacket, hood, drawstrings, hair and trouser hems. The original runs it on Havok (the units leave
// and rejoin the world through 0xFAC690 / 0xFAC5F0); the port solves the same rig with position based dynamics.
//
// Data layout (decoded from plr0_main0_def_s00.sim, checked against every unit):
// - SimOnPhysics.simRootBones / simBones / simHitBones: SimAssociationUnit per bone, `body` links a PhRigidBodyParam of the
//   PhObjectDesc, `constraint` its PhBallsocketConstraintParam (simBones only), `param.boneName` the bone. The body sits at
//   `bodyOffsetPos` in the bone's frame turned by `offsetRot` (defaultPosition = bone + bodyOffsetPos exactly, defaultRotation
//   = offsetRot; FMDL bind poses carry no rotation) and the joint at `constraintOffsetPos` (0, the bone itself).
// - PhObjectDesc.bodies interleaves each PhRigidBodyParam with its PhPrimitiveShapeParam; `bodyIndices` holds (A, B) per
//   constraint as rigid body ordinals, A the simulated child and B its parent.
// - motionType 1 is keyframed (follows its bone), 2 dynamic. Shape type 5 is taken as a capsule along local y (radius
//   size.x, half height size.y), other types as spheres of radius size.x.
// - The ball socket: `defaultPosition` the joint, `refA` and `refB` the swing axes of A and B in the bind frame, `limit`
//   the cone half angle in degrees; `stopTwistFlag` (set on every joint) locks the twist.
// - SimWindControl.bones get the wind of the scene times `coefficient`; SimEngineOnPhysicsParam.convertMoveToWind makes
//   the motion of those bones through the air count as wind (drag against the relative air velocity).
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
    // the body in its bone's frame
    glm::vec3 offset_position{0.0f};
    glm::quat offset_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    // PhRigidBodyParam defaultPosition / defaultRotation (the bind frame of the file)
    glm::vec3 bind_position{0.0f};
    glm::quat bind_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    SimShape shape;

    // dynamic bodies: the joint to the parent body and the stick the solver moves (all in the body's own frame unless noted)
    int parent = -1;
    int constraint = -1;
    glm::vec3 pivot_local{0.0f};
    glm::vec3 pivot_parent{0.0f};
    glm::vec3 end_local{0.0f};
    glm::vec3 direction_local{0.0f, -1.0f, 0.0f};
    float length = 0.0f;
    // rotation of the body relative to its parent in the bind pose, the cone axis in the parent's frame, cos of the limit
    glm::quat relative_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 cone_axis_parent{0.0f, -1.0f, 0.0f};
    float cone_cos = -1.0f;
    // the stick end is the parent's stick end (a chain link): the distance constraint moves both by their masses
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
    // dynamic bodies, parents before children
    const std::vector<int>& Order() const { return order_; }
    const std::vector<int>& Hits() const { return hits_; }
    // per dynamic body (index into Order) and hit body (index into Hits): the closest the stick end may come to the hit
    // shape, the sum of the radii but never more than the bind pose distance (cloth authored inside the hit shapes stays put)
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

// One simulated model. Step takes the animated bone matrices in model space and the model's world transform, simulates in
// world space (walking and turning swing the cloth) and writes the simulated bones back, with every bone below them moved
// along, before ComputeSkin.
class SimPhysics {
public:
    // the next Step snaps to the animated pose (first frame, teleport, the demo functor's resume)
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

// PT_SIM=0 turns the data driven simulation off (the demos skin the sim bones rigidly, the mirror body falls back to
// SimBones)
bool SimPhysicsEnabled();
// the scene's wind (VfxScene: WindGlobal and the demo wind functor), read by the wind controlled bodies
void SetSimWind(const glm::vec3& wind);
glm::vec3 SimWind();

}
