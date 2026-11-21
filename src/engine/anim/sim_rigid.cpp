#include "engine/anim/sim_rigid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"

namespace pt::anim {
namespace {

constexpr float kSubStep = 1.0f / 60.0f;
constexpr float kMaxFrame = 1.0f / 15.0f;
constexpr float kGravity = 9.8f;
constexpr int kVelocityIterations = 3;
constexpr int kPositionIterations = 2;
constexpr float kContactBias = 0.05f;
constexpr float kContactCorrection = 0.95f;
constexpr float kWindDrag = 2.0f;
constexpr float kJointBias = 0.5f;
constexpr float kJointCorrection = 0.5f;
constexpr float kLimitBias = 0.05f;
constexpr float kLimitCorrection = 0.95f;
constexpr float kTeleportDistance = 1.0f;

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

glm::mat3 Skew(const glm::vec3& v) {
    return glm::mat3(0.0f, v.z, -v.y, -v.z, 0.0f, v.x, v.y, -v.x, 0.0f);
}

glm::quat Turned(const glm::quat& q, const glm::vec3& turn) {
    const glm::quat dq(0.0f, turn.x, turn.y, turn.z);
    return glm::normalize(q + 0.5f * (dq * q));
}

glm::vec3 RotationVector(const glm::quat& from, const glm::quat& to) {
    glm::quat d = to * glm::inverse(from);
    if (d.w < 0.0f) {
        d = -d;
    }
    const glm::vec3 v(d.x, d.y, d.z);
    const float s = glm::length(v);
    if (s < 1e-7f) {
        return v * 2.0f;
    }
    return v * (2.0f * std::atan2(s, d.w) / s);
}

glm::vec3 Moments(const SimBody& b) {
    const float m = std::max(b.mass, 1e-4f);
    const glm::vec3 s = b.shape.size;
    if (b.shape.type == 5) {
        const float r = std::max(s.x, 1e-4f);
        const float h = std::max(s.y, 0.0f);
        const float cylinder = glm::pi<float>() * r * r * 2.0f * h;
        const float ball = 4.0f / 3.0f * glm::pi<float>() * r * r * r;
        const float mc = m * cylinder / (cylinder + ball);
        const float ms = m - mc;
        const float along = mc * r * r * 0.5f + ms * 0.4f * r * r;
        const float across = mc * (r * r * 0.25f + (2.0f * h) * (2.0f * h) / 12.0f) + ms * (0.4f * r * r + h * h + 0.375f * h * r);
        return glm::vec3(across, along, across);
    }
    if (b.shape.type == 2) {
        return glm::vec3(m / 3.0f * (s.y * s.y + s.z * s.z), m / 3.0f * (s.x * s.x + s.z * s.z), m / 3.0f * (s.x * s.x + s.y * s.y));
    }
    const float r = std::max(s.x, 1e-4f);
    return glm::vec3(0.4f * m * r * r);
}

glm::quat Arc(const glm::vec3& a, const glm::vec3& b) {
    const float c = glm::dot(a, b);
    if (c < -0.999999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
        if (glm::dot(axis, axis) < 1e-6f) {
            axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), a);
        }
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 v = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + c, v.x, v.y, v.z));
}

glm::vec3 Perpendicular(const glm::vec3& v) {
    const glm::vec3 a = std::abs(v.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::normalize(glm::cross(v, a));
}

}

bool SimRigid::BodyFrame(size_t body, glm::vec3& position, glm::quat& rotation) const {
    if (body >= state_.size() || !active_[body]) {
        return false;
    }
    position = state_[body].position;
    rotation = state_[body].rotation;
    return true;
}

bool SimRigid::SetBody(size_t body, const glm::vec3& position, const glm::quat& rotation, const glm::vec3& velocity, const glm::vec3& spin) {
    if (body >= state_.size() || !active_[body] || !dynamic_[body]) {
        return false;
    }
    state_[body] = State{position, glm::normalize(rotation), velocity, spin};
    if (!history_.empty()) {
        const glm::mat4 to_model = glm::inverse(last_model_);
        history_.back().position[body] = glm::vec3(to_model * glm::vec4(state_[body].position, 1.0f));
        history_.back().rotation[body] = glm::normalize(RotationOf(to_model) * state_[body].rotation);
    }
    return true;
}

void SimRigid::Remember(const glm::mat4& model_world) {
    const size_t n = state_.size();
    const glm::mat4 to_model = glm::inverse(model_world);
    const glm::quat to_model_rotation = RotationOf(to_model);
    if (history_.empty() || history_.back().time != time_) {
        history_.emplace_back();
    }
    Snapshot& now = history_.back();
    now.time = time_;
    now.position.resize(n);
    now.rotation.resize(n);
    for (size_t i = 0; i < n; ++i) {
        now.position[i] = glm::vec3(to_model * glm::vec4(state_[i].position, 1.0f));
        now.rotation[i] = glm::normalize(to_model_rotation * state_[i].rotation);
    }
    if (delay_ > 0.0f) {
        const double at = time_ - delay_;
        while (history_.size() > 2 && history_[1].time <= at) {
            history_.pop_front();
        }
    } else {
        while (history_.size() > (delay_ < 0.0f ? 2u : 1u)) {
            history_.pop_front();
        }
    }
}

void SimRigid::Drawn(std::vector<glm::vec3>& position, std::vector<glm::quat>& rotation) const {
    const Snapshot& last = history_.back();
    if (delay_ < 0.0f) {
        const Snapshot& before = history_.front();
        position = before.position;
        rotation = before.rotation;
        return;
    }
    if (delay_ == 0.0f) {
        position = last.position;
        rotation = last.rotation;
        return;
    }
    const double at = time_ - delay_;
    size_t k = 0;
    while (k + 1 < history_.size() && history_[k + 1].time <= at) {
        ++k;
    }
    const Snapshot& a = history_[k];
    const Snapshot& b = history_[std::min(k + 1, history_.size() - 1)];
    const double span = b.time - a.time;
    const float f = span > 1e-9 ? static_cast<float>(std::clamp((at - a.time) / span, 0.0, 1.0)) : 0.0f;
    position.resize(a.position.size());
    rotation.resize(a.rotation.size());
    for (size_t i = 0; i < a.position.size(); ++i) {
        position[i] = glm::mix(a.position[i], b.position[i], f);
        rotation[i] = glm::normalize(glm::slerp(a.rotation[i], b.rotation[i], f));
    }
}

void SimRigid::Bind(const SimRig& rig, const Skeleton& skeleton) {
    rig_ = &rig;
    skeleton_ = &skeleton;
    const auto& bodies = rig.Bodies();
    const size_t n = bodies.size();
    bone_.assign(n, -1);
    active_.assign(n, false);
    dynamic_.assign(n, false);
    inverse_mass_.assign(n, 0.0f);
    inverse_inertia_.assign(n, glm::vec3(0.0f));
    max_speed_.assign(n, 100.0f);
    max_spin_.assign(n, glm::pi<float>());
    for (size_t i = 0; i < n; ++i) {
        const SimBody& b = bodies[i];
        bone_[i] = b.bone.empty() ? -1 : skeleton.FindName(b.bone);
        active_[i] = bone_[i] >= 0;
        dynamic_[i] = b.dynamic;
        if (b.dynamic) {
            inverse_mass_[i] = 1.0f / std::max(b.mass, 1e-4f);
            const glm::vec3 moments = Moments(b);
            inverse_inertia_[i] = glm::vec3(1.0f / std::max(moments.x, 1e-9f), 1.0f / std::max(moments.y, 1e-9f), 1.0f / std::max(moments.z, 1e-9f));
        }
        max_speed_[i] = b.max_linear_velocity > 0.0f ? b.max_linear_velocity : 100.0f;
        max_spin_[i] = glm::radians(b.max_angular_velocity > 0.0f ? b.max_angular_velocity : 180.0f);
    }
    joints_.clear();
    for (int index : rig.Order()) {
        const size_t i = static_cast<size_t>(index);
        const SimBody& b = bodies[i];
        if (b.parent < 0 || b.constraint < 0 || !active_[static_cast<size_t>(b.parent)]) {
            active_[i] = false;
            continue;
        }
        const SimBody& p = bodies[static_cast<size_t>(b.parent)];
        const SimConstraint& c = rig.Constraints()[static_cast<size_t>(b.constraint)];
        Joint j;
        j.child = index;
        j.parent = b.parent;
        j.pivot_child = glm::inverse(b.bind_rotation) * (c.pivot - b.bind_position);
        j.pivot_parent = glm::inverse(p.bind_rotation) * (c.pivot - p.bind_position);
        const float la = glm::length(c.ref_a);
        const float lb = glm::length(c.ref_b);
        j.axis_child = glm::inverse(b.bind_rotation) * (la > 1e-6f ? c.ref_a / la : glm::vec3(0.0f, -1.0f, 0.0f));
        j.axis_parent = glm::inverse(p.bind_rotation) * (lb > 1e-6f ? c.ref_b / lb : glm::vec3(0.0f, -1.0f, 0.0f));
        if (c.limited) {
            j.limit = glm::radians(std::clamp(c.limit_degrees, 0.0f, 180.0f));
            j.cos_limit = std::cos(j.limit);
        }
        j.twist = c.stop_twist;
        j.bind_relative = glm::normalize(glm::inverse(p.bind_rotation) * b.bind_rotation);
        joints_.push_back(j);
    }
    int missing = 0;
    for (int index : rig.Order()) {
        missing += active_[static_cast<size_t>(index)] ? 0 : 1;
    }
    if (missing) {
        LogWarn("sim: {} {} of {} bodies have no bone or parent in the skeleton", rig.Name(), missing, rig.Order().size());
    }
    segment_a_.assign(n, glm::vec3(0.0f));
    segment_b_.assign(n, glm::vec3(0.0f));
    radius_.assign(n, 0.0f);
    permitted_.assign(n, 0.0f);
    collision_id_.assign(n, 0);
    wind_.assign(n, false);
    for (size_t i = 0; i < n; ++i) {
        const SimBody& b = bodies[i];
        const float half = b.shape.type == 5 ? b.shape.size.y : 0.0f;
        segment_a_[i] = b.shape.offset + b.shape.rotation * glm::vec3(0.0f, -half, 0.0f);
        segment_b_[i] = b.shape.offset + b.shape.rotation * glm::vec3(0.0f, half, 0.0f);
        radius_[i] = b.shape.size.x;
        permitted_[i] = b.permitted_depth;
        collision_id_[i] = b.collision_id;
        wind_[i] = b.wind;
    }
    wind_coefficient_ = rig.WindCoefficient();
    move_to_wind_ = rig.ConvertMoveToWind();
    pairs_.clear();
    for (size_t i = 0; i < n; ++i) {
        if (!active_[i] || !dynamic_[i]) {
            continue;
        }
        for (size_t k = 0; k < n; ++k) {
            if (k != i && active_[k] && !dynamic_[k] && collision_id_[k] != collision_id_[i]) {
                pairs_.emplace_back(i, k);
            }
        }
    }
    state_.assign(n, State{});
    to_.assign(n, State{});
    const size_t bones = skeleton.Size();
    std::vector<int> depth(bones, 0);
    for (size_t i = 0; i < bones; ++i) {
        int d = 0;
        for (int p = skeleton.parents[i]; p >= 0 && static_cast<size_t>(p) < bones && d <= static_cast<int>(bones); p = skeleton.parents[static_cast<size_t>(p)]) {
            ++d;
        }
        depth[i] = d;
    }
    bone_order_.resize(bones);
    for (size_t i = 0; i < bones; ++i) {
        bone_order_[i] = static_cast<int>(i);
    }
    std::stable_sort(bone_order_.begin(), bone_order_.end(), [&](int a, int b) { return depth[static_cast<size_t>(a)] < depth[static_cast<size_t>(b)]; });
    warm_ = true;
}

SimRigid::State SimRigid::Animated(const SimRig& rig, size_t body, const std::vector<glm::mat4>& world, const glm::mat4& model_world) const {
    const SimBody& b = rig.Bodies()[body];
    const glm::mat4 m = model_world * world[static_cast<size_t>(bone_[body])] * glm::translate(glm::mat4(1.0f), b.offset_position) *
                        glm::mat4_cast(b.offset_rotation);
    State s;
    s.position = glm::vec3(m[3]);
    s.rotation = RotationOf(m);
    return s;
}

glm::mat3 SimRigid::InverseInertia(size_t body) const {
    if (!dynamic_[body]) {
        return glm::mat3(0.0f);
    }
    const glm::mat3 r = glm::mat3_cast(state_[body].rotation);
    const glm::vec3& d = inverse_inertia_[body];
    return r * glm::mat3(d.x, 0.0f, 0.0f, 0.0f, d.y, 0.0f, 0.0f, 0.0f, d.z) * glm::transpose(r);
}

float SimRigid::Twist(const Joint& j, const glm::quat& relative, const glm::vec3& axis) const {
    const glm::vec3 bind_axis = glm::normalize(j.bind_relative * j.axis_child);
    const glm::quat zero = Arc(j.axis_parent, axis) * Arc(bind_axis, j.axis_parent) * j.bind_relative;
    const glm::quat t = relative * glm::inverse(zero);
    return std::remainder(2.0f * std::atan2(glm::dot(glm::vec3(t.x, t.y, t.z), axis), t.w), glm::two_pi<float>());
}

namespace {
void ClosestPoints(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1, glm::vec3& cp, glm::vec3& cq) {
    const glm::vec3 d1 = p1 - p0;
    const glm::vec3 d2 = q1 - q0;
    const glm::vec3 r = p0 - q0;
    const float a = glm::dot(d1, d1);
    const float e = glm::dot(d2, d2);
    const float f = glm::dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= 1e-9f && e <= 1e-9f) {
    } else if (a <= 1e-9f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e <= 1e-9f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2);
            const float den = a * e - b * b;
            s = den > 1e-9f ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    cp = p0 + d1 * s;
    cq = q0 + d2 * t;
}
}

bool SimRigid::Touch(size_t a, size_t b, Contact& out) const {
    const State& sa = state_[a];
    const State& sb = state_[b];
    glm::vec3 pa, pb;
    ClosestPoints(sa.position + sa.rotation * segment_a_[a], sa.position + sa.rotation * segment_b_[a], sb.position + sb.rotation * segment_a_[b],
                  sb.position + sb.rotation * segment_b_[b], pa, pb);
    glm::vec3 n = pa - pb;
    const float d = glm::length(n);
    const float depth = radius_[a] + radius_[b] - d;
    if (depth <= 0.0f) {
        return false;
    }
    n = d > 1e-6f ? n / d : glm::vec3(0.0f, 1.0f, 0.0f);
    out.a = a;
    out.b = b;
    out.normal = n;
    out.depth = depth;
    out.permitted = std::max(permitted_[a], permitted_[b]);
    out.ra = (pa - n * radius_[a]) - sa.position;
    out.rb = (pb + n * radius_[b]) - sb.position;
    return true;
}

void SimRigid::FindContacts() {
    contacts_.clear();
    for (const auto& [a, b] : pairs_) {
        Contact c;
        if (Touch(a, b, c)) {
            contacts_.push_back(c);
        }
    }
}

void SimRigid::SolveVelocities(float h) {
    for (int iteration = 0; iteration < kVelocityIterations; ++iteration) {
        for (const Joint& j : joints_) {
            const size_t a = static_cast<size_t>(j.child);
            const size_t b = static_cast<size_t>(j.parent);
            if (!active_[a]) {
                continue;
            }
            State& sa = state_[a];
            State& sb = state_[b];
            const glm::mat3 ia = InverseInertia(a);
            const glm::mat3 ib = InverseInertia(b);
            const float ma = inverse_mass_[a];
            const float mb = inverse_mass_[b];
            {
                const glm::vec3 ra = sa.rotation * j.pivot_child;
                const glm::vec3 rb = sb.rotation * j.pivot_parent;
                const glm::vec3 error = (sa.position + ra) - (sb.position + rb);
                const glm::vec3 relative = (sa.velocity + glm::cross(sa.spin, ra)) - (sb.velocity + glm::cross(sb.spin, rb));
                const glm::mat3 sra = Skew(ra);
                const glm::mat3 srb = Skew(rb);
                const glm::mat3 k = glm::mat3(ma + mb) - sra * ia * sra - srb * ib * srb;
                const glm::vec3 impulse = -(glm::inverse(k) * (relative + error * (kJointBias / h)));
                sa.velocity += impulse * ma;
                sa.spin += ia * glm::cross(ra, impulse);
                sb.velocity -= impulse * mb;
                sb.spin -= ib * glm::cross(rb, impulse);
            }
            if (j.cos_limit > -1.0f) {
                const glm::vec3 da = sa.rotation * j.axis_child;
                const glm::vec3 db = sb.rotation * j.axis_parent;
                const float c = std::clamp(glm::dot(da, db), -1.0f, 1.0f);
                const float angle = std::acos(c);
                glm::vec3 n = glm::cross(da, db);
                const float ln = glm::length(n);
                if (ln > 1e-6f && angle > j.limit) {
                    n /= ln;
                    const float rate = glm::dot(sb.spin - sa.spin, n);
                    const float k = glm::dot(n, ia * n) + glm::dot(n, ib * n);
                    if (k > 1e-12f) {
                        float lambda = -(rate + (angle - j.limit) * (kLimitBias / h)) / k;
                        lambda = std::min(lambda, 0.0f);
                        sa.spin -= ia * n * lambda;
                        sb.spin += ib * n * lambda;
                    }
                }
            }
            if (j.twist) {
                const glm::vec3 axis_in_parent = glm::normalize(glm::inverse(sb.rotation) * (sa.rotation * j.axis_child));
                const float wrapped = Twist(j, glm::inverse(sb.rotation) * sa.rotation, axis_in_parent);
                const glm::vec3 t = sb.rotation * axis_in_parent;
                const float rate = glm::dot(sa.spin - sb.spin, t);
                const float k = glm::dot(t, ia * t) + glm::dot(t, ib * t);
                if (k > 1e-12f) {
                    const float lambda = -(rate + wrapped * (kLimitBias / h)) / k;
                    sa.spin += ia * t * lambda;
                    sb.spin -= ib * t * lambda;
                }
            }
        }
        for (Contact& c : contacts_) {
            State& sa = state_[c.a];
            State& sb = state_[c.b];
            const glm::mat3 ia = InverseInertia(c.a);
            const glm::mat3 ib = InverseInertia(c.b);
            const float ma = inverse_mass_[c.a];
            const float mb = inverse_mass_[c.b];
            const glm::vec3 relative = (sa.velocity + glm::cross(sa.spin, c.ra)) - (sb.velocity + glm::cross(sb.spin, c.rb));
            const float vn = glm::dot(relative, c.normal);
            const glm::vec3 ca = glm::cross(c.ra, c.normal);
            const glm::vec3 cb = glm::cross(c.rb, c.normal);
            const float k = ma + mb + glm::dot(ca, ia * ca) + glm::dot(cb, ib * cb);
            if (k <= 1e-12f) {
                continue;
            }
            const float bias = std::max(c.depth - c.permitted, 0.0f) * (kContactBias / h);
            const float lambda = (bias - vn) / k;
            const float total = std::max(c.impulse + lambda, 0.0f);
            const float applied = total - c.impulse;
            c.impulse = total;
            const glm::vec3 p = c.normal * applied;
            sa.velocity += p * ma;
            sa.spin += ia * glm::cross(c.ra, p);
            sb.velocity -= p * mb;
            sb.spin -= ib * glm::cross(c.rb, p);
        }
    }
}

void SimRigid::SolvePositions() {
    for (int iteration = 0; iteration < kPositionIterations; ++iteration) {
        for (const Joint& j : joints_) {
            const size_t a = static_cast<size_t>(j.child);
            const size_t b = static_cast<size_t>(j.parent);
            if (!active_[a]) {
                continue;
            }
            State& sa = state_[a];
            State& sb = state_[b];
            const glm::mat3 ia = InverseInertia(a);
            const glm::mat3 ib = InverseInertia(b);
            const float ma = inverse_mass_[a];
            const float mb = inverse_mass_[b];
            {
                const glm::vec3 ra = sa.rotation * j.pivot_child;
                const glm::vec3 rb = sb.rotation * j.pivot_parent;
                const glm::vec3 error = (sa.position + ra) - (sb.position + rb);
                const glm::mat3 sra = Skew(ra);
                const glm::mat3 srb = Skew(rb);
                const glm::mat3 k = glm::mat3(ma + mb) - sra * ia * sra - srb * ib * srb;
                const glm::vec3 impulse = -(glm::inverse(k) * (error * kJointCorrection));
                sa.position += impulse * ma;
                sa.rotation = Turned(sa.rotation, ia * glm::cross(ra, impulse));
                sb.position -= impulse * mb;
                sb.rotation = Turned(sb.rotation, -(ib * glm::cross(rb, impulse)));
            }
            if (j.cos_limit > -1.0f) {
                const glm::vec3 da = sa.rotation * j.axis_child;
                const glm::vec3 db = sb.rotation * j.axis_parent;
                const float angle = std::acos(std::clamp(glm::dot(da, db), -1.0f, 1.0f));
                glm::vec3 n = glm::cross(da, db);
                const float ln = glm::length(n);
                if (ln > 1e-6f && angle > j.limit) {
                    n /= ln;
                    const float k = glm::dot(n, ia * n) + glm::dot(n, ib * n);
                    if (k > 1e-12f) {
                        const float lambda = (angle - j.limit) * kLimitCorrection / k;
                        sa.rotation = Turned(sa.rotation, ia * n * lambda);
                        sb.rotation = Turned(sb.rotation, -(ib * n * lambda));
                    }
                }
            }
            if (j.twist) {
                const glm::vec3 axis_in_parent = glm::normalize(glm::inverse(sb.rotation) * (sa.rotation * j.axis_child));
                const float twist = Twist(j, glm::inverse(sb.rotation) * sa.rotation, axis_in_parent);
                const glm::vec3 t = sb.rotation * axis_in_parent;
                const float k = glm::dot(t, ia * t) + glm::dot(t, ib * t);
                if (k > 1e-12f) {
                    const float lambda = twist * kLimitCorrection / k;
                    sa.rotation = Turned(sa.rotation, -(ia * t * lambda));
                    sb.rotation = Turned(sb.rotation, ib * t * lambda);
                }
            }
        }
        for (const auto& [a, b] : pairs_) {
            Contact c;
            if (!Touch(a, b, c) || c.depth <= c.permitted) {
                continue;
            }
            State& sa = state_[a];
            State& sb = state_[b];
            const glm::mat3 ia = InverseInertia(a);
            const glm::mat3 ib = InverseInertia(b);
            const glm::vec3 ca = glm::cross(c.ra, c.normal);
            const glm::vec3 cb = glm::cross(c.rb, c.normal);
            const float k = inverse_mass_[a] + inverse_mass_[b] + glm::dot(ca, ia * ca) + glm::dot(cb, ib * cb);
            if (k <= 1e-12f) {
                continue;
            }
            const glm::vec3 p = c.normal * ((c.depth - c.permitted) * kContactCorrection / k);
            sa.position += p * inverse_mass_[a];
            sa.rotation = Turned(sa.rotation, ia * glm::cross(c.ra, p));
            sb.position -= p * inverse_mass_[b];
            sb.rotation = Turned(sb.rotation, -(ib * glm::cross(c.rb, p)));
        }
    }
}

bool SimRigid::Step(const SimRig& rig, const Skeleton& skeleton, std::vector<glm::mat4>& world, const glm::mat4& model_world, float dt,
                    const glm::vec3& wind) {
    if (!SimPhysicsEnabled() || rig.DynamicCount() == 0 || world.size() < skeleton.Size()) {
        return false;
    }
    if (rig_ != &rig || skeleton_ != &skeleton) {
        Bind(rig, skeleton);
    }
    const auto& bodies = rig.Bodies();
    const size_t n = bodies.size();
    if (!(dt >= 0.0f)) {
        warm_ = true;
    }
    const glm::vec3 model_position(model_world[3]);
    const glm::vec3 displacement = warm_ ? glm::vec3(0.0f) : model_position - previous_model_;
    if (move_to_wind_ && !warm_) {
        for (size_t i = 0; i < n; ++i) {
            state_[i].position += displacement;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (active_[i] && !dynamic_[i]) {
            to_[i] = Animated(rig, i, world, model_world);
            if (!warm_ && glm::length(to_[i].position - state_[i].position) > kTeleportDistance) {
                warm_ = true;
            }
        }
    }
    if (warm_) {
        for (size_t i = 0; i < n; ++i) {
            if (active_[i]) {
                state_[i] = Animated(rig, i, world, model_world);
                to_[i] = state_[i];
            }
        }
        warm_ = false;
        time_ = 0.0;
        history_.clear();
    } else {
        glm::vec3 air = wind;
        if (move_to_wind_ && dt > 1e-6f) {
            air -= displacement / dt;
        }
        float remaining = std::min(dt, kMaxFrame);
        while (remaining > 1e-6f) {
            const float h = remaining <= kSubStep * 1.25f ? remaining : kSubStep;
            remaining -= h;
            for (size_t i = 0; i < n; ++i) {
                if (!active_[i]) {
                    continue;
                }
                if (!dynamic_[i]) {
                    State& s = state_[i];
                    s.velocity = (to_[i].position - s.position) / h;
                    s.spin = RotationVector(s.rotation, to_[i].rotation) / h;
                    continue;
                }
                const SimBody& b = bodies[i];
                State& s = state_[i];
                if (!b.no_gravity) {
                    s.velocity.y -= kGravity * h;
                }
                if (wind_[i]) {
                    s.velocity += (air - s.velocity) * (kWindDrag * wind_coefficient_ * h);
                }
                s.velocity *= std::pow(std::clamp(1.0f - b.linear_damping, 0.0f, 1.0f), h);
                s.spin *= std::pow(std::clamp(1.0f - b.angular_damping, 0.0f, 1.0f), h);
            }
            FindContacts();
            SolveVelocities(h);
            for (size_t i = 0; i < n; ++i) {
                if (!active_[i]) {
                    continue;
                }
                State& s = state_[i];
                if (!dynamic_[i]) {
                    s.position = to_[i].position;
                    s.rotation = to_[i].rotation;
                    continue;
                }
                const float speed = glm::length(s.velocity);
                if (speed > max_speed_[i]) {
                    s.velocity *= max_speed_[i] / speed;
                }
                const float spin = glm::length(s.spin);
                if (spin > max_spin_[i]) {
                    s.spin *= max_spin_[i] / spin;
                }
                s.position += s.velocity * h;
                s.rotation = Turned(s.rotation, s.spin * h);
            }
            SolvePositions();
        }
        for (size_t i = 0; i < n; ++i) {
            if (active_[i] && !dynamic_[i]) {
                state_[i].position = to_[i].position;
                state_[i].rotation = to_[i].rotation;
            }
        }
        time_ += std::max(dt, 0.0f);
    }
    previous_model_ = model_position;
    last_model_ = model_world;
    Remember(model_world);

    static const bool trace = std::getenv("PT_SIM_TRACE") != nullptr;
    if (trace) {
        std::string line;
        for (const Joint& j : joints_) {
            const size_t i = static_cast<size_t>(j.child);
            if (!active_[i]) {
                continue;
            }
            const State animated = Animated(rig, i, world, model_world);
            const float turn = glm::degrees(glm::length(RotationVector(animated.rotation, state_[i].rotation)));
            line += std::format(" {} {:.2f} deg {:.1f} deg/s", bodies[i].bone, turn, glm::degrees(glm::length(state_[i].spin)));
        }
        LogInfo("sim trace: {} dt {:.4f}{}", rig.Name(), dt, line);
    }

    std::vector<glm::vec3> drawn_position;
    std::vector<glm::quat> drawn_rotation;
    Drawn(drawn_position, drawn_rotation);
    std::vector<glm::mat4> delta(world.size(), glm::mat4(1.0f));
    std::vector<uint8_t> moved(world.size(), 0);
    for (const Joint& j : joints_) {
        const size_t i = static_cast<size_t>(j.child);
        if (!active_[i]) {
            continue;
        }
        const SimBody& b = bodies[i];
        const size_t bone = static_cast<size_t>(bone_[i]);
        const glm::mat4 body = glm::translate(glm::mat4(1.0f), drawn_position[i]) * glm::mat4_cast(drawn_rotation[i]);
        const glm::mat4 m = body * glm::mat4_cast(glm::inverse(b.offset_rotation)) * glm::translate(glm::mat4(1.0f), -b.offset_position);
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
