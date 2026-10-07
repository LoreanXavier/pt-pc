#include "game/script_api.h"

#include <string_view>

#include "engine/core/log.h"

namespace pt::game {
namespace {

std::vector<ScriptCall> g_calls;

const char* const kStubApi[][2] = {
    {"Fox", "Log"}, {"Fox", "GetPlatformName"}, {"Fox", "SetActMode"}, {"Fox", "Error"}, {"Fox", "ExportSerializeInfo"}, {"Fox", "Require"},
    {"GameFloorLevel", "IsCurrentFloorName"}, {"GameFloorLevel", "RelocateGimmicks"}, {"GameFloorLevel", "GetLoopCount"},
    {"GameFloorLevel", "StartNazoTrueEnd"}, {"GameFloorLevel", "SubFloorLevel"}, {"GameFloorLevel", "SetFloorLevel"},
    {"GameFloorLevel", "AddFloorLevel"}, {"GameFloorLevel", "GetFloorLevel"}, {"GameFloorLevel", "SetFloorLevelAndName"}, {"GameFloorLevel", "GoNextFloor"},
    {"GrRenderPlugin", "AddPlugin"},
    {"GrTools", "LoadShaderPack"}, {"GrTools", "SetTerrainMaterialTexture"}, {"GrTools", "SetEnablePackedSmallTextureStreaming"},
    {"GrTools", "SetEnableLnmForTerrainNormal"}, {"GrTools", "SetEnableLnmForDecalNormal"}, {"GrTools", "GetDeviceName"},
    {"GrTools", "FontSystemLoad"}, {"GrTools", "AddExtraShaderPerfLua"}, {"GrTools", "FontSystemInit"}, {"GrTools", "SetDefaultTextureLoadPath"},
    {"GrTools", "SetReflectionTexture"}, {"GrTools", "SetMaterialTexture"}, {"GrTools", "SetMaterialParamBinary"}, {"GrTools", "SetGen8RenderingMode"},
    {"GrTools", "SetupSystemShaderResources"},
    {"GameSystem", "SoundPostEvent"}, {"GameSystem", "SoundPostEvent2D"}, {"GameSystem", "StopBGM"}, {"GameSystem", "IsPlayingBGM"},
    {"GameSystem", "CallBGM"}, {"GameSystem", "PadEnable"},
    {"Geo", "GeoLuaSetDebugMaterialColor"}, {"Geo", "GeoLuaSetDebugCollisionColor"},
    {"AssetConfiguration", "SetTargetDirectory"}, {"AssetConfiguration", "GetConfigurationFromAssetManager"},
    {"AssetConfiguration", "SetDefaultTargetDirectory"}, {"AssetConfiguration", "RegisterExtensionInfo"},
    {"AssetConfiguration", "SetDefaultCategory"}, {"AssetConfiguration", "GetDefaultCategory"},
    {"AssetConfiguration", "SetLanguageGroupExtention"}, {"AssetConfiguration", "SetGroupCurrentLanguage"},
    {"GameObject", "GetGameObjectId"}, {"GameObject", "SendCommand"},
    {"ShGameMainControl", "LoadStage"}, {"ShGameMainControl", "ChangeStageId"}, {"ShGameMainControl", "ActivateStage"},
    {"ShGameMainControl", "UnloadStage"}, {"ShGameMainControl", "SetNextStageByPath"}, {"ShGameMainControl", "UnloadStageAll"},
    {"ShGameMainControl", "GoToLocation"}, {"ShGameMainControl", "IsGuiEditor"}, {"ShGameMainControl", "StandbyStage"},
    {"ShGameMainControl", "DeactivateStage"}, {"ShGameMainControl", "ChangeStageLabel"},
    {"Entity", "IsNull"},
    {"Pad", "RegisterButtonAssign"}, {"Pad", "RegisterAxisAssign"}, {"Pad", "ConfigDefaultAssigns"},
    {"Gimmick", "AddMotionPath"}, {"Gimmick", "AddPartsPath"},
    {"GameController", "ChangeGameStep"}, {"GameController", "SendMessage"}, {"GameController", "VisibleControlSubtitle"},
    {"GameController", "StopFullScreenBlur"}, {"GameController", "FinishEndingRestartGame"}, {"GameController", "DisableOption"},
    {"GameController", "StartFullScreenBlur"}, {"GameController", "GotoGameOver"}, {"GameController", "GotoEnding"},
    {"GameController", "ResetGame"}, {"GameController", "SaveGame"},
    {"FadeFunction", "SetFadeColor"}, {"FadeFunction", "CallFadeOut"}, {"FadeFunction", "FadeCustomSetting"}, {"FadeFunction", "CallFadeIn"},
    {"FadeFunction", "InitFadeSetting"}, {"FadeFunction", "CallStrongFadeOut"}, {"FadeFunction", "IsFadeProcessing"}, {"FadeFunction", "IsFadeOut"},
    {"DemoDaemon", "Play"}, {"DemoDaemon", "SetDemoTransform"}, {"DemoDaemon", "StopAll"}, {"DemoDaemon", "IsDemoPlaying"},
    {"FoxGameFrame", "SetGameFrameWaitType"},
    {"SoundDaemon", "RegisterAnimEvent"}, {"SoundDaemon", "MakeLeftRightAnimEventPair"}, {"SoundDaemon", "Create"},
    {"TppEffectUtility", "EnableColorCorrectionLutControl"}, {"TppEffectUtility", "SetColorCorrectionLut"},
    {"SoundCoreDaemon", "SetAssetPath"}, {"SoundCoreDaemon", "Create"}, {"SoundCoreDaemon", "SetInterferenceRTPCName"},
    {"SoundCoreDaemon", "SetDopplerRTPCName"}, {"SoundCoreDaemon", "SetRearParameter"},
    {"FxDaemon", "InitializeReserveObject"}, {"FxDaemon", "Initialize"},
    {"EdGraphFactory", "CreateSetting"}, {"EdGraphFactory", "AddSetting"}, {"EdGraphFactory", "GetInstance"}, {"EdGraphFactory", "DefaultSetting"},
    {"UiDaemon", "SetFontTypeTransTable"}, {"UiDaemon", "GetInstance"}, {"UiDaemon", "ClearDrawPriorityTable"},
    {"UiDaemon", "SetDrawPriorityTable"}, {"UiDaemon", "SetPrefetchTextureTable"},
    {"NavWorldDaemon", "AddWorld"}, {"NavWorldDaemon", "AddScene"},
    {"SubtitlesCommand", "SetVoiceLanguage"}, {"SubtitlesCommand", "SetLanguage"},
    {"FxSystemConfig", "SetLimitInstanceMemorySize"}, {"FxSystemConfig", "SetLimitInstanceMemoryDefaultSize"},
    {"SubtitlesDaemon", "SetDefaultVoiceLanguage"}, {"SubtitlesDaemon", "GetDefaultVoiceLanguage"},
    {"EdDemoEditBlockController", "AddToolsBlockPath"}, {"Preference", "GetPreferenceEntity"}, {"EdAnimGraphControlAdapter", "GetInstance"},
    {"Editor", "Setting"}, {"Editor", "GetInstance"}, {"PhDaemon", "SetMemorySize"}, {"PhDaemon", "SetMaxRigidBodyNum"},
    {"Script", "LoadLibrary"}, {"EdPreview", "GetManager"}, {"ShDemo", "Skip"}, {"ShNazoManager", "SetCondition"}, {"ShNazoManager", "SetState"},
    {"DemoDummyFloorLevel", "IsMyFloor_FloorName"}, {"PathMapper", "Add"}, {"EditableBlockPackage", "RegisterPackageExtensionInfo"},
    {"CameraPriority", "RegisterPriorities"}, {"CameraSelector", "SetMainInstance"}, {"Pad2", "Init"}, {"ReplayService", "Boot"},
    {"NtDaemon", "Create"}, {"FoxTestLuaActor", "ExecGlobal"}, {"MiniPerfView", "SetEnable"}, {"BlockSizeView", "SetEnable"},
    {"FoxFadeIo", "Create"}, {"GsRouteDataNodeEvent", "SetEventDefinitionPath"}, {"GsRouteDataEdgeEvent", "SetEventDefinitionPath"},
    {"ShLightCapture", "InitInstance"}, {"ShParameter", "ReloadParameterTables"}, {"EdGraphAdapter", "Setting"},
    {"DataCluster", "GetActorsByClassName"}, {"DataActor", "GetActorsByClassName"},
    {"ShGameStatus", "RegisterGameFlags"}, {"ShGameStatus", "GetGameFlag"}, {"ShGameStatus", "SetGameFlag"},
};

int StubFunction(lua_State* L) {
    ScriptCall call;
    call.module = lua_tostring(L, lua_upvalueindex(1));
    call.function = lua_tostring(L, lua_upvalueindex(2));
    const int count = lua_gettop(L);
    std::string joined;
    for (int i = 1; i <= count; ++i) {
        std::string arg = lua_istable(L, i) ? "{table}" : LuaVm::ToString(L, i);
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += arg;
        call.args.push_back(std::move(arg));
    }
    if (call.module != "Fox" || call.function != "Log") {
        LogDebug("lua stub {}.{}({})", call.module, call.function, joined);
    }
    if (g_calls.size() < 4096) {
        g_calls.push_back(std::move(call));
    }
    return 0;
}

}

void RegisterStubApi(LuaVm& vm) {
    lua_State* L = vm.State();
    for (const auto& entry : kStubApi) {
        lua_getglobal(L, entry[0]);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setglobal(L, entry[0]);
        }
        lua_pushstring(L, entry[0]);
        lua_pushstring(L, entry[1]);
        lua_pushcclosure(L, StubFunction, 2);
        lua_setfield(L, -2, entry[1]);
        lua_pop(L, 1);
    }
    vm.SetModuleNumber("GameObject", "NULL_ID", 65535.0);
}

const std::vector<ScriptCall>& RecentStubCalls() {
    return g_calls;
}

void ClearStubCalls() {
    g_calls.clear();
}

}
