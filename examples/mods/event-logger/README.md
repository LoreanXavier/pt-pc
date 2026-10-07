# Event Logger

A mod made of one `init.lua` and nothing else. It listens to the four events a mod script gets (FloorEnter, StepChange,
Message, Tick) and writes them to `pt.log` as `mod: Event Logger: ...` lines.

Copy the folder into `mods/` next to `pt.exe`, start the game, walk one loop and read `pt.log`. Use it as the starting
point for your own script; the full list of what `init.lua` can call is in `docs/lua_api.md`.
