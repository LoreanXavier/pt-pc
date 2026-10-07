import argparse
import itertools
import json
import re
from pathlib import Path

import numpy as np

FNV_PRIME = 16777619
FNV_OFFSET = 2166136261
FNV_PRIME_INVERSE = pow(FNV_PRIME, -1, 1 << 32)
MASK = 0xFFFFFFFF

CATEGORIES = ["", "sfx", "voice", "bgm", "bg", "radio", "sys", "plr", "ene", "man", "gmk", "gimmick", "se", "amb",
              "demo", "vib", "motion", "pad", "tel", "phone", "fx", "env", "evt", "event", "cut", "ui"]

EXTRA_WORDS = """
all any baby bag bang bath bathroom beep bell bgm blood body bottle break breath breathing broken bug bulb buzz call
camera caster ceiling chain chair chandelier change chant child chime cockroach cord corridor count countdown crack
crash creak creaking cry crying cursor cut dad dark daughter demo digital dog door doorknob down drain drip drop
drone eat electric ending enemy enter exit eye face fall family fan father fear fetus fire flash flashlight flesh
flicker floor fly foot footstep fridge gasp get ghost girl glass glitch grab groan gun hall hallway hand hang head
heart heartbeat herald hit horror hum impact kill kitchen knob knock lamp laugh laughing light lisa lobby lock look
loop loud low mask meat message mimicry mirror moan monster mother mouth music news noise number ocho open painting
passage peephole phone photo piano picture pipe push radio rain rainfall refri refrigerator religion ring ringing
rise room rumble run scream shake shock shot shout sigh signal silence sink slam slow son sound speaker spirit stab
stair stairs start static step sting stinger stop storm street subliminal surprise talk tap tension theater thunder
tick toilet tone touch turn tv umbilical unlock unrest voice wake walk wall water whisper wind window wood woman
worm yell zoom
""".split()

SUFFIXES = ([""] + ["_%02d" % i for i in range(1, 13)] + ["%02d" % i for i in range(1, 13)] +
            ["_%d" % i for i in range(1, 10)] + ["%d" % i for i in range(1, 10)] +
            ["_lp", "_loop", "_s", "_m", "_l", "_2d", "_3d", "_st", "_ed", "_a", "_b", "_c", "_on", "_off", "_in",
             "_out", "_01_lp", "_02_lp", "_01_2d", "_02_2d", "_lp_01", "_s_01", "_m_01", "_l_01"] +
            ["_f%03d" % i for i in range(0, 180, 10)])

COMPLEMENTARY_ACTIONS = [
    (("Play",), ("Stop", "Break"), [(("play_", ""), ("stop_", ""))]),
    (("SetBusVolume", "SetVolume", "Mute"), ("ResetBusVolume", "ResetVolume", "Unmute"),
     [(("set_", ""), ("reset_", "")), (("", "_off"), ("", "_on")), (("mute_", ""), ("unmute_", "")),
      (("set_", "_off"), ("set_", "_on")), (("set_", "_mute"), ("reset_", "_mute")), (("", "_mute"), ("", "_unmute"))]),
    (("Pause",), ("Resume",), [(("pause_", ""), ("resume_", ""))]),
]

RTPC_EVENT_ENDINGS = ["", "_on", "_off", "_0", "_1", "_min", "_max", "_reset", "_high", "_low", "_default", "_start",
                      "_end", "_in", "_out", "_up", "_down"]


def fnv(text: str, state: int = FNV_OFFSET) -> int:
    for byte in text.lower().encode("utf-8"):
        state = ((state * FNV_PRIME) & MASK) ^ byte
    return state


def unfnv(state: int, text: str) -> int:
    for byte in reversed(text.lower().encode("utf-8")):
        state = ((state ^ byte) * FNV_PRIME_INVERSE) & MASK
    return state


def extend(states: np.ndarray, text: str) -> np.ndarray:
    out = states.astype(np.uint64)
    for byte in text.lower().encode("utf-8"):
        out = ((out * FNV_PRIME) & MASK) ^ byte
    return out


def encode_matrix(texts):
    lengths = np.array([len(t) for t in texts], dtype=np.int64)
    matrix = np.zeros((len(texts), int(lengths.max()) if len(texts) else 1), dtype=np.uint64)
    for row, text in enumerate(texts):
        raw = text.lower().encode("utf-8")
        matrix[row, :len(raw)] = np.frombuffer(raw, dtype=np.uint8)
    return matrix, lengths


def unfnv_many(target: int, matrix: np.ndarray, lengths: np.ndarray) -> np.ndarray:
    states = np.full(len(lengths), target, dtype=np.uint64)
    for column in range(matrix.shape[1] - 1, -1, -1):
        active = lengths > column
        states[active] = ((states[active] ^ matrix[active, column]) * FNV_PRIME_INVERSE) & MASK
    return states


class Heads:
    def __init__(self, vocabulary, depth):
        self.parts = [[""] + [c + "_" for c in CATEGORIES if c]]
        for _ in range(depth):
            self.parts.append([""] + [t + "_" for t in vocabulary])
        self.sizes = [len(part) for part in self.parts]

    def text(self, flat_index: int) -> str:
        pieces = []
        for part, size in zip(self.parts, self.sizes):
            pieces.append(part[flat_index % size])
            flat_index //= size
        return "".join(pieces)

    def states(self, start_state: int) -> np.ndarray:
        states = np.array([start_state], dtype=np.uint64)
        for part in self.parts:
            states = np.concatenate([extend(states, piece) for piece in part])
        return states


class HeadIndex:
    def __init__(self, heads: Heads, befores):
        forward = [heads.states(fnv(before)) for before in befores]
        keys = forward[0] << np.uint64(32) | forward[1]
        self.order = np.argsort(keys, kind="stable")
        self.keys = keys[self.order]

    def match(self, keys: np.ndarray):
        positions = np.searchsorted(self.keys, keys)
        positions = np.minimum(positions, len(self.keys) - 1)
        hits = np.nonzero(self.keys[positions] == keys)[0]
        for tail_index in hits:
            position = int(positions[tail_index])
            while position < len(self.keys) and self.keys[position] == keys[tail_index]:
                yield int(tail_index), int(self.order[position])
                position += 1


class Tails:
    def __init__(self, vocabulary, suffixes):
        self.texts = sorted({token + suffix for token in [""] + list(vocabulary) for suffix in suffixes} - {""})
        self.matrix, self.lengths = encode_matrix(self.texts)


def solve(templates, heads: Heads, index: HeadIndex, tails: Tails):
    backward = [unfnv_many(unfnv(target, after), tails.matrix, tails.lengths) for _, after, target in templates]
    keys = backward[0] << np.uint64(32) | backward[1]
    return sorted({heads.text(head) + tails.texts[tail] for tail, head in index.match(keys)})


def vocabulary_from_candidates(path: Path, limit: int):
    counts = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        for token in re.split(r"[^A-Za-z0-9]+", line):
            token = token.lower()
            if 2 <= len(token) <= 12 and not token.isdigit():
                counts[token] = counts.get(token, 0) + 1
    ranked = sorted(counts, key=lambda t: (-counts[t], t))
    return ranked[:limit]


def english_words(limit: int):
    try:
        from wordfreq import top_n_list
    except ImportError:
        return []
    return [w for w in top_n_list("en", limit) if re.fullmatch(r"[a-z]{2,12}", w)]


def base_vocabulary(names, labels):
    tokens = set(EXTRA_WORDS)
    for entry in names["resolved"].values():
        tokens.update(t.lower() for t in re.split(r"[_\s]+", entry["name"]) if t)
    for label in labels:
        tokens.update(t.lower() for t in re.split(r"[_\s]+", label) if t)
    tokens.update("abcdefghijklmnopqrstuvwxyz")
    tokens.update("%02d" % i for i in range(0, 21))
    tokens.update("f%03d" % i for i in range(0, 180, 10))
    tokens.discard("")
    return sorted(tokens)


def build_constraints(events, unresolved_ids):
    by_target = {}
    for event in events["events"]:
        for action in event.get("actions", []):
            target = (action.get("target") or {}).get("id")
            if target:
                by_target.setdefault(target, []).append((event, action["type"]))
    constraints = []
    for target, users in by_target.items():
        for first_kinds, second_kinds, patterns in COMPLEMENTARY_ACTIONS:
            firsts = {e["id"] for e, kind in users if kind.startswith(first_kinds) and e["id"] in unresolved_ids}
            seconds = {e["id"] for e, kind in users if kind.startswith(second_kinds) and e["id"] in unresolved_ids}
            for first, second in itertools.product(sorted(firsts), sorted(seconds)):
                if first == second:
                    continue
                for (before_a, after_a), (before_b, after_b) in patterns:
                    constraints.append({"kind": "%s/%s pair on one target" % (first_kinds[0], second_kinds[0]),
                                        "templates": [(before_a, after_a, first), (before_b, after_b, second)],
                                        "names": [before_a.capitalize() + "{}" + after_a,
                                                  before_b.capitalize() + "{}" + after_b]})
    for event in events["events"]:
        if event["id"] not in unresolved_ids:
            continue
        for action in event.get("actions", []):
            state = action.get("state") or {}
            if action["type"] == "SetState" and state.get("id") in unresolved_ids:
                for before in ("set_state_", "set_"):
                    constraints.append({"kind": "set state event and its state",
                                        "templates": [(before, "", event["id"]), ("", "", state["id"])],
                                        "names": [before.capitalize() + "{}", "{}"]})
            target = action.get("target") or {}
            if action["type"].startswith("SetGameParameter") and target.get("id") in unresolved_ids:
                for ending in RTPC_EVENT_ENDINGS:
                    for before in ("set_rtpc_", "set_"):
                        constraints.append({"kind": "set game parameter event and its parameter",
                                            "templates": [(before, ending, event["id"]), ("", "", target["id"])],
                                            "names": [before.capitalize() + "{}" + ending, "{}"]})
    return constraints


def main():
    ap = argparse.ArgumentParser(description="Search Wwise FNV-1 names under paired constraints (meet in the middle). "
                                             "Every result satisfies two independent 32-bit hashes.")
    ap.add_argument("--names", type=Path, required=True, help="names.json from audio_dump.py")
    ap.add_argument("--events", type=Path, required=True, help="events.json from audio_dump.py")
    ap.add_argument("--media", type=Path, help="media.json from audio_dump.py, adds marker label tokens")
    ap.add_argument("--candidates", type=Path, help="name_candidates.txt from audio_dump.py, adds its tokens")
    ap.add_argument("--candidate-tokens", type=int, default=3000)
    ap.add_argument("--english", type=int, default=0, help="add this many common English words (needs wordfreq)")
    ap.add_argument("--head-depth", type=int, default=1, help="vocabulary tokens before the tail token")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    names = json.loads(args.names.read_text(encoding="utf-8"))
    events = json.loads(args.events.read_text(encoding="utf-8"))
    labels = []
    if args.media and args.media.exists():
        media = json.loads(args.media.read_text(encoding="utf-8"))
        labels = [m["label"] for item in media for m in item.get("markers", []) if m.get("label")]
    head_vocabulary = base_vocabulary(names, labels)
    tail_vocabulary = set(head_vocabulary)
    if args.candidates and args.candidates.exists():
        tail_vocabulary |= set(vocabulary_from_candidates(args.candidates, args.candidate_tokens))
    tail_vocabulary |= set(english_words(args.english))
    tail_vocabulary = sorted(tail_vocabulary)
    if args.head_depth == 1:
        head_vocabulary = tail_vocabulary
    unresolved_ids = {int(k) for k in names["unresolved"]}
    constraints = build_constraints(events, unresolved_ids)
    heads = Heads(head_vocabulary, args.head_depth)
    tails = Tails(tail_vocabulary, SUFFIXES)
    print("head vocabulary %d, depth %d, tail vocabulary %d (%d tails), %d constraints" % (
        len(head_vocabulary), args.head_depth, len(tail_vocabulary), len(tails.texts), len(constraints)))
    indexes = {}
    found = {}
    for constraint in constraints:
        befores = tuple(before for before, _, _ in constraint["templates"])
        if befores not in indexes:
            indexes[befores] = HeadIndex(heads, befores)
        stems = solve(constraint["templates"], heads, indexes[befores], tails)
        for stem in stems:
            for (_, _, target), pattern in zip(constraint["templates"], constraint["names"]):
                name = pattern.format(stem)
                if fnv(name) != target:
                    continue
                entry = found.setdefault(target, {"name": name, "sources": ["search: " + constraint["kind"]],
                                                  "alternatives": []})
                if entry["name"] != name and name not in entry["alternatives"]:
                    entry["alternatives"].append(name)
        if stems:
            print("  %s %s -> %s" % (constraint["kind"], ["%08X" % t for _, _, t in constraint["templates"]], stems[:4]))
    merged = json.loads(args.out.read_text(encoding="utf-8")) if args.out.exists() else {}
    for target, entry in found.items():
        merged.setdefault(str(target), entry)
    args.out.write_text(json.dumps(dict(sorted(merged.items())), indent=1), encoding="utf-8")
    print("found %d names, %d in %s" % (len(found), len(merged), args.out))


if __name__ == "__main__":
    main()
