"""Fox Engine motion files for P.T.: motion archives (.mtar), animations inside them (gani), rigs (.frig) and
help-bone drivers (.frdv). The animation tracks use the same node tree and key codec as the demo streams
(tools/fsm.py). Rig driven motions are also evaluated through the HumanFinger rig into model space bone poses.
Layouts and evidence are in docs/formats/motion.md.
"""
import argparse
import json
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import foxhash  # noqa: E402
import fsm  # noqa: E402
import pathcode  # noqa: E402

MTAR_MAGIC = 0x0C012B72
GANI_MAGIC = 0x0BFCA2D2
FRIG_MAGIC = 0x21EA256C
SKL_LIST = 0x91E4534B       # StrCode32("SKL_LIST"): bone name table
UNIT = 0xC6E937B9           # StrCode32("UNIT"): motion units with inline tracks
EVENTS = 0x1622762D         # event table node (data type 3, read by 0xAB2270)
ROOT_UNIT = 0x01AD535D      # model root (MOVE) unit, same hash in demo streams

DEFAULT_ARCHIVES = [Path("dump/fpk/resident_fpk/Assets/sh/motion/mtar/gimmick/ShGimmick_layers.mtar"),
                    Path("dump/fpk/player2_common_motion_fpk/Assets/sh/motion/mtar/player/ShPlayer_layers.mtar")]
DEFAULT_SETUP = Path("dump/lua/as/sh/level_asset/chara/gimmick/ShGimmickSetUp.lua")


def read_mtar(data):
    """Header: u32 magic, u32 count, u16 a, u16 b, 20 zero bytes; entries (u64 PathCode64, u32 offset, u32 size)."""
    magic, count, a, b = struct.unpack_from("<IIHH", data, 0)
    if magic != MTAR_MAGIC:
        raise ValueError("not a motion archive (magic %08X)" % magic)
    entries = []
    for i in range(count):
        code, offset, size = struct.unpack_from("<QII", data, 0x20 + 16 * i)
        entries.append({"pathCode": code, "offset": offset, "size": size})
    return {"count": count, "headerA": a, "headerB": b, "entries": entries}


def motion_paths(setup_lua):
    """PathCode64 -> (motion key, path) from Gimmick.AddMotionPath calls."""
    out = {}
    if setup_lua and Path(setup_lua).exists():
        text = Path(setup_lua).read_text(encoding="utf-8", errors="replace")
        for key, path in re.findall(r'key\s*=\s*"(\w+)",\s*path\s*=\s*"([^"]+)"', text):
            out[pathcode.raw_path_code64(path)] = (key, path)
    return out


def read_gani(data, offset):
    """Gani: u32 magic 0x0BFCA2D2, u32 header size (0x20), u32 size, u32 0, then the node tree."""
    magic, header, size, zero = struct.unpack_from("<4I", data, offset)
    if magic != GANI_MAGIC:
        raise ValueError("not a gani at 0x%X (magic %08X)" % (offset, magic))
    nodes = fsm.parse_tree(data, offset + header)
    gani = {"offset": offset, "size": size, "nodes": nodes, "bones": {}, "units": None, "events": []}
    for n in nodes:
        if n["hash"] == SKL_LIST and "nameTable" in n:
            gani["bones"] = {e["hash"]: e["name"] for e in n["nameTable"]}
        elif n["hash"] == UNIT and "units" in n:
            gani["units"] = n["units"]
        elif n["hash"] == EVENTS and "rawData" in n:
            # two levels (0xAB2270): a set (u32 hash, u16 count, i32 offsets) of event tables with the
            # demo event table layout (u32 track hash, u16 count, i32 offsets, records as in DEMO type 2)
            top = n["rawData"][0]
            set_hash, set_count = struct.unpack_from("<IH", data, top)
            for toff in struct.unpack_from("<%di" % set_count, data, top + 8):
                table = top + toff
                table_hash, count = struct.unpack_from("<IH", data, table)
                offsets = struct.unpack_from("<%di" % count, data, table + 8)
                for off in offsets:
                    ev = fsm.decode_event(data, table + off, offset + size)
                    ev["set"] = "%08X" % set_hash
                    ev["table"] = "%08X" % table_hash
                    gani["events"].append(ev)
        elif n["name"] == "MOTION" and "params" in n:
            gani["params"] = {p["name"]: p["value"] for p in n["params"]}
    return gani


def unit_name(gani, unit_hash, bone_names):
    if unit_hash in gani["bones"]:
        return gani["bones"][unit_hash]
    if unit_hash == ROOT_UNIT:
        return "(root)"
    if unit_hash == fsm.h32("RIG_ROOT"):
        return "RIG_ROOT"
    if unit_hash in bone_names:
        return bone_names[unit_hash]
    return RIG_UNIT_NAMES.get(unit_hash, "#%08X" % unit_hash)


def decode_gani(data, gani, bone_names):
    """Keys per track (inline data, offsets relative to the track entry; 0xAAC010) and per-frame samples."""
    units = gani["units"]
    frames = units["frames"]
    out_units = []
    for u in units["units"]:
        static = bool(u["flags"] & 4)
        tracks = []
        for t in u["tracks"]:
            keys = fsm.read_keys(data, t["dataOffset"], t["kind"], t["bits"], static, frames)
            block = {"start": 0, "count": frames}
            samples = fsm.sample_blocks([(block, keys, 0)], t["kind"], frames)
            tracks.append({"index": t["index"], "kind": t["kind"], "kindName": fsm.KIND_NAMES.get(t["kind"]),
                           "bits": t["bits"], "keys": keys, "samples": samples})
        out_units.append({"hash": u["hash"], "name": unit_name(gani, u["hash"], bone_names), "flags": u["flags"],
                          "static": static, "loop": bool(u["flags"] & 1), "tracks": tracks})
    return out_units


def gani_to_json(name, key, path, gani, units):
    frames = gani["units"]["frames"]
    result = {"name": name, "motionKey": key, "path": path, "frames": frames,
              "ticksPerFrame": gani["units"]["ticksPerFrame"], "unitsUnknown": "%08X" % gani["units"]["unknown"],
              "params": gani.get("params"), "bones": [], "events": [], "perFrame": {}}
    for u in units:
        entry = {"unit": u["name"], "hash": "%08X" % u["hash"], "flags": u["flags"], "static": u["static"],
                 "loop": u["loop"], "tracks": []}
        for t in u["tracks"]:
            entry["tracks"].append({"index": t["index"], "kindName": t["kindName"], "bits": t["bits"],
                                    "keyCount": len(t["keys"]),
                                    "keys": [[f, fsm.rounded(v, 6)] for f, v in t["keys"]]})
        result["bones"].append(entry)
        rot = next((t for t in u["tracks"] if t["kind"] in (0, 5)), None)
        pos = next((t for t in u["tracks"] if t["kind"] not in (0, 5)), None)
        rows = []
        for f in range(frames + 1):
            q = fsm.rounded(rot["samples"][f]) if rot else None
            p = fsm.rounded(pos["samples"][f]) if pos else None
            rows.append((q or [None] * 4) + (p or [None] * 3))
        result["perFrame"][u["name"]] = rows
    result["perFrameColumns"] = ["qx", "qy", "qz", "qw", "tx", "ty", "tz"]
    for ev in gani["events"]:
        result["events"].append({"set": ev["set"], "table": ev["table"], "type": ev["type"], "sections": ev["sections"],
                                 "ints": ev["ints"], "floats": ev["floats"],
                                 "strings": ["#%012X" % s for s in ev["strings"]]})
    return result


RIG_BONES = ["SKL_000_WAIST", "SKL_001_SPINE", "SKL_002_CHEST", "SKL_003_NECK", "SKL_004_HEAD", "SKL_010_LSHLD", "SKL_013_LHAND",
             "SKL_020_RSHLD", "SKL_023_RHAND", "SKL_030_LTHIGH", "SKL_032_LFOOT", "SKL_033_LTOE", "SKL_040_RTHIGH", "SKL_042_RFOOT",
             "SKL_043_RTOE", "SKL_101_LF10", "SKL_201_RF10"]
# gani units of rig driven motions: StrCode32("RIG_" + first joint bone name), confirmed for all 17 HumanFinger units
RIG_UNIT_NAMES = {fsm.h32("RIG_" + b): "RIG_" + b for b in RIG_BONES}
RIG_TYPES = {1: "root", 2: "rotation", 3: "leg", 4: "localRotation", 7: "waist", 8: "arm", 11: "chain"}
DEFAULT_RIG = Path("dump/fpk/resident_fpk/Assets/sh/rig/frig/human_finger.frig")
RIG_SKELETONS = {"gimmick": Path("dump/models/resident/och0_main0_def.json"), "player": Path("dump/models/resident/plr0_main0_def.json")}


def read_frig(data):
    """Rig (.frig): u32 magic 0x21EA256C, name offset, 0x66, unit count, track count, file size, joint table offset,
    mask table offset, unit offsets. Unit: u32 type, u16 tracks, u16 joints, i16 parent joint, i16 parent unit, then a
    per-type layout (joints and tracks as u16 lists; legs and arms: hinge axis at +0x20). Joint table: u32 count, (u32 unit,
    u32 bone StrCode32). Masks: u32 unit count, u32 count, offsets; each u32 hash, char[12] name, float weight per unit."""
    magic, name_off, f8, count, tracks, size, joints_off, masks_off = struct.unpack_from("<8I", data, 0)
    if magic != FRIG_MAGIC:
        raise ValueError("not a rig (magic %08X)" % magic)
    rig = {"name": fsm.cstring(data, name_off), "trackCount": tracks, "size": size, "field8": f8, "units": [], "joints": [],
           "masks": []}
    for o in struct.unpack_from("<%dI" % count, data, 0x20):
        kind, ntracks, njoints, parent_joint, parent_unit = struct.unpack_from("<IHHhh", data, o)

        def w(k):
            return struct.unpack_from("<h", data, o + k)[0]
        u = {"type": kind, "typeName": RIG_TYPES.get(kind), "parentJoint": parent_joint, "parentUnit": parent_unit}
        if kind == 1:
            u.update(joints=[], tracks=[w(0x10), w(0x12)])
        elif kind in (2, 4):
            u.update(joints=[w(0x10)], tracks=[w(0x12)])
        elif kind == 7:
            u.update(joints=[w(0x10)], tracks=[w(0x12), w(0x14)])
        elif kind == 11:
            u.update(joints=[w(0x10) + i for i in range(njoints)], tracks=[w(0x12) + i for i in range(ntracks)])
        elif kind == 3:
            u.update(joints=[w(0x30), w(0x32)], tracks=[w(0x34), w(0x36)], end=w(0x38),
                     axis=list(struct.unpack_from("<3f", data, o + 0x20)))
        elif kind == 8:
            u.update(joints=[w(0x30), w(0x32), w(0x34)], tracks=[w(0x36), w(0x38), w(0x3A)], end=w(0x3C),
                     axis=list(struct.unpack_from("<3f", data, o + 0x20)))
        else:
            u.update(joints=[], tracks=[], unsupported=True)
        rig["units"].append(u)
    n = struct.unpack_from("<I", data, joints_off)[0]
    rig["joints"] = [list(struct.unpack_from("<II", data, joints_off + 4 + 8 * i)) for i in range(n)]
    weights, masks = struct.unpack_from("<II", data, masks_off)
    for off in struct.unpack_from("<%dI" % masks, data, masks_off + 8):
        m = masks_off + off
        rig["masks"].append({"hash": "%08X" % struct.unpack_from("<I", data, m)[0], "name": fsm.cstring(data, m + 4),
                             "weights": list(struct.unpack_from("<%df" % weights, data, m + 0x10))})
    return rig


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + bw * ax + ay * bz - az * by, aw * by + bw * ay + az * bx - ax * bz,
            aw * bz + bw * az + ax * by - ay * bx, aw * bw - ax * bx - ay * by - az * bz)


def _qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _scale(a, k):
    return tuple(x * k for x in a)


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _unit(a):
    n = math.sqrt(_dot(a, a))
    return _scale(a, 1.0 / n) if n > 0 else a


def _qrot(q, v):
    u = q[:3]
    t = _scale(_cross(u, v), 2.0)
    return _add(_add(v, _scale(t, q[3])), _cross(u, t))


def _frame_quat(wa, wb, la, lb):
    """Rotation taking la -> wa, lb -> wb, lb x la -> wb x wa (sum of outer products, then a quaternion)."""
    wc, lc = _cross(wb, wa), _cross(lb, la)
    m = [[wa[i] * la[j] + wb[i] * lb[j] + wc[i] * lc[j] for j in range(3)] for i in range(3)]
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s)
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        q = (0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s)
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        q = ((m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s)
    else:
        s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
        q = ((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s)
    n = math.sqrt(_dot(q, q))
    return tuple(c / n for c in q)


def _two_bone(start, target, pole, axis, upper_local, lower_local):
    """0xAC0F50 (leg) / 0xAB4770 (arm): law of cosines toward target, knee in the plane of the pole, hinge about axis."""
    d = _sub(target, start)
    dist = math.sqrt(_dot(d, d))
    l1, l2 = math.sqrt(_dot(upper_local, upper_local)), math.sqrt(_dot(lower_local, lower_local))
    dn = _scale(d, 1.0 / dist)
    e = _unit(_cross(d, _cross(pole, d)))
    x = max((dist * dist + l1 * l1 - l2 * l2) / (2 * dist), 0.0) if dist < l1 + l2 else l1
    h = math.sqrt(max(l1 * l1 - x * x, 0.0))
    knee = _add(_scale(e, h), _scale(dn, x))
    hinge = _cross(e, dn)
    upper = _frame_quat(hinge, _unit(knee), axis, _scale(upper_local, 1.0 / l1))
    lower = _frame_quat(hinge, _unit(_sub(d, knee)), axis, _scale(lower_local, 1.0 / l2))
    return upper, lower


def load_skeleton(model_json):
    """fmdl.py JSON sidecar: bone names, parents and bind world positions (bind local = world - parent world)."""
    bones = json.loads(Path(model_json).read_text(encoding="utf-8"))["bones"]
    names = [b["name"] for b in bones]
    parents = [b["parent"] for b in bones]
    world = [tuple(b["world"][:3]) for b in bones]
    local = [_sub(world[i], world[p]) if p >= 0 else world[i] for i, p in enumerate(parents)]
    return {"model": str(model_json).replace("\\", "/"), "names": names, "parents": parents, "local": local,
            "index": {fsm.h32(n): i for i, n in enumerate(names)}}


def rig_channels(rig, units, frame):
    """Channel values by track index at an integer frame; IK targets moved from motion space into root space (0xAC2BD0)."""
    ch = {}
    for u in units:
        for t in u["tracks"]:
            ch[t["index"]] = tuple(t["samples"][min(frame, len(t["samples"]) - 1)])
    root = rig["units"][0]
    q_root, p_root = ch[root["tracks"][0]], ch[root["tracks"][1]][:3]
    for u in rig["units"]:
        k = u["tracks"][0] if u["type"] == 3 else u["tracks"][1] if u["type"] == 8 else None
        if k is not None:
            ch[k] = _qrot(_qconj(q_root), _sub(ch[k][:3], p_root))
    return ch


def evaluate_rig(rig, skel, ch):
    """Model space (rotation, position) per skeleton bone; rig units in order, then every other bone follows its parent
    (0xAC3E60, 0xAA2BB0). Checked against the original functions executed natively (docs/formats/motion.md)."""
    n = len(skel["names"])
    rot, pos = [None] * n, [None] * n
    bone = [skel["index"].get(h, -1) for _, h in rig["joints"]]

    def parent(b):
        p = skel["parents"][b]
        return ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0)) if p < 0 else (rot[p], pos[p])

    def place(b, q, local):
        qp, pp = parent(b)
        rot[b] = _qmul(qp, q) if local else q
        pos[b] = _add(pp, _qrot(qp, skel["local"][b]))

    for u in rig["units"]:
        kind, tr = u["type"], u["tracks"]
        if kind == 7:
            b = bone[u["joints"][0]]
            rot[b], pos[b] = ch[tr[0]], ch[tr[1]][:3]
        elif kind == 2:
            place(bone[u["joints"][0]], ch[tr[0]], False)
        elif kind in (4, 11):
            for j, k in zip(u["joints"], tr):
                place(bone[j], ch[k], True)
        elif kind == 3:
            b0, b1, end = bone[u["joints"][0]], bone[u["joints"][1]], bone[u["end"]]
            qp, pp = parent(b0)
            hip = _add(pp, _qrot(qp, skel["local"][b0]))
            pole = _qrot(ch[tr[1]], (1.0, 0.0, 0.0))
            r1, r2 = _two_bone(hip, ch[tr[0]][:3], pole, tuple(u["axis"]), skel["local"][b1], skel["local"][end])
            rot[b0], pos[b0] = r1, hip
            rot[b1], pos[b1] = r2, _add(hip, _qrot(r1, skel["local"][b1]))
        elif kind == 8:
            b0, b1, b2, end = (bone[j] for j in u["joints"] + [u["end"]])
            place(b0, ch[tr[0]], False)
            shoulder = _add(pos[b0], _qrot(rot[b0], skel["local"][b1]))
            pole = _qrot(ch[tr[2]], (1.0, 0.0, 0.0))
            r1, r2 = _two_bone(shoulder, ch[tr[1]][:3], pole, tuple(u["axis"]), skel["local"][b2], skel["local"][end])
            rot[b1], pos[b1] = r1, shoulder
            rot[b2], pos[b2] = r2, _add(shoulder, _qrot(r1, skel["local"][b2]))
    for b in range(n):
        if rot[b] is None:
            place(b, (0.0, 0.0, 0.0, 1.0), True)
    return rot, pos


def rig_to_json(rig, skel, units, frames, step):
    rows = []
    for f in sorted(set(list(range(0, frames + 1, step)) + [frames])):
        rot, pos = evaluate_rig(rig, skel, rig_channels(rig, units, f))
        rows.append({"frame": f, "position": [fsm.rounded(p, 6) for p in pos], "rotation": [fsm.rounded(q, 6) for q in rot]})
    return {"rig": rig["name"], "skeleton": skel["model"], "bones": skel["names"], "frames": rows}


HELP_TYPES = {1: "slideByAngle", 2: "follow", 7: "twist", 11: "swingTurn", 12: "twistSlide", 13: "swingTurnSlide"}


def read_frdv(data):
    """Help-bone driver file: 'FRDV', u32 version, u32 count, u32 0, u32 entryOffsets[count], 0x80-byte entries:
    u16 type, i16 driven, i16 source, i16 (unused here), i16 parent, i16 reference (-1: none), f32 weight (+0x10),
    f32 min and max (+0x18, +0x1C), u32 axis (+0x20), f32 slide (+0x24), f32 slide min and max (+0x2C, +0x30),
    u32 slide axis (+0x34), vec4 a (+0x40), vec4 b (+0x50)."""
    tag, version, count = struct.unpack_from("<4sII", data, 0)
    offsets = struct.unpack_from("<%dI" % count, data, 0x10)
    entries = []
    for o in offsets:
        kind, driven, source, unused, parent, reference = struct.unpack_from("<H5h", data, o)
        weight, _, lo, hi, axis, slide, _, slide_lo, slide_hi, slide_axis = struct.unpack_from("<4fIf3fI", data, o + 0x10)
        entries.append({"type": kind, "typeName": HELP_TYPES.get(kind), "driven": driven, "source": source, "parent": parent,
                        "reference": reference, "weight": weight, "min": lo, "max": hi, "axis": axis, "slide": slide,
                        "slideMin": slide_lo, "slideMax": slide_hi, "slideAxis": slide_axis,
                        "a": list(struct.unpack_from("<3f", data, o + 0x40)), "b": list(struct.unpack_from("<3f", data, o + 0x50))})
    return {"tag": tag.decode("ascii", "replace"), "version": "%08X" % version, "count": count,
            "entryOffsets": list(offsets), "entries": entries}


def _qnorm(q):
    n = math.sqrt(sum(x * x for x in q))
    return tuple(x / n for x in q)


def _hb_slerp(q, t):
    """Slerp from the identity as 0xAE7420 inlines it: the shorter way, linear weights from |dot| >= 0.999."""
    a, d = (0.0, 0.0, 0.0, 1.0), q[3]
    if d < 0:
        a, d = (0.0, 0.0, 0.0, -1.0), -d
    d = min(d, 1.0)
    if d >= 0.999:
        w0, w1 = 1 - t, t
    else:
        th = math.acos(d)
        w0, w1 = math.sin((1 - t) * th) / math.sin(th), math.sin(t * th) / math.sin(th)
    return _qnorm(tuple(w0 * x + w1 * y for x, y in zip(a, q)))


def _hb_arc(a, b):
    """Shortest arc from a to b; a half turn about a x (basis axis of a's smallest component) when they are opposite."""
    d = _dot(a, b)
    if d + 1.0 <= 1e-5:
        e, m = (0.0, 1.0, 0.0), abs(a[1])
        if abs(a[0]) < m:
            e, m = (1.0, 0.0, 0.0), abs(a[0])
        if abs(a[2]) <= m:
            e = (0.0, 0.0, 1.0)
        return tuple(_unit(_cross(a, e))) + (0.0,)
    s = _add(a, b)
    if _dot(s, s) < 1e-6:
        return (0.0, 0.0, 0.0, 1.0)
    k = math.sqrt(2.0 + 2.0 * d)
    return tuple(_scale(_cross(a, b), 1.0 / k)) + (k * 0.5,)


def _hb_twist(q, axis, weight):
    t = _qmul(_qconj(_hb_arc(axis, _qrot(q, axis))), q)
    return _hb_slerp(t, weight) if weight >= 0 else _qconj(_hb_slerp(t, -weight))


def _hb_limit(v, lo, hi):
    if v - lo < 0:
        v = lo
    if hi - v < 0:
        v = hi
    return v


def evaluate_help_bones(entries, rot, pos, local):
    """0xAE7420 for the entry types of P.T.'s files, on model space rotations (x, y, z, w) and positions, in entry order.
    Checked against the original run natively (docs/formats/motion.md, section .frdv)."""
    rot, pos = list(rot), [tuple(p[:3]) for p in pos]
    for e in entries:
        kind = e["type"]
        if kind not in HELP_TYPES:
            continue
        parent = rot[e["parent"]]
        q = rot[e["source"]] if e["reference"] == -1 else _qmul(_qconj(rot[e["reference"]]), rot[e["source"]])
        a, b = tuple(e["a"]), tuple(e["b"])
        offset = list(local[e["driven"]][:3])
        if kind in (12, 13) and e["slideAxis"] & 3 < 3:
            offset[e["slideAxis"] & 3] = 0.1 * _hb_limit(e["slide"] * _dot(b, _qrot(q, a)), e["slideMin"], e["slideMax"])
        result = parent
        if kind == 1:
            deg = _hb_limit(e["weight"] * 114.59155 * math.acos(min(abs(q[3]), 1.0)), e["min"], e["max"])
            if e["axis"] & 3 < 3:
                offset[e["axis"] & 3] = deg * 0.1
        elif kind == 2:
            result = _qnorm(_qmul(parent, _hb_slerp(q, e["weight"])))
        elif kind in (7, 12):
            result = _qnorm(_qmul(parent, _hb_twist(q, a, e["weight"])))
        else:
            v = _qrot(q, a)
            half = max(-1.0, min(1.0, math.sqrt(max(0.0, 2.0 * (_dot(a, v) + 1.0))) * 0.5))
            theta = 2.0 * math.acos(half)
            x, y = _dot(b, v), -_dot(_cross(a, b), v)
            g = 2.0 / math.pi * math.atan2(abs(x), abs(y) + 1e-10)
            if kind == 11:
                g = g if x >= 0 else -g
            else:
                g = 1.0 - g if y >= 0 else g - 1.0
            angle = _hb_limit(e["weight"] * g * theta, e["min"] * 0.017453294, e["max"] * 0.017453294)
            s = [0.0, 0.0, 0.0]
            s[e["axis"]] = math.sin(angle * 0.5)
            result = _qnorm(_qmul(parent, tuple(s) + (math.cos(angle * 0.5),)))
        rot[e["driven"]] = result
        pos[e["driven"]] = _add(pos[e["parent"]], _qrot(parent, tuple(offset)))
    return rot, pos


def help_bones_json(name, frdv_path, skel, entries, count, seed=1):
    """Random model space poses (near-identity to large relative rotations) and the evaluated help bones."""
    import random
    rnd = random.Random(seed)

    def rq(spread):
        axis = _unit(tuple(rnd.gauss(0, 1) for _ in range(3)))
        ang = rnd.uniform(-math.pi, math.pi) * spread
        return tuple(_scale(axis, math.sin(ang / 2))) + (math.cos(ang / 2),)
    n = len(skel["names"])
    poses = []
    for i in range(count):
        base = rq(1.0)
        rot = [_qnorm(_qmul(base, rq([0.05, 0.3, 1.0][i % 3]))) for _ in range(n)]
        pos = [tuple(rnd.uniform(-1, 1) for _ in range(3)) for _ in range(n)]
        out_rot, out_pos = evaluate_help_bones(entries, rot, pos, skel["local"])
        poses.append({"rotation": [fsm.rounded(q, 7) for q in rot], "position": [fsm.rounded(p, 7) for p in pos],
                      "outRotation": [fsm.rounded(q, 7) for q in out_rot], "outPosition": [fsm.rounded(p, 7) for p in out_pos]})
    return {"name": name, "frdv": frdv_path, "fmdl": "/Assets/sh/chara/%s/Scenes/%s.fmdl" % (name[:3], name),
            "skeleton": skel["model"], "bones": skel["names"], "entries": entries, "poses": poses}


def main():
    ap = argparse.ArgumentParser(description="Fox motion archives (.mtar/.gani), rigs (.frig) and .frdv files.")
    ap.add_argument("files", type=Path, nargs="*", help=".mtar, .frig or .frdv files (default: the P.T. archives)")
    ap.add_argument("--setup", type=Path, default=DEFAULT_SETUP, help="ShGimmickSetUp.lua for motion key names")
    ap.add_argument("--models", type=Path, default=Path("dump/models"), help="fmdl.py JSON root (bone names)")
    ap.add_argument("--json", type=Path, metavar="DIR", help="write DIR/motion_<key or hash>.json per animation")
    ap.add_argument("--only", nargs="*", help="motion keys to write (default: all)")
    ap.add_argument("--rig", type=Path, default=DEFAULT_RIG, help="rig for rig driven motions (JSON section 'rig')")
    ap.add_argument("--rig-skeleton", type=Path, help="fmdl.py JSON of the posed model (default: och0 for gimmick, plr0 for player)")
    ap.add_argument("--rig-step", type=int, default=30, help="frame step of the evaluated rig poses in the JSON")
    ap.add_argument("--help-poses", type=int, default=0, metavar="N",
                    help="with --json and .frdv files: DIR/helpbones_<model>.json with N random poses and their help bones")
    args = ap.parse_args()

    files = args.files or DEFAULT_ARCHIVES
    names = motion_paths(args.setup)
    bone_names = fsm.load_bone_names(args.models)
    for path in files:
        data = path.read_bytes()
        magic = struct.unpack_from("<I", data, 0)[0]
        if magic == FRIG_MAGIC:
            print("%s: %s" % (path.name, json.dumps(read_frig(data))))
            continue
        if data[:4] == b"FRDV":
            frdv = read_frdv(data)
            if args.json and args.help_poses:
                skel = load_skeleton(args.models / "resident" / (path.stem + ".json"))
                result = help_bones_json(path.stem, "/Assets/sh/chara/%s/Scenes/%s" % (path.stem[:3], path.name), skel,
                                         frdv["entries"], args.help_poses)
                args.json.mkdir(parents=True, exist_ok=True)
                target = args.json / ("helpbones_%s.json" % path.stem)
                target.write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")
                print("%s: %d entries -> %s" % (path.name, frdv["count"], target))
            else:
                print("%s: %s" % (path.name, json.dumps(frdv)))
            continue
        mtar = read_mtar(data)
        print("%s: %d animations, header %04X %04X" % (path.name, mtar["count"], mtar["headerA"], mtar["headerB"]))
        for e in mtar["entries"]:
            key, gpath = names.get(e["pathCode"], (None, None))
            gani = read_gani(data, e["offset"])
            label = key or "%016X" % e["pathCode"]
            u = gani["units"]
            print("  %-16s %016X  %6d bytes  frames %5d  units %3d  tracks %3d  bones %2d  events %d  %s" % (
                label, e["pathCode"], e["size"], u["frames"] if u else 0, u["unitCount"] if u else 0,
                u["trackCount"] if u else 0, len(gani["bones"]), len(gani["events"]), gpath or ""))
            if args.json and u and (not args.only or key in args.only):
                units = decode_gani(data, gani, bone_names)
                result = gani_to_json(label, key, gpath, gani, units)
                result["archive"] = str(path).replace("\\", "/")
                skel_path = args.rig_skeleton or RIG_SKELETONS["player" if "player" in str(path).lower() else "gimmick"]
                if u["unknown"] & 1 and args.rig.exists() and skel_path.exists():
                    rig = read_frig(args.rig.read_bytes())
                    result["rig"] = rig_to_json(rig, load_skeleton(skel_path), units, u["frames"], max(1, args.rig_step))
                args.json.mkdir(parents=True, exist_ok=True)
                target = args.json / ("motion_%s.json" % label)
                target.write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")
                print("    -> %s" % target)


if __name__ == "__main__":
    main()
