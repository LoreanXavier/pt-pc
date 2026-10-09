#include "game/player_animation.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/anim/anim_codec.h"
#include "engine/anim/gani.h"
#include "engine/anim/help_bones.h"
#include "engine/anim/motion_data.h"
#include "engine/anim/skeleton.h"
#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/fs/vfs.h"

namespace pt::game {
namespace {

constexpr float kPi = 3.14159265f;
constexpr const char* kMotionPackage = "/Assets/sh/level_asset/chara/player/game_object/player2_common_motion.fpk";
constexpr const char* kMotionArchive = "/Assets/sh/motion/mtar/player/ShPlayer_layers.mtar";
constexpr const char* kRigFile = "/Assets/sh/rig/frig/human_finger.frig";
constexpr const char* kSkeletonFile = "/Assets/sh/chara/plr/Scenes/plr0_main0_def.fmdl";
constexpr const char* kClipNames[kClipCount] = {"front", "right", "back", "left", "stand"};
constexpr const char* kStartNames[] = {"start front", "start right", "start back", "start left"};
constexpr const char* kStopNames[] = {"stop front", "stop right", "stop back", "stop left"};
// The walk and stand states (0x9874F0 case 6, 0x986420) play motion 0xf784cef74395 on layer (0, 1) while the handy light is
// on and 0xc1a2050d3802 while it is off. The archive's only one-frame animation is this pose: the left hand raised to the
// chin 0.25 m in front of the eye, where the Spot_Mask constants put the light (rendering.md 12.5 and 12.20)
constexpr uint64_t kLightArmMotion = 0xFC517D57B3A9F1EEull;

PlayerLocomotion g_locomotion;
anim::MotionArchive g_archive;
anim::Rig g_rig;
anim::Skeleton g_skeleton;
anim::RigBinding g_binding;
anim::RigPose g_light_arm;
std::vector<float> g_light_arm_weights;
int g_head_bone = -1;
bool g_rig_ready = false;
bool g_last_left = false;

int DirectionOf(const glm::vec3& delta) {
    if (std::abs(delta.z) >= std::abs(delta.x)) {
        return delta.z > 0.0f ? 0 : 2;
    }
    return delta.x < 0.0f ? 1 : 3;
}

std::vector<WalkFootstep> Footsteps(const anim::GaniMotion& motion) {
    const uint64_t left = StrCode64("FOOT_GROUND_L") & kStrCode64Mask;
    const uint64_t right = StrCode64("FOOT_GROUND_R") & kStrCode64Mask;
    std::vector<WalkFootstep> steps;
    for (const anim::GaniEvent& e : motion.events) {
        const uint64_t kind = e.event.StringArg(1);
        if (kind != left && kind != right) {
            continue;
        }
        for (const anim::EventSection& s : e.event.sections) {
            if (s.start >= 0 && static_cast<uint32_t>(s.start) <= motion.frames) {
                steps.push_back({static_cast<float>(s.start), kind == left});
            }
        }
    }
    std::sort(steps.begin(), steps.end(), [](const WalkFootstep& a, const WalkFootstep& b) { return a.frame < b.frame; });
    return steps;
}

std::vector<WalkSound> SoundEvents(const anim::GaniMotion& motion) {
    const uint64_t kinds[] = {StrCode64("FOOT_LEAVE_L") & kStrCode64Mask, StrCode64("FOOT_LEAVE_R") & kStrCode64Mask,
                              StrCode64("FOOT_CREAK") & kStrCode64Mask};
    std::vector<WalkSound> sounds;
    for (const anim::GaniEvent& e : motion.events) {
        const uint64_t kind = e.event.StringArg(1);
        if (std::find(std::begin(kinds), std::end(kinds), kind) == std::end(kinds)) {
            continue;
        }
        for (const anim::EventSection& s : e.event.sections) {
            if (s.start >= 0 && static_cast<uint32_t>(s.start) <= motion.frames) {
                sounds.push_back({static_cast<float>(s.start), kind});
            }
        }
    }
    std::sort(sounds.begin(), sounds.end(), [](const WalkSound& a, const WalkSound& b) { return a.frame < b.frame; });
    return sounds;
}

// RIG_ROOT xz relative to frame 0 and its horizontal path length per frame
void RootPath(const anim::GaniMotion& motion, BodyClip& clip) {
    const anim::GaniUnit* unit = motion.FindUnit(anim::kHashRigRoot);
    if (!unit || unit->translation < 0) {
        return;
    }
    glm::vec2 first(0.0f);
    glm::vec2 previous(0.0f);
    float length = 0.0f;
    for (uint32_t f = 0; f <= motion.frames; ++f) {
        const glm::vec3 p(motion.Sample(*unit, unit->translation, static_cast<double>(f)));
        const glm::vec2 xz(p.x, p.z);
        if (f == 0) {
            first = xz;
        } else {
            length += glm::length(xz - previous);
        }
        clip.path.push_back(length);
        clip.root.push_back(xz - first);
        previous = xz;
    }
}

BodyClip MakeClip(const anim::GaniMotion& motion) {
    BodyClip clip;
    clip.path_code = motion.path_code;
    clip.frames = static_cast<float>(motion.frames);
    RootPath(motion, clip);
    clip.steps = Footsteps(motion);
    clip.sounds = SoundEvents(motion);
    clip.motion = &motion;
    return clip;
}

// 0xACCF70: starts in (from, to], frame 0 when advancing from frame 0
template <typename Event, typename Fn>
void ForEachEvent(const std::vector<Event>& events, float from, float to, float length, bool loop, Fn&& fn) {
    for (const Event& e : events) {
        for (float at = e.frame; at <= to; at += length) {
            if (at > from || (at == 0.0f && from == 0.0f && to > 0.0f)) {
                fn(e);
            }
            if (!loop || length <= 0.0f) {
                break;
            }
        }
    }
}

bool LoadRig(Vfs& vfs) {
    auto rig_bytes = vfs.ReadFile(kRigFile);
    auto fmdl_bytes = vfs.ReadFile(kSkeletonFile);
    std::string error;
    if (!rig_bytes || !g_rig.Parse(*rig_bytes, &error) || !g_rig.Evaluable()) {
        LogWarn("player anim: rig {} unusable {}", kRigFile, error);
        return false;
    }
    if (!fmdl_bytes || !anim::ReadFmdlSkeleton(*fmdl_bytes, g_skeleton) || g_skeleton.Empty()) {
        LogWarn("player anim: {} has no skeleton", kSkeletonFile);
        return false;
    }
    g_binding = anim::RigBinding{};
    g_binding.Bind(g_rig, g_skeleton);
    g_head_bone = g_skeleton.FindName("SKL_004_HEAD");
    return g_head_bone >= 0;
}

}

float BodyClip::PathAt(float frame) const {
    if (path.size() < 2) {
        return 0.0f;
    }
    const float loops = std::floor(frame / frames);
    const float f = std::clamp(frame - loops * frames, 0.0f, frames);
    const size_t i = std::min(static_cast<size_t>(f), path.size() - 2);
    const float t = f - static_cast<float>(i);
    return loops * path.back() + path[i] + (path[i + 1] - path[i]) * t;
}

glm::vec2 BodyClip::RootAt(float frame) const {
    if (root.size() < 2) {
        return glm::vec2(0.0f);
    }
    const float loops = std::floor(frame / frames);
    const float f = std::clamp(frame - loops * frames, 0.0f, frames);
    const size_t i = std::min(static_cast<size_t>(f), root.size() - 2);
    const float t = f - static_cast<float>(i);
    return loops * root.back() + root[i] + (root[i + 1] - root[i]) * t;
}

const PlayerLocomotion& PlayerLocomotionData() {
    return g_locomotion;
}

bool LoadPlayerAnimation(Vfs& vfs) {
    g_locomotion = PlayerLocomotion{};
    g_rig_ready = LoadRig(vfs);
    vfs.LoadPackage(kMotionPackage);
    auto bytes = vfs.ReadFile(kMotionArchive);
    std::string error;
    g_archive = anim::MotionArchive{};
    if (!bytes || !g_archive.Parse(std::move(*bytes), &error)) {
        LogWarn("player anim: {} unreadable {}", kMotionArchive, error);
        return false;
    }
    g_light_arm = anim::RigPose{};
    g_light_arm_weights.clear();
    for (const anim::GaniMotion& motion : g_archive.Motions()) {
        if (motion.path_code == kLightArmMotion && g_rig_ready) {
            // the layer's mask is in the motion graph (ShPlayer_layers.mog, not read); the rig's LArm mask moves the arm,
            // the hand and the fingers that hold the light
            anim::RigOutput evaluated;
            anim::SampleRigPose(g_rig, motion, 0.0, g_light_arm, false);
            anim::EvaluateRig(g_rig, g_binding, g_skeleton, g_light_arm, evaluated);
            anim::MakeRigPoseRelative(g_rig, g_binding, g_skeleton, evaluated, g_light_arm);
            for (const anim::RigMask& mask : g_rig.Masks()) {
                if (mask.name == "LArm") {
                    g_light_arm_weights = mask.weights;
                }
            }
            LogInfo("player anim: light arm = motion {:#x}, LArm mask {}", motion.path_code, g_light_arm_weights.empty() ? "missing" : "found");
            continue;
        }
        const anim::GaniUnit* root = motion.FindUnit(anim::kHashRigRoot);
        if (!root || root->translation < 0 || motion.frames < 2) {
            continue;
        }
        const auto& keys = root->tracks[static_cast<size_t>(root->translation)].keys;
        const glm::vec3 delta = keys.size() < 2 ? glm::vec3(0.0f) : glm::vec3(keys.back().value) - glm::vec3(keys.front().value);
        const float distance = glm::length(delta);
        int slot = -1;
        const char* name = nullptr;
        if (distance < 0.05f && root->Loop()) {
            slot = kClipStand;
            name = kClipNames[kClipStand];
            g_locomotion.clips[kClipStand] = MakeClip(motion);
        } else if (distance >= 0.5f && root->Loop()) {
            slot = DirectionOf(delta);
            name = kClipNames[slot];
            g_locomotion.clips[static_cast<size_t>(slot)] = MakeClip(motion);
        } else if (!root->Loop()) {
            slot = DirectionOf(delta);
            const bool start = distance >= 0.5f;
            name = start ? kStartNames[slot] : kStopNames[slot];
            (start ? g_locomotion.starts : g_locomotion.stops)[static_cast<size_t>(slot)] = MakeClip(motion);
        }
        if (name) {
            std::string steps;
            for (const WalkFootstep& s : Footsteps(motion)) {
                steps += std::format(" {}@{}", s.left ? "L" : "R", s.frame);
            }
            LogInfo("player anim: {} = motion {:#x}: {} frames, root {:.3f} m, footsteps{}", name, motion.path_code, motion.frames, distance,
                    steps.empty() ? " none" : steps);
        }
    }
    g_locomotion.loaded = g_rig_ready && g_locomotion.clips[kClipStand].Valid();
    for (size_t d = 0; d < 4; ++d) {
        g_locomotion.loaded = g_locomotion.loaded && g_locomotion.clips[d].Valid() && g_locomotion.starts[d].Valid() && g_locomotion.stops[d].Valid();
    }
    if (!g_locomotion.loaded) {
        LogWarn("player anim: {} lacks walk loops, start/stop clips, stand idle or the HumanFinger rig", kMotionArchive);
    }
    return g_locomotion.loaded;
}

int WalkDirection(float heading_delta) {
    const float t = heading_delta + kPi;
    const float d = t >= 0.0f ? std::fmod(t, 2.0f * kPi) - kPi : kPi - std::fmod(-t, 2.0f * kPi);
    const int bucket = ((static_cast<int>((kPi - d + kPi * 0.25f) * 0.63661975f) * 2 + 4) & 6);
    return bucket / 2;
}

bool LastFootLeft(bool fallback) {
    return g_locomotion.loaded ? g_last_left : fallback;
}

const BodyClip* PlayerBody::Current() const {
    if (!g_locomotion.loaded) {
        return nullptr;
    }
    switch (kind_) {
    case Kind::Start:
        return &g_locomotion.starts[static_cast<size_t>(clip_)];
    case Kind::Stop:
        return &g_locomotion.stops[static_cast<size_t>(clip_)];
    default:
        return &g_locomotion.clips[static_cast<size_t>(clip_)];
    }
}

void PlayerBody::Reset(int clip) {
    kind_ = Kind::Node;
    clip_ = clip;
    frame_ = 0.0f;
    blend_ = 1.0f;
    evaluated_ = false;
    from_.tracks.clear();
    sim_bones_.Reset();
    sim_.Reset();
}

void PlayerBody::Play(Kind kind, int clip) {
    Evaluate();
    from_ = pose_;
    kind_ = kind;
    clip_ = clip;
    frame_ = 0.0f;
    blend_ = 0.0f;
    evaluated_ = false;
}

BodyAdvance PlayerBody::Advance(float frames, float blend_frames, bool drawn_only) {
    // the jacket simulation steps by the game time that passed (blend_frames is dt at 59.94 motion frames per second,
    // independent of the walk rate), not by a fixed 1/60 s whatever the frame rate
    sim_seconds_ += blend_frames / 59.94006f;
    BodyAdvance out;
    const BodyClip* clip = Current();
    if (!clip || !clip->Valid() || frames <= 0.0f) {
        return out;
    }
    const bool loop = kind_ == Kind::Node;
    const float from = frame_;
    const float to = loop ? frame_ + frames : std::min(frame_ + frames, clip->frames);
    out.distance = clip->PathAt(to) - clip->PathAt(from);
    out.root = clip->RootAt(to) - clip->RootAt(from);
    ForEachEvent(clip->steps, from, to, clip->frames, loop, [&](const WalkFootstep& s) {
        ++out.footsteps;
        if (!drawn_only) {
            g_last_left = s.left;
        }
    });
    ForEachEvent(clip->sounds, from, to, clip->frames, loop, [&](const WalkSound& s) { out.sounds.push_back(s.event); });
    frame_ = loop ? std::fmod(to, clip->frames) : to;
    blend_ = std::min(1.0f, blend_ + blend_frames / kBlendFrames);
    evaluated_ = false;
    return out;
}

void PlayerBody::Evaluate() {
    if (evaluated_) {
        return;
    }
    evaluated_ = true;
    const BodyClip* clip = Current();
    if (!g_rig_ready || !clip || !clip->motion) {
        return;
    }
    anim::SampleRigPose(g_rig, *clip->motion, frame_, pose_, kind_ == Kind::Node);
    anim::EvaluateRig(g_rig, g_binding, g_skeleton, pose_, out_);
    anim::MakeRigPoseRelative(g_rig, g_binding, g_skeleton, out_, pose_);
    if (blend_ < 1.0f && !from_.tracks.empty()) {
        anim::BlendRigPose(g_rig, from_, pose_, blend_, pose_);
        anim::EvaluateRig(g_rig, g_binding, g_skeleton, pose_, out_);
    }
    if (static_cast<size_t>(g_head_bone) < out_.position.size()) {
        head_ = out_.position[static_cast<size_t>(g_head_bone)];
        head_rotation_ = out_.rotation[static_cast<size_t>(g_head_bone)];
    }
}

glm::vec3 PlayerBody::Head() {
    Evaluate();
    return head_;
}

glm::quat PlayerBody::HeadRotation() {
    Evaluate();
    return head_rotation_;
}

namespace {
// The leg units of the HumanFinger rig and the foot rotation units, left first (FootPlant)
struct LegUnits {
    int leg[2] = {-1, -1};
    int foot[2] = {-1, -1};
    bool found = false;
};

const LegUnits& FindLegUnits() {
    static LegUnits units;
    if (units.found || !g_rig_ready) {
        return units;
    }
    units.found = true;
    const auto bone_name = [](int joint) {
        const int bone = g_binding.Bone(joint);
        return bone >= 0 && static_cast<size_t>(bone) < g_skeleton.names.size() ? g_skeleton.names[static_cast<size_t>(bone)] : std::string();
    };
    const auto& rig_units = g_rig.Units();
    for (size_t u = 0; u < rig_units.size(); ++u) {
        const anim::RigUnit& unit = rig_units[u];
        if (unit.joints.empty()) {
            continue;
        }
        const std::string name = bone_name(unit.joints[0]);
        if (unit.Is(anim::RigUnitType::Leg)) {
            units.leg[name.find("_L") != std::string::npos ? 0 : 1] = static_cast<int>(u);
        } else if (name == "SKL_032_LFOOT" || name == "SKL_042_RFOOT") {
            units.foot[name == "SKL_032_LFOOT" ? 0 : 1] = static_cast<int>(u);
            LogInfo("player anim: foot rotation unit {} for {} is of type {}", u, name, unit.type);
        }
    }
    return units;
}

glm::quat TrackQuat(const glm::vec4& v) {
    return glm::quat(v.w, v.x, v.y, v.z);
}

glm::vec4 QuatTrack(const glm::quat& q) {
    return glm::vec4(q.x, q.y, q.z, q.w);
}
}

// moves the legs' IK targets (relative to the thigh's parent, MakeRigPoseRelative) to the planted ones and turns their knee
// swivels and the feet's rotations about the model's vertical axis (PlayerBody::FootPlant)
bool PlayerBody::ApplyFootPlant(const FootPlant& plant, const anim::RigOutput& evaluated, anim::RigPose& pose) const {
    const LegUnits& units = FindLegUnits();
    if (!pose.relative) {
        return false;
    }
    bool changed = false;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    for (int side = 0; side < 2; ++side) {
        if (units.leg[side] < 0 || !plant.active[side]) {
            continue;
        }
        const glm::quat turn = glm::angleAxis(plant.yaw[side], up);
        const anim::RigUnit& leg = g_rig.Units()[static_cast<size_t>(units.leg[side])];
        const int thigh = g_binding.Bone(leg.joints[0]);
        if (thigh < 0 || static_cast<size_t>(thigh) >= g_skeleton.parents.size()) {
            continue;
        }
        const int parent = g_skeleton.parents[static_cast<size_t>(thigh)];
        const bool has_parent = parent >= 0 && static_cast<size_t>(parent) < evaluated.position.size();
        const glm::vec3 pp = has_parent ? evaluated.position[static_cast<size_t>(parent)] : glm::vec3(0.0f);
        const glm::quat pr = has_parent ? evaluated.rotation[static_cast<size_t>(parent)] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (leg.tracks.size() >= 2 && static_cast<size_t>(leg.tracks[1]) < pose.tracks.size()) {
            glm::vec4& target = pose.tracks[static_cast<size_t>(leg.tracks[0])];
            target = glm::vec4(plant.target[side] - pp, 0.0f);
            glm::vec4& swivel = pose.tracks[static_cast<size_t>(leg.tracks[1])];
            swivel = QuatTrack(glm::normalize(glm::conjugate(pr) * turn * pr * TrackQuat(swivel)));
            changed = true;
        }
        // a foot placed in model space (rotation unit) turns with its target; a foot rotation relative to the leg follows the leg
        if (units.foot[side] >= 0) {
            const anim::RigUnit& foot = g_rig.Units()[static_cast<size_t>(units.foot[side])];
            if (foot.Is(anim::RigUnitType::Rotation) && !foot.tracks.empty() && static_cast<size_t>(foot.tracks[0]) < pose.tracks.size()) {
                glm::vec4& q = pose.tracks[static_cast<size_t>(foot.tracks[0])];
                q = QuatTrack(glm::normalize(turn * TrackQuat(q)));
            }
        }
    }
    return changed;
}

bool PlayerBody::FootTargets(glm::vec3 out[2]) {
    Evaluate();
    const LegUnits& units = FindLegUnits();
    if (!g_rig_ready || !pose_.relative) {
        return false;
    }
    for (int side = 0; side < 2; ++side) {
        out[side] = glm::vec3(0.0f);
        if (units.leg[side] < 0) {
            return false;
        }
        const anim::RigUnit& leg = g_rig.Units()[static_cast<size_t>(units.leg[side])];
        const int thigh = g_binding.Bone(leg.joints[0]);
        if (thigh < 0 || static_cast<size_t>(thigh) >= g_skeleton.parents.size() || leg.tracks.empty() ||
            static_cast<size_t>(leg.tracks[0]) >= pose_.tracks.size()) {
            return false;
        }
        const int parent = g_skeleton.parents[static_cast<size_t>(thigh)];
        const glm::vec3 pp = parent >= 0 && static_cast<size_t>(parent) < out_.position.size() ? out_.position[static_cast<size_t>(parent)] : glm::vec3(0.0f);
        out[side] = glm::vec3(pose_.tracks[static_cast<size_t>(leg.tracks[0])]) + pp;
    }
    return true;
}

bool PlayerBody::Skin(const anim::HelpBones* help, std::vector<glm::mat4>& skin, bool light_arm, const anim::SimRig* sim,
                      const glm::mat4* body_world, const HandReach* reach) {
    Evaluate();
    const anim::RigOutput* out = &out_;
    if (light_arm && g_light_arm.tracks.size() == pose_.tracks.size() && g_light_arm.relative == pose_.relative &&
        g_light_arm_weights.size() == g_rig.Units().size()) {
        // a layer over the body pose by the mask's unit weights (0xAC4440 per unit type)
        arm_pose_ = pose_;
        for (size_t u = 0; u < g_rig.Units().size(); ++u) {
            const float w = g_light_arm_weights[u];
            if (w <= 0.0f) {
                continue;
            }
            anim::RigPose layered;
            anim::BlendRigPose(g_rig, arm_pose_, g_light_arm, w, layered);
            for (int t : g_rig.Units()[u].tracks) {
                if (t >= 0 && static_cast<size_t>(t) < arm_pose_.tracks.size()) {
                    arm_pose_.tracks[static_cast<size_t>(t)] = layered.tracks[static_cast<size_t>(t)];
                }
            }
        }
        anim::EvaluateRig(g_rig, g_binding, g_skeleton, arm_pose_, arm_out_);
        out = &arm_out_;
    }
    if (plant_.Any() && g_rig_ready) {
        plant_pose_ = out == &arm_out_ ? arm_pose_ : pose_;
        if (ApplyFootPlant(plant_, *out, plant_pose_)) {
            anim::EvaluateRig(g_rig, g_binding, g_skeleton, plant_pose_, plant_out_);
            out = &plant_out_;
        }
    }
    const size_t n = std::min({g_skeleton.Size(), out->position.size(), out->rotation.size()});
    if (!g_rig_ready || n == 0) {
        return false;
    }
    world_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        world_[i] = glm::translate(glm::mat4(1.0f), out->position[i]) * glm::mat4_cast(out->rotation[i]);
    }
    if (reach && *reach) {
        glm::mat4 hand(1.0f);
        glm::mat4 target(1.0f);
        if (BoneModel("SKL_013_LHAND", hand) && (*reach)(hand, target)) {
            ReachLeftHand(target);
        }
    }
    if (help) {
        help->Apply(g_skeleton, world_);
    }
    // a time since the last skinning longer than 0.25 s (the mirror captures run now and then) restarts from the pose
    if (!sim || !body_world || !sim_.Step(*sim, g_skeleton, world_, *body_world, sim_seconds_, anim::SimWind())) {
        sim_bones_.Apply(g_skeleton, world_, sim_seconds_);
    }
    sim_seconds_ = 0.0f;
    anim::ComputeSkin(g_skeleton, world_, skin);
    if (std::getenv("PT_HANDY_BONES") != nullptr && body_world) {
        LogHandBones(*body_world);
    }
    return true;
}


namespace {
// the shortest turn from unit vector u to unit vector v
glm::quat ArcBetween(const glm::vec3& u, const glm::vec3& v) {
    const float c = glm::dot(u, v);
    if (c < -0.99999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), u);
        if (glm::length(axis) < 1.0e-4f) {
            axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), u);
        }
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(u, v);
    return glm::normalize(glm::quat(1.0f + c, axis.x, axis.y, axis.z));
}
}  // namespace

// turns `root` and every bone below it about `pivot` (model space)
void PlayerBody::TurnSubtree(int root, const glm::quat& turn, const glm::vec3& pivot) {
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(turn) * glm::translate(glm::mat4(1.0f), -pivot);
    for (size_t i = 0; i < world_.size() && i < g_skeleton.parents.size(); ++i) {
        int b = static_cast<int>(i);
        while (b >= 0 && b != root) {
            b = g_skeleton.parents[static_cast<size_t>(b)];
        }
        if (b == root) {
            world_[i] = m * world_[i];
        }
    }
}

void PlayerBody::ReachLeftHand(const glm::mat4& target) {
    const int upper = g_skeleton.FindName("SKL_011_LUARM");
    const int fore = g_skeleton.FindName("SKL_012_LFARM");
    const int hand = g_skeleton.FindName("SKL_013_LHAND");
    if (upper < 0 || fore < 0 || hand < 0 || static_cast<size_t>(std::max({upper, fore, hand})) >= world_.size()) {
        return;
    }
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 s = at(upper);
    const glm::vec3 e = at(fore);
    const glm::vec3 w = at(hand);
    const glm::vec3 t(target[3]);
    const float a = glm::length(e - s);
    const float b = glm::length(w - e);
    const glm::vec3 to = t - s;
    if (a < 1.0e-4f || b < 1.0e-4f || glm::length(to) < 1.0e-4f) {
        return;
    }
    const float d = std::clamp(glm::length(to), std::abs(a - b) + 1.0e-4f, a + b - 1.0e-4f);
    const glm::vec3 n = glm::normalize(to);
    // the elbow stays on the side it bends to now, and hangs: the target above the shoulder would otherwise lift it over the head
    glm::vec3 pole = (e - s) + glm::vec3(0.0f, -0.5f * a, 0.0f);
    pole -= n * glm::dot(pole, n);
    if (glm::length(pole) < 1.0e-5f) {
        pole = glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    pole = glm::normalize(pole);
    const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const glm::vec3 elbow = s + n * (a * cos_a) + pole * (a * std::sqrt(1.0f - cos_a * cos_a));
    TurnSubtree(upper, ArcBetween(glm::normalize(e - s), glm::normalize(elbow - s)), s);
    const glm::vec3 wrist = at(hand);
    TurnSubtree(fore, ArcBetween(glm::normalize(wrist - elbow), glm::normalize(s + n * d - elbow)), elbow);
    const glm::quat now = glm::quat_cast(glm::mat3(world_[static_cast<size_t>(hand)]));
    const glm::quat want = glm::quat_cast(glm::mat3(target));
    TurnSubtree(hand, glm::normalize(want * glm::inverse(now)), at(hand));
}

bool PlayerBody::BoneModel(const char* name, glm::mat4& out) const {
    if (!g_rig_ready || g_skeleton.Empty() || world_.empty()) {
        return false;
    }
    const int b = g_skeleton.FindName(name);
    if (b < 0 || static_cast<size_t>(b) >= world_.size()) {
        return false;
    }
    out = world_[static_cast<size_t>(b)];
    return true;
}

void PlayerBody::LogHandBones(const glm::mat4& body_world) const {
    if (!g_rig_ready || g_skeleton.Empty() || world_.empty()) {
        return;
    }
    for (const char* name : {"SKL_013_LHAND", "SKL_023_RHAND", "SKL_101_LF10", "SKL_104_LF21", "SKL_107_LF31", "SKL_110_LF40"}) {
        const int b = g_skeleton.FindName(name);
        if (b < 0 || static_cast<size_t>(b) >= world_.size()) {
            continue;
        }
        const glm::vec4 p = body_world * world_[static_cast<size_t>(b)] * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        LogInfo("handy bones: {} at world ({:.3f} {:.3f} {:.3f})", name, p.x, p.y, p.z);
    }

}
}
