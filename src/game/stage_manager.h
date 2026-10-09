#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/assets/ftex.h"
#include "engine/assets/geom.h"
#include "engine/fs/vfs.h"
#include "engine/physics/collision_world.h"
#include "engine/render/model_cache.h"
#include "engine/render/scene_renderer.h"
#include "game/stage_data.h"

namespace pt::game {

struct BodyState {
    bool enable = true;
    bool visible = true;
    bool geom_active = true;
};

struct SurfaceTriangle {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    glm::vec3 c{0.0f};
    uint32_t material = 0;
};

struct StageScript {
    std::string path;
    std::vector<uint8_t> code;
};

struct Stage {
    uint32_t id = 0;
    std::string label;
    std::string package_path;
    bool resident = false;
    bool active = false;
    std::shared_ptr<FoxPackage> fpk;
    std::shared_ptr<FoxPackage> fpkd;
    std::vector<std::unique_ptr<StageData>> files;
    StageData* main = nullptr;
    std::vector<StageScript> scripts;
    glm::mat4 file_root{1.0f};
    glm::mat4 runtime_root{1.0f};
    glm::mat4 file_to_world{1.0f};

    struct Draw {
        const StageData* file = nullptr;
        const fox2::Entity* entity = nullptr;
        const GpuMesh* mesh = nullptr;
        glm::mat4 file_transform{1.0f};
        glm::vec4 color{1.0f};
    };
    std::vector<Draw> draws;

    struct CollisionPiece {
        const fox2::Entity* entity = nullptr;
        std::vector<GeomTriangle> triangles;
        std::shared_ptr<const std::vector<GeomTriangle>> surfaces;
        // the triangles the camera's line checks see (tags & 0x700, StageManager::LineChecks)
        std::shared_ptr<const std::vector<GeomTriangle>> lines;
        // every detailed shape over the model's positions, for the street walk (StageManager::PrepareWalkSurfaces)
        std::shared_ptr<const std::vector<GeomTriangle>> walk;
        std::string geom_file;
        std::string model_file;
        glm::mat4 file_transform{1.0f};
        // StageManager::BuildCollision's world space copies of the above for the transform they were made with: a rebuild after a
        // geom toggle or a stage move only copies them, where it transformed every triangle of every stage (a loop change in
        // the hallway rebuilt up to 380,000 triangles several times within a few frames)
        struct Cache {
            bool valid = false;
            glm::mat4 transform{1.0f};
            std::string owner_label;
            std::string owner;
            std::vector<CollisionTriangle> world;
            std::vector<CollisionTriangle> lines;
            std::vector<CollisionTriangle> reflections;
            std::vector<SurfaceTriangle> surfaces;
        };
        mutable Cache cache;
    };
    std::vector<CollisionPiece> collision;
    // the ending street walk (Game::StartStreetWalk): the street has no movement hull (geom node 0), the player never walked it in
    // the original; the detailed surfaces of node 1 are walked instead (StageManager::PrepareWalkSurfaces), inside invisible walls
    // that come as one more collision piece without an entity
    bool walk_detail_surfaces = false;

    glm::mat4 ToWorld(const glm::mat4& file_space) const { return file_to_world * file_space; }
    bool ConnectorWorld(const std::string& name, glm::mat4& out) const;
    const StageData* FileOf(const fox2::Entity* entity) const;
    BodyState& Body(const fox2::Entity* entity);
    const BodyState* FindBody(const fox2::Entity* entity) const;

private:
    std::unordered_map<const fox2::Entity*, BodyState> bodies_;
};

class StageManager {
public:
    StageManager(Vfs& vfs, ModelCache& models);
    ~StageManager();

    Stage* LoadResident(const std::string& fpk_path);
    void RequestLocation(const std::string& fpk_path);
    void RequestLoad(const std::string& fpk_path, const std::string& label, const std::string& connector, const std::string& base_label,
                     const std::string& base_connector);
    void RequestActivate(const std::string& label);
    void RequestUnload(const std::string& label);
    void Update();

    Stage* SetNextStageByPath(const std::string& fpk_path);
    Stage* LoadStage(const std::string& fpk_path, const std::string& label, const std::string& connector, const std::string& base_label,
                     const std::string& base_connector);
    bool ActivateStage(const std::string& label);
    bool DeactivateStage(const std::string& label);
    bool ChangeStageId(const std::string& from, const std::string& to);
    bool UnloadStage(const std::string& label);
    void UnloadAll();
    bool IsActive(const std::string& label) const;
    bool IsLoadedInactive(const std::string& label) const;
    bool IsAllUnloaded() const { return stages_.empty() && !pending_location_; }
    static constexpr size_t kSlots = 3;

    Stage* Find(const std::string& label);
    Stage* FindById(uint32_t id);
    Stage* Resident() { return resident_.get(); }
    bool IsLoaded(const std::string& label) const { return stages_.contains(label); }
    bool Empty() const { return stages_.empty(); }
    const std::map<std::string, std::unique_ptr<Stage>>& Stages() const { return stages_; }
    void ForEachStage(const std::function<void(Stage&)>& fn, bool include_resident = true);

    uint64_t Generation() const { return generation_; }
    uint64_t CollisionGeneration() const { return collision_generation_; }
    // geom toggles only (BodyState::geom_active): UpdateCollisionActive, not a rebuild
    uint64_t GeomGeneration() const { return geom_generation_; }
    void MarkVisualsDirty() { ++generation_; }
    void MarkCollisionDirty() {
        ++collision_generation_;
        collision_deferrable_ = false;
    }
    void MarkGeomDirty() { ++geom_generation_; }

    // the street walk: loads every shape of the stage's collision with its model's positions (node 1 included), so the detailed
    // surfaces that have no movement hull can be walked; the stage's walk_detail_surfaces then adds them to the player's world
    void PrepareWalkSurfaces(Stage& stage, std::vector<GeomTriangle> bounds);
    void CollectDraws(std::vector<DrawItem>& out) const;
    void CollectLights(std::vector<std::pair<const Stage*, const LightPlacement*>>& out) const;
    // `lines`, when given, gets the line check triangles of the same pieces (the focus ray, RenderSceneBuilder::FocusDistance)
    // Every piece of every stage goes in, the pieces whose body has geom off as inactive owners (CollisionWorld::SetOwnerActive)
    void BuildCollision(CollisionWorld& world, std::vector<SurfaceTriangle>* surfaces = nullptr, CollisionWorld* lines = nullptr,
                        CollisionWorld* reflections = nullptr);
    // After geom toggles: the owners of the last BuildCollision switched to their bodies' geom_active and the material surfaces
    // of the active pieces collected again, in the order a rebuild gives (the stages and pieces stay the same until the next
    // MarkCollisionDirty)
    void UpdateCollisionActive(CollisionWorld& world, std::vector<SurfaceTriangle>* surfaces = nullptr, CollisionWorld* lines = nullptr,
                               CollisionWorld* reflections = nullptr) const;
    // True while every change since the last build is the preload of an inactive stage (LoadStage, the next hallway copy loaded at
    // the clock): its worlds can be built on a worker (StartCollisionBuild) and taken a few ticks later (FinishCollisionBuild),
    // while the player is still a corridor away from the new stage
    bool CollisionDeferrable() const { return collision_deferrable_; }
    void StartCollisionBuild();
    bool CollisionBuildRunning() const { return pending_collision_ != nullptr; }
    void FinishCollisionBuild(CollisionWorld& world, std::vector<SurfaceTriangle>* surfaces, CollisionWorld& lines, CollisionWorld& reflections);

    std::function<void(Stage&)> on_loaded;
    std::function<void(Stage&)> on_unloading;

private:
    std::unique_ptr<Stage> Load(const std::string& fpk_path, const std::string& label);
    // The part of Load that reads the packages and parses their data sets: it touches nothing of the manager's, so the next
    // hallway copy's is parsed on a worker after each hallway load (StartPrefetch) and Load takes it when the same package is
    // loaded next; the parse was most of the 23 ms tick at the clock. Errors are logged by Load.
    // A prefetch also reads ahead what Load would read on the main thread: the movement hull of every .geom, and the models the
    // cache does not hold (`known` at the start of the prefetch) with their textures unpacked. The f100 clock's maze package
    // brings 7 new models whose texture reads alone took 42 ms of the main thread.
    // For a .geom new to the caches, its surfaces and line checks too (Surfaces, LineChecks).
    struct PreGeom {
        std::vector<uint8_t> bytes;
        std::vector<GeomTriangle> hull;
        bool skipped = false;
        bool derived = false;
        std::shared_ptr<const std::vector<GeomTriangle>> surfaces;
        std::shared_ptr<const std::vector<GeomTriangle>> lines;
    };
    struct ParsedStage {
        std::unique_ptr<Stage> stage;
        std::vector<std::string> errors;
        std::unordered_map<std::string, std::shared_ptr<const PreGeom>> geoms;
        std::unordered_map<std::string, std::shared_ptr<FmdlModel>> models;
        std::vector<std::pair<std::string, std::shared_ptr<FtexTexture>>> textures;
    };
    struct KnownAssets {
        std::unordered_set<std::string> models;
        std::unordered_set<std::string> textures;
        std::unordered_set<std::string> geoms;
    };
    static ParsedStage Parse(Vfs& vfs, const std::string& fpk_path, const KnownAssets* known = nullptr);
    uint32_t load_count_ = 0;
    std::string deferred_prefetch_;
    void StartPrefetch(const std::string& fpk_path);
public:
    void DropPrefetch();
    // On a floor change: the loaded copies' ShTrapExecLoadStage conditions load targetMazeFpk instead of targetFpk when the floor
    // is their nextMazeFloor (f100 loads hallway_maze_A at its clock), so that package is parsed ahead instead of another hallway
    void PrefetchMazeFor(std::string_view floor);
    uint32_t LoadCount() const { return load_count_; }
    // false while the machine is low on memory (main.cpp's memory guard): no parse is kept ahead
    void SetPrefetchAllowed(bool allowed) {
        prefetch_allowed_ = allowed;
        if (!allowed) {
            DropPrefetch();
        }
    }
private:
    std::shared_ptr<const std::vector<GeomTriangle>> Surfaces(const std::string& geom_file, const std::string& model_file,
                                                              std::span<const uint8_t> geom, const std::vector<GeomTriangle>& hull,
                                                              bool skipped_fmdl_materials);
    std::shared_ptr<const std::vector<GeomTriangle>> LineChecks(const std::string& geom_file, std::span<const uint8_t> geom, const ModelEntry* model);
    // the uncached work of Surfaces and LineChecks, also run by a prefetch (`model`: the FMDL already parsed, else it is read)
    static std::shared_ptr<const std::vector<GeomTriangle>> BuildSurfaces(Vfs& vfs, const std::string& model_file, std::span<const uint8_t> geom,
                                                                          const std::vector<GeomTriangle>& hull, bool skipped_fmdl_materials,
                                                                          const FmdlModel* model);
    static std::shared_ptr<const std::vector<GeomTriangle>> BuildLines(std::span<const uint8_t> geom, std::span<const glm::vec3> positions);
    void Place(Stage& stage, const glm::mat4& runtime_root);
    void Unload(std::unique_ptr<Stage> stage);
    // a worker build reads the stages: it is joined and dropped before any of them moves, goes or is renamed
    void DropCollisionBuild();

    // the stage and piece of each owner in the current worlds, with the owner ids
    struct CollisionOwners {
        const Stage* stage = nullptr;
        const Stage::CollisionPiece* piece = nullptr;
        uint32_t world = 0;
        uint32_t line = 0;
        uint32_t reflection = 0;
    };
    struct CollisionEntry {
        const Stage* stage = nullptr;
        std::string label;
        const Stage::CollisionPiece* piece = nullptr;
    };
    std::vector<CollisionEntry> CollisionEntries() const;
    static void BuildWorlds(const std::vector<CollisionEntry>& entries, CollisionWorld& world, CollisionWorld* lines, CollisionWorld* reflections,
                            std::vector<CollisionOwners>& owners);
    struct PendingCollision;

    struct PendingLoad {
        std::string fpk_path;
        std::string label;
        std::string connector;
        std::string base_label;
        std::string base_connector;
    };

    Vfs& vfs_;
    ModelCache& models_;
    std::optional<std::string> pending_location_;
    std::optional<PendingLoad> pending_load_;
    std::optional<std::string> pending_activate_;
    std::optional<std::string> pending_unload_;
    std::map<std::string, std::unique_ptr<Stage>> stages_;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<GeomTriangle>>> surface_cache_;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<GeomTriangle>>> line_cache_;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<GeomTriangle>>> walk_cache_;
    std::unique_ptr<Stage> resident_;
    uint32_t next_id_ = 1;
    uint64_t generation_ = 1;
    uint64_t collision_generation_ = 1;
    uint64_t geom_generation_ = 0;
    // the collision generation the current or the pending worlds were built from
    uint64_t built_generation_ = 0;
    std::vector<CollisionOwners> collision_owners_;
    std::unique_ptr<PendingCollision> pending_collision_;
    bool collision_deferrable_ = false;
    bool prefetch_allowed_ = true;
    std::string prefetch_path_;
    std::future<ParsedStage> prefetch_;
};

}
