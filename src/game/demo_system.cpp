#include "game/demo_system.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "engine/audio/dsp.h"
#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"
#include "engine/vfx/vfx_file.h"
#include "game/game.h"
#include "game/game_sound.h"

namespace pt::game {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kFilmHalfHeight = 6.75f;
constexpr double kGameFrame = 1.0 / 30.0;
// 0xB134B0 and 0xB13610 poll the streams once a frame, so a demo starts 2 game frames after Play at the earliest; the rest is I/O.
// The start room doors (no save) start 2 frames after the trap frame in 15 captures, 3 in 17, 4 in 5, 1 and 6 once each; the port
// takes the median. A save being written holds the stream one more frame: the hallway exits that save (f010, 4 of 5 captures at 4,
// one at 5) and the preface after the first-boot option screen, whose options save starts when its setout ends (2 of 2 at 4).
/* Median of the captures: a trap's demo starts 2 to 4 frames after the trap frame, its stream still buffering (0xB16700 waits for bit 0x1000000). */
constexpr double kStartupFrames = 3.0;
constexpr double kSaveIoWindow = 6.0 / 30.0;
constexpr double kFinishFrames = 2.0;
// The audio clock (SoundSystem::RenderedFrames) moves in the output's periods (10 ms and more), not per tick: the demo clock
// runs on game time and takes the audio's lead or lag back slowly. The error against the audio, smoothed over about 20
// periods, is left alone within 1.5 demo frames (the periods' steps), beyond that taken back at up to 5 % of the speed; an
// error over 6 frames (a stall of 0.1 s, the game loop drops time beyond its catch-up) moves the clock to the audio at once
constexpr double kAudioErrorSmoothing = 0.05;
constexpr double kAudioWindowFrames = 1.5;
constexpr double kAudioCatchUp = 0.05;
constexpr double kAudioResyncFrames = 6.0;

// a held scenery demo's run to its frame, demo frames a tick
constexpr double kSceneryFramesPerTick = 240.0;
constexpr uint64_t kFunctorTakePlayer = 0x9FE662692727;
constexpr uint64_t kFunctorReleasePlayer = 0x7CBF30BCCFC2;
constexpr uint64_t kFunctorPostSound = 0x1CA12DC9F6C4;
constexpr uint64_t kFunctorSoundLengthA = 0x436C48773668;
constexpr uint64_t kFunctorSoundLengthB = 0xD0044B945DC2;
constexpr uint64_t kFunctorSoundId = 0x222ADA450A7D;
constexpr uint64_t kFunctorFocusDistance = 0x7D2CDA51F34A;
constexpr uint64_t kFunctorAperture = 0x132A74BFDAB0;
constexpr uint64_t kFunctorFocalLength = 0xDA7CC51B6E8C;
constexpr uint64_t kFunctorShutterSpeed = 0x56FAACF8F8A1;
constexpr uint64_t kFunctorExposureCompensation = 0xF3F47F2C0016;
constexpr uint64_t kFunctorMinExposure = 0x27D0DA57A8B3;
constexpr uint64_t kFunctorMaxExposure = 0x26B0AB0BAB22;
constexpr uint64_t kFunctorBloomSize = 0x0F01431D2840;
constexpr uint64_t kFunctorNearClip = 0xD0E6DA362CC8;
constexpr uint64_t kFunctorFarClip = 0xFD5CF0AF8B5C;
constexpr uint64_t kFunctorAddExposure = 0x9B8D9550E838;
constexpr uint64_t kFunctorKeyValue = 0x8B724706CDAB;
constexpr uint64_t kFunctorBloomWeight = 0x01B49013BFFA;
constexpr uint64_t kFunctorBloomExtraction = 0x289062F11223;
constexpr uint64_t kFunctorFadeIn = 0xFA384E14AB0E;
constexpr uint64_t kFunctorFadeOut = 0xDCB1EDCE2762;
constexpr uint64_t kFunctorFadeColor = 0x2210363B6ED3;
constexpr uint64_t kFunctorBlurBand = 0x80F81AC653DC;
constexpr uint64_t kFunctorBlurRate = 0xC47B4C2312D7;
constexpr uint64_t kFunctorBlurEnable = 0x5200186357D4;
constexpr uint64_t kFunctorEffectKill = 0x62B47A5B81EC;
constexpr uint64_t kFunctorPlayerMesh = 0xBFA41F9E6521;
constexpr uint64_t kFunctorHandyLightOn = 0x36A45BC5DA0B;
constexpr uint64_t kFunctorHandyLightOff = 0x7A30CE028AD6;
constexpr uint64_t kFunctorHandyLightLumen = 0x94E41E1EE297;
constexpr uint64_t kFunctorPlayDemo = 0xD4BA82456D41;
constexpr uint32_t kKeyDemoId = 0xAB55D7A3;
constexpr uint64_t kFunctorSubtitle = 0x4D4D2A467CB6;
constexpr uint32_t kKeyMessageId = 0xB03CD7C0;
constexpr uint64_t kFunctorFilmGrain = 0x92EE88A8CDDD;
constexpr uint32_t kKeyFilmGrain = 0x88A8CDDD;
constexpr uint64_t kFunctorBandingCancellerOn = 0x7985AB60C5D8;
constexpr uint64_t kFunctorBandingCancellerOff = 0x31CED6821BFF;
constexpr uint64_t kFunctorEffectOffset = 0x0F4338F4BC1B;
constexpr uint32_t kKeyOffsetTranslation = 0x0CC3A0CD;
constexpr uint32_t kKeyOffsetRotation = 0x3AA64CF2;
constexpr uint32_t kKeyMeshName = 0x5366B1F9;
constexpr uint32_t kKeyMeshOn = 0x517DBD2A;
// 0x7708B0 (registrar 0x771C60): the simulation units of the demo model `demoObjectName` leave the world at the section start
// (0xFAC690) and rejoin at its end (0xFAC5F0), restarting from the pose
constexpr uint64_t kFunctorSimulation = 0x116E2D91CDAE;
constexpr uint32_t kKeyDemoObjectName = 0x0C8D2F3D;
constexpr const char* kPlayerParts = "/Assets/sh/parts/chara/plr/plr0_main0_def_v00.parts";
constexpr const char* kPlayerPartsPackage = "/Assets/sh/level_asset/chara/player/game_object/plparts_normal.fpk";
constexpr const char* kPlayerPartsDataPackage = "/Assets/sh/level_asset/chara/player/game_object/plparts_normal.fpkd";
constexpr uint64_t kFunctorEffectCreateTimed = 0x203658B3A68B;
constexpr uint64_t kFunctorEffectCreateOthers[] = {0xBA70C25C5CA2, 0x02F0E5F78C3C, 0xE3ED34F2D068, 0x8CD77E7F0E7E, 0x458172C4FCB2};
constexpr uint32_t kKeyEffectTime = 0xB5E26689;

bool EffectCreateFunctor(uint64_t functor) {
    return functor == kFunctorEffectCreateTimed ||
           std::find(std::begin(kFunctorEffectCreateOthers), std::end(kFunctorEffectCreateOthers), functor) != std::end(kFunctorEffectCreateOthers);
}
constexpr uint64_t kFunctorUiText = 0xECEC4D259E21;
constexpr uint64_t kFunctorScreenTone = 0x1B110A64F883;
constexpr uint64_t kFunctorLightSize = 0x9E9941D23878;
constexpr uint64_t kFunctorLightShadowStrength = 0x86F8B0A3DDC3;
constexpr uint64_t kFunctorPointLightShadowStrength = 0x85DE4F60A97A;
constexpr uint64_t kFunctorLightSpecular = 0xCDC4B0257160;
constexpr uint32_t kKeyLightSize = 0xF6E2F706;
constexpr uint32_t kKeyShadowStrength = 0x41A07680;

constexpr uint32_t kKeyMessage = 0x73F3ECE4;
constexpr uint32_t kKeyEventId = 0xE715467F;
constexpr uint32_t kKeyEventName = 0xBD0B148A;
constexpr uint32_t kKeyFocusDistance = 0x90C1D608;
constexpr uint32_t kKeyAperture = 0x4813E0FD;
constexpr uint32_t kKeyFocalLength = 0x5AC7DAF9;
constexpr uint32_t kKeyShutterSpeed = 0x3C247A69;
constexpr uint32_t kKeyExposureCompensation = 0x354A96AB;
constexpr uint32_t kKeyMinExposure = 0xE2937ED7;
constexpr uint32_t kKeyMaxExposure = 0xF8CE4CAA;
constexpr uint32_t kKeyBloomSize = 0xE86C4C68;
constexpr uint32_t kKeyNearClip = 0xA68F1769;
constexpr uint32_t kKeyFarClip = 0xA919F25A;
constexpr uint32_t kKeyKeyValue = 0xF6D228CE;
constexpr uint32_t kKeyBloomWeight = 0x33324EAF;
constexpr uint32_t kKeyBloomExtraction = 0x944F8C03;
constexpr uint32_t kKeyAddExposure[6] = {0xFFDBB53D, 0x3F22BEF2, 0x4A0AD00E, 0x0A82D753, 0x2D2AE594, 0x7012BEF6};
constexpr uint32_t kKeyFadeTime = 0xA85B4A35;
constexpr uint32_t kKeyColor = 0x78F4ACF2;
constexpr uint32_t kKeyPixelBand = 0x7EB7D23A;
constexpr uint32_t kKeyBlendRate = 0x6E577F36;
constexpr uint32_t kKeyEnable = 0x1CEC027A;
constexpr uint32_t kKeyLightName = 0x0C8D2F3D;
constexpr uint32_t kKeyLocatorName = 0xEC624270;
constexpr uint32_t kKeyLightType = 0x1F069F8C;
constexpr uint32_t kKeyTemperature = 0x8AE71C43;
constexpr uint32_t kKeyLumen = 0x65291325;
constexpr uint32_t kKeyDeflection = 0xF16480C5;
constexpr uint32_t kKeyPowerScale = 0x081E11B9;
constexpr uint32_t kKeyAttenuation = 0x706196B3;
constexpr uint32_t kKeyPenumbra = 0x0264EA9F;
constexpr uint32_t kKeyUmbra = 0x712B61BD;
constexpr uint32_t kKeyRotation = 0x4BAA6AFC;
constexpr uint32_t kKeyTranslation = 0x7B47D88A;
constexpr uint32_t kKeyShadow = 0x9EC6ED71;
constexpr uint32_t kKeyInnerRadius = 0x24EDC727;
constexpr uint32_t kKeyOuterRadius = 0xB866793C;
constexpr uint32_t kKeyShadowPenumbra = 0x2A383233;
constexpr uint32_t kKeyShadowUmbra = 0xB3878239;
constexpr uint32_t kKeyShadowAttenuation = 0xC2809567;
constexpr uint32_t kKeyViewBias = 0x26C9C23D;
constexpr uint32_t kKeyBias = 0x5BD7EC42;
constexpr uint32_t kKeySpecular = 0x098894AF;
constexpr uint32_t kKeyEffectFile = 0xC5D7E215;
constexpr uint32_t kKeyInstanceName = 0xEF98A387;
constexpr uint32_t kKeyEffectName = 0x9BA4FB3B;
constexpr uint32_t kKeyPosition = 0x5254CF6A;
constexpr uint32_t kKeyEffectRotation = 0xB50ABC80;
constexpr uint32_t kKeyTargetName = 0x94A7AF05;
constexpr uint32_t kKeyObjectName = 0x37171C7E;
constexpr uint32_t kKeyPositionOffset = 0x7EBB25E1;
constexpr uint32_t kKeyRotationOffset = 0x441EAC0D;
constexpr uint32_t kKeyColorScale = 0x20AF0B89;
constexpr uint32_t kKeyStartSlope = 0xE88854B9;
constexpr uint32_t kKeyEndSlope = 0x996184FA;
constexpr uint32_t kKeyUiFile = 0xEBD4EBE2;
constexpr uint32_t kEndingCaption = 0x7D33E7DF;
constexpr uint64_t kSoundObjectBase = 0x7D000000;
constexpr uint32_t kSoundObjectCount = 32;
constexpr uint32_t kKeyUiText = 0xE5B44417;

const uint64_t kFunctorMessage = kDemoSendMessageFunctor;

struct LightFunctors {
    uint64_t create, color, world, enable, shadow, range, shadow_angle, view_bias, bias, angle;
};

LightFunctors MakeLightFunctors(const char* cls) {
    auto h = [&](const char* m) { return StrCode64(std::string(cls) + "_" + m) & kStrCode64Mask; };
    return {h("CreateLight"), h("SetColor"), h("SetWorldMatrix"), h("SetEnable"), h("SetShadow"), h("SetRange"), h("SetShadowAngularAttenuation"),
            h("SetViewBias"), h("SetBias"), h("SetAngle")};
}

const LightFunctors& SpotFunctors() {
    static const LightFunctors f = MakeLightFunctors("DemoLightFunctor");
    return f;
}

const LightFunctors& PointFunctors() {
    static const LightFunctors f = MakeLightFunctors("DemoPointLightFunctor");
    return f;
}

uint64_t Code(const char* text) {
    return StrCode64(text) & kStrCode64Mask;
}

glm::mat4 Compose(const glm::quat& rotation, const glm::vec3& translation) {
    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation);
}

float CameraCurve(int type, float t, float rate, float center) {
    t = std::clamp(t, 0.0f, 1.0f);
    float c = t;
    switch (type) {
    case 0: return 1.0f;
    case 1: return t;
    case 2: c = std::sin(t * kPi * 0.5f); break;
    case 3: {
        const float cc = std::clamp(center, 0.01f, 0.99f);
        const float a = (0.5f - 3.0f * cc * cc + 2.0f * cc * cc * cc) / (cc * cc * (cc - 1.0f) * (cc - 1.0f));
        const float b = -2.0f - 2.0f * a;
        const float d = 3.0f + a;
        c = std::clamp(a * t * t * t * t + b * t * t * t + d * t * t, 0.0f, 1.0f);
        break;
    }
    default: return t;
    }
    const float r = std::clamp(rate, 0.0f, 1.0f);
    return r < 1.0f ? c * r + (1.0f - r) * t : c;
}

void YawPitch(const glm::quat& rotation, float& yaw, float& pitch) {
    const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    yaw = std::atan2(-forward.x, -forward.z);
    pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
}

glm::quat CameraQuat(float yaw, float pitch) {
    return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f));
}

float FoxYawOf(const glm::quat& rotation) {
    const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    return std::atan2(forward.x, forward.z);
}

template <typename C>
void ApplyRoll(C& camera, const glm::quat& rotation) {
    if constexpr (requires { camera.roll = 0.0f; }) {
        float yaw = 0.0f;
        float pitch = 0.0f;
        YawPitch(rotation, yaw, pitch);
        const glm::vec3 up = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 base_up = CameraQuat(yaw, pitch) * glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        camera.roll = std::atan2(glm::dot(glm::cross(base_up, up), forward), glm::dot(base_up, up));
    }
    if constexpr (requires { camera.rotation = rotation; }) {
        camera.rotation = rotation;
    }
}

std::string SoundVfxEvent(std::span<const uint8_t> data) {
    for (size_t i = 2; i + 5 < data.size(); ++i) {
        if (std::memcmp(data.data() + i, "Play_", 5) != 0 && std::memcmp(data.data() + i, "Stop_", 5) != 0 &&
            std::memcmp(data.data() + i, "Set_", 4) != 0) {
            continue;
        }
        const size_t length = data[i - 2] | (static_cast<size_t>(data[i - 1]) << 8);
        if (length > 4 && length < 128 && i + length <= data.size()) {
            return std::string(reinterpret_cast<const char*>(data.data() + i), length);
        }
    }
    return {};
}

float Param(const anim::StreamEvent& e, uint32_t key, float fallback, float t) {
    const anim::ExecParam* p = e.Param(key);
    if (!p) {
        return fallback;
    }
    const float a = e.Float(key, fallback, false);
    if (!p->interpolated) {
        return a;
    }
    return a + (e.Float(key, fallback, true) - a) * t;
}

glm::vec4 ParamVector(const anim::StreamEvent& e, uint32_t key, glm::vec4 fallback, float t) {
    const anim::ExecParam* p = e.Param(key);
    if (!p) {
        return fallback;
    }
    const glm::vec4 a = e.Vector(key, fallback, false);
    if (!p->interpolated) {
        return a;
    }
    const glm::vec4 b = e.Vector(key, fallback, true);
    if (p->type == anim::ParamType::Quat) {
        return anim::SlerpKey(a, b, t);
    }
    return a + (b - a) * t;
}

}

glm::mat4 PlayingDemo::Transform() const {
    return Compose(rotation, translation);
}

DemoSystem::DemoSystem(Game& game) : game_(game), gimmicks_(std::make_unique<GimmickAnimation>(game)) {
    for (const char* name : {"Play", "Start", "PlayInit", "Finish", "PlayEnd", "Interrupt", "Skip", "FinishMotion", "OpenDoor20", "hideOcho",
                             "InvisibleStaticModel", "PadEnable", "DisableOption", "Endf120", "PlayRadio", "PlayGimmick", "enable_vfx_dust_glass",
                             "enable_voice_breath", "GotoGameOver"}) {
        message_names_[Code(name)] = name;
    }
}

DemoSystem::~DemoSystem() = default;

void DemoSystem::IndexStage(Stage& stage) {
    for (const auto& file : stage.files) {
        const fox2::DataSetFile& f = *file->file;
        for (const MessageScript& script : file->message_scripts) {
            if (!script.message_name.empty()) {
                message_names_[Code(script.message_name.c_str())] = script.message_name;
            }
        }
        for (const fox2::Entity& e : f.Entities()) {
            if (e.class_name != "DemoData") {
                continue;
            }
            auto info = std::make_shared<DemoInfo>();
            if (!ReadDemoInfo(f, e, *info)) {
                continue;
            }
            info->stage_id = stage.id;
            for (const auto& [name, path] : info->model_files) {
                string_names_[Code(name.c_str())] = name;
            }
            for (const auto& [name, kind] : info->setup_lights) {
                string_names_[Code(name.c_str())] = name;
            }
            for (const auto& [key, path] : info->file_params) {
                string_names_[Code(key.c_str())] = path;
            }
            demos_[info->demo_id] = info;
        }
    }
}

void DemoSystem::ForgetStage(const Stage& stage) {
    for (auto it = demos_.begin(); it != demos_.end();) {
        it = it->second->stage_id == stage.id ? demos_.erase(it) : std::next(it);
    }
}

const DemoInfo* DemoSystem::Find(std::string_view demo_id) const {
    auto it = demos_.find(demo_id);
    return it == demos_.end() ? nullptr : it->second.get();
}

std::string DemoSystem::MessageName(uint64_t hash) const {
    auto it = message_names_.find(hash);
    return it == message_names_.end() ? std::format("{:#x}", hash) : it->second;
}

std::shared_ptr<DemoStreamData> DemoSystem::LoadStream(DemoInfo& info) {
    if (info.stream || info.stream_failed || info.stream_path.empty()) {
        return info.stream;
    }
    std::string path = info.stream_path;
    std::optional<std::vector<uint8_t>> bytes;
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        const std::string localized = path.substr(0, slash + 1) + "#Eng/" + path.substr(slash + 1);
        bytes = game_.GetVfs().ReadFile(localized);
        if (bytes) {
            path = localized;
        }
    }
    if (!bytes) {
        bytes = game_.GetVfs().ReadFile(path);
    }
    if (!bytes) {
        LogWarn("demo: stream {} not found", path);
        info.stream_failed = true;
        return nullptr;
    }
    auto stream = std::make_shared<DemoStreamData>();
    std::string error;
    if (!stream->Load(std::move(*bytes), &error)) {
        LogWarn("demo: stream {} unreadable: {}", path, error);
        info.stream_failed = true;
        return nullptr;
    }
    std::string messages;
    for (const anim::StreamEvent* e : stream->events) {
        if (e->exec && e->functor == kFunctorMessage) {
            messages += std::format(" {}@{}", MessageName(e->String(kKeyMessage)), e->Start());
        }
    }
    LogInfo("demo: {} stream {} ({} frames, DemoData {}, {} actors, {} tracks, {} events{}), messages:{}", info.demo_id, path, stream->length_frames,
            info.length_frames, stream->file.Actors().size(), stream->tracks.size(), stream->events.size(),
            stream->sound_wem.empty() ? "" : std::format(", {} byte sound stream", stream->sound_wem.size()), messages.empty() ? " none" : messages);
    info.stream = stream;
    return stream;
}

std::shared_ptr<const anim::Skeleton> DemoSystem::LoadSkeleton(const std::string& fmdl) {
    auto it = skeletons_.find(fmdl);
    if (it != skeletons_.end()) {
        return it->second;
    }
    std::shared_ptr<anim::Skeleton> skeleton;
    if (auto bytes = game_.GetVfs().ReadFile(fmdl)) {
        skeleton = std::make_shared<anim::Skeleton>();
        if (!anim::ReadFmdlSkeleton(*bytes, *skeleton) || skeleton->Empty()) {
            skeleton.reset();
        }
    }
    skeletons_[fmdl] = skeleton;
    return skeleton;
}

std::shared_ptr<const anim::HelpBones> DemoSystem::LoadHelpBones(const std::string& path) {
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
        LogWarn("demo: help bones {} unusable {}", path, bytes ? error : "(not found)");
        help.reset();
    }
    help_bones_.emplace(path, help);
    return help;
}

// The parts file of a demo model: its DemoData parts, else the player's own parts for the player's model.
std::string DemoSystem::PartsPath(const PlayingDemo& demo, const std::string& model) const {
    const DemoInfo& info = *demo.info;
    if (auto it = info.model_parts.find(model); it != info.model_parts.end()) {
        if (auto parts = info.parts_files.find(it->second); parts != info.parts_files.end()) {
            return parts->second;
        }
        return {};
    }
    return model == demo.player_model ? std::string(kPlayerParts) : std::string();
}

// The DemoStreamAnimation's helpBoneFiles, else the helpBoneFile of the model's parts (the ending's player parts and the
// player's own model, plr0_main0_def_v00.parts).
std::string DemoSystem::HelpBonePath(const PlayingDemo& demo, const std::string& model) const {
    const DemoInfo& info = *demo.info;
    if (auto it = info.help_bone_files.find(model); it != info.help_bone_files.end()) {
        return it->second;
    }
    const std::string parts_path = PartsPath(demo, model);
    if (auto bytes = parts_path.empty() ? std::nullopt : game_.GetVfs().ReadFile(parts_path)) {
        fox2::DataSetFile file;
        if (file.Load(parts_path, *bytes)) {
            for (const fox2::Entity& e : file.Entities()) {
                if (e.class_name == "ModelDescription") {
                    return file.GetString(e, "helpBoneFile");
                }
            }
        }
    }
    return {};
}

// The bone simulation of a model: a .sim its parts name (any string of the file; the parts' simulation entry is not decoded),
// else Fox_Files/<model>_s00.sim beside the model's Scenes folder, where plr0, bab0 and pab0 keep theirs (the player's
// /Assets/sh/chara/plr/Fox_Files/plr0_main0_def_s00.sim is in ending.fpkd and plparts_normal.fpkd). Null with PT_SIM=0.
std::shared_ptr<const anim::SimRig> DemoSystem::LoadSimRig(const std::string& parts_path, const std::string& fmdl) {
    if (!anim::SimPhysicsEnabled() || fmdl.empty()) {
        return nullptr;
    }
    const std::string key = parts_path + "|" + fmdl;
    if (auto it = sim_rigs_.find(key); it != sim_rigs_.end()) {
        return it->second;
    }
    std::string path;
    if (auto bytes = parts_path.empty() ? std::nullopt : game_.GetVfs().ReadFile(parts_path)) {
        fox2::DataSetFile file;
        if (file.Load(parts_path, *bytes)) {
            for (const fox2::Entity& e : file.Entities()) {
                for (const fox2::Property& p : e.properties) {
                    for (size_t i = 0; i < p.Count() && path.empty(); ++i) {
                        const std::string value = file.ElementString(p, i);
                        if (value.ends_with(".sim")) {
                            path = value;
                        }
                    }
                }
            }
        }
    }
    if (path.empty()) {
        const size_t slash = fmdl.rfind('/');
        const size_t folder = slash == std::string::npos || slash == 0 ? std::string::npos : fmdl.rfind('/', slash - 1);
        const size_t dot = fmdl.rfind('.');
        if (folder != std::string::npos && dot != std::string::npos && dot > slash) {
            path = fmdl.substr(0, folder) + "/Fox_Files/" + fmdl.substr(slash + 1, dot - slash - 1) + "_s00.sim";
        }
    }
    std::shared_ptr<anim::SimRig> rig;
    if (auto bytes = path.empty() ? std::nullopt : game_.GetVfs().ReadFile(path)) {
        rig = std::make_shared<anim::SimRig>();
        std::string error;
        if (!rig->Load(path, *bytes, &error)) {
            LogWarn("demo: simulation {} unusable ({})", path, error);
            rig.reset();
        }
    }
    sim_rigs_.emplace(key, rig);
    return rig;
}

// First-person demos animate the player's own body (plr0 from plr0_main0_def_v00.parts), one model the original keeps
// for the whole game. Its mesh groups start hidden as the parts' invisibleMeshNames list them (MESH_arm, the headless
// first-person jacket and arms); the mesh functor 0xBFA41F9E6521 and VisibleMesh events show or hide groups, and a group is
// drawn only when neither it nor a parent group is hidden (SetPlayerMeshVisible). The body is drawn only in demos whose
// mesh functor made the model visible: gc_p00_020 and gc_p00_030 show MESH_arm and hide MESH_ROOT, the parent of the head,
// hair and third-person body. The captures show no part of the head or hair in the other first-person demos (gc_p00_022 in
// explore_boot_rb 2920 to 3150, gc_p00_010 in floor_f060 1140 to 1220).
void DemoSystem::LoadPlayerParts() {
    if (player_parts_loaded_) {
        return;
    }
    player_parts_loaded_ = true;
    game_.GetVfs().LoadPackage(kPlayerPartsPackage);
    game_.GetVfs().LoadPackage(kPlayerPartsDataPackage);
    auto bytes = game_.GetVfs().ReadFile(kPlayerParts);
    fox2::DataSetFile file;
    if (!bytes || !file.Load(kPlayerParts, *bytes)) {
        LogWarn("demo: player parts {} unreadable", kPlayerParts);
        return;
    }
    for (const fox2::Entity& e : file.Entities()) {
        if (e.class_name != "ModelDescription") {
            continue;
        }
        player_model_file_ = file.GetString(e, "modelFile");
        player_help_bone_file_ = file.GetString(e, "helpBoneFile");
        if (const fox2::Property* invisible = file.FindProperty(e, "invisibleMeshNames")) {
            for (size_t i = 0; i < invisible->Count(); ++i) {
                player_hidden_.insert(Code(file.ElementString(*invisible, i).c_str()));
            }
        }
        player_default_hidden_ = player_hidden_;
        break;
    }
}

const std::string& DemoSystem::PlayerModelFile() {
    LoadPlayerParts();
    return player_model_file_;
}

const std::set<uint64_t>& DemoSystem::PlayerDefaultHidden() {
    LoadPlayerParts();
    return player_default_hidden_;
}

std::shared_ptr<const anim::HelpBones> DemoSystem::PlayerHelpBones() {
    LoadPlayerParts();
    return LoadHelpBones(player_help_bone_file_);
}

std::shared_ptr<const anim::SimRig> DemoSystem::PlayerSimRig() {
    LoadPlayerParts();
    return LoadSimRig(kPlayerParts, player_model_file_);
}

// Show (0xCD1930 from the functor, 0xCD1600 from VisibleMesh) clears a group's own hidden bit, hide (0xCD1FD0, 0xCD1C50) sets
// it; their tail ORs every group's flags with its parent's, so a hidden group hides its children (HiddenMeshes). Only the
// functor's handler 0x945620 also clears the model's hide bits; VisibleMesh (0x786EE0) changes the group lists alone, so
// gc_p07_030's VisibleMesh of MESH_head_a (the hair, under MESH_head and MESH_ROOT) draws nothing.
void DemoSystem::SetPlayerMeshVisible(PlayingDemo& demo, uint64_t mesh, bool visible, bool functor) {
    LoadPlayerParts();
    mesh &= kStrCode64Mask;
    if (visible) {
        player_hidden_.erase(mesh);
    } else {
        player_hidden_.insert(mesh);
    }
    if (functor) {
        demo.player_visible = true;
    }
    LogInfo("demo: {} player mesh {:#x} {}{}", demo.demo_id, mesh, visible ? "shown" : "hidden", functor ? " (mesh functor, body visible)" : " (VisibleMesh)");
}

bool DemoSystem::Play(std::string_view demo_id) {
    for (const PlayingDemo& existing : playing_) {
        if (existing.demo_id == demo_id && !existing.finished) {
            LogInfo("demo: {} is already playing, Play ignored", demo_id);
            return true;
        }
    }
    auto found = demos_.find(demo_id);
    if (found == demos_.end()) {
        LogWarn("demo: {} has no DemoData", demo_id);
        return false;
    }
    // the Archive's cutscenes (archive.h): a demo played opens its entry
    game_.NoteArchive("demo:" + std::string(demo_id));
    PlayingDemo demo;
    demo.demo_id = std::string(demo_id);
    demo.startup_frames = kStartupFrames;
    demo.info = found->second;
    demo.stream = LoadStream(*found->second);
    demo.length_frames = demo.stream ? demo.stream->length_frames : static_cast<uint32_t>(std::max(found->second->length_frames, 0));
    demo.length = demo.length_frames / kDemoFramesPerSecond;
    demo.rotation = found->second->transform_rotation;
    demo.translation = found->second->transform_translation;
    if (auto it = pending_transforms_.find(demo_id); it != pending_transforms_.end()) {
        demo.has_transform = true;
        demo.rotation = it->second.first;
        demo.translation = it->second.second;
        pending_transforms_.erase(it);
    }
    for (const DemoControlCharacter& c : demo.info->control_characters) {
        if (c.character_id == "Player") {
            demo.player_model = c.model;
        }
    }
    // 0x798D40: the demo camera copies the game camera (the lens the renderer last used, exposure from the floor lighting row of 0x912F40)
    DemoCameraParams& cam = demo.camera_params;
    cam.focus_distance = game_focus_distance_;
    cam.aperture = game_aperture_;
    cam.shutter_speed = game_shutter_speed_;
    if (const LightingRow* row = game_.Parameters().Lighting(game_.FloorLightingRow())) {
        cam.exposure_compensation = row->exposure_compensation;
        cam.min_exposure = row->min_exposure;
        cam.max_exposure = row->max_exposure;
    }
    std::erase_if(playing_, [&](const PlayingDemo& d) { return d.demo_id == demo_id; });
    LogInfo("demo: {} playing ({} frames, {:.2f} s) at ({:.3f} {:.3f} {:.3f}) rot ({:.3f} {:.3f} {:.3f} {:.3f}), game frame {}", demo_id,
            demo.length_frames, demo.length, demo.translation.x, demo.translation.y, demo.translation.z, demo.rotation.x, demo.rotation.y,
            demo.rotation.z, demo.rotation.w, game_.Frame());
    PreloadModels(demo);
    Notify(demo, "PlayInit");
    playing_.push_back(std::move(demo));
    return true;
}

bool DemoSystem::PlayScenery(std::string_view demo_id, double frame) {
    if (!Play(demo_id)) {
        return false;
    }
    playing_.back().hold_frame = std::max(frame, 0.0);
    LogInfo("demo: {} held as scenery at frame {:.0f}", demo_id, frame);
    return true;
}

bool DemoSystem::SceneryReady() const {
    for (const PlayingDemo& demo : playing_) {
        if (demo.Held() && !demo.finished && demo.started && demo.frame + 1e-6 >= demo.hold_frame) {
            return true;
        }
    }
    return false;
}

bool DemoSystem::IsHeld(std::string_view demo_id) const {
    for (const PlayingDemo& demo : playing_) {
        if (demo.demo_id == demo_id && demo.Held() && !demo.finished) {
            return true;
        }
    }
    return false;
}

const DemoCameraParams* DemoSystem::SceneryCameraParams() const {
    for (const PlayingDemo& demo : playing_) {
        if (demo.Held() && !demo.finished && demo.started) {
            return &demo.camera_params;
        }
    }
    return nullptr;
}

void DemoSystem::SetDemoTransform(std::string_view demo_id, const glm::quat& rotation, const glm::vec3& translation) {
    for (PlayingDemo& demo : playing_) {
        if (demo.demo_id == demo_id && !demo.finished) {
            demo.has_transform = true;
            demo.rotation = rotation;
            demo.translation = translation;
            return;
        }
    }
    pending_transforms_[std::string(demo_id)] = {rotation, translation};
}

void DemoSystem::ToggleLoop(std::string_view demo_id) {
    for (PlayingDemo& demo : playing_) {
        if (demo.demo_id == demo_id) {
            demo.loop = !demo.loop;
            LogInfo("demo: {} loop {}", demo_id, demo.loop ? "on" : "off");
        }
    }
}

void DemoSystem::StopAll() {
    for (PlayingDemo& demo : playing_) {
        if (demo.finished) {
            continue;
        }
        LogInfo("demo: {} interrupted", demo.demo_id);
        Notify(demo, "Interrupt");
        ReleasePlayer(demo, "interrupt");
        if (demo.sound_id && game_.Audio()) {
            game_.Audio()->StopPlayingId(demo.sound_id, 0.2f);
        }
        for (DemoEffect& effect : demo.effects) {
            DetachEffectSound(effect, 0.2f);
        }
        demo.finished = true;
    }
    playing_.clear();
    RebuildOutputs();
    UpdateShownCamera();
}

void DemoSystem::Skip(std::string_view demo_id) {
    for (PlayingDemo& demo : playing_) {
        if ((demo_id.empty() || demo.demo_id == demo_id) && !demo.finished && !demo.skip_requested) {
            LogInfo("demo: skip {} at frame {:.1f}", demo.demo_id, demo.frame);
            demo.skip_requested = true;
            Notify(demo, "Skip");
        }
    }
}

bool DemoSystem::IsPlaying(std::string_view demo_id) const {
    for (const PlayingDemo& demo : playing_) {
        if (demo.demo_id == demo_id && !demo.finished) {
            return true;
        }
    }
    return false;
}

double DemoSystem::PlayTime(std::string_view demo_id) const {
    for (const PlayingDemo& demo : playing_) {
        if (demo.demo_id == demo_id) {
            return demo.time;
        }
    }
    return 0.0;
}

bool DemoSystem::ControlsPlayer() const {
    for (const PlayingDemo& demo : playing_) {
        if (demo.player_taken && !demo.finished) {
            return true;
        }
    }
    return false;
}

void DemoSystem::Notify(PlayingDemo& demo, std::string_view message) {
    LogInfo("demo: {} message {} at frame {:.1f}", demo.demo_id, message, demo.frame);
    game_.Messages().PostDemoMessage(demo.demo_id, message);
}

void DemoSystem::Start(PlayingDemo& demo) {
    demo.started = true;
    demo.frame = 0.0;
    demo.time = 0.0;
    demo.previous_frame = -1.0;
    Notify(demo, "Play");
    Notify(demo, "Start");
    auto* sound = dynamic_cast<GameSound*>(game_.Audio());
    if (demo.stream && !demo.stream->sound_wem.empty() && game_.Audio() && !demo.Held()) {
        demo.sound_id = game_.Audio()->PlayStream(demo.stream->sound_wem, nullptr);
        demo.sound_started = true;
        // 0xB171F0 hands a demo on another's clock its time (the parent's minus the event frame) for the sound object too (+0x68 bit
        // 0x20, +0x70), so its audio plays at the parent's time, not from its start
        double synced = 0.0;
        if (!demo.sync_parent.empty()) {
            const auto parent = std::find_if(playing_.begin(), playing_.end(), [&](const PlayingDemo& p) { return p.demo_id == demo.sync_parent; });
            if (parent != playing_.end() && !parent->finished) {
                synced = std::max(0.0, parent->frame - demo.sync_offset);
            }
        }
        if (synced > 0.0 && demo.sound_id) {
            game_.Audio()->SeekPlayingId(demo.sound_id, static_cast<float>(synced / kDemoFramesPerSecond));
        }
        if (sound && sound->Ready() && demo.sound_id) {
            demo.audio_clock = true;
            demo.audio_error = 0.0;
            demo.audio_start_frames = demo.audio_last_frames = sound->System().RenderedFrames();
            demo.audio_start_frames -= std::min<uint64_t>(demo.audio_start_frames,
                                                          static_cast<uint64_t>(synced / kDemoFramesPerSecond * audio::kOutputRate));
        }
        LogInfo("demo: {} sound stream playing id {}{}{}", demo.demo_id, demo.sound_id, demo.audio_clock ? ", clock follows the audio output" : "",
                synced > 0.0 ? std::format(", from {:.3f} s (parent {} frame {:.1f})", synced / kDemoFramesPerSecond, demo.sync_parent, synced + demo.sync_offset)
                             : std::string());
    }
    if (demo.demo_id == "gc_p06_010_final" && !demo.Held()) {
        game_.ShowCaption(kEndingCaption);
    }
}

void DemoSystem::ResyncAudio(std::string_view demo_id) {
    for (PlayingDemo& demo : playing_) {
        if (demo.demo_id != demo_id || !demo.started || demo.finished || !demo.sound_id || !game_.Audio()) continue;
        game_.Audio()->SeekPlayingId(demo.sound_id, static_cast<float>(demo.frame / kDemoFramesPerSecond));
        if (auto* sound = dynamic_cast<GameSound*>(game_.Audio()); sound && sound->Ready()) {
            demo.audio_clock = true;
            demo.audio_base = demo.frame;
            demo.audio_start_frames = demo.audio_last_frames = sound->System().RenderedFrames();
            demo.audio_error = 0.0;
        }
        LogInfo("demo: {} sound moved to frame {:.1f}", demo.demo_id, demo.frame);
    }
}

void DemoSystem::Restart(PlayingDemo& demo) {
    demo.frame -= demo.length_frames;
    if (demo.frame < 0.0) {
        demo.frame = 0.0;
    }
    demo.time = demo.frame / kDemoFramesPerSecond;
    demo.previous_frame = -1.0;
    demo.next_event = 0;
    demo.interpolations.clear();
    demo.start_values.clear();
    demo.skip_events.clear();
    demo.audio_clock = false;
    for (DemoEffect& effect : demo.effects) {
        DetachEffectSound(effect, -1.0f);
    }
    demo.effects.clear();
    LogInfo("demo: {} loops", demo.demo_id);
}

void DemoSystem::Update(float dt) {
    for (size_t i = 0; i < playing_.size(); ++i) {
        PlayingDemo& demo = playing_[i];
        if (demo.finished) {
            continue;
        }
        if (demo.skip_requested) {
            Finish(demo, true);
            continue;
        }
        const double step = static_cast<double>(dt) * time_scale;
        if (demo.camera_linger > 0.0) {
            demo.camera_linger -= step;
            if (demo.camera_linger <= 1e-4) {
                demo.camera_linger = 0.0;
                demo.camera_valid = false;
            }
        }
        if (demo.end_reached) {
            // 0x108A4F0, 0xB12F70: the streams end a game frame after the clock, state 3 (0xB13DD0) finishes in the next
            demo.finish_wait += step;
            if (demo.finish_wait + 1e-4 < kFinishFrames * kGameFrame) {
                continue;
            }
            Finish(demo, false);
            continue;
        }
        if (!demo.started) {
            // 0xB134B0, 0xB13610: states 0 and 1 wait for the streams, the start comes three game frames after the request, four when
            // a save was requested in the 6 frames before the third
            demo.startup += step;
            if (demo.startup + 1e-4 < demo.startup_frames * kGameFrame) {
                continue;
            }
            if (!demo.startup_io_checked) {
                demo.startup_io_checked = true;
                if (game_.SaveInFlight(kSaveIoWindow)) {
                    demo.startup_frames += 1.0;
                    LogInfo("demo: {} stream wait +1 frame, save in progress", demo.demo_id);
                    continue;
                }
            }
            Start(demo);
        } else if (demo.Held()) {
            // scenery: fast to the held frame (its events run as in play), then the clock stops there
            if (demo.frame + 1e-6 >= demo.hold_frame) {
                continue;
            }
            demo.frame = std::min(demo.hold_frame, demo.frame + kSceneryFramesPerTick);
        } else if (demo.advance_wait + 1e-4 < kGameFrame) {
            // 0xB13B80: state 2 advances the clock from the game frame after the start
            demo.advance_wait += step;
            if (demo.advance_wait + 1e-4 < kGameFrame) {
                continue;
            }
            demo.frame += demo.advance_wait * kDemoFramesPerSecond;
        } else {
            demo.frame += step * kDemoFramesPerSecond;
            auto* sound = demo.audio_clock ? dynamic_cast<GameSound*>(game_.Audio()) : nullptr;
            if (sound && time_scale == 1.0f) {
                const uint64_t rendered = sound->System().RenderedFrames();
                if (rendered != demo.audio_last_frames) {
                    const double audio_frame =
                        demo.audio_base + static_cast<double>(rendered - demo.audio_start_frames) / audio::kOutputRate * kDemoFramesPerSecond;
                    const double error = audio_frame - demo.frame;
                    demo.audio_last_frames = rendered;
                    demo.audio_frame = audio_frame;
                    if (std::abs(error) > kAudioResyncFrames) {
                        demo.frame = std::max(demo.previous_frame, audio_frame);
                        demo.audio_error = 0.0;
                    } else {
                        demo.audio_error += (error - demo.audio_error) * kAudioErrorSmoothing;
                    }
                }
                const double limit = kAudioCatchUp * step * kDemoFramesPerSecond;
                const double correction =
                    std::clamp(demo.audio_error - std::clamp(demo.audio_error, -kAudioWindowFrames, kAudioWindowFrames), -limit, limit);
                demo.frame += correction;
                demo.audio_error -= correction;
            }
        }
        if (!demo.sync_parent.empty()) {
            // 0xB171F0: the synced player takes its time from the parent's player minus the offset whenever that is positive, and
            // does not run its own clock (0xB16550 registers none while +0x288 is set), audio included
            const auto parent = std::find_if(playing_.begin(), playing_.end(), [&](const PlayingDemo& p) { return p.demo_id == demo.sync_parent; });
            if (parent != playing_.end() && !parent->finished && parent->frame - demo.sync_offset > 0.0) {
                demo.frame = parent->frame - demo.sync_offset;
            }
        }
        if (demo.frame >= demo.length_frames && demo.loop && demo.length_frames > 0) {
            RunEvents(demo);
            Restart(demo);
        }
        demo.frame = std::min(demo.frame, static_cast<double>(demo.length_frames));
        demo.time = demo.frame / kDemoFramesPerSecond;
        RunEvents(demo);
        std::erase_if(demo.skip_events, [&](const anim::StreamEvent* e) { return e->length <= demo.frame; });
        if (demo.demo_id == "gc_p06_010_final" && game_.Effects().caption_id == kEndingCaption) {
            game_.Effects().caption_time = static_cast<float>(demo.time);
        }
        UpdateInterpolations(demo);
        UpdateActors(demo);
        UpdateCamera(demo);
        UpdatePlayer(demo);
        demo.previous_frame = demo.frame;
        demo.end_reached = demo.frame >= demo.length_frames;
    }
    std::erase_if(playing_, [](const PlayingDemo& demo) { return demo.finished; });
    for (const PendingPlay& play : std::exchange(pending_plays_, {})) {
        if (Play(play.id) && !playing_.empty() && playing_.back().demo_id == play.id && !playing_.back().started) {
            playing_.back().sync_parent = play.parent;
            playing_.back().sync_offset = play.offset;
        }
    }
    RebuildOutputs();
    UpdateShownCamera();
    for (int i = 3; i > 0; --i) {
        lens_history_[i] = lens_history_[i - 1];
        lens_history_[i].age += dt;
    }
    const DemoCameraParams* lens = CameraParams();
    lens_history_[0] = LensSample{lens != nullptr, 0.0, lens ? *lens : DemoCameraParams{}};
}

const DemoCameraParams* DemoSystem::DofLens() const {
    // the newest sample at least one original frame old (two port ticks)
    for (const LensSample& s : lens_history_) {
        if (s.age + 1.0e-4 >= kGameFrame) {
            return s.valid ? &s.params : nullptr;
        }
    }
    return nullptr;
}

void DemoSystem::RunEvents(PlayingDemo& demo) {
    if (!demo.stream) {
        return;
    }
    const auto& events = demo.stream->events;
    std::vector<const anim::StreamEvent*> due;
    while (demo.next_event < events.size()) {
        const anim::StreamEvent& e = *events[demo.next_event];
        if (static_cast<double>(e.Start()) > demo.frame + 1e-6) {
            break;
        }
        ++demo.next_event;
        due.push_back(&e);
    }
    // functor priorities of the registrars: effect kill 0x775CD0 400, effect create 0x775F30 300, the rest 0
    static const uint64_t kEffectCreate = Code("FxEffectCreateEventFunctor");
    auto priority = [&](const anim::StreamEvent* e) {
        if (e->type != anim::kEventExecCommand) {
            return 0;
        }
        return e->functor == kFunctorEffectKill ? 400 : e->functor == kEffectCreate || EffectCreateFunctor(e->functor) ? 300 : 0;
    };
    std::stable_sort(due.begin(), due.end(), [&](const anim::StreamEvent* a, const anim::StreamEvent* b) {
        return a->Start() != b->Start() ? a->Start() < b->Start() : priority(a) > priority(b);
    });
    for (const anim::StreamEvent* e : due) {
        for (auto it = demo.interpolations.begin(); it != demo.interpolations.end();) {
            if (it->end <= e->Start()) {
                const anim::StreamEvent* done = it->event;
                it = demo.interpolations.erase(it);
                RunFunctor(demo, *done, 1.0f, false);
            } else {
                ++it;
            }
        }
        RunEvent(demo, *e);
    }
}

DemoModel* DemoSystem::FindModel(PlayingDemo& demo, std::string_view name) {
    for (DemoModel& m : demo.models) {
        if (m.name == name) {
            return &m;
        }
    }
    return nullptr;
}

DemoLight* DemoSystem::FindLight(PlayingDemo& demo, std::string_view name) {
    for (DemoLight& l : demo.lights) {
        if (l.name == name) {
            return &l;
        }
    }
    return nullptr;
}

DemoEffect* DemoSystem::FindEffect(PlayingDemo& demo, uint64_t instance) {
    if (!instance || instance == Code("")) {
        return nullptr;
    }
    for (DemoEffect& e : demo.effects) {
        if (e.instance == instance) {
            return &e;
        }
    }
    return nullptr;
}

std::string DemoSystem::ModelPath(const PlayingDemo& demo, const std::string& model, const std::string& event_path) const {
    const DemoInfo& info = *demo.info;
    if (auto it = info.model_parts.find(model); it != info.model_parts.end()) {
        if (auto parts = info.parts_files.find(it->second); parts != info.parts_files.end()) {
            if (auto bytes = game_.GetVfs().ReadFile(parts->second)) {
                fox2::DataSetFile file;
                if (file.Load(parts->second, *bytes)) {
                    for (const fox2::Entity& e : file.Entities()) {
                        if (e.class_name == "ModelDescription") {
                            return file.GetString(e, "modelFile");
                        }
                    }
                }
            }
        }
        return {};
    }
    if (auto it = info.model_files.find(model); it != info.model_files.end()) {
        return it->second;
    }
    return event_path;
}

// DemoData fileParams belong to their demo: gc_p04_290 (f100) maps fxsd_sh_ene_fs01_f070.vfx to fxsd_sh_man_fs_f100.vfx while
// gc_p04_280 (f070) keeps it, so through the shared names gc_p04_280 played a man's footsteps from f100 (Play_man_fs_f100_01,
// 11.9 s) behind the player where floor_f070 has Lisa's (Play_ene_fs_f070_01, 8.9 s, at 2307 on the rear channels while she
// still stands at the railing in gc_p04_120); gc_p01_010 and gc_p01_020 map fxsd_sh_dooropn01.vfx to door_open01 and 02
std::string DemoSystem::FilePath(const PlayingDemo& demo, uint64_t code) const {
    if (demo.info) {
        for (const auto& [key, path] : demo.info->file_params) {
            if (Code(key.c_str()) == code) {
                return path;
            }
        }
    }
    auto it = string_names_.find(code);
    return it == string_names_.end() ? std::string() : it->second;
}

std::string DemoSystem::EventModelName(const PlayingDemo& demo, const anim::StreamEvent& e) const {
    const uint64_t code = e.StringArg(0);
    if (auto it = string_names_.find(code); it != string_names_.end() && !it->second.empty()) {
        return it->second;
    }
    std::string name;
    if (demo.stream) {
        for (const anim::StreamActor& actor : demo.stream->file.Actors()) {
            if (Code(actor.target.c_str()) == code) {
                name = actor.target;
            }
        }
    }
    return name;
}

DemoSystem::ModelSource DemoSystem::ResolveModel(const PlayingDemo& demo, const std::string& name, bool skinned) {
    ModelSource source;
    std::string event_path;
    if (auto it = demo.info->model_files.find(name); it != demo.info->model_files.end()) {
        event_path = it->second;
    }
    const bool owned = demo.info->model_parts.contains(name) || !event_path.empty();
    source.fmdl = ModelPath(demo, name, event_path);
    source.drawn = owned && !source.fmdl.empty();
    if (skinned && name == demo.player_model) {
        // 0x786A70: a CreateModel whose name is a DemoControlCharacterDesc binds that character's own model (0x7920E0) and
        // does not load modelFiles, which gc_p00_022 fills with plr0_main0_def.fmdl and the other first-person demos leave
        // empty. 0x945620 acts on the same model, which exists before the demo: gc_p00_020 shows MESH_arm before its model event
        LoadPlayerParts();
        source.player_own = !player_model_file_.empty();
        source.fmdl = player_model_file_;
        source.drawn = source.player_own;
    }
    return source;
}

// States 0 and 1 (0xB134B0, 0xB13610) hold a demo until its streams are ready, the models of a resident demo are in memory with
// its package and the player's own model exists before any demo. The port builds a model at its first use, so the models of the
// stream's CreateModel events are built here, before the start: building plr0 at gc_p00_022's frame 0 stalled the first boot for
// 0.68 s while its frame 0 fade in ran.
void DemoSystem::PreloadModels(PlayingDemo& demo) {
    if (!demo.stream) {
        return;
    }
    for (const anim::StreamEvent* e : demo.stream->events) {
        if (e->type != anim::kEventCreateModel) {
            continue;
        }
        const std::string name = EventModelName(demo, *e);
        if (name.empty()) {
            continue;
        }
        const bool skinned = e->ints.size() >= 2 && e->ints[1] == 1;
        const ModelSource source = ResolveModel(demo, name, skinned);
        if (source.drawn) {
            game_.Models().Get(source.fmdl);
        }
        if (skinned && !source.fmdl.empty() && LoadSkeleton(source.fmdl) && source.drawn) {
            LoadHelpBones(HelpBonePath(demo, name));
            LoadSimRig(PartsPath(demo, name), source.fmdl);
        }
    }
}

void DemoSystem::CreateModel(PlayingDemo& demo, const anim::StreamEvent& e) {
    const std::string name = EventModelName(demo, e);
    if (name.empty() || FindModel(demo, name)) {
        return;
    }
    DemoModel model;
    model.name = name;
    model.skinned = e.ints.size() >= 2 && e.ints[1] == 1;
    const DemoStreamData& stream = *demo.stream;
    if (model.skinned) {
        model.root_actor = stream.FindActor(anim::ActorKind::ModelRoot, name);
        model.skeleton_actor = stream.FindActor(anim::ActorKind::Skeleton, name);
        model.points_actor = stream.FindActor(anim::ActorKind::MotionPoints, name);
    } else {
        model.root_actor = stream.FindActor(anim::ActorKind::Locator, name);
    }
    const ModelSource source = ResolveModel(demo, name, model.skinned);
    model.fmdl = source.fmdl;
    model.drawn = source.drawn;
    model.player_own = source.player_own;
    if (model.drawn) {
        if (const ModelEntry* entry = game_.Models().Get(model.fmdl)) {
            model.mesh = entry->mesh.get();
        }
    }
    if (model.skinned && !model.fmdl.empty()) {
        model.skeleton = LoadSkeleton(model.fmdl);
    } else if (model.skinned && name == demo.player_model) {
        model.skeleton = LoadSkeleton("/Assets/sh/chara/plr/Scenes/plr0_main0_def.fmdl");
    }
    if (model.skeleton && model.drawn) {
        model.help_bones = LoadHelpBones(HelpBonePath(demo, name));
        model.sim_rig = LoadSimRig(PartsPath(demo, name), model.fmdl);
    }
    if (model.sim_rig) {
        const uint64_t code = Code(name.c_str());
        for (const anim::StreamEvent* e : stream.events) {
            if (e->functor == kFunctorSimulation && (e->String(kKeyDemoObjectName) & kStrCode64Mask) == code) {
                model.sim_suspend_sections.emplace_back(e->Start(), e->End());
            }
        }
        LogInfo("demo: {} simulates model {} with {} ({} dynamic bodies, {} suspend sections)", demo.demo_id, name, model.sim_rig->Name(),
                model.sim_rig->DynamicCount(), model.sim_suspend_sections.size());
    }
    if (model.skeleton && model.skeleton_actor >= 0) {
        const anim::StreamActor& skel = stream.file.Actors()[static_cast<size_t>(model.skeleton_actor)];
        int missing = 0;
        for (const anim::ActorUnit& u : skel.units) {
            model.bone_of_unit.push_back(model.skeleton->Find(u.hash));
            missing += model.bone_of_unit.back() < 0 ? 1 : 0;
        }
        if (missing) {
            LogWarn("demo: {} model {}: {} bone channels unresolved", demo.demo_id, name, missing);
        }
        model.pose.Reset(model.skeleton->Size());
    }
    LogInfo("demo: {} creates {} model {} {}{}{}", demo.demo_id, model.skinned ? "skinned" : "static", name,
            model.player_own ? "(player's own body) " + model.fmdl : model.fmdl.empty() ? "(player's own)" : model.fmdl, model.drawn ? (model.mesh ? "" : " (mesh missing)") : " (not drawn)",
            model.help_bones ? std::format(", {} help bones", model.help_bones->Entries().size()) : "");
    demo.models.push_back(std::move(model));
}

void DemoSystem::RunEvent(PlayingDemo& demo, const anim::StreamEvent& e) {
    switch (e.type) {
    case anim::kEventExecCommand: {
        // 0xB20960: footer flag 2 marks a skip event, run only when the demo is skipped before its frame (0xB212A0, 0xB21570)
        if (e.footer_flags & 2) {
            demo.skip_events.push_back(&e);
            break;
        }
        const bool ranged = e.interpolated > 0 && e.End() > e.Start();
        RunFunctor(demo, e, ranged ? 0.0f : 1.0f, true);
        if (ranged) {
            demo.interpolations.push_back({&e, e.Start(), e.End()});
        }
        break;
    }
    case anim::kEventCreateCamera:
        demo.camera_created = true;
        break;
    case anim::kEventDeleteCamera:
        UpdateCamera(demo);
        demo.camera_created = false;
        // 0x79A910 (from 0x793AB0) disables a deleted camera at once only when its body is not the current one; the current body's
        // camera gets +0xb1 instead, and the camera selector (0x7DF8B0) draws it in this frame and disables it after that
        demo.camera_linger = demo.camera_valid ? kGameFrame : 0.0;
        if (!demo.player_taken) {
            HandCameraBack(demo);
        }
        break;
    case anim::kEventCreateModel:
        CreateModel(demo, e);
        break;
    case anim::kEventDeleteModel:
        std::erase_if(demo.models, [&](const DemoModel& m) { return Code(m.name.c_str()) == e.StringArg(0); });
        break;
    case anim::kEventCreateLocator: {
        auto it = string_names_.find(e.StringArg(0));
        std::string name = it == string_names_.end() ? std::string() : it->second;
        if (name.empty() && demo.stream) {
            for (const anim::StreamActor& actor : demo.stream->file.Actors()) {
                if (actor.kind == anim::ActorKind::Locator && Code(actor.target.c_str()) == e.StringArg(0)) {
                    name = actor.target;
                }
            }
        }
        if (!name.empty()) {
            demo.locators.insert(name);
        }
        break;
    }
    case anim::kEventDeleteLocator:
        std::erase_if(demo.locators, [&](const std::string& n) { return Code(n.c_str()) == e.StringArg(0); });
        break;
    case anim::kEventVisibleModel:
        for (DemoModel& m : demo.models) {
            if (Code(m.name.c_str()) == e.StringArg(0)) {
                m.visible = e.ints.empty() || e.ints[0] != 0;
            }
        }
        break;
    case anim::kEventVisibleMesh:
        if (!demo.player_model.empty() && Code(demo.player_model.c_str()) == e.StringArg(0)) {
            SetPlayerMeshVisible(demo, e.StringArg(1), e.ints.empty() || e.ints[0] != 0, false);
            break;
        }
        for (DemoModel& m : demo.models) {
            if (Code(m.name.c_str()) == e.StringArg(0)) {
                if (!e.ints.empty() && e.ints[0] == 0) {
                    m.hidden_meshes.insert(e.StringArg(1));
                } else {
                    m.hidden_meshes.erase(e.StringArg(1));
                }
                LogInfo("demo: {} mesh {:#x} of {} {}", demo.demo_id, e.StringArg(1), m.name,
                        m.hidden_meshes.contains(e.StringArg(1)) ? "hidden" : "shown");
            }
        }
        break;
    default:
        break;
    }
}

void DemoSystem::RunFunctor(PlayingDemo& demo, const anim::StreamEvent& e, float t, bool first) {
    const uint64_t f = e.functor;
    if (demo.Held()) {
        // scenery: no messages, player, sounds, fades, texts, captions or handy light (PlayingDemo::hold_frame)
        static const uint64_t ui_create = Code("DemoUiFunctor_Create");
        static const uint64_t ui_start = Code("DemoUiFunctor_Start");
        if (f == kFunctorMessage || f == kFunctorTakePlayer || f == kFunctorReleasePlayer || f == kFunctorPostSound ||
            f == kFunctorSoundLengthA || f == kFunctorSoundLengthB || f == kFunctorSoundId || f == kFunctorFadeIn || f == kFunctorFadeOut ||
            f == kFunctorFadeColor || f == ui_create || f == ui_start || f == kFunctorUiText || f == kFunctorSubtitle ||
            f == kFunctorHandyLightOn || f == kFunctorHandyLightOff || f == kFunctorHandyLightLumen || f == kFunctorPlayerMesh ||
            f == kFunctorBlurEnable) {
            return;
        }
    }
    DemoCameraParams& cam = demo.camera_params;
    auto camera_value = [&](uint32_t key, float& target, uint32_t bit) {
        const anim::ExecParam* p = e.Param(key);
        if ((e.footer_flags & 8) && p && p->interpolated) {
            // 0xB2CD90, 0xB36270: footer flag 8 starts the blend at the camera's value (start getters of 0x78D610), not the stored start
            auto [it, inserted] = demo.start_values.try_emplace({&e, key}, target);
            if (first) {
                it->second = target;
            }
            target = it->second + (e.Float(key, target, true) - it->second) * t;
        } else {
            target = Param(e, key, target, t);
        }
        cam.set_mask |= bit;
    };
    if (f == kFunctorMessage) {
        if (!first) {
            return;
        }
        const std::string name = MessageName(e.String(kKeyMessage));
        if (name == "Play" || name == "PlayEnd" || name == "Finish" || name == "Interrupt" || name == "Skip" || name == "Start") {
            return;
        }
        if (name == "FinishMotion") {
            // one FinishMotion a play: the data's stands for the runtime one (demo.md Playback), which ran every FinishMotion script
            // a second time 3.5 frames later, so gc_p05_010 rang a second bell sequence 115 ms behind the first on f160
            demo.finish_motion_sent = true;
        }
        Notify(demo, name);
        return;
    }
    if (f == kFunctorTakePlayer) {
        if (first && !demo.player_model.empty() && !demo.player_taken && !demo.player_released) {
            demo.player_taken = true;
            LogInfo("demo: {} takes the player ({}) at frame {:.1f}", demo.demo_id, demo.player_model, demo.frame);
        }
        return;
    }
    if (f == kFunctorReleasePlayer) {
        if (first) {
            ReleasePlayer(demo, "release functor", static_cast<double>(e.Start()));
        }
        return;
    }
    if (f == kFunctorPostSound || f == kFunctorSoundLengthA || f == kFunctorSoundLengthB || f == kFunctorSoundId) {
        if (!first) {
            return;
        }
        const uint32_t id = static_cast<uint32_t>(e.Int(f == kFunctorSoundId ? kKeyEventName : kKeyEventId, 0));
        if (id != 0 && id != 0x811C9DC5u) {
            LogInfo("demo: {} sound {:#x} at frame {}{}", demo.demo_id, id, e.Start(), e.length >= 0 ? std::format(" (length {})", e.length) : "");
            game_.PostSoundId(id);
        }
        return;
    }
    if (f == kFunctorFocusDistance) {
        camera_value(kKeyFocusDistance, cam.focus_distance, 1u << 0);
    } else if (f == kFunctorAperture) {
        camera_value(kKeyAperture, cam.aperture, 1u << 1);
    } else if (f == kFunctorFocalLength) {
        camera_value(kKeyFocalLength, cam.functor_focal_length, 1u << 2);
    } else if (f == kFunctorShutterSpeed) {
        camera_value(kKeyShutterSpeed, cam.shutter_speed, 1u << 3);
    } else if (f == kFunctorExposureCompensation) {
        camera_value(kKeyExposureCompensation, cam.exposure_compensation, 1u << 4);
    } else if (f == kFunctorMinExposure) {
        camera_value(kKeyMinExposure, cam.min_exposure, 1u << 5);
    } else if (f == kFunctorMaxExposure) {
        camera_value(kKeyMaxExposure, cam.max_exposure, 1u << 6);
    } else if (f == kFunctorBloomSize) {
        camera_value(kKeyBloomSize, cam.bloom_size, 1u << 7);
    } else if (f == kFunctorNearClip) {
        camera_value(kKeyNearClip, cam.near_clip, 1u << 8);
    } else if (f == kFunctorFarClip) {
        camera_value(kKeyFarClip, cam.far_clip, 1u << 9);
    } else if (f == kFunctorKeyValue) {
        camera_value(kKeyKeyValue, cam.key_value, 1u << 10);
    } else if (f == kFunctorBloomWeight) {
        camera_value(kKeyBloomWeight, cam.bloom_weight, 1u << 11);
    } else if (f == kFunctorBloomExtraction) {
        camera_value(kKeyBloomExtraction, cam.bloom_extraction, 1u << 12);
    } else if (f == kFunctorAddExposure) {
        for (int i = 0; i < 6; ++i) {
            camera_value(kKeyAddExposure[i], cam.add_exposure[i], 1u << 13);
        }
    } else if (f == kFunctorFadeIn || f == kFunctorFadeOut) {
        if (first) {
            const float seconds = e.Float(kKeyFadeTime, 1.0f);
            LogInfo("demo: {} fade {} {:.2f} s at frame {}", demo.demo_id, f == kFunctorFadeIn ? "in" : "out", seconds, e.Start());
            if (f == kFunctorFadeIn) {
                game_.Effects().CallFadeIn(seconds);
            } else {
                game_.Effects().CallFadeOut(seconds);
            }
        }
    } else if (f == kFunctorFadeColor) {
        const glm::vec4 c = glm::clamp(ParamVector(e, kKeyColor, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), t), 0.0f, 1.0f) * 255.0f;
        game_.Effects().SetFadeColor(static_cast<int>(c.r + 0.5f), static_cast<int>(c.g + 0.5f), static_cast<int>(c.b + 0.5f),
                                     static_cast<int>(c.a + 0.5f));
    } else if (f == kFunctorBlurEnable) {
        if (first) {
            game_.Effects().full_screen_blur = e.Bool(kKeyEnable, false);
            LogInfo("demo: {} full screen blur {}", demo.demo_id, game_.Effects().full_screen_blur ? "on" : "off");
        }
    } else if (f == kFunctorBlurBand) {
        game_.Effects().blur_fetch_band = Param(e, kKeyPixelBand, game_.Effects().blur_fetch_band, t);
    } else if (f == kFunctorBlurRate) {
        game_.Effects().blur_blend_rate = Param(e, kKeyBlendRate, game_.Effects().blur_blend_rate, t);
    } else if (f == kFunctorFilmGrain) {
        // 0x931480: film grain strength, FilmGrain *(0x1C91C38) +0x38 clamped to [0, 1]
        ScreenEffects& fx = game_.Effects();
        fx.film_grain_strength = std::clamp(Param(e, kKeyFilmGrain, fx.film_grain_strength, t), 0.0f, 1.0f);
    } else if (f == kFunctorBandingCancellerOn || f == kFunctorBandingCancellerOff) {
        // 0x9312C0: ShColourBandingCanceller *(0x1C913A8) +0x78
        if (first) {
            game_.Effects().colour_banding_canceller = f == kFunctorBandingCancellerOn;
            LogInfo("demo: {} colour banding canceller {} at frame {}", demo.demo_id, f == kFunctorBandingCancellerOn ? "on" : "off", e.Start());
        }
    } else if (f == kFunctorScreenTone) {
        demo.screen.tone_set = true;
        demo.screen.color_scale = ParamVector(e, kKeyColorScale, demo.screen.color_scale, t);
        demo.screen.start_slope = Param(e, kKeyStartSlope, demo.screen.start_slope, t);
        demo.screen.end_slope = Param(e, kKeyEndSlope, demo.screen.end_slope, t);
    } else {
        const LightFunctors& spot = SpotFunctors();
        const LightFunctors& point = PointFunctors();
        auto matches = [&](const LightFunctors& l) {
            return f == l.create || f == l.color || f == l.world || f == l.enable || f == l.shadow || f == l.range || f == l.shadow_angle ||
                   f == l.view_bias || f == l.bias || f == l.angle;
        };
        const bool is_spot = matches(spot);
        const bool is_point = matches(point);
        const bool light_extra =
            f == kFunctorLightSize || f == kFunctorLightShadowStrength || f == kFunctorPointLightShadowStrength || f == kFunctorLightSpecular;
        if (is_spot || is_point || light_extra) {
            const uint64_t light_code = e.String(kKeyLightName);
            auto name_it = string_names_.find(light_code);
            const std::string name = name_it != string_names_.end() ? name_it->second : std::format("{:#x}", light_code);
            DemoLight* light = FindLight(demo, name);
            if (!light) {
                DemoLight created;
                created.demo_id = demo.demo_id;
                created.name = name;
                auto kind = demo.info->setup_lights.find(name);
                created.point = kind != demo.info->setup_lights.end() ? kind->second == "Point" : is_point;
                demo.lights.push_back(std::move(created));
                light = &demo.lights.back();
            }
            const LightFunctors& lf = light->point ? point : spot;
            if (f == lf.create) {
                light->light_type = e.Int(kKeyLightType, light->light_type);
                const uint64_t locator = e.String(kKeyLocatorName);
                auto it = string_names_.find(locator);
                light->locator = it != string_names_.end() ? it->second : std::string();
                // 0xB285A0: the end of a CreateLight section removes the light (0xB244C0, 0xB122A0)
                light->end_frame = e.End() > e.Start() ? e.End() : -1;
            } else if (f == lf.color) {
                light->color = ParamVector(e, kKeyColor, light->color, t);
                light->temperature = Param(e, kKeyTemperature, light->temperature, t);
                light->lumen = Param(e, kKeyLumen, light->lumen, t);
                light->deflection = Param(e, kKeyDeflection, light->deflection, t);
                light->power_scale = Param(e, kKeyPowerScale, light->power_scale, t);
                light->attenuation_exponent = Param(e, kKeyAttenuation, light->attenuation_exponent, t);
                light->penumbra = Param(e, kKeyPenumbra, light->penumbra, t);
                light->umbra = Param(e, kKeyUmbra, light->umbra, t);
            } else if (f == lf.world) {
                light->local_rotation = anim::ToQuat(ParamVector(e, kKeyRotation, anim::FromQuat(light->local_rotation), t));
                light->local_translation = glm::vec3(ParamVector(e, kKeyTranslation, glm::vec4(light->local_translation, 1.0f), t));
            } else if (f == lf.enable) {
                light->enabled = e.Bool(kKeyEnable, light->enabled);
            } else if (f == lf.shadow) {
                light->shadow = e.Bool(kKeyShadow, light->shadow);
            } else if (f == lf.range) {
                light->inner_range = Param(e, kKeyInnerRadius, light->inner_range, t);
                light->outer_range = Param(e, kKeyOuterRadius, light->outer_range, t);
            } else if (f == lf.shadow_angle) {
                light->shadow_penumbra = Param(e, kKeyShadowPenumbra, light->shadow_penumbra, t);
                light->shadow_umbra = Param(e, kKeyShadowUmbra, light->shadow_umbra, t);
                light->shadow_attenuation_exponent = Param(e, kKeyShadowAttenuation, light->shadow_attenuation_exponent, t);
            } else if (f == lf.view_bias) {
                light->view_bias = Param(e, kKeyViewBias, light->view_bias, t);
            } else if (f == lf.bias) {
                light->bias = Param(e, kKeyBias, light->bias, t);
            } else if (f == lf.angle) {
                light->penumbra = Param(e, kKeyPenumbra, light->penumbra, t);
                light->umbra = Param(e, kKeyUmbra, light->umbra, t);
            } else if (f == kFunctorLightSpecular) {
                light->specular = e.Bool(kKeySpecular, light->specular);
            } else if (f == kFunctorLightSize) {
                light->light_size = Param(e, kKeyLightSize, light->light_size, t);
            } else if (f == kFunctorLightShadowStrength || f == kFunctorPointLightShadowStrength) {
                light->shadow_strength = Param(e, kKeyShadowStrength, light->shadow_strength, t);
            }
            return;
        }
        static const uint64_t kEffectCreate = Code("FxEffectCreateEventFunctor");
        static const uint64_t kEffectConnect = Code("EffectConnectToNullFunctor");
        static const uint64_t kConstrain = Code("DemoConstraintFunctor_ConstrainObject");
        static const uint64_t kUiCreate = Code("DemoUiFunctor_Create");
        static const uint64_t kUiStart = Code("DemoUiFunctor_Start");
        if (f == kFunctorSimulation) {
            // applied by SimulateModel over the event's section
            if (first) {
                LogInfo("demo: {} physics of {:#x} off from frame {} to {}", demo.demo_id, e.String(kKeyDemoObjectName) & kStrCode64Mask, e.Start(), e.End());
            }
            return;
        }
        if (f == kFunctorPlayerMesh) {
            if (first) {
                SetPlayerMeshVisible(demo, e.String(kKeyMeshName), e.Bool(kKeyMeshOn, true), true);
            }
            return;
        }
        // 0x9459B0: the player's handy light (player command 0x3B841BD55DF5) vf+0x10 SetEnable, vf+0x20 SetLumen, as the
        // Lua command SetHandyLight; gc_p00_030 turns the flashlight on at frame 228 when the player picks it up
        if (f == kFunctorHandyLightOn || f == kFunctorHandyLightOff || f == kFunctorHandyLightLumen) {
            if (!first) {
                return;
            }
            HandyLight& light = game_.GetPlayer().handy_light;
            if (f == kFunctorHandyLightLumen) {
                const auto p = std::find_if(e.params.begin(), e.params.end(), [](const anim::ExecParam& p) { return p.type == anim::ParamType::Float; });
                if (p != e.params.end()) {
                    light.lumen = e.Float(p->key, light.lumen);
                }
            } else {
                light.enable = f == kFunctorHandyLightOn;
            }
            LogInfo("demo: {} handy light {} (lumen {}) at frame {}", demo.demo_id, light.enable ? "on" : "off", light.lumen, e.Start());
            return;
        }
        // 0x797650: starts another demo by name (gc_p02_080, the f120 bug screen, runs gc_p02_060, gc_p02_070 and gc_p02_100)
        if (f == kFunctorPlayDemo) {
            if (!first) {
                return;
            }
            const uint64_t code = e.String(kKeyDemoId) & kStrCode64Mask;
            const auto child = std::find_if(demos_.begin(), demos_.end(), [&](const auto& d) { return Code(d.first.c_str()) == code; });
            if (child == demos_.end()) {
                LogWarn("demo: {} plays unknown demo {:#x} at frame {}", demo.demo_id, code, e.Start());
            } else {
                LogInfo("demo: {} plays {} at frame {}", demo.demo_id, child->first, e.Start());
                pending_plays_.push_back({child->first, demo.demo_id, static_cast<double>(e.Start())});
            }
            return;
        }
        // 0x857C20: subtitle message msgId on the track of key (empty: the default track), white unless enableTextColorChange;
        // the preface texts and the ending's teaser note
        if (f == kFunctorSubtitle) {
            if (first) {
                const uint32_t message = static_cast<uint32_t>(e.String(kKeyMessageId));
                LogInfo("demo: {} subtitle {:#x} at frame {}", demo.demo_id, message, e.Start());
                game_.ShowCaption(message);
            }
            return;
        }
        if (f == kFunctorEffectKill) {
            if (!first) {
                return;
            }
            const uint64_t instance = e.String(kKeyInstanceName);
            size_t killed = 0;
            for (DemoEffect& effect : demo.effects) {
                if (effect.instance == instance) {
                    EndEffectSound(effect);
                    ++killed;
                }
            }
            std::erase_if(demo.effects, [&](const DemoEffect& effect) { return effect.instance == instance; });
            LogInfo("demo: {} kills effect instance {:#x} at frame {} ({} live)", demo.demo_id, instance, e.Start(), killed);
            return;
        }
        if (f == kEffectCreate || EffectCreateFunctor(f)) {
            if (!first) {
                return;
            }
            DemoEffect created;
            created.demo_id = demo.demo_id;
            created.instance = e.String(kKeyInstanceName);
            created.created_frame = e.Start();
            created.end_frame = e.End() > e.Start() ? e.End() : -1;
            if (f == kFunctorEffectCreateTimed) {
                const float seconds = Param(e, kKeyEffectTime, 0.0f, 1.0f);
                if (seconds > 0.0f) {
                    created.end_frame = e.Start() + static_cast<int>(std::lround(seconds * 60.0f));
                }
            }
            demo.effects.push_back(std::move(created));
            DemoEffect* effect = &demo.effects.back();
            effect->effect_name = e.String(kKeyEffectName);
            effect->file = e.String(kKeyEffectFile);
            effect->file_path = FilePath(demo, effect->file);
            if (demo.Held() && effect->file_path.find("/filter/") != std::string::npos) {
                // the screen filters (the white fades of fx_sh_filfad*) are not scenery
                demo.effects.pop_back();
                return;
            }
            effect->position = glm::vec3(e.Vector(kKeyPosition, glm::vec4(effect->position, 0.0f)));
            effect->rotation = glm::vec3(e.Vector(kKeyEffectRotation, glm::vec4(effect->rotation, 0.0f)));
            // any effect with an FxSoundCallProgramEffectNode posts its event when the instance starts, not only the fxsd files:
            // fx_sh_viwdis01_s1 (gc_p02_060, the f120 screen glitch) loops Play_sfx_bug_loop_01 until gc_p02_060 kills it
            std::string sound;
            if (!effect->file_path.empty()) {
                if (auto bytes = game_.GetVfs().ReadFile(effect->file_path)) {
                    vfx::SoundNode node;
                    if (vfx::ReadSoundNode(*bytes, node)) {
                        sound = node.play;
                        effect->sound_stop = node.stop;
                        effect->sound_stop_playing = node.stop_playing;
                        effect->sound_stop_fade = node.fade;
                        effect->sound_stop_curve = node.curve;
                    } else if (effect->file_path.find("/sound/") != std::string::npos) {
                        sound = SoundVfxEvent(*bytes);
                        effect->sound_stop_playing = true;
                        effect->sound_stop_fade = 0.3f;
                    }
                }
            }
            LogInfo("demo: {} effect {} at frame {}{}", demo.demo_id,
                    effect->file_path.empty() ? std::format("{:#x}", effect->file) : effect->file_path, e.Start(),
                    sound.empty() ? "" : " sound " + sound);
            if (!sound.empty()) {
                effect->sound_event = sound;
                effect->sound_pending = true;
            }
            return;
        }
        // 0x7858A0: offset of an effect connected to a null, translation then rotation in degrees (0xB10360 flags 8 and 0x10)
        if (f == kFunctorEffectOffset) {
            if (DemoEffect* effect = first ? FindEffect(demo, e.String(kKeyInstanceName)) : nullptr) {
                const glm::vec3 r = glm::radians(glm::vec3(e.Vector(kKeyOffsetRotation)));
                effect->connect_offset = glm::vec3(e.Vector(kKeyOffsetTranslation));
                effect->connect_rotation = glm::angleAxis(r.y, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(r.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
                                           glm::angleAxis(r.z, glm::vec3(0.0f, 0.0f, 1.0f));
            }
            return;
        }
        if (f == kEffectConnect) {
            if (DemoEffect* effect = FindEffect(demo, e.String(kKeyInstanceName))) {
                auto it = string_names_.find(e.String(kKeyTargetName));
                std::string locator = it != string_names_.end() ? it->second : std::string();
                if (locator.empty() && demo.stream) {
                    for (const anim::StreamActor& actor : demo.stream->file.Actors()) {
                        if (Code(actor.target.c_str()) == e.String(kKeyTargetName)) {
                            locator = actor.target;
                        }
                    }
                }
                effect->null_locator = locator;
            }
            return;
        }
        if (f == kConstrain) {
            const uint64_t light_code = e.String(kKeyLightName);
            auto name_it = string_names_.find(light_code);
            std::string locator;
            for (const anim::StreamActor& actor : demo.stream->file.Actors()) {
                if (Code(actor.target.c_str()) == e.String(kKeyObjectName)) {
                    locator = actor.target;
                }
            }
            if (name_it != string_names_.end()) {
                DemoLight* light = FindLight(demo, name_it->second);
                if (!light) {
                    DemoLight created;
                    created.demo_id = demo.demo_id;
                    created.name = name_it->second;
                    auto kind = demo.info->setup_lights.find(created.name);
                    created.point = kind != demo.info->setup_lights.end() && kind->second == "Point";
                    demo.lights.push_back(std::move(created));
                    light = &demo.lights.back();
                }
                light->constraint_locator = locator;
                light->constraint_offset = glm::vec3(e.Vector(kKeyPositionOffset));
                light->constraint_rotation_offset = glm::vec3(e.Vector(kKeyRotationOffset));
            }
            return;
        }
        if (f == kUiCreate || f == kUiStart || f == kFunctorUiText) {
            if (first) {
                DemoUiEvent ui;
                ui.demo_id = demo.demo_id;
                ui.functor = f;
                ui.graph = e.String(kKeyLightName);
                ui.file = e.String(kKeyUiFile);
                ui.text = e.String(kKeyUiText);
                ui.frame = e.Start();
                ui.file_path = FilePath(demo, ui.file);
                const char* kind = f == kUiCreate ? "create" : f == kUiStart ? "start" : "text";
                LogInfo("demo: {} UI {} at frame {} graph {:#x} file {} text {:#x}", demo.demo_id, kind, e.Start(), ui.graph,
                        ui.file_path.empty() ? std::format("{:#x}", ui.file) : ui.file_path, ui.text);
                ui_events_.push_back(std::move(ui));
                if (ui_events_.size() > 256) {
                    ui_events_.erase(ui_events_.begin());
                }
            }
            return;
        }
        if (e.Has(kKeyInstanceName)) {
            if (DemoEffect* effect = FindEffect(demo, e.String(kKeyInstanceName))) {
                for (const anim::ExecParam& p : e.params) {
                    if (p.type == anim::ParamType::Vector4 || p.type == anim::ParamType::Vector3 || p.type == anim::ParamType::Color) {
                        effect->parameters[(f << 16) ^ p.key] = ParamVector(e, p.key, glm::vec4(0.0f), t);
                    }
                }
                return;
            }
        }
        std::vector<float>& values = demo.screen.values[f];
        values.clear();
        uint64_t target = 0;
        for (const anim::ExecParam& p : e.params) {
            if (p.type == anim::ParamType::Float || p.type == anim::ParamType::Bool || p.type == anim::ParamType::Int) {
                values.push_back(p.type == anim::ParamType::Float ? Param(e, p.key, 0.0f, t) : static_cast<float>(e.Int(p.key)));
            } else if (p.type == anim::ParamType::Color || p.type == anim::ParamType::Vector4 || p.type == anim::ParamType::Vector3) {
                const glm::vec4 v = ParamVector(e, p.key, glm::vec4(0.0f), t);
                values.insert(values.end(), {v.x, v.y, v.z, v.w});
            } else if (p.type == anim::ParamType::String && !target) {
                target = e.String(p.key);
            }
        }
        if (target) {
            demo.screen.named_values[{f, target}] = values;
        }
        if (first && demo.logged_functors.insert(f).second) {
            LogInfo("demo: {} functor {:#x} at frame {} ({} params) recorded", demo.demo_id, f, e.Start(), e.params.size());
        }
    }
}

void DemoSystem::UpdateInterpolations(PlayingDemo& demo) {
    for (const DemoInterpolation& i : demo.interpolations) {
        const float span = static_cast<float>(std::max(1, i.end - i.start));
        const float t = std::clamp(static_cast<float>(demo.frame - i.start) / span, 0.0f, 1.0f);
        RunFunctor(demo, *i.event, t, false);
    }
    std::erase_if(demo.interpolations, [&](const DemoInterpolation& i) { return demo.frame >= i.end; });
}

void DemoSystem::UpdateActors(PlayingDemo& demo) {
    if (!demo.stream) {
        return;
    }
    const DemoStreamData& stream = *demo.stream;
    const glm::mat4 origin = demo.Transform();
    const uint32_t block_frame = static_cast<uint32_t>(demo.frame);
    for (DemoModel& m : demo.models) {
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 translation(0.0f);
        if (m.root_actor >= 0) {
            m.active = stream.file.ActorActive(static_cast<size_t>(m.root_actor), block_frame);
            if (stream.SampleTransform(static_cast<size_t>(m.root_actor), demo.frame, rotation, translation)) {
                m.world = origin * Compose(rotation, translation);
            }
        }
        if (!m.skeleton || m.skeleton_actor < 0) {
            continue;
        }
        const bool active = stream.file.ActorActive(static_cast<size_t>(m.skeleton_actor), block_frame);
        if (!active && !m.skin.empty()) {
            continue;
        }
        const anim::StreamActor& skel = stream.file.Actors()[static_cast<size_t>(m.skeleton_actor)];
        glm::vec4 v;
        for (size_t u = 0; u < skel.units.size() && u < m.bone_of_unit.size(); ++u) {
            const int bone = m.bone_of_unit[u];
            if (bone < 0) {
                continue;
            }
            if (stream.Sample(skel.units[u].rotation, demo.frame, v)) {
                m.pose.rotation[static_cast<size_t>(bone)] = anim::ToQuat(v);
            }
            if (stream.Sample(skel.units[u].translation, demo.frame, v)) {
                m.pose.offset[static_cast<size_t>(bone)] = glm::vec3(v);
            }
        }
        anim::ComputeBoneWorld(*m.skeleton, m.pose, m.bone_world);
        if (m.help_bones) {
            m.help_bones->Apply(*m.skeleton, m.bone_world);
        }
        SimulateModel(demo, m);
        anim::ComputeSkin(*m.skeleton, m.bone_world, m.skin);
    }
    std::map<std::string, glm::mat4> locators;
    for (size_t a = 0; a < stream.file.Actors().size(); ++a) {
        const anim::StreamActor& actor = stream.file.Actors()[a];
        if (actor.kind != anim::ActorKind::Locator) {
            continue;
        }
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 translation(0.0f);
        if (stream.SampleTransform(a, demo.frame, rotation, translation)) {
            locators[actor.target] = origin * Compose(rotation, translation);
        }
    }
    for (DemoLight& light : demo.lights) {
        glm::mat4 local = Compose(light.local_rotation, light.local_translation);
        auto it = locators.find(light.constraint_locator.empty() ? light.locator : light.constraint_locator);
        if (it != locators.end()) {
            light.world = it->second * glm::translate(glm::mat4(1.0f), light.constraint_offset);
        } else {
            light.world = origin * local;
        }
    }
    for (DemoEffect& effect : demo.effects) {
        if (effect.end_frame >= 0 && demo.frame >= effect.end_frame) {
            EndEffectSound(effect);
        }
    }
    std::erase_if(demo.effects, [&](const DemoEffect& effect) { return effect.end_frame >= 0 && demo.frame >= effect.end_frame; });
    for (DemoEffect& effect : demo.effects) {
        auto it = locators.find(effect.null_locator);
        effect.world = it != locators.end() ? it->second * Compose(effect.connect_rotation, effect.connect_offset)
                                            : origin * glm::translate(glm::mat4(1.0f), effect.position);
        if (effect.sound_pending && (demo.frame >= effect.created_frame + 1 || !effect.null_locator.empty())) {
            effect.sound_pending = false;
            StartEffectSound(effect);
        } else if (effect.sound_object) {
            auto* sound = dynamic_cast<GameSound*>(game_.Audio());
            if (!sound || !sound->System().IsPlaying(effect.sound_id)) {
                LogInfo("sound: {} ended at ({:.2f} {:.2f} {:.2f})", effect.sound_event, effect.world[3].x, effect.world[3].y, effect.world[3].z);
                effect.sound_object = 0;
                effect.sound_id = 0;
            } else {
                sound->System().SetObjectTransform(effect.sound_object, glm::vec3(effect.world[3]), glm::vec3(effect.world[2]));
            }
        }
    }
}

// The model's bone simulation in world space, stepped by the demo time that passed (the stream does not animate the simulated
// bones: the ending's plr0 stream has 91 of its 191 bones). The physics functor's sections take the units out of the world: the
// bones follow the animation (rigid on their parents) and the simulation restarts from the pose at the section end, as after
// a jump of the clock (a loop or a skip back).
void DemoSystem::SimulateModel(PlayingDemo& demo, DemoModel& m) {
    if (!m.sim_rig || !m.skeleton) {
        return;
    }
    if (!m.sim) {
        m.sim = std::make_shared<anim::SimPhysics>();
    }
    const bool suspended = std::any_of(m.sim_suspend_sections.begin(), m.sim_suspend_sections.end(), [&](const auto& section) {
        return demo.frame >= section.first && demo.frame < section.second;
    });
    if (suspended != m.sim_suspended) {
        m.sim_suspended = suspended;
        LogInfo("demo: {} {} simulation {} at frame {:.1f}", demo.demo_id, m.name, suspended ? "suspended" : "resumed", demo.frame);
    }
    const double elapsed = m.sim_frame >= 0.0 ? demo.frame - m.sim_frame : -1.0;
    m.sim_frame = demo.frame;
    if (suspended || elapsed < 0.0) {
        m.sim->Reset();
    }
    if (suspended) {
        return;
    }
    const float dt = static_cast<float>(std::max(0.0, elapsed) / kDemoFramesPerSecond);
    m.sim->Step(*m.sim_rig, *m.skeleton, m.bone_world, m.world, dt, anim::SimWind());
}

void DemoSystem::UpdateCamera(PlayingDemo& demo) {
    if (!demo.camera_created && demo.camera_linger > 0.0) {
        return;
    }
    if (!demo.stream || !demo.camera_created || demo.stream->camera_actor < 0) {
        demo.camera_valid = false;
        return;
    }
    const DemoStreamData& stream = *demo.stream;
    if (demo.frame <= 0.0 && !demo.camera_seen) {
        // the demo camera takes over after the demo clock's first advance, as in the original: the key at frame 0 can be the authoring
        // pose (gc_p00_022: (0, 1, 7) with a 48 mm lens, the shot starts at frame 1), which put the camera and the listener in the
        // passage for one tick at the first boot; explore_boot_rb goes from the player camera straight to the frame 1 pose
        demo.camera_valid = false;
        demo.blend_from = game_.GetPlayer().MakeCamera();
        demo.blend_from_set = true;
        return;
    }
    glm::quat rotation = demo.info->camera_start_rotation;
    glm::vec3 translation = demo.info->camera_start_translation;
    stream.SampleTransform(static_cast<size_t>(stream.camera_actor), demo.frame, rotation, translation);
    float focal = demo.info->camera_start_focal;
    glm::vec4 v;
    if (stream.camera_param_actor >= 0) {
        const anim::StreamActor& actor = stream.file.Actors()[static_cast<size_t>(stream.camera_param_actor)];
        if (!actor.units.empty() && !actor.units[0].tracks.empty() && stream.Sample(actor.units[0].tracks[0], demo.frame, v)) {
            focal = v.x;
        }
    }
    const glm::quat world_rotation = glm::normalize(demo.rotation * rotation);
    const glm::vec3 world_position = demo.translation + demo.rotation * translation;
    if (!demo.camera_valid && demo.info->camera_interp_type != 0 && demo.info->camera_interp_frames > 0) {
        const Camera game_camera = !demo.camera_seen && demo.blend_from_set ? demo.blend_from : game_.GetPlayer().MakeCamera();
        demo.blend_active = true;
        demo.blend_start = static_cast<int32_t>(demo.frame);
        demo.blend_frames = demo.info->camera_interp_frames;
        demo.blend_position = game_camera.position;
        demo.blend_rotation = CameraQuat(game_camera.yaw, game_camera.pitch);
        demo.blend_focal = kFilmHalfHeight / std::tan(game_camera.fov_y * 0.5f);
        demo.blend_offset = world_position - game_camera.position;
        LogInfo("demo: {} camera blends in from ({:.3f} {:.3f} {:.3f}) over {} frames (type {})", demo.demo_id, game_camera.position.x,
                game_camera.position.y, game_camera.position.z, demo.blend_frames, demo.info->camera_interp_type);
    }
    if (!demo.camera_seen) {
        float yaw = 0.0f;
        float pitch = 0.0f;
        YawPitch(world_rotation, yaw, pitch);
        LogInfo("demo: {} camera at frame {:.1f}: ({:.3f} {:.3f} {:.3f}) yaw {:.1f} pitch {:.1f} focal {:.1f}", demo.demo_id, demo.frame,
                world_position.x, world_position.y, world_position.z, glm::degrees(yaw), glm::degrees(pitch), focal);
    }
    demo.camera_valid = true;
    demo.camera_seen = true;
    demo.camera_rotation = world_rotation;
    demo.camera_position = world_position;
    demo.camera_focal = focal;
    demo.camera_params.focal_length = focal;
    if (demo.blend_active) {
        const float t = static_cast<float>((demo.frame - demo.blend_start) / std::max(1, demo.blend_frames));
        if (t >= 1.0f) {
            demo.blend_active = false;
        } else {
            const float w =
                CameraCurve(demo.info->camera_interp_type, t, demo.info->camera_interp_curve_rate, demo.info->camera_interp_scurve_center);
            demo.camera_rotation = anim::SlerpShortest(demo.blend_rotation, world_rotation, w);
            demo.camera_position = world_position - (1.0f - w) * demo.blend_offset;
            demo.camera_focal = demo.blend_focal + (focal - demo.blend_focal) * w;
        }
    }
}

bool DemoSystem::PlayerRootWorld(const PlayingDemo& demo, glm::vec3& position, glm::quat& rotation, double frame) const {
    if (!demo.stream || demo.player_model.empty()) {
        return false;
    }
    const int actor = demo.stream->FindActor(anim::ActorKind::ModelRoot, demo.player_model);
    glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 t(0.0f);
    if (actor < 0 || !demo.stream->SampleTransform(static_cast<size_t>(actor), frame >= 0.0 ? frame : demo.frame, r, t)) {
        return false;
    }
    position = demo.translation + demo.rotation * t;
    rotation = glm::normalize(demo.rotation * r);
    return true;
}

void DemoSystem::UpdatePlayer(PlayingDemo& demo) {
    if (demo.player_released && demo.camera_valid && !demo.camera_handed_back) {
        float yaw = 0.0f;
        float pitch = 0.0f;
        YawPitch(demo.camera_rotation, yaw, pitch);
        game_.GetPlayer().FollowCamera(yaw, pitch);
        return;
    }
    if (!demo.player_taken || demo.player_released || demo.frame <= 0.0) {
        return;
    }
    glm::vec3 position;
    glm::quat rotation;
    if (PlayerRootWorld(demo, position, rotation)) {
        game_.GetPlayer().Warp(position, FoxYawOf(rotation));
    }
}

// 0x914800: the demo model's transform at the functor frame
void DemoSystem::ReleasePlayer(PlayingDemo& demo, const char* reason, double functor_frame) {
    if (!demo.player_taken || demo.player_released) {
        return;
    }
    demo.player_released = true;
    demo.player_taken = false;
    glm::vec3 position;
    glm::quat rotation;
    bool placed = false;
    if (functor_frame >= 0.0 && PlayerRootWorld(demo, position, rotation, functor_frame)) {
        placed = true;
    } else if (const DemoControlCharacter* c = demo.info->ControlCharacter(demo.player_model)) {
        position = demo.translation + demo.rotation * c->translation;
        rotation = glm::normalize(demo.rotation * c->rotation);
        placed = true;
    } else {
        placed = PlayerRootWorld(demo, position, rotation);
    }
    if (placed) {
        game_.GetPlayer().Warp(position, FoxYawOf(rotation));
        LogInfo("demo: {} releases the player ({}) at frame {:.1f}: ({:.3f} {:.3f} {:.3f}) fox yaw {:.1f}", demo.demo_id, reason, demo.frame,
                position.x, position.y, position.z, glm::degrees(FoxYawOf(rotation)));
    } else {
        LogInfo("demo: {} releases the player ({}) at frame {:.1f} in place", demo.demo_id, reason, demo.frame);
    }
}

void DemoSystem::HandCameraBack(PlayingDemo& demo) {
    if (!demo.camera_seen || demo.camera_handed_back || demo.player_model.empty()) {
        return;
    }
    demo.camera_handed_back = true;
    float yaw = 0.0f;
    float pitch = 0.0f;
    YawPitch(demo.camera_rotation, yaw, pitch);
    game_.GetPlayer().HandCameraBack(demo.camera_position, yaw, pitch);
    LogInfo("demo: {} hands the camera back at ({:.4f} {:.4f} {:.4f}): yaw {:.1f} pitch {:.1f}", demo.demo_id, demo.camera_position.x,
            demo.camera_position.y, demo.camera_position.z, glm::degrees(yaw), glm::degrees(pitch));
}

void DemoSystem::Finish(PlayingDemo& demo, bool skipped) {
    if (demo.finished) {
        return;
    }
    if (!skipped) {
        RunEvents(demo);
    }
    if (!skipped && !demo.finish_motion_sent) {
        demo.finish_motion_sent = true;
        Notify(demo, "FinishMotion");
    }
    if (skipped) {
        for (const anim::StreamEvent* e : std::exchange(demo.skip_events, {})) {
            LogInfo("demo: {} skip event functor {:#x} (until frame {})", demo.demo_id, e->functor, e->length);
            RunFunctor(demo, *e, 1.0f, true);
        }
    }
    ReleasePlayer(demo, skipped ? "skip" : "end");
    HandCameraBack(demo);
    Notify(demo, "Finish");
    Notify(demo, "PlayEnd");
    if (skipped && demo.sound_id && game_.Audio()) {
        game_.Audio()->StopPlayingId(demo.sound_id, 0.2f);
    }
    for (DemoEffect& effect : demo.effects) {
        DetachEffectSound(effect, skipped ? 0.2f : -1.0f);
    }
    LogInfo("demo: {} finished at frame {:.1f}{}, game frame {}", demo.demo_id, demo.frame, skipped ? " (skipped)" : "", game_.Frame());
    demo.finished = true;
}

const PlayingDemo* DemoSystem::ActiveCamera() const {
    for (auto it = playing_.rbegin(); it != playing_.rend(); ++it) {
        if (!it->finished && it->camera_valid && !it->Held()) {
            return &*it;
        }
    }
    return nullptr;
}

// The camera selector (0x7DF8B0) renders the first enabled camera and, with none enabled, leaves the view's camera as it was. While a
// demo holds the player, his camera is not the one on screen: explore_boot_rb keeps gc_p00_022's last camera, rolled 72 degrees, until
// gc_p00_020's (3148-3166), and floor_f060 keeps gc_p00_030's last camera until the player is released two frames after it (2777-2778).
void DemoSystem::UpdateShownCamera() {
    if (const PlayingDemo* active = ActiveCamera()) {
        shown_camera_.valid = true;
        shown_camera_.demo_id = active->demo_id;
        shown_camera_.position = active->camera_position;
        shown_camera_.rotation = active->camera_rotation;
        shown_camera_.focal = active->camera_focal;
        shown_camera_.params = active->camera_params;
        holding_camera_ = false;
        return;
    }
    if (shown_camera_.valid && ControlsPlayer()) {
        if (!holding_camera_) {
            LogInfo("demo: {} last camera kept while the player is held, game frame {}", shown_camera_.demo_id,
                    game_.Frame());
        }
        holding_camera_ = true;
        return;
    }
    shown_camera_.valid = false;
    holding_camera_ = false;
}

bool DemoSystem::CameraWorld(glm::vec3& position, glm::quat& rotation, float& fov_y) const {
    float focal = 0.0f;
    if (const PlayingDemo* active = ActiveCamera()) {
        position = active->camera_position;
        rotation = active->camera_rotation;
        focal = active->camera_focal;
    } else if (holding_camera_) {
        position = shown_camera_.position;
        rotation = shown_camera_.rotation;
        focal = shown_camera_.focal;
    } else {
        return false;
    }
    fov_y = 2.0f * std::atan(kFilmHalfHeight / std::max(1.0f, focal));
    return true;
}

bool DemoSystem::CameraOverride(Camera& camera) const {
    glm::vec3 position;
    glm::quat rotation;
    float fov_y = 0.0f;
    if (!CameraWorld(position, rotation, fov_y)) {
        return false;
    }
    camera.position = position;
    YawPitch(rotation, camera.yaw, camera.pitch);
    camera.fov_y = fov_y;
    camera.near_plane = 0.05f;
    // a demo camera copies the game camera's clip distances at creation (0x798D40) unless its farClip functor ran
    if (const DemoCameraParams* params = CameraParams(); params && (params->set_mask & (1u << 9))) {
        camera.far_plane = params->far_clip;
    }
    ApplyRoll(camera, rotation);
    return true;
}

const DemoCameraParams* DemoSystem::CameraParams() const {
    if (const PlayingDemo* active = ActiveCamera()) {
        return &active->camera_params;
    }
    return holding_camera_ ? &shown_camera_.params : nullptr;
}

void DemoSystem::SetGameLens(float focus_distance, float aperture, float shutter_speed) {
    game_focus_distance_ = focus_distance;
    game_aperture_ = aperture;
    game_shutter_speed_ = shutter_speed;
}

const DemoScreenState* DemoSystem::ScreenState() const {
    for (auto it = playing_.rbegin(); it != playing_.rend(); ++it) {
        if (!it->finished && (it->screen.tone_set || !it->screen.values.empty())) {
            return &it->screen;
        }
    }
    return nullptr;
}

void DemoSystem::StartEffectSound(DemoEffect& effect) {
    const glm::vec3 position(effect.world[3]);
    auto* sound = dynamic_cast<GameSound*>(game_.Audio());
    if (!sound || !sound->Ready()) {
        game_.PostSound(effect.sound_event, position, true);
        return;
    }
    const uint64_t object = kSoundObjectBase + (next_sound_object_++ % kSoundObjectCount);
    for (PlayingDemo& other : playing_) {
        for (DemoEffect& e : other.effects) {
            if (e.sound_object == object) {
                e.sound_object = 0;
                e.sound_id = 0;
            }
        }
    }
    sound->System().RegisterObject(object, "DemoEffect");
    sound->System().SetObjectTransform(object, position, glm::vec3(effect.world[2]));
    sound->SetAreaSends(object, position);
    effect.sound_id = sound->System().PostEvent(effect.sound_event, object);
    effect.sound_object = effect.sound_id ? object : 0;
    LogInfo("sound: {} at ({:.2f} {:.2f} {:.2f}){}", effect.sound_event, position.x, position.y, position.z,
            effect.null_locator.empty() ? "" : " following " + effect.null_locator);
}

// 0xB6E0F0, the sound node's instance release when its effect instance goes (the kill functor 0x62B47A5B81EC removes it at once
// through the kill queue of 0xB5B4D0 and 0xB5B7F0): a sound still playing is stopped with the node's fade and curve when flag bit 2 is
// set (0x37CD447C), else soundStop is posted on the node's object when set, else it plays out
void DemoSystem::EndEffectSound(DemoEffect& effect) {
    effect.sound_pending = false;
    if (!effect.sound_object) {
        return;
    }
    auto* sound = dynamic_cast<GameSound*>(game_.Audio());
    if (sound && effect.sound_id && sound->System().IsPlaying(effect.sound_id)) {
        if (effect.sound_stop_playing) {
            sound->System().StopPlayingId(effect.sound_id, effect.sound_stop_fade, static_cast<audio::Interp>(std::min(effect.sound_stop_curve, 9u)));
        } else if (!effect.sound_stop.empty()) {
            sound->System().PostEvent(effect.sound_stop, effect.sound_object);
        }
    }
    LogInfo("sound: {} {} at ({:.2f} {:.2f} {:.2f})", effect.sound_event,
            effect.sound_stop_playing ? std::format("stopped over {:.2f} s (curve {})", effect.sound_stop_fade, effect.sound_stop_curve)
            : effect.sound_stop.empty() ? std::string("left to finish")
                                        : "stopped by " + effect.sound_stop,
            effect.world[3].x, effect.world[3].y, effect.world[3].z);
    effect.sound_object = 0;
    effect.sound_id = 0;
}

void DemoSystem::DetachEffectSound(DemoEffect& effect, float fade) {
    effect.sound_pending = false;
    if (!effect.sound_object) {
        return;
    }
    auto* sound = dynamic_cast<GameSound*>(game_.Audio());
    if (sound && effect.sound_id && fade >= 0.0f) {
        sound->System().StopPlayingId(effect.sound_id, fade);
    }
    LogInfo("sound: {} {} at ({:.2f} {:.2f} {:.2f})", effect.sound_event, fade >= 0.0f ? "stopped" : "left to finish", effect.world[3].x,
            effect.world[3].y, effect.world[3].z);
    effect.sound_object = 0;
    effect.sound_id = 0;
}

void DemoSystem::RebuildOutputs() {
    lights_.clear();
    effects_.clear();
    for (const PlayingDemo& demo : playing_) {
        for (const DemoLight& light : demo.lights) {
            if (light.end_frame < 0 || demo.frame < light.end_frame) {
                lights_.push_back(light);
            }
        }
        effects_.insert(effects_.end(), demo.effects.begin(), demo.effects.end());
    }
}

void DemoSystem::CollectDraws(std::vector<DrawItem>& out) const {
    for (const PlayingDemo& demo : playing_) {
        for (const DemoModel& m : demo.models) {
            if (!m.drawn || !m.visible || !m.mesh || (demo.Held() && m.skinned)) {
                continue;
            }
            DrawItem item;
            item.mesh = m.mesh;
            item.transform = m.world;
            item.source = (std::hash<std::string>{}(demo.demo_id) * 31u) ^ std::hash<std::string>{}(m.name) ^ 0x44454D4Full;
            if (m.player_own) {
                if (!demo.player_visible) {
                    continue;
                }
                item.hidden_meshes = HiddenMeshes(*m.mesh, player_hidden_);
            } else if (!m.hidden_meshes.empty()) {
                item.hidden_meshes = HiddenMeshes(*m.mesh, m.hidden_meshes);
            }
            if (m.skinned && !m.skin.empty()) {
                AssignSkin(item, std::span<const glm::mat4>(m.skin));
            }
            out.push_back(item);
        }
    }
}

}
