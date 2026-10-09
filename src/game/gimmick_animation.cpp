#include "game/gimmick_animation.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <format>
#include <limits>

#include "engine/anim/demo_file.h"
#include "engine/core/log.h"
#include "engine/core/pathcode.h"
#include "engine/core/strcode.h"
#include "game/game.h"
#include "game/game_sound.h"

namespace pt::game {
namespace {

constexpr const char* kDefaultMtar = "/Assets/sh/motion/mtar/gimmick/ShGimmick_layers.mtar";
constexpr uint32_t kDialogueEvent = 0xC48783C5;
// ShGimmick body clock: vtable+0x80 (0x12570D0) sets entry+0x88 = dt x 299.7003 ticks and the layers step ticksPerFrame
// ticks per gani frame (5 in every P.T. motion), so gimmick motions run at 59.94 frames per second (60000 / 1001).
/* ShGimmick advances its body clock by dt x 299.7003 ticks and every P.T. gani has 5 ticks a frame, so gimmicks animate at 59.94 fps. */
constexpr double kBodyTicksPerSecond = 299.7003;

double FramesPerSecond(const anim::GaniMotion* motion) {
    const uint32_t ticks = motion && motion->ticks_per_frame ? motion->ticks_per_frame : 5;
    return kBodyTicksPerSecond / static_cast<double>(ticks);
}

struct DialogueCondition {
    uint32_t id;
    const char* name;
};

constexpr DialogueCondition kDialogueConditions[] = {
    {0x3C67FB9B, "TRIA1000_111010_0_mib"},
    {0x749ED06C, "TRIA1000_121010_0_baby"},
};

template <typename T>
T At(std::span<const uint8_t> data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

std::string CString(std::span<const uint8_t> data, size_t offset) {
    std::string out;
    while (offset < data.size() && data[offset] != 0 && out.size() < 128) {
        out.push_back(static_cast<char>(data[offset++]));
    }
    return out;
}

std::string ModelPrefix(const std::string& model) {
    const size_t slash = model.find_last_of('/');
    const std::string file = slash == std::string::npos ? model : model.substr(slash + 1);
    return file.size() >= 3 ? file.substr(0, 3) : file;
}

}

bool ReadConnectPoints(std::span<const uint8_t> data, std::vector<ConnectPoint>& out) {
    out.clear();
    size_t node = At<uint32_t>(data, 4);
    for (int guard = 0; guard < 256 && node && node + 0x30 <= data.size(); ++guard) {
        ConnectPoint cnp;
        const uint32_t name_offset = At<uint32_t>(data, node + 4);
        const int32_t data_offset = At<int32_t>(data, node + 0x0C);
        const int32_t next = At<int32_t>(data, node + 0x20);
        const uint32_t params = At<uint32_t>(data, node + 0x24);
        cnp.name = name_offset ? CString(data, node + name_offset) : std::string();
        if (data_offset) {
            const size_t d = node + static_cast<size_t>(data_offset);
            cnp.translation = glm::vec3(At<float>(data, d), At<float>(data, d + 4), At<float>(data, d + 8));
            cnp.rotation = glm::quat(At<float>(data, d + 28), At<float>(data, d + 16), At<float>(data, d + 20), At<float>(data, d + 24));
        }
        if (params) {
            const size_t p = node + params;
            if (At<uint16_t>(data, p) == 1) {
                const uint32_t value_offset = At<uint32_t>(data, p + 16);
                cnp.parent = value_offset ? CString(data, p + 12 + value_offset) : std::string();
            }
        }
        out.push_back(std::move(cnp));
        if (!next) {
            break;
        }
        node += static_cast<size_t>(static_cast<int64_t>(next));
    }
    return !out.empty();
}

void GimmickAnimation::EnsureArchive() {
    if (archive_loaded_) {
        return;
    }
    archive_loaded_ = true;
    std::string path = kDefaultMtar;
    if (Stage* resident = game_.Stages().Resident()) {
        for (const auto& file : resident->files) {
            for (const fox2::Entity& e : file->file->Entities()) {
                if (e.class_name == "ShGimmickParameter") {
                    const std::string mtar = file->file->GetString(e, "mtarFile");
                    if (!mtar.empty()) {
                        path = mtar;
                    }
                }
            }
        }
    }
    auto bytes = game_.GetVfs().ReadFile(path);
    std::string error;
    if (!bytes || !archive_.Parse(std::move(*bytes), &error)) {
        LogWarn("gimmick anim: motion archive {} unreadable {}", path, error);
        return;
    }
    LogInfo("gimmick anim: {} holds {} motions", path, archive_.Motions().size());
}

const anim::GaniMotion* GimmickAnimation::Motion(std::string_view key) {
    if (key.empty()) {
        return nullptr;
    }
    EnsureArchive();
    auto it = by_key_.find(key);
    if (it != by_key_.end()) {
        return it->second;
    }
    const anim::GaniMotion* motion = nullptr;
    if (const std::string* path = game_.Objects().MotionPath(key)) {
        motion = archive_.Find(PathCode64(*path));
        if (motion) {
            LogInfo("gimmick anim: motion {} = {} ({} frames, {:.2f} s{})", key, *path, motion->frames, motion->frames / FramesPerSecond(motion),
                    motion->RigDriven() ? ", rig driven" : "");
        } else {
            LogWarn("gimmick anim: motion {} path {} not in the archive", key, *path);
        }
    }
    by_key_.emplace(std::string(key), motion);
    return motion;
}

float GimmickAnimation::MotionSeconds(std::string_view key) {
    const anim::GaniMotion* motion = Motion(key);
    return motion ? static_cast<float>(motion->frames / FramesPerSecond(motion)) : 0.0f;
}

bool GimmickAnimation::MotionLoops(std::string_view key) {
    const anim::GaniMotion* motion = Motion(key);
    return !motion || motion->Loops();
}

void GimmickAnimation::StopSounds(GimmickType type) {
    Record& r = records_[static_cast<size_t>(type)];
    if (r.dialogue_id && game_.Audio()) {
        game_.Audio()->StopPlayingId(r.dialogue_id, 0.0f);
    }
    r.dialogue_id = 0;
}

void GimmickAnimation::EnsureModel(Record& record, const Gimmick& gimmick) {
    if (record.model == gimmick.model_file && record.parts == gimmick.parts_path) {
        return;
    }
    record.model = gimmick.model_file;
    record.parts = gimmick.parts_path;
    record.skeleton.reset();
    record.connect_points.clear();
    record.has_pose = false;
    if (record.model.empty()) {
        return;
    }
    auto bytes = game_.GetVfs().ReadFile(record.model);
    auto skeleton = std::make_shared<anim::Skeleton>();
    if (!bytes || !anim::ReadFmdlSkeleton(*bytes, *skeleton) || skeleton->Empty()) {
        LogWarn("gimmick anim: {} has no skeleton", record.model);
        return;
    }
    record.bone_codes.clear();
    for (const std::string& name : skeleton->names) {
        record.bone_codes.push_back(StrCode64(name) & kStrCode64Mask);
    }
    record.skeleton = skeleton;
    record.pose.Reset(skeleton->Size());
    record.rig.reset();
    record.help_bones.reset();
    record.rig_binding = anim::RigBinding{};
    record.rig_pose_valid = record.rig_from_valid = false;
    if (!record.parts.empty()) {
        if (auto parts = game_.GetVfs().ReadFile(record.parts)) {
            fox2::DataSetFile file;
            if (file.Load(record.parts, *parts)) {
                for (const fox2::Entity& e : file.Entities()) {
                    if (e.class_name != "ModelDescription") {
                        continue;
                    }
                    const std::string cnp = file.GetString(e, "connectPointFile");
                    if (auto cnp_bytes = cnp.empty() ? std::nullopt : game_.GetVfs().ReadFile(cnp)) {
                        ReadConnectPoints(*cnp_bytes, record.connect_points);
                    }
                    record.help_bones = LoadHelpBones(file.GetString(e, "helpBoneFile"));
                    const std::string rig_path = file.GetString(e, "gameRigFile");
                    if (auto rig = rig_path.empty() ? nullptr : LoadRig(rig_path)) {
                        const bool all = record.rig_binding.Bind(*rig, *skeleton);
                        record.rig = rig;
                        LogInfo("gimmick anim: {} rig {} ({}, {} units, {} joints{})", record.model, rig_path, rig->Name(), rig->Units().size(),
                                rig->Joints().size(), all ? "" : ", some joints not in the skeleton");
                    }
                }
            }
        }
    }
    std::string points;
    for (const ConnectPoint& c : record.connect_points) {
        points += " " + c.name + "@" + c.parent;
    }
    LogInfo("gimmick anim: {} skeleton {} bones, connect points:{}", record.model, skeleton->Size(), points.empty() ? " none" : points);
}

void GimmickAnimation::BuildFallbackPose(Record& record) {
    if (!ocho_fallback_ready_) {
        ocho_fallback_ready_ = true;
        auto bytes = game_.GetVfs().ReadFile("/Assets/sh/demo/demo_stream/gc_p01_090.fsm");
        anim::DemoStreamFile file;
        if (bytes && record.skeleton && file.Parse(std::move(*bytes), nullptr)) {
            const int actor = file.FindActor(anim::ActorKind::Skeleton, "HM_och0_main0_def");
            ocho_fallback_.Reset(record.skeleton->Size());
            if (actor >= 0) {
                for (const anim::ActorUnit& u : file.Actors()[static_cast<size_t>(actor)].units) {
                    const int bone = record.skeleton->Find(u.hash);
                    if (bone < 0) {
                        continue;
                    }
                    anim::DecodedTrack track;
                    glm::vec4 v;
                    if (u.rotation >= 0 && file.DecodeTrack(static_cast<size_t>(u.rotation), 31, track) && track.Sample(0.0, v)) {
                        ocho_fallback_.rotation[static_cast<size_t>(bone)] = anim::ToQuat(v);
                    }
                }
            }
            const int waist = record.skeleton->FindName("SKL_000_WAIST");
            if (waist >= 0) {
                ocho_fallback_.offset[static_cast<size_t>(waist)] = glm::vec3(0.0f, 1.26f, 0.0f);
            }
            LogInfo("gimmick anim: Ocho held at gc_p01_090 frame 0, no rig for gani motions");
        }
    }
    if (ocho_fallback_.Size() == record.pose.Size()) {
        record.pose = ocho_fallback_;
    }
}

std::shared_ptr<const anim::HelpBones> GimmickAnimation::LoadHelpBones(const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }
    if (auto it = help_bones_.find(path); it != help_bones_.end()) {
        return it->second;
    }
    auto help = std::make_shared<anim::HelpBones>();
    std::string error;
    auto bytes = game_.GetVfs().ReadFile(path);
    if (!bytes || !help->Parse(*bytes, &error)) {
        LogWarn("gimmick anim: help bones {} unusable {}", path, bytes ? error : "(not found)");
        help.reset();
    } else {
        LogInfo("gimmick anim: help bones {} ({} drivers{})", path, help->Entries().size(),
                help->UnknownEntries() ? std::format(", {} of unknown types", help->UnknownEntries()) : "");
    }
    help_bones_.emplace(path, help);
    return help;
}

std::shared_ptr<const anim::Rig> GimmickAnimation::LoadRig(const std::string& path) {
    auto it = rigs_.find(path);
    if (it != rigs_.end()) {
        return it->second;
    }
    std::shared_ptr<anim::Rig> rig;
    auto bytes = game_.GetVfs().ReadFile(path);
    std::string error;
    if (bytes) {
        rig = std::make_shared<anim::Rig>();
        if (!rig->Parse(*bytes, &error) || !rig->Evaluable()) {
            LogWarn("gimmick anim: rig {} unusable {}", path, error);
            rig.reset();
        }
    } else {
        LogWarn("gimmick anim: rig {} not found", path);
    }
    rigs_.emplace(path, rig);
    return rig;
}

void GimmickAnimation::SampleRig(Record& record, double frame, bool blending) {
    const anim::Skeleton& skeleton = *record.skeleton;
    anim::RigPose pose;
    anim::RigOutput out;
    anim::SampleRigPose(*record.rig, *record.gani, frame, pose, record.gani->Loops());
    record.root_translation = anim::RigRootTranslation(*record.rig, pose);
    record.root_rotation = anim::RigRootRotation(*record.rig, pose);
    anim::EvaluateRig(*record.rig, record.rig_binding, skeleton, pose, out);
    anim::MakeRigPoseRelative(*record.rig, record.rig_binding, skeleton, out, pose);
    if (blending && record.rig_from_valid) {
        anim::BlendRigPose(*record.rig, record.rig_from, pose, record.blend, pose);
        anim::EvaluateRig(*record.rig, record.rig_binding, skeleton, pose, out);
    }
    record.rig_pose = std::move(pose);
    record.rig_pose_valid = true;
    anim::RigOutputToPose(skeleton, out, record.pose);
    if (blending && !record.rig_from_valid) {
        anim::BlendPose(record.from, record.pose, record.blend, record.pose);
    }
}

void GimmickAnimation::SampleMotion(const Record& record, const anim::GaniMotion& motion, double frame, anim::Pose& out) const {
    const anim::Skeleton& skeleton = *record.skeleton;
    out.Reset(skeleton.Size());
    const double f = motion.WrapFrame(frame, motion.Loops());
    for (const anim::GaniUnit& unit : motion.units) {
        if (unit.hash == anim::kHashModelRootUnit) {
            continue;
        }
        const int bone = skeleton.Find(unit.hash);
        if (bone < 0) {
            continue;
        }
        if (unit.rotation >= 0) {
            out.rotation[static_cast<size_t>(bone)] = anim::ToQuat(motion.Sample(unit, unit.rotation, f));
        }
        if (unit.translation >= 0) {
            out.offset[static_cast<size_t>(bone)] = glm::vec3(motion.Sample(unit, unit.translation, f));
        }
    }
}

void GimmickAnimation::FireEvents(Record& record, const Gimmick& gimmick, double from, double to) {
    const anim::GaniMotion* motion = record.gani;
    // motion events come from the body update, which runs only while the body is active and not suspended (0x95E880); a record
    // whose locators were unloaded (a game over, Endf120) stays silent until 0x953A80 activates it at its new locator
    if (!motion || motion->frames == 0 || to <= from || !gimmick.active || !gimmick.enabled) {
        return;
    }
    const double length = static_cast<double>(motion->frames);
    // a clip without the loop bit plays once and holds its last frame (0xAA7580), so its events fire on the first pass only
    const double last_pass = motion->Loops() ? std::numeric_limits<double>::max() : 0.0;
    for (const anim::GaniEvent& ge : motion->events) {
        for (const anim::EventSection& s : ge.event.sections) {
            if (s.start < 0) {
                continue;
            }
            const double first = std::ceil((from - s.start) / length);
            for (double k = std::max(first, 0.0); k <= last_pass && s.start + k * length <= to; k += 1.0) {
                const double at = s.start + k * length;
                if (at <= from) {
                    continue;
                }
                const anim::StreamEvent& e = ge.event;
                glm::vec3 position(gimmick.world[3]);
                const uint64_t bone_code = e.StringArg(0);
                for (size_t b = 0; b < record.bone_codes.size() && b < record.world.size(); ++b) {
                    if (record.bone_codes[b] == bone_code) {
                        position = glm::vec3(gimmick.world * record.world[b][3]);
                    }
                }
                const int mode = e.ints.empty() ? -1 : e.ints[0];
                if ((mode == 2 || mode == 6) && e.ints.size() >= 2) {
                    game_.PostSoundIdAt(static_cast<uint32_t>(e.ints[1]), position);
                } else if (mode == 0 && e.ints.size() >= 3 && static_cast<uint32_t>(e.ints[1]) == kDialogueEvent) {
                    const char* condition = nullptr;
                    for (const DialogueCondition& c : kDialogueConditions) {
                        if (c.id == static_cast<uint32_t>(e.ints[2])) {
                            condition = c.name;
                        }
                    }
                    const std::string chara = ModelPrefix(record.model);
                    auto* sound = dynamic_cast<GameSound*>(game_.Audio());
                    LogInfo("gimmick anim: {} dialogue {:#x} chara {} condition {}", gimmick.name, static_cast<uint32_t>(e.ints[1]), chara,
                            condition ? condition : "?");
                    // 0x965A10 hands the event to the record (0x128A570 -> 0x1253B50, which picks chara bab or pab by the
                    // partsType), and the sound control posts it on a "Gimmick" object owned by this record.
                    if (sound && sound->Ready() && condition) {
                        const std::string_view args[] = {chara, condition};
                        record.dialogue_id = sound->PostGimmickDialogue(kDialogueEvent, args, gimmick.type);
                    }
                } else {
                    LogDebug("gimmick anim: {} event {:#x} at frame {} not handled", gimmick.name, e.type, s.start);
                }
                break;
            }
        }
    }
}

void GimmickAnimation::ResetSession() {
    for (size_t i = 0; i < records_.size(); ++i) {
        StopSounds(static_cast<GimmickType>(i));
        records_[i] = Record{};
    }
}

void GimmickAnimation::Update(float dt) {
    EnsureArchive();
    for (const Gimmick& g : game_.Objects().Gimmicks()) {
        Record& r = records_[static_cast<size_t>(g.type)];
        EnsureModel(r, g);
        if (!r.skeleton) {
            continue;
        }
        const bool changed = g.motion != r.motion || g.motion_time + 1e-4f < r.motion_time;
        if (changed) {
            const anim::GaniMotion* previous = r.gani;
            const bool previous_done = previous && r.motion_time * FramesPerSecond(previous) >= previous->frames;
            r.from = r.pose;
            r.rig_from = r.rig_pose;
            r.rig_from_valid = r.rig_pose_valid;
            r.blend = r.has_pose ? 0.0f : 1.0f;
            r.blend_frames = (g.motion == g.name && previous_done) ? kIdleInterpFrames : kPlayInterpFrames;
            r.motion = g.motion;
            r.gani = Motion(g.motion);
            r.played_frames = 0.0;
        }
        const double fps = FramesPerSecond(r.gani);
        const double frames_now = static_cast<double>(g.motion_time) * fps;
        if (!changed) {
            FireEvents(r, g, r.played_frames, frames_now);
        } else if (r.gani) {
            FireEvents(r, g, -1e-3, frames_now);
        }
        r.played_frames = frames_now;
        r.motion_time = g.motion_time;
        const bool blending = r.blend < 1.0f;
        if (blending) {
            r.blend = std::min(1.0f, r.blend + g.anim_rate * dt * static_cast<float>(fps) / std::max(1.0f, r.blend_frames));
        }
        const bool rig = r.gani && r.gani->RigDriven() && r.rig && r.rig_binding.Valid();
        if (r.gani && !r.gani->RigDriven()) {
            anim::Pose target;
            SampleMotion(r, *r.gani, frames_now, target);
            r.pose = std::move(target);
            r.has_pose = true;
            r.rig_pose_valid = false;
            if (blending) {
                anim::BlendPose(r.from, r.pose, r.blend, r.pose);
            }
        } else if (rig) {
            SampleRig(r, frames_now, blending);
            r.has_pose = true;
        } else if (r.gani && g.type == GimmickType::Ocho) {
            BuildFallbackPose(r);
            r.has_pose = true;
            r.rig_pose_valid = false;
            if (blending) {
                anim::BlendPose(r.from, r.pose, r.blend, r.pose);
            }
        }
        anim::ComputeBoneWorld(*r.skeleton, r.pose, r.world);
        if (r.help_bones && r.has_pose) {
            r.help_bones->Apply(*r.skeleton, r.world);
        }
        anim::ComputeSkin(*r.skeleton, r.world, r.skin);
    }
}

std::span<const glm::mat4> GimmickAnimation::Skin(GimmickType type) const {
    const Record& r = records_[static_cast<size_t>(type)];
    return r.has_pose ? std::span<const glm::mat4>(r.skin) : std::span<const glm::mat4>();
}

bool GimmickAnimation::BoneWorld(GimmickType type, std::string_view bone, glm::vec3& out) const {
    const Record& r = records_[static_cast<size_t>(type)];
    if (!r.skeleton) {
        return false;
    }
    const int index = r.skeleton->FindName(bone);
    if (index < 0 || static_cast<size_t>(index) >= r.world.size()) {
        return false;
    }
    const Gimmick& g = game_.Objects().Gimmicks()[static_cast<size_t>(type)];
    out = glm::vec3(g.world * r.world[static_cast<size_t>(index)][3]);
    return true;
}

bool GimmickAnimation::RigRoot(GimmickType type, glm::vec3& translation, glm::quat& rotation) const {
    const Record& r = records_[static_cast<size_t>(type)];
    if (!r.rig_pose_valid) {
        return false;
    }
    translation = r.root_translation;
    rotation = r.root_rotation;
    return true;
}

bool GimmickAnimation::RigRootLoop(GimmickType type, glm::vec3& start_translation, glm::quat& start_rotation, glm::vec3& end_translation,
                                   glm::quat& end_rotation) const {
    const Record& r = records_[static_cast<size_t>(type)];
    if (!r.rig_pose_valid || !r.rig || !r.gani) {
        return false;
    }
    bool loop = false;
    for (const anim::RigUnit& unit : r.rig->Units()) {
        if (!unit.Is(anim::RigUnitType::Root) || unit.tracks.empty()) {
            continue;
        }
        for (const anim::GaniUnit& u : r.gani->units) {
            for (const anim::GaniTrack& t : u.tracks) {
                loop = loop || (t.index == unit.tracks[0] && u.Loop());
            }
        }
    }
    if (!loop) {
        return false;
    }
    anim::RigPose pose;
    anim::SampleRigPose(*r.rig, *r.gani, 0.0, pose);
    start_translation = anim::RigRootTranslation(*r.rig, pose);
    start_rotation = anim::RigRootRotation(*r.rig, pose);
    anim::SampleRigPose(*r.rig, *r.gani, static_cast<double>(r.gani->frames), pose, false);
    end_translation = anim::RigRootTranslation(*r.rig, pose);
    end_rotation = anim::RigRootRotation(*r.rig, pose);
    return true;
}

bool GimmickAnimation::ConnectPointWorld(GimmickType type, std::string_view name, glm::vec3& out) const {
    const Record& r = records_[static_cast<size_t>(type)];
    for (const ConnectPoint& c : r.connect_points) {
        if (c.name != name || !r.skeleton) {
            continue;
        }
        const Gimmick& g = game_.Objects().Gimmicks()[static_cast<size_t>(type)];
        const int bone = r.skeleton->FindName(c.parent);
        glm::mat4 parent = g.world;
        if (bone >= 0 && static_cast<size_t>(bone) < r.world.size()) {
            parent = g.world * r.world[static_cast<size_t>(bone)];
        }
        out = glm::vec3(parent * glm::vec4(c.translation, 1.0f));
        return true;
    }
    return false;
}

}
