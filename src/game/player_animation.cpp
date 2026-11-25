#include "game/player_animation.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/anim/anim_codec.h"
#include "engine/anim/gani.h"
#include "engine/anim/help_bones.h"
#include "engine/anim/motion_data.h"
#include "engine/anim/skeleton.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/resource_path.h"
#include "engine/core/strcode.h"
#include "engine/fs/mods.h"
#include "engine/fs/vfs.h"
#include "game/player_run_cycle.h"

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

struct StrafeClip {
    static constexpr size_t kBones = 9;
    int frames = 0;
    float distance = 0.0f;
    int bone[kBones]{};
    std::vector<glm::vec3> place[2];
    std::vector<glm::quat> turn[2];
};
StrafeClip g_strafe;

void LoadStrafeClip() {
    g_strafe = StrafeClip{};
    const std::optional<std::vector<uint8_t>> bytes = mods::ReadDiskFile(ExecutableDir() / "survival" / "body_clips.json");
    json::Value doc;
    std::string error;
    if (!bytes || !json::Parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), doc, &error)) {
        LogWarn("player anim: survival/body_clips.json {}, the side walk stays the original's", bytes ? error : std::string("missing"));
        return;
    }
    const json::Value* clip = doc.Find("strafe");
    const json::Value* bones = clip ? clip->Find("bones") : nullptr;
    if (!clip || !bones || bones->Size() != StrafeClip::kBones) return;
    StrafeClip out;
    out.frames = static_cast<int>(clip->Number("frames", 0));
    out.distance = static_cast<float>(clip->Number("distance", 0.0));
    for (size_t b = 0; b < StrafeClip::kBones; ++b) {
        out.bone[b] = g_skeleton.FindName(bones->At(b)->string.c_str());
        if (out.bone[b] < 0) return;
    }
    static const char* const kSides[2] = {"right", "left"};
    for (size_t side = 0; side < 2; ++side) {
        const json::Value* rows = clip->Find(kSides[side]);
        if (!rows || static_cast<int>(rows->Size()) != out.frames || out.frames < 2 || out.distance <= 0.0f) return;
        for (size_t f = 0; f < rows->Size(); ++f) {
            const json::Value* row = rows->At(f);
            if (!row || row->Size() != StrafeClip::kBones) return;
            for (size_t b = 0; b < StrafeClip::kBones; ++b) {
                const json::Value* v = row->At(b);
                if (!v || v->Size() != 7) return;
                const auto n = [v](size_t k) { return static_cast<float>(v->At(k)->number); };
                out.place[side].push_back(glm::vec3(n(0), n(1), n(2)));
                out.turn[side].push_back(glm::quat(n(3), n(4), n(5), n(6)));
            }
        }
    }
    g_strafe = std::move(out);
    LogInfo("player anim: the side step from survival/body_clips.json: {} frames, {:.2f} m a cycle", g_strafe.frames, g_strafe.distance);
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

float PlayerStrafeDistance() {
    return g_strafe.frames > 0 ? g_strafe.distance : 0.0f;
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
    if (g_rig_ready) LoadStrafeClip();
    return g_locomotion.loaded;
}

bool PlayerPoseSkin(const anim::HelpBones* help, float frame, const PlayerPoseEdit& edit, std::vector<glm::mat4>& skin) {
    const BodyClip& stand = g_locomotion.clips[kClipStand];
    if (!g_rig_ready || !stand.motion) {
        return false;
    }
    anim::RigPose pose;
    anim::RigOutput out;
    const double at = stand.frames > 0.0f ? std::fmod(static_cast<double>(std::max(frame, 0.0f)), static_cast<double>(stand.frames)) : 0.0;
    anim::SampleRigPose(g_rig, *stand.motion, at, pose, true);
    anim::EvaluateRig(g_rig, g_binding, g_skeleton, pose, out);
    const size_t n = std::min({g_skeleton.Size(), out.position.size(), out.rotation.size()});
    if (n == 0) {
        return false;
    }
    std::vector<glm::mat4> world(n);
    for (size_t i = 0; i < n; ++i) {
        world[i] = glm::translate(glm::mat4(1.0f), out.position[i]) * glm::mat4_cast(out.rotation[i]);
    }
    if (edit) {
        edit(g_skeleton, world);
    }
    if (help) {
        help->Apply(g_skeleton, world);
    }
    anim::ComputeSkin(g_skeleton, world, skin);
    return true;
}

bool PlayerClipTravel(int clip, float& frames, float& travel, const anim::Skeleton* target) {
    if (clip < 0 || clip >= kClipCount) return false;
    const BodyClip& c = g_locomotion.clips[clip];
    if (!c.Valid()) return false;
    frames = c.frames;
    travel = c.path.back();
    if (target) {
        const int sw = g_skeleton.FindName("SKL_000_WAIST"), st = g_skeleton.FindName("SKL_033_LTOE");
        const int tw = target->FindName("SKL_000_WAIST"), tt = target->FindName("SKL_033_LTOE");
        if (sw >= 0 && st >= 0 && tw >= 0 && tt >= 0) {
            const float source_leg = g_skeleton.bind_world[static_cast<size_t>(sw)].y - g_skeleton.bind_world[static_cast<size_t>(st)].y;
            const float target_leg = target->bind_world[static_cast<size_t>(tw)].y - target->bind_world[static_cast<size_t>(tt)].y;
            if (source_leg > 1e-3f) travel *= target_leg / source_leg;
        }
    }
    return true;
}

bool PlayerClipPose(int clip, double frame, const anim::Skeleton& target, anim::Pose& out) {
    if (clip < 0 || clip >= kClipCount || !g_rig_ready) return false;
    const BodyClip& c = g_locomotion.clips[clip];
    if (!c.motion || c.frames <= 0.0f) return false;
    anim::RigPose pose;
    anim::RigOutput evaluated;
    const double at = std::fmod(std::max(frame, 0.0), static_cast<double>(c.frames));
    anim::SampleRigPose(g_rig, *c.motion, at, pose, true);
    anim::EvaluateRig(g_rig, g_binding, g_skeleton, pose, evaluated);
    const size_t n = std::min({g_skeleton.Size(), evaluated.position.size(), evaluated.rotation.size()});
    if (n == 0) return false;
    out.Reset(target.Size());
    for (size_t i = 0; i < target.Size(); ++i) {
        const int j = g_skeleton.FindName(target.names[i]);
        if (j < 0 || static_cast<size_t>(j) >= n) continue;
        const int16_t p = g_skeleton.parents[static_cast<size_t>(j)];
        const glm::quat parent = p >= 0 && static_cast<size_t>(p) < n ? evaluated.rotation[static_cast<size_t>(p)] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        out.rotation[i] = glm::normalize(glm::inverse(parent) * evaluated.rotation[static_cast<size_t>(j)]);
    }
    const int sw = g_skeleton.FindName("SKL_000_WAIST"), st = g_skeleton.FindName("SKL_033_LTOE");
    const int tw = target.FindName("SKL_000_WAIST"), tt = target.FindName("SKL_033_LTOE");
    if (sw >= 0 && st >= 0 && tw >= 0 && tt >= 0 && static_cast<size_t>(sw) < n) {
        const float source_leg = g_skeleton.bind_world[static_cast<size_t>(sw)].y - g_skeleton.bind_world[static_cast<size_t>(st)].y;
        const float target_leg = target.bind_world[static_cast<size_t>(tw)].y - target.bind_world[static_cast<size_t>(tt)].y;
        const float size = source_leg > 1e-3f ? target_leg / source_leg : 1.0f;
        glm::vec3 moved = evaluated.position[static_cast<size_t>(sw)] - g_skeleton.bind_world[static_cast<size_t>(sw)];
        moved.y -= source_leg;
        out.offset[static_cast<size_t>(tw)] = glm::vec3(moved.x * size, target_leg + moved.y * size, 0.0f) - target.bind_local[static_cast<size_t>(tw)];
    }
    return true;
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
    run_frame_ = -1.0f;
}

namespace {
float RightStep(const BodyClip& clip) {
    for (const WalkFootstep& s : clip.steps) {
        if (!s.left) return s.frame;
    }
    return 0.0f;
}

float StrideFrame(const BodyClip& clip, float phase) {
    float frame = std::fmod(phase * clip.frames + RightStep(clip), clip.frames);
    if (frame < 0.0f) frame += clip.frames;
    return frame;
}
}

float PlayerBody::StridePhase() const {
    const BodyClip* clip = Current();
    if (!clip || clip->frames <= 0.0f) return 0.0f;
    const float phase = (frame_ - RightStep(*clip)) / clip->frames;
    return phase - std::floor(phase);
}

void PlayerBody::Steer(int clip) {
    const float phase = StridePhase();
    clip_ = clip;
    frame_ = StrideFrame(g_locomotion.clips[static_cast<size_t>(clip)], phase);
    evaluated_ = false;
}

void PlayerBody::EvaluateStride() {
    const float length = std::min(1.0f, glm::length(stride_));
    const float across = std::abs(stride_.x), along = std::abs(stride_.y);
    const float sum = across + along;
    const float side = sum > 1.0e-4f ? length * across / sum : 0.0f;
    const float ahead = sum > 1.0e-4f ? length * along / sum : 0.0f;
    const struct { int clip; float weight; } parts[] = {
        {stride_.y >= 0.0f ? 0 : 2, ahead}, {stride_.x >= 0.0f ? 3 : 1, side}, {kClipStand, 1.0f - length}};
    const float phase = StridePhase();
    float total = 0.0f;
    for (const auto& part : parts) {
        if (part.weight < 1.0e-3f) continue;
        const BodyClip& clip = g_locomotion.clips[static_cast<size_t>(part.clip)];
        anim::SampleRigPose(g_rig, *clip.motion, part.clip == kClipStand ? 0.0f : StrideFrame(clip, phase), stride_pose_, true);
        anim::EvaluateRig(g_rig, g_binding, g_skeleton, stride_pose_, stride_out_);
        anim::MakeRigPoseRelative(g_rig, g_binding, g_skeleton, stride_out_, stride_pose_);
        if (total <= 0.0f) {
            pose_ = stride_pose_;
        } else {
            anim::BlendRigPose(g_rig, pose_, stride_pose_, part.weight / (total + part.weight), pose_);
        }
        total += part.weight;
    }
    anim::EvaluateRig(g_rig, g_binding, g_skeleton, pose_, out_);
}

BodyAdvance PlayerBody::Advance(float frames, float blend_frames, bool drawn_only) {
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
    blend_ = std::min(1.0f, blend_ + blend_frames / (run > 0.0f ? 2.0f * kBlendFrames : kBlendFrames));
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
    if (stride_on_ && kind_ == Kind::Node && clip_ != kClipStand) {
        EvaluateStride();
    } else {
        anim::SampleRigPose(g_rig, *clip->motion, frame_, pose_, kind_ == Kind::Node);
        anim::EvaluateRig(g_rig, g_binding, g_skeleton, pose_, out_);
        anim::MakeRigPoseRelative(g_rig, g_binding, g_skeleton, out_, pose_);
    }
    if (blend_ < 1.0f && !from_.tracks.empty()) {
        anim::BlendRigPose(g_rig, from_, pose_, run > 0.0f ? blend_ * blend_ * (3.0f - 2.0f * blend_) : blend_, pose_);
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

namespace {
constexpr std::array<glm::vec4, 5> kEvenFingers = {glm::vec4(0.0f, 0.0f, 0.35f, 0.35f), glm::vec4(0.0f, 1.0f, 1.15f, 0.8f), glm::vec4(0.0f, 1.0f, 1.15f, 0.8f),
                                                   glm::vec4(0.0f, 1.0f, 1.15f, 0.8f), glm::vec4(0.0f, 1.0f, 1.15f, 0.8f)};
}

namespace {
void Follow(float& value, float& speed, float target, float omega, float dt) {
    if (dt <= 0.0f) return;
    const float off = value - target;
    const float push = (speed + omega * off) * dt;
    const float decay = std::exp(-omega * dt);
    speed = (speed - omega * push) * decay;
    value = std::clamp(target + (off + push) * decay, 0.0f, 1.0f);
    if (std::abs(value - target) < 0.002f && std::abs(speed) < 0.02f) {
        value = target;
        speed = 0.0f;
    }
}
}

bool PlayerBody::Skin(const anim::HelpBones* help, std::vector<glm::mat4>& skin, bool light_arm, const anim::SimRig* sim,
                      const glm::mat4* body_world, const HandReach* reach, const glm::mat4* right_target, float right_grip, float left_grip) {
    Evaluate();
    const anim::RigOutput* out = &out_;
    if (light_arm && g_light_arm.tracks.size() == pose_.tracks.size() && g_light_arm.relative == pose_.relative &&
        g_light_arm_weights.size() == g_rig.Units().size()) {
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
    if (ragdoll && ragdoll->Active() && body_world) {
        ragdoll->ApplyToBones(g_skeleton, *body_world, world_);
        if (help) {
            help->Apply(g_skeleton, world_);
        }
        if (sim_seconds_ > 0.25f) sim_.Reset();
        sim_.SetDelay(0.0f);
        if (!sim || !sim_.Step(*sim, g_skeleton, world_, *body_world, sim_seconds_, anim::SimWind())) {
            sim_bones_.Apply(g_skeleton, world_, sim_seconds_);
        }
        sim_seconds_ = 0.0f;
        anim::ComputeSkin(g_skeleton, world_, skin);
        return true;
    }
    snap_now_ = ease_reach && std::exchange(snap_reach, false);
    ++skin_serial_;
    if (run > 0.0f) {
        RunOverlay(run);
    }
    if (strafe_weight > 0.0f && g_strafe.frames > 0) {
        StrafeOverlay();
    }
    for (int side = 0; side < 2; ++side) {
        if (!KneeToToe(side, pose_knee_toe_[side], pose_toe_side_[side], &pose_knee_out_[side])) pose_toe_side_[side] = pose_knee_out_[side] = 0.0f;
    }
    if (std::abs(hips_lag) > 1.0e-4f) LagHips(hips_lag);
    if (snap_now_ || sim_seconds_ > 0.25f) {
        steady_now_ = steady;
    } else {
        steady_now_ += (steady - steady_now_) * (1.0f - std::exp(-sim_seconds_ * 7.0f));
    }
    if (steady_now_ > 1.0e-3f) {
        SteadyTorso(steady_now_);
    } else {
        steady_primed_ = false;
    }
    if (ease_reach) {
        IdleOverlay();
    }
    if (const int b = g_skeleton.FindName("SKL_002_CHEST"); b >= 0 && static_cast<size_t>(b) < world_.size()) {
        natural_chest_ = world_[static_cast<size_t>(b)];
        natural_chest_known_ = true;
    } else {
        natural_chest_known_ = false;
    }
    if (snap_now_) twist_primed_[0] = twist_primed_[1] = false;
    for (int side = 0; side < 2; ++side) reach_age_[side] = snap_now_ ? 1.0f : reach_age_[side] + sim_seconds_;
    if (snap_now_ || sim_seconds_ > 0.25f) {
        carry_now_ = carry;
    } else {
        carry_now_ += (carry - carry_now_) * (1.0f - std::exp(-sim_seconds_ * 7.0f));
    }
    if (glm::dot(carry_now_, carry_now_) > 1.0e-6f) {
        CarryOverlay(carry_now_);
    }
    if (aim > 0.0f) {
        AimOverlay(aim, aim_pitch);
    }
    if (melee != glm::vec3(0.0f)) {
        MeleeOverlay(melee);
    }
    if (ease_reach) {
        IdleHeadOverlay();
    }
    if (body_world) {
        HoldFeet(*body_world);
    }
    if (aim_knees) {
        AimKnee(0);
        AimKnee(1);
    }
    if (ease_reach) {
        KeepArmOut(false);
        KeepArmOut(true);
    }
    {
        const char* const kArm[3] = {"SKL_021_RUARM", "SKL_022_RFARM", "SKL_023_RHAND"};
        natural_known_ = true;
        for (int i = 0; i < 3; ++i) {
            const int b = g_skeleton.FindName(kArm[i]);
            if (b < 0 || static_cast<size_t>(b) >= world_.size()) {
                natural_known_ = false;
                break;
            }
            natural_right_[i] = world_[static_cast<size_t>(b)];
        }
    }
    glm::mat4 carried_target(1.0f);
    if (right_target && carry_weight > 0.0f && natural_chest_known_) {
        const glm::mat4 authored = natural_chest_ * carry_hand;
        const float w = std::clamp(carry_weight, 0.0f, 1.0f);
        carried_target = glm::mat4_cast(glm::slerp(glm::normalize(glm::quat_cast(glm::mat3(*right_target))), glm::normalize(glm::quat_cast(glm::mat3(authored))), w));
        carried_target[3] = glm::vec4(glm::mix(glm::vec3((*right_target)[3]), glm::vec3(authored[3]), w), 1.0f);
        right_target = &carried_target;
    }
    if (!ease_reach) {
        if (right_target) ReachHand(true, *right_target, true);
    } else if (glm::mat4 hand(1.0f); BoneModel("SKL_023_RHAND", hand)) {
        if (snap_now_) {
            right_reach_ = right_target ? 1.0f : 0.0f;
            right_reach_speed_ = 0.0f;
        }
        Follow(right_reach_, right_reach_speed_, right_target ? 1.0f : 0.0f, reach_rate, sim_seconds_);
        if (right_target) right_target_ = *right_target;
        if (right_reach_ >= 1.0f) {
            pole_share_[1] = 1.0f;
            ReachHand(true, right_target_, true);
        } else if (right_reach_ > 0.0f) {
            const float k = right_reach_;
            glm::mat4 between = glm::mat4_cast(glm::slerp(glm::normalize(glm::quat_cast(glm::mat3(hand))), glm::normalize(glm::quat_cast(glm::mat3(right_target_))), k));
            between[3] = glm::vec4(glm::mix(glm::vec3(hand[3]), glm::vec3(right_target_[3]), k), 1.0f);
            pole_share_[1] = k;
            ReachHand(true, between, true);
        } else {
            pole_primed_[1] = false;
            twist_primed_[1] = false;
        }
    }
    if (const int fore = g_skeleton.FindName("SKL_022_RFARM"), hand = g_skeleton.FindName("SKL_023_RHAND");
        fore >= 0 && hand >= 0 && static_cast<size_t>(std::max(fore, hand)) < world_.size()) {
        right_fore_[0] = glm::vec3(world_[static_cast<size_t>(fore)][3]);
        right_fore_[1] = glm::vec3(world_[static_cast<size_t>(hand)][3]);
        right_fore_seen_ = ease_reach;
    }
    if (reach && *reach) {
        glm::mat4 hand(1.0f);
        glm::mat4 target(1.0f);
        const bool known = BoneModel("SKL_013_LHAND", hand);
        const bool wanted = known && (*reach)(hand, target);
        if (!ease_reach) {
            if (wanted) ReachLeftHand(target);
        } else if (known) {
            if (snap_now_) {
                left_reach_ = wanted ? 1.0f : 0.0f;
                left_reach_speed_ = 0.0f;
                left_shift_ = glm::vec3(0.0f);
                left_turn_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                left_wanted_ = wanted;
            }
            const float reach_before = left_reach_;
            Follow(left_reach_, left_reach_speed_, wanted ? 1.0f : 0.0f, reach_rate, sim_seconds_);
            const float wear = std::exp(-sim_seconds_ * 8.0f);
            left_shift_ *= wear;
            left_turn_ = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), left_turn_, wear);
            if (wanted) {
                if (!left_wanted_ && reach_before > 0.03f) {
                    left_shift_ = glm::vec3(left_target_[3]) - glm::vec3(target[3]);
                    left_turn_ = glm::normalize(glm::quat_cast(glm::mat3(left_target_)) * glm::inverse(glm::quat_cast(glm::mat3(target))));
                }
                if (left_wanted_ && left_raw_known_) {
                    const glm::quat was = glm::normalize(glm::quat_cast(glm::mat3(left_raw_))), now = glm::normalize(glm::quat_cast(glm::mat3(target)));
                    if (2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(was, now)))) > 0.3f) {
                        left_turn_ = glm::normalize(left_turn_ * was * glm::inverse(now));
                    }
                    if (const glm::vec3 jump = glm::vec3(left_raw_[3]) - glm::vec3(target[3]); glm::length(jump) > 0.04f) left_shift_ += jump;
                }
                left_raw_ = target;
                left_raw_known_ = true;
                left_target_ = glm::mat4_cast(left_turn_) * glm::mat4(glm::mat3(target));
                left_target_[3] = glm::vec4(glm::vec3(target[3]) + left_shift_, 1.0f);
            }
            left_wanted_ = wanted;
            if (!wanted) left_raw_known_ = false;
            if (left_reach_ > 0.0f) {
                const float k = left_reach_;
                const glm::quat hand_turn = glm::normalize(glm::quat_cast(glm::mat3(hand)));
                glm::quat delta = glm::normalize(glm::normalize(glm::quat_cast(glm::mat3(left_target_))) * glm::inverse(hand_turn));
                if (delta.w < 0.0f) delta = -delta;
                float delta_angle = 2.0f * std::acos(std::min(1.0f, delta.w));
                glm::vec3 delta_axis(delta.x, delta.y, delta.z);
                if (glm::length(delta_axis) > 1.0e-5f) {
                    delta_axis = glm::normalize(delta_axis);
                    if ((between_serial_ + 1 == skin_serial_ || between_serial_ == skin_serial_) && glm::dot(delta_axis, between_axis_) < 0.0f && delta_angle > 2.0f) {
                        delta_axis = -delta_axis;
                        delta_angle = 6.2831853f - delta_angle;
                    }
                    between_axis_ = delta_axis;
                    between_serial_ = skin_serial_;
                } else {
                    delta_axis = glm::vec3(0.0f, 1.0f, 0.0f);
                    delta_angle = 0.0f;
                }
                glm::mat4 between = glm::mat4_cast(glm::angleAxis(delta_angle * k, delta_axis) * hand_turn);
                between[3] = glm::vec4(glm::mix(glm::vec3(hand[3]), glm::vec3(left_target_[3]), k), 1.0f);
                between[3].z += 0.035f * 4.0f * k * (1.0f - k);
                const float transit = 1.0f - std::clamp((k - 0.85f) / 0.13f, 0.0f, 1.0f);
                glm::vec3 way = TorsoPush(glm::vec3(between[3]), glm::mix(0.058f * ArmRoom(), 0.075f, std::clamp(k * 3.0f, 0.0f, 1.0f))) * transit;
                way.y = 0.0f;
                if (way.x * between[3].x < 0.0f) way.x = 0.0f;
                between[3] += glm::vec4(EasedPush(left_way_push_, way), 0.0f);
                glm::vec3 arm_side(1.0f, 0.0f, 0.0f);
                const bool mid_reach = k < 0.98f;
                if (right_fore_seen_) {
                    const glm::vec3 line = right_fore_[1] - right_fore_[0];
                    if (const float length = glm::length(line); length > 1.0e-3f) {
                        const glm::vec3 along_dir = line / length;
                        arm_side = arm_side - along_dir * along_dir.x;
                        arm_side = glm::length(arm_side) > 1.0e-3f ? glm::normalize(arm_side) : glm::vec3(1.0f, 0.0f, 0.0f);
                    }
                    between[3] += glm::vec4(arm_side * left_arm_push_, 0.0f);
                }
                pole_share_[0] = k;
                ReachLeftHand(between);
                if (right_fore_seen_ && mid_reach) {
                    const glm::vec3 line = right_fore_[1] - right_fore_[0];
                    const float length = glm::length(line);
                    float short_by = -1.0f;
                    if (length > 1.0e-3f) {
                        const glm::vec3 along_dir = line / length;
                        const glm::vec3 over = glm::normalize(glm::cross(along_dir, arm_side));
                        constexpr float kClear = 0.078f;
                        static const char* const kPoints[] = {"SKL_013_LHAND", "SKL_104_LF21", "SKL_106_LF23", "SKL_107_LF31", "SKL_109_LF33", "SKL_111_LF41", "SKL_113_LF43",
                                                              "SKL_114_LF51", "SKL_116_LF53", "SKL_103_LF12"};
                        float near_end = 0.0f;
                        for (const char* name : kPoints) {
                            const int b = g_skeleton.FindName(name);
                            if (b < 0 || static_cast<size_t>(b) >= world_.size()) continue;
                            const glm::vec3 from = glm::vec3(world_[static_cast<size_t>(b)][3]) - right_fore_[0];
                            const float along = glm::dot(from, along_dir) / length;
                            if (name == kPoints[0]) near_end = std::clamp((0.95f - along) / 0.1f, 0.0f, 1.0f);
                            const float height = std::abs(glm::dot(from, over));
                            if (along < -0.1f || along > 1.5f || height > 0.11f) continue;
                            const float t = std::clamp((0.11f - height) / 0.04f, 0.0f, 1.0f) * near_end;
                            short_by = std::max(short_by, kClear * t * t * (3.0f - 2.0f * t) - glm::dot(from, arm_side));
                        }
                    }
                    const float dt = std::min(sim_seconds_, 0.05f);
                    left_arm_push_ = std::clamp(left_arm_push_ + (short_by > 0.0f ? short_by * std::min(1.0f, dt * 40.0f) : std::max(short_by, -0.3f * dt)), 0.0f, 0.3f);
                } else {
                    const float dt = std::min(sim_seconds_, 0.05f);
                    left_arm_push_ = std::max(0.0f, left_arm_push_ - 0.5f * dt);
                }
            } else {
                pole_primed_[0] = false;
                twist_primed_[0] = false;
                forearm_primed_ = false;
            }
        }
    }
    if (const float amount = ease_reach ? right_grip * right_reach_ : right_grip; amount > 0.0f) {
        ShapeFingers(true, finger_shape[1].value_or(kEvenFingers), amount);
    }
    if (const float amount = ease_reach ? left_grip * left_reach_ : left_grip; amount > 0.0f) {
        ShapeFingers(false, finger_shape[0].value_or(kEvenFingers), amount);
    }
    snap_now_ = false;
    if (help) {
        help->Apply(g_skeleton, world_);
    }
    if (run > 0.0f && run_laid_ > 0.0f && ease_reach) {
        const int waist = g_skeleton.FindName("SKL_000_WAIST");
        const struct { const char* name; float share; } kSeat[] = {
            {"SKL_518_LBUD_HLP", 0.85f}, {"SKL_524_RBUD_HLP", 0.85f}, {"SKL_519_LMRF_HLP", 0.45f}, {"SKL_525_RMRF_HLP", 0.45f}};
        for (const auto& seat : kSeat) {
            const int b = g_skeleton.FindName(seat.name);
            if (waist < 0 || b < 0 || static_cast<size_t>(std::max(b, waist)) >= world_.size()) continue;
            const glm::mat4& w = world_[static_cast<size_t>(waist)];
            const glm::vec3 place(w * glm::vec4(g_skeleton.bind_world[static_cast<size_t>(b)] - g_skeleton.bind_world[static_cast<size_t>(waist)], 1.0f));
            const float k = seat.share * std::min(1.0f, run_laid_ * 1.4f);
            glm::mat4& m = world_[static_cast<size_t>(b)];
            const glm::quat turned = glm::slerp(glm::normalize(glm::quat_cast(glm::mat3(m))), glm::normalize(glm::quat_cast(glm::mat3(w))), k);
            const glm::vec3 moved = glm::mix(glm::vec3(m[3]), place, k);
            m = glm::mat4_cast(turned);
            m[3] = glm::vec4(moved, 1.0f);
        }
    }
    const bool kinematic = run > 0.3f || ease_reach;
    if (kinematic || sim_seconds_ > 0.25f) sim_.Reset();
    sim_.SetDelay(1.0f / 29.97f);
    if (kinematic || !sim || !body_world || !sim_.Step(*sim, g_skeleton, world_, *body_world, sim_seconds_, anim::SimWind())) {
        sim_bones_.Apply(g_skeleton, world_, sim_seconds_);
    }
    sim_seconds_ = 0.0f;
    anim::ComputeSkin(g_skeleton, world_, skin);
    static const bool handy_bones = std::getenv("PT_HANDY_BONES") != nullptr;
    if (handy_bones && body_world) {
        LogHandBones(*body_world);
    }
    return true;
}

namespace {
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
}

void PlayerBody::LagHips(float lag) {
    const int waist = g_skeleton.FindName("SKL_000_WAIST"), spine = g_skeleton.FindName("SKL_001_SPINE");
    if (waist < 0 || spine < 0 || static_cast<size_t>(std::max(waist, spine)) >= world_.size()) return;
    const glm::vec3 pivot(world_[static_cast<size_t>(waist)][3]);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    TurnSubtree(waist, glm::angleAxis(-lag, up), pivot);
    TurnSubtree(spine, glm::angleAxis(lag, up), pivot);
    for (int side = 0; side < 2; ++side) {
        if (plant_.active[side]) ReachFoot(side, plant_.target[side], lag);
    }
}

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

void PlayerBody::ReachFoot(int side, const glm::vec3& target, float twist) {
    const int thigh = g_skeleton.FindName(side == 0 ? "SKL_030_LTHIGH" : "SKL_040_RTHIGH"), leg = g_skeleton.FindName(side == 0 ? "SKL_031_LLEG" : "SKL_041_RLEG"),
              foot = g_skeleton.FindName(side == 0 ? "SKL_032_LFOOT" : "SKL_042_RFOOT");
    const int n = static_cast<int>(world_.size());
    if (thigh < 0 || leg < 0 || foot < 0 || thigh >= n || leg >= n || foot >= n) return;
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 hip = at(thigh);
    const float upper = glm::distance(hip, at(leg)), lower = glm::distance(at(leg), at(foot));
    const float now = glm::distance(hip, at(foot));
    if (upper < 1.0e-4f || lower < 1.0e-4f || now < 1.0e-4f || glm::distance(hip, target) < 1.0e-4f) return;
    const glm::quat pointed = glm::normalize(glm::quat_cast(glm::mat3(world_[static_cast<size_t>(foot)])));
    const float want = std::clamp(glm::distance(hip, target), std::abs(upper - lower) + 0.01f, (upper + lower) * 0.9985f);
    const auto inner = [&](float length) { return std::acos(std::clamp((upper * upper + lower * lower - length * length) / (2.0f * upper * lower), -1.0f, 1.0f)); };
    const glm::vec3 hinge = glm::normalize(glm::vec3(world_[static_cast<size_t>(leg)][0]));
    glm::vec3 axis = glm::cross(at(leg) - hip, at(foot) - at(leg));
    {
        const float size = glm::length(axis) / std::max(upper * lower, 1.0e-6f);
        const float agree = size > 1.0e-6f ? glm::dot(axis / glm::length(axis), hinge) : 0.0f;
        const float plane = glm::smoothstep(0.1f, 0.57f, size) * glm::smoothstep(0.6f, 0.85f, agree);
        const glm::vec3 mixed = plane > 0.0f ? glm::mix(hinge, axis / glm::length(axis), plane) : hinge;
        axis = glm::length(mixed) > 1.0e-4f ? glm::normalize(mixed) : hinge;
    }
    TurnSubtree(leg, glm::angleAxis(inner(std::min(now, (upper + lower) * 0.99999f)) - inner(want), axis), at(leg));
    const glm::vec3 line = glm::normalize(target - hip);
    TurnSubtree(thigh, ArcBetween(glm::normalize(at(foot) - hip), line), hip);
    const glm::quat wanted = glm::normalize(glm::angleAxis(twist, glm::vec3(0.0f, 1.0f, 0.0f)) * pointed);
    const glm::quat turned = glm::normalize(glm::quat_cast(glm::mat3(world_[static_cast<size_t>(foot)])));
    TurnSubtree(foot, wanted * glm::conjugate(turned), at(foot));
}

bool PlayerBody::KneeToToe(int side, float& angle, float& toe_side_share, float* knee_out) const {
    const int thigh = g_skeleton.FindName(side == 0 ? "SKL_030_LTHIGH" : "SKL_040_RTHIGH"), leg = g_skeleton.FindName(side == 0 ? "SKL_031_LLEG" : "SKL_041_RLEG"),
              foot = g_skeleton.FindName(side == 0 ? "SKL_032_LFOOT" : "SKL_042_RFOOT"), toe = g_skeleton.FindName(side == 0 ? "SKL_033_LTOE" : "SKL_043_RTOE");
    const int n = static_cast<int>(world_.size());
    if (thigh < 0 || leg < 0 || foot < 0 || toe < 0 || thigh >= n || leg >= n || foot >= n || toe >= n) return false;
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 hip = at(thigh);
    if (glm::distance(hip, at(foot)) < 1.0e-3f) return false;
    const glm::vec3 line = glm::normalize(at(foot) - hip);
    const glm::vec3 ahead = at(toe) - at(foot);
    const glm::vec3 toe_side = ahead - line * glm::dot(ahead, line);
    const glm::vec3 knee_off = at(leg) - hip;
    const glm::vec3 knee_side = knee_off - line * glm::dot(knee_off, line);
    if (glm::length(ahead) < 1.0e-4f || glm::length(toe_side) < 1.0e-4f || glm::length(knee_side) < 1.0e-3f) return false;
    const glm::vec3 a = glm::normalize(knee_side), b = glm::normalize(toe_side);
    angle = std::atan2(glm::dot(glm::cross(a, b), line), glm::dot(a, b));
    toe_side_share = glm::length(toe_side) / glm::length(ahead);
    if (knee_out) *knee_out = glm::length(knee_side);
    return true;
}

void PlayerBody::AimKnee(int side) {
    float now = 0.0f, share = 0.0f;
    float& turn = knee_turn_[side & 1];
    if (!KneeToToe(side, now, share)) {
        turn = 0.0f;
        return;
    }
    const int thigh = g_skeleton.FindName(side == 0 ? "SKL_030_LTHIGH" : "SKL_040_RTHIGH"), foot = g_skeleton.FindName(side == 0 ? "SKL_032_LFOOT" : "SKL_042_RFOOT");
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 hip = at(thigh), line = glm::normalize(at(foot) - hip);
    const float angle = std::remainder(now - pose_knee_toe_[side & 1], 6.2831853f);
    const float keep = (1.0f - glm::smoothstep(1.745f, 2.443f, std::abs(angle))) * glm::smoothstep(0.3f, 0.6f, share) * glm::smoothstep(0.3f, 0.6f, pose_toe_side_[side & 1]) *
                       glm::smoothstep(0.012f, 0.035f, pose_knee_out_[side & 1]);
    const float wanted = angle * keep;
    turn = snap_now_ || sim_seconds_ > 0.25f ? wanted : turn + (wanted - turn) * (1.0f - std::exp(-sim_seconds_ * 25.0f));
    if (std::abs(turn) < 1.0e-4f) return;
    const glm::quat pointed = glm::normalize(glm::quat_cast(glm::mat3(world_[static_cast<size_t>(foot)])));
    TurnSubtree(thigh, glm::angleAxis(turn, line), hip);
    const glm::quat turned = glm::normalize(glm::quat_cast(glm::mat3(world_[static_cast<size_t>(foot)])));
    TurnSubtree(foot, pointed * glm::conjugate(turned), at(foot));
}

void PlayerBody::HoldFeet(const glm::mat4& body_world) {
    constexpr float kLow = 0.135f;
    constexpr float kStance = 1.3f;
    constexpr float kSwing = 1.7f;
    constexpr float kSkimHeight = 0.035f;
    constexpr float kLeash = 0.12f;
    constexpr float kWide = 0.21f;
    constexpr float kNarrow = 0.03f;
    constexpr float kSlip = 2.5f;
    constexpr float kTwist = 0.9599311f;
    constexpr float kLiftSeconds = 0.13f;
    constexpr float kLiftHeight = 0.05f;
    const int ankles[2] = {g_skeleton.FindName("SKL_032_LFOOT"), g_skeleton.FindName("SKL_042_RFOOT")};
    const int waist = g_skeleton.FindName("SKL_000_WAIST");
    const float dt = sim_seconds_;
    const glm::vec3 body(body_world[3]);
    const float body_yaw = std::atan2(body_world[2].x, body_world[2].z) - hips_lag;
    const glm::mat4 to_model = glm::inverse(body_world);
    const bool fresh = dt > 0.25f;
    if (dt > 1.0e-5f && !fresh) {
        const glm::vec2 step(body.x - held_body_.x, body.z - held_body_.z);
        if (glm::length(step) > 0.3f * dt) held_way_ = glm::normalize(step);
    }
    const bool going_now = dt > 1.0e-5f && glm::length(glm::vec2(body.x - held_body_.x, body.z - held_body_.z)) > 0.5f * dt;
    const bool walking_aside = stride_on_ && std::abs(stride_.x) > std::abs(stride_.y);
    for (int side = 0; side < 2; ++side) {
        HeldFoot& foot = held_[side];
        if (ankles[side] < 0 || static_cast<size_t>(ankles[side]) >= world_.size()) {
            foot = HeldFoot{};
            continue;
        }
        const glm::vec3 ankle(world_[static_cast<size_t>(ankles[side])][3]);
        const glm::vec3 there(body_world * glm::vec4(ankle, 1.0f));
        if (fresh || !foot.primed || dt <= 1.0e-5f) {
            const bool keep = foot.primed && !fresh;
            if (!keep) foot = HeldFoot{};
            foot.before = ankle;
            if (!keep) foot.shown = there;
            foot.primed = true;
            if (!keep) continue;
        }
        float over = 0.0f;
        if (dt > 1.0e-5f) {
            const glm::vec3 going = glm::mat3(body_world) * ((ankle - foot.before) / dt) + (body - held_body_) / dt;
            const float with = glm::dot(glm::vec2(going.x, going.z), held_way_);
            const glm::vec2 across = glm::vec2(going.x, going.z) - held_way_ * with;
            over = glm::length(glm::vec2(with > 0.0f ? with : std::max(0.0f, -with - kSlip), glm::length(across)));
            foot.before = ankle;
        }
        const bool low = there.y - body.y < kLow;
        const int hold = strafe_weight > 0.5f && g_strafe.frames > 0 ? 0 : hold_feet[side];
        const glm::vec2 own_side(std::cos(body_yaw), -std::sin(body_yaw));
        const float outward = side == 0 ? 1.0f : -1.0f;
        const auto side_fixed = [&] { return glm::vec3(own_side.x, 0.0f, own_side.y) * (outward * foot.side_fix); };
        const float quick = dt > 1.0e-5f ? std::max(0.0f, 0.9f * glm::length(glm::vec2(body.x - held_body_.x, body.z - held_body_.z)) / dt - kStance) : 0.0f;
        const bool stance = hold == 2 && low && over < (foot.held ? kSwing : kStance) + quick;
        if (dt > 1.0e-5f) foot.lift = std::max(0.0f, foot.lift - dt);
        if (plant_.active[side]) {
            const glm::vec3 asked(body_world * glm::vec4(plant_.target[side], 1.0f));
            foot.held = false;
            foot.planted = true;
            foot.side_fix = 0.0f;
            foot.twist = hips_lag;
            foot.off = glm::vec3(asked.x - there.x, 0.0f, asked.z - there.z);
            if (const float length = glm::length(foot.off); length > 0.15f) foot.off *= 0.15f / length;
            foot.shown_step = there + foot.off - foot.shown;
            foot.shown = there + foot.off;
            float rising = 0.0f;
            if (foot.lift > 0.0f) {
                const float arc = std::sin(3.14159265f * foot.lift / kLiftSeconds);
                rising = foot.lift_height * arc * arc;
            }
            if (glm::length(foot.off) > 1.0e-4f || rising > 0.0f) ReachFoot(side, glm::vec3(to_model * glm::vec4(foot.shown + glm::vec3(0.0f, rising, 0.0f), 1.0f)), 0.0f);
            continue;
        }
        if (foot.planted) {
            foot.planted = false;
            glm::vec3 from(foot.shown.x + foot.shown_step.x - there.x, 0.0f, foot.shown.z + foot.shown_step.z - there.z);
            if (const float length = glm::length(from); length > 0.2f) from *= 0.2f / length;
            foot.off = from;
            foot.lift = 0.0f;
        }
        if (!low) foot.wait_swing = false;
        if (foot.held && !stance) {
            foot.held = false;
            foot.lift = kLiftSeconds;
            foot.lift_height = (walking_aside ? 0.3f : 1.0f) * kLiftHeight * std::clamp((glm::length(foot.off) - 0.02f) / 0.1f, 0.0f, 1.0f);
            foot.back_from = foot.off;
            foot.back_twist = foot.twist;
        } else if (!foot.held && stance && foot.lift <= 0.0f && !foot.wait_swing) {
            foot.held = true;
            foot.floor = there + foot.off + side_fixed();
            foot.side_fix = 0.0f;
            foot.yaw = body_yaw + foot.twist;
        }
        if (foot.held) {
            foot.off = glm::vec3(foot.floor.x - there.x, 0.0f, foot.floor.z - there.z);
            bool taut = false;
            const bool aside_low = low && walking_aside;
            if (const float leash = aside_low ? 0.18f : kLeash, length = glm::length(foot.off); length > leash) {
                foot.off *= leash / length;
                foot.floor = there + foot.off;
                taut = hold == 2 && !aside_low;
            }
            if (hold == 1) foot.yaw = body_yaw;
            foot.twist = std::remainder(foot.yaw - body_yaw, 6.2831853f);
            float across = 0.0f;
            if (waist >= 0 && static_cast<size_t>(waist) < world_.size()) {
                const glm::vec3 middle(body_world * world_[static_cast<size_t>(waist)][3]);
                const glm::vec2 own_side(std::cos(body_yaw), -std::sin(body_yaw));
                across = (side == 0 ? -1.0f : 1.0f) * ((foot.floor.x - middle.x) * own_side.x + (foot.floor.z - middle.z) * own_side.y);
            }
            if (taut || std::abs(foot.twist) > kTwist || (hold == 2 && !aside_low && (across > 0.03f || -across > kWide + 0.03f))) {
                foot.wait_swing = hold == 2;
                foot.held = false;
                foot.lift = kLiftSeconds;
                foot.lift_height = kLiftHeight;
                foot.back_from = foot.off;
                foot.back_twist = foot.twist;
            }
        }
        if (!foot.held && dt > 1.0e-5f) {
            const float keep = std::exp(-dt / 0.04f);
            const float way = std::clamp(foot.lift / kLiftSeconds, 0.0f, 1.0f);
            const float left = way * way * (3.0f - 2.0f * way);
            const bool timed = foot.lift > 0.0f;
            glm::vec3 off = timed ? foot.back_from * left : foot.off * keep;
            const glm::vec3 moved = there + off + side_fixed() - foot.shown;
            if (const float back = moved.x * held_way_.x + moved.z * held_way_.y; hold == 2 && going_now && back < 0.0f && glm::length(foot.off) > 1.0e-4f) {
                off -= glm::vec3(held_way_.x, 0.0f, held_way_.y) * back;
                if (const float length = glm::length(off); length > kLeash) off *= kLeash / length;
                if (timed && left > 0.1f) foot.back_from = off / left;
            }
            foot.off = off;
            foot.twist = timed ? foot.back_twist * left : foot.twist * keep;
            if (glm::length(foot.off) < 5.0e-4f) foot.off = glm::vec3(0.0f);
            if (std::abs(foot.twist) < 2.0e-3f) foot.twist = 0.0f;
        }
        if (!foot.held && dt > 1.0e-5f) {
            float wanted = 0.0f;
            if (hold == 2 && waist >= 0 && static_cast<size_t>(waist) < world_.size()) {
                const glm::vec3 middle(body_world * world_[static_cast<size_t>(waist)][3]);
                const glm::vec3 placed = there + foot.off;
                const float out = outward * ((placed.x - middle.x) * own_side.x + (placed.z - middle.z) * own_side.y);
                wanted = std::clamp(out, kNarrow, kWide) - out;
            }
            foot.side_fix += (wanted - foot.side_fix) * (1.0f - std::exp(-dt * (hold == 2 ? (walking_aside ? 12.0f : 60.0f) : 10.0f)));
            if (std::abs(foot.side_fix) < 5.0e-4f && wanted == 0.0f) foot.side_fix = 0.0f;
        } else if (foot.held) {
            foot.side_fix = 0.0f;
        }
        if (dt > 1.0e-5f) {
            foot.skim_time = hold == 2 && low && !foot.held && over > kStance ? foot.skim_time + dt : 0.0f;
            const float wanted = std::clamp((foot.skim_time - 0.035f) / 0.05f, 0.0f, 1.0f);
            foot.skim += (wanted - foot.skim) * (1.0f - std::exp(-dt * (wanted > foot.skim ? 30.0f : 14.0f)));
            if (foot.skim < 1.0e-3f && wanted <= 0.0f) foot.skim = 0.0f;
        }
        float lifted = kSkimHeight * foot.skim;
        if (foot.lift > 0.0f) {
            const float arc = std::sin(3.14159265f * foot.lift / kLiftSeconds);
            lifted = std::max(lifted, foot.lift_height * arc * arc);
        }
        foot.shown_step = there + foot.off + side_fixed() - foot.shown;
        foot.shown = there + foot.off + side_fixed();
        if (glm::length(foot.off) > 1.0e-5f || std::abs(foot.side_fix) > 1.0e-5f || std::abs(foot.twist) > 1.0e-4f || lifted > 0.0f) {
            ReachFoot(side, glm::vec3(to_model * glm::vec4(foot.shown + glm::vec3(0.0f, lifted, 0.0f), 1.0f)), foot.twist);
        }
    }
    held_body_ = body;
}

void PlayerBody::StrafeOverlay() {
    const float w = std::clamp(strafe_weight, 0.0f, 1.0f);
    const size_t side = strafe_left ? 1 : 0;
    const float at = (strafe_phase - std::floor(strafe_phase)) * static_cast<float>(g_strafe.frames);
    const int f0 = static_cast<int>(at) % g_strafe.frames, f1 = (f0 + 1) % g_strafe.frames;
    const float t = at - std::floor(at);
    const auto clip_bone = [&](size_t b) {
        const size_t i0 = static_cast<size_t>(f0) * StrafeClip::kBones + b, i1 = static_cast<size_t>(f1) * StrafeClip::kBones + b;
        glm::mat4 m = glm::mat4_cast(glm::slerp(g_strafe.turn[side][i0], g_strafe.turn[side][i1], t));
        m[3] = glm::vec4(glm::mix(g_strafe.place[side][i0], g_strafe.place[side][i1], t), 1.0f);
        return m;
    };
    const auto mixed = [&](const glm::mat4& from, const glm::mat4& to) {
        glm::mat4 m = glm::mat4_cast(glm::slerp(glm::normalize(glm::quat_cast(glm::mat3(from))), glm::normalize(glm::quat_cast(glm::mat3(to))), w));
        m[3] = glm::vec4(glm::mix(glm::vec3(from[3]), glm::vec3(to[3]), w), 1.0f);
        return m;
    };
    const size_t waist = static_cast<size_t>(g_strafe.bone[0]);
    if (waist >= world_.size()) return;
    const glm::vec3 shift = (glm::vec3(clip_bone(0)[3]) - glm::vec3(world_[waist][3])) * w;
    for (glm::mat4& m : world_) m[3] += glm::vec4(shift, 0.0f);
    for (size_t b = 1; b < StrafeClip::kBones; ++b) {
        const size_t bone = static_cast<size_t>(g_strafe.bone[b]);
        if (bone < world_.size()) world_[bone] = mixed(world_[bone], clip_bone(b));
    }
}

void PlayerBody::RunOverlay(float amount) {
    run_laid_ = 0.0f;
    const BodyClip* clip = Current();
    const auto bone = [](const char* name) { return g_skeleton.FindName(name); };
    const int n = static_cast<int>(world_.size());
    const auto ok = [&](int b) { return b >= 0 && b < n; };
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const int waist = bone("SKL_000_WAIST"), spine = bone("SKL_001_SPINE"), chest = bone("SKL_002_CHEST"), neck = bone("SKL_003_NECK"),
              head = bone("SKL_004_HEAD");
    const int lthigh = bone("SKL_030_LTHIGH"), rthigh = bone("SKL_040_RTHIGH");
    const int lknee = bone("SKL_031_LLEG"), lankle = bone("SKL_032_LFOOT");
    if (!clip || clip->frames <= 0.0f || !ok(waist) || !ok(spine) || !ok(chest) || !ok(neck) || !ok(head) || !ok(lthigh) || !ok(rthigh) ||
        !ok(lknee) || !ok(lankle)) {
        return;
    }
    const bool walking = kind_ == Kind::Node && clip_ != kClipStand;
    const bool striding = walking || (kind_ == Kind::Start && clip_ != kClipStand);
    Follow(run_node_, run_node_speed_, striding ? 1.0f : 0.0f, 10.0f, sim_seconds_);
    if (snap_now_) {
        run_node_ = striding ? 1.0f : 0.0f;
        run_node_speed_ = 0.0f;
    }
    {
        const float dt = sim_seconds_;
        if (run_node_ <= 0.0f) {
            run_primed_ = false;
            run_frame_ = -1.0f;
        }
        if (walking) {
            const float walk_frame = stride_on_ ? StrideFrame(g_locomotion.clips[0], StridePhase()) : frame_;
            float wanted = std::fmod((walk_frame - kWalkLeftReach) / clip->frames, 1.0f);
            if (wanted < 0.0f) wanted += 1.0f;
            float step = run_rate_ * dt;
            if (run_frame_ >= 0.0f) {
                float frames = walk_frame - run_frame_;
                if (frames < 0.0f) frames += clip->frames;
                step = frames / clip->frames;
                if (dt > 1.0e-5f) run_rate_ = std::min(step / dt, 4.0f);
            }
            run_frame_ = walk_frame;
            if (!run_primed_) {
                run_phase_ = wanted;
                run_primed_ = true;
            } else {
                const float off = std::remainder(wanted - run_phase_ - step, 1.0f);
                run_phase_ += step + std::clamp(off * (1.0f - std::exp(-dt * 7.0f)), -0.33f * step, 0.33f * step);
            }
        } else {
            run_frame_ = -1.0f;
            if (run_primed_) run_phase_ += run_rate_ * dt;
        }
        run_phase_ -= std::floor(run_phase_);
    }
    amount *= run_node_;
    if (stride_on_) {
        const float ahead = std::clamp((stride_.y - 0.05f) / 0.6f, 0.0f, 1.0f);
        amount *= ahead * ahead * (3.0f - 2.0f * ahead);
    }
    if (amount <= 0.0f) {
        return;
    }
    run_laid_ = amount;
    constexpr float kAnkleHeight = 0.045f;
    const float stand = at(waist).y - 0.5f * (at(lthigh).y + at(rthigh).y) + glm::distance(at(lthigh), at(lknee)) +
                        glm::distance(at(lknee), at(lankle)) + kAnkleHeight;
    static const bool trace = std::getenv("PT_RUN_TRACE") != nullptr;
    if (trace) {
        const int knee = bone("SKL_031_LLEG");
        const glm::vec3 d = ok(knee) ? glm::normalize(at(knee) - at(lthigh)) : glm::vec3(0.0f);
        LogInfo("run: the walk at frame {:.2f}, its left thigh forward {:.3f}", frame_, d.z);
    }
    const float phase = run_phase_;
    const float place = phase * static_cast<float>(run_cycle::kSamples);
    const int i0 = static_cast<int>(place) % run_cycle::kSamples;
    const int i1 = (i0 + 1) % run_cycle::kSamples;
    const float f = place - std::floor(place);
    const auto value = [&](int c) { return glm::mix(run_cycle::kCycle[i0][c], run_cycle::kCycle[i1][c], f); };
    const glm::vec3 forward(0.0f, 0.0f, 1.0f), up(0.0f, 1.0f, 0.0f);
    const glm::vec3 left(at(lthigh).x > at(rthigh).x ? 1.0f : -1.0f, 0.0f, 0.0f);
    constexpr float kSwing[run_cycle::kSegments][2] = {{-0.04f, 0.0f}, {-0.06f, 0.0f}, {1.04f, 1.04f}, {1.0f, 1.04f}, {1.0f, 1.0f},
                                                       {1.04f, 1.04f}, {1.0f, 1.04f}, {1.0f, 1.0f}, {1.04f, 1.04f}, {0.0f, 0.0f},
                                                       {1.04f, 1.04f}, {0.0f, 0.0f}};
    const auto raw = [&](int segment) { return glm::vec3(value(segment * 3), value(segment * 3 + 1), value(segment * 3 + 2)); };
    const auto swing_of = [](const glm::vec3& d) { return std::atan2(d.z, -d.y); };
    const auto opened_of = [&](int segment, float swing) {
        return std::clamp(swing * kSwing[segment][swing > 0.0f ? 0 : 1], -2.6f, 2.6f);
    };
    const auto direction = [&](int segment) {
        glm::vec3 d = raw(segment);
        const float sagittal = std::sqrt(d.y * d.y + d.z * d.z);
        float angle = 0.0f;
        if (segment <= 1) {
            const float lean = std::atan2(d.z, d.y) + kSwing[segment][0];
            d.y = sagittal * std::cos(lean);
            d.z = sagittal * std::sin(lean);
            return glm::normalize(left * d.x + up * d.y + forward * d.z);
        }
        if (segment == 9 || segment == 11) {
            const float upper = swing_of(raw(segment - 1));
            angle = swing_of(d) + opened_of(segment - 1, upper) - upper;
        } else {
            angle = opened_of(segment, swing_of(d));
        }
        d.y = -sagittal * std::cos(angle);
        d.z = sagittal * std::sin(angle);
        return glm::normalize(left * d.x + up * d.y + forward * d.z);
    };
    const float arms = std::clamp(run_arms, 0.0f, 1.0f);
    const auto aim = [&](int from, int to, const glm::vec3& want, float share = 1.0f) {
        if (!ok(from) || !ok(to) || share <= 0.0f) return;
        const glm::vec3 now = at(to) - at(from);
        if (glm::length(now) < 1.0e-5f) return;
        const glm::vec3 u = glm::normalize(now);
        const glm::vec3 v = glm::normalize(glm::mix(u, want, amount * share));
        TurnSubtree(from, ArcBetween(u, v), at(from));
    };
    constexpr int kDirections = run_cycle::kSegments * 3;
    constexpr float kBounce = 0.60f;
    static constexpr float kHipsLow = [] {
        float low = run_cycle::kCycle[0][kDirections];
        for (int i = 1; i < run_cycle::kSamples; ++i) low = low < run_cycle::kCycle[i][kDirections] ? low : run_cycle::kCycle[i][kDirections];
        return low;
    }();
    const float hips = (kHipsLow + 0.022f) + (value(kDirections) - kHipsLow) * kBounce;
    const float sway_left = value(kDirections + 1), sway_forward = value(kDirections + 2);
    const float pelvis_turn = value(kDirections + 3), pelvis_drop = value(kDirections + 4), shoulder_turn = value(kDirections + 5);
    const glm::vec3 turn_axis = glm::normalize(glm::cross(left, forward));
    const glm::vec3 drop_axis = glm::normalize(glm::cross(left, up));
    TurnSubtree(waist, glm::angleAxis(pelvis_turn * amount, turn_axis) * glm::angleAxis(pelvis_drop * amount, drop_axis), at(waist));
    aim(spine, neck, direction(0));
    TurnSubtree(chest, glm::angleAxis((shoulder_turn * arms - pelvis_turn) * amount, turn_axis), at(chest));
    aim(neck, head, direction(1));
    const char* kLegs[2][4] = {{"SKL_030_LTHIGH", "SKL_031_LLEG", "SKL_032_LFOOT", "SKL_033_LTOE"},
                               {"SKL_040_RTHIGH", "SKL_041_RLEG", "SKL_042_RFOOT", "SKL_043_RTOE"}};
    for (int side = 0; side < 2; ++side) {
        const int thigh = bone(kLegs[side][0]), leg = bone(kLegs[side][1]), foot = bone(kLegs[side][2]), toe = bone(kLegs[side][3]);
        aim(thigh, leg, direction(2 + side * 3));
        aim(leg, foot, direction(3 + side * 3));
        aim(foot, toe, direction(4 + side * 3));
    }
    const char* kArms[2][3] = {{"SKL_011_LUARM", "SKL_012_LFARM", "SKL_013_LHAND"}, {"SKL_021_RUARM", "SKL_022_RFARM", "SKL_023_RHAND"}};
    for (int side = 0; side < 2; ++side) {
        const int upper = bone(kArms[side][0]), fore = bone(kArms[side][1]), hand = bone(kArms[side][2]);
        aim(upper, fore, direction(8 + side * 2), arms);
        aim(fore, hand, direction(9 + side * 2), arms);
    }
    const glm::vec3 target = glm::vec3(at(waist).x, hips * stand, at(waist).z) + (left * sway_left + forward * sway_forward) * stand;
    const glm::mat4 move = glm::translate(glm::mat4(1.0f), (target - at(waist)) * amount);
    for (glm::mat4& m : world_) m = move * m;
    if (trace) {
        LogInfo("run: amount {:.3f}, frame {:.2f} of {:.0f}, phase {:.3f}, hips {:.3f} m, feet {:.3f} {:.3f} m", amount, frame_, clip->frames, phase, at(waist).y,
                ok(bone("SKL_032_LFOOT")) ? at(bone("SKL_032_LFOOT")).y : 0.0f, ok(bone("SKL_042_RFOOT")) ? at(bone("SKL_042_RFOOT")).y : 0.0f);
        std::string joints;
        for (const char* name : {"SKL_000_WAIST", "SKL_002_CHEST", "SKL_003_NECK", "SKL_004_HEAD", "SKL_030_LTHIGH", "SKL_031_LLEG", "SKL_032_LFOOT",
                                 "SKL_033_LTOE", "SKL_040_RTHIGH", "SKL_041_RLEG", "SKL_042_RFOOT", "SKL_043_RTOE", "SKL_011_LUARM", "SKL_012_LFARM",
                                 "SKL_013_LHAND", "SKL_021_RUARM", "SKL_022_RFARM", "SKL_023_RHAND"}) {
            const int b = bone(name);
            const glm::vec3 q = ok(b) ? at(b) : glm::vec3(0.0f);
            joints += std::format(" {:.3f},{:.3f},{:.3f}", q.x, q.y, q.z);
        }
        LogInfo("run joints:{}", joints);
    }
}

void PlayerBody::IdleOverlay() {
    const int waist = g_skeleton.FindName("SKL_000_WAIST"), spine = g_skeleton.FindName("SKL_001_SPINE"),
              chest = g_skeleton.FindName("SKL_002_CHEST"), neck = g_skeleton.FindName("SKL_003_NECK"),
              head = g_skeleton.FindName("SKL_004_HEAD");
    const int n = static_cast<int>(world_.size());
    if (waist < 0 || spine < 0 || chest < 0 || neck < 0 || head < 0 || waist >= n || spine >= n || chest >= n || neck >= n || head >= n) {
        return;
    }
    const float dt = std::min(sim_seconds_, 0.05f);
    idle_time_ += dt;
    const bool standing_idle = (kind_ == Kind::Node && clip_ == kClipStand) && run_laid_ < 0.05f && aim < 0.1f && melee == glm::vec3(0.0f);
    const float target_idle = standing_idle ? 1.0f : 0.0f;
    idle_weight_ = snap_now_ || sim_seconds_ > 0.25f ? target_idle : idle_weight_ + (target_idle - idle_weight_) * (1.0f - std::exp(-dt * 6.0f));
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 across(1.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f), ahead(0.0f, 0.0f, 1.0f);

    if (idle_weight_ > 1.0e-3f) {
        const float shift = std::sin(idle_time_ * 0.55f);
        const glm::vec3 settle(0.014f * shift * idle_weight_, -0.018f * idle_weight_, 0.0f);
        for (size_t i = 0; i < world_.size() && i < g_skeleton.parents.size(); ++i) {
            int b = static_cast<int>(i);
            bool in_leg = false;
            while (b >= 0) {
                if (b == waist) break;
                const std::string& nm = g_skeleton.names[static_cast<size_t>(b)];
                if (nm == "SKL_032_LFOOT" || nm == "SKL_042_RFOOT" || nm == "SKL_033_LTOE" || nm == "SKL_043_RTOE") in_leg = true;
                b = g_skeleton.parents[static_cast<size_t>(b)];
            }
            if (!in_leg) world_[i][3] += glm::vec4(settle, 0.0f);
        }
        const float roll = 0.018f * shift * idle_weight_;
        TurnSubtree(waist, glm::angleAxis(roll, ahead), at(waist));
        TurnSubtree(spine, glm::angleAxis(-0.6f * roll, ahead), at(spine));
    }

    const float breath_share = 1.0f - 0.7f * std::clamp(run_laid_, 0.0f, 1.0f);
    const float breath = std::sin(idle_time_ * 1.75f) * breath_share;
    TurnSubtree(chest, glm::angleAxis(-0.014f * breath, across) * glm::angleAxis(0.009f * std::sin(idle_time_ * 0.62f) * idle_weight_, up), at(chest));
    TurnSubtree(neck, glm::angleAxis(0.010f * breath, across), at(neck));
}

void PlayerBody::IdleHeadOverlay() {
    const int neck = g_skeleton.FindName("SKL_003_NECK"), head = g_skeleton.FindName("SKL_004_HEAD");
    const int n = static_cast<int>(world_.size());
    if (neck < 0 || head < 0 || neck >= n || head >= n) return;
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 across(1.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f);
    const float dt = std::min(sim_seconds_, 0.05f);
    const float free_head = std::clamp(1.0f - aim, 0.0f, 1.0f);
    const float target_ly = std::clamp(look_yaw, -0.42f, 0.42f) * free_head;
    const float target_lp = std::clamp(look_pitch, -0.24f, 0.28f) * free_head;
    const float alpha = snap_now_ || sim_seconds_ > 0.25f ? 1.0f : (1.0f - std::exp(-dt * 10.0f));
    look_yaw_now_ += (target_ly - look_yaw_now_) * alpha;
    look_pitch_now_ += (target_lp - look_pitch_now_) * alpha;
    const float scan_yaw = look_yaw_now_ + 0.024f * std::sin(idle_time_ * 0.41f) * idle_weight_ * free_head;
    if (std::abs(scan_yaw) > 1.0e-4f || std::abs(look_pitch_now_) > 1.0e-4f) {
        TurnSubtree(neck, glm::angleAxis(scan_yaw * 0.42f, up) * glm::angleAxis(-look_pitch_now_ * 0.42f, across), at(neck));
        TurnSubtree(head, glm::angleAxis(scan_yaw * 0.58f, up) * glm::angleAxis(-look_pitch_now_ * 0.58f, across), at(head));
    }
}

void PlayerBody::AimOverlay(float amount, float pitch) {
    const int spine = g_skeleton.FindName("SKL_001_SPINE"), chest = g_skeleton.FindName("SKL_002_CHEST"), neck = g_skeleton.FindName("SKL_003_NECK");
    const int head = g_skeleton.FindName("SKL_004_HEAD"), right = g_skeleton.FindName("SKL_020_RSHLD"), left = g_skeleton.FindName("SKL_010_LSHLD");
    const int n = static_cast<int>(world_.size());
    if (spine < 0 || neck < 0 || spine >= n || neck >= n || chest >= n || head >= n || right >= n || left >= n) {
        return;
    }
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 across(1.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f), ahead(0.0f, 0.0f, 1.0f);
    pitch = std::clamp(pitch, -1.2f, 1.2f);
    const float target_yaw = std::clamp(aim_yaw, -0.65f, 0.65f);
    const float dt = std::min(sim_seconds_, 0.05f);
    aim_yaw_now_ = snap_now_ || sim_seconds_ > 0.25f ? target_yaw : aim_yaw_now_ + (target_yaw - aim_yaw_now_) * (1.0f - std::exp(-dt * 24.0f));
    const float yaw_lead = aim_yaw_now_ * amount;
    const float long_gun = std::clamp(aim_long, 0.0f, 1.0f);
    const float torso = (0.16f - 0.03f * long_gun - 0.5f * pitch - aim_recoil) * amount;
    const float blade = -(0.12f + 0.68f * long_gun) * amount;
    TurnSubtree(spine, glm::angleAxis(blade * 0.45f + yaw_lead * 0.30f, up) * glm::angleAxis(torso * 0.6f, across), at(spine));
    if (chest >= 0) TurnSubtree(chest, glm::angleAxis(blade * 0.55f + yaw_lead * 0.50f, up) * glm::angleAxis(torso * 0.4f, across), at(chest));
    TurnSubtree(neck, glm::angleAxis(-blade + yaw_lead * 0.20f, up) * glm::angleAxis((0.14f - 0.04f * long_gun - pitch) * amount - torso, across), at(neck));
    if (head >= 0) TurnSubtree(head, glm::angleAxis(0.04f * long_gun * amount, ahead), at(head));
    if (right >= 0) {
        TurnSubtree(right, glm::angleAxis((0.12f - 0.02f * long_gun) * amount, up) * glm::angleAxis(-0.07f * long_gun * amount, ahead), at(right));
    }
    if (left >= 0) TurnSubtree(left, glm::angleAxis(-(0.14f + 0.06f * long_gun) * amount, up), at(left));
}

void PlayerBody::SteadyTorso(float amount) {
    const int spine = g_skeleton.FindName("SKL_001_SPINE"), chest = g_skeleton.FindName("SKL_002_CHEST"), neck = g_skeleton.FindName("SKL_003_NECK");
    const int n = static_cast<int>(world_.size());
    if (spine < 0 || chest < 0 || neck < 0 || spine >= n || chest >= n || neck >= n) {
        return;
    }
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    amount = std::clamp(amount, 0.0f, 1.0f);
    const glm::mat4& m = world_[static_cast<size_t>(chest)];
    const glm::vec3 own_up = glm::normalize(glm::vec3(m[1]));
    const glm::vec3 across = glm::normalize(glm::vec3(m[0]));
    const glm::vec3 other = glm::cross(own_up, across);
    if (std::abs(across.z) + std::abs(other.z) > 1.0e-4f) {
        float back = std::atan2(-across.z, other.z);
        if (across.x * std::cos(back) + other.x * std::sin(back) < 0.0f) back += back > 0.0f ? -3.14159265f : 3.14159265f;
        back *= amount;
        TurnSubtree(spine, glm::angleAxis(back * 0.5f, own_up), at(spine));
        TurnSubtree(chest, glm::angleAxis(back * 0.5f, own_up), at(chest));
        TurnSubtree(neck, glm::angleAxis(-back, own_up), at(neck));
    }
    const float height = at(chest).y;
    if (!steady_primed_) {
        steady_height_ = height;
        steady_primed_ = true;
    }
    steady_height_ += (height - steady_height_) * (1.0f - std::exp(-sim_seconds_ * 2.0f));
    const glm::vec3 lift(0.0f, std::clamp((steady_height_ - height) * amount * 0.85f, -0.04f, 0.04f), 0.0f);
    for (size_t i = 0; i < world_.size() && i < g_skeleton.parents.size(); ++i) {
        int from = static_cast<int>(i);
        while (from >= 0 && from != spine) from = g_skeleton.parents[static_cast<size_t>(from)];
        if (from == spine) world_[i][3] += glm::vec4(lift, 0.0f);
    }
}

void PlayerBody::CarryOverlay(const glm::vec2& stance) {
    const int spine = g_skeleton.FindName("SKL_001_SPINE"), chest = g_skeleton.FindName("SKL_002_CHEST"), neck = g_skeleton.FindName("SKL_003_NECK");
    const int n = static_cast<int>(world_.size());
    if (spine < 0 || chest < 0 || neck < 0 || spine >= n || chest >= n || neck >= n) {
        return;
    }
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 across(1.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f);
    const float blade = -stance.x, lean = stance.y;
    TurnSubtree(spine, glm::angleAxis(blade * 0.45f, up) * glm::angleAxis(lean * 0.6f, across), at(spine));
    TurnSubtree(chest, glm::angleAxis(blade * 0.55f, up) * glm::angleAxis(lean * 0.4f, across), at(chest));
    TurnSubtree(neck, glm::angleAxis(-blade, up) * glm::angleAxis(-lean * 0.7f, across), at(neck));
}

void PlayerBody::MeleeOverlay(const glm::vec3& lean) {
    const int spine = g_skeleton.FindName("SKL_001_SPINE"), chest = g_skeleton.FindName("SKL_002_CHEST"), neck = g_skeleton.FindName("SKL_003_NECK");
    const int n = static_cast<int>(world_.size());
    if (spine < 0 || neck < 0 || spine >= n || neck >= n || chest >= n) {
        return;
    }
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 across(1.0f, 0.0f, 0.0f), up(0.0f, 1.0f, 0.0f), ahead(0.0f, 0.0f, 1.0f);
    auto turn = [&](float share) {
        return glm::angleAxis(lean.y * share, up) * glm::angleAxis(lean.x * share, across) * glm::angleAxis(lean.z * share, ahead);
    };
    TurnSubtree(spine, turn(0.4f), at(spine));
    if (chest >= 0) TurnSubtree(chest, turn(0.6f), at(chest));
    TurnSubtree(neck, glm::angleAxis(-0.8f * lean.y, up) * glm::angleAxis(-0.5f * lean.x, across) * glm::angleAxis(-lean.z, ahead), at(neck));
}

void PlayerBody::ShapeFingers(bool right, const std::array<glm::vec4, 5>& shape, float amount) {
    const char side = right ? 'R' : 'L';
    const float sign = right ? 1.0f : -1.0f;
    for (int joint = 0; joint < 3; ++joint) {
        for (size_t b = 0; b < g_skeleton.Size() && b < world_.size(); ++b) {
            const std::string& name = g_skeleton.names[b];
            if (name.size() < 12 || name.compare(0, 4, "SKL_") != 0 || name[8] != side || name[9] != 'F') continue;
            const int finger = name[10] - '1';
            if (finger < 0 || finger > 4 || name[11] != static_cast<char>((finger == 0 ? '0' : '1') + joint)) continue;
            const glm::vec4& s = shape[static_cast<size_t>(finger)];
            const glm::mat4& w = world_[b];
            const glm::vec3 across = glm::normalize(glm::vec3(w * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
            glm::quat turn = glm::angleAxis(sign * s[joint + 1] * amount, across);
            if (joint == 0 && s.x != 0.0f) {
                turn = glm::angleAxis(sign * s.x * amount, glm::normalize(glm::vec3(w * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)))) * turn;
            }
            TurnSubtree(static_cast<int>(b), turn, glm::vec3(w[3]));
        }
    }
}

glm::vec3 PlayerBody::TorsoPush(const glm::vec3& point, float margin, float hem, float hem_side) const {
    static const int waist = g_skeleton.FindName("SKL_000_WAIST"), chest = g_skeleton.FindName("SKL_002_CHEST"), neck = g_skeleton.FindName("SKL_003_NECK");
    const int n = static_cast<int>(world_.size());
    if (waist < 0 || chest < 0 || neck < 0 || waist >= n || chest >= n || neck >= n) return glm::vec3(0.0f);
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 bottom = at(waist);
    const glm::vec3 line = at(neck) - bottom;
    const float length = glm::length(line);
    if (length < 1.0e-3f) return glm::vec3(0.0f);
    const glm::vec3 up = line / length;
    const float along = glm::dot(point - bottom, up);
    const float height = std::clamp((along + 0.17f) / 0.04f, 0.0f, 1.0f) * std::clamp((length - 0.04f - along) / 0.06f, 0.0f, 1.0f);
    if (height <= 0.0f) return glm::vec3(0.0f);
    const float k = std::clamp(along / length, 0.0f, 1.0f);
    const auto axis_of = [&](int b, int c) { return glm::vec3(world_[static_cast<size_t>(b)][c]); };
    glm::vec3 across = glm::mix(axis_of(waist, 0), axis_of(chest, 0), std::min(1.0f, k * 2.0f));
    across -= up * glm::dot(across, up);
    if (glm::length(across) < 1.0e-3f) return glm::vec3(0.0f);
    across = glm::normalize(across);
    const glm::vec3 ahead = glm::cross(across, up);
    const glm::vec3 off = point - (bottom + up * along);
    const float sign = glm::dot(ahead, axis_of(chest, 2)) >= 0.0f ? 1.0f : -1.0f;
    const float x = glm::dot(off, across);
    const float z = glm::dot(off, ahead) * sign - 0.01f;
    const float distance = std::sqrt(x * x + z * z);
    static constexpr float kHeights[8] = {-0.13f, -0.08f, 0.0f, 0.08f, 0.16f, 0.24f, 0.32f, 0.38f};
    static constexpr float kOutline[8][8] = {{0.154f, 0.212f, 0.218f, 0.192f, 0.177f, 0.209f, 0.217f, 0.214f}, {0.154f, 0.214f, 0.220f, 0.192f, 0.173f, 0.213f, 0.217f, 0.214f},
                                             {0.147f, 0.203f, 0.198f, 0.190f, 0.165f, 0.199f, 0.202f, 0.194f}, {0.160f, 0.200f, 0.197f, 0.189f, 0.150f, 0.192f, 0.202f, 0.191f},
                                             {0.165f, 0.192f, 0.202f, 0.199f, 0.149f, 0.202f, 0.204f, 0.188f}, {0.165f, 0.194f, 0.223f, 0.224f, 0.169f, 0.233f, 0.217f, 0.185f},
                                             {0.160f, 0.185f, 0.201f, 0.257f, 0.169f, 0.246f, 0.235f, 0.182f}, {0.150f, 0.166f, 0.236f, 0.253f, 0.184f, 0.276f, 0.242f, 0.167f}};
    int row = 0;
    while (row < 6 && along > kHeights[row + 1]) ++row;
    const float up_share = std::clamp((along - kHeights[row]) / (kHeights[row + 1] - kHeights[row]), 0.0f, 1.0f);
    float turned = std::atan2(x, z) / (2.0f * kPi);
    if (turned < 0.0f) turned += 1.0f;
    turned *= 8.0f;
    const int side_a = static_cast<int>(turned) % 8, side_b = (side_a + 1) % 8;
    const float side_share = turned - std::floor(turned);
    const float radius = glm::mix(glm::mix(kOutline[row][side_a], kOutline[row][side_b], side_share), glm::mix(kOutline[row + 1][side_a], kOutline[row + 1][side_b], side_share), up_share) +
                         0.05f * run_laid_ + 0.018f * (stride_on_ ? std::min(1.0f, glm::length(stride_)) : 0.0f) + margin +
                         (hem > 0.0f && distance > 1.0e-4f ? hem * std::clamp(hem_side * x / distance, 0.0f, 1.0f) * std::clamp((0.04f - along) / 0.06f, 0.0f, 1.0f) : 0.0f);
    const float reach = distance / radius;
    if (reach >= 1.0f) return glm::vec3(0.0f);
    const float inside = 1.0f - reach;
    const float scale = reach > 1.0e-3f ? inside * inside / (inside + 0.03f) / reach : 0.0f;
    const glm::vec3 push = reach > 1.0e-3f ? across * (x * scale) + ahead * (sign * z * scale) : across * radius;
    return push * (height * height * (3.0f - 2.0f * height));
}

float PlayerBody::StockRoom(const glm::vec3& off, float tall) const {
    const float length = glm::length(off), up_length = glm::length(stock_comb);
    if (length < 1.0e-5f || up_length < 1.0e-5f) return tall;
    const float wide = std::max(tall - 0.02f, 0.02f);
    const float c = glm::dot(off, stock_comb) / (length * up_length);
    const float s2 = std::max(1.0f - c * c, 0.0f);
    return 1.0f / std::sqrt(c * c / (tall * tall) + s2 / (wide * wide));
}

glm::vec3 PlayerBody::EasedPush(glm::vec3& state, const glm::vec3& push) const {
    state = snap_now_ ? push : glm::mix(state, push, 1.0f - std::exp(-sim_seconds_ * 45.0f));
    return state;
}

float PlayerBody::ArmRoom() const {
    return glm::mix(0.4f, 1.0f, std::clamp(run_laid_ * run_arms * 1.5f, 0.0f, 1.0f));
}

void PlayerBody::KeepArmOut(bool right) {
    const int upper = g_skeleton.FindName(right ? "SKL_021_RUARM" : "SKL_011_LUARM");
    const int fore = g_skeleton.FindName(right ? "SKL_022_RFARM" : "SKL_012_LFARM");
    const int hand = g_skeleton.FindName(right ? "SKL_023_RHAND" : "SKL_013_LHAND");
    if (upper < 0 || fore < 0 || hand < 0 || static_cast<size_t>(std::max({upper, fore, hand})) >= world_.size()) return;
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    const glm::vec3 s = at(upper), e = at(fore), w = at(hand);
    const float a = glm::length(e - s), b = glm::length(w - e);
    if (a < 1.0e-4f || b < 1.0e-4f) return;
    const float room_share = ArmRoom();
    float hem = 0.0f;
    {
        static const int waist = g_skeleton.FindName("SKL_000_WAIST");
        const int knee = g_skeleton.FindName(right ? "SKL_041_RLEG" : "SKL_031_LLEG");
        if (waist >= 0 && knee >= 0 && static_cast<size_t>(std::max(waist, knee)) < world_.size()) {
            glm::vec3 side(world_[static_cast<size_t>(waist)][0]);
            side.y = 0.0f;
            if (glm::length(side) > 1.0e-4f) hem = std::max(0.0f, (right ? -1.0f : 1.0f) * glm::dot(at(knee) - at(waist), glm::normalize(side)) - 0.13f);
        }
    }
    const float hem_side = right ? -1.0f : 1.0f;
    const glm::vec3 knuckles(world_[static_cast<size_t>(hand)] * glm::vec4(right ? -0.085f : 0.085f, 0.0f, 0.0f, 1.0f));
    glm::vec3 push = TorsoPush(w, 0.058f * room_share, hem, hem_side);
    if (const glm::vec3 p = TorsoPush(knuckles, 0.04f * room_share, hem, hem_side); glm::dot(p, p) > glm::dot(push, push)) push = p;
    if (const glm::vec3 p = TorsoPush(0.5f * (e + w), 0.072f * room_share, hem, hem_side); glm::dot(p, p) > glm::dot(push, push)) push = p;
    for (const char* name : {"SKL_106_LF23", "SKL_109_LF33", "SKL_113_LF43", "SKL_116_LF53", "SKL_103_LF12"}) {
        std::string joint = name;
        if (right) joint[4] = '2', joint[8] = 'R';
        const int b_finger = g_skeleton.FindName(joint.c_str());
        if (b_finger < 0 || static_cast<size_t>(b_finger) >= world_.size()) continue;
        if (const glm::vec3 p = TorsoPush(at(b_finger), 0.022f * room_share, hem, hem_side); glm::dot(p, p) > glm::dot(push, push)) push = p;
    }
    push = EasedPush(hand_push_[right ? 1 : 0], push);
    glm::vec3 push_near = TorsoPush(e, 0.07f * room_share);
    if (const glm::vec3 p = 1.3f * TorsoPush(glm::mix(e, w, 0.2f), 0.07f * room_share); glm::dot(p, p) > glm::dot(push_near, push_near)) push_near = p;
    const glm::vec3 push_elbow = EasedPush(elbow_push_[right ? 1 : 0], push_near);
    if (glm::dot(push, push) < 1.0e-8f && glm::dot(push_elbow, push_elbow) < 1.0e-8f) return;
    const glm::vec3 t = w + push;
    const glm::vec3 to = t - s;
    const float d = std::clamp(glm::length(to), std::abs(a - b) + 1.0e-4f, a + b - 1.0e-4f);
    if (glm::length(to) < 1.0e-4f) return;
    const glm::vec3 n = glm::normalize(to);
    glm::vec3 out = e + push_elbow - s;
    out -= n * glm::dot(out, n);
    if (glm::length(out) < 1.0e-5f) return;
    const glm::vec3 pole = glm::normalize(out);
    const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const glm::vec3 elbow = s + n * (a * cos_a) + pole * (a * std::sqrt(1.0f - cos_a * cos_a));
    TurnSubtree(upper, ArcBetween(glm::normalize(e - s), glm::normalize(elbow - s)), s);
    const glm::vec3 wrist = at(hand);
    TurnSubtree(fore, ArcBetween(glm::normalize(wrist - elbow), glm::normalize(s + n * d - elbow)), elbow);
}

void PlayerBody::ReachLeftHand(const glm::mat4& target) {
    ReachHand(false, target, true);
}

void PlayerBody::ReachHand(bool right, const glm::mat4& target, bool turn_hand) {
    const int upper = g_skeleton.FindName(right ? "SKL_021_RUARM" : "SKL_011_LUARM");
    const int fore = g_skeleton.FindName(right ? "SKL_022_RFARM" : "SKL_012_LFARM");
    const int hand = g_skeleton.FindName(right ? "SKL_023_RHAND" : "SKL_013_LHAND");
    if (upper < 0 || fore < 0 || hand < 0 || static_cast<size_t>(std::max({upper, fore, hand})) >= world_.size()) {
        return;
    }
    const auto at = [&](int b) { return glm::vec3(world_[static_cast<size_t>(b)][3]); };
    glm::vec3 s = at(upper);
    glm::vec3 e = at(fore);
    glm::vec3 w = at(hand);
    const glm::vec3 t(target[3]);
    const float a = glm::length(e - s);
    const float b = glm::length(w - e);
    static const int collar = g_skeleton.FindName("SKL_010_LSHLD");
    if (const float past = glm::length(t - s) - (a + b - 0.045f); ease_reach && !right && past > 0.0f && collar >= 0 && static_cast<size_t>(collar) < world_.size()) {
        const glm::vec3 shift = glm::normalize(t - s) * std::min(past, 0.08f) * pole_share_[0];
        const glm::vec3 root = at(collar);
        if (glm::length(s - root) > 1.0e-3f && glm::length(s + shift - root) > 1.0e-3f) {
            TurnSubtree(collar, ArcBetween(glm::normalize(s - root), glm::normalize(s + shift - root)), root);
        }
        s = at(upper);
        e = at(fore);
        w = at(hand);
    }
    const glm::vec3 to = t - s;
    if (a < 1.0e-4f || b < 1.0e-4f || glm::length(to) < 1.0e-4f) {
        return;
    }
    float d = std::clamp(glm::length(to), std::abs(a - b) + 1.0e-4f, a + b - 1.0e-4f);
    if (constexpr float kSoft = 0.03f; ease_reach && glm::length(to) > a + b - kSoft) {
        d = a + b - kSoft * std::exp(-(glm::length(to) - (a + b - kSoft)) / kSoft);
    }
    const glm::vec3 n = glm::normalize(to);
    glm::vec3 pole = (e - s) + glm::vec3(0.0f, -0.5f * a, 0.0f);
    if (const std::optional<glm::vec3>& hint = elbow_hint[right ? 1 : 0]; hint) {
        const float share = right ? std::clamp(right_hint_share, 0.0f, 1.0f) : 1.0f;
        if (share >= 1.0f || glm::length(pole) < 1.0e-5f || glm::length(*hint) < 1.0e-5f) {
            pole = *hint;
        } else {
            pole = glm::mix(glm::normalize(pole), glm::normalize(*hint), share);
        }
    }
    const float carried = ease_reach && natural_chest_known_ ? std::clamp(carry_weight, 0.0f, 1.0f) : 0.0f;
    if (carried > 0.0f) {
        const glm::vec3 authored = glm::mat3(natural_chest_) * carry_pole[right ? 1 : 0];
        pole = glm::length(pole) > 1.0e-5f ? glm::mix(glm::normalize(pole), authored, carried) : authored;
    }
    pole -= n * glm::dot(pole, n);
    if (glm::length(pole) < 1.0e-5f) {
        pole = glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    pole = glm::normalize(pole);
    if (const float k = straight_wrist[right ? 1 : 0]; k > 0.0f) {
        const glm::vec3 rest = glm::transpose(glm::mat3(world_[static_cast<size_t>(hand)])) * glm::normalize(w - e);
        const glm::vec3 line = glm::normalize(glm::mat3(target) * rest);
        const glm::vec3 across = line - n * glm::dot(line, n);
        if (glm::length(across) > 1.0e-3f) {
            const glm::vec3 want = -glm::normalize(across);
            const float round = std::atan2(glm::dot(n, glm::cross(pole, want)), glm::dot(pole, want));
            const auto ease = [](float v) {
                v = std::clamp(v, 0.0f, 1.0f);
                return v * v * (3.0f - 2.0f * v);
            };
            const float swing = ease_reach ? std::clamp(round, -k, k) * ease((kPi - std::abs(round)) / (kPi - 1.6f)) * ease(glm::length(across) / 0.2f) : std::clamp(round, -k, k);
            float allowed = swing;
            if (ease_reach) {
                const int chest_bone = g_skeleton.FindName("SKL_002_CHEST"), waist_bone = g_skeleton.FindName("SKL_000_WAIST");
                const glm::vec3 chest_at = chest_bone >= 0 ? at(chest_bone) : glm::vec3(0.0f, 1.2f, 0.0f);
                const float low = waist_bone >= 0 ? at(waist_bone).y - 0.12f : 0.9f;
                const float d0 = std::clamp(glm::length(to), std::abs(a - b) + 1.0e-4f, a + b - 1.0e-4f);
                const float cos0 = std::clamp((a * a + d0 * d0 - b * b) / (2.0f * a * d0), -1.0f, 1.0f);
                const auto clear = [&](float angle) {
                    const glm::vec3 p = glm::angleAxis(angle, n) * pole;
                    const glm::vec3 el = s + n * (a * cos0) + p * (a * std::sqrt(1.0f - cos0 * cos0));
                    for (const float along : {0.0f, 0.3f, 0.6f, 0.85f}) {
                        const glm::vec3 q = glm::mix(el, t, along);
                        if (q.y < low || q.y > chest_at.y + 0.28f) continue;
                        const float x = (q.x - chest_at.x) / 0.235f, z = (q.z - chest_at.z - 0.005f) / 0.19f;
                        if (x * x + z * z < 1.0f) return false;
                    }
                    if (right && stock_line) {
                        const glm::vec3 line = stock_line->second - stock_line->first;
                        for (const float along : {0.4f, 0.7f}) {
                            const glm::vec3 q = glm::mix(s, el, along);
                            const float u = std::clamp(glm::dot(q - stock_line->first, line) / std::max(glm::dot(line, line), 1.0e-5f), 0.0f, 1.0f);
                            if (glm::distance(q, stock_line->first + line * u) < 0.1f) return false;
                        }
                        for (const float along : {0.0f, 0.25f, 0.5f, 0.68f}) {
                            const glm::vec3 q = glm::mix(el, t, along);
                            const float u = std::clamp(glm::dot(q - stock_line->first, line) / std::max(glm::dot(line, line), 1.0e-5f), 0.0f, 1.0f);
                            if (glm::distance(q, stock_line->first + line * u) < 0.085f + stock_deep) return false;
                        }
                    }
                    return true;
                };
                if (!clear(allowed)) {
                    float lo = 0.0f, hi = 1.0f;
                    for (int i = 0; i < 10; ++i) {
                        const float mid = 0.5f * (lo + hi);
                        if (clear(swing * mid)) lo = mid;
                        else hi = mid;
                    }
                    allowed = swing * lo;
                }
            }
            pole = glm::angleAxis(allowed * (1.0f - carried), n) * pole;
        }
    }
    if (ease_reach) {
        const int side = right ? 1 : 0;
        glm::vec3 own = (e - s) - n * glm::dot(e - s, n);
        own = glm::length(own) > 1.0e-5f ? glm::normalize(own) : pole;
        glm::vec3 wanted = glm::mix(own, pole, pole_share_[side]);
        wanted -= n * glm::dot(wanted, n);
        if (glm::length(wanted) > 1.0e-4f) pole = glm::normalize(wanted);
        const glm::vec3 from = pole_primed_[side] ? pole_[side] : own;
        glm::vec3 eased = glm::mix(from, pole, 1.0f - std::exp(-sim_seconds_ * 10.0f));
        eased -= n * glm::dot(eased, n);
        if (glm::length(eased) > 1.0e-4f) pole = glm::normalize(eased);
        pole_[side] = pole;
        pole_primed_[side] = true;
        glm::vec3 last = glm::mix(own, pole, pole_share_[side]);
        last -= n * glm::dot(last, n);
        if (glm::length(last) > 1.0e-4f) pole = glm::normalize(last);
    }
    const float cos_a = std::clamp((a * a + d * d - b * b) / (2.0f * a * d), -1.0f, 1.0f);
    const float reach_out = a * std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    glm::vec3 elbow = s + n * (a * cos_a) + pole * reach_out;
    if (ease_reach && reach_out > 0.015f) {
        const int side = right ? 1 : 0;
        const float room = glm::mix(0.07f * ArmRoom(), (right ? 0.078f : 0.088f) - 0.018f * run_laid_, std::clamp(pole_share_[side] * 3.0f, 0.0f, 1.0f));
        const glm::vec3 grip = s + n * d;
        const glm::vec3 center = s + n * (a * cos_a);
        const glm::vec3 u0 = pole;
        const glm::vec3 v0 = glm::cross(n, u0);
        const bool has_stock = right && stock_line.has_value();
        const glm::vec3 stock_a = has_stock ? stock_line->first : glm::vec3(0.0f);
        const glm::vec3 stock_ab = has_stock ? (stock_line->second - stock_line->first) : glm::vec3(0.0f);
        const float stock_ab2 = std::max(glm::dot(stock_ab, stock_ab), 1.0e-5f);
        const bool has_comb = has_stock && glm::dot(stock_comb, stock_comb) > 1.0e-8f;
        const bool has_fore = !right && fore_line.has_value();
        const glm::vec3 fore_a = has_fore ? fore_line->first : glm::vec3(0.0f);
        const glm::vec3 fore_ab = has_fore ? (fore_line->second - fore_line->first) : glm::vec3(0.0f);
        const float fore_ab2 = std::max(glm::dot(fore_ab, fore_ab), 1.0e-5f);

        auto pole_at = [&](float phi) {
            return std::cos(phi) * u0 + std::sin(phi) * v0;
        };
        auto arm_penalty = [&](float phi) {
            const glm::vec3 el = center + pole_at(phi) * reach_out;
            float pen = 0.0f;
            for (const float along : {0.48f, 0.74f}) {
                const glm::vec3 p = TorsoPush(glm::mix(s, el, along), room - 0.010f);
                const float l = glm::length(p);
                if (l > 0.0f) pen += l;
            }
            for (const float along : {0.0f, 0.16f, 0.32f, 0.48f, 0.64f}) {
                const float r = glm::mix(room, std::max(room - 0.016f, 0.045f), along / 0.64f);
                const glm::vec3 p = TorsoPush(glm::mix(el, grip, along), r);
                const float l = glm::length(p);
                if (l > 0.0f) pen += l;
            }
            if (has_stock) {
                for (const float along : {0.45f, 0.72f, 1.0f}) {
                    const glm::vec3 q = glm::mix(s, el, along);
                    const float u = std::clamp(glm::dot(q - stock_a, stock_ab) / stock_ab2, 0.0f, 1.0f);
                    const float gap = glm::length(q - (stock_a + stock_ab * u));
                    constexpr float kUpperClear = 0.092f;
                    if (gap < kUpperClear) pen += (kUpperClear - gap) * 1.4f;
                }
                for (const float along : {0.0f, 0.14f, 0.28f, 0.42f, 0.56f, 0.68f}) {
                    const glm::vec3 q = glm::mix(el, grip, along);
                    const float u = std::clamp(glm::dot(q - stock_a, stock_ab) / stock_ab2, 0.0f, 1.0f);
                    const glm::vec3 off = q - (stock_a + stock_ab * u);
                    const float gap = glm::length(off);
                    const float clear = StockRoom(off, glm::mix(0.086f, 0.062f, along / 0.68f) + stock_deep * 0.5f);
                    if (gap < clear) pen += (clear - gap) * 1.6f;
                    if (has_comb) {
                        const glm::vec3 ca = stock_a + stock_comb;
                        const float uc = std::clamp(glm::dot(q - ca, stock_ab) / stock_ab2, 0.0f, 0.76f);
                        const float gc = glm::length(q - (ca + stock_ab * uc));
                        const float cc = glm::mix(0.068f, 0.048f, along / 0.68f);
                        if (gc < cc) pen += (cc - gc) * 1.5f;
                    }
                }
            }
            if (has_fore) {
                for (const float along : {0.50f, 0.75f, 1.0f}) {
                    const glm::vec3 q = glm::mix(s, el, along);
                    const float u = std::clamp(glm::dot(q - fore_a, fore_ab) / fore_ab2, 0.0f, 1.0f);
                    const float gap = glm::length(q - (fore_a + fore_ab * u));
                    constexpr float kForeUpperClear = 0.114f;
                    if (gap < kForeUpperClear) pen += (kForeUpperClear - gap) * 1.3f;
                }
                for (const float along : {0.0f, 0.16f, 0.32f, 0.48f, 0.64f}) {
                    const glm::vec3 q = glm::mix(el, grip, along);
                    const float u = std::clamp(glm::dot(q - fore_a, fore_ab) / fore_ab2, 0.0f, 1.0f);
                    const float gap = glm::length(q - (fore_a + fore_ab * u));
                    const float clear = glm::mix(0.106f, 0.072f, along / 0.64f);
                    if (gap < clear) pen += (clear - gap) * 1.5f;
                }
            }
            return pen;
        };

        static const int chest_bone = g_skeleton.FindName("SKL_002_CHEST");
        const glm::mat3 chest = chest_bone >= 0 && static_cast<size_t>(chest_bone) < world_.size() ? glm::mat3(world_[static_cast<size_t>(chest_bone)]) : glm::mat3(1.0f);
        bool has_last = false;
        float last_phi = 0.0f;
        if (reach_age_[side] < 0.05f) {
            glm::vec3 last = chest * drawn_pole_[side];
            last -= n * glm::dot(last, n);
            if (glm::length(last) > 1.0e-4f) {
                last = glm::normalize(last);
                last_phi = std::atan2(glm::dot(last, v0), glm::dot(last, u0));
                has_last = true;
            }
        }
        auto wrap_diff = [](float a_rad, float b_rad) {
            float diff = a_rad - b_rad;
            while (diff > kPi) diff -= 2.0f * kPi;
            while (diff < -kPi) diff += 2.0f * kPi;
            return diff;
        };
        const float stock_mid_y = has_stock ? (stock_a + stock_ab * 0.45f).y : 0.0f;
        constexpr float kStayWeight = 0.35f;
        auto score = [&](float phi) {
            const float pen = arm_penalty(phi);
            const glm::vec3 p = pole_at(phi);
            const glm::vec3 el = center + p * reach_out;
            const float over_sh = std::max(0.0f, el.y - (s.y - 0.16f));
            const float over_stock = has_stock ? std::max(0.0f, el.y - (stock_mid_y - 0.03f)) : 0.0f;
            const float fwd_el = has_stock ? std::max(0.0f, (el.z - s.z) - 0.13f) : 0.0f;
            const float up_bias = std::max(0.0f, p.y);
            const float side_bias = right ? std::max(0.0f, p.x) : std::max(0.0f, -p.x);
            const float stay = has_last ? wrap_diff(phi, last_phi) : 0.0f;
            return pen * 50.0f + pen * pen * 240.0f + over_sh * 240.0f + over_stock * 110.0f + fwd_el * 120.0f +
                   up_bias * up_bias * 3.0f + 0.0080f * phi * phi + 0.0020f * side_bias * side_bias + kStayWeight * stay * stay;
        };

        float best_phi = 0.0f;
        float best_pen = arm_penalty(0.0f);
        const glm::vec3 el0 = center + u0 * reach_out;
        const bool need_sweep = carried < 0.999f && (best_pen > 1.0e-4f || el0.y > s.y - 0.16f || (has_stock && (el0.y > stock_mid_y - 0.03f || (el0.z - s.z) > 0.13f)));
        if (need_sweep) {
            constexpr float kMaxPhi = 1.70f;
            constexpr int kSamples = 36;
            constexpr float kStep = (2.0f * kMaxPhi) / static_cast<float>(kSamples);
            float best_score = score(0.0f);
            for (int i = 0; i <= kSamples; ++i) {
                const float phi = -kMaxPhi + static_cast<float>(i) * kStep;
                const float sc = score(phi);
                if (sc < best_score) {
                    best_score = sc;
                    best_phi = phi;
                }
            }
            float lo = std::max(-kMaxPhi, best_phi - kStep);
            float hi = std::min(kMaxPhi, best_phi + kStep);
            constexpr float kInvPhi = 0.61803399f;
            float x1 = hi - kInvPhi * (hi - lo);
            float x2 = lo + kInvPhi * (hi - lo);
            float f1 = score(x1);
            float f2 = score(x2);
            for (int iter = 0; iter < 8; ++iter) {
                if (f1 < f2) {
                    hi = x2;
                    x2 = x1;
                    f2 = f1;
                    x1 = hi - kInvPhi * (hi - lo);
                    f1 = score(x1);
                } else {
                    lo = x1;
                    x1 = x2;
                    f1 = f2;
                    x2 = lo + kInvPhi * (hi - lo);
                    f2 = score(x2);
                }
            }
            best_phi = f1 < f2 ? x1 : x2;
            best_pen = arm_penalty(best_phi);
        }

        float final_phi = best_phi;
        if (has_last) {
            constexpr float kOmega = 48.0f;
            const float dt = std::min(sim_seconds_, 0.05f);
            const float target = last_phi + wrap_diff(best_phi, last_phi);
            const float off = last_phi - target;
            const float push = (swivel_speed_[side] + kOmega * off) * dt;
            const float decay = std::exp(-kOmega * dt);
            swivel_speed_[side] = (swivel_speed_[side] - kOmega * push) * decay;
            final_phi = target + (off + push) * decay;
        } else {
            swivel_speed_[side] = 0.0f;
        }
        const float settle = std::clamp((carried - 0.85f) / 0.15f, 0.0f, 1.0f);
        pole = glm::normalize(pole_at(final_phi * (1.0f - settle * settle * (3.0f - 2.0f * settle))));
        elbow = center + pole * reach_out;
        drawn_pole_[side] = glm::transpose(chest) * pole;
        reach_age_[side] = 0.0f;
    }
    glm::vec3 root = s;
    glm::vec3 goal = s + n * d;
    shoulder_moved[right ? 1 : 0] = glm::vec3(0.0f);
    if (const WristLimits& limits = wrist_limits[right ? 1 : 0]; limits.on) {
        const glm::mat3 turn(target);
        const glm::vec3 x = glm::normalize(g_skeleton.bind_world[static_cast<size_t>(hand)] - g_skeleton.bind_world[static_cast<size_t>(fore)]);
        const glm::vec3 y = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) - x * x.y);
        const glm::vec3 z = glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f) - x * x.z - y * glm::dot(glm::vec3(0.0f, 0.0f, 1.0f), y));
        const glm::vec3 seen = glm::transpose(turn) * glm::normalize(goal - elbow);
        const float palm = std::clamp(std::atan2(glm::dot(seen, y), glm::dot(seen, x)), -limits.back, limits.palm);
        const float side = std::clamp(std::atan2(glm::dot(seen, z), glm::dot(seen, x)), -limits.thumb, limits.little);
        glm::vec3 line = turn * glm::normalize(x + y * std::tan(palm) + z * std::tan(side));
        if (ease_reach && !right) {
            if (const glm::vec3 own = goal - elbow; glm::length(own) > 1.0e-4f) {
                const glm::vec3 mixed = glm::mix(glm::normalize(own), line, pole_share_[0]);
                if (glm::length(mixed) > 1.0e-3f) line = glm::normalize(mixed);
            }
            if (forearm_primed_) {
                const glm::vec3 eased = glm::mix(forearm_, line, 1.0f - std::exp(-sim_seconds_ * 12.0f));
                if (glm::length(eased) > 1.0e-3f) line = glm::normalize(eased);
            }
            forearm_ = line;
            forearm_primed_ = true;
        }
        goal = t;
        elbow = t - line * b;
        const glm::vec3 back = s - elbow;
        root = elbow + (glm::length(back) > 1.0e-4f ? glm::normalize(back) : -line) * a;
        shoulder_moved[right ? 1 : 0] = root - s;
        for (size_t i = 0; i < world_.size() && i < g_skeleton.parents.size(); ++i) {
            int from = static_cast<int>(i);
            while (from >= 0 && from != upper) from = g_skeleton.parents[static_cast<size_t>(from)];
            if (from == upper) world_[i][3] += glm::vec4(root - s, 0.0f);
        }
    }
    const glm::vec3 u_dir = glm::normalize(elbow - root);
    TurnSubtree(upper, ArcBetween(glm::normalize(at(fore) - root), u_dir), root);
    const glm::vec3 f_dir = glm::normalize(goal - elbow);
    const glm::vec3 bend_vec = f_dir - u_dir * glm::dot(f_dir, u_dir);
    if (const float bend_len = glm::length(bend_vec); bend_len > 0.02f) {
        const glm::vec3 bend_in = bend_vec / bend_len;
        glm::vec3 cur_z(world_[static_cast<size_t>(upper)] * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
        cur_z -= u_dir * glm::dot(cur_z, u_dir);
        if (glm::length(cur_z) > 1.0e-3f) {
            cur_z = glm::normalize(cur_z);
            float roll = std::atan2(glm::dot(u_dir, glm::cross(cur_z, bend_in)), glm::dot(cur_z, bend_in));
            const float w_blend = std::clamp((bend_len - 0.02f) / 0.10f, 0.0f, 1.0f);
            const int arm = right ? 1 : 0;
            if (hinge_serial_[arm] + 1 == skin_serial_ || hinge_serial_[arm] == skin_serial_) {
                roll += 6.2831853f * std::round((hinge_roll_[arm] - roll) / 6.2831853f);
            }
            if (w_blend < 1.0e-3f) roll = std::remainder(roll, 6.2831853f);
            hinge_roll_[arm] = roll;
            hinge_serial_[arm] = skin_serial_;
            TurnSubtree(upper, glm::angleAxis(roll * w_blend, u_dir), root);
        }
    }
    const glm::vec3 wrist = at(hand);
    TurnSubtree(fore, ArcBetween(glm::normalize(wrist - elbow), f_dir), elbow);
    if (!turn_hand) {
        return;
    }
    const glm::quat now = glm::quat_cast(glm::mat3(world_[static_cast<size_t>(hand)]));
    glm::quat want = glm::quat_cast(glm::mat3(target));
    if (ease_reach && !right && glm::length(goal - elbow) > 1.0e-4f) {
        const glm::vec3 along = glm::normalize(goal - elbow);
        const glm::vec3 fingers = glm::normalize(glm::vec3(want * glm::vec3(1.0f, 0.0f, 0.0f)));
        const float bent = std::acos(std::clamp(glm::dot(along, fingers), -1.0f, 1.0f));
        constexpr float kLimit = 0.78f;
        if (bent > kLimit) {
            glm::vec3 axis = glm::cross(fingers, along);
            if (glm::length(axis) > 1.0e-4f) {
                axis = glm::normalize(axis);
                float give = bent;
                if ((give_serial_ + 1 == skin_serial_ || give_serial_ == skin_serial_) && glm::dot(axis, give_axis_) < 0.0f) {
                    axis = -axis;
                    give = 6.2831853f - bent;
                }
                give_axis_ = axis;
                give_serial_ = skin_serial_;
                want = glm::angleAxis((give - kLimit) * 0.45f, axis) * want;
            }
        }
    }
    TurnSubtree(hand, glm::normalize(want * glm::inverse(now)), at(hand));
    if (ease_reach && right && !stock_line && glm::length(goal - elbow) > 1.0e-4f) {
        const glm::mat4& placed = world_[static_cast<size_t>(hand)];
        const glm::vec3 along = glm::normalize(goal - elbow);
        const glm::vec3 fingers = glm::normalize(glm::vec3(placed * glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f)));
        const float bent = std::acos(std::clamp(glm::dot(along, fingers), -1.0f, 1.0f));
        constexpr float kLimit = 0.62f;
        if (const glm::vec3 axis = glm::cross(fingers, along); bent > kLimit && glm::length(axis) > 1.0e-4f) {
            const glm::vec3 knuckles(placed * glm::vec4(-0.085f, 0.0f, 0.0f, 1.0f));
            TurnSubtree(hand, glm::angleAxis((bent - kLimit) * 0.35f, glm::normalize(axis)), knuckles);
        }
    }
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

const anim::Skeleton* PlayerBody::BodySkeleton() {
    return g_rig_ready && !g_skeleton.Empty() ? &g_skeleton : nullptr;
}

bool PlayerBody::NaturalBone(const char* name, glm::mat4& out) const {
    const std::string_view wanted(name);
    if (wanted == "SKL_002_CHEST") {
        if (natural_chest_known_) out = natural_chest_;
        return natural_chest_known_;
    }
    if (!natural_known_) {
        return false;
    }
    const int i = wanted == "SKL_021_RUARM" ? 0 : wanted == "SKL_022_RFARM" ? 1 : wanted == "SKL_023_RHAND" ? 2 : -1;
    if (i < 0) {
        return false;
    }
    out = natural_right_[i];
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
