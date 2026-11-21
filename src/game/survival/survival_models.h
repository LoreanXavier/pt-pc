#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "engine/render/mesh.h"
#include "engine/render/texture_manager.h"

namespace pt {
class ModelCache;
struct DrawItem;
}

namespace pt::game {

struct SurvivalReloadKey {
    float t = 0.0f;
    glm::vec3 at{0.0f};
    glm::vec3 turn{0.0f};
    float hand = 0.0f;
    glm::vec3 left{0.0f};
    glm::quat left_turn{1.0f, 0.0f, 0.0f, 0.0f};
    float away = 0.0f;
    float elbow = 0.2f;
    std::array<glm::vec4, 5> fingers{};
    float thumb = 0.0f;
    glm::vec4 right_thumb{0.0f};
    float item = 0.0f;
    float seat = 0.0f;
    float spent = 0.0f;
    float open = 0.0f;
};

struct SurvivalReload {
    std::vector<SurvivalReloadKey> keys;
    glm::vec3 item_at{0.0f};
    glm::vec3 item_turn{0.0f};
    float eject = 0.3f;
};

struct SurvivalGunModel {
    const GpuMesh* body = nullptr;
    const GpuMesh* moving = nullptr;
    const GpuMesh* hammer = nullptr;
    const GpuMesh* magazine = nullptr;
    glm::vec3 muzzle{0.0f};
    glm::vec3 ejection{0.0f};
    glm::vec3 cylinder_axis{0.0f};
    glm::vec3 cylinder_swing{0.03f, -0.012f, 0.0f};
    glm::vec3 hammer_pivot{0.0f};
    glm::vec3 sight_rear{0.0f};
    glm::vec3 sight_front{0.0f, 0.0f, 1.0f};
    glm::vec3 wrist{0.0f};
    float rake = 0.0f;
    glm::vec3 grip_turn{0.0f};
    std::array<glm::vec4, 5> fingers = {glm::vec4(0.0f, 0.0f, 0.35f, 0.35f), glm::vec4(0.0f, 1.0f, 1.15f, 0.8f), glm::vec4(0.0f, 1.0f, 1.15f, 0.8f),
                                        glm::vec4(0.0f, 1.0f, 1.15f, 0.8f), glm::vec4(0.0f, 1.0f, 1.15f, 0.8f)};
    std::optional<glm::vec4> trigger_index;
    glm::vec3 support{0.03f, -0.05f, -0.062f};
    glm::vec3 support_turn{0.0f};
    std::array<glm::vec4, 5> support_fingers = {glm::vec4(0.0f, 0.0f, 0.26f, 0.26f), glm::vec4(0.0f, 0.75f, 0.86f, 0.6f),
                                                glm::vec4(0.0f, 0.75f, 0.86f, 0.6f), glm::vec4(0.0f, 0.75f, 0.86f, 0.6f),
                                                glm::vec4(0.0f, 0.75f, 0.86f, 0.6f)};
    struct PressPoint {
        glm::vec3 place{0.0f};
        glm::vec3 step{0.0f};
    };
    std::array<std::vector<PressPoint>, 2> press;
    glm::vec3 PressAt(size_t side, const glm::vec3& place) const;
    float slide_travel = 0.028f;
    glm::vec3 magazine_base{0.0f, -0.106f, -0.029f};
    glm::vec3 magazine_drop{0.0f, -0.950f, -0.311f};
    glm::vec3 pump_grip{0.0f, 0.034f, 0.29f};
    float pump_travel = 0.085f;
    glm::vec3 loading_port{0.0f, 0.02f, 0.075f};
    glm::vec3 lever_pivot{0.0f};
    glm::vec3 hinge{0.0f};
    float barrel_side = 0.0f;
    SurvivalReload reload;
    struct SkinMaterial {
        uint32_t index = 0;
        uint8_t channel = 0;
        MaterialGpu original{};
    };
    std::vector<SkinMaterial> skin_materials;
    bool loaded = false;
};

struct SurvivalModels;
void ApplyWeaponSkin(TextureManager& textures, const SurvivalGunModel& gun, int skin_id);
void ApplyWeaponSkinAnimated(TextureManager& textures, const SurvivalGunModel& gun, int skin_id, float time_sec);
void ApplyTorchSkin(TextureManager& textures, const SurvivalModels& models, int torch_id, float lens_gain = 60.0f);
void EmitMythicWeaponVfx(TextureManager& textures, const SurvivalModels& models, const SurvivalGunModel& gun,
                         const glm::mat4& gun_world, int skin_id, float time_sec, uint8_t hidden_views,
                         bool view_attached, bool turns_with_view, float emission_scale, std::vector<DrawItem>& out);

struct SurvivalModels {
    std::array<SurvivalGunModel, 6> guns;
    std::unique_ptr<GpuMesh> ammo_box;
    std::unique_ptr<GpuMesh> medkit;
    std::unique_ptr<GpuMesh> flash;
    std::unique_ptr<GpuMesh> tracer;
    std::unique_ptr<GpuMesh> casing;
    std::unique_ptr<GpuMesh> shell;
    std::unique_ptr<GpuMesh> pipe;
    std::array<std::unique_ptr<GpuMesh>, 2> pipe_halves;
    std::unique_ptr<GpuMesh> flare;
    std::unique_ptr<GpuMesh> flare_tip;
    struct Drawn {
        std::array<const GpuMesh*, 6> ammo{};
        const GpuMesh* medkit = nullptr;
        const GpuMesh* pipe = nullptr;
        std::array<const GpuMesh*, 2> pipe_halves{};
        const GpuMesh* flare = nullptr;
        const GpuMesh* casing = nullptr;
        const GpuMesh* shell = nullptr;
    } drawn;
    std::unique_ptr<GpuMesh> fuse_box, fuse_lever, fuse_lamp_out, fuse_lamp_on;
    std::unique_ptr<GpuMesh> radio, photo_piece, lantern, lantern_broken;
    std::unique_ptr<GpuMesh> door_frame, door_leaf, door_dark;
    std::array<uint32_t, 3> gun_metals{};
    uint32_t lantern_glow = 0;
    std::array<std::unique_ptr<GpuMesh>, 3> blood;
    std::unique_ptr<GpuMesh> wound;
    std::unique_ptr<GpuMesh> droplet;
    uint32_t mist_texture = 0;
    uint32_t flare_glow_texture = 0;
    uint32_t contact_texture = 0;
    uint32_t car_contact_texture = 0;
    std::unique_ptr<GpuMesh> hole;
    std::unique_ptr<GpuMesh> spark;
    std::unique_ptr<GpuMesh> loader;
    std::unique_ptr<GpuMesh> loader_empty;
    std::unique_ptr<GpuMesh> rounds;
    const GpuMesh* chest_light = nullptr;
    glm::vec3 chest_lens{0.0f, 0.022f, 0.034f};
    glm::vec3 chest_clip{0.0f, -0.02f, -0.0182f};
    glm::vec3 chest_mount{0.085f, 0.15f, 0.115f};
    glm::vec3 chest_turn{0.0f};
    std::vector<SurvivalGunModel::SkinMaterial> chest_materials;
    uint32_t lens_material = 0;
    MaterialGpu lens_original{};
    std::unique_ptr<GpuMesh> lens_glow;
    uint32_t mythic_vfx_material = 0;
    std::unique_ptr<GpuMesh> mythic_mote;
    std::unique_ptr<GpuMesh> mythic_bar;
    bool armory_built = false;
    bool full_built = false;
    uint8_t armory_slots_loaded = 0;
    std::unique_ptr<GpuMesh> backdrop;
    struct Block {
        glm::mat4 to_local{1.0f};
        glm::vec3 half{0.0f};
    };
    std::vector<Block> backdrop_blocks;
    bool BuildArmorySlot(ModelCache& models, int slot);
    bool BuildArmory(ModelCache& models);
    bool Build(ModelCache& models);
    std::vector<std::unique_ptr<GpuMesh>> owned;
};

inline constexpr float kHouseFront = 21.5f;
inline constexpr float kHouseDoorX = -0.6f;
inline constexpr float kHouseDoorHalf = 0.41f;
inline constexpr float kHouseDoorHeight = 2.02f;
inline constexpr float kHouseDoorSill = 0.16f;
inline constexpr glm::vec2 kDoorwayX{-1.19f, -0.01f};
inline constexpr float kDoorwayMouth = 22.28f;
inline constexpr float kDoorwayTop = 2.49f;
inline constexpr float kPlinthFront = 22.54f;
inline constexpr float kTowerFront = 23.1f;
inline constexpr float kTowerBack = 12.0f;
inline constexpr glm::vec2 kStreetLines{-19.5f, 13.5f};

struct StreetTower {
    const GpuMesh* base = nullptr;
    const GpuMesh* storey = nullptr;
    const GpuMesh* top = nullptr;
    int storeys = 0;
    float storey_height = 0.0f;
};
struct StreetHouseMeshes {
    StreetTower tower;
    std::unique_ptr<GpuMesh> doorway;
    std::unique_ptr<GpuMesh> house;
    std::unique_ptr<GpuMesh> door;
    std::unique_ptr<GpuMesh> backdrop;
};
bool BuildStreetHouse(ModelCache& models, int floors, StreetHouseMeshes& out);

struct CityBox {
    glm::mat4 place;
    glm::vec3 half;
};
std::unique_ptr<GpuMesh> BuildCityShell(ModelCache& models, const std::vector<CityBox>& boxes);
std::unique_ptr<GpuMesh> BuildCityGround(ModelCache& models, uint32_t road_material, float road_tile);

}
