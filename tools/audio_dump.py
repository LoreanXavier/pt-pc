import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
import foxhash
import fsm
import sbp
import wwise_bank

DEFAULT_VGMSTREAM = Path(r"C:\Tools\vgmstream\vgmstream-cli.exe")
BANK_ORDER = ["Init", "sys_resident", "bg_common", "sfx_common", "bgm_common"]
EVENT_PREFIXES = ["Play", "Stop", "Pause", "Resume", "Set", "Reset", "Mute", "Unmute", "Seek", "Break"]
WWISE_DEFAULT_NAMES = [
    "Master Audio Bus", "Master Motion Bus", "Master Secondary Bus", "Master Control Bus", "Environmental Bus",
    "Init", "None", "Default",
]
DATA_EXTENSIONS = {"fox2", "evf", "sdf", "vfx", "vfxlf", "lua", "uigb", "uilb", "uia", "uif", "subp", "parts", "sim",
                   "tgt", "fcnp", "frdv", "frig", "mog", "atsh", "ph", "pcsp", "lpsh", "mtar", "gani", "sab"}
REFERENCE_EXTENSIONS = DATA_EXTENSIONS | {"fsm"}
TRUSTED_SOURCES = ("lua:", "data:", "eboot:", "wem-marker:", "sab", "bank-", "wwise-default")


class Layout:
    def __init__(self, root: Path):
        self.root = root
        self.dump = root / "dump"
        self.sound_assets = self.dump / "chunk1" / "as" / "sh" / "sound" / "asset"
        self.demo_streams = self.dump / "chunk1" / "as" / "sh" / "demo" / "demo_stream"
        self.fpk = self.dump / "fpk"
        self.lua = self.dump / "lua"
        self.eboot = self.dump / "elf" / "eboot.elf"
        self.eboot_strings = self.dump / "strings" / "eboot_strings.txt"
        self.fox2_names = self.dump / "fox2" / "names.txt"
        self.subtitles = self.fpk / "subtitle_fpk" / "Assets" / "sh" / "ui" / "Subtitles" / "subp" / "EngVoice" / "EngText"
        self.out = self.dump / "audio"
        self.banks = self.out / "banks"
        self.wem = self.out / "wem"
        self.hirc = self.out / "hirc"


def step_unpack(layout: Layout):
    layout.banks.mkdir(parents=True, exist_ok=True)
    for package in sorted(layout.sound_assets.glob("*.sbp")):
        data = package.read_bytes()
        for entry in sbp.read_sbp(data):
            blob = data[entry["offset"]:entry["offset"] + entry["size"]]
            (layout.banks / ("%s.%s" % (package.stem, entry["kind"]))).write_bytes(blob)
    shutil.copyfile(layout.sound_assets / "Init.bnk", layout.banks / "Init.bnk")
    print("unpack: %s" % ", ".join(sorted(p.name for p in layout.banks.iterdir())))


def load_banks(layout: Layout):
    banks = {}
    for name in BANK_ORDER:
        path = layout.banks / ("%s.bnk" % name)
        if path.exists():
            banks[name] = wwise_bank.load_bank(path)
    return banks


def demo_stream_files(layout: Layout):
    files = []
    for path in sorted(layout.demo_streams.rglob("*.fsm")):
        language = path.parent.name.lstrip("#") if path.parent.name.startswith("#") else ""
        files.append((path, language))
    for path in sorted(layout.fpk.rglob("*.fsm")):
        files.append((path, ""))
    return files


def step_extract(layout: Layout, banks):
    media = []
    for name, bank in banks.items():
        if not bank.media:
            continue
        target = layout.wem / name
        target.mkdir(parents=True, exist_ok=True)
        for media_id in bank.media:
            blob = bank.media_bytes(media_id)
            (target / ("%d.wem" % media_id)).write_bytes(blob)
            media.append({"key": "%s/%d" % (name, media_id), "bank": name, "media_id": media_id,
                          "wem": "wem/%s/%d.wem" % (name, media_id), "wav": "%s/%d.wav" % (name, media_id),
                          "bytes": len(blob)})
    target = layout.wem / "demo_stream"
    for path, language in demo_stream_files(layout):
        data = path.read_bytes()
        stream = fsm.sound_stream(data, fsm.read_chunks(data))
        if stream is None:
            continue
        stem = path.stem + ("_" + language if language else "")
        target.mkdir(parents=True, exist_ok=True)
        (target / (stem + ".wem")).write_bytes(stream["data"])
        media.append({"key": "demo_stream/%s" % stem, "bank": "demo_stream", "media_id": None,
                      "demo": path.stem, "language": language or None,
                      "wem": "wem/demo_stream/%s.wem" % stem, "wav": "demo_stream/%s.wav" % stem,
                      "bytes": len(stream["data"])})
    for item in media:
        blob = (layout.out / item["wem"]).read_bytes()
        item.update(wwise_bank.wem_format(blob) or {})
        markers = wwise_bank.wem_markers(blob)
        if markers:
            item["markers"] = markers
    print("extract: %d media files" % len(media))
    return media


def run_vgmstream(vgmstream: Path, source: Path, target: Path):
    target.parent.mkdir(parents=True, exist_ok=True)
    info = subprocess.run([str(vgmstream), "-I", "-m", str(source)], capture_output=True, text=True)
    decode = subprocess.run([str(vgmstream), "-i", "-o", str(target), str(source)], capture_output=True, text=True)
    result = {"decode_ok": decode.returncode == 0 and target.exists()}
    if not result["decode_ok"]:
        result["decode_error"] = (decode.stderr or decode.stdout).strip()[-400:]
    try:
        meta = json.loads(info.stdout.strip().splitlines()[0])
        result["codec"] = meta.get("encoding")
        result["channel_layout"] = meta.get("channelLayout")
        result["samples"] = meta.get("numberOfSamples")
        looping = meta.get("loopingInfo")
        if looping:
            result["loop_start"] = looping.get("start")
            result["loop_end"] = looping.get("end")
    except (ValueError, IndexError):
        result["info_error"] = (info.stderr or info.stdout).strip()[-400:]
    return result


def spectral_flatness(mono: np.ndarray):
    window = np.hanning(len(mono))
    power = np.abs(np.fft.rfft(mono * window)) ** 2 + 1e-20
    return float(np.exp(np.mean(np.log(power))) / np.mean(power))


def analyze_wav(path: Path):
    info = sf.info(str(path))
    channels = info.channels
    sum_squares = np.zeros(channels)
    peak = np.zeros(channels)
    clipped = 0
    count = 0
    flatness = []
    frame = 4096
    duplicate_pairs = None
    for block in sf.blocks(str(path), blocksize=info.samplerate, dtype="float32", always_2d=True):
        sum_squares += np.sum(block.astype(np.float64) ** 2, axis=0)
        peak = np.maximum(peak, np.max(np.abs(block), axis=0))
        clipped += int(np.count_nonzero(np.abs(block) >= 0.999))
        count += len(block)
        if channels > 2:
            pairs = {(a, b) for a in range(channels) for b in range(a + 1, channels)
                     if np.array_equal(block[:, a], block[:, b])}
            duplicate_pairs = pairs if duplicate_pairs is None else duplicate_pairs & pairs
        if len(block) >= frame:
            mono = block.mean(axis=1)
            middle = mono[(len(mono) - frame) // 2:(len(mono) - frame) // 2 + frame]
            if np.sqrt(np.mean(middle ** 2)) > 1e-3:
                flatness.append(spectral_flatness(middle))
    if count == 0:
        return {"frames": 0, "channels": channels, "sample_rate": info.samplerate, "flags": ["empty"]}

    def db(value):
        return round(20 * np.log10(max(float(value), 1e-10)), 2)

    channel_rms = np.sqrt(sum_squares / count)
    total_rms = float(np.sqrt(sum_squares.sum() / (count * channels)))
    result = {
        "frames": count,
        "channels": channels,
        "sample_rate": info.samplerate,
        "duration": round(count / info.samplerate, 3),
        "rms_db": db(total_rms),
        "peak_db": db(peak.max()),
        "channel_rms_db": [db(v) for v in channel_rms],
        "crest_db": round(db(peak.max()) - db(total_rms), 2),
        "clipped_fraction": round(clipped / (count * channels), 6),
        "flatness": round(float(np.median(flatness)), 3) if flatness else None,
    }
    if duplicate_pairs:
        result["identical_channel_pairs"] = sorted([a, b] for a, b in duplicate_pairs
                                                   if channel_rms[a] > 0)
    flags = []
    if peak.max() < 0.001:
        flags.append("silent")
    elif total_rms < 0.001:
        flags.append("very_quiet")
    if result["flatness"] is not None and result["flatness"] > 0.4 and result["crest_db"] < 15:
        flags.append("noise_like")
    if result["clipped_fraction"] > 0.001:
        flags.append("clipping")
    result["flags"] = flags
    return result


def step_decode(layout: Layout, media, vgmstream: Path, threads: int):
    def work(item):
        item.update(run_vgmstream(vgmstream, layout.out / item["wem"], layout.out / item["wav"]))
        if item["decode_ok"]:
            item.update(analyze_wav(layout.out / item["wav"]))
        return item

    with ThreadPoolExecutor(max_workers=threads) as pool:
        list(pool.map(work, media))
    failures = [m["key"] for m in media if not m.get("decode_ok")]
    print("decode: %d ok, %d failed %s" % (len(media) - len(failures), len(failures), failures[:10]))


def read_subp(path: Path):
    data = path.read_bytes()
    count = struct.unpack_from("<H", data, 2)[0]
    entries = {}
    for index in range(count):
        key, offset = struct.unpack_from("<II", data, 4 + index * 8)
        line_count = data[offset + 2]
        text_size = struct.unpack_from("<H", data, offset + 4)[0]
        times = struct.unpack_from("<%dH" % (line_count * 2), data, offset + 12)
        text_start = offset + 12 + line_count * 4
        text = data[text_start:text_start + text_size].decode("utf-8", "replace")
        pieces = text.split("$")
        lines = []
        for line_index in range(line_count):
            lines.append({"start_cs": times[line_index * 2], "end_cs": times[line_index * 2 + 1],
                          "text": pieces[line_index].replace("\r\n", " ").strip() if line_index < len(pieces) else None})
        entries[key] = {"header": data[offset:offset + 12].hex(), "lines": lines}
    return entries


def subtitle_links(layout: Layout, media):
    sal_records = []
    for path in sorted(layout.banks.glob("*.sab")):
        sal_records.extend(dict(record, source=path.name) for record in sbp.read_sal(path.read_bytes()))
    subtitles = {}
    subp_path = layout.subtitles / "trial.subp"
    if subp_path.exists():
        subtitles = read_subp(subp_path)
    labels = {m["label"] for item in media for m in item.get("markers", []) if m.get("label")}
    label_by_key = {foxhash.strcode64(label): label for label in labels}
    for record in sal_records:
        record["marker_label"] = label_by_key.get(record["key"])
        subtitle_key = foxhash.strcode64(record["subtitle_id"].lower()) & 0xFFFFFFFF
        entry = subtitles.get(subtitle_key)
        record["subtitle_found"] = entry is not None
        if entry:
            record["subtitle_lines"] = entry["lines"]
    by_label = {r["marker_label"]: r for r in sal_records if r["marker_label"]}
    for item in media:
        for marker in item.get("markers", []):
            record = by_label.get(marker.get("label"))
            if record:
                marker["subtitle_id"] = record["subtitle_id"]
                marker["subtitle_lines"] = record.get("subtitle_lines")
    return sal_records


def identifier_runs(data: bytes, minimum=3):
    return [m.group().decode("ascii") for m in re.finditer(rb"[A-Za-z0-9_][A-Za-z0-9_\-. ]{%d,127}" % (minimum - 1), data)]


def lua_string_literals(text: str):
    return re.findall(r'"((?:[^"\\\n]|\\.){1,200})"', text) + re.findall(r"'((?:[^'\\\n]|\\.){1,200})'", text)


def expand_candidate(text: str):
    text = text.strip()
    if not text:
        return set()
    found = {text}
    for piece in re.split(r"[\s/\\.,;:()\[\]{}\"'=<>|+*]+", text):
        if len(piece) >= 2:
            found.add(piece)
    return found


def identifier_prefixes(text: str):
    prefixes = set()
    if re.fullmatch(r"[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]{2,80}", text):
        for end in range(4, len(text) + 1):
            if text[end - 1].isalnum():
                prefixes.add(text[:end])
    return prefixes


def file_kind(path: Path):
    return path.name.rsplit(".", 1)[-1].lower() if "." in path.name else ""


def gather_candidates(layout: Layout, banks, media, sal_records):
    sources = defaultdict(set)

    def add(text, source):
        for piece in expand_candidate(text):
            sources[piece].add(source)

    for path in list(layout.lua.rglob("*.lua")) + list(layout.fpk.rglob("*.lua")):
        text = path.read_bytes().decode("latin-1")
        for literal in lua_string_literals(text):
            add(literal, "lua:" + path.name)
    for path in layout.fpk.rglob("*"):
        if not path.is_file() or path.suffix == ".lua":
            continue
        kind = file_kind(path)
        label = ("data:" if kind in DATA_EXTENSIONS or kind.startswith("lng") else "binary:") + path.name
        for run in identifier_runs(path.read_bytes()):
            add(run, label)
            for prefix in identifier_prefixes(run):
                sources[prefix].add(label + " (prefix)")
    if layout.fox2_names.exists():
        for line in layout.fox2_names.read_text(encoding="utf-8", errors="replace").splitlines():
            add(line, "data:fox2/names.txt")
    if layout.eboot_strings.exists():
        for line in layout.eboot_strings.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split(" ", 1)
            if len(parts) == 2:
                add(parts[1], "eboot:" + parts[0])
    for item in media:
        for marker in item.get("markers", []):
            if marker.get("label"):
                add(marker["label"], "wem-marker:" + item["key"])
    for record in sal_records:
        add(record["subtitle_id"], "sab")
    for name in WWISE_DEFAULT_NAMES:
        add(name, "wwise-default")
    for name, bank in banks.items():
        add(name, "bank-file")
        for bank_name in bank.bank_names.values():
            add(bank_name, "bank-STID")
    return sources


def hash_targets(banks):
    targets = defaultdict(set)
    for name, bank in banks.items():
        targets[bank.bank_id].add("bank")
        for bank_id in bank.bank_names:
            targets[bank_id].add("bank")
        settings = bank.global_settings
        if settings:
            for group in settings["state_groups"]:
                targets[group["id"]].add("state_group")
                for transition in group["transitions"]:
                    targets[transition["from"]].add("state")
                    targets[transition["to"]].add("state")
            for group in settings["switch_groups"]:
                targets[group["id"]].add("switch_group")
                targets[group["rtpc_id"]].add("game_parameter")
            for parameter in settings["game_parameters"]:
                targets[parameter["id"]].add("game_parameter")
        for obj in bank.objects:
            kind = obj["type"]
            if kind in ("Event", "DialogueEvent"):
                targets[obj["id"]].add("event" if kind == "Event" else "dialogue_event")
            elif kind in ("Bus", "AuxBus", "FeedbackBus"):
                targets[obj["id"]].add("bus")
            elif kind == "FxShareSet":
                targets[obj["id"]].add("shareset")
            node = obj.get("node") or {}
            for group in node.get("states", []) + obj.get("states", []):
                targets[group["state_group_id"]].add("state_group")
                for state in group["states"]:
                    targets[state["state_id"]].add("state")
            for rtpc in node.get("rtpcs", []) + obj.get("rtpcs", []):
                targets[rtpc["rtpc_id"]].add("game_parameter")
            if kind == "Action":
                if "state_group_id" in obj:
                    targets[obj["state_group_id"]].add("state_group")
                    targets[obj["state_id"]].add("state")
                if "switch_group_id" in obj:
                    targets[obj["switch_group_id"]].add("switch_group")
                    targets[obj["switch_id"]].add("switch")
                if obj["action"].startswith(("SetGameParameter", "ResetGameParameter")):
                    targets[obj["target_id"]].add("game_parameter")
                if "bank_id" in obj:
                    targets[obj["bank_id"]].add("bank")
            if kind == "SwitchCntr":
                group_kind = "state" if obj["group_type"] == "State" else "switch"
                targets[obj["group_id"]].add(group_kind + "_group")
                targets[obj["default_switch"]].add(group_kind)
                for package in obj["switches"]:
                    targets[package["switch_id"]].add(group_kind)
            if kind == "LayerCntr":
                for layer in obj["layers"]:
                    targets[layer["crossfade_rtpc_id"]].add("game_parameter")
            if kind == "DialogueEvent":
                for argument in obj["arguments"]:
                    targets[argument["group_id"]].add(argument["group_type"].lower() + "_group")
                stack = [obj["tree"]] if obj["tree"] else []
                while stack:
                    tree_node = stack.pop()
                    targets[tree_node["key"]].add("argument_value")
                    stack.extend(tree_node.get("children", []))
            for stinger in obj.get("stingers", []):
                targets[stinger["trigger_id"]].add("trigger")
    targets.pop(0, None)
    return targets


def source_rank(source: str):
    if source.endswith("(prefix)"):
        return 8
    for rank, prefix in enumerate(("lua:", "data:", "wem-marker:", "sab", "bank-", "eboot:", "wwise-default")):
        if source.startswith(prefix):
            return rank
    return 7


def resolve_names(candidates, targets, derived=False):
    by_hash = defaultdict(list)
    for text, source in candidates.items():
        by_hash[wwise_bank.fnv1_32(text)].append((text, source))
    resolved = {}
    for target_id in targets:
        matches = by_hash.get(target_id)
        if not matches:
            continue
        pick = min(matches, key=lambda m: (min(source_rank(s) for s in m[1]), m[0]))
        sources = sorted(pick[1], key=source_rank)
        if derived:
            level = "likely"
        elif any(s.startswith(TRUSTED_SOURCES) and not s.endswith("(prefix)") for s in sources):
            level = "confirmed"
        else:
            level = "likely"
        resolved[target_id] = {"name": pick[0], "confidence": level, "sources": sources[:6],
                               "alternatives": sorted({m[0] for m in matches} - {pick[0]})[:4]}
    return resolved


TOKEN_ALTERNATIVES = [
    ["s", "m", "l", "ss", "ll", "xs", "xl"],
    ["no", "s", "m", "l", "on", "off", "low", "mid", "high", "min", "max", "weak", "strong", "direct", "indirect"],
    ["l", "r", "c"],
    ["2d", "3d"],
    ["lp", "loop", "st", "ed", "in", "out", "start", "end", "stop"],
    ["on", "off", "open", "close", "in", "out", "up", "down", "start", "end"],
    ["eng", "fre", "spa", "ger", "ita", "jpn", "por", "rus"],
]


def token_variants(text: str):
    tokens = text.split("_")
    variants = set()
    for index, token in enumerate(tokens):
        for group in TOKEN_ALTERNATIVES:
            if token.lower() in group:
                for alternative in group:
                    variants.add("_".join(tokens[:index] + [alternative] + tokens[index + 1:]))
        if index > 0:
            variants.add("_".join(tokens[:index] + tokens[index + 1:]))
    for start in range(len(tokens)):
        for end in range(start + 1, len(tokens) + 1):
            variants.add("_".join(tokens[start:end]))
    return variants


def derived_candidates(names):
    derived = {}
    for text in names:
        match = re.match(r"^(%s)_(.+)$" % "|".join(EVENT_PREFIXES), text, re.IGNORECASE)
        stems = {text}
        if match:
            stems.add(match.group(2))
        for variant in token_variants(text):
            derived.setdefault(variant, "token variant of " + text)
            for prefix in EVENT_PREFIXES[:2]:
                derived.setdefault("%s_%s" % (prefix, variant), "token variant of " + text)
        for stem in list(stems):
            for prefix in EVENT_PREFIXES:
                derived.setdefault("%s_%s" % (prefix, stem), "prefix swap of " + text)
            for number in re.finditer(r"\d+", stem):
                width = len(number.group())
                limit = 100 if width <= 2 else 1000
                for value in range(limit):
                    variant = stem[:number.start()] + str(value).zfill(width) + stem[number.end():]
                    for prefix in EVENT_PREFIXES[:2] + [""]:
                        derived.setdefault(prefix + ("_" if prefix else "") + variant, "number variant of " + text)
            for suffix in ("_lp", "_loop", "_s", "_m", "_l", "_01", "_02", "_st", "_2d", "_3d"):
                derived.setdefault(stem + suffix, "suffix variant of " + text)
                if stem.endswith(suffix):
                    derived.setdefault(stem[:-len(suffix)], "suffix variant of " + text)
    return derived


def load_search_results(paths, targets):
    found = {}
    for path in paths or []:
        if not path.exists():
            continue
        for key, entry in json.loads(path.read_text(encoding="utf-8")).items():
            target = int(key)
            if target in targets and wwise_bank.fnv1_32(entry["name"]) == target:
                found[target] = {"name": entry["name"], "confidence": entry.get("confidence", "likely"),
                                 "sources": entry.get("sources", ["search: " + path.name]),
                                 "alternatives": entry.get("alternatives", [])}
    return found


def step_names(layout: Layout, banks, media, sal_records, search_paths=None):
    candidates = gather_candidates(layout, banks, media, sal_records)
    trusted = sorted(text for text, sources in candidates.items()
                     if any(s.startswith(TRUSTED_SOURCES) and not s.endswith("(prefix)") for s in sources))
    (layout.out / "name_candidates.txt").write_text("\n".join(trusted) + "\n", encoding="utf-8")
    targets = hash_targets(banks)
    resolved = resolve_names(candidates, targets)
    print("names: %d candidate strings, %d hashed ids, %d resolved from game files" % (
        len(candidates), len(targets), len(resolved)))
    searched = {k: v for k, v in load_search_results(search_paths, targets).items() if k not in resolved}
    resolved.update(searched)
    print("names: %d added from search results" % len(searched))
    for round_index in range(3):
        seeds = {entry["name"] for entry in resolved.values()}
        seeds |= {text for text in candidates if re.match(r"^(%s)_" % "|".join(EVENT_PREFIXES), text)}
        seeds |= {m["label"] for item in media for m in item.get("markers", []) if m.get("label")}
        derived = derived_candidates(seeds)
        extra = resolve_names({k: {"derived: " + v} for k, v in derived.items() if k not in candidates},
                              {t: k for t, k in targets.items() if t not in resolved}, derived=True)
        if not extra:
            break
        resolved.update(extra)
        print("names: round %d derived %d more" % (round_index + 1, len(extra)))
    for target_id, entry in resolved.items():
        entry["kinds"] = sorted(targets[target_id])
    unresolved = {"%d" % k: sorted(v) for k, v in targets.items() if k not in resolved}
    names = {"%d" % k: v for k, v in sorted(resolved.items(), key=lambda kv: kv[1]["name"].lower())}
    (layout.out / "names.json").write_text(json.dumps({"resolved": names, "unresolved": unresolved}, indent=1),
                                           encoding="utf-8")
    return resolved, targets


def executable_segment(data: bytes):
    header_offset = struct.unpack_from("<Q", data, 0x20)[0]
    entry_size, entry_count = struct.unpack_from("<HH", data, 0x36)
    for index in range(entry_count):
        kind, flags, offset, address, _, file_size = struct.unpack_from("<IIQQQQ", data, header_offset + index * entry_size)
        if kind == 1 and flags & 1:
            return offset, offset + file_size, address
    raise ValueError("no executable PT_LOAD segment")


def instruction_with_immediate(disassembler, data, start, base, hit, value):
    votes = Counter()
    for back in range(96, 104):
        origin = hit - back
        for instruction in disassembler.disasm(data[origin:hit + 16], base + origin - start):
            offset = instruction.address - base + start
            if offset > hit:
                break
            if offset + instruction.size <= hit:
                continue
            immediates = {int(x, 16) & 0xFFFFFFFF for x in re.findall(r"0x[0-9a-f]+", instruction.op_str)}
            if offset + instruction.size >= hit + 4 and value in immediates:
                votes["0x%X %s %s" % (instruction.address, instruction.mnemonic, instruction.op_str)] += 1
            break
    if not votes:
        return None
    text, count = votes.most_common(1)[0]
    return text if count >= 3 else None


def eboot_references(layout: Layout, wanted):
    if not layout.eboot.exists():
        return {}
    import capstone
    data = layout.eboot.read_bytes()
    start, end, base = executable_segment(data)
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    found = defaultdict(list)
    for value in wanted:
        needle = struct.pack("<I", value)
        position = data.find(needle, start, end)
        while position >= 0:
            confirmed = instruction_with_immediate(disassembler, data, start, base, position, value)
            if confirmed:
                found[value].append("eboot " + confirmed)
            position = data.find(needle, position + 1, end)
    return found


def data_references(layout: Layout, wanted):
    found = defaultdict(list)
    needles = {struct.pack("<I", value): value for value in wanted}
    paths = [p for p in layout.fpk.rglob("*") if p.is_file() and file_kind(p) in REFERENCE_EXTENSIONS]
    paths += list(layout.demo_streams.rglob("*.fsm"))
    for path in paths:
        data = path.read_bytes()
        for needle, value in needles.items():
            position = data.find(needle)
            hits = 0
            while position >= 0 and hits < 8:
                found[value].append("%s+0x%X" % (path.relative_to(layout.dump).as_posix(), position))
                hits += 1
                position = data.find(needle, position + 1)
    return found


def curve_in_db(curve):
    if curve["scaling"] != "dB":
        return {"scaling": curve["scaling"], "points": curve["points"]}
    points = []
    for x, y, interpolation in curve["points"]:
        decibels = -96.3 if y <= -1.0 else round(20 * np.log10(y + 1.0), 2)
        points.append([x, decibels, interpolation])
    return {"scaling": "dB (stored y in -1..0, shown as 20*log10(y+1))", "points": points}


class Graph:
    def __init__(self, banks, names, media_by_key):
        self.banks = banks
        self.names = names
        self.media_by_key = media_by_key
        self.objects = {}
        for bank_name, bank in banks.items():
            for obj in bank.objects:
                if obj["id"] in self.objects and self.objects[obj["id"]]["type"] != obj["type"]:
                    continue
                self.objects.setdefault(obj["id"], obj)
        self.bank_by_id = {bank.bank_id: bank_name for bank_name, bank in banks.items()}
        self.media_bank = {}
        for bank_name, bank in banks.items():
            for media_id in bank.media:
                self.media_bank.setdefault(media_id, bank_name)

    def name(self, object_id):
        entry = self.names.get(object_id)
        return entry["name"] if entry else None

    def named(self, object_id):
        return {"id": object_id, "name": self.name(object_id)}

    def parents(self, obj):
        chain = []
        seen = set()
        while obj is not None and obj["id"] not in seen:
            seen.add(obj["id"])
            chain.append(obj)
            node = obj.get("node")
            parent_id = node.get("parent_id") if node else None
            obj = self.objects.get(parent_id) if parent_id else None
        return chain

    def effective(self, obj):
        chain = self.parents(obj)
        result = {"volume_db": 0.0, "pitch_cents": 0.0, "lpf": 0.0}
        positioning = None
        bus_id = None
        for item in chain:
            node = item.get("node") or {}
            props = node.get("props", {})
            result["volume_db"] += props.get("Volume", 0.0)
            result["pitch_cents"] += props.get("Pitch", 0.0)
            result["lpf"] += props.get("LPF", 0.0)
            if positioning is None and node.get("positioning", {}).get("override_parent"):
                positioning = node["positioning"]
            if bus_id is None and node.get("override_bus_id"):
                bus_id = node["override_bus_id"]
        result = {k: round(v, 3) for k, v in result.items()}
        result["positioning"] = self.describe_positioning(positioning)
        result["bus"] = self.describe_bus(bus_id)
        aux = next((i["node"]["aux"] for i in chain if i.get("node", {}).get("aux", {}).get("user_aux_buses")), None)
        if aux:
            result["user_aux_sends"] = [self.named(bus) for bus in aux["user_aux_buses"] if bus]
        return result

    def describe_positioning(self, positioning):
        if not positioning:
            return {"mode": "2D (no override in the hierarchy)"}
        if not positioning.get("has_3d"):
            return {"mode": "2D", "panner": positioning.get("enable_panner", False)}
        description = {"mode": "3D", "type": positioning.get("type_3d"), "spatialized": positioning.get("spatialized"),
                       "attenuation_id": positioning.get("attenuation_id")}
        if "dynamic" in positioning:
            description["dynamic"] = positioning["dynamic"]
        if "path_mode" in positioning:
            description["path_mode"] = positioning["path_mode"]
            description["path_vertices"] = positioning.get("path_vertices")
        attenuation = self.objects.get(positioning.get("attenuation_id"))
        if attenuation:
            curves = {slot: curve_in_db(curve) for slot, curve in attenuation["curves"].items() if curve}
            description["attenuation"] = {"max_distance": attenuation.get("max_distance"),
                                          "cone": attenuation.get("cone"), "curves": curves}
        return description

    def describe_bus(self, bus_id):
        if not bus_id:
            return None
        path = []
        seen = set()
        while bus_id and bus_id not in seen:
            seen.add(bus_id)
            bus = self.objects.get(bus_id)
            path.append({"id": bus_id, "name": self.name(bus_id),
                         "volume_db": (bus or {}).get("props", {}).get("Volume", 0.0) if bus else None})
            bus_id = bus.get("parent_bus_id") if bus else None
        return path

    def media_entry(self, source):
        media_id = source["source_id"]
        entry = {"media_id": media_id, "plugin": source["plugin"]}
        if source["plugin"] in ("Wwise Silence", "Wwise Tone Generator", "Wwise Motion Generator", "ExternalSource"):
            if source.get("params"):
                entry["params"] = source["params"]
            if source["plugin"] == "ExternalSource":
                entry["note"] = "external source cookie, audio supplied by the game at runtime"
            return entry
        bank_name = self.media_bank.get(media_id)
        entry["bank"] = bank_name or self.bank_by_id.get(source.get("file_id"))
        media = self.media_by_key.get("%s/%d" % (bank_name, media_id)) if bank_name else None
        if media:
            entry["wav"] = media["wav"]
            for key in ("duration", "channels", "rms_db", "loop_start", "loop_end"):
                if media.get(key) is not None:
                    entry[key] = media[key]
            if media.get("markers"):
                entry["markers"] = media["markers"]
            if media.get("transcript"):
                entry["transcript"] = media["transcript"]
        else:
            entry["missing"] = True
        return entry

    def tree(self, object_id, depth=0, seen=None):
        seen = set() if seen is None else seen
        obj = self.objects.get(object_id)
        if obj is None:
            return {"id": object_id, "missing": True}
        node = {"id": object_id, "type": obj["type"]}
        if self.name(object_id):
            node["name"] = self.name(object_id)
        if object_id in seen or depth > 12:
            node["cycle"] = True
            return node
        seen = seen | {object_id}
        base = obj.get("node") or {}
        props = base.get("props", {})
        for key in ("Volume", "Pitch", "LPF", "Loop", "InitialDelay", "DelayTime"):
            if key in props:
                node[key.lower()] = props[key]
        kind = obj["type"]
        if kind == "Sound":
            node["media"] = self.media_entry(obj["source"])
            if "Loop" in props:
                node["looping"] = "infinite" if props["Loop"] == 0 else props["Loop"]
        elif kind == "FeedbackNode":
            node["media"] = [self.media_entry(s) for s in obj["sources"]]
        elif kind == "MusicTrack":
            node["track_type"] = obj["track_type"]
            node["media"] = [self.media_entry(s) for s in obj["sources"]]
            node["playlist"] = obj["playlist"]
        elif kind in ("RanSeqCntr", "SwitchCntr", "LayerCntr", "ActorMixer", "MusicSegment", "MusicRanSeqCntr"):
            if kind == "RanSeqCntr":
                node["mode"] = obj["mode"]
                node["random_mode"] = obj["random_mode"]
                node["loop_count"] = obj["loop_count"]
                node["continuous"] = obj["continuous"]
                node["avoid_repeat"] = obj["avoid_repeat_count"]
            if kind == "SwitchCntr":
                node["group"] = dict(self.named(obj["group_id"]), type=obj["group_type"])
                node["default_switch"] = self.named(obj["default_switch"])
                node["switches"] = [dict(self.named(p["switch_id"]), nodes=p["nodes"]) for p in obj["switches"]]
            if kind == "LayerCntr":
                node["layers"] = [{"rtpc": self.named(l["crossfade_rtpc_id"]),
                                   "children": [a["child_id"] for a in l["associations"]]} for l in obj["layers"]]
            if kind == "MusicSegment":
                node["duration_ms"] = obj["duration_ms"]
                node["markers"] = obj["markers"]
                node["tempo"] = obj["meter"]["tempo"]
            if kind == "MusicRanSeqCntr":
                node["playlist"] = obj["playlist"]
            node["children"] = [self.tree(child, depth + 1, seen) for child in obj.get("children", [])]
        return node

    def flat_media(self, tree_node, out):
        media = tree_node.get("media")
        if isinstance(media, dict):
            out.append(media)
        elif isinstance(media, list):
            out.extend(media)
        for child in tree_node.get("children", []):
            self.flat_media(child, out)
        return out

    def describe_action(self, action_id):
        action = self.objects.get(action_id)
        if action is None:
            return {"id": action_id, "missing": True}
        described = {"id": action_id, "type": action["action"]}
        props = action.get("props", {})
        if "DelayTime" in props:
            described["delay_ms"] = props["DelayTime"]
        if "TransitionTime" in props:
            described["fade_ms"] = props["TransitionTime"]
        for key in ("fade_curve", "value_meaning", "value"):
            if key in action:
                described[key] = action[key]
        if "bank_id" in action:
            described["bank"] = self.bank_by_id.get(action["bank_id"]) or self.name(action["bank_id"])
        if "state_group_id" in action:
            described["state_group"] = self.named(action["state_group_id"])
            described["state"] = self.named(action["state_id"])
            return described
        if "switch_group_id" in action:
            described["switch_group"] = self.named(action["switch_group_id"])
            described["switch"] = self.named(action["switch_id"])
            return described
        target_id = action["target_id"]
        target = self.objects.get(target_id)
        described["target"] = dict(self.named(target_id), type=target["type"] if target else None,
                                   is_bus=action["target_is_bus"])
        if action["action"].startswith(("SetGameParameter", "ResetGameParameter")):
            described["target"]["type"] = "GameParameter"
            return described
        if target is None or target["type"] in ("Bus", "AuxBus", "FeedbackBus"):
            return described
        tree = self.tree(target_id)
        described["tree"] = tree
        described["media"] = self.flat_media(tree, [])
        described["effective"] = self.effective(target)
        return described


def step_events(layout: Layout, banks, resolved, media, references):
    media_by_key = {m["key"]: m for m in media}
    graph = Graph(banks, resolved, media_by_key)
    events = []
    for bank_name, bank in banks.items():
        for obj in bank.objects:
            if obj["type"] not in ("Event", "DialogueEvent"):
                continue
            entry = resolved.get(obj["id"])
            event = {"id": obj["id"], "id_hex": "%08X" % obj["id"], "name": entry["name"] if entry else None,
                     "name_confidence": entry["confidence"] if entry else None,
                     "name_sources": entry["sources"] if entry else [], "bank": bank_name, "type": obj["type"],
                     "referenced_by": references.get(obj["id"], [])}
            if obj["type"] == "Event":
                event["actions"] = [graph.describe_action(action_id) for action_id in obj["action_ids"]]
            else:
                event["arguments"] = [dict(graph.named(a["group_id"]), type=a["group_type"]) for a in obj["arguments"]]
                event["decision_tree"] = obj["tree"]
                leaves = []
                stack = [obj["tree"]] if obj["tree"] else []
                while stack:
                    item = stack.pop()
                    if "audio_node_id" in item:
                        leaves.append(item["audio_node_id"])
                    stack.extend(item.get("children", []))
                event["audio_nodes"] = [graph.tree(leaf) for leaf in leaves if leaf]
            events.append(event)
    events.sort(key=lambda e: ((e["name"] or "~").lower(), e["id"]))
    reached = set()
    for event in events:
        for action in event.get("actions", []):
            for item in action.get("media", []):
                if item.get("wav"):
                    reached.add("%s/%d" % (item["bank"], item["media_id"]))
        for node in event.get("audio_nodes", []):
            for item in graph.flat_media(node, []):
                if item.get("wav"):
                    reached.add("%s/%d" % (item["bank"], item["media_id"]))
    unreached = sorted(k for k in media_by_key if k not in reached and not k.startswith("demo_stream/"))
    document = {
        "note": "Generated by tools/audio_dump.py from Wwise bank version 88. Wwise names are FNV-1 32 hashes of the "
                "lowercase name. confirmed: a verbatim string from Lua, data files, eboot, wem markers or the sab "
                "hashes to the id; likely: matched only through a binary-noise string or a derived variant "
                "(prefix swap, number or suffix variant) of a known name; guess: constrained brute force.",
        "counts": {"events": len(events), "named": sum(1 for e in events if e["name"]),
                   "named_by_confidence": dict(Counter(e["name_confidence"] for e in events if e["name"]))},
        "events": events,
        "media_not_reached_by_any_event": unreached,
        "demo_stream_media": [m for m in media if m["bank"] == "demo_stream"],
        "buses": [{"id": obj["id"], "name": graph.name(obj["id"]), "type": obj["type"],
                   "parent": obj.get("parent_bus_id"), "props": obj.get("props"), "ducks": obj.get("ducks"),
                   "fx": [dict(slot, plugin=(graph.objects.get(slot["fx_id"]) or {}).get("plugin"))
                          for slot in obj.get("fx", {}).get("slots", [])],
                   "channel_config": obj.get("channel_config")}
                  for bank in banks.values() for obj in bank.objects if obj["type"] in ("Bus", "AuxBus", "FeedbackBus")],
    }
    (layout.out / "events.json").write_text(json.dumps(document, indent=1), encoding="utf-8")
    lines = []
    for event in events:
        media_items = [m for a in event.get("actions", []) for m in a.get("media", [])]
        durations = [m.get("duration") for m in media_items if m.get("duration")]
        kinds = ",".join(sorted({a["type"] for a in event.get("actions", [])}))
        lines.append("%08X  %-44s %-9s %-13s %-28s media %3d  %-12s %s" % (
            event["id"], event["name"] or "?", event["name_confidence"] or "", event["bank"], kinds[:28],
            len(media_items), ("%.1f-%.1fs" % (min(durations), max(durations))) if durations else "",
            "refs %d" % len(event["referenced_by"]) if event["referenced_by"] else ""))
    (layout.out / "events.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("events: %d events, %d named %s, %d bank media not reached by any event" % (
        len(events), document["counts"]["named"], document["counts"]["named_by_confidence"], len(unreached)))
    return document


def step_hirc_json(layout: Layout, banks):
    layout.hirc.mkdir(parents=True, exist_ok=True)
    for name, bank in banks.items():
        (layout.hirc / ("%s.json" % name)).write_text(json.dumps(bank.to_json(), indent=1), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description="Unpack, decode and index P.T. audio (Wwise banks, sbp, fsm streams).")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--vgmstream", type=Path, default=DEFAULT_VGMSTREAM)
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--skip-decode", action="store_true", help="reuse media.json from an earlier run")
    ap.add_argument("--search-results", type=Path, nargs="*", help="JSON {id: {name, sources}} from wwise_names.py "
                    "or manual searches; each name is re-checked against its hash")
    args = ap.parse_args()

    layout = Layout(args.root)
    layout.out.mkdir(parents=True, exist_ok=True)
    step_unpack(layout)
    banks = load_banks(layout)
    step_hirc_json(layout, banks)
    media_path = layout.out / "media.json"
    if args.skip_decode and media_path.exists():
        media = json.loads(media_path.read_text(encoding="utf-8"))
    else:
        media = step_extract(layout, banks)
        step_decode(layout, media, args.vgmstream, args.threads)
    sal_records = subtitle_links(layout, media)
    transcripts_path = layout.out / "asr.json"
    if transcripts_path.exists():
        transcripts = json.loads(transcripts_path.read_text(encoding="utf-8"))
        for item in media:
            text = (transcripts.get(item["key"]) or {}).get("text")
            if text:
                item["transcript"] = text
    media_path.write_text(json.dumps(media, indent=1), encoding="utf-8")
    (layout.out / "sab.json").write_text(json.dumps(sal_records, indent=1), encoding="utf-8")
    search_paths = args.search_results
    if search_paths is None:
        search_paths = sorted(layout.out.glob("name_search*.json"))
    resolved, targets = step_names(layout, banks, media, sal_records, search_paths)
    unresolved = Counter(kind for target, kinds in targets.items() if target not in resolved for kind in kinds)
    print("names: %d of %d hashed ids resolved; unresolved by kind %s" % (len(resolved), len(targets), dict(unresolved)))
    event_ids = [t for t, kinds in targets.items() if kinds & {"event", "dialogue_event", "game_parameter", "state",
                                                                 "state_group", "bus", "switch", "switch_group"}]
    references = eboot_references(layout, event_ids)
    for key, value in data_references(layout, event_ids).items():
        references.setdefault(key, []).extend(value)
    (layout.out / "references.json").write_text(json.dumps(
        {"%08X" % k: {"name": (resolved.get(k) or {}).get("name"), "kinds": sorted(targets[k]), "refs": v}
         for k, v in sorted(references.items())}, indent=1), encoding="utf-8")
    step_events(layout, banks, resolved, media, references)


if __name__ == "__main__":
    main()
