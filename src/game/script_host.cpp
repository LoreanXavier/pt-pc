#include "game/script_host.h"

#include "engine/core/log.h"
#include "game/game.h"
#include "game/script_api.h"

namespace pt::game {

std::string ScriptTableName(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string_view::npos) {
        name = name.substr(0, dot);
    }
    return std::string(name);
}

bool ScriptHost::Init() {
    RegisterStubApi(vm_);
    RegisterEntityApi(vm_.State(), this);
    RegisterGameBindings(vm_, game_);
    return true;
}

void ScriptHost::LoadStageScripts(Stage& stage) {
    int loaded = 0;
    for (const StageScript& script : stage.scripts) {
        if (vm_.RunChunk(script.path, script.code)) {
            ++loaded;
        }
    }
    LogInfo("script: {} of {} scripts of {} loaded", loaded, stage.scripts.size(), stage.package_path);
}

int ScriptHost::ResultCode(int result_index) {
    lua_State* L = vm_.State();
    int code = 0;
    if (lua_isboolean(L, result_index)) {
        code = lua_toboolean(L, result_index) ? 1 : 0;
    } else if (lua_isnumber(L, result_index)) {
        code = static_cast<int>(lua_tonumber(L, result_index));
    }
    lua_settop(L, result_index - 1);
    return code;
}

bool ScriptHost::CallStateFunction(const char* name) {
    int first = 0;
    if (!vm_.CallTableFunction("gameState", name, 0, 1, nullptr, &first)) {
        LogWarn("script: gameState.{} failed or missing", name);
        return false;
    }
    ResultCode(first);
    return true;
}

int ScriptHost::CallTrapExec(const std::string& script_path, Stage& stage, const fox2::Entity& trap, const fox2::Entity& condition, TrapFlag flag) {
    const std::string table = ScriptTableName(script_path);
    int first = 0;
    const bool ok = vm_.CallTableFunction(table, "Exec", 1, 1,
                                          [&](lua_State* L) {
                                              lua_createtable(L, 0, 4);
                                              lua_pushstring(L, TrapFlagString(flag));
                                              lua_setfield(L, -2, "trapFlagString");
                                              PushEntity(L, stage, &condition, false);
                                              lua_setfield(L, -2, "conditionHandle");
                                              PushEntity(L, stage, &trap, true);
                                              lua_setfield(L, -2, "trapBodyHandle");
                                              PushEntity(L, stage, &trap, false);
                                              lua_setfield(L, -2, "trapHandle");
                                          },
                                          &first);
    if (!ok) {
        LogWarn("script: {}.Exec failed or missing", table);
        return 0;
    }
    return ResultCode(first);
}

int ScriptHost::CallOnMessage(const std::string& script_path, Stage& stage, const fox2::Entity& script, std::string_view demo_id,
                              std::string_view message) {
    const std::string table = ScriptTableName(script_path);
    int first = 0;
    const bool ok = vm_.CallTableFunction(table, "OnMessage", 4, 1,
                                          [&](lua_State* L) {
                                              PushEntity(L, stage, &script, false);
                                              PushEntity(L, stage, &script, true);
                                              lua_pushlstring(L, demo_id.data(), demo_id.size());
                                              lua_pushlstring(L, message.data(), message.size());
                                          },
                                          &first);
    if (!ok) {
        LogWarn("script: {}.OnMessage failed or missing", table);
        return 0;
    }
    return ResultCode(first);
}

Stage* ScriptHost::FindStage(uint32_t stage_id) {
    return game_.Stages().FindById(stage_id);
}

void ScriptHost::OnBodyChanged(Stage& stage, const fox2::Entity& entity, BodyField field) {
    const StageData* file = stage.FileOf(&entity);
    const BodyState& body = stage.Body(&entity);
    const bool value = field == BodyField::Enable ? body.enable : field == BodyField::Visible ? body.visible : body.geom_active;
    LogDebug("body: {} {} {} = {}", entity.class_name, file ? file->file->EntityName(entity) : std::string(),
             field == BodyField::Enable ? "enable" : field == BodyField::Visible ? "visible" : "geom", value);
    if (field == BodyField::GeomActive) {
        game_.Stages().MarkGeomDirty();
    } else {
        game_.Stages().MarkVisualsDirty();
    }
}

void ScriptHost::OnSetupMessageBox(Stage& stage, const fox2::Entity& entity) {
    game_.Messages().SetupMessageBox(stage, entity);
}

}
