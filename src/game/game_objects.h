#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <vector>

namespace pt {
struct GpuMesh;
struct DrawItem;
}

namespace pt::game {

class Game;
class StageManager;

using CommandValue = std::variant<bool, double, std::string>;

struct GameCommand {
    std::string id;
    std::map<std::string, CommandValue, std::less<>> args;

    std::optional<bool> Bool(std::string_view key) const;
    std::optional<double> Number(std::string_view key) const;
    std::optional<std::string> String(std::string_view key) const;
    std::string Describe() const;
};

enum class GimmickType : uint8_t { Baby = 0, Ocho = 1, CeilLamp = 2, Freezer = 3, Bag = 4, Count = 5 };

struct Gimmick {
    GimmickType type = GimmickType::Baby;
    std::string name;
    std::string parts_path;
    std::string model_file;
    const GpuMesh* mesh = nullptr;
    bool enabled = false;
    bool shown = false;
    bool placed = false;
    bool active = false;
    uint32_t stage_id = 0;
    glm::mat4 world{1.0f};
    glm::mat4 locator{1.0f};
    std::string motion;
    float motion_time = 0.0f;
    bool return_to_idle = false;
    bool motion_requested = false;
    bool silent_logged = false;
    float anim_rate = 1.0f;
    std::string logic_state = "None";
    bool lights[3] = {false, false, false};
    bool stage_light = false;
    bool stage_light_red = false;
    bool freezer_strong = false;
    float freezer_timer = 0.0f;
    float freezer_interval = 0.0f;
    uint8_t hidden_views = 0;
    std::set<uint64_t> hidden_meshes;
    std::set<uint64_t> parts_hidden_meshes;
    std::string sound_cnp;
    std::vector<uint32_t> sound_handles;
};

void SetFixedRandomSeed(uint32_t seed);
uint32_t ReadTsc();

class OchoLogic {
public:
    explicit OchoLogic(Game& game) : game_(game) {}

    void ResetState();
    void LogicControl(int state);
    void Update(float dt);
    int State() const { return state_; }
    int LookPhase() const { return look_phase_; }
    bool Visible() const { return visible_; }
    bool HasKilled() const { return has_killed_; }
    void Setup();
    void RelocateForReset();
    const glm::mat4& World() const { return world_; }
    void ApplyRootMotion(const glm::vec3& translation, const glm::quat& rotation);
    int SpawnIndex() const { return spawn_; }
    int WarpPhase() const { return warp_phase_; }
    float TimerA() const { return timer_a_; }

private:
    struct Spawn {
        glm::vec3 world{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        float radius = 4.0f;
        bool variant = false;
        bool detectable = false;
    };

    void Sense(float dt);
    void BuildSpawns();
    void UpdateWarp();
    void UpdateChase();
    bool UpdateLookBack();
    void Kill();
    void Appear();
    int ChooseSpawn();
    bool Detect() const;
    void PlaceAt(const glm::vec3& position, const glm::quat& rotation);
    void Show(bool visible);
    void SelectSpawnViews();
    void StopVoice();
    uint32_t Random();

    Game& game_;
    std::array<Spawn, 9> spawns_{};
    int state_ = 0;
    int spawn_ = 4;
    int voice_ = 0;
    int warp_phase_ = 0;
    float timer_a_ = 0.0f;
    float timer_b_ = 0.0f;
    float wait_ = 40.0f;
    float visible_duration_ = 30.0f;
    bool visible_ = false;
    bool appear_request_ = false;
    bool has_killed_ = false;
    float dash_step_ = 0.0f;
    std::array<float, 180> ring_{};
    uint32_t ring_count_ = 0;
    int look_phase_ = 0;
    uint32_t steps_sound_ = 0;
    glm::vec3 steps_origin_{0.0f};
    glm::vec3 steps_position_{0.0f};
    uint32_t voice_sound_ = 0;
    uint32_t herald_sound_ = 0;
    glm::mat4 world_{1.0f};
    glm::vec3 player_pos_{0.0f};
    glm::vec3 player_forward_{0.0f, 0.0f, 1.0f};
    glm::quat player_rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    float player_yaw_ = 0.0f;
    float frame_dt_ = 0.0f;
    glm::vec3 dash_rest_{0.0f};
    int dash_frames_ = 0;
    void UpdateDash(float frame_dt);
    glm::vec3 ocho_pos_{0.0f};
    float distance2_ = 0.0f;
    float facing_ = 0.0f;
    uint32_t rng_ = 0x2545F491u;
};

class GameObjects {
public:
    static constexpr uint32_t kNullId = 65535;
    static constexpr uint32_t kPlayerId = 0x200;
    static constexpr uint32_t kGimmickBase = 0xA00;

    explicit GameObjects(Game& game);

    uint32_t GetId(std::string_view type, std::string_view name) const;
    bool SendCommand(uint32_t id, const GameCommand& command);

    Gimmick* FindGimmick(std::string_view name);
    Gimmick& GetGimmick(GimmickType type) { return gimmicks_[static_cast<size_t>(type)]; }
    const std::array<Gimmick, static_cast<size_t>(GimmickType::Count)>& Gimmicks() const { return gimmicks_; }
    OchoLogic& Ocho() { return ocho_; }

    void Reset();
    void ResetToLocators();
    void RelocateGimmicks(StageManager& stages);
    void SetGimmickLight(GimmickType type, size_t index, bool on);
    void SetGimmickEnabled(GimmickType type, bool enabled);
    void PlayGimmickMotion(GimmickType type, std::string_view key, bool return_to_idle);
    void GimmickLogicControl(GimmickType type, std::string_view state);
    void GimmickLogicControl(GimmickType type, int state);
    void Update(float dt);
    void AddPartsPath(std::string_view part, std::string_view path);
    void AddMotionPath(std::string_view key, std::string_view path);
    const std::string* MotionPath(std::string_view key) const;
    void ResolveModels();
    void CollectDraws(std::vector<DrawItem>& out) const;
    void DrawnCharacters(std::vector<glm::vec3>& feet) const;
    bool GimmickDrawn(std::string_view name) const;
    void SetOchoTransform(const glm::mat4& world);
    void ShowOcho(bool visible);
    void HideRecordsWithoutLocator();
    const glm::vec3& DialoguePosition() const { return dialogue_position_; }
    const glm::vec3& DialogueForward() const { return dialogue_forward_; }

private:
    bool PlayerCommand(const GameCommand& command);
    bool GimmickCommand(Gimmick& gimmick, const GameCommand& command);
    void UpdateFreezer(Gimmick& g, float dt);
    void UpdateInView(Gimmick& g, float dt);
    void UpdateOchoRootMotion();
    void MotionStarted(const Gimmick& g);
    glm::vec3 SoundPosition(const Gimmick& g) const;
    uint32_t Random();

    Game& game_;
    std::array<Gimmick, static_cast<size_t>(GimmickType::Count)> gimmicks_{};
    std::map<std::string, std::string, std::less<>> parts_paths_;
    std::map<std::string, std::string, std::less<>> motion_paths_;
    OchoLogic ocho_;
    std::string ocho_root_motion_;
    float ocho_root_time_ = 0.0f;
    glm::vec3 ocho_root_translation_{0.0f};
    glm::quat ocho_root_rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    bool ocho_root_valid_ = false;
    glm::vec3 dialogue_position_{0.0f};
    glm::vec3 dialogue_forward_{0.0f, 0.0f, 1.0f};
    uint32_t rng_ = 0x6C078965u;
    uint32_t archive_noted_ = 0;
};

std::string_view GimmickName(GimmickType type);

}
