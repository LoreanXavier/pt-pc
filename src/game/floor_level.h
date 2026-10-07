#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace pt::game {

class Game;

enum class NazoId : int { XMark = 0, Hello = 1, Peephole = 2, Photo = 3, TrueEnd = 4 };

class FloorLevel {
public:
    static constexpr int kTableSize = 31;
    static constexpr float kFallSafetyY = -70.0f;
    static constexpr double kRandomEventChance60 = 9.259259e-06;
    static constexpr double kRandomEventChance30 = 1.8518518e-05;

    explicit FloorLevel(Game& game);

    void ResetLoopCount();
    void ProcessPending();
    void Update(float dt);
    void AddFloorLevel();
    void SubFloorLevel();
    void SetPending(int value) { pending_ = value; }
    void NextFloor();
    void OnStartGame();
    void RelocateGimmicks();
    void ResetBaby();
    void ResetOcho();
    void GoNextFloor(bool save = true);

    bool IsCurrentFloorName(std::string_view name) const;
    const std::string& CurrentFloorName() const { return table_[index_]; }
    int Index() const { return index_; }
    void SetIndex(int index);
    bool SetFloorLevel(std::string_view name);
    void SetFloorLevelAndName(int index, std::string_view name);
    int IndexOf(std::string_view name) const;
    int LoopCount() const { return loop_count_; }
    void SetLoopCount(int count) { loop_count_ = count; }
    const std::string& SaveFloorName() const { return save_floor_name_; }
    void SetSaveFloorName(std::string_view name) { save_floor_name_ = std::string(name); }
    void SeedRandom(uint32_t seed) { rng_ = seed ? seed : 0x9E3779B9u; }
    const glm::mat4& StageTransform() const { return stage_transform_; }
    void SetStageTransform(const glm::mat4& transform) { stage_transform_ = transform; }
    bool LisaKilled() const { return lisa_killed_; }
    int Pending() const { return pending_; }
    void SetLisaKilled(bool killed) { lisa_killed_ = killed; }

private:
    uint32_t NextRandom();
    void RandomEvents(float dt);

    Game& game_;
    std::array<std::string, kTableSize> table_{};
    int index_ = 0;
    int loop_count_ = 1;
    int pending_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
    std::string save_floor_name_ = "f000";
    glm::mat4 stage_transform_{1.0f};
    bool lisa_killed_ = false;
};

}
