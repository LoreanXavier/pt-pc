# Wall Pictures

A texture replacement. The framed pictures on the hallway walls all share one texture atlas,
`shsb_hous001_a1_bsm`; this mod ships a PNG of the same path with a magenta and cyan test pattern and the words
"PT MOD TEST" painted over it, so you can see at a glance that the replacement is in use.

Copy the folder into `mods/` next to `pt.exe` and start the game. `pt.log` reports
`mods: texture /Assets/sh/environ/object/shsb/house/shsb_hous001/sourceimages/shsb_hous001_a1_bsm from a 2048 x 2048 PNG`
when the hallway loads.

To make your own: `python tools/ftex.py --path /Assets/<path>/<name> --out <dir>` writes the original picture as
`<dir>/single/<path>/<name>.png`. Edit it and place it under `Assets\<path>\<name>.png` in your mod folder.
