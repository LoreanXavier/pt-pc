# Lua API

Native functions exposed to Lua through static `luaL_Reg` tables in `.data` (found by `{string, text pointer}` pairs ending in a null entry). Module names come from the registering function's string references; where one registrar sets up several modules the table is matched to the module by script usage. `uses` counts calls in the game's own Lua scripts.

Modules bound through Fox reflection (GameObject, Entity, Pad, GrTools, GrRenderPlugin and others) are not in these tables and are not listed here.

Mod scripts (`mods/<mod>/init.lua`) run in a Lua state of their own and see none of these modules. Their API, the `Mod` table, is described in [modding.md](modding.md).

## FoxGameFrame

Table 0x1B9AA50, registered by 0x4EBF50, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetGameFrameWaitType | 0x4EBFF0 | 16 |

## Math.Vector3

Table 0x1B9AC60, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| GetX | 0x4F67A0 |  |
| GetY | 0x4F67D0 |  |
| GetZ | 0x4F6810 |  |
| GetLength | 0x4F6840 |  |
| GetLengthSqr | 0x4F6910 |  |
| __add | 0x4F63A0 |  |
| __sub | 0x4F6420 |  |
| __mul | 0x4F64A0 |  |
| __unm | 0x4F6950 |  |
| Dot | 0x4F6550 |  |
| Cross | 0x4F65B0 |  |
| Normalize | 0x4F6650 |  |
| __tostring | 0x4F6710 |  |

## Math.Vector3

Table 0x1B9AD40, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| __call | 0x4F5FB0 |  |
| Add | 0x4F63A0 |  |
| Sub | 0x4F6420 |  |
| Scale | 0x4F64A0 |  |
| Dot | 0x4F6550 |  |
| Cross | 0x4F65B0 |  |
| Normalize | 0x4F6650 |  |
| ToString | 0x4F6710 |  |

## Math.Vector4

Table 0x1B9ADD0, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| GetXYZ | 0x4F5D00 |  |
| GetX | 0x4F5D60 |  |
| GetY | 0x4F5D90 |  |
| GetZ | 0x4F5DD0 |  |
| GetW | 0x4F5E00 |  |
| GetLength | 0x4F5E40 |  |
| GetLengthSqr | 0x4F5F00 |  |
| __add | 0x4F59A0 |  |
| __sub | 0x4F5A20 |  |
| __mul | 0x4F5AA0 |  |
| __unm | 0x4F5F40 |  |
| Dot | 0x4F5B50 |  |
| Normalize | 0x4F5BB0 |  |
| __tostring | 0x4F5C60 |  |

## Math.Vector3

Table 0x1B9AEC0, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| __call | 0x4F5440 |  |
| Add | 0x4F59A0 |  |
| Sub | 0x4F5A20 |  |
| Scale | 0x4F5AA0 |  |
| Dot | 0x4F5B50 |  |
| Normalize | 0x4F5BB0 |  |
| ToString | 0x4F5C60 |  |

## Math.Quat

Table 0x1B9AF40, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| __mul | 0x4F5110 |  |
| __div | 0x4F5290 |  |
| Rotate | 0x4F5340 |  |
| __tostring | 0x4F5070 |  |
| Normalize | 0x4F4FC0 |  |

## Math.Quat

Table 0x1B9AFA0, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| __call | 0x4F4320 |  |
| Identity | 0x4F4760 |  |
| GetX | 0x4F47B0 |  |
| GetY | 0x4F47E0 |  |
| GetZ | 0x4F4820 |  |
| GetW | 0x4F4850 |  |
| Rotation | 0x4F4890 |  |
| RotationX | 0x4F4B40 |  |
| RotationY | 0x4F4CC0 |  |
| RotationZ | 0x4F4E40 |  |
| Normalize | 0x4F4FC0 |  |
| ToString | 0x4F5070 |  |

## Math.Matrix3

Table 0x1B9B070, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| GetCol0 | 0x4F3D60 |  |
| GetCol1 | 0x4F3DF0 |  |
| GetCol2 | 0x4F3E80 |  |
| __add | 0x4F3F10 |  |
| __sub | 0x4F4000 |  |
| __mul | 0x4F40F0 |  |
| __tostring | 0x4F3C70 |  |

## Math.Matrix3

Table 0x1B9B0F0, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| __call | 0x4F32C0 |  |
| Identity | 0x4F3580 |  |
| RotationX | 0x4F35F0 |  |
| RotationY | 0x4F3790 |  |
| RotationZ | 0x4F3940 |  |
| Inverse | 0x4F3B00 |  |
| ToString | 0x4F3C70 |  |

## Math.Matrix4

Table 0x1B9B170, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| GetCol0 | 0x4F29F0 |  |
| GetCol1 | 0x4F2A80 |  |
| GetCol2 | 0x4F2B10 |  |
| GetCol3 | 0x4F2BA0 |  |
| __add | 0x4F2C30 |  |
| __sub | 0x4F2D40 |  |
| __mul | 0x4F2E50 |  |
| GetTranslation | 0x4F3170 |  |
| Translate | 0x4F3200 |  |
| __tostring | 0x4F2880 |  |

## Math.Matrix4

Table 0x1B9B220, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| __call | 0x4F1290 |  |
| Identity | 0x4F19A0 |  |
| Translation | 0x4F1A10 |  |
| RotationX | 0x4F1AA0 |  |
| RotationY | 0x4F1C50 |  |
| RotationZ | 0x4F1E10 |  |
| RotationZYX | 0x4F1FE0 |  |
| Rotation | 0x4F21D0 |  |
| RotTranslation | 0x4F2350 |  |
| Scale | 0x4F2500 |  |
| Inverse | 0x4F2590 |  |
| ToString | 0x4F2880 |  |

## Math.WideVector3

Table 0x1B9B2F0, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| GetVector3 | 0x4F0D00 |  |
| __tostring | 0x4F0DE0 |  |

## Math.WideVector3

Table 0x1B9B320, registered by 0x4F0F90, module name guess.

| function | address | uses |
| --- | --- | --- |
| __call | 0x4F06B0 |  |
| Zero | 0x4F0C70 |  |
| ToString | 0x4F0DE0 |  |

## AssetConfiguration

Table 0x1B9B390, registered by 0x4FB460, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetDefaultCategory | 0x4F9E40 | 18 |
| GetDefaultCategory | 0x4F9F60 | 1 |
| SetCategory | 0x4F9FE0 |  |
| SetDefaultTargetDirectory | 0x4FA140 | 5 |
| SetTargetDirectory | 0x4FA200 | 32 |
| SetDefaultLanguageDirectory | 0x4FA320 |  |
| SetLanguageDirectory | 0x4FA3E0 | 1 |
| SetConnectAddress | 0x4FA540 |  |
| SetDefaultAssetLevel | 0x4FA510 |  |
| ResetDefaultAssetLevel | 0x4FA530 |  |
| SetAutoUpdateMode | 0x4FA580 |  |
| AddProjectName | 0x4FA550 |  |
| ClearProjectName | 0x4FA570 |  |
| RegisterExtensionInfo | 0x4FA5A0 | 4 |
| SetProjectRootPath | 0x4FA7B0 |  |
| ResetProjectRootPath | 0x4FA8D0 |  |
| SetPackFileRootDirectory | 0x4FA970 |  |
| ResetPackFileRootDirectory | 0x4FA980 |  |
| SetPackFileCopyMode | 0x4FA990 |  |
| SetPackFileCopyEnabled | 0x4FA9A0 |  |
| GetConfigurationFromAssetManager | 0x4FB510 | 7 |
| SetLanguageGroupExtention | 0x4FAFD0 | 1 |
| SetGroupCurrentLanguage | 0x4FAA00 | 1 |
| GetGroupCurrentLanguage | 0x4FAE20 |  |
| IsDiscOrHddImage | 0x4FB530 |  |

## Color

Table 0x1B9B920, registered by 0x5017E0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| GetR | 0x501ED0 |  |
| GetG | 0x501F00 |  |
| GetB | 0x501F40 |  |
| GetA | 0x501F70 |  |
| __add | 0x501CA0 |  |
| __sub | 0x501D20 |  |
| __mul | 0x501DA0 |  |
| __tostring | 0x501E30 |  |

## Color

Table 0x1B9B9B0, registered by 0x5017E0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| __call | 0x501870 |  |
| Add | 0x501CA0 |  |
| Sub | 0x501D20 |  |
| Scale | 0x501DA0 |  |
| ToString | 0x501E30 |  |

## PlatformConfiguration

Table 0x1B9C670, registered by 0x511CB0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetVideoRecordingEnabled | 0x511D70 |  |
| SetAudioRecordingEnabled | 0x511D80 |  |
| SetScreenShotEnabled | 0x511D90 |  |
| SetShareScreenEnabled | 0x511DA0 |  |

## HidDriver

Table 0x1B9CB30, registered by 0x519BB0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Override | 0x11949D0 |  |
| Input | 0x1194A70 |  |
| Clear | 0x11951C0 |  |
| Frame | 0x1195260 |  |
| Sleep | 0x11953C0 |  |
| Release | 0x1195550 |  |
| IRelease | 0x11955F0 |  |
| SetPadSensibility | 0x1195690 |  |
| ResetDefaultPadSensibility | 0x1195770 |  |
| SetPadDataCacheCapacity | 0x11957A0 |  |

## PI

Table 0x1B9D5A0, registered by 0x524C30, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Sin | 0x524C80 |  |
| Cos | 0x524D80 |  |
| Tan | 0x524E90 |  |
| Absf | 0x524FA0 |  |
| Asin | 0x524FE0 |  |
| Acos | 0x525120 |  |
| Atan2 | 0x525240 |  |
| Atan | 0x525410 |  |
| Exp | 0x525520 |  |
| Floor | 0x525560 |  |
| Ceil | 0x5255A0 |  |
| Mod | 0x5255E0 |  |
| Log | 0x525640 |  |
| Pow | 0x525680 |  |
| Sqrt | 0x5256E0 |  |
| Rsqrt | 0x525720 |  |
| Saturate | 0x525780 |  |
| FRnd | 0x5257E0 |  |
| DegreeToRadian | 0x525810 |  |
| RadianToDegree | 0x525840 |  |
| Min | 0x525870 |  |
| Max | 0x525910 |  |
| Clamp | 0x5259B0 |  |
| NormalizeRadian | 0x525A40 |  |

## SoundCoreDaemon

Table 0x1BA3C30, registered by 0x5E3AF0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Create | 0x5E3BA0 | 1 |
| SetAssetPath | 0x5E3BE0 | 4 |
| WaitAsync | 0x5E3C40 |  |
| SetInterferenceRTPCName | 0x5E3C90 | 1 |
| SetDopplerRTPCName | 0x5E3CE0 | 1 |
| SetRearParameter | 0x5E3D20 | 1 |
| SetGlobalRTPC | 0x5E3D80 |  |
| ResetGlobalRTPC | 0x5E3E00 |  |
| SetState | 0x5E3E60 |  |
| ResetState | 0x5E3EF0 |  |
| SetGlobalAttenuationRate | 0x5E3F50 |  |
| ResetGlobalAttenuationRate | 0x5E3FB0 |  |
| SetGlobalSonicSpeed | 0x5E4000 |  |
| ResetGlobalSonicSpeed | 0x5E4060 |  |

## Ai

Table 0x1BAA950, registered by 0x648880, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| GetCharacterCount | 0x648940 |  |
| RegisterKnowledgeTags | 0x648970 |  |
| DoesUseAibFile | 0x648A80 |  |

## Ch

Table 0x1BB0690, registered by 0x6EFCB0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| FindCharacterObjectByCharacterId | 0x6EFD60 |  |
| FindCharacters | 0x6EFDF0 |  |
| FindCharactersSphere | 0x6F0030 |  |
| FindCharacter | 0x6F02A0 |  |
| FindCharacterSyncUniqueId | 0x6F0410 |  |
| FindCharacterObjects | 0x6F04C0 |  |
| FindCharacterObjectsSphere | 0x6F06E0 |  |
| FindCharacterObjectsOr | 0x6F0950 |  |
| FindCharacterObjectsOrSphere | 0x6F0B70 |  |
| FindCharacterCoreInfo | 0x6F0DE0 |  |
| FindCharacterCoreInfoSphere | 0x6F1000 |  |
| FindCharacterCoreInfoOr | 0x6F1270 |  |
| FindCharacterCoreInfoOrSphere | 0x6F1490 |  |
| SetCharacterRealizationPriority | 0x6F1700 |  |
| IsRealizedCharacter | 0x6F1820 |  |
| GetMessageBox | 0x6F1920 |  |
| Log | 0x6F19C0 |  |
| Warning | 0x6F1B60 |  |

## ChWorldCharacterHolder

Table 0x1BB4E30, registered by 0x7486B0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Register | 0x748830 |  |
| Unregister | 0x7488D0 |  |

## Demo

Table 0x1BB77F0, registered by 0x76EC20, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| GetCharacterTransformFromCharacter | 0x76ECE0 |  |
| DisableCharacterIntetrp | 0x76EE70 |  |
| EnableCharacterInterpolation | 0x76EEE0 |  |

## FxGeoMaterial

Table 0x1BBC5F0, registered by 0x7F3480, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| CreateInfoManager | 0x7F3220 |  |
| DestroyInfoManager | 0x7F3280 |  |
| ClearCache | 0x7F32C0 |  |
| CreateExtraCache | 0x7F3370 |  |
| RemoveExtraCache | 0x7F3400 |  |

## VoiceCommand

Table 0x1BBED00, registered by 0x8302A0, 0x830380, 0x830470, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetVoiceTypePriority | 0x8304F0 |  |
| SetVoiceEventType | 0x8305A0 |  |

## SubtitlesCommand

Table 0x1BC0600, registered by 0x8511B0, 0x851230, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Display | 0x8500E0 |  |
| DisplayBinary | 0x8501C0 |  |
| DisplayText | 0x850280 |  |
| DisplayUiLang | 0x850380 |  |
| DisplayVoiceLess | 0x850460 |  |
| Stop | 0x850530 |  |
| StopAll | 0x8505C0 |  |
| SetVoiceLanguage | 0x850630 | 2 |
| SetLanguage | 0x8506C0 | 2 |
| SetDefaultGeneratorName | 0x850780 |  |
| SetWaveSyncGeneratorName | 0x850810 |  |
| SetTextColor | 0x8508A0 |  |
| SetSubFilePath | 0x850960 |  |
| SetSubpFilePath | 0x850990 |  |
| SetSubFpkFilePath | 0x8509C0 |  |
| SetChapterNameWithID | 0x8509F0 |  |
| ConvertMsgIDString | 0x850A00 |  |
| ConvertMsgIDInt | 0x850AF0 |  |
| SetChapterNameWithID | 0x8509F0 |  |
| SetDefaultSubPriority | 0x850BB0 |  |
| SetSubPriority | 0x850C70 |  |
| ResetSubPriority | 0x850D50 |  |
| SetSubtitlesForceDisplay | 0x850DD0 |  |
| IsSubtitlesForceDisplay | 0x850E80 |  |
| SetSubtitlesForceNonDisplay | 0x850ED0 |  |
| IsSubtitlesForceNonDisplay | 0x850F80 |  |
| CreateSdTextRelationalTable | 0x850FD0 |  |
| ConvertToSubtitlesId | 0x850FE0 |  |
| SetIsEnabledUiPrioStrong | 0x8510A0 |  |
| SetIsEnableToDisplaySubtitles | 0x8510F0 |  |
| IsPlayingSubtitles | 0x851150 |  |
| ReloadSubtitlesBlockPackage | 0x851190 |  |

## GkTacticalService

Table 0x1BC4490, registered by 0x88D040, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| GetTacticalActionEdge | 0x88D0E0 |  |

## GkTacticalWorld

Table 0x1BC51E0, registered by 0x89D5F0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| ExecuteActiveGenerators | 0x89D750 |  |
| Register | 0x89D760 |  |
| Unregister | 0x89D890 |  |
| SearchObject | 0x89D9B0 |  |
| SearchObjectSphere | 0x89DD50 |  |
| SearchObjectSquarePyramid | 0x89E140 |  |

## GsRoute/ROUTE_ID_EMPTY

Table 0x1BC6330, registered by 0x8A3090, module name guess.

| function | address | uses |
| --- | --- | --- |
| SetRouteSystemScript | 0x8A3BA0 |  |
| GetRouteId | 0x8A3C60 |  |

## ShLightProbe

Table 0x1BC7B40, registered by 0x8BB020, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetReadLpshDebugLogEnable | 0x8BB0D0 |  |
| SetReadLpshDebugLogDisable | 0x8BB0E0 |  |

## TppTonemap

Table 0x1BC8B50, registered by 0x8E2470, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetEnable | 0x8E2510 |  |

## TppSky

Table 0x1BC8FA0, registered by 0x8E7770, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetEnable | 0x8E7810 |  |

## ShGameMainControl

Table 0x1BCA810, registered by 0x90D6C0, 0x90D820, 0x90E560, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| GoToLocation | 0x90DC30 | 3 |
| SetNextStageByPath | 0x90DD80 | 4 |
| StandbyStage | 0x90DE10 |  |
| LoadStage | 0x90DF40 | 12 |
| UnloadStage | 0x90E200 | 4 |
| UnloadStageAll | 0x90E2A0 | 2 |
| ActivateStage | 0x90E320 | 5 |
| DeactivateStage | 0x90E3C0 |  |
| ChangeStageId | 0x90E460 | 8 |
| ChangeStageLabel | 0x90E470 |  |
| IsGuiEditor | 0x90DC10 | 1 |

## ShGameStatus

Table 0x1BCA910, registered by 0x90EE40, 0x90EF40, 0x90F060, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| RegisterGameFlags | 0x90F000 |  |
| GetGameFlag | 0x90F020 |  |
| SetGameFlag | 0x90F040 |  |

## ShNazoManager

Table 0x1BCADB0, registered by 0x912AC0, 0x912B40, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetState | 0x912990 |  |
| SetCondition | 0x912A40 | 1 |

## FadeFunction

Table 0x1BCAF20, registered by 0x913100, 0x913240, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| InitFadeSetting | 0x913950 | 1 |
| FadeSettingDump | 0x9139B0 |  |
| FadeCustomSetting | 0x9139C0 | 1 |
| FadeIgnore | 0x913A60 |  |
| CallFadeOut | 0x913B80 | 5 |
| CallStrongFadeOut | 0x913C80 |  |
| CallFadeIn | 0x913D40 | 1 |
| SetFadeTime | 0x913E30 |  |
| ResetFadeTime | 0x913E80 |  |
| SetFadeColor | 0x913EA0 | 6 |
| ResetFadeColor | 0x914000 |  |
| IsFadeProcessing | 0x914070 |  |
| IsFadeOut | 0x9140D0 |  |

## ShDemo

Table 0x1BCB940, registered by 0x91C630, module name likely.

| function | address | uses |
| --- | --- | --- |
| Skip | 0x91D1C0 | 1 |

## ShGameKit

Table 0x1BCB9C0, registered by 0x91E420, 0x91E440, module name guess.

| function | address | uses |
| --- | --- | --- |
| Set | 0x91E010 |  |
| Reset | 0x91E140 |  |

## GameSystem

Table 0x1BCC1B0, registered by 0x923E20, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SoundPostEvent | 0x924BF0 | 54 |
| SoundPostEvent2D | 0x924C70 |  |
| CallBGM | 0x924CF0 | 4 |
| StopBGM | 0x924D50 | 4 |
| IsPlayingBGM | 0x924DB0 | 4 |
| PadEnable | 0x924E30 |  |

## GameController

Table 0x1BCC220, registered by 0x923E20, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SendMessage | 0x924E70 | 3 |
| GotoEnding | 0x924F10 |  |
| ResetGame | 0x924F30 |  |
| FinishEndingRestartGame | 0x924F50 | 1 |
| GotoGameOver | 0x924F90 | 1 |
| ChangeGameStep | 0x924FB0 | 3 |
| StartFullScreenBlur | 0x925180 | 1 |
| StopFullScreenBlur | 0x925190 | 2 |
| VisibleControlSubtitle | 0x9251A0 | 2 |
| SaveGame | 0x925230 |  |
| DisableOption | 0x925250 | 1 |

## GameFloorLevel

Table 0x1BCC2E0, registered by 0x923E20, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| AddFloorLevel | 0x925270 |  |
| SubFloorLevel | 0x9252A0 | 1 |
| GetFloorLevel | 0x9252D0 |  |
| SetFloorLevel | 0x925300 | 1 |
| IsCurrentFloorName | 0x9253C0 | 131 |
| SetFloorLevelAndName | 0x925460 |  |
| GoNextFloor | 0x925500 |  |
| RelocateGimmicks | 0x925570 | 5 |
| GetLoopCount | 0x925580 | 4 |
| StartNazoTrueEnd | 0x9255D0 | 1 |

## ShParameter

Table 0x1BCE200, registered by 0x9522D0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| ReloadParameterTables | 0x952290 | 1 |
| SetSVarsKeyNames | 0x9522B0 |  |

## Gimmick

Table 0x1BCE2B0, registered by 0x952A50, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| AddPartsPath | 0x952790 | 5 |
| AddMotionPath | 0x9528F0 | 11 |

## GameCore

Table 0x1BD0B70, registered by 0x9703C0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| StartRecording | 0x970470 |  |
| StopRecording | 0x970600 |  |
| PlayRecord | 0x9706D0 |  |
| StopPlayingRecord | 0x9707A0 |  |
| EraseRecord | 0x970870 |  |
| GetRecordState | 0x970940 |  |
| DebugDumpRecord | 0x9709E0 |  |
| SetGameObjectPerfEnabled | 0x970AB0 |  |
| ExportGameObjectPerf | 0x970AD0 |  |
| ExportGameObjectPerfGraph | 0x970AE0 |  |

## lua51.package

Table 0x1BD5A30, registered by 0xA3E380, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| loadlib | 0xA3F050 |  |
| seeall | 0xA3F0E0 |  |

## lua51.base_package

Table 0x1BD5A60, registered by 0xA3E380, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| module | 0xA3E5B0 |  |
| require | 0xA3E830 |  |

## lua51.math

Table 0x1BD5A90, registered by 0xA46FD0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| abs | 0xA47070 |  |
| acos | 0xA470A0 |  |
| asin | 0xA470D0 |  |
| atan2 | 0xA47100 |  |
| atan | 0xA47150 |  |
| ceil | 0xA47180 |  |
| cosh | 0xA471B0 |  |
| cos | 0xA471F0 |  |
| deg | 0xA47230 |  |
| exp | 0xA47260 |  |
| floor | 0xA47290 |  |
| fmod | 0xA472C0 |  |
| frexp | 0xA47310 |  |
| ldexp | 0xA47370 |  |
| log10 | 0xA473C0 |  |
| log | 0xA47400 |  |
| max | 0xA47430 |  |
| min | 0xA474B0 |  |
| modf | 0xA47530 |  |
| pow | 0xA475A0 |  |
| rad | 0xA475F0 |  |
| random | 0xA47620 |  |
| randomseed | 0xA47770 |  |
| sinh | 0xA47790 |  |
| sin | 0xA477D0 |  |
| sqrt | 0xA47800 |  |
| tanh | 0xA47830 |  |
| tan | 0xA47860 |  |

## lua51.table

Table 0x1BD5C60, registered by 0xA47890, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| concat | 0xA478B0 |  |
| foreach | 0xA47A70 |  |
| foreachi | 0xA47B40 |  |
| getn | 0xA47C00 |  |
| maxn | 0xA47C40 |  |
| insert | 0xA47D10 |  |
| remove | 0xA47DE0 |  |
| setn | 0xA47EB0 |  |
| sort | 0xA47F00 |  |

## lua51.io

Table 0x1BD5D00, registered by 0xA48410, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| close | 0xA48830 |  |
| flush | 0xA488C0 |  |
| input | 0xA48970 |  |
| lines | 0xA48990 |  |
| open | 0xA48AF0 |  |
| output | 0xA48BF0 |  |
| popen | 0xA48C10 |  |
| read | 0xA48D00 |  |
| tmpfile | 0xA48D60 |  |
| type | 0xA48DF0 |  |
| write | 0xA48EA0 |  |

## lua51.io_file

Table 0x1BD5DC0, registered by 0xA48410, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| close | 0xA48830 |  |
| flush | 0xA49730 |  |
| lines | 0xA495D0 |  |
| read | 0xA497E0 |  |
| seek | 0xA49830 |  |
| setvbuf | 0xA49920 |  |
| write | 0xA49A10 |  |
| __gc | 0xA49A60 |  |
| __tostring | 0xA49AC0 |  |

## lua51.bit

Table 0x1BD6090, registered by 0xA4D2E0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| tobit | 0xA4D390 |  |
| bnot | 0xA4D400 |  |
| band | 0xA4D470 |  |
| bor | 0xA4D550 |  |
| bxor | 0xA4D630 |  |
| lshift | 0xA4D710 |  |
| rshift | 0xA4D7D0 |  |
| arshift | 0xA4D890 |  |
| rol | 0xA4D950 |  |
| ror | 0xA4DA10 |  |
| bswap | 0xA4DAD0 |  |
| tohex | 0xA4DB40 |  |

## lua51.debug

Table 0x1BD61B0, registered by 0xA51D20, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| debug | 0xA51D40 |  |
| getfenv | 0xA51D50 |  |
| gethook | 0xA51D80 |  |
| getinfo | 0xA51EB0 |  |
| getlocal | 0xA52250 |  |
| getregistry | 0xA52360 |  |
| getmetatable | 0xA52380 |  |
| getupvalue | 0xA523C0 |  |
| setfenv | 0xA523D0 |  |
| sethook | 0xA52430 |  |
| setlocal | 0xA525A0 |  |
| setmetatable | 0xA526B0 |  |
| setupvalue | 0xA52710 |  |
| traceback | 0xA52740 |  |

## lua51.coroutine

Table 0x1BD62D0, registered by 0xA52C10, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| create | 0xA52D80 |  |
| resume | 0xA52DF0 |  |
| running | 0xA52E80 |  |
| status | 0xA52EB0 |  |
| wrap | 0xA52F90 |  |
| yield | 0xA53020 |  |

## lua51.base

Table 0x1BD6360, registered by 0xA52C10, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| assert | 0xA53480 |  |
| collectgarbage | 0xA534F0 |  |
| dofile | 0xA535A0 |  |
| error | 0xA53610 |  |
| gcinfo | 0xA53680 |  |
| getfenv | 0xA536B0 |  |
| getmetatable | 0xA53700 |  |
| loadfile | 0xA53750 |  |
| load | 0xA537A0 |  |
| loadstring | 0xA53830 |  |
| next | 0xA53330 |  |
| pcall | 0xA538D0 |  |
| print | 0xA53930 |  |
| rawequal | 0xA53940 |  |
| rawget | 0xA53990 |  |
| rawset | 0xA539E0 |  |
| select | 0xA53A40 |  |
| setfenv | 0xA53AE0 |  |
| setmetatable | 0xA53BB0 |  |
| tonumber | 0xA53C50 |  |
| tostring | 0xA53D80 |  |
| type | 0xA53EA0 |  |
| unpack | 0xA53EF0 |  |
| xpcall | 0xA53FD0 |  |

## lua51.string

Table 0x1BD6530, registered by 0xA541F0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| byte | 0xA542C0 |  |
| char | 0xA54450 |  |
| dump | 0xA54540 |  |
| find | 0xA545F0 |  |
| format | 0xA54600 |  |
| gfind | 0xA54CD0 |  |
| gmatch | 0xA54CE0 |  |
| gsub | 0xA54D40 |  |
| len | 0xA55220 |  |
| lower | 0xA55280 |  |
| match | 0xA55360 |  |
| rep | 0xA55370 |  |
| reverse | 0xA55430 |  |
| sub | 0xA55520 |  |
| upper | 0xA55600 |  |

## lua51.os

Table 0x1BD6630, registered by 0xA56940, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| clock | 0xA56960 |  |
| date | 0xA56990 |  |
| difftime | 0xA56CA0 |  |
| execute | 0xA56CF0 |  |
| exit | 0xA56D10 |  |
| getenv | 0xA56D30 |  |
| remove | 0xA56D50 |  |
| rename | 0xA56DE0 |  |
| setlocale | 0xA56E80 |  |
| time | 0xA56EE0 |  |
| tmpname | 0xA571E0 |  |

## DemoPlayback

Table 0x1BDAAB0, registered by 0xB18680, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Restart | 0xB18780 |  |
| Pause | 0xB18880 |  |
| SetLoopMode | 0xB188C0 |  |
| SetTransform | 0xB18940 |  |
| Skip | 0xB18AE0 |  |
| SetAutoPause | 0xB18B20 |  |
| GetCurrentFrame | 0xB18C10 |  |

## DemoDaemon

Table 0x1BDAE20, registered by 0xB1B920, 0xB1BEB0, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Play | 0xB1F230 | 6 |
| StopAll | 0xB1F2A0 | 2 |
| PauseAll | 0xB1F2C0 |  |
| SkipAll | 0xB1F370 |  |
| RestartAll | 0xB1F410 |  |
| GetMessageBox | 0xB1F530 |  |
| FindDemoBody | 0xB1F550 |  |
| GetDemoBodies | 0xB1F670 |  |
| FindBlockPositionSetter | 0xB1F780 |  |
| IsDemoPlaying | 0xB1F8A0 |  |
| IsDemoPaused | 0xB1F940 |  |
| SetDemoTransform | 0xB1F9E0 | 5 |
| SetCameraInitFromUser | 0xB1FA80 |  |
| PlayProceduralDemo | 0xB1FAD0 |  |
| StopProceduralDemo | 0xB1FB40 |  |
| IsProceduralDemoPlaying | 0xB1FBB0 |  |

## FxDaemon

Table 0x1BDDAE0, registered by 0xB61900, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Initialize | 0xB619A0 | 1 |
| InitializeReserveObject | 0xB619C0 | 5 |

## FxSystemConfig

Table 0x1BDF3E0, registered by 0xBA8CF0, 0xBA8F20, 0xBA9070, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| SetLimitInstanceMemorySize | 0xBA8D70 | 2 |
| SetLimitInstanceMemoryDefaultSize | 0xBA8DE0 | 2 |
| ResetLimitInstanceMemorySize | 0xBA8E50 |  |
| SetExecutionPrioritySetting | 0xBA8E80 |  |

## Geo

Table 0x1BDFDC0, registered by 0xBBC040, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| RegisterMaterial | 0xBBC100 |  |
| UnregisterMaterial | 0xBBC2A0 |  |
| RegisterTexture | 0xBBC350 |  |

## Gr

Table 0x1BEBF80, registered by 0xCF3B30, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| DBGLocate | 0xCF3BC0 |  |
| DBGColor | 0xCF3BF0 |  |
| DBGFontSize | 0xCF3C40 |  |
| DBGPrint | 0xCF3C70 |  |
| ModelConvert | 0xCF3C90 |  |

## Nav

Table 0x1BF8BC0, registered by 0xE71980, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| ReserveDataChunkIdMapper | 0xE71960 |  |
| GetWorld | 0xE71A30 |  |
| GetNavmeshBelongPosition | 0xE71BD0 |  |
| IsPositionOnNav | 0xE71D50 |  |
| IsPointOnNavmeshFromDirection | 0xE71F50 |  |
| IsPositionInBounder | 0xE72150 |  |
| GetNearestWaypointPosition | 0xE72440 |  |
| IsPossibleRectilinearTravelFromPosition | 0xE726C0 |  |
| IsPossibleRectilinearTravelFromDirection | 0xE72990 |  |
| IsEqualEdgeHandle | 0xE72C80 |  |
| GetNearestMeshPosition | 0xE72E20 |  |
| GetNavigationGraphNodePosition | 0xE730D0 |  |
| GetSegmentGraphNodePosition | 0xE73260 |  |
| GetIntersectPositionNavmeshCapsule | 0xE733F0 |  |

## INVALID_TACTICAL_ACTION_ID/NavTactical

Table 0x1BF8D00, registered by 0xE773C0, module name guess.

| function | address | uses |
| --- | --- | --- |
| SetTacticalActionSystemScript | 0xE77390 |  |

## Nav

Table 0x1BF9510, registered by 0xE84B70, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| RegisterStrategyPriorityParameter | 0xE84F30 |  |

## Nav

Table 0x1BF9740, registered by 0xE8AB10, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| RegisterRestDistanceParameter | 0xE8B040 |  |

## NtDaemon

Table 0x1BFCFD0, registered by 0xEFC400, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Create | 0xEFBFF0 | 1 |

## SoundDaemon

Table 0x1C024C0, registered by 0xF90F20, module name confirmed.

| function | address | uses |
| --- | --- | --- |
| Create | 0xF90FD0 | 1 |
| RegisterAnimEvent | 0xF91020 | 7 |
| MakeLeftRightAnimEventPair | 0xF910A0 | 2 |

