import argparse
import json
import os
import struct
import sys
import traceback
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fmdl

REPO = Path(__file__).resolve().parent.parent
FPK_ROOT = REPO / "dump" / "fpk"
GEOM_ROOT = REPO / "dump" / "geom"

FOXDATA_VERSION = 201209110
NAME_GEOM = 0x006B936E
HEADER = struct.Struct("<IIIIIIQ")
NODE = struct.Struct("<IIIiIiiiii")
BLOCK = struct.Struct("<BBHHHHHIIIQ")
BLOCK_STRIDE = 0x20
SHAPE = struct.Struct("<IIIIQII")
SHAPE_SIZE = 0x20
VERTEX_HEADER = struct.Struct("<IIIIqII")
MATERIAL = struct.Struct("<IQ")

NODE_FLAGS_GROUP = 6

PRIM_POLY = 2
PRIM_BOX = 3
PRIM_AABB = 4
PRIM_NAMES = {0: "DOT", 1: "LINE", 2: "POLY", 3: "BOX", 4: "AABB", 5: "REFERENCE", 8: "PATH", 11: "FREE_AREA"}

SHAPE_DOUBLE_SIDED = 0x200
SHAPE_FMDL_VERTICES = 0x800

COLLISION_TAG_NAMES = {
    1: "RECOIL", 2: "CHARA", 3: "SOUND", 4: "PLAYER", 5: "ENEMY", 6: "BULLET", 7: "MISSILE", 8: "BOMB", 10: "BLOOD",
    11: "IK", 12: "STAIRWAY", 13: "STOP_EYE", 14: "CLIFF", 17: "DONT_FALL", 18: "CAMERA", 22: "CLIFF_FLOOR",
    23: "BULLET_MARK", 24: "HEIGHT_LIMIT", 29: "DOUBLE_SIDE", 30: "WATER_SURFACE", 31: "CHARA_2", 32: "TARGET_BLOCK",
    33: "DOG", 35: "NO_EFFECT", 36: "EVENT_PHYSICS", 37: "NO_WALL_MOVE", 38: "MISSILE2", 40: "RESERVE1",
    41: "RESERVE2", 42: "RESERVE3", 43: "RESERVE4", 48: "SAHELAN", 49: "RIDE_ON_OUTER", 50: "FLAME",
    51: "IGNORE_PHYSICS", 52: "CLIMB", 53: "HORSE", 54: "VEHICLE", 55: "MARKER", 56: "RIDE_ON",
    57: "THROUGH_LINE_OF_FIRE", 58: "THROUGH_ITEM_CHECK", 59: "NO_CREEP", 60: "NO_FULTON", 61: "FULTON", 62: "ITEM",
    63: "BOSS",
}


class GeomError(Exception):
    pass


def signed28(value):
    value &= 0x0FFFFFFF
    return value - 0x10000000 if value & 0x08000000 else value


def tag_names(tags):
    return [COLLISION_TAG_NAMES.get(bit, "bit%d" % bit) for bit in range(64) if tags >> bit & 1]


def read_nodes(data, offset, depth=0, parent=None, out=None):
    out = [] if out is None else out
    while True:
        name_hash, name_offset, flags, data_offset, data_size, parent_offset, child_offset, previous_offset, next_offset, params_offset = NODE.unpack_from(data, offset)
        node = {"offset": offset, "depth": depth, "parent": parent, "name_hash": name_hash, "name_offset": name_offset,
                "flags": flags, "data_offset": offset + data_offset if data_offset else 0, "data_size": data_size,
                "params_offset": params_offset, "index": len(out)}
        out.append(node)
        if child_offset:
            read_nodes(data, offset + child_offset, depth + 1, node["index"], out)
        if not next_offset:
            return out
        offset += next_offset


def read_shape(data, offset):
    info, next_raw, previous_raw, child_raw, tags, name, vertex_raw = SHAPE.unpack_from(data, offset)
    shape = {
        "offset": offset,
        "prim_type": info & 0xF,
        "flags": (info >> 4) & 0xFFFFF,
        "prim_count": info >> 24,
        "next": signed28(next_raw) * 16,
        "previous": signed28(previous_raw) * 16,
        "child": signed28(child_raw) * 16,
        "tags": tags,
        "name": name,
        "vertex_header": signed28(vertex_raw) * 16,
    }
    if (next_raw | previous_raw | child_raw | vertex_raw) >> 28:
        raise GeomError("shape at 0x%X uses the top offset bits" % offset)
    return shape


def read_prims(data, shape):
    base = shape["offset"] + SHAPE_SIZE
    kind = shape["prim_type"]
    count = shape["prim_count"]
    if kind == PRIM_POLY:
        prims = []
        for i in range(count):
            a, b, c, d, info = struct.unpack_from("<HHHHH", data, base + i * 10)
            prims.append({"indices": (a, b, c, d), "info": info})
        return prims
    if kind == PRIM_AABB:
        return [{"radii": list(struct.unpack_from("<3f", data, base + i * 32)),
                 "center": list(struct.unpack_from("<3f", data, base + i * 32 + 16))} for i in range(count)]
    if kind == PRIM_BOX:
        return [{"scale": list(struct.unpack_from("<4f", data, base + i * 96)),
                 "matrix": list(struct.unpack_from("<16f", data, base + i * 96 + 32))} for i in range(count)]
    raise GeomError("unsupported primitive type %d at 0x%X" % (kind, shape["offset"]))


def read_vertex_header(data, offset):
    count, index_offset, relative, origin_index, data_offset, fmdl_offset, padding = VERTEX_HEADER.unpack_from(data, offset)
    return {"offset": offset, "count": count, "index_offset": index_offset, "relative": relative,
            "origin_index": origin_index, "data_offset": data_offset, "fmdl_offset": fmdl_offset, "padding": padding}


def read_block(data, offset):
    final, header_count, headers_size, vb_legacy, pad0, headers_legacy, pad1, vertex_offset, headers_offset, next_offset, tags = BLOCK.unpack_from(data, offset)
    block = {"offset": offset, "header_count": header_count, "headers_size": headers_size,
             "legacy_offsets": [vb_legacy, headers_legacy], "vertex_header_offset": offset + vertex_offset,
             "shapes_offset": offset + headers_offset, "next_section": next_offset, "tags": tags, "shapes": []}
    stack = [(offset + headers_offset, 0, None)]
    seen = set()
    while stack:
        shape_offset, depth, parent = stack.pop()
        if shape_offset in seen:
            raise GeomError("shape loop at 0x%X" % shape_offset)
        seen.add(shape_offset)
        shape = read_shape(data, shape_offset)
        shape["depth"] = depth
        shape["parent"] = parent
        shape["prims"] = read_prims(data, shape)
        if shape["vertex_header"]:
            shape["vertices"] = read_vertex_header(data, shape_offset + shape["vertex_header"])
        index = len(block["shapes"])
        block["shapes"].append(shape)
        if shape["next"]:
            stack.append((shape_offset + shape["next"], depth, parent))
        if shape["child"]:
            stack.append((shape_offset + shape["child"], depth + 1, index))
    if len(block["shapes"]) != header_count:
        raise GeomError("block at 0x%X: walked %d shapes, header says %d" % (offset, len(block["shapes"]), header_count))
    return block


def read_group(data, offset, size):
    group = {"offset": offset, "size": size, "blocks": [], "materials": [], "aux_materials": []}
    position = offset
    while data[position] == 0:
        group["blocks"].append(read_block(data, position))
        position += BLOCK_STRIDE
    materials_base = position + BLOCK_STRIDE
    first, total, aux_first, aux_total = struct.unpack_from("<BBBB", data, materials_base)
    group["material_header"] = [first, total, aux_first, aux_total]
    entries = materials_base + 4
    for key, start in (("materials", first), ("aux_materials", aux_first)):
        cursor = entries + start * MATERIAL.size
        while cursor + MATERIAL.size <= offset + size:
            name, placeholder = MATERIAL.unpack_from(data, cursor)
            if name == 0:
                break
            group[key].append(name)
            cursor += MATERIAL.size
    return group


def parse(data):
    version, nodes_offset, file_size, name_hash, name_offset, flags, checksum = HEADER.unpack_from(data, 0)
    if version != FOXDATA_VERSION:
        raise GeomError("unexpected version %d" % version)
    if name_hash != NAME_GEOM:
        raise GeomError("unexpected root name hash 0x%08X" % name_hash)
    if file_size != len(data):
        raise GeomError("header size %d does not match file size %d" % (file_size, len(data)))
    geom = {"version": version, "name_hash": name_hash, "flags": flags, "checksum": checksum,
            "nodes": read_nodes(data, nodes_offset)}
    for node in geom["nodes"]:
        if node["flags"] == NODE_FLAGS_GROUP and node["data_offset"] and node["data_size"]:
            node["group"] = read_group(data, node["data_offset"], node["data_size"])
    return geom


def load_fmdl(fmdl_path):
    data = Path(fmdl_path).read_bytes()
    model = fmdl.parse(data)
    vertex_base = model["buffers"][fmdl.BUFFER_VERTEX_DATA][0]
    position_streams = {}
    for mesh_index, mesh in enumerate(model["meshes"]):
        layout = model["layouts"][mesh["layout"]]
        for stream in model["streams"][layout["first_stream"]:layout["first_stream"] + layout["stream_count"]]:
            if stream["slot"] == 0:
                position_streams[stream["offset"]] = (mesh_index, stream["file_buffer"], stream["stride"])
    if not position_streams:
        raise GeomError("fmdl has no position stream")
    first = next(iter(position_streams.values()))
    buffer = model["file_buffers"][first[1]]
    raw = data[vertex_base + buffer["offset"]:vertex_base + buffer["offset"] + buffer["size"]]
    return {"path": Path(fmdl_path), "data": data, "model": model, "positions": raw, "stride": first[2],
            "streams": position_streams}


def decode_info(info):
    material = (info >> 2) & 0x7F
    aux = (info >> 9) & 0x7F
    return (-1 if info & 1 or material == 0x7F else material), (-1 if info & 2 or aux == 0x7F else aux)


def shape_vertices(data, shape, source):
    header = shape["vertices"]
    if shape["flags"] & SHAPE_FMDL_VERTICES:
        if source is None:
            raise GeomError("shape at 0x%X needs FMDL vertices but no model was found" % shape["offset"])
        count = header["count"]
        stride = source["stride"]
        if header["fmdl_offset"] + count * stride > len(source["positions"]):
            raise GeomError("FMDL vertex range 0x%X+%d outside position buffer (%d bytes)" % (header["fmdl_offset"], count, len(source["positions"])))
        raw = np.frombuffer(source["positions"], dtype=np.uint8, count=count * stride, offset=header["fmdl_offset"]).reshape(count, stride)
        return np.ascontiguousarray(raw[:, :12]).view("<f4").reshape(count, 3).astype(np.float64)
    start = header["offset"] + header["data_offset"]
    table = np.frombuffer(data, dtype="<f4", count=header["count"] * 4, offset=start).reshape(-1, 4)[:, :3].astype(np.float64)
    vertices = table[header["index_offset"]:]
    if header["relative"]:
        vertices = vertices + table[header["origin_index"]]
    return vertices


def collect_triangles(data, geom, source=None):
    parts = []
    top_level = [n for n in geom["nodes"] if n["depth"] == 0]
    for node in geom["nodes"]:
        group = node.get("group")
        if not group:
            continue
        top = geom["nodes"][node["parent"]] if node["parent"] is not None else node
        for block_index, block in enumerate(group["blocks"]):
            for shape in block["shapes"]:
                if shape["prim_type"] != PRIM_POLY:
                    continue
                vertices = shape_vertices(data, shape, source)
                quads = np.array([p["indices"] for p in shape["prims"]], dtype=np.int64)
                if quads.max() >= len(vertices):
                    raise GeomError("poly index %d outside %d vertices at shape 0x%X" % (quads.max(), len(vertices), shape["offset"]))
                materials = np.array([decode_info(p["info"])[0] for p in shape["prims"]], dtype=np.int64)
                quad_mask = (quads[:, 3] != quads[:, 0]) & (quads[:, 3] != quads[:, 2])
                triangles = np.concatenate([quads[:, [0, 1, 2]], quads[quad_mask][:, [0, 2, 3]]])
                triangle_materials = np.concatenate([materials, materials[quad_mask]])
                fmdl_mesh = None
                if shape["flags"] & SHAPE_FMDL_VERTICES and source is not None:
                    fmdl_mesh = source["streams"].get(shape["vertices"]["fmdl_offset"], (None,))[0]
                parts.append({"node": top["index"], "node_slot": top_level.index(top), "node_hash": top["name_hash"],
                              "block": block_index, "tags": shape["tags"], "shape_name": shape["name"],
                              "shape_flags": shape["flags"], "vertices": vertices, "triangles": triangles,
                              "materials": triangle_materials, "material_hashes": group["materials"],
                              "fmdl_mesh": fmdl_mesh, "quads": int(quad_mask.sum()), "polys": len(quads)})
    return parts


def material_hash(part, index):
    return part["material_hashes"][index] if 0 <= index < len(part["material_hashes"]) else 0


def merge_parts(parts):
    merged = {}
    for part in parts:
        for index in np.unique(part["materials"]):
            key = (part["node_slot"], part["tags"], material_hash(part, int(index)))
            merged.setdefault(key, []).append((part, part["materials"] == index))
    out = []
    for key, items in sorted(merged.items()):
        vertices = []
        triangles = []
        base = 0
        for part, mask in items:
            chosen = part["triangles"][mask]
            used, remap = np.unique(chosen, return_inverse=True)
            vertices.append(part["vertices"][used])
            triangles.append(remap.reshape(-1, 3) + base)
            base += len(used)
        out.append({"key": key, "vertices": np.concatenate(vertices), "triangles": np.concatenate(triangles)})
    return out


def part_label(key):
    slot, tags, material = key
    return "node%d_tags_%016X_mat_%08X" % (slot, tags, material)


def write_obj(path, merged, name):
    lines = ["# collision exported from %s by tools/geom.py" % name]
    base = 1
    for part in merged:
        slot, tags, material = part["key"]
        lines.append("o %s" % part_label(part["key"]))
        lines.append("# tags: %s" % " ".join(tag_names(tags)))
        lines.append("usemtl geo_%08X" % material)
        lines.extend("v %.6f %.6f %.6f" % tuple(v) for v in part["vertices"])
        lines.extend("f %d %d %d" % (t[0] + base, t[1] + base, t[2] + base) for t in part["triangles"])
        base += len(part["vertices"])
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_gltf(path, merged, name, extras):
    writer = fmdl.GltfWriter("pt-port tools/geom.py")
    root = writer.node({"name": name, "extras": extras})
    for part in merged:
        slot, tags, material = part["key"]
        positions = part["vertices"].astype(np.float32)
        wide = len(positions) > 0xFFFF
        indices = part["triangles"].reshape(-1).astype(np.uint32 if wide else np.uint16)
        primitive = {"attributes": {"POSITION": writer.accessor(positions, fmdl.GL_FLOAT, "VEC3", bounds=True)},
                     "indices": writer.accessor(indices, fmdl.GL_UNSIGNED_INT if wide else fmdl.GL_UNSIGNED_SHORT, "SCALAR", target=fmdl.GL_ELEMENT_ARRAY_BUFFER)}
        label = part_label(part["key"])
        writer.doc["meshes"].append({"name": label, "primitives": [primitive]})
        writer.node({"name": label, "mesh": len(writer.doc["meshes"]) - 1,
                     "extras": {"node": slot, "tags": "0x%016X" % tags, "tag_names": tag_names(tags),
                                "material": "0x%08X" % material}}, root)
    doc, blob = writer.finish(Path(path).stem + ".bin")
    Path(path).write_text(json.dumps(doc, indent=1), encoding="utf-8")
    if blob:
        Path(path).with_suffix(".bin").write_bytes(blob)


def summarize(geom, parts):
    nodes = []
    for node in geom["nodes"]:
        entry = {"index": node["index"], "depth": node["depth"], "name_hash": "0x%08X" % node["name_hash"], "flags": node["flags"]}
        group = node.get("group")
        if group:
            entry["blocks"] = [{
                "tags": "0x%016X" % b["tags"], "tag_names": tag_names(b["tags"]), "shapes": len(b["shapes"]),
                "prim_types": sorted({PRIM_NAMES.get(s["prim_type"], str(s["prim_type"])) for s in b["shapes"]}),
                "polys": sum(s["prim_count"] for s in b["shapes"] if s["prim_type"] == PRIM_POLY),
                "shape_flags": sorted({"0x%X" % s["flags"] for s in b["shapes"] if s["prim_type"] == PRIM_POLY}),
                "shape_names": sorted({"0x%08X" % s["name"] for s in b["shapes"] if s["prim_type"] == PRIM_POLY}),
            } for b in group["blocks"]]
            entry["materials"] = ["0x%08X" % m for m in group["materials"]]
            entry["aux_materials"] = ["0x%08X" % m for m in group["aux_materials"]]
        nodes.append(entry)
    return {"nodes": nodes,
            "triangles": int(sum(len(p["triangles"]) for p in parts)),
            "polys": int(sum(p["polys"] for p in parts)),
            "quads": int(sum(p["quads"] for p in parts))}


def triangle_areas(vertices, triangles):
    edge_a = vertices[triangles[:, 1]] - vertices[triangles[:, 0]]
    edge_b = vertices[triangles[:, 2]] - vertices[triangles[:, 0]]
    return 0.5 * np.linalg.norm(np.cross(edge_a, edge_b), axis=1)


def check_parts(parts, source):
    problems = []
    warnings = []
    notes = {"warnings": warnings}
    if not parts:
        return problems, notes
    everything = np.concatenate([p["vertices"][np.unique(p["triangles"])] for p in parts])
    if not np.isfinite(everything).all():
        problems.append("non-finite collision vertices")
    notes["extent_min"] = [float(x) for x in everything.min(axis=0)]
    notes["extent_max"] = [float(x) for x in everything.max(axis=0)]
    notes["degenerate_triangles"] = int(sum(np.sum(triangle_areas(p["vertices"], p["triangles"]) < 1e-12) for p in parts))
    if source is None:
        return problems, notes
    indices = fmdl.index_buffer(source["data"], source["model"])
    meshes = {i: fmdl.decode_mesh(source["data"], source["model"], i, indices) for i in range(len(source["model"]["meshes"]))}
    positions = np.concatenate([m["attributes"][fmdl.USAGE_POSITION] for m in meshes.values()]).astype(np.float64)
    lo = positions.min(axis=0)
    hi = positions.max(axis=0)
    margin = 0.02 * float(np.max(hi - lo)) + 0.02
    total_area = 0.0
    outside_area = 0.0
    agree = 0
    considered = 0
    for p in parts:
        v = p["vertices"]
        t = p["triangles"]
        area = triangle_areas(v, t)
        centroid = v[t].mean(axis=1)
        outside = np.any((centroid < lo - margin) | (centroid > hi + margin), axis=1)
        total_area += float(area.sum())
        outside_area += float(area[outside].sum())
        if p["fmdl_mesh"] is not None and not p["shape_flags"] & SHAPE_DOUBLE_SIDED:
            normals = meshes[p["fmdl_mesh"]]["attributes"].get(fmdl.USAGE_NORMAL)
            if normals is not None:
                face = np.cross(v[t[:, 1]] - v[t[:, 0]], v[t[:, 2]] - v[t[:, 0]])
                usable = np.linalg.norm(face, axis=1) > 1e-12
                dots = (face * normals[:, :3][t].sum(axis=1)).sum(axis=1)
                agree += int(np.sum(dots[usable] > 0))
                considered += int(np.sum(usable))
    notes["area_outside_model_fraction"] = outside_area / total_area if total_area else 0.0
    if notes["area_outside_model_fraction"] > 0.05:
        warnings.append("%.1f%% of the collision area lies outside the render model bounds" % (100 * notes["area_outside_model_fraction"]))
    if considered:
        notes["winding_vs_fmdl_normals"] = agree / considered
        if notes["winding_vs_fmdl_normals"] < 0.9:
            warnings.append("only %.1f%% of FMDL-backed triangles face along the model normals" % (100 * notes["winding_vs_fmdl_normals"]))
    return problems, notes


def paired_fmdl(geom_path):
    candidate = Path(geom_path).with_suffix(".fmdl")
    return candidate if candidate.exists() else None


def load(path):
    path = Path(path)
    data = path.read_bytes()
    geom = parse(data)
    fmdl_path = paired_fmdl(path)
    source = load_fmdl(fmdl_path) if fmdl_path else None
    return data, geom, source, collect_triangles(data, geom, source)


def export_geom(path, out_dir, formats=("gltf", "obj")):
    path = Path(path)
    data, geom, source, parts = load(path)
    merged = merge_parts(parts)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    name = path.stem
    summary = summarize(geom, parts)
    problems, notes = check_parts(parts, source)
    warnings = notes["warnings"]
    extras = {"source": fmdl.source_path_of(path), "package": fmdl.package_name_of(path),
              "fmdl": fmdl.source_path_of(source["path"]) if source else None}
    if "gltf" in formats:
        write_gltf(out_dir / (name + ".gltf"), merged, name, extras)
    if "obj" in formats:
        write_obj(out_dir / (name + ".obj"), merged, name)
    groups = [{"label": part_label(m["key"]), "node": m["key"][0], "tags": "0x%016X" % m["key"][1],
               "tag_names": tag_names(m["key"][1]), "material": "0x%08X" % m["key"][2],
               "triangles": int(len(m["triangles"]))} for m in merged]
    sidecar = {**extras, "checksum": "0x%016X" % geom["checksum"], **summary, "groups": groups,
               "checks": {"problems": problems, "notes": notes}}
    (out_dir / (name + ".json")).write_text(json.dumps(sidecar, indent=1), encoding="utf-8")
    return {"name": name, "source": extras["source"], "triangles": summary["triangles"], "polys": summary["polys"],
            "groups": len(groups), "uses_fmdl": any(p["fmdl_mesh"] is not None for p in parts), "problems": problems,
            "warnings": warnings,
            "notes": {k: notes[k] for k in ("area_outside_model_fraction", "winding_vs_fmdl_normals", "degenerate_triangles") if k in notes}}


def export_job(job):
    path, out_dir, formats = job
    try:
        return export_geom(path, out_dir, formats)
    except Exception as error:
        return {"name": Path(path).stem, "source": fmdl.source_path_of(path), "error": "%s: %s" % (type(error).__name__, error),
                "trace": traceback.format_exc()}


def collect_inputs(inputs):
    files = []
    for item in inputs:
        item = Path(item)
        if item.is_dir():
            files.extend(sorted(item.rglob("*.geom")))
        elif item.suffix.lower() == ".geom":
            files.append(item)
        elif (FPK_ROOT / (str(item) + "_fpk")).is_dir():
            files.extend(sorted((FPK_ROOT / (str(item) + "_fpk")).rglob("*.geom")))
        else:
            raise SystemExit("not a .geom file, directory or package name: %s" % item)
    return files


def command_export(args):
    files = collect_inputs(args.inputs)
    formats = tuple(args.formats.split(","))
    jobs = [(str(f), str(Path(args.out) if args.out else GEOM_ROOT / fmdl.package_name_of(f)), formats) for f in files]
    workers = max(1, min(args.jobs, len(jobs)))
    if workers > 1:
        with ProcessPoolExecutor(workers) as pool:
            results = list(pool.map(export_job, jobs))
    else:
        results = [export_job(j) for j in jobs]
    by_dir = {}
    for job, result in zip(jobs, results):
        by_dir.setdefault(job[1], []).append(result)
    failures_total = 0
    for out_dir, rows in sorted(by_dir.items()):
        failures = [r for r in rows if "error" in r]
        flagged = [r for r in rows if r.get("problems")]
        failures_total += len(failures)
        index = {"files": len(rows), "exported": len(rows) - len(failures),
                 "failures": [{"name": r["name"], "error": r["error"]} for r in failures],
                 "flagged": [{"name": r["name"], "problems": r["problems"]} for r in flagged],
                 "warnings": [{"name": r["name"], "warnings": r["warnings"]} for r in rows if r.get("warnings")],
                 "triangles": sum(r.get("triangles", 0) for r in rows),
                 "rows": [r for r in rows if "error" not in r]}
        Path(out_dir).mkdir(parents=True, exist_ok=True)
        (Path(out_dir) / "index.json").write_text(json.dumps(index, indent=1), encoding="utf-8")
        print("%s: %d files, %d exported, %d failed, %d flagged, %d triangles" % (out_dir, len(rows), index["exported"], len(failures), len(flagged), index["triangles"]))
        for r in failures:
            print("  FAIL %s: %s" % (r["name"], r["error"]))
        for r in flagged:
            print("  CHECK %s: %s" % (r["name"], "; ".join(r["problems"])))
        for r in rows:
            if r.get("warnings"):
                print("  note %s: %s" % (r["name"], "; ".join(r["warnings"])))
    return 1 if failures_total else 0


def command_info(args):
    for f in collect_inputs(args.inputs):
        data = Path(f).read_bytes()
        geom = parse(data)
        print("%s  version %d  checksum 0x%016X" % (f, geom["version"], geom["checksum"]))
        for node in geom["nodes"]:
            print("  %snode 0x%08X flags 0x%X data %d bytes" % ("  " * node["depth"], node["name_hash"], node["flags"], node["data_size"]))
            group = node.get("group")
            if not group:
                continue
            for i, block in enumerate(group["blocks"]):
                print("  %s  block %d tags 0x%016X [%s] shapes %d" % ("  " * node["depth"], i, block["tags"], " ".join(tag_names(block["tags"])), len(block["shapes"])))
                if args.verbose:
                    for shape in block["shapes"]:
                        extra = ""
                        if "vertices" in shape:
                            v = shape["vertices"]
                            extra = " vertices %d (index offset %d, origin %d, relative %d, fmdl offset 0x%X)" % (v["count"], v["index_offset"], v["origin_index"], v["relative"], v["fmdl_offset"])
                        print("  %s    %s%s x%d flags 0x%X name 0x%08X%s" % ("  " * node["depth"], "  " * shape["depth"], PRIM_NAMES.get(shape["prim_type"], shape["prim_type"]), shape["prim_count"], shape["flags"], shape["name"], extra))
            if group["materials"] or group["aux_materials"]:
                print("  %s  materials %s aux %s" % ("  " * node["depth"], ["0x%08X" % m for m in group["materials"]], ["0x%08X" % m for m in group["aux_materials"]]))
    return 0


def command_preview(args):
    import meshview
    out_dir = Path(args.out) if args.out else GEOM_ROOT / "previews"
    out_dir.mkdir(parents=True, exist_ok=True)
    for f in collect_inputs(args.inputs):
        data, geom, source, parts = load(f)
        merged = merge_parts(parts)
        positions = []
        triangles = []
        groups = []
        base = 0
        for index, part in enumerate(merged):
            positions.append(part["vertices"])
            triangles.append(part["triangles"] + base)
            groups.append(np.full(len(part["triangles"]), index))
            base += len(part["vertices"])
        positions = np.concatenate(positions)
        triangles = np.concatenate(triangles)
        groups = np.concatenate(groups)
        eye = [float(v) for v in args.eye.split(",")] if args.eye else None
        target = [float(v) for v in args.target.split(",")] if args.target else None
        panels, labels = meshview.standard_panels(positions, triangles, args.size, groups, eye=eye, target=target, fov=args.fov)
        title = "%s %s collision: %d triangles, %d groups (color = node, tags, material)" % (
            fmdl.package_name_of(f), Path(f).stem, len(triangles), len(merged))
        output = out_dir / ("%s__%s%s.png" % (fmdl.package_name_of(f), Path(f).stem, args.suffix))
        meshview.save_grid(panels, labels, output, columns=2, title=title)
        print(output)
    return 0


def sheet_tile(job):
    import meshview
    path, size = job
    try:
        data, geom, source, parts = load(path)
        merged = merge_parts(parts)
        positions = np.concatenate([m["vertices"] for m in merged])
        triangles = []
        groups = []
        base = 0
        for index, m in enumerate(merged):
            triangles.append(m["triangles"] + base)
            groups.append(np.full(len(m["triangles"]), index))
            base += len(m["vertices"])
        triangles = np.concatenate(triangles)
        panel = meshview.render_panel(positions, triangles, "flat", size, meshview.facing_orbit(positions, triangles), np.concatenate(groups))
        label = "%s|%d tris  %d groups" % (Path(path).stem, len(triangles), len(merged))
    except Exception as error:
        panel = np.full((size, size, 3), 0.3)
        label = "%s|FAILED %s" % (Path(path).stem, type(error).__name__)
    return label, panel


def command_sheet(args):
    import meshview
    files = collect_inputs(args.inputs)
    jobs = [(str(f), args.size) for f in files]
    with ProcessPoolExecutor(max(1, min(args.jobs, len(jobs)))) as pool:
        tiles = list(pool.map(sheet_tile, jobs))
    out = Path(args.out) if args.out else GEOM_ROOT / "previews" / ("%s__sheet.png" % fmdl.package_name_of(files[0]))
    out.parent.mkdir(parents=True, exist_ok=True)
    meshview.contact_sheet(tiles, out, columns=args.columns, title="%s: %d collision files, colored by node, tags and material" % (fmdl.package_name_of(files[0]), len(tiles)))
    print(out)
    return 0


def main():
    parser = argparse.ArgumentParser(description="Fox Engine .geom collision (FoxData version 201209110, P.T.) reader and exporter.")
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="print the node, block and shape structure")
    info.add_argument("inputs", nargs="+")
    info.add_argument("-v", "--verbose", action="store_true")
    export = sub.add_parser("export", help="export collision triangles to glTF and OBJ")
    export.add_argument("inputs", nargs="+", help=".geom file, directory, or package name such as pt14_hallway")
    export.add_argument("--out", help="output directory (default dump/geom/<package>)")
    export.add_argument("--formats", default="gltf,obj")
    export.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    preview = sub.add_parser("preview", help="render preview PNGs of the collision")
    preview.add_argument("inputs", nargs="+")
    preview.add_argument("--out")
    preview.add_argument("--size", type=int, default=512)
    preview.add_argument("--eye")
    preview.add_argument("--target")
    preview.add_argument("--fov", type=float, default=75.0)
    preview.add_argument("--suffix", default="")
    sheet = sub.add_parser("sheet", help="render one thumbnail per collision file into a contact sheet")
    sheet.add_argument("inputs", nargs="+")
    sheet.add_argument("--out")
    sheet.add_argument("--size", type=int, default=224)
    sheet.add_argument("--columns", type=int, default=10)
    sheet.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()
    return {"info": command_info, "export": command_export, "preview": command_preview, "sheet": command_sheet}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
