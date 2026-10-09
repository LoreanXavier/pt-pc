#include "engine/anim/sim_physics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/data/fox2.h"

namespace pt::anim {
namespace {

// No engine parameter of the file sets these (SimEngineOnPhysicsParam has only isEnableGeoCheck, convertMoveToWind and the
// LOD range): the step is the original's 60 Hz motion rate, the rest are the solver's own.
constexpr float kStep = 1.0f / 60.0f;
constexpr int kMaxSubsteps = 8;
constexpr int kIterations = 4;
constexpr float kGravity = 9.8f;
// a gap longer than this restarts from the animated pose instead of integrating it
constexpr float kMaxFrameTime = 0.25f;
// a keyframed body that jumps this far in one frame is a cut or a teleport: restart from the animated pose
constexpr float kTeleportDistance = 1.0f;
// wind acceleration per m/s of air speed relative to the body, times SimWindControlParam.coefficient
constexpr float kWindDrag = 2.0f;

glm::vec3 g_wind{0.0f};

glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const float lf = glm::length(from);
    const float lt = glm::length(to);
    if (lf < 1e-8f || lt < 1e-8f) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const glm::vec3 u = from / lf;
    const glm::vec3 v = to / lt;
    const float c = glm::dot(u, v);
    if (c > 0.999999f) {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    if (c < -0.999999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), u);
        if (glm::dot(axis, axis) < 1e-4f) {
            axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), u);
        }
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(u, v);
    const float s = std::sqrt((1.0f + c) * 2.0f);
    return glm::normalize(glm::quat(s * 0.5f, axis.x / s, axis.y / s, axis.z / s));
}

glm::vec3 Perpendicular(const glm::vec3& v) {
    const glm::vec3 a = std::abs(v.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::normalize(glm::cross(v, a));
}

// the direction d turned toward the cone axis onto the cone of half angle acos(cos_limit)
glm::vec3 ClampToCone(const glm::vec3& axis, const glm::vec3& d, float cos_limit) {
    glm::vec3 perp = d - axis * glm::dot(d, axis);
    const float length = glm::length(perp);
    perp = length > 1e-6f ? perp / length : Perpendicular(axis);
    const float sin_limit = std::sqrt(std::max(0.0f, 1.0f - cos_limit * cos_limit));
    return axis * cos_limit + perp * sin_limit;
}

glm::vec3 ClosestOnSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

// the hit shape's core segment (capsule) or point (sphere) in the frame of its body
void HitSegment(const SimBody& body, const glm::vec3& position, const glm::quat& rotation, glm::vec3& a, glm::vec3& b) {
    const glm::vec3 center = position + rotation * body.shape.offset;
    if (body.shape.type == 5) {
        const glm::vec3 half = rotation * (body.shape.rotation * glm::vec3(0.0f, body.shape.size.y, 0.0f));
        a = center - half;
        b = center + half;
    } else {
        a = b = center;
    }
}

glm::quat RotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float l = glm::length(r[c]);
        if (l > 1e-8f) {
            r[c] /= l;
        }
    }
    return glm::normalize(glm::quat_cast(r));
}

glm::vec3 Vec3(const glm::vec4& v) {
    return glm::vec3(v.x, v.y, v.z);
}

}

bool SimPhysicsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("PT_SIM");
        return !(env && (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0 || std::strcmp(env, "off") == 0));
    }();
    return enabled;
}

void SetSimWind(const glm::vec3& wind) {
    g_wind = wind;
}

glm::vec3 SimWind() {
    return g_wind;
}

bool SimRig::Load(std::string_view name, std::span<const uint8_t> data, std::string* error) {
    auto fail = [&](std::string text) {
        if (error) {
            *error = std::move(text);
        }
        return false;
    };
    name_ = std::string(name);
    bodies_.clear();
    constraints_.clear();
    order_.clear();
    hits_.clear();
    hit_distance_.clear();
    fox2::DataSetFile file;
    if (!file.Load(name_, data)) {
        return fail("not a fox2 file");
    }
    const fox2::Entity* sim = nullptr;
    const fox2::Entity* desc = nullptr;
    for (const fox2::Entity& e : file.Entities()) {
        if (e.class_name == "SimOnPhysics" && !sim) {
            sim = &e;
        } else if (e.class_name == "PhObjectDesc" && !desc) {
            desc = &e;
        }
    }
    if (!sim || !desc) {
        return fail("no SimOnPhysics or PhObjectDesc");
    }

    // PhObjectDesc.bodies: each PhRigidBodyParam followed by its PhPrimitiveShapeParam
    std::unordered_map<const fox2::Entity*, int> rigid_index;
    std::vector<const fox2::Entity*> shapes;
    if (const fox2::Property* list = file.FindProperty(*desc, "bodies")) {
        for (size_t i = 0; i < list->Count(); ++i) {
            const fox2::Entity* e = file.ElementEntity(*list, i);
            if (!e) {
                continue;
            }
            if (e->class_name == "PhRigidBodyParam") {
                rigid_index[e] = static_cast<int>(bodies_.size());
                SimBody b;
                b.mass = file.GetFloat(*e, "mass", 0, 1.0f);
                b.linear_damping = file.GetFloat(*e, "linearVelocityDamp");
                b.angular_damping = file.GetFloat(*e, "angularVelocityDamp");
                b.max_linear_velocity = file.GetFloat(*e, "maxLinearVelocity", 0, 100.0f);
                b.dynamic = file.GetInt(*e, "motionType") == 2;
                b.no_gravity = file.GetBool(*e, "isNoGravity");
                b.bind_position = Vec3(file.GetVec4(*e, "defaultPosition"));
                b.bind_rotation = glm::normalize(file.GetQuat(*e, "defaultRotation"));
                bodies_.push_back(b);
                shapes.push_back(nullptr);
            } else if (e->class_name == "PhPrimitiveShapeParam" && !shapes.empty() && !shapes.back()) {
                shapes.back() = e;
            }
        }
    }
    if (bodies_.empty()) {
        return fail("no rigid bodies");
    }
    for (size_t i = 0; i < bodies_.size(); ++i) {
        if (const fox2::Entity* s = shapes[i]) {
            SimShape& shape = bodies_[i].shape;
            shape.type = file.GetInt(*s, "type");
            shape.size = Vec3(file.GetVec4(*s, "size"));
            shape.offset = Vec3(file.GetVec4(*s, "offset"));
            shape.rotation = glm::normalize(file.GetQuat(*s, "rotation"));
        }
        bodies_[i].radius = bodies_[i].shape.size.x;
    }

    // constraints: bodyIndices holds (A, B) per constraint as rigid body ordinals
    std::unordered_map<const fox2::Entity*, int> constraint_index;
    if (const fox2::Property* list = file.FindProperty(*desc, "constraints")) {
        for (size_t i = 0; i < list->Count(); ++i) {
            const fox2::Entity* e = file.ElementEntity(*list, i);
            SimConstraint c;
            c.a = file.GetInt(*desc, "bodyIndices", 2 * i, -1);
            c.b = file.GetInt(*desc, "bodyIndices", 2 * i + 1, -1);
            if (e) {
                c.pivot = Vec3(file.GetVec4(*e, "defaultPosition"));
                c.ref_a = Vec3(file.GetVec4(*e, "refA"));
                c.ref_b = Vec3(file.GetVec4(*e, "refB"));
                c.limited = file.GetBool(*e, "limitedFlag");
                c.limit_degrees = file.GetFloat(*e, "limit");
                c.stop_twist = file.GetBool(*e, "stopTwistFlag", 0, true);
                constraint_index[e] = static_cast<int>(constraints_.size());
            }
            constraints_.push_back(c);
        }
    }

    // association units: the bone of each body and its offset
    size_t bound = 0;
    for (const char* group : {"simRootBones", "simBones", "simTransBones", "simHitBones"}) {
        const bool hit = std::strcmp(group, "simHitBones") == 0;
        for (const auto& [key, unit] : file.GetEntityMap(*sim, group)) {
            if (!unit) {
                continue;
            }
            const fox2::Entity* body = file.GetEntity(*unit, "body");
            auto it = body ? rigid_index.find(body) : rigid_index.end();
            if (it == rigid_index.end()) {
                continue;
            }
            SimBody& b = bodies_[static_cast<size_t>(it->second)];
            const fox2::Entity* param = file.GetEntity(*unit, "param");
            b.bone = param ? file.GetString(*param, "boneName") : std::string();
            if (b.bone.empty()) {
                b.bone = key;
            }
            b.offset_position = Vec3(file.GetVec4(*unit, "bodyOffsetPos"));
            b.offset_rotation = glm::normalize(file.GetQuat(*unit, "offsetRot"));
            b.hit = b.hit || hit;
            if (const fox2::Entity* c = file.GetEntity(*unit, "constraint")) {
                if (auto ci = constraint_index.find(c); ci != constraint_index.end()) {
                    b.constraint = ci->second;
                }
            }
            ++bound;
        }
    }

    // wind control and engine parameters
    std::unordered_set<std::string> wind_bones;
    if (const fox2::Property* controls = file.FindProperty(*sim, "controls")) {
        for (size_t i = 0; i < controls->Count(); ++i) {
            const fox2::Entity* control = file.ElementEntity(*controls, i);
            if (!control || control->class_name != "SimWindControl") {
                continue;
            }
            if (const fox2::Property* bones = file.FindProperty(*control, "bones")) {
                for (size_t k = 0; k < bones->Count(); ++k) {
                    wind_bones.insert(file.ElementString(*bones, k));
                }
            }
            if (const fox2::Entity* param = file.GetEntity(*control, "controlParam")) {
                wind_coefficient_ = file.GetFloat(*param, "coefficient", 0, 1.0f);
            }
        }
    }
    if (const fox2::Entity* engine = file.GetEntity(*sim, "engineParam")) {
        convert_move_to_wind_ = file.GetBool(*engine, "convertMoveToWind");
    }
    for (SimBody& b : bodies_) {
        b.wind = b.dynamic && wind_bones.contains(b.bone);
    }

    // the parent of every dynamic body: the B of the constraint whose A it is (the unit's constraint when it names one)
    const int count = static_cast<int>(bodies_.size());
    for (size_t c = 0; c < constraints_.size(); ++c) {
        const SimConstraint& k = constraints_[c];
        if (k.a < 0 || k.a >= count || k.b < 0 || k.b >= count || k.a == k.b) {
            continue;
        }
        SimBody& a = bodies_[static_cast<size_t>(k.a)];
        if (!a.dynamic || a.parent >= 0 || (a.constraint >= 0 && a.constraint != static_cast<int>(c))) {
            continue;
        }
        a.parent = k.b;
        a.constraint = static_cast<int>(c);
    }
    // parents before children; dynamic bodies without a usable parent chain stay out
    std::vector<int> depth(bodies_.size(), -1);
    auto depth_of = [&](auto&& self, int i, int guard) -> int {
        if (depth[static_cast<size_t>(i)] != -1 || guard > count) {
            return depth[static_cast<size_t>(i)];
        }
        const SimBody& b = bodies_[static_cast<size_t>(i)];
        if (!b.dynamic) {
            return depth[static_cast<size_t>(i)] = 0;
        }
        if (b.parent < 0 || b.bone.empty()) {
            return depth[static_cast<size_t>(i)] = -2;
        }
        const int p = self(self, b.parent, guard + 1);
        return depth[static_cast<size_t>(i)] = p < 0 ? -2 : p + 1;
    };
    for (int i = 0; i < count; ++i) {
        depth_of(depth_of, i, 0);
        if (bodies_[static_cast<size_t>(i)].dynamic && depth[static_cast<size_t>(i)] > 0) {
            order_.push_back(i);
        }
    }
    std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) { return depth[static_cast<size_t>(a)] < depth[static_cast<size_t>(b)]; });

    // the stick of each dynamic body: from its joint to its only simulated child's joint (a chain link), else through the
    // body's centre to the mirror of the joint (the end links hang 2 x bodyOffsetPos below their bone)
    std::vector<int> children(bodies_.size(), 0);
    std::vector<int> only_child(bodies_.size(), -1);
    for (int i : order_) {
        const int p = bodies_[static_cast<size_t>(i)].parent;
        ++children[static_cast<size_t>(p)];
        only_child[static_cast<size_t>(p)] = i;
    }
    for (int i : order_) {
        SimBody& b = bodies_[static_cast<size_t>(i)];
        const SimBody& parent = bodies_[static_cast<size_t>(b.parent)];
        const SimConstraint& c = constraints_[static_cast<size_t>(b.constraint)];
        const glm::quat inverse = glm::inverse(b.bind_rotation);
        const glm::quat parent_inverse = glm::inverse(parent.bind_rotation);
        b.pivot_local = inverse * (c.pivot - b.bind_position);
        b.pivot_parent = parent_inverse * (c.pivot - parent.bind_position);
        if (children[static_cast<size_t>(i)] == 1) {
            const SimBody& child = bodies_[static_cast<size_t>(only_child[static_cast<size_t>(i)])];
            b.end_local = inverse * (constraints_[static_cast<size_t>(child.constraint)].pivot - b.bind_position);
        } else {
            b.end_local = -b.pivot_local;
        }
        if (glm::length(b.end_local - b.pivot_local) < 1e-4f) {
            b.end_local = b.pivot_local + glm::vec3(0.0f, -0.05f, 0.0f);
        }
        b.length = glm::length(b.end_local - b.pivot_local);
        b.direction_local = (b.end_local - b.pivot_local) / b.length;
        b.relative_rotation = glm::normalize(parent_inverse * b.bind_rotation);
        // the cone: refA of the child turned onto refB of the parent, applied to the stick's bind direction
        const glm::vec3 direction = b.bind_rotation * b.direction_local;
        const glm::vec3 axis = RotationBetween(c.ref_a, c.ref_b) * direction;
        b.cone_axis_parent = glm::normalize(parent_inverse * axis);
        b.cone_cos = c.limited ? std::cos(glm::radians(std::clamp(c.limit_degrees, 0.0f, 180.0f))) : -1.0f;
    }
    for (int i : order_) {
        SimBody& b = bodies_[static_cast<size_t>(i)];
        const SimBody& parent = bodies_[static_cast<size_t>(b.parent)];
        b.linked_to_parent_end = parent.dynamic && children[static_cast<size_t>(b.parent)] == 1 &&
                                 glm::length(b.pivot_parent - parent.end_local) < 2e-3f;
    }

    // keyframed hit bodies and the distance each stick end keeps from them
    for (int i = 0; i < count; ++i) {
        const SimBody& b = bodies_[static_cast<size_t>(i)];
        if (b.hit && !b.dynamic && !b.bone.empty()) {
            hits_.push_back(i);
        }
    }
    hit_distance_.assign(order_.size() * hits_.size(), 0.0f);
    for (size_t k = 0; k < order_.size(); ++k) {
        const SimBody& b = bodies_[static_cast<size_t>(order_[k])];
        const glm::vec3 end = b.bind_position + b.bind_rotation * b.end_local;
        for (size_t h = 0; h < hits_.size(); ++h) {
            const SimBody& hit = bodies_[static_cast<size_t>(hits_[h])];
            glm::vec3 sa;
            glm::vec3 sb;
            HitSegment(hit, hit.bind_position, hit.bind_rotation, sa, sb);
            const float bind = glm::length(end - ClosestOnSegment(end, sa, sb));
            hit_distance_[k * hits_.size() + h] = std::min(b.radius + hit.radius, bind);
        }
    }

    int wind = 0;
    float max_limit = 0.0f;
    for (int i : order_) {
        wind += bodies_[static_cast<size_t>(i)].wind ? 1 : 0;
    }
    for (const SimConstraint& c : constraints_) {
        max_limit = std::max(max_limit, c.limited ? c.limit_degrees : 180.0f);
    }
    LogInfo("sim: {} {} bodies ({} units, {} dynamic, {} keyframed hit, {} wind x {}), {} constraints (limits up to {} degrees){}",
            name_, bodies_.size(), bound, order_.size(), hits_.size(), wind, wind_coefficient_, constraints_.size(), max_limit,
            convert_move_to_wind_ ? ", motion as wind" : "");
    if (order_.empty()) {
        return fail("no dynamic body with a parent");
    }
    return true;
}

void SimPhysics::Bind(const SimRig& rig, const Skeleton& skeleton) {
    rig_ = &rig;
    skeleton_ = &skeleton;
    const auto& bodies = rig.Bodies();
    bone_.assign(bodies.size(), -1);
    active_.assign(bodies.size(), false);
    for (size_t i = 0; i < bodies.size(); ++i) {
        bone_[i] = bodies[i].bone.empty() ? -1 : skeleton.FindName(bodies[i].bone);
        active_[i] = bone_[i] >= 0 && !bodies[i].dynamic;
    }
    int missing = 0;
    for (int i : rig.Order()) {
        const SimBody& b = bodies[static_cast<size_t>(i)];
        active_[static_cast<size_t>(i)] = bone_[static_cast<size_t>(i)] >= 0 && active_[static_cast<size_t>(b.parent)];
        missing += active_[static_cast<size_t>(i)] ? 0 : 1;
    }
    if (missing) {
        LogWarn("sim: {} {} of {} bodies have no bone or parent in the skeleton", rig.Name(), missing, rig.Order().size());
    }
    frame_.assign(bodies.size(), Frame{});
    previous_kinematic_.assign(bodies.size(), Frame{});
    end_.assign(rig.Order().size(), glm::vec3(0.0f));
    previous_end_ = end_;
    velocity_ = end_;
    // skeleton bones parents first, for moving the bones below the simulated ones
    const size_t n = skeleton.Size();
    std::vector<int> depth(n, -1);
    for (size_t i = 0; i < n; ++i) {
        int d = 0;
        int p = skeleton.parents[i];
        while (p >= 0 && static_cast<size_t>(p) < n && d <= static_cast<int>(n)) {
            ++d;
            p = skeleton.parents[static_cast<size_t>(p)];
        }
        depth[i] = d;
    }
    bone_order_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        bone_order_[i] = static_cast<int>(i);
    }
    std::stable_sort(bone_order_.begin(), bone_order_.end(), [&](int a, int b) { return depth[static_cast<size_t>(a)] < depth[static_cast<size_t>(b)]; });
    warm_ = true;
}

SimPhysics::Frame SimPhysics::Animated(const SimRig& rig, size_t body, const std::vector<glm::mat4>& world, const glm::mat4& model_world) const {
    const SimBody& b = rig.Bodies()[body];
    const glm::mat4 m = model_world * world[static_cast<size_t>(bone_[body])] * glm::translate(glm::mat4(1.0f), b.offset_position) *
                        glm::mat4_cast(b.offset_rotation);
    return Frame{glm::vec3(m[3]), RotationOf(m)};
}

void SimPhysics::Snap(const SimRig& rig, const std::vector<glm::mat4>& world, const glm::mat4& model_world) {
    const auto& bodies = rig.Bodies();
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (active_[i]) {
            frame_[i] = Animated(rig, i, world, model_world);
            if (!bodies[i].dynamic) {
                previous_kinematic_[i] = frame_[i];
            }
        }
    }
    for (size_t k = 0; k < rig.Order().size(); ++k) {
        const size_t i = static_cast<size_t>(rig.Order()[k]);
        if (active_[i]) {
            end_[k] = frame_[i].position + frame_[i].rotation * bodies[i].end_local;
        }
        previous_end_[k] = end_[k];
        velocity_[k] = glm::vec3(0.0f);
    }
    warm_ = false;
}

// One Gauss-Seidel visit of a dynamic body: the joint distance (both stick ends by mass when the joint is the parent's stick
// end), the keyframed hit shapes, the cone limit around the parent's axis, then the body's frame from its stick with the
// twist of its parent (stopTwistFlag). The closing pass (constrain false, parents first) keeps the stick length and the
// cone only, so every link ends rigid and inside its limit after the children moved the parents' ends.
void SimPhysics::Solve(const SimRig& rig, size_t k, const std::vector<int>& slot, bool constrain) {
    const auto& bodies = rig.Bodies();
    const size_t i = static_cast<size_t>(rig.Order()[k]);
    if (!active_[i]) {
        return;
    }
    const SimBody& b = bodies[i];
    const size_t p = static_cast<size_t>(b.parent);
    const Frame& parent = frame_[p];
    const int parent_slot = b.linked_to_parent_end ? slot[p] : -1;
    auto pivot_of = [&]() { return parent_slot >= 0 ? end_[static_cast<size_t>(parent_slot)] : parent.position + parent.rotation * b.pivot_parent; };
    glm::vec3 pivot = pivot_of();
    glm::vec3& end = end_[k];
    const glm::quat follow = glm::normalize(parent.rotation * b.relative_rotation);

    glm::vec3 delta = end - pivot;
    float length = glm::length(delta);
    if (!constrain) {
        // the closing pass: the frame from the stick and the cone, after the children moved the stick ends
    } else if (length < 1e-6f) {
        end = pivot + follow * b.direction_local * b.length;
    } else {
        const glm::vec3 correction = delta * ((length - b.length) / length);
        if (parent_slot >= 0) {
            const float wa = 1.0f / std::max(b.mass, 1e-3f);
            const float wb = 1.0f / std::max(bodies[p].mass, 1e-3f);
            end -= correction * (wa / (wa + wb));
            end_[static_cast<size_t>(parent_slot)] += correction * (wb / (wa + wb));
            pivot = pivot_of();
        } else {
            end -= correction;
        }
    }

    for (size_t h = 0; constrain && h < rig.Hits().size(); ++h) {
        const size_t hi = static_cast<size_t>(rig.Hits()[h]);
        if (!active_[hi]) {
            continue;
        }
        glm::vec3 sa;
        glm::vec3 sb;
        HitSegment(bodies[hi], frame_[hi].position, frame_[hi].rotation, sa, sb);
        const glm::vec3 closest = ClosestOnSegment(end, sa, sb);
        const glm::vec3 out = end - closest;
        const float distance = glm::length(out);
        const float keep = rig.HitDistance(k, h);
        if (distance < keep && distance > 1e-6f) {
            end = closest + out * (keep / distance);
        }
    }

    // the cone last, as a hard limit: in the closing pass too, against the parent's final frame
    if (b.cone_cos > -1.0f) {
        const glm::vec3 axis = parent.rotation * b.cone_axis_parent;
        const glm::vec3 offset = end - pivot;
        const float distance = glm::length(offset);
        const glm::vec3 d = distance > 1e-6f ? offset / distance : axis;
        if (glm::dot(d, axis) < b.cone_cos) {
            end = pivot + ClampToCone(axis, d, b.cone_cos) * b.length;
        }
    }
    delta = end - pivot;
    length = glm::length(delta);
    const glm::vec3 direction = length > 1e-6f ? delta / length : follow * b.direction_local;
    end = pivot + direction * b.length;

    const glm::quat rotation = glm::normalize(RotationBetween(follow * b.direction_local, direction) * follow);
    frame_[i] = Frame{pivot - rotation * b.pivot_local, rotation};
}

bool SimPhysics::Step(const SimRig& rig, const Skeleton& skeleton, std::vector<glm::mat4>& world, const glm::mat4& model_world, float dt,
                      const glm::vec3& wind) {
    if (!SimPhysicsEnabled() || rig.DynamicCount() == 0 || world.size() < skeleton.Size()) {
        return false;
    }
    if (rig_ != &rig || skeleton_ != &skeleton) {
        Bind(rig, skeleton);
    }
    const auto& bodies = rig.Bodies();
    const auto& order = rig.Order();
    if (!(dt >= 0.0f) || dt > kMaxFrameTime) {
        warm_ = true;
    }

    std::vector<Frame> kinematic(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (active_[i] && !bodies[i].dynamic) {
            kinematic[i] = Animated(rig, i, world, model_world);
            if (!warm_ && glm::length(kinematic[i].position - previous_kinematic_[i].position) > kTeleportDistance) {
                warm_ = true;
            }
        }
    }
    if (warm_) {
        Snap(rig, world, model_world);
        return true;
    }

    std::vector<int> slot(bodies.size(), -1);
    for (size_t k = 0; k < order.size(); ++k) {
        slot[static_cast<size_t>(order[k])] = static_cast<int>(k);
    }
    // substeps of at most kStep that end on this frame's pose; the keyframed bodies move along between the two frames
    const int steps = dt > 1e-6f ? std::clamp(static_cast<int>(std::ceil(dt / kStep - 1e-3f)), 1, kMaxSubsteps) : 1;
    const float h = dt > 1e-6f ? dt / static_cast<float>(steps) : 0.0f;
    const glm::vec3 gravity(0.0f, -kGravity, 0.0f);
    for (int s = 0; s < steps; ++s) {
        const float f = static_cast<float>(s + 1) / static_cast<float>(steps);
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (active_[i] && !bodies[i].dynamic) {
                frame_[i].position = glm::mix(previous_kinematic_[i].position, kinematic[i].position, f);
                frame_[i].rotation = glm::normalize(glm::slerp(previous_kinematic_[i].rotation, kinematic[i].rotation, f));
            }
        }
        if (h > 0.0f) {
            for (size_t k = 0; k < order.size(); ++k) {
                const SimBody& b = bodies[static_cast<size_t>(order[k])];
                if (!active_[static_cast<size_t>(order[k])]) {
                    continue;
                }
                glm::vec3 v = velocity_[k] * std::exp(-b.linear_damping * h);
                glm::vec3 a = b.no_gravity ? glm::vec3(0.0f) : gravity;
                if (b.wind) {
                    // convertMoveToWind: the air acts on the body's velocity through it, not only on the wind
                    const glm::vec3 air = rig.ConvertMoveToWind() ? wind - v : wind;
                    a += air * (kWindDrag * rig.WindCoefficient());
                }
                v += a * h;
                const float speed = glm::length(v);
                if (speed > b.max_linear_velocity && speed > 0.0f) {
                    v *= b.max_linear_velocity / speed;
                }
                previous_end_[k] = end_[k];
                end_[k] += v * h;
            }
        }
        for (int it = 0; it < kIterations; ++it) {
            for (size_t k = 0; k < order.size(); ++k) {
                Solve(rig, k, slot, true);
            }
        }
        for (size_t k = 0; k < order.size(); ++k) {
            Solve(rig, k, slot, false);
        }
        if (h > 0.0f) {
            for (size_t k = 0; k < order.size(); ++k) {
                velocity_[k] = (end_[k] - previous_end_[k]) / h;
            }
        }
    }
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (active_[i] && !bodies[i].dynamic) {
            previous_kinematic_[i] = kinematic[i];
        }
    }

    // the bones of the simulated bodies, then every other bone below them moved with its parent
    const glm::mat4 to_model = glm::inverse(model_world);
    std::vector<glm::mat4> delta(world.size(), glm::mat4(1.0f));
    std::vector<uint8_t> moved(world.size(), 0);
    for (int index : order) {
        const size_t i = static_cast<size_t>(index);
        if (!active_[i]) {
            continue;
        }
        const SimBody& b = bodies[i];
        const size_t bone = static_cast<size_t>(bone_[i]);
        const glm::mat4 body = glm::translate(glm::mat4(1.0f), frame_[i].position) * glm::mat4_cast(frame_[i].rotation);
        const glm::mat4 m = to_model * body * glm::mat4_cast(glm::inverse(b.offset_rotation)) * glm::translate(glm::mat4(1.0f), -b.offset_position);
        delta[bone] = m * glm::inverse(world[bone]);
        world[bone] = m;
        moved[bone] = 2;
    }
    for (int index : bone_order_) {
        const size_t i = static_cast<size_t>(index);
        const int parent = skeleton.parents[i];
        if (moved[i] || parent < 0 || static_cast<size_t>(parent) >= world.size() || !moved[static_cast<size_t>(parent)]) {
            continue;
        }
        delta[i] = delta[static_cast<size_t>(parent)];
        world[i] = delta[i] * world[i];
        moved[i] = 1;
    }
    return true;
}

}
