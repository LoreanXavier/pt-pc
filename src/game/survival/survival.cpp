#include "game/survival/survival.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <span>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/physics/collision_world.h"
#include "engine/platform/input.h"
#include "engine/render/camera.h"
#include "engine/render/model_cache.h"
#include "game/game.h"
#include "game/game_objects.h"
#include "game/gimmick_animation.h"
#include "game/player.h"
#include "game/player_animation.h"
#include "game/run_stride.h"
#include "engine/audio/wem.h"
#include "game/survival/survival_bodies.h"
#include "game/survival/survival_sounds.h"

namespace pt::game {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kMotionFps = 59.94f;
constexpr glm::vec2 kArenaMin(-18.5f, 23.0f);
constexpr float kFaceRoom = 0.62f;
constexpr glm::vec2 kArenaMax(12.5f, 189.0f);
constexpr float kRoadMinX = -7.0f;
constexpr float kRoadMaxX = 4.5f;
constexpr const char* kDollModel = "/Assets/sh/chara/dol/Scenes/dol0_main0_def.fmdl";
constexpr std::string_view kTickEvent = "Play_sfx_digiclock_tick_solo";
constexpr std::string_view kHeartbeatEvent = "Play_sfx_heartbeat";
constexpr const char* kMidnightVoices[] = {"Play_radio_f010", "Play_radio_f050", "Play_radio_voice_06",
                                          "Play_radio_voice_04", "Play_radio_voice_07", "Play_radio_voice_08"};
constexpr uint32_t kStreetDonor = 0x35BC649Du;
constexpr uint32_t kConcreteSteps[] = {870939230, 287251558, 20008044, 175143664, 537402674,
                                       205513966, 902227847, 786104213, 469498237, 174544081};
constexpr uint32_t kBloodSteps[] = {441947342, 782042260, 341698758, 653827715, 348489762, 748697604, 869417874, 665917600};
constexpr uint32_t kBlows[] = {16807362, 65369247, 428421965, 995344595, 1054851282, 614031349, 991946540};
constexpr uint32_t kBlood[] = {402091748, 932688280, 631528804};
constexpr uint32_t kFridgeBlood[] = {1019312711, 153700685, 593277454, 871593140};
constexpr uint32_t kLaugh[] = {93661487, 748023997, 1065241238, 73183476, 453537861, 823343976};
constexpr uint32_t kBreath[] = {618929925, 788918991, 794368086, 1055384029, 86561618, 826936335};
constexpr uint32_t kChase[] = {306539074, 905007846, 881819544, 841752205, 109440032};
constexpr uint32_t kLightOff[] = {157915525};
constexpr uint32_t kLightDown[] = {221193726};
constexpr uint32_t kLightOn[] = {1058735461};
constexpr uint32_t kBell[] = {922098571, 1067843490, 792871684, 79191630, 678268306};

constexpr std::string_view kWalk = "Ocho";
constexpr std::string_view kDash = "OchoDash";
constexpr std::string_view kStop = "OchoStop";
constexpr float kPounceFly = 0.26f;
constexpr float kPounceHold = 0.42f;
constexpr float kPounceDrop = 0.25f;
constexpr std::string_view kCatchDemo = "gc_p07_030";
constexpr std::string_view kHallwayPackage = "/Assets/sh/level/promotion/pt_2014/hallway/pt14_hallway.fpk";
constexpr std::string_view kWakeDemo = "gc_p00_020";
constexpr std::string_view kStartPackage = "/Assets/sh/level/promotion/pt_2014/start/pt14_start.fpk";

struct KindSpec {
    float health;
    float speed;
    float damage;
    float reach;
    float windup;
    float recover;
    float rise;
    float stun;
    float mass;
    glm::vec3 box_center;
    glm::vec3 box_half;
    float head_radius;
    glm::vec3 tint;
    const char* name;
};

constexpr KindSpec kKinds[] = {
    {80.0f, 1.82f, 14.0f, 0.95f, 0.5f, 1.0f, 0.6f, 1.9f, 1.0f, {0.0f, 0.82f, 0.0f}, {0.25f, 0.82f, 0.17f}, 0.12f, {1.0f, 1.0f, 1.0f}, "doll"},
    {45.0f, 3.65f, 9.0f, 1.05f, 0.45f, 0.75f, 0.35f, 1.5f, 0.7f, {0.0f, 0.42f, 0.02f}, {0.36f, 0.28f, 0.64f}, 0.12f, {0.78f, 0.75f, 0.74f}, "crawler"},
    {300.0f, 1.38f, 32.0f, 1.95f, 0.6f, 1.2f, 0.8f, 0.9f, 2.2f, {0.0f, 1.08f, 0.05f}, {0.27f, 1.08f, 0.2f}, 0.13f, {0.86f, 0.86f, 0.9f}, "stretched"},
    {170.0f, 2.88f, 22.0f, 1.0f, 0.4f, 1.0f, 1.4f, 1.4f, 1.2f, {0.0f, 0.8f, 0.0f}, {0.24f, 0.8f, 0.17f}, 0.11f, {1.0f, 1.0f, 1.0f}, "mother"},
    {700.0f, 3.4f, 60.0f, 1.0f, 0.25f, 1.1f, 1.8f, 0.0f, 3.0f, {0.0f, 0.8f, 0.0f}, {0.24f, 0.8f, 0.17f}, 0.11f, {1.0f, 1.0f, 1.0f}, "lisa"},
    {950.0f, 1.24f, 45.0f, 2.1f, 0.95f, 1.5f, 3.0f, 0.6f, 7.0f, {0.0f, 1.56f, 0.0f}, {0.46f, 1.56f, 0.32f}, 0.22f, {0.7f, 0.68f, 0.68f}, "giant"},
};

float KindScale(SurvivalEnemyKind k) {
    return k == SurvivalEnemyKind::Giant ? 1.9f : 1.0f;
}

const KindSpec& Kind(SurvivalEnemyKind k) {
    return kKinds[static_cast<size_t>(k)];
}

}

float SurvivalNight::KindScaleOf(SurvivalEnemyKind k) {
    return KindScale(k);
}

float SurvivalNight::KindWindup(SurvivalEnemyKind k) {
    return Kind(k).windup;
}

float SurvivalNight::KindMassOf(SurvivalEnemyKind k) {
    return Kind(k).mass;
}

namespace {

bool WalksLikeDoll(SurvivalEnemyKind k) {
    return k == SurvivalEnemyKind::Doll || k == SurvivalEnemyKind::Stretched || k == SurvivalEnemyKind::Giant;
}

bool UsesLisa(SurvivalEnemyKind k) {
    return k == SurvivalEnemyKind::Mother || k == SurvivalEnemyKind::Lisa;
}

struct DifficultySpec {
    float health, damage, speed, count, drops, rounds, life, player_health;
};
constexpr DifficultySpec kDifficulty[] = {
    {0.75f, 0.6f, 0.88f, 0.8f, 1.35f, 1.35f, 75.0f, 120.0f},
    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 60.0f, 100.0f},
    {1.25f, 1.5f, 1.07f, 1.3f, 0.75f, 1.25f, 45.0f, 100.0f},
    {1.5f, 2.0f, 1.13f, 1.5f, 0.55f, 1.4f, 35.0f, 100.0f},
    {1.7f, 2.5f, 1.18f, 1.6f, 0.42f, 1.5f, 30.0f, 100.0f},
};

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

using run_stride::kNightPaceSpeed;
using run_stride::kNightSprintSpeed;
constexpr float kAimTap = 0.22f;

float WrapAngle(float a) {
    return std::remainder(a, 2.0f * kPi);
}

void Spring(glm::vec2& x, glm::vec2& v, float stiffness, float damping, float dt) {
    const int steps = std::max(1, static_cast<int>(std::ceil(dt / 0.004f)));
    const float h = dt / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        v += (-stiffness * x - damping * v) * h;
        x += v * h;
    }
}

}

float SurvivalNight::SprintWeight() const {
    return run_stride::Weight(speed_scale_, kNightSprintSpeed, kNightPaceSpeed);
}

float SurvivalNight::RunWeight() const {
    return active_ && !hud_.dead ? run_stride::Weight(speed_scale_, kNightSprintSpeed) : 0.0f;
}

float SurvivalNight::BodyRunWeight() const {
    return active_ && !hud_.dead ? body_run_ : 0.0f;
}

float SurvivalNight::BodyRunArms() const {
    const float pace = run_stride::Weight(kNightPaceSpeed, kNightSprintSpeed);
    return Smooth(std::clamp((BodyRunWeight() - pace) / (1.0f - pace), 0.0f, 1.0f));
}

const SurvivalNight::WeaponSpec& SurvivalNight::Spec(SurvivalWeapon w) {
    static const WeaponSpec kSpecs[] = {
        {0, 0, 0, 0, 8.0f, 1, 0.0f, 7.0f, 0.0f, 12.0f, 0.0f, 0.0f, "pc_survival_flashlight"},
        {15, 45, 12, 84, 22.0f, 1, 0.012f, 0.26f, 1.35f, 70.0f, 0.03f, 0.18f, "pc_survival_handgun"},
        {6, 10, 4, 26, 24.0f, 9, 0.045f, 0.75f, 0.5f, 34.0f, 0.08f, 0.15f, "pc_survival_shotgun"},
        {6, 8, 4, 24, 135.0f, 1, 0.006f, 0.55f, 2.2f, 100.0f, 0.11f, 0.6f, "pc_survival_magnum"},
        {7, 10, 5, 30, 100.0f, 1, 0.004f, 0.58f, 0.42f, 110.0f, 0.10f, 0.55f, "pc_survival_lever"},
        {2, 8, 4, 26, 26.0f, 12, 0.07f, 0.22f, 1.7f, 26.0f, 0.10f, 0.2f, "pc_survival_double"},
    };
    return kSpecs[static_cast<size_t>(w)];
}

SurvivalNight::SurvivalNight(Game& game) : game_(game) {
    auto_play_ = std::getenv("PT_SURVIVAL_AUTO") != nullptr;
    if (const char* p = std::getenv("PT_SURVIVAL_PHYSICS"); p && p[0] == '0') physics_ = false;
    auto_passive_ = std::getenv("PT_SURVIVAL_PASSIVE") != nullptr;
    if (const char* s = std::getenv("PT_SURVIVAL_SCRIPT")) {
        std::string all(s);
        size_t at = 0;
        while (at < all.size()) {
            const size_t end = all.find(' ', at);
            const std::string item = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? all.size() : end + 1;
            const size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            script_.push_back({static_cast<float>(std::atof(item.substr(0, colon).c_str())), item.substr(colon + 1)});
        }
    }
}

void SurvivalNight::RunScript(uint32_t& held, uint32_t& pressed) {
    pressed |= std::exchange(act_pressed_, 0u);
    for (ScriptStep& step : script_) {
        if (step.done || run_time_ < step.at) continue;
        step.done = true;
        LogInfo("survival: script {:.2f} {}", run_time_, step.action);
        ScriptAction(step.action, pressed);
    }
}

void SurvivalNight::Act(const std::string& action) {
    LogInfo("survival: act {}", action);
    ScriptAction(action, act_pressed_);
}

void SurvivalNight::ScriptAction(const std::string& a, uint32_t& pressed) {
    if (a == "sights") {
        sights_ = !sights_;
        LogInfo("survival: sights {}", sights_ ? "in" : "out");
    }
    if (a == "aim+") script_held_ |= kSurvivalAim;
    if (a == "info+") script_held_ |= kSurvivalInfo;
    if (a == "info-") script_held_ &= ~kSurvivalInfo;
    if (a == "aim-") script_held_ &= ~kSurvivalAim;
    if (a == "sprint+") script_held_ |= kSurvivalSprint;
    if (a == "sprint-") script_held_ &= ~kSurvivalSprint;
    if (a == "fire+") script_held_ |= kSurvivalFire;
    if (a == "fire-") script_held_ &= ~kSurvivalFire;
    if (a == "fire") pressed |= kSurvivalFire;
    if (a == "reload") pressed |= kSurvivalReload;
    if (a == "flashclick") TriggerFlashlightClick();
    if (a == "wake") StartWake("pc_survival_title", 0);
    if (a == "supply") StartSupply();
    if (a == "tosupply" && supply_on_) game_.GetPlayer().Warp(supply_at_ + glm::vec3(1.5f, 0.0f, 0.0f), game_.GetPlayer().FoxYaw());
    if (a == "lay") {
        const std::pair<SurvivalPickupKind, SurvivalWeapon> row[] = {
            {SurvivalPickupKind::Ammo, SurvivalWeapon::Handgun}, {SurvivalPickupKind::Ammo, SurvivalWeapon::Magnum},
            {SurvivalPickupKind::Ammo, SurvivalWeapon::Shotgun}, {SurvivalPickupKind::Ammo, SurvivalWeapon::Lever},
            {SurvivalPickupKind::Health, SurvivalWeapon::Handgun}, {SurvivalPickupKind::Flare, SurvivalWeapon::Handgun},
            {SurvivalPickupKind::Pipe, SurvivalWeapon::Handgun}};
        glm::vec3 at = game_.GetPlayer().Feet() + glm::vec3(-1.2f, 0.0f, 1.7f);
        for (const auto& [kind, weapon] : row) {
            Drop(at, kind, weapon);
            Pickup& p = pickups_.back();
            p.position = at;
            p.position.y = p.ground = GroundAt(at, at.y);
            p.velocity = glm::vec3(0.0f);
            p.settled = true;
            p.life = 1e9f;
            at.x += 0.42f;
        }
        at = game_.GetPlayer().Feet() + glm::vec3(-0.6f, 0.0f, 1.2f);
        for (const Debris::Kind kind : {Debris::Kind::Casing, Debris::Kind::Shell}) {
            Debris d;
            d.kind = kind;
            d.id = next_id_++;
            d.position = at + glm::vec3(0.0f, 0.02f, 0.0f);
            d.ground = GroundAt(at, at.y);
            d.rotation = glm::angleAxis(1.2f, glm::vec3(0.0f, 1.0f, 0.0f));
            d.life = 1e9f;
            debris_.push_back(d);
            at.x += 0.12f;
        }
    }
    if (a == "swap") pressed |= kSurvivalSwap;
    if (a == "wave") countdown_ = 0.01f;
    if (a.rfind("wave=", 0) == 0) {
        wave_ = std::max(0, std::atoi(a.c_str() + 5) - 1);
        countdown_ = 0.01f;
        lamps_out_ = SurvivalLampsOut(wave_ + 1);
        north_end_ = NorthEndOf(SurvivalNightOf(wave_ + 1));
        night_door_ = SurvivalNightOf(wave_ + 1) >= 4 ? 1.0f : 0.0f;
    }
    if (a == "clear") {
        for (const Enemy& e : enemies_) {
            if (UsesLisa(e.kind)) game_.Demos().Gimmicks().ReleaseActor(e.id);
        }
        enemies_.clear();
        bodies_.clear();
        enemy_ragdolls_.clear();
        slot_used_ = {};
        to_spawn_ = 0;
    }
    if (a == "hold") countdown_ = 1e6f;
    if (a.rfind("weapon=", 0) == 0) {
        const int w = std::clamp(std::atoi(a.c_str() + 7), 0, static_cast<int>(SurvivalWeapon::Count) - 1);
        WeaponState& s = weapons_[static_cast<size_t>(w)];
        s.owned = true;
        s.clip = Spec(static_cast<SurvivalWeapon>(w)).clip_size;
        s.reserve = Spec(static_cast<SurvivalWeapon>(w)).start_reserve;
        SelectWeapon(static_cast<SurvivalWeapon>(w));
    }
    if (a.rfind("hurt=", 0) == 0) {
        const Camera view = game_.ViewCamera(true);
        HurtPlayer(static_cast<float>(std::atof(a.c_str() + 5)), view.position + view.Forward() * 2.0f);
    }
    if (a.rfind("drop=", 0) == 0) {
        const int w = std::clamp(std::atoi(a.c_str() + 5), 0, static_cast<int>(SurvivalWeapon::Count) - 1);
        float ahead = 1.0f, side = 0.0f;
        if (const size_t where = a.find('@'); where != std::string::npos) std::sscanf(a.c_str() + where + 1, "%f,%f", &ahead, &side);
        Player& p = game_.GetPlayer();
        const float yaw = p.FoxYaw();
        const glm::vec3 at = p.Feet() + glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)) * ahead + glm::vec3(std::cos(yaw), 0.0f, -std::sin(yaw)) * side;
        Drop(at, SurvivalPickupKind::Weapon, static_cast<SurvivalWeapon>(w));
        Pickup& d = pickups_.back();
        d.position = at;
        d.position.y = d.ground = GroundAt(at, at.y);
        d.velocity = glm::vec3(0.0f);
        d.settled = true;
        p.Warp(d.position + glm::vec3(0.0f, 0.05f, 0.0f), p.BodyFoxYaw());
    }
    if (a == "freeze") frozen_ = true;
    if (a == "aimat") aim_at_head_ = true;
    if (a == "aimat-") aim_at_head_ = false;
    if (a.rfind("studio=", 0) == 0) Studio(std::atoi(a.c_str() + 7));
    if (a == "quiet") {
        for (const Enemy& e : enemies_) {
            if (UsesLisa(e.kind)) game_.Demos().Gimmicks().ReleaseActor(e.id);
        }
        enemies_.clear();
        bodies_.clear();
        enemy_ragdolls_.clear();
        slot_used_ = {};
        quiet_ = true;
    }
    if (a == "unhurt") unhurt_ = true;
    if (a.rfind("accuse=", 0) == 0) Accuse(std::atoi(a.c_str() + 7) - 1);
    if (a.rfind("boon=", 0) == 0) {
        const int b = std::clamp(std::atoi(a.c_str() + 5), 0, static_cast<int>(SurvivalBoon::Count) - 1);
        offer_[0] = static_cast<SurvivalBoon>(b);
        offer_count_ = 1;
        TakeBoon(0);
    }
    if (a == "offer") OfferBoons();
    if (a.rfind("pick=", 0) == 0) TakeBoon(std::atoi(a.c_str() + 5));
    if (a == "blackout") {
        blackout_length_ = 10.0f;
        blackout_time_ = 0.0f;
        Omen("pc_survival_omen_blackout", 3.5f);
    }
    if (a == "melee") pressed |= kSurvivalMelee;
    if (a == "flare") pressed |= kSurvivalFlare;
    if (a == "use+") script_held_ |= kSurvivalUse;
    if (a == "use-") script_held_ &= ~kSurvivalUse;
    if (a == "pipe") {
        pipe_hits_ = pipe_hits_full_ = 14;
        pipe_time_ = pipe_time_full_ = 100.0f;
    }
    if (a.rfind("flares=", 0) == 0) flares_ = std::max(0, std::atoi(a.c_str() + 7));
    if (a.rfind("fear=", 0) == 0) fear_ = std::clamp(static_cast<float>(std::atof(a.c_str() + 5)), 0.0f, 1.0f);
    if (a.rfind("mission=", 0) == 0) forced_mission_ = std::clamp(std::atoi(a.c_str() + 8), 0, 5);
    if (a.rfind("look=", 0) == 0 && !task_.spots.empty()) {
        Player& player = game_.GetPlayer();
        const glm::vec3 thing = task_.spots[0];
        glm::vec3 out = task_.facing;
        if (task_.kind == SurvivalMission::Fragments || task_.kind == SurvivalMission::Lamps) {
            out = player.Feet() - thing;
            out.y = 0.0f;
            out = glm::length(out) > 1e-3f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
        }
        float reach = 2.0f, around = 0.0f;
        std::sscanf(a.c_str() + 5, "%f,%f", &reach, &around);
        const float c = std::cos(glm::radians(around)), s = std::sin(glm::radians(around));
        out = glm::vec3(out.x * c + out.z * s, 0.0f, -out.x * s + out.z * c);
        glm::vec3 feet = thing + out * reach;
        feet.y = GroundAt(feet + glm::vec3(0.0f, 1.0f, 0.0f), player.Feet().y);
        player.Warp(feet, std::atan2(-out.x, -out.z));
        const float middle = task_.kind == SurvivalMission::Radio ? 0.15f + RadioLift() : task_.kind == SurvivalMission::Lamps ? 0.4f
                             : task_.kind == SurvivalMission::Door                     ? 1.0f
                                                                                       : 0.0f;
        const glm::vec3 to = glm::normalize(thing + glm::vec3(0.0f, middle, 0.0f) - (feet + glm::vec3(0.0f, 1.6f, 0.0f)));
        player.yaw = player.target_yaw = std::atan2(-to.x, -to.z);
        player.pitch = player.target_pitch = Player::ClampPitch(std::asin(std::clamp(to.y, -1.0f, 1.0f)));
        LogInfo("survival: looking at the task's thing at {:.1f} {:.1f} {:.1f} from {:.1f} {:.1f} {:.1f}", thing.x, thing.y, thing.z, feet.x, feet.y, feet.z);
    }
    if (a.rfind("clip=", 0) == 0) {
        weapons_[static_cast<size_t>(weapon_)].clip = std::max(0, std::atoi(a.c_str() + 5));
        gun_.slide_locked = weapon_ == SurvivalWeapon::Handgun && weapons_[static_cast<size_t>(weapon_)].clip == 0;
        gun_.chambered = weapons_[static_cast<size_t>(weapon_)].clip > 0;
    }
    if (a.rfind("spawn=", 0) == 0) {
        if (const size_t where = a.find('@'); where != std::string::npos &&
            std::sscanf(a.c_str() + where + 1, "%f,%f", &spawn_at_.x, &spawn_at_.y) == 2) {
            spawn_at_set_ = true;
        }
        const auto kind = static_cast<SurvivalEnemyKind>(std::clamp(std::atoi(a.c_str() + 6), 0, static_cast<int>(SurvivalEnemyKind::Count) - 1));
        if (const size_t tilde = a.find('~'); tilde != std::string::npos) {
            glm::vec3 at;
            if (SpawnPoint(at, 20.0f, 33.0f)) {
                if (Enemy* e = SpawnEnemyAt(kind, at, 1.0f, static_cast<uint8_t>(std::atoi(a.c_str() + tilde + 1)), false)) (void)e;
            }
        } else {
            SpawnEnemy(kind);
        }
        spawn_at_set_ = false;
    }
}

SurvivalNight::~SurvivalNight() = default;

uint32_t SurvivalNight::PlanSeed(int wave, uint32_t stream) const {
    uint32_t h = night_seed_ ^ 0x6E164Bu ^ (static_cast<uint32_t>(wave) * 0x9E3779B9u) ^ (stream * 0x85EBCA6Bu);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float SurvivalNight::PlanRand() {
    plan_rng_ = plan_rng_ * 1664525u + 1013904223u;
    return static_cast<float>(plan_rng_ >> 8) / 16777216.0f;
}

float SurvivalNight::Rand() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) / 16777216.0f;
}

void SurvivalNight::EnsureArmoryAssets(int slot) {
    if (slot >= 1 && slot <= 6) {
        if ((models_.armory_slots_loaded & (1u << static_cast<unsigned>(slot))) != 0) return;
        const auto t0 = std::chrono::steady_clock::now();
        if (own_materials_begin_ == 0) own_materials_begin_ = game_.Models().Textures().MaterialCount();
        models_.BuildArmorySlot(game_.Models(), slot);
        own_materials_end_ = std::max(own_materials_end_, game_.Models().Textures().MaterialCount());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        LogInfo("survival: armory slot {} ready in {:.1f} ms", slot, ms);
        return;
    }
    if (armory_ready_) return;
    armory_ready_ = true;
    const auto t0 = std::chrono::steady_clock::now();
    if (own_materials_begin_ == 0) own_materials_begin_ = game_.Models().Textures().MaterialCount();
    models_.BuildArmory(game_.Models());
    own_materials_end_ = std::max(own_materials_end_, game_.Models().Textures().MaterialCount());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LogInfo("survival: armory models ready in {:.1f} ms", ms);
}

void SurvivalNight::EnsureAssets() {
    if (assets_ready_) return;
    assets_ready_ = true;
    const Gimmick& ocho = game_.Objects().GetGimmick(GimmickType::Ocho);
    lisa_model_ = ocho.model_file;
    lisa_parts_ = ocho.parts_path;
    lisa_mesh_ = ocho.mesh;
    if (lisa_mesh_) lisa_hidden_ = HiddenMeshes(*lisa_mesh_, ocho.parts_hidden_meshes);
    GimmickAnimation& anim = game_.Demos().Gimmicks();
    lisa_skeleton_ = lisa_model_.empty() ? nullptr : anim.RetargetSkeleton(lisa_model_);
    if (const ModelEntry* doll = game_.Models().Get(kDollModel)) doll_mesh_ = doll->mesh.get();
    doll_skeleton_ = anim.RetargetSkeleton(kDollModel);
    if (doll_skeleton_) doll_hit_ = HitBonesOf(*doll_skeleton_);
    if (lisa_skeleton_) lisa_hit_ = HitBonesOf(*lisa_skeleton_);
    if (doll_skeleton_) poser_ = std::make_unique<SurvivalPoser>(*doll_skeleton_);
    PlayerClipTravel(kClipFront, doll_walk_frames_, doll_walk_travel_, doll_skeleton_);
    LogInfo("survival: Lisa {} ({}), the doll dol0 {} (poser {}), the walk {:.0f} frames {:.2f} m", lisa_model_, lisa_mesh_ ? "loaded" : "missing",
            doll_mesh_ ? "loaded" : "missing", poser_ && poser_->Valid() ? "ready" : "missing", doll_walk_frames_, doll_walk_travel_);

    if (own_materials_begin_ == 0) own_materials_begin_ = game_.Models().Textures().MaterialCount();
    armory_ready_ = true;
    models_.Build(game_.Models());
    own_materials_end_ = std::max(own_materials_end_, game_.Models().Textures().MaterialCount());
    MakeArms();

    namespace s = survival_sounds;
    auto recorded = [](std::string_view name, auto synthesized) {
        s::Sound sound = s::FromFile(name);
        return sound ? sound : synthesized();
    };
    auto own = [](std::vector<s::Sound> sounds, float gain_db, float vary_db, float speed, float vary_speed) {
        Cue cue{{}, gain_db, vary_db, speed, vary_speed};
        for (s::Sound& sound : sounds) {
            if (sound) cue.takes.push_back(s::Hold(std::move(sound), true));
        }
        return cue;
    };
    GameAudio* audio = game_.Audio();
    auto pt = [audio](std::span<const uint32_t> ids, float gain_db, float vary_db = 0.0f, float speed = 1.0f, float vary_speed = 0.0f) {
        Cue cue{{}, gain_db, vary_db, speed, vary_speed};
        for (uint32_t id : ids) {
            s::Sound media = audio ? audio->FindMedia(id) : nullptr;
            if (media) {
                cue.takes.push_back(s::Hold(std::move(media), speed != 1.0f || vary_speed > 0.0f));
            } else if (audio) {
                LogWarn("survival: P.T.'s media {} is not loaded", id);
            }
        }
        return cue;
    };
    auto shots = [&](SurvivalWeapon weapon, std::string_view name, auto synthesized) {
        std::vector<s::Sound> pool = s::FromFilePool(name);
        if (pool.empty()) pool.push_back(synthesized());
        shot_[static_cast<size_t>(weapon)] = own(std::move(pool), 6.0f, 0.7f, 1.0f, 0.012f);
    };
    shots(SurvivalWeapon::Handgun, "shot_handgun", [] { return s::Gunshot(95.0f, 0.06f, 0.035f, 3800.0f, 0.24f, 0.8f, 21); });
    shots(SurvivalWeapon::Shotgun, "shot_shotgun", [] { return s::Gunshot(54.0f, 0.13f, 0.075f, 2100.0f, 0.45f, 0.97f, 22); });
    shots(SurvivalWeapon::Magnum, "shot_revolver", [] { return s::Gunshot(66.0f, 0.11f, 0.06f, 3000.0f, 0.55f, 0.97f, 23); });
    shots(SurvivalWeapon::Lever, "shot_lever", [] { return s::Gunshot(60.0f, 0.12f, 0.07f, 2600.0f, 0.5f, 0.97f, 24); });
    shots(SurvivalWeapon::Double, "shot_double", [] { return s::Gunshot(52.0f, 0.14f, 0.08f, 2000.0f, 0.45f, 0.97f, 25); });
    for (size_t w = 0; w < shot_.size(); ++w) {
        for (const s::Take& take : shot_[w].takes) {
            if (survival_acoustics::Source source = survival_acoustics::Prepare(take); source.Valid()) shot_sources_[w].push_back(std::move(source));
        }
    }
    reload_sound_ = recorded("reload_handgun_empty", [] { return s::Reload(true); });
    reload_tactical_ = recorded("reload_handgun", [] { return s::Reload(false); });
    empty_ = recorded("dry_fire", [] { return s::DryFire(); });
    swap_sound_ = recorded("swap", [] { return s::Swap(); });
    pump_sound_ = recorded("pump", [] { return s::Pump(); });
    shell_in_sound_ = recorded("shell_in", [] { return s::ShellIn(); });
    revolver_reload_ = recorded("reload_revolver", [] { return s::RevolverReload(); });
    lever_sound_ = recorded("lever", [] { return s::Pump(); });
    lever_round_sound_ = recorded("lever_round", [] { return s::ShellIn(); });
    double_reload_ = recorded("reload_double", [] { return s::RevolverReload(); });
    take_one_sound_ = s::FromFile("radio_take_one");
    if (GameAudio* audio = game_.Audio(); audio && !take_one_sound_) {
        static const int take_one = [] { const char* v = std::getenv("PT_SURVIVAL_TAKE_ONE"); return v ? std::atoi(v) : 1; }();
        const std::shared_ptr<const audio::Media> riddle = audio->FindMedia(551553252u), shout = audio->FindMedia(541620790u),
                                                  hiss = audio->FindMedia(104800781u);
        take_one_sound_ = s::RadioTakeOne(riddle.get(), shout.get(), hiss.get(), take_one);
    }
    voices_ = s::FromFolder("voice");
    siren_sound_ = s::Siren();
    tick_sound_ = s::Tick(false);
    casing_ = own({s::Casing(false)}, -12.0f, 1.5f, 1.0f, 0.05f);
    shell_ = own({s::Casing(true)}, -12.0f, 1.5f, 1.0f, 0.05f);
    magazine_ = own({s::Drop()}, -6.0f, 1.5f, 1.0f, 0.06f);
    impact_ = own({s::BulletImpact()}, -11.0f, 2.0f, 1.0f, 0.08f);
    armor_ = own({s::ArmorDeflect()}, -3.0f, 1.5f, 1.0f, 0.06f);
    glass_ = own({s::GlassBreak()}, -1.0f, 1.5f, 1.0f, 0.07f);
    pickup_ = own({s::PickupTake()}, -3.5f, 1.0f, 1.0f, 0.04f);
    heal_cue_ = own({s::HealTake()}, -2.5f, 0.5f, 1.0f, 0.02f);
    walker_step_ = pt(kBloodSteps, -14.0f, 2.0f, 0.92f, 0.05f);
    crawl_ = pt(kBloodSteps, -17.0f, 3.0f, 1.35f, 0.08f);
    stomp_ = pt(kConcreteSteps, 5.0f, 1.5f, 0.55f, 0.04f);
    thud_ = own({s::HeavyThud()}, -1.0f, 1.5f, 1.0f, 0.05f);
    body_fall_ = own({s::BodyFall()}, -3.0f, 2.0f, 1.0f, 0.07f);
    hit_ = pt(kBlows, -9.0f, 2.0f, 1.18f, 0.06f);
    gore_ = pt(kBlood, 1.0f, 2.0f, 1.0f, 0.05f);
    rise_ = pt(kFridgeBlood, 3.0f, 2.0f, 0.78f, 0.05f);
    landing_ = own({s::PickupLand()}, -5.0f, 1.5f, 1.0f, 0.06f);
    lisa_laugh_ = pt(kLaugh, 0.0f, 1.0f);
    lisa_breath_ = pt(kBreath, 2.0f, 1.0f);
    lisa_run_ = pt(kChase, 1.0f, 1.0f, 1.0f, 0.03f);
    pounce_ = pt(kBreath, 5.0f, 1.0f, 1.6f, 0.08f);
    swing_ = own({s::Swing()}, -6.0f, 1.5f, 1.0f, 0.06f);
    hurt_ = pt(kBlows, -2.0f, 1.0f, 0.92f, 0.04f);
    confirm_ = pt(kBlows, -6.0f, 1.5f, 0.78f, 0.05f);
    confirm_head_ = pt(kBlood, -3.5f, 1.0f, 1.22f, 0.04f);
    confirm_kill_ = pt(kBlows, -5.0f, 1.0f, 0.62f, 0.03f);
    fall_ = own({s::BodyFall()}, 1.0f, 0.0f, 0.88f, 0.0f);
    lights_fail_ = pt(kLightOff, 0.0f);
    lights_down_ = pt(kLightDown, 2.0f);
    lights_on_ = pt(kLightOn, -6.0f);
    burst_ = pt(kLightOn, -2.0f, 0.0f, 0.85f);
    bell_ = pt(kBell, 8.0f, 1.0f, 1.0f, 0.01f);
    LogInfo("survival: shot takes {} {} {}, P.T.'s takes: {} steps, {} blows, {} of Lisa", shot_[static_cast<size_t>(SurvivalWeapon::Handgun)].takes.size(),
            shot_[static_cast<size_t>(SurvivalWeapon::Shotgun)].takes.size(), shot_[static_cast<size_t>(SurvivalWeapon::Magnum)].takes.size(),
            walker_step_.takes.size() + stomp_.takes.size(), hit_.takes.size(), lisa_laugh_.takes.size() + lisa_breath_.takes.size() + lisa_run_.takes.size());
}

float SurvivalNight::SoundRand() {
    sound_rng_ = sound_rng_ * 1664525u + 1013904223u;
    return static_cast<float>(sound_rng_ >> 8) / 16777216.0f;
}

void SurvivalNight::Play(Cue& cue, const glm::vec3* at, float gain_db, float speed) {
    GameAudio* audio = game_.Audio();
    if (cue.takes.empty() || !audio) return;
    size_t pick = static_cast<size_t>(SoundRand() * static_cast<float>(cue.takes.size())) % cue.takes.size();
    if (pick == cue.last && cue.takes.size() > 1) pick = (pick + 1) % cue.takes.size();
    cue.last = pick;
    const float db = cue.gain_db + gain_db + voice_duck_db_ + (2.0f * SoundRand() - 1.0f) * cue.vary_db;
    const float rate = cue.speed * speed * (1.0f + (2.0f * SoundRand() - 1.0f) * cue.vary_speed);
    survival_sounds::Sound sound = survival_sounds::Varied(cue.takes[pick], std::pow(10.0f, db / 20.0f), rate);
    if (!sound) return;
    if (at) {
        audio->PlayMediaAs(std::move(sound), kStreetDonor, *at);
    } else {
        audio->PlayMedia(std::move(sound), nullptr);
    }
}

void SurvivalNight::PlayShot(const glm::vec3& ear, const glm::vec3& forward, const glm::vec3& right) {
    Cue& cue = shot_[static_cast<size_t>(weapon_)];
    std::vector<survival_acoustics::Source>& sources = shot_sources_[static_cast<size_t>(weapon_)];
    GameAudio* audio = game_.Audio();
    if (!audio || sources.empty()) {
        Play(cue, nullptr);
        return;
    }
    size_t pick = static_cast<size_t>(SoundRand() * static_cast<float>(sources.size())) % sources.size();
    if (pick == cue.last && sources.size() > 1) pick = (pick + 1) % sources.size();
    cue.last = pick;
    const float db = cue.gain_db + voice_duck_db_ + (2.0f * SoundRand() - 1.0f) * cue.vary_db;
    const float rate = cue.speed * (1.0f + (2.0f * SoundRand() - 1.0f) * cue.vary_speed);
    const auto ray = [this](const glm::vec3& from, const glm::vec3& dir, float length, float& distance, glm::vec3& normal) {
        RayHit hit;
        bool found = false;
        if (game_.Collision().Raycast(from, dir, length, hit)) {
            found = true;
            distance = hit.distance;
            normal = hit.normal;
        }
        for (const SurvivalModels::Block& b : models_.backdrop_blocks) {
            const glm::vec3 o(b.to_local * glm::vec4(from, 1.0f));
            const glm::vec3 d(b.to_local * glm::vec4(dir, 0.0f));
            float lo = 0.0f, hi = found ? distance : length;
            int axis_in = -1;
            bool miss = false;
            for (int k = 0; k < 3 && !miss; ++k) {
                if (std::abs(d[k]) < 1e-7f) {
                    miss = std::abs(o[k]) > b.half[k];
                    continue;
                }
                float t0 = (-b.half[k] - o[k]) / d[k], t1 = (b.half[k] - o[k]) / d[k];
                if (t0 > t1) std::swap(t0, t1);
                if (t0 > lo) {
                    lo = t0;
                    axis_in = k;
                }
                hi = std::min(hi, t1);
                miss = lo > hi;
            }
            if (miss || axis_in < 0 || lo <= 0.0f) continue;
            glm::vec3 n_local(0.0f);
            n_local[axis_in] = d[axis_in] > 0.0f ? -1.0f : 1.0f;
            found = true;
            distance = lo;
            normal = glm::normalize(glm::vec3(glm::transpose(b.to_local) * glm::vec4(n_local, 0.0f)));
        }
        return found;
    };
    const survival_acoustics::Space space = survival_acoustics::Probe(ray, ear, forward, right);
    survival_sounds::Sound sound = survival_acoustics::Render(sources[pick], space, std::pow(10.0f, db / 20.0f), rate, next_id_++);
    if (!sound) return;
    static const char* dump = std::getenv("PT_SHOT_DUMP");
    if (dump || shots_dumped_ == 0) LogInfo("survival: shot at ({:.1f} {:.1f} {:.1f}): {}", ear.x, ear.y, ear.z, space.Describe());
    if (dump) {
        std::vector<float> pcm(static_cast<size_t>(sound->Frames()) * 2);
        std::unique_ptr<audio::MediaReader> reader = sound->OpenReader();
        size_t done = 0;
        while (reader && done < sound->Frames()) {
            const uint32_t n = reader->Read(pcm.data() + done * 2, static_cast<uint32_t>(std::min<uint64_t>(4096, sound->Frames() - done)));
            if (n == 0) break;
            done += n;
        }
        const std::string path = std::format("{}/shot_{:03}.wav", dump, shots_dumped_);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            const uint32_t bytes = static_cast<uint32_t>(done * 2 * sizeof(float));
            auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f);
            u32(36 + bytes);
            std::fwrite("WAVEfmt ", 1, 8, f);
            u32(16);
            u16(3);
            u16(2);
            u32(48000);
            u32(48000 * 8);
            u16(8);
            u16(32);
            std::fwrite("data", 1, 4, f);
            u32(bytes);
            std::fwrite(pcm.data(), 1, bytes, f);
            std::fclose(f);
        }
    }
    ++shots_dumped_;
    audio->PlayMedia(std::move(sound), nullptr);
}

void SurvivalNight::Toll(int count) {
    const Camera view = game_.ViewCamera(true);
    const glm::vec3 ahead = glm::normalize(glm::vec3(view.Forward().x, 0.0f, view.Forward().z) + glm::vec3(1.0e-4f, 0.0f, 0.0f));
    bell_at_ = view.position + ahead * 18.0f + glm::vec3(0.0f, 12.0f, 0.0f);
    tolls_left_ = count;
    toll_wait_ = 0.0f;
}

void SurvivalNight::UpdateTolls(float dt) {
    toll_wait_ -= dt;
    if (tolls_left_ <= 0 || toll_wait_ > 0.0f) return;
    Play(bell_, &bell_at_);
    --tolls_left_;
    toll_wait_ = 1.6f;
}

void SurvivalNight::Stomp(const glm::vec3& at, float gain_db) {
    Play(stomp_, &at, gain_db);
    Play(thud_, &at, gain_db);
}

void SurvivalNight::PlaySound(const std::shared_ptr<const audio::Media>& media, const glm::vec3* at, uint32_t* playing) {
    if (static const bool log = std::getenv("PT_SURVIVAL_SOUND_LOG") != nullptr; log && media) {
        const std::pair<const std::shared_ptr<const audio::Media>*, const char*> known[] = {
            {&lever_sound_, "lever"}, {&lever_round_sound_, "lever_round"}, {&double_reload_, "reload_double"}, {&pump_sound_, "pump"},
            {&shell_in_sound_, "shell_in"}, {&revolver_reload_, "reload_revolver"}, {&reload_sound_, "reload_handgun_empty"},
            {&reload_tactical_, "reload_handgun"}, {&take_one_sound_, "take_one"}};
        for (const auto& [member, name] : known) {
            if (*member == media) LogInfo("survival sound: {} at {:.2f} s", name, time_);
        }
    }
    std::shared_ptr<const audio::Media> sound = media;
    if (sound && !playing && voice_duck_db_ < -0.1f) {
        sound = survival_sounds::Varied(survival_sounds::Hold(media, false), std::pow(10.0f, voice_duck_db_ / 20.0f), 1.0f);
    }
    const uint32_t id = sound && game_.Audio() ? game_.Audio()->PlayMedia(sound, at) : 0;
    if (playing) *playing = id;
}

uint32_t SurvivalNight::PostEvent(std::string_view name, const glm::vec3* at) {
    return game_.Audio() ? game_.Audio()->PostEvent(name, at) : 0;
}

void SurvivalNight::Start(int best_wave, SurvivalDifficulty difficulty, SurvivalStart start) {
    EnsureAssets();
    const bool restarting_on_street = active_;
    Stop();
    active_ = true;
    MatchCrossStreet(true);
    SettleStreetLayers(true);
    DressBareThings(true);
    if (street_material_ != kNoCrossStreet) city_.Ensure(game_.Models(), street_material_, kStreetTile);
    city_.Sheets(game_.Models(), true);
    LoadTaskModels();
    best_wave_ = best_wave;
    start_ = start;
    difficulty_ = start == SurvivalStart::Daily ? SurvivalDifficulty::Normal : difficulty;
    const SurvivalCheckpoint& kept = game_.SurvivalProfileData().checkpoints[static_cast<size_t>(difficulty_)];
    if (start_ == SurvivalStart::Continue && kept.wave <= 0) start_ = SurvivalStart::Fresh;
    record_ = 0;
    ResetRun();
    kept_wave_ = 0;
    if (start_ == SurvivalStart::Continue) LoadCheckpoint(kept);
    if (restarting_on_street) {
        PlaceUprightOnStreet();
    }
    Banner("pc_survival_title", 0, 3.5f);
    if (start_ == SurvivalStart::Daily) {
        Omen(DailyMutatorById(daily_mutator_).name_key, 4.5f);
    }
    if (const Stage* street = game_.Stages().Find("current")) {
        catch_ready_ = game_.Demos().IndexPackageDemo(kHallwayPackage, kCatchDemo, street->id);
        wake_ready_ = game_.Demos().IndexPackageDemo(kStartPackage, kWakeDemo, street->id);
    }
    if (!game_.GetPlayer().handy_light.enable) game_.ToggleStreetFlashlightImmediate();
    ApplyLooks(true);
    LogInfo("survival: started (best wave {}, difficulty {}, {})", best_wave_, static_cast<int>(difficulty_),
            start_ == SurvivalStart::Daily ? "the day's night" : start_ == SurvivalStart::Continue ? "from its checkpoint" : "from the first wave");
}

void SurvivalNight::ResetRun() {
    EndTask();
    task_ = Task{};
    fear_ = 0.0f;
    dark_time_ = 0.0f;
    supply_on_ = false;
    supply_ids_.clear();
    supply_next_ = 0;
    boss_due_ = false;
    boss_flash_ = 0.0f;
    phantoms_.clear();
    phantom_wait_ = 8.0f;
    whisper_wait_ = 20.0f;
    for (const Enemy& e : enemies_) {
        if (UsesLisa(e.kind)) game_.Demos().Gimmicks().ReleaseActor(e.id);
    }
    enemies_.clear();
    bodies_.clear();
    enemy_ragdolls_.clear();
    player_ragdoll_.Clear();
    player_fall_heard_ = false;
    has_last_hit_from_ = false;
    dropped_gun_.active = false;
    game_.GetPlayer().SetRagdoll(nullptr);
    pickups_.clear();
    tracers_.clear();
    debris_.clear();
    delayed_.clear();
    splits_.clear();
    slot_used_ = {};
    wave_ = 0;
    events_ = 0;
    night_seed_ = start_ == SurvivalStart::Daily  ? static_cast<uint32_t>(SurvivalToday()) * 2654435761u
                  : game_.Config().seed != 0u    ? (game_.Config().seed * 2654435761u) | 1u
                                                 : ReadTsc() | 1u;
    hud_daily_pending_ = start_ == SurvivalStart::Daily;
    plan_rng_ = night_seed_ ^ 0x6E164Bu;
    plan_ = PlanSurvivalWave(1, night_seed_);
    blackout_at_ = blackout_time_ = -1.0f;
    fog_level_ = siren_level_ = hell_level_ = 0.0f;
    lamps_out_ = 0;
    lamp_failing_ = lamp_fail_at_ = -1.0f;
    north_end_ = NorthEndOf(1);
    night_door_ = 0.0f;
    HushMidnight();
    kills_ = 0;
    headshots_ = 0;
    run_time_ = 0.0f;
    score_ = chain_ = best_chain_ = shots_ = hits_ = 0;
    chain_timer_ = wave_hurt_ = 0.0f;
    intensity_ = hurt_recent_ = hot_time_ = calm_time_ = relief_ = 0.0f;
    surge_ = 0;
    max_health_ = kDifficulty[static_cast<size_t>(difficulty_)].player_health;
    health_ = max_health_;
    stamina_ = 1.0f;
    sprinting_ = winded_ = false;
    sights_ = false;
    sights_w_ = 0.0f;
    aim_held_ = -1.0f;
    weapons_ = {};
    weapons_[static_cast<size_t>(SurvivalWeapon::Flashlight)].owned = true;
    WeaponState& handgun = weapons_[static_cast<size_t>(SurvivalWeapon::Handgun)];
    handgun.owned = true;
    handgun.clip = Spec(SurvivalWeapon::Handgun).clip_size;
    const float start_rounds_scale = std::max(1.0f, kDifficulty[static_cast<size_t>(difficulty_)].rounds);
    handgun.reserve = std::min(Spec(SurvivalWeapon::Handgun).max_reserve,
                               static_cast<int>(std::lround(static_cast<float>(Spec(SurvivalWeapon::Handgun).start_reserve) * start_rounds_scale)));
    weapon_ = SurvivalWeapon::Handgun;
    gun_ = GunAnim{};
    shot_timer_ = reload_timer_ = burst_timer_ = 0.0f;
    view_kick_ = view_kick_speed_ = gun_kick_ = gun_kick_speed_ = glm::vec2(0.0f);
    bloom_ = 0.0f;
    swap_ = 1.0f;
    dead_fall_ = dead_fall_speed_ = 0.0f;
    lean_ = 0.0f;
    caught_ = false;
    caught_lethal_ = true;
    caught_by_ = 0;
    if (waking_ && game_.Demos().IsPlaying(kWakeDemo)) {
        game_.Demos().Skip(kWakeDemo);
        game_.Demos().ResetPlayerBody();
    }
    waking_ = false;
    wake_third_ = false;
    wake_light_on_ = false;
    wake_banner_key_.clear();
    wake_banner_value_ = 0;
    if (wake_locked_) {
        game_.GetPlayer().locks.Remove('B', "SurvivalWake");
        wake_locked_ = false;
    }
    const uint32_t notice_serial = hud_.notice_serial, glitch_serial = hud_.glitch_serial, accuse_serial = hud_.accuse_serial;
    hud_ = SurvivalHud{};
    hud_.active = true;
    hud_.notice_serial = notice_serial;
    hud_.glitch_serial = glitch_serial;
    hud_.accuse_serial = accuse_serial;
    notices_.clear();
    notice_on_ = false;
    notice_task_key_.clear();
    glitch_wait_ = 30.0f;
    accuse_wait_ = 25.0f;
    accuse_last_ = -1;
    hud_.daily = hud_daily_pending_;
    credits_paid_ = false;
    daily_goal_fresh_ = false;
    missions_done_ = 0;
    bosses_down_ = 0;
    last_payout_total_ = 0;
    last_payout_daily_ = 0;
    boons_ = SurvivalBoons{};
    if (start_ == SurvivalStart::Daily) {
        const DailyMutatorDef& mut = DailyMutatorForDate(SurvivalToday());
        daily_mutator_ = mut.id;
        hud_.daily_mutator = mut.id;
        max_health_ = std::max(30.0f, max_health_ * mut.player_hp_mult);
        health_ = max_health_;
        if (mut.starting_boon >= 0 && mut.starting_boon < static_cast<int>(SurvivalBoon::Count)) {
            ++boons_.rank[static_cast<size_t>(mut.starting_boon)];
        }
        if (mut.heavy_iron) {
            for (const SurvivalWeapon hw : {SurvivalWeapon::Shotgun, SurvivalWeapon::Magnum}) {
                WeaponState& ws = weapons_[static_cast<size_t>(hw)];
                const WeaponSpec hs = Gun(hw);
                ws.owned = true;
                ws.clip = hs.clip_size;
                ws.reserve = hs.start_reserve;
            }
            weapon_ = SurvivalWeapon::Magnum;
        }
        if (mut.half_lamps) lamps_out_ = 3;
    } else {
        daily_mutator_ = 0;
        hud_.daily_mutator = -1;
    }
    offer_count_ = 0;
    flares_ = 0;
    flares_out_.clear();
    melee_timer_ = throw_timer_ = -1.0f;
    flash_click_timer_ = -1.0f;
    flash_click_toggled_ = false;
    pipe_hits_ = 0;
    pipe_time_ = 0.0f;
    last_breath_used_ = false;
    if (radio_noise_ && game_.Audio()) game_.Audio()->StopPlayingId(radio_noise_, 0.3f);
    radio_noise_ = 0;
    hud_.best_wave = best_wave_;
    hud_.difficulty = difficulty_;
    if (dead_locked_) {
        game_.GetPlayer().locks.Remove('B', "SurvivalDead");
        dead_locked_ = false;
    }
    if (offer_locked_) {
        game_.GetPlayer().locks.Remove('B', "SurvivalOffer");
        offer_locked_ = false;
    }
    stowing_ = false;
    draw_pick_ = 0.0f;
    sprint_hold_ = 0.0f;
    raise_ = raise_speed_ = body_recoil_ = body_recoil_speed_ = reload_front_ = reload_front_speed_ = 0.0f;
    beam_start_ = 0.0f;
    kept_break_ = false;
    marks_.clear();
    thrown_ = false;
    shove_told_ = false;
    HushVoice();
    if (heartbeat_ && game_.Audio()) game_.Audio()->StopPlayingId(heartbeat_, 1.0f);
    heartbeat_ = 0;
    if (low_heartbeat_ && game_.Audio()) game_.Audio()->StopPlayingId(low_heartbeat_, 1.0f);
    low_heartbeat_ = 0;
    since_swap_ = 0.0f;
    StartCountdown(6.0f);
}

void SurvivalNight::Stop() {
    if (active_) LogInfo("survival: stopped on wave {} ({} kills)", wave_, kills_);
    if (active_ && !hud_.dead) KeepRecord();
    if (active_) ApplyLooks(false);
    EndTask();
    HushMidnight();
    HushVoice();
    phantoms_.clear();
    fear_ = 0.0f;
    if (radio_noise_ && game_.Audio()) game_.Audio()->StopPlayingId(radio_noise_, 0.3f);
    radio_noise_ = 0;
    blackout_time_ = -1.0f;
    hell_level_ = 0.0f;
    DimStreetGlow(1.0f, glm::vec3(1.0f));
    MatchCrossStreet(false);
    SettleStreetLayers(false);
    city_.Sheets(game_.Models(), false);
    DressBareThings(false);
    if (active_ && models_.chest_light) game_.SetHandyPoseOverride(std::nullopt);
    if (active_) {
        game_.GetPlayer().SetElbowHints(std::nullopt, std::nullopt);
        game_.GetPlayer().SetAimStance(0.0f, 0.0f);
        game_.GetPlayer().SetSteady(0.0f);
    }
    if (caught_ && game_.Demos().IsPlaying(kCatchDemo)) game_.Demos().Skip(kCatchDemo);
    caught_ = false;
    if (waking_ && game_.Demos().IsPlaying(kWakeDemo)) {
        game_.Demos().Skip(kWakeDemo);
        game_.Demos().ResetPlayerBody();
    }
    waking_ = false;
    wake_third_ = false;
    wake_light_on_ = false;
    if (wake_locked_) {
        game_.GetPlayer().locks.Remove('B', "SurvivalWake");
        wake_locked_ = false;
    }
    const bool was_dead = hud_.dead;
    active_ = false;
    const uint32_t notice_serial = hud_.notice_serial, glitch_serial = hud_.glitch_serial, accuse_serial = hud_.accuse_serial;
    hud_ = SurvivalHud{};
    hud_.notice_serial = notice_serial;
    hud_.glitch_serial = glitch_serial;
    hud_.accuse_serial = accuse_serial;
    dead_fall_ = dead_fall_speed_ = 0.0f;
    if (was_dead) {
        game_.GetPlayer().pitch = game_.GetPlayer().target_pitch = 0.0f;
        game_.Effects().SetFadeColor(0, 0, 0, 255);
    }
    for (const Enemy& e : enemies_) {
        if (UsesLisa(e.kind)) game_.Demos().Gimmicks().ReleaseActor(e.id);
    }
    enemies_.clear();
    bodies_.clear();
    enemy_ragdolls_.clear();
    player_ragdoll_.Clear();
    player_fall_heard_ = false;
    has_last_hit_from_ = false;
    dropped_gun_.active = false;
    game_.GetPlayer().SetRagdoll(nullptr);
    pickups_.clear();
    tracers_.clear();
    debris_.clear();
    marks_.clear();
    flares_out_.clear();
    melee_timer_ = throw_timer_ = -1.0f;
    flash_click_timer_ = -1.0f;
    flash_click_toggled_ = false;
    pipe_hits_ = 0;
    slot_used_ = {};
    speed_scale_ = 1.0f;
    fov_kick_ = shake_ = lean_ = 0.0f;
    body_run_ = sprint_hold_ = 0.0f;
    raise_ = raise_speed_ = body_recoil_ = body_recoil_speed_ = reload_front_ = reload_front_speed_ = 0.0f;
    stowing_ = false;
    draw_pick_ = 0.0f;
    view_kick_ = glm::vec2(0.0f);
    game_.GetPlayer().sprint_rate = 1.0f;
    game_.GetPlayer().sprint_stride = 1.0f;
    if (dead_locked_) {
        game_.GetPlayer().locks.Remove('B', "SurvivalDead");
        dead_locked_ = false;
    }
    if (offer_locked_) {
        game_.GetPlayer().locks.Remove('B', "SurvivalOffer");
        offer_locked_ = false;
    }
    offer_count_ = 0;
    beam_start_ = 0.0f;
    if (heartbeat_ && game_.Audio()) game_.Audio()->StopPlayingId(heartbeat_, 0.5f);
    heartbeat_ = 0;
    if (surge_noise_ && game_.Audio()) game_.Audio()->StopPlayingId(surge_noise_, 0.3f);
    surge_noise_ = 0;
    if (low_heartbeat_ && game_.Audio()) game_.Audio()->StopPlayingId(low_heartbeat_, 0.5f);
    low_heartbeat_ = 0;
}

int SurvivalNight::TakeRecord() {
    return std::exchange(record_, 0);
}

void SurvivalNight::Banner(const std::string& key, int value, float seconds) {
    hud_.banner_key = key;
    hud_.banner_value = value;
    hud_.banner = seconds;
    Voice(key);
}

void SurvivalNight::Voice(std::string_view key, float delay) {
    const auto it = voices_.find(key);
    if (it != voices_.end()) Speak(it->second, key, delay);
}

void SurvivalNight::Speak(const std::shared_ptr<const audio::Media>& media, std::string_view name, float delay) {
    if (!media) return;
    if (voice_queue_.size() >= 2) voice_queue_.pop_front();
    voice_queue_.push_back({media, std::string(name), time_ + delay});
}

void SurvivalNight::UpdateVoice(float dt) {
    GameAudio* audio = game_.Audio();
    if (!audio) return;
    const bool speaking = voice_playing_ && audio->IsPlaying(voice_playing_);
    voice_duck_db_ = speaking ? std::max(-8.0f, voice_duck_db_ - dt * 80.0f) : std::min(0.0f, voice_duck_db_ + dt * 13.0f);
    if (speaking) return;
    voice_playing_ = 0;
    if (!voice_queue_.empty() && time_ >= voice_queue_.front().at) {
        if (!voice_noise_ && !radio_noise_) voice_noise_ = PostEvent("Play_radio_noise_lp", nullptr);
        PlaySound(survival_sounds::Varied(survival_sounds::Hold(voice_queue_.front().media, false), 1.6f, 1.0f), nullptr, &voice_playing_);
        LogInfo("survival: the radio says {}", voice_queue_.front().name);
        voice_queue_.pop_front();
        voice_quiet_ = 0.0f;
        return;
    }
    voice_quiet_ += dt;
    if (voice_noise_ && voice_queue_.empty() && voice_quiet_ > 0.3f) {
        audio->StopPlayingId(voice_noise_, 0.4f);
        voice_noise_ = 0;
    }
}

void SurvivalNight::HushVoice() {
    voice_queue_.clear();
    if (GameAudio* audio = game_.Audio()) {
        for (uint32_t* id : {&voice_playing_, &voice_noise_}) {
            if (*id) audio->StopPlayingId(*id, 0.3f);
        }
    }
    voice_playing_ = voice_noise_ = 0;
    voice_duck_db_ = 0.0f;
}

void SurvivalNight::Note(const std::string& key, int value, float hold) {
    hud_.pickup_key = key;
    hud_.pickup_value = value;
    hud_.pickup_note = hold;
}

int SurvivalNight::AliveEnemies() const {
    return static_cast<int>(std::count_if(enemies_.begin(), enemies_.end(), [](const Enemy& e) { return e.state != Enemy::State::Dying; }));
}

size_t SurvivalNight::TakeSkinSlot() {
    for (size_t i = 0; i < kSkinSlots; ++i) {
        if (!slot_used_[i] && time_ >= slot_free_at_[i]) {
            slot_used_[i] = true;
            return i;
        }
    }
    for (Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dying && e.timer < 1e3f) {
            e.timer = 1e3f;
            break;
        }
    }
    return kSkinSlots;
}

void SurvivalNight::StartCountdown(float seconds) {
    countdown_ = seconds;
    last_tick_ = static_cast<int>(std::ceil(seconds));
    if (phase_ != SurvivalPhase::Break) phase_ = SurvivalPhase::Countdown;
}

uint32_t SurvivalNight::PlanEvents() {
    const float w = static_cast<float>(wave_);
    struct Chance {
        uint32_t event;
        int from;
        float chance;
    };
    const Chance chances[] = {
        {kEventBlackout, 4, 0.18f + 0.004f * w}, {kEventFog, 9, 0.12f + 0.003f * w},   {kEventFlank, 13, 0.12f + 0.003f * w},
        {kEventHorde, 16, 0.10f + 0.003f * w},   {kEventSiren, 21, 0.06f + 0.002f * w}, {kEventDrought, 26, 0.06f + 0.002f * w},
        {kEventHunt, 36, 0.08f + 0.002f * w},
    };
    const int most = wave_ < 30 ? 1 : wave_ < 60 ? 2 : 3;
    uint32_t events = 0;
    int count = 0;
    const size_t first = static_cast<size_t>(PlanRand() * static_cast<float>(std::size(chances)));
    for (size_t i = 0; i < std::size(chances) && count < (events ? most + 1 : most); ++i) {
        const Chance& c = chances[(first + i) % std::size(chances)];
        if (wave_ < c.from || (events & c.event) != 0 || PlanRand() >= c.chance) continue;
        events |= c.event;
        ++count;
    }
    return events;
}

void SurvivalNight::StartWave() {
    if (blackout_time_ >= 0.0f) blackout_time_ = std::max(blackout_time_, blackout_length_ + 1.2f);
    if (offer_count_ > 0) TakeBoon(0);
    if (kept_break_) SaveCheckpoint(true);
    kept_break_ = false;
    last_breath_used_ = false;
    ++wave_;
    plan_rng_ = PlanSeed(wave_, 0u);
    if (boons_[SurvivalBoon::FlarePouch] > 0 && flares_ < MaxFlares()) {
        ++flares_;
        Note("pc_survival_pickup_flare");
    }
    phase_ = SurvivalPhase::Wave;
    supply_on_ = false;
    supply_ids_.clear();
    if (wave_ >= 12 && wave_ >= supply_next_ && StartSupply()) supply_next_ = wave_ + 3 + static_cast<int>(Rand() * 3.0f);
    const DifficultySpec& d = kDifficulty[static_cast<size_t>(difficulty_)];
    plan_ = PlanSurvivalWave(wave_, night_seed_);
    static const int env_mission = [] { const char* m = std::getenv("PT_SURVIVAL_MISSION"); return m ? std::clamp(std::atoi(m), 0, 5) : 0; }();
    if (const int forced = forced_mission_ ? forced_mission_ : env_mission; forced > 0) {
        plan_.mission = static_cast<SurvivalMission>(forced);
        if (plan_.mission == SurvivalMission::Fuse) plan_.events |= kEventBlackout;
        if (plan_.mission == SurvivalMission::Lamps) plan_.events |= kEventHell;
    }
    const float daily_count_mult = start_ == SurvivalStart::Daily ? DailyMutatorById(daily_mutator_).enemy_count_mult : 1.0f;
    to_spawn_ = std::min(wave_ > kSurvivalWaves ? 240 : 72, static_cast<int>(std::lround(static_cast<float>(plan_.count) * d.count * daily_count_mult)));
    wave_cap_ = plan_.cap;
    spawn_interval_ = plan_.interval;
    spawn_timer_ = 0.3f;
    spawned_this_wave_ = 0;
    lisa_due_ = plan_.lisa_last;
    wave_time_ = 0.0f;
    if (const char* forced = std::getenv("PT_SURVIVAL_EVENTS")) {
        events_ = static_cast<uint32_t>(std::strtoul(forced, nullptr, 0));
    } else {
        events_ = plan_.events | (plan_.more_events ? PlanEvents() : 0u);
    }
    if (start_ == SurvivalStart::Daily) {
        const DailyMutatorDef& mut = DailyMutatorById(daily_mutator_);
        if (mut.red_lamps) events_ |= kEventHell;
        if (mut.dense_fog) events_ |= kEventFog;
        if (mut.lisa_stalks && wave_ >= 2) lisa_due_ = true;
    }
    blackout_at_ = -1.0f;
    if (events_ & kEventBlackout) {
        const bool whole = plan_.boss != SurvivalBoss::None || wave_ == 28 || plan_.mission == SurvivalMission::Fuse;
        blackout_at_ = whole ? 0.0f : 6.0f + 14.0f * PlanRand();
        blackout_length_ = whole ? 900.0f : std::min(30.0f, 7.0f + 0.8f * static_cast<float>(std::min(wave_, 30)) + 6.0f * PlanRand());
    }
    if (events_ & kEventHorde) {
        to_spawn_ += 8;
        wave_cap_ += 6;
        spawn_interval_ *= 0.5f;
    }
    const int night = SurvivalNightOf(wave_), in_night = SurvivalWaveInNight(wave_);
    if (night > 1 && in_night == 1) {
        Banner("pc_survival_night", night, 3.0f);
    } else {
        Banner("pc_survival_wave", in_night, 2.5f);
    }
    lamps_out_ = SurvivalLampsOut(wave_);
    if (start_ == SurvivalStart::Daily && DailyMutatorById(daily_mutator_).half_lamps) {
        lamps_out_ = std::max(lamps_out_, 3);
    }
    lamp_failing_ = -1.0f;
    lamp_fail_at_ = night == 2 && in_night % 5 == 0 ? 8.0f + 10.0f * PlanRand() : -1.0f;
    if (events_ & kEventSiren) {
        PlaySound(siren_sound_, nullptr);
    } else {
        Toll(3);
    }
    static constexpr std::pair<uint32_t, const char*> kOmens[] = {
        {kEventHell, "pc_survival_omen_hell"},   {kEventHunt, "pc_survival_omen_hunt"},   {kEventFlank, "pc_survival_omen_flank"},     {kEventHorde, "pc_survival_omen_horde"},
        {kEventFog, "pc_survival_omen_fog"},     {kEventDrought, "pc_survival_omen_drought"}, {kEventSiren, "pc_survival_omen_siren"},
    };
    const char* omen = nullptr;
    float omen_time = 0.0f;
    for (const auto& [event, key] : kOmens) {
        if (events_ & event) {
            omen = key;
            omen_time = 4.0f;
            break;
        }
    }
    if (plan_.theme) {
        omen = plan_.theme;
        omen_time = 4.5f;
    }
    if (omen) Omen(omen, omen_time);
    if (wave_ > best_wave_) {
        best_wave_ = wave_;
        record_ = wave_;
    }
    hud_.best_wave = best_wave_;
    LogInfo("survival: wave {} ({} enemies, {} at once, every {:.2f} s, events {:#x}, health x{:.2f}, affixes {:#x} at {:.0f} %{}{})", wave_,
            to_spawn_, wave_cap_, spawn_interval_, events_, plan_.health, plan_.affixes, plan_.affix_share * 100.0f,
            plan_.theme ? std::string(", ") + plan_.theme : std::string(), plan_.mission != SurvivalMission::None ? ", a task" : "");
    StartTask();
    boss_due_ = plan_.boss != SurvivalBoss::None || plan_.giant_boss;
}

SurvivalEnemyKind SurvivalNight::PickKind() {
    if (lisa_due_ && to_spawn_ == 1) {
        lisa_due_ = false;
        return SurvivalEnemyKind::Lisa;
    }
    auto alive = [&](SurvivalEnemyKind k) {
        return std::any_of(enemies_.begin(), enemies_.end(), [k](const Enemy& e) { return e.kind == k && e.state != Enemy::State::Dying; });
    };
    if ((events_ & kEventHunt) && !alive(SurvivalEnemyKind::Lisa) && (spawned_this_wave_ == 0 || Rand() < 0.2f)) {
        return SurvivalEnemyKind::Lisa;
    }
    if ((events_ & kEventHorde) && PlanRand() < 0.6f) return SurvivalEnemyKind::Crawler;
    auto count = [&](SurvivalEnemyKind k) {
        return std::count_if(enemies_.begin(), enemies_.end(), [k](const Enemy& e) { return e.kind == k && e.state != Enemy::State::Dying; });
    };
    const float lisa_roll = PlanRand();
    if (count(SurvivalEnemyKind::Lisa) == 0 && lisa_roll < plan_.lisa_chance) return SurvivalEnemyKind::Lisa;
    const float w = static_cast<float>(wave_);
    const float doll = plan_.weights[0];
    const float crawler = plan_.weights[1];
    const float stretched = count(SurvivalEnemyKind::Stretched) < (plan_.weights[2] > plan_.weights[0] ? 6 : 3) ? plan_.weights[2] : 0.0f;
    const float mother = count(SurvivalEnemyKind::Mother) < (plan_.weights[3] > plan_.weights[0] ? 4 : 2) && lisa_mesh_ ? plan_.weights[3] : 0.0f;
    if (plan_.giants > 0 && plan_.giant_first && spawned_this_wave_ < std::min(plan_.giants, wave_ >= 20 ? 2 : 1)) return SurvivalEnemyKind::Giant;
    const float giant = count(SurvivalEnemyKind::Giant) < plan_.giants ? 0.05f + 0.01f * std::min(w, 40.0f) : 0.0f;
    const float r = PlanRand() * (doll + crawler + stretched + mother + giant);
    if (r < doll) return SurvivalEnemyKind::Doll;
    if (r < doll + crawler) return SurvivalEnemyKind::Crawler;
    if (r < doll + crawler + stretched) return SurvivalEnemyKind::Stretched;
    if (r < doll + crawler + stretched + mother) return SurvivalEnemyKind::Mother;
    return giant > 0.0f ? SurvivalEnemyKind::Giant : SurvivalEnemyKind::Doll;
}

float SurvivalNight::GroundAt(const glm::vec3& p, float fallback) const {
    RayHit hit;
    if (game_.Collision().Raycast(p + glm::vec3(0.0f, 1.2f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 4.0f, hit) && hit.normal.y > 0.5f) {
        return hit.point.y;
    }
    return fallback;
}

bool SurvivalNight::Blocked(const glm::vec3& from, const glm::vec3& dir, float distance) const {
    RayHit hit;
    for (const float h : {0.45f, 1.2f}) {
        if (game_.Collision().Raycast(from + glm::vec3(0.0f, h, 0.0f), dir, distance, hit) && std::abs(hit.normal.y) < 0.6f) return true;
    }
    return false;
}

bool SurvivalNight::SpawnPoint(glm::vec3& out, float closest, float farthest) {
    const glm::vec3 player = game_.GetPlayer().Feet();
    static const char* forced = std::getenv("PT_SURVIVAL_SPAWN");
    if (spawn_at_set_) {
        const Camera view = game_.ViewCamera(true);
        glm::vec3 ahead = view.Forward();
        ahead.y = 0.0f;
        ahead = glm::length(ahead) > 1e-4f ? glm::normalize(ahead) : glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 side(-ahead.z, 0.0f, ahead.x);
        out = player + ahead * spawn_at_.x + side * spawn_at_.y;
        out.y = GroundAt(out, player.y);
        return true;
    }
    if (float x = 0.0f, z = 0.0f; forced && std::sscanf(forced, "%f %f", &x, &z) == 2) {
        out = glm::vec3(x, 0.0f, z);
        out.y = GroundAt(out + glm::vec3(0.0f, player.y, 0.0f), player.y);
        return true;
    }
    const float behind = -game_.ViewCamera(true).Forward().z;
    const bool flank = (events_ & kEventFlank) && std::abs(behind) > 0.2f && Rand() < 0.5f;
    for (int attempt = 0; attempt < 24; ++attempt) {
        const float side = flank ? (behind > 0.0f ? 1.0f : -1.0f) : Rand() < 0.6f ? 1.0f : -1.0f;
        float z = player.z + side * (closest + (farthest - closest) * Rand());
        if (z < kArenaMin.y + 2.0f || z > north_end_ - 2.0f) z = player.z - (z - player.z);
        z = std::clamp(z, kArenaMin.y + 2.0f, north_end_ - 2.0f);
        const float x = kRoadMinX + (kRoadMaxX - kRoadMinX) * Rand();
        const std::span<const StreetBody> bodies = StreetBodies();
        if (std::any_of(bodies.begin(), bodies.end(), [&](const StreetBody& b) { return b.Holds({x, z}); })) continue;
        RayHit hit;
        const float level = spawn_level_known_ && std::abs(player.y - spawn_level_) > 1.2f ? spawn_level_ : player.y;
        const glm::vec3 top(x, level + 3.0f, z);
        if (!game_.Collision().Raycast(top, glm::vec3(0.0f, -1.0f, 0.0f), 6.0f, hit) || hit.normal.y < 0.7f) continue;
        if (std::abs(hit.point.y - level) > 1.2f) continue;
        std::vector<SphereHit> touching;
        game_.Collision().SphereOverlap(hit.point + glm::vec3(0.0f, 0.9f, 0.0f), 0.5f, touching);
        if (!touching.empty()) continue;
        if (glm::distance(glm::vec2(x, z), glm::vec2(player.x, player.z)) < closest * 0.7f) continue;
        if (Crowded(hit.point)) continue;
        out = hit.point;
        return true;
    }
    return false;
}

bool SurvivalNight::Reaches(const Enemy& e, const glm::vec3& feet) const {
    if (std::abs(e.position.y - feet.y) > 1.6f) return false;
    const glm::vec3 from = e.position + glm::vec3(0.0f, e.kind == SurvivalEnemyKind::Crawler ? 0.4f : 1.1f * KindScale(e.kind), 0.0f);
    const glm::vec3 line = feet + glm::vec3(0.0f, 1.2f, 0.0f) - from;
    const float far_off = glm::length(line);
    RayHit wall;
    return far_off < 0.6f || !game_.Collision().Raycast(from, line / far_off, far_off - 0.3f, wall);
}

bool SurvivalNight::Crowded(const glm::vec3& at) const {
    return std::any_of(enemies_.begin(), enemies_.end(), [&](const Enemy& e) {
        const float room = e.kind == SurvivalEnemyKind::Giant ? 2.0f : 1.1f;
        return e.state != Enemy::State::Dying && glm::distance(glm::vec2(e.position.x, e.position.z), glm::vec2(at.x, at.z)) < room;
    });
}

void SurvivalNight::SpawnEnemy(SurvivalEnemyKind kind) {
    if (UsesLisa(kind) ? !lisa_mesh_ : !doll_mesh_ || !poser_ || !poser_->Valid()) {
        kind = UsesLisa(kind) && doll_mesh_ ? SurvivalEnemyKind::Doll : SurvivalEnemyKind::Mother;
        if (UsesLisa(kind) ? !lisa_mesh_ : !doll_mesh_) return;
    }
    uint8_t affix = PickAffix(kind);
    glm::vec3 at;
    bool placed = false;
    if (affix & kAffixPerched) {
        FindPerches();
        const glm::vec3 feet = game_.GetPlayer().Feet();
        std::vector<glm::vec3> free;
        for (const glm::vec3& p : perches_) {
            const float d = glm::distance(glm::vec2(p.x, p.z), glm::vec2(feet.x, feet.z));
            const bool taken = std::any_of(enemies_.begin(), enemies_.end(), [&](const Enemy& o) { return glm::distance(o.position, p) < 1.0f; });
            if (d > 8.0f && d < 22.0f && !taken) free.push_back(p);
        }
        if (!free.empty()) {
            at = free[std::min(free.size() - 1, static_cast<size_t>(PlanRand() * static_cast<float>(free.size())))];
            placed = true;
        } else {
            affix &= static_cast<uint8_t>(~kAffixPerched);
        }
    }
    const bool near_rise = UsesLisa(kind) || kind == SurvivalEnemyKind::Giant;
    if (!placed && !UsesLisa(kind) && kind != SurvivalEnemyKind::Giant) {
        const float share = DoorSpawn(at);
        if (share > 0.0f && Rand() < share && !Crowded(at)) placed = true;
    }
    if (!placed && !SpawnPoint(at, near_rise ? 12.0f : 20.0f, near_rise ? 24.0f : 33.0f)) return;
    SpawnEnemyAt(kind, at, 1.0f, affix, true);
}

SurvivalNight::Enemy* SurvivalNight::SpawnEnemyAt(SurvivalEnemyKind kind, const glm::vec3& at, float rise, uint8_t affix, bool counted) {
    if (UsesLisa(kind) ? !lisa_mesh_ : !doll_mesh_ || !poser_ || !poser_->Valid()) return nullptr;
    const size_t slot = TakeSkinSlot();
    if (slot >= kSkinSlots) return nullptr;
    const KindSpec& spec = Kind(kind);
    const DifficultySpec& d = kDifficulty[static_cast<size_t>(difficulty_)];
    Enemy e;
    e.id = 0x5500u + next_id_++;
    e.kind = kind;
    e.position = at;
    const glm::vec3 to = game_.GetPlayer().Feet() - at;
    e.yaw = std::atan2(to.x, to.z);
    const float daily_hp = start_ == SurvivalStart::Daily ? DailyMutatorById(daily_mutator_).enemy_hp_mult : 1.0f;
    const float daily_spd = start_ == SurvivalStart::Daily ? DailyMutatorById(daily_mutator_).enemy_speed_mult : 1.0f;
    e.max_health = e.health = spec.health * plan_.health * d.health * daily_hp * ((affix & kAffixArmored) ? 1.15f : 1.0f);
    e.speed = spec.speed * (0.92f + 0.16f * Rand()) * plan_.speed * d.speed * daily_spd * ((events_ & kEventSiren) ? 1.15f : 1.0f);
    if (!e.boss && (events_ & kEventSiren) == 0) {
        e.speed = std::min(e.speed, 3.92f);
    }
    e.damage = spec.damage * plan_.damage * d.damage;
    e.affix = affix;
    e.perched = (affix & kAffixPerched) != 0;
    e.timer = spec.rise * (1.0f - std::clamp(rise, 0.0f, 1.0f));
    e.slot = slot;
    e.anim_frame = Rand() * 60.0;
    e.gait = Rand();
    e.weave = Rand() * 6.28f;
    e.twitch_wait = 0.5f + Rand();
    e.head_radius = spec.head_radius;
    e.head = at + glm::vec3(0.0f, 1.6f, 0.0f);
    enemies_.push_back(e);
    if (counted) {
        --to_spawn_;
        ++spawned_this_wave_;
    }
    Play(kind == SurvivalEnemyKind::Lisa ? lisa_laugh_ : kind == SurvivalEnemyKind::Crawler ? crawl_ : rise_, &at);
    if (kind == SurvivalEnemyKind::Lisa) Banner("pc_survival_lisa", 0, 3.0f);
    if (kind == SurvivalEnemyKind::Giant) {
        Stomp(at, 3.0f);
        shake_ = std::min(1.0f, shake_ + 0.45f);
    }
    LogInfo("survival: {} rises at ({:.1f} {:.1f} {:.1f}), {:.0f} hp{}", spec.name, at.x, at.y, at.z, e.health,
            affix ? std::format(", affix {:#x}", affix) : std::string());
    return &enemies_.back();
}

uint8_t SurvivalNight::PickAffix(SurvivalEnemyKind kind) {
    if (!plan_.affixes || PlanRand() >= plan_.affix_share) return 0;
    uint8_t can = 0;
    switch (kind) {
    case SurvivalEnemyKind::Doll: can = kAffixSplitter | kAffixArmored | kAffixShy | kAffixScreamer; break;
    case SurvivalEnemyKind::Stretched: can = kAffixSplitter | kAffixArmored | kAffixShy; break;
    case SurvivalEnemyKind::Crawler: can = kAffixShy | kAffixPerched; break;
    default: return 0;
    }
    can &= plan_.affixes;
    int n = 0;
    uint8_t pool[8];
    for (uint8_t bit = 1; bit != 0 && bit <= kAffixPerched; bit = static_cast<uint8_t>(bit << 1)) {
        if (can & bit) pool[n++] = bit;
    }
    return n ? pool[std::min(n - 1, static_cast<int>(PlanRand() * static_cast<float>(n)))] : 0;
}

void SurvivalNight::FindPerches() {
    if (perches_found_) return;
    perches_found_ = true;
    const float road = game_.GetPlayer().Feet().y;
    for (float z = kArenaMin.y + 2.0f; z < kArenaMax.y - 2.0f; z += 0.6f) {
        for (float x = kArenaMin.x + 1.0f; x < kArenaMax.x - 1.0f; x += 0.6f) {
            RayHit hit;
            if (!game_.Collision().Raycast(glm::vec3(x, road + 4.0f, z), glm::vec3(0.0f, -1.0f, 0.0f), 6.0f, hit) || hit.normal.y < 0.85f) continue;
            const float up = hit.point.y - road;
            if (up < 0.9f || up > 1.8f) continue;
            RayHit below;
            bool road_near = false;
            for (const glm::vec2 o : {glm::vec2(2.5f, 0.0f), glm::vec2(-2.5f, 0.0f), glm::vec2(0.0f, 3.0f), glm::vec2(0.0f, -3.0f)}) {
                if (game_.Collision().Raycast(glm::vec3(x + o.x, road + 4.0f, z + o.y), glm::vec3(0.0f, -1.0f, 0.0f), 6.0f, below) &&
                    std::abs(below.point.y - road) < 0.25f) {
                    road_near = true;
                }
            }
            if (!road_near) continue;
            if (std::any_of(perches_.begin(), perches_.end(), [&](const glm::vec3& p) { return glm::distance(p, hit.point) < 1.6f; })) continue;
            perches_.push_back(hit.point);
        }
    }
    LogInfo("survival: {} perches on the cars", perches_.size());
}

bool SurvivalNight::InBeam(const Enemy& e) const {
    if (!game_.GetPlayer().handy_light.enable) return false;
    glm::vec3 from, beam;
    game_.HandyBeam(from, beam);
    const glm::vec3 chest = e.position + glm::vec3(0.0f, e.kind == SurvivalEnemyKind::Crawler ? 0.45f : 1.1f, 0.0f);
    const glm::vec3 to = chest - from;
    const float distance = glm::length(to);
    if (distance < 0.5f || distance > 18.0f || glm::dot(to / distance, glm::normalize(beam)) < 0.95f) return false;
    RayHit wall;
    return !game_.Collision().Raycast(from, to / distance, distance - 0.3f, wall);
}

void SurvivalNight::Scream(Enemy& e) {
    Play(lisa_laugh_, &e.position, 6.0f, 0.62f);
    Play(pounce_, &e.position, 2.0f, 0.8f);
    int roused = 0;
    for (Enemy& o : enemies_) {
        if (&o == &e || o.state == Enemy::State::Dying || o.kind == SurvivalEnemyKind::Giant || UsesLisa(o.kind)) continue;
        if (glm::distance(o.position, e.position) > 14.0f) continue;
        o.frenzy = 6.0f;
        ++roused;
    }
    shake_ = std::min(1.0f, shake_ + 0.15f);
    LogInfo("survival: a screamer screams, {} roused", roused);
}

void SurvivalNight::PoseLisa(Enemy& e, std::string_view motion, float rate, float dt) {
    if (e.motion != motion) {
        e.motion = std::string(motion);
        if (motion != kWalk) e.anim_frame = 0.0;
    }
    float natural = 0.0f;
    if (auto it = root_speed_.find(motion); it != root_speed_.end()) natural = it->second;
    if (motion == kWalk && natural < 0.5f) natural = 1.0f;
    float play = rate;
    if (rate < 0.0f) {
        play = natural > 0.02f ? std::clamp(e.speed / natural, 0.55f, 4.0f) : 1.0f;
    }
    e.anim_frame += dt * kMotionFps * play;
    GimmickAnimation& anim = game_.Demos().Gimmicks();
    if (!anim.PoseActor(e.id, lisa_model_, lisa_parts_, motion, e.anim_frame, 8.0f, skins_[e.slot])) return;
    if (!root_speed_.contains(motion)) {
        const float speed = anim.ActorRootSpeed(e.id);
        root_speed_[std::string(motion)] = speed;
        LogInfo("survival: motion {} walks {:.2f} m/s", motion, speed);
    }
}

void SurvivalNight::PoseEnemy(Enemy& e, float dt) {
    const KindSpec& spec = Kind(e.kind);
    const glm::vec3 feet = game_.GetPlayer().Feet();
    const bool moving = e.state == Enemy::State::Chasing && e.stagger <= 0.0f && e.pause <= 0.0f;
    e.twitch_wait -= dt;
    if (e.twitch_wait <= 0.0f) {
        e.twitch_wait = 0.35f + 1.6f * Rand();
        e.twitch_to = Rand() < 0.3f ? glm::vec2(0.0f) : glm::vec2((Rand() - 0.5f) * 0.9f, (Rand() - 0.5f) * 1.1f);
    }
    e.twitch += (e.twitch_to - e.twitch) * std::min(1.0f, dt * 26.0f);
    const float freeze = hit_pause_ > 0.0f && e.hurt > 0.85f ? 0.0f : 1.0f;
    if (UsesLisa(e.kind)) {
        if (e.state == Enemy::State::Dying && physics_ && lisa_skeleton_ && !skins_[e.slot].empty()) {
            anim::Ragdoll& rd = enemy_ragdolls_[e.id];
            const glm::mat4 m = EnemyTransform(e, false);
            if (!rd.Active()) {
                const size_t n = lisa_skeleton_->Size();
                std::vector<glm::mat4> bw(n, glm::mat4(1.0f));
                for (size_t b = 0; b < n && b < skins_[e.slot].size(); ++b) {
                    bw[b] = skins_[e.slot][b] * glm::translate(glm::mat4(1.0f), lisa_skeleton_->bind_world[b]);
                }
                if (rd.Activate(*lisa_skeleton_, bw, m, KindScale(e.kind), e.knock)) {
                    e.ragdoll = true;
                    glm::vec3 d = e.has_last_hit ? e.last_hit_dir : glm::vec3(e.fall_dir.x, 0.0f, e.fall_dir.y);
                    d = glm::length(d) > 1e-3f ? glm::normalize(d) : glm::vec3(-std::sin(e.yaw), 0.0f, -std::cos(e.yaw));
                    rd.SetCollapse(d, e.last_hit_head);
                    if (e.last_hit_head) {
                        rd.AddPartImpulse(anim::Ragdoll::kHead, d * 4.8f + glm::vec3(0.0f, 1.0f, 0.0f));
                        rd.AddPartImpulse(anim::Ragdoll::kChest, d * 1.4f);
                    } else {
                        const glm::vec3 at = e.has_last_hit ? e.last_hit_at : rd.ChestPosition();
                        rd.AddImpulse(at, d * std::clamp(e.last_hit_power * 3.4f, 2.0f, 6.2f) + glm::vec3(0.0f, 0.85f, 0.0f), 0.72f);
                    }
                }
            }
            if (rd.Active()) {
                const auto ground = [&](const glm::vec3& p) { return GroundAt(p, e.position.y); };
                const auto sweep = [&](const glm::vec3& from, const glm::vec3& to, glm::vec3& point, glm::vec3& normal) {
                    const glm::vec3 dv = to - from;
                    const float l = glm::length(dv);
                    RayHit hit;
                    if (l < 1e-4f || !game_.Collision().Raycast(from, dv / l, l, hit)) return false;
                    point = hit.point;
                    normal = hit.normal;
                    return true;
                };
                rd.Step(dt, ground, sweep);
                const float impact = rd.TakeImpact();
                if (impact > 1.2f && !e.fall_heard) {
                    e.fall_heard = true;
                    const glm::vec3 at = rd.PelvisPosition();
                    Play(body_fall_, &at, std::min(4.0f, -4.0f + impact * 1.5f));
                }
                if (impact > 0.0f || rd.Asleep()) e.landed = true;
                std::vector<glm::mat4> bw(lisa_skeleton_->Size(), glm::mat4(1.0f));
                rd.ApplyToBones(*lisa_skeleton_, m, bw);
                anim::ComputeSkin(*lisa_skeleton_, bw, skins_[e.slot]);
                e.head = rd.HeadPosition();
                return;
            }
        }
        switch (e.state) {
        case Enemy::State::Rising: PoseLisa(e, kStop, 0.4f, dt); break;
        case Enemy::State::Chasing: PoseLisa(e, hud_.dead ? kStop : kWalk, hud_.dead ? 1.0f : e.kind == SurvivalEnemyKind::Lisa ? 0.9f : -1.0f, dt * freeze); break;
        case Enemy::State::Striking: PoseLisa(e, kDash, 1.0f, dt); break;
        case Enemy::State::Stunned: PoseLisa(e, kStop, 0.3f, dt); break;
        case Enemy::State::Dying: PoseLisa(e, kStop, 0.5f, dt * (e.fall > 1.4f ? 0.0f : 1.0f)); break;
        }
        if (lisa_skeleton_) {
            const int h = lisa_skeleton_->FindName("SKL_004_HEAD");
            if (h >= 0 && static_cast<size_t>(h) < skins_[e.slot].size()) {
                const glm::mat4 to_world = glm::translate(glm::mat4(1.0f), e.position) * glm::rotate(glm::mat4(1.0f), e.yaw, glm::vec3(0, 1, 0));
                e.head = glm::vec3(to_world * skins_[e.slot][static_cast<size_t>(h)] *
                                   glm::vec4(lisa_skeleton_->bind_world[static_cast<size_t>(h)] + glm::vec3(0.0f, 0.1f, 0.02f), 1.0f));
            }
        }
        return;
    }
    SurvivalPoser& poser = *poser_;
    const float scale = KindScale(e.kind);
    const glm::mat4 to_model = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f / scale)) * glm::rotate(glm::mat4(1.0f), -e.yaw, glm::vec3(0, 1, 0)) *
                               glm::translate(glm::mat4(1.0f), -e.position);
    const glm::vec3 chest = glm::vec3(to_model * glm::vec4(feet + glm::vec3(0.0f, 1.3f, 0.0f), 1.0f));
    if (e.kind == SurvivalEnemyKind::Crawler) {
        float crouch = 0.0f, leap = 0.0f, curl = 0.0f, claw = 0.0f;
        if (e.state == Enemy::State::Striking) {
            const float fly_at = spec.windup, hold_at = fly_at + e.fly, drop_at = hold_at + (e.latched ? kPounceHold : 0.0f);
            if (e.timer < fly_at) {
                crouch = Smooth(e.timer / (0.6f * fly_at)) * (0.92f + 0.08f * std::sin(time_ * 63.0f));
            } else if (e.timer < drop_at) {
                leap = Smooth((e.timer - fly_at) / 0.06f);
                claw = e.timer >= hold_at ? 1.0f : 0.0f;
            } else {
                leap = 1.0f - Smooth((e.timer - drop_at) / kPounceDrop);
            }
        } else if (e.state == Enemy::State::Stunned) {
            crouch = 0.6f;
        } else if (e.state == Enemy::State::Dying) {
            curl = Smooth(e.timer / 0.5f);
        } else if (e.state == Enemy::State::Rising) {
            crouch = 1.0f - Smooth(e.timer / spec.rise);
        }
        poser.Crawler(e.gait, crouch, leap, curl, e.twitch, time_, claw);
    } else {
        const float size = (e.kind == SurvivalEnemyKind::Stretched ? SurvivalPoser::kStretchedLegs : 1.0f) * scale;
        const float natural = doll_walk_travel_ * size / (doll_walk_frames_ / kMotionFps);
        float rate = 0.0f;
        if (moving && !hud_.dead) rate = natural > 0.05f ? e.speed * e.pace * (e.slow > 0.0f ? 0.35f : 1.0f) / natural : 1.0f;
        if (e.state == Enemy::State::Striking) rate = 0.35f;
        if (e.state == Enemy::State::Rising || e.state == Enemy::State::Stunned) rate = 0.1f;
        if (e.state == Enemy::State::Dying) rate = 0.0f;
        e.anim_frame += dt * kMotionFps * rate * freeze;
        const double half = doll_walk_frames_ * 0.5;
        if (rate > 0.05f && std::floor(e.anim_frame / half) != std::floor(e.step_frame / half)) {
            const float d = glm::distance(e.position, feet);
            if (e.kind == SurvivalEnemyKind::Giant) {
                if (d < 45.0f) Stomp(e.position);
                if (d < 18.0f) shake_ = std::min(1.0f, shake_ + 0.32f * (1.0f - d / 18.0f));
            } else if (d < 24.0f) {
                Play(walker_step_, &e.position, 0.0f, 1.0f / std::sqrt(KindScale(e.kind)));
            }
        }
        e.step_frame = e.anim_frame;
        if (e.kind == SurvivalEnemyKind::Giant) {
            float raise = 0.0f, slam = 0.0f;
            if (e.state == Enemy::State::Striking) {
                const float after = 1.0f - Smooth((e.timer - spec.windup - 0.5f) / 0.6f);
                raise = Smooth(e.timer / spec.windup) * after;
                slam = Smooth((e.timer - spec.windup) / 0.22f) * after;
            }
            poser.Doll(e.anim_frame, 0.0f, chest + glm::vec3(0.0f, -0.45f, 0.0f), e.twitch, raise, slam);
        } else if (e.kind == SurvivalEnemyKind::Doll) {
            float reach = 0.0f;
            if (e.state == Enemy::State::Striking) reach = Smooth(e.timer / spec.windup) * (1.0f - Smooth((e.timer - spec.windup - 0.15f) / 0.3f));
            glm::vec3 at = chest;
            if (e.state == Enemy::State::Chasing) {
                const float d = glm::distance(glm::vec2(e.position.x, e.position.z), glm::vec2(feet.x, feet.z));
                reach = 0.55f * (1.0f - Smooth((d - 1.5f) / 2.5f));
                if (e.scream >= 0.0f) {
                    reach = Smooth(e.scream / 0.3f) * (1.0f - Smooth((e.scream - 1.1f) / 0.4f));
                    at = glm::vec3(0.0f, 2.7f, 0.35f);
                }
            }
            const float quirk = e.kind == SurvivalEnemyKind::Doll ? std::sin(static_cast<float>(e.id) * 2.17f) : 0.0f;
            poser.Doll(e.anim_frame, reach, at, e.twitch, 0.0f, 0.0f, quirk);
        } else {
            float raise = 0.0f, slam = 0.0f;
            if (e.state == Enemy::State::Striking) {
                const float after = 1.0f - Smooth((e.timer - spec.windup - 0.45f) / 0.5f);
                raise = Smooth(e.timer / spec.windup) * after;
                slam = Smooth((e.timer - spec.windup) / 0.18f) * after;
            }
            const glm::vec3 low = chest + glm::vec3(0.0f, -0.5f, 0.0f);
            poser.Stretched(e.anim_frame, raise, slam, low, e.twitch);
        }
        if (e.state == Enemy::State::Dying) {
            if (!physics_) poser.Slacken(e.timer / 0.6f);
        } else {
            poser.Flinch(e.bend, e.twist);
        }
    }
    ApplyPhysical(e, poser, dt);
    poser.Skin(skins_[e.slot]);
    static const bool pose_log = std::getenv("PT_SURVIVAL_POSE_LOG") != nullptr;
    if (pose_log && game_.Frame() % 30 == 0) {
        LogInfo("survival pose: {} state {} at ({:.2f} {:.2f} {:.2f}) sink {:.2f} waist y {:.3f} head y {:.3f}", spec.name, static_cast<int>(e.state),
                e.position.x, e.position.y, e.position.z, e.sink, poser.Joint(poser.waist()).y, poser.Joint(poser.head()).y);
    }
    const glm::mat4 to_world = glm::translate(glm::mat4(1.0f), e.position) * glm::rotate(glm::mat4(1.0f), e.yaw, glm::vec3(0, 1, 0)) *
                               glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    const glm::vec3 joint = poser.Joint(poser.head());
    const int h = poser.head();
    glm::vec3 up(0.0f, 0.1f, 0.02f);
    if (h >= 0 && static_cast<size_t>(h) < skins_[e.slot].size()) up = glm::vec3(skins_[e.slot][static_cast<size_t>(h)] * glm::vec4(up, 0.0f));
    e.head = glm::vec3(to_world * glm::vec4(joint + up, 1.0f));
}

void SurvivalNight::ApplyPhysical(Enemy& e, SurvivalPoser& poser, float dt) {
    if (!physics_ || !doll_skeleton_ || UsesLisa(e.kind)) return;
    const glm::mat4 m = EnemyTransform(e, false);
    const auto ground = [&](const glm::vec3& p) { return GroundAt(p, e.position.y); };
    const auto sweep = [&](const glm::vec3& from, const glm::vec3& to, glm::vec3& point, glm::vec3& normal) {
        const glm::vec3 d = to - from;
        const float l = glm::length(d);
        RayHit hit;
        if (l < 1e-4f || !game_.Collision().Raycast(from, d / l, l, hit)) return false;
        point = hit.point;
        normal = hit.normal;
        return true;
    };
    if (e.state == Enemy::State::Dying) {
        anim::Ragdoll& rd = enemy_ragdolls_[e.id];
        const float scale = KindScale(e.kind);
        if (!rd.Active()) {
            std::vector<glm::mat4> bw;
            poser.BoneWorld(bw);
            if (rd.Activate(*doll_skeleton_, bw, m, scale, e.knock)) {
                e.ragdoll = true;
                e.crumple = false;
                glm::vec3 d = e.has_last_hit ? e.last_hit_dir : glm::vec3(e.fall_dir.x, 0.0f, e.fall_dir.y);
                d = glm::length(d) > 1e-3f ? glm::normalize(d) : glm::vec3(-std::sin(e.yaw), 0.0f, -std::cos(e.yaw));
                rd.SetCollapse(d, e.last_hit_head);
                const float mass_inv = 1.0f / std::max(std::sqrt(Kind(e.kind).mass), 0.7f);
                if (e.last_hit_head) {
                    rd.AddPartImpulse(anim::Ragdoll::kHead, d * (5.2f * mass_inv) + glm::vec3(0.0f, 1.0f * mass_inv, 0.0f));
                    rd.AddPartImpulse(anim::Ragdoll::kChest, d * (1.5f * mass_inv));
                } else {
                    const glm::vec3 at = e.has_last_hit ? e.last_hit_at : rd.ChestPosition();
                    const float speed = std::clamp(e.last_hit_power * 3.8f * mass_inv, 2.0f, 6.8f);
                    rd.AddImpulse(at, d * speed + glm::vec3(0.0f, 0.85f * mass_inv, 0.0f), 0.78f * scale);
                }
            }
        }
        if (rd.Active()) {
            rd.Step(dt, ground, sweep);
            const float impact = rd.TakeImpact();
            if (impact > 1.2f && !e.fall_heard) {
                e.fall_heard = true;
                const glm::vec3 at = rd.PelvisPosition();
                if (e.kind == SurvivalEnemyKind::Giant) {
                    Stomp(at, 4.0f);
                    const float d = glm::distance(at, game_.GetPlayer().Feet());
                    shake_ = std::min(1.0f, shake_ + std::max(0.0f, 0.8f * (1.0f - d / 20.0f)));
                } else {
                    Play(body_fall_, &at, std::min(4.0f, -4.0f + impact * 1.5f));
                    Play(walker_step_, &at, 2.0f, 0.8f);
                }
            }
            if (impact > 0.0f || rd.Asleep()) e.landed = true;
            poser.ApplyRagdoll(rd, m);
            return;
        }
    }
    anim::PhysicalBody& body = bodies_[e.id];
    if (!body.Valid() && !body.Build(*doll_skeleton_, KindScale(e.kind))) return;
    joint_world_.resize(doll_skeleton_->Size());
    for (size_t b = 0; b < joint_world_.size(); ++b) joint_world_[b] = glm::vec3(m * glm::vec4(poser.Joint(static_cast<int>(b)), 1.0f));
    const glm::mat3 frame(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
    body.Follow(joint_world_, glm::normalize(glm::quat_cast(frame) * poser.Turn(poser.head())));
    body.SetDrive(1.0f);
    body.Step(dt, ground, sweep);
    if (body.Deviation() > 0.004f) poser.ApplyBody(body, glm::inverse(m), 1.0f);
}

void SurvivalNight::StrikeBody(const Enemy& e, const glm::vec3& at, const glm::vec3& velocity, float radius) {
    if (const auto it = bodies_.find(e.id); it != bodies_.end()) it->second.Strike(at, velocity, radius * KindScale(e.kind));
}

void SurvivalNight::UpdateEnemies(float dt) {
    BuildWays();
    UpdateWays();
    const Player& player = game_.GetPlayer();
    const glm::vec3 target = player.Feet();
    const bool dead = hud_.dead;
    const Camera view = game_.ViewCamera(true);
    auto watched = [&](const Enemy& e) {
        const glm::vec3 chest = e.position + glm::vec3(0.0f, 1.2f, 0.0f);
        const glm::vec3 to = chest - view.position;
        const float distance = glm::length(to);
        if (distance < 1e-3f || glm::dot(to / distance, view.Forward()) < std::cos(0.62f)) return false;
        RayHit wall;
        return !game_.Collision().Raycast(view.position, to / distance, distance - 0.3f, wall);
    };
    for (Enemy& e : enemies_) {
        const KindSpec& spec = Kind(e.kind);
        e.timer += dt;
        e.hurt = std::max(0.0f, e.hurt - dt * 4.0f);
        e.strike_cooldown = std::max(0.0f, e.strike_cooldown - dt);
        e.slow = std::max(0.0f, e.slow - dt);
        e.stagger = std::max(0.0f, e.stagger - dt);
        e.shoved = std::max(0.0f, e.shoved - dt);
        e.frenzy = std::max(0.0f, e.frenzy - dt);
        e.pace = (e.frenzy > 0.0f ? 1.45f : 1.0f) * (1.0f + 0.25f * LanternShare()) * (e.boss ? 1.0f + 0.15f * static_cast<float>(e.phase) : 1.0f);
        if ((e.affix & kAffixShy) && e.state != Enemy::State::Dying) {
            e.pace *= InBeam(e) || InFlareLight(e.position) ? 0.08f : StreetLightLevel() < 0.5f ? 1.8f : 1.25f;
        }
        if (const glm::vec3 shove = e.knock * dt; e.state != Enemy::State::Dying && !frozen_ && glm::length(shove) > 1e-5f) {
            if (Blocked(e.position, glm::normalize(shove), glm::length(shove) + 0.3f)) {
                e.knock = glm::vec3(0.0f);
            } else {
                e.position += shove;
            }
        }
        e.knock *= std::exp(-dt * 7.0f);
        e.sound_wait = std::max(0.0f, e.sound_wait - dt);
        e.run_sound_wait = std::max(0.0f, e.run_sound_wait - dt);
        e.flinch *= std::exp(-dt * 9.0f);
        Spring(e.bend, e.bend_speed, 95.0f, 11.0f, dt);
        if (const float l = glm::length(e.bend); l > 0.95f) e.bend *= 0.95f / l;
        {
            glm::vec2 twist(e.twist, 0.0f), speed(e.twist_speed, 0.0f);
            Spring(twist, speed, 80.0f, 10.0f, dt);
            e.twist = std::clamp(twist.x, -0.8f, 0.8f);
            e.twist_speed = speed.x;
        }
        for (Enemy::Wound& w : e.wounds) {
            w.age += dt;
            w.drip -= dt;
            if (w.drip > 0.0f || w.age > 4.0f || e.state == Enemy::State::Dying || w.bone < 0 || static_cast<size_t>(w.bone) >= skins_[e.slot].size()) continue;
            w.drip = 0.25f + 0.45f * Rand() + 0.2f * w.age;
            const glm::vec3 p(EnemyTransform(e) * skins_[e.slot][static_cast<size_t>(w.bone)] * glm::vec4(w.point, 1.0f));
            Debris d;
            d.kind = Debris::Kind::Blood;
            d.id = next_id_++;
            d.position = p;
            d.velocity = glm::vec3(Rand() - 0.5f, -0.2f, Rand() - 0.5f) * 0.4f;
            d.ground = GroundAt(p, e.position.y);
            d.life = 1.5f;
            d.size = 0.5f + 0.4f * Rand();
            debris_.push_back(d);
        }
        glm::vec3 to = target - e.position;
        to.y = 0.0f;
        const float distance = glm::length(to);
        auto face = [&](float turn_rate) {
            if (distance < 1e-3f) return;
            const float want = std::atan2(to.x, to.z);
            const float d = WrapAngle(want - e.yaw);
            e.yaw = WrapAngle(e.yaw + std::clamp(d, -turn_rate * dt, turn_rate * dt));
        };
        switch (e.state) {
        case Enemy::State::Rising: {
            face(3.0f);
            e.sink = 1.0f - Smooth(e.timer / spec.rise);
            if (e.timer >= spec.rise) {
                e.state = Enemy::State::Chasing;
                e.timer = 0.0f;
                e.sink = 0.0f;
            }
            break;
        }
        case Enemy::State::Chasing: {
            if (dead && distance < 1.6f + 0.4f * static_cast<float>(e.id % 3)) break;
            if (e.stagger > 0.0f) break;
            if (e.perched) {
                face(6.0f);
                e.perch_wait += dt;
                const bool tired = to_spawn_ <= 0 && e.perch_wait > 20.0f;
                if (!dead && (distance < 9.0f || tired) && e.strike_cooldown <= 0.0f) {
                    e.perched = false;
                    e.state = Enemy::State::Striking;
                    e.timer = spec.windup * 0.4f;
                    e.struck = false;
                    e.leap_from = e.position;
                    e.fly = 0.5f;
                    e.span = 9.5f;
                }
                break;
            }
            if (e.drop >= 0.0f) {
                e.drop = std::min(1.0f, e.drop + dt / 0.7f);
                e.position = glm::mix(e.leap_from, e.leap_to, e.drop) + glm::vec3(0.0f, 0.5f * std::sin(kPi * e.drop), 0.0f);
                if (e.drop >= 1.0f) {
                    e.drop = -1.0f;
                    e.up_time = 0.0f;
                }
                break;
            }
            e.up_time = e.position.y - target.y > 1.6f ? e.up_time + dt : 0.0f;
            if (e.up_time > 1.5f) {
                glm::vec3 down(0.0f);
                e.up_time = 0.0f;
                if (WayDown(e.position, down)) {
                    e.leap_from = e.position;
                    e.leap_to = down;
                    e.drop = 0.0f;
                }
            }
            if ((e.affix & kAffixScreamer) && !e.screamed && !dead && distance < 11.0f && e.scream < 0.0f) {
                e.scream = 0.0f;
                e.bend_speed += glm::vec2(0.0f, -7.0f);
                e.twitch_to = glm::vec2(0.0f);
            }
            if (e.scream >= 0.0f) {
                const float before = e.scream;
                e.scream += dt;
                face(3.0f);
                if (before < 0.45f && e.scream >= 0.45f) Scream(e);
                if (e.scream >= 1.5f) {
                    e.scream = -1.0f;
                    e.screamed = true;
                }
                break;
            }
            if (e.kind == SurvivalEnemyKind::Doll && !dead) {
                e.pause = std::max(0.0f, e.pause - dt);
                e.pause_wait -= dt;
                if (e.pause_wait <= 0.0f) {
                    e.pause_wait = 3.0f + 4.0f * Rand();
                    if (distance > 7.0f && Rand() < 0.35f) {
                        e.pause = 0.7f + 1.0f * Rand();
                        e.twitch_wait = 0.0f;
                    }
                }
                if (e.pause > 0.0f) {
                    face(1.5f);
                    if (e.twitch_wait > 0.25f) e.twitch_wait = 0.25f;
                    break;
                }
            }
            const float turn_rate = e.kind == SurvivalEnemyKind::Lisa ? 9.0f : e.kind == SurvivalEnemyKind::Crawler ? 7.0f : 4.0f;
            e.way_wait -= dt;
            if (e.way_wait <= 0.0f) {
                e.way_wait = 0.25f;
                e.way_round = distance > 1.5f && std::abs(target.y - e.position.y) < 1.2f && Blocked(e.position, to / distance, std::min(distance, 10.0f));
            }
            glm::vec3 way(0.0f);
            const bool on_way = e.way_round && WayOn(e.position, way);
            if (on_way) {
                e.yaw = WrapAngle(e.yaw + std::clamp(WrapAngle(std::atan2(way.x, way.z) - e.yaw), -turn_rate * dt, turn_rate * dt));
            } else {
                face(turn_rate);
            }
            float speed = e.speed * e.pace * (e.slow > 0.0f ? 0.35f : 1.0f) * (dead ? 0.4f : 1.0f);
            if (e.kind == SurvivalEnemyKind::Lisa && !dead) {
                const bool seen = watched(e);
                const float seen_scale = e.boss && e.phase >= 2 ? 0.72f : (distance < 6.0f ? 0.65f : 0.48f);
                const float unseen_scale = e.boss && e.phase >= 2 ? 2.0f : 1.75f;
                speed *= seen ? seen_scale : unseen_scale;
                if (distance < 10.0f && e.sound_wait <= 0.0f) {
                    Play(lisa_breath_, &e.position);
                    e.sound_wait = 4.5f;
                }
                if (!seen && distance < 32.0f && e.run_sound_wait <= 0.0f) {
                    Play(lisa_run_, &e.position);
                    e.run_sound_wait = 1.15f + 0.2f * SoundRand();
                }
            }
            glm::vec3 forward = on_way ? way : glm::vec3(std::sin(e.yaw), 0.0f, std::cos(e.yaw));
            if (e.kind == SurvivalEnemyKind::Crawler && distance > 3.0f && !on_way) {
                const float weave = std::sin(time_ * 2.3f + e.weave) * std::min(1.0f, (distance - 3.0f) / 6.0f) * 0.8f;
                forward = glm::normalize(forward + glm::vec3(forward.z, 0.0f, -forward.x) * weave);
            }
            if (e.kind == SurvivalEnemyKind::Doll && distance < 5.0f) speed *= 1.25f;
            float step = speed * dt;
            e.push_through = std::max(0.0f, e.push_through - dt);
            e.hemmed = std::max(0.0f, e.hemmed - dt * 0.5f);
            if (e.push_through <= 0.0f && Blocked(e.position, forward, 0.55f + step)) {
                bool free = false;
                for (const float turn : {0.6f, -0.6f, 1.2f, -1.2f}) {
                    const glm::vec3 d(std::sin(e.yaw + turn), 0.0f, std::cos(e.yaw + turn));
                    if (!Blocked(e.position, d, 0.55f + step)) {
                        forward = d;
                        free = true;
                        break;
                    }
                }
                if (!free) {
                    step = 0.0f;
                    e.hemmed += dt * 1.5f;
                    if (e.hemmed > 1.5f) {
                        e.hemmed = 0.0f;
                        e.push_through = 1.2f;
                    }
                }
            }
            if (frozen_) {
                e.gait += step / (SurvivalPoser::kCrawlStride / SurvivalPoser::kCrawlDuty);
                break;
            }
            e.position += forward * step;
            e.gait += step / (SurvivalPoser::kCrawlStride / SurvivalPoser::kCrawlDuty);
            if (e.kind == SurvivalEnemyKind::Crawler && e.sound_wait <= 0.0f && step > 0.0f) {
                if (distance < 24.0f) Play(crawl_, &e.position);
                e.sound_wait = 0.24f + 0.16f * SoundRand();
            }
            const float strike_from = e.kind == SurvivalEnemyKind::Crawler ? 2.8f : spec.reach;
            if (!dead && distance < strike_from && e.strike_cooldown <= 0.0f) {
                e.state = Enemy::State::Striking;
                e.timer = 0.0f;
                e.struck = false;
                e.leap_from = e.position;
                const glm::vec3 dir = distance > 1e-3f ? to / distance : glm::vec3(std::sin(e.yaw), 0.0f, std::cos(e.yaw));
                e.leap_to = e.position + dir * std::max(0.0f, distance - 1.2f);
                e.fly = kPounceFly;
                e.span = 3.4f;
            }
            break;
        }
        case Enemy::State::Striking: {
            if (e.kind == SurvivalEnemyKind::Crawler) {
                const Camera own = player.MakeCamera();
                glm::vec3 away = e.leap_from - target;
                away.y = 0.0f;
                away = glm::length(away) > 1e-3f ? glm::normalize(away) : -glm::vec3(std::sin(e.yaw), 0.0f, std::cos(e.yaw));
                const glm::vec3 cling = own.position + away * 0.62f - glm::vec3(0.0f, 0.9f, 0.0f);
                const float fly_at = spec.windup, hold_at = fly_at + e.fly;
                e.pitch = 0.0f;
                if (e.timer < fly_at) {
                    face(14.0f);
                    e.twitch_wait = 0.0f;
                    if (e.timer - dt < fly_at - 0.12f && e.timer >= fly_at - 0.12f) Play(pounce_, &e.position);
                } else if (e.timer < hold_at) {
                    const float u = (e.timer - fly_at) / e.fly;
                    glm::vec3 end = cling;
                    glm::vec3 span = end - e.leap_from;
                    span.y = 0.0f;
                    if (glm::length(span) > e.span) {
                        end = e.leap_from + glm::normalize(span) * e.span;
                        end.y = GroundAt(end, e.leap_from.y);
                    }
                    e.leap_to = end;
                    e.position = glm::mix(e.leap_from, end, u) + glm::vec3(0.0f, 0.3f * std::sin(kPi * u), 0.0f);
                    const glm::vec3 v = end - e.leap_from;
                    e.pitch = std::atan2(v.y + 0.3f * kPi * std::cos(kPi * u), std::max(0.3f, glm::length(glm::vec2(v.x, v.z))));
                    face(20.0f);
                } else {
                    if (!e.struck) {
                        e.struck = true;
                        glm::vec3 off = e.position - target;
                        off.y = 0.0f;
                        e.latched = !dead && glm::length(off) < 1.2f && Reaches(e, target);
                        if (e.latched) {
                            HurtPlayer(e.damage, e.position, static_cast<int>(e.kind));
                            shake_ = std::min(1.0f, shake_ + 0.6f);
                            Play(gore_, &own.position, 0.0f, 1.2f);
                        } else {
                            Play(crawl_, &e.position, 4.0f, 0.9f);
                        }
                    }
                    const float drop_at = hold_at + (e.latched ? kPounceHold : 0.0f);
                    if (e.timer < drop_at) {
                        const float shake = std::sin(e.timer * 47.0f) * 0.018f;
                        e.position = cling + glm::vec3(shake, std::cos(e.timer * 39.0f) * 0.012f, 0.0f);
                        e.pitch = 1.05f;
                        face(30.0f);
                    } else {
                        if (e.timer - dt < drop_at) e.leap_to = e.position;
                        const float k = Smooth((e.timer - drop_at) / kPounceDrop);
                        const float ground = GroundAt(e.position, e.leap_from.y);
                        e.position.y = glm::mix(e.leap_to.y, ground, k);
                        e.pitch = (e.latched ? 1.05f : 0.0f) * (1.0f - k);
                        if (distance > 1e-3f && e.timer < drop_at + kPounceDrop + 0.35f) e.position -= to / distance * (2.0f * dt);
                    }
                    if (e.timer >= drop_at + kPounceDrop + spec.recover) {
                        e.state = Enemy::State::Chasing;
                        e.timer = 0.0f;
                        e.strike_cooldown = 0.8f;
                        e.latched = false;
                        e.pitch = 0.0f;
                    }
                }
                break;
            }
            const bool heavy = e.kind == SurvivalEnemyKind::Stretched || e.kind == SurvivalEnemyKind::Giant;
            face(heavy ? 2.0f : 6.0f);
            if (e.timer < spec.windup && distance > 0.5f) e.position += glm::normalize(to) * dt * (heavy ? 0.4f : 1.2f);
            const float lands = spec.windup + (e.kind == SurvivalEnemyKind::Stretched ? 0.16f : e.kind == SurvivalEnemyKind::Giant ? 0.2f : 0.0f);
            if (!e.struck && e.timer >= lands) {
                e.struck = true;
                if (!dead && distance < spec.reach + (e.kind == SurvivalEnemyKind::Giant ? 0.7f : 0.45f) && Reaches(e, target)) {
                    if (e.kind == SurvivalEnemyKind::Lisa) {
                        Catch(e);
                    } else {
                        HurtPlayer(e.damage, e.position, static_cast<int>(e.kind));
                    }
                }
                if (e.kind == SurvivalEnemyKind::Giant) {
                    Stomp(e.position, 3.0f);
                    shake_ = std::min(1.0f, shake_ + std::max(0.0f, 0.7f * (1.0f - distance / 12.0f)));
                }
            }
            if (e.timer >= lands + spec.recover * 0.5f) {
                e.state = Enemy::State::Chasing;
                e.timer = 0.0f;
                e.strike_cooldown = spec.recover;
            }
            break;
        }
        case Enemy::State::Stunned: {
            if (e.timer <= 0.0f) {
                e.state = Enemy::State::Chasing;
                e.timer = 0.0f;
                e.sink = 0.0f;
            }
            e.timer -= 2.0f * dt;
            break;
        }
        case Enemy::State::Dying: {
            if (e.kind != SurvivalEnemyKind::Crawler && !e.ragdoll && !e.landed && !(e.crumple && e.timer < 0.32f)) {
                e.fall_speed += (5.5f + 9.0f * std::sin(std::max(0.0f, e.fall))) * dt / std::sqrt(KindScale(e.kind));
                e.fall += e.fall_speed * dt;
                if (e.fall >= kPi * 0.5f) {
                    e.fall = kPi * 0.5f;
                    if (e.fall_speed > 2.0f) {
                        if (e.kind == SurvivalEnemyKind::Giant) {
                            Stomp(e.position, 4.0f);
                        } else {
                            Play(body_fall_, &e.position);
                            Play(walker_step_, &e.position, 2.0f, 0.8f);
                        }
                        if (e.kind == SurvivalEnemyKind::Giant) {
                            const float d = glm::distance(e.position, target);
                            shake_ = std::min(1.0f, shake_ + std::max(0.0f, 0.8f * (1.0f - d / 20.0f)));
                        }
                    }
                    e.fall_speed = -e.fall_speed * 0.18f;
                    if (std::abs(e.fall_speed) < 0.4f) {
                        e.landed = true;
                        e.fall_speed = 0.0f;
                    }
                }
            }
            if ((e.landed || e.kind == SurvivalEnemyKind::Crawler) && !e.blood_left && e.timer > 0.4f) {
                e.blood_left = true;
                Mark pool;
                pool.kind = Mark::Kind::Blood;
                pool.id = next_id_++;
                const glm::vec2 lie = e.kind == SurvivalEnemyKind::Crawler ? glm::vec2(0.0f) : e.fall_dir * (0.75f * KindScale(e.kind));
                pool.position = e.position + glm::vec3(lie.x, 0.0f, lie.y);
                if (const auto rd = enemy_ragdolls_.find(e.id); e.ragdoll && rd != enemy_ragdolls_.end() && rd->second.Active()) {
                    pool.position = glm::mix(rd->second.PelvisPosition(), rd->second.ChestPosition(), 0.6f);
                } else if (const auto it = bodies_.find(e.id); e.ragdoll && it != bodies_.end()) {
                    glm::vec3 chest(0.0f);
                    pool.position = doll_skeleton_ && it->second.Joint(doll_skeleton_->FindName("SKL_002_CHEST"), chest) ? glm::mix(it->second.Waist(), chest, 0.6f)
                                                                                                                      : it->second.Waist();
                }
                pool.position.y = GroundAt(pool.position, e.position.y) + 0.006f;
                pool.spin = Rand() * 6.28f;
                pool.size = e.kind == SurvivalEnemyKind::Giant ? 2.2f : e.kind == SurvivalEnemyKind::Stretched ? 1.4f : 1.0f;
                pool.grow = 0.0f;
                pool.variant = static_cast<int>(Rand() * 3.0f) % 3;
                marks_.push_back(pool);
            }
            if (e.timer > 9.0f) e.sink = Smooth((e.timer - 9.0f) / 1.6f);
            break;
        }
        }
        if (e.state != Enemy::State::Dying) {
            for (const Enemy& other : enemies_) {
                if (&other == &e || other.state == Enemy::State::Dying) continue;
                glm::vec3 d = e.position - other.position;
                d.y = 0.0f;
                const float l = glm::length(d);
                const float room = 0.4f * (KindScale(e.kind) + KindScale(other.kind)) - 0.05f;
                if (l > 1e-4f && l < room) e.position += d / l * std::min(room - l, 2.0f * dt);
            }
            if (!dead && !(e.kind == SurvivalEnemyKind::Crawler && e.state == Enemy::State::Striking)) {
                const float gap = e.kind == SurvivalEnemyKind::Crawler ? 1.0f : e.kind == SurvivalEnemyKind::Giant ? 1.5f : 0.7f;
                glm::vec3 off = e.position - target;
                off.y = 0.0f;
                const float l = glm::length(off);
                if (l > 1e-4f && l < gap) e.position += off / l * std::min(gap - l, 4.0f * dt);
            }
            e.position.x = std::clamp(e.position.x, kArenaMin.x, kArenaMax.x);
            e.position.z = std::clamp(e.position.z, kArenaMin.y, north_end_);
            if (!(e.kind == SurvivalEnemyKind::Crawler && e.state == Enemy::State::Striking) && e.drop < 0.0f) e.position.y = GroundAt(e.position, e.position.y);
        }
        PoseEnemy(e, dt);
    }
    for (const glm::vec3& at : splits_) {
        if (Enemy* c = SpawnEnemyAt(SurvivalEnemyKind::Crawler, at + glm::vec3(0.0f, 0.0f, 0.0f), 0.8f, 0, false)) {
            c->max_health = c->health = c->max_health * 0.6f;
            Play(crawl_, &at, 4.0f, 1.1f);
        }
    }
    splits_.clear();
    const glm::vec3 feet = game_.GetPlayer().Feet();
    for (Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dying || UsesLisa(e.kind) || e.latched) continue;
        if (e.kind == SurvivalEnemyKind::Crawler && e.state == Enemy::State::Striking) continue;
        glm::vec3 away(e.position.x - feet.x, 0.0f, e.position.z - feet.z);
        const float d = glm::length(away);
        const float keep = e.kind == SurvivalEnemyKind::Giant ? 0.95f : 0.62f;
        if (d < 1e-3f) continue;
        away /= d;
        float out = keep - d;
        const glm::vec3 eye = game_.GetPlayer().Eye();
        const glm::vec2 head(e.head.x - eye.x, e.head.z - eye.z);
        if (std::abs(e.head.y - eye.y) < 0.8f) out = std::max(out, kFaceRoom - glm::length(head));
        if (out > 0.0f && !Blocked(e.position, away, out)) e.position += away * std::min(out, 3.0f * dt);
    }
    for (auto it = enemies_.begin(); it != enemies_.end();) {
        if (it->state == Enemy::State::Dying && it->timer > 10.6f) {
            if (UsesLisa(it->kind)) game_.Demos().Gimmicks().ReleaseActor(it->id);
            slot_used_[it->slot] = false;
            slot_free_at_[it->slot] = time_ + 1.0f;
            bodies_.erase(it->id);
            enemy_ragdolls_.erase(it->id);
            it = enemies_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<SurvivalNight::HitBone> SurvivalNight::HitBonesOf(const anim::Skeleton& skeleton) {
    constexpr struct {
        const char* from;
        const char* to;
        float radius;
    } kParts[] = {
        {"SKL_000_WAIST", "SKL_001_SPINE", 0.165f}, {"SKL_001_SPINE", "SKL_002_CHEST", 0.165f}, {"SKL_002_CHEST", "SKL_003_NECK", 0.165f},
        {"SKL_003_NECK", "SKL_004_HEAD", 0.075f},   {"SKL_011_LUARM", "SKL_021_RUARM", 0.09f},  {"SKL_030_LTHIGH", "SKL_040_RTHIGH", 0.12f},
        {"SKL_011_LUARM", "SKL_012_LFARM", 0.07f},  {"SKL_012_LFARM", "SKL_013_LHAND", 0.06f},  {"SKL_021_RUARM", "SKL_022_RFARM", 0.07f},
        {"SKL_022_RFARM", "SKL_023_RHAND", 0.06f},  {"SKL_030_LTHIGH", "SKL_031_LLEG", 0.1f},   {"SKL_031_LLEG", "SKL_032_LFOOT", 0.075f},
        {"SKL_032_LFOOT", "SKL_033_LTOE", 0.06f},   {"SKL_040_RTHIGH", "SKL_041_RLEG", 0.1f},   {"SKL_041_RLEG", "SKL_042_RFOOT", 0.075f},
        {"SKL_042_RFOOT", "SKL_043_RTOE", 0.06f},
    };
    std::vector<HitBone> bones;
    for (const auto& part : kParts) {
        const int from = skeleton.FindName(part.from), to = skeleton.FindName(part.to);
        if (from >= 0 && to >= 0) bones.push_back({from, to, part.radius});
    }
    return bones;
}

namespace {

float RayCapsule(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& a, const glm::vec3& b, float radius) {
    const glm::vec3 ba = b - a, oa = origin - a;
    const float baba = glm::dot(ba, ba), bard = glm::dot(ba, dir), baoa = glm::dot(ba, oa), rdoa = glm::dot(dir, oa), oaoa = glm::dot(oa, oa);
    const float along = baba > 1e-9f ? std::clamp(baoa / baba, 0.0f, 1.0f) : 0.0f;
    if (glm::length(oa - ba * along) <= radius) return 0.0f;
    const float qa = baba - bard * bard, qb = baba * rdoa - baoa * bard, qc = baba * oaoa - baoa * baoa - radius * radius * baba;
    const float h = qb * qb - qa * qc;
    if (qa > 1e-9f && h >= 0.0f) {
        const float t = (-qb - std::sqrt(h)) / qa;
        const float y = baoa + t * bard;
        if (y > 0.0f && y < baba) return t;
    }
    float best = -1.0f;
    for (const glm::vec3& end : {a, b}) {
        const glm::vec3 oc = origin - end;
        const float cb = glm::dot(dir, oc), cc = glm::dot(oc, oc) - radius * radius, disc = cb * cb - cc;
        if (disc < 0.0f) continue;
        const float t = -cb - std::sqrt(disc);
        if (t > 0.0f && (best < 0.0f || t < best)) best = t;
    }
    return best;
}

}

bool SurvivalNight::HitEnemy(const glm::vec3& origin, const glm::vec3& dir, float max_distance, const Enemy* skip, Enemy*& hit, float& t,
                             bool& head) const {
    hit = nullptr;
    t = max_distance;
    for (const Enemy& e : enemies_) {
        if (&e == skip || e.state == Enemy::State::Dying || e.sink > 0.6f) continue;
        const KindSpec& spec = Kind(e.kind);
        {
            const float size = KindScale(e.kind);
            const glm::vec3 middle = e.position + glm::vec3(0.0f, 1.0f * size, 0.0f);
            const float along = std::max(0.0f, glm::dot(middle - origin, dir));
            if (glm::length(origin + dir * along - middle) > 2.4f * size) continue;
        }
        {
            const glm::vec3 oc = origin - e.head;
            const float b = glm::dot(oc, dir);
            const float c = glm::dot(oc, oc) - e.head_radius * e.head_radius;
            const float disc = b * b - c;
            if (disc >= 0.0f) {
                const float s = -b - std::sqrt(disc);
                if (s + 2.0f * std::sqrt(disc) > 0.0f && std::max(s, 0.01f) < t) {
                    t = std::max(s, 0.01f);
                    hit = const_cast<Enemy*>(&e);
                    head = true;
                    continue;
                }
            }
        }
        const std::vector<HitBone>& bones = UsesLisa(e.kind) ? lisa_hit_ : doll_hit_;
        const anim::Skeleton* skeleton = UsesLisa(e.kind) ? lisa_skeleton_ : doll_skeleton_;
        const std::vector<glm::mat4>& skin = skins_[e.slot];
        if (skeleton && !bones.empty() && !skin.empty()) {
            const glm::mat4 world = EnemyTransform(e);
            const float size = KindScale(e.kind);
            auto joint = [&](int bone) { return glm::vec3(world * skin[static_cast<size_t>(bone)] * glm::vec4(skeleton->bind_world[static_cast<size_t>(bone)], 1.0f)); };
            for (const HitBone& bone : bones) {
                if (static_cast<size_t>(std::max(bone.from, bone.to)) >= skin.size()) continue;
                const float at = RayCapsule(origin, dir, joint(bone.from), joint(bone.to), bone.radius * size);
                if (at < 0.0f || std::max(at, 0.01f) >= t) continue;
                t = std::max(at, 0.01f);
                hit = const_cast<Enemy*>(&e);
                head = false;
            }
            continue;
        }
        const float c = std::cos(-e.yaw), s = std::sin(-e.yaw);
        auto local = [&](const glm::vec3& v) { return glm::vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z); };
        const float drop = e.sink * 1.9f * KindScale(e.kind);
        const glm::vec3 o = local(origin - e.position - glm::vec3(0.0f, -drop, 0.0f)) - spec.box_center;
        const glm::vec3 d = local(dir);
        float lo = 0.0f, hi = t;
        bool miss = false;
        for (int axis = 0; axis < 3 && !miss; ++axis) {
            if (std::abs(d[axis]) < 1e-7f) {
                miss = std::abs(o[axis]) > spec.box_half[axis];
                continue;
            }
            float a = (-spec.box_half[axis] - o[axis]) / d[axis];
            float b = (spec.box_half[axis] - o[axis]) / d[axis];
            if (a > b) std::swap(a, b);
            lo = std::max(lo, a);
            hi = std::min(hi, b);
            miss = lo > hi;
        }
        if (miss || std::max(lo, 0.01f) >= t) continue;
        t = std::max(lo, 0.01f);
        hit = const_cast<Enemy*>(&e);
        head = false;
    }
    return hit != nullptr;
}

void SurvivalNight::Damage(Enemy& e, float amount, const glm::vec3& from, const glm::vec3& at, const glm::vec3& dir, bool head, bool round) {
    if (e.state == Enemy::State::Dying) return;
    const KindSpec& spec = Kind(e.kind);
    if ((e.affix & kAffixArmored) && !head) {
        amount *= 0.25f;
        for (int i = 0; i < 4; ++i) {
            Debris d;
            d.kind = Debris::Kind::Spark;
            d.id = next_id_++;
            d.position = at - dir * 0.02f;
            const glm::vec3 r(Rand() - 0.5f, Rand() - 0.5f, Rand() - 0.5f);
            d.velocity = glm::normalize(-dir + r * 1.8f) * (2.5f + 3.5f * Rand());
            d.ground = e.position.y;
            d.life = 0.1f + 0.12f * Rand();
            debris_.push_back(d);
        }
        Play(armor_, &at, 0.0f, 0.92f + 0.16f * SoundRand());
    }
    e.health -= amount;
    e.hurt = 1.0f;
    hud_.hit_head = head;
    const SurvivalWeapon by = round ? weapon_ : SurvivalWeapon::Handgun;
    const WeaponSpec& weapon = Spec(by);
    glm::vec3 push = e.position - from;
    push.y = 0.0f;
    if (glm::length(push) > 1e-3f) {
        push = glm::normalize(push);
        e.knock += push * std::min(3.6f, 10.0f * weapon.push * amount / 40.0f / spec.mass);
        const float c = std::cos(-e.yaw), s = std::sin(-e.yaw);
        const glm::vec2 local(c * push.x + s * push.z, -s * push.x + c * push.z);
        const glm::vec3 off = at - e.position;
        const float across = c * off.x + s * off.z;
        if (WalksLikeDoll(e.kind) || e.kind == SurvivalEnemyKind::Crawler) {
            const float share = e.kind == SurvivalEnemyKind::Crawler ? 0.85f : 1.0f;
            e.bend_speed += local * share * std::min(22.0f, (head ? 0.88f : 0.54f) * amount / spec.mass);
            if (const float l = glm::length(e.bend_speed); l > 24.0f) e.bend_speed *= 24.0f / l;
            e.twist_speed += share * std::clamp(across / 0.15f, -1.0f, 1.0f) * std::min(14.0f, 0.48f * amount / spec.mass) + (Rand() - 0.5f) * 3.0f;
            e.flinch += local * std::min(0.28f, 0.007f * amount / spec.mass);
        } else {
            e.flinch += local * std::min(0.65f, (head ? 0.032f : 0.020f) * amount / spec.mass);
        }
    }
    {
        glm::vec3 along = dir;
        along.y = std::max(along.y, -0.2f);
        const float per = by == SurvivalWeapon::Shotgun || by == SurvivalWeapon::Double ? 0.14f : 0.36f;
        const float speed = std::clamp(per * amount / spec.mass, 1.0f, head ? 11.0f : 10.0f);
        StrikeBody(e, at, glm::normalize(along + glm::vec3(0.0f, 0.15f, 0.0f)) * speed, head ? 0.4f : 0.7f);
    }
    if (e.state == Enemy::State::Chasing) {
        e.stagger = std::max(e.stagger, std::min(0.5f, (0.12f + 0.008f * amount) / std::sqrt(spec.mass)) *
                                             (1.0f + 0.3f * static_cast<float>(boons_[SurvivalBoon::StoppingPower])));
    }
    if (amount / spec.mass > 45.0f && e.state == Enemy::State::Chasing) e.stagger = std::max(e.stagger, 0.45f);
    hit_pause_ = std::max(hit_pause_, head ? 0.06f : 0.035f);
    e.last_hit_at = at;
    e.last_hit_dir = dir;
    e.last_hit_head = head;
    e.last_hit_power = std::clamp(weapon.push * (by == SurvivalWeapon::Shotgun || by == SurvivalWeapon::Double ? 1.55f : 1.0f) *
                                      (0.65f + 0.35f * std::min(amount / 40.0f, 2.0f)),
                                  0.6f, 2.2f);
    e.has_last_hit = true;
    Bleed(e, at, dir, head, std::clamp(amount / 35.0f, 0.3f, 1.8f));
    if (e.health <= 0.0f) {
        if (head) ++headshots_;
        Kill(e, dir);
    }
}

void SurvivalNight::Kill(Enemy& e, const glm::vec3& dir) {
    if ((e.affix & kAffixSplitter) && e.kind != SurvivalEnemyKind::Crawler) {
        splits_.push_back(e.position);
        Spray(e.position + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 14, 3.0f);
        Play(gore_, &e.position, 4.0f, 0.8f);
    }
    e.state = Enemy::State::Dying;
    e.timer = 0.0f;
    e.fall = 0.0f;
    e.fall_speed = 0.8f;
    glm::vec2 d(dir.x, dir.z);
    e.fall_dir = glm::length(d) > 1e-3f ? glm::normalize(d) : glm::vec2(-std::sin(e.yaw), -std::cos(e.yaw));
    e.crumple = WalksLikeDoll(e.kind) && (e.hurt >= 1.0f && hud_.hit_head ? true : Rand() < 0.45f);
    if (const auto it = bodies_.find(e.id); physics_ && it != bodies_.end() && it->second.Valid()) {
        e.ragdoll = true;
        e.crumple = false;
        glm::vec3 along(dir.x, 0.0f, dir.z);
        along = glm::length(along) > 1e-3f ? glm::normalize(along) : glm::vec3(-std::sin(e.yaw), 0.0f, -std::cos(e.yaw));
        const float size = KindScale(e.kind);
        it->second.Strike(it->second.Waist() + glm::vec3(0.0f, 0.45f * size, 0.0f), along * 1.6f, 0.6f * size);
    }
    if (e.crumple) {
        e.fall_dir = glm::vec2(std::sin(e.yaw), std::cos(e.yaw));
        e.fall_speed = 0.2f;
    }
    ++kills_;
    Score(e, hud_.hit_head);
    Frighten(-0.02f);
    if (const int r = boons_[SurvivalBoon::Bloodletting]) health_ = std::min(max_health_, health_ + 2.0f * static_cast<float>(r));
    Play(gore_, &e.position);
    const glm::vec3 at = e.position;
    const DifficultySpec& diff = kDifficulty[static_cast<size_t>(difficulty_)];
    if (e.boss) {
        Drop(at + glm::vec3(0.0f, 0.0f, -0.5f), SurvivalPickupKind::Ammo, AmmoNeeded());
        Drop(at + glm::vec3(0.5f, 0.0f, -0.5f), SurvivalPickupKind::Health, SurvivalWeapon::Handgun);
        if (flares_ < MaxFlares()) Drop(at + glm::vec3(-0.5f, 0.0f, -0.5f), SurvivalPickupKind::Flare, SurvivalWeapon::Handgun);
        const int bonus = 2000 + 40 * wave_;
        score_ += bonus;
        Frighten(-0.5f);
        ++bosses_down_;
        Banner("pc_survival_boss_down", 0, 3.0f);
        LogInfo("survival: the boss down (+{})", bonus);
    }
    if (e.kind == SurvivalEnemyKind::Lisa) {
        Drop(at + glm::vec3(0.4f, 0.0f, 0.0f), SurvivalPickupKind::Health, SurvivalWeapon::Handgun);
        Drop(at - glm::vec3(0.4f, 0.0f, 0.0f), SurvivalPickupKind::Ammo, AmmoNeeded());
        Drop(at + glm::vec3(0.0f, 0.0f, 0.4f), SurvivalPickupKind::Ammo, AmmoNeeded());
    } else {
        const float r = Rand();
        if (e.kind == SurvivalEnemyKind::Giant) {
            Drop(at + glm::vec3(0.5f, 0.0f, 0.0f), SurvivalPickupKind::Ammo, AmmoNeeded());
            Drop(at - glm::vec3(0.5f, 0.0f, 0.0f), SurvivalPickupKind::Ammo, AmmoNeeded());
            if (Rand() < 0.6f * diff.drops) Drop(at + glm::vec3(0.0f, 0.0f, 0.5f), SurvivalPickupKind::Health, SurvivalWeapon::Handgun);
        }
        const float scavenger = 1.0f + 0.25f * static_cast<float>(boons_[SurvivalBoon::Scavenger]);
        const float supplied = supply_on_ ? 0.75f : 1.0f;
        const float ammo = (e.kind == SurvivalEnemyKind::Stretched || e.kind == SurvivalEnemyKind::Mother ? 0.75f : 0.3f) * diff.rounds *
                           (e.kind == SurvivalEnemyKind::Giant ? 0.0f : 1.0f) * scavenger * supplied;
        const float health = (e.kind == SurvivalEnemyKind::Stretched ? 0.25f : 0.08f) * diff.drops * scavenger * supplied;
        const bool drought = (events_ & kEventDrought) != 0;
        const bool pipe_out = pipe_hits_ > 0 || std::any_of(pickups_.begin(), pickups_.end(), [](const Pickup& p) { return p.kind == SurvivalPickupKind::Pipe; });
        if (!drought && wave_ > 6 && !pipe_out && Rand() < 0.025f * scavenger) Drop(at, SurvivalPickupKind::Pipe, SurvivalWeapon::Handgun);
        if (!drought && wave_ >= 3 && flares_ < MaxFlares() && Rand() < 0.03f * scavenger) Drop(at, SurvivalPickupKind::Flare, SurvivalWeapon::Handgun);
        bool dropped_ammo = false;
        if (drought) {
        } else if (r < ammo) {
            Drop(at, SurvivalPickupKind::Ammo, AmmoNeeded());
            dropped_ammo = true;
        } else if (r < ammo + health) {
            Drop(at, SurvivalPickupKind::Health, SurvivalWeapon::Handgun);
        }
        if (!drought && !dropped_ammo && wave_ <= 2 && kills_ % 3 == 0) {
            int total_ammo = 0;
            for (size_t wi = 1; wi < weapons_.size(); ++wi) {
                if (weapons_[wi].owned) total_ammo += weapons_[wi].clip + weapons_[wi].reserve;
            }
            if (total_ammo < 15) {
                Drop(at, SurvivalPickupKind::Ammo, AmmoNeeded());
            }
        }
    }
    LogInfo("survival: {} down ({} kills)", Kind(e.kind).name, kills_);
}

void SurvivalNight::Omen(const char* key, float seconds) {
    hud_.omen_key = key;
    hud_.omen = seconds;
    Vision vision = VisionOf(key);
    if ((vision == Vision::Fuse && plan_.mission != SurvivalMission::Fuse) || (vision == Vision::Lantern && plan_.mission != SurvivalMission::Lamps)) {
        vision = Vision::None;
    }
    Notice(key, "", vision != Vision::None ? 6.0f : 5.0f, vision);
    Voice(key);
    LogInfo("survival: warning {}", key);
}

void SurvivalNight::HurtPlayer(float amount, const glm::vec3& from, int cause) {
    if (hud_.dead || unhurt_ || caught_) return;
    last_hit_from_ = from;
    has_last_hit_from_ = true;
    task_.hold = std::max(0.0f, task_.hold - 1.0f);
    Frighten(0.3f * amount / std::max(1.0f, max_health_));
    health_ -= amount;
    wave_hurt_ += amount;
    hurt_recent_ = std::min(1.0f, hurt_recent_ + 0.35f + amount / 100.0f);
    hud_.hurt = 1.0f;
    hud_.hurt_arc = 1.0f;
    const Camera view = game_.ViewCamera(true);
    const glm::vec3 to = from - view.position;
    const float bearing = std::atan2(to.x, -to.z);
    const float facing = std::atan2(view.Forward().x, -view.Forward().z);
    hud_.hurt_angle = WrapAngle(bearing - facing);
    shake_ = std::min(1.0f, shake_ + 0.5f + amount / 80.0f);
    view_kick_speed_ += glm::vec2(-1.2f, -std::sin(hud_.hurt_angle) * 1.6f + (Rand() - 0.5f) * 0.6f) * (0.4f + amount / 60.0f);
    speed_scale_ = std::min(speed_scale_, 0.55f);
    Play(hurt_, nullptr);
    LogInfo("survival: player hit for {:.0f}, {:.0f} left", amount, std::max(0.0f, health_));
    if (health_ <= 0.0f && boons_[SurvivalBoon::LastBreath] > 0 && !last_breath_used_) {
        health_ = 1.0f;
        last_breath_used_ = true;
        hud_.hurt = 1.0f;
        shake_ = 1.0f;
        Note("pc_survival_last_breath");
        LogInfo("survival: the last breath");
    }
    if (health_ <= 0.0f) Die(cause);
}

void SurvivalNight::Catch(Enemy& lisa) {
    if (hud_.dead || caught_ || unhurt_) return;
    Player& player = game_.GetPlayer();
    const glm::vec3 feet = SafeFlatSpot(player.Feet(), 0.85f, 0.07f);
    glm::vec3 to = lisa.position - feet;
    to.y = 0.0f;
    if (!catch_ready_ || glm::length(to) < 1e-3f) {
        HurtPlayer(health_ + 1.0f, lisa.position, catch_cause_);
        return;
    }
    const float fox_yaw = std::atan2(to.x, to.z);
    player.Warp(feet, fox_yaw);
    game_.Demos().SetDemoTransform(kCatchDemo, glm::angleAxis(fox_yaw, glm::vec3(0.0f, 1.0f, 0.0f)), feet);
    if (!game_.Demos().Play(kCatchDemo, true)) {
        HurtPlayer(health_ + 1.0f, lisa.position, catch_cause_);
        return;
    }
    caught_ = true;
    const bool easy_survive = difficulty_ == SurvivalDifficulty::Easy && catch_cause_ != kSurvivalCauseDark && health_ > max_health_ * 0.40f;
    caught_damage_ = easy_survive ? max_health_ * 0.65f : 0.0f;
    caught_lethal_ = !easy_survive;
    caught_cause_ = catch_cause_;
    caught_by_ = lisa.id;
    caught_at_ = feet;
    caught_yaw_ = fox_yaw;
    hud_.caught = true;
    sprinting_ = false;
    sights_ = false;
    reload_timer_ = 0.0f;
    if (low_heartbeat_ && game_.Audio()) game_.Audio()->StopPlayingId(low_heartbeat_, 0.2f);
    low_heartbeat_ = 0;
    LogInfo("survival: Lisa caught the player on wave {} ({}, {:.2f} m off, {})", wave_, Kind(lisa.kind).name, glm::length(to), caught_lethal_ ? "lethal" : "survivable");
}

void SurvivalNight::Score(const Enemy& e, bool head) {
    static constexpr int kWorth[] = {100, 120, 300, 250, 2500, 800};
    chain_ = chain_timer_ > 0.0f ? chain_ + 1 : 1;
    chain_timer_ = 3.0f;
    best_chain_ = std::max(best_chain_, chain_);
    const float times = 1.0f + 0.2f * static_cast<float>(std::min(chain_ - 1, 20));
    const int gain = static_cast<int>(std::lround(static_cast<float>(kWorth[static_cast<size_t>(e.kind)]) * (head ? 1.5f : 1.0f) * times));
    score_ += gain;
}

void SurvivalNight::UpdateDirector(float dt) {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    int close_by = 0;
    for (const Enemy& e : enemies_) {
        if (e.state != Enemy::State::Dying && glm::distance(glm::vec2(e.position.x, e.position.z), glm::vec2(feet.x, feet.z)) < 7.0f) ++close_by;
    }
    hurt_recent_ = std::max(0.0f, hurt_recent_ - dt * 0.12f);
    const float low = std::clamp((0.5f - health_ / max_health_) * 2.0f, 0.0f, 1.0f);
    const float want = std::clamp(0.18f * static_cast<float>(close_by) + hurt_recent_ + 0.4f * low, 0.0f, 1.0f);
    intensity_ += (want - intensity_) * (1.0f - std::exp(-dt));
    relief_ = std::max(0.0f, relief_ - dt);
    if (intensity_ > 0.72f) {
        hot_time_ += dt;
        if (hot_time_ > 3.0f && relief_ <= 0.0f) {
            relief_ = 6.0f;
            hot_time_ = 0.0f;
            surge_ = 0;
            LogInfo("survival: director, a breath (intensity {:.2f})", intensity_);
        }
    } else {
        hot_time_ = 0.0f;
    }
    if (intensity_ < 0.15f && AliveEnemies() < std::max(2, wave_cap_ / 2) && to_spawn_ > 2) {
        calm_time_ += dt;
        if (calm_time_ > 6.0f && surge_ == 0) {
            surge_ = 3;
            calm_time_ = 0.0f;
            spawn_timer_ = std::min(spawn_timer_, 1.2f);
            if (!surge_noise_) surge_noise_ = PostEvent("Play_radio_noise_lp", nullptr);
            surge_noise_left_ = 1.6f;
            LogInfo("survival: director, a surge");
        }
    } else {
        calm_time_ = 0.0f;
    }
    if (surge_noise_ && (surge_noise_left_ -= dt) <= 0.0f) {
        if (game_.Audio()) game_.Audio()->StopPlayingId(surge_noise_, 0.6f);
        surge_noise_ = 0;
    }
}

void SurvivalNight::Die(int cause, bool caught) {
    health_ = 0.0f;
    hud_.dead = true;
    hud_.dead_time = 0.0f;
    hud_.can_retry = false;
    const SurvivalProfile& profile = game_.SurvivalProfileData();
    const int best_score = start_ == SurvivalStart::Daily ? (profile.daily_date == SurvivalToday() ? profile.daily_score : 0)
                                                          : profile.best_score[static_cast<size_t>(difficulty_)];
    hud_.cause = cause;
    hud_.caught_end = caught;
    hud_.end_kills = kills_;
    hud_.end_headshots = headshots_;
    hud_.end_accuracy = shots_ > 0 ? static_cast<int>(std::lround(100.0f * static_cast<float>(hits_) / static_cast<float>(shots_))) : 0;
    hud_.end_score = score_;
    hud_.end_chain = best_chain_;
    hud_.end_seconds = run_time_;
    hud_.end_new_best = score_ > 0 && score_ > best_score;
    shake_ = std::min(shake_, 0.3f);
    phase_ = SurvivalPhase::Dead;
    sprinting_ = false;
    sights_ = false;
    reload_timer_ = 0.0f;
    dead_fall_ = 0.0f;
    dead_fall_speed_ = 0.0f;
    const Camera view = game_.ViewCamera(true);
    dead_eye_ = view.position;
    Player& player = game_.GetPlayer();
    dead_ground_ = GroundAt(player.Feet(), player.Feet().y);
    lean_ = 0.0f;
    player.sprint_lean = 0.0f;
    player_ragdoll_.Clear();
    player_fall_heard_ = false;
    dropped_gun_.active = false;
    if (!caught) {
        if (const anim::Skeleton* skel = PlayerBody::BodySkeleton()) {
            const glm::mat4 pose = player.PoseTransform();
            std::vector<glm::mat4> bw = player.WorldBones();
            if (bw.size() < skel->Size()) {
                bw.resize(skel->Size(), glm::mat4(1.0f));
                for (size_t b = 0; b < skel->Size(); ++b) bw[b] = glm::translate(glm::mat4(1.0f), skel->bind_world[b]);
            }
            const glm::vec3 feet = player.Feet();
            glm::vec3 away = has_last_hit_from_ ? (feet - last_hit_from_) : glm::vec3(-std::sin(player.BodyFoxYaw()), 0.0f, -std::cos(player.BodyFoxYaw()));
            away.y = 0.0f;
            if (glm::length(away) < 1e-3f) away = glm::vec3(-std::sin(player.BodyFoxYaw()), 0.0f, -std::cos(player.BodyFoxYaw()));
            else away = glm::normalize(away);
            if (player_ragdoll_.Activate(*skel, bw, pose, 1.0f, away * 0.6f)) {
                player_ragdoll_.SetCollapse(away, false);
                const glm::vec3 side(-away.z, 0.0f, away.x);
                player_ragdoll_.AddPartImpulse(anim::Ragdoll::kChest, away * 2.1f + glm::vec3(0.0f, 0.35f, 0.0f), side * 1.4f + glm::vec3(0.0f, 0.9f, 0.0f));
                player_ragdoll_.AddPartImpulse(anim::Ragdoll::kPelvis, away * 1.1f);
                player.SetRagdoll(&player_ragdoll_);
            }
        }
        if (weapon_ != SurvivalWeapon::Flashlight) {
            const glm::mat4 gun_w = WeaponTransform(view);
            dropped_gun_.active = true;
            dropped_gun_.weapon = weapon_;
            dropped_gun_.pos = glm::vec3(gun_w[3]);
            glm::mat3 r(gun_w);
            for (int c = 0; c < 3; ++c) {
                const float l = glm::length(r[c]);
                if (l > 1e-6f) r[c] /= l;
            }
            dropped_gun_.rot = glm::normalize(glm::quat_cast(r));
            const glm::vec3 right_v(-std::cos(player.BodyFoxYaw()), 0.0f, std::sin(player.BodyFoxYaw()));
            const glm::vec3 fwd_v(std::sin(player.BodyFoxYaw()), 0.0f, std::cos(player.BodyFoxYaw()));
            dropped_gun_.vel = right_v * (0.85f + 0.4f * Rand()) + fwd_v * (0.65f + 0.4f * Rand()) + glm::vec3(0.0f, 0.7f, 0.0f);
            dropped_gun_.spin = glm::vec3(3.5f * (Rand() - 0.5f), 4.0f * (Rand() - 0.5f), 5.0f);
            dropped_gun_.ground = dead_ground_;
            dropped_gun_.bounces = 0;
        }
    }
    player.locks.Add('B', "SurvivalDead", 3u);
    dead_locked_ = true;
    Play(fall_, nullptr);
    heartbeat_ = low_heartbeat_ ? std::exchange(low_heartbeat_, 0u) : PostEvent(kHeartbeatEvent, nullptr);
    hud_.banner = 0.0f;
    LogInfo("survival: the player fell on wave {} ({} kills, best {}, cause {}{})", wave_, kills_, best_wave_, cause, caught ? ", out of the catch" : "");
    KeepRecord();
}

glm::vec3 SurvivalNight::SafeFlatSpot(const glm::vec3& desired, float radius, float max_delta) const {
    const float z_hi = std::max(32.0f, north_end_ - 8.0f);
    auto clamp_road = [&](glm::vec3 p) {
        p.x = std::clamp(p.x, -4.0f, 1.8f);
        p.z = std::clamp(p.z, 30.0f, z_hi);
        return p;
    };
    auto check_spot = [&](glm::vec3& p) -> bool {
        p = clamp_road(p);
        const float center_y = GroundAt(p, p.y);
        float min_y = center_y, max_y = center_y;
        const float xs[3] = {-radius, 0.0f, radius};
        const float zs[3] = {-radius * 1.15f, 0.0f, radius * 0.90f};
        for (float dx : xs) {
            for (float dz : zs) {
                RayHit hit;
                const glm::vec3 probe(p.x + dx, center_y + 1.35f, p.z + dz);
                if (!game_.Collision().Raycast(probe, glm::vec3(0.0f, -1.0f, 0.0f), 3.2f, hit) || hit.normal.y < 0.72f) {
                    return false;
                }
                min_y = std::min(min_y, hit.point.y);
                max_y = std::max(max_y, hit.point.y);
                if (max_y - min_y > max_delta) return false;
            }
        }
        const float check_dist = radius + 0.45f;
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * (kPi * 0.25f);
            const glm::vec3 dir(std::cos(a), 0.0f, std::sin(a));
            for (const float h : {0.16f, 0.65f, 1.35f}) {
                RayHit hit;
                if (game_.Collision().Raycast(glm::vec3(p.x, max_y + h, p.z), dir, check_dist, hit)) {
                    if (std::abs(hit.normal.y) < 0.72f || hit.point.y > max_y + 0.08f) return false;
                }
            }
        }
        p.y = max_y + 0.015f;
        return true;
    };

    glm::vec3 cand = desired;
    if (check_spot(cand)) return cand;

    for (const float ring : {0.85f, 1.65f, 2.50f}) {
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * (kPi * 0.25f);
            cand = desired + glm::vec3(std::cos(a) * ring, 0.0f, std::sin(a) * ring);
            if (check_spot(cand)) return cand;
        }
    }

    const float lane_z = std::clamp(desired.z, 36.0f, std::min(140.0f, z_hi));
    for (const float dz : {0.0f, -2.5f, 2.5f, -5.0f, 5.0f}) {
        cand = glm::vec3(-1.15f, desired.y, std::clamp(lane_z + dz, 32.0f, z_hi));
        if (check_spot(cand)) return cand;
    }

    glm::vec3 fallback(-1.15f, desired.y, 36.0f);
    fallback.y = GroundAt(fallback, fallback.y) + 0.015f;
    return fallback;
}

void SurvivalNight::PlaceUprightOnStreet() {
    Player& player = game_.GetPlayer();
    const glm::vec3 spot = SafeFlatSpot(player.Feet(), 0.55f, 0.10f);
    player.Warp(spot, 0.0f);
    player.pitch = player.target_pitch = 0.0f;
    player.handy_light.enable = true;
    game_.Effects().SetFadeColor(0, 0, 0, 255);
    game_.Effects().CallFadeOut(0.01f);
    game_.Effects().CallFadeIn(0.45f);
}

void SurvivalNight::Retry() {
    LogInfo("survival: retry (difficulty {})", static_cast<int>(difficulty_));
    if (start_ == SurvivalStart::Continue) start_ = SurvivalStart::Fresh;
    ResetRun();
    kept_wave_ = 0;
    PlaceUprightOnStreet();
    ApplyLooks(true);
    Banner("pc_survival_title", 0, 3.0f);
}

void SurvivalNight::StartWake(const std::string& banner_key, int banner_value) {
    Player& player = game_.GetPlayer();
    const glm::vec3 spot = SafeFlatSpot(player.Feet(), 0.95f, 0.055f);
    wake_at_ = spot;
    wake_yaw_ = 0.0f;
    player.Warp(wake_at_, wake_yaw_);
    player.handy_light.enable = false;
    wake_light_on_ = false;
    game_.Effects().SetFadeColor(0, 0, 0, 255);
    game_.Effects().CallFadeOut(0.01f);
    game_.Demos().SetDemoTransform(kWakeDemo, glm::angleAxis(wake_yaw_, glm::vec3(0.0f, 1.0f, 0.0f)), wake_at_);
    if (!game_.Demos().Play(kWakeDemo, true)) {
        player.handy_light.enable = true;
        game_.Effects().CallFadeIn(0.6f);
        Banner(banner_key, banner_value, 3.0f);
        return;
    }
    waking_ = true;
    wake_third_ = false;
    game_.Demos().SetDemoThirdPersonBody(kWakeDemo, false);
    hud_.caught = true;
    hud_.banner = 0.0f;
    wake_banner_key_ = banner_key;
    wake_banner_value_ = banner_value;
    swap_ = 1.0f;
    if (!wake_locked_) {
        player.locks.Add('B', "SurvivalWake", 3u);
        wake_locked_ = true;
    }
    LogInfo("survival: waking on the street via {} at ({:.2f} {:.2f} {:.2f}), third person {}", kWakeDemo, wake_at_.x, wake_at_.y, wake_at_.z,
            wake_third_);
}

int SurvivalNight::Night() const {
    return SurvivalNightOf(phase_ == SurvivalPhase::Wave || phase_ == SurvivalPhase::Dead ? std::max(wave_, 1) : wave_ + 1);
}

float SurvivalNight::NorthEndOf(int night) {
    return night < 3 ? kArenaMax.y : std::max(kArenaMin.y + 47.0f, kArenaMax.y - 35.0f * static_cast<float>(night - 2));
}

void SurvivalNight::UpdateLaterNights(float dt) {
    const int night = Night();
    const float door = night >= 4 ? 1.0f : 0.0f;
    night_door_ = night_door_ < door ? std::min(door, night_door_ + 0.25f * dt) : door;
    const float end = NorthEndOf(night);
    north_end_ = north_end_ > end ? std::max(end, north_end_ - 1.2f * dt) : end;
    Player& player = game_.GetPlayer();
    if (north_end_ < kArenaMax.y && player.controller.position.z > north_end_ - 1.0f) player.controller.position.z = north_end_ - 1.0f;
    player.controller.position.x = std::clamp(player.controller.position.x, kArenaMin.x + 0.3f, kArenaMax.x - 0.3f);
    if (night >= 3) {
        const float bank = std::clamp(1.0f - (north_end_ - player.Feet().z) / 20.0f, 0.0f, 1.0f);
        fog_level_ = std::max(fog_level_, std::min(1.0f, 0.4f + 0.12f * static_cast<float>(night - 3) + 0.6f * bank));
    }
}

void SurvivalNight::Midnight() {
    phase_ = SurvivalPhase::Break;
    StartCountdown(26.0f);
    Toll(12);
    midnight_ = 0.0f;
    wave_hurt_ = 0.0f;
    const int next = SurvivalNightOf(wave_ + 1);
    Banner("pc_survival_midnight", 0, 6.0f);
    static constexpr const char* kTolls[] = {"pc_survival_toll_lamps", "pc_survival_toll_fog", "pc_survival_toll_door", "pc_survival_toll_lisa"};
    if (next >= 2 && next - 2 < static_cast<int>(std::size(kTolls))) Omen(kTolls[next - 2], 6.0f);
    LogInfo("survival: midnight, night {} begins ({} kills)", next, kills_);
    if (wave_ == kSurvivalWaves) Unlock(kUnlockGold | kUnlockRed);
    EndTask();
    Frighten(0.3f);
    KeepRecord();
    SaveCheckpoint();
    hud_.kept = 0.0f;
    Drop(game_.GetPlayer().Feet() + glm::vec3(1.5f, 0.0f, -1.0f), SurvivalPickupKind::Ammo, AmmoNeeded());
}

void SurvivalNight::UpdateMidnight(float dt) {
    if (midnight_ < 0.0f) return;
    if (phase_ != SurvivalPhase::Break) {
        HushMidnight();
        return;
    }
    const float before = midnight_;
    midnight_ += dt;
    constexpr float kRadioAt = 19.5f;
    const int told = SurvivalNightOf(wave_ + 1) - 2;
    if (before < kRadioAt && midnight_ >= kRadioAt && told >= 0 && told < static_cast<int>(std::size(kMidnightVoices))) {
        const glm::vec3 feet = game_.GetPlayer().Feet();
        const glm::vec3 radio(kHouseDoorX + 0.6f, GroundAt(glm::vec3(kHouseDoorX, feet.y + 2.0f, kHouseFront + 0.6f), feet.y) + 1.0f, kHouseFront - 1.2f);
        midnight_noise_ = PostEvent("Play_radio_noise_lp", &radio);
        midnight_voice_ = PostEvent(kMidnightVoices[told], &radio);
        LogInfo("survival: the radio in the house at midnight ({}{})", kMidnightVoices[told], midnight_voice_ ? "" : ", not heard");
    }
    const bool speaking = midnight_voice_ && game_.Audio() && game_.Audio()->IsPlaying(midnight_voice_);
    if (speaking) countdown_ = std::max(countdown_, 5.5f);
    if (midnight_voice_ && !speaking && midnight_ > kRadioAt + 0.5f) HushMidnight();
}

void SurvivalNight::HushMidnight() {
    for (uint32_t* id : {&midnight_voice_, &midnight_noise_}) {
        if (*id && game_.Audio()) game_.Audio()->StopPlayingId(*id, 1.5f);
        *id = 0;
    }
    midnight_ = -1.0f;
}

SurvivalWeapon SurvivalNight::AmmoNeeded() {
    if (Night() >= 6 && weapons_[static_cast<size_t>(SurvivalWeapon::Lever)].owned) return SurvivalWeapon::Lever;
    SurvivalWeapon best = SurvivalWeapon::Handgun;
    float lowest = 1e9f;
    for (size_t i = 1; i < weapons_.size(); ++i) {
        if (!weapons_[i].owned) continue;
        const WeaponSpec spec = Gun(static_cast<SurvivalWeapon>(i));
        float share = static_cast<float>(weapons_[i].reserve + weapons_[i].clip) / static_cast<float>(spec.max_reserve) + 0.25f * Rand();
        if (static_cast<SurvivalWeapon>(i) == SurvivalWeapon::Handgun) {
            for (size_t j = 2; j < weapons_.size(); ++j) {
                if (weapons_[j].owned) {
                    share += 0.4f;
                    break;
                }
            }
        }
        if (share < lowest) {
            lowest = share;
            best = static_cast<SurvivalWeapon>(i);
        }
    }
    return best;
}

void SurvivalNight::Drop(const glm::vec3& at, SurvivalPickupKind kind, SurvivalWeapon weapon) {
    if (kind == SurvivalPickupKind::Ammo) {
        int ammo_on_road = 0;
        for (const Pickup& existing : pickups_) {
            if (existing.kind == SurvivalPickupKind::Ammo) ++ammo_on_road;
        }
        if (ammo_on_road >= 8) return;
    }
    Pickup p;
    p.id = next_id_++;
    p.kind = kind;
    p.weapon = weapon;
    const float a = Rand() * 6.28f;
    const glm::vec3 side(std::cos(a), 0.0f, std::sin(a));
    p.position = at + side * 0.35f + glm::vec3(0.0f, 0.6f, 0.0f);
    p.ground = GroundAt(p.position, at.y);
    p.velocity = side * 1.1f + glm::vec3(0.0f, 1.8f, 0.0f);
    p.life = kind == SurvivalPickupKind::Weapon ? 1e9f : kind == SurvivalPickupKind::Pipe ? 90.0f : kDifficulty[static_cast<size_t>(difficulty_)].life;
    pickups_.push_back(p);
}

void SurvivalNight::UpdatePickups(float dt) {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    for (auto it = pickups_.begin(); it != pickups_.end();) {
        it->age += dt;
        if (!it->settled) {
            it->velocity.y -= 9.8f * dt;
            it->position += it->velocity * dt;
            if (it->position.y <= it->ground) {
                it->position.y = it->ground;
                if (it->velocity.y < -2.0f) {
                    it->velocity *= glm::vec3(0.4f, -0.3f, 0.4f);
                } else {
                    it->settled = true;
                    it->velocity = glm::vec3(0.0f);
                    Play(landing_, &it->position);
                }
            }
        }
        bool taken = false;
        if (!hud_.dead && it->age > 0.5f && glm::distance(glm::vec2(feet.x, feet.z), glm::vec2(it->position.x, it->position.z)) < 1.15f &&
            std::abs(feet.y - it->position.y) < 1.5f) {
            switch (it->kind) {
            case SurvivalPickupKind::Ammo: {
                WeaponState& s = weapons_[static_cast<size_t>(it->weapon)];
                const WeaponSpec spec = Gun(it->weapon);
                if (s.owned && s.reserve < spec.max_reserve) {
                    const int add = std::min(spec.pickup_ammo, spec.max_reserve - s.reserve);
                    s.reserve += add;
                    static constexpr const char* kRounds[] = {"", "pc_survival_pickup_rounds_handgun", "pc_survival_pickup_rounds_shotgun",
                                                              "pc_survival_pickup_rounds_magnum", "pc_survival_pickup_rounds_lever",
                                                              "pc_survival_pickup_rounds_double"};
                    Note(kRounds[static_cast<size_t>(it->weapon)], add);
                    taken = true;
                }
                break;
            }
            case SurvivalPickupKind::Health:
                if (health_ < max_health_) {
                    health_ = std::min(max_health_, health_ + 40.0f);
                    hud_.heal = 1.0f;
                    SpawnHealEffect(feet);
                    Note("pc_survival_pickup_health");
                    taken = true;
                }
                break;
            case SurvivalPickupKind::Weapon: {
                WeaponState& s = weapons_[static_cast<size_t>(it->weapon)];
                const WeaponSpec spec = Gun(it->weapon);
                if (!s.owned) {
                    s.owned = true;
                    s.clip = spec.clip_size;
                    s.reserve = spec.start_reserve;
                    SelectWeapon(it->weapon, true);
                } else if (s.reserve < spec.max_reserve) {
                    s.reserve = std::min(spec.max_reserve, s.reserve + spec.start_reserve);
                } else {
                    break;
                }
                static constexpr const char* kGuns[] = {"", "", "pc_survival_pickup_shotgun", "pc_survival_pickup_magnum", "pc_survival_pickup_lever",
                                                        "pc_survival_pickup_double"};
                Note(kGuns[static_cast<size_t>(it->weapon)]);
                taken = true;
                break;
            }
            case SurvivalPickupKind::Pipe:
                if (pipe_hits_ * 2 < pipe_hits_full_ && melee_timer_ < 0.0f) {
                    const float grip = 1.0f + 0.6f * static_cast<float>(boons_[SurvivalBoon::IronGrip]);
                    pipe_hits_full_ = static_cast<int>(std::lround(14.0f * grip));
                    pipe_hits_ = pipe_hits_full_;
                    pipe_time_ = pipe_time_full_ = 100.0f * grip;
                    Note("pc_survival_pickup_pipe", 0, 5.0f);
                    taken = true;
                }
                break;
            case SurvivalPickupKind::Flare:
                if (flares_ < MaxFlares()) {
                    ++flares_;
                    Note("pc_survival_pickup_flare");
                    taken = true;
                }
                break;
            }
        }
        if (taken) {
            Play(it->kind == SurvivalPickupKind::Health ? heal_cue_ : pickup_, nullptr);
            LogInfo("survival: picked up {}", hud_.pickup_key);
        }
        if (taken || it->age > it->life) {
            it = pickups_.erase(it);
        } else {
            ++it;
        }
    }
}

void SurvivalNight::SelectWeapon(SurvivalWeapon weapon, bool from_road) {
    if (stowing_ ? weapon == pending_weapon_ : weapon == weapon_) return;
    draw_pick_ = from_road ? 1.0f : 0.0f;
    if (BodyHoldsGun() && weapon_ != SurvivalWeapon::Flashlight && !hud_.dead && !game_.Demos().IsPlaying(kCatchDemo)) {
        if (!stowing_) PlaySound(swap_sound_, nullptr);
        stowing_ = true;
        pending_weapon_ = weapon;
        reload_timer_ = 0.0f;
        sights_ = false;
        return;
    }
    stowing_ = false;
    TakeWeapon(weapon);
    PlaySound(swap_sound_, nullptr);
}

void SurvivalNight::TakeWeapon(SurvivalWeapon weapon) {
    weapon_ = weapon;
    if (weapon == SurvivalWeapon::Flashlight) sights_ = false;
    since_swap_ = 0.0f;
    reload_timer_ = 0.0f;
    shot_timer_ = std::max(shot_timer_, 0.35f);
    swap_ = 1.0f;
    gun_ = GunAnim{};
    gun_.slide_locked = weapon == SurvivalWeapon::Handgun && weapons_[static_cast<size_t>(weapon)].clip == 0;
    gun_.chambered = weapons_[static_cast<size_t>(weapon)].clip > 0;
}

void SurvivalNight::NextWeapon(int step) {
    const int count = static_cast<int>(weapons_.size());
    for (int k = 1; k <= count; ++k) {
        const int next = ((static_cast<int>(stowing_ ? pending_weapon_ : weapon_) + step * k) % count + count) % count;
        if (weapons_[static_cast<size_t>(next)].owned) {
            SelectWeapon(static_cast<SurvivalWeapon>(next));
            return;
        }
    }
}

float SurvivalNight::Spread() const {
    const WeaponSpec spec = Gun(weapon_);
    const Player& player = game_.GetPlayer();
    const float moving = player.StickMagnitude() > 0.1f ? 1.35f : 1.0f;
    return spec.spread * (1.0f - 0.72f * aim_) * moving * (sprinting_ ? 2.0f : 1.0f) + bloom_;
}

void SurvivalNight::Spray(const glm::vec3& at, const glm::vec3& dir, int count, float speed) {
    for (int i = 0; i < count; ++i) {
        Debris d;
        d.kind = Debris::Kind::Blood;
        d.id = next_id_++;
        d.position = at;
        const glm::vec3 r(Rand() - 0.5f, Rand() - 0.2f, Rand() - 0.5f);
        d.velocity = (dir * (0.6f + 0.6f * Rand()) + r * 1.4f) * speed;
        d.ground = GroundAt(at, at.y - 1.5f);
        d.life = 1.2f;
        d.size = 0.6f + 0.9f * Rand();
        debris_.push_back(d);
    }
}

void SurvivalNight::Bleed(Enemy& e, const glm::vec3& at, const glm::vec3& dir, bool head, float power) {
    const float amount = (head ? 1.5f : 1.0f) * power;
    const float ground = GroundAt(at, at.y - 1.5f);
    const glm::vec3 far_side = at + dir * 0.22f;
    auto drop = [&](const glm::vec3& from, const glm::vec3& velocity, float size) {
        Debris d;
        d.kind = Debris::Kind::Blood;
        d.id = next_id_++;
        d.position = from;
        d.velocity = velocity;
        d.ground = ground;
        d.life = 1.4f;
        d.size = size;
        debris_.push_back(d);
    };
    auto drop_size = [&](int i) { return i % 4 == 0 ? 4.0f + 3.5f * Rand() : 2.0f + 1.5f * Rand(); };
    for (int i = 0, n = static_cast<int>(16.0f * amount); i < n; ++i) {
        const glm::vec3 r(Rand() - 0.5f, Rand() - 0.3f, Rand() - 0.5f);
        const float quick = Rand();
        drop(at - dir * 0.015f, glm::normalize(-dir + r * 1.6f) * (1.8f + 5.2f * quick * quick), drop_size(i));
    }
    for (int i = 0, n = static_cast<int>(22.0f * amount); i < n; ++i) {
        const glm::vec3 r(Rand() - 0.5f, Rand() - 0.4f, Rand() - 0.5f);
        drop(far_side, glm::normalize(dir + r * 0.55f) * (4.5f + 8.0f * Rand()), drop_size(i));
    }
    auto plume = [&](const glm::vec3& from, const glm::vec3& velocity, float life, float size) {
        Debris d;
        d.kind = Debris::Kind::Mist;
        d.id = next_id_++;
        d.position = from;
        d.velocity = velocity;
        d.ground = at.y - 3.0f;
        d.life = life;
        d.size = size * std::sqrt(power);
        d.spin = glm::normalize(velocity);
        debris_.push_back(d);
    };
    const glm::vec3 side = glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)) + glm::vec3(1e-3f, 0.0f, 0.0f));
    const glm::vec3 lift = glm::cross(side, dir);
    for (int i = 0, n = head ? 5 : 4; i < n; ++i) {
        const float a = 6.283f * (static_cast<float>(i) + 0.7f * Rand()) / static_cast<float>(n);
        const glm::vec3 across = side * std::cos(a) + lift * std::sin(a);
        plume(at - dir * 0.02f, glm::normalize(across - dir * 0.5f) * (2.0f + 2.2f * Rand()), 0.2f + 0.14f * Rand(), (head ? 0.36f : 0.28f) * (0.8f + 0.5f * Rand()));
    }
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 r(Rand() - 0.5f, Rand() - 0.35f, Rand() - 0.5f);
        plume(far_side, glm::normalize(dir + r * 0.6f) * (4.0f + 3.0f * Rand()), 0.2f + 0.16f * Rand(), (head ? 0.5f : 0.4f) * (0.8f + 0.5f * Rand()));
    }
    RayHit wall;
    if (!game_.Collision().Raycast(at + dir * 0.3f, dir, 3.0f, wall)) {
        Mark m;
        m.kind = Mark::Kind::Blood;
        m.id = next_id_++;
        m.position = glm::vec3(at.x + dir.x * (0.8f + Rand()), ground + 0.004f, at.z + dir.z * (0.8f + Rand()));
        m.spin = Rand() * 6.28f;
        m.size = (0.3f + 0.25f * Rand()) * amount;
        m.grow = 0.5f;
        m.variant = static_cast<int>(Rand() * 3.0f) % 3;
        marks_.push_back(m);
    } else {
        Mark m;
        m.kind = std::abs(wall.normal.y) > 0.6f ? Mark::Kind::Blood : Mark::Kind::Splat;
        m.id = next_id_++;
        m.position = wall.point + wall.normal * 0.004f;
        m.normal = wall.normal;
        m.spin = Rand() * 6.28f;
        m.size = (head ? 0.75f : 0.5f) * (0.8f + 0.4f * Rand()) * (1.0f - 0.15f * wall.distance);
        m.grow = 0.6f;
        m.variant = static_cast<int>(Rand() * 3.0f) % 3;
        marks_.push_back(m);
    }
    AddWound(e, at, dir, head);
}

glm::mat4 SurvivalNight::EnemyTransform(const Enemy& e, bool sunk) const {
    const float height = e.kind == SurvivalEnemyKind::Stretched ? 2.3f : e.kind == SurvivalEnemyKind::Crawler ? 0.9f : e.kind == SurvivalEnemyKind::Giant ? 3.6f : 1.9f;
    const float size = KindScale(e.kind);
    const float knees = e.state == Enemy::State::Dying && e.crumple && !e.ragdoll ? 0.42f * size * Smooth(e.timer / 0.32f) : 0.0f;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), e.position + glm::vec3(0.0f, -height * (sunk ? e.sink : 0.0f) - knees, 0.0f));
    if (e.state == Enemy::State::Dying && e.kind != SurvivalEnemyKind::Crawler && !e.ragdoll) {
        const glm::vec3 dir(e.fall_dir.x, 0.0f, e.fall_dir.y);
        const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0, 1, 0), dir));
        m = glm::translate(m, dir * (0.05f * size * std::sin(e.fall)) + glm::vec3(0.0f, 0.16f * size * std::sin(e.fall), 0.0f)) *
            glm::rotate(glm::mat4(1.0f), e.fall, axis);
    }
    m = m * glm::rotate(glm::mat4(1.0f), e.yaw, glm::vec3(0, 1, 0));
    if (e.kind == SurvivalEnemyKind::Crawler && std::abs(e.pitch) > 1e-4f) {
        m = m * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f)) * glm::rotate(glm::mat4(1.0f), -e.pitch, glm::vec3(1, 0, 0)) *
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f));
    }
    const float flinch = glm::length(e.flinch);
    if (flinch > 1e-4f) m = m * glm::rotate(glm::mat4(1.0f), flinch, glm::normalize(glm::cross(glm::vec3(0, 1, 0), glm::vec3(e.flinch.x, 0.0f, e.flinch.y))));
    if (size != 1.0f) m = glm::scale(m, glm::vec3(size));
    return m;
}

void SurvivalNight::AddWound(Enemy& e, const glm::vec3& at, const glm::vec3& dir, bool head) {
    if (UsesLisa(e.kind) || !doll_skeleton_ || skins_[e.slot].empty()) return;
    const std::vector<glm::mat4>& skin = skins_[e.slot];
    const anim::Skeleton& sk = *doll_skeleton_;
    const glm::mat4 to_model = glm::inverse(EnemyTransform(e));
    const glm::vec3 o(to_model * glm::vec4(at - dir * 0.4f, 1.0f));
    const glm::vec3 d = glm::normalize(glm::vec3(to_model * glm::vec4(dir, 0.0f)));
    int best = -1;
    float best_distance = 1e9f, best_t = 0.0f;
    glm::vec3 best_point(0.0f);
    for (size_t b = 0; b < sk.Size() && b < skin.size(); ++b) {
        const std::string& name = sk.names[b];
        if (name.size() < 8 || name.compare(0, 4, "SKL_") != 0 || name[4] != '0') continue;
        const glm::vec3 p(skin[b] * glm::vec4(sk.bind_world[b], 1.0f));
        const float t = std::max(0.0f, glm::dot(p - o, d));
        const float distance = glm::length(o + d * t - p);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<int>(b);
            best_t = t;
            best_point = p;
        }
    }
    if (best < 0 || best_distance > 0.35f) return;
    const std::string& name = sk.names[static_cast<size_t>(best)];
    const float radius = head || name.find("HEAD") != std::string::npos ? 0.1f
                         : name.find("CHEST") != std::string::npos || name.find("SPINE") != std::string::npos || name.find("WAIST") != std::string::npos ? 0.13f
                         : name.find("THIGH") != std::string::npos ? 0.08f
                                                                    : 0.055f;
    glm::vec3 out = (o + d * best_t) - best_point - d * radius;
    out = glm::length(out) > 1e-4f ? glm::normalize(out) : -d;
    const glm::vec3 surface = best_point + out * (radius * 0.92f);
    const glm::mat4 bind = glm::inverse(skin[static_cast<size_t>(best)]);
    Enemy::Wound w;
    w.bone = best;
    w.point = glm::vec3(bind * glm::vec4(surface, 1.0f));
    w.normal = glm::normalize(glm::vec3(bind * glm::vec4(out, 0.0f)));
    w.size = (head ? 1.3f : 1.0f) * (0.8f + 0.4f * Rand());
    w.drip = 0.1f;
    e.wounds.push_back(w);
    if (e.wounds.size() > 10) e.wounds.erase(e.wounds.begin());
}

void SurvivalNight::Impact(const glm::vec3& at, const glm::vec3& normal) {
    const bool vertical = std::abs(normal.y) < 0.65f;
    const int sparks = vertical ? 9 : 6;
    for (int i = 0; i < sparks; ++i) {
        Debris d;
        d.kind = Debris::Kind::Spark;
        d.id = next_id_++;
        d.position = at + normal * 0.012f;
        const glm::vec3 r(Rand() - 0.5f, Rand() - 0.35f, Rand() - 0.5f);
        d.velocity = glm::normalize(normal + r * 1.8f) * (3.2f + 5.2f * Rand());
        d.ground = at.y - 2.0f;
        d.life = 0.14f + 0.16f * Rand();
        debris_.push_back(d);
    }
    if (vertical && Rand() < 0.55f) Play(impact_, &at, -2.0f, 0.9f + 0.25f * Rand());
    Mark hole;
    hole.kind = Mark::Kind::Hole;
    hole.id = next_id_++;
    hole.position = at + normal * 0.003f;
    hole.normal = normal;
    hole.spin = Rand() * 6.28f;
    hole.size = 0.8f + 0.5f * Rand();
    marks_.push_back(hole);
    int holes = 0, pools = 0;
    for (auto it = marks_.rbegin(); it != marks_.rend(); ++it) {
        if (it->kind == Mark::Kind::Hole ? ++holes > 48 : ++pools > 24) it->age = 1e9f;
    }
}

void SurvivalNight::Eject(Debris::Kind kind, const glm::mat4& gun, const glm::vec3& at) {
    Debris d;
    d.kind = kind;
    d.id = next_id_++;
    d.position = glm::vec3(gun * glm::vec4(at, 1.0f));
    const glm::mat3 r(gun);
    if (kind == Debris::Kind::Magazine) {
        d.velocity = r * glm::vec3(0.0f, -1.2f, -0.2f);
        d.rotation = glm::quat_cast(r);
        d.spin = glm::vec3(1.5f, 0.0f, 2.0f) * (Rand() - 0.5f);
        d.life = 8.0f;
    } else {
        d.velocity = r * glm::vec3(-2.0f - Rand(), 1.6f + 0.8f * Rand(), -0.4f + 0.3f * Rand());
        d.rotation = glm::quat_cast(r);
        d.spin = glm::vec3(8.0f + 10.0f * Rand(), 15.0f * (Rand() - 0.5f), 6.0f * (Rand() - 0.5f));
        d.life = 7.0f;
    }
    d.ground = GroundAt(d.position, game_.GetPlayer().Feet().y);
    debris_.push_back(d);
}

void SurvivalNight::SpawnHealEffect(const glm::vec3& feet) {
    for (int i = 0; i < 18; ++i) {
        Debris d;
        d.kind = Debris::Kind::Heal;
        d.id = next_id_++;
        const float angle = 6.2831853f * (static_cast<float>(i) / 18.0f) + 0.35f * Rand();
        const float radius = 0.28f + 0.38f * Rand();
        d.position = feet + glm::vec3(std::cos(angle) * radius, 0.12f + 0.65f * Rand(), std::sin(angle) * radius);
        d.velocity = glm::vec3(-std::sin(angle) * 0.22f, 0.95f + 0.65f * Rand(), std::cos(angle) * 0.22f);
        d.spin = glm::vec3(angle, radius, 1.4f + 0.8f * Rand());
        d.life = 0.95f + 0.55f * Rand();
        d.size = 0.14f + 0.14f * Rand();
        debris_.push_back(d);
    }
}

void SurvivalNight::UpdateEffects(float dt) {
    for (Tracer& t : tracers_) t.age += dt;
    std::erase_if(tracers_, [](const Tracer& t) { return t.age > std::max(0.05f, (glm::distance(t.from, t.to) + 3.0f) / 400.0f); });
    {
        int holes = 0, pools = 0;
        for (auto it = marks_.rbegin(); it != marks_.rend(); ++it) {
            if (it->kind == Mark::Kind::Hole ? ++holes > 48 : ++pools > 24) it->age = std::max(it->age, 1e9f);
        }
    }
    for (Debris& d : debris_) {
        d.age += dt;
        if (d.kind == Debris::Kind::Mist) {
            d.velocity *= std::exp(-dt * 6.0f);
            d.velocity.y -= 4.5f * dt;
            d.position += d.velocity * dt;
            continue;
        }
        if (d.kind == Debris::Kind::Heal) {
            d.spin.x += d.spin.z * dt;
            d.position.y += d.velocity.y * dt;
            d.position.x += std::cos(d.spin.x) * 0.18f * dt;
            d.position.z += std::sin(d.spin.x) * 0.18f * dt;
            d.velocity.y *= std::exp(-dt * 0.55f);
            continue;
        }
        if (d.bounces >= 3 && d.kind != Debris::Kind::Blood) continue;
        d.velocity.y -= 9.8f * dt;
        d.position += d.velocity * dt;
        if (glm::length(d.spin) > 0.0f) d.rotation = glm::normalize(d.rotation * glm::quat(d.spin * dt));
        if (d.position.y <= d.ground) {
            d.position.y = d.ground;
            if (d.kind == Debris::Kind::Blood) {
                if (d.age < d.life && Rand() < 0.35f) {
                    Mark m;
                    m.kind = Mark::Kind::Blood;
                    m.id = next_id_++;
                    m.position = d.position + glm::vec3(0.0f, 0.004f, 0.0f);
                    m.spin = Rand() * 6.28f;
                    m.size = (0.1f + 0.12f * Rand()) * (0.8f + 0.06f * d.size);
                    m.grow = 1.0f;
                    m.variant = static_cast<int>(Rand() * 3.0f) % 3;
                    marks_.push_back(m);
                }
                d.age = d.life;
                continue;
            }
            if (d.kind == Debris::Kind::Spark) {
                d.age = d.life;
                continue;
            }
            if (d.bounces == 0) {
                if (d.kind == Debris::Kind::PipeHead || d.kind == Debris::Kind::PipeTail) {
                    Play(armor_, &d.position, -2.0f, 0.85f);
                } else if (d.kind == Debris::Kind::Magazine) {
                    Play(magazine_, &d.position, 0.0f, 1.0f);
                }
            }
            ++d.bounces;
            d.velocity = glm::vec3(d.velocity.x * 0.45f, -d.velocity.y * 0.35f, d.velocity.z * 0.45f);
            d.spin *= 0.5f;
            if (d.bounces >= 3) {
                d.velocity = glm::vec3(0.0f);
                d.spin = glm::vec3(0.0f);
                if (d.kind == Debris::Kind::PipeHead || d.kind == Debris::Kind::PipeTail) {
                    glm::vec3 along = glm::vec3(d.rotation * glm::vec3(0.0f, 0.0f, 1.0f)) * glm::vec3(1.0f, 0.0f, 1.0f);
                    along = glm::length(along) > 1e-3f ? glm::normalize(along) : glm::vec3(1.0f, 0.0f, 0.0f);
                    d.rotation = glm::quatLookAt(-along, glm::vec3(0.0f, 1.0f, 0.0f));
                    d.position.y = d.ground + 0.02f;
                }
            }
        }
    }
    std::erase_if(debris_, [](const Debris& d) { return d.age >= d.life; });
    for (Mark& m : marks_) {
        m.age += dt;
        m.grow = std::min(1.0f, m.grow + dt / 2.2f);
    }
    std::erase_if(marks_, [](const Mark& m) { return m.age > (m.kind == Mark::Kind::Hole ? 40.0f : 70.0f); });
    for (Delayed& c : delayed_) {
        c.wait -= dt;
        if (c.wait <= 0.0f && c.cue) Play(*c.cue, nullptr, c.gain_db, c.speed);
    }
    std::erase_if(delayed_, [](const Delayed& c) { return c.wait <= 0.0f; });
    hud_.hit_mark = std::max(0.0f, hud_.hit_mark - dt / (hud_.hit_mark_kill ? 0.45f : 0.26f));
}

void SurvivalNight::StartReload() {
    WeaponState& state = weapons_[static_cast<size_t>(weapon_)];
    const WeaponSpec spec = Gun(weapon_);
    reload_from_empty_ = state.clip == 0;
    sprinting_ = false;
    if (weapon_ == SurvivalWeapon::Handgun) {
        reload_length_ = reload_from_empty_ ? 1.35f : 1.1f;
        PlaySound(reload_from_empty_ ? reload_sound_ : reload_tactical_, nullptr);
    } else if (SurvivalCycled(weapon_)) {
        reload_length_ = 0.25f + spec.reload;
        gun_.pumped_after_reload = false;
    } else {
        reload_length_ = spec.reload;
        PlaySound(weapon_ == SurvivalWeapon::Double ? double_reload_ : revolver_reload_, nullptr);
    }
    reload_timer_ = reload_length_;
    gun_.magazine_dropped = false;
    gun_.cases_out = false;
    LogInfo("survival: reload {} ({} in the gun, {} left)", Spec(weapon_).name_key, state.clip, state.reserve);
}

float SurvivalNight::ShellStep() const {
    if (!SurvivalCycled(weapon_) || reload_timer_ <= 0.0f) return -1.0f;
    const float step = Gun(weapon_).reload;
    const float turn = reload_length_ > step + 0.01f ? reload_length_ - step : 0.0f;
    return std::clamp((reload_length_ - reload_timer_ - turn) / step, 0.0f, 1.0f);
}

void SurvivalNight::FinishReloadStep(bool cut) {
    WeaponState& state = weapons_[static_cast<size_t>(weapon_)];
    const WeaponSpec spec = Gun(weapon_);
    if (SurvivalCycled(weapon_)) {
        const bool home = !cut || ShellStep() >= 0.75f;
        gun_.reload_cut = cut;
        if (home && state.reserve > 0 && state.clip < spec.clip_size) {
            ++state.clip;
            --state.reserve;
            PlaySound(weapon_ == SurvivalWeapon::Lever ? lever_round_sound_ : shell_in_sound_, nullptr);
        }
        if (!cut && state.clip < spec.clip_size && state.reserve > 0 && !fire_held_) {
            reload_length_ = spec.reload;
            reload_timer_ = spec.reload;
            return;
        }
        if (!gun_.chambered) {
            gun_.pumping = true;
            gun_.pump_timer = 0.0f;
            gun_.chambered = true;
            shot_timer_ = std::max(shot_timer_, 0.35f);
            PlaySound(weapon_ == SurvivalWeapon::Lever ? lever_sound_ : pump_sound_, nullptr);
        }
        reload_timer_ = 0.0f;
        return;
    }
    const int take = std::min(spec.clip_size - state.clip, state.reserve);
    state.clip += take;
    state.reserve -= take;
    reload_timer_ = 0.0f;
    gun_.slide_locked = false;
}

void SurvivalNight::Fire(const Camera& camera) {
    WeaponState& state = weapons_[static_cast<size_t>(weapon_)];
    const WeaponSpec spec = Gun(weapon_);
    if (state.clip <= 0 || (SurvivalCycled(weapon_) && !gun_.chambered)) {
        PlaySound(empty_, nullptr);
        shot_timer_ = 0.25f;
        return;
    }
    --state.clip;
    LogInfo("survival: round fired, {} in the gun", state.clip);
    shot_timer_ = spec.interval;
    gun_.since_shot = 0.0f;
    const bool third = game_.ThirdPersonCameraActive() && game_.ThirdPersonShown();
    const Camera& aim_view = camera;
    const glm::vec3 forward = aim_view.Forward();
    const glm::vec3 right = aim_view.Right();
    const glm::vec3 up = aim_view.Up();
    const float spread = Spread();
    const glm::mat4 gun = WeaponTransform(camera);
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    muzzle_at_ = glm::vec3(gun * glm::vec4(model.muzzle, 1.0f));
    muzzle_ = 0.05f;
    muzzle_roll_ = Rand() * 6.28f;
    muzzle_size_ = 0.8f + 0.45f * Rand();
    bool landed = false, landed_head = false;
    const int kills_before = kills_;
    float landed_at = 0.0f;
    std::vector<std::pair<Enemy*, float>> struck;
    const bool settled = spec.pellets == 1 && bloom_ < 0.002f && game_.GetPlayer().StickMagnitude() <= 0.1f;
    for (int pellet = 0; pellet < spec.pellets; ++pellet) {
        const float a = 2.0f * kPi * Rand();
        const float r = spec.pellets > 1 ? spread * std::sqrt(Rand()) : spread * Rand() * (settled ? 0.2f : 1.0f);
        const glm::vec3 view_dir = glm::normalize(forward + right * (std::cos(a) * r) + up * (std::sin(a) * r));
        glm::vec3 origin = camera.position;
        glm::vec3 dir = view_dir;
        if (third) origin += view_dir * std::max(0.0f, glm::dot(game_.GetCamera().position - camera.position, view_dir));
        RayHit wall;
        bool walled = game_.Collision().Raycast(origin, dir, spec.range, wall);
        float wall_at = walled ? wall.distance : spec.range;
        if (third) {
            Enemy* first = nullptr;
            float first_at = wall_at;
            bool first_head = false;
            const glm::vec3 target = origin + dir * (HitEnemy(origin, dir, wall_at, nullptr, first, first_at, first_head) ? first_at : wall_at);
            const glm::vec3 to = target - muzzle_at_;
            const float length = glm::length(to);
            if (RayHit block; length > 0.4f && game_.Collision().Raycast(muzzle_at_, to / length, length - 0.3f, block)) {
                origin = muzzle_at_;
                dir = to / length;
                wall = block;
                walled = true;
                wall_at = block.distance;
            }
        }
        Enemy* hit = nullptr;
        float t = wall_at;
        bool head = false;
        float end = wall_at;
        static const bool aim_log = std::getenv("PT_SURVIVAL_AIM_LOG") != nullptr;
        if ((aim_log || aim_at_head_) && pellet == 0) {
            Enemy* struck_now = nullptr;
            float at = wall_at;
            bool on_head = false;
            const bool body = HitEnemy(origin, dir, wall_at, nullptr, struck_now, at, on_head);
            const glm::vec3 lands = origin + dir * (body ? at : wall_at);
            const float off = std::acos(std::clamp(glm::dot(glm::normalize(lands - aim_view.position), aim_view.Forward()), -1.0f, 1.0f));
            const Camera eye = game_.GetCamera();
            const glm::vec3 from_eye = lands - eye.position;
            const float eye_miss = glm::length(from_eye - eye.Forward() * glm::dot(from_eye, eye.Forward()));
            LogInfo("survival: {} shot lands {:.2f} deg off the crosshair ({}, {:.1f} m); the eye's ray passes it {:.2f} m off", third ? "third person" : "first person",
                    glm::degrees(off), body ? (on_head ? "the head" : "the body") : walled ? "a wall" : "nothing", glm::length(lands - aim_view.position), eye_miss);
        }
        if (HitEnemy(origin, dir, wall_at, nullptr, hit, t, head)) {
            end = t;
            const glm::vec3 at = origin + dir * t;
            const bool buckshot = weapon_ == SurvivalWeapon::Shotgun || weapon_ == SurvivalWeapon::Double;
            const float falloff = buckshot ? 1.0f - 0.85f * Smooth((t - 7.0f) / 18.0f) : 1.0f;
            const float head_gain = (buckshot ? 1.5f : 3.0f) *
                                    (1.0f + 0.25f * static_cast<float>(boons_[SurvivalBoon::Headhunter]));
            const float daily_head = start_ == SurvivalStart::Daily ? DailyMutatorById(daily_mutator_).headshot_mult : 1.0f;
            const float amount = spec.damage * falloff * (head ? head_gain * daily_head : 1.0f);
            const bool alive = hit->state != Enemy::State::Dying;
            Damage(*hit, amount, origin, at, dir, head);
            if (alive) {
                auto it = std::find_if(struck.begin(), struck.end(), [&](const auto& s) { return s.first == hit; });
                if (it == struck.end()) {
                    struck.push_back({hit, amount});
                } else {
                    it->second += amount;
                }
            }
            if (!landed || Rand() < 0.3f) {
                Play(hit_, &at, head ? 2.0f : 0.0f);
                if (head) Play(gore_, &at, -6.0f, 1.1f);
            }
            if (!landed) landed_at = t;
            landed = true;
            landed_head = landed_head || head;
            Enemy* through = hit;
            float past = t;
            const int bodies = weapon_ == SurvivalWeapon::Magnum || weapon_ == SurvivalWeapon::Lever ? 1 + boons_[SurvivalBoon::HeavyRounds] : 0;
            for (int k = 0; k < bodies; ++k) {
                Enemy* next = nullptr;
                float t2 = 0.0f;
                bool head2 = false;
                const float start = past + 0.25f;
                if (start >= wall_at || !HitEnemy(origin + dir * start, dir, wall_at - start, through, next, t2, head2)) break;
                past = start + t2;
                Damage(*next, spec.damage * 0.6f * (head2 ? 2.0f : 1.0f), origin, origin + dir * past, dir, head2);
                end = past;
                through = next;
            }
        } else if (walled) {
            Impact(wall.point, wall.normal);
        }
        TaskShot(origin, dir, end + 0.3f);
        if (pellet < 3 && end > 1.5f) tracers_.push_back({muzzle_at_, origin + dir * std::min(end, 60.0f), 0.0f});
    }
    if (weapon_ == SurvivalWeapon::Shotgun || weapon_ == SurvivalWeapon::Double) {
        for (auto& [e, amount] : struck) {
            if (e->state != Enemy::State::Chasing && e->state != Enemy::State::Striking) continue;
            const KindSpec& kind = Kind(e->kind);
            if (amount / kind.mass < 60.0f) continue;
            glm::vec3 back = e->position - camera.position;
            back.y = 0.0f;
            if (glm::length(back) > 1e-3f) e->knock += glm::normalize(back) * std::min(4.2f, 7.0f * amount / 260.0f / kind.mass);
            e->stagger = std::max(e->stagger, 0.6f);
            if (e->state == Enemy::State::Striking && e->kind != SurvivalEnemyKind::Giant) {
                e->state = Enemy::State::Chasing;
                e->timer = 0.0f;
                e->strike_cooldown = std::max(e->strike_cooldown, 0.6f);
            }
        }
    }
    PlayShot(camera.position, camera.Forward(), camera.Right());
    ++shots_;
    if (landed) ++hits_;
    if (landed) {
        hud_.hit_head = landed_head;
        const bool killed = kills_ > kills_before;
        hud_.hit_mark = 1.0f;
        hud_.hit_mark_head = landed_head;
        hud_.hit_mark_kill = killed;
        const float wait = std::max(0.06f, landed_at / 343.0f);
        delayed_.push_back({wait, landed_head ? &confirm_head_ : &confirm_, 0.0f, 1.0f});
        if (killed) delayed_.push_back({wait + 0.05f, &confirm_kill_, 0.0f, 1.0f});
    }
    Player& p = game_.GetPlayer();
    const float kick = spec.kick * (1.0f - 0.35f * aim_) * (0.85f + 0.3f * Rand());
    const float side = (Rand() - 0.5f) * kick * 0.6f;
    view_kick_speed_ += glm::vec2(kick, side) * 36.0f;
    p.pitch = Player::ClampPitch(p.pitch + kick * 0.3f);
    p.target_pitch = Player::ClampPitch(p.target_pitch + kick * 0.3f);
    const float back = weapon_ == SurvivalWeapon::Handgun ? 0.05f : (SurvivalLongGun(weapon_) ? 0.044f : 0.05f) * (1.0f - 0.3f * aim_);
    gun_kick_speed_ += glm::vec2(back, kick * (SurvivalLongGun(weapon_) ? 2.4f : 4.0f)) * 30.0f;
    bloom_ = std::min(0.06f, bloom_ + spec.spread * 0.45f);
    shake_ = std::min(1.0f, shake_ + kick * 3.5f);
    sprinting_ = false;
    switch (weapon_) {
    case SurvivalWeapon::Handgun:
        gun_.slide = 1.0f;
        Eject(Debris::Kind::Casing, gun, model.ejection);
        if (state.clip == 0) gun_.slide_locked = true;
        break;
    case SurvivalWeapon::Shotgun:
        gun_.pumping = true;
        gun_.shell_pending = true;
        gun_.pump_timer = -0.18f;
        gun_.chambered = state.clip > 0;
        break;
    case SurvivalWeapon::Magnum:
        gun_.hammer = 0.0f;
        gun_.trigger = -1.0f;
        gun_.cylinder_to += 2.0f * kPi / 6.0f;
        break;
    case SurvivalWeapon::Lever:
        gun_.hammer_fall = 1.0f;
        gun_.pumping = true;
        gun_.shell_pending = true;
        gun_.pump_timer = -0.14f;
        gun_.chambered = state.clip > 0;
        break;
    default: break;
    }
}

void SurvivalNight::FlashBurst(const Camera& view) {
    const Camera camera = game_.ThirdPersonCameraActive() ? game_.GetCamera() : view;
    if (burst_timer_ > 0.0f) {
        PlaySound(empty_, nullptr);
        return;
    }
    const WeaponSpec spec = Gun(SurvivalWeapon::Flashlight);
    burst_timer_ = spec.interval;
    burst_light_ = 0.35f;
    burst_dir_ = camera.Forward();
    burst_at_ = camera.position + burst_dir_ * 0.3f;
    Play(burst_, nullptr);
    int stunned = 0;
    for (Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dying) continue;
        const glm::vec3 chest = e.position + glm::vec3(0.0f, e.kind == SurvivalEnemyKind::Crawler ? 0.45f : 1.2f * KindScale(e.kind), 0.0f);
        const glm::vec3 to = chest - camera.position;
        const float distance = glm::length(to);
        if (distance > spec.range || glm::dot(to / distance, burst_dir_) < 0.87f) continue;
        RayHit wall;
        if (game_.Collision().Raycast(camera.position, to / distance, distance, wall)) continue;
        Damage(e, spec.damage, camera.position, chest, to / distance, false);
        if (e.state == Enemy::State::Dying) continue;
        const float stun = Kind(e.kind).stun;
        if (stun <= 0.0f || e.state == Enemy::State::Rising) {
            e.slow = 0.8f;
        } else {
            e.state = Enemy::State::Stunned;
            e.timer = stun;
            e.latched = false;
            e.pitch = 0.0f;
        }
        ++stunned;
    }
    LogInfo("survival: flashlight burst, {} caught", stunned);
}

void SurvivalNight::AutoPlay(uint32_t& held, uint32_t& pressed, const Camera& view) {
    held = pressed = 0;
    const Camera camera = game_.ThirdPersonCameraActive() ? game_.GetCamera() : view;
    Player& player = game_.GetPlayer();
    if (hud_.dead) {
        if (hud_.dead_time > 4.0f) pressed |= kSurvivalFire;
        return;
    }
    if (auto_passive_) return;
    const Enemy* nearest = nullptr;
    float best = 1e9f;
    for (const Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dying || e.state == Enemy::State::Rising) continue;
        const float d = glm::distance(e.position, player.Feet());
        if (d < best) {
            best = d;
            nearest = &e;
        }
    }
    if (nearest) {
        const glm::vec3 aim = nearest->kind == SurvivalEnemyKind::Crawler ? nearest->position + glm::vec3(0.0f, 0.42f, 0.0f)
                                                                          : nearest->position + glm::vec3(0.0f, 1.15f * KindScale(nearest->kind), 0.0f);
        const glm::vec3 d = glm::normalize(aim - camera.position);
        player.yaw = player.target_yaw = std::atan2(-d.x, -d.z);
        player.pitch = player.target_pitch = Player::ClampPitch(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        held |= kSurvivalAim;
        if (shot_timer_ <= 0.0f && glm::dot(camera.Forward(), d) > 0.99f) pressed |= kSurvivalFire;
        if (best < 4.5f && burst_timer_ <= 0.0f) FlashBurst(camera);
    }
    if (phase_ == SurvivalPhase::Break && !pickups_.empty() && std::fmod(time_, 0.5f) < 1.0f / 60.0f) {
        const Pickup& p = pickups_.front();
        if (p.settled) player.Warp(p.position + glm::vec3(0.0f, 0.05f, 0.0f), player.BodyFoxYaw());
    }
    const WeaponState& s = weapons_[static_cast<size_t>(weapon_)];
    if (s.clip == 0 && s.reserve > 0) pressed |= kSurvivalReload;
    if (s.clip == 0 && s.reserve == 0 && reload_timer_ <= 0.0f) {
        for (size_t i = weapons_.size() - 1; i > 0; --i) {
            if (weapons_[i].owned && (weapons_[i].clip > 0 || weapons_[i].reserve > 0) && static_cast<size_t>(weapon_) != i) {
                SelectWeapon(static_cast<SurvivalWeapon>(i));
                break;
            }
        }
    }
}

void SurvivalNight::UpdatePlayer(float dt, const InputState& input, const Camera& camera) {
    Player& player = game_.GetPlayer();
    uint32_t held = input.survival_held;
    uint32_t pressed = input.survival_pressed;
    if (auto_play_) AutoPlay(held, pressed, camera);
    if (!script_.empty() || act_pressed_) RunScript(held, pressed);
    held |= script_held_;
    if (trigger_dead_) {
        if (((held | pressed) & kSurvivalFire) != 0) {
            held &= ~kSurvivalFire;
            pressed &= ~kSurvivalFire;
        } else {
            trigger_dead_ = false;
            LogInfo("survival: the card's click let go");
        }
    }
    fire_held_ = (held & kSurvivalFire) != 0;
    held_ = held;
    hud_.task_shown = std::clamp(hud_.task_shown + dt * ((held & kSurvivalInfo) != 0 ? 6.0f : -3.0f), 0.0f, 1.0f);
    body_run_ += (RunWeight() - body_run_) * std::min(1.0f, dt * 5.0f);
    if (hud_.dead) {
        hud_.dead_time += dt;
        hud_.can_retry = hud_.dead_time > 2.5f;
        if (dead_fall_ < 1.0f) {
            dead_fall_speed_ += dt * 3.2f;
            dead_fall_ = std::min(1.0f, dead_fall_ + dead_fall_speed_ * dt);
        }
        lean_ = 0.0f;
        player.sprint_lean = 0.0f;
        if (player_ragdoll_.Active()) {
            const auto ground = [&](const glm::vec3& p) { return GroundAt(p, dead_ground_); };
            const auto sweep = [&](const glm::vec3& from, const glm::vec3& to, glm::vec3& point, glm::vec3& normal) {
                const glm::vec3 dv = to - from;
                const float l = glm::length(dv);
                RayHit hit;
                if (l < 1e-4f || !game_.Collision().Raycast(from, dv / l, l, hit)) return false;
                point = hit.point;
                normal = hit.normal;
                return true;
            };
            player_ragdoll_.Step(dt, ground, sweep);
            const float impact = player_ragdoll_.TakeImpact();
            if (impact > 1.2f && !player_fall_heard_) {
                player_fall_heard_ = true;
                const glm::vec3 at = player_ragdoll_.PelvisPosition();
                Play(body_fall_, &at, std::min(3.5f, -3.0f + impact * 1.3f));
            }
        }
        if (dropped_gun_.active) {
            dropped_gun_.vel.y -= 9.81f * dt;
            dropped_gun_.pos += dropped_gun_.vel * dt;
            const float spin_len = glm::length(dropped_gun_.spin);
            if (spin_len > 1e-4f) {
                dropped_gun_.rot = glm::normalize(glm::angleAxis(spin_len * dt, dropped_gun_.spin / spin_len) * dropped_gun_.rot);
            }
            const float floor = GroundAt(dropped_gun_.pos, dropped_gun_.ground) + 0.028f;
            if (dropped_gun_.pos.y < floor) {
                dropped_gun_.pos.y = floor;
                if (dropped_gun_.bounces == 0) Play(armor_, &dropped_gun_.pos, -3.0f, 0.8f);
                ++dropped_gun_.bounces;
                dropped_gun_.vel.y = std::abs(dropped_gun_.vel.y) > 0.35f ? -dropped_gun_.vel.y * 0.22f : 0.0f;
                dropped_gun_.vel.x *= 0.55f;
                dropped_gun_.vel.z *= 0.55f;
                const glm::vec3 fwd = dropped_gun_.rot * glm::vec3(0.0f, 0.0f, 1.0f);
                const float yaw = std::atan2(fwd.x, fwd.z);
                const glm::quat flat = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(kPi * 0.5f, glm::vec3(0.0f, 0.0f, 1.0f));
                dropped_gun_.rot = glm::normalize(glm::slerp(dropped_gun_.rot, flat, std::min(1.0f, dt * 12.0f)));
                dropped_gun_.spin *= 0.35f;
            }
        }
        shake_ = std::max(0.0f, shake_ - dt * 4.0f);
        speed_scale_ = 1.0f;
        player.sprint_rate = 1.0f;
        player.sprint_stride = 1.0f;
        fov_kick_ = 0.0f;
        aim_ = std::max(0.0f, aim_ - dt * 4.0f);
        sights_w_ = std::max(0.0f, sights_w_ - dt * 4.0f);
        if (hud_.can_retry) {
            if ((pressed & kSurvivalFire) != 0) {
                Retry();
            } else if ((pressed & kSurvivalReload) != 0) {
                game_.RequestSurvivalHub();
            }
        }
        return;
    }
    if (aim_at_head_) {
        const Enemy* nearest = nullptr;
        for (const Enemy& e : enemies_) {
            if (e.state == Enemy::State::Dying) continue;
            if (!nearest || glm::distance(e.head, camera.position) < glm::distance(nearest->head, camera.position)) nearest = &e;
        }
        if (nearest) {
            const glm::vec3 to = glm::normalize(nearest->head - camera.position);
            const glm::vec3 f = camera.Forward();
            const float yaw = WrapAngle(std::atan2(-to.x, -to.z) - std::atan2(-f.x, -f.z));
            const float pitch = std::asin(std::clamp(to.y, -1.0f, 1.0f)) - std::asin(std::clamp(f.y, -1.0f, 1.0f));
            player.yaw = player.target_yaw = player.yaw + yaw;
            player.pitch = player.target_pitch = Player::ClampPitch(player.pitch + pitch);
        }
    }
    if (look_primed_ && dt > 0.0f) {
        const glm::vec2 turn(WrapAngle(player.yaw - last_yaw_), player.pitch - last_pitch_);
        look_rate_ += (turn / dt - look_rate_) * (1.0f - std::exp(-dt / 0.06f));
    }
    last_yaw_ = player.yaw;
    last_pitch_ = player.pitch;
    look_primed_ = true;
    if (dt > 0.0f) {
        constexpr float kMost = 0.3f;
        const float firm = 1.0f - 0.7f * aim_;
        const glm::vec2 want = glm::vec2(kMost * std::tanh(look_rate_.x * 0.09f / kMost), 0.6f * kMost * std::tanh(look_rate_.y * 0.09f / (0.6f * kMost))) * firm;
        const float omega = glm::mix(11.0f, 16.0f, aim_), damping = 0.62f;
        for (float left = dt; left > 1.0e-6f;) {
            const float h = std::min(left, 1.0f / 120.0f);
            sway_speed_ += (omega * omega * (want - sway_) - 2.0f * damping * omega * sway_speed_) * h;
            sway_ += sway_speed_ * h;
            const float aside = player.StickMagnitude() > 0.05f ? std::sin(WrapAngle(player.StickHeading() - player.FoxYaw())) * std::min(1.0f, player.StickMagnitude()) : 0.0f;
            side_lean_speed_ += (100.0f * (0.07f * aside * firm - side_lean_) - 2.0f * 0.8f * 10.0f * side_lean_speed_) * h;
            side_lean_ += side_lean_speed_ * h;
            left -= h;
        }
    }
    const bool aim_button = (held & kSurvivalAim) != 0;
    if (aim_button) {
        aim_held_ = aim_held_ < 0.0f ? 0.0f : aim_held_ + dt;
    } else {
        if (aim_held_ >= 0.0f && aim_held_ < kAimTap && weapon_ != SurvivalWeapon::Flashlight) {
            sights_ = !sights_;
            LogInfo("survival: sights {}", sights_ ? "in" : "out");
        }
        aim_held_ = -1.0f;
    }
    const bool forward = player.StickMagnitude() > 0.45f && std::abs(WrapAngle(player.StickHeading() - player.FoxYaw())) < 1.0f;
    if (sights_ && (pressed & kSurvivalSprint) != 0 && forward) sights_ = false;
    const bool aiming = (aim_button || sights_) && !sprinting_ && reload_timer_ <= 0.0f && swap_ < 0.5f;
    aim_ += ((aiming ? 1.0f : 0.0f) - aim_) * std::min(1.0f, dt * 8.0f);
    {
        const float after_shot = gun_.since_shot < 0.8f ? 1.0f - Smooth((gun_.since_shot - 0.5f) / 0.3f) : 0.0f;
        const float target = std::max(aiming ? 1.0f : 0.0f, after_shot);
        const auto spring = [dt](float& value, float& speed, float to, float omega) {
            const float x = omega * dt;
            const float k = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
            const float off = value - to;
            const float carried = (speed + omega * off) * dt;
            speed = (speed - omega * carried) * k;
            value = to + (off + carried) * k;
        };
        spring(raise_, raise_speed_, target, after_shot > raise_ ? 26.0f : 11.0f);
        spring(reload_front_, reload_front_speed_, reload_timer_ > 0.0f ? 1.0f : 0.0f, 9.5f);
        reload_front_ = std::clamp(reload_front_, 0.0f, 1.0f);
        raise_ = std::clamp(raise_, 0.0f, 1.0f);
        spring(body_recoil_, body_recoil_speed_, std::max(0.0f, 5.2f * gun_kick_.x + 0.25f * gun_kick_.y), 42.0f);
    }
    sights_w_ += ((sights_ ? 1.0f : 0.0f) - sights_w_) * std::min(1.0f, dt * 10.0f);
    const bool on_trigger = aim_ > 0.4f || fire_held_ || shot_timer_ > 0.0f;
    trigger_finger_ += ((on_trigger ? 1.0f : 0.0f) - trigger_finger_) * std::min(1.0f, dt * (on_trigger ? 16.0f : 6.0f));
    sprint_hold_ = (held & kSurvivalSprint) != 0 ? 0.45f : std::max(0.0f, sprint_hold_ - dt);
    if ((held & kSurvivalSprint) == 0 && (aim_button || (pressed & (kSurvivalFire | kSurvivalReload | kSurvivalMelee | kSurvivalFlare)) != 0)) sprint_hold_ = 0.0f;
    const bool wants = sprint_hold_ > 0.0f && forward && !aiming && !winded_;
    const bool was_sprinting = sprinting_;
    sprinting_ = wants && stamina_ > 0.0f;
    if (sprinting_ && !was_sprinting) LogInfo("survival: sprint ({:.0f} % stamina)", stamina_ * 100.0f);
    if (sprinting_) {
        stamina_ = std::max(0.0f, stamina_ - dt * 0.155f / (1.0f + 0.3f * static_cast<float>(boons_[SurvivalBoon::LongLegs])));
        stamina_wait_ = 1.10f;
        if (stamina_ <= 0.0f) {
            winded_ = true;
            LogInfo("survival: winded");
        }
    } else {
        stamina_wait_ = std::max(0.0f, stamina_wait_ - dt);
        if (stamina_wait_ <= 0.0f) stamina_ = std::min(1.0f, stamina_ + dt * 0.28f);
        if (winded_ && stamina_ >= 0.25f) winded_ = false;
    }
    const float speed_target = sprinting_ ? kNightSprintSpeed * (1.0f + 0.04f * static_cast<float>(boons_[SurvivalBoon::LongLegs])) : aiming ? 0.85f : kNightPaceSpeed;
    speed_scale_ += (speed_target - speed_scale_) * std::min(1.0f, dt * (speed_target > speed_scale_ ? 3.0f : 5.0f));
    player.sprint_rate = run_stride::Cadence(speed_scale_);
    player.sprint_stride = speed_scale_ / player.sprint_rate;
    const float sprint_weight = SprintWeight();
    const float zoom = weapon_ == SurvivalWeapon::Magnum || weapon_ == SurvivalWeapon::Lever ? 0.1f
                       : weapon_ == SurvivalWeapon::Shotgun || weapon_ == SurvivalWeapon::Double ? 0.06f
                                                                                                   : 0.07f;
    const float sights_zoom = weapon_ == SurvivalWeapon::Lever ? 0.24f : weapon_ == SurvivalWeapon::Magnum ? 0.2f
                              : weapon_ == SurvivalWeapon::Shotgun || weapon_ == SurvivalWeapon::Double ? 0.12f
                                                                                                          : 0.16f;
    float nearest = 1e9f;
    for (const Enemy& e : enemies_) {
        const glm::vec3 to = e.position + glm::vec3(0.0f, e.state == Enemy::State::Dying ? 0.3f : 1.0f, 0.0f) - camera.position;
        const float along = glm::dot(to, camera.Forward());
        if (along > 0.1f && along < nearest && glm::length(to - camera.Forward() * along) < (e.kind == SurvivalEnemyKind::Giant ? 1.2f : 0.75f)) nearest = along;
    }
    const float close = nearest < 3.6f ? 1.0f - Smooth((nearest - 1.6f) / 2.0f) : 0.0f;
    close_body_ += (close - close_body_) * std::min(1.0f, dt * 8.0f);
    if (nearest < 3.6f) close_body_at_ += (nearest - close_body_at_) * std::min(1.0f, dt * 10.0f);
    fov_kick_ = 0.1f * sprint_weight - (zoom + sights_zoom * sights_w_) * aim_ * (1.0f - close_body_) - 0.012f * std::max(0.0f, 1.0f - gun_.since_shot / 0.08f);
    lean_ += (0.015f * sprint_weight * std::min(1.0f, player.StickMagnitude() * 1.5f) - lean_) * std::min(1.0f, dt * 7.0f);
    bob_weight_ += (std::min(1.0f, player.StickMagnitude()) - bob_weight_) * std::min(1.0f, dt * 6.0f);
    const float error = run_stride::AdvanceBob(bob_phase_, bob_steps_, player.sounding_steps, player.StickMagnitude(), player.sprint_rate, dt);
    static const bool trace = std::getenv("PT_SURVIVAL_BOB_TRACE") != nullptr;
    if (trace && !std::isnan(error)) LogInfo("survival: step at bob error {:.2f} rad, cadence {:.2f}, stride {:.2f}", error, player.sprint_rate, player.sprint_stride);
    shake_ = std::max(0.0f, shake_ - dt * 2.8f);
    bloom_ = std::max(0.0f, bloom_ - dt * (0.08f + 0.1f * aim_));
    shot_timer_ = std::max(0.0f, shot_timer_ - dt);
    burst_timer_ = std::max(0.0f, burst_timer_ - dt);
    if (stowing_) {
        swap_ = std::min(1.0f, swap_ + dt / 0.16f);
        if (swap_ >= 1.0f) {
            stowing_ = false;
            TakeWeapon(pending_weapon_);
        }
    } else {
        const float seconds = !BodyHoldsGun() ? 1.0f / 2.2f : SurvivalLongGun(weapon_) ? 0.75f + 0.15f * draw_pick_ : 0.5f + 0.1f * draw_pick_;
        swap_ = std::max(0.0f, swap_ - dt / seconds);
    }
    if (offer_count_ == 0) {
        if (melee_timer_ < 0.0f) {
            auto select_slot = [&](SurvivalWeapon w) {
                if (weapons_[static_cast<size_t>(w)].owned) SelectWeapon(w);
            };
            if ((pressed & kSurvivalSlot1) != 0) select_slot(SurvivalWeapon::Handgun);
            else if ((pressed & kSurvivalSlot2) != 0) select_slot(SurvivalWeapon::Shotgun);
            else if ((pressed & kSurvivalSlot3) != 0) select_slot(SurvivalWeapon::Magnum);
            else if ((pressed & kSurvivalSlot4) != 0) select_slot(SurvivalWeapon::Lever);
            else if ((pressed & kSurvivalSlot5) != 0) select_slot(SurvivalWeapon::Double);
            else if ((pressed & kSurvivalSlot0) != 0) select_slot(SurvivalWeapon::Flashlight);
            else if ((pressed & kSurvivalSwapPrev) != 0) NextWeapon(-1);
            else if ((pressed & kSurvivalSwap) != 0) NextWeapon(1);
        }
        if ((pressed & kSurvivalMelee) != 0) StartMelee();
        if ((pressed & kSurvivalFlare) != 0) StartThrow();
    }
    if (offer_count_ > 0 && hud_.offer_age >= 0.35f) {
        if (pressed & kSurvivalPick1) TakeBoon(0);
        if (pressed & kSurvivalPick2) TakeBoon(1);
        if (pressed & kSurvivalPick3) TakeBoon(2);
    }
    if (const bool choosing = offer_count_ > 0; choosing != offer_locked_) {
        if (choosing) {
            player.locks.Add('B', "SurvivalOffer", 3u);
        } else {
            player.locks.Remove('B', "SurvivalOffer");
        }
        offer_locked_ = choosing;
    }
    if (offer_count_ > 0) return;
    WeaponState& state = weapons_[static_cast<size_t>(weapon_)];
    const WeaponSpec spec = Gun(weapon_);
    if (reload_timer_ > 0.0f) {
        gun_.trigger = -1.0f;
        if (SurvivalCycled(weapon_) && (pressed & kSurvivalFire) != 0 && state.clip > 0) {
            FinishReloadStep(true);
            shot_timer_ = std::max(shot_timer_, 0.1f);
            fire_buffer_ = 0.6f;
            LogInfo("survival: the shotgun's loading stopped to fire ({} in it)", state.clip);
            return;
        }
        reload_timer_ -= dt / survival_sounds::kReloadPace;
        if (reload_timer_ <= 0.0f) FinishReloadStep();
        if (reload_timer_ > 0.0f) return;
    }
    const bool can_reload = weapon_ != SurvivalWeapon::Flashlight && reload_timer_ <= 0.0f && state.clip < spec.clip_size && state.reserve > 0 &&
                            swap_ < 0.3f && melee_timer_ < 0.0f && throw_timer_ < 0.0f;
    if (can_reload && ((pressed & kSurvivalReload) != 0 || ((pressed & kSurvivalFire) != 0 && state.clip == 0))) {
        StartReload();
        return;
    }
    fire_buffer_ = (pressed & kSurvivalFire) != 0 ? std::max(fire_buffer_, 0.12f) : std::max(0.0f, fire_buffer_ - dt);
    if (weapon_ == SurvivalWeapon::Magnum && gun_.trigger >= 0.0f) {
        if (melee_timer_ >= 0.0f || throw_timer_ >= 0.0f || swap_ >= 0.25f) {
            gun_.trigger = -1.0f;
            return;
        }
        gun_.trigger += dt;
        if (gun_.trigger >= 0.07f) Fire(camera);
        return;
    }
    if (fire_buffer_ > 0.0f && reload_timer_ <= 0.0f && shot_timer_ <= 0.0f && swap_ < 0.25f && sprint_weight < 0.4f && melee_timer_ < 0.0f &&
        throw_timer_ < 0.0f) {
        fire_buffer_ = 0.0f;
        if (weapon_ == SurvivalWeapon::Flashlight) {
            FlashBurst(camera);
        } else if (weapon_ == SurvivalWeapon::Magnum && state.clip > 0) {
            gun_.trigger = 0.0f;
        } else {
            Fire(camera);
        }
    }
}

void SurvivalNight::UpdateGun(float dt) {
    GunAnim& g = gun_;
    g.since_shot += dt;
    const glm::mat4 gun = WeaponTransform(game_.ViewCamera(true));
    const SurvivalGunModel& model = models_.guns[static_cast<size_t>(weapon_)];
    const bool reloading = reload_timer_ > 0.0f;
    const float t = reloading ? reload_length_ - reload_timer_ : 0.0f;
    switch (weapon_) {
    case SurvivalWeapon::Handgun: {
        if (!g.slide_locked) g.slide = std::max(0.0f, g.slide - dt / 0.06f);
        if (g.since_shot < 0.025f) g.slide = 1.0f;
        if (reloading) {
            g.reload_at = t / reload_length_;
            g.tilt = 1.0f;
            ReloadPose pose;
            const bool home = ReloadNow(pose) && pose.seat > 0.98f;
            g.magazine = home ? 0.0f : t < 0.18f ? Smooth((t - 0.02f) / 0.12f) : 1.0f;
            if (!g.magazine_dropped && t > 0.1f) {
                g.magazine_dropped = true;
                Eject(Debris::Kind::Magazine, gun, glm::vec3(0.0f, -0.05f, -0.03f));
            }
            if (reload_from_empty_ && t >= 1.1f) {
                g.slide_locked = false;
                g.slide = std::max(0.0f, 1.0f - (t - 1.1f) / 0.025f);
            }
        } else {
            g.magazine = 0.0f;
            g.tilt = std::max(0.0f, g.tilt - dt * 4.0f);
        }
        break;
    }
    case SurvivalWeapon::Shotgun: {
        if (g.pumping) {
            const float before = g.pump_timer;
            g.pump_timer += dt;
            if (before < 0.0f && g.pump_timer >= 0.0f) PlaySound(pump_sound_, nullptr);
            const float p = g.pump_timer;
            if (p >= 0.0f) {
                g.slide = p < 0.12f ? Smooth(p / 0.12f) : 1.0f - Smooth((p - 0.2f) / 0.1f);
                if (g.shell_pending && p >= 0.12f) {
                    g.shell_pending = false;
                    Eject(Debris::Kind::Shell, gun, model.ejection);
                }
                if (p > 0.32f) {
                    g.slide = 0.0f;
                    g.pumping = false;
                }
            }
        }
        if (reloading) {
            g.tilt = std::min(1.0f, g.tilt + dt * 3.5f);
            g.reload_cut = false;
        } else {
            g.tilt = std::max(0.0f, g.tilt - dt * (g.reload_cut ? 7.0f : 3.5f));
        }
        g.lift = ShellStep();
        if (reloading) g.reload_at = std::max(0.0f, g.lift);
        break;
    }
    case SurvivalWeapon::Magnum: {
        g.cylinder += (g.cylinder_to - g.cylinder) * std::min(1.0f, dt * 22.0f);
        g.hammer = g.trigger >= 0.0f ? Smooth(g.trigger / 0.07f) : std::max(0.0f, g.hammer - dt / 0.02f);
        if (reloading) {
            g.reload_at = t / reload_length_;
            g.tilt = 1.0f;
            if (!g.cases_out && g.reload_at > model.reload.eject) {
                g.cases_out = true;
                const int spent = Spec(weapon_).clip_size - weapons_[static_cast<size_t>(weapon_)].clip;
                for (int i = 0; i < spent; ++i) {
                    const float a = 2.0f * kPi * static_cast<float>(i) / 6.0f;
                    const glm::vec3 at = model.cylinder_axis + model.cylinder_swing * g.swing + glm::vec3(std::cos(a) * 0.0122f, std::sin(a) * 0.0122f, -0.016f);
                    Eject(Debris::Kind::Casing, gun, at);
                    debris_.back().velocity = glm::mat3(gun) * glm::vec3(0.25f + 0.2f * Rand(), 0.15f * (Rand() - 0.5f), -0.35f - 0.3f * Rand());
                    debris_.back().spin *= 0.25f;
                }
            }
        } else {
            g.tilt = std::max(0.0f, g.tilt - dt * 4.0f);
        }
        ReloadPose pose;
        g.swing = ReloadNow(pose) ? pose.open : 0.0f;
        break;
    }
    case SurvivalWeapon::Lever: {
        if (g.pumping) {
            const float before = g.pump_timer;
            g.pump_timer += dt;
            if (before < 0.0f && g.pump_timer >= 0.0f) PlaySound(lever_sound_, nullptr);
            const float p = g.pump_timer;
            if (p >= 0.0f) {
                g.lever = p < 0.16f ? Smooth(p / 0.16f) : 1.0f - Smooth((p - 0.24f) / 0.14f);
                if (g.lever > 0.6f) g.hammer_fall = 0.0f;
                if (g.shell_pending && p >= 0.16f) {
                    g.shell_pending = false;
                    Eject(Debris::Kind::Casing, gun, model.ejection);
                    debris_.back().velocity = glm::mat3(gun) * glm::vec3(0.4f * (Rand() - 0.5f), 2.6f + Rand(), -0.6f);
                }
                if (p > 0.4f) {
                    g.lever = 0.0f;
                    g.pumping = false;
                }
            }
        }
        if (reloading) {
            g.tilt = std::min(1.0f, g.tilt + dt * 3.5f);
            g.reload_cut = false;
        } else {
            g.tilt = std::max(0.0f, g.tilt - dt * (g.reload_cut ? 7.0f : 3.5f));
        }
        g.lift = ShellStep();
        if (reloading) g.reload_at = std::max(0.0f, g.lift);
        break;
    }
    case SurvivalWeapon::Double: {
        if (reloading) {
            g.reload_at = t / reload_length_;
            g.tilt = 1.0f;
            if (!g.cases_out && g.reload_at > model.reload.eject) {
                g.cases_out = true;
                const int spent = Spec(weapon_).clip_size - weapons_[static_cast<size_t>(weapon_)].clip;
                const glm::mat4 barrels = gun * BarrelsTurn();
                for (int i = 0; i < spent; ++i) {
                    const float side = (i == 0 ? 1.0f : -1.0f) * model.barrel_side;
                    Eject(Debris::Kind::Shell, barrels, model.ejection + glm::vec3(side, 0.0f, 0.01f));
                    debris_.back().velocity = glm::mat3(gun) * glm::vec3(0.3f * (Rand() - 0.5f), 1.4f + 0.6f * Rand(), -2.2f - Rand());
                }
            }
        } else {
            g.tilt = std::max(0.0f, g.tilt - dt * 4.0f);
        }
        ReloadPose pose;
        g.open = ReloadNow(pose) ? pose.open : 0.0f;
        break;
    }
    default: break;
    }
    const float heavy = weapon_ == SurvivalWeapon::Shotgun || weapon_ == SurvivalWeapon::Double ? 1.0f
                        : weapon_ == SurvivalWeapon::Lever                                      ? 0.9f
                        : weapon_ == SurvivalWeapon::Magnum                                     ? 0.8f
                                                                                                : 0.0f;
    Spring(view_kick_, view_kick_speed_, glm::mix(170.0f, 95.0f, heavy), glm::mix(21.0f, 15.5f, heavy), dt);
    Spring(gun_kick_, gun_kick_speed_, glm::mix(300.0f, 120.0f, heavy), glm::mix(29.0f, 18.0f, heavy), dt);
}

void SurvivalNight::AimChestLamp(float dt) {
    if (glm::mat4 chest(1.0f); ChestLight(chest)) {
        const Camera own = game_.GetPlayer().MakeCamera();
        RayHit hit;
        const float reach = game_.Collision().Raycast(own.position, own.Forward(), 20.0f, hit) ? std::max(hit.distance, 3.0f) : 20.0f;
        beam_reach_ = beam_reach_ > 0.0f ? beam_reach_ + (reach - beam_reach_) * (1.0f - std::exp(-std::max(dt, 0.0f) * 4.0f)) : reach;
        const glm::vec3 lens(chest * glm::vec4(models_.chest_lens, 1.0f));
        const glm::vec3 beam_dir = glm::normalize(own.position + own.Forward() * beam_reach_ - lens);
        float start = 0.8f * game_.ThirdPersonBodyShare();
        if (start > 0.0f) {
            RayHit wall;
            if (game_.Collision().Raycast(lens, beam_dir, start + 0.35f, wall)) start = std::min(start, wall.distance - 0.35f);
            glm::vec3 flat(beam_dir.x, 0.0f, beam_dir.z);
            flat = glm::length(flat) > 1.0e-3f ? glm::normalize(flat) : glm::vec3(0.0f, 0.0f, 1.0f);
            for (const Enemy& e : enemies_) {
                if (e.state == Enemy::State::Dying) continue;
                glm::vec3 to = e.position - lens;
                to.y = 0.0f;
                const float along = glm::dot(to, flat);
                const float size = KindScale(e.kind);
                if (along > 0.0f && glm::length(to - flat * along) < 0.5f + 0.3f * size) start = std::min(start, along - 0.3f * size - 0.35f);
            }
            start = std::max(start, 0.0f);
        }
        beam_start_ = dt > 0.0f ? beam_start_ + (start - beam_start_) * (1.0f - std::exp(-dt * 14.0f)) : start;
        game_.SetHandyPoseOverride(std::pair(lens + beam_dir * beam_start_, beam_dir));
        if (static const bool log = std::getenv("PT_SURVIVAL_POSE_LOG") != nullptr; log && game_.Frame() % 60 == 0) {
            const glm::vec3 beam = glm::normalize(own.position + own.Forward() * beam_reach_ - lens);
            LogInfo("survival: chest light at ({:.2f} {:.2f} {:.2f}) beam ({:.3f} {:.3f} {:.3f}) reach {:.2f} (view ray {:.2f}), eye ({:.2f} {:.2f} {:.2f}) forward ({:.3f} {:.3f} {:.3f}), "
                    "beam starts {:.2f} m ahead (wanted {:.2f})",
                    lens.x, lens.y, lens.z, beam.x, beam.y, beam.z, beam_reach_, reach, own.position.x, own.position.y, own.position.z, own.Forward().x,
                    own.Forward().y, own.Forward().z, beam_start_, start);
        }
    }
}

void SurvivalNight::Update(float dt, const InputState& input, const Camera& camera) {
    if (!active_) return;
    time_ += dt;
    if (caught_) {
        if (game_.Demos().IsPlaying(kCatchDemo)) return;
        caught_ = false;
        hud_.caught = false;
        game_.GetPlayer().Warp(caught_at_, caught_yaw_);
        for (auto it = enemies_.begin(); it != enemies_.end(); ++it) {
            if (it->id != caught_by_) continue;
            if (!caught_lethal_ && it->boss) {
                Blink(*it);
                it->strike_cooldown = 2.5f;
                break;
            }
            game_.Demos().Gimmicks().ReleaseActor(it->id);
            slot_used_[it->slot] = false;
            slot_free_at_[it->slot] = time_ + 1.0f;
            enemies_.erase(it);
            break;
        }
        if (caught_lethal_) {
            game_.Effects().CallFadeOut(0.0f);
            Die(caught_cause_, true);
            dead_fall_ = 1.0f;
        } else {
            health_ = caught_damage_ > 0.0f ? std::max(18.0f, health_ - caught_damage_) : std::max(18.0f, max_health_ * 0.20f);
            caught_damage_ = 0.0f;
            game_.GetPlayer().pitch = 0.0f;
            game_.GetPlayer().target_pitch = 0.0f;
            hud_.hurt = 1.0f;
            shake_ = 0.7f;
            Frighten(0.35f);
            game_.Effects().CallFadeIn(0.85f);
            Play(hurt_, nullptr);
            for (Enemy& other : enemies_) {
                if (other.state == Enemy::State::Dying) continue;
                glm::vec3 away = other.position - caught_at_;
                away.y = 0.0f;
                const float d = glm::length(away);
                if (d < 5.5f) {
                    const glm::vec3 dir = d > 1e-3f ? away / d : glm::vec3(std::sin(caught_yaw_), 0.0f, std::cos(caught_yaw_));
                    const float push = 5.5f - d;
                    if (!Blocked(other.position, dir, push)) {
                        other.position.x = std::clamp(other.position.x + dir.x * push, kArenaMin.x, kArenaMax.x);
                        other.position.z = std::clamp(other.position.z + dir.z * push, kArenaMin.y, north_end_);
                        other.position.y = GroundAt(other.position, other.position.y);
                    }
                    other.stagger = std::max(other.stagger, 2.0f);
                    other.strike_cooldown = std::max(other.strike_cooldown, 2.0f);
                }
            }
            LogInfo("survival: survived Lisa's catch on wave {} ({:.0f} hp left)", wave_, health_);
        }
    }
    if (waking_) {
        wake_third_ = false;
        game_.Demos().SetDemoThirdPersonBody(kWakeDemo, false);
        const double frame = game_.Demos().PlayFrame(kWakeDemo);
        if (!wake_light_on_ && frame >= 740.0) {
            wake_light_on_ = true;
            if (!game_.GetPlayer().handy_light.enable) {
                game_.GetPlayer().handy_light.enable = true;
                PostEvent("Play_sfx_light_on_01", nullptr);
            }
        }
        AimChestLamp(dt);
        if (game_.Demos().IsPlaying(kWakeDemo)) return;
        waking_ = false;
        wake_third_ = false;
        hud_.caught = false;
        game_.Demos().ResetPlayerBody();
        game_.GetPlayer().pitch = 0.0f;
        game_.GetPlayer().target_pitch = 0.0f;
        if (wake_locked_) {
            game_.GetPlayer().locks.Remove('B', "SurvivalWake");
            wake_locked_ = false;
        }
        if (!game_.GetPlayer().handy_light.enable) {
            game_.GetPlayer().handy_light.enable = true;
            PostEvent("Play_sfx_light_on_01", nullptr);
        }
        swap_ = 0.85f;
        StartCountdown(4.0f);
        if (!wake_banner_key_.empty()) Banner(wake_banner_key_, wake_banner_value_, 3.0f);
        LogInfo("survival: wake-up complete, countdown started");
    }
    hit_pause_ = std::max(0.0f, hit_pause_ - dt);
    hud_.banner = std::max(0.0f, hud_.banner - dt);
    hud_.omen = std::max(0.0f, hud_.omen - dt);
    hud_.pickup_note = std::max(0.0f, hud_.pickup_note - dt);
    hud_.hurt = std::max(0.0f, hud_.hurt - dt * 1.5f);
    hud_.heal = std::max(0.0f, hud_.heal - dt * 0.85f);
    chain_timer_ = std::max(0.0f, chain_timer_ - dt);
    hud_.hurt_arc = std::max(0.0f, hud_.hurt_arc - dt * 0.8f);
    static const bool flash_hold = std::getenv("PT_SURVIVAL_FLASH_HOLD") != nullptr;
    muzzle_ = flash_hold && muzzle_ > 0.0f ? muzzle_ : std::max(0.0f, muzzle_ - dt);
    burst_light_ = std::max(0.0f, burst_light_ - dt);
    AimChestLamp(dt);
    UpdatePlayer(dt, input, camera);
    if (!active_) return;
    UpdateGun(dt);
    if (!hud_.dead) {
        UpdateMelee(dt, camera);
        UpdateThrow(dt, camera);
        UpdateFlashClick(dt);
    }
    UpdateFlares(dt);
    UpdateTask(dt, held_);
    UpdateSupply();
    UpdateDark(dt);
    UpdateNotices(dt);
    UpdateFear(dt, camera);
    if (phase_ == SurvivalPhase::Wave) {
        wave_time_ += dt;
        if (blackout_at_ >= 0.0f && blackout_time_ < 0.0f && wave_time_ >= blackout_at_) {
            const float triggered_at = blackout_at_;
            blackout_at_ = -1.0f;
            blackout_time_ = 0.0f;
            LogInfo("survival: the street's lights go out on wave {} for {:.0f} s", wave_, blackout_length_);
            if (flares_ < MaxFlares() && !hud_.dead) {
                const Player& player = game_.GetPlayer();
                Drop(player.Feet() + glm::vec3(std::sin(player.FoxYaw()), 0.0f, std::cos(player.FoxYaw())) * 1.6f, SurvivalPickupKind::Flare,
                     SurvivalWeapon::Handgun);
            }
            if (triggered_at > 0.0f || !(events_ & ~kEventBlackout)) Omen("pc_survival_omen_blackout", 3.5f);
        }
    }
    UpdateLamps(dt);
    UpdateMidnight(dt);
    UpdateVoice(dt);
    if (blackout_time_ >= 0.0f) {
        const float before = blackout_time_;
        if (phase_ == SurvivalPhase::Break && midnight_ < 0.0f) blackout_time_ = std::max(blackout_time_, blackout_length_ + 1.2f);
        blackout_time_ += dt;
        if (before <= 0.0f && blackout_time_ > 0.0f) Play(lights_fail_, nullptr);
        if (before < 0.9f && blackout_time_ >= 0.9f && blackout_time_ < blackout_length_) Play(lights_down_, nullptr);
        if (before < blackout_length_ + 1.2f && blackout_time_ >= blackout_length_ + 1.2f) Play(lights_on_, nullptr);
        if (blackout_time_ > blackout_length_ + 2.7f) blackout_time_ = -1.0f;
    }
    const bool wave_on = phase_ == SurvivalPhase::Wave;
    auto ease = [dt](float& level, bool on, float in, float out) {
        level = on ? std::min(1.0f, level + dt / in) : std::max(0.0f, level - dt / out);
    };
    ease(fog_level_, wave_on && (events_ & kEventFog), 6.0f, 5.0f);
    UpdateLaterNights(dt);
    ease(siren_level_, wave_on && (events_ & kEventSiren) && wave_time_ > 3.0f, 6.0f, 4.0f);
    {
        const bool hell_on = wave_on && (events_ & kEventHell) && wave_time_ > 1.5f;
        const float to = hell_on ? (task_.kind == SurvivalMission::Lamps ? LanternShare() : 1.0f) : 0.0f;
        hell_level_ = hell_level_ < to ? std::min(to, hell_level_ + dt / 8.0f) : std::max(to, hell_level_ - dt / 5.0f);
    }
    DimStreetGlow(std::min(BlackoutLevel(), GlowLevel()), HellTint());
    if (!hud_.dead) {
        run_time_ += dt;
        UpdateTolls(dt);
        if (phase_ == SurvivalPhase::Countdown || phase_ == SurvivalPhase::Break) {
            if (offer_count_ == 0) countdown_ -= dt;
            const int second = static_cast<int>(std::ceil(countdown_));
            if (second != last_tick_ && second >= 0) {
                last_tick_ = second;
                if (second <= 5 && second >= 1) {
                    if (!PostEvent(kTickEvent, nullptr)) PlaySound(tick_sound_, nullptr);
                }
            }
            if (countdown_ <= 0.0f) StartWave();
        } else if (phase_ == SurvivalPhase::Wave) {
            UpdateDirector(dt);
            UpdateBosses(dt);
            spawn_timer_ -= dt * (relief_ > 0.0f ? 0.4f : 1.0f);
            if (!quiet_ && to_spawn_ > 0 && spawn_timer_ <= 0.0f && AliveEnemies() < wave_cap_) {
                const bool lisa_was_due = lisa_due_;
                const int before = to_spawn_;
                plan_rng_ = PlanSeed(wave_, 0x100u + static_cast<uint32_t>(spawned_this_wave_));
                SpawnEnemy(PickKind());
                if (to_spawn_ == before) {
                    lisa_due_ = lisa_was_due;
                    spawn_timer_ = 0.5f;
                } else {
                    spawn_timer_ = spawn_interval_ * (surge_ > 0 ? 0.3f : 1.0f);
                    if (surge_ > 0) --surge_;
                }
            }
            if (to_spawn_ <= 0 && AliveEnemies() == 0 && !boss_due_ && splits_.empty() && SurvivalNightEnds(wave_)) {
                Midnight();
            } else if (to_spawn_ <= 0 && AliveEnemies() == 0 && !boss_due_ && splits_.empty()) {
                phase_ = SurvivalPhase::Break;
                StartCountdown(12.0f);
                Banner("pc_survival_cleared", SurvivalWaveInNight(wave_), 3.5f);
                const bool clean = wave_hurt_ <= 0.0f;
                const int bonus = 100 * wave_ * (clean ? 2 : 1);
                score_ += bonus;
                wave_hurt_ = 0.0f;
                LogInfo("survival: wave {} cleared{} (+{}, score {})", wave_, clean ? " untouched" : "", bonus, score_);
                if (wave_ == 25) Unlock(kUnlockBlued);
                if (wave_ == 50) Unlock(kUnlockAmber);
                if (wave_ == 75) Unlock(kUnlockNickel);
                if (start_ == SurvivalStart::Daily && wave_ == 10) {
                    SurvivalProfile& prof = game_.SurvivalProfileData();
                    const int today = SurvivalToday();
                    if (prof.daily_last_win_date != today) {
                        daily_goal_fresh_ = true;
                        prof.daily_streak = (prof.daily_last_win_date == SurvivalYesterday(today)) ? (prof.daily_streak + 1) : 1;
                        prof.daily_last_win_date = today;
                        ++prof.daily_wins;
                        if (prof.daily_wins >= 1) prof.GrantSkin(40);
                        if (prof.daily_wins >= 2) prof.GrantTorch(3);
                        if (prof.daily_wins >= 3) prof.GrantSkin(41);
                        if (prof.daily_wins >= 5) prof.GrantTorch(4);
                        if (prof.daily_wins >= 7) prof.GrantSkin(54);
                        game_.MarkSurvivalProfile();
                        Note("pc_survival_daily_cleared");
                        LogInfo("survival: daily goal cleared (wins {}, streak {})", prof.daily_wins, prof.daily_streak);
                    }
                }
                if (task_.kind == SurvivalMission::Radio && task_.tuned && !task_.done) {
                    FinishTask();
                }
                const bool task_offer = task_.offer_due || (start_ == SurvivalStart::Daily && DailyMutatorById(daily_mutator_).lisa_stalks && (wave_ % 2 == 0));
                EndTask();
                Frighten(-0.3f);
                if (plan_.checkpoint) SaveCheckpoint();
                if (plan_.boon || task_offer) OfferBoons();
                Toll(1);
                LogInfo("survival: wave {} cleared ({} kills)", wave_, kills_);
                const Player& player = game_.GetPlayer();
                const glm::vec3 feet = player.Feet();
                const glm::vec3 ahead = feet + glm::vec3(std::sin(player.FoxYaw()), 0.0f, std::cos(player.FoxYaw())) * 2.5f;
                for (const auto& unlock : kSurvivalWeaponUnlocks) {
                    const auto w = static_cast<SurvivalWeapon>(unlock.weapon_index);
                    if (wave_ == unlock.after_wave && !weapons_[static_cast<size_t>(w)].owned) {
                        Drop(ahead, SurvivalPickupKind::Weapon, w);
                        Banner("pc_survival_weapon_near", 0, 3.5f);
                    }
                }
                if (wave_ == 6 && pipe_hits_ <= 0) {
                    Drop(ahead + glm::vec3(0.6f, 0.0f, 0.0f), SurvivalPickupKind::Pipe, SurvivalWeapon::Handgun);
                    Banner("pc_survival_pipe_near", 0, 3.5f);
                }
                Drop(feet + glm::vec3(1.5f, 0.0f, -1.0f), SurvivalPickupKind::Ammo, AmmoNeeded());
            }
        }
    }
    static const bool prof = std::getenv("PT_SURVIVAL_PROF") != nullptr;
    const auto enemies_from = std::chrono::steady_clock::now();
    UpdateEnemies(dt);
    if (prof) {
        static double total = 0.0, worst = 0.0;
        static int ticks = 0, most = 0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - enemies_from).count();
        total += ms;
        worst = std::max(worst, ms);
        most = std::max(most, static_cast<int>(enemies_.size()));
        if (++ticks >= 300) {
            LogInfo("survival prof: enemies {:.3f} ms a tick ({:.3f} worst) with up to {} of them", total / ticks, worst, most);
            total = worst = 0.0;
            ticks = most = 0;
        }
    }
    UpdatePickups(dt);
    UpdateEffects(dt);

    const float health_share = std::clamp(health_ / max_health_, 0.0f, 1.0f);
    hud_.low_health = hud_.dead ? 1.0f : std::clamp((0.4f - health_share) / 0.4f, 0.0f, 1.0f);
    hud_.heart_rate = hud_.dead ? 0.0f : glm::mix(150.0f, 68.0f, Smooth(health_share)) + 25.0f * RunWeight();
    heart_clock_ += dt;
    heart_phase_ += dt * hud_.heart_rate / 60.0f;
    if (heart_phase_ >= 1.0f) {
        heart_phase_ -= std::floor(heart_phase_);
        hud_.last_beat = run_time_;
        std::rotate(hud_.beats.begin(), hud_.beats.begin() + 1, hud_.beats.end());
        hud_.beats.back() = run_time_;
    }
    if (game_.Audio()) {
        const bool low = !hud_.dead && hud_.low_health > 0.4f;
        if (low && !low_heartbeat_) low_heartbeat_ = PostEvent(kHeartbeatEvent, nullptr);
        if (!low && low_heartbeat_ && !hud_.dead) {
            game_.Audio()->StopPlayingId(low_heartbeat_, 1.5f);
            low_heartbeat_ = 0;
        }
    }
    ammo_idle_ += dt;
    since_swap_ += dt;
    if (gun_.since_shot < dt * 1.5f || reload_timer_ > 0.0f || swap_ > 0.0f || hud_.pickup_note > 2.4f) ammo_idle_ = 0.0f;
    const WeaponState& shown = weapons_[static_cast<size_t>(weapon_)];
    const bool running_low = weapon_ != SurvivalWeapon::Flashlight && shown.clip <= std::max(1, Spec(weapon_).clip_size / 4);
    hud_.ammo_shown = running_low ? 1.0f : 1.0f - 0.72f * Smooth((ammo_idle_ - 3.0f) / 1.5f);
    hud_.weapon_name_shown = 1.0f - Smooth((since_swap_ - 1.8f) / 0.8f);

    const WeaponState& state = weapons_[static_cast<size_t>(weapon_)];
    hud_.phase = phase_;
    hud_.wave = wave_;
    hud_.countdown = countdown_;
    hud_.enemies_left = to_spawn_ + AliveEnemies();
    hud_.health = health_ / max_health_;
    hud_.stamina = stamina_;
    hud_.winded = winded_;
    hud_.weapon = weapon_;
    for (size_t i = 0; i < hud_.owned.size(); ++i) hud_.owned[i] = weapons_[i].owned;
    hud_.clip = state.clip;
    hud_.clip_size = Spec(weapon_).clip_size;
    hud_.reserve = state.reserve;
    hud_.reloading = reload_timer_ > 0.0f;
    hud_.pipe = pipe_hits_ > 0 ? std::min(static_cast<float>(pipe_hits_) / static_cast<float>(std::max(1, pipe_hits_full_)), pipe_time_ / std::max(1.0f, pipe_time_full_)) : -1.0f;
    hud_.flares = flares_;
    hud_.reload_progress = hud_.reloading && reload_length_ > 0.0f ? 1.0f - reload_timer_ / reload_length_ : 0.0f;
    hud_.offer_age += dt;
    hud_.taken_age += dt;
    hud_.kept = std::max(0.0f, hud_.kept - dt);
    if (offer_count_ == 0 && hud_.offer_taken < 0) hud_.offer_count = 0;
    if (hud_.offer_taken >= 0 && hud_.taken_age > 1.2f) {
        hud_.offer_count = 0;
        hud_.offer_taken = -1;
    }
    if (boons_[SurvivalBoon::LampsGrace] > 0 && !hud_.dead && NearLitLamp(game_.GetPlayer().Feet(), 4.5f)) {
        health_ = std::min(max_health_, health_ + dt);
    }
    hud_.run_seconds = run_time_;
    hud_.burst_ready = 1.0f - burst_timer_ / Gun(SurvivalWeapon::Flashlight).interval;
    hud_.aim = aim_;
    hud_.sights = sights_w_;
    hud_.first_person = !game_.ThirdPersonCameraActive() || !game_.ThirdPersonShown();
    hud_.spread = Spread();
    hud_.lisa_health = hud_.giant_health = -1.0f;
    float giant_now = 0.0f, giant_max = 0.0f;
    for (const Enemy& e : enemies_) {
        if (e.state == Enemy::State::Dying) continue;
        if (e.kind == SurvivalEnemyKind::Lisa) hud_.lisa_health = std::max(0.0f, e.health / e.max_health);
        if (e.kind == SurvivalEnemyKind::Giant) {
            giant_now += std::max(0.0f, e.health);
            giant_max += e.max_health;
        }
    }
    if (giant_max > 0.0f) hud_.giant_health = giant_now / giant_max;
    const Camera view = game_.ViewCamera(true);
    hud_.view = view.View();
    hud_.fov_y = view.fov_y;
    hud_.eye = view.position;
}

}
