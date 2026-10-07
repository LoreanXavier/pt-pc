#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/platform/input.h"
#include "engine/render/camera.h"
#include "engine/render/scene_renderer.h"
#include "game/save_data.h"

namespace pt {
class Vfs;
class ModelCache;
}

namespace pt::game {

class Game;
class GameSound;
class InputScript;
struct ArchiveEntry;
struct Stage;

class ArchiveTheater {
public:
    struct Settings {
        bool sound = false;
        bool open_device = false;
        float volume = 1.0f;
        float demo_rate = 1.0f;
        GameOptions options;
        float model_ev = -5.5f;
    };

    ArchiveTheater(Vfs& vfs, ModelCache& models, const ArchiveEntry& entry, const Settings& settings);
    ~ArchiveTheater();
    ArchiveTheater(const ArchiveTheater&) = delete;
    ArchiveTheater& operator=(const ArchiveTheater&) = delete;

    bool Start();
    void Update(float dt, const InputState& input);
    bool Finished() const { return phase_ == Phase::Done; }
    void Stop();
    Game& Sandbox() { return *game_; }
    GameSound* Sound() { return sound_.get(); }
    const ArchiveEntry& Entry() const { return entry_; }
    bool Showing() const;
    bool ModelView() const { return model_mode_; }
    Camera ViewCamera() const;
    void CollectDraws(std::vector<DrawItem>& out);

private:
    enum class Kind { Model, Preface, Opening, Room, Hallway, Kill, Ending, Teaser };
    enum class Phase { Boot, Walk, Settle, Playing, Model, Done };

    void StartWalk();
    void Present();
    void UpdateAim(float dt);
    void StartModel();
    void UpdateModel(float dt, const InputState& input);
    bool FindPlacement(const std::string& demo, glm::mat4& transform, glm::vec3* viewpoint);
    void Finish(const char* why);

    Vfs& vfs_;
    ModelCache& models_;
    const ArchiveEntry& entry_;
    Settings settings_;
    Kind kind_ = Kind::Model;
    Phase phase_ = Phase::Boot;
    std::unique_ptr<Game> game_;
    std::unique_ptr<GameSound> sound_;
    std::unique_ptr<InputScript> script_;
    uint64_t frame_ = 0;
    int browse_index_ = -1;
    bool prepared_ = false;
    int settle_ticks_ = 0;
    bool demo_seen_ = false;
    double phase_time_ = 0.0;
    bool teaser_fast_ = false;
    bool aim_ = false;
    bool aim_started_ = false;
    glm::vec3 look_target_{0.0f};
    bool model_mode_ = false;
    bool gimmick_ = false;
    int gimmick_type_ = -1;
    const void* model_mesh_ = nullptr;
    std::vector<DrawItem> model_draws_;
    glm::vec3 spot_{0.0f};
    glm::mat4 model_offset_{1.0f};
    float spot_yaw_ = 0.0f;
    glm::vec3 target_{0.0f};
    float yaw_ = 0.0f;
    float pitch_ = -0.1f;
    float distance_ = 2.5f;
    float min_distance_ = 0.5f;
    float max_distance_ = 6.0f;
};

}
