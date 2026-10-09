# Modding

Start with [the quickstart and model/character/level examples](modding-quickstart.md).

The port plays P.T. as the PS4 original does. Mods are the one exception, and only when you install one: with no `mods`
folder (or an empty one) the game reads exactly the files it reads without mod support, and every lookup is a single null
check.

A mod can:

- replace any game file under `/Assets/` (packages, data sets, models, scripts, sound packages and so on),
- replace textures (`.ftex`, or a plain `.png`),
- replace single sounds by their Wwise media id (`.wem`, or a 16-bit PCM `.wav`),
- run its own Lua script (`init.lua`) that reacts to game events.

## Installing a mod

Put the mod's folder into `mods/` next to `pt.exe`:

```
pt.exe
mods/
  brighter-hallway/
    mod.json
    init.lua
    Assets/
      sh/...
```

The log (`pt.log`) lists every mod found at start:

```
mods: Brighter Hallway 1.0 (12 files), init.lua
mods: 1 found in C:\Games\PT\mods, 12 files replaced
```

PC Settings shows a **Mods** row under Graphics when at least one mod is installed. Its page lists the mods with an On/Off
switch. A change is saved to `pt.ini` and takes effect at the next start.

Command line:

| flag | effect |
| --- | --- |
| `--mods <dir>` | read mods from `<dir>` instead of `mods/` next to `pt.exe` |
| `--no-mods` | start without any mod, whatever is installed or enabled |

Headless runs (`--headless`, which the comparison and test tools use) load mods only when `--mods` is given.

## Folder layout

```
mods/<folder>/
  mod.json          optional
  init.lua          optional
  Assets/           optional; the folder name is not case sensitive
    <path>          replaces the game file /Assets/<path>
```

The folder name identifies the mod in `pt.ini`. Folders whose names begin with `.` are skipped.

### mod.json

```json
{
  "name": "Brighter Hallway",
  "version": "1.0",
  "author": "you",
  "description": "Raises the hallway lamp's intensity.",
  "priority": 10,
  "enabled": true
}
```

Every key is optional. Without `name`, the folder name is used. `priority` defaults to 0 and `enabled` defaults to true.
The parser accepts `//` and `/* */` comments, trailing commas and keys it does not know. A `mod.json` that cannot be read
logs a warning, and the mod then loads with the defaults.

Use ASCII for `name`, `version`, `author` and `description` if you want them to display in every menu language. The menu
fonts only contain the characters the game's own text uses.

`pt.ini` can switch a mod on or off whatever its `enabled` says (the settings page writes these lines):

```ini
[mods]
brighter-hallway = 0
```

### Which mod wins

When two enabled mods carry the same file:

1. The higher `priority` wins.
2. On equal priority, the folder whose name sorts **last** wins (case is ignored). That gives the usual load-order
   prefixes: `99_patch` overrides `10_base`.

The whole file is replaced. Nothing is merged.

## Replacing files

The file `mods/<folder>/Assets/<path>` stands in for the game's `/Assets/<path>`. Matching ignores case and treats `\` the
same as `/`. The replacement applies wherever the game reads that file:

- loose reads of the archive (`Vfs::ReadFile`): sound packages, demo streams, models, parts and effects;
- **whole packages**: `Assets/sh/level/.../foo.fpk` or `.fpkd` replaces the entire package when it is loaded;
- **single files inside a package**: a file whose path is the package entry's path (for example a stage's `.lua` or
  `.fox2`, `/Assets/sh/...`) replaces that entry, even though the package itself comes from the game;
- the start-up scripts (`ShParameterTables.lua`, `ShGimmickSetUp.lua`, the sound `setup.lua`).

Use `python tools/psarc.py <game>/chunk1.psarc --extract <dir>` and `python tools/fpk.py <package> --extract <dir>` to
see the original paths and files. The second command also decrypts package entries.

### Textures

Textures live in `texture.qar` by path. A mod replaces a texture with either of these:

| file | works for | notes |
| --- | --- | --- |
| `Assets/<path>/<name>.ftex` plus its `<name>.1.ftexs` ... | every texture | the original Fox format. The game's `.ftexs` are never mixed with a mod's `.ftex`, so ship all of them |
| `Assets/<path>/<name>.png` | the textures of models, effects and the UI (everything drawn through the texture manager) | any size up to 8192 px. It loads as uncompressed RGBA8 with a mip chain built at load time. Colour or linear follows the game's texture. Not for cube or volume textures |

`.dds` is **not** supported. Convert it to PNG, or build an `.ftex`.

A PNG is used as given: the enhanced texture upscaler does not process it. A modded `.ftex` can still be upscaled if
Enhanced textures is on.

To find a texture's path and get its picture to edit, run `python tools/ftex.py --path /Assets/<path>/<name>.ftex --out
<dir>`. It writes `<name>.png`. Edit that file and place it at `mods/<folder>/Assets/<path>/<name>.png`.

Some textures are read without the texture manager, and these take only an `.ftex`:

- the renderer's own resources: the material atlas, the film grain noise and the colour LUTs;
- the button icon atlas;
- the game textures with button prompts painted in. When a controller other than a PlayStation pad is used, they are
  repainted from the `.ftex`, and that repaint covers a `.png` replacement.

### Sounds

You have three ways to replace a sound:

- **One sound**: `Assets/sh/sound/wem/<media id>.wem` replaces the Wwise media with that id, in whichever bank holds it.
  `python tools/wwise_bank.py extract <bank> --out <dir>` writes every embedded media file as `<id>.wem`. Accepted formats
  are Wwise Vorbis, Wwise IMA ADPCM and 16-bit PCM. A plain 16-bit PCM WAV file works as well: name it `<id>.wav`. Any sample
  rate works. Loops, markers and the sound's volume, pitch and positioning come from the bank, so keep the length and loop
  points close to the original's.
- **A whole bank package**: `Assets/sh/sound/asset/<name>.sbp` (see `tools/sbp.py`).
- **Init.bnk**: `Assets/sh/sound/asset/Init.bnk`.

Media streamed from outside the banks is not covered by the `<id>.wem` rule.

### Game scripts

The game's own Lua scripts can be replaced like any other file. Ship the decrypted source text: the encryption of the
originals is not needed. A replaced script runs in the game's Lua state with the game's full API, so it can change
anything, including saving. This is the only way a mod can affect the game beyond what it shows and plays.

## Mod scripts: init.lua

`mods/<folder>/init.lua` runs once at start, after the game's own start-up scripts. It does not run in the game's Lua state:
mod scripts have a Lua 5.1 state of their own, and every mod gets its own environment table there. A mod script therefore
cannot see or change the game's scripts, globals or save data, and mods cannot see each other's variables.

What a mod script has:

| | |
| --- | --- |
| base | `assert error ipairs next pairs pcall xpcall select tonumber tostring type unpack rawequal rawget rawset setmetatable getmetatable` (`getmetatable` works on tables only) |
| libraries | own copies of `string table math coroutine`, plus `os.clock os.time os.date os.difftime` |
| not available | `io`, `load`, `loadstring`, `dofile`, `require`, `module`, `getfenv`, `setfenv`, `debug`, `collectgarbage`, the rest of `os` |

### The Mod table

| function | |
| --- | --- |
| `Mod.Log(...)` (also `print`) | writes one line `mod: <name>: <text>` to `pt.log`. A mod gets at most 500 lines |
| `Mod.On(event, fn)` | calls `fn` on an event (below). Several handlers per event run in the order they were added |
| `Mod.Floor()` | the current floor's name, for example `"f040"` |
| `Mod.Loop()` | the loop count on that floor |
| `Mod.Step()` | the game controller's current step number |
| `Mod.Name` | the mod's name |

### Events

| event | arguments | when |
| --- | --- | --- |
| `"FloorEnter"` | `floor_name, loop` | the hallway moves on (the game's `floor: NextFloor` log line): `floor_name` is the floor now current, `loop` its loop count |
| `"StepChange"` | `old, new` | the game controller requests another step (`controller: step` log line) |
| `"Tick"` | `dt` | every game update, `dt` in seconds. Not while the game is paused |
| `"Message"` | `name, sender` | the game posts a message to its scripts: `sender` is `"controller"` or the demo id |

`FloorEnter` does not fire for the floor a session starts on. Call `Mod.Floor()` from your first `Tick` to read that one.

### Example

```lua
-- mods/floor-logger/init.lua
local entered = 0

Mod.Log("hello from", Mod.Name)

Mod.On("FloorEnter", function(floor, loop)
    entered = entered + 1
    Mod.Log("entered", floor, "loop", loop, "(" .. entered .. " floors this session)")
end)

Mod.On("Message", function(name, sender)
    if sender ~= "controller" then
        Mod.Log("demo", sender, "sent", name)
    end
end)

local clock = 0
Mod.On("Tick", function(dt)
    clock = clock + dt
end)
```

### Errors never stop the game

A mod script stops at the first error and loses all of its hooks until the next start. The error is written to `pt.log`
once:

```
mods: floor-logger: floor-logger/init.lua:7: attempt to index a nil value; this mod's hooks are off until the next start
```

Each call into a mod (`init.lua` itself, and each handler call) has a budget of 20 million Lua instructions, so an endless
loop counts as an error. All mod scripts together may use 64 MB. Precompiled Lua bytecode is refused; ship the source.
Other mods keep running.

## Limitations

- Mods are read once at start. Adding, removing or switching a mod needs a restart. Changing a mod's files while the game
  runs has an unpredictable effect, because files are read when the game loads them.
- A file is replaced whole. Packages, data sets and banks are not merged.
- The mod script API is deliberately small: events and read-only queries. Use a replaced game script for gameplay changes.
- `.dds` textures are not supported. PNG works only for textures drawn through the texture manager.
- Sound replacement works per embedded media id or per package. Wwise events and their parameters cannot be edited one by
  one.
- At most 20 mods are listed on the settings page. More are still loaded and can be switched in `pt.ini`.
- Headless runs without `--mods` ignore mods.

## For developers

| | |
| --- | --- |
| `src/engine/fs/mods.h/.cpp` | discovery, `mod.json`, the override index (one hash lookup per read, lower-cased path without `/Assets/`), and `mods::ReadOverride`, which the readers call (null check without mods) |
| `src/engine/fs/vfs.cpp`, `fpk.cpp` | file, package and package entry replacement |
| `src/engine/assets/ftex.cpp`, `src/engine/render/texture_manager.cpp` | `.ftex` and `.png` textures |
| `src/engine/audio/sound_package.cpp` | `<id>.wem` / `<id>.wav` media |
| `src/engine/script/mod_lua.h/.cpp` | the mod script sandbox |
| `src/game/game.cpp` (`LoadModScripts`), `floor_level.cpp`, `game_controller.cpp`, `message_system.cpp` | events |
| `src/main.cpp` (`MountMods`, `ModSections`) | `--mods`, `--no-mods`, the settings page |
| `tests/mods_test.cpp` (`pt_mods_test`) | priority rules, `mod.json`, file lookups and the sandbox; needs no game files |
