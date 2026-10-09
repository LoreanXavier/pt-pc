#include <fstream>
#include <string>

#include "engine/core/log.h"
#include "game/game.h"
#include "game/lua_entity.h"
#include "game/script_host.h"

namespace pt::game {
namespace {

constexpr const char* kGameKey = "pt.Game";

Game& G(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kGameKey);
    auto* game = static_cast<Game*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *game;
}

std::string Str(lua_State* L, int index) {
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    return text ? std::string(text, length) : std::string();
}

int FoxLog(lua_State* L) {
    std::string text;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        text += LuaVm::ToString(L, i);
    }
    LogDebug("lua: {}", text);
    return 0;
}

int FloorIsCurrentFloorName(lua_State* L) {
    lua_pushboolean(L, G(L).Floor().IsCurrentFloorName(Str(L, 1)));
    return 1;
}

int FloorGetLoopCount(lua_State* L) {
    lua_pushnumber(L, G(L).Floor().LoopCount());
    return 1;
}

int FloorSetFloorLevel(lua_State* L) {
    G(L).Floor().SetFloorLevel(Str(L, 1));
    return 0;
}

int FloorSubFloorLevel(lua_State* L) {
    G(L).Floor().SubFloorLevel();
    return 0;
}

int FloorAddFloorLevel(lua_State* L) {
    G(L).Floor().AddFloorLevel();
    return 0;
}

int FloorGetFloorLevel(lua_State* L) {
    lua_pushnumber(L, G(L).Floor().Index());
    return 1;
}

int FloorSetFloorLevelAndName(lua_State* L) {
    G(L).Floor().SetFloorLevelAndName(static_cast<int>(luaL_checknumber(L, 1)), Str(L, 2));
    return 0;
}

int FloorRelocateGimmicks(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

int FloorStartNazoTrueEnd(lua_State* L) {
    G(L).Nazo().Activate(NazoId::TrueEnd);
    return 0;
}

int FloorGoNextFloor(lua_State* L) {
    G(L).Floor().GoNextFloor();
    return 0;
}

int SystemSoundPostEvent(lua_State* L) {
    glm::vec3 position(0.0f);
    const bool positional = ReadVector3(L, 2, position);
    G(L).PostSound(Str(L, 1), position, positional);
    return 0;
}

int SystemSoundPostEvent2D(lua_State* L) {
    G(L).PostSound(Str(L, 1), glm::vec3(0.0f), false);
    return 0;
}

int SoundRegisterAnimEvent(lua_State* L) {
    G(L).RegisterAnimEvent(Str(L, 1), Str(L, 2));
    return 0;
}

int SystemCallBGM(lua_State* L) {
    G(L).CallBgm(Str(L, 1));
    return 0;
}

int SystemStopBGM(lua_State* L) {
    G(L).StopBgm(Str(L, 1));
    return 0;
}

int SystemIsPlayingBGM(lua_State* L) {
    lua_pushboolean(L, G(L).IsPlayingBgm(Str(L, 1)));
    return 1;
}

int SystemPadEnable(lua_State* L) {
    G(L).Controller().SetPadEnablePending(lua_toboolean(L, 1) != 0);
    return 0;
}

int ObjectGetGameObjectId(lua_State* L) {
    lua_pushnumber(L, G(L).Objects().GetId(Str(L, 1), Str(L, 2)));
    return 1;
}

int ObjectSendCommand(lua_State* L) {
    const uint32_t id = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (!lua_istable(L, 2)) {
        return 0;
    }
    GameCommand command;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const std::string key = Str(L, -2);
            if (lua_isboolean(L, -1)) {
                command.args[key] = lua_toboolean(L, -1) != 0;
            } else if (lua_type(L, -1) == LUA_TNUMBER) {
                command.args[key] = lua_tonumber(L, -1);
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                command.args[key] = Str(L, -1);
            }
        }
        lua_pop(L, 1);
    }
    if (auto id_value = command.String("id")) {
        command.id = *id_value;
        command.args.erase("id");
    }
    G(L).Objects().SendCommand(id, command);
    return 0;
}

int MainLoadStage(lua_State* L) {
    G(L).Stages().RequestLoad(Str(L, 1), Str(L, 2), Str(L, 3), Str(L, 4), Str(L, 5));
    return 0;
}

int MainChangeStageId(lua_State* L) {
    G(L).Stages().ChangeStageId(Str(L, 1), Str(L, 2));
    return 0;
}

int MainActivateStage(lua_State* L) {
    G(L).Stages().RequestActivate(Str(L, 1));
    return 0;
}

int MainDeactivateStage(lua_State* L) {
    G(L).Stages().DeactivateStage(Str(L, 1));
    return 0;
}

int MainUnloadStage(lua_State* L) {
    G(L).Stages().RequestUnload(Str(L, 1));
    return 0;
}

int MainSetNextStageByPath(lua_State* L) {
    G(L).Stages().RequestLocation(Str(L, 1));
    return 0;
}

int MainUnloadStageAll(lua_State* L) {
    G(L).Stages().UnloadAll();
    return 0;
}

int MainIsGuiEditor(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

int ControllerChangeGameStep(lua_State* L) {
    G(L).Controller().ChangeGameStep(Str(L, 1));
    return 0;
}

int ControllerSendMessage(lua_State* L) {
    G(L).SendControllerMessage(Str(L, 1));
    return 0;
}

int ControllerVisibleControlSubtitle(lua_State* L) {
    G(L).SetSubtitleVisible(Str(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}


int ControllerStartFullScreenBlur(lua_State* L) {
    G(L).Effects().full_screen_blur = true;
    return 0;
}

int ControllerStopFullScreenBlur(lua_State* L) {
    G(L).Effects().full_screen_blur = false;
    return 0;
}

int ControllerFinishEndingRestartGame(lua_State* L) {
    G(L).Controller().FinishEnding();
    return 0;
}

int ControllerDisableOption(lua_State* L) {
    G(L).Controller().DisableOption();
    return 0;
}

int ControllerGotoGameOver(lua_State* L) {
    G(L).Controller().SetStep(16);
    return 0;
}

int ControllerGotoEnding(lua_State* L) {
    G(L).Controller().SetStep(21);
    return 0;
}

int ControllerResetGame(lua_State* L) {
    G(L).Controller().SetStep(17);
    return 0;
}

int ControllerSaveGame(lua_State* L) {
    G(L).Controller().SaveGame();
    return 0;
}

int FadeSetFadeColor(lua_State* L) {
    G(L).Effects().SetFadeColor(static_cast<int>(luaL_optnumber(L, 1, 0)), static_cast<int>(luaL_optnumber(L, 2, 0)),
                                static_cast<int>(luaL_optnumber(L, 3, 0)), static_cast<int>(luaL_optnumber(L, 4, 255)));
    return 0;
}

int FadeCallFadeOut(lua_State* L) {
    ScreenEffects& fx = G(L).Effects();
    fx.CallFadeOut(static_cast<float>(luaL_optnumber(L, 1, fx.fade_default_time)));
    return 0;
}

int FadeCallFadeIn(lua_State* L) {
    ScreenEffects& fx = G(L).Effects();
    fx.CallFadeIn(static_cast<float>(luaL_optnumber(L, 1, fx.fade_default_time)));
    return 0;
}

int FadeCustomSetting(lua_State* L) {
    G(L).Effects().FadeCustomSetting(static_cast<int>(luaL_optnumber(L, 1, 0)), static_cast<int>(luaL_optnumber(L, 2, 0)));
    return 0;
}

int FadeCallStrongFadeOut(lua_State* L) {
    ScreenEffects& fx = G(L).Effects();
    fx.CallStrongFadeOut(static_cast<float>(luaL_optnumber(L, 1, fx.fade_default_time)));
    return 0;
}

int FadeSetFadeTime(lua_State* L) {
    G(L).Effects().fade_default_time = static_cast<float>(luaL_optnumber(L, 1, 1.0));
    return 0;
}

int FadeResetFadeTime(lua_State* L) {
    G(L).Effects().fade_default_time = 1.0f;
    return 0;
}

int FadeResetFadeColor(lua_State* L) {
    G(L).Effects().fade_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return 0;
}

int FadeIgnore(lua_State* L) {
    ScreenEffects& fx = G(L).Effects();
    fx.fade_ignore = !fx.fade_ignore;
    lua_pushboolean(L, fx.fade_ignore);
    return 1;
}

int FadeNoop(lua_State*) {
    return 0;
}

int FadeIsFadeProcessing(lua_State* L) {
    lua_pushboolean(L, G(L).Effects().IsFadeProcessing());
    return 1;
}

int FadeIsFadeOut(lua_State* L) {
    lua_pushboolean(L, G(L).Effects().IsFadeOut());
    return 1;
}

int DemoPlay(lua_State* L) {
    G(L).Demos().Play(Str(L, 1));
    return 0;
}

int DemoSetDemoTransform(lua_State* L) {
    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 translation(0.0f);
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "x");
        lua_getfield(L, 2, "y");
        lua_getfield(L, 2, "z");
        lua_getfield(L, 2, "w");
        rotation = glm::quat(static_cast<float>(lua_tonumber(L, -1)), static_cast<float>(lua_tonumber(L, -4)), static_cast<float>(lua_tonumber(L, -3)),
                             static_cast<float>(lua_tonumber(L, -2)));
        lua_pop(L, 4);
    }
    ReadVector3(L, 3, translation);
    G(L).Demos().SetDemoTransform(Str(L, 1), rotation, translation);
    return 0;
}

int DemoStopAll(lua_State* L) {
    G(L).Demos().StopAll();
    return 0;
}

int DemoIsDemoPlaying(lua_State* L) {
    lua_pushboolean(L, lua_gettop(L) >= 1 ? G(L).Demos().IsPlaying(Str(L, 1)) : G(L).Demos().IsAnyPlaying());
    return 1;
}

int ShDemoSkip(lua_State* L) {
    G(L).Demos().Skip(Str(L, 1));
    return 0;
}

int EffectEnableLut(lua_State* L) {
    G(L).Effects().lut_control = lua_toboolean(L, 1) != 0;
    return 0;
}

int EffectSetLut(lua_State* L) {
    G(L).Effects().lut = Str(L, 1);
    return 0;
}

int NazoSetCondition(lua_State* L) {
    G(L).Nazo().SetCondition(Str(L, 1));
    return 0;
}

int DebugGetEntity(lua_State* L) {
    Stage* stage = G(L).Stages().Find(Str(L, 1));
    if (!stage) {
        lua_pushnil(L);
        return 1;
    }
    for (const auto& file : stage->files) {
        if (const fox2::Entity* e = file->file->ByShortName(Str(L, 2))) {
            PushEntity(L, *stage, e, lua_toboolean(L, 3) != 0);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

std::string TableString(lua_State* L, int index, const char* key) {
    if (!lua_istable(L, index)) {
        return {};
    }
    lua_getfield(L, index, key);
    std::string value = Str(L, -1);
    lua_pop(L, 1);
    return value;
}

int DebugDumpCollision(lua_State* L) {
    Game& game = G(L);
    std::ofstream out(Str(L, 1), std::ios::binary);
    for (const CollisionTriangle& t : game.Collision().Triangles()) {
        if (!game.Collision().OwnerActive(t.owner)) {
            continue;
        }
        out << t.a.x << ' ' << t.a.y << ' ' << t.a.z << ' ' << t.b.x << ' ' << t.b.y << ' ' << t.b.z << ' ' << t.c.x << ' ' << t.c.y << ' ' << t.c.z
            << ' ' << game.Collision().OwnerName(t.owner) << '\n';
    }
    game.Stages().ForEachStage([&](Stage& stage) {
        for (const auto& file : stage.files) {
            for (const TrapPlacement& trap : file->traps) {
                for (const glm::mat4& box : trap.boxes) {
                    const glm::mat4 w = stage.ToWorld(box);
                    out << "trap " << stage.label << ' ' << trap.name << ' ' << (stage.Body(trap.entity).enable ? 1 : 0);
                    for (int c = 0; c < 4; ++c) {
                        out << ' ' << w[c][0] << ' ' << w[c][1] << ' ' << w[c][2];
                    }
                    out << '\n';
                }
            }
        }
        glm::mat4 door;
        for (const char* name : {"startDoor", "endDoor", "door"}) {
            if (stage.ConnectorWorld(name, door)) {
                out << "conn " << stage.label << ' ' << name << ' ' << door[3][0] << ' ' << door[3][1] << ' ' << door[3][2] << '\n';
            }
        }
        out << "stage " << stage.label << ' ' << stage.id;
        for (int c = 0; c < 4; ++c) {
            out << ' ' << stage.file_to_world[c][0] << ' ' << stage.file_to_world[c][1] << ' ' << stage.file_to_world[c][2];
        }
        out << '\n';
    }, false);
    for (const SurfaceTriangle& t : game.Surfaces()) {
        out << "surface " << std::hex << t.material << std::dec << ' ' << t.a.x << ' ' << t.a.y << ' ' << t.a.z << ' ' << t.b.x << ' ' << t.b.y << ' '
            << t.b.z << ' ' << t.c.x << ' ' << t.c.y << ' ' << t.c.z << '\n';
    }
    const glm::vec3 p = game.GetPlayer().Feet();
    out << "player " << p.x << ' ' << p.y << ' ' << p.z << '\n';
    LogInfo("debug: collision dumped to {}", Str(L, 1));
    return 0;
}

int DebugSurface(lua_State* L) {
    Game& game = G(L);
    const glm::vec3 feet = game.GetPlayer().Feet();
    LogInfo("debug: surface material {:08X} at ({:.2f} {:.2f} {:.2f})", game.SurfaceMaterial(feet), feet.x, feet.y, feet.z);
    return 0;
}

int DebugRaycast(lua_State* L) {
    Game& game = G(L);
    const Player& player = game.GetPlayer();
    RayHit hit;
    const glm::vec3 origin = player.Feet() + glm::vec3(0.0f, static_cast<float>(luaL_optnumber(L, 1, 1.0)), 0.0f);
    const glm::vec3 direction = player.BodyForward();
    if (game.Collision().Raycast(origin, direction, 5.0f, hit)) {
        const CollisionTriangle& t = game.Collision().Triangles()[hit.triangle];
        LogInfo("raycast: {:.2f} m, owner {}, normal ({:.2f} {:.2f} {:.2f})", hit.distance, game.Collision().OwnerName(t.owner), hit.normal.x,
                hit.normal.y, hit.normal.z);
    } else {
        LogInfo("raycast: no hit");
    }
    return 0;
}

int GimmickAddPartsPath(lua_State* L) {
    G(L).Objects().AddPartsPath(TableString(L, 1, "partName"), TableString(L, 1, "path"));
    return 0;
}

int GimmickAddMotionPath(lua_State* L) {
    G(L).Objects().AddMotionPath(TableString(L, 1, "key"), TableString(L, 1, "path"));
    return 0;
}

int GameStatusSet(lua_State* L) {
    G(L).Status().Acquire(Str(L, 2), Str(L, 1));
    return 0;
}

int GameStatusReset(lua_State* L) {
    G(L).Status().Release(Str(L, 2), Str(L, 1));
    return 0;
}

int ParameterReloadTables(lua_State* L) {
    G(L).Parameters().LoadFromLua(L, 1);
    return 0;
}

}

void RegisterGameBindings(LuaVm& vm, Game& game) {
    lua_State* L = vm.State();
    lua_pushlightuserdata(L, &game);
    lua_setfield(L, LUA_REGISTRYINDEX, kGameKey);

    const luaL_Reg fox[] = {{"Log", FoxLog}, {nullptr, nullptr}};
    vm.RegisterModule("Fox", fox);
    const luaL_Reg floor[] = {
        {"IsCurrentFloorName", FloorIsCurrentFloorName}, {"GetLoopCount", FloorGetLoopCount}, {"SetFloorLevel", FloorSetFloorLevel},
        {"SubFloorLevel", FloorSubFloorLevel}, {"AddFloorLevel", FloorAddFloorLevel}, {"GetFloorLevel", FloorGetFloorLevel},
        {"SetFloorLevelAndName", FloorSetFloorLevelAndName}, {"RelocateGimmicks", FloorRelocateGimmicks},
        {"StartNazoTrueEnd", FloorStartNazoTrueEnd}, {"GoNextFloor", FloorGoNextFloor}, {nullptr, nullptr},
    };
    vm.RegisterModule("GameFloorLevel", floor);
    const luaL_Reg system[] = {
        {"SoundPostEvent", SystemSoundPostEvent}, {"SoundPostEvent2D", SystemSoundPostEvent2D}, {"CallBGM", SystemCallBGM},
        {"StopBGM", SystemStopBGM}, {"IsPlayingBGM", SystemIsPlayingBGM}, {"PadEnable", SystemPadEnable}, {nullptr, nullptr},
    };
    vm.RegisterModule("GameSystem", system);
    const luaL_Reg sound[] = {{"RegisterAnimEvent", SoundRegisterAnimEvent}, {nullptr, nullptr}};
    vm.RegisterModule("SoundDaemon", sound);
    const luaL_Reg object[] = {{"GetGameObjectId", ObjectGetGameObjectId}, {"SendCommand", ObjectSendCommand}, {nullptr, nullptr}};
    vm.RegisterModule("GameObject", object);
    vm.SetModuleNumber("GameObject", "NULL_ID", GameObjects::kNullId);
    const luaL_Reg main_control[] = {
        {"LoadStage", MainLoadStage}, {"ChangeStageId", MainChangeStageId}, {"ChangeStageLabel", MainChangeStageId},
        {"ActivateStage", MainActivateStage}, {"DeactivateStage", MainDeactivateStage}, {"UnloadStage", MainUnloadStage},
        {"SetNextStageByPath", MainSetNextStageByPath}, {"UnloadStageAll", MainUnloadStageAll}, {"IsGuiEditor", MainIsGuiEditor},
        {nullptr, nullptr},
    };
    vm.RegisterModule("ShGameMainControl", main_control);
    const luaL_Reg controller[] = {
        {"ChangeGameStep", ControllerChangeGameStep}, {"SendMessage", ControllerSendMessage},
        {"VisibleControlSubtitle", ControllerVisibleControlSubtitle}, {"StartFullScreenBlur", ControllerStartFullScreenBlur},
        {"StopFullScreenBlur", ControllerStopFullScreenBlur}, {"FinishEndingRestartGame", ControllerFinishEndingRestartGame},
        {"DisableOption", ControllerDisableOption}, {"GotoGameOver", ControllerGotoGameOver}, {"GotoEnding", ControllerGotoEnding},
        {"ResetGame", ControllerResetGame}, {"SaveGame", ControllerSaveGame}, {nullptr, nullptr},
    };
    vm.RegisterModule("GameController", controller);
    const luaL_Reg fade[] = {
        {"SetFadeColor", FadeSetFadeColor}, {"CallFadeOut", FadeCallFadeOut}, {"CallFadeIn", FadeCallFadeIn},
        {"FadeCustomSetting", FadeCustomSetting}, {"CallStrongFadeOut", FadeCallStrongFadeOut}, {"IsFadeProcessing", FadeIsFadeProcessing},
        {"IsFadeOut", FadeIsFadeOut}, {"SetFadeTime", FadeSetFadeTime}, {"ResetFadeTime", FadeResetFadeTime}, {"ResetFadeColor", FadeResetFadeColor},
        {"FadeIgnore", FadeIgnore}, {"InitFadeSetting", FadeNoop}, {"FadeSettingDump", FadeNoop}, {nullptr, nullptr},
    };
    vm.RegisterModule("FadeFunction", fade);
    const luaL_Reg demo[] = {
        {"Play", DemoPlay}, {"SetDemoTransform", DemoSetDemoTransform}, {"StopAll", DemoStopAll}, {"IsDemoPlaying", DemoIsDemoPlaying},
        {nullptr, nullptr},
    };
    vm.RegisterModule("DemoDaemon", demo);
    const luaL_Reg sh_demo[] = {{"Skip", ShDemoSkip}, {nullptr, nullptr}};
    vm.RegisterModule("ShDemo", sh_demo);
    const luaL_Reg effect[] = {{"EnableColorCorrectionLutControl", EffectEnableLut}, {"SetColorCorrectionLut", EffectSetLut}, {nullptr, nullptr}};
    vm.RegisterModule("TppEffectUtility", effect);
    const luaL_Reg nazo[] = {{"SetCondition", NazoSetCondition}, {nullptr, nullptr}};
    vm.RegisterModule("ShNazoManager", nazo);
    const luaL_Reg gimmick[] = {{"AddPartsPath", GimmickAddPartsPath}, {"AddMotionPath", GimmickAddMotionPath}, {nullptr, nullptr}};
    vm.RegisterModule("Gimmick", gimmick);
    const luaL_Reg debug[] = {{"GetEntity", DebugGetEntity}, {"Raycast", DebugRaycast}, {"DumpCollision", DebugDumpCollision}, {"Surface", DebugSurface}, {nullptr, nullptr}};
    vm.RegisterModule("PtDebug", debug);
    const luaL_Reg status[] = {{"Set", GameStatusSet}, {"Reset", GameStatusReset}, {nullptr, nullptr}};
    vm.RegisterModule("ShGameStatus", status);
    const luaL_Reg parameter[] = {{"ReloadParameterTables", ParameterReloadTables}, {nullptr, nullptr}};
    vm.RegisterModule("ShParameter", parameter);
}

}
