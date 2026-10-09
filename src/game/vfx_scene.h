#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/anim/skeleton.h"
#include "engine/render/camera.h"
#include "engine/render/scene_lighting.h"
#include "engine/vfx/vfx_file.h"
#include "engine/vfx/vfx_system.h"
#include "game/gimmick_animation.h"

namespace pt {
class VfxPass;
namespace fox2 {
struct Entity;
class DataSetFile;
}
}

namespace pt::game {

class Game;
struct Stage;

class VfxScene {
public:
    VfxScene();
    ~VfxScene();

    void Update(Game& game, float dt);
    // `blend`: the effects between their places before the last tick (SnapshotWorlds) and their current ones, as the camera
    void Prepare(Game& game, const Camera& camera, float aspect, SceneLighting& lighting, VfxPass& pass, float blend = 1.0f);
    void SnapshotWorlds() { system_.SnapshotWorlds(); }
    void Clear();

    vfx::System& System() { return system_; }
    size_t Instances() const { return system_.InstanceCount(); }
    size_t Particles() const { return system_.ParticleCount(); }

private:
    struct Locator {
        const fox2::Entity* entity = nullptr;
        const fox2::DataSetFile* file = nullptr;
        std::string path;
        glm::mat4 file_transform{1.0f};
        std::optional<uint32_t> seed;
        bool failed = false;
    };

    struct StageEntry {
        std::vector<Locator> locators;
        bool has_wind = false;
        glm::vec3 wind{0.0f};
        bool active = false;
    };

    struct PartConnection {
        bool cnp = false;
        std::string target;
        glm::vec3 offset{0.0f};
        glm::vec4 general{0.0f};
        uint32_t kind = 0;
    };

    struct PartEffect {
        std::string name;
        std::string file;
        uint32_t seed = 0;
        std::vector<PartConnection> connections;
        bool on = false;
        // the effect's FxSoundCallProgramEffectNode: posted when an instance starts, released when it goes (0xB6E050, 0xB6E0F0)
        std::optional<vfx::SoundNode> sound;
        uint32_t sound_id = 0;
        glm::vec3 sound_at{0.0f};
    };

    void EndPartSound(Game& game, PartEffect& part);

    struct GimmickEntry {
        std::string parts;
        anim::Skeleton skeleton;
        std::vector<ConnectPoint> connect_points;
        std::vector<PartEffect> effects;
        bool retrigger = false;
        float timer = 0.0f;
    };

    void UpdateSystems(Game& game, float dt);
    void PrepareList(Game& game, const Camera& camera, float aspect, SceneLighting& lighting, VfxPass& pass, float blend);
    void SyncStages(Game& game);
    void SyncDemos(Game& game);
    void SyncGimmicks(Game& game);
    void SyncTest(Game& game);
    glm::vec3 Wind(Game& game) const;
    void LoadGimmickParts(Game& game, size_t index, const std::string& parts);
    bool PartMatrix(Game& game, size_t index, const PartConnection& c, glm::mat4& out) const;
    vfx::ViewInfo View(const Camera& camera, float aspect) const;

    vfx::System system_;
    std::unordered_map<uint32_t, StageEntry> stages_;
    std::array<GimmickEntry, 5> gimmicks_;
    std::set<vfx::InstanceKey> demo_keys_;
    std::map<vfx::InstanceKey, std::string> demo_key_demo_;
    // effects of newly loaded stages and gimmicks, whose textures the next Prepare loads ahead of their first draw
    std::vector<std::string> preload_;
    bool reader_set_ = false;
    bool test_spawned_ = false;
    uint64_t logged_frame_ = 0;
    // the vfx list built each frame, swapped with VfxPass's (VfxPass::Submit), so the frames reuse two lists
    vfx::RenderList render_list_;
    // the last PrepareList's preload and build times, for the slow prepare log
    double preload_ms_ = 0.0;
    double build_ms_ = 0.0;
    uint64_t prepared_ = 0;
    uint64_t updates_ = 0;
    uint64_t prepares_ = 0;
    double update_seconds_ = 0.0;
    double prepare_seconds_ = 0.0;
};

}
