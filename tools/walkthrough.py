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

SCENARIOS = {
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
            r"ui: option menu sent PhotoOption",
            r"ui: subliminal image 1",
            r"ui: option menu closed",
            r"input script: \d+ expectations, 0 failed",
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
            r"ui: option menu sent PhotoOption",
            r"ui: subliminal image 1",
            r"ui: option menu closed",
            r"input script: \d+ expectations, 0 failed",
        ],
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
    "archive": {
        # the Archive (Extras, gameplay.md 14.7): a cutscene opened from its row plays in the theater's own session and the menu comes
        # back on that row, then a model shows in the theater's viewer and is left; the player's session is unchanged and play goes on
        "start_floor": "f005",
        "frames": 14000,
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
    # one scenario per Game+ tier (PT_GAMEPLUS_TIER forces the tier; each checks its tier's content and the lower tiers'):
    # tier 1, the hallway: the bathtub Lisa in the tub of the bathroom whose sink holds the baby
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
            r"game\+: radio line Play_radio_voice_08 after the broadcast \(f050 pass 1",
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
            r"controller: ChangeGameStep\(Endf120\)",
            r"demo: gc_p02_500 playing",
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
    """A Windows SAPI voice saying text once in 6 s of quiet room noise (about -70 dBFS), 16 kHz mono."""
    import random
    import struct
    import wave
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
    route.write_text(header + load_route(scenario["route"], shot_dir), encoding="utf-8")
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
    env = {**os.environ, "PT_SYSTEM_LANGUAGE": "en-US", **scenario.get("env", {})}
    if vr:
        # the VR mode against the headless test runtime next to the executable (docs/vr.md), at a small view size to keep the
        # two eyes a frame quick; its log goes to the work folder
        env["XR_RUNTIME_JSON"] = str(Path(exe).resolve().parent / "xr_test_runtime" / "pt_xr_test_runtime.json")
        env["PT_XRTEST_LOG"] = str(work / "runtime.log")
        env.setdefault("PT_XRTEST_VIEW", "512x552")
    if scenario.get("voice_input"):
        env["PT_VOICE_INPUT"] = str(voice_input(work, scenario["voice_input"]))
    started = time.time()
    subprocess.run(cmd, cwd=work, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800, env=env)
    elapsed = time.time() - started
    log = (work / "pt.log").read_text(encoding="utf-8", errors="replace").splitlines()
    position = 0
    results = []
    for pattern in scenario["expect"]:
        regex = re.compile(pattern)
        found = None
        for i in range(position, len(log)):
            if regex.search(log[i]):
                found = i
                break
        results.append((pattern, found))
        if found is not None:
            position = found + 1
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
