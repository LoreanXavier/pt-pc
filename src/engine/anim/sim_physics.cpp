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

glm::vec3 ClosestOnSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    const float t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

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
                b.max_angular_velocity = file.GetFloat(*e, "maxAngularVelocity", 0, 180.0f);
                b.collision_id = file.GetInt(*e, "collisionId");
                b.friction = file.GetFloat(*e, "friction");
                b.permitted_depth = file.GetFloat(*e, "permittedDepth");
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

}
