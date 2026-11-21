#include "engine/anim/ragdoll.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace pt::anim {
namespace {

constexpr float kSubStep = 1.0f / 180.0f;
constexpr float kMaxDt = 1.0f / 20.0f;
constexpr float kGravity = 9.81f;
constexpr int kVelIters = 6;
constexpr int kPosIters = 4;
constexpr float kJointBias = 0.25f;
constexpr float kJointPos = 0.65f;
constexpr float kLimitBias = 0.12f;
constexpr float kLimitPos = 0.72f;
constexpr float kContactBias = 0.16f;
constexpr float kContactPos = 0.65f;
constexpr float kRestitution = 0.05f;
constexpr float kFriction = 0.72f;
constexpr float kLinearDamp = 0.45f;
constexpr float kAngularDamp = 2.6f;

glm::quat RotationOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float l = glm::length(r[c]);
        if (l > 1e-7f) r[c] /= l;
    }
    return glm::normalize(glm::quat_cast(r));
}

glm::mat3 Skew(const glm::vec3& v) {
    return glm::mat3(0.0f, v.z, -v.y, -v.z, 0.0f, v.x, v.y, -v.x, 0.0f);
}

glm::quat IntegrateQuat(const glm::quat& q, const glm::vec3& turn) {
    const float angle = glm::length(turn);
    if (angle < 1e-7f) return q;
    const glm::quat dq = glm::angleAxis(angle, turn / angle);
    return glm::normalize(dq * q);
}

glm::vec3 CapsuleInvInertia(float mass, float radius, float half_len, const glm::vec3& local_axis) {
    const float r = std::max(radius, 0.02f);
    const float h = std::max(half_len, 0.01f);
    const float cyl = glm::pi<float>() * r * r * 2.0f * h;
    const float sph = (4.0f / 3.0f) * glm::pi<float>() * r * r * r;
    const float mc = mass * cyl / (cyl + sph);
    const float ms = mass - mc;
    const float i_along = std::max(mc * 0.5f * r * r + ms * 0.4f * r * r, 1e-4f);
    const float i_across = std::max(mc * (0.25f * r * r + (2.0f * h) * (2.0f * h) / 12.0f) +
                                        ms * (0.4f * r * r + h * h + 0.375f * h * r),
                                    1e-4f);
    const glm::vec3 a = glm::abs(local_axis);
    glm::vec3 inertia(i_across);
    if (a.x >= a.y && a.x >= a.z) {
        inertia.x = i_along;
    } else if (a.y >= a.x && a.y >= a.z) {
        inertia.y = i_along;
    } else {
        inertia.z = i_along;
    }
    return glm::vec3(1.0f / inertia.x, 1.0f / inertia.y, 1.0f / inertia.z);
}

void ClosestPointsSegments(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1,
                           glm::vec3& cp, glm::vec3& cq) {
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

float SignedTwist(const glm::quat& rel, const glm::vec3& axis_in_parent) {
    const glm::vec3 v(rel.x, rel.y, rel.z);
    const glm::vec3 proj = axis_in_parent * glm::dot(v, axis_in_parent);
    glm::quat twist(rel.w, proj.x, proj.y, proj.z);
    const float len = glm::length(twist);
    if (len < 1e-6f) return 0.0f;
    twist /= len;
    const float s = glm::dot(glm::vec3(twist.x, twist.y, twist.z), axis_in_parent);
    return std::remainder(2.0f * std::atan2(s, twist.w), glm::two_pi<float>());
}

}

void Ragdoll::Clear() {
    active_ = false;
    asleep_ = false;
    age_ = 0.0f;
    still_time_ = 0.0f;
    impact_ = 0.0f;
    collapse_timer_ = 0.0f;
    joints_.clear();
    contacts_.clear();
    self_pairs_.clear();
    saved_local_.clear();
    part_of_bone_.clear();
    bone_order_.clear();
}

bool Ragdoll::Activate(const Skeleton& skeleton, const std::vector<glm::mat4>& bone_world, const glm::mat4& model_world,
                       float size, const glm::vec3& initial_vel) {
    Clear();
    const size_t n_bones = skeleton.Size();
    if (n_bones == 0 || bone_world.size() < n_bones) return false;

    size_ = std::max(size, 0.2f);
    const float mass_scale = size_ * size_ * size_;

    struct Spec {
        const char* start;
        const char* end;
        float mass;
        float radius;
        glm::vec3 fallback_end;
    };
    static constexpr std::array<Spec, kPartCount> kSpecs = {{
        {"SKL_000_WAIST", "SKL_001_SPINE", 14.0f, 0.140f, {0.0f, 0.14f, 0.0f}},
        {"SKL_001_SPINE", "SKL_003_NECK", 24.0f, 0.155f, {0.0f, 0.38f, 0.0f}},
        {"SKL_004_HEAD", nullptr, 5.0f, 0.108f, {0.0f, 0.17f, 0.0f}},
        {"SKL_011_LUARM", "SKL_012_LFARM", 2.4f, 0.062f, {0.26f, 0.0f, 0.0f}},
        {"SKL_012_LFARM", "SKL_013_LHAND", 1.8f, 0.052f, {0.26f, 0.0f, 0.0f}},
        {"SKL_021_RUARM", "SKL_022_RFARM", 2.4f, 0.062f, {-0.26f, 0.0f, 0.0f}},
        {"SKL_022_RFARM", "SKL_023_RHAND", 1.8f, 0.052f, {-0.26f, 0.0f, 0.0f}},
        {"SKL_030_LTHIGH", "SKL_031_LLEG", 8.5f, 0.088f, {0.0f, -0.42f, 0.0f}},
        {"SKL_031_LLEG", "SKL_032_LFOOT", 4.8f, 0.066f, {0.0f, -0.42f, 0.0f}},
        {"SKL_040_RTHIGH", "SKL_041_RLEG", 8.5f, 0.088f, {0.0f, -0.42f, 0.0f}},
        {"SKL_041_RLEG", "SKL_042_RFOOT", 4.8f, 0.066f, {0.0f, -0.42f, 0.0f}},
    }};

    part_of_bone_.assign(n_bones, -1);
    for (int i = 0; i < kPartCount; ++i) {
        const Spec& sp = kSpecs[static_cast<size_t>(i)];
        const int b0 = skeleton.FindName(sp.start);
        if (b0 < 0) return false;
        const int b1 = sp.end ? skeleton.FindName(sp.end) : -1;
        Body& body = bodies_[static_cast<size_t>(i)];
        body.bone = b0;
        body.end_bone = b1;
        const glm::mat4 w0 = model_world * bone_world[static_cast<size_t>(b0)];
        const glm::vec3 p0(w0[3]);
        const glm::quat r0 = RotationOf(w0);
        glm::vec3 p1 = b1 >= 0 ? glm::vec3((model_world * bone_world[static_cast<size_t>(b1)])[3])
                               : p0 + r0 * (sp.fallback_end * size_);
        if (i == kHead) {
            p1 = p0 + r0 * (sp.fallback_end * size_);
        }
        body.pos = 0.5f * (p0 + p1);
        body.prev_pos = body.pos;
        body.rot = r0;
        body.vel = initial_vel;
        body.ang = glm::vec3(0.0f);
        body.mass = sp.mass * mass_scale;
        body.inv_mass = 1.0f / body.mass;
        body.radius = sp.radius * size_;
        const glm::quat inv_r0 = glm::inverse(r0);
        body.joint_in_body = inv_r0 * (p0 - body.pos);
        const glm::vec3 end_in_body = inv_r0 * (p1 - body.pos);
        const glm::vec3 seg = end_in_body - body.joint_in_body;
        const float seg_len = glm::length(seg);
        const glm::vec3 dir = seg_len > 1e-4f ? seg / seg_len : glm::vec3(0.0f, 1.0f, 0.0f);
        const float trim = std::min(body.radius * 0.45f, seg_len * 0.28f);
        body.cap_a = body.joint_in_body + dir * trim;
        body.cap_b = end_in_body - dir * trim;
        const float half_len = std::max(0.5f * glm::length(body.cap_b - body.cap_a), 0.02f);
        body.inv_inertia_local = CapsuleInvInertia(body.mass, body.radius, half_len, dir);
        part_of_bone_[static_cast<size_t>(b0)] = i;
    }

    saved_local_.assign(n_bones, glm::mat4(1.0f));
    for (size_t i = 0; i < n_bones; ++i) {
        const int p = skeleton.parents[i];
        if (p >= 0 && static_cast<size_t>(p) < n_bones) {
            saved_local_[i] = glm::inverse(bone_world[static_cast<size_t>(p)]) * bone_world[i];
        } else {
            saved_local_[i] = bone_world[i];
        }
    }

    std::vector<int> depth(n_bones, 0);
    for (size_t i = 0; i < n_bones; ++i) {
        int d = 0;
        for (int p = skeleton.parents[i]; p >= 0 && static_cast<size_t>(p) < n_bones && d <= static_cast<int>(n_bones);
             p = skeleton.parents[static_cast<size_t>(p)]) {
            ++d;
        }
        depth[i] = d;
    }
    bone_order_.resize(n_bones);
    for (size_t i = 0; i < n_bones; ++i) bone_order_[i] = static_cast<int>(i);
    std::stable_sort(bone_order_.begin(), bone_order_.end(),
                     [&](int a, int b) { return depth[static_cast<size_t>(a)] < depth[static_cast<size_t>(b)]; });

    auto add_cone_joint = [&](int child, int parent, const glm::vec3& axis_c, const glm::vec3& axis_p,
                              float cone_deg, float twist_min_deg, float twist_max_deg) {
        Joint j;
        j.child = child;
        j.parent = parent;
        j.is_hinge = false;
        const Body& bc = bodies_[static_cast<size_t>(child)];
        const Body& bp = bodies_[static_cast<size_t>(parent)];
        const glm::vec3 world_pivot = bc.pos + bc.rot * bc.joint_in_body;
        j.pivot_child = bc.joint_in_body;
        j.pivot_parent = glm::inverse(bp.rot) * (world_pivot - bp.pos);
        j.axis_child = glm::normalize(axis_c);
        j.axis_parent = glm::normalize(axis_p);
        j.cone_limit = glm::radians(cone_deg);
        j.twist_min = glm::radians(twist_min_deg);
        j.twist_max = glm::radians(twist_max_deg);
        joints_.push_back(j);
    };

    auto add_hinge_joint = [&](int child, int parent, const glm::vec3& hinge_axis, const glm::vec3& ref_dir,
                               float min_deg, float max_deg) {
        Joint j;
        j.child = child;
        j.parent = parent;
        j.is_hinge = true;
        const Body& bc = bodies_[static_cast<size_t>(child)];
        const Body& bp = bodies_[static_cast<size_t>(parent)];
        const glm::vec3 world_pivot = bc.pos + bc.rot * bc.joint_in_body;
        j.pivot_child = bc.joint_in_body;
        j.pivot_parent = glm::inverse(bp.rot) * (world_pivot - bp.pos);
        j.hinge_axis_child = glm::normalize(hinge_axis);
        j.hinge_axis_parent = glm::normalize(hinge_axis);
        j.hinge_ref_child = glm::normalize(ref_dir);
        j.hinge_ref_parent = glm::normalize(ref_dir);
        j.hinge_min = glm::radians(min_deg);
        j.hinge_max = glm::radians(max_deg);
        joints_.push_back(j);
    };

    add_cone_joint(kChest, kPelvis, {0.0f, 1.0f, 0.0f}, {0.0f, 0.98f, 0.20f}, 34.0f, -28.0f, 28.0f);
    add_cone_joint(kHead, kChest, {0.0f, 1.0f, 0.0f}, {0.0f, 0.97f, 0.24f}, 42.0f, -45.0f, 45.0f);

    add_cone_joint(kLUpperArm, kChest, {1.0f, 0.0f, 0.0f}, {0.52f, -0.76f, 0.38f}, 80.0f, -45.0f, 45.0f);
    add_cone_joint(kRUpperArm, kChest, {-1.0f, 0.0f, 0.0f}, {-0.52f, -0.76f, 0.38f}, 80.0f, -45.0f, 45.0f);

    add_hinge_joint(kLForearm, kLUpperArm, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0.0f, 138.0f);
    add_hinge_joint(kRForearm, kRUpperArm, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, 0.0f, 138.0f);

    add_cone_joint(kLThigh, kPelvis, {0.0f, -1.0f, 0.0f}, {0.16f, -0.84f, 0.52f}, 66.0f, -26.0f, 26.0f);
    add_cone_joint(kRThigh, kPelvis, {0.0f, -1.0f, 0.0f}, {-0.16f, -0.84f, 0.52f}, 66.0f, -26.0f, 26.0f);

    add_hinge_joint(kLShin, kLThigh, {1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 0.0f, 132.0f);
    add_hinge_joint(kRShin, kRThigh, {1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 0.0f, 132.0f);

    self_pairs_ = {
        {kLThigh, kRThigh}, {kLShin, kRShin}, {kLThigh, kRShin}, {kRThigh, kLShin},
        {kLForearm, kPelvis}, {kRForearm, kPelvis}, {kLForearm, kChest}, {kRForearm, kChest},
        {kLForearm, kLThigh}, {kRForearm, kRThigh}, {kHead, kLUpperArm}, {kHead, kRUpperArm},
    };

    active_ = true;
    asleep_ = false;
    age_ = 0.0f;
    still_time_ = 0.0f;
    impact_ = 0.0f;
    return true;
}

void Ragdoll::AddImpulse(const glm::vec3& at, const glm::vec3& delta_v, float radius) {
    if (!active_) return;
    asleep_ = false;
    still_time_ = 0.0f;
    const float r = std::max(radius, 0.1f);
    for (int i = 0; i < kPartCount; ++i) {
        Body& b = bodies_[static_cast<size_t>(i)];
        const glm::vec3 pa = b.pos + b.rot * b.cap_a;
        const glm::vec3 pb = b.pos + b.rot * b.cap_b;
        const glm::vec3 ab = pb - pa;
        const float ab2 = glm::dot(ab, ab);
        const float t = ab2 > 1e-6f ? std::clamp(glm::dot(at - pa, ab) / ab2, 0.0f, 1.0f) : 0.5f;
        const glm::vec3 closest = pa + ab * t;
        const float d = glm::distance(at, closest);
        if (d >= r) continue;
        const float w = (1.0f - d / r) * (1.0f - d / r);
        const glm::vec3 dv = delta_v * w;
        b.vel += dv;
        const glm::vec3 arm = closest - b.pos;
        const glm::vec3 j_imp = dv * b.mass;
        b.ang += InvInertiaWorld(i) * glm::cross(arm, j_imp) * 0.45f;
    }
}

void Ragdoll::AddPartImpulse(Part part, const glm::vec3& delta_v, const glm::vec3& angular_kick) {
    if (!active_) return;
    const int idx = static_cast<int>(part);
    if (idx < 0 || idx >= kPartCount) return;
    asleep_ = false;
    still_time_ = 0.0f;
    bodies_[static_cast<size_t>(idx)].vel += delta_v;
    bodies_[static_cast<size_t>(idx)].ang += angular_kick;
}

void Ragdoll::SetCollapse(const glm::vec3& fall_dir, bool headshot) {
    collapse_timer_ = headshot ? 0.32f : 0.26f;
    collapse_headshot_ = headshot;
    glm::vec3 d(fall_dir.x, 0.0f, fall_dir.z);
    collapse_dir_ = glm::length(d) > 1e-3f ? glm::normalize(d) : glm::vec3(0.0f, 0.0f, -1.0f);
}

glm::mat3 Ragdoll::InvInertiaWorld(int idx) const {
    const Body& b = bodies_[static_cast<size_t>(idx)];
    const glm::mat3 r = glm::mat3_cast(b.rot);
    const glm::vec3& d = b.inv_inertia_local;
    return r * glm::mat3(d.x, 0.0f, 0.0f, 0.0f, d.y, 0.0f, 0.0f, 0.0f, d.z) * glm::transpose(r);
}

void Ragdoll::ApplyCollapseDrive(float h) {
    if (collapse_timer_ <= 0.0f) return;
    const float w = std::clamp(collapse_timer_ / 0.28f, 0.0f, 1.0f);
    for (int side = 0; side < 2; ++side) {
        Body& thigh = bodies_[static_cast<size_t>(side == 0 ? kLThigh : kRThigh)];
        Body& shin = bodies_[static_cast<size_t>(side == 0 ? kLShin : kRShin)];
        const glm::vec3 knee_axis = thigh.rot * glm::vec3(1.0f, 0.0f, 0.0f);
        const float buckle = (collapse_headshot_ ? 9.5f : 6.5f) * w * h;
        shin.ang += knee_axis * buckle;
        thigh.ang -= knee_axis * (buckle * 0.55f);
    }
    Body& pelvis = bodies_[static_cast<size_t>(kPelvis)];
    Body& chest = bodies_[static_cast<size_t>(kChest)];
    const glm::vec3 fold_axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), collapse_dir_);
    if (glm::length(fold_axis) > 1e-3f) {
        const glm::vec3 ax = glm::normalize(fold_axis);
        chest.ang += ax * ((collapse_headshot_ ? 4.5f : 3.2f) * w * h);
        pelvis.vel += collapse_dir_ * (0.45f * w * h);
    }
    collapse_timer_ = std::max(0.0f, collapse_timer_ - h);
}

void Ragdoll::GatherContacts(const std::vector<float>& floor_y) {
    contacts_.clear();
    for (int i = 0; i < kPartCount; ++i) {
        const Body& b = bodies_[static_cast<size_t>(i)];
        const float fy = floor_y[static_cast<size_t>(i)];
        const glm::vec3 pa = b.pos + b.rot * b.cap_a;
        const glm::vec3 pb = b.pos + b.rot * b.cap_b;
        const glm::vec3 pm = 0.5f * (pa + pb);
        const glm::vec3 pts[3] = {pa, pb, pm};
        for (int k = 0; k < 3; ++k) {
            const float depth = (fy + b.radius) - pts[k].y;
            if (depth > 0.0f) {
                Contact c;
                c.body_a = i;
                c.body_b = -1;
                c.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                c.ra = (pts[k] - glm::vec3(0.0f, b.radius, 0.0f)) - b.pos;
                c.depth = depth;
                contacts_.push_back(c);
            }
        }
    }
    for (const auto& [a, b] : self_pairs_) {
        const Body& ba = bodies_[static_cast<size_t>(a)];
        const Body& bb = bodies_[static_cast<size_t>(b)];
        glm::vec3 cp, cq;
        ClosestPointsSegments(ba.pos + ba.rot * ba.cap_a, ba.pos + ba.rot * ba.cap_b,
                              bb.pos + bb.rot * bb.cap_a, bb.pos + bb.rot * bb.cap_b, cp, cq);
        const glm::vec3 diff = cp - cq;
        const float dist = glm::length(diff);
        const float sum_r = (ba.radius + bb.radius) * 0.88f;
        const float depth = sum_r - dist;
        if (depth > 0.002f) {
            const glm::vec3 n = dist > 1e-5f ? diff / dist : glm::vec3(1.0f, 0.0f, 0.0f);
            Contact c;
            c.body_a = a;
            c.body_b = b;
            c.normal = n;
            c.ra = (cp - n * (ba.radius * 0.88f)) - ba.pos;
            c.rb = (cq + n * (bb.radius * 0.88f)) - bb.pos;
            c.depth = depth;
            contacts_.push_back(c);
        }
    }
}

void Ragdoll::SolveVelocities(float h) {
    for (int iter = 0; iter < kVelIters; ++iter) {
        for (const Joint& j : joints_) {
            Body& ba = bodies_[static_cast<size_t>(j.child)];
            Body& bb = bodies_[static_cast<size_t>(j.parent)];
            const glm::mat3 ia = InvInertiaWorld(j.child);
            const glm::mat3 ib = InvInertiaWorld(j.parent);

            {
                const glm::vec3 ra = ba.rot * j.pivot_child;
                const glm::vec3 rb = bb.rot * j.pivot_parent;
                const glm::vec3 err = (ba.pos + ra) - (bb.pos + rb);
                const glm::vec3 rel = (ba.vel + glm::cross(ba.ang, ra)) - (bb.vel + glm::cross(bb.ang, rb));
                const glm::mat3 sra = Skew(ra);
                const glm::mat3 srb = Skew(rb);
                const glm::mat3 k = glm::mat3(ba.inv_mass + bb.inv_mass) - sra * ia * sra - srb * ib * srb;
                const glm::vec3 imp = -(glm::inverse(k) * (rel + err * (kJointBias / h)));
                ba.vel += imp * ba.inv_mass;
                ba.ang += ia * glm::cross(ra, imp);
                bb.vel -= imp * bb.inv_mass;
                bb.ang -= ib * glm::cross(rb, imp);
            }

            if (j.is_hinge) {
                const glm::vec3 ac = ba.rot * j.hinge_axis_child;
                const glm::vec3 ap = bb.rot * j.hinge_axis_parent;
                const glm::vec3 err = glm::cross(ac, ap);
                const float err_len = glm::length(err);
                if (err_len > 1e-5f) {
                    const glm::vec3 n = err / err_len;
                    const float angle = std::asin(std::clamp(err_len, 0.0f, 1.0f));
                    const float rate = glm::dot(ba.ang - bb.ang, n);
                    const float k = glm::dot(n, ia * n) + glm::dot(n, ib * n);
                    if (k > 1e-9f) {
                        const float lambda = (angle * (kLimitBias / h) - rate) / k;
                        ba.ang += ia * n * lambda;
                        bb.ang -= ib * n * lambda;
                    }
                }
                const glm::vec3 a_mid = glm::normalize(ac + ap + glm::vec3(1e-6f, 0.0f, 0.0f));
                const glm::vec3 rel_w = ba.ang - bb.ang;
                const glm::vec3 perp_w = rel_w - a_mid * glm::dot(rel_w, a_mid);
                const float pw_len = glm::length(perp_w);
                if (pw_len > 1e-5f) {
                    const glm::vec3 n = perp_w / pw_len;
                    const float k = glm::dot(n, ia * n) + glm::dot(n, ib * n);
                    if (k > 1e-9f) {
                        const float lambda = -pw_len * 0.75f / k;
                        ba.ang += ia * n * lambda;
                        bb.ang -= ib * n * lambda;
                    }
                }
                glm::vec3 u = bb.rot * j.hinge_ref_parent;
                u -= a_mid * glm::dot(u, a_mid);
                glm::vec3 v = ba.rot * j.hinge_ref_child;
                v -= a_mid * glm::dot(v, a_mid);
                if (glm::length(u) > 1e-4f && glm::length(v) > 1e-4f) {
                    u = glm::normalize(u);
                    v = glm::normalize(v);
                    const float theta = std::atan2(glm::dot(a_mid, glm::cross(u, v)), glm::dot(u, v));
                    const float rate = glm::dot(ba.ang - bb.ang, a_mid);
                    const float k = glm::dot(a_mid, ia * a_mid) + glm::dot(a_mid, ib * a_mid);
                    if (k > 1e-9f) {
                        if (theta > j.hinge_max) {
                            const float lambda = std::min(0.0f, -(rate + (theta - j.hinge_max) * (kLimitBias / h)) / k);
                            ba.ang += ia * a_mid * lambda;
                            bb.ang -= ib * a_mid * lambda;
                        } else if (theta < j.hinge_min) {
                            const float lambda = std::max(0.0f, -(rate + (theta - j.hinge_min) * (kLimitBias / h)) / k);
                            ba.ang += ia * a_mid * lambda;
                            bb.ang -= ib * a_mid * lambda;
                        }
                    }
                }
            } else {
                const glm::vec3 da = ba.rot * j.axis_child;
                const glm::vec3 db = bb.rot * j.axis_parent;
                const float angle = std::acos(std::clamp(glm::dot(da, db), -1.0f, 1.0f));
                glm::vec3 n = glm::cross(da, db);
                const float ln = glm::length(n);
                if (ln > 1e-5f && angle > j.cone_limit) {
                    n /= ln;
                    const float rate = glm::dot(bb.ang - ba.ang, n);
                    const float k = glm::dot(n, ia * n) + glm::dot(n, ib * n);
                    if (k > 1e-9f) {
                        const float lambda = std::min(0.0f, -(rate + (angle - j.cone_limit) * (kLimitBias / h)) / k);
                        ba.ang -= ia * n * lambda;
                        bb.ang += ib * n * lambda;
                    }
                }
                const glm::quat rel = glm::inverse(bb.rot) * ba.rot;
                const glm::vec3 axis_in_p = glm::normalize(rel * j.axis_child);
                const float twist = SignedTwist(rel, axis_in_p);
                const glm::vec3 t_world = bb.rot * axis_in_p;
                const float rate = glm::dot(ba.ang - bb.ang, t_world);
                const float k = glm::dot(t_world, ia * t_world) + glm::dot(t_world, ib * t_world);
                if (k > 1e-9f) {
                    if (twist > j.twist_max) {
                        const float lambda = std::min(0.0f, -(rate + (twist - j.twist_max) * (kLimitBias / h)) / k);
                        ba.ang += ia * t_world * lambda;
                        bb.ang -= ib * t_world * lambda;
                    } else if (twist < j.twist_min) {
                        const float lambda = std::max(0.0f, -(rate + (twist - j.twist_min) * (kLimitBias / h)) / k);
                        ba.ang += ia * t_world * lambda;
                        bb.ang -= ib * t_world * lambda;
                    }
                }
            }
        }

        for (Contact& c : contacts_) {
            Body& ba = bodies_[static_cast<size_t>(c.body_a)];
            const glm::mat3 ia = InvInertiaWorld(c.body_a);
            const float ma = ba.inv_mass;
            Body* bb = c.body_b >= 0 ? &bodies_[static_cast<size_t>(c.body_b)] : nullptr;
            const glm::mat3 ib = bb ? InvInertiaWorld(c.body_b) : glm::mat3(0.0f);
            const float mb = bb ? bb->inv_mass : 0.0f;

            const glm::vec3 va = ba.vel + glm::cross(ba.ang, c.ra);
            const glm::vec3 vb = bb ? (bb->vel + glm::cross(bb->ang, c.rb)) : glm::vec3(0.0f);
            const glm::vec3 rel = va - vb;
            const float vn = glm::dot(rel, c.normal);

            const glm::vec3 ca = glm::cross(c.ra, c.normal);
            const glm::vec3 cb = bb ? glm::cross(c.rb, c.normal) : glm::vec3(0.0f);
            const float kn = ma + mb + glm::dot(ca, ia * ca) + glm::dot(cb, ib * cb);
            if (kn <= 1e-9f) continue;

            if (iter == 0 && !bb && vn < -1.2f && (c.body_a == kPelvis || c.body_a == kChest || c.body_a == kHead)) {
                impact_ = std::max(impact_, -vn);
            }
            const float bounce = vn < -1.0f ? -kRestitution * vn : 0.0f;
            const float bias = std::max(c.depth - 0.002f, 0.0f) * (kContactBias / h);
            const float dlambda = (std::max(bias, bounce) - vn) / kn;
            const float prev = c.normal_impulse;
            c.normal_impulse = std::max(0.0f, prev + dlambda);
            const float applied = c.normal_impulse - prev;
            const glm::vec3 pn = c.normal * applied;
            ba.vel += pn * ma;
            ba.ang += ia * glm::cross(c.ra, pn);
            if (bb) {
                bb->vel -= pn * mb;
                bb->ang -= ib * glm::cross(c.rb, pn);
            }

            if (!bb && c.normal_impulse > 0.0f) {
                const glm::vec3 v_now = ba.vel + glm::cross(ba.ang, c.ra);
                const glm::vec3 vt = v_now - c.normal * glm::dot(v_now, c.normal);
                const float speed_t = glm::length(vt);
                if (speed_t > 1e-5f) {
                    const glm::vec3 t_dir = vt / speed_t;
                    const glm::vec3 ct = glm::cross(c.ra, t_dir);
                    const float kt = ma + glm::dot(ct, ia * ct);
                    if (kt > 1e-9f) {
                        const glm::vec3 jt_delta = -t_dir * (speed_t / kt);
                        glm::vec3 jt_new = c.tangent_impulse + jt_delta;
                        const float max_f = kFriction * c.normal_impulse;
                        const float jt_len = glm::length(jt_new);
                        if (jt_len > max_f) jt_new *= max_f / jt_len;
                        const glm::vec3 jt_apply = jt_new - c.tangent_impulse;
                        c.tangent_impulse = jt_new;
                        ba.vel += jt_apply * ma;
                        ba.ang += ia * glm::cross(c.ra, jt_apply);
                    }
                }
            }
        }
    }
}

void Ragdoll::SolvePositions(const std::vector<float>& floor_y) {
    for (int iter = 0; iter < kPosIters; ++iter) {
        for (const Joint& j : joints_) {
            Body& ba = bodies_[static_cast<size_t>(j.child)];
            Body& bb = bodies_[static_cast<size_t>(j.parent)];
            const glm::mat3 ia = InvInertiaWorld(j.child);
            const glm::mat3 ib = InvInertiaWorld(j.parent);

            {
                const glm::vec3 ra = ba.rot * j.pivot_child;
                const glm::vec3 rb = bb.rot * j.pivot_parent;
                const glm::vec3 err = (ba.pos + ra) - (bb.pos + rb);
                const glm::mat3 sra = Skew(ra);
                const glm::mat3 srb = Skew(rb);
                const glm::mat3 k = glm::mat3(ba.inv_mass + bb.inv_mass) - sra * ia * sra - srb * ib * srb;
                const glm::vec3 imp = -(glm::inverse(k) * (err * kJointPos));
                ba.pos += imp * ba.inv_mass;
                ba.rot = IntegrateQuat(ba.rot, ia * glm::cross(ra, imp));
                bb.pos -= imp * bb.inv_mass;
                bb.rot = IntegrateQuat(bb.rot, -(ib * glm::cross(rb, imp)));
            }

            if (j.is_hinge) {
                const glm::vec3 ac = ba.rot * j.hinge_axis_child;
                const glm::vec3 ap = bb.rot * j.hinge_axis_parent;
                const glm::vec3 err = glm::cross(ac, ap);
                const float err_len = glm::length(err);
                if (err_len > 1e-4f) {
                    const glm::vec3 n = err / err_len;
                    const float angle = std::asin(std::clamp(err_len, 0.0f, 1.0f));
                    const float k = glm::dot(n, ia * n) + glm::dot(n, ib * n);
                    if (k > 1e-9f) {
                        const float lambda = angle * kLimitPos / k;
                        ba.rot = IntegrateQuat(ba.rot, ia * n * lambda);
                        bb.rot = IntegrateQuat(bb.rot, -(ib * n * lambda));
                    }
                }
                const glm::vec3 a_mid = glm::normalize(ba.rot * j.hinge_axis_child + bb.rot * j.hinge_axis_parent + glm::vec3(1e-6f, 0.0f, 0.0f));
                glm::vec3 u = bb.rot * j.hinge_ref_parent;
                u -= a_mid * glm::dot(u, a_mid);
                glm::vec3 v = ba.rot * j.hinge_ref_child;
                v -= a_mid * glm::dot(v, a_mid);
                if (glm::length(u) > 1e-4f && glm::length(v) > 1e-4f) {
                    u = glm::normalize(u);
                    v = glm::normalize(v);
                    const float theta = std::atan2(glm::dot(a_mid, glm::cross(u, v)), glm::dot(u, v));
                    const float k = glm::dot(a_mid, ia * a_mid) + glm::dot(a_mid, ib * a_mid);
                    if (k > 1e-9f) {
                        if (theta > j.hinge_max) {
                            const float lambda = (theta - j.hinge_max) * kLimitPos / k;
                            ba.rot = IntegrateQuat(ba.rot, -(ia * a_mid * lambda));
                            bb.rot = IntegrateQuat(bb.rot, ib * a_mid * lambda);
                        } else if (theta < j.hinge_min) {
                            const float lambda = (j.hinge_min - theta) * kLimitPos / k;
                            ba.rot = IntegrateQuat(ba.rot, ia * a_mid * lambda);
                            bb.rot = IntegrateQuat(bb.rot, -(ib * a_mid * lambda));
                        }
                    }
                }
            } else {
                const glm::vec3 da = ba.rot * j.axis_child;
                const glm::vec3 db = bb.rot * j.axis_parent;
                const float angle = std::acos(std::clamp(glm::dot(da, db), -1.0f, 1.0f));
                glm::vec3 n = glm::cross(da, db);
                const float ln = glm::length(n);
                if (ln > 1e-5f && angle > j.cone_limit) {
                    n /= ln;
                    const float k = glm::dot(n, ia * n) + glm::dot(n, ib * n);
                    if (k > 1e-9f) {
                        const float lambda = (angle - j.cone_limit) * kLimitPos / k;
                        ba.rot = IntegrateQuat(ba.rot, ia * n * lambda);
                        bb.rot = IntegrateQuat(bb.rot, -(ib * n * lambda));
                    }
                }
                const glm::quat rel = glm::inverse(bb.rot) * ba.rot;
                const glm::vec3 axis_in_p = glm::normalize(rel * j.axis_child);
                const float twist = SignedTwist(rel, axis_in_p);
                const glm::vec3 t_world = bb.rot * axis_in_p;
                const float k = glm::dot(t_world, ia * t_world) + glm::dot(t_world, ib * t_world);
                if (k > 1e-9f) {
                    if (twist > j.twist_max) {
                        const float lambda = (twist - j.twist_max) * kLimitPos / k;
                        ba.rot = IntegrateQuat(ba.rot, -(ia * t_world * lambda));
                        bb.rot = IntegrateQuat(bb.rot, ib * t_world * lambda);
                    } else if (twist < j.twist_min) {
                        const float lambda = (j.twist_min - twist) * kLimitPos / k;
                        ba.rot = IntegrateQuat(ba.rot, ia * t_world * lambda);
                        bb.rot = IntegrateQuat(bb.rot, -(ib * t_world * lambda));
                    }
                }
            }
        }

        for (int i = 0; i < kPartCount; ++i) {
            Body& b = bodies_[static_cast<size_t>(i)];
            const float fy = floor_y[static_cast<size_t>(i)];
            const glm::vec3 pa = b.pos + b.rot * b.cap_a;
            const glm::vec3 pb = b.pos + b.rot * b.cap_b;
            const float min_y = std::min(pa.y, pb.y);
            const float depth = (fy + b.radius) - min_y;
            if (depth > 0.0f) {
                b.pos.y += depth * kContactPos;
            }
        }
    }
}

void Ragdoll::Step(float dt, const GroundFn& ground, const SweepFn& sweep) {
    if (!active_ || dt <= 0.0f) return;
    age_ += dt;
    if (asleep_) return;

    const float clamped = std::min(dt, kMaxDt);
    const int steps = std::clamp(static_cast<int>(std::ceil(clamped / kSubStep)), 1, 10);
    const float h = clamped / static_cast<float>(steps);

    std::vector<float> floor_y(kPartCount, 0.0f);
    for (int i = 0; i < kPartCount; ++i) {
        Body& b = bodies_[static_cast<size_t>(i)];
        b.prev_pos = b.pos;
        float fy = ground ? ground(b.pos) : 0.0f;
        if (fy > b.pos.y + 0.45f) fy = ground ? ground(bodies_[kPelvis].pos) : 0.0f;
        floor_y[static_cast<size_t>(i)] = fy;
    }

    for (int s = 0; s < steps; ++s) {
        const float lin_d = std::exp(-kLinearDamp * h);
        const float ang_d = std::exp(-kAngularDamp * h);
        for (int i = 0; i < kPartCount; ++i) {
            Body& b = bodies_[static_cast<size_t>(i)];
            b.vel.y -= kGravity * h;
            b.vel *= lin_d;
            b.ang *= ang_d;
        }
        ApplyCollapseDrive(h);
        GatherContacts(floor_y);
        SolveVelocities(h);

        for (int i = 0; i < kPartCount; ++i) {
            Body& b = bodies_[static_cast<size_t>(i)];
            const float v_len = glm::length(b.vel);
            if (v_len > 18.0f) b.vel *= 18.0f / v_len;
            const float w_len = glm::length(b.ang);
            if (w_len > 22.0f) b.ang *= 22.0f / w_len;
            b.pos += b.vel * h;
            b.rot = IntegrateQuat(b.rot, b.ang * h);
        }
        SolvePositions(floor_y);
    }

    if (sweep) {
        for (int i = 0; i < kPartCount; ++i) {
            Body& b = bodies_[static_cast<size_t>(i)];
            const glm::vec3 move = b.pos - b.prev_pos;
            const float dist = glm::length(move);
            if (dist < 0.004f) continue;
            glm::vec3 hit_pt(0.0f), hit_n(0.0f);
            if (sweep(b.prev_pos, b.pos + (move / dist) * b.radius, hit_pt, hit_n)) {
                if (glm::dot(hit_n, move) > 0.0f) hit_n = -hit_n;
                b.pos = hit_pt + hit_n * (b.radius + 0.01f);
                const float vn = glm::dot(b.vel, hit_n);
                if (vn < 0.0f) {
                    b.vel -= hit_n * ((1.0f + kRestitution) * vn);
                    b.vel *= 0.65f;
                    b.ang *= 0.7f;
                }
            }
        }
    }

    float max_v = 0.0f;
    float max_w = 0.0f;
    for (int i = 0; i < kPartCount; ++i) {
        max_v = std::max(max_v, glm::length(bodies_[static_cast<size_t>(i)].vel));
        max_w = std::max(max_w, glm::length(bodies_[static_cast<size_t>(i)].ang));
    }
    if (age_ > 0.65f && max_v < 0.12f && max_w < 0.35f) {
        still_time_ += dt;
        if (still_time_ > 0.45f) {
            asleep_ = true;
            for (Body& b : bodies_) {
                b.vel = glm::vec3(0.0f);
                b.ang = glm::vec3(0.0f);
            }
        }
    } else {
        still_time_ = 0.0f;
    }
}

void Ragdoll::ApplyToBones(const Skeleton& skeleton, const glm::mat4& model_world, std::vector<glm::mat4>& bone_world) const {
    const size_t n = skeleton.Size();
    if (!active_ || bone_world.size() < n) return;
    const glm::mat4 to_model = glm::inverse(model_world);
    const glm::quat to_model_rot = RotationOf(to_model);

    const int spine_bone = skeleton.FindName("SKL_001_SPINE");
    const int chest_bone = skeleton.FindName("SKL_002_CHEST");
    const int neck_bone = skeleton.FindName("SKL_003_NECK");

    auto body_model_mat = [&](int part_idx) {
        const Body& b = bodies_[static_cast<size_t>(part_idx)];
        const glm::vec3 joint_world = b.pos + b.rot * b.joint_in_body;
        const glm::vec3 joint_model(to_model * glm::vec4(joint_world, 1.0f));
        const glm::quat rot_model = glm::normalize(to_model_rot * b.rot);
        glm::mat4 m = glm::mat4_cast(rot_model);
        m[3] = glm::vec4(joint_model, 1.0f);
        return m;
    };

    const glm::quat pelvis_r = glm::normalize(to_model_rot * bodies_[kPelvis].rot);
    const glm::quat chest_r = glm::normalize(to_model_rot * bodies_[kChest].rot);
    const glm::quat head_r = glm::normalize(to_model_rot * bodies_[kHead].rot);

    for (const int idx : bone_order_) {
        const size_t i = static_cast<size_t>(idx);
        const int part = part_of_bone_[i];
        if (part >= 0) {
            glm::mat4 m = body_model_mat(part);
            if (idx == spine_bone) {
                const glm::vec3 p(m[3]);
                m = glm::mat4_cast(glm::normalize(glm::slerp(pelvis_r, chest_r, 0.55f)));
                m[3] = glm::vec4(p, 1.0f);
            }
            bone_world[i] = m;
        } else {
            const int p = skeleton.parents[i];
            if (p >= 0 && static_cast<size_t>(p) < n) {
                bone_world[i] = bone_world[static_cast<size_t>(p)] * saved_local_[i];
                if (idx == chest_bone) {
                    const glm::vec3 pos(bone_world[i][3]);
                    bone_world[i] = glm::mat4_cast(chest_r);
                    bone_world[i][3] = glm::vec4(pos, 1.0f);
                } else if (idx == neck_bone) {
                    const glm::vec3 pos(bone_world[i][3]);
                    bone_world[i] = glm::mat4_cast(glm::normalize(glm::slerp(chest_r, head_r, 0.5f)));
                    bone_world[i][3] = glm::vec4(pos, 1.0f);
                }
            }
        }
    }
}

void Ragdoll::ApplyToPose(const Skeleton& skeleton, const glm::mat4& model_world, const std::vector<glm::vec3>& scale,
                          Pose& pose) const {
    const size_t n = skeleton.Size();
    if (!active_ || pose.rotation.size() < n) return;
    const glm::mat4 to_model = glm::inverse(model_world);
    const glm::quat to_model_rot = RotationOf(to_model);

    const glm::quat pelvis_r = glm::normalize(to_model_rot * bodies_[kPelvis].rot);
    const glm::quat chest_r = glm::normalize(to_model_rot * bodies_[kChest].rot);
    const glm::quat head_r = glm::normalize(to_model_rot * bodies_[kHead].rot);

    const int waist_bone = bodies_[kPelvis].bone;
    const int spine_bone = skeleton.FindName("SKL_001_SPINE");
    const int chest_bone = skeleton.FindName("SKL_002_CHEST");
    const int neck_bone = skeleton.FindName("SKL_003_NECK");

    std::vector<glm::quat> world_rot(n, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    std::vector<glm::vec3> world_pos(n, glm::vec3(0.0f));

    for (const int idx : bone_order_) {
        const size_t i = static_cast<size_t>(idx);
        const int p = skeleton.parents[i];
        const glm::quat parent_r = (p >= 0 && static_cast<size_t>(p) < n) ? world_rot[static_cast<size_t>(p)] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::vec3 parent_p = (p >= 0 && static_cast<size_t>(p) < n) ? world_pos[static_cast<size_t>(p)] : glm::vec3(0.0f);
        const glm::vec3 parent_s = (p >= 0 && static_cast<size_t>(p) < scale.size()) ? scale[static_cast<size_t>(p)] : glm::vec3(1.0f);

        const int part = part_of_bone_[i];
        if (idx == waist_bone) {
            const glm::vec3 waist_world = bodies_[kPelvis].pos + bodies_[kPelvis].rot * bodies_[kPelvis].joint_in_body;
            const glm::vec3 waist_model(to_model * glm::vec4(waist_world, 1.0f));
            world_rot[i] = pelvis_r;
            world_pos[i] = waist_model;
            pose.rotation[i] = glm::normalize(glm::inverse(parent_r) * pelvis_r);
            const glm::vec3 local_need = glm::inverse(parent_r) * (waist_model - parent_p);
            pose.offset[i] = local_need / glm::max(parent_s, glm::vec3(1e-4f)) - skeleton.bind_local[i];
        } else if (part >= 0) {
            const glm::quat target_r = (idx == spine_bone)
                                           ? glm::normalize(glm::slerp(pelvis_r, chest_r, 0.55f))
                                           : glm::normalize(to_model_rot * bodies_[static_cast<size_t>(part)].rot);
            world_rot[i] = target_r;
            pose.rotation[i] = glm::normalize(glm::inverse(parent_r) * target_r);
            world_pos[i] = parent_p + parent_r * ((skeleton.bind_local[i] + pose.offset[i]) * parent_s);
        } else if (idx == chest_bone) {
            world_rot[i] = chest_r;
            pose.rotation[i] = glm::normalize(glm::inverse(parent_r) * chest_r);
            world_pos[i] = parent_p + parent_r * ((skeleton.bind_local[i] + pose.offset[i]) * parent_s);
        } else if (idx == neck_bone) {
            const glm::quat nr = glm::normalize(glm::slerp(chest_r, head_r, 0.5f));
            world_rot[i] = nr;
            pose.rotation[i] = glm::normalize(glm::inverse(parent_r) * nr);
            world_pos[i] = parent_p + parent_r * ((skeleton.bind_local[i] + pose.offset[i]) * parent_s);
        } else {
            world_rot[i] = parent_r * pose.rotation[i];
            world_pos[i] = parent_p + parent_r * ((skeleton.bind_local[i] + pose.offset[i]) * parent_s);
        }
    }
}

glm::vec3 Ragdoll::PartPosition(Part part) const {
    const int idx = static_cast<int>(part);
    if (!active_ || idx < 0 || idx >= kPartCount) return glm::vec3(0.0f);
    return bodies_[static_cast<size_t>(idx)].pos;
}

float Ragdoll::TakeImpact() {
    const float out = impact_;
    impact_ = 0.0f;
    return out;
}

}
