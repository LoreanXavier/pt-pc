# Audio

P.T. uses Wwise 2013 (bank version 88) inside Fox Engine's sound layer. All bank media is embedded in the banks; there
are no loose `.wem`, no stream packages and no ATRAC9. Cutscene audio is interleaved into some demo streams (`.fsm`).
Tools: `tools/sbp.py` (list or extract a sound bank package), `tools/wwise_bank.py` (`info`, `json`, `extract` on a
bank), `tools/fsm.py --extract` (the stream audio), `tools/audio_dump.py` (the whole pipeline: unpack, decode with
vgmstream, index events and names), `tools/wwise_names.py` (name search).

## Inputs

| file | role |
| --- | --- |
| `as/sh/sound/asset/Init.bnk` | Wwise init bank: buses, aux buses, effects, state groups, game parameters |
| `as/sh/sound/asset/{sys_resident,bg_common,sfx_common,bgm_common}.sbp` | Fox sound bank packages, one bank each (sfx_common also a `.sab`) |
| `/Assets/sh/sound/package/sh_common_sound.sdf` (resident fpkd) | Fox `SoundDataFileInfo`: which banks to load |
| `*.fsm` (resident fpk, `demo_stream/`, `demo_stream/#Eng/`) | demo streams, five of them carry audio |
| `/Assets/sh/effect/vfx_data/sound/fxsd_*.vfx` | effect files whose sound node names a Wwise event |
| `as/sh/level/ui/subtitles/EngVoice/<Lang>Text/subtitle.fpk` holding `/Assets/sh/ui/Subtitles/subp/EngVoice/<Lang>Text/trial.subp` | subtitle text, linked to voice media through the `.sab` |

## Fox sound bank package (.sbp)

Little-endian. Same layout GzsTool reads for MGSV.

| offset | size | field |
| --- | --- | --- |
| 0x00 | 4 | magic `SBPL` |
| 0x04 | 1 | entry count |
| 0x05 | 2 | header size, always 8 + 12 * count |
| 0x07 | 1 | 0 |
| 0x08 | 12 * n | entries: char[4] kind (`bnk\0`, `sab\0`; GzsTool also knows `stp\0`), u32 offset, u32 size |

Payloads start at 0x20 and are 16-byte aligned. In P.T.: `bg_common`, `bgm_common`, `sys_resident` hold one `bnk`;
`sfx_common` holds a `bnk` and a 1,440-byte `sab`.

## SAL3 (.sab): voice marker to subtitle table

| offset | size | field |
| --- | --- | --- |
| 0x00 | 4 | magic `SAL3` |
| 0x04 | 4 | record count (18) |
| 0x08 | 16 * n | u64 key, u64 record offset (from the start of the table) |

Record: u32 action count (1), char[4] action kind (`st`, read as subtitle; guess), u32 parameter size (12), 12
parameter bytes (meaning unknown), 6 unknown bytes, the subtitle id (ASCII, zero terminated, for example
`tria1000_101010`), then the key again as 6 big-endian bytes.

The key is Fox StrCode64 of a Wwise marker label found in a voice `.wem` (see Media). All 18 keys resolve: 17 labels of
the form `TRIA1000_<id>_0_<speaker>` (speaker `caster`, `baby`, `mib`) and `Peephole_Theater`. The subtitle id's StrCode64
low 32 bits, computed on the lowercase id, is the key of the entry in `trial.subp`. The chain at runtime: Wwise marker
callback, label, StrCode64, `.sab` record, subtitle id, `.subp` entry. `sbp.py --sal` prints the records.

`.subp` index: u8 1, u8 0x4C, u16 entry count, then entries of u32 key and u32 offset. The entry layout is in `ui.md`.

## Wwise banks

All five banks: `BKHD` version 88, project id 397, language 0 (SFX), feedback flag 1. The bank id is FNV-1 32 of the
lowercase bank name (`sfx_common` = 0x5C07F582). `BKHD` is padded so that `DATA` starts 16-byte aligned. Chunks:

| bank | chunks | media | HIRC objects |
| --- | --- | --- | --- |
| Init | BKHD, STMG, HIRC, ENVS | 0 | 66: Bus 22, AuxBus 9, FeedbackBus 1, FxShareSet 3, FxCustom 4, State 27 |
| sys_resident | BKHD, DIDX, DATA, HIRC, STID | 5 | 90 |
| bg_common | same | 6 | 21 |
| sfx_common | same | 291 | 1,030 |
| bgm_common | same | 11 | 64 |

`DIDX` is a list of u32 media id, u32 offset into `DATA`, u32 size. `STID` maps bank ids to names. `ENVS` holds the
obstruction and occlusion curves for volume and LPF.

HIRC: u32 count, then items of u8 type, u32 size, u32 id, body. Object types in the five banks (1,271 total): Sound 390,
Action 301, Event 246, RanSeqCntr 63, FxCustom 52, Attenuation 40, ActorMixer 35, FeedbackNode 34, LayerCntr 29, State
27, Bus 22, MusicTrack 10, AuxBus 9, SwitchCntr 5, FxShareSet 3, MusicSegment 2, MusicRanSeqCntr 1, DialogueEvent 1,
FeedbackBus 1. Actions: Play 183, Stop 65, SetGameParameter 19, SetState 10, SetAkProp 10, Break 10, Seek 2, Pause 1,
Resume 1.

`tools/wwise_bank.py` parses every object and checks that each body is consumed to the byte; its output matches
wwiser (github.com/bnnm/wwiser) on all 1,271 objects. Points specific to version 88:

- NodeBaseParams order: fx (override flag, count, bypass bits, per slot index, fx id, share set flag, rendered flag),
  override bus id, parent id, two priority flags, prop bundle (u8 count, u8 ids, 4-byte values), ranged prop bundle (min
  and max pairs), positioning, aux (four flags, four aux bus ids when the last flag is set), advanced settings (12
  bytes), state chunk (u32 group count; per group u32 id, u8 sync type, u16 state count, per state u32 state id and u32
  instance id), RTPCs (u16 count; per RTPC u32 game parameter, u32 parameter id, u32 curve id, u8 scaling, u16 point
  count, points of f32 x, f32 y, u32 interpolation), and, because the feedback flag is set, a trailing u32 feedback bus
  id. Buses end with the same trailing u32.
- Prop values are a float or int union: Loop, TransitionTime and DelayTime are ints (milliseconds for the times), the
  rest floats (dB for volumes, cents for pitch).
- Positioning: u8 flags (bit 0 overrides parent); if set, u8 2D available, u8 3D available, u8 panner flag when 2D; when
  3D, u32 type, u32 attenuation id, u8 spatialized; type 1 is followed by u8 dynamic (game-defined emitter position),
  type 0 by a path automation block (user-defined paths).
- Curve scaling `dB` (2): stored y is in -1..0 and means 20 * log10(y + 1) dB, with -1 as silence; a positive y means
  -20 * log10(1 - y) dB. Curves interpolate the stored value, then scale it. Shapes Log1 and Exp1 are t(3 - t) / 2 and
  t(t + 1) / 2. Example: the footstep attenuation reaches -12 dB at 30 m and silence at 50 m.
- Attenuation: u8 cone flag (cone: inside angle, outside angle, outside volume, LPF), five s8 curve slots in the order
  VolumeDry, VolumeAuxGameDef, VolumeAuxUserDef, LowPassFilter, Spread (guess at the slot names), u8 curve count,
  curves, RTPCs.
- Music: tracks carry sources plus a playlist of play-at, begin trim, end trim and source duration (f64 milliseconds);
  segments carry duration and entry and exit markers; the random/sequence container has a nested playlist tree.
  `Play_bgm_corridor_f110` plays a 42.5 s segment once, then loops a 250 s segment; both trim the same five 7-channel
  stems.
- Plugins: sources Vorbis, PCM, ADPCM, Wwise Silence, Wwise Tone Generator, Wwise Motion Generator (34, in
  FeedbackNodes: pad vibration), one external source (sys_resident, audio supplied by the game at runtime: the demo
  streams). Effects: Peak Limiter on the Master Audio Bus, RoomVerb on five aux buses, Matrix Reverb and Stereo Delay on
  one aux bus each; their parameter blocks are decoded.

Wwise Motion Generator parameters (FxCustom object whose id is the source id, plugin 0x01950005): f32 period (s), f32
period multiplier, f32 duration (s), f32 attack, decay, sustain time and release (s), f32 sustain level (dB), u16
duration type (0 period * multiplier, 1 duration, 2 ADSR), u16 curve count, then per curve u16 point count and points
of f32 x (s, 0 to period), f32 y (0 to 1), u32 interpolation. Curve 0 drives the large motor, curve 1 the small motor.

Init bank: volume threshold -80 dB, 256 voices, state groups `chara`, `condition` (arguments of the dialogue event),
`cutscene_state` (env_off, bgm_off, bgm_env_off, bgm_off_3s) and `state_common` (splash_screen, in_game, game_over,
transitions of 0 to 2 s), eleven game parameters (`rainfall` default 0.5, `rear_rtpc`, `volumeRtpc`, eight unnamed).
Bus tree: Master Audio Bus with `environment` (aux buses Lobby, Bathroom, startroom, passage, LFE and two unnamed), an
unnamed SFX parent (telephone, Player, ENEMY, Radio, gimmick, BLOOD and two unnamed), an unnamed parent of ME and
Music, and a few more unnamed buses; plus Master Secondary Bus and Master Motion Bus at the top.

## Media

313 bank media plus 5 demo streams, all 48 kHz. Container: RIFF/WAVE with Wwise codec tags.

| codec tag | codec | count | notes |
| --- | --- | --- | --- |
| 0xFFFF | Wwise Vorbis | 188 | 66-byte `fmt` with Wwise's extension, no Ogg framing; vgmstream decodes it |
| 0xFFFE | PCM, 16-bit | 97 | 24-byte `fmt` (channel mask in the extension), `JUNK`, `data` |
| 0x0002 | Wwise IMA ADPCM | 33 | 4-bit, 36-byte blocks per channel |

Channels: mono 282 (4 of them LFE-only), stereo 4, 6.0 18, 7.0 5, 7.1 9. In all 32 multichannel files the back and side
pairs are bit-identical; this is in the source data. Seven media carry loop points. 31 media carry cue points with
`LIST/adtl/labl` labels (the marker labels used by the `.sab`, and names such as `sfx_rid_voice_e_01`).

Wwise Vorbis (0xFFFF): the `vorb` block follows the 24-byte WAVEFORMATEXTENSIBLE part. `vorb` +0x00 sample count, +0x10
setup packet offset and +0x14 first audio packet offset (relative to `data`), +0x28 and +0x29 the small and large block
size exponents (8 and 11 in all P.T. files). Packets are u16 size plus payload (no granule). The setup packet holds
10-bit codebook ids into the aoTuV 6.03 codebook library and a trimmed setup; it is rebuilt into a standard Vorbis setup
header as ww2ogg and vgmstream do. Audio packets drop the packet type bit and, for long blocks, the previous and next
window flags. The port's decoder does the same rebuild with libvorbis and the codebook library from ww2ogg; its output
matches vgmstream within 1 LSB.

Wwise IMA ADPCM (0x0002): each channel has its own 36-byte block per frame (u16 sample, u8 step index, u8 reserved, 32
data bytes). A block gives 64 samples: the header sample, then 63 nibbles, low nibble first; the last nibble is not
used. Delta is `((2 * (n & 7) + 1) * step) >> 3`, negated when `n & 8`, clamped to s16, step index clamped to 0..88.

## Demo streams (.fsm)

Sequence of chunks: char[4] tag, u32 size including the 8-byte header. `DEMO`, `SND ` and `END ` chunks continue with an
f64 time in seconds. `SYS ` (16 bytes) opens the language copies.

- `DEMO`: cutscene data (`fsm.md`). Some tracks hold Wwise event ids as u32: gc_p00_010 and gc_p00_160 the 2D footsteps
  and doors; gc_p00_020, gc_p00_030 and gc_p07_030 pad vibration events; gc_p02_100, gc_p06_010_final and gc_p07_030
  `cutscene_state` events; gc_p07_030 also sets and resets the Radio bus volume.
- `SND `: an interleaved Wwise `.wem`. The first `SND ` has an extra 16 bytes after the time: u32 total stream size,
  u32 2, 8 zero bytes. Concatenating the payloads gives the complete RIFF file.
- `END `: end time.

Streams with audio (all Wwise Vorbis 7.1): gc_p00_020 (14.4 s), gc_p00_030 (8.1 s), gc_p02_100 (38.0 s, a repeated
number chant), gc_p07_030 (21.9 s), and `#Eng/gc_p06_010_final` (148.5 s, the ending). The base gc_p06_010_final carries
no audio; the English copy is the base plus a `SYS ` chunk and 30 `SND ` chunks. The game plays a stream through the
external source Sound, on a bus muted during the splash screen and the game over state.

## .sdf and .evf

Both are binary fox2 DataSet files (`fox2.md`).

- `sh_common_sound.sdf`: one `SoundDataFileInfo` with `loadBanks` = sys_resident, bg_common, sfx_common, bgm_common;
  `prepareBanks` and `prepareEvents` empty.
- `gc_p04_270_snd.evf`: demo event timeline, `EventDataUnit` entities (`eventName` ExecCommand, `paramString` commands)
  with `TimeSection` start and end frames. It creates the effect `fxsd_sh_ene_fs01_f040.vfx` on shot `konShotCamera1`,
  whose sound node posts `Play_ene_fs_f040_01`. `gc_p06_010_light_bc.evf` (ending) holds lighting only.
- `fxsd_*.vfx` (40 files in resident, hallway and hallway_maze_C fpkd): Fox effect files with a sound node; the event
  name is stored as u16 length plus text.

## Event names

Wwise ids of events, buses, states, switches, game parameters and banks are FNV-1 32 of the lowercase name; other
object ids are not name hashes. `audio_dump.py` hashes candidate strings from the Lua scripts, all package contents,
the fox2 name list, the executable's strings, wem marker labels and the `.sab`, then derives variants (Play/Stop swaps,
number, token and suffix variants). `wwise_names.py` searches the rest with paired constraints (a Play and a Stop on
the same target, a SetState event and its state) that must satisfy two independent 32-bit hashes, meeting in the middle
over an English word list plus game tokens.

Result: 186 of 247 events named (87 by a verbatim game string, 99 by a derived variant or paired search); 230 of 320
hashed ids overall. Unnamed: 60 events and the dialogue event, 15 buses, 8 game parameters, 3 effect share sets, 2
switches (Material values besides wood, tile, conc, BLOOD), 1 state.

Naming pattern: `Play_`/`Stop_` + category (`sfx`, `voice`, `bgm`, `bg`, `radio`, `sys`, `plr`, `ene`, `man`, `tel`,
`hint`) + subject + optional `_NN`, `_lp` (loop), `_2d` (2D variant), `_s`/`_m`/`_l` (size), `fNNN` (hallway floor id);
`Set_state_<state>`, `Set_rtpc_<parameter>_<value>`.

Where events come from: Lua (`GameSystem.SoundPostEvent`, `CallBGM`, `StopBGM`, `IsPlayingBGM`;
`SoundDaemon.RegisterAnimEvent` for the player's motion events: FOOT_GROUND_L/R `Play_plr_footstep_wk_l/r`,
FOOT_LEAVE_L/R `Play_plr_footstep_lv_l/r`, FOOT_CREAK `Play_plr_footstep_creak`), fox2 trap parameters (`soundId`),
SoundSource `eventName`, SoundAreaParameter `ambientEvent`, `fxsd_*.vfx`, immediates in the executable (62 sites),
`ShGimmick_layers.mtar` motion tracks (12 ids) and demo stream tracks.

Game parameters with a known effect: `volumeRtpc` (1689461822; the ambience beds scale their volume with it, -96 dB at
0 to 0 dB at 1; the area system ramps it per ambience object), `rainfall` (511469240, default 0.5: rain layer
crossfades), `player_direction` (3641826420: 0 to 359, crossfades the five corridor music stems; the game sets it from
the camera yaw every frame), 2556335218 (volume of water drops and other room details; `Play_bg_bathroom01` sets 0,
passage and start room set 1). Switch group `Material` (wood default, tile, conc, BLOOD, two unnamed) selects the
footstep containers. The dialogue event 3297215429 takes `chara` then `condition` (`bab` with `TRIA1000_121010_0_baby`,
`pab` with `TRIA1000_111010_0_mib`).

## Running the tools

`python tools/audio_dump.py --root <dump root> --vgmstream <vgmstream-cli>` expects the extracted archive, packages,
decrypted Lua and the fox2 name list under the root (see `--help`) and writes everything to `<root>/audio`: `banks/`
(unpacked bnk and sab), `wem/`, `<bank>/*.wav`, `demo_stream/*.wav`, `hirc/<bank>.json` (full parse), `media.json`
(codec, channels, loop, markers, duration, levels), `names.json` (resolved names with sources and confidence),
`events.json` (per event: actions, target tree, media and WAV paths, effective volume, positioning with attenuation
curves, bus path, aux sends, marker and subtitle links), `events.txt`, `references.json`, `sab.json`. It needs numpy,
soundfile, capstone and vgmstream-cli; `--skip-decode` reuses `media.json`. `wwise_names.py --names ... --events ...
--out ...` extends the name list (the English list needs the wordfreq package).

For a mod that replaces one sound, `python tools/wwise_bank.py extract <bank> --out <dir>` is enough: it writes every
embedded media file as `<id>.wem` (docs/modding.md).

## Open

- Names of 60 events, 15 buses, 8 game parameters, 2 Material switch values and the dialogue event.
- `.sab` parameter bytes and the `.subp` header fields marked unknown.
