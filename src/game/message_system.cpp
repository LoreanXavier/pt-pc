#include "game/message_system.h"

#include "engine/core/log.h"
#include "game/game.h"

namespace pt::game {
namespace {

constexpr double kGameFrame = 1.0 / 30.0;

bool FloorMatches(Game& game, const MessageScript& script) {
    if (!script.order_floor) {
        return true;
    }
    // 0x91B0E0 subscribes only when the current floor is in floorNames, so an empty list never does. The one script with that
    // data is the maze C exit door's ShDemoScript_open_hallway_door (orderFloor true, floorNames empty, where the hallway's copy
    // has orderFloor false): left dead, the maze's closed door stays drawn under the swinging demo door and, behind the player,
    // beside the hallway's own door. The port applies the authored intent (hide the door while gc_p00_010 plays).
    if (script.floor_names.empty() && script.class_name == "ShDemoScript" && script.demo_id == "gc_p00_010") {
        return true;
    }
    for (const std::string& floor : script.floor_names) {
        if (game.Floor().IsCurrentFloorName(floor)) {
            return true;
        }
    }
    return false;
}

}

void MessageSystem::PostControllerMessage(std::string_view message) {
    queue_.push_back({false, {}, std::string(message), game_.Time() + kGameFrame});
    LogInfo("message: controller {}", message);
    if (ModLua* mods = game_.ModScripts()) {
        mods->Message(message, "controller");
    }
}

void MessageSystem::PostDemoMessage(std::string_view demo_id, std::string_view message) {
    queue_.push_back({true, std::string(demo_id), std::string(message), game_.Time() + kGameFrame});
    LogInfo("message: demo {} {}", demo_id, message);
    if (ModLua* mods = game_.ModScripts()) {
        mods->Message(message, demo_id);
    }
}

bool MessageSystem::TryRegister(Stage& stage, const MessageScript& script) {
    if (registered_.contains({stage.id, script.entity})) {
        return false;
    }
    if (!stage.Body(script.entity).enable || !FloorMatches(game_, script)) {
        return false;
    }
    registered_.insert({stage.id, script.entity});
    return true;
}

void MessageSystem::RegisterStage(Stage& stage) {
    int count = 0;
    for (const auto& file : stage.files) {
        for (const MessageScript& script : file->message_scripts) {
            count += TryRegister(stage, script) ? 1 : 0;
        }
    }
    LogInfo("message: {} scripts of stage {} ({}) registered on floor {}", count, stage.id, stage.label, game_.Floor().CurrentFloorName());
}

void MessageSystem::ForgetStage(const Stage& stage) {
    std::erase_if(registered_, [&](const auto& key) { return key.first == stage.id; });
}

void MessageSystem::SetupMessageBox(Stage& stage, const fox2::Entity& entity) {
    for (const auto& file : stage.files) {
        for (const MessageScript& script : file->message_scripts) {
            if (script.entity == &entity) {
                if (TryRegister(stage, script)) {
                    LogInfo("message: {} registered by SetupMessageBox", script.name);
                }
                return;
            }
        }
    }
}

void MessageSystem::Dispatch() {
    // 0x52D170, 0x119E390: posts wait in the hub's ring for the next game frame's dispatch
    auto ready = [&] { return !queue_.empty() && queue_.front().ready <= game_.Time() + 1e-4; };
    for (int round = 0; round < 64 && ready(); ++round) {
        std::deque<Message> batch;
        while (ready()) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        for (const Message& message : batch) {
            Deliver(message);
        }
    }
    if (ready()) {
        LogWarn("message: {} messages left after 64 rounds", queue_.size());
    }
}

void MessageSystem::Deliver(const Message& message) {
    struct Target {
        uint32_t stage_id;
        const MessageScript* script;
    };
    std::vector<Target> targets;
    game_.Stages().ForEachStage([&](Stage& stage) {
        for (const auto& file : stage.files) {
            for (const MessageScript& script : file->message_scripts) {
                if (script.message_name != message.name) {
                    continue;
                }
                const bool is_demo_script = script.class_name == "ShDemoScript";
                if (is_demo_script != message.from_demo || (is_demo_script && script.demo_id != message.demo_id)) {
                    continue;
                }
                if (!registered_.contains({stage.id, script.entity})) {
                    continue;
                }
                targets.push_back({stage.id, &script});
            }
        }
    });
    for (const Target& target : targets) {
        Stage* stage = game_.Stages().FindById(target.stage_id);
        if (!stage) {
            continue;
        }
        LogInfo("message: {} {} -> {} ({})", message.from_demo ? message.demo_id : "controller", message.name, target.script->name,
                target.script->script_file);
        game_.Scripts().CallOnMessage(target.script->script_file, *stage, *target.script->entity, message.demo_id, message.name);
    }
}

}
