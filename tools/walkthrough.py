import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ROUTES = REPO / "tests" / "walkthrough"
DEFAULT_EXE = REPO / "build" / "release" / "pt.exe"
DEFAULT_GAME = REPO / "game" / "CUSA01127"

HALLWAY = ["walk", "exit"]
LATE = (["start", "f050a", "exit", "f050b", "exit"] + HALLWAY + HALLWAY + ["f090", "exit"] + HALLWAY +
        ["mazeA", "mazeB", "mazeC", "f120", "start", "f160", "exit"])

FULL = (["start", "walk", "exit", "f005", "exit"] + HALLWAY + ["f030", "exit"] + HALLWAY + ["f060", "exit"] + LATE[1:])

# a save of the port finished three times (Game+ tier 3): its header (version 3), brightness 7, floor f000, the finished tag 'NGP1'
# at 0x70 and the finish count 3 at 0x74 (save_data.cpp), slot sequence 1; no museum key anywhere
ARCHIVE_BACKFILL_SAVE = ((0x6E69777470).to_bytes(8, "little") + (3).to_bytes(4, "little") + bytes(0x2C) + bytes([0x07]) + bytes(7) + b"f000"
                         + bytes(0x2C) + (0x3150474E).to_bytes(4, "little") + (3).to_bytes(4, "little") + (1).to_bytes(4, "little") + bytes(4))
# a 1.0.3 save finished once (Game+ tier 1): ARCHIVE_BACKFILL_SAVE with the finish count 1
GAMEPLUS_103_SAVE = ARCHIVE_BACKFILL_SAVE[:0x74] + (1).to_bytes(4, "little") + ARCHIVE_BACKFILL_SAVE[0x78:]
# a save as Endf120 writes it (gameplay.md 3): brightness 5, floor f120, XMark, Hello and Peephole cleared, slot sequence 1; its start
# room door leads into f160, the last loop (the state of the shadPS4 save sv_x_f120)
F120_SAVE = ((0x6E69777470).to_bytes(8, "little") + (3).to_bytes(4, "little") + bytes(0x2C) + bytes([0x05]) + bytes(7) + b"f120"
             + bytes(0x24) + bytes([1, 1, 1, 0, 0]) + bytes(0x0B) + (1).to_bytes(4, "little") + bytes(4))
# what the halls page counts after the retroactive unlock: the guaranteed keys' exhibits, the content that opens with the finish
ARCHIVE_BACKFILL_COUNT = r"museum: 73 / 107 found \(68%\), bonus 0"

SCENARIOS = {
    "removed_options": {
        "start_floor": "f010",
        "frames": 5000,
        "args": ["--virtual-pads", "--audio-offline"],
        "work_files": {"pt.ini": b"[display]\npause_on_focus_loss=1\nmute_in_background=1\n[graphics]\nenhanced_textures=1\n"},
        "route": ["removed_options"],
        "expect": [
            r"expect ok .*: PC setting pc_focus_pause absent",
            r"expect ok .*: PC setting pc_focus_mute absent",
            r"expect ok .*: PC setting pc_enhanced_textures absent",
            r"input script: \d+ expectations, 0 failed",
        ],
        "forbid": [(r"startup:", r"zzz never", r"enhanced textures|texture preparation|pause menu opened|frozen in the background")],
    },
    "full": {
        "start_floor": None,
        "frames": 90000,
        "route": FULL,
        "expect": [
            r"save: no save data, first boot",
            r"controller: ChangeGameStep\(StartGame\)",
            r"floor: NextFloor f000 -> f010",
            r"floor: NextFloor f010 -> f005",
            r"floor: NextFloor f005 -> f020",
            r"floor: NextFloor f020 -> f030",
            r"floor: NextFloor f030 -> f040",
            r"floor: NextFloor f040 -> f060",
            r"trap: ShDemoExec gc_p01_022",
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"nazo: XMark cleared",
            r"floor: NextFloor f050 -> f070",
            r"floor: NextFloor f070 -> f080",
            r"floor: NextFloor f080 -> f090",
            r"nazo: Hello cleared",
            r"floor: NextFloor f090 -> f100",
            r"floor: NextFloor f100 -> f110",
            r"nazo: peephole theater started, view from",
            r"nazo: Peephole cleared",
            r"floor: NextFloor f110 -> f120",
            r"controller: ChangeGameStep\(Endf120\)",
            r"floor: NextFloor f120 -> f160",
            r"nazo: TrueEnd cleared",
            r"floor: NextFloor f160 -> ending",
            r"controller: ChangeGameStep\(GotoEnding\)",
            r"stage: .*ending.fpk",
        ],
    },
    "early": {
        "start_floor": "f010",
        "frames": 20000,
        "route": ["start", "f005", "exit"] + HALLWAY + ["f030", "exit"] + HALLWAY,
        "expect": [
            r"floor: NextFloor f010 -> f005",
            r"trap: ShDemoExec gc_p04_310",
            r"trap: ShDemoExec gc_p01_070",
            r"shsb_hous001_door002_lobby_open visible = true",
            r"floor: NextFloor f005 -> f020",
            r"trap: ShDemoExec gc_p04_340",
            r"trap: ShDemoExec gc_p01_081",
            r"floor: NextFloor f020 -> f030",
            r"trap: ShDemoExec gc_p01_011",
            r"trap: ShDemoExec gc_p01_020",
            r"trap: ShDemoExec gc_p01_021",
            r"demo: gc_p01_010 playing",
            r"floor: NextFloor f030 -> f040",
            r"trap: ShDemoExec gc_p04_350",
            r"floor: NextFloor f040 -> f060",
        ],
    },
    "f060": {
        "start_floor": "f040",
        "frames": 12000,
        "route": ["start", "f060", "exit"],
        "expect": [
            r"floor: NextFloor f040 -> f060",
            r"trap: ShDemoExec gc_p01_110",
            r"trap: ShDemoExec gc_p00_030",
            r"demo: gc_p01_050 playing",
            r"trap: ShDemoExec gc_p01_090",
            r"trap: ShDemoExec gc_p01_020",
            r"trap: ShDemoExec gc_p01_022",
            r"floor: NextFloor f060 -> f050",
        ],
    },
    "lisa": {
        "start_floor": "f080",
        "frames": 14000,
        "route": ["start", "lisa"],
        "expect": [
            r"floor: NextFloor f080 -> f090",
            r"ocho: LogicControl 1",
            r"ocho: appears at spawn",
            r"ocho: LogicControl 4",
            r"ocho: dash reached the player in \d+ game frames",
            r"ocho: LogicControl 3",
            r"ocho: look back armed",
            r"demo: gc_p07_030 playing",
            r"controller: step 15 -> 16",
        ],
    },
    # Lisa's steps and the pause menu (OchoLogic::UpdateChase): the menu held at several points of her walk, the steps gone once he stands
    "lisa_pause": {
        "pad": True,
        "start_floor": "f080",
        "frames": 14000,
        "args": ["--audio-offline"],
        "route": ["start", "lisa_pause"],
        "expect": [
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "photo": {
        "start_floor": "f005",
        "frames": 4000,
        "route": ["start", "photo"],
        "expect": [
            r"floor: NextFloor f005 -> f020",
            r"nazo: Photo active",
            r"nazo: photo piece PhotoLisa collected",
            r"ui: subliminal image 0 flag true",
            r"nazo: photo piece PhotoTree collected",
            r"ui: subliminal image 1 flag true",
        ],
    },
    "photooption": {
        "pad": True,
        "start_floor": "f005",
        "frames": 4500,
        "route": ["start", "photo", "photooption"],
        "expect": [
            r"floor: NextFloor f005 -> f020",
            r"nazo: photo piece PhotoTree collected",
            r"ui: option menu opened \(pause\)",
            r"ui: option menu sent PhotoOption, (1\.9[89]|2\.0[0-2]) s after the zoom press",
            r"ui: subliminal image 1",
            r"ui: option menu closed",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # all five pieces and the option frame: word3 is 0x1FC (Activate's bit 4 stays), the Archive's complete photo is noted anyway
    "photoall": {
        "pad": True,
        "start_floor": "f005",
        "frames": 6500,
        "route": ["start", "photo", "photoall", "photooption"],
        "expect": [
            r"nazo: photo piece PhotoGap collected",
            r"nazo: photo piece PhotoBath collected",
            r"nazo: photo piece PhotoStair collected",
            r"ui: option menu sent PhotoOption",
            r"archive: photo:complete reached in play",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the option screen's photo part with the mouse: a click whose down and up come in one frame is the PS4's quick tap (the press
    # starts zoom_in, PhotoOption follows when it has ended); a 3 s hold sends it at the release
    "photooption_mouse": {
        "pad": True,
        "start_floor": "f020",
        "frames": 1600,
        "route": ["start", "photooption_mouse"],
        "expect": [
            r"nazo: Photo active",
            r"ui: option menu opened \(pause\)",
            r"mouse button right down and up in one frame",
            r"ui: option menu zoom pressed, zoom_in started",
            r"ui: option menu zoom released, PhotoOption waits for the zoom_in",
            r"ui: option menu sent PhotoOption, (1\.9[89]|2\.0[0-2]) s after the zoom press",
            r"ui: option menu closed",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "photooption_mousehold": {
        "pad": True,
        "start_floor": "f020",
        "frames": 1600,
        "route": ["start", "photooption_mousehold"],
        "expect": [
            r"ui: option menu opened \(pause\)",
            r"mouse button right down",
            r"ui: option menu zoom pressed, zoom_in started",
            r"mouse button right up",
            r"ui: option menu zoom released$",
            r"ui: option menu sent PhotoOption",
            r"ui: option menu closed",
        ],
        "forbid": [(r"ui: option menu zoom pressed", r"ui: option menu zoom released", r"sent PhotoOption")],
    },
    # the option screen's photo part against the shadPS4 captures photops4_* (docs/gameplay.md 6.4): every press shorter than the
    # zoom_in (a one-frame tap, 0.3 s, 1 s) sends PhotoOption when the zoom_in ends, 2 s after the press; a 3 s hold at its release;
    # a close during zoom_in keeps the release, and the reopened menu sends it when its setin ends
    "photooption_hold1s": {
        "pad": True,
        "start_floor": "f020",
        "frames": 1600,
        "route": ["start", "photooption_hold1s"],
        "expect": [
            r"ui: option menu opened \(pause\)",
            r"ui: option menu zoom pressed, zoom_in started",
            r"ui: option menu zoom released, PhotoOption waits for the zoom_in",
            r"ui: option menu sent PhotoOption, (1\.9[89]|2\.0[0-2]) s after the zoom press",
            r"ui: option menu closed",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "photooption_hold3s": {
        "pad": True,
        "start_floor": "f020",
        "frames": 1600,
        "route": ["start", "photooption_hold3s"],
        "expect": [
            r"ui: option menu opened \(pause\)",
            r"ui: option menu zoom pressed, zoom_in started",
            r"ui: option menu zoom released$",
            r"ui: option menu sent PhotoOption, (2\.9[89]|3\.0[0-2]) s after the zoom press",
            r"ui: option menu closed",
            r"input script: \d+ expectations, 0 failed",
        ],
        "forbid": [(r"ui: option menu zoom pressed", r"ui: option menu zoom released", r"sent PhotoOption")],
    },
    "photooption_close": {
        "pad": True,
        "start_floor": "f020",
        "frames": 2000,
        "route": ["start", "photooption_close"],
        "expect": [
            r"ui: option menu opened \(pause\)",
            r"ui: option menu zoom pressed, zoom_in started",
            r"ui: option menu zoom released, PhotoOption waits for the zoom_in",
            r"ui: option menu closed",
            r"ui: option menu opened \(pause\)",
            r"ui: option menu sent PhotoOption",
            r"ui: subliminal image 1",
            r"ui: option menu closed",
        ],
        "forbid": [(r"ui: option menu zoom pressed", r"ui: option menu opened \(pause\)", r"sent PhotoOption"),
                   (r"sent PhotoOption", r"game: stopped", r"ui: option menu zoom pressed")],
    },
    # the Lisa piece with the right mouse button: only the hold of more than 2 s collects it (0x1264B90), every shorter one, the
    # one-frame click included, is armed and released without effect
    "photo_mouse": {
        "pad": True,
        "start_floor": "f020",
        "frames": 2600,
        "route": ["start", "photo_mouse"],
        "expect": [
            r"nazo: Photo active",
            r"mouse button right down and up in one frame",
            r"nazo: action armed \(state 3, flag 0x8\)",
            r"nazo: action released after 0\.\d+ s",
            r"nazo: action released after 0\.\d+ s",
            r"nazo: action released after (0\.9\d|1\.0\d) s",
            r"nazo: action released after (1\.9\d|2\.00) s",
            r"nazo: action armed \(state 3, flag 0x8\)",
            r"nazo: action held 2\.\d+ s, committed",
            r"nazo: photo piece PhotoLisa collected",
        ],
        "forbid": [(r"nazo: Photo active", r"nazo: action held 2\.\d+ s, committed", r"photo piece PhotoLisa collected")],
    },
    "restartsave": {
        "start_floor": "f110",
        "frames": 9000,
        "route": ["restartsave"],
        "expect": [
            r"controller: ChangeGameStep\(Endf120\)",
            r"save: skipped \(floor f120\)",
            r"controller: ChangeGameStep\(GotoEnding\)",
            r"controller: ChangeGameStep\(FinishEndingRestartGame\)",
            r"save: skipped \(floor f120\)",
        ],
    },
    "mirrorswap": {
        "start_floor": "f060",
        "frames": 20000,
        "args": ["--audio-offline"],
        "route": LATE[:LATE.index("mazeA") + 1] + ["mirrorswap"],
        "expect": [
            r"floor: NextFloor f100 -> f110",
            r"input script frame \d+: teleport to \(-17.100",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # issue 80: f120 into f160, quit from Options, start again. The original saves no progress on f160: the start room door and
    # the f160 exit door have needSave false, and ShDemoExec (0x91CE40) reads needSave only with nextFloorDemo, so the clock
    # demo gc_p05_010's needSave true is never used. shadPS4 issue80_f160 (booted from sv_x_f120): one GameSave write, the
    # options-only save of the pause menu's close, with its icon, none from the door or the clock demo; issue80_resume boots
    # the start room on f120 and its door leads into f160 again. The relaunch must do the same.
    "save160": {
        "pad": True,
        "save": True,
        "save_files": {"PT_Save_Data0": F120_SAVE},
        "start_floor": None,
        "frames": 4000,
        "route": ["padattach", "start", "quit_options"],
        "relaunch": {"frames": 2500, "route": ["start"]},
        "expect": [
            r"save: loaded floor f120",
            r"floor: NextFloor f120 -> f160 \(loop 1\)",
            r"trap: ShDemoExec gc_p05_010 \(next floor false, save true\)",
            r"save: options only \(floor f120 kept\)",
            r"save: slot \d written \(floor f120, cleared 11100",
            r"ui: quit selected from Options",
            r"walkthrough: relaunch",
            r"save: loaded floor f120",
            r"floor: NextFloor f120 -> f160 \(loop 1\)",
        ],
        "forbid": [
            # nothing is written and no save icon shows on f160 before the menu's options save
            (r"floor: NextFloor f120 -> f160", r"save: options only", r"save: slot \d written"),
            (r"floor: NextFloor f120 -> f160", r"save: options only", r"ui: save icon shows"),
        ],
    },
    # the save request's dialogs (SaveRequest_Update 0x9476D0): a broken slot file (shorter than a save) shows sys_load_failed_3 and
    # writes a new save once it is closed; the boot waits for it (step 1)
    "savebroken": {
        "pad": True,
        "save": True,
        "save_files": {"PT_Save_Data0": b"\x70\x74\x77\x69\x6e\x00\x00\x00"},
        "start_floor": None,
        "frames": 3000,
        "route": ["padattach", "savedialog"],
        "expect": [
            r"save: .*PT_Save_Data0 is broken",
            r"save: broken save data, dialog sys_load_failed_3 shown",
            r"save: dialog sys_load_failed_3 closed",
            r"save: slot \d written \(floor f000",
            r"controller: step 1 -> 2",
        ],
    },
    # a newer save version (0x13) or a wrong magic (0x14): sys_load_failed_2, nothing written, not a first boot
    "savenewer": {
        "pad": True,
        "save": True,
        "save_files": {"PT_Save_Data0": b"\x70\x74\x77\x69\x6e\x00\x00\x00\x04" + b"\x00" * 0x77},
        "start_floor": None,
        "frames": 3000,
        "route": ["padattach", "savedialog"],
        "expect": [
            r"save: .*PT_Save_Data0 has a bad header \(magic 0x6e69777470, version 4\)",
            r"save: unreadable save data, dialog sys_load_failed_2 shown",
            r"save: dialog sys_load_failed_2 closed",
            r"controller: step 1 -> 2",
        ],
    },
    # out of space at the first save: the no-space dialog, one retry; a second failure gives sys_save_failed_4
    "savenospace": {
        "pad": True,
        "save": True,
        "env": {"PT_SAVE_FAIL": "nospace:2"},
        "start_floor": None,
        "frames": 3000,
        "route": ["padattach", "savedialog", "savedialog"],
        "expect": [
            r"save: no save data, first boot",
            r"save: write failed \(no space, forced by PT_SAVE_FAIL\)",
            r"save: dialog pc_save_no_space shown",
            r"save: dialog pc_save_no_space closed",
            r"save: write failed \(no space, forced by PT_SAVE_FAIL\)",
            r"save: dialog sys_save_failed_4 shown",
            r"save: dialog sys_save_failed_4 closed",
            r"controller: step 1 -> 2",
        ],
    },
    # the option defaults from the system language (Options_StaticInit 0x91D7B0): a French system's first boot has French subtitles on
    "langdefault": {
        "save": True,
        "env": {"PT_SYSTEM_LANGUAGE": "fr-FR"},
        "start_floor": None,
        "frames": 1500,
        "route": ["langcheck_fr"],
        "expect": [
            r"options: defaults for system language fr-FR: subtitles true language 1",
            r"save: no save data, first boot",
            r"save: slot \d written",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # a save's options win over the system language: English, subtitles off, brightness 7 chosen on a French system
    "langsaved": {
        "save": True,
        "env": {"PT_SYSTEM_LANGUAGE": "fr-FR"},
        "save_files": {"PT_Save_Data0": (0x6E69777470).to_bytes(8, "little") + (3).to_bytes(4, "little") + bytes(0x2C)
                       + bytes([0x07]) + bytes(7) + b"f010" + bytes(0x34)  + (1).to_bytes(4, "little") + bytes(4)},
        "start_floor": None,
        "frames": 1500,
        "route": ["langcheck_saved"],
        "expect": [
            r"options: defaults for system language fr-FR: subtitles true language 1",
            r"save: loaded floor f010",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the HELLO letter H is a target that counts as in view only in front of the camera (the original's ndc.z > -1 is 0 < depth < far):
    # walking and turning at the end of the first leg with H behind the player gives no step, turning to H and away gives one
    "hello": {
        "start_floor": "f090",
        "frames": 4000,
        "route": ["start", "hello_walk", "hello_look"],
        "expect": [
            r"floor: NextFloor f090 -> f090 \(loop 2\)",
            r"nazo: Hello step -> 0x8",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "loops": {
        "start_floor": "f080",
        "frames": 150000,
        "route": ["start"] + ["walk", "exit"] * 31,
        "expect": [
            r"floor: NextFloor f080 -> f090",
            r"floor: NextFloor f090 -> f090 \(loop 2\)",
            r"floor: NextFloor f090 -> f090 \(loop 30\)",
            r"trapForceGameOver.lua:17: attempt to index global 'Command'",
            r"floor: NextFloor f090 -> f090 \(loop 31\)",
        ],
    },
    "gouge_respawn_browser": {
        "optional": True,
        "start_floor": "f060",
        "frames": 16000,
        "route": ["start", "browse_gouge", "start", "gouge_death_before", "gameover", "gouge_after_respawn"],
        "args": ["--audio-offline"],
        "expect": [
            r"loop browser: entered f050 \(pass 2\)",
            r"nazo: .* words 0x4/0 ",
            r"message: gc_p07_030 GotoGameOver",
            r"controller: ChangeGameStep\(StartGame\)",
            r"floor: NextFloor f060 -> f050",
            r"nazo: .* words 0x4/0 ",
        ],
    },
    "gouge_respawn": {
        "optional": True,
        "start_floor": "f060",
        "frames": 16000,
        "route": ["start", "f050a", "exit", "gouge_death_before", "gameover", "gouge_after_respawn"],
        "args": ["--audio-offline"],
        "expect": [
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"nazo: .* words 0x4/0 ",
            r"message: gc_p07_030 GotoGameOver",
            r"controller: ChangeGameStep\(StartGame\)",
            r"floor: NextFloor f060 -> f050",
            r"nazo: .* words 0x4/0 ",
        ],
    },
    "gouge_browser_previous": {
        "optional": True,
        "start_floor": "f060",
        "frames": 7000,
        "route": ["start", "browse_before_gouge", "start", "f050a", "exit", "gouge_death_before"],
        "args": ["--audio-offline"],
        "expect": [
            r"loop browser: entered f050 \(pass 1\)",
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"nazo: .* words 0x4/0 ",
        ],
    },
    "gouge_browser_backtrack": {
        "optional": True,
        "start_floor": "f060",
        "frames": 12000,
        "route": ["start", "xmark_browser_later", "f050a", "exit", "gouge_death_before"],
        "args": ["--audio-offline"],
        "expect": [
            r"floor: NextFloor f120 -> f160",
            r"nazo: .* words 0x8/1 ",
            r"loop browser: entered f050 \(pass 1\)",
            r"nazo: .* words 0x4/0 ",
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"nazo: .* words 0x4/0 ",
        ],
    },
    "xmark_browser_boundaries": {
        "optional": True,
        "start_floor": None,
        "frames": 60000,
        "save": True,
        "route": ["start", "browse_states"],
        "args": ["--audio-offline"],
        "expect": [
            r"floor: f050 loop 1 ",
            r"nazo: .* words 0x4/0 ",
            r"floor: f050 loop 2 ",
            r"nazo: .* words 0x4/0 ",
            *[pattern for floor in ("f070", "f080", "f090", "f100", "f110", "f120", "f160")
              for pattern in (rf"floor: {floor} loop 1 ", r"nazo: .* words 0x8/1 ")],
        ],
    },
    "gameover": {
        "start_floor": "f060",
        "frames": 12000,
        "route": ["start", "f050a", "exit", "gameover", "bag"],
        "args": ["--audio-offline"],
        "expect": [
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"ocho: LogicControl 3",
            r"ocho: steps stopped, their sound followed her [1-9]",
            r"ocho: look back armed",
            r"demo: gc_p07_030 playing",
            r"ocho: kill",
            r"message: gc_p07_030 GotoGameOver",
            r"controller: step 15 -> 16",
            r"controller: ChangeGameStep\(StartGame\)",
            r"gimmick: Bag talks \(zoom after Lisa\'s kill\)",
            r"gimmick anim: Bag dialogue 0xc48783c5",
            r"gimmick: Bag in view with zoom, stays silent",
        ],
    },
    # a game over while the f050 radio talks: its subtitle ends with the radio (Set_state_game_over's Stop_ALL), not in the wake-up
    "radio_gameover": {
        "start_floor": "f060",
        "frames": 4000,
        "route": ["start", "radio_gameover"],
        "args": ["--audio-offline"],
        "expect": [
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"sound: marker TRIA1000_1n1010_0_caster -> subtitle TRIA1000_1n1010",
            r"ui: subtitle area control shows tria1000_1n1010",
            r"expect ok .*: speech 1",
            r"controller: step 15 -> 16",
            r"ui: subtitle 0xbf6d9a27 tria1000_1n1010 ended with its sound",
            r"expect ok .*: speech 0",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "late": {
        "start_floor": "f060",
        "frames": 40000,
        "route": LATE,
        "expect": [
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"nazo: XMark cleared",
            r"message: controller Clearf050",
            r"floor: NextFloor f050 -> f070",
            r"floor: NextFloor f070 -> f080",
            r"floor: NextFloor f080 -> f090",
            r"nazo: Hello cleared",
            r"floor: NextFloor f090 -> f100",
            r"floor: NextFloor f100 -> f110",
            r"nazo: peephole theater started, view from",
            r"nazo: Peephole cleared",
            r"message: controller f110_GoNextFloor",
            r"stage: .*pt14_hallway_maze_C.fpk loaded as nextC",
            r"floor: NextFloor f110 -> f120",
            r"controller: ChangeGameStep\(Endf120\)",
            r"floor: NextFloor f120 -> f160",
            r"nazo: TrueEnd ten steps",
            r"nazo: TrueEnd heard Jack",
            r"nazo: TrueEnd phone ringing",
            r"nazo: TrueEnd cleared",
            r"floor: NextFloor f160 -> ending",
            r"controller: ChangeGameStep\(GotoEnding\)",
            r"stage: .*ending.fpk",
        ],
    },
    "peephole": {
        "start_floor": "f060",
        "frames": 20000,
        "args": ["--audio-offline"],
        "route": LATE[:LATE.index("mazeB") + 1],
        "expect": [
            r"floor: NextFloor f100 -> f110",
            r"nazo: action armed \(state 2",
            r"nazo: peephole theater started, view from",
            r"nazo: peephole aborted \(zoom released during the theater\)",
            r"nazo: peephole overlay removed next game frame \([12] ticks after abort/clear\)",
            r"nazo: action armed \(state 2",
            r"nazo: peephole theater started, view from",
            r"nazo: Peephole cleared",
            r"message: controller f110_GoNextFloor",
        ],
    },
    # A loop browser pick and a progress reset start a new session, which must hold what a fresh boot holds: the gimmick records
    # with their meshes, Lisa without her kill, no speech of the old session (sstate compare, expect gimmick/ocho/body/speech)
    # the radio of the first loop after a progress reset made on the last loop (reported: heard from far away), against a fresh boot
    "radio_fresh": {"start_floor": None, "frames": 30000, "save": True, "route": ["start", "walk", "f005"], "expect": []},
    "radio_reset": {"start_floor": "f160", "frames": 40000, "save": True, "route": ["start", "session_reset", "start", "walk", "f005"], "expect": []},
    # audit: the session state on arriving in every loop by play, and by the loop browser (compare the logged states per floor)
    "full_states": {"start_floor": None, "frames": 400000, "save": True,
                    "route": [p for part in FULL for p in ([part, "state_dump"] if part == "exit" else [part])], "expect": []},
    "browse_states": {"start_floor": None, "frames": 200000, "save": True, "route": ["start", "browse_states"], "expect": []},
    "reset": {
        "start_floor": None,
        "frames": 40000,
        "save": True,
        "route": ["session_save", "browse_gouge", "start", "lisa_follows", "gameover", "bag", "session_reset", "start", "walk", "meshes"],
        "expect": [
            r"loop browser: selected f050 \(pass 2\)",
            r"expect ok .*: handy light 1",
            r"expect ok .*: ocho state 3",
            r"ocho: kill",
            r"gimmick: Bag talks",
            r"save: confirmed reset restarts the current game session",
            r"controller: new session gimmicks reset after stage unload",
            r"expect ok .*: session state as saved",
            r"floor: NextFloor f000 -> f010",
            r"expect ok .*: gimmick CeilLamp drawn 1",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "browse": {
        "start_floor": None,
        "frames": 40000,
        "route": ["browse_first", "start", "walk", "meshes", "browse_gouge", "start", "lisa_follows", "gameover", "browse_again", "browse_lit", "browse_fridge",
                  "start", "walk", "fridge"],
        "expect": [
            r"loop browser: selected f010",
            r"expect ok .*: body shsb_hous001_ocho001_0000 hidden",
            r"floor: NextFloor f000 -> f010",
            r"expect ok .*: gimmick CeilLamp drawn 1",
            r"loop browser: selected f050 \(pass 2\)",
            r"nazo: XMark active",
            r"expect ok .*: ocho state 3",
            r"ocho: kill",
            r"expect ok .*: session state as saved",
            r"loop browser: selected f070",
            r"expect ok .*: handy light 1",
            r"loop browser: selected f080",
            r"expect ok .*: gimmick Freezer drawn 1",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # A loop browser pick of the first hallway (start room on f000) skips the opening: the start room door stands ajar as the
    # opening's gc_p01_100 leaves it, and gc_p00_160 runs as after a fresh boot: at LocatorStartDoorPos, a take of a few cm (0.077 m
    # after the boot's walk at the demo rate 1, 0.12 m at the walkthrough's 20) and the release where the boot's run releases him
    "browse_door": {
        "start_floor": None,
        "frames": 3300,
        "route": ["browse_door"],
        "expect": [
            r"loop browser: selected f010",
            r"controller: start room door set ajar as the skipped opening leaves it",
            r"expect ok .*: body shsb_hous001_door001_open_20 visible",
            r"expect ok .*: body shsb_hous001_door001_close hidden",
            r"demo: gc_p00_160 playing \(\d+ frames, [\d.]+ s\) at \(0\.398 0\.000 11\.837\) rot \(0\.000 0\.000 0\.000 1\.000\)",
            r"floor: NextFloor f000 -> f010",
            r"demo: gc_p00_160 takes the player .*jump to the root 0\.[01]\d+ m",
            r"demo: gc_p00_160 releases the player \(release functor\) at frame [\d.]+: \(-0\.013 0\.000 12\.615\) fox yaw -?0\.0",
            r"expect ok .*: body shsb_hous001_door001_open_20 hidden",
            r"input script: 4 expectations, 0 failed",
        ],
    },
    "photomode": {
        "start_floor": "f005",
        "frames": 12000,
        "route": ["start", "walk", "photo_toggle", "meshes", "exit", "walk", "meshes"],
        "expect": [
            r"extras: photo mode on",
            r"extras: free camera off",
            r"expect ok .*: session state as saved",
            r"floor: NextFloor f020 -> f030",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "archive_unused": {
        "optional": True,
        "start_floor": "f000",
        "frames": 4000,
        "save": True,
        "work_files": {"pt.ini": b"[progress]\ngame_finished = 1\narchive_backfill = 1\n"},
        "args": ["--release-locks"],
        "route": ["archive_unused"],
        "expect": [r"input script: 19 expectations, 0 failed"],
        "forbid": [(r"save:", r"input script:", r"archive: retroactive unlock")],
    },
    # Coffee with Lisa's row (Game+ extras hall) opens with its own key, not with the finish alone (archive_unused: locked without it)
    "lisadate_museum_open": {
        "optional": True,
        "start_floor": "f000",
        "frames": 4000,
        "save": True,
        "work_files": {"pt.ini": b"[progress]\ngame_finished = 1\narchive = lisa_date\narchive_backfill = 1\n"},
        "args": ["--release-locks"],
        "route": ["lisadate_museum_open"],
        "expect": [r"museum: \d+ / \d+ found \(\d+%\), bonus 1", r"input script: 5 expectations, 0 failed"],
    },
    "archive": {
        # the Archive (Extras, gameplay.md 14.7): a cutscene opened from its row plays in the theater's own session and the menu comes
        # back on that row, then a model shows in the theater's viewer and is left; the player's session is unchanged and play goes on.
        # --no-release-locks: the release game opens the museum only after a finished game (archive_unused tests that lock)
        "start_floor": "f005",
        "frames": 14000,
        "args": ["--no-release-locks"],
        "route": ["start", "walk", "archive", "exit", "walk"],
        "expect": [
            r"expect ok .*: PC settings cursor on pc_archive_cutscenes",
            r"expect ok .*: archive entry door open",
            r"archive: theater opens door",
            r"theater: door done \(the demo ended\)",
            r"archive: theater ended",
            r"expect ok .*: menu page 1",
            r"archive: theater opens lisa_stand",
            r"theater: gimmick Ocho in motion OchoStop at",
            r"expect ok .*: session state as saved",
            r"floor: NextFloor f020 -> f030",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the PC page's two picture loaders (UiAssets::PreloadPreviews, Sentry NATIVE-2F): the Museum's halls and the loop browser open
    # with every entry unlocked and close again; tools/unicode_e2e_check.py (previews) runs the route under other code pages
    "pc_previews": {
        "start_floor": "f005",
        "frames": 6000,
        "work_files": {"pt.ini": b"[progress]\ngame_finished = 1\narchive_backfill = 1\n"},
        # every frame drawn: the loaders run in the menu's drawing, which headless otherwise does for the shot frames only
        "args": ["--render-all"],
        "route": ["start", "pc_previews"],
        "expect": [
            r"expect ok .*: PC settings cursor on pc_loop_browser",
            r"expect ok .*: PC settings cursor on pc_archive",
            r"museum: \d+ / \d+ found",
            r"input script: \d+ expectations, 0 failed",
        ],
        "forbid": [(r"museum: \d+ / \d+ found", r"input script:", r"error crash:|ui: preview .* not shown")],
    },
    # the Museum with a pad only: Circle leaves the model viewer and a cutscene, Cross leaves a cutscene but not the model viewer
    # the Museum's music hall (gameplay.md 14.7, museum_music.h): tracks played from the player's sound banks and the ending's demo
    # stream, unlocked by what play reached (pt.ini), the unused ones by the finish; play, seek, next, pause, the loop, the end
    "archive_music": {
        "start_floor": "f005",
        "frames": 9000,
        "save": True,
        "work_files": {"pt.ini": b"[progress]\ngame_finished = 1\narchive_backfill = 1\narchive = event:Play_bg_startroom01,"
                                 b"event:Play_bg_passage01,event:Play_bgm_corridor_f110,demo:gc_p06_010_final,demo:gc_p01_090\n"},
        "args": ["--release-locks", "--audio-offline"],
        "route": ["start", "archive_music"],
        "expect": [
            r"expect ok .*: PC settings cursor on pc_archive_cutscenes",
            r"expect ok .*: PC settings cursor on pc_archive_models",
            r"expect ok .*: archive entry lisa_bathroom_walk open",
            r"expect ok .*: archive entry lisa_backwards locked",
            r"expect ok .*: PC settings cursor on pc_archive_music",
            r"expect ok .*: archive entry music_unrest03 locked",
            r"expect ok .*: archive entry music_unrest01 open",
            r"museum: music_startroom plays media:584815259 \(39\.3 s, from 0\.0 s",
            r"museum: music_startroom at 1[1-3]\.\d s",
            r"museum: music_startroom at [1-4]\.\d s",
            r"museum: music_startroom stops",
            r"museum: music_hallway plays media:174571516 \(33\.8 s",
            r"museum: music_hallway paused at",
            r"museum: music_hallway resumes at",
            r"museum: music_hallway at 33\.\d s",
            r"museum: music_hallway ended",
            r"museum: music_f110 plays media:112489217 \(292\.5 s",
            r"museum: loop on",
            r"museum: music_f110 at 289\.\d s",
            r"museum: music_f110 looped back to 4[23]\.\d s",
            r"museum: loop off",
            r"museum: music_ending plays stream:/Assets/sh/demo/demo_stream/gc_p06_010_final\.fsm \(148\.5 s",
            r"museum: music_unrest01 plays media:812266907",
            r"museum: music_unrest01 at 0\.0 s",
            r"museum: music_unrest01 stops",
            r"expect ok .*: PC settings cursor on pc_archive_music",
            r"input script: \d+ expectations, 0 failed",
        ],
        # the locked tracks between the ended one and the f110 music do not play
        "forbid": [(r"museum: music_hallway ended", r"museum: music_f110 plays", r"museum: music_\w+ plays")],
    },
    # the music hall's tracks open as play reaches them (optional: the full route with the sound system running, which posts the
    # events; without --audio-offline a headless run has no sound and records no event): the start room's and the hallway's
    # ambience (the sound areas), unrest03 on f030, herald01 on f060 (the baby's trap), unrest02 on f080, the f110 music, the ending
    "archive_music_play": {
        "optional": True,
        "start_floor": None,
        "frames": 90000,
        "args": ["--audio-offline"],
        "route": FULL,
        "expect": [
            r"archive: event:Play_bg_startroom01 reached in play",
            r"floor: NextFloor f000 -> f010",
            r"archive: event:Play_bg_passage01 reached in play",
            r"floor: NextFloor f020 -> f030",
            r"archive: event:Play_bgm_unrest03 reached in play",
            r"floor: NextFloor f040 -> f060",
            r"archive: event:Play_bgm_herald01 reached in play",
            r"floor: NextFloor f070 -> f080",
            r"archive: event:Play_bgm_unrest02 reached in play",
            r"floor: NextFloor f100 -> f110",
            r"archive: event:Play_bgm_corridor_f110 reached in play",
            r"floor: NextFloor f160 -> ending",
            r"archive: demo:gc_p06_010_final reached in play",
        ],
    },
    # Lisa's herald music on f090, posted by its id (OchoLogic's warp, 0xD567DD28), opens the music hall's herald track
    "archive_herald": {
        "optional": True,
        "start_floor": "f080",
        "frames": 14000,
        "args": ["--audio-offline"],
        "route": ["start", "lisa"],
        "expect": [
            r"floor: NextFloor f080 -> f090",
            r"ocho: LogicControl 1",
            r"sound: id 0xd567dd28",
            r"archive: event:Play_bgm_herald01 reached in play",
        ],
    },
    # Lisa's free camera entries (Models hall): her walk to the bathroom door (gc_p01_090), her backward steps (gc_p04_120) and her
    # catch (gc_p07_030), each opened by its demo seen in play, played to its end with the free camera framed on her
    "archive_lisa": {
        "start_floor": "f005",
        "frames": 12000,
        # the demos at their own pace: the free camera is moved while she walks
        "demo_rate": 1.0,
        "save": True,
        "work_files": {"pt.ini": b"[progress]\ngame_finished = 1\narchive_backfill = 1\narchive = demo:gc_p01_090,demo:gc_p07_030\n"},
        "args": ["--release-locks", "--virtual-pads", "--audio-offline"],
        "route": ["start", "archive_lisa"],
        "expect": [
            r"expect ok .*: archive entry lisa_bathroom_walk open",
            r"expect ok .*: archive entry lisa_backwards locked",
            r"expect ok .*: archive entry lisa_catch open",
            r"archive: theater opens lisa_bathroom_walk",
            r"theater: lisa_bathroom_walk free camera at \(-1\.1\d 1\.66 25\.4\d\) looking at Lisa \(-11\.6\d 1\.26 25\.4\d\)",
            r"theater: lisa_bathroom_walk free camera moved (?!0\.00)\d+\.\d+ m",
            r"theater: lisa_bathroom_walk done \(the demo ended\)",
            r"archive: theater ended",
            r"archive: theater opens lisa_backwards",
            r"theater: lisa_backwards free camera at \(-3\.5\d 4\.15 24\.5\d\) looking at Lisa",
            r"theater: lisa_backwards done \(the demo ended\)",
            r"archive: theater opens lisa_catch",
            r"theater: lisa_catch free camera at .* looking at Lisa",
            r"expect ok .*: archive viewer open",
            r"theater: lisa_catch done \(the demo ended\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the retroactive unlock (Game::BackfillArchive, archive.h ArchiveFinishKeys): a save finished three times (Game+ tier 3, its tag
    # and count at 0x70) with no museum keys in pt.ini gets, once, exactly the keys every finished game reaches and nothing chance, a
    # look, a zoom, a death or Game+ decides; the halls page then counts them (the counter); a hall's wall shows found exhibits and
    # "???" with the hint of where the others come from
    "archive_backfill": {
        "optional": True,
        "start_floor": None,
        "frames": 4000,
        "save": True,
        "save_files": {"PT_Save_Data0": ARCHIVE_BACKFILL_SAVE},
        "args": ["--release-locks"],
        "route": ["archive_backfill"],
        "expect": [
            r"loop browser: save has the finished flag",
            r"archive: retroactive unlock for a profile finished 3 time\(s\) \(Game\+ tier 3\): 53 of 53 keys every finished game reaches "
            r"granted: demo:gc_p00_010, demo:gc_p00_020, demo:gc_p00_022, demo:gc_p00_030, demo:gc_p00_160, demo:gc_p01_011, demo:gc_p01_020, "
            r"demo:gc_p01_022, demo:gc_p01_050, demo:gc_p01_070, demo:gc_p01_081, demo:gc_p01_090, demo:gc_p01_110, demo:gc_p02_080, "
            r"demo:gc_p02_090, demo:gc_p02_500, demo:gc_p02_510, demo:gc_p02_520, "
            r"demo:gc_p03_010, demo:gc_p03_011, demo:gc_p03_070, demo:gc_p04_120, demo:gc_p04_280, demo:gc_p04_300, demo:gc_p04_310, "
            r"demo:gc_p04_320, demo:gc_p04_340, demo:gc_p04_350, demo:gc_p05_010, demo:gc_p06_010_final, event:Play_Peephole_Theater, "
            r"event:Play_bg_passage01, event:Play_bg_startroom01, event:Play_bgm_corridor_f110, event:Play_bgm_herald01, event:Play_bgm_unrest02, "
            r"event:Play_bgm_unrest03, event:Play_radio_religion_01, floor:f005, floor:f010, floor:f050, floor:f160, gimmick:Baby, "
            r"gimmick:Bag, gimmick:CeilLamp, gimmick:Freezer, gimmick:Ocho, overlay, voice:tria1000_101010, voice:tria1000_171010, "
            r"voice:tria1000_1b1010, voice:tria1000_1g1010, voice:tria1000_1n1010$",
            ARCHIVE_BACKFILL_COUNT,
            r"expect ok .*: PC settings cursor on pc_archive_hall_radio",
            r"input script: 32 expectations, 0 failed",
        ],
        # given once: not again while the museum is open
        "forbid": [(r"museum: \d+ / \d+ found", r"input script:", r"archive: retroactive unlock")],
    },
    # the same in Turkish (the save's subtitle language 7): the halls page, the counter and a wall with "???" and its hints
    "archive_backfill_tr": {
        "optional": True,
        "start_floor": None,
        "frames": 4000,
        "save": True,
        "save_files": {"PT_Save_Data0": ARCHIVE_BACKFILL_SAVE[:0x2E] + bytes([7, 1]) + ARCHIVE_BACKFILL_SAVE[0x30:]},
        "args": ["--release-locks"],
        "route": ["archive_backfill"],
        "expect": [ARCHIVE_BACKFILL_COUNT, r"input script: 32 expectations, 0 failed"],
    },
    "archivepad": {
        "pad": True,
        "start_floor": "f005",
        "frames": 8000,
        "args": ["--no-release-locks"],
        "route": ["start", "walk", "archive_pad"],
        "expect": [
            r"archive: theater opens lisa_stand",
            r"expect ok .*: archive viewer open",
            r"expect ok .*: archive viewer closed",
            r"archive: theater opens door",
            r"theater: door done \(left\)",
            r"archive: theater opens door",
            r"theater: door done \(left\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "pccontrols": {
        # the Controls row follows the device shown by the button prompts, and mouse and gamepad sensitivity stay independent
        "pad": True,
        "start_floor": "f010",
        "frames": 1500,
        "route": ["start", "pc_controls_device"],
        "expect": [
            r"expect ok .*: PC setting pc_gamepad_sensitivity = 2",
            r"expect ok .*: PC setting pc_mouse = 4",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "pc_scroll": {
        "start_floor": "f010",
        "frames": 1500,
        "route": ["start", "pc_scroll"],
        "expect": [
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the Museum's music hall with a pad's stick: a stick held, drifting about the move threshold or slightly deflected plays the
    # selection sound once per move (the repeat is ten a second after 0.4 s), not once every other frame
    "archive_stick": {
        "pad": True,
        "start_floor": "f005",
        "frames": 3000,
        "save": True,
        "work_files": {"pt.ini": b"[progress]\ngame_finished = 1\narchive_backfill = 1\narchive = event:Play_bg_startroom01,"
                                 b"event:Play_bg_passage01,event:Play_bgm_corridor_f110,demo:gc_p06_010_final,demo:gc_p01_090\n"},
        "args": ["--release-locks", "--audio-offline"],
        "route": ["start", "archive_stick"],
        "expect": [
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "speech": {
        "start_floor": "f060",
        "frames": 12000,
        "save": True,
        "args": ["--audio-offline"],
        "route": ["start", "walk", "speech_reset"],
        "expect": [
            r"sound: marker TRIA1000_1n1010",
            r"expect ok .*: speech 1",
            r"save: confirmed reset restarts the current game session",
            r"expect ok .*: speech 0",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "pad": {
        "optional": True,
        "pad": True,
        "start_floor": None,
        "frames": 60000,
        "extra": ["--options-menu"],
        "route": (["padboot", "start", "walk", "exit", "f005", "exit"] + HALLWAY + ["f030", "exit"] + HALLWAY +
                  ["f060", "exit", "f050a", "exit", "f050b", "exit"]),
        "expect": [
            r"save: no save data, first boot",
            r"ui: option menu opened \(first boot\)",
            r"virtual pad 0: 'DualSense",
            r"expect ok .*: brightness 6",
            r"ui: option menu closed",
            r"controller: ChangeGameStep\(StartGame\)",
            r"ui: option menu opened \(pause\)",
            r"floor: NextFloor f000 -> f010",
            r"floor: NextFloor f010 -> f005",
            r"floor: NextFloor f005 -> f020",
            r"floor: NextFloor f020 -> f030",
            r"trap: ShDemoExec gc_p01_021",
            r"floor: NextFloor f030 -> f040",
            r"floor: NextFloor f040 -> f060",
            r"trap: ShDemoExec gc_p01_022",
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"nazo: XMark cleared",
            r"floor: NextFloor f050 -> f070",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Game+ (on from the first finish, no setting): a finish from f120 counts and the ending's restart follows; the hallway's
    # own copy of the bathtub Lisa stays hidden (she is drawn only where gameplus_mazeb checks)
    "gameplus_finish": {
        "optional": True,
        "start_floor": "f120",
        "frames": 40000,
        "save": True,
        "args": ["--street-offer", "restart"],
        "route": ["f120", "start", "f160", "exit", "gameplus_hidden"],
        "expect": [
            r"floor: NextFloor f160 -> ending",
            r"game\+: finished flag set \(save tag\)",
            r"game\+: finish 1 counted",
            r"controller: step 30 -> 17",
            r"controller: ChangeGameStep\(StartGame\)",
            r"expect ok frame \d+: body shsb_bath001_ocho001_0000 hidden",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the real New Game+ flow (no forced tier): a finish from f120 counts (tier 1), the ending's load and its restart release the
    # models nothing holds (Game::ReleaseUnusedAssets), and the hallway after the restart still shows the bathtub Lisa (1.0.4: the
    # released hallway mesh she borrowed was left dangling and she was gone)
    "gameplus_restart_tub": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f120",
        "frames": 40000,
        "save": True,
        "args": ["--street-offer", "restart"],
        "route": ["f120", "start", "f160", "exit", "gameplus_after_ending", "start", "walk", "gameplus_check_tub"],
        "expect": [
            r"models: ending load: released",
            r"game\+: finish 1 counted",
            r"models: reset after the ending: released",
            r"controller: ChangeGameStep\(StartGame\)",
            r"game\+: tub Lisa in \S+ \(f0\d\d\), baby in its sink",
            r"expect ok frame \d+: game\+ tub lisa shown",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Game+ in maze B (f110) after a finish (the marker set by the input script): the bathtub Lisa sits in the tub of the
    # bathroom where the baby talks
    "gameplus_mazeb": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f060",
        "frames": 20000,
        "route": ["gameplus_mark"] + LATE[:LATE.index("mazeB") + 1] + ["gameplus_mazeb_on"],
        "expect": [
            r"game\+: finished flag set \(save tag\)",
            r"floor: NextFloor f100 -> f110",
            r"game\+: tub Lisa in \S+ \(f110\), baby in its sink",
            r"expect ok frame \d+: game\+ tub lisa shown",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Game+ after a second finish (two markers): Lisa's left side drawn as hsh0's while she appears, dashes and kills (f080)
    "gameplus_arm": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f080",
        "frames": 14000,
        "route": ["gameplus_mark", "gameplus_mark", "start", "lisa"],
        "expect": [
            r"game\+: finish 2 counted",
            r"game\+: Lisa's hsh0 left side ready",
            r"ocho: appears at spawn",
            r"demo: gc_p07_030 playing",
        ],
    },
    # the Game+ notice: "New Game Plus 2/4" once when play reaches a hallway after two finishes, not again after Lisa's kill and
    # the respawn in the same tier
    "gameplus_notice": {
        "optional": True,
        "gameplus": True,
        "save": True,
        "start_floor": "f080",
        "frames": 14000,
        "route": ["gameplus_mark", "gameplus_mark", "start", "lisa"],
        "expect": [
            r"game\+: finish 2 counted",
            r"game\+: notice for tier 2 given",
            r"notice: shown pc_gameplus_notice.2",
            r"demo: gc_p07_030 playing",
        ],
    },
    # one scenario per Game+ tier (PT_GAMEPLUS_TIER forces the tier; each checks its tier's content and the lower tiers'):
    # tier 1, the hallway: the bathtub Lisa in the tub of the bathroom whose sink holds the baby
    # a profile upgraded from 1.0.3: its save finished once (the tag 'NGP1' and the plain finish count 1 at 0x74, as 1.0.3 wrote it)
    # and a pt.ini without the 1.0.4 Game+ setting; the hallway after the start room shows the bathtub Lisa (tier 1, no forced tier)
    "gameplus_save103": {
        "optional": True,
        "gameplus": True,
        "start_floor": None,
        "frames": 6000,
        "save": True,
        "save_files": {"PT_Save_Data0": GAMEPLUS_103_SAVE},
        "work_files": {"pt.ini": b"[camera]\nroll = 1\n"},
        "route": ["gameplus_save_loaded", "start", "walk", "gameplus_check_tub"],
        "expect": [
            r"expect ok frame \d+: game\+ tier 1",
            r"game\+: tub Lisa in \S+ \(f010\), baby in its sink",
            r"expect ok frame \d+: game\+ tub lisa shown",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "gameplus_tier1": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f010",
        "frames": 6000,
        "env": {"PT_GAMEPLUS_TIER": "1"},
        "route": ["start", "gameplus_check_tub"],
        "expect": [
            r"game\+: content in this build",
            r"game\+: tub Lisa in \S+ \(f0\d\d\), baby in its sink",
            r"expect ok frame \d+: game\+ tub lisa shown",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # tier 2, f080: the bathtub Lisa still there, and Lisa with hsh0's left side through her appearance, dash and kill
    "gameplus_tier2": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f080",
        "frames": 14000,
        "env": {"PT_GAMEPLUS_TIER": "2"},
        "route": ["start", "gameplus_check_tub", "lisa"],
        "expect": [
            r"game\+: Lisa's hsh0 left side ready",
            r"game\+: tub Lisa in",
            r"expect ok frame \d+: game\+ tub lisa shown",
            r"ocho: appears at spawn",
            r"demo: gc_p07_030 playing",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the bathtub Lisa after a reset that releases the unused models (PT_TEST_VRAM_TIGHT, as on a card short of video memory or
    # after the ending): 1.0.4 kept her mesh pointer, put the freed mesh into the next stage and crashed in PrepareFrame (Sentry,
    # RTX 2060 and RTX 3080, 2026-10-15); she must be drawn again after Lisa's kill and the respawn
    "gameplus_tub_release": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f080",
        "frames": 20000,
        "env": {"PT_GAMEPLUS_TIER": "1", "PT_TEST_VRAM_TIGHT": "1"},
        "route": ["start", "gameplus_check_tub", "lisa", "start", "gameplus_check_tub"],
        "expect": [
            r"game\+: tub Lisa in",
            r"demo: gc_p07_030 playing",
            r"game\+: tub Lisa taken out of the stages before the unused models are released",
            r"models: reset with the video memory short: released",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # tier 2, the intercom in place of the painting (game_plus_intercom_play.cpp): every ring forced, f060 to f050 with the captions on;
    # walking up to it on f050's first pass gives Lisa's tell (her chase comes on the next loop), then the chime, the screen sputtering
    # on and off again; leaving into f050's second pass logs her chase starting after the tell
    "gameplus_intercom": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f060",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "2", "PT_INTERCOM_RING": "1"},
        "args": ["--audio-offline"],
        "work_files": {"pt.ini": b"[accessibility]\nsound_captions = 1\n"},
        "route": ["start", "intercom_ring", "exit"],
        "expect": [
            r"game\+: intercom shown, painting hidden in \S+ \(f060\)",
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"expect ok frame \d+: intercom shown in \d active stage",
            r"game\+: intercom roll for f050 pass 1: rings \(forced",
            r"game\+: intercom crackles \(Lisa's tell, her chase next; f050 pass 1",
            r"sound caption: pc_cap_intercom_crackle",
            r"game\+: intercom rings \(after the tell; f050 pass 1",
            r"sound caption: pc_cap_intercom_chime",
            r"game\+: intercom screen sputters on",
            r"sound caption: pc_cap_intercom_hum",
            r"game\+: intercom lit over, the screen grey again",
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"game\+: intercom: Lisa's chase \(state 3\) began \d+\.\d+ s after the tell",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the same walk in the turned or moved stage copies (f010 and f160 at (-31.58 0.20 16.14), f005 at (-20.62 -1.20 -5.99)), also
    # with every ring forced
    "gameplus_intercom_wall_f010_t0": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f000",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "0"},
        "args": ["--audio-offline"],
        "route": ["start", "intercom_wall"],
        "expect": [
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    "gameplus_intercom_wall_f010_t2": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f000",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "2"},
        "args": ["--audio-offline"],
        "route": ["start", "intercom_wall"],
        "expect": [
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    "gameplus_intercom_wall_f010_t2ring": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f000",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "2", "PT_INTERCOM_RING": "1"},
        "args": ["--audio-offline"],
        "route": ["start", "intercom_wall"],
        "expect": [
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    "gameplus_intercom_wall_f005_t0": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f005",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "0"},
        "args": ["--audio-offline"],
        "route": ["start", "intercom_wall"],
        "expect": [
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    "gameplus_intercom_wall_f005_t2": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f005",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "2"},
        "args": ["--audio-offline"],
        "route": ["start", "intercom_wall"],
        "expect": [
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    "gameplus_intercom_wall_f005_t2ring": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f005",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "2", "PT_INTERCOM_RING": "1"},
        "args": ["--audio-offline"],
        "route": ["start", "intercom_wall"],
        "expect": [
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    # F9 logs the capture pose (and the route lines) as a key press through the input device, headless as with a window
    "pose_f9": {
        "optional": True,
        "frames": 3000,
        "args": ["--virtual-pads"],
        "route": ["start", "f9_pose"],
        "expect": [
            r"capture pose: floor f\d+ loop \d+ step \d+, camera",
            r"capture route: goto ",
            r"capture route: lookat ",
        ],
    },
    # walking straight into the wall where the intercom (tier 2) or the painting (tiers 0 and 1) hangs: the closest the feet get
    # ("gave up" in the log) must be the same in all three
    "gameplus_intercom_wall_t0": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f060",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "0"},
        "route": ["start", "intercom_wall"],
        "expect": [
            r"floor: NextFloor f060 -> f050",
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    "gameplus_intercom_wall_t1": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f060",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "1"},
        "route": ["start", "intercom_wall"],
        "expect": [
            r"floor: NextFloor f060 -> f050",
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    "gameplus_intercom_wall_t2": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f060",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "2"},
        "route": ["start", "intercom_wall"],
        "expect": [
            r"floor: NextFloor f060 -> f050",
            r"input script: gave up on \(\S+ \S+ \S+\) at \(\S+ \S+ \S+\), frame \d+",
        ],
    },
    # tier 2 on the HELLO loop (f090, its second pass): no ring; at the corridor's end the intercom crackles a few seconds before
    # Lisa's herald, which comes when her Warp's hidden wait (40 to 89 s) runs out as ever
    "gameplus_intercom_herald": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f090",
        "frames": 9000,
        "env": {"PT_GAMEPLUS_TIER": "2"},
        "args": ["--audio-offline"],
        "route": ["start", "intercom_herald"],
        "expect": [
            r"floor: NextFloor f090 -> f090 \(loop 2\)",
            r"ocho: LogicControl 1",
            r"game\+: intercom crackles \(Lisa's tell; f090 pass 2",
            r"game\+: intercom tell for Lisa's herald, [34]\.\d+ s before it",
            r"game\+: intercom: Lisa's herald [34]\.\d+ s after the tell",
        ],
    },
    # tier 1 (one finish): the painting stays and the intercom never rings, even with every ring forced
    "gameplus_intercom_none": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f060",
        "frames": 4000,
        "env": {"PT_GAMEPLUS_TIER": "1", "PT_INTERCOM_RING": "1"},
        "route": ["start", "intercom_none"],
        "expect": [
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"expect ok frame \d+: intercom not shown",
            r"expect ok frame \d+: intercom idle",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # tier 3, f060 to the red maze: the bathroom loop's bed is unrest01, the radio's three extra lines after its broadcast on both
    # f050 passes, Lisa's laugh at the HELL writing, mystery01 in the red maze; the bathtub Lisa still there. The audio is mixed
    # offline, frame by frame, so the radio's 96 s broadcast ends in game time as it does in play
    "gameplus_tier3": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f040",
        "frames": 40000,
        "env": {"PT_GAMEPLUS_TIER": "3"},
        "args": ["--audio-offline"],
        "route": (["start", "f060", "exit", "f050a", "gameplus_radio", "exit", "f050b", "gameplus_radio"] +
                  LATE[LATE.index("f050b") + 1:LATE.index("mazeB") + 1] + ["gameplus_mazeb_on"]),
        "expect": [
            r"game\+: unrest01 in place of herald01 \(f060",
            r"game\+: tub Lisa in",
            r"game\+: radio line Play_radio_voice_06 after the broadcast \(f050 pass 1",
            # the lines' markers open their Museum exhibits (radio_06 to 08), each line its own
            r"archive: voice:tria1000_1d1010 reached in play",
            r"archive: voice:tria1000_1e1010 reached in play",
            r"game\+: radio line Play_radio_voice_08 after the broadcast \(f050 pass 1",
            r"archive: voice:tria1000_1f1010 reached in play",
            r"game\+: radio line Play_radio_voice_06 after the broadcast \(f050 pass 2",
            r"game\+: radio line Play_radio_voice_08 after the broadcast \(f050 pass 2",
            r"game\+: Lisa laughs at the HELL writing \(first pair",
            r"game\+: Lisa laughs at the HELL writing \(second pair",
            r"floor: NextFloor f100 -> f110",
            r"game\+: mystery01 in place of corridor_f110",
            r"expect ok frame \d+: game\+ tub lisa shown",
        ],
    },
    # tier 4, the tier 3 route: the unused roach demos beside their used siblings (f060, both f050 passes, f070), the man's footsteps
    # on f100; the tier 1 to 3 content still there
    "gameplus_tier4": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f040",
        "frames": 40000,
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "args": ["--audio-offline"],
        "route": (["start", "f060", "exit", "f050a", "gameplus_radio", "exit", "f050b", "gameplus_radio"] +
                  LATE[LATE.index("f050b") + 1:LATE.index("mazeB") + 1] + ["gameplus_mazeb_on"]),
        "expect": [
            r"game\+: demo gc_p03_012 with gc_p03_011",
            r"game\+: unrest01 in place of herald01 \(f060",
            r"game\+: demo gc_p04_330 with gc_p04_320",
            r"game\+: radio line Play_radio_voice_08 after the broadcast \(f050 pass 2",
            r"game\+: demo gc_p04_200 with gc_p04_120",
            r"game\+: Lisa laughs at the HELL writing \(first pair",
            r"game\+: demo gc_p04_290, the man's footsteps",
            r"game\+: mystery01 in place of corridor_f110",
            r"expect ok frame \d+: game\+ tub lisa shown",
        ],
    },
    # Coffee with Lisa (Game+ tier 4, docs/superpowers/specs/2026-10-13-coffee-with-lisa-design.md): three f160 loops with a
    # rush of Lisa's each (the first ends in her kill and the game over, the count stays), then the zoom on her portrait (the
    # wedding photo on the telephone table) for 10 s starts the date; its warm answers end it well and the player is back. The
    # hints come with it: the light's tint and the sting after each rush, the glint after the third, the zoom's heartbeat
    "lisadate_trigger": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f160",
        "frames": 70000,
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "args": ["--audio-offline"],
        "work_files": {"pt.ini": b"[accessibility]\nsound_captions = 1\n"},
        "route": (["start", "lisadate_rush", "lisadate_kill", "start", "lisadate_rush", "walk", "exit", "lisadate_rush",
                   "lisadate_portrait", "lisadate_good"]),
        "expect": [
            r"lisa date: rush 1/3 \(dash on f160",
            r"lisa date: hint light tint 0 -> 1",
            r"lisa date: hint sting 1/3",
            r"sound caption: pc_cap_soft_notes",
            r"message: gc_p07_030 GotoGameOver",
            r"lisa date: rush 2/3 \(dash on f160",
            r"lisa date: hint light tint 1 -> 2",
            r"lisa date: hint sting 2/3",
            r"floor: NextFloor f160 -> f160 \(loop 2\)",
            r"lisa date: rush 3/3 \(dash on f160",
            r"lisa date: hint glint lit on the portrait \(rush 3/3\)",
            r"lisa date: hint light tint 2 -> 3",
            r"lisa date: hint sting 3/3",
            r"lisa date: portrait zoom armed",
            r"lisa date: hint heartbeat 1 at",
            r"sound caption: pc_cap_soft_heartbeat",
            r"lisa date: hint glint and tint fade \(the date started\)",
            r"lisa date: portrait held 10 s, the date starts",
            r"lisa date: started \(the portrait zoom\)",
            r"lisa date: ending good",
            r"lisa date: returned to f160 \(ending good\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the three endings by `slisadate` (any tier: the hook starts the date where the script stands), one after another in the
    # start room of f160, each back to the game; then the Museum's row replays it
    "lisadate_endings": {
        "optional": True,
        "start_floor": "f160",
        "frames": 36000,
        "args": ["--audio-offline"],
        "route": ["lisadate_wait", "lisadate_good", "lisadate_bad", "lisadate_true", "lisadate_museum"],
        "expect": [
            r"lisa date: started \(the input script\)",
            r"lisa date: music music",
            r"lisa date: sound sweet -> dread",
            r"lisa date: the ambient bed 0x1A95368D \(playing id [1-9]",
            r"lisa date: choice c12 -> c12_a",
            r"lisa date: photo",
            r"lisa date: sound dread -> sweet",
            r"lisa date: ending good \(affection 20",
            r"lisa date: returned to f160 \(ending good\)",
            r"lisa date: started \(the input script\)",
            r"lisa date: choice c8 -> c8_b",
            r"lisa date: sound \w+ -> scare",
            r"lisa date: Play_bgm_unrest01 \(playing id [1-9]",
            r"lisa date: the kill demo's sound",
            r"lisa date: scare",
            r"lisa date: ending bad",
            r"lisa date: returned to f160 \(ending bad\)",
            r"lisa date: started \(the input script\)",
            r"lisa date: choice c11 -> c11_c \(affection \+0, now \d+, flag peephole\)",
            r"lisa date: ending true \(affection \d+, flags 4",
            r"lisa date: returned to f160 \(ending true\)",
            r"lisa date: started \(the Museum\)",
            r"lisa date: ending good",
            r"lisa date: returned to the menu on f160 \(ending good\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the pause menu over the date: it opens and closes over it, and its "Leave the date" ends the date with no ending
    "lisadate_menu": {
        "optional": True,
        "pad": True,
        "start_floor": "f160",
        "frames": 6000,
        "route": ["lisadate_wait", "lisadate_menu"],
        "expect": [
            r"lisa date: started \(the input script\)",
            r"ui: option menu opened \(pause\)",
            r"lisa date: music music",
            r"lisa date: choice c1 -> c1_a",
            r"ui: option menu opened \(pause\)",
            r"ui: leave the date selected from Options",
            r"lisa date: left from the menu \(no ending\)",
            r"lisa date: returned to f160 \(ending none\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # VR only (python tools/walkthrough.py --vr lisadate_vr): the date on the virtual screen, its lines and choices by the right
    # controller's A (the test runtime's script presses it every 25 frames; the layers of frames 1700 and 2300 are written)
    "lisadate_vr": {
        "optional": True,
        "vr_only": True,
        "start_floor": "f160",
        "frames": 14000,
        "args": ["--audio-offline"],
        "env": {"PT_XRTEST_SCRIPT": "input 1400 1403 /user/hand/right/input/a/click 1;input 1425 1428 /user/hand/right/input/a/click 1;input 1450 1453 /user/hand/right/input/a/click 1;input 1475 1478 /user/hand/right/input/a/click 1;input 1500 1503 /user/hand/right/input/a/click 1;input 1525 1528 /user/hand/right/input/a/click 1;input 1550 1553 /user/hand/right/input/a/click 1;input 1575 1578 /user/hand/right/input/a/click 1;input 1600 1603 /user/hand/right/input/a/click 1;input 1625 1628 /user/hand/right/input/a/click 1;input 1650 1653 /user/hand/right/input/a/click 1;input 1675 1678 /user/hand/right/input/a/click 1;input 1700 1703 /user/hand/right/input/a/click 1;input 1725 1728 /user/hand/right/input/a/click 1;input 1750 1753 /user/hand/right/input/a/click 1;input 1775 1778 /user/hand/right/input/a/click 1;input 1800 1803 /user/hand/right/input/a/click 1;input 1825 1828 /user/hand/right/input/a/click 1;input 1850 1853 /user/hand/right/input/a/click 1;input 1875 1878 /user/hand/right/input/a/click 1;input 1900 1903 /user/hand/right/input/a/click 1;input 1925 1928 /user/hand/right/input/a/click 1;input 1950 1953 /user/hand/right/input/a/click 1;input 1975 1978 /user/hand/right/input/a/click 1;input 2000 2003 /user/hand/right/input/a/click 1;input 2025 2028 /user/hand/right/input/a/click 1;input 2050 2053 /user/hand/right/input/a/click 1;input 2075 2078 /user/hand/right/input/a/click 1;input 2100 2103 /user/hand/right/input/a/click 1;input 2125 2128 /user/hand/right/input/a/click 1;input 2150 2153 /user/hand/right/input/a/click 1;input 2175 2178 /user/hand/right/input/a/click 1;input 2200 2203 /user/hand/right/input/a/click 1;input 2225 2228 /user/hand/right/input/a/click 1;input 2250 2253 /user/hand/right/input/a/click 1;input 2275 2278 /user/hand/right/input/a/click 1;input 2300 2303 /user/hand/right/input/a/click 1;input 2325 2328 /user/hand/right/input/a/click 1;input 2350 2353 /user/hand/right/input/a/click 1;input 2375 2378 /user/hand/right/input/a/click 1;input 2400 2403 /user/hand/right/input/a/click 1;input 2425 2428 /user/hand/right/input/a/click 1;input 2450 2453 /user/hand/right/input/a/click 1;input 2475 2478 /user/hand/right/input/a/click 1;input 2500 2503 /user/hand/right/input/a/click 1;input 2525 2528 /user/hand/right/input/a/click 1;input 2550 2553 /user/hand/right/input/a/click 1;input 2575 2578 /user/hand/right/input/a/click 1;input 2600 2603 /user/hand/right/input/a/click 1;input 2625 2628 /user/hand/right/input/a/click 1;input 2650 2653 /user/hand/right/input/a/click 1;input 2675 2678 /user/hand/right/input/a/click 1;input 2700 2703 /user/hand/right/input/a/click 1;input 2725 2728 /user/hand/right/input/a/click 1;input 2750 2753 /user/hand/right/input/a/click 1;input 2775 2778 /user/hand/right/input/a/click 1;input 2800 2803 /user/hand/right/input/a/click 1;input 2825 2828 /user/hand/right/input/a/click 1;input 2850 2853 /user/hand/right/input/a/click 1;input 2875 2878 /user/hand/right/input/a/click 1;input 2900 2903 /user/hand/right/input/a/click 1;input 2925 2928 /user/hand/right/input/a/click 1;input 2950 2953 /user/hand/right/input/a/click 1;input 2975 2978 /user/hand/right/input/a/click 1;input 3000 3003 /user/hand/right/input/a/click 1;input 3025 3028 /user/hand/right/input/a/click 1;input 3050 3053 /user/hand/right/input/a/click 1;input 3075 3078 /user/hand/right/input/a/click 1;input 3100 3103 /user/hand/right/input/a/click 1;input 3125 3128 /user/hand/right/input/a/click 1;input 3150 3153 /user/hand/right/input/a/click 1;input 3175 3178 /user/hand/right/input/a/click 1;input 3200 3203 /user/hand/right/input/a/click 1;input 3225 3228 /user/hand/right/input/a/click 1;input 3250 3253 /user/hand/right/input/a/click 1;input 3275 3278 /user/hand/right/input/a/click 1;input 3300 3303 /user/hand/right/input/a/click 1;input 3325 3328 /user/hand/right/input/a/click 1;input 3350 3353 /user/hand/right/input/a/click 1;input 3375 3378 /user/hand/right/input/a/click 1;input 3400 3403 /user/hand/right/input/a/click 1;input 3425 3428 /user/hand/right/input/a/click 1;input 3450 3453 /user/hand/right/input/a/click 1;input 3475 3478 /user/hand/right/input/a/click 1;input 3500 3503 /user/hand/right/input/a/click 1;input 3525 3528 /user/hand/right/input/a/click 1;input 3550 3553 /user/hand/right/input/a/click 1;input 3575 3578 /user/hand/right/input/a/click 1;input 3600 3603 /user/hand/right/input/a/click 1;input 3625 3628 /user/hand/right/input/a/click 1;input 3650 3653 /user/hand/right/input/a/click 1;input 3675 3678 /user/hand/right/input/a/click 1;input 3700 3703 /user/hand/right/input/a/click 1;input 3725 3728 /user/hand/right/input/a/click 1;input 3750 3753 /user/hand/right/input/a/click 1;input 3775 3778 /user/hand/right/input/a/click 1;input 3800 3803 /user/hand/right/input/a/click 1;input 3825 3828 /user/hand/right/input/a/click 1;input 3850 3853 /user/hand/right/input/a/click 1;input 3875 3878 /user/hand/right/input/a/click 1;input 3900 3903 /user/hand/right/input/a/click 1;input 3925 3928 /user/hand/right/input/a/click 1;input 3950 3953 /user/hand/right/input/a/click 1;input 3975 3978 /user/hand/right/input/a/click 1;input 4000 4003 /user/hand/right/input/a/click 1;input 4025 4028 /user/hand/right/input/a/click 1;input 4050 4053 /user/hand/right/input/a/click 1;input 4075 4078 /user/hand/right/input/a/click 1;input 4100 4103 /user/hand/right/input/a/click 1;input 4125 4128 /user/hand/right/input/a/click 1;input 4150 4153 /user/hand/right/input/a/click 1;input 4175 4178 /user/hand/right/input/a/click 1;input 4200 4203 /user/hand/right/input/a/click 1;input 4225 4228 /user/hand/right/input/a/click 1;input 4250 4253 /user/hand/right/input/a/click 1;input 4275 4278 /user/hand/right/input/a/click 1;input 4300 4303 /user/hand/right/input/a/click 1;input 4325 4328 /user/hand/right/input/a/click 1;input 4350 4353 /user/hand/right/input/a/click 1;input 4375 4378 /user/hand/right/input/a/click 1;input 4400 4403 /user/hand/right/input/a/click 1;input 4425 4428 /user/hand/right/input/a/click 1;input 4450 4453 /user/hand/right/input/a/click 1;input 4475 4478 /user/hand/right/input/a/click 1;input 4500 4503 /user/hand/right/input/a/click 1;input 4525 4528 /user/hand/right/input/a/click 1;input 4550 4553 /user/hand/right/input/a/click 1;input 4575 4578 /user/hand/right/input/a/click 1;input 4600 4603 /user/hand/right/input/a/click 1;input 4625 4628 /user/hand/right/input/a/click 1;input 4650 4653 /user/hand/right/input/a/click 1;input 4675 4678 /user/hand/right/input/a/click 1;input 4700 4703 /user/hand/right/input/a/click 1;input 4725 4728 /user/hand/right/input/a/click 1;input 4750 4753 /user/hand/right/input/a/click 1;input 4775 4778 /user/hand/right/input/a/click 1;input 4800 4803 /user/hand/right/input/a/click 1;input 4825 4828 /user/hand/right/input/a/click 1;input 4850 4853 /user/hand/right/input/a/click 1;input 4875 4878 /user/hand/right/input/a/click 1;input 4900 4903 /user/hand/right/input/a/click 1;input 4925 4928 /user/hand/right/input/a/click 1;input 4950 4953 /user/hand/right/input/a/click 1;input 4975 4978 /user/hand/right/input/a/click 1;input 5000 5003 /user/hand/right/input/a/click 1;input 5025 5028 /user/hand/right/input/a/click 1;input 5050 5053 /user/hand/right/input/a/click 1;input 5075 5078 /user/hand/right/input/a/click 1;input 5100 5103 /user/hand/right/input/a/click 1;input 5125 5128 /user/hand/right/input/a/click 1;input 5150 5153 /user/hand/right/input/a/click 1;input 5175 5178 /user/hand/right/input/a/click 1;input 5200 5203 /user/hand/right/input/a/click 1;input 5225 5228 /user/hand/right/input/a/click 1;input 5250 5253 /user/hand/right/input/a/click 1;input 5275 5278 /user/hand/right/input/a/click 1;input 5300 5303 /user/hand/right/input/a/click 1;input 5325 5328 /user/hand/right/input/a/click 1;input 5350 5353 /user/hand/right/input/a/click 1;input 5375 5378 /user/hand/right/input/a/click 1;input 5400 5403 /user/hand/right/input/a/click 1;input 5425 5428 /user/hand/right/input/a/click 1;input 5450 5453 /user/hand/right/input/a/click 1;input 5475 5478 /user/hand/right/input/a/click 1;input 5500 5503 /user/hand/right/input/a/click 1;input 5525 5528 /user/hand/right/input/a/click 1;input 5550 5553 /user/hand/right/input/a/click 1;input 5575 5578 /user/hand/right/input/a/click 1;input 5600 5603 /user/hand/right/input/a/click 1;input 5625 5628 /user/hand/right/input/a/click 1;input 5650 5653 /user/hand/right/input/a/click 1;input 5675 5678 /user/hand/right/input/a/click 1;input 5700 5703 /user/hand/right/input/a/click 1;input 5725 5728 /user/hand/right/input/a/click 1;input 5750 5753 /user/hand/right/input/a/click 1;input 5775 5778 /user/hand/right/input/a/click 1;input 5800 5803 /user/hand/right/input/a/click 1;input 5825 5828 /user/hand/right/input/a/click 1;input 5850 5853 /user/hand/right/input/a/click 1;input 5875 5878 /user/hand/right/input/a/click 1;input 5900 5903 /user/hand/right/input/a/click 1;input 5925 5928 /user/hand/right/input/a/click 1;input 5950 5953 /user/hand/right/input/a/click 1;input 5975 5978 /user/hand/right/input/a/click 1;input 6000 6003 /user/hand/right/input/a/click 1;input 6025 6028 /user/hand/right/input/a/click 1;input 6050 6053 /user/hand/right/input/a/click 1;input 6075 6078 /user/hand/right/input/a/click 1;input 6100 6103 /user/hand/right/input/a/click 1;input 6125 6128 /user/hand/right/input/a/click 1;input 6150 6153 /user/hand/right/input/a/click 1;input 6175 6178 /user/hand/right/input/a/click 1;input 6200 6203 /user/hand/right/input/a/click 1;input 6225 6228 /user/hand/right/input/a/click 1;input 6250 6253 /user/hand/right/input/a/click 1;input 6275 6278 /user/hand/right/input/a/click 1;input 6300 6303 /user/hand/right/input/a/click 1;input 6325 6328 /user/hand/right/input/a/click 1;input 6350 6353 /user/hand/right/input/a/click 1;input 6375 6378 /user/hand/right/input/a/click 1;input 6400 6403 /user/hand/right/input/a/click 1;input 6425 6428 /user/hand/right/input/a/click 1;input 6450 6453 /user/hand/right/input/a/click 1;input 6475 6478 /user/hand/right/input/a/click 1;input 6500 6503 /user/hand/right/input/a/click 1;input 6525 6528 /user/hand/right/input/a/click 1;input 6550 6553 /user/hand/right/input/a/click 1;input 6575 6578 /user/hand/right/input/a/click 1;input 6600 6603 /user/hand/right/input/a/click 1;input 6625 6628 /user/hand/right/input/a/click 1;input 6650 6653 /user/hand/right/input/a/click 1;input 6675 6678 /user/hand/right/input/a/click 1;input 6700 6703 /user/hand/right/input/a/click 1;input 6725 6728 /user/hand/right/input/a/click 1;input 6750 6753 /user/hand/right/input/a/click 1;input 6775 6778 /user/hand/right/input/a/click 1;input 6800 6803 /user/hand/right/input/a/click 1;input 6825 6828 /user/hand/right/input/a/click 1;input 6850 6853 /user/hand/right/input/a/click 1;input 6875 6878 /user/hand/right/input/a/click 1;input 6900 6903 /user/hand/right/input/a/click 1;input 6925 6928 /user/hand/right/input/a/click 1;input 6950 6953 /user/hand/right/input/a/click 1;input 6975 6978 /user/hand/right/input/a/click 1;input 7000 7003 /user/hand/right/input/a/click 1;input 7025 7028 /user/hand/right/input/a/click 1;input 7050 7053 /user/hand/right/input/a/click 1;input 7075 7078 /user/hand/right/input/a/click 1;input 7100 7103 /user/hand/right/input/a/click 1;input 7125 7128 /user/hand/right/input/a/click 1;input 7150 7153 /user/hand/right/input/a/click 1;input 7175 7178 /user/hand/right/input/a/click 1;input 7200 7203 /user/hand/right/input/a/click 1;input 7225 7228 /user/hand/right/input/a/click 1;input 7250 7253 /user/hand/right/input/a/click 1;input 7275 7278 /user/hand/right/input/a/click 1;input 7300 7303 /user/hand/right/input/a/click 1;input 7325 7328 /user/hand/right/input/a/click 1;input 7350 7353 /user/hand/right/input/a/click 1;input 7375 7378 /user/hand/right/input/a/click 1;input 7400 7403 /user/hand/right/input/a/click 1;input 7425 7428 /user/hand/right/input/a/click 1;input 7450 7453 /user/hand/right/input/a/click 1;input 7475 7478 /user/hand/right/input/a/click 1;input 7500 7503 /user/hand/right/input/a/click 1;input 7525 7528 /user/hand/right/input/a/click 1;input 7550 7553 /user/hand/right/input/a/click 1;input 7575 7578 /user/hand/right/input/a/click 1;input 7600 7603 /user/hand/right/input/a/click 1;input 7625 7628 /user/hand/right/input/a/click 1;input 7650 7653 /user/hand/right/input/a/click 1;input 7675 7678 /user/hand/right/input/a/click 1;input 7700 7703 /user/hand/right/input/a/click 1;input 7725 7728 /user/hand/right/input/a/click 1;input 7750 7753 /user/hand/right/input/a/click 1;input 7775 7778 /user/hand/right/input/a/click 1;input 7800 7803 /user/hand/right/input/a/click 1;input 7825 7828 /user/hand/right/input/a/click 1;input 7850 7853 /user/hand/right/input/a/click 1;input 7875 7878 /user/hand/right/input/a/click 1;input 7900 7903 /user/hand/right/input/a/click 1;input 7925 7928 /user/hand/right/input/a/click 1;input 7950 7953 /user/hand/right/input/a/click 1;input 7975 7978 /user/hand/right/input/a/click 1;input 8000 8003 /user/hand/right/input/a/click 1;input 8025 8028 /user/hand/right/input/a/click 1;input 8050 8053 /user/hand/right/input/a/click 1;input 8075 8078 /user/hand/right/input/a/click 1;input 8100 8103 /user/hand/right/input/a/click 1;input 8125 8128 /user/hand/right/input/a/click 1;input 8150 8153 /user/hand/right/input/a/click 1;input 8175 8178 /user/hand/right/input/a/click 1;input 8200 8203 /user/hand/right/input/a/click 1;input 8225 8228 /user/hand/right/input/a/click 1;input 8250 8253 /user/hand/right/input/a/click 1;input 8275 8278 /user/hand/right/input/a/click 1;input 8300 8303 /user/hand/right/input/a/click 1;input 8325 8328 /user/hand/right/input/a/click 1;input 8350 8353 /user/hand/right/input/a/click 1;input 8375 8378 /user/hand/right/input/a/click 1;input 8400 8403 /user/hand/right/input/a/click 1;input 8425 8428 /user/hand/right/input/a/click 1;input 8450 8453 /user/hand/right/input/a/click 1;input 8475 8478 /user/hand/right/input/a/click 1;input 8500 8503 /user/hand/right/input/a/click 1;input 8525 8528 /user/hand/right/input/a/click 1;input 8550 8553 /user/hand/right/input/a/click 1;input 8575 8578 /user/hand/right/input/a/click 1;input 8600 8603 /user/hand/right/input/a/click 1;input 8625 8628 /user/hand/right/input/a/click 1;input 8650 8653 /user/hand/right/input/a/click 1;input 8675 8678 /user/hand/right/input/a/click 1;input 8700 8703 /user/hand/right/input/a/click 1;input 8725 8728 /user/hand/right/input/a/click 1;input 8750 8753 /user/hand/right/input/a/click 1;input 8775 8778 /user/hand/right/input/a/click 1;input 8800 8803 /user/hand/right/input/a/click 1;input 8825 8828 /user/hand/right/input/a/click 1;input 8850 8853 /user/hand/right/input/a/click 1;input 8875 8878 /user/hand/right/input/a/click 1;input 8900 8903 /user/hand/right/input/a/click 1;input 8925 8928 /user/hand/right/input/a/click 1;input 8950 8953 /user/hand/right/input/a/click 1;input 8975 8978 /user/hand/right/input/a/click 1;input 9000 9003 /user/hand/right/input/a/click 1;input 9025 9028 /user/hand/right/input/a/click 1;input 9050 9053 /user/hand/right/input/a/click 1;input 9075 9078 /user/hand/right/input/a/click 1;input 9100 9103 /user/hand/right/input/a/click 1;input 9125 9128 /user/hand/right/input/a/click 1;input 9150 9153 /user/hand/right/input/a/click 1;input 9175 9178 /user/hand/right/input/a/click 1;input 9200 9203 /user/hand/right/input/a/click 1;input 9225 9228 /user/hand/right/input/a/click 1;input 9250 9253 /user/hand/right/input/a/click 1;input 9275 9278 /user/hand/right/input/a/click 1;input 9300 9303 /user/hand/right/input/a/click 1;input 9325 9328 /user/hand/right/input/a/click 1;input 9350 9353 /user/hand/right/input/a/click 1;input 9375 9378 /user/hand/right/input/a/click 1;input 9400 9403 /user/hand/right/input/a/click 1;input 9425 9428 /user/hand/right/input/a/click 1;input 9450 9453 /user/hand/right/input/a/click 1;input 9475 9478 /user/hand/right/input/a/click 1;input 9500 9503 /user/hand/right/input/a/click 1;input 9525 9528 /user/hand/right/input/a/click 1;input 9550 9553 /user/hand/right/input/a/click 1;input 9575 9578 /user/hand/right/input/a/click 1;input 9600 9603 /user/hand/right/input/a/click 1;input 9625 9628 /user/hand/right/input/a/click 1;input 9650 9653 /user/hand/right/input/a/click 1;input 9675 9678 /user/hand/right/input/a/click 1;input 9700 9703 /user/hand/right/input/a/click 1;input 9725 9728 /user/hand/right/input/a/click 1;input 9750 9753 /user/hand/right/input/a/click 1;input 9775 9778 /user/hand/right/input/a/click 1;input 9800 9803 /user/hand/right/input/a/click 1;input 9825 9828 /user/hand/right/input/a/click 1;input 9850 9853 /user/hand/right/input/a/click 1;input 9875 9878 /user/hand/right/input/a/click 1;input 9900 9903 /user/hand/right/input/a/click 1;input 9925 9928 /user/hand/right/input/a/click 1;input 9950 9953 /user/hand/right/input/a/click 1;input 9975 9978 /user/hand/right/input/a/click 1;input 10000 10003 /user/hand/right/input/a/click 1;input 10025 10028 /user/hand/right/input/a/click 1;input 10050 10053 /user/hand/right/input/a/click 1;input 10075 10078 /user/hand/right/input/a/click 1;input 10100 10103 /user/hand/right/input/a/click 1;input 10125 10128 /user/hand/right/input/a/click 1;input 10150 10153 /user/hand/right/input/a/click 1;input 10175 10178 /user/hand/right/input/a/click 1;input 10200 10203 /user/hand/right/input/a/click 1;input 10225 10228 /user/hand/right/input/a/click 1;input 10250 10253 /user/hand/right/input/a/click 1;input 10275 10278 /user/hand/right/input/a/click 1;input 10300 10303 /user/hand/right/input/a/click 1;input 10325 10328 /user/hand/right/input/a/click 1;input 10350 10353 /user/hand/right/input/a/click 1;input 10375 10378 /user/hand/right/input/a/click 1;input 10400 10403 /user/hand/right/input/a/click 1;input 10425 10428 /user/hand/right/input/a/click 1;input 10450 10453 /user/hand/right/input/a/click 1;input 10475 10478 /user/hand/right/input/a/click 1;input 10500 10503 /user/hand/right/input/a/click 1;input 10525 10528 /user/hand/right/input/a/click 1;input 10550 10553 /user/hand/right/input/a/click 1;input 10575 10578 /user/hand/right/input/a/click 1;input 10600 10603 /user/hand/right/input/a/click 1;input 10625 10628 /user/hand/right/input/a/click 1;input 10650 10653 /user/hand/right/input/a/click 1;input 10675 10678 /user/hand/right/input/a/click 1;input 10700 10703 /user/hand/right/input/a/click 1;input 10725 10728 /user/hand/right/input/a/click 1;input 10750 10753 /user/hand/right/input/a/click 1;input 10775 10778 /user/hand/right/input/a/click 1;input 10800 10803 /user/hand/right/input/a/click 1;input 10825 10828 /user/hand/right/input/a/click 1;input 10850 10853 /user/hand/right/input/a/click 1;input 10875 10878 /user/hand/right/input/a/click 1;input 10900 10903 /user/hand/right/input/a/click 1;input 10925 10928 /user/hand/right/input/a/click 1;input 10950 10953 /user/hand/right/input/a/click 1;input 10975 10978 /user/hand/right/input/a/click 1;input 11000 11003 /user/hand/right/input/a/click 1;input 11025 11028 /user/hand/right/input/a/click 1;input 11050 11053 /user/hand/right/input/a/click 1;input 11075 11078 /user/hand/right/input/a/click 1;input 11100 11103 /user/hand/right/input/a/click 1;input 11125 11128 /user/hand/right/input/a/click 1;input 11150 11153 /user/hand/right/input/a/click 1;input 11175 11178 /user/hand/right/input/a/click 1;input 11200 11203 /user/hand/right/input/a/click 1;input 11225 11228 /user/hand/right/input/a/click 1;input 11250 11253 /user/hand/right/input/a/click 1;input 11275 11278 /user/hand/right/input/a/click 1;input 11300 11303 /user/hand/right/input/a/click 1;input 11325 11328 /user/hand/right/input/a/click 1;input 11350 11353 /user/hand/right/input/a/click 1;input 11375 11378 /user/hand/right/input/a/click 1;input 11400 11403 /user/hand/right/input/a/click 1;input 11425 11428 /user/hand/right/input/a/click 1;input 11450 11453 /user/hand/right/input/a/click 1;input 11475 11478 /user/hand/right/input/a/click 1;input 11500 11503 /user/hand/right/input/a/click 1;input 11525 11528 /user/hand/right/input/a/click 1;input 11550 11553 /user/hand/right/input/a/click 1;input 11575 11578 /user/hand/right/input/a/click 1;input 11600 11603 /user/hand/right/input/a/click 1;input 11625 11628 /user/hand/right/input/a/click 1;input 11650 11653 /user/hand/right/input/a/click 1;input 11675 11678 /user/hand/right/input/a/click 1;input 11700 11703 /user/hand/right/input/a/click 1;input 11725 11728 /user/hand/right/input/a/click 1;input 11750 11753 /user/hand/right/input/a/click 1;input 11775 11778 /user/hand/right/input/a/click 1;input 11800 11803 /user/hand/right/input/a/click 1;input 11825 11828 /user/hand/right/input/a/click 1;input 11850 11853 /user/hand/right/input/a/click 1;input 11875 11878 /user/hand/right/input/a/click 1;input 11900 11903 /user/hand/right/input/a/click 1;input 11925 11928 /user/hand/right/input/a/click 1;input 11950 11953 /user/hand/right/input/a/click 1;input 11975 11978 /user/hand/right/input/a/click 1;dump 1700;dump 2300", "PT_XRTEST_OUT": "."},
        "route": ["lisadate_wait", "lisadate_vr"],
        "expect": [
            r"lisa date: started \(the input script\)",
            r"vr: virtual screen",
            r"lisa date: choice c1 -> c1_a",
            r"lisa date: choice c12 -> c12_a",
            r"lisa date: ending good",
            r"lisa date: returned to f160 \(ending good\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Coffee with Lisa's selfie (the GOOD route's photo): four dates, each to the photo mode's panel, its values set (her
    # expression and tilt, yours), the shot, the polaroid developing and developed, her reaction to the faces and the polaroid's
    # PNG (PT_PHOTO_DIR, the work folder unless set). The date's music is the owner's music.ogg with its loop
    "lisadate_selfie": {
        "optional": True,
        "start_floor": "f160",
        "frames": 16000,
        # the polaroid is a crop of a drawn frame (the date's UI knows its rectangle only once drawn): a headless run that renders
        # just the last frame and the frames of a screenshot never gets one, so every frame is rendered
        "args": ["--audio-offline", "--render-all"],
        "route": ["lisadate_wait", "lisadate_selfie"],
        "expect": [
            r"lisa date: music music\.ogg, 48000 Hz, 2 channels, 135\.6 s",
            r"lisa date: music loop 33\.553 s to 86\.583 s, crossfade 1\.5 s, outro to 135\.638 s",
            r"lisa date: selfie panel up \(with the player's model\)",
            r"lisa date: photo \(selfie: Lisa smile tilt \+1, you frown tilt \+1\)",
            r"lisa date: selfie developed, saving the polaroid",
            r"lisa date: polaroid saved as pt_\d{8}_\d{6}(_\d+)?\.png",
            r"lisa date: line selfie_she_smiles \(lisa\)",
            r"lisa date: ending good",
            r"lisa date: photo \(selfie: Lisa big grin tilt \+2, you big grin tilt \+2\)",
            r"lisa date: polaroid saved as pt_",
            r"lisa date: line selfie_both_grin \(lisa\)",
            r"lisa date: photo \(selfie: Lisa grumpy tilt \+0, you grumpy tilt -1\)",
            r"lisa date: polaroid saved as pt_",
            r"lisa date: line selfie_both_grumpy \(lisa\)",
            r"lisa date: photo \(selfie: Lisa grumpy tilt -1, you smile tilt \+1\)",
            r"lisa date: polaroid saved as pt_",
            r"lisa date: line selfie_you_smile \(lisa\)",
            r"lisa date: returned to f160 \(ending good\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Coffee with Lisa's music (the owner's track: the intro, the loop A 33.553 s to B 86.583 s with its 1.5 s crossfaded seam, the
    # outro at the ending): the date waits past the first seam, then its GOOD ending lets the pass run past B to the end of the file
    # and the ending card stays until the music has ended by itself (tools/lisa_music_seam_check.py measures an --audio-capture)
    "lisadate_music": {
        "optional": True,
        "start_floor": "f160",
        "frames": 22000,
        "args": ["--audio-offline"],
        "route": ["lisadate_wait", "lisadate_music"],
        "expect": [
            r"lisa date: music loop 33\.553 s to 86\.583 s, crossfade 1\.5 s, outro to 135\.638 s",
            r"lisa date: started \(the input script\)",
            r"lisa date: music music \(playing id [1-9]",
            r"lisa date: music seam 1 \(B 86\.583 s to A 33\.553 s\)",
            r"lisa date: ending good",
            r"lisa date: music released at \d+\.\d+ s of the file, to its end in \d+\.\d s",
            r"lisa date: music outro, the pass runs past B 86\.583 s to the end",
            r"lisa date: music ended \(135\.638 s, the end of its file\)",
            r"lisa date: finished \(good\)",
            r"lisa date: returned to f160 \(ending good\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # VR only (python tools/walkthrough.py --vr lisadate_vr_selfie): the selfie's panel on the virtual screen, driven by the
    # controllers as a pad (the left stick is the D-pad, A moves down the rows and takes the photo, X takes it from any row); the
    # layers of frames 1600, 1950 and 2400 are written
    "lisadate_vr_selfie": {
        "optional": True,
        "vr_only": True,
        "start_floor": "f160",
        "frames": 9000,
        "args": ["--audio-offline"],
        "env": {"PT_XRTEST_SCRIPT": ";".join(
            ["input 1620 1626 /user/hand/left/input/thumbstick 1 0", "input 1660 1663 /user/hand/right/input/a/click 1",
             "input 1700 1706 /user/hand/left/input/thumbstick 1 0", "input 1740 1743 /user/hand/right/input/a/click 1",
             "input 1780 1786 /user/hand/left/input/thumbstick -1 0", "input 1820 1823 /user/hand/right/input/a/click 1",
             "input 1860 1866 /user/hand/left/input/thumbstick 1 0", "input 1900 1903 /user/hand/right/input/a/click 1",
             "input 2060 2063 /user/hand/right/input/a/click 1", "dump 1600", "dump 1950", "dump 2400"]),
            "PT_XRTEST_OUT": "."},
        "route": ["lisadate_wait", "lisadate_vr_selfie"],
        "expect": [
            r"lisa date: started \(the input script\)",
            r"vr: virtual screen",
            r"lisa date: selfie panel up \(with the player's model\)",
            r"lisa date: selfie Lisa expression \+1",
            r"lisa date: photo \(selfie: Lisa smile tilt \+1, you frown tilt \+1\)",
            r"lisa date: polaroid saved as pt_",
            r"lisa date: line selfie_she_smiles \(lisa\)",
            r"lisa date: returned to f160 \(ending good\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Coffee with Lisa's button prompts for each device (python tools/walkthrough.py --shots DIR lisadate_prompts_keyboard ...): a
    # line, the choices, the hold to leave bar and the selfie's panel with the glyphs of the keyboard (the bound keys), a PlayStation,
    # an Xbox, a Nintendo and a Steam pad (Button prompts: Automatic), and an Xbox pad with Button prompts set to Nintendo
    **{f"lisadate_prompts_{name}": {
        "optional": True,
        "pad": kind is not None,
        "start_floor": "f160",
        "frames": 9000,
        "args": ["--audio-offline"],
        "work_files": {"pt.ini": f"[input]\nprompt_style = {style}\n".encode()},
        "route": ["lisadate_wait"] + ([f"lisadate_pad_{kind}"] if kind else []) + ["lisadate_prompts"],
        "expect": [
            r"lisa date: started \(the input script\)",
            r"lisa date: choice c1 up",
            r"lisa date: selfie panel up",
            r"lisa date: returned to f160 \(ending good\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    } for name, kind, style in [("keyboard", None, 0), ("playstation", "ps5", 0), ("xbox", "xbox", 0), ("nintendo", "switch", 0),
                                ("steam", "steam", 0), ("xbox_as_nintendo", "xbox", 3)]},
    # the date in each of the game's languages (the system language's defaults on a first boot): its text from text/<code>.txt in a
    # font that covers it, captured for the fit of its lines (python tools/walkthrough.py --shots DIR lisadate_lang_deu ...)
    **{f"lisadate_lang_{code}": {
        "optional": True,
        "save": True,
        "start_floor": "f160",
        "frames": 9000,
        "env": {"PT_SYSTEM_LANGUAGE": tag},
        "route": ["lisadate_wait", "lisadate_lang"],
        "expect": [
            r"lisa date: started \(the input script\)",
            r"lisa date: ending true",
            r"lisa date: returned to f160 \(ending true\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    } for code, tag in [('fra', 'fr-FR'), ('deu', 'de-DE'), ('spa', 'es-ES'), ('jpn', 'ja-JP'), ('ita', 'it-IT'), ('por', 'pt-PT'), ('tur', 'tr-TR'), ('zhs', 'zh-CN'), ('ara', 'ar-SA'), ('rus', 'ru-RU'), ('ukr', 'uk-UA'), ('ces', 'cs-CZ'), ('pol', 'pl-PL')]},
    # Coffee with Lisa's hints (tier 4) by `slisarushes`: the light after rush 1, 2 and 3, the glint, the zoom's heartbeat and pink
    # edges let go at 9.4 s; then the lit loop left by its exit door: the tint and glint fade with a sad note, the count stays 3
    "lisadate_hints": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f160",
        "frames": 20000,
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "args": ["--audio-offline", "--f160-light", "white"],
        "work_files": {"pt.ini": b"[accessibility]\nsound_captions = 1\n"},
        "route": ["start", "lisadate_hints", "walk", "exit", "lisadate_hints_after"],
        "expect": [
            r"lisa date: rush count set to 1/3",
            r"lisa date: hint light tint 0 -> 1",
            r"lisa date: hint sting 1/3",
            r"sound caption: pc_cap_soft_notes",
            r"lisa date: hint light tint 1 -> 2",
            r"lisa date: hint sting 2/3",
            r"lisa date: hint glint lit on the portrait \(rush 3/3\)",
            r"lisa date: hint light tint 2 -> 3",
            r"lisa date: hint sting 3/3",
            r"lisa date: portrait zoom armed",
            r"lisa date: hint heartbeat 1 at",
            r"sound caption: pc_cap_soft_heartbeat",
            r"lisa date: hint heartbeat 8 at",
            r"lisa date: portrait zoom released after 9\.\d s",
            r"lisa date: hint heartbeat and pink edges fade",
            r"expect ok frame \d+: lisa date not running",
            r"floor: NextFloor f160 -> f160",
            r"lisa date: hint glint and tint fade \(NextFloor f160 -> f160\), a sad note follows",
            r"lisa date: hint light tint 3 -> 0",
            r"lisa date: hint sad note",
            r"sound caption: pc_cap_sad_note",
            r"expect ok frame \d+: lisa rushes 3",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Coffee with Lisa's zoom on the armed portrait with the view shaking at the edge of its area (the held zoom's view, the zoom
    # itself widening the portrait's place in it): the portrait stays armed, the heartbeat runs on and the date starts
    "lisadate_portrait_edge": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f160",
        "frames": 6000,
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "args": ["--audio-offline"],
        "route": ["start", "lisadate_portrait_edge"],
        "expect": [
            r"lisa date: rush count set to 3/3",
            r"lisa date: portrait zoom armed",
            r"lisa date: hint heartbeat 1 at",
            r"lisa date: portrait held 10 s, the date starts",
            r"lisa date: started \(the portrait zoom\)",
            r"expect ok frame \d+: lisa date running",
            r"input script: \d+ expectations, 0 failed",
        ],
        "forbid": [(r"lisa date: portrait zoom armed", r"lisa date: portrait held 10 s", r"lisa date: portrait (zoom released|left the view)")],
    },
    # the game's own f160 light colour (--f160-light red, the roll's red): the tint steps aside and the light is the game's red
    "lisadate_hints_aside": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f160",
        "frames": 8000,
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "args": ["--audio-offline", "--f160-light", "red"],
        "route": ["start", "lisadate_hints_aside"],
        "expect": [
            r"floor: f160 handy light roll 1 -> \(1 0 0\)",
            r"lisa date: hint light tint 0 -> 2",
            r"lisa date: hint light tint steps aside for the game's own colour \(1 0 0\)",
            r"lisa date: hint sting 2/3",
        ],
    },
    # below tier 4 Lisa's rushes on f160 are not counted and her portrait gives the plain zoom: nothing starts
    "lisadate_none": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f160",
        "frames": 30000,
        "env": {"PT_GAMEPLUS_TIER": "3"},
        "args": ["--audio-offline"],
        "route": ["start", "lisadate_portrait", "lisadate_rush", "lisadate_none"],
        "expect": [
            r"ocho: LogicControl 4",
            r"expect ok frame \d+: lisa rushes 0",
            r"expect ok frame \d+: lisa date not running",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the base game before any finish: none of Game+ (maze B's tub is empty)
    "gameplus_none": {
        "optional": True,
        "start_floor": "f060",
        "frames": 20000,
        "route": LATE[:LATE.index("mazeB") + 1] + ["gameplus_mazeb_off"],
        "expect": [
            r"floor: NextFloor f100 -> f110",
            r"expect ok frame \d+: game\+ tub lisa not shown",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the Game+ setting (pt.ini [game] game_plus, the Extras row): the row is not there before the first finish
    "gameplus_row_hidden": {
        "pad": True,
        "start_floor": "f010",
        "frames": 1500,
        "route": ["start", "gameplus_row_hidden"],
        "expect": [
            r"expect ok frame \d+: PC setting pc_third_person = 0",
            r"expect ok frame \d+: PC setting pc_game_plus absent",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # after a finish the row is there (On); switched off in a loop it changes nothing until the next floor, which then plays the
    # original game; switched on again it waits for the floor after, which goes on with the saved tier
    "gameplus_toggle": {
        "optional": True,
        "gameplus": True,
        "pad": True,
        "start_floor": "f010",
        "frames": 14000,
        "route": ["gameplus_mark", "start", "gameplus_row_off", "f005", "exit", "gameplus_tier0", "walk", "gameplus_row_on", "exit",
                  "gameplus_tier1_back"],
        "expect": [
            r"game\+: finished flag set \(save tag\)",
            r"expect ok frame \d+: PC setting pc_game_plus = 1",
            r"expect ok frame \d+: PC setting pc_game_plus = 0",
            r"floor: NextFloor f005 -> f020",
            r"game\+: setting off from this loop",
            r"expect ok frame \d+: game\+ tier 0",
            r"expect ok frame \d+: PC setting pc_game_plus = 1",
            r"floor: NextFloor f020 -> f030",
            r"game\+: setting on from this loop \(tier 1\)",
            r"expect ok frame \d+: game\+ tier 1",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Game+ off in a finished profile (two finishes, the tier forced to 4): maze B plays as the original game, the second finish
    # is not counted toward the tier, no bathtub Lisa
    "gameplus_off": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f060",
        "frames": 20000,
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "work_files": {"pt.ini": b"[game]\ngame_plus = 0\n"},
        "args": ["--settings", "pt.ini"],
        "route": ["gameplus_mark", "gameplus_mark"] + LATE[:LATE.index("mazeB") + 1] + ["gameplus_off_mazeb"],
        "expect": [
            r"game\+: finished flag set \(save tag\)",
            r"game\+: finish 1 counted",
            r"game\+: finish not counted toward the tier \(Game\+ off\), finishes stay 1",
            r"floor: NextFloor f100 -> f110",
            r"expect ok frame \d+: game\+ tier 0",
            r"expect ok frame \d+: game\+ tub lisa not shown",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # Game+ off (tier 4 forced): Lisa's rushes on f160 are not counted and her portrait gives the plain zoom
    "gameplus_off_lisa": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f160",
        "frames": 30000,
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "work_files": {"pt.ini": b"[game]\ngame_plus = 0\n"},
        "args": ["--audio-offline", "--settings", "pt.ini"],
        "route": ["start", "lisadate_portrait", "lisadate_rush", "gameplus_off_lisa"],
        "expect": [
            r"ocho: LogicControl 4",
            r"expect ok frame \d+: game\+ tier 0",
            r"expect ok frame \d+: lisa rushes 0",
            r"expect ok frame \d+: lisa date not running",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # speedrun mode (Extras > Speedrun timer, game time): a full run from the first boot to the ending with a split per loop, the
    # results page after the credits, "Return to menu" (the restart with the option screen first) and the next run's start;
    # the records go next to the pt.ini
    "speedrun_full": {
        "optional": True,
        "pad": True,
        "frames": 120000,
        "work_files": {"pt.ini": b"[extras]\nspeedrun = 2\n"},
        "args": ["--settings", "pt.ini", "--options-menu"],
        "route": ["speedrun_boot"] + FULL + ["speedrun_menu"],
        "expect": [
            r"speedrun: timer on \(game time\)",
            r"speedrun: run started on f000 \(full run\)",
            r"speedrun: split 1 f000 ",
            r"speedrun: split 2 f010 ",
            r"speedrun: split \d+ f050 ",
            r"speedrun: split \d+ f050#2 ",
            r"speedrun: split \d+ f120 ",
            r"speedrun: split \d+ f160 ",
            r"speedrun: run finished: real \S+ game \S+, new personal best",
            r"ui: option menu opened \(pause, PC settings\)",
            r"speedrun: return to menu",
            r"controller: step 30 -> 17",
            r"ui: option menu opened \(first boot\)",
            r"speedrun: run started on f000 \(full run\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # speedrun mode from a mid-game start (no records), real time, with the LiveSplit client on against a stub server: the
    # results page's "Explore outside" starts the street walk
    "speedrun_street": {
        "optional": True,
        "pad": True,
        "start_floor": "f120",
        "frames": 40000,
        "work_files": {"pt.ini": b"[extras]\nspeedrun = 1\nlivesplit = 1\nlivesplit_port = {livesplit_port}\n"},
        "args": ["--settings", "pt.ini"],
        "livesplit": ["reset", "initgametime", "starttimer", "pausegametime", "setgametime", "split"],
        "route": ["f120", "start", "f160", "exit", "speedrun_street"],
        "expect": [
            r"speedrun: timer on \(real time\)",
            r"livesplit: server client on, 127\.0\.0\.1:\d+",
            r"livesplit: connected to 127\.0\.0\.1:\d+",
            r"speedrun: run started on f120 \(not from the start",
            r"speedrun: split 1 f120 ",
            r"speedrun: run finished: real \S+ game \S+ \(not from the start\)",
            r"street: offer answered: walk the street",
            r"street: walk started",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the street walk's "Return to the house" on the option screen (shown only in the walk, H twice): the ending's restart follows
    "street_return": {
        "pad": True,
        "start_floor": "f010",
        "frames": 9000,
        "route": ["street_return"],
        "expect": [
            r"ui: option menu opened \(pause\)",
            r"street: walk started",
            r"ui: option menu opened \(pause\)",
            r"ui: return to the house selected from Options",
            r"street: left by the player",
            r"controller: step 31 -> 29",
            r"controller: step 30 -> 17",
            r"controller: ChangeGameStep\(StartGame\)",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the survival night (Game+ tier 4) in the street walk, played by PT_SURVIVAL_AUTO: its first wave cleared, the second begun
    "survival_night": {
        "start_floor": "f010",
        "frames": 5200,
        "route": ["survival_night"],
        "env": {"PT_SURVIVAL_AUTO": "1", "PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"street: walk started",
            r"survival: the first person arm made from the player's model",
            r"survival: started",
            r"survival: wave 1 \(",
            r"survival: doll rises",
            r"survival: doll down",
            r"survival: wave 1 cleared",
            r"survival: wave 2 \(",
        ],
    },
    # the survival night's death and retry: the auto player never shoots, the first wave brings it down, the fall plays and fire
    # starts the night again from its countdown
    "survival_death": {
        "start_floor": "f010",
        "frames": 5200,
        "route": ["survival_death"],
        "env": {"PT_SURVIVAL_AUTO": "1", "PT_SURVIVAL_PASSIVE": "1", "PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"survival: started \(best wave 0, difficulty 1, from the first wave\)",
            r"survival: wave 1 \(",
            r"survival: player hit for 14",
            r"survival: the player fell on wave 1",
            r"survival: retry \(difficulty 1\)",
            # (a retry stands the player up on the street at once, the wake-up is not played again)
            r"survival: looks applied",
            r"survival: wave 1 \(",
        ],
    },
    # the loop browser's survival night: picked at the nightmare difficulty, it starts in the start room on f000 (the player standing
    # up from the floor); its door opens onto the street, the walk starts on the house's porch with its door shutting and the night
    # begins
    "survival_browser": {
        "start_floor": "f010",
        "frames": 5000,
        "route": ["survival_browser"],
        "env": {"PT_SURVIVAL_AUTO": "1"},
        "expect": [
            r"loop browser: the survival night \(difficulty 3\)",
            r"loop browser: the night's start room, its door opens onto the street",
            r"loop browser: the start room's door opens onto the street",
            r"loop browser: out of the start room onto the street",
            r"street: the house's door closing behind the player",
            r"street: walk started",
            r"survival: started \(best wave 0, difficulty 3, from the first wave\)",
            r"street: the house's door shut behind the player",
            r"survival: wave 1 \(",
        ],
    },
    # the night's radio offer by the mouse: a click on the first card before the cards have come in takes nothing, the pointer
    # lights a card, a click on the third takes that one (GameUi::UpdateSurvivalOffer)
    "survival_offer_pointer": {
        "start_floor": "f010",
        "frames": 5200,
        "route": ["survival_offer_pointer"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"survival: started",
            r"survival: the radio offers ",
            r"survival: the offer's card 3 taken with the pointer",
            r"survival: took pc_boon_",
            r"survival: the card's click let go",
            r"survival: round fired",
        ],
        "forbid": [(r"survival: the offer's card 3 taken", r"survival: the card's click let go", r"survival: round fired")],
    },
    # the dark's count: past the grace on wave 9 it begins, the light stops it, run out it brings Lisa
    "survival_dark": {
        "start_floor": "f010",
        "frames": 7000,
        "route": ["survival_dark"],
        "env": {"PT_GAMEPLUS_TIER": "4", "PT_SURVIVAL_EVENTS": "0"},
        "expect": [
            r"survival: wave 9 \(",
            r"survival: the dark's count begins at 8 on wave 9",
            r"survival: the dark's count stopped at",
            r"survival: the dark's count begins at 8 on wave 9",
            r"survival: the dark's count ran out on wave 9",
            r"survival: Lisa caught the player on wave 9",
        ],
    },
    # the night's end screen out of Lisa's catch in each language (python tools/walkthrough.py --shots DIR survival_end_tur ...): the
    # survival_dark route, its pictures every 25 frames through the catch and the screen after it
    **{f"survival_end_{code}": {
        "optional": True,
        "start_floor": "f010",
        "frames": 7000,
        "route": ["survival_dark"],
        "env": {"PT_GAMEPLUS_TIER": "4", "PT_SURVIVAL_EVENTS": "0", "PT_SYSTEM_LANGUAGE": tag},
        "expect": [
            r"survival: the dark's count ran out on wave 9",
            r"survival: Lisa caught the player on wave 9",
            r"survival: the player fell on wave 9 \(.*, cause 6, out of the catch\)",
        ],
    } for code, tag in [('fra', 'fr-FR'), ('deu', 'de-DE'), ('spa', 'es-ES'), ('jpn', 'ja-JP'), ('ita', 'it-IT'), ('por', 'pt-PT'), ('tur', 'tr-TR'), ('zhs', 'zh-CN'), ('ara', 'ar-SA'), ('rus', 'ru-RU'), ('ukr', 'uk-UA'), ('ces', 'cs-CZ'), ('pol', 'pl-PL')]},
    # the radio's supply on wave 12: the lamp named, the boxes taken
    "survival_supply": {
        "start_floor": "f010",
        "frames": 5400,
        "route": ["survival_supply"],
        "env": {"PT_GAMEPLUS_TIER": "4", "PT_SURVIVAL_EVENTS": "0"},
        "expect": [
            r"survival: the radio names the lamp at",
            r"survival: wave 12 \(",
            r"survival: the lamp's supply reached",
        ],
    },
    # third person: the handgun stays in the right hand through a sprint and its stop (no draw again when the run ends)
    "survival_third_run": {
        "start_floor": "f010",
        "frames": 2400,
        "route": ["survival_third_run"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"survival: started",
            r"third person: on",
        ],
    },
    # third person: the crosshair held on a doll's head 7 m off and to the side, the handgun's rounds land under it (on the head)
    "survival_third_aim": {
        "start_floor": "f010",
        "frames": 2600,
        "route": ["survival_third_aim"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"survival: started",
            r"third person: on",
            r"survival: third person shot lands 0\.[0-4]\d deg off the crosshair \(the head, ",
            r"survival: doll down",
        ],
        "forbid": [(r"survival: act aimat", r"survival: doll down", r"survival: third person shot lands (?!0\.[0-4]\d deg off the crosshair \(the head)")],
    },
    # third person: the offer stops the walk, Shift tapped at a walk, the shotgun let down from the top of the view (pictures)
    "survival_third_moves": {
        "start_floor": "f010",
        "frames": 3400,
        "args": ["--virtual-pads"],
        "route": ["survival_third_moves"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"third person: on",
            r"survival: the radio offers ",
            r"survival: took pc_boon_",
            r"survival: sprint \(",
        ],
    },
    # third person: W + Shift on and off over and over with every weapon; measured by tools/survival_guns/motion_check.py
    "survival_third_transitions": {
        "start_floor": "f010",
        "frames": 6500,
        "args": ["--virtual-pads"],
        "route": ["survival_third_transitions"],
        "env": {"PT_GAMEPLUS_TIER": "4", "PT_BODY_MOTION": "1"},
        "expect": [
            r"third person: on",
            r"survival: act weapon=0",
            r"input script seq frame",
        ],
    },
    # third person: A and D over and over, alone, with W, with W and Shift, overlapping; measured by tools/survival_guns/motion_check.py
    "survival_third_strafe": {
        "start_floor": "f010",
        "frames": 4000,
        "args": ["--virtual-pads"],
        "route": ["survival_third_strafe"],
        "env": {"PT_GAMEPLUS_TIER": "4", "PT_BODY_MOTION": "1"},
        "expect": [
            r"third person: on",
            r"survival: sprint \(",
            r"input script seq frame",
        ],
    },
    # third person: the aim, a shot, a reload, a blow and the change of weapon at rest and at a walk; measured the same
    "survival_third_actions": {
        "start_floor": "f010",
        "frames": 6500,
        "args": ["--virtual-pads"],
        "route": ["survival_third_actions"],
        "env": {"PT_GAMEPLUS_TIER": "4", "PT_BODY_MOTION": "1"},
        "expect": [
            r"third person: on",
            r"survival: act weapon=3",
            r"input script seq frame",
        ],
    },
    # third person: the offer coming on a running and a walking body, paced as a window (measured, pictures on every tick)
    "survival_third_offer_stop": {
        "start_floor": "f010",
        "frames": 6000,
        "args": ["--virtual-pads", "--display-rate", "170"],
        "route": ["survival_third_offer_stop"],
        "env": {"PT_GAMEPLUS_TIER": "4", "PT_BODY_MOTION": "1"},
        "expect": [
            r"third person: on",
            r"survival: the radio offers ",
            r"survival: took pc_boon_",
            r"survival: the radio offers ",
            r"survival: took pc_boon_",
        ],
    },
    # a real mouse button on a card of the radio's offer, held as a click is: no round fired until it is let go and pressed again
    "survival_offer_click": {
        "start_floor": "f010",
        "frames": 2600,
        "args": ["--virtual-pads"],
        "route": ["survival_offer_click"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"survival: the radio offers ",
            r"survival: the offer's card 3 taken with the pointer",
            r"survival: took pc_boon_",
            r"input script seq frame",
            r"survival: round fired",
        ],
        "forbid": [(r"survival: the offer's card 3 taken", r"input script seq frame", r"survival: round fired")],
    },
    # the radio's offer taken by the keyboard's 2 (the mouse is the cards' under the offer, and the keys were read with the look alone)
    "survival_offer_keys": {
        "start_floor": "f010",
        "frames": 2600,
        "args": ["--virtual-pads"],
        "route": ["survival_offer_keys"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"survival: the radio offers ",
            r"survival: took pc_boon_\w+ \(rank \d+, card 2\)",
        ],
        "forbid": [(r"survival: the radio offers ", r"zzz never", r"taken with the pointer|: unknown name")],
    },
    # third person: a long gun taken off the road, carried through a run and its stop, weapons changed (pictures)
    "survival_third_draw": {
        "start_floor": "f010",
        "frames": 2600,
        "args": ["--virtual-pads"],
        "route": ["survival_third_draw"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"third person: on",
            r"survival: sprint \(",
        ],
    },
    # a round fired with a body on the player lands (the old box took none from inside it)
    "survival_point_blank": {
        "start_floor": "f010",
        "frames": 5200,
        "route": ["survival_point_blank"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"survival: started",
            r"survival: doll rises",
            r"survival: doll down",
        ],
    },
    # the menu takes input from its first frames: a second Esc closes it, F10 right after Esc reaches the PC page
    "menu_setin_input": {
        "start_floor": "f010",
        "frames": 900,
        "args": ["--virtual-pads"],
        "route": ["menu_setin_input"],
        "expect": [
            r"ui: option menu opened \(pause\)",
            r"ui: option menu closed",
            r"ui: option menu opened \(pause\)",
            r"ui: option menu page PC settings",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the night's loop browser entry (Game::NightUnlocked): four finishes (Game+ tier 4, not forced) do not show it; the run played at
    # tier 4 unlocks it the moment its last loop's door opens onto the night, before the street, and pt.ini keeps it at once
    "survival_unlock": {
        "optional": True,
        "gameplus": True,
        "start_floor": "f120",
        "frames": 40000,
        "save": True,
        "route": ["f120", "start", "f160", "survival_unlock_mark", "exit", "survival_unlock_after"],
        "expect": [
            r"game\+: finish 4 counted",
            r"expect ok frame \d+: survival night locked",
            r"loop browser: the survival night's entry unlocks \(the last loop's door opens onto the night\)",
            r"ending: Game\+ tier 4: the exit door opens onto the street night",
            r"loop browser: the survival night's unlock written to pt\.ini",
            r"ending: Game\+ tier 4 finished, no credits",
            r"survival: started",
            r"expect ok frame \d+: survival night unlocked, kept",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # a run at Game+ tier 4 reaching the ending (the loop browser's): the ending notes its tier, the street question's walk is the
    # survival night's, and taking it starts the night
    "survival_offer": {
        "start_floor": "f010",
        "frames": 7200,
        "route": ["survival_offer"],
        "env": {"PT_GAMEPLUS_TIER": "4"},
        "expect": [
            r"controller: step 28 -> 33",
            r"street: credits finished, walk offered",
            r"expect ok .*: PC settings cursor on pc_street_survival",
            r"street: offer answered: walk the street",
            r"street: walk started",
            r"survival: started",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the street question of the end of the credits (soffer): the page, its rows and the cursor; --shots keeps the bar's place
    "street_offer": {
        "start_floor": "f010",
        "frames": 1200,
        "route": ["street_offer"],
        "expect": [
            r"street: credits finished, walk offered",
            r"expect ok .*: menu open",
            r"expect ok .*: PC settings cursor on pc_street_walk",
            r"expect ok .*: PC settings cursor on pc_street_restart",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the port's credits page after the ending's credits (scredits) and the PC settings corner line; --shots keeps every card
    "port_credits": {
        "start_floor": "f010",
        "frames": 5500,
        "route": ["port_credits"],
        "expect": [
            r"expect ok .*: menu open",
            r"credits: port credits page started",
            r"credits: port credits page finished",
            r"street: credits finished, walk offered",
            r"controller: step 33 -> 32",
            r"expect ok .*: PC settings cursor on pc_street_walk",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the page in Japanese, skipped with a confirm press: the street question follows at once
    "port_credits_skip": {
        "optional": True,
        "start_floor": "f010",
        "frames": 4000,
        "route": ["port_credits_skip"],
        "expect": [
            r"credits: port credits page started",
            r"credits: port credits page skipped",
            r"street: credits finished, walk offered",
            r"expect ok .*: PC settings cursor on pc_street_walk",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "notices": {
        "start_floor": "f010",
        "frames": 16000,
        "args": ["--virtual-pads", "--audio-offline", "--fake-update", "99.0.0"],
        "route": ["notices"],
        "expect": [
            r"notice: queued pc_update_available",
            r"notice: shown pc_update_available",
            r"notice: ended pc_update_available.* after 7\.[5-9] s",
            r"notice: shown pc_gameplus_notice.2",
            r"notice: ended pc_gameplus_notice.2 after 7\.[5-9] s",
        ],
    },
    # a notice shown once: pushed in the start room, faded out there, then the door into the loop (a stage change) and the hallway; the
    # log has it queued, shown and ended once and never again after it ended
    "notifyrepeat": {
        "start_floor": "f010",
        "frames": 12000,
        "args": ["--virtual-pads", "--audio-offline"],
        "route": ["notifyrepeat", "f005", "exit"],
        "expect": [
            r"notice: queued pc_gameplus_notice.2",
            r"notice: shown pc_gameplus_notice.2",
            r"notice: ended pc_gameplus_notice.2 after 7\.[5-9] s",
            r"floor: NextFloor f010 -> f005",
            r"floor: NextFloor f005 -> f020",
        ],
        # after the line ended in the start room no notice is queued or shown again, through the door and the next loop
        "forbid": [(r"notice: ended", r"zzz never", r"notice: (queued|shown|cut)")],
    },
    # the same with the door opened 2.5 s after the line appeared (before its time is out): the door's demo takes the banner away, the line
    # is cut there (it was on screen long enough) and the loop shows nothing
    "notifyrepeat_early": {
        "start_floor": "f010",
        "frames": 12000,
        "args": ["--virtual-pads", "--audio-offline"],
        "route": ["notifyrepeat_early", "f005", "exit"],
        "expect": [
            r"notice: shown pc_gameplus_notice.2",
            r"notice: cut pc_gameplus_notice.2 after [2-6]\.\d s",
            r"floor: NextFloor f005 -> f020",
        ],
        "forbid": [(r"notice: cut", r"zzz never", r"notice: (queued|shown|ended)"), (r"notice: shown", r"zzz never", r"notice: ended")],
    },
    "notifyrepeat_all": {
        "start_floor": "f010",
        "frames": 20000,
        "args": ["--virtual-pads", "--audio-offline", "--fake-update", "99.0.0"],
        "route": ["notifyrepeat_all", "f005", "exit"],
        "expect": [
            r"notice: shown pc_update_available",
            r"notice: ended pc_update_available.* after 7\.[5-9] s",
            r"notice: shown pc_gameplus_notice.2",
            r"notice: ended pc_gameplus_notice.2 after 7\.[5-9] s",
            r"floor: NextFloor f010 -> f005",
            r"floor: NextFloor f005 -> f020",
        ],
        "forbid": [(r"notice: ended pc_gameplus_notice.2", r"zzz never", r"notice: (queued|shown|cut)")],
    },
    "prompts": {
        "pad": True,
        # the VR mode shows the controllers' A, B, X and Y on every prompt (docs/vr.md), so the device prompts are not its
        "not_vr": True,
        "start_floor": "f010",
        "frames": 4000,
        "route": ["prompts"],
        "expect": [
            r"prompt textures: the fallen frame's R3 shows mouse3 \(keyboard\)",
            r"ui: option menu prompts show keyboard buttons",
            r"ui: option menu page PC settings",
            r"input: button prompts follow PlayStation",
            r"prompt textures: the fallen frame's R3 shows the data's button \(PlayStation\)",
            r"ui: option menu prompts show PlayStation buttons",
            r"ui: option menu prompts show keyboard buttons",
            r"ui: option menu prompts show PlayStation buttons",
            r"input: button prompts follow Xbox",
            r"prompt textures: the fallen frame's R3 shows rs \(Xbox\)",
            r"ui: option menu prompts show Xbox buttons",
            r"ui: option menu page PC settings",
            r"ui: option menu prompts show PlayStation buttons",
            r"prompt textures: the fallen frame's R3 shows stick \(Nintendo\)",
            r"ui: option menu prompts show Nintendo buttons",
            r"ui: option menu prompts show keyboard buttons",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "gouge": {
        "pad": True,
        "start_floor": "f060",
        "frames": 14000,
        "route": ["start", "f050a", "exit", "gouge", "exit"],
        "expect": [
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"input script frame \d+: mouse button right down",
            r"input script frame \d+: key X tapped",
            r"nazo: XMark cleared",
            r"message: controller Clearf050",
            r"input script frame \d+: mouse button right up",
            r"floor: NextFloor f050 -> f070",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the original gouges with its interact button (`Action`): an Act key does it as the X key does (issue #7)
    "gougee": {
        "pad": True,
        "start_floor": "f060",
        "frames": 14000,
        "route": ["start", "f050a", "exit", "gouge_e", "exit"],
        "expect": [
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"input script frame \d+: mouse button right down",
            r"input script frame \d+: key E tapped",
            r"nazo: XMark cleared",
            r"message: controller Clearf050",
            r"floor: NextFloor f050 -> f070",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "gougemouse": {
        "pad": True,
        "start_floor": "f060",
        "frames": 14000,
        "route": ["start", "f050a", "exit", "gouge_mouse", "exit"],
        "expect": [
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"input script frame \d+: mouse button right down",
            r"input script frame \d+: mouse button left tapped",
            r"nazo: XMark cleared",
            r"message: controller Clearf050",
            r"floor: NextFloor f050 -> f070",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "lisalook": {
        "start_floor": "f060",
        "frames": 16000,
        "route": ["start", "f050a", "exit", "lisalook"],
        "expect": [
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"ocho: LogicControl 3",
            r"expect ok .*: ocho look-back phase 0",
            r"ocho: look back armed",
            r"ocho: kill",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # the start room door into f050 loop 1 with the stick held: Lisa steps during the door demo, no breath before its release
    "lisadoor": {
        "start_floor": "f060",
        "frames": 2400,
        "demo_rate": 1.0,
        "args": ["--audio-offline"],
        "route": ["lisa_door"],
        "expect": [
            r"floor: NextFloor f060 -> f050 \(loop 1\)",
            r"ocho: LogicControl 2",
            r"sound: id 0x7b136101 on Lisa",
            r"demo: gc_p00_010 releases the player",
        ],
        "forbid": [(r"ocho: LogicControl 2", r"demo: gc_p00_010 releases the player", r"sound: id 0xc910062e on Lisa")],
    },
    "lisadoor_release": {
        "start_floor": "f060",
        "frames": 12000,
        "args": ["--audio-offline"],
        "route": ["lisa_door_release"],
        "expect": [
            r"floor: NextFloor f050 -> f050 \(loop 2\)",
            r"sound: id 0x7b136101 on Lisa",
            r"expect ok frame \d+: event 0x7b136101 alive",
            r"demo: gc_p00_010 releases the player",
            r"expect ok frame \d+: event 0x7b136101 not alive",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    "fakecrash": {
        "optional": True,
        "start_floor": "f110",
        "frames": 9000,
        "demo_rate": 1.0,
        "args": ["--audio-offline"],
        "route": ["start", "fakecrash"],
        "expect": [
            r"floor: NextFloor f110 -> f120",
            r"trap: ShDemoExec gc_p02_080",
            r"demo: gc_p02_060 effect .*fx_sh_viwdis01_s1.vfx at frame 0 sound Play_sfx_bug_loop_01",
            r"sound: Play_sfx_bug_loop_01 at",
            r"sound: Play_sfx_bug_loop_01 stopped over 0.10 s \(curve 6\)",
            r"demo: gc_p02_080 plays gc_p02_070 at frame 3540",
            r"demo: gc_p02_080 plays gc_p02_100 at frame 3600",
            r"controller: DisableOption, pause menu blocked \(no save\)",
            r"demo: gc_p02_080 UI text at frame 5401",
            r"ui: bug screen [0-6] of 7",
            r"archive: bug:sh_bug_[1-7] reached in play",
            r"expect ok .*: bug picture side fill on",
            r"controller: ChangeGameStep\(Endf120\)",
            r"demo: gc_p02_500 playing",
        ],
    },
    # the fake crash's picture opens its Museum exhibit (crash_4, sh_bug_5, the red one; case 3 forced) and no other, with the
    # Museum's locks on and the game finished once
    "archive_crash": {
        "optional": True,
        "start_floor": "f110",
        "frames": 9000,
        "demo_rate": 1.0,
        "save": True,
        "work_files": {"pt.ini": b"[progress]\ngame_finished = 1\narchive_backfill = 1\n"},
        "args": ["--audio-offline", "--release-locks", "--bug-screen", "3"],
        "route": ["start", "fakecrash", "archive_crash"],
        "expect": [
            r"ui: bug screen 3 of 7 \(sh_bug_5",
            r"archive: bug:sh_bug_5 reached in play",
            r"expect ok .*: archive entry crash_4 open",
            r"input script: 5 expectations, 0 failed",
        ],
    },
    # the true end heard through the recognizer: the microphone is a SAPI "Jack" in room noise every 6 s (PT_VOICE_INPUT),
    # the route says nothing. The first f160 pass walks through (the word alone does not end it), the second does the
    # steps and the wait; checks the utterance, the condition and the end (formats/voice.md)
    "voice": {
        "optional": True,
        "start_floor": "f060",
        "frames": 40000,
        "route": LATE[:LATE.index("f160")] + ["walk", "exit", "f160_mic", "exit"],
        "voice_input": "Jack",
        "expect": [
            r"floor: NextFloor f120 -> f160",
            r"nazo: voice recognition on",
            r"voice: whisper.cpp CPU code ggml-cpu-",
            r"floor: NextFloor f160 -> f160 \(loop 2\)",
            r"voice: utterance \d+ .*: the word",
            r"nazo: TrueEnd heard Jack",
            r"nazo: TrueEnd cleared",
            r"floor: NextFloor f160 -> ending",
        ],
    },
    # the photo mode and the free camera during the ending's outro (issue #51)
    "pickup_light": {
        "optional": True,
        "start_floor": "f040",
        "frames": 9000,
        "demo_rate": 1.0,
        "route": ["start", "pickup_light"],
        "expect": [
            r"trap: ShDemoExec gc_p00_030",
        ],
    },
    "ending_photo": {
        "optional": True,
        "start_floor": "f120",
        "frames": 40000,
        "route": ["f120", "start", "f160", "exit", "ending_photo"],
        "expect": [
            r"floor: NextFloor f160 -> ending",
            r"extras: photo mode on",
            r"extras: free camera on",
        ],
    },
    # the ending on a GPU with 2 GB of video memory (PT_VRAM_BUDGET_MB, docs/rendering.md 12.27): the hallway's models and textures
    # go when the ending loads alone, and the ending plays to its end and its credits
    "ending_lowvram": {
        "start_floor": "f120",
        "frames": 40000,
        "env": {"PT_VRAM_BUDGET_MB": "2048"},
        "route": ["f120", "start", "f160", "exit"],
        "expect": [
            r"(vulkan|d3d12): test video memory budget 2048 MB",
            r"floor: NextFloor f160 -> ending",
            r"models: ending load: released \d+ models \(\d+ MB of meshes\) and \d+ textures",
            r"controller: step 27 -> 28",
            r"demo: gc_p06_010_final finished at frame 8902",
            r"controller: step 28 -> 33",
        ],
    },
    # the ending's hold to skip (port addition): a tap does nothing, a 1.5 s hold of Esc ends the ending demo as its end would;
    # the finish counts (Game+, the loop browser) and the port's credits page follows
    "ending_skip": {
        "start_floor": "f120",
        "frames": 40000,
        "save": True,
        "args": ["--virtual-pads", "--audio-offline"],
        "route": ["f120", "start", "f160", "exit", "ending_skip"],
        "expect": [
            r"floor: NextFloor f160 -> ending",
            r"models: ending load: released \d+ models",
            r"controller: step 27 -> 28",
            r"ending: skip hold released after 0\.[0-9]+ s",
            r"ending: skipped by the player after a 1\.[5-9][0-9]* s hold",
            r"demo: gc_p06_010_final skipped to its end at frame [0-9.]+ of 8902 \(fade out, colour of its end\)",
            r"demo: gc_p06_010_final finished at frame [0-9.]+ \(skipped\)",
            r"message: gc_p06_010_final Finish -> ShDemoScript_FinishEnding",
            r"loop browser: finished flag set",
            r"game\+: finish 1 counted",
            r"credits: port credits page started",
            r"controller: step 28 -> 33",
            r"expect ok .*: menu closed",
            r"input script: \d+ expectations, 0 failed",
        ],
    },
    # a relaunch at f120 through the start room's door (a 1.0.3 player's 29.6 s freeze, 1.0.4): f160 listens, so the speech runtime
    # loads at the door, and on the player's PC its libraries took 41 s to load (PT_VOICE_LOAD_STALL_MS holds the loader lock here
    # as they did). A thread started meanwhile cannot run, and the collision rebuild at the start room's swap started and joined
    # its own: the main thread waited for the loads. The worlds after the swap are built during the door's demo on the worker pool
    # and taken at the swap, the same triangles; no frame from the door until the loads end may take 100 ms
    "door_freeze": {
        "start_floor": "f120",
        "frames": 3000,
        "demo_rate": 1.0,
        "args": ["--render-all"],
        "voice_input": "",
        "env": {"PT_VOICE_INPUT_LIVE": "1", "PT_VOICE_LOAD_STALL_MS": "8000", "PT_STATUS_LOG": "1"},
        "route": ["start", "door_freeze"],
        "expect": [
            r"floor: NextFloor f120 -> f160",
            r"stage: collision after the swap of next into current built on a worker",
            r"stage: _unload_ \(id 2, .*pt14_start\.fpk\) unloaded",
            r"stage: collision 9493 triangles, 370 material surfaces, 182293 line check triangles \(swap of next into current, worker",
            r"voice: whisper\.cpp CPU code ggml-cpu-",
        ],
        "forbid": [
            (r"stage: next \(id 3\) activated", r"voice: whisper\.cpp CPU code", r"frame spike: (\d{4,}|[1-9]\d\d)\.\d ms"),
        ],
    },
    "ending": {
        "optional": True,
        "start_floor": "f060",
        "frames": 52000,
        "route": LATE + ["ending"],
        "expect": [
            r"floor: NextFloor f160 -> ending",
            r"controller: ChangeGameStep\(GotoEnding\)",
            r"stage: .*ending.fpk",
        ],
    },
}


# Field of view option (PC settings > Accessibility, +30 degrees): the widened picture must change nothing the game decides from what the
# player sees (docs/gameplay.md 14.10). The HELLO letters' in-view test, the bag's talk (a zoom with the bag in the middle half of the
# screen) and the peephole theater (zoomed, so the original's view) run again with the option at its widest, expecting the same log lines
for _base in ("hello", "gameover", "peephole"):
    SCENARIOS[_base + "_fov"] = {**SCENARIOS[_base], "work_files": {"pt.ini": b"[accessibility]\nfield_of_view = 30\n"}}


def load_route(names, shots=None):
    lines = []
    for name in names:
        for line in (ROUTES / f"{name}.txt").read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "{shots}" in line:
                if not shots:
                    continue
                line = line.replace("{shots}", Path(shots).as_posix())
            lines.append(f"700 {line}")
    return "\n".join(lines) + "\n"


def voice_input(work, text):
    """A Windows SAPI voice saying text once in 6 s of quiet room noise (about -70 dBFS), 16 kHz mono; the noise alone for no text."""
    import random
    import struct
    import wave
    word = ()
    if text:
        spoken = work / "spoken.wav"
        script = ("Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
                  "$f = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, 16, 1); "
                  f"$s.SetOutputToWaveFile('{spoken}', $f); $s.Speak('{text}'); $s.Dispose()")
        subprocess.run(["powershell", "-NoProfile", "-Command", script], check=True, stdout=subprocess.DEVNULL)
        with wave.open(str(spoken)) as w:
            word = struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes()))
    rng = random.Random(1)
    lead = 2 * 16000
    samples = [int(rng.gauss(0, 10)) for _ in range(6 * 16000)]
    for i, s in enumerate(word):
        samples[lead + i] = max(-32768, min(32767, samples[lead + i] + s // 4))
    path = work / "microphone.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    return path


class LiveSplitStub:
    """A stand-in for LiveSplit's TCP server on a free loopback port: it keeps every line the game sends."""

    def __init__(self):
        import socket
        import threading
        self.lines = []
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.server.settimeout(0.5)
        self.port = self.server.getsockname()[1]
        self.stopped = False
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        buffer = b""
        connection = None
        while not self.stopped:
            try:
                if connection is None:
                    connection, _ = self.server.accept()
                    connection.settimeout(0.5)
                data = connection.recv(4096)
                if not data:
                    connection = None
                    continue
                buffer += data
                while b"\r\n" in buffer:
                    line, buffer = buffer.split(b"\r\n", 1)
                    self.lines.append(line.decode("utf-8", "replace"))
            except OSError:
                continue

    def stop(self):
        self.stopped = True
        self.thread.join(timeout=2)
        self.server.close()


def updates_itself(exe) -> bool:
    """an install (pt-install-manifest.txt) or a portable copy (pt-files.json) with its pt-updater beside the game: the release
    package's layouts, where an offered version is asked about at the start (docs/updates.md, "In-game update")"""
    folder = Path(exe).resolve().parent
    updater = (folder / "pt-updater.exe").is_file() or (folder / "pt-updater").is_file()
    return updater and ((folder / "pt-install-manifest.txt").is_file() or (folder / "pt-files.json").is_file())


def run(name, scenario, exe, game, demo_rate, keep, shots, pad=False, pt_args=(), vr=False):
    work = Path(tempfile.mkdtemp(prefix=f"pt_walk_{name}_"))
    livesplit = LiveSplitStub() if scenario.get("livesplit") else None
    route = work / "route.txt"
    shot_dir = None
    if shots:
        shot_dir = Path(shots).resolve() / name
        shot_dir.mkdir(parents=True, exist_ok=True)
    pad = pad or scenario.get("pad", False)
    header = "" if scenario.get("pad") or not pad else "700 pattach 0 ps5\n700 pbot 0\n"
    # a scenario that offers a newer version (--fake-update) run in a copy that updates itself answers the start's question first
    routes = list(scenario["route"])
    asked = "--fake-update" in scenario.get("args", []) and updates_itself(exe)
    if asked:
        routes.insert(0, "update_question_no")
    route.write_text(header + load_route(routes, shot_dir), encoding="utf-8")
    # a scenario with "save" gets a save store of its own in the work folder (Reset progress needs one)
    save = ["--save-dir", str(work / "save")] if scenario.get("save") else ["--no-save"]
    if scenario.get("save"):
        (work / "save").mkdir()
        # "save_files": slot files written before the run (the save dialogs' fixtures)
        for file_name, data in scenario.get("save_files", {}).items():
            (work / "save" / file_name).write_bytes(data)
    # "work_files": files written into the work folder before the run (a pt.ini for --settings)
    for file_name, data in scenario.get("work_files", {}).items():
        if livesplit:
            data = data.replace(b"{livesplit_port}", str(livesplit.port).encode())
        (work / file_name).write_bytes(data)
    cmd = [str(exe), "--game", str(game), "--headless", *save, "--frames", str(scenario["frames"]), "--demo-rate",
           str(scenario.get("demo_rate", demo_rate)),
           "--input-script", str(route), "--width", "1280", "--height", "720", "--seed", str(scenario.get("seed", 1))]
    if scenario.get("start_floor"):
        cmd += ["--start-floor", scenario["start_floor"]]
    if pad:
        cmd += ["--virtual-pads", "--audio-offline"] + scenario.get("extra", [])
    cmd += scenario.get("args", [])
    if vr:
        cmd += ["--vr"]
    cmd += list(pt_args)
    # the runs use the default options, never the user's own pt.ini (its upscaler or language would change the run)
    if "--settings" not in cmd:
        cmd += ["--settings", str(work / "pt.ini")]
    # the option defaults follow the system language (Options_StaticInit 0x91D7B0); the routes expect an English system
    env = {**os.environ, "PT_SYSTEM_LANGUAGE": "en-US", **{k: v.replace("{work}", str(work)) for k, v in scenario.get("env", {}).items()}}
    # the photo mode's (and the date's selfie's) PNGs into the work folder, never the user's Pictures
    env.setdefault("PT_PHOTO_DIR", str(work / "photos"))
    if vr:
        # the VR mode against the headless test runtime next to the executable (docs/vr.md), at a small view size to keep the
        # two eyes a frame quick; its log goes to the work folder
        env["XR_RUNTIME_JSON"] = str(Path(exe).resolve().parent / "xr_test_runtime" / "pt_xr_test_runtime.json")
        env["PT_XRTEST_LOG"] = str(work / "runtime.log")
        env.setdefault("PT_XRTEST_VIEW", "512x552")
    if scenario.get("voice_input") is not None:
        env["PT_VOICE_INPUT"] = str(voice_input(work, scenario["voice_input"]))
    started = time.time()
    subprocess.run(cmd, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800, env=env)
    log = (work / "pt.log").read_text(encoding="utf-8", errors="replace").splitlines()
    # "relaunch": the game started again on the same save folder after the first run quit, with its own frames and route; the
    # expectations run over both logs, the second after the line "walkthrough: relaunch"
    if scenario.get("relaunch"):
        relaunch = scenario["relaunch"]
        (work / "pt.log").replace(work / "pt_first.log")
        route.write_text(header + load_route(relaunch["route"], shot_dir), encoding="utf-8")
        again = list(cmd)
        again[again.index("--frames") + 1] = str(relaunch["frames"])
        subprocess.run(again, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800, env=env)
        log += ["walkthrough: relaunch"] + (work / "pt.log").read_text(encoding="utf-8", errors="replace").splitlines()
    elapsed = time.time() - started
    position = 0
    results = []
    # such a copy asks instead of showing the update notice (main.cpp, the release check's answer): the question and its Not now in
    # place of the notice's milestones
    expect = list(scenario["expect"])
    if asked:
        expect = [r"update: asking at the start to update to", r"update: the player chose Not now at the start's question",
                  *(pattern for pattern in expect if "pc_update_available" not in pattern)]
    for pattern in expect:
        regex = re.compile(pattern)
        found = None
        for i in range(position, len(log)):
            if regex.search(log[i]):
                found = i
                break
        results.append((pattern, found))
        if found is not None:
            position = found + 1
    # "forbid": (from, to, pattern): no line matching pattern between the first line matching from and the next matching to
    for start, end, pattern in scenario.get("forbid", []):
        first = next((i for i, line in enumerate(log) if re.search(start, line)), None)
        last = next((i for i in range(first, len(log)) if re.search(end, log[i])), len(log)) if first is not None else None
        hit = next((log[i] for i in range(first, last) if re.search(pattern, log[i])), None) if first is not None else None
        results.append((f"no {pattern} between {start} and {end}" + (f" ({hit.strip()})" if hit else ""),
                        first if first is not None and hit is None else None))
    if livesplit:
        livesplit.stop()
        # "livesplit": commands the stub server must have received, in order
        at = 0
        for command in scenario["livesplit"]:
            hit = next((i for i in range(at, len(livesplit.lines)) if livesplit.lines[i].split(" ")[0] == command), None)
            results.append((f"livesplit received {command}", hit))
            if hit is not None:
                at = hit + 1
    # a "gameplus" scenario needs a build with PT_GAMEPLUS; in one without it is skipped
    if scenario.get("gameplus") and any("game+: not in this build" in line for line in log):
        results = [("skipped: PT_GAMEPLUS is off in this build", -1)]
    stop = next((line for line in reversed(log) if "game: stopped" in line), "")
    if not keep:
        # the game's files can stay locked for a moment after it exits (the scan of an antivirus, a closing handle);
        # a leftover temp folder must not abort the remaining scenarios
        for attempt in range(10):
            try:
                shutil.rmtree(work)
                break
            except OSError:
                time.sleep(0.5)
    return name, results, elapsed, stop, work


def main():
    parser = argparse.ArgumentParser(description="Headless walkthrough of the game with scripted routes; checks the log for each milestone in order")
    parser.add_argument("scenarios", nargs="*", help=f"scenarios to run (default: all but ending): {', '.join(SCENARIOS)}")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--game", default=str(DEFAULT_GAME))
    parser.add_argument("--demo-rate", type=float, default=20.0)
    parser.add_argument("--keep", action="store_true", help="keep the work folder with pt.log and the route")
    parser.add_argument("--shots", help="folder for the screenshots the routes take at fixed spots (none without it)")
    parser.add_argument("--pad", action="store_true", help="drive the routes through a virtual gamepad only (the pad scenario always does)")
    parser.add_argument("--vr", action="store_true", help="run in the VR mode against the headless OpenXR test runtime (docs/vr.md)")
    parser.add_argument("--pt-arg", action="append", default=[], metavar="ARG",
                        help="an option passed on to every pt.exe (repeatable; --pt-arg=--third-person plays the routes in the third person view)")
    parser.add_argument("--jobs", type=int, default=1, help="scenarios run at once")
    args = parser.parse_args()
    names = args.scenarios or [name for name, scenario in SCENARIOS.items()
                               if not scenario.get("optional") and not (args.vr and scenario.get("not_vr"))]
    for name in names:
        if name not in SCENARIOS:
            sys.exit(f"unknown scenario {name}")
        if SCENARIOS[name].get("vr_only") and not args.vr:
            sys.exit(f"{name} runs in the VR mode only: add --vr")
    failed = 0
    with ThreadPoolExecutor(max_workers=max(1, min(args.jobs, len(names)))) as pool:
        jobs = [pool.submit(run, name, SCENARIOS[name], Path(args.exe), Path(args.game), args.demo_rate, args.keep, args.shots, args.pad,
                            args.pt_arg, args.vr)
                for name in names]
        for job in jobs:
            name, results, elapsed, stop, work = job.result()
            passed = sum(1 for _, found in results if found is not None)
            status = "PASS" if passed == len(results) else "FAIL"
            if results and results[0][1] == -1:
                status = "SKIP"
            failed += status == "FAIL"
            print(f"{status} {name}: {passed}/{len(results)} milestones, {elapsed:.1f} s")
            for pattern, found in results:
                if found is None:
                    print(f"    missing: {pattern}")
            if stop:
                print(f"    {stop.split('info', 1)[-1].strip()}")
            if args.keep:
                print(f"    log: {work / 'pt.log'}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
