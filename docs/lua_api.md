# Lua API

The port has two Lua 5.1 states. The game's own scripts (the three start-up scripts and the `.lua` files inside each
stage's data package) run in the game state, which exposes the modules below. A mod reaches that state only by
replacing one of those scripts (see [modding.md](modding.md), "Game scripts"). A mod's `init.lua` runs in a separate
sandboxed state and sees only the `Mod` table described at the end.

Everything here is what the port implements. Functions of the original PS4 game that the port does not implement are
listed under "Stubs": calling them does nothing.

## The game state

The game state is created with the complete Lua 5.1 standard library (`base`, `package`, `string`, `table`, `math`,
`io`, `os`, `debug`), so a replaced game script can read and write files. There is no sandbox here. An error in a game
script is logged as `lua: <error>` with a traceback and the chunk is abandoned; the game goes on.

### How the game calls scripts

The three start-up scripts run once at start, in this order:

1. `/Assets/sh/level_asset/chara/parameter/ShParameterTables.lua`
2. `/Assets/sh/level_asset/chara/gimmick/ShGimmickSetUp.lua`
3. `/Assets/sh/sound/scripts/motion/setup.lua`

When a stage loads, every entry of its `.fpkd` package whose path ends in `.lua` runs, in package order. Each such
script defines a global table named after its file name without the extension (`demoScriptFinishEnding.lua` defines
`demoScriptFinishEnding`), and the game calls functions in those tables:

| call | from | arguments | return |
| --- | --- | --- | --- |
| `gameState.OnInit()`, `OnLoadStart`, `OnLoadHallway`, `OnStartOption`, `OnStartLogo`, `OnPreOpeningDemo`, `OnPreGame`, `OnGameOver`, `OnResetGameStopGame`, `OnResetGameUnloadStage`, `OnPreEndingStopGame`, `OnPreEndingUnLoadStage`, `OnLoadEnding`, `OnEnding`, `OnRestartGame` | the game controller's steps; `gameState.lua` lives in the resident package | none | ignored |
| `<scriptFile>.Exec(ctx)` | a trap condition with a `GeoTrapScriptCallbackDataElement` exec, on enter and on leave (not while inside) | one table: `trapFlagString` (`"GEO_TRAP_S_ENTER"`, `"GEO_TRAP_S_OUT"`, else `"GEO_TRAP_S_NONE"`), `conditionHandle` (entity), `trapHandle` (entity), `trapBodyHandle` (body entity) | `true` or `1` marks the condition done |
| `<scriptFile>.OnMessage(script, scriptBody, demoId, message)` | a message reaching a message script entity of a loaded stage | the script entity, its body, the sending demo's id (empty for the controller), the message name | ignored |

`<scriptFile>` is the `scriptFile` property of the entity, reduced to the file name without extension. A missing
table or function is logged as `script: <table>.Exec failed or missing` (or `OnMessage`) and treated as 0.

### Fox

| function | effect |
| --- | --- |
| `Fox.Log(...)` | concatenates the arguments and writes them to the log at debug level, as `debug lua: <text>` |

### GameFloorLevel

| function | effect |
| --- | --- |
| `IsCurrentFloorName(name)` | `true` when the current floor is `name` (for example `"f040"`) |
| `GetLoopCount()` | the loop count on the current floor |
| `GetFloorLevel()` | the current floor's index in the floor table |
| `SetFloorLevel(name)` | makes `name` the current floor |
| `SetFloorLevelAndName(index, name)` | sets the floor index and the name stored at it |
| `AddFloorLevel()` | moves one floor forward |
| `SubFloorLevel()` | moves one floor back |
| `GoNextFloor()` | the hallway transition to the next floor |
| `RelocateGimmicks()` | does nothing in the port (the relocation is done by the floor logic); returns 0 |
| `StartNazoTrueEnd()` | activates the true ending puzzle |

### GameSystem

| function | effect |
| --- | --- |
| `SoundPostEvent(name, position)` | posts the Wwise event `name`. With a `position` table (`x`, `y`, `z` numbers) the sound is positional, otherwise 2D |
| `SoundPostEvent2D(name)` | posts the event without a position |
| `CallBGM(name)` | starts the music event |
| `StopBGM(name)` | stops it |
| `IsPlayingBGM(name)` | `true` while it plays |
| `PadEnable(bool)` | requests the pad lock on (`false`) or off (`true`); applied at the controller's next update |

### SoundDaemon

| function | effect |
| --- | --- |
| `RegisterAnimEvent(animEvent, soundEvent)` | maps an animation event name to a sound event |

### GameObject

| function | effect |
| --- | --- |
| `GetGameObjectId(type, name)` | the id of a game object, or `GameObject.NULL_ID` (65535) |
| `SendCommand(id, command)` | sends a command table. The field `id` is the command's name; every other field whose value is a boolean, number or string is an argument. Other values are ignored |

### ShGameMainControl

| function | effect |
| --- | --- |
| `LoadStage(fpkPath, label, connector, baseLabel, baseConnector)` | requests a stage load, placed by connecting `connector` to `baseConnector` of the stage `baseLabel` |
| `SetNextStageByPath(fpkPath)` | requests the start location |
| `ActivateStage(label)` | requests activation |
| `DeactivateStage(label)` | deactivates |
| `UnloadStage(label)` | requests an unload |
| `UnloadStageAll()` | unloads every stage |
| `ChangeStageId(from, to)`, `ChangeStageLabel(from, to)` | renames a stage's label (both names do the same) |
| `IsGuiEditor()` | always `false` |

### GameController

| function | effect |
| --- | --- |
| `ChangeGameStep(name)` | requests a step by name: `Init`, `EndPreface`, `StartGame`, `ResetGame`, `Endf120`, `GotoGameOver`, `GotoEnding`, `FinishEndingRestartGame`. Any other name logs `controller: unknown game step` |
| `SendMessage(name)` | posts a controller message to the message scripts (and to mods' `Message` handlers with sender `"controller"`) |
| `GotoGameOver()` | step 16 |
| `GotoEnding()` | step 21 |
| `ResetGame()` | step 17 |
| `FinishEndingRestartGame()` | the end of the ending's credits |
| `SaveGame()` | requests a save at the controller's next update |
| `DisableOption()` | blocks the pause menu from the next update on |
| `StartFullScreenBlur()`, `StopFullScreenBlur()` | the full screen blur on and off |
| `VisibleControlSubtitle(messageId, bool)` | shows or hides a subtitle by message id |

### FadeFunction

| function | effect |
| --- | --- |
| `SetFadeColor(r, g, b, a)` | 0 to 255 each; `a` defaults to 255, the others to 0 |
| `ResetFadeColor()` | opaque black |
| `CallFadeOut(seconds)`, `CallFadeIn(seconds)`, `CallStrongFadeOut(seconds)` | start a fade; `seconds` defaults to the fade time |
| `SetFadeTime(seconds)` | the default fade time (defaults to 1) |
| `ResetFadeTime()` | back to 1 second |
| `FadeCustomSetting(normal, strong)` | the fade priorities |
| `FadeIgnore()` | toggles the ignore flag and returns its new value |
| `IsFadeProcessing()` | `true` while a fade runs |
| `IsFadeOut()` | `true` when faded out |
| `InitFadeSetting()`, `FadeSettingDump()` | do nothing |

### DemoDaemon and ShDemo

| function | effect |
| --- | --- |
| `DemoDaemon.Play(demoId)` | plays a demo |
| `DemoDaemon.SetDemoTransform(demoId, quat, position)` | places a demo; `quat` is a table with `x`, `y`, `z`, `w`, `position` a table with `x`, `y`, `z` |
| `DemoDaemon.StopAll()` | stops every demo |
| `DemoDaemon.IsDemoPlaying(demoId)` | `true` while that demo plays; without an argument, `true` while any demo plays |
| `ShDemo.Skip(demoId)` | skips a demo |

### TppEffectUtility

| function | effect |
| --- | --- |
| `EnableColorCorrectionLutControl(bool)` | the script's LUT choice on or off |
| `SetColorCorrectionLut(name)` | the colour LUT by name (the default is `common_saturation_a`) |

### ShNazoManager

| function | effect |
| --- | --- |
| `SetCondition(name)` | sets a puzzle condition |

### Gimmick

| function | effect |
| --- | --- |
| `AddPartsPath({partName = ..., path = ...})` | registers a gimmick part's model path |
| `AddMotionPath({key = ..., path = ...})` | registers a gimmick motion path |

### ShGameStatus

| function | effect |
| --- | --- |
| `Set(holder, flag)` | the holder acquires the status flag (for example `S_DISABLE_GAME_PAUSE`) |
| `Reset(holder, flag)` | releases it |

### ShParameter

| function | effect |
| --- | --- |
| `ReloadParameterTables(table)` | reads the parameter table that `ShParameterTables.lua` builds: its `playerParameter`, `handyLightParameter` and `lightingParameter` children |

### PtDebug (port only)

| function | effect |
| --- | --- |
| `GetEntity(stageLabel, shortName, body)` | the entity of that name in the stage, as a body handle when `body` is true; `nil` when not found |
| `Raycast(height)` | casts 5 m forward from the player's feet raised by `height` (default 1) and logs the hit |
| `DumpCollision(path)` | writes the active collision triangles, traps, connectors and surfaces to a text file |
| `Surface()` | logs the surface material under the player |

### Entities

Entity handles are userdata. They come from `Exec` and `OnMessage` arguments, from `GetChildren`, from entity-typed
properties and from `PtDebug.GetEntity`. A "body" handle refers to the same entity's run-time state.

Methods:

| method | effect |
| --- | --- |
| `GetDataBody()`, `GetDataBodyWithReferrer()` | the body handle of the entity |
| `GetChildren()` | a table of the entity's `children` (or `members`) handles, keyed by name, or by index when a child has no name |
| `IsKindOf(className)` | `true` for the entity's class, for `"Entity"`, for `"Data"`, for a body also `"<class>Body"` and `"DataBody"`, and `"Light"` for `PointLight` and `SpotLight` |
| `GetClassName()` | the class name, with `Body` appended for a body handle |
| `GetWorldTransform()` | a table with `translation` (Vector3), `rotQuat` (Quat) and `scale` (Vector3) |
| `Visible()`, `Invisible()` | sets the body's visibility |
| `SetupMessageBox()` | registers the entity as a message receiver |

Fields read with `.`:

| field | value |
| --- | --- |
| `name` | the entity's name |
| `worldTransform` | as `GetWorldTransform()` |
| `enable`, `isVisible`, `isGeomActive` | body handles only: the run-time flags |
| any other name | the entity's data set property of that name: numbers, booleans, strings, paths, other entities, Vector3 values (with the Vector3 metatable), Vector4 and Color values (plain tables with `x`, `y`, `z`, `w`), Quat values. A property that is not a single static value (an array, a list, a string map) comes as a table |

Writing `enable`, `isVisible` or `isGeomActive` on a handle changes the body's flag. Writing any other field is
ignored (logged at debug level). `==` compares two handles by stage, entity and body. `tostring` gives
`Class(name)`. `Entity.IsNull(handle)` is `true` for anything that is not a live handle of a loaded stage.

### Vector3 and Quat

`Vector3(x, y, z)` returns a table with fields `x`, `y`, `z` and the methods `GetX()`, `GetY()`, `GetZ()`.
`Quat(x, y, z, w)` returns a table with `x`, `y`, `z`, `w` (`w` defaults to 1) and the same three getters. Both print
their components with `tostring`. The functions that take a position or a rotation accept any table with those fields.

### Stubs

These exist so the original scripts run. They take any arguments, do nothing and return nothing:

| module | functions |
| --- | --- |
| `Fox` | `GetPlatformName`, `SetActMode`, `Error`, `ExportSerializeInfo`, `Require` |
| `GrRenderPlugin` | `AddPlugin` |
| `GrTools` | `LoadShaderPack`, `SetTerrainMaterialTexture`, `SetEnablePackedSmallTextureStreaming`, `SetEnableLnmForTerrainNormal`, `SetEnableLnmForDecalNormal`, `GetDeviceName`, `FontSystemLoad`, `AddExtraShaderPerfLua`, `FontSystemInit`, `SetDefaultTextureLoadPath`, `SetReflectionTexture`, `SetMaterialTexture`, `SetMaterialParamBinary`, `SetGen8RenderingMode`, `SetupSystemShaderResources` |
| `Geo` | `GeoLuaSetDebugMaterialColor`, `GeoLuaSetDebugCollisionColor` |
| `AssetConfiguration` | `SetTargetDirectory`, `GetConfigurationFromAssetManager`, `SetDefaultTargetDirectory`, `RegisterExtensionInfo`, `SetDefaultCategory`, `GetDefaultCategory`, `SetLanguageGroupExtention`, `SetGroupCurrentLanguage` |
| `ShGameMainControl` | `GoToLocation`, `StandbyStage` |
| `Pad` | `RegisterButtonAssign`, `RegisterAxisAssign`, `ConfigDefaultAssigns` |
| `FoxGameFrame` | `SetGameFrameWaitType` |
| `SoundDaemon` | `MakeLeftRightAnimEventPair`, `Create` |
| `SoundCoreDaemon` | `SetAssetPath`, `Create`, `SetInterferenceRTPCName`, `SetDopplerRTPCName`, `SetRearParameter` |
| `FxDaemon` | `InitializeReserveObject`, `Initialize` |
| `FxSystemConfig` | `SetLimitInstanceMemorySize`, `SetLimitInstanceMemoryDefaultSize` |
| `EdGraphFactory` | `CreateSetting`, `AddSetting`, `GetInstance`, `DefaultSetting` |
| `UiDaemon` | `SetFontTypeTransTable`, `GetInstance`, `ClearDrawPriorityTable`, `SetDrawPriorityTable`, `SetPrefetchTextureTable` |
| `NavWorldDaemon` | `AddWorld`, `AddScene` |
| `SubtitlesCommand` | `SetVoiceLanguage`, `SetLanguage` |
| `SubtitlesDaemon` | `SetDefaultVoiceLanguage`, `GetDefaultVoiceLanguage` |
| `ShNazoManager` | `SetState` |
| `ShGameStatus` | `RegisterGameFlags`, `GetGameFlag`, `SetGameFlag` |
| `DemoDummyFloorLevel` | `IsMyFloor_FloorName` |
| one function each | `EdDemoEditBlockController.AddToolsBlockPath`, `Preference.GetPreferenceEntity`, `EdAnimGraphControlAdapter.GetInstance`, `Editor.Setting`, `Editor.GetInstance`, `PhDaemon.SetMemorySize`, `PhDaemon.SetMaxRigidBodyNum`, `Script.LoadLibrary`, `EdPreview.GetManager`, `PathMapper.Add`, `EditableBlockPackage.RegisterPackageExtensionInfo`, `CameraPriority.RegisterPriorities`, `CameraSelector.SetMainInstance`, `Pad2.Init`, `ReplayService.Boot`, `NtDaemon.Create`, `FoxTestLuaActor.ExecGlobal`, `MiniPerfView.SetEnable`, `BlockSizeView.SetEnable`, `FoxFadeIo.Create`, `GsRouteDataNodeEvent.SetEventDefinitionPath`, `GsRouteDataEdgeEvent.SetEventDefinitionPath`, `ShLightCapture.InitInstance`, `EdGraphAdapter.Setting`, `DataCluster.GetActorsByClassName`, `DataActor.GetActorsByClassName` |

A stub call is logged at debug level as `lua stub <Module>.<Function>(<args>)`.

## The mod state (init.lua)

Every enabled mod's `init.lua` runs once at start, after the three start-up scripts and before any stage loads, in
mod order (highest priority first, then the folder name that sorts last). All mod scripts share one Lua 5.1 state,
with a separate environment table per mod.

### Available

| | |
| --- | --- |
| base | `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `xpcall`, `select`, `tonumber`, `tostring`, `type`, `unpack`, `rawequal`, `rawget`, `rawset`, `setmetatable`, `getmetatable` (tables only; returns `nil` for anything else), `print` (same as `Mod.Log`), `_G` (the mod's environment), `_VERSION` |
| libraries | own copies of `string`, `table`, `math`, `coroutine`; `os` with only `clock`, `time`, `date`, `difftime` |
| not available | `io`, `debug`, `package`, `load`, `loadstring`, `loadfile`, `dofile`, `require`, `module`, `getfenv`, `setfenv`, `collectgarbage`, `gcinfo`, `newproxy`, the rest of `os` |

### Mod

| member | |
| --- | --- |
| `Mod.Name` | string: the mod's name from `mod.json`, else its folder name |
| `Mod.Log(...)` | converts every argument with `tostring`, joins them with single spaces and writes `mod: <name>: <text>` to `pt.log`. At most 500 lines per mod per run; the 500th line carries a note and further lines are dropped |
| `Mod.On(event, fn)` | adds `fn` as a handler of `event` (one of the four names below). Handlers of one event run in the order they were added. An unknown event name raises an error |
| `Mod.Floor()` | string: the current floor name, for example `"f040"` |
| `Mod.Loop()` | integer: the loop count on the current floor |
| `Mod.Step()` | integer: the game controller's current step; -1 before the game has started. Inside a `StepChange` handler it still returns `old` |

### Events

| event | handler arguments | fired |
| --- | --- | --- |
| `FloorEnter` | `floor` (string), `loop` (integer) | when the hallway moves on to the next floor; not for the floor a session starts on |
| `StepChange` | `old` (integer), `new` (integer) | when the controller requests a step other than the one requested before; `old` is the previously requested step |
| `Tick` | `dt` (number, seconds) | every game update; not while paused |
| `Message` | `name` (string), `sender` (string) | when a message is posted to the game's scripts; `sender` is `"controller"` or the sending demo's id |

Handlers run synchronously at the event site, mod by mod in mod order. An event raised while handlers are running is
not delivered to mods.

### Limits

| limit | value |
| --- | --- |
| Lua instructions per call into a mod (`init.lua` itself and each handler call) | 20,000,000 |
| memory for all mod scripts together | 64 MB |
| `Mod.Log` lines per mod per run | 500 |
| precompiled chunks | refused; `init.lua` must be source text |

The first error in a mod (while loading, in `init.lua`, in a handler, over the instruction budget, out of memory, or
an unknown event name) is logged once as `mods: <name>: <error>; this mod's hooks are off until the next start`. That
mod's handlers are removed; the other mods and the game continue.
