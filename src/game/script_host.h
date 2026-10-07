#pragma once

#include <set>
#include <string>
#include <string_view>

#include "engine/script/lua_vm.h"
#include "game/lua_entity.h"
#include "game/trap_system.h"

namespace pt::game {

class Game;
struct Stage;

std::string ScriptTableName(std::string_view path);

class ScriptHost : public EntityHost {
public:
    explicit ScriptHost(Game& game) : game_(game) {}

    bool Init();
    LuaVm& Vm() { return vm_; }

    void LoadStageScripts(Stage& stage);
    bool CallStateFunction(const char* name);
    int CallTrapExec(const std::string& script_path, Stage& stage, const fox2::Entity& trap, const fox2::Entity& condition, TrapFlag flag);
    int CallOnMessage(const std::string& script_path, Stage& stage, const fox2::Entity& script, std::string_view demo_id, std::string_view message);

    Stage* FindStage(uint32_t stage_id) override;
    void OnBodyChanged(Stage& stage, const fox2::Entity& entity, BodyField field) override;
    void OnSetupMessageBox(Stage& stage, const fox2::Entity& entity) override;

private:
    int ResultCode(int result_index);

    Game& game_;
    LuaVm vm_;
};

void RegisterGameBindings(LuaVm& vm, Game& game);

}
