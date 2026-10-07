#include "engine/anim/rig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "engine/anim/anim_codec.h"

namespace pt::anim {
namespace {

template <typename T>
T At(std::span<const uint8_t> data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

std::string CString(std::span<const uint8_t> data, size_t offset, size_t limit) {
    std::string out;
    while (offset < data.size() && data[offset] != 0 && out.size() < limit) {
        out.push_back(static_cast<char>(data[offset++]));
    }
    return out;
}

const glm::quat kIdentity(1.0f, 0.0f, 0.0f, 0.0f);

glm::vec4* Slot(RigPose& pose, int track) {
    return track >= 0 && static_cast<size_t>(track) < pose.tracks.size() ? &pose.tracks[static_cast<size_t>(track)] : nullptr;
}

glm::quat TrackRotation(const RigPose& pose, int track) {
    if (track < 0 || static_cast<size_t>(track) >= pose.tracks.size()) {
        return kIdentity;
    }
    return glm::normalize(ToQuat(pose.tracks[static_cast<size_t>(track)]));
}

glm::vec3 TrackVector(const RigPose& pose, int track) {
    return track >= 0 && static_cast<size_t>(track) < pose.tracks.size() ? glm::vec3(pose.tracks[static_cast<size_t>(track)]) : glm::vec3(0.0f);
}

glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float length2 = glm::dot(v, v);
    return length2 > 1e-20f ? v / std::sqrt(length2) : fallback;
}

glm::vec3 AnyPerpendicular(const glm::vec3& v) {
    const glm::vec3 other = std::abs(v.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return SafeNormalize(glm::cross(v, other), glm::vec3(0.0f, 0.0f, 1.0f));
}

glm::quat FrameRotation(const glm::vec3& world_a, const glm::vec3& world_b, const glm::vec3& local_a, const glm::vec3& local_b) {
    const glm::mat3 m = glm::outerProduct(world_a, local_a) + glm::outerProduct(world_b, local_b) +
                        glm::outerProduct(glm::cross(world_b, world_a), glm::cross(local_b, local_a));
    return glm::normalize(glm::quat_cast(m));
}

void SolveTwoBone(const glm::vec3& start, const glm::vec3& target, const glm::vec3& pole, const glm::vec3& axis, const glm::vec3& upper_local,
                  const glm::vec3& lower_local, glm::quat& upper, glm::quat& lower) {
    const glm::vec3 d = target - start;
    const float dist = glm::length(d);
    const float l1 = glm::length(upper_local);
    const float l2 = glm::length(lower_local);
    if (l1 <= 0.0f || l2 <= 0.0f) {
        upper = lower = kIdentity;
        return;
    }
    const glm::vec3 dn = dist > 1e-8f ? d / dist : SafeNormalize(upper_local, glm::vec3(0.0f, -1.0f, 0.0f));
    glm::vec3 e = glm::cross(d, glm::cross(pole, d));
    e = glm::dot(e, e) > 1e-20f ? glm::normalize(e) : AnyPerpendicular(dn);
    float x = l1;
    if (dist < l1 + l2) {
        x = std::max((dist * dist + l1 * l1 - l2 * l2) / (2.0f * std::max(dist, 1e-8f)), 0.0f);
    }
    const float h = std::sqrt(std::max(l1 * l1 - x * x, 0.0f));
    const glm::vec3 knee = e * h + dn * x;
    const glm::vec3 hinge = glm::cross(e, dn);
    upper = FrameRotation(hinge, SafeNormalize(knee, dn), axis, upper_local / l1);
    lower = FrameRotation(hinge, SafeNormalize(d - knee, dn), axis, lower_local / l2);
}

class Solver {
public:
    Solver(const Skeleton& skeleton, RigOutput& out) : skeleton_(skeleton), out_(out), done_(skeleton.Size(), 0) {
        out_.rotation.assign(skeleton.Size(), kIdentity);
        out_.position.assign(skeleton.Size(), glm::vec3(0.0f));
    }

    bool Valid(int bone) const { return bone >= 0 && static_cast<size_t>(bone) < skeleton_.Size(); }
    const glm::vec3& Local(int bone) const { return skeleton_.bind_local[static_cast<size_t>(bone)]; }
    const glm::quat& Rotation(int bone) const { return out_.rotation[static_cast<size_t>(bone)]; }
    const glm::vec3& Position(int bone) const { return out_.position[static_cast<size_t>(bone)]; }

    void Parent(int bone, glm::quat& rotation, glm::vec3& position) const {
        const int parent = skeleton_.parents[static_cast<size_t>(bone)];
        rotation = Valid(parent) ? Rotation(parent) : kIdentity;
        position = Valid(parent) ? Position(parent) : glm::vec3(0.0f);
    }

    void Set(int bone, const glm::quat& rotation, const glm::vec3& position) {
        out_.rotation[static_cast<size_t>(bone)] = rotation;
        out_.position[static_cast<size_t>(bone)] = position;
        done_[static_cast<size_t>(bone)] = 1;
    }

    void Place(int bone, const glm::quat& rotation, bool local) {
        glm::quat pr;
        glm::vec3 pp;
        Parent(bone, pr, pp);
        Set(bone, local ? pr * rotation : rotation, pp + pr * Local(bone));
    }

    void Chain(int first, int second, int end, const glm::vec3& start, const glm::vec3& target, const glm::vec3& pole, const glm::vec3& axis) {
        glm::quat r1;
        glm::quat r2;
        SolveTwoBone(start, target, pole, axis, Local(second), Local(end), r1, r2);
        Set(first, r1, start);
        Set(second, r2, start + r1 * Local(second));
    }

    void FillRest() {
        for (size_t i = 0; i < done_.size(); ++i) {
            Fill(static_cast<int>(i), 0);
        }
    }

private:
    void Fill(int bone, int depth) {
        if (done_[static_cast<size_t>(bone)] || depth > 256) {
            return;
        }
        const int parent = skeleton_.parents[static_cast<size_t>(bone)];
        if (Valid(parent)) {
            Fill(parent, depth + 1);
        }
        Place(bone, kIdentity, true);
    }

    const Skeleton& skeleton_;
    RigOutput& out_;
    std::vector<uint8_t> done_;
};

}

bool Rig::Parse(std::span<const uint8_t> data, std::string* error) {
    *this = Rig{};
    if (data.size() < 0x20 || At<uint32_t>(data, 0) != kMagic) {
        if (error) {
            *error = "not a rig file";
        }
        return false;
    }
    const uint32_t name_offset = At<uint32_t>(data, 0x04);
    const uint32_t unit_count = At<uint32_t>(data, 0x0C);
    track_count_ = At<uint32_t>(data, 0x10);
    const uint32_t joints_offset = At<uint32_t>(data, 0x18);
    const uint32_t masks_offset = At<uint32_t>(data, 0x1C);
    if (unit_count > 1024 || 0x20 + 4ull * unit_count > data.size()) {
        if (error) {
            *error = std::format("bad unit count {}", unit_count);
        }
        return false;
    }
    name_ = CString(data, name_offset, 64);
    evaluable_ = true;
    for (uint32_t i = 0; i < unit_count; ++i) {
        const size_t o = At<uint32_t>(data, 0x20 + 4 * static_cast<size_t>(i));
        RigUnit u;
        u.type = At<uint32_t>(data, o);
        const uint16_t tracks = At<uint16_t>(data, o + 4);
        const uint16_t joints = At<uint16_t>(data, o + 6);
        u.parent_joint = At<int16_t>(data, o + 8);
        u.parent_unit = At<int16_t>(data, o + 0x0A);
        auto word = [&](size_t k) { return static_cast<int>(At<int16_t>(data, o + k)); };
        auto axis = [&] { return glm::vec3(At<float>(data, o + 0x20), At<float>(data, o + 0x24), At<float>(data, o + 0x28)); };
        switch (static_cast<RigUnitType>(u.type)) {
        case RigUnitType::Root:
            u.tracks = {word(0x10), word(0x12)};
            break;
        case RigUnitType::Rotation:
        case RigUnitType::LocalRotation:
            u.joints = {word(0x10)};
            u.tracks = {word(0x12)};
            break;
        case RigUnitType::Waist:
            u.joints = {word(0x10)};
            u.tracks = {word(0x12), word(0x14)};
            break;
        case RigUnitType::Chain:
            for (uint16_t k = 0; k < joints; ++k) {
                u.joints.push_back(word(0x10) + k);
            }
            for (uint16_t k = 0; k < tracks; ++k) {
                u.tracks.push_back(word(0x12) + k);
            }
            break;
        case RigUnitType::Leg:
            u.axis = axis();
            u.joints = {word(0x30), word(0x32)};
            u.tracks = {word(0x34), word(0x36)};
            u.end_joint = word(0x38);
            break;
        case RigUnitType::Arm:
            u.axis = axis();
            u.joints = {word(0x30), word(0x32), word(0x34)};
            u.tracks = {word(0x36), word(0x38), word(0x3A)};
            u.end_joint = word(0x3C);
            break;
        default:
            evaluable_ = false;
            break;
        }
        units_.push_back(std::move(u));
    }
    if (joints_offset && joints_offset + 4 <= data.size()) {
        const uint32_t count = At<uint32_t>(data, joints_offset);
        for (uint32_t i = 0; i < count && i < 4096; ++i) {
            const size_t e = joints_offset + 4 + 8 * static_cast<size_t>(i);
            joints_.push_back({At<uint32_t>(data, e), At<uint32_t>(data, e + 4)});
        }
    }
    if (masks_offset && masks_offset + 8 <= data.size()) {
        const uint32_t weights = At<uint32_t>(data, masks_offset);
        const uint32_t count = At<uint32_t>(data, masks_offset + 4);
        for (uint32_t i = 0; i < count && i < 256 && weights <= 1024; ++i) {
            const size_t m = masks_offset + At<uint32_t>(data, masks_offset + 8 + 4 * static_cast<size_t>(i));
            RigMask mask;
            mask.hash = At<uint32_t>(data, m);
            mask.name = CString(data, m + 4, 12);
            for (uint32_t k = 0; k < weights; ++k) {
                mask.weights.push_back(At<float>(data, m + 0x10 + 4 * static_cast<size_t>(k)));
            }
            masks_.push_back(std::move(mask));
        }
    }
    for (const RigUnit& u : units_) {
        for (int t : u.tracks) {
            evaluable_ = evaluable_ && t >= 0 && static_cast<uint32_t>(t) < track_count_;
        }
    }
    return true;
}

bool Rig::ChannelIsRotation(size_t unit, size_t channel) const {
    if (unit >= units_.size()) {
        return false;
    }
    switch (units_[unit].type) {
    case 1:
    case 5:
    case 7:
    case 9:
        return channel == 0;
    case 3:
    case 6:
        return channel != 0;
    case 8:
    case 10:
        return channel != 1;
    default:
        return true;
    }
}

bool RigBinding::Bind(const Rig& rig, const Skeleton& skeleton) {
    joint_bone.clear();
    bool all = true;
    for (const RigJoint& j : rig.Joints()) {
        joint_bone.push_back(skeleton.Find(j.hash));
        all = all && joint_bone.back() >= 0;
    }
    return all;
}

int RigBinding::Bone(int joint) const {
    return joint >= 0 && static_cast<size_t>(joint) < joint_bone.size() ? joint_bone[static_cast<size_t>(joint)] : -1;
}

bool SampleRigPose(const Rig& rig, const GaniMotion& motion, double frame, RigPose& out, bool loop) {
    out.relative = false;
    out.tracks.assign(rig.TrackCount(), glm::vec4(0.0f));
    for (size_t u = 0; u < rig.Units().size(); ++u) {
        const RigUnit& unit = rig.Units()[u];
        for (size_t c = 0; c < unit.tracks.size(); ++c) {
            if (glm::vec4* slot = rig.ChannelIsRotation(u, c) ? Slot(out, unit.tracks[c]) : nullptr) {
                *slot = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
    }
    if (!motion.RigDriven()) {
        return false;
    }
    const double f = motion.WrapFrame(frame, loop);
    for (const GaniUnit& unit : motion.units) {
        for (const GaniTrack& track : unit.tracks) {
            if (track.index < out.tracks.size()) {
                out.tracks[track.index] = SampleKeys(track.keys, track.kind, f);
            }
        }
    }
    const glm::quat to_root = glm::conjugate(RigRootRotation(rig, out));
    const glm::vec3 root = RigRootTranslation(rig, out);
    for (const RigUnit& unit : rig.Units()) {
        const int track = unit.Is(RigUnitType::Leg) ? unit.tracks[0] : unit.Is(RigUnitType::Arm) ? unit.tracks[1] : -1;
        if (glm::vec4* v = Slot(out, track)) {
            *v = glm::vec4(to_root * (glm::vec3(*v) - root), 0.0f);
        }
    }
    return true;
}

glm::vec3 RigRootTranslation(const Rig& rig, const RigPose& pose) {
    for (const RigUnit& unit : rig.Units()) {
        if (unit.Is(RigUnitType::Root) && unit.tracks.size() >= 2) {
            return TrackVector(pose, unit.tracks[1]);
        }
    }
    return glm::vec3(0.0f);
}

glm::quat RigRootRotation(const Rig& rig, const RigPose& pose) {
    for (const RigUnit& unit : rig.Units()) {
        if (unit.Is(RigUnitType::Root) && !unit.tracks.empty()) {
            return TrackRotation(pose, unit.tracks[0]);
        }
    }
    return kIdentity;
}

void EvaluateRig(const Rig& rig, const RigBinding& binding, const Skeleton& skeleton, const RigPose& pose, RigOutput& out) {
    Solver s(skeleton, out);
    const glm::vec3 x_axis(1.0f, 0.0f, 0.0f);
    for (const RigUnit& u : rig.Units()) {
        switch (static_cast<RigUnitType>(u.type)) {
        case RigUnitType::Waist:
            if (const int bone = binding.Bone(u.joints[0]); s.Valid(bone)) {
                s.Set(bone, TrackRotation(pose, u.tracks[0]), TrackVector(pose, u.tracks[1]));
            }
            break;
        case RigUnitType::Rotation:
            if (const int bone = binding.Bone(u.joints[0]); s.Valid(bone)) {
                s.Place(bone, TrackRotation(pose, u.tracks[0]), false);
            }
            break;
        case RigUnitType::LocalRotation:
        case RigUnitType::Chain:
            for (size_t k = 0; k < u.joints.size() && k < u.tracks.size(); ++k) {
                if (const int bone = binding.Bone(u.joints[k]); s.Valid(bone)) {
                    s.Place(bone, TrackRotation(pose, u.tracks[k]), true);
                }
            }
            break;
        case RigUnitType::Leg: {
            const int thigh = binding.Bone(u.joints[0]);
            const int knee = binding.Bone(u.joints[1]);
            const int foot = binding.Bone(u.end_joint);
            if (!s.Valid(thigh) || !s.Valid(knee) || !s.Valid(foot)) {
                break;
            }
            glm::quat pr;
            glm::vec3 pp;
            s.Parent(thigh, pr, pp);
            glm::vec3 target = TrackVector(pose, u.tracks[0]);
            glm::quat swivel = TrackRotation(pose, u.tracks[1]);
            if (pose.relative) {
                target += pp;
                swivel = pr * swivel;
            }
            s.Chain(thigh, knee, foot, pp + pr * s.Local(thigh), target, swivel * x_axis, u.axis);
            break;
        }
        case RigUnitType::Arm: {
            const int clavicle = binding.Bone(u.joints[0]);
            const int upper = binding.Bone(u.joints[1]);
            const int lower = binding.Bone(u.joints[2]);
            const int hand = binding.Bone(u.end_joint);
            if (!s.Valid(clavicle) || !s.Valid(upper) || !s.Valid(lower) || !s.Valid(hand)) {
                break;
            }
            glm::quat pr;
            glm::vec3 pp;
            s.Parent(clavicle, pr, pp);
            s.Place(clavicle, TrackRotation(pose, u.tracks[0]), false);
            const glm::vec3 shoulder = s.Position(clavicle) + s.Rotation(clavicle) * s.Local(upper);
            const glm::vec3 target = TrackVector(pose, u.tracks[1]) + (pose.relative ? pp : glm::vec3(0.0f));
            s.Chain(upper, lower, hand, shoulder, target, TrackRotation(pose, u.tracks[2]) * x_axis, u.axis);
            break;
        }
        default:
            break;
        }
    }
    s.FillRest();
}

void MakeRigPoseRelative(const Rig& rig, const RigBinding& binding, const Skeleton& skeleton, const RigOutput& evaluated, RigPose& pose) {
    if (pose.relative) {
        return;
    }
    pose.relative = true;
    const int n = static_cast<int>(std::min(skeleton.Size(), evaluated.position.size()));
    for (const RigUnit& u : rig.Units()) {
        const bool leg = u.Is(RigUnitType::Leg);
        if (!leg && !u.Is(RigUnitType::Arm)) {
            continue;
        }
        const int first = binding.Bone(u.joints[0]);
        if (first < 0 || first >= n) {
            continue;
        }
        const int parent = skeleton.parents[static_cast<size_t>(first)];
        const bool has_parent = parent >= 0 && parent < n;
        const glm::quat pr = has_parent ? evaluated.rotation[static_cast<size_t>(parent)] : kIdentity;
        const glm::vec3 pp = has_parent ? evaluated.position[static_cast<size_t>(parent)] : glm::vec3(0.0f);
        if (glm::vec4* v = Slot(pose, leg ? u.tracks[0] : u.tracks[1])) {
            *v = glm::vec4(glm::vec3(*v) - pp, 0.0f);
        }
        if (glm::vec4* q = leg ? Slot(pose, u.tracks[1]) : nullptr) {
            *q = FromQuat(glm::normalize(glm::conjugate(pr) * ToQuat(*q)));
        }
    }
}

void BlendRigPose(const Rig& rig, const RigPose& from, const RigPose& to, float weight, RigPose& out) {
    if (from.tracks.size() != to.tracks.size() || from.relative != to.relative) {
        out = to;
        return;
    }
    RigPose result = to;
    const float w = std::clamp(weight, 0.0f, 1.0f);
    for (size_t u = 0; u < rig.Units().size(); ++u) {
        const RigUnit& unit = rig.Units()[u];
        for (size_t c = 0; c < unit.tracks.size(); ++c) {
            if (glm::vec4* slot = Slot(result, unit.tracks[c])) {
                const glm::vec4& a = from.tracks[static_cast<size_t>(unit.tracks[c])];
                const glm::vec4& b = to.tracks[static_cast<size_t>(unit.tracks[c])];
                *slot = rig.ChannelIsRotation(u, c) ? SlerpKey(a, b, w) : a + (b - a) * w;
            }
        }
    }
    out = std::move(result);
}

void RigOutputToPose(const Skeleton& skeleton, const RigOutput& output, Pose& out) {
    const size_t n = std::min(skeleton.Size(), output.rotation.size());
    out.Reset(n);
    for (size_t i = 0; i < n; ++i) {
        const int parent = skeleton.parents[i];
        const bool has_parent = parent >= 0 && static_cast<size_t>(parent) < n;
        const glm::quat inverse = glm::conjugate(has_parent ? output.rotation[static_cast<size_t>(parent)] : kIdentity);
        const glm::vec3 pp = has_parent ? output.position[static_cast<size_t>(parent)] : glm::vec3(0.0f);
        out.rotation[i] = glm::normalize(inverse * output.rotation[i]);
        out.offset[i] = inverse * (output.position[i] - pp) - skeleton.bind_local[i];
    }
}

}
