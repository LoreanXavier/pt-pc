# Modding

The port reads the game's files from the extracted PS4 data. A mod is a folder under `mods/` next to `pt.exe` that
stands in for some of those files, and optionally runs a small Lua script. With no `mods` folder the game loads exactly
what it loads without mod support.

A mod can:

- replace any game file under `/Assets/` (packages, data sets, models, scripts, sound packages, banks),
- replace a texture with an `.ftex` or a plain `.png`,
- replace a single sound by its Wwise media id with a `.wem` or a 16-bit PCM `.wav`,
- run its own `init.lua` that listens to a few game events and writes to the log.

The full Lua reference, both for `init.lua` and for the game's own scripts, is in [lua_api.md](lua_api.md).

## Installing a mod

Put the mod's folder into `mods/` next to `pt.exe`:

```
C:\Games\PT\
  pt.exe
  pt.log
  mods\
    brighter-hallway\
      mod.json
      init.lua
      Assets\
        sh\...
```

Mods are read once at start and stay as found until the next start. `pt.log` lists them:

```
info  mods: Brighter Hallway 1.0 (12 files), init.lua
info  mods: 1 found in C:\Games\PT\mods, 12 files replaced
```

(Every log line carries a timestamp in front of the level, left out here.)

Once at least one mod folder exists, PC Settings gets a Mods row at the bottom of its left column, under Graphics.
The Mods page lists the mods with an Off/On switch each. The switch is saved to `pt.ini` and applies at the next start.
The page shows at most 20 mods, ten per column; further mods still load and can be switched in `pt.ini` by hand.

Command line:

| flag | effect |
| --- | --- |
| `--mods <folder>` | read mods from `<folder>` instead of `mods/` next to `pt.exe`. A warning is logged if it is not a folder |
| `--no-mods` | start without any mod, whatever is installed or enabled |

A run without a window (`--headless`, used by the tests) loads mods only when `--mods` is given.

## Folder layout

```
mods\<folder>\
  mod.json      optional
  init.lua      optional, the name must be exactly init.lua in lower case
  Assets\       optional, any letter case
    <path>      stands in for the game's /Assets/<path>
```

Every subfolder of `mods/` is a mod. Folders whose name starts with `.` are skipped, and loose files in `mods/` are
ignored. The folder name is the mod's identity in `pt.ini`, so renaming the folder resets its switch.

`init.lua` is found whatever its letter case, but it is then opened by the exact name `init.lua`. On Windows that makes
no difference; on Linux `Init.lua` would be listed and then fail with "init.lua cannot be read".

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

Every key is optional and the key names are matched without regard to case. Without `name` the folder name is used.
`priority` defaults to 0 (a number, or a number in quotes; it is clamped to plus or minus one billion). `enabled`
defaults to true and accepts `true`/`false`, `1`/`0`, or those as strings. `version` can be a string or a number.
The parser also accepts a UTF-8 BOM, `//` and `/* */` comments, trailing commas, and keys it does not know (nested
objects and arrays are skipped). A `mod.json` that is not a JSON object logs a warning and the mod loads with the
folder name and the defaults.

Keep `name`, `version`, `author` and `description` to plain ASCII if you want them to read the same in every menu
language. The settings page cuts the "name version" label to 26 characters plus `...`, and shows the description, the
author in parentheses and the restart notice as the row's help text.

### Enabling and disabling

The order is: `enabled` from `mod.json`, then `pt.ini` over it. The settings page writes this section:

```ini
[mods]
; a folder in mods/ = 1 on, 0 off; applies at the next start
brighter-hallway = 0
```

A disabled mod is still listed in the log (with `, disabled`) and on the settings page, but none of its files or its
`init.lua` are used.

### Which mod wins

When two enabled mods carry the same file:

1. The higher `priority` wins.
2. On equal priority, the folder whose name sorts last wins (compared in lower case). That gives the usual load-order
   prefixes: `99_patch` overrides `10_base`.

The whole file is replaced. Nothing is merged. The same order decides which `init.lua` runs first: the winning mod's
script runs first and its event handlers are called first.

## Replacing files

`mods\<folder>\Assets\<path>` stands in for the game's `/Assets/<path>`. Matching ignores case, treats `\` like `/`,
and ignores `//` and `./` in paths. Only paths under `/Assets/` can be replaced (the archive's own `as/` spelling of
the same path counts as the same file). A replacement file larger than 1 GiB is refused with a log line and the game's
own file is used.

The replacement applies wherever the game reads that file:

- any loose read from the archive: sound packages (`.sbp`), `Init.bnk`, demo streams, models, parts, effects;
- a whole package: `Assets/sh/level/.../foo.fpk` or `.fpkd` replaces the entire package when it is loaded (the log
  says `vfs: <path> from a mod`);
- a single file inside a package: a file whose path equals the package entry's full path (a stage's `.lua` or `.fox2`
  inside its `.fpkd`, for example) replaces that entry even though the package itself comes from the game;
- the three start-up scripts: `Assets/sh/level_asset/chara/parameter/ShParameterTables.lua`,
  `Assets/sh/level_asset/chara/gimmick/ShGimmickSetUp.lua` and `Assets/sh/sound/scripts/motion/setup.lua`.

A replaced package entry or script is used as it is on disk: ship plain, decrypted content. The game's own entries are
encrypted inside the packages; a mod's file is not decrypted.

To see the original paths and files, extract the archive and the packages with the tools in the `tools/` folder of the
source: `python tools/psarc.py <game>/chunk1.psarc --extract <dir>` and `python tools/fpk.py <package> --extract <dir>`.
The second one also decrypts the package entries.

With any mod file active, the background texture pre-decode that normally runs ahead of a stage load is switched off,
so stage loads can take a little longer than without mods.

### Textures

Textures live in `texture.qar` by path. A mod replaces one with either of these:

| file | works for | notes |
| --- | --- | --- |
| `Assets/<path>/<name>.ftex` plus its `<name>.1.ftexs`, `<name>.2.ftexs`, ... | every texture | the original Fox format. The game's `.ftexs` streams are never mixed with a mod's `.ftex`: a mip whose `.ftexs` is missing from the mod is left out |
| `Assets/<path>/<name>.png` | textures loaded through the texture manager: model materials, effects, UI pictures | 1 to 8192 px on each side, any aspect. Loaded as uncompressed RGBA8 with a full mip chain down to 1 x 1, each level the 2 x 2 average of the one above (averaged in linear light for a colour texture). Colour (sRGB) or linear follows the game's texture; without a game texture of that path, colour. Not for cube or volume textures |

`.dds` is not supported. Convert it to PNG or build an `.ftex`.

The `.ftex` loader understands pixel formats 0 (BGRA8), 1 (R8), 2 (BC1), 3 (BC2), 4 (BC3), 5 (BC5), 6 (R32 float)
and 8 (BC7). The PNG is decoded with stb_image, so it is the file name `.png` that matters; a JPEG renamed to `.png`
loads too, but there is no reason to do that.

To get a texture's picture to edit, run `python tools/ftex.py --path /Assets/<path>/<name> --out <dir>` (the `.ftex` extension may be left on). It
writes `<dir>/single/<path>/<name>.png`. Edit that and place it at `mods\<folder>\Assets\<path>\<name>.png`.

A PNG that cannot be used (wrong size, cube or volume texture, not an image) is logged and the game's texture stays.

Some textures are read without the texture manager and take only an `.ftex`:

- the renderer's own resources: the material atlas (`/Assets/fox/effect/gr_pic/materials_alp_rgba32_nomip_nrt`), the
  film grain noise (`/Assets/sh/effect/vfx_pic/view/fx_viwfilnis01_iy`) and the colour LUTs;
- the button icon atlas and its glow (`/Assets/sh/ui/texture/ButtonIcon/cmn_btn_icon_a_ps4_alp`,
  `/Assets/sh/ui/ModelAsset/sys_option/Pictures/cmn_btn_icon_a_ps4_blr`);
- the one game texture that has a button prompt painted into it, the fallen frame's label
  (`/Assets/sh/environ/object/shsb/label/shsb_labl001/sourceimages/shsb_labl001_p1_bsm_alp`). With a controller other
  than a PlayStation pad it is repainted from the `.ftex`, and that repaint is shown over a `.png` replacement.

Enhanced textures: a PNG is shown as given and is never upscaled. An enhanced replacement is matched to the exact bytes
of the `.ftex` it was made from, so an enhanced set built before a mod was installed does not apply to the mod's
`.ftex`; the mod's texture is shown as it is. Generating the enhanced textures again with the mod installed produces an
upscaled version of the mod's texture.

### Sounds

Three ways to replace a sound:

- One sound: `Assets/sh/sound/wem/<media id>.wem` replaces the Wwise media with that id in whichever loaded bank holds
  it. `python tools/wwise_bank.py extract <bank or .sbp> --out <dir>` writes every embedded media file as
  `<dir>/<bank name>/<id>.wem`. The file
  must be a RIFF/WAVE with `fmt` and `data` chunks in one of these codecs: Wwise Vorbis (tag 0xFFFF), Wwise IMA ADPCM
  (tag 0x0002), or 16-bit PCM (tag 0x0001 or 0xFFFE). A plain 16-bit PCM WAV file works too: name it `<id>.wav`.
  `.wem` is tried first, then `.wav`. Any sample rate works; the mixer resamples. Loop points come from the file's
  `smpl` chunk and markers from its `cue` and `labl` chunks, so keep them if the original has them. The sound's volume,
  pitch and positioning come from the bank and are not changed.
- A whole bank package: `Assets/sh/sound/asset/<name>.sbp` (see `tools/sbp.py`).
- `Init.bnk`: `Assets/sh/sound/asset/Init.bnk`.

A `<id>.wem` or `.wav` that cannot be parsed is logged as ignored and the bank's own media is used. Media streamed from
outside the banks (the demo streams) is not covered by the `<id>` rule; replace that stream file whole.

### Game scripts

The game's own Lua scripts can be replaced like any other file. Ship the plain source text. A replaced script runs in
the game's Lua state with the game's full API (see [lua_api.md](lua_api.md)), so it can change anything the scripts
can, including saving. This is the only way a mod affects gameplay; `init.lua` cannot.

## Mod scripts: init.lua

`mods\<folder>\init.lua` runs once at start, after the game's three start-up scripts and before any stage is loaded. It
does not run in the game's Lua state. Mod scripts share a Lua 5.1 state of their own, and each mod gets its own
environment table in it, so a mod script cannot see or change the game's scripts, globals or save data, and mods cannot
see each other's variables. A mod with only an `init.lua` and no `Assets` folder is fine.

What a mod script has:

| | |
| --- | --- |
| base | `assert error ipairs next pairs pcall xpcall select tonumber tostring type unpack rawequal rawget rawset setmetatable getmetatable`, plus `print` (same as `Mod.Log`), `_G` (the mod's own environment) and `_VERSION`. `getmetatable` works on tables only |
| libraries | the mod's own copies of `string`, `table`, `math` and `coroutine`, plus `os.clock`, `os.time`, `os.date` and `os.difftime`. Changing a library table changes it for that mod only |
| not available | `io`, `debug`, `package`, `load`, `loadstring`, `loadfile`, `dofile`, `require`, `module`, `getfenv`, `setfenv`, `collectgarbage`, `gcinfo`, `newproxy`, the rest of `os` |

### The Mod table

| function | |
| --- | --- |
| `Mod.Log(...)` (also `print`) | joins the arguments with spaces, as `tostring` renders them, and writes one line `mod: <name>: <text>` to `pt.log`. A mod gets at most 500 such lines per run; the 500th says so and the rest are dropped |
| `Mod.On(event, fn)` | calls `fn` on an event (below). Several handlers per event run in the order they were added. An unknown event name is an error |
| `Mod.Floor()` | the current floor's name, for example `"f040"` |
| `Mod.Loop()` | the loop count on that floor |
| `Mod.Step()` | the game controller's current step number (-1 while `init.lua` runs, before the game has started) |
| `Mod.Name` | the mod's name (from `mod.json`, else the folder name) |

### Events

| event | arguments | when |
| --- | --- | --- |
| `"FloorEnter"` | `floor_name, loop` | the hallway moves on to the next floor (the `floor: NextFloor` log line): `floor_name` is the floor now current, `loop` its loop count |
| `"StepChange"` | `old, new` | the game controller requests another step (the `controller: step` log line); the numbers are the previously requested and the newly requested step |
| `"Tick"` | `dt` | every game update, `dt` in seconds. Not while the game is paused |
| `"Message"` | `name, sender` | a message is posted to the game's scripts: `sender` is `"controller"` or the id of the demo that sent it |

`FloorEnter` does not fire for the floor a session starts on. Call `Mod.Floor()` from your first `Tick` to read that
one.

### Errors never stop the game

A mod script stops at its first error and loses all of its hooks until the next start. The error is written to
`pt.log` once:

```
error mods: Floor Logger: floor-logger/init.lua:7: attempt to index a nil value; this mod's hooks are off until the next start
```

Each call into a mod (`init.lua` itself, and every handler call) has a budget of 20 million Lua instructions, so an
endless loop counts as an error. All mod scripts together may use 64 MB. Precompiled Lua bytecode is refused; ship the
source. Other mods keep running.

## Examples

### A retexture

Replace the picture of a texture with a PNG. Find the path with `tools/ftex.py` (or by searching the extracted
`chunk1.psarc` for the model's material), edit the PNG, and place it under the same path:

```
mods\my-retexture\
  mod.json
  Assets\sh\environ\object\shsb\bath\shsb_bath001\sourceimages\shsb_bath001_dc_bsm_alp.png
```

```json
{ "name": "My retexture", "version": "1.0" }
```

The log confirms it when the texture loads: `info  mods: texture /Assets/sh/environ/.../shsb_bath001_dc_bsm_alp from a 1024 x 1024 PNG`.

### A sound swap

Replace one embedded sound by its media id:

```
python tools/wwise_bank.py extract <extracted>/as/sh/sound/asset/sfx_common.sbp --out wem
```

The media files land in `wem/sfx_common/<id>.wem`. The extracted archive spells the top folder `as/`; inside a mod it is
`Assets/`.

Pick the `<id>.wem` you want to change, make your own 16-bit WAV of about the same length, and ship it as:

```
mods\quiet-door\Assets\sh\sound\wem\123456789.wav
```

The log says `info  mods: sound media 123456789 from /Assets/sh/sound/wem/123456789.wav` when the bank loads.

### A floor logger

```lua
-- mods\floor-logger\init.lua
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

## Troubleshooting with pt.log

`pt.log` sits next to `pt.exe`. Search it for `mods:` and `mod:`.

| line | meaning |
| --- | --- |
| `mods: <dir> is not a folder` | the `--mods` folder does not exist. Without `--mods`, a missing `mods/` folder logs nothing |
| `mods: <folder>/mod.json: <reason>; the folder name and defaults are used` | `mod.json` is not a JSON object or cannot be read |
| `mods: <name> <version> (<n> files), init.lua, disabled` | one line per mod found. `-` stands for no version; `init.lua` and `disabled` appear when they apply |
| `mods: <n> found in <dir>, <m> files replaced` | the summary. `<m>` counts the distinct asset paths the enabled mods replace |
| `mods: <path> is <n> bytes, more than the <max> an override may have; the game's own file is used` | a replacement file over 1 GiB |
| `vfs: <path> from a mod` | a whole package came from a mod. A single replaced file inside a package is not logged; check its effect, or add a `Fox.Log` to a replaced script |
| `mods: texture <path> from a <w> x <h> PNG` | a PNG replacement was loaded |
| `mods: <path>.png ignored: <reason>` | the PNG was not usable (size outside 1 to 8192, not an image, cube or volume texture); the game's texture is shown |
| `mods: sound media <id> from <path>` | a `.wem` or `.wav` replaced the bank's media |
| `mods: <path> ignored: <reason>` | the `.wem` or `.wav` could not be parsed; the bank's media is used |
| `mods: <name> init.lua ran` | the mod's script loaded and ran without error |
| `mods: <name>: init.lua cannot be read` | the file exists under another name or case, or cannot be opened |
| `mod: <name>: <text>` | `Mod.Log` output |
| `mods: <name>: <error>; this mod's hooks are off until the next start` | the script failed (syntax error, runtime error, unknown event, instruction budget, memory, precompiled chunk) |
| `lua: load <path>: <error>` or `lua: <error>` | a replaced game script failed to compile or run in the game's Lua state |
| `script: <table>.Exec failed or missing` or `script: <table>.OnMessage failed or missing` | a replaced stage script no longer defines the function the game calls |

If nothing about mods appears at all, the game ran without a `mods/` folder, with `--no-mods`, or headless without
`--mods`.

## Limitations

- Mods are read once at start. Adding, removing or switching a mod needs a restart. Editing a mod's files while the
  game runs has an unpredictable effect, because files are read when the game loads them.
- A file is replaced whole. Packages, data sets and banks are not merged.
- The `init.lua` API is small by design: four events and three read-only queries. Use a replaced game script for
  gameplay changes.
- `.dds` textures are not supported. PNG works only for textures drawn through the texture manager.
- Sound replacement works per embedded media id or per package. Wwise events and their parameters cannot be edited one
  by one.
- The settings page shows at most 20 mods. More are still loaded and can be switched in `pt.ini`.

## Examples

`examples/mods/` has three small mods that were checked in the game: `wall-pictures` (a PNG over the framed wall
pictures), `radio-tone` (a WAV over the f010 radio broadcast) and `event-logger` (an `init.lua` that logs every
event). Copy a folder into `mods/` next to the executable to try it.
