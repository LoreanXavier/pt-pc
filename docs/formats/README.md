# File format notes

These came out of reverse engineering the game's data. They are the only description of these formats I know of, and
the tools in `tools/` are written against them. Byte layouts are complete and are what the port's loaders implement;
where a field's meaning is a guess it says so.

| File | Format | Tool |
|---|---|---|
| `psarc.md` | `chunk1.psarc`, the main archive, and the `.fpk` packages inside it | `tools/psarc.py`, `tools/fpk.py` |
| `pkg.md`, `self.md` | PS4 package and executable containers | `tools/pkginfo.py`, `tools/self2elf.py`, `tools/dynlib.py` |
| `foxcrypt.md` | the encryption on the Lua files | `tools/foxcrypt.py` |
| `fox2.md` | Fox Engine data sets (entities and their properties) | `tools/fox2.py`, `tools/foxhash.py` |
| `fmdl.md`, `geom.md` | models and collision geometry | `tools/fmdl.py`, `tools/geom.py` (`tools/meshview.py` is their preview rasterizer) |
| `textures.md` | `texture.qar`, `.ftex`, `.pftxs`, `pathid_list_ps4.bin` | `tools/qar.py`, `tools/ftex.py`, `tools/pathcode.py` |
| `fsm.md`, `motion.md` | cutscene streams, animation archives, rigs | `tools/fsm.py`, `tools/motion.py` |
| `fsop.md` | the PS4 shader packs | `tools/fsop.py`, `tools/gcn.py`, `tools/gcnexpr.py` |
| `ui.md` | fonts, language files, subtitles, UI models, layouts and animations | `tools/ffnt.py`, `tools/lng.py`, `tools/uif.py` |
| `vfx.md` | particle effects and lens flares | `tools/vfx.py` |
| `audio.md` | Wwise banks, sound packages and streams | `tools/sbp.py`, `tools/wwise_bank.py`, `tools/wwise_names.py`, `tools/audio_dump.py` |
| `voice.md` | the voice recognition data and the port's recognizer | `tools/voice_check.py`, `tools/voice_mic_session.ps1` |

Every tool prints its options with `--help`. Most of them want the archive extracted first:

    python tools/psarc.py <game>/chunk1.psarc --extract <dir>
    python tools/fpk.py <dir>/as/sh/level/common/resident.fpk --extract <dir>/fpk/resident
