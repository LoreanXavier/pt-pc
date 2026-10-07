#include "game/game_objects.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <format>

#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "game/game.h"

namespace pt::game {
namespace {

constexpr float kPi = 3.14159265f;
constexpr std::string_view kGimmickNames[] = {"Baby", "Ocho", "CeilLamp", "Freezer", "Bag"};

constexpr uint32_t kHeraldPlay = 0xD567DD28;
constexpr uint32_t kHeraldStop = 0x0D5C27DE;
constexpr uint32_t kVoicePlay[3] = {0xAD52F3C2, 0x0CB2A9B7, 0xC910062E};
constexpr uint32_t kVoiceStop[3] = {0x3C3AAC9C, 0xEE4E7A5D, 0x020ADF6C};
constexpr uint32_t kStepsPlay = 0x7B136101;
constexpr uint32_t kStepsStop = 0xFEB3DEB3;

struct SpawnDef {
    float x, y, z, yaw, radius;
    bool variant;
    bool detectable;
};

constexpr SpawnDef kSpawns[9] = {
    {0.35f, 0.0f, -15.0f, kPi, 4.0f, false, true},        {-2.8f, 0.0f, -19.1f, kPi / 2, 1.7f, false, true},
    {-1.8f, 0.0f, -20.5f, 0.0f, 4.0f, true, false},       {-12.0f, 0.0f, -22.5f, kPi / 2, 4.0f, false, true},
    {-5.5f, 0.0f, -22.5f, kPi / 2, 4.0f, false, true},    {0.34f, 0.0f, -17.8f, 3.4033921f, 4.0f, false, true},
    {0.065f, 0.0f, -10.35f, kPi, 4.0f, false, true},      {1.29f, -0.39f, -22.5f, 4.712389f, 4.0f, false, false},
    {1.29f, -0.39f, -19.4f, 4.712389f, 4.0f, false, false},
};

float Wrap(float a) {
    const float t = a + kPi;
    return t >= 0.0f ? std::fmod(t, 2.0f * kPi) - kPi : kPi - std::fmod(-t, 2.0f * kPi);
}

float FoxYawOf(const glm::vec3& v) {
    if (std::abs(v.x) + std::abs(v.z) < 1e-6f) {
        return 0.0f;
    }
    return std::atan2(v.x, v.z);
}

uint32_t Mix(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 5;
    return x;
}

uint32_t g_fixed_seed = 0;
uint32_t g_seed_counter = 0;

}

void SetFixedRandomSeed(uint32_t seed) {
    g_fixed_seed = seed;
    g_seed_counter = 0;
}

uint32_t ReadTsc() {
    if (g_fixed_seed) {
        return Mix(g_fixed_seed + ++g_seed_counter * 0x9E3779B9u);
    }
    return static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

std::string_view GimmickName(GimmickType type) {
    return kGimmickNames[static_cast<size_t>(type)];
}

std::optional<bool> GameCommand::Bool(std::string_view key) const {
    auto it = args.find(key);
    if (it == args.end()) {
        return std::nullopt;
    }
    if (const bool* b = std::get_if<bool>(&it->second)) {
        return *b;
    }
    if (const double* d = std::get_if<double>(&it->second)) {
        return *d != 0.0;
    }
    return std::nullopt;
}

std::optional<double> GameCommand::Number(std::string_view key) const {
    auto it = args.find(key);
    if (it == args.end()) {
        return std::nullopt;
    }
    if (const double* d = std::get_if<double>(&it->second)) {
        return *d;
    }
    return std::nullopt;
}

std::optional<std::string> GameCommand::String(std::string_view key) const {
    auto it = args.find(key);
    if (it == args.end()) {
        return std::nullopt;
    }
    if (const std::string* s = std::get_if<std::string>(&it->second)) {
        return *s;
    }
    return std::nullopt;
}

std::string GameCommand::Describe() const {
    std::string text = id + " {";
    bool first = true;
    for (const auto& [key, value] : args) {
        text += first ? " " : ", ";
        first = false;
        text += key + "=";
        if (const bool* b = std::get_if<bool>(&value)) {
            text += *b ? "true" : "false";
        } else if (const double* d = std::get_if<double>(&value)) {
            text += std::format("{}", *d);
        } else {
            text += std::get<std::string>(value);
        }
    }
    return text + " }";
}

uint32_t OchoLogic::Random() {
    rng_ = Mix(rng_ ^ ReadTsc());
    return rng_;
}

void OchoLogic::BuildSpawns() {
    const glm::mat4 floor = game_.Floor().StageTransform();
    const glm::quat floor_rotation = glm::quat_cast(glm::mat3(floor));
    for (size_t i = 0; i < spawns_.size(); ++i) {
        const SpawnDef& d = kSpawns[i];
        Spawn& s = spawns_[i];
        s.world = glm::vec3(floor * glm::vec4(d.x, d.y, d.z, 1.0f));
        s.rotation = floor_rotation * glm::angleAxis(d.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        s.radius = d.radius;
        s.variant = d.variant;
        s.detectable = d.detectable;
    }
}

void OchoLogic::Sense(float dt) {
    const Player& player = game_.GetPlayer();
    player_pos_ = player.PublishedFeet();
    player_yaw_ = player.PublishedBodyFoxYaw();
    player_forward_ = glm::vec3(std::sin(player_yaw_), 0.0f, std::cos(player_yaw_));
    player_rotation_ = glm::angleAxis(player_yaw_, glm::vec3(0.0f, 1.0f, 0.0f));
    timer_a_ += dt;
    timer_b_ += dt;
    ocho_pos_ = glm::vec3(world_[3]);
    const glm::vec3 d = player_pos_ - ocho_pos_;
    distance2_ = glm::dot(d, d);
    facing_ = std::abs(Wrap(FoxYawOf(d) - player_yaw_));
}

void OchoLogic::ApplyRootMotion(const glm::vec3& translation, const glm::quat& rotation) {
    world_ = world_ * glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation);
    game_.Objects().SetOchoTransform(world_);
}

void OchoLogic::PlaceAt(const glm::vec3& position, const glm::quat& rotation) {
    world_ = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation);
    game_.Objects().SetOchoTransform(world_);
}

void OchoLogic::Show(bool visible) {
    game_.Objects().ShowOcho(visible);
}

void OchoLogic::SelectSpawnViews() {
    game_.Objects().GetGimmick(GimmickType::Ocho).hidden_views = spawns_[spawn_].variant ? 1u : 2u;
}

void OchoLogic::StopVoice() {
    game_.PostSoundId(kVoiceStop[voice_ == 2 ? 2 : voice_ == 1 ? 1 : 0]);
}

void OchoLogic::ResetState() {
    state_ = 0;
    spawn_ = 4;
    voice_ = 0;
    timer_a_ = timer_b_ = 0.0f;
    visible_duration_ = 30.0f;
    wait_ = 40.0f;
    warp_phase_ = 0;
    appear_request_ = false;
    ring_count_ = 0;
    dash_rest_ = glm::vec3(0.0f);
    dash_frames_ = 0;
    player_yaw_ = game_.GetPlayer().BodyFoxYaw();
    ring_.fill(player_yaw_);
    look_phase_ = 0;
    BuildSpawns();
    visible_ = false;
    Show(false);
    if (herald_sound_) {
        game_.PostSoundId(kHeraldStop);
        herald_sound_ = 0;
    }
    if (steps_sound_) {
        game_.PostSoundId(kStepsStop);
        steps_sound_ = 0;
    }
    if (voice_sound_) {
        game_.PostSoundId(kVoiceStop[2]);
        voice_sound_ = 0;
    }
}

void OchoLogic::Setup() {
    ResetState();
    has_killed_ = false;
    game_.Floor().SetLisaKilled(false);
    dash_step_ = 0.0f;
    world_ = glm::mat4(1.0f);
    game_.Objects().SetOchoTransform(world_);
}

void OchoLogic::RelocateForReset() {
    if (state_ != 1) {
        return;
    }
    BuildSpawns();
    Sense(0.0f);
    PlaceAt(spawns_[spawn_].world, spawns_[spawn_].rotation);
}

void OchoLogic::LogicControl(int s) {
    timer_a_ = 0.0f;
    StopVoice();
    ResetState();
    game_.Objects().SetGimmickEnabled(GimmickType::Ocho, s != 0);
    if (s == 5) {
        dash_step_ = 0.5f;
    } else if (s == 1) {
        BuildSpawns();
        Sense(0.0f);
        PlaceAt(spawns_[spawn_].world, spawns_[spawn_].rotation);
        timer_a_ = 0.0f;
        warp_phase_ = 2;
        wait_ = static_cast<float>(Random() % 50) + 40.0f;
        visible_ = false;
        Show(false);
        game_.PostSoundId(kHeraldStop);
        StopVoice();
    }
    state_ = s;
    const char* motion = "Ocho";
    if (s == 2 || s == 3 || (s == 1 && (spawn_ == 7 || spawn_ == 8))) {
        motion = "OchoStop";
    } else if (s == 4) {
        motion = "OchoDash";
    }
    game_.Objects().PlayGimmickMotion(GimmickType::Ocho, motion, false);
    LogInfo("ocho: LogicControl {} (wait {:.0f} s)", s, wait_);
}

bool OchoLogic::Detect() const {
    const Spawn& s = spawns_[spawn_];
    return visible_ && s.detectable && distance2_ < s.radius * s.radius && facing_ >= 2.6179938f;
}

int OchoLogic::ChooseSpawn() {
    std::vector<int> candidates;
    const bool f090 = game_.Floor().IsCurrentFloorName("f090");
    for (int i = 0; i < 9; ++i) {
        if (i == spawn_ || (f090 && (i == 0 || i == 5 || i == 6))) {
            continue;
        }
        const glm::vec3 d = spawns_[i].world - player_pos_;
        const float d2 = glm::dot(d, d);
        if (d2 > 30.25f && (d2 > 289.0f || std::abs(Wrap(player_yaw_ - FoxYawOf(d))) > kPi / 2 || spawns_[i].variant)) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) {
        return 2;
    }
    return candidates[Random() % candidates.size()];
}

void OchoLogic::Appear() {
    spawn_ = ChooseSpawn();
    PlaceAt(spawns_[spawn_].world, spawns_[spawn_].rotation);
    visible_ = true;
    Show(true);
    SelectSpawnViews();
    game_.Objects().PlayGimmickMotion(GimmickType::Ocho, (spawn_ == 7 || spawn_ == 8) ? "OchoStop" : "Ocho", false);
    voice_ = static_cast<int>(Random() % 3);
    game_.PostSoundIdAt(kVoicePlay[voice_], glm::vec3(world_[3]));
    voice_sound_ = 1;
    LogInfo("ocho: appears at spawn {} ({:.2f} {:.2f} {:.2f})", spawn_, world_[3][0], world_[3][1], world_[3][2]);
}

void OchoLogic::UpdateWarp() {
    if (warp_phase_ == 2) {
        if (timer_a_ > wait_) {
            timer_a_ = 0.0f;
            warp_phase_ = 0;
            game_.PostSoundId(kHeraldPlay);
            herald_sound_ = 1;
        }
    } else if (warp_phase_ == 0) {
        if (timer_a_ > 8.0f) {
            timer_a_ = 0.0f;
            warp_phase_ = 1;
            visible_duration_ = 30.0f;
            appear_request_ = true;
            game_.PostSoundId(kHeraldPlay);
            herald_sound_ = 1;
        }
    } else {
        if (appear_request_) {
            Appear();
            appear_request_ = false;
        }
        if (Detect()) {
            LogicControl(4);
            return;
        }
        if (timer_a_ > visible_duration_) {
            timer_a_ = 0.0f;
            warp_phase_ = 2;
            wait_ = static_cast<float>(Random() % 50) + 40.0f;
            visible_ = false;
            Show(false);
            game_.PostSoundId(kHeraldStop);
            StopVoice();
        }
    }
}

void OchoLogic::UpdateChase() {
    const glm::vec3 position = player_pos_ - 3.0f * player_forward_;
    PlaceAt(position, player_rotation_);
    const Player& player = game_.GetPlayer();
    if (player.Walking()) {
        if (!game_.IsSoundPlaying(steps_sound_)) {
            if (timer_b_ > 0.5f) {
                steps_sound_ = game_.PostSoundIdOnOcho(kStepsPlay, position);
                steps_origin_ = steps_position_ = position;
                game_.PostSoundId(kVoiceStop[2]);
            }
        } else {
            game_.MoveOchoSound(position);
            steps_position_ = position;
        }
        timer_a_ = 0.0f;
    } else if (player.Standing()) {
        timer_b_ = 0.0f;
        if (steps_sound_) {
            game_.PostSoundId(kStepsStop);
            steps_sound_ = 0;
            LogInfo("ocho: steps stopped, their sound followed her {:.2f} m", glm::length(steps_position_ - steps_origin_));
        }
        if (timer_a_ > 2.5f && !game_.IsSoundPlaying(voice_sound_)) {
            voice_sound_ = game_.PostSoundIdOnOcho(kVoicePlay[2], ocho_pos_);
        }
    }
}

bool OchoLogic::UpdateLookBack() {
    ring_[ring_count_++ % ring_.size()] = player_yaw_;
    float sum = 0.0f;
    for (float v : ring_) {
        sum += v;
    }
    const float deviation = std::abs(Wrap(player_yaw_ - sum / static_cast<float>(ring_.size())));
    if (deviation <= 1.8325957f) {
        if (look_phase_ != 0 && timer_a_ > 5.0f) {
            timer_a_ = 0.0f;
            look_phase_ = 0;
        }
        return false;
    }
    if (look_phase_ == 0) {
        look_phase_ = 1;
        ring_.fill(player_yaw_);
        timer_a_ = 0.0f;
        LogInfo("ocho: look back armed");
        return false;
    }
    if (look_phase_ == 1) {
        look_phase_ = 2;
        return true;
    }
    timer_a_ = 0.0f;
    return false;
}

void OchoLogic::UpdateDash(float frame_dt) {
    dash_step_ += 1.2f * frame_dt;
    const glm::vec3 p = glm::vec3(world_[3]) + dash_rest_;
    const glm::vec3 d = player_pos_ - p;
    const float length2 = glm::dot(d, d);
    const glm::vec3 dir = length2 < 1e-8f ? glm::vec3(0.0f, 0.0f, 1.0f) : d / std::sqrt(length2);
    const glm::vec3 move = dir * dash_step_;
    PlaceAt(glm::vec3(world_[3]) + dash_rest_ + 0.5f * move, glm::angleAxis(FoxYawOf(dir), glm::vec3(0.0f, 1.0f, 0.0f)));
    dash_rest_ = 0.5f * move;
    ++dash_frames_;
    if (distance2_ < 0.64000005f) {
        LogInfo("ocho: dash reached the player in {} game frames (step {:.3f} m per frame)", dash_frames_, dash_step_);
        LogicControl(has_killed_ ? 2 : 3);
    }
}

void OchoLogic::Kill() {
    has_killed_ = true;
    game_.Floor().SetLisaKilled(true);
    game_.Floor().SetPending(8);
    game_.Demos().SetDemoTransform("gc_p07_030", player_rotation_, player_pos_);
    game_.Demos().Play("gc_p07_030");
    LogInfo("ocho: kill");
    ResetState();
}

void OchoLogic::Update(float dt) {
    if (state_ == 0) {
        frame_dt_ = 0.0f;
        dash_rest_ = glm::vec3(0.0f);
        return;
    }
    frame_dt_ += dt;
    if (!game_.GameFrameTick()) {
        if (state_ == 4 && dash_rest_ != glm::vec3(0.0f)) {
            PlaceAt(glm::vec3(world_[3]) + dash_rest_, glm::quat_cast(glm::mat3(world_)));
            dash_rest_ = glm::vec3(0.0f);
        }
        return;
    }
    dt = frame_dt_;
    frame_dt_ = 0.0f;
    Sense(dt);
    switch (state_) {
    case 1:
        UpdateWarp();
        break;
    case 2:
        UpdateChase();
        break;
    case 3:
        UpdateChase();
        if (UpdateLookBack()) {
            if (has_killed_) {
                LogicControl(2);
            } else {
                visible_ = true;
                Show(true);
                SelectSpawnViews();
                Kill();
            }
        }
        break;
    case 4:
        UpdateDash(dt);
        break;
    case 5:
        Kill();
        break;
    default:
        break;
    }
}

GameObjects::GameObjects(Game& game) : game_(game), ocho_(game) {
    Reset();
}

uint32_t GameObjects::Random() {
    rng_ = Mix(rng_ ^ ReadTsc());
    return rng_;
}

void GameObjects::Reset() {
    for (size_t i = 0; i < gimmicks_.size(); ++i) {
        Gimmick& g = gimmicks_[i];
        Gimmick fresh;
        fresh.type = static_cast<GimmickType>(i);
        fresh.name = std::string(kGimmickNames[i]);
        fresh.parts_path = std::move(g.parts_path);
        fresh.model_file = std::move(g.model_file);
        fresh.mesh = g.mesh;
        fresh.parts_hidden_meshes = std::move(g.parts_hidden_meshes);
        fresh.hidden_meshes = fresh.parts_hidden_meshes;
        g = std::move(fresh);
    }
    ocho_root_valid_ = false;
}

void GameObjects::AddPartsPath(std::string_view part, std::string_view path) {
    parts_paths_[std::string(part)] = std::string(path);
    if (Gimmick* g = FindGimmick(part)) {
        g->parts_path = std::string(path);
    }
}

void GameObjects::AddMotionPath(std::string_view key, std::string_view path) {
    motion_paths_[std::string(key)] = std::string(path);
}

const std::string* GameObjects::MotionPath(std::string_view key) const {
    auto it = motion_paths_.find(key);
    return it == motion_paths_.end() ? nullptr : &it->second;
}

void GameObjects::ResolveModels() {
    for (Gimmick& g : gimmicks_) {
        if (g.parts_path.empty()) {
            continue;
        }
        g.mesh = nullptr;
        g.parts_hidden_meshes.clear();
        auto bytes = game_.GetVfs().ReadFile(g.parts_path);
        if (!bytes) {
            LogWarn("gimmick: parts {} not found", g.parts_path);
            continue;
        }
        fox2::DataSetFile file;
        if (!file.Load(g.parts_path, *bytes)) {
            continue;
        }
        for (const fox2::Entity& e : file.Entities()) {
            if (e.class_name == "ModelDescription") {
                g.model_file = file.GetString(e, "modelFile");
                if (const fox2::Property* invisible = file.FindProperty(e, "invisibleMeshNames")) {
                    for (size_t i = 0; i < invisible->Count(); ++i) {
                        g.parts_hidden_meshes.insert(StrCode64(file.ElementString(*invisible, i)) & kStrCode64Mask);
                    }
                }
                g.hidden_meshes = g.parts_hidden_meshes;
                break;
            }
        }
        if (const ModelEntry* model = game_.Models().Get(g.model_file)) {
            g.mesh = model->mesh.get();
        }
        LogInfo("gimmick: {} parts {} model {} {}", g.name, g.parts_path, g.model_file, g.mesh ? "loaded" : "missing");
    }
}

uint32_t GameObjects::GetId(std::string_view type, std::string_view name) const {
    if (type == "ShPlayer") {
        return name == "Player" ? kPlayerId : kNullId;
    }
    if (type == "ShGimmick") {
        for (size_t i = 0; i < std::size(kGimmickNames); ++i) {
            if (kGimmickNames[i] == name) {
                return kGimmickBase | static_cast<uint32_t>(i);
            }
        }
    }
    return kNullId;
}

Gimmick* GameObjects::FindGimmick(std::string_view name) {
    for (Gimmick& g : gimmicks_) {
        if (g.name == name) {
            return &g;
        }
    }
    return nullptr;
}

bool GameObjects::SendCommand(uint32_t id, const GameCommand& command) {
    if (id == kPlayerId) {
        return PlayerCommand(command);
    }
    if ((id & 0xFE00) == kGimmickBase && (id & 0x1FF) < gimmicks_.size()) {
        return GimmickCommand(gimmicks_[id & 0x1FF], command);
    }
    LogWarn("gameobject: command {} to unknown id {:#x}", command.Describe(), id);
    return false;
}

bool GameObjects::PlayerCommand(const GameCommand& command) {
    Player& player = game_.GetPlayer();
    LogInfo("gameobject: Player {}", command.Describe());
    if (command.id == "SetHandyLight") {
        if (auto enable = command.Bool("enable")) {
            player.handy_light.enable = *enable;
        }
        if (auto lumen = command.Number("lumen")) {
            player.handy_light.lumen = static_cast<float>(*lumen);
        }
        return true;
    }
    if (command.id == "SetDisableLeftStick") {
        player.SetDisableLeftStick(true);
        return true;
    }
    if (command.id == "UnsetDisableLeftStick") {
        player.SetDisableLeftStick(false);
        return true;
    }
    if (command.id == "Warp") {
        player.Warp(player.Feet(), static_cast<float>(command.Number("rotY").value_or(0.0)));
        return true;
    }
    LogWarn("gameobject: unhandled player command {}", command.id);
    return false;
}

bool GameObjects::GimmickCommand(Gimmick& gimmick, const GameCommand& command) {
    LogInfo("gameobject: {} {}", gimmick.name, command.Describe());
    if (command.id == "SetEnabled") {
        const bool enabled = command.Bool("enabled").value_or(false);
        if (command.Bool("isAll").value_or(false)) {
            for (size_t i = 0; i < gimmicks_.size(); ++i) {
                SetGimmickEnabled(static_cast<GimmickType>(i), enabled);
            }
        } else {
            SetGimmickEnabled(gimmick.type, enabled);
        }
        return true;
    }
    if (command.id == "PlayMotion") {
        gimmick.motion = command.String("motionKey").value_or("");
        gimmick.motion_time = 0.0f;
        gimmick.return_to_idle = command.Bool("returnToIdle").value_or(true);
        MotionStarted(gimmick);
        return true;
    }
    if (command.id == "LogicControl") {
        GimmickLogicControl(gimmick.type, command.String("state").value_or("None"));
        return true;
    }
    if (command.id == "StageLight") {
        const bool on = command.Bool("switch").value_or(true);
        const bool red = command.Bool("isRed").value_or(false);
        gimmick.stage_light = on;
        gimmick.stage_light_red = red;
        if (gimmick.type == GimmickType::CeilLamp) {
            gimmick.lights[1] = on && !red;
            gimmick.lights[2] = on && red;
            const uint64_t unlit = StrCode64("MESH_off") & kStrCode64Mask;
            const uint64_t lit = StrCode64("MESH_on_ST") & kStrCode64Mask;
            const uint64_t lit_red = StrCode64("MESH_on_red_ST") & kStrCode64Mask;
            gimmick.hidden_meshes.erase(unlit);
            gimmick.hidden_meshes.erase(lit);
            gimmick.hidden_meshes.erase(lit_red);
            if (!on) {
                gimmick.hidden_meshes.insert({lit, lit_red});
            } else if (red) {
                gimmick.hidden_meshes.insert({unlit, lit});
            } else {
                gimmick.hidden_meshes.insert({unlit, lit_red});
            }
        }
        return true;
    }
    if (command.id == "CallSound") {
        const std::string cnp = command.String("cnp").value_or("");
        if (!cnp.empty()) {
            gimmick.sound_cnp = cnp;
        }
        const glm::vec3 at = SoundPosition(gimmick);
        const std::string sound = command.String("soundId").value_or("");
        LogInfo("sound: {} at ({:.2f} {:.2f} {:.2f}) on {} {}", sound, at.x, at.y, at.z, gimmick.name, gimmick.sound_cnp);
        if (const uint32_t id = game_.Audio() ? game_.Audio()->PostRecordEvent(static_cast<int>(gimmick.type), sound, at) : 0) {
            gimmick.sound_handles.push_back(id);
        }
        return true;
    }
    if (command.id == "PostSoundEvent") {
        const glm::vec3 at = SoundPosition(gimmick);
        const std::string sound = command.String("soundId").value_or("");
        LogInfo("sound: {} at ({:.2f} {:.2f} {:.2f}) on {}", sound, at.x, at.y, at.z, gimmick.name);
        if (const uint32_t id = game_.Audio() ? game_.Audio()->PostRecordEvent(static_cast<int>(gimmick.type), sound, at) : 0) {
            gimmick.sound_handles.push_back(id);
        }
        return true;
    }
    if (command.id == "Warp") {
        const float rot = static_cast<float>(command.Number("rotY").value_or(0.0));
        gimmick.world = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(gimmick.world[3])), rot, glm::vec3(0.0f, 1.0f, 0.0f));
        return true;
    }
    LogWarn("gameobject: unhandled gimmick command {}", command.id);
    return false;
}

glm::vec3 GameObjects::SoundPosition(const Gimmick& g) const {
    glm::vec3 at(g.world[3]);
    if (!g.sound_cnp.empty()) {
        game_.Demos().Gimmicks().ConnectPointWorld(g.type, g.sound_cnp, at);
    }
    return at;
}

void GameObjects::SetGimmickLight(GimmickType type, size_t index, bool on) {
    Gimmick& g = gimmicks_[static_cast<size_t>(type)];
    if (index < std::size(g.lights)) {
        g.lights[index] = on;
    }
}

void GameObjects::SetGimmickEnabled(GimmickType type, bool enabled) {
    Gimmick& g = gimmicks_[static_cast<size_t>(type)];
    if (type == GimmickType::Freezer) {
        if (enabled) {
            if (!g.freezer_strong) {
                g.lights[0] = true;
                g.freezer_timer = 0.0f;
            }
        } else {
            g.lights[0] = g.lights[1] = g.lights[2] = false;
        }
    } else if (type == GimmickType::CeilLamp) {
        if (enabled) {
            g.lights[0] = g.lights[1] = true;
        } else {
            g.lights[0] = g.lights[1] = g.lights[2] = false;
        }
    }
    g.shown = enabled;
    g.enabled = enabled;
    if (!enabled) {
        game_.Demos().Gimmicks().StopSounds(type);
    }
    if (!enabled) {
        for (uint32_t id : g.sound_handles) {
            if (game_.Audio()) {
                game_.Audio()->StopPlayingId(id, 0.0f);
            }
        }
        g.sound_handles.clear();
    }
    g.hidden_views = 0;
}

void GameObjects::MotionStarted(const Gimmick& g) {
    dialogue_position_ = glm::vec3(g.world[3]);
}

void GameObjects::PlayGimmickMotion(GimmickType type, std::string_view key, bool return_to_idle) {
    Gimmick& g = gimmicks_[static_cast<size_t>(type)];
    g.motion = std::string(key);
    g.return_to_idle = return_to_idle;
    g.motion_time = 0.0f;
    MotionStarted(g);
    if (MotionPath(key)) {
        g.motion_requested = true;
    }
    if (type == GimmickType::Freezer) {
        if (key == "Freezer") {
            g.freezer_strong = false;
        } else if (key == "FreezerStrong") {
            g.freezer_strong = true;
        }
    }
    LogDebug("gimmick: {} motion {} (return to idle {})", g.name, key, return_to_idle);
}

void GameObjects::GimmickLogicControl(GimmickType type, std::string_view state) {
    int value = 0;
    if (state == "Warp") {
        value = 1;
    } else if (state == "Chase") {
        value = 2;
    } else if (state == "KillChase") {
        value = 3;
    } else if (state == "Dash") {
        value = 4;
    } else if (state == "Kill") {
        value = 5;
    }
    GimmickLogicControl(type, value);
}

void GameObjects::GimmickLogicControl(GimmickType type, int state) {
    if (type != GimmickType::Ocho) {
        return;
    }
    static constexpr const char* kNames[] = {"None", "Warp", "Chase", "KillChase", "Dash", "Kill"};
    gimmicks_[static_cast<size_t>(type)].logic_state = kNames[std::clamp(state, 0, 5)];
    ocho_.LogicControl(state);
}

void GameObjects::SetOchoTransform(const glm::mat4& world) {
    gimmicks_[static_cast<size_t>(GimmickType::Ocho)].world = world;
}

void GameObjects::ShowOcho(bool visible) {
    Gimmick& g = gimmicks_[static_cast<size_t>(GimmickType::Ocho)];
    g.shown = visible;
    g.hidden_views = 0;
}

void GameObjects::RelocateGimmicks(StageManager& stages) {
    for (Gimmick& g : gimmicks_) {
        g.placed = false;
    }
    for (const char* label : {"next", "current"}) {
        Stage* stage = stages.Find(label);
        if (!stage) {
            continue;
        }
        for (const auto& file : stage->files) {
            const fox2::DataSetFile& f = *file->file;
            for (const LocatorPlacement& locator : file->game_objects) {
                if (locator.class_name != "ShGimmick") {
                    continue;
                }
                const fox2::Entity* params = f.GetEntity(*locator.entity, "parameters");
                Gimmick* g = FindGimmick(params ? f.GetString(*params, "partsType") : std::string());
                if (!g || g->placed) {
                    continue;
                }
                g->placed = true;
                g->stage_id = stage->id;
                g->locator = stage->ToWorld(locator.world);
                if (g->type != GimmickType::Ocho) {
                    g->world = g->locator;
                }
            }
        }
    }
}

void GameObjects::HideRecordsWithoutLocator() {
    std::array<bool, static_cast<size_t>(GimmickType::Count)> found{};
    game_.Stages().ForEachStage([&](Stage& stage) {
        for (const auto& file : stage.files) {
            const fox2::DataSetFile& f = *file->file;
            for (const LocatorPlacement& locator : file->game_objects) {
                if (locator.class_name != "ShGimmick") {
                    continue;
                }
                const fox2::Entity* params = f.GetEntity(*locator.entity, "parameters");
                if (Gimmick* g = FindGimmick(params ? f.GetString(*params, "partsType") : std::string())) {
                    found[static_cast<size_t>(g->type)] = true;
                }
            }
        }
    }, false);
    for (Gimmick& g : gimmicks_) {
        if (!found[static_cast<size_t>(g.type)]) {
            if (g.active) {
                LogDebug("gimmick: {} deactivated (no locator)", g.name);
            }
            g.shown = false;
            g.placed = false;
            g.active = false;
        }
    }
}

void GameObjects::ResetToLocators() {
    RelocateGimmicks(game_.Stages());
    for (Gimmick& g : gimmicks_) {
        if (!g.enabled) {
            continue;
        }
        if (g.placed) {
            g.active = true;
        }
        if (g.type != GimmickType::Ocho && g.placed) {
            g.shown = true;
        }
        if (g.motion.empty() && g.type != GimmickType::Bag) {
            g.motion = g.name;
        }
        if (g.placed) {
            dialogue_position_ = glm::vec3(g.locator[3]);
            const glm::vec3 z(g.locator[2]);
            dialogue_forward_ = glm::length(z) > 1e-6f ? glm::normalize(z) : glm::vec3(0.0f, 0.0f, 1.0f);
        }
    }
    ocho_.RelocateForReset();
}

void GameObjects::UpdateFreezer(Gimmick& g, float dt) {
    if (!g.shown) {
        return;
    }
    g.freezer_timer += dt;
    const Player& player = game_.GetPlayer();
    if (g.freezer_timer >= 0.4f && g.freezer_timer <= 0.7f && player.PublishedPitch() >= 0.6632251f) {
        glm::vec3 open(g.world[3]);
        game_.Demos().Gimmicks().ConnectPointWorld(GimmickType::Freezer, "CNP_FRZ_OPEN", open);
        const glm::vec3 d = player.PublishedFeet() - open;
        if (d.x * d.x + d.z * d.z <= 0.16000001f && g.lights[0] && !g.lights[1]) {
            g.lights[1] = true;
        }
    }
    if (g.freezer_interval < g.freezer_timer) {
        g.freezer_interval = 2.0f + static_cast<float>(Random()) * 6.9849193e-10f;
        g.freezer_timer = 0.0f;
        if (!g.freezer_strong) {
            g.lights[0] = true;
        }
    }
}

void GameObjects::UpdateInView(Gimmick& g, float dt) {
    if (!g.enabled) {
        return;
    }
    const bool zoom = (game_.GetPlayer().HeldButtons() & kPadZoom) != 0;
    const bool in_view = zoom && game_.InViewNdc(glm::vec3(g.world[3]), 0.5f);
    if (g.type == GimmickType::Ocho) {
        const float target = in_view ? 3.0f : 1.0f;
        g.anim_rate += target <= g.anim_rate ? -3.0f * dt : 3.0f * dt;
        g.anim_rate = std::clamp(g.anim_rate, 1.0f, target);
    } else if (g.type == GimmickType::Bag) {
        if (in_view && game_.Floor().LisaKilled() && !g.motion_requested) {
            LogInfo("gimmick: Bag talks (zoom after Lisa's kill)");
            PlayGimmickMotion(GimmickType::Bag, "BagTalk", false);
        } else if (in_view && g.motion_requested && !g.silent_logged) {
            LogInfo("gimmick: Bag in view with zoom, stays silent (already talked this session)");
        }
        g.silent_logged = in_view && g.motion_requested;
    }
}

void GameObjects::Update(float dt) {
    for (Gimmick& g : gimmicks_) {
        if (!g.motion.empty() && g.active && g.enabled) {
            g.motion_time += dt * g.anim_rate;
            const float length = game_.Demos().Gimmicks().MotionSeconds(g.motion);
            if (g.motion_time > (length > 0.0f ? length : 4.0f)) {
                if (g.return_to_idle) {
                    g.motion = g.name;
                    g.return_to_idle = false;
                    g.motion_time = 0.0f;
                    MotionStarted(g);
                } else if (length > 0.0f && !game_.Demos().Gimmicks().MotionLoops(g.motion)) {
                    g.motion_time = length;
                }
            }
        }
        if (g.type == GimmickType::Ocho || g.type == GimmickType::Bag) {
            UpdateInView(g, dt);
        }
        if (!g.sound_handles.empty() && game_.Audio()) {
            std::erase_if(g.sound_handles, [&](uint32_t id) { return !game_.Audio()->IsPlaying(id); });
            if (!g.sound_handles.empty()) {
                game_.Audio()->MoveRecordSound(static_cast<int>(g.type), SoundPosition(g));
            }
        }
        if (g.type == GimmickType::Freezer) {
            UpdateFreezer(g, dt);
        }
        const uint32_t bit = 1u << static_cast<uint32_t>(g.type);
        if (!(archive_noted_ & bit) && g.enabled && g.shown && g.mesh && (g.type == GimmickType::Ocho || g.placed) && g.hidden_views == 0) {
            archive_noted_ |= bit;
            game_.NoteArchive("gimmick:" + g.name);
        }
    }
    if (!game_.Config().theater || game_.Theater().ocho) {
        ocho_.Update(dt);
    }
    game_.Demos().Gimmicks().Update(dt);
    UpdateOchoRootMotion();
    static const bool trace = std::getenv("PT_GIMMICK_TRACE") != nullptr;
    if (trace) {
        static std::array<std::string, static_cast<size_t>(GimmickType::Count)> last;
        for (const Gimmick& g : gimmicks_) {
            std::string now = std::format("enabled {} shown {} placed {} active {} mesh {} views {} hidden meshes {}", g.enabled ? 1 : 0,
                                          g.shown ? 1 : 0, g.placed ? 1 : 0, g.active ? 1 : 0, g.mesh ? 1 : 0, g.hidden_views,
                                          g.hidden_meshes.size());
            std::string& was = last[static_cast<size_t>(g.type)];
            if (now != was) {
                LogInfo("gimmick trace: {} {} on {} loop {} step {}", g.name, now, game_.Floor().CurrentFloorName(), game_.Floor().LoopCount(),
                        game_.Controller().Step());
                was = std::move(now);
            }
        }
    }
}

void GameObjects::UpdateOchoRootMotion() {
    const Gimmick& g = gimmicks_[static_cast<size_t>(GimmickType::Ocho)];
    glm::vec3 translation(0.0f);
    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
    if (!game_.Demos().Gimmicks().RigRoot(GimmickType::Ocho, translation, rotation)) {
        ocho_root_valid_ = false;
        return;
    }
    GimmickAnimation& gimmick_anim = game_.Demos().Gimmicks();
    const float length = gimmick_anim.MotionSeconds(g.motion);
    const bool continued = ocho_root_valid_ && g.motion == ocho_root_motion_ && g.motion_time >= ocho_root_time_;
    const bool wrapped = continued && length > 0.0f && std::floor(g.motion_time / length) > std::floor(ocho_root_time_ / length);
    glm::vec3 start_t(0.0f);
    glm::vec3 end_t(0.0f);
    glm::quat start_q(1.0f, 0.0f, 0.0f, 0.0f);
    glm::quat end_q(1.0f, 0.0f, 0.0f, 0.0f);
    const bool seam = wrapped && gimmick_anim.RigRootLoop(GimmickType::Ocho, start_t, start_q, end_t, end_q);
    if (continued && g.enabled && (!wrapped || seam)) {
        glm::vec3 step = translation - ocho_root_translation_;
        glm::quat turn = glm::conjugate(ocho_root_rotation_) * rotation;
        if (seam) {
            step = (end_t - ocho_root_translation_) + (translation - start_t);
            turn = glm::conjugate(ocho_root_rotation_) * end_q * glm::conjugate(start_q) * rotation;
        }
        ocho_.ApplyRootMotion(glm::normalize(turn * glm::conjugate(rotation)) * step, glm::normalize(turn));
    }
    ocho_root_motion_ = g.motion;
    ocho_root_time_ = g.motion_time;
    ocho_root_translation_ = translation;
    ocho_root_rotation_ = rotation;
    ocho_root_valid_ = true;
}

namespace {

bool Drawable(const Gimmick& g) {
    return g.enabled && g.shown && g.mesh && (g.type == GimmickType::Ocho || g.placed);
}

}

void GameObjects::DrawnCharacters(std::vector<glm::vec3>& feet) const {
    for (const Gimmick& g : gimmicks_) {
        if (g.type == GimmickType::Ocho && Drawable(g) && (g.hidden_views & 1u) == 0) {
            feet.push_back(glm::vec3(g.world[3]));
        }
    }
}

bool GameObjects::GimmickDrawn(std::string_view name) const {
    for (const Gimmick& g : gimmicks_) {
        if (g.name == name) {
            return Drawable(g);
        }
    }
    return false;
}

void GameObjects::CollectDraws(std::vector<DrawItem>& out) const {
    for (const Gimmick& g : gimmicks_) {
        if (!Drawable(g)) {
            continue;
        }
        DrawItem item;
        item.mesh = g.mesh;
        item.transform = g.world;
        if (!g.hidden_meshes.empty()) {
            item.hidden_meshes = HiddenMeshes(*g.mesh, g.hidden_meshes);
        }
        item.hidden_views = g.hidden_views;
        item.character_shadow = g.type == GimmickType::Ocho && (ocho_.State() == 2 || ocho_.State() == 3);
        AssignSkin(item, game_.Demos().Gimmicks().Skin(g.type));
        out.push_back(item);
    }
}

}
