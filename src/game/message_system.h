#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace pt::fox2 {
struct Entity;
}

namespace pt::game {

class Game;
struct Stage;
struct MessageScript;

class MessageSystem {
public:
    explicit MessageSystem(Game& game) : game_(game) {}

    void PostControllerMessage(std::string_view message);
    void PostDemoMessage(std::string_view demo_id, std::string_view message);
    void Dispatch();
    void Clear() { queue_.clear(); }
    bool Pending() const { return !queue_.empty(); }

    void RegisterStage(Stage& stage);
    void ForgetStage(const Stage& stage);
    void SetupMessageBox(Stage& stage, const fox2::Entity& entity);
    bool IsRegistered(uint32_t stage_id, const fox2::Entity* entity) const { return registered_.contains({stage_id, entity}); }

private:
    struct Message {
        bool from_demo = false;
        std::string demo_id;
        std::string name;
        double ready = 0.0;
    };

    void Deliver(const Message& message);
    bool TryRegister(Stage& stage, const MessageScript& script);

    Game& game_;
    std::deque<Message> queue_;
    std::set<std::pair<uint32_t, const fox2::Entity*>> registered_;
};

}
