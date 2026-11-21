#include "engine/anim/physical_body.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pt::anim {

namespace {

constexpr float kTwoPi = 6.2831853f;
constexpr float kStep = 1.0f / 240.0f;
constexpr int kIterations = 4;
constexpr float kDamping = 0.55f;
constexpr glm::vec3 kGravity(0.0f, -9.81f, 0.0f);
constexpr float kGroundFriction = 14.0f;
constexpr float kAir = 0.4f;

}

void PhysicalBody::Add(const Skeleton& skeleton, const char* name, float mass, float radius, float frequency) {
    Particle p;
    p.bone = skeleton.FindName(name);
    p.inverse_mass = 1.0f / mass;
    p.radius = radius * size_;
    p.frequency = frequency;
    joints_.push_back(p);
}

int PhysicalBody::ParticleOf(int bone) const {
    return bone >= 0 && static_cast<size_t>(bone) < particle_of_bone_.size() ? particle_of_bone_[static_cast<size_t>(bone)] : -1;
}

bool PhysicalBody::Build(const Skeleton& skeleton, float size) {
    joints_.clear();
    links_.clear();
    size_ = size;
    primed_ = asleep_ = false;
    enum : int { kWaist, kSpine, kChest, kNeck, kHead, kTop, kLUpper, kLFore, kLHand, kRUpper, kRFore, kRHand, kLThigh, kLLeg, kLFoot, kLToe,
                 kRThigh, kRLeg, kRFoot, kRToe };
    Add(skeleton, "SKL_000_WAIST", 11.0f, 0.14f, 5.0f);
    Add(skeleton, "SKL_001_SPINE", 9.0f, 0.13f, 3.2f);
    Add(skeleton, "SKL_002_CHEST", 11.0f, 0.15f, 2.6f);
    Add(skeleton, "SKL_003_NECK", 2.0f, 0.06f, 2.6f);
    Add(skeleton, "SKL_004_HEAD", 4.5f, 0.11f, 2.2f);
    joints_.push_back({});
    joints_.back().bone = -1;
    joints_.back().inverse_mass = 1.0f;
    joints_.back().radius = 0.08f * size_;
    joints_.back().frequency = 2.2f;
    for (const char* side : {"L", "R"}) {
        const bool left = side[0] == 'L';
        Add(skeleton, left ? "SKL_011_LUARM" : "SKL_021_RUARM", 2.5f, 0.06f, 3.0f);
        Add(skeleton, left ? "SKL_012_LFARM" : "SKL_022_RFARM", 1.8f, 0.05f, 2.7f);
        Add(skeleton, left ? "SKL_013_LHAND" : "SKL_023_RHAND", 0.6f, 0.05f, 2.4f);
    }
    for (const char* side : {"L", "R"}) {
        const bool left = side[0] == 'L';
        Add(skeleton, left ? "SKL_030_LTHIGH" : "SKL_040_RTHIGH", 8.0f, 0.09f, 6.5f);
        Add(skeleton, left ? "SKL_031_LLEG" : "SKL_041_RLEG", 4.0f, 0.06f, 8.0f);
        Add(skeleton, left ? "SKL_032_LFOOT" : "SKL_042_RFOOT", 1.2f, 0.05f, 10.0f);
        Add(skeleton, left ? "SKL_033_LTOE" : "SKL_043_RTOE", 0.4f, 0.035f, 10.0f);
    }
    for (size_t i = 0; i < joints_.size(); ++i) {
        if (static_cast<int>(i) != kTop && joints_[i].bone < 0) {
            joints_.clear();
            return false;
        }
    }
    particle_of_bone_.assign(skeleton.Size(), -1);
    for (size_t i = 0; i < joints_.size(); ++i) {
        if (joints_[i].bone >= 0) particle_of_bone_[static_cast<size_t>(joints_[i].bone)] = static_cast<int>(i);
    }
    head_ = kHead;
    top_ = kTop;
    waist_ = kWaist;
    auto link = [&](int a, int b, float stiffness = 1.0f) { links_.push_back({a, b, 0.0f, stiffness, false, true, false}); };
    auto reach = [&](int a, int b, float metres) { links_.push_back({a, b, metres * size_, 1.0f, false, false, true}); };
    auto limit = [&](int a, int b, float fraction) { links_.push_back({a, b, fraction, 1.0f, true, true, false}); };
    link(kSpine, kWaist);
    link(kChest, kSpine);
    link(kNeck, kChest);
    link(kHead, kNeck);
    link(kTop, kHead);
    link(kTop, kNeck);
    for (int s = 0; s < 2; ++s) {
        const int upper = s ? kRUpper : kLUpper, thigh = s ? kRThigh : kLThigh;
        link(upper, kChest);
        link(upper + 1, upper);
        link(upper + 2, upper + 1);
        link(thigh, kWaist);
        link(thigh + 1, thigh);
        link(thigh + 2, thigh + 1);
        link(thigh + 3, thigh + 2);
        limit(thigh, thigh + 2, 0.55f);
        limit(upper, upper + 2, 0.35f);
        limit(kWaist, thigh + 2, 0.5f);
    }
    for (const auto& group : {std::vector<int>{kWaist, kSpine, kLThigh, kRThigh}, std::vector<int>{kChest, kNeck, kLUpper, kRUpper}}) {
        for (size_t i = 0; i < group.size(); ++i) {
            for (size_t j = i + 1; j < group.size(); ++j) {
                const bool bone = (group[i] == kWaist && group[j] == kSpine) || (group[i] == kChest && group[j] == kNeck);
                if (!bone) link(group[i], group[j]);
            }
        }
    }
    link(kWaist, kChest, 0.35f);
    link(kLThigh, kLUpper, 0.2f);
    link(kRThigh, kRUpper, 0.2f);
    link(kLThigh, kRUpper, 0.15f);
    link(kRThigh, kLUpper, 0.15f);
    link(kHead, kChest, 0.3f);
    limit(kWaist, kHead, 0.7f);
    reach(kLFoot, kRFoot, 0.85f);
    reach(kLLeg, kRLeg, 0.6f);
    return true;
}

void PhysicalBody::Follow(const std::vector<glm::vec3>& bone_world, const glm::quat& head_world) {
    if (!Valid()) return;
    for (Particle& p : joints_) {
        glm::vec3 t(0.0f);
        if (p.bone >= 0 && static_cast<size_t>(p.bone) < bone_world.size()) {
            t = bone_world[static_cast<size_t>(p.bone)];
        } else if (joints_[static_cast<size_t>(head_)].bone >= 0) {
            t = bone_world[static_cast<size_t>(joints_[static_cast<size_t>(head_)].bone)] + head_world * glm::vec3(0.0f, 0.18f * size_, 0.0f);
        }
        if (!primed_) {
            p.x = p.target = p.last_target = t;
            p.v = glm::vec3(0.0f);
        } else {
            p.last_target = p.target;
            p.target = t;
        }
    }
    if (!primed_ || drive_ > 0.5f) {
        for (Link& l : links_) {
            const float d = glm::distance(joints_[static_cast<size_t>(l.a)].target, joints_[static_cast<size_t>(l.b)].target);
            if (l.maximum) continue;
            if (l.minimum) {
                if (!l.follow) continue;
                l.rest = l.rest * d;
                l.follow = false;
            } else {
                l.rest = d;
            }
        }
    }
    primed_ = true;
}

void PhysicalBody::Strike(const glm::vec3& at, const glm::vec3& velocity, float radius) {
    if (!Valid() || !primed_) return;
    asleep_ = false;
    still_ = 0.0f;
    for (Particle& p : joints_) {
        const float d = glm::distance(p.x, at);
        if (d >= radius) continue;
        const float w = (1.0f - d / radius) * (1.0f - d / radius);
        p.v += velocity * w;
    }
}

void PhysicalBody::SolveLinks(float) {
    for (int it = 0; it < kIterations; ++it) {
        for (const Link& l : links_) {
            Particle& a = joints_[static_cast<size_t>(l.a)];
            Particle& b = joints_[static_cast<size_t>(l.b)];
            const glm::vec3 d = b.x - a.x;
            const float len = glm::length(d);
            if (len < 1e-6f) continue;
            if ((l.minimum && len >= l.rest) || (l.maximum && len <= l.rest)) continue;
            const float w = a.inverse_mass + b.inverse_mass;
            if (w <= 0.0f) continue;
            const glm::vec3 corr = d * ((len - l.rest) / len * l.stiffness / w);
            a.x += corr * a.inverse_mass;
            b.x -= corr * b.inverse_mass;
        }
    }
}

void PhysicalBody::Step(float dt, const GroundFn& ground, const SweepFn& sweep) {
    if (!Valid() || !primed_ || dt <= 0.0f) return;
    if (asleep_) {
        deviation_ = 0.0f;
        for (const Particle& p : joints_) deviation_ = std::max(deviation_, glm::distance(p.x, p.target));
        return;
    }
    const int steps = std::clamp(static_cast<int>(std::ceil(dt / kStep)), 1, 12);
    const float h = dt / static_cast<float>(steps);
    const float drive = std::clamp(drive_, 0.0f, 1.0f);
    std::vector<float> floor(joints_.size());
    std::vector<glm::vec3> start(joints_.size());
    for (size_t i = 0; i < joints_.size(); ++i) {
        floor[i] = ground ? ground(joints_[i].x) : -1e9f;
        start[i] = joints_[i].x;
    }
    std::vector<glm::vec3> before(joints_.size());
    for (int s = 0; s < steps; ++s) {
        const float f0 = static_cast<float>(s) / static_cast<float>(steps), f1 = static_cast<float>(s + 1) / static_cast<float>(steps);
        for (size_t i = 0; i < joints_.size(); ++i) {
            Particle& p = joints_[i];
            const glm::vec3 t0 = glm::mix(p.last_target, p.target, f0), t1 = glm::mix(p.last_target, p.target, f1);
            p.x += (t1 - t0) * drive;
            before[i] = p.x;
            const float w = kTwoPi * p.frequency;
            const glm::vec3 a = (-(w * w) * (p.x - t1) - 2.0f * kDamping * w * p.v) * drive + kGravity * (1.0f - drive);
            p.v += a * h;
            p.v *= std::exp(-kAir * h);
            p.x += p.v * h;
        }
        SolveLinks(h);
        for (size_t i = 0; i < joints_.size(); ++i) {
            Particle& p = joints_[i];
            const float lowest = drive >= 0.5f ? std::min(floor[i] + p.radius, p.target.y) : floor[i] + p.radius;
            bool grounded = false;
            if (p.x.y < lowest) {
                const float down = (before[i].y - p.x.y) / h;
                if (static_cast<int>(i) == waist_ || i == 1 || i == 2) impact_ = std::max(impact_, down);
                p.x.y = lowest;
                grounded = true;
            }
            p.v = (p.x - before[i]) / h;
            if (grounded) {
                p.v.y = std::clamp(p.v.y, 0.0f, 1.5f);
                const float k = std::exp(-kGroundFriction * h);
                p.v.x *= k;
                p.v.z *= k;
            }
        }
    }
    if (sweep && drive < 0.5f) {
        for (size_t i = 0; i < joints_.size(); ++i) {
            Particle& p = joints_[i];
            const glm::vec3 move = p.x - start[i];
            const float l = glm::length(move);
            if (l < 0.005f) continue;
            glm::vec3 point(0.0f), normal(0.0f);
            if (sweep(start[i], p.x + move / l * p.radius, point, normal)) {
                if (glm::dot(normal, move) > 0.0f) normal = -normal;
                if (normal.y > 0.7f) continue;
                p.x = point + normal * p.radius;
                p.v -= normal * std::min(0.0f, glm::dot(p.v, normal));
                p.v *= 0.5f;
            }
        }
        SolveLinks(dt);
    }
    deviation_ = 0.0f;
    float fastest = 0.0f;
    for (const Particle& p : joints_) {
        deviation_ = std::max(deviation_, glm::distance(p.x, p.target));
        fastest = std::max(fastest, glm::length(p.v));
    }
    if (drive < 0.1f) {
        still_ = fastest < 0.08f ? still_ + dt : 0.0f;
        if (still_ > 0.6f) {
            asleep_ = true;
            for (Particle& p : joints_) p.v = glm::vec3(0.0f);
        }
    }
}

bool PhysicalBody::Joint(int bone, glm::vec3& out) const {
    const int p = ParticleOf(bone);
    if (p < 0) return false;
    out = joints_[static_cast<size_t>(p)].x;
    return true;
}

glm::vec3 PhysicalBody::HeadTop() const {
    return top_ >= 0 ? joints_[static_cast<size_t>(top_)].x : glm::vec3(0.0f);
}

glm::vec3 PhysicalBody::Waist() const {
    return waist_ >= 0 ? joints_[static_cast<size_t>(waist_)].x : glm::vec3(0.0f);
}

float PhysicalBody::TakeImpact() {
    return std::exchange(impact_, 0.0f);
}

}
