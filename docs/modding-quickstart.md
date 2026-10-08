# Making your first mod

Start with a copy of a folder in `examples/mods`. Put it in `mods` beside the game executable, restart, and search `pt.log` for `mods:`. A mod's `Assets` folder mirrors the game's `/Assets` paths. Keep extracted original files in a private working folder; distribute your changes only where you have permission.

| What you want to change | File to replace | Starting example |
| --- | --- | --- |
| Observe loops and messages | `init.lua` using the `Mod` API | `event-logger` |
| Recolour an existing model or character | Texture PNG at its material's exact path | `wall-pictures` in the public source |
| Replace a static model | Compatible `.fmdl`; accompanying materials/textures and `.geom` if collision changes | `native-model-template` |
| Replace Lisa or another animated character | Compatible `.fmdl` with the original skeleton, bone names and skin weights; matching textures | `character-template` |
| Move level objects or change level lights | The stage's `.fox2`, retaining its entity references and game script hooks | `level-template` |
| Change authored gameplay | The original game script's `/Assets` path, not the sandbox `init.lua` | See [game script API](lua_api.md) |

`init.lua` is an event observer: it does not offer model spawning, character replacement, or level editing functions. OBJ, FBX and glTF are not runtime replacement formats. `tools/fmdl.py export` exports native models to glTF for inspection; it does not import your edited glTF back into the game.

## A working script example

Copy `examples/mods/event-logger` to `mods/event-logger`. Start the game and check for `mod: Event Logger: started`. Enter another loop and check the next `FloorEnter` line. See the complete event/argument table in [modding.md](modding.md#mod-scripts-initlua). Remove the folder to undo the change.

## Native model replacement

1. Extract the relevant package from your own game files using `tools/psarc.py` and `tools/fpk.py`. The package reader's extraction decrypts its entries. Keep the path under `Assets`, including its case/spelling for readability.
2. Inspect the original with `python tools/fmdl.py info "work/original.fmdl"`. Use `export` to inspect materials and bones in a modelling tool.
3. Produce a compatible Fox Engine FMDL 2.03 replacement. The port has no bundled Blender-to-FMDL importer. A different Fox revision is not automatically compatible.
4. Put it at the same relative path inside `mods/my-model/Assets`. Supply its texture replacements at the material's referenced paths. A model change alone does not replace collision: retain the original `.geom` or produce compatible collision data too.
5. Start on the relevant loop, check the asset load log and look at the object from several angles. Remove the mod and compare the same position.

The template carries no original model or hidden conversion step. Fill it with your own compatible assets before enabling it.

## Animated character replacement

Lisa's model path used by the port is `/Assets/sh/chara/och/Scenes/och0_main0_def.fmdl`. A safe first change is a texture replacement; geometry changes must retain compatible bone names, weights and materials. Keep the original skeleton and motion bindings initially. The `.parts` setup and authored motions decide which model is loaded and how it animates; replacing the visual mesh does not create a new AI behavior.

Test both idle and cutscene animation, including skinning, hair, shadow passes and the mirror. A rigid static model placed at Lisa's model path is not a compatible character replacement. Use the `character-template` README as the checklist.

## Level editing

Find the stage's `.fox2` in its extracted package, then inspect it with `tools/fox2.py convert` and `summary` (see [format and command details](formats/fox2.md#tool)). The output lists models, transforms, lights, traps and entity links. The JSON/XML outputs are inspection formats; the game loads `.fox2` binary files.

For a first edit, change the transform of one decoration with a Fox-compatible editor while preserving its model and entity links. Place the resulting binary `.fox2` at its exact `Assets` path in `level-template`. Avoid changing doors, connectors or puzzle traps until you understand their scripts. A visual transform can also require a collision transform update.

Replacing a stage package replaces the entire package. Prefer one `.fox2` override to an entire `.fpk`/`.fpkd` unless you intend to supply every required entry. The native format and command-line tools are documented in [FOX2](formats/fox2.md).

## If the change does not appear

Check that the folder is enabled in PC Settings, restart, and search `pt.log` for the mod name. Check the complete path rather than only the filename. A file beside `mod.json` is not an asset override; it belongs under `Assets`. Mods are not hot-reloaded. If the replacement crashes, disable it and validate its native format before changing engine code.
