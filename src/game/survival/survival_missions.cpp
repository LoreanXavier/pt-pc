#include <algorithm>
#include <cmath>
#include <format>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/log.h"
#include "engine/fs/vfs.h"
#include "engine/physics/collision_world.h"
#include "engine/platform/input.h"
#include "engine/render/camera.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_lighting.h"
#include "game/game.h"
#include "game/player.h"
#include "game/survival/survival.h"

namespace pt::game {

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint64_t kTaskSource = 0x5355525600000000ull + 0x70000u;

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

constexpr float kFuseHold = 2.5f;
constexpr float kRadioHold = 1.5f;
constexpr float kDoorHold = 3.0f;
constexpr float kUseReach = 1.8f;
constexpr int kPieces = 4;
constexpr int kLanterns = 3;
constexpr float kRadioReach = 4.5f;
constexpr float kRadioSpeech = 15.0f;
constexpr const char* kVoices[] = {"Play_radio_voice_01", "Play_radio_voice_03", "Play_radio_voice_04", "Play_radio_voice_06",
                                   "Play_radio_voice_07", "Play_radio_voice_08"};

glm::vec2 Flat(const glm::vec3& v) {
    return {v.x, v.z};
}

constexpr const char* kHallwayPackage = "/Assets/sh/level/promotion/pt_2014/hallway/pt14_hallway.fpk";
constexpr const char* kMeterModel = "/Assets/sh/environ/object/shsb/utility_pole/shsb_utpl001/scenes/shsb_utpl001.fmdl";
constexpr const char* kRadioModel = "/Assets/sh/environ/object/shsb/radio/shsb_radi001/scenes/shsb_radi001.fmdl";
constexpr const char* kCartonModel = "/Assets/sh/environ/object/shsb/carton/shsb_crto001/scenes/shsb_crto001_crto002.fmdl";
constexpr const char* kLampLitModel = "/Assets/sh/environ/object/shsb/light/shsb_lght003/scenes/shsb_lght003_vrtn002.fmdl";
constexpr const char* kLampOutModel = "/Assets/sh/environ/object/shsb/light/shsb_lght003/scenes/shsb_lght003.fmdl";
constexpr const char* kDoorModel = "/Assets/sh/environ/object/shsb/house/shsb_hous001/scenes/shsb_hous001_door001.fmdl";
constexpr float kCartonTop = 0.48f;
constexpr float kLampGlass = 0.56f;
constexpr float kLampLying = 0.1f;
constexpr glm::vec3 kLampPosts[] = {{7.89f, -0.14f, 26.57f}, {7.89f, -0.14f, 53.68f},  {7.89f, -0.14f, 99.73f},   {7.89f, -0.14f, 143.88f},
                                    {-12.49f, -0.14f, 62.7f}, {-12.49f, -0.14f, 109.73f}, {-12.49f, -0.14f, 164.76f}};
constexpr float kPostHalf = 0.107f;

}

void SurvivalNight::LoadTaskModels() {
    if (task_models_tried_) return;
    task_models_tried_ = true;
    Vfs& vfs = game_.GetVfs();
    ModelCache& models = game_.Models();
    const bool mounted_here = !vfs.ReadFile(kRadioModel) && vfs.LoadPackage(kHallwayPackage) != nullptr;
    auto get = [&](const std::string& path) -> const ModelEntry* { return vfs.ReadFile(path) ? models.Get(path) : nullptr; };
    auto mesh = [](const ModelEntry* e) -> const GpuMesh* { return e ? e->mesh.get() : nullptr; };
    if (vfs.ReadFile(kMeterModel)) {
        task_models_.fuse_box = mesh(models.GetSubset(
            kMeterModel, std::string(kMeterModel) + "#survival_fuse_box", [](const Vertex& v) { return v.position.x < -0.085f && v.position.y < 1.12f; },
            [](Vertex& v) { v.position += glm::vec3(0.3514f, -0.657f, -0.0025f); }));
    }
    task_models_.radio = mesh(get(kRadioModel));
    task_models_.carton = mesh(get(kCartonModel));
    for (size_t i = 0; i < task_models_.pieces.size(); ++i) {
        task_models_.pieces[i] = mesh(get(std::format("/Assets/sh/environ/object/shsb/label/shsb_labl001/scenes/shsb_labl001_mapc{:03}.fmdl", i + 1)));
    }
    const ModelEntry* lamp = get(kLampLitModel);
    task_models_.lamp_lit = mesh(lamp);
    for (size_t k = 0; lamp && lamp->mesh && k < lamp->materials.size() && k < lamp->material_indices.size(); ++k) {
        if (lamp->materials[k].technique.find("Constant") == std::string::npos) continue;
        for (size_t s = 0; s < lamp->mesh->submeshes.size(); ++s) {
            if (lamp->mesh->submeshes[s].material == lamp->material_indices[k]) task_models_.lamp_globe = s;
        }
        if (const MaterialGpu* own = models.Textures().Material(lamp->material_indices[k])) {
            MaterialGpu red = *own;
            red.albedo_factor = glm::vec4(1.0f, 0.09f, 0.06f, 1.0f);
            red.params.y = 700.0f;
            task_models_.lamp_red = static_cast<int32_t>(models.Textures().AddMaterial(red));
        }
    }
    task_models_.lamp_out = mesh(get(kLampOutModel));
    const ModelEntry* door = get(kDoorModel);
    task_models_.door = mesh(door);
    for (size_t i = 0; door && i < door->materials.size() && i < door->material_indices.size(); ++i) {
        if (door->materials[i].name.starts_with("wood")) task_models_.door_wood = static_cast<int32_t>(door->material_indices[i]);
    }
    if (mounted_here) vfs.UnloadPackage(kHallwayPackage);
    const auto pieces = std::count_if(task_models_.pieces.begin(), task_models_.pieces.end(), [](const GpuMesh* p) { return p != nullptr; });
    LogInfo("survival: P.T.'s things for the tasks: the meter box {}, the radio {}, the carton {}, {} pieces, the lamps {}/{}, the door {} (wood {}){}",
            task_models_.fuse_box ? "yes" : "no", task_models_.radio ? "yes" : "no", task_models_.carton ? "yes" : "no", pieces,
            task_models_.lamp_lit ? "yes" : "no", task_models_.lamp_out ? "yes" : "no", task_models_.door ? "yes" : "no", task_models_.door_wood,
            mounted_here ? ", the hallway's package read for them" : "");
}

float SurvivalNight::HouseDoor() const {
    if (!active_) return -1.0f;
    if (task_.kind == SurvivalMission::Door && task_.at_house) return task_.open;
    return night_door_ > 0.0f ? night_door_ : -1.0f;
}

float SurvivalNight::RadioLift() const {
    return task_.kind == SurvivalMission::Radio && !task_.perched && task_models_.radio && task_models_.carton ? kCartonTop : 0.0f;
}

float SurvivalNight::LanternGlass() const {
    return task_models_.lamp_lit ? kLampGlass : 0.2f;
}

bool SurvivalNight::RoadSpot(glm::vec3& at, float closest, float farthest) {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    for (int attempt = 0; attempt < 32; ++attempt) {
        const float side = PlanRand() < 0.5f ? 1.0f : -1.0f;
        const float z = std::clamp(feet.z + side * (closest + (farthest - closest) * PlanRand()), 24.5f, std::min(187.0f, north_end_ - 2.0f));
        const float x = -6.5f + 10.5f * PlanRand();
        const std::span<const StreetBody> bodies = StreetBodies();
        if (std::any_of(bodies.begin(), bodies.end(), [&](const StreetBody& b) { return b.Holds({x, z}); })) continue;
        RayHit hit;
        if (!game_.Collision().Raycast(glm::vec3(x, feet.y + 3.0f, z), glm::vec3(0.0f, -1.0f, 0.0f), 6.0f, hit) || hit.normal.y < 0.8f) continue;
        if (std::abs(hit.point.y - feet.y) > 1.0f) continue;
        std::vector<SphereHit> touching;
        game_.Collision().SphereOverlap(hit.point + glm::vec3(0.0f, 0.7f, 0.0f), 0.5f, touching);
        if (!touching.empty()) continue;
        const float d = glm::distance(Flat(hit.point), Flat(feet));
        if (d < closest * 0.8f || d > farthest * 1.2f) continue;
        if (std::any_of(task_.spots.begin(), task_.spots.end(), [&](const glm::vec3& s) { return glm::distance(s, hit.point) < 4.0f; })) continue;
        at = hit.point;
        return true;
    }
    return false;
}

void SurvivalNight::Studio(int kind) {
    uint32_t pressed = 0;
    ScriptAction("quiet", pressed);
    studio_ = true;
    EndTask();
    task_ = Task{};
    task_.kind = static_cast<SurvivalMission>(std::clamp(kind, 0, 5));
    if (task_.kind == SurvivalMission::None) return;
    Player& player = game_.GetPlayer();
    player.pitch = player.target_pitch = 0.0f;
    const Camera view = game_.ViewCamera(true);
    glm::vec3 ahead = view.Forward();
    ahead.y = 0.0f;
    ahead = glm::length(ahead) > 1e-3f ? glm::normalize(ahead) : glm::vec3(0.0f, 0.0f, 1.0f);
    float reach = 1.0f, below = 0.0f;
    switch (task_.kind) {
    case SurvivalMission::Fuse: reach = 1.3f; break;
    case SurvivalMission::Radio:
        reach = 0.62f;
        below = 0.17f;
        task_.perched = true;
        break;
    case SurvivalMission::Lamps:
        reach = 1.15f;
        below = 0.36f;
        break;
    case SurvivalMission::Door:
        reach = 3.6f;
        below = 1.05f;
        break;
    default: break;
    }
    if (task_.kind == SurvivalMission::Door && game_.House()) {
        const glm::vec3 door(kHouseDoorX, GroundAt(glm::vec3(kHouseDoorX, view.position.y + 1.0f, kHouseFront + 0.6f), view.position.y - 1.6f), kHouseFront);
        const glm::vec3 feet = door + glm::vec3(0.7f, 0.0f, 3.3f);
        player.Warp(feet, std::atan2(door.x - feet.x, door.z - feet.z));
        const glm::vec3 to = glm::normalize(door + glm::vec3(0.0f, 1.05f, 0.0f) - (feet + glm::vec3(0.0f, 1.6f, 0.0f)));
        player.yaw = player.target_yaw = std::atan2(-to.x, -to.z);
        player.pitch = player.target_pitch = Player::ClampPitch(std::asin(std::clamp(to.y, -1.0f, 1.0f)));
        task_.spots.push_back(door);
        task_.facing = glm::vec3(0.0f, 0.0f, 1.0f);
        task_.at_house = true;
        task_.taken.assign(1, false);
        studio_lamp_ = feet + glm::vec3(0.22f, 1.68f, 0.0f);
        studio_reach_ = glm::distance(feet, door);
        LogInfo("survival: studio, the house's door {:.2f} m ahead", studio_reach_);
        return;
    }
    task_.spots.push_back(view.position + ahead * reach - glm::vec3(0.0f, below, 0.0f));
    task_.facing = -ahead;
    task_.taken.assign(task_.spots.size(), false);
    const glm::vec3 right = glm::normalize(glm::cross(ahead, glm::vec3(0.0f, 1.0f, 0.0f)));
    studio_lamp_ = view.position + right * 0.22f + glm::vec3(0.0f, 0.08f, 0.0f);
    studio_reach_ = reach;
    LogInfo("survival: studio, the task {}'s thing {:.2f} m ahead", kind, reach);
}

bool SurvivalNight::PostSpot(glm::vec3& at, glm::vec3& normal) {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    std::vector<const glm::vec3*> fit;
    const glm::vec3* nearest = nullptr;
    float nearest_d = 1e9f;
    for (const glm::vec3& post : kLampPosts) {
        if (post.z > north_end_ - 2.0f) continue;
        const float d = glm::distance(Flat(post), Flat(feet));
        if (d >= 14.0f && d <= 30.0f) fit.push_back(&post);
        if (d > 10.0f && d < nearest_d) {
            nearest_d = d;
            nearest = &post;
        }
    }
    const glm::vec3* post = !fit.empty() ? fit[std::min(fit.size() - 1, static_cast<size_t>(PlanRand() * static_cast<float>(fit.size())))] : nearest;
    if (!post) return false;
    normal = glm::vec3(post->x > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
    at = *post + normal * kPostHalf;
    at.y = GroundAt(at + normal * 0.6f + glm::vec3(0.0f, 1.0f, 0.0f), feet.y) + 1.35f;
    return true;
}

bool SurvivalNight::WallSpot(glm::vec3& at, glm::vec3& normal) {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    for (int attempt = 0; attempt < 40; ++attempt) {
        const float side = PlanRand() < 0.5f ? 1.0f : -1.0f;
        const float z = std::clamp(feet.z + side * (14.0f + 16.0f * PlanRand()), 24.5f, std::min(186.0f, north_end_ - 2.0f));
        const float across = PlanRand() < 0.5f ? 1.0f : -1.0f;
        const glm::vec3 from(-1.25f, feet.y + 1.35f, z);
        const glm::vec3 dir(across, 0.0f, 0.0f);
        RayHit hit;
        if (!game_.Collision().Raycast(from, dir, 16.0f, hit) || hit.distance < 5.0f) continue;
        if (std::abs(hit.normal.y) > 0.2f || glm::dot(hit.normal, -dir) < 0.75f) continue;
        RayHit a, b;
        const bool wide = game_.Collision().Raycast(from + glm::vec3(0.0f, 0.0f, 0.3f), dir, 16.0f, a) &&
                          game_.Collision().Raycast(from - glm::vec3(0.0f, 0.0f, 0.3f), dir, 16.0f, b) && std::abs(a.distance - hit.distance) < 0.08f &&
                          std::abs(b.distance - hit.distance) < 0.08f;
        if (!wide) continue;
        normal = glm::normalize(glm::vec3(hit.normal.x, 0.0f, hit.normal.z));
        at = hit.point + normal * 0.004f;
        return true;
    }
    return false;
}

void SurvivalNight::StartTask() {
    EndTask();
    task_ = Task{};
    task_.kind = plan_.mission;
    if (task_.kind == SurvivalMission::None) return;
    glm::vec3 at(0.0f), normal(0.0f, 0.0f, 1.0f);
    const glm::vec3 feet = game_.GetPlayer().Feet();
    switch (task_.kind) {
    case SurvivalMission::Fuse:
        if (!PostSpot(at, normal) && !WallSpot(at, normal)) {
            if (!RoadSpot(at, 14.0f, 28.0f)) at = feet + glm::vec3(0.0f, 0.0f, 18.0f);
            at.x = at.x > -1.25f ? 5.6f : -8.1f;
            normal = glm::vec3(at.x > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
            at.y = GroundAt(at + glm::vec3(0.0f, 1.0f, 0.0f), feet.y) + 1.35f;
        }
        task_.spots.push_back(at);
        task_.facing = normal;
        break;
    case SurvivalMission::Radio: {
        FindPerches();
        std::vector<glm::vec3> free;
        for (const glm::vec3& p : perches_) {
            const float d = glm::distance(Flat(p), Flat(feet));
            if (d > 10.0f && d < 24.0f) free.push_back(p);
        }
        if (!free.empty()) {
            at = free[std::min(free.size() - 1, static_cast<size_t>(PlanRand() * static_cast<float>(free.size())))];
        } else if (!RoadSpot(at, 10.0f, 24.0f)) {
            at = feet + glm::vec3(0.0f, 0.0f, 14.0f);
        }
        task_.perched = !free.empty();
        task_.spots.push_back(at);
        const glm::vec2 to = Flat(feet) - Flat(at);
        task_.facing = glm::length(to) > 1e-3f ? glm::vec3(to.x, 0.0f, to.y) / glm::length(to) : glm::vec3(0.0f, 0.0f, 1.0f);
        task_.noise = PostEvent("Play_radio_noise_lp", &task_.spots[0]);
        break;
    }
    case SurvivalMission::Fragments:
        for (int i = 0; i < kPieces; ++i) {
            if (RoadSpot(at, 7.0f + 3.0f * static_cast<float>(i), 13.0f + 3.0f * static_cast<float>(i))) task_.spots.push_back(at);
        }
        break;
    case SurvivalMission::Lamps:
        for (int i = 0; i < kLanterns; ++i) {
            if (RoadSpot(at, 8.0f, 26.0f)) task_.spots.push_back(at);
        }
        break;
    case SurvivalMission::Door: {
        if (game_.House()) {
            at = glm::vec3(kHouseDoorX, GroundAt(glm::vec3(kHouseDoorX, feet.y + 2.0f, kHouseFront + 0.6f), feet.y), kHouseFront);
            task_.spots.push_back(at);
            task_.facing = glm::vec3(0.0f, 0.0f, 1.0f);
            task_.at_house = true;
            task_.open = 0.0f;
            task_.door_wait = 2.5f;
            PostEvent("Play_sfx_door_open_01", &task_.spots[0]);
            break;
        }
        if (!RoadSpot(at, 16.0f, 26.0f)) at = feet + glm::vec3(0.0f, 0.0f, 20.0f);
        task_.spots.push_back(at);
        const glm::vec2 to = Flat(feet) - Flat(at);
        task_.facing = glm::length(to) > 1e-3f ? glm::vec3(to.x, 0.0f, to.y) / glm::length(to) : glm::vec3(0.0f, 0.0f, 1.0f);
        task_.door_wait = 2.0f;
        break;
    }
    default: break;
    }
    if (task_.spots.empty()) {
        LogWarn("survival: no place found for wave {}'s task", wave_);
        task_ = Task{};
        return;
    }
    task_.taken.assign(task_.spots.size(), false);
    static constexpr const char* kKeys[] = {"", "pc_survival_task_fuse", "pc_survival_task_radio", "pc_survival_task_fragments", "pc_survival_task_lamps",
                                            "pc_survival_task_door"};
    hud_.task_key = kKeys[static_cast<size_t>(task_.kind)];
    hud_.task_age = -2.6f;
    hud_.task_done = hud_.task_lost = false;
    LogInfo("survival: wave {}'s task {} ({} things, the first at {:.1f} {:.1f} {:.1f})", wave_, hud_.task_key, task_.spots.size(), task_.spots[0].x,
            task_.spots[0].y, task_.spots[0].z);
    Notice(hud_.task_key, "", 6.0f, VisionOf(hud_.task_key));
    notice_task_key_ = hud_.task_key;
    notice_task_count_ = 0;
    notice_task_over_ = false;
}

void SurvivalNight::EndTask() {
    if (game_.Audio()) {
        if (task_.noise) game_.Audio()->StopPlayingId(task_.noise, 0.5f);
        if (task_.voice) game_.Audio()->StopPlayingId(task_.voice, 1.0f);
    }
    task_.noise = task_.voice = 0;
    if (!task_.done && task_.kind != SurvivalMission::None) {
        hud_.task_lost = true;
        hud_.task_age = 0.0f;
        task_.lost = true;
    }
    task_.kind = SurvivalMission::None;
    task_.spots.clear();
    task_.taken.clear();
    hud_.task_marks.clear();
    hud_.use_key.clear();
    hud_.use_progress = 0.0f;
}

void SurvivalNight::FinishTask() {
    if (task_.done) return;
    task_.done = true;
    hud_.task_done = true;
    ++missions_done_;
    hud_.task_age = 0.0f;
    const glm::vec3 at = task_.spots.empty() ? game_.GetPlayer().Feet() : task_.spots[0];
    const glm::vec3 below(at.x, GroundAt(at + glm::vec3(0.0f, 0.5f, 0.0f), game_.GetPlayer().Feet().y), at.z);
    int points = 800 + 20 * wave_;
    switch (task_.kind) {
    case SurvivalMission::Fuse:
        blackout_length_ = std::max(0.0f, blackout_time_);
        Drop(below + task_.facing * 0.8f, SurvivalPickupKind::Ammo, AmmoNeeded());
        break;
    case SurvivalMission::Radio:
        Drop(below + glm::vec3(0.5f, 0.0f, 0.0f), SurvivalPickupKind::Ammo, AmmoNeeded());
        Drop(below - glm::vec3(0.5f, 0.0f, 0.0f), SurvivalPickupKind::Ammo, AmmoNeeded());
        Drop(below + glm::vec3(0.0f, 0.0f, 0.5f), SurvivalPickupKind::Health, SurvivalWeapon::Handgun);
        if (flares_ < MaxFlares()) Drop(below - glm::vec3(0.0f, 0.0f, 0.5f), SurvivalPickupKind::Flare, SurvivalWeapon::Handgun);
        task_.offer_due = true;
        break;
    case SurvivalMission::Fragments:
        task_.offer_due = true;
        points += 400;
        break;
    case SurvivalMission::Lamps: break;
    case SurvivalMission::Door:
        Drop(below + task_.facing * 1.2f, SurvivalPickupKind::Ammo, AmmoNeeded());
        break;
    default: break;
    }
    score_ += points;
    fear_ = std::max(0.0f, fear_ - 0.35f);
    Toll(1);
    LogInfo("survival: wave {}'s task done (+{})", wave_, points);
}

void SurvivalNight::UpdateTask(float dt, uint32_t held) {
    hud_.task_marks.clear();
    hud_.use_key.clear();
    if (hud_.task_age > -10.0f) hud_.task_age += dt;
    if (task_.kind == SurvivalMission::None) {
        hud_.use_progress = 0.0f;
        return;
    }
    task_.age += dt;
    const glm::vec3 feet = game_.GetPlayer().Feet();
    const bool using_it = (held & kSurvivalUse) != 0 && !hud_.dead && melee_timer_ < 0.0f && reload_timer_ <= 0.0f;
    auto hold_at = [&](const glm::vec3& spot, float need, const char* prompt) {
        const glm::vec3 foot(spot.x, feet.y, spot.z);
        if (glm::distance(foot, feet) > kUseReach + (task_.kind == SurvivalMission::Fuse ? 0.6f : 0.0f) || std::abs(spot.y - feet.y) > 2.6f) {
            task_.hold = std::max(0.0f, task_.hold - dt * 2.0f);
            return false;
        }
        hud_.use_key = prompt;
        task_.hold = using_it ? task_.hold + dt : std::max(0.0f, task_.hold - dt * 2.0f);
        hud_.use_progress = std::clamp(task_.hold / need, 0.0f, 1.0f);
        if (task_.hold >= need) {
            task_.hold = 0.0f;
            return true;
        }
        return false;
    };
    hud_.use_progress = 0.0f;
    if (task_.done) {
        hud_.task_count = hud_.task_need;
    } else {
        switch (task_.kind) {
        case SurvivalMission::Fuse: {
            const glm::vec3 box = task_.spots[0];
            hud_.task_marks.push_back(box + glm::vec3(0.0f, 0.4f, 0.0f));
            if (hold_at(box, kFuseHold, "pc_survival_use_fuse")) {
                Play(landing_, &task_.spots[0], 0.0f, 1.6f);
                FinishTask();
            }
            break;
        }
        case SurvivalMission::Radio: {
            const glm::vec3 radio = task_.spots[0];
            hud_.task_marks.push_back(radio + glm::vec3(0.0f, 0.5f + RadioLift(), 0.0f));
            if (!task_.tuned) {
                if (hold_at(radio, kRadioHold, "pc_survival_use_radio")) {
                    task_.tuned = true;
                    const char* voice = kVoices[static_cast<size_t>(night_seed_ + static_cast<uint32_t>(wave_)) % std::size(kVoices)];
                    task_.voice = PostEvent(voice, &task_.spots[0]);
                    surge_ = std::max(surge_, 6);
                    LogInfo("survival: the radio tuned in ({}{})", voice, task_.voice ? "" : ", not heard");
                }
                hud_.task_count = 0;
                hud_.task_need = 0;
            } else {
                const bool by_it = glm::distance(Flat(radio), Flat(feet)) < kRadioReach && std::abs(radio.y - feet.y) < 2.5f;
                if (by_it) task_.spoken += dt;
                const bool voice_on = task_.voice && game_.Audio() && game_.Audio()->IsPlaying(task_.voice);
                const float length = voice_on ? kRadioSpeech : 12.0f;
                hud_.task_count = static_cast<int>(std::floor(std::min(1.0f, task_.spoken / length) * 100.0f));
                hud_.task_need = 100;
                if (task_.spoken >= length || (!voice_on && task_.voice && task_.spoken >= 12.0f)) FinishTask();
            }
            break;
        }
        case SurvivalMission::Fragments: {
            int taken = 0;
            for (size_t i = 0; i < task_.spots.size(); ++i) {
                if (task_.taken[i]) {
                    ++taken;
                    continue;
                }
                const glm::vec3 p = task_.spots[i];
                hud_.task_marks.push_back(p + glm::vec3(0.0f, 0.35f, 0.0f));
                if (!hud_.dead && glm::distance(Flat(p), Flat(feet)) < 1.0f && std::abs(p.y - feet.y) < 1.4f) {
                    task_.taken[i] = true;
                    ++taken;
                    Play(pickup_, nullptr, 0.0f, 0.8f);
                    Play(lisa_breath_, nullptr, -8.0f, 1.0f);
                    Frighten(0.12f);
                    const int total_taken = static_cast<int>(std::count(task_.taken.begin(), task_.taken.end(), true));
                    LogInfo("survival: a piece of the photograph ({} of {})", total_taken, task_.spots.size());
                }
            }
            hud_.task_count = taken;
            hud_.task_need = static_cast<int>(task_.spots.size());
            if (taken == hud_.task_need) FinishTask();
            break;
        }
        case SurvivalMission::Lamps: {
            int broken = 0;
            for (size_t i = 0; i < task_.spots.size(); ++i) {
                if (task_.taken[i]) {
                    ++broken;
                } else {
                    hud_.task_marks.push_back(task_.spots[i] + glm::vec3(0.0f, 0.75f, 0.0f));
                }
            }
            hud_.task_count = broken;
            hud_.task_need = static_cast<int>(task_.spots.size());
            if (broken == hud_.task_need) FinishTask();
            break;
        }
        case SurvivalMission::Door: {
            const glm::vec3 door = task_.spots[0];
            hud_.task_marks.push_back(door + glm::vec3(0.0f, 2.3f, 0.0f));
            if (hold_at(door + task_.facing * 0.9f, kDoorHold, "pc_survival_use_door")) {
                Play(thud_, &task_.spots[0], 4.0f, 0.8f);
                Play(landing_, &task_.spots[0], 0.0f, 0.6f);
                shake_ = std::min(1.0f, shake_ + 0.3f);
                FinishTask();
            }
            task_.door_wait -= dt;
            if (!task_.done && task_.door_wait <= 0.0f) {
                task_.door_wait = 7.0f;
                if (spawned_this_wave_ + to_spawn_ < plan_.count + 12) ++to_spawn_;
            }
            break;
        }
        default: break;
        }
    }
    if (task_.kind == SurvivalMission::Door) {
        const float target = task_.done ? 0.0f : 1.0f - 0.8f * hud_.use_progress;
        const float rate = task_.done ? 16.0f : task_.at_house && target > task_.open ? 1.3f : 6.0f;
        task_.open += (target - task_.open) * (1.0f - std::exp(-dt * rate));
    }
}

bool SurvivalNight::TaskShot(const glm::vec3& origin, const glm::vec3& dir, float reach) {
    if (task_.kind != SurvivalMission::Lamps || task_.done) return false;
    int best = -1;
    float best_t = reach;
    for (size_t i = 0; i < task_.spots.size(); ++i) {
        if (task_.taken[i]) continue;
        const glm::vec3 c = task_.spots[i] + glm::vec3(0.0f, LanternGlass(), 0.0f);
        const float t = glm::dot(c - origin, dir);
        if (t <= 0.0f || t > best_t) continue;
        if (glm::distance(origin + dir * t, c) > 0.2f) continue;
        best = static_cast<int>(i);
        best_t = t;
    }
    if (best < 0) return false;
    const glm::vec3 c = task_.spots[static_cast<size_t>(best)] + glm::vec3(0.0f, LanternGlass(), 0.0f);
    task_.taken[static_cast<size_t>(best)] = true;
    for (int i = 0; i < 18; ++i) {
        Debris d;
        d.kind = Debris::Kind::Spark;
        d.id = next_id_++;
        d.position = c;
        d.velocity = glm::vec3(Rand() - 0.5f, 0.6f + Rand(), Rand() - 0.5f) * 4.0f;
        d.ground = c.y - 0.2f;
        d.life = 0.3f + 0.5f * Rand();
        debris_.push_back(d);
    }
    Play(glass_, &c, 0.0f, 1.0f);
    Play(rise_, &c, -6.0f, 1.3f);
    LogInfo("survival: a hell lantern broken");
    return true;
}

float SurvivalNight::LanternShare() const {
    if (task_.kind != SurvivalMission::Lamps || task_.spots.empty()) return 0.0f;
    const auto burning = std::count(task_.taken.begin(), task_.taken.end(), false);
    return static_cast<float>(burning) / static_cast<float>(task_.spots.size());
}

float SurvivalNight::DoorSpawn(glm::vec3& at) const {
    if (task_.kind == SurvivalMission::Door && !task_.done && !task_.spots.empty()) {
        at = task_.spots[0] + task_.facing * 0.6f;
        const glm::vec3 feet = game_.GetPlayer().Feet();
        return glm::distance(Flat(at), Flat(feet)) < 3.0f ? 0.0f : 0.6f;
    }
    if (night_door_ < 0.5f || !game_.House()) return 0.0f;
    const glm::vec3 feet = game_.GetPlayer().Feet();
    at = glm::vec3(kHouseDoorX, GroundAt(glm::vec3(kHouseDoorX, feet.y + 2.0f, kHouseFront + 0.6f), feet.y), kHouseFront + 0.6f);
    return glm::distance(Flat(at), Flat(feet)) > 4.0f ? 0.3f : 0.0f;
}

void SurvivalNight::AddTaskDraws(std::vector<DrawItem>& out) const {
    auto add = [&](const GpuMesh* mesh, const glm::mat4& m, uint32_t part) {
        if (!mesh) return;
        DrawItem item;
        item.mesh = mesh;
        item.transform = m;
        item.source = kTaskSource + part;
        out.push_back(item);
    };
    auto facing = [](const glm::vec3& at, const glm::vec3& normal) {
        const glm::vec3 x = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal));
        glm::mat4 m(1.0f);
        m[0] = glm::vec4(x, 0.0f);
        m[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        m[2] = glm::vec4(normal, 0.0f);
        m[3] = glm::vec4(at, 1.0f);
        return m;
    };
    const TaskModels& pt = task_models_;
    auto at = [](const glm::vec3& v) { return glm::translate(glm::mat4(1.0f), v); };
    auto turn = [](float angle, const glm::vec3& axis) { return glm::rotate(glm::mat4(1.0f), angle, axis); };
    const bool house_task = task_.kind == SurvivalMission::Door && task_.at_house;
    if (night_door_ > 0.02f && !house_task && game_.House()) {
        const glm::vec3 feet = game_.GetPlayer().Feet();
        const glm::vec3 door(kHouseDoorX, GroundAt(glm::vec3(kHouseDoorX, feet.y + 2.0f, kHouseFront + 0.6f), feet.y), kHouseFront);
        add(models_.door_dark.get(), facing(door, glm::vec3(0.0f, 0.0f, 1.0f)) * at(glm::vec3(0.0f, 0.0f, -1.5f)) * glm::scale(glm::mat4(1.0f), glm::vec3(2.2f, 1.2f, 1.0f)), 60);
    }
    if (task_.kind == SurvivalMission::None || task_.spots.empty()) return;
    switch (task_.kind) {
    case SurvivalMission::Fuse: {
        const glm::mat4 m = facing(task_.spots[0], task_.facing);
        const bool own = pt.fuse_box != nullptr;
        add(own ? pt.fuse_box : models_.fuse_box.get(), m, 0);
        const glm::vec3 pivot = own ? glm::vec3(0.255f, 0.0f, 0.2f) : glm::vec3(0.215f, 0.0f, 0.09f);
        const glm::mat4 lamp = at(own ? glm::vec3(0.03f, 0.01f, 0.178f) : glm::vec3(0.0f));
        const float up = task_.done ? 1.0f : Smooth(hud_.use_progress);
        add(models_.fuse_lever.get(), m * at(pivot) * turn(-kPi * up, glm::vec3(1, 0, 0)), 1);
        const bool blink = std::fmod(time_, 1.0f) < 0.55f;
        if (task_.done) {
            add(models_.fuse_lamp_on.get(), m * lamp, 2);
        } else if (blink) {
            add(models_.fuse_lamp_out.get(), m * lamp, 2);
        }
        break;
    }
    case SurvivalMission::Radio: {
        glm::mat4 m = facing(task_.spots[0], task_.facing);
        if (pt.radio) {
            if (RadioLift() > 0.0f) {
                add(pt.carton, m, 1);
                m = m * at(glm::vec3(0.0f, RadioLift(), 0.0f));
            }
            add(pt.radio, m * turn(-0.5f * kPi, glm::vec3(0, 1, 0)), 0);
        } else {
            add(models_.radio.get(), m, 0);
        }
        break;
    }
    case SurvivalMission::Fragments:
        for (size_t i = 0; i < task_.spots.size(); ++i) {
            if (task_.taken[i]) continue;
            const glm::mat4 m = at(task_.spots[i]) * turn(static_cast<float>(i) * 1.9f + 0.4f, glm::vec3(0, 1, 0));
            if (const GpuMesh* piece = pt.pieces[(i * 3 + 1) % pt.pieces.size()]) {
                add(piece, m * at(glm::vec3(0.0f, 0.004f, 0.0f)) * turn(0.5f * kPi, glm::vec3(1, 0, 0)), static_cast<uint32_t>(i));
            } else {
                add(models_.photo_piece.get(), m, static_cast<uint32_t>(i));
            }
        }
        break;
    case SurvivalMission::Lamps:
        for (size_t i = 0; i < task_.spots.size(); ++i) {
            const glm::mat4 m = at(task_.spots[i]) * turn(static_cast<float>(i) * 0.7f, glm::vec3(0, 1, 0));
            if (pt.lamp_lit && pt.lamp_out) {
                if (task_.taken[i]) {
                    const glm::mat4 lying = m * at(glm::vec3(0.0f, kLampLying, 0.0f)) * turn(0.5f * kPi, glm::vec3(0, 0, 1));
                    if (pt.lamp_globe < kMaxHiddenMeshes) {
                        add(pt.lamp_lit, lying, static_cast<uint32_t>(i));
                        out.back().hidden_meshes.set(pt.lamp_globe);
                    } else {
                        add(pt.lamp_out, lying, static_cast<uint32_t>(i));
                    }
                } else if (pt.lamp_globe < kMaxHiddenMeshes) {
                    add(pt.lamp_lit, m, static_cast<uint32_t>(i));
                    out.back().hidden_meshes.set(pt.lamp_globe);
                    add(pt.lamp_lit, m, 16 + static_cast<uint32_t>(i));
                    out.back().hidden_meshes.set();
                    out.back().hidden_meshes.reset(pt.lamp_globe);
                    out.back().material_override = pt.lamp_red >= 0 ? pt.lamp_red : static_cast<int32_t>(models_.lantern_glow);
                } else {
                    add(pt.lamp_lit, m, static_cast<uint32_t>(i));
                }
            } else {
                add(task_.taken[i] ? models_.lantern_broken.get() : models_.lantern.get(), m, static_cast<uint32_t>(i));
            }
        }
        break;
    case SurvivalMission::Door: {
        const glm::mat4 m = facing(task_.spots[0], task_.facing);
        if (task_.at_house) {
            if (task_.open > 0.02f) {
                add(models_.door_dark.get(), m * at(glm::vec3(0.0f, 0.0f, -1.5f)) * glm::scale(glm::mat4(1.0f), glm::vec3(2.2f, 1.2f, 1.0f)), 1);
            }
            break;
        }
        add(models_.door_frame.get(), m, 0);
        if (pt.door && pt.door_wood >= 0 && models_.door_frame) out.back().material_override = pt.door_wood;
        if (task_.open > 0.02f) add(models_.door_dark.get(), m, 1);
        if (pt.door) {
            add(pt.door, m * at(glm::vec3(0.4f, 0.0f, 0.0f)) * turn(1.05f * task_.open, glm::vec3(0, 1, 0)), 2);
        } else {
            add(models_.door_leaf.get(), m * at(glm::vec3(-0.4f, 0.0f, 0.0f)) * turn(-1.05f * task_.open, glm::vec3(0, 1, 0)), 2);
        }
        break;
    }
    default: break;
    }
}

void SurvivalNight::AddTaskLights(SceneLighting& lighting) const {
    auto point = [&](uint64_t id, const glm::vec3& at, const glm::vec3& intensity, float range) {
        SceneLight l;
        l.id = id;
        l.type = LightType::Point;
        l.position = at;
        l.intensity = intensity;
        l.inner_range = 0.0f;
        l.outer_range = range;
        l.dimmer = 0.05f;
        l.diffuse_scale = 1.0f;
        l.cast_shadow = false;
        lighting.lights.push_back(l);
    };
    if (night_door_ > 0.02f && !(task_.kind == SurvivalMission::Door && task_.at_house) && game_.House()) {
        const glm::vec3 feet = game_.GetPlayer().Feet();
        const glm::vec3 red(kHouseDoorX, GroundAt(glm::vec3(kHouseDoorX, feet.y + 2.0f, kHouseFront + 0.6f), feet.y) + 1.1f, kHouseFront - 0.35f);
        point(kTaskSource + 33, red, glm::vec3(1.0f, 0.08f, 0.05f) * (260.0f * night_door_), 6.0f);
    }
    if (task_.kind == SurvivalMission::None || task_.spots.empty()) return;
    if (studio_) {
        const float gain = task_.kind == SurvivalMission::Door ? 80.0f : 37.0f;
        point(kTaskSource + 40, studio_lamp_, glm::vec3(1.0f, 0.86f, 0.68f) * (gain * studio_reach_ * studio_reach_), studio_reach_ + 1.5f);
    }
    switch (task_.kind) {
    case SurvivalMission::Fuse: {
        const bool blink = std::fmod(time_, 1.0f) < 0.55f;
        const glm::vec3 lamp = task_.spots[0] + task_.facing * (task_models_.fuse_box ? 0.45f : 0.25f) + glm::vec3(0.0f, 0.2f, 0.0f);
        if (task_.done) {
            point(kTaskSource + 32, lamp, glm::vec3(0.3f, 1.0f, 0.4f) * 25.0f, 2.0f);
        } else if (blink) {
            point(kTaskSource + 32, lamp, glm::vec3(1.0f, 0.15f, 0.08f) * 60.0f, 3.0f);
        }
        break;
    }
    case SurvivalMission::Radio:
        point(kTaskSource + 32, task_.spots[0] + task_.facing * 0.3f + glm::vec3(0.0f, 0.25f + RadioLift(), 0.0f),
              glm::vec3(1.0f, 0.55f, 0.2f) * (task_.tuned && !task_.done ? 70.0f : 35.0f), 3.0f);
        break;
    case SurvivalMission::Fragments:
        for (size_t i = 0; i < task_.spots.size() && i < 6; ++i) {
            if (!task_.taken[i]) point(kTaskSource + 32 + i, task_.spots[i] + glm::vec3(0.0f, 0.3f, 0.0f), glm::vec3(1.0f, 0.2f, 0.25f) * 30.0f, 2.2f);
        }
        break;
    case SurvivalMission::Lamps:
        for (size_t i = 0; i < task_.spots.size() && i < 6; ++i) {
            if (task_.taken[i]) continue;
            const float flicker = 0.85f + 0.1f * std::sin(time_ * 19.0f + static_cast<float>(i) * 2.0f) + 0.05f * std::sin(time_ * 31.0f + static_cast<float>(i));
            point(kTaskSource + 32 + i, task_.spots[i] + glm::vec3(0.0f, LanternGlass(), 0.0f), glm::vec3(1.0f, 0.06f, 0.04f) * (420.0f * flicker), 8.0f);
        }
        break;
    case SurvivalMission::Door:
        if (task_.open > 0.02f) {
            const glm::vec3 red = task_.at_house ? task_.spots[0] - task_.facing * 0.35f : task_.spots[0] + task_.facing * 0.5f;
            point(kTaskSource + 32, red + glm::vec3(0.0f, 1.1f, 0.0f), glm::vec3(1.0f, 0.08f, 0.05f) * (260.0f * task_.open), task_.at_house ? 6.0f : 5.0f);
        }
        break;
    default: break;
    }
}

void SurvivalNight::Frighten(float amount) {
    const float late = std::clamp((static_cast<float>(wave_) - 20.0f) / 20.0f, 0.0f, 1.0f);
    const float cold = std::max(0.2f, 1.0f - 0.4f * static_cast<float>(boons_[SurvivalBoon::ColdBlood]));
    if (amount > 0.0f) {
        fear_ = std::min(1.0f, fear_ + amount * late * cold);
    } else {
        fear_ = std::max(0.0f, fear_ + amount);
    }
}

void SurvivalNight::UpdateFear(float dt, const Camera& camera) {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    if (hud_.dead) {
        fear_ = std::max(0.0f, fear_ - dt * 0.3f);
        phantoms_.clear();
    } else {
        float rise = 0.0f;
        if (StreetLightLevel() < 0.5f) rise += 0.018f;
        int close = 0;
        bool lisa_near = false;
        for (const Enemy& e : enemies_) {
            if (e.state == Enemy::State::Dying) continue;
            const float d = glm::distance(Flat(e.position), Flat(feet));
            if (d < 5.0f) ++close;
            if (e.kind == SurvivalEnemyKind::Lisa && d < 15.0f) lisa_near = true;
        }
        rise += 0.02f * static_cast<float>(std::min(close, 3));
        if (lisa_near) rise += 0.04f;
        if (health_ < 0.3f * max_health_) rise += 0.015f;
        if (rise > 0.0f) Frighten(rise * dt);
        float fall = phase_ == SurvivalPhase::Wave ? 0.0f : 0.06f;
        if (NearLitLamp(feet, 4.5f) && StreetLightLevel() > 0.5f) fall += 0.05f;
        if (InFlareLight(feet)) fall += 0.07f;
        if (close == 0 && !lisa_near && StreetLightLevel() > 0.5f) fall += 0.012f;
        Frighten(-fall * dt);
    }
    hud_.fear = fear_;
    const bool afraid = fear_ > 0.66f && phase_ == SurvivalPhase::Wave && !hud_.dead;
    phantom_wait_ -= dt * (afraid ? 1.0f : 0.0f);
    if (afraid && phantom_wait_ <= 0.0f && phantoms_.size() < 2) {
        phantom_wait_ = 6.0f + 8.0f * Rand() - 4.0f * (fear_ - 0.66f);
        std::vector<const Enemy*> walkers;
        for (const Enemy& e : enemies_) {
            if (e.state == Enemy::State::Chasing && (e.kind == SurvivalEnemyKind::Doll || e.kind == SurvivalEnemyKind::Stretched)) walkers.push_back(&e);
        }
        if (!walkers.empty()) {
            const Enemy& of = *walkers[std::min(walkers.size() - 1, static_cast<size_t>(Rand() * static_cast<float>(walkers.size())))];
            glm::vec3 ahead = camera.Forward();
            ahead.y = 0.0f;
            ahead = glm::length(ahead) > 1e-3f ? glm::normalize(ahead) : glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 side(-ahead.z, 0.0f, ahead.x);
            const float s = Rand() < 0.5f ? 1.0f : -1.0f;
            const float d = 12.0f + 14.0f * Rand();
            glm::vec3 at = feet + (ahead * 0.85f + side * (0.55f * s)) * d;
            at.x = std::clamp(at.x, -7.0f, 4.5f);
            at.y = GroundAt(at + glm::vec3(0.0f, 1.0f, 0.0f), feet.y);
            Phantom p;
            p.id = next_id_++;
            p.of = of.id;
            p.position = at;
            const glm::vec2 to = Flat(feet) - Flat(at);
            p.yaw = std::atan2(to.x, to.y);
            p.life = 3.0f + 3.0f * Rand();
            phantoms_.push_back(p);
            LogInfo("survival: a phantom at {:.1f} m (fear {:.2f})", d, fear_);
        }
    }
    for (Phantom& p : phantoms_) {
        p.age += dt;
        const auto of = std::find_if(enemies_.begin(), enemies_.end(), [&](const Enemy& e) { return e.id == p.of; });
        const glm::vec3 to = p.position + glm::vec3(0.0f, 1.2f, 0.0f) - camera.position;
        const float d = glm::length(to);
        const bool looked_at = d > 1e-3f && glm::dot(to / d, camera.Forward()) > std::cos(7.0f * kPi / 180.0f);
        if (p.fade < 0.0f && (looked_at || d < 8.0f || of == enemies_.end() || of->state == Enemy::State::Dying || p.age > p.life)) {
            p.fade = 0.0f;
            if (looked_at) Play(lisa_breath_, &p.position, -10.0f, 0.8f);
        }
        if (p.fade >= 0.0f) p.fade += dt;
        if (p.fade < 0.0f && of != enemies_.end()) {
            const glm::vec2 dir = Flat(feet) - Flat(p.position);
            if (glm::length(dir) > 1e-3f) p.position += glm::vec3(dir.x, 0.0f, dir.y) / glm::length(dir) * (of->speed * 0.6f * dt);
        }
    }
    std::erase_if(phantoms_, [](const Phantom& p) { return p.fade > 0.25f; });
    whisper_wait_ -= afraid ? dt : 0.0f;
    if (afraid && whisper_wait_ <= 0.0f && fear_ > 0.8f) {
        whisper_wait_ = 16.0f + 14.0f * Rand();
        const glm::vec3 behind = camera.position - camera.Forward() * 2.5f;
        Play(Rand() < 0.5f ? lisa_laugh_ : lisa_breath_, &behind, -6.0f, 1.0f);
        LogInfo("survival: Lisa heard behind (fear {:.2f})", fear_);
    }
    false_left_ = std::max(0.0f, false_left_ - dt);
    if (afraid && fear_ > 0.85f && false_left_ <= 0.0f && Rand() < dt * 0.15f) false_left_ = 0.35f;
    hud_.false_left = false_left_ > 0.0f ? std::max(1, hud_.enemies_left + static_cast<int>(Rand() * 30.0f) - 10) : -1;
}

void SurvivalNight::AddPhantomDraws(std::vector<DrawItem>& out) const {
    for (const Phantom& p : phantoms_) {
        const auto of = std::find_if(enemies_.begin(), enemies_.end(), [&](const Enemy& e) { return e.id == p.of; });
        if (of == enemies_.end() || !doll_mesh_ || skins_[of->slot].empty()) continue;
        Enemy shown = *of;
        shown.position = p.position - glm::vec3(0.0f, p.fade > 0.0f ? 1.9f * Smooth(p.fade / 0.25f) : 0.0f, 0.0f);
        shown.yaw = p.yaw;
        shown.sink = 0.0f;
        DrawItem item;
        item.mesh = doll_mesh_;
        item.transform = EnemyTransform(shown, false);
        item.skin = skins_[of->slot];
        item.tint = glm::vec4(0.35f, 0.32f, 0.34f, 1.0f);
        item.source = 0x5355525600000000ull + 0x78000u + p.id;
        out.push_back(item);
    }
}

void SurvivalNight::UpdateDark(float dt) {
    constexpr float kCount[] = {10.0f, 8.0f, 6.0f, 5.0f, 4.0f};
    constexpr float kGrace = 15.0f;
    constexpr int kFrom = 8;
    constexpr float kReach = 3.5f;
    const glm::vec3 here = game_.GetPlayer().Feet();
    const bool listening = task_.kind == SurvivalMission::Radio && task_.tuned && !task_.done && !task_.spots.empty() &&
                           glm::distance(Flat(task_.spots[0]), Flat(here)) < kRadioReach && std::abs(task_.spots[0].y - here.y) < 2.5f;
    if (task_.hold > 0.0f || listening) {
        hud_.dark_count = 0;
        return;
    }
    const float limit = kCount[static_cast<size_t>(difficulty_)];
    const float to_light = ToTheLight(game_.GetPlayer().Feet());
    const bool rule = phase_ == SurvivalPhase::Wave && wave_ >= kFrom && !hud_.dead && !caught_ && to_light < limit * kReach;
    if (!rule || to_light <= 0.0f) {
        if (hud_.dark_count > 0) LogInfo("survival: the dark's count stopped at {}", hud_.dark_count);
        hud_.dark_count = 0;
        dark_time_ = std::max(0.0f, std::min(dark_time_, 0.5f * kGrace) - 3.0f * dt);
        return;
    }
    dark_time_ += dt;
    if (dark_time_ < kGrace) return;
    const float left = kGrace + limit - dark_time_;
    const int count = std::max(1, static_cast<int>(std::ceil(left)));
    if (count != hud_.dark_count) {
        if (hud_.dark_count == 0) LogInfo("survival: the dark's count begins at {} on wave {}, {:.1f} m from the light", count, wave_, to_light);
        hud_.dark_count = count;
        if (!PostEvent("Play_sfx_digiclock_tick_solo", nullptr)) PlaySound(tick_sound_, nullptr);
        if (count <= 3) Play(lisa_breath_, nullptr);
        Frighten(0.03f);
    }
    if (left <= 0.0f) {
        hud_.dark_count = 0;
        dark_time_ = 0.0f;
        DarkCatch();
    }
}

void SurvivalNight::DarkCatch() {
    LogInfo("survival: the dark's count ran out on wave {}", wave_);
    const glm::vec3 feet = game_.GetPlayer().Feet();
    const int to_spawn = to_spawn_;
    const glm::vec2 spawn_at = spawn_at_;
    const bool spawn_at_set = spawn_at_set_;
    const size_t before = enemies_.size();
    spawn_at_ = glm::vec2(1.0f, 0.0f);
    spawn_at_set_ = true;
    SpawnEnemy(SurvivalEnemyKind::Lisa);
    spawn_at_ = spawn_at;
    spawn_at_set_ = spawn_at_set;
    to_spawn_ = to_spawn;
    if (enemies_.size() > before && enemies_.back().kind == SurvivalEnemyKind::Lisa) {
        enemies_.back().sink = 0.0f;
        catch_cause_ = kSurvivalCauseDark;
        Catch(enemies_.back());
        catch_cause_ = static_cast<int>(SurvivalEnemyKind::Lisa);
    }
    if (!caught_) HurtPlayer(health_ + 1.0f, feet, kSurvivalCauseDark);
}

bool SurvivalNight::StartSupply() {
    const glm::vec3 feet = game_.GetPlayer().Feet();
    std::vector<glm::vec2> lamps = LitLamps();
    std::erase_if(lamps, [&](const glm::vec2& lamp) {
        const float d = glm::distance(lamp, glm::vec2(feet.x, feet.z));
        return d < 15.0f || d > 45.0f;
    });
    if (lamps.empty()) return false;
    const glm::vec2 lamp = lamps[std::min(lamps.size() - 1, static_cast<size_t>(Rand() * static_cast<float>(lamps.size())))];
    supply_at_ = glm::vec3(lamp.x, 0.0f, lamp.y);
    supply_at_.y = GroundAt(supply_at_ + glm::vec3(0.0f, feet.y + 1.0f, 0.0f), feet.y);
    supply_ids_.clear();
    auto box = [&](glm::vec2 side, SurvivalPickupKind kind, SurvivalWeapon weapon) {
        Drop(supply_at_ + glm::vec3(side.x, 0.0f, side.y), kind, weapon);
        pickups_.back().life = 240.0f;
        supply_ids_.push_back(pickups_.back().id);
    };
    box({0.6f, 0.0f}, SurvivalPickupKind::Ammo, AmmoNeeded());
    box({-0.6f, 0.0f}, SurvivalPickupKind::Ammo, AmmoNeeded());
    box({0.0f, 0.7f}, SurvivalPickupKind::Ammo, AmmoNeeded());
    box({0.0f, -0.7f}, SurvivalPickupKind::Health, SurvivalWeapon::Handgun);
    box({0.9f, 0.9f}, SurvivalPickupKind::Health, SurvivalWeapon::Handgun);
    if (flares_ < MaxFlares()) box({-0.9f, 0.9f}, SurvivalPickupKind::Flare, SurvivalWeapon::Handgun);
    supply_on_ = true;
    Notice("pc_survival_supply", "", 4.5f, Vision::None);
    Play(thud_, &supply_at_);
    LogInfo("survival: the radio names the lamp at ({:.1f} {:.1f}), {:.0f} m off, on wave {}", lamp.x, lamp.y, glm::distance(lamp, glm::vec2(feet.x, feet.z)), wave_);
    return true;
}

void SurvivalNight::UpdateSupply() {
    if (supply_on_) {
        const bool left = std::any_of(pickups_.begin(), pickups_.end(), [this](const Pickup& p) {
            return std::find(supply_ids_.begin(), supply_ids_.end(), p.id) != supply_ids_.end();
        });
        const glm::vec3 feet = game_.GetPlayer().Feet();
        if (!left || glm::distance(glm::vec2(feet.x, feet.z), glm::vec2(supply_at_.x, supply_at_.z)) < 3.0f) {
            supply_on_ = false;
            supply_ids_.clear();
            LogInfo("survival: the lamp's supply reached");
        }
    }
    hud_.supply_on = supply_on_;
    hud_.supply_at = supply_at_ + glm::vec3(0.0f, 0.7f, 0.0f);
}

}
