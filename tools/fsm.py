"""Fox Engine demo stream (.fsm) reader for P.T.

Lists chunks, extracts the interleaved Wwise stream, and decodes the DEMO packets: the node tree
header (actors and tracks), the motion blocks (bit-packed keys) and the event blocks. Layout and
evidence are in docs/formats/fsm.md; runtime behaviour in docs/demo.md.
"""
import argparse
import json
import math
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import foxhash  # noqa: E402

CHUNK_HEADER = struct.Struct("<4sI")
FIRST_SOUND_HEADER = struct.Struct("<dII8x")

FPS = 60000.0 / 1001.0          # demo frame rate (0xB13610 multiplies by 59.940059940059946)
TICKS_PER_FRAME = 5              # units header field, time unit of the key deltas

DEFAULT_STREAMS = [Path("dump/fpk/resident_fpk/Assets/sh/demo/demo_stream"),
                   Path("dump/chunk1/as/sh/demo/demo_stream")]


def h32(text):
    return foxhash.strcode64(text) & 0xFFFFFFFF


def h64(text):
    return foxhash.strcode64(text)


NODE_NAMES = ["ROOT", "DEMO", "CAMERA", "MOVE", "CameraParam", "SI Frame", "LOCATOR", "MESH_EVENT", "MOTION",
              "SKEL", "MODEL", "SKELINFO", "MTPINFO", "MTEV", "SHADER", "MTP", "MTP_LIST", "MTP_PARENT_LIST",
              "UNIT", "TARGET_NAME", "SLOPE_DIR", "SLOPE_ANGLE", "Transform"]
HASH32_NAMES = {h32(n): n for n in NODE_NAMES}

EVENT_TYPES = {
    0x2379C011: "ExecCommand", 0xD3185DFF: "DemoStart", 0xD2A6999A: "DemoEnd", 0x2A36C514: "ClipEnd",
    0x7F4D8E71: "CreateCamera", 0xF4603510: "DeleteCamera", 0xC5806267: "CreateModel", 0x75586EE7: "DeleteModel",
    0x966D8EC3: "CreateLocator", 0x3148F136: "DeleteLocator", 0x3E09E7B9: "VisibleModel", 0x6B491E18: "VisibleMesh",
}

# ExecCommand functor names (StrCode64 of "<Class>Functor[_<Method>]"; the event also stores "DemoEvent_" + name).
FUNCTOR_NAMES = ["DemoSendMessageFunctor", "FxEffectCreateEventFunctor", "EffectConnectToNullFunctor",
                 "DemoConstraintFunctor_ConstrainObject", "DemoUiFunctor_Create", "DemoUiFunctor_Start"]
for _cls in ("DemoLightFunctor", "DemoPointLightFunctor"):
    for _m in ("CreateLight", "SetColor", "SetWorldMatrix", "SetRange", "SetShadow", "SetShadowAngularAttenuation",
               "SetViewBias", "SetEnable", "SetBias", "SetAngle"):
        FUNCTOR_NAMES.append("%s_%s" % (_cls, _m))

# Functors identified from their handlers (docs/demo.md section 10); keys are StrCode64 values.
FUNCTOR_LABELS = {
    0x9FE662692727: "player take control (Sh 0x914390)", 0x7CBF30BCCFC2: "player release (Sh 0x914800)",
    0x1CA12DC9F6C4: "post Wwise event (Sh 0x91ADA0)", 0x436C48773668: "Wwise event with length (0x77CB60)",
    0xD0044B945DC2: "Wwise event with length (0x77CB60)",
    0x7D2CDA51F34A: "camera focusDistance", 0x132A74BFDAB0: "camera aperture", 0xDA7CC51B6E8C: "camera focalLength",
    0x56FAACF8F8A1: "camera shutterSpeed", 0xF3F47F2C0016: "camera exposureCompensation",
    0x27D0DA57A8B3: "camera minExposure", 0x26B0AB0BAB22: "camera maxExposure", 0x0F01431D2840: "camera bloomSize",
    0xD0E6DA362CC8: "camera nearClipDistance", 0xFD5CF0AF8B5C: "camera farClipDistance",
    0x7375EBD4EB24: "camera effectiveFocalLengthRatio", 0x80F81AC653DC: "FullScreenBlur fetch pixel band",
    0xC47B4C2312D7: "FullScreenBlur previous frame blend rate", 0x5200186357D4: "FullScreenBlur enable",
    0xFA384E14AB0E: "FadeIn", 0xDCB1EDCE2762: "FadeOut", 0x2210363B6ED3: "SetFadeColor",
}

MESSAGE_NAMES = ["Play", "Finish", "FinishMotion", "OpenDoor20", "hideOcho", "Endf120", "GotoGameOver", "PlayRadio",
                 "PlayGimmick", "PadEnable", "DisableOption", "InvisibleStaticModel", "enable_voice_breath",
                 "enable_vfx_dust_glass", "PlayEnd", "Interrupt", "Skip", "Start"]
PARAM_KEYS = {h32("message"): "message", h32("TARGET_NAME"): "TARGET_NAME"}

STRING64 = {0xB8A0BF169F98: ""}
for _n in FUNCTOR_NAMES:
    STRING64[h64(_n)] = _n
    STRING64[h64("DemoEvent_" + _n)] = "DemoEvent_" + _n
for _n in MESSAGE_NAMES:
    STRING64[h64(_n)] = _n

# Track kinds: low 4 bits of the track descriptor byte. Component counts from the table at 0x13D3FA0.
KIND_NAMES = {0: "rotation", 1: "float1", 2: "float2", 3: "float3", 4: "float4", 5: "rotation_slerp", 6: "float3_root"}
KIND_COMPONENTS = {1: 1, 2: 2, 3: 3, 4: 4, 6: 3}

# ExecCommand parameter types (high 16 bits of the descriptor word) and their value arrays.
PARAM_TYPES = {0x04: ("int", "ints", 1), 0x05: ("int", "ints", 1), 0x0A: ("bool", "ints", 1),
               0x08: ("float", "floats", 1), 0x0B: ("string", "strings", 1), 0x16: ("string", "strings", 1),
               0x0E: ("vector3", "floats", 3), 0x020E: ("vector3", "floats", 3), 0x040E: ("vector3", "floats", 3),
               0x0F: ("vector4", "floats", 4), 0x10: ("vector4", "floats", 4), 0x0210: ("quat", "floats", 4),
               0x13: ("color", "floats", 4), 0x14: ("file", "strings", 1)}


# ---------------------------------------------------------------- chunks and sound (unchanged behaviour)

def read_chunks(data: bytes):
    chunks = []
    pos = 0
    while pos + CHUNK_HEADER.size <= len(data):
        tag, size = CHUNK_HEADER.unpack_from(data, pos)
        if size == 0:
            break
        if size < CHUNK_HEADER.size or pos + size > len(data):
            raise ValueError("chunk at 0x%X has bad size 0x%X" % (pos, size))
        time = None
        if tag != b"SYS " and size >= 0x10:
            time = struct.unpack_from("<d", data, pos + 8)[0]
        chunks.append({"offset": pos, "tag": tag.decode("ascii", "replace").strip(), "size": size, "time": time})
        pos += size
    return chunks


def sound_stream(data: bytes, chunks):
    sound_chunks = [c for c in chunks if c["tag"] == "SND"]
    if not sound_chunks:
        return None
    first = sound_chunks[0]
    _, total_size, stream_kind = FIRST_SOUND_HEADER.unpack_from(data, first["offset"] + 8)
    parts = []
    for index, chunk in enumerate(sound_chunks):
        payload_start = chunk["offset"] + (0x20 if index == 0 else 0x10)
        parts.append(data[payload_start:chunk["offset"] + chunk["size"]])
    stream = b"".join(parts)
    if len(stream) < total_size:
        raise ValueError("sound stream is %d bytes, header says %d" % (len(stream), total_size))
    return {"total_size": total_size, "stream_kind": stream_kind, "chunk_count": len(sound_chunks),
            "data": stream[:total_size]}


# ---------------------------------------------------------------- key codec (shared with tools/motion.py)

def read_bits(buf, pos, count):
    """LSB-first bit field; equivalent to the runtime's 16-bit little-endian word reader."""
    start = pos >> 3
    raw = int.from_bytes(buf[start:start + (((pos & 7) + count + 7) >> 3)], "little")
    return (raw >> (pos & 7)) & ((1 << count) - 1)


def decode_quat(buf, pos, bits):
    """Axis-angle quaternion, 3*bits+3 bits (0xAD6980)."""
    mask = (1 << bits) - 1
    scale = 1.0 / mask
    angle = read_bits(buf, pos, bits)
    ax = read_bits(buf, pos + bits, bits) * scale
    ay = read_bits(buf, pos + 2 * bits, bits) * scale
    az = 1.0 - ax - ay
    signs = read_bits(buf, pos + 3 * bits, 3)
    length2 = ax * ax + ay * ay + az * az
    length = math.sqrt(length2) if length2 > 1.1754944e-38 else 1.0
    half = angle * scale * math.pi * 0.5
    k = math.sin(half) / length
    return (ax * (-k if signs & 1 else k), ay * (-k if signs & 2 else k), az * (-k if signs & 4 else k),
            math.cos(half))


def decode_vector(buf, pos, bits, count):
    """count components of 32-bit floats (bits == 32) or 16-bit floats with exponent bias 8 (0xAD46F0, 0xAD4610)."""
    out = []
    for i in range(count):
        raw = read_bits(buf, pos + i * bits, bits)
        if bits == 32:
            out.append(struct.unpack("<f", struct.pack("<I", raw))[0])
        else:
            out.append(half_to_float(raw))
    return tuple(out)


def half_to_float(raw):
    # the runtime ignores denormals: exponent 0 gives 0
    exponent = (raw >> 10) & 0x1F
    bits = ((raw & 0x8000) << 16) | ((raw & 0x3FF) << 13) | ((exponent + 119) << 23 if exponent else 0)
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def key_bits(kind, bits):
    if kind in (0, 5):
        return 3 * bits + 3
    return KIND_COMPONENTS[kind] * bits


def read_keys(buf, byte_offset, kind, bits, static, frames):
    """Key stream: value, then (u8 delta frames, value) pairs until the deltas cover `frames`."""
    size = key_bits(kind, bits)
    if kind in (0, 5):
        reader = lambda p: decode_quat(buf, p, bits)  # noqa: E731
    else:
        count = KIND_COMPONENTS[kind]
        reader = lambda p: decode_vector(buf, p, bits, count)  # noqa: E731
    pos = byte_offset * 8
    keys = [(0, reader(pos))]
    if static:
        return keys
    frame = 0
    pos += size
    while frame < frames:
        delta = read_bits(buf, pos, 8)
        if delta == 0:
            break
        frame += delta
        keys.append((frame, reader(pos + 8)))
        pos += 8 + size
    return keys


# ---------------------------------------------------------------- node tree

def cstring(buf, offset):
    end = buf.index(b"\0", offset)
    return buf[offset:end].decode("ascii", "replace")


NODE = struct.Struct("<IIIiIiiiiIII")


def parse_params(buf, offset):
    """Parameter list: u16 type, u16 next, u32 key hash, u32 key string offset, value (type 1 string, 2 float)."""
    params = []
    while True:
        kind, nxt, key, key_off = struct.unpack_from("<HHII", buf, offset)
        name = cstring(buf, offset + 4 + key_off) if key_off else PARAM_KEYS.get(key, "#%08X" % key)
        if kind == 1:
            value_hash, value_off = struct.unpack_from("<II", buf, offset + 12)
            value = cstring(buf, offset + 12 + value_off) if value_off else "#%08X" % value_hash
        elif kind == 2:
            value = struct.unpack_from("<f", buf, offset + 12)[0]
        else:
            value = buf[offset + 12:offset + 16].hex()
        params.append({"name": name, "type": kind, "value": value})
        if not nxt:
            return params
        offset += nxt


def parse_units(buf, offset):
    """Units table: u32 unitCount, trackCount, unknown, frameCount, ticksPerFrame, u32 unitOffsets[]."""
    unit_count, track_count, unknown, frames, ticks = struct.unpack_from("<5I", buf, offset)
    offsets = struct.unpack_from("<%dI" % unit_count, buf, offset + 20)
    end = offset + 20 + 4 * unit_count
    units = []
    for unit_offset in offsets:
        u = offset + unit_offset
        unit_hash, count, flags = struct.unpack_from("<IBB", buf, u)
        tracks = []
        for i in range(count):
            e = u + 8 + 8 * i
            data_off, index, kind_byte, bits = struct.unpack_from("<iHBB", buf, e)
            tracks.append({"index": index, "kind": kind_byte & 0xF, "more": bool(kind_byte & 0x80), "bits": bits,
                           "dataOffset": (e + data_off) if data_off else None})
        units.append({"hash": unit_hash, "flags": flags, "tracks": tracks})
        end = max(end, u + 8 + 8 * count)
    return {"unitCount": unit_count, "trackCount": track_count, "unknown": unknown, "frames": frames,
            "ticksPerFrame": ticks, "units": units, "end": end}


def parse_name_table(buf, offset):
    """u32 count, then (u32 hash, u32 string offset relative to the entry)."""
    count = struct.unpack_from("<I", buf, offset)[0]
    out = []
    for i in range(count):
        e = offset + 4 + 8 * i
        value, str_off = struct.unpack_from("<II", buf, e)
        out.append({"hash": value, "name": cstring(buf, e + str_off) if str_off else None})
    return out


def parse_tree(buf, root):
    nodes = []

    def visit(offset, depth, parent):
        while True:
            (name_hash, name_off, has_units, data_off, data_size, _parent, child, _prev, nxt, extra_size,
             _r28, _r2c) = NODE.unpack_from(buf, offset)
            name = cstring(buf, offset + name_off) if name_off else HASH32_NAMES.get(name_hash, "#%08X" % name_hash)
            node = {"offset": offset, "name": name, "hash": name_hash, "depth": depth, "parent": parent,
                    "hasUnits": has_units, "dataOffset": data_off, "dataSize": data_size, "extraSize": extra_size}
            body = offset + (0x40 if name_off else 0x30)
            params_at = body
            unit_count = struct.unpack_from("<I", buf, offset + data_off)[0] if has_units and data_off else 0
            if has_units and 0 < unit_count < 4096 and offset + data_off + 20 + 4 * unit_count <= len(buf):
                node["units"] = parse_units(buf, offset + data_off)
                params_at = (node["units"]["end"] + 15) & ~15
            elif has_units:
                node["rawData"] = (offset + data_off, data_size)
            elif data_off and data_size:
                node["nameTable"] = parse_name_table(buf, offset + data_off)
            if extra_size and params_at < offset + (0x40 if name_off else 0x30) + extra_size:
                node["params"] = parse_params(buf, params_at)
            index = len(nodes)
            nodes.append(node)
            if child:
                visit(offset + child, depth + 1, index)
            if not nxt:
                return
            offset += nxt

    visit(root, 0, None)
    return nodes


def target_name(node):
    for p in node.get("params", []):
        if p["name"] == "TARGET_NAME":
            return p["value"]
    return None


# ---------------------------------------------------------------- packets

def parse_motion_packet(data, chunk):
    o = chunk["offset"] + 16
    end = chunk["offset"] + chunk["size"]
    _, flags, start, count, tracks, size, unknown18 = struct.unpack_from("<7I", data, o)
    offsets = struct.unpack_from("<%dI" % tracks, data, o + 0x1C)
    p = o + 0x1C + 4 * tracks
    offset_vector = None
    if flags & 4:
        offset_vector = struct.unpack_from("<3f", data, p)
        p += 12
    track_flags = struct.unpack_from("<%dH" % tracks, data, p) if flags & 2 else (0,) * tracks
    return {"time": chunk["time"], "flags": flags, "start": start, "count": count, "size": size,
            "unknown18": unknown18, "offsets": offsets, "offsetVector": offset_vector, "trackFlags": track_flags,
            "payload": data[o:end]}


def parse_event_packet(data, chunk):
    o = chunk["offset"] + 16
    end = chunk["offset"] + chunk["size"]
    _, size, unknown8, table_hash, count = struct.unpack_from("<5I", data, o)
    table = o + 0x0C
    offsets = struct.unpack_from("<%dI" % count, data, table + 8)
    events = []
    for i, off in enumerate(offsets):
        record_end = table + offsets[i + 1] if i + 1 < count else end
        events.append(decode_event(data, table + off, record_end))
    return {"time": chunk["time"], "unknown8": unknown8, "tableHash": table_hash, "events": events}


def section_value(value):
    if value < 0:
        return value, False
    return value & ~0x40000000, bool(value & 0x40000000)


def decode_event(data, offset, end):
    event_type, info, n_ints, n_floats, n_strings = struct.unpack_from("<IBBBB", data, offset)
    n_sections = info & 0x3F
    size_class = info >> 6
    sections = []
    for i in range(n_sections):
        if size_class == 0:
            s, e = struct.unpack_from("<ii", data, offset + 8 + 8 * i)
        elif size_class == 1:
            s, e = struct.unpack_from("<hh", data, offset + 8 + 4 * i)
        elif size_class == 2:
            s, e = struct.unpack_from("<bb", data, offset + 8 + 2 * i)
        else:
            s = e = -1
        (s, s_cut), (e, e_cut) = section_value(s), section_value(e)
        sections.append({"start": s, "end": e, "startOutsideBlock": s_cut, "endOutsideBlock": e_cut})
    elem = {0: 8, 1: 4, 2: 2, 3: 0}[size_class]
    p = offset + ((n_sections * elem + 0xB) & 0x7FC)
    ints = list(struct.unpack_from("<%dI" % n_ints, data, p))
    floats = list(struct.unpack_from("<%df" % n_floats, data, p + 4 * n_ints))
    strings = [s & 0xFFFFFFFFFFFF for s in struct.unpack_from("<%dQ" % n_strings, data, p + 4 * (n_ints + n_floats))]
    event = {"type": EVENT_TYPES.get(event_type, "#%08X" % event_type), "typeHash": event_type,
             "sections": sections, "ints": ints, "floats": floats, "strings": strings}
    if event_type == 0x2379C011:
        event.update(decode_exec_command(ints, floats, strings))
    return event


def parse_exec_footer(ints):
    """Footer words from the end (likely grammar, parses all 7,637 records in P.T.):
    (6 | flags << 16) last; with flags bit 1: (4 | 1 << 16), length, -1, (tag | n << 16) before it;
    otherwise (2 | blocks << 16), then per block (3 | n << 16) plain, or (4 | m << 16) and
    (curve | k << 16 | 1 << 24) for k interpolated + m plain parameters.
    Returns (descriptor count, interpolated count, curve, length, footer word count) or None."""
    if len(ints) < 2 or (ints[-1] & 0xFFFF) != 6:
        return None
    flags = ints[-1] >> 16
    if flags & 2:
        if len(ints) >= 5 and (ints[-2] & 0xFFFF) == 4:
            return ints[-5] >> 16, 0, None, ints[-3], 5
        return None
    tag, blocks = ints[-2] & 0xFFFF, ints[-2] >> 16
    if tag != 2:
        return None
    if blocks == 0:
        return 0, 0, None, None, 2
    if blocks == 1 and len(ints) >= 3 and (ints[-3] & 0xFFFF) in (3, 5):
        return ints[-3] >> 16, 0, None, None, 3
    if blocks == 2 and len(ints) >= 4 and (ints[-3] & 0xFFFF) == 4 and (ints[-4] >> 24) == 1:
        k = (ints[-4] >> 16) & 0xFF
        return k + (ints[-3] >> 16), k, ints[-4] & 0xFFFF, None, 4
    return None


def decode_exec_command(ints, floats, strings):
    """Raw ints, then descriptors (key32, type << 16 | index), then the footer (parse_exec_footer)."""
    out = {}
    if not ints:
        return out
    out["footer"] = {"version": ints[-1] & 0xFFFF, "flags": ints[-1] >> 16}
    footer = parse_exec_footer(ints)
    descriptors = []
    interp = 0
    raw_count = len(ints)
    if footer is None:
        out["footerUnknown"] = True
    else:
        count, interp, curve, length, words = footer
        first = len(ints) - words - 2 * count
        raw_count = first
        for n in range(count):
            key, v = ints[first + 2 * n], ints[first + 2 * n + 1]
            descriptors.append((key, v >> 16, v & 0xFFFF))
        if interp:
            out["interpolated"] = {"count": interp, "curve": curve}
        if length is not None:
            out["length"] = length
    params = {}
    for n, (key, t, idx) in enumerate(descriptors):
        tname, array, size = PARAM_TYPES.get(t, ("type%X" % t, "ints", 1))
        span = size * 2 if n < interp else size
        if array == "strings":
            value = STRING64.get(strings[idx], "#%012X" % strings[idx]) if idx < len(strings) else None
        elif array == "floats":
            vals = [round(x, 6) for x in floats[idx:idx + span]]
            value = (vals[0] if vals else None) if span == 1 else vals
        else:
            value = ints[idx] if idx < raw_count else None
        params[PARAM_KEYS.get(key, "#%08X" % key)] = {"type": tname, "value": value}
    out["params"] = params
    out["rawInts"] = ints[:raw_count]
    if len(strings) >= 5:
        functor = strings[-2]
        out["functor"] = STRING64.get(functor, "#%012X" % functor)
        out["functorHash"] = functor
        if functor in FUNCTOR_LABELS:
            out["functorLabel"] = FUNCTOR_LABELS[functor]
        out["clipString"] = "#%012X" % strings[-5] if strings[-5] not in STRING64 else STRING64[strings[-5]]
        out["category"] = STRING64.get(strings[-1], "#%012X" % strings[-1])
    return out


# ---------------------------------------------------------------- demo

ACTOR_KIND = {"CAMERA": "camera", "LOCATOR": "locator", "MOTION": "model", "DEMO": "demo", "ROOT": "root"}


class Demo:
    def __init__(self, path, demo_data=None):
        self.path = Path(path)
        self.name = self.path.stem
        self.data = self.path.read_bytes()
        self.chunks = read_chunks(self.data)
        self.demo_data = demo_data
        header = next(c for c in self.chunks if c["tag"] == "DEMO" and
                      struct.unpack_from("<I", self.data, c["offset"] + 16)[0] == 1)
        self.header_words = struct.unpack_from("<4I", self.data, header["offset"] + 16)
        self.nodes = parse_tree(self.data, header["offset"] + 0x20)
        self.blocks = []
        self.event_packets = []
        for c in self.chunks:
            if c["tag"] != "DEMO":
                continue
            kind = struct.unpack_from("<I", self.data, c["offset"] + 16)[0]
            if kind == 0:
                self.blocks.append(parse_motion_packet(self.data, c))
            elif kind == 2:
                self.event_packets.append(parse_event_packet(self.data, c))
        # 0xA93970 keeps a packet's offset vector at +0x180 of the stream state only when the packet has one and the channels
        # add +0x180 when they are evaluated, so a packet without a vector uses the last one an earlier packet set
        applied = None
        for b in self.blocks:
            if b["offsetVector"]:
                applied = b["offsetVector"]
            b["appliedOffset"] = applied
        self._build_tracks()
        self.stream_frames = max((b["start"] + b["count"] for b in self.blocks), default=0)
        self.demo_ends = [s["start"] for p in self.event_packets for e in p["events"] if e["type"] == "DemoEnd"
                          for s in e["sections"]]
        self.length = self.demo_ends[0] if self.demo_ends else self.stream_frames
        self.loops = max(1, len(self.demo_ends))

    def _build_tracks(self):
        self.tracks = {}
        self.actors = []
        for index, node in enumerate(self.nodes):
            if "units" not in node:
                continue
            chain = []
            p = node["parent"]
            while p is not None:
                chain.append(self.nodes[p]["name"])
                p = self.nodes[p]["parent"]
            group = next((n for n in chain if n in ("CAMERA", "LOCATOR", "MOTION")), None)
            name = target_name(node)
            if node["name"] == "CameraParam":
                kind = "cameraParam"
            elif node["name"] == "SI Frame":
                kind = "siFrame"
            elif node["name"] == "MOVE" and group == "CAMERA":
                kind = "camera"
            elif node["name"] == "MOVE" and group == "LOCATOR":
                kind = "locator"
            elif node["name"] == "MOVE" and group == "MOTION":
                kind = "modelRoot"
            elif node["name"] == "SKEL":
                kind = "skeleton"
            elif node["name"] == "MTP":
                kind = "motionPoints"
            elif node["name"] == "MOTION" and name is None:
                kind = "timeline"
            else:
                kind = node["name"]
            actor = {"node": index, "nodeName": node["name"], "kind": kind, "target": name,
                     "path": "/".join(reversed(chain)) + "/" + node["name"], "units": []}
            for unit in node["units"]["units"]:
                unit_tracks = []
                for t in unit["tracks"]:
                    info = {"index": t["index"], "kind": t["kind"], "kindName": KIND_NAMES.get(t["kind"]),
                            "bits": t["bits"], "unit": unit["hash"], "unitFlags": unit["flags"],
                            "actor": len(self.actors)}
                    self.tracks[t["index"]] = info
                    unit_tracks.append(t["index"])
                actor["units"].append({"hash": unit["hash"], "flags": unit["flags"], "tracks": unit_tracks})
            self.actors.append(actor)

    def track_blocks(self, index):
        """Per block: (block, keys or None, trackFlags). Keys are absolute frames; offsets applied."""
        info = self.tracks[index]
        out = []
        for b in self.blocks:
            if index >= len(b["offsets"]):
                continue
            tf = b["trackFlags"][index]
            off = b["offsets"][index]
            if tf & 8 or not off:
                out.append((b, None, tf))
                continue
            keys = read_keys(b["payload"], off, info["kind"], info["bits"], tf & 1, b["count"])
            if tf & 4 and b["appliedOffset"]:
                ov = b["appliedOffset"]
                keys = [(f, tuple(v[i] + ov[i] for i in range(len(v)))) for f, v in keys]
            out.append((b, [(b["start"] + f, v) for f, v in keys], tf))
        return out

    def actor_active(self, actor_index):
        """Frames where the actor's first track carries flag 2 are inactive (0xAAC280)."""
        first = self.actors[actor_index]["units"][0]["tracks"][0]
        return [(b["start"], b["count"], not (b["trackFlags"][first] & 2)) for b in self.blocks]


def slerp(a, b, t):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0:
        b = tuple(-x for x in b)
        dot = -dot
    if dot > 0.9995:
        r = tuple(x + (y - x) * t for x, y in zip(a, b))
    else:
        theta = math.acos(min(1.0, dot))
        s = math.sin(theta)
        wa = math.sin((1 - t) * theta) / s
        wb = math.sin(t * theta) / s
        r = tuple(wa * x + wb * y for x, y in zip(a, b))
    n = math.sqrt(sum(x * x for x in r)) or 1.0
    return tuple(x / n for x in r)


def sample_blocks(blocks, kind, frames):
    """Sample one track at integer frames 0..frames (inclusive) from per-block key lists."""
    out = [None] * (frames + 1)
    last = None
    for b, keys, tf in blocks:
        start, count = b["start"], b["count"]
        if start > frames:
            break
        span = range(start, min(start + count, frames) + 1)
        if keys is None:
            for f in span:
                if out[f] is None:
                    out[f] = last
            continue
        if len(keys) == 1:
            for f in span:
                if out[f] is None:
                    out[f] = keys[0][1]
            last = keys[0][1]
            continue
        k = 0
        for f in span:
            while k + 1 < len(keys) - 1 and keys[k + 1][0] <= f:
                k += 1
            if out[f] is not None:
                continue
            f0, v0 = keys[k]
            f1, v1 = keys[k + 1] if k + 1 < len(keys) else keys[k]
            t = 0.0 if f1 == f0 else min(1.0, max(0.0, (f - f0) / (f1 - f0)))
            if kind in (0, 5):
                out[f] = slerp(v0, v1, t)
            else:
                out[f] = tuple(x + (y - x) * t for x, y in zip(v0, v1))
        last = keys[-1][1]
    return out


# ---------------------------------------------------------------- DemoData from fox2 JSON

def load_demo_data(root):
    """DemoData entities (and their linked entities) from the fox2.py JSON output."""
    result = {}
    root = Path(root)
    if not root.exists():
        return result
    for f in sorted(root.glob("*/*.json")):
        if f.parent.name.startswith("_"):
            continue
        try:
            j = json.loads(f.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            continue
        entities = j.get("entities", [])
        if not any(e.get("class") == "DemoData" for e in entities):
            continue
        by_addr = {e["address"]: e for e in entities}

        def static(e):
            return {k: v["value"] for k, v in e["static"].items()}

        for e in entities:
            if e["class"] != "DemoData":
                continue
            s = static(e)
            d = {k: s.get(k) for k in ("demoId", "demoLength", "onMemory", "flags", "priority", "demoStreamPath",
                                       "motionPath", "audioPath", "cameraInterpType", "cameraInterpFrame",
                                       "cameraInterpCurveRate", "cameraInterpScurveCenter", "cameraTranslation",
                                       "cameraRotation", "cameraParam", "cameraDistanceToLookAt",
                                       "cameraStartTranslation", "cameraStartRotation", "cameraStartParam",
                                       "cameraStartDistanceToLookAt", "fileParams", "setupLights", "evfFiles")}
            d["source"] = j.get("assetPath")
            tr = s.get("transform")
            if tr and tr["addr"] in by_addr:
                ts = static(by_addr[tr["addr"]])
                d["transform"] = {"rotation": ts.get("transform_rotation_quat"),
                                  "translation": ts.get("transform_translation")}
            d["clips"] = [{k: v for k, v in static(by_addr[c["addr"]]).items() if k != "owner"}
                          for c in s.get("clipDatas", []) if c and c["addr"] in by_addr]
            d["controlCharacters"] = {k: {kk: vv for kk, vv in static(by_addr[c["addr"]]).items() if kk != "owner"}
                                      for k, c in (s.get("controlCharacters") or {}).items()
                                      if c and c["addr"] in by_addr}
            params = {}
            for k, link in (s.get("entityParams") or {}).items():
                tgt = link.get("target")
                if tgt and tgt["index"] < len(entities):
                    te = entities[tgt["index"]]
                    params[k] = {"class": te["class"], **{kk: vv for kk, vv in static(te).items()
                                                          if kk not in ("dataSet", "name")}}
            d["entityParams"] = params
            sa = (s.get("streamAnimation") or {}).get("target")
            if sa and sa["index"] < len(entities):
                st = static(entities[sa["index"]])
                d["streamAnimation"] = {k: st.get(k) for k in ("streamPath", "locatorTypes", "cameraTypes",
                                                               "modelFiles", "helpBoneFiles", "partsFiles",
                                                               "modelPartsDictionary", "updateJobCount")}
            result[d["demoId"]] = d
    return result


# ---------------------------------------------------------------- export

def rounded(values, digits=5):
    if values is None:
        return None
    return [round(v, digits) for v in values]


def load_bone_names(root):
    """StrCode32 -> bone name from the fmdl JSON sidecars written by tools/fmdl.py (optional)."""
    names = {}
    for f in Path(root).glob("*/*.json"):
        try:
            j = json.loads(f.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            continue
        for b in j.get("bones", []) if isinstance(j, dict) else []:
            names[h32(b["name"])] = b["name"]
    return names


def demo_to_json(demo, bone_names, bone_frames=True, all_loops=False):
    frames = demo.length if not all_loops else demo.stream_frames
    dd = demo.demo_data or {}
    stream_animation = dd.get("streamAnimation") or {}
    result = {
        "name": demo.name, "file": str(demo.path).replace("\\", "/"), "bytes": len(demo.data),
        "fps": FPS, "ticksPerFrame": TICKS_PER_FRAME,
        "chunks": dict(Counter(c["tag"] for c in demo.chunks)),
        "endTime": next((c["time"] for c in demo.chunks if c["tag"] == "END"), None),
        "length": demo.length, "seconds": demo.length / FPS, "loops": demo.loops, "streamFrames": demo.stream_frames,
        "sampledFrames": frames, "headerWords": list(demo.header_words), "demoData": dd or None,
        "tree": [], "actors": [], "tracks": [], "blocks": [], "events": [],
    }
    for n in demo.nodes:
        entry = {k: n[k] for k in ("name", "depth", "parent", "offset")}
        entry["hash"] = "%08X" % n["hash"]
        if "params" in n:
            entry["params"] = {p["name"]: p["value"] for p in n["params"]}
        if "units" in n:
            u = n["units"]
            entry["units"] = {"unitCount": u["unitCount"], "trackCount": u["trackCount"], "frames": u["frames"],
                              "ticksPerFrame": u["ticksPerFrame"], "unknown": "%08X" % u["unknown"]}
        if "nameTable" in n:
            entry["nameTable"] = [{"hash": "%08X" % x["hash"], "name": x["name"]} for x in n["nameTable"]]
        result["tree"].append(entry)
    for b in demo.blocks:
        result["blocks"].append({"time": b["time"], "start": b["start"], "count": b["count"], "flags": b["flags"],
                                 "size": b["size"], "unknown18": b["unknown18"],
                                 "offsetVector": list(b["offsetVector"]) if b["offsetVector"] else None})
    samples = {}
    for index in sorted(demo.tracks):
        info = demo.tracks[index]
        per_block = demo.track_blocks(index)
        keys = []
        flags = Counter()
        for b, ks, tf in per_block:
            flags[tf] += 1
            if ks is None or b["start"] > frames:
                continue
            for f, v in ks:
                if f <= frames and (not keys or keys[-1][0] != f):
                    keys.append([f, rounded(v, 6)])
        actor = demo.actors[info["actor"]]
        unit_name = bone_names.get(info["unit"]) or HASH32_NAMES.get(info["unit"]) or "#%08X" % info["unit"]
        result["tracks"].append({"index": index, "actor": actor["target"], "actorKind": actor["kind"],
                                 "unit": unit_name, "kind": info["kind"], "kindName": info["kindName"],
                                 "bits": info["bits"], "unitFlags": info["unitFlags"],
                                 "trackFlags": {str(k): v for k, v in sorted(flags.items())}, "keys": keys})
        samples[index] = sample_blocks(per_block, info["kind"], frames)
    for ai, actor in enumerate(demo.actors):
        entry = {"target": actor["target"], "kind": actor["kind"], "path": actor["path"],
                 "units": [{"unit": bone_names.get(u["hash"]) or HASH32_NAMES.get(u["hash"]) or "#%08X" % u["hash"],
                            "flags": u["flags"], "tracks": u["tracks"]} for u in actor["units"]]}
        name = actor["target"]
        binding = {}
        if name:
            for key in ("locatorTypes", "cameraTypes", "modelFiles", "helpBoneFiles", "modelPartsDictionary"):
                table = stream_animation.get(key) or {}
                if name in table:
                    binding[key] = table[name]
            if name in (dd.get("controlCharacters") or {}):
                binding["controlCharacter"] = dd["controlCharacters"][name]
            for pname, p in (dd.get("entityParams") or {}).items():
                if name in (p.get("partNames") or {}):
                    binding["entityParam"] = {"name": pname, "part": p["partNames"][name]}
        entry["binding"] = binding
        inactive = [[s, c] for s, c, active in demo.actor_active(ai) if not active and s < frames]
        if inactive:
            entry["inactiveBlocks"] = inactive
        result["actors"].append(entry)
    cam = next((a for a in demo.actors if a["kind"] == "camera"), None)
    if cam:
        rot, pos = cam["units"][0]["tracks"][:2]
        focal = next((a["units"][0]["tracks"][0] for a in demo.actors if a["kind"] == "cameraParam"), None)
        rows = []
        for f in range(frames + 1):
            r, t = samples[rot][f], samples[pos][f]
            fl = samples[focal][f][0] if focal is not None and samples[focal][f] else None
            rows.append([f] + rounded(t) + rounded(r) + [round(fl, 4) if fl is not None else None])
        result["camera"] = {"columns": ["frame", "px", "py", "pz", "qx", "qy", "qz", "qw", "focalLength"],
                            "frames": rows}
    transforms = {}
    for actor in demo.actors:
        if actor["kind"] in ("locator", "modelRoot", "siFrame", "timeline", "motionPoints"):
            rows = []
            tracks = [t for u in actor["units"] for t in u["tracks"]]
            for f in range(frames + 1):
                row = [f]
                for t in tracks:
                    row.append(rounded(samples[t][f]))
                rows.append(row)
            transforms["%s:%s" % (actor["kind"], actor["target"])] = {
                "tracks": [demo.tracks[t]["kindName"] for t in tracks], "frames": rows}
    result["transforms"] = transforms
    if bone_frames:
        skinned = {}
        for actor in demo.actors:
            if actor["kind"] != "skeleton":
                continue
            bones = []
            for u in actor["units"]:
                rot = next((t for t in u["tracks"] if demo.tracks[t]["kind"] in (0, 5)), None)
                pos = next((t for t in u["tracks"] if demo.tracks[t]["kind"] not in (0, 5)), None)
                bones.append((bone_names.get(u["hash"]) or "#%08X" % u["hash"], rot, pos))
            rows = []
            for f in range(frames + 1):
                row = []
                for _, rot, pos in bones:
                    q = rounded(samples[rot][f]) if rot is not None else None
                    t = rounded(samples[pos][f]) if pos is not None else None
                    row.append((q or [None] * 4) + (t or [None] * 3))
                rows.append(row)
            skinned[actor["target"]] = {"bones": [b[0] for b in bones], "columns": ["qx", "qy", "qz", "qw",
                                                                                    "tx", "ty", "tz"],
                                        "frames": rows}
        result["skinned"] = skinned
    for packet in demo.event_packets:
        block_start = int(round(packet["time"] * FPS))
        loop = min(block_start // demo.length, demo.loops - 1) if demo.length else 0
        if not all_loops and loop > 0:
            continue
        for e in packet["events"]:
            if e["sections"] and e["sections"][0]["startOutsideBlock"]:
                continue  # continuation of an event listed in an earlier block
            entry = {"packetTime": round(packet["time"], 4), "loop": loop, "type": e["type"],
                     "sections": e["sections"]}
            if e["type"] == "ExecCommand":
                for k in ("functor", "functorLabel", "params", "interpolated", "length", "footer", "clipString",
                          "category", "rawInts"):
                    if k in e:
                        entry[k] = e[k]
            else:
                entry["ints"] = e["ints"]
                entry["floats"] = [round(x, 6) for x in e["floats"]]
                entry["strings"] = [STRING64.get(s, "#%012X" % s) for s in e["strings"]]
            result["events"].append(entry)
    result["events"].sort(key=lambda x: (x["sections"][0]["start"] if x["sections"] else -1))
    return result


def extend_string_table(demo_data, names_file=None):
    """Add model, locator, bone and message names so event strings resolve."""
    extra = set()
    for d in demo_data.values():
        sa = d.get("streamAnimation") or {}
        for key in ("locatorTypes", "cameraTypes", "modelFiles", "helpBoneFiles", "partsFiles"):
            extra.update((sa.get(key) or {}).keys())
            extra.update(v for v in (sa.get(key) or {}).values() if isinstance(v, str))
        extra.update((d.get("setupLights") or {}).keys())
        extra.update((d.get("fileParams") or {}).keys())
        extra.update((d.get("fileParams") or {}).values())
        for c in d.get("clips", []):
            extra.add(c.get("cameraName", ""))
    if names_file and Path(names_file).exists():
        extra.update(line.strip() for line in Path(names_file).read_text(encoding="utf-8", errors="replace").splitlines())
    for s in extra:
        if s:
            STRING64.setdefault(h64(s), s)


def summary_lines(demo, result):
    dd = demo.demo_data or {}
    lines = ["%s: %d frames (%.3f s at 59.94 fps), %d loop(s) in the stream (%d frames), %d motion blocks, "
             "%d event packets, chunks %s" % (demo.name, demo.length, demo.length / FPS, demo.loops,
                                               demo.stream_frames, len(demo.blocks), len(demo.event_packets),
                                               result["chunks"])]
    if dd:
        lines.append("  DemoData: demoLength %s, onMemory %s, clips %s, audio %s" % (
            dd.get("demoLength"), dd.get("onMemory"),
            ", ".join("%s %s %d-%d" % (c["name"], c["cameraName"], c["startFrame"], c["endFrame"])
                      for c in dd.get("clips", [])) or "none", dd.get("audioPath") or "none"))
    for a in result["actors"]:
        bind = a["binding"]
        b = ", ".join("%s=%s" % (k, v if not isinstance(v, dict) else v.get("characterId", v.get("name", v)))
                      for k, v in bind.items())
        lines.append("  actor %-14s %-34s units %3d tracks %3d%s%s" % (
            a["kind"], a["target"], len(a["units"]), sum(len(u["tracks"]) for u in a["units"]),
            ("  [" + b + "]") if b else "", ("  inactive blocks %d" % len(a["inactiveBlocks"]))
            if a.get("inactiveBlocks") else ""))
    cam = result.get("camera")
    if cam:
        f0, fl = cam["frames"][0], cam["frames"][-1]
        lines.append("  camera frame 0 pos %s rot %s focal %s; frame %d pos %s rot %s focal %s" % (
            f0[1:4], f0[4:8], f0[8], fl[0], fl[1:4], fl[4:8], fl[8]))
        if dd.get("cameraTranslation"):
            lines.append("  DemoData camera start %s %s param %s; end %s %s param %s" % (
                dd.get("cameraStartTranslation"), dd.get("cameraStartRotation"), dd.get("cameraStartParam"),
                dd.get("cameraTranslation"), dd.get("cameraRotation"), dd.get("cameraParam")))
    types = Counter(e["type"] for e in result["events"])
    functors = Counter("%s (%s)" % (e.get("functor"), e["functorLabel"]) if e.get("functorLabel") else e.get("functor")
                       for e in result["events"] if e["type"] == "ExecCommand")
    lines.append("  events (first loop): %s" % dict(types))
    if functors:
        lines.append("  functors: %s" % ", ".join("%s x%d" % (k, v) for k, v in functors.most_common()))
    msgs = [(e["sections"][0]["start"], e["params"].get("message", {}).get("value"))
            for e in result["events"] if e.get("functor") == "DemoSendMessageFunctor"]
    if msgs:
        lines.append("  messages: %s" % ", ".join("%s@%d" % (m, f) for f, m in msgs))
    return lines


# ---------------------------------------------------------------- placement check against the level data

def quat_rotate(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def demo_placements(levels_root):
    """demoId -> list of (level, how, locator name, position, rotation) from the level summaries."""
    out = defaultdict(list)
    objects = {}
    for f in sorted(Path(levels_root).glob("*.json")):
        try:
            j = json.loads(f.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            continue
        for fi in j.get("files", []):
            for o in fi.get("placedObjects", []) + fi.get("staticModels", []):
                objects[o["name"]] = (f.stem, o)
    for f in sorted(Path(levels_root).glob("*.json")):
        j = json.loads(f.read_text(encoding="utf-8"))
        for fi in j.get("files", []):
            for t in fi.get("traps", []):
                for c in t.get("conditions", []):
                    for e in c.get("exec", []):
                        if e.get("funcName") == "ShDemoExec":
                            # 0x91CE40: origin is demoCenterLink with orderDemoCenter, else the trap transform
                            link = e.get("demoCenterLink") if e.get("orderDemoCenter") else None
                            if not link:
                                objects[t["name"]] = (f.stem, t)
                                link = t["name"]
                            out[e["demoId"]].append((f.stem, "trap " + t["name"].split("|")[-1], link))
            for m in fi.get("messageScripts", []):
                prm = m.get("parameters") or {}
                if "demoCenterLink" in prm or "demoId" in prm:
                    demo = prm.get("demoId") or m.get("demoIdNext")
                    if demo:
                        out[demo].append((f.stem, "script " + m["name"].split("|")[-1],
                                          prm.get("demoCenterLink") if prm.get("orderDemoCenter") else None))
    return out, objects


def validate(results, levels_root, target):
    placements, objects = demo_placements(levels_root)
    lines = ["Demo placement check: demo-space camera and player paths mapped through the demo center locator",
             "(DemoDaemon.SetDemoTransform(rotQuat, translation) of the locator world transform). Positions are",
             "level file coordinates (dump/fox2/_levels). Camera forward is the camera's local -Z.", ""]
    for name, result in results:
        demo = name.split("_Eng")[0]
        if demo not in placements:
            continue
        cam = result.get("camera")
        roots = {k: v for k, v in result.get("transforms", {}).items() if k.startswith("modelRoot:")}
        if not cam and not roots:
            continue
        for level, how, link in placements[demo]:
            if link and link in objects:
                o = objects[link][1]
                rot, pos = tuple(o["rotation"]), tuple(o["position"])
                where = "%s at %s rot %s" % (link.split("|")[-1], [round(x, 3) for x in pos], rot)
            else:
                rot, pos = (0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0)
                where = "no demo center (DemoData transform, stage frame)"
            lines.append("%s via %s %s: %s" % (demo, level, how, where))

            def world(p):
                r = quat_rotate(rot, p)
                return tuple(round(r[i] + pos[i], 3) for i in range(3))
            points = []
            if cam:
                for row in (cam["frames"][0], cam["frames"][-1]):
                    wp = world(row[1:4])
                    fwd = quat_rotate(quat_mul(rot, tuple(row[4:8])), (0.0, 0.0, -1.0))
                    lines.append("    camera frame %4d world %s forward %s focal %s" % (
                        row[0], list(wp), [round(x, 2) for x in fwd], row[8]))
                    points.append(wp)
            for key, tr in roots.items():
                for row in (tr["frames"][0], tr["frames"][-1]):
                    if row[2] is None:
                        continue
                    wp = world(row[2])
                    lines.append("    %s frame %4d world %s" % (key, row[0], list(wp)))
                    points.append(wp)
            if points:
                near = []
                for oname, (lvl, o) in objects.items():
                    if lvl != level:
                        continue
                    d = min(math.dist(o["position"], p) for p in points)
                    near.append((d, oname.split("|")[-1], [round(x, 3) for x in o["position"]]))
                near.sort()
                lines.append("    nearest level objects: " + "; ".join("%s %s d=%.2f" % (n, p, d)
                                                                         for d, n, p in near[:5]))
        lines.append("")
    Path(target).write_text("\n".join(lines), encoding="utf-8")
    return lines


def main():
    ap = argparse.ArgumentParser(description="List a Fox demo stream (.fsm), extract its interleaved sound stream, "
                                             "and decode its DEMO packets.")
    ap.add_argument("files", type=Path, nargs="*", help="default: every demo stream under dump/")
    ap.add_argument("--extract", type=Path, metavar="DIR", help="write <name>.wem for files that carry SND chunks")
    ap.add_argument("--suffix", default="", help="text appended to the output file name")
    ap.add_argument("--json", type=Path, metavar="DIR", help="write <DIR>/<name>.json per demo and summary.txt")
    ap.add_argument("--fox2", type=Path, default=Path("dump/fox2"), help="fox2.py JSON root with the DemoData")
    ap.add_argument("--models", type=Path, default=Path("dump/models"), help="fmdl.py JSON root (bone names)")
    ap.add_argument("--names", type=Path, default=Path("dump/fox2/names.txt"), help="extra candidate strings")
    ap.add_argument("--no-bone-frames", action="store_true", help="omit per-frame bone arrays")
    ap.add_argument("--all-loops", action="store_true", help="sample and list events for the whole stream")
    ap.add_argument("--levels", type=Path, default=Path("dump/fox2/_levels"),
                    help="fox2.py level summaries for the placement check (written to validation.txt)")
    args = ap.parse_args()

    files = args.files
    if not files:
        files = []
        for d in DEFAULT_STREAMS:
            files += sorted(d.rglob("*.fsm"))
    demo_data = load_demo_data(args.fox2) if args.json else {}
    extend_string_table(demo_data, args.names)
    bone_names = load_bone_names(args.models) if args.json else {}
    summary = []
    results = []
    for path in files:
        data = path.read_bytes()
        chunks = read_chunks(data)
        counts = Counter(c["tag"] for c in chunks)
        end_time = next((c["time"] for c in chunks if c["tag"] == "END"), None)
        print("%s: %d bytes, %s, end time %s" % (path.name, len(data), dict(counts), end_time))
        stream = sound_stream(data, chunks)
        if stream is not None:
            print("  sound stream: %d bytes in %d SND chunks, kind %d, starts with %s" % (
                stream["total_size"], stream["chunk_count"], stream["stream_kind"], stream["data"][:4]))
            if args.extract:
                args.extract.mkdir(parents=True, exist_ok=True)
                target = args.extract / ("%s%s.wem" % (path.stem, args.suffix))
                target.write_bytes(stream["data"])
                print("  written %s" % target)
        if args.json:
            demo = Demo(path, demo_data.get(path.stem))
            result = demo_to_json(demo, bone_names, bone_frames=not args.no_bone_frames, all_loops=args.all_loops)
            name = demo.name if "#" not in str(path) else demo.name + "_" + path.parent.name.strip("#")
            if stream is not None:
                result["soundStream"] = {"bytes": stream["total_size"], "chunks": stream["chunk_count"],
                                         "kind": stream["stream_kind"]}
            args.json.mkdir(parents=True, exist_ok=True)
            target = args.json / (name + ".json")
            target.write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")
            lines = summary_lines(demo, result)
            lines[0] = lines[0].replace(demo.name + ":", name + ":", 1)
            summary += lines + [""]
            results.append((name, {k: result.get(k) for k in ("camera", "transforms")}))
            print("  %s: %d actors, %d tracks, %d events -> %s" % (name, len(result["actors"]), len(result["tracks"]),
                                                                  len(result["events"]), target))
    if args.json:
        (args.json / "summary.txt").write_text("\n".join(summary), encoding="utf-8")
        print("summary -> %s" % (args.json / "summary.txt"))
        if args.levels.exists():
            validate(results, args.levels, args.json / "validation.txt")
            print("placement check -> %s" % (args.json / "validation.txt"))


if __name__ == "__main__":
    main()
