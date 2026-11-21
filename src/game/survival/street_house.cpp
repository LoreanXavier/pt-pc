#include "game/survival/street_house.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/assets/geom.h"
#include "engine/core/log.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_lighting.h"
#include "engine/render/scene_renderer.h"
#include "engine/fs/vfs.h"
#include "game/game.h"
#include "game/loop_browser.h"
#include "game/survival/street_city.h"

namespace pt::game {

namespace {

constexpr uint64_t kHouseSource = 0x484F555345000000ull;
constexpr float kDoorSwing = 1.6f;
constexpr float kDoorShut = 1.2f;

float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

void AddWall(std::vector<GeomTriangle>& out, glm::vec2 a, glm::vec2 b, const glm::vec3& toward, float height = 6.0f) {
    const float top = height;
    const glm::vec3 a0(a.x, -3.0f, a.y), a1(a.x, top, a.y), b0(b.x, -3.0f, b.y), b1(b.x, top, b.y);
    for (auto t : {std::array{a0, b0, b1}, std::array{a0, b1, a1}}) {
        if (glm::dot(glm::cross(t[1] - t[0], t[2] - t[0]), toward) < 0.0f) std::swap(t[1], t[2]);
        GeomTriangle tri{};
        tri.a = t[0];
        tri.b = t[1];
        tri.c = t[2];
        out.push_back(tri);
    }
}

void AddFloor(std::vector<GeomTriangle>& out, glm::vec2 a, glm::vec2 b, float y) {
    const glm::vec3 p00(a.x, y, a.y), p10(b.x, y, a.y), p11(b.x, y, b.y), p01(a.x, y, b.y);
    for (auto t : {std::array{p00, p10, p11}, std::array{p00, p11, p01}}) {
        if (glm::cross(t[1] - t[0], t[2] - t[0]).y < 0.0f) std::swap(t[1], t[2]);
        GeomTriangle tri{};
        tri.a = t[0];
        tri.b = t[1];
        tri.c = t[2];
        out.push_back(tri);
    }
}

}

StreetHouse::StreetHouse(Game& game) : game_(game) {}

StreetHouse::~StreetHouse() = default;

bool StreetHouse::Ensure() {
    if (!tried_) {
        tried_ = true;
        ready_ = BuildStreetHouse(game_.Models(), kBrowseEnding, meshes_);
        if (!ready_) LogWarn("street: the house could not be built");
        constexpr const char* kDoorModel = "/Assets/sh/environ/object/shsb/house/shsb_hous001/scenes/shsb_hous001_door001.fmdl";
        constexpr const char* kHallwayPackage = "/Assets/sh/level/promotion/pt_2014/hallway/pt14_hallway.fpk";
        Vfs& vfs = game_.GetVfs();
        if (!vfs.ReadFile(kDoorModel)) vfs.LoadPackage(kHallwayPackage);
        const ModelEntry* door = vfs.ReadFile(kDoorModel) ? game_.Models().Get(kDoorModel) : nullptr;
        if (ready_ && door && door->mesh) {
            const glm::vec3 lo = door->mesh->bounds_min, hi = door->mesh->bounds_max, size = hi - lo;
            const bool thin_x = size.x < size.z;
            glm::mat4 turn = thin_x ? glm::rotate(glm::mat4(1.0f), 1.5707964f, glm::vec3(0, 1, 0)) : glm::mat4(1.0f);
            const float width = thin_x ? size.z : size.x, height = size.y;
            const float sy = height > 1.8f && height < 2.4f ? 1.0f : (kHouseDoorHeight - 0.01f) / height;
            const float sx = (2.0f * kHouseDoorHalf - 0.01f) / std::max(width, 0.1f);
            auto turned = [&](const glm::mat4& m) {
                const glm::vec3 a = glm::vec3(m * glm::vec4(lo, 1.0f)), b = glm::vec3(m * glm::vec4(hi, 1.0f));
                return std::pair{glm::min(a, b), glm::max(a, b)};
            };
            if (const auto [a, b] = turned(turn); std::abs(b.x) < std::abs(a.x)) turn = glm::rotate(glm::mat4(1.0f), 3.1415927f, glm::vec3(0, 1, 0)) * turn;
            const auto [mn, mx] = turned(turn);
            pt_door_fit_ = glm::scale(glm::mat4(1.0f), glm::vec3(sx, sy, 1.0f)) *
                           glm::translate(glm::mat4(1.0f), glm::vec3(-mn.x, -mn.y, -0.5f * (mn.z + mx.z))) * turn;
            pt_door_ = door->mesh.get();
            for (size_t i = 0; i < door->materials.size() && i < door->material_indices.size(); ++i) {
                if (door->materials[i].name.starts_with("wood")) door_wood_ = static_cast<int32_t>(door->material_indices[i]);
            }
            LogInfo("street: the house's door is P.T.'s (bounds ({:.3f} {:.3f} {:.3f}) to ({:.3f} {:.3f} {:.3f}), scaled {:.3f} x {:.3f})", lo.x, lo.y,
                    lo.z, hi.x, hi.y, hi.z, sx, sy);
        }
    }
    return ready_;
}

std::vector<GeomTriangle> StreetHouse::Walls() const {
    std::vector<GeomTriangle> walls;
    if (!ready_) return walls;
    const float back = kHouseFront + 0.12f;
    AddWall(walls, {kStreetLines.x, kTowerFront}, {kDoorwayX.x, kTowerFront}, glm::vec3(0.0f, 0.0f, 1.0f));
    AddWall(walls, {kDoorwayX.x, kTowerFront}, {kDoorwayX.x, back}, glm::vec3(1.0f, 0.0f, 0.0f));
    AddWall(walls, {kDoorwayX.x, back}, {kDoorwayX.y, back}, glm::vec3(0.0f, 0.0f, 1.0f));
    AddWall(walls, {kDoorwayX.y, back}, {kDoorwayX.y, kTowerFront}, glm::vec3(-1.0f, 0.0f, 0.0f));
    AddWall(walls, {kDoorwayX.y, kTowerFront}, {kStreetLines.y, kTowerFront}, glm::vec3(0.0f, 0.0f, 1.0f));
    AddFloor(walls, {kDoorwayX.x, kHouseFront}, {kDoorwayX.y, kPlinthFront}, kHouseDoorSill);
    for (const StreetEdge& e : StreetEdges()) AddWall(walls, e.a, e.b, glm::vec3(e.toward.x, 0.0f, e.toward.y), 3.2f);
    for (const StreetBody& b : StreetBodies()) {
        const glm::vec2 z(-b.x_axis.y, b.x_axis.x);
        const glm::vec2 corners[4] = {b.at - b.x_axis * b.half_x + z * b.back, b.at + b.x_axis * b.half_x + z * b.back, b.at + b.x_axis * b.half_x + z * b.front,
                                      b.at - b.x_axis * b.half_x + z * b.front};
        for (int i = 0; i < 4; ++i) {
            const glm::vec2 a = corners[i], c = corners[(i + 1) % 4], out = 0.5f * (a + c) - (b.at + z * (0.5f * (b.back + b.front)));
            AddWall(walls, a, c, glm::vec3(out.x, 0.0f, out.y), 2.6f);
        }
    }
    AddFloor(walls, {-21.0f, 20.0f}, {15.0f, 192.0f}, -0.02f);
    return walls;
}

bool StreetHouse::Hides(const DrawItem& item) const {
    if (!ready_) return false;
    const glm::vec3 at(item.transform[3]);
    return at.x > kStreetLines.x - 1.2f && at.x < kStreetLines.y + 1.0f && at.z > kTowerBack && at.z < kHouseFront - 0.02f;
}

void StreetHouse::Begin(Entry entry) {
    entry_ = entry;
    open_ = entry == Entry::Porch ? 1.0f : 0.0f;
    slam_pending_ = entry == Entry::Porch;
    LogInfo("street: the house's door {}", entry == Entry::Porch ? "closing behind the player" : "shut");
}

glm::vec3 StreetHouse::EntryFeet() const {
    return entry_ == Entry::Porch ? glm::vec3(kHouseDoorX, kHouseDoorSill + 0.04f, kHouseFront + 0.75f) : glm::vec3(kHouseDoorX, 0.05f, 27.0f);
}

void StreetHouse::Update(float dt) {
    if (!ready_) return;
    if (const SurvivalNight* night = game_.Survival(); night && night->HouseDoor() >= 0.0f) {
        open_ = night->HouseDoor();
        slam_pending_ = false;
        return;
    }
    if (open_ <= 0.0f) return;
    open_ = std::max(0.0f, open_ - dt * kDoorShut);
    if (slam_pending_ && open_ <= 0.0f) {
        slam_pending_ = false;
        if (game_.Audio()) {
            const glm::vec3 at(kHouseDoorX, 1.0f, kHouseFront);
            game_.Audio()->PostEvent("Play_sfx_door_close_01", &at);
        }
        LogInfo("street: the house's door shut behind the player");
    }
}

void StreetHouse::AddDraws(std::vector<DrawItem>& out) const {
    if (!ready_) return;
    DrawItem item;
    const StreetTower& tower = meshes_.tower;
    item.mesh = tower.base;
    item.source = kHouseSource + 3;
    out.push_back(item);
    item.mesh = tower.storey;
    item.source = kHouseSource + 4;
    for (int k = 0; k < tower.storeys; ++k) {
        item.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, tower.storey_height * static_cast<float>(k), 0.0f));
        out.push_back(item);
    }
    item.mesh = tower.top;
    item.source = kHouseSource + 5;
    out.push_back(item);
    item.transform = glm::mat4(1.0f);
    item.mesh = meshes_.doorway.get();
    item.source = kHouseSource + 6;
    if (pt_door_ && door_wood_ >= 0) item.material_override = door_wood_;
    out.push_back(item);
    item.material_override = -1;
    item.mesh = meshes_.house.get();
    item.source = kHouseSource;
    out.push_back(item);
    const SurvivalNight* night = game_.Survival();
    if (meshes_.backdrop && !(night && night->Active() && night->CityUp())) {
        item.mesh = meshes_.backdrop.get();
        item.source = kHouseSource + 1;
        out.push_back(item);
    }
    item.mesh = pt_door_ ? pt_door_ : meshes_.door.get();
    item.transform = glm::translate(glm::mat4(1.0f), glm::vec3(kHouseDoorX - kHouseDoorHalf + 0.005f, kHouseDoorSill, kHouseFront - 0.03f)) *
                     glm::rotate(glm::mat4(1.0f), kDoorSwing * Smooth(open_), glm::vec3(0, 1, 0)) * (pt_door_ ? pt_door_fit_ : glm::mat4(1.0f));
    item.source = kHouseSource + 2;
    out.push_back(item);
}

void StreetHouse::AddLights(SceneLighting& out, float street) const {
    if (!ready_) return;
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
        out.lights.push_back(l);
    };
    const float level = std::clamp(street, 0.0f, 1.0f);
    point(kHouseSource + 16, {kHouseDoorX, 2.62f, kDoorwayMouth + 0.35f}, glm::vec3(1.0f, 0.72f, 0.42f) * (480.0f * level), 7.0f);
    out.lights.back().dimmer = 0.12f;
}

}
