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

// The Archive's theater (docs/gameplay.md 14.7): a cutscene or a model of the Archive shown in a game session of its own, a second
// Game over the same loaded data, while the player's game stays paused behind the menu that opened it. Nothing of the player's
// session is read or written: the theater's game has no save, its traps and Lisa's logic stop once its demo is placed, and it is
// thrown away when the viewer ends.
//
// Cutscenes play as the game plays them, with their sound, subtitles, lights and the scripts that answer their messages: the
// preface and the openings run the boot's own steps; a hallway demo enters its loop through the start room's door as a loop browser
// pick does (Game::PrepareTheaterLoop, the walk of the loop browser's previews), then plays at the place the level data gives it (the
// trap or script that starts it in play, its demo center locator) with the player in that trap when the demo has no camera; the
// ending loads the ending as the f160 exit does. Models stand at the start room's player start in the room's own light, alone in
// the picture: a gimmick in its own motion, any other model as its file holds it.
class ArchiveTheater {
public:
    struct Settings {
        // a sound system of its own (the player's is paused with the menu); with a device when the game has a window
        bool sound = false;
        bool open_device = false;
        float volume = 1.0f;
        // tests: --demo-rate
        float demo_rate = 1.0f;
        // the player's subtitle and language options
        GameOptions options;
        // the model viewer's held exposure (ScreenEffects::pinned_ev): the room's lit surfaces as in play; the Museum's thumbnail
        // capture (main.cpp, kMuseumPreviewModelEv) holds it higher, since its shots of the dark start room read near black
        float model_ev = -5.5f;
    };

    ArchiveTheater(Vfs& vfs, ModelCache& models, const ArchiveEntry& entry, const Settings& settings);
    ~ArchiveTheater();
    ArchiveTheater(const ArchiveTheater&) = delete;
    ArchiveTheater& operator=(const ArchiveTheater&) = delete;

    bool Start();
    // one game tick; `input` is the player's (the model viewer turns with it)
    void Update(float dt, const InputState& input);
    bool Finished() const { return phase_ == Phase::Done; }
    // the viewer is left (the menu's back)
    void Stop();
    Game& Sandbox() { return *game_; }
    GameSound* Sound() { return sound_.get(); }
    const ArchiveEntry& Entry() const { return entry_; }
    // something is on screen; before that (the session loading) the picture is black
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
    // a demo without a camera: the view turns to what it moves (UpdateAim)
    bool aim_ = false;
    bool aim_started_ = false;
    glm::vec3 look_target_{0.0f};
    // the model viewer
    bool model_mode_ = false;
    bool gimmick_ = false;
    int gimmick_type_ = -1;
    const void* model_mesh_ = nullptr;
    std::vector<DrawItem> model_draws_;
    glm::vec3 spot_{0.0f};
    // a gimmick's offset from the spot: its bounds' foot on the floor
    glm::mat4 model_offset_{1.0f};
    float spot_yaw_ = 0.0f;
    glm::vec3 target_{0.0f};
    float yaw_ = 0.0f;
    float pitch_ = -0.1f;
    float distance_ = 2.5f;
    glm::vec3 model_home_{0.0f};
    float model_radius_ = 0.5f;
    float min_distance_ = 0.5f;
    float max_distance_ = 6.0f;
};

}
