#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "engine/data/fox2.h"

namespace pt::game {

class Game;
struct Stage;
struct StageData;
struct TrapPlacement;
struct TrapCondition;
struct TrapCallback;

enum class TrapFlag { None = 0, Enter = 1, Out = 2, Inside = 4 };

const char* TrapFlagString(TrapFlag flag);

class TrapSystem {
public:
    explicit TrapSystem(Game& game) : game_(game) {}

    void Update(float dt);
    void ForgetStage(const Stage& stage);
    void DumpTraps(const Stage& stage) const;
    bool debug_log = false;
    bool big_zoom = false;

private:
    struct ConditionState {
        TrapFlag flag = TrapFlag::None;
        bool done = false;
    };
    struct ElementState {
        bool was_in_view = false;
        bool last_logged = false;
        float timer = 0.0f;
        bool done = false;
    };
    struct Key {
        uint32_t stage_id;
        const void* entity;
        bool operator==(const Key& o) const { return stage_id == o.stage_id && entity == o.entity; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return std::hash<const void*>()(k.entity) ^ (static_cast<size_t>(k.stage_id) << 1); }
    };
    struct Context {
        Stage* stage = nullptr;
        const StageData* file = nullptr;
        const TrapPlacement* trap = nullptr;
        const TrapCondition* condition = nullptr;
        TrapFlag flag = TrapFlag::None;
        float dt = 0.0f;
    };

    bool PlayerInside(const Stage& stage, const TrapPlacement& trap) const;
    bool RunChecks(const Context& ctx);
    bool Check(const Context& ctx, const TrapCallback& callback);
    int RunExecs(const Context& ctx);
    int ExecDemo(const Context& ctx, const fox2::Entity& element);
    int ExecDoor(const Context& ctx, const fox2::Entity& element);
    int ExecNazo(const Context& ctx, const fox2::Entity& element);
    float TrapYaw(const Context& ctx) const;
    bool TargetInView(const Context& ctx, const fox2::Entity* locator, float area, glm::vec3* position) const;
    bool ButtonPressed(const std::string& button) const;
    bool GougePressed() const;

    Game& game_;
    std::unordered_map<Key, bool, KeyHash> traps_;
    std::unordered_map<Key, ConditionState, KeyHash> conditions_;
    std::unordered_map<Key, ElementState, KeyHash> elements_;
};

}
