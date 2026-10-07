import argparse
import json
import os
import struct
import sys
import traceback
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
FPK_ROOT = REPO / "dump" / "fpk"
MODELS_ROOT = REPO / "dump" / "models"
TEXTURES_ROOT = REPO / "dump" / "textures"

MAGIC = b"FMDL"
HEADER = struct.Struct("<4sfQQQIIIIII")
FEATURE_HEADER = struct.Struct("<BBHI")
BUFFER_HEADER = struct.Struct("<III")

FEATURE_BONES = 0
FEATURE_MESH_GROUPS = 1
FEATURE_MESH_GROUP_DEFS = 2
FEATURE_MESHES = 3
FEATURE_MATERIAL_INSTANCES = 4
FEATURE_BONE_GROUPS = 5
FEATURE_TEXTURE_REFS = 6
FEATURE_MATERIAL_PARAMS = 7
FEATURE_SHADERS = 8
FEATURE_LAYOUTS = 9
FEATURE_STREAMS = 10
FEATURE_ELEMENTS = 11
FEATURE_STRINGS = 12
FEATURE_BOUNDING_BOXES = 13
FEATURE_FILE_BUFFERS = 14
FEATURE_LOD_INFO = 16
FEATURE_INDEX_SLICES = 17
FEATURE_18 = 18
FEATURE_20 = 20
FEATURE_PATH_HASHES = 21
FEATURE_NAME_HASHES = 22

FEATURE_ENTRY_SIZES = {
    FEATURE_BONES: 0x30,
    FEATURE_MESH_GROUPS: 0x8,
    FEATURE_MESH_GROUP_DEFS: 0x20,
    FEATURE_MESHES: 0x30,
    FEATURE_MATERIAL_INSTANCES: 0x10,
    FEATURE_BONE_GROUPS: 0x44,
    FEATURE_TEXTURE_REFS: 0x4,
    FEATURE_MATERIAL_PARAMS: 0x4,
    FEATURE_SHADERS: 0x4,
    FEATURE_LAYOUTS: 0x8,
    FEATURE_STREAMS: 0x8,
    FEATURE_ELEMENTS: 0x4,
    FEATURE_STRINGS: 0x8,
    FEATURE_BOUNDING_BOXES: 0x20,
    FEATURE_FILE_BUFFERS: 0x10,
    FEATURE_LOD_INFO: 0x10,
    FEATURE_INDEX_SLICES: 0x8,
    FEATURE_18: 0x8,
    FEATURE_20: 0x80,
    FEATURE_PATH_HASHES: 0x8,
    FEATURE_NAME_HASHES: 0x8,
}

BUFFER_PARAM_VECTORS = 0
BUFFER_VERTEX_DATA = 2
BUFFER_STRINGS = 3

FILE_BUFFER_VERTICES = 0
FILE_BUFFER_INDICES = 1

USAGE_POSITION = 0
USAGE_BONE_WEIGHT0 = 1
USAGE_NORMAL = 2
USAGE_COLOR = 3
USAGE_BONE_INDEX0 = 7
USAGE_UV0 = 8
USAGE_TANGENT = 14

USAGE_NAMES = {
    0: "POSITION",
    1: "BONE_WEIGHT0",
    2: "NORMAL",
    3: "COLOR",
    7: "BONE_INDEX0",
    8: "UV0",
    9: "UV1",
    10: "UV2",
    11: "UV3",
    12: "BONE_WEIGHT1",
    13: "BONE_INDEX1",
    14: "TANGENT",
}

ELEMENT_FORMATS = {
    1: ("R32G32B32_FLOAT", "<f4", 3, "float"),
    6: ("R16G16B16A16_FLOAT", "<f2", 4, "float"),
    7: ("R16G16_FLOAT", "<f2", 2, "float"),
    8: ("R8G8B8A8_UNORM", "u1", 4, "unorm"),
    9: ("R8G8B8A8_UINT", "u1", 4, "uint"),
}

GL_UNSIGNED_BYTE = 5121
GL_UNSIGNED_SHORT = 5123
GL_UNSIGNED_INT = 5125
GL_FLOAT = 5126
GL_ARRAY_BUFFER = 34962
GL_ELEMENT_ARRAY_BUFFER = 34963

MESH_FLAG_DOUBLE_SIDED = 0x20
MESH_FLAG_ALPHA_TEST = 0x80
BLEND_SHADER_MARKERS = ("translucent", "glass")


class FmdlError(Exception):
    pass


def set_bits(mask):
    return [bit for bit in range(64) if mask >> bit & 1]


def parse_tables(data, header):
    features = {}
    pos = header["file_desc_offset"]
    for kind in set_bits(header["feature_types"]):
        ftype, overflow, count, offset = FEATURE_HEADER.unpack_from(data, pos)
        if ftype != kind:
            raise FmdlError("feature header %d has type %d, bitmask says %d" % (len(features), ftype, kind))
        features[ftype] = (overflow * 0x10000 + count, header["features_offset"] + offset)
        pos += FEATURE_HEADER.size
    if len(features) != header["feature_count"]:
        raise FmdlError("feature count %d does not match bitmask (%d)" % (header["feature_count"], len(features)))
    buffers = {}
    for kind in set_bits(header["buffer_types"]):
        btype, offset, size = BUFFER_HEADER.unpack_from(data, pos)
        if btype != kind:
            raise FmdlError("buffer header has type %d, bitmask says %d" % (btype, kind))
        buffers[btype] = (header["buffers_offset"] + offset, size)
        pos += BUFFER_HEADER.size
    return features, buffers


def feature_records(data, features, kind, fmt):
    if kind not in features:
        return []
    count, offset = features[kind]
    record = struct.Struct(fmt)
    size = FEATURE_ENTRY_SIZES[kind]
    return [record.unpack_from(data, offset + i * size) for i in range(count)]


def parse(data):
    if data[:4] != MAGIC:
        raise FmdlError("not an FMDL file")
    fields = HEADER.unpack_from(data, 0)
    header = dict(zip(("magic", "version", "file_desc_offset", "feature_types", "buffer_types", "feature_count",
                       "buffer_count", "features_offset", "features_size", "buffers_offset", "buffers_size"), fields))
    header["magic"] = header["magic"].decode("ascii")
    header["version"] = round(header["version"], 4)
    features, buffers = parse_tables(data, header)
    model = {"header": header, "features": features, "buffers": buffers}

    strings = []
    if BUFFER_STRINGS in buffers:
        string_base = buffers[BUFFER_STRINGS][0]
        for btype, length, offset in feature_records(data, features, FEATURE_STRINGS, "<HHI"):
            if btype != BUFFER_STRINGS:
                raise FmdlError("string header points at buffer type %d" % btype)
            strings.append(data[string_base + offset:string_base + offset + length].decode("utf-8", "replace"))
    model["strings"] = strings
    name_hashes = [r[0] for r in feature_records(data, features, FEATURE_NAME_HASHES, "<Q")]
    path_hashes = [r[0] for r in feature_records(data, features, FEATURE_PATH_HASHES, "<Q")]
    model["name_hashes"] = name_hashes
    model["path_hashes"] = path_hashes

    def name(index):
        if strings:
            return strings[index] if 0 <= index < len(strings) else None
        if 0 <= index < len(name_hashes):
            return "0x%012X" % name_hashes[index]
        return None

    def path(index):
        if strings:
            return strings[index] if 0 <= index < len(strings) else None
        if 0 <= index < len(path_hashes):
            return "0x%016X" % path_hashes[index]
        return None

    model["bones"] = [
        {"name": name(r[0]), "parent": r[1], "bbox": r[2], "flags": r[3], "local": list(r[6:10]), "world": list(r[10:14])}
        for r in feature_records(data, features, FEATURE_BONES, "<HhHHII4f4f")]
    model["mesh_groups"] = [
        {"name": name(r[0]), "flags": r[1], "parent": r[2], "vectors_index": r[3]}
        for r in feature_records(data, features, FEATURE_MESH_GROUPS, "<HHhh")]
    model["mesh_group_defs"] = [
        {"unknown": r[0], "group": r[1], "mesh_count": r[2], "first_mesh": r[3], "bbox": r[4], "first_slice": r[6],
         "padding": [r[5], r[7], r[8], r[9]]}
        for r in feature_records(data, features, FEATURE_MESH_GROUP_DEFS, "<IHHHHIHHIQ")]
    model["meshes"] = [
        {"flags": r[0], "material": r[1], "bone_group": r[2], "layout": r[3], "vertex_count": r[4],
         "vertex_start": r[5], "index_start": r[7], "index_count": r[8], "first_slice": r[9],
         "padding": [r[6]] + list(r[10:])}
        for r in feature_records(data, features, FEATURE_MESHES, "<IHHHHHHIII5I")]
    model["material_instances"] = [
        {"name": name(r[0]), "shader": r[2], "texture_count": r[3], "vector_count": r[4],
         "first_texture_param": r[5], "first_vector_param": r[6], "padding": [r[1], r[7]]}
        for r in feature_records(data, features, FEATURE_MATERIAL_INSTANCES, "<HHHBBHHI")]
    bone_groups = []
    if FEATURE_BONE_GROUPS in features:
        count, offset = features[FEATURE_BONE_GROUPS]
        for i in range(count):
            base = offset + i * FEATURE_ENTRY_SIZES[FEATURE_BONE_GROUPS]
            max_weights, bone_count = struct.unpack_from("<HH", data, base)
            if bone_count > 32:
                raise FmdlError("bone group %d has %d bones" % (i, bone_count))
            bones = list(struct.unpack_from("<%dH" % bone_count, data, base + 4))
            bone_groups.append({"max_weights": max_weights, "bones": bones})
    model["bone_groups"] = bone_groups
    model["texture_refs"] = [
        {"name": name(r[0]), "directory": path(r[1])}
        for r in feature_records(data, features, FEATURE_TEXTURE_REFS, "<HH")]
    model["material_params"] = [
        {"name": name(r[0]), "reference": r[1]}
        for r in feature_records(data, features, FEATURE_MATERIAL_PARAMS, "<HH")]
    model["shaders"] = [
        {"shader": name(r[0]), "technique": name(r[1])}
        for r in feature_records(data, features, FEATURE_SHADERS, "<HH")]
    model["layouts"] = [
        {"stream_count": r[0], "element_count": r[1], "unknown": r[2], "uv_count": r[3], "first_stream": r[4],
         "first_element": r[5]}
        for r in feature_records(data, features, FEATURE_LAYOUTS, "<BBBBHH")]
    model["streams"] = [
        {"file_buffer": r[0], "element_count": r[1], "stride": r[2], "slot": r[3], "offset": r[4]}
        for r in feature_records(data, features, FEATURE_STREAMS, "<BBBBI")]
    model["elements"] = [
        {"usage": r[0], "format": r[1], "offset": r[2]}
        for r in feature_records(data, features, FEATURE_ELEMENTS, "<BBH")]
    model["bounding_boxes"] = [
        {"max": list(r[0:4]), "min": list(r[4:8])}
        for r in feature_records(data, features, FEATURE_BOUNDING_BOXES, "<4f4f")]
    model["file_buffers"] = [
        {"type": r[0], "unknown": r[1], "size": r[2], "offset": r[3], "padding": r[4]}
        for r in feature_records(data, features, FEATURE_FILE_BUFFERS, "<HHIII")]
    model["lod_info"] = [
        {"lod_count": r[0], "padding": r[1], "values": list(r[2:5])}
        for r in feature_records(data, features, FEATURE_LOD_INFO, "<HH3f")]
    model["index_slices"] = [
        {"start": r[0], "count": r[1]}
        for r in feature_records(data, features, FEATURE_INDEX_SLICES, "<II")]
    model["feature18"] = [r[0] for r in feature_records(data, features, FEATURE_18, "<Q")]
    model["feature20"] = [
        {"leading": r[0], "values": list(r[1:7]), "levels": r[7]}
        for r in feature_records(data, features, FEATURE_20, "<I6fI")]

    vectors = []
    if BUFFER_PARAM_VECTORS in buffers:
        offset, size = buffers[BUFFER_PARAM_VECTORS]
        vectors = [list(struct.unpack_from("<4f", data, offset + i * 16)) for i in range(size // 16)]
    model["param_vectors"] = vectors
    return model


def lod_count(model):
    return model["lod_info"][0]["lod_count"] if model["lod_info"] else 1


def read_element(data, base, count, stride, element):
    format_name, dtype, components, kind = ELEMENT_FORMATS[element["format"]]
    item_size = np.dtype(dtype).itemsize * components
    if element["offset"] + item_size > stride:
        raise FmdlError("element usage %d at offset %d does not fit stride %d" % (element["usage"], element["offset"], stride))
    if base + count * stride > len(data):
        raise FmdlError("vertex stream at 0x%X with %d vertices runs past the end of the file" % (base, count))
    raw = np.frombuffer(data, dtype=np.uint8, count=count * stride, offset=base).reshape(count, stride)
    values = np.ascontiguousarray(raw[:, element["offset"]:element["offset"] + item_size]).view(dtype).reshape(count, components)
    if kind == "float":
        return values.astype(np.float32)
    if kind == "unorm":
        return values.astype(np.float32) / 255.0
    return values.copy()


def index_buffer(data, model):
    vertex_base = model["buffers"][BUFFER_VERTEX_DATA][0]
    for file_buffer in model["file_buffers"]:
        if file_buffer["type"] == FILE_BUFFER_INDICES:
            start = vertex_base + file_buffer["offset"]
            return np.frombuffer(data, dtype="<u2", count=file_buffer["size"] // 2, offset=start)
    raise FmdlError("no index buffer")


def mesh_lod_ranges(model, mesh_index):
    mesh = model["meshes"][mesh_index]
    slices = model["index_slices"][mesh["first_slice"]:mesh["first_slice"] + lod_count(model)]
    ranges = [(mesh["index_start"], mesh["index_count"])]
    for extra in slices[1:]:
        ranges.append((mesh["index_start"] + extra["start"], extra["count"]))
    return ranges, slices


def decode_mesh(data, model, mesh_index, indices, lod=0):
    mesh = model["meshes"][mesh_index]
    layout = model["layouts"][mesh["layout"]]
    vertex_base = model["buffers"][BUFFER_VERTEX_DATA][0]
    count = mesh["vertex_count"]
    attributes = {}
    vertex_format = []
    cursor = layout["first_element"]
    for stream in model["streams"][layout["first_stream"]:layout["first_stream"] + layout["stream_count"]]:
        file_buffer = model["file_buffers"][stream["file_buffer"]]
        if file_buffer["type"] != FILE_BUFFER_VERTICES:
            raise FmdlError("stream points at file buffer %d of type %d" % (stream["file_buffer"], file_buffer["type"]))
        base = vertex_base + file_buffer["offset"] + stream["offset"]
        for element in model["elements"][cursor:cursor + stream["element_count"]]:
            if element["format"] not in ELEMENT_FORMATS:
                raise FmdlError("unknown element format %d" % element["format"])
            attributes[element["usage"]] = read_element(data, base, count, stream["stride"], element)
            vertex_format.append({"usage": USAGE_NAMES.get(element["usage"], str(element["usage"])),
                                  "format": ELEMENT_FORMATS[element["format"]][0], "file_buffer": stream["file_buffer"],
                                  "slot": stream["slot"], "stride": stream["stride"], "offset": element["offset"]})
        cursor += stream["element_count"]
    if cursor != layout["first_element"] + layout["element_count"]:
        raise FmdlError("mesh %d: streams consume %d elements, layout has %d" % (mesh_index, cursor - layout["first_element"], layout["element_count"]))
    if USAGE_POSITION not in attributes:
        raise FmdlError("mesh %d has no positions" % mesh_index)

    ranges, slices = mesh_lod_ranges(model, mesh_index)
    lod = min(lod, len(ranges) - 1)
    start, index_count = ranges[lod]
    if start + index_count > len(indices):
        raise FmdlError("mesh %d LOD %d index range %d+%d exceeds index buffer (%d)" % (mesh_index, lod, start, index_count, len(indices)))
    if index_count % 3:
        raise FmdlError("mesh %d LOD %d index count %d is not a multiple of 3" % (mesh_index, lod, index_count))
    triangles = indices[start:start + index_count].astype(np.uint32).reshape(-1, 3)
    lod_index_max = []
    for lod_start, lod_count_value in ranges:
        segment = indices[lod_start:lod_start + lod_count_value]
        lod_index_max.append(int(segment.max()) if len(segment) and lod_start + lod_count_value <= len(indices) else -1)

    joints = None
    weights = None
    if USAGE_BONE_INDEX0 in attributes and model["bone_groups"]:
        group = np.array(model["bone_groups"][mesh["bone_group"]]["bones"], dtype=np.int64)
        local = attributes[USAGE_BONE_INDEX0].astype(np.int64)
        weights = attributes.get(USAGE_BONE_WEIGHT0)
        if weights is None:
            weights = np.zeros(local.shape, dtype=np.float32)
            weights[:, 0] = 1.0
        out_of_group = (local >= len(group)) & (weights > 0)
        joints = group[np.clip(local, 0, len(group) - 1)]
        joints[out_of_group] = 0
        weights = np.where(out_of_group, 0.0, weights).astype(np.float32)

    return {
        "index": mesh_index,
        "vertex_count": count,
        "attributes": attributes,
        "vertex_format": vertex_format,
        "triangles": triangles,
        "lod_ranges": ranges,
        "lod_slices": [(s["start"], s["count"]) for s in slices],
        "lod_index_max": lod_index_max,
        "joints": joints,
        "weights": weights,
    }


def texture_info(reference):
    name = reference["name"] or ""
    directory = reference["directory"] or ""
    stem = name.rsplit(".", 1)[0] if "." in name else name
    info = {"name": name, "directory": directory, "stem": stem}
    if name.startswith("0x") or directory.startswith("0x"):
        return info
    info["ftex"] = directory + stem + ".ftex"
    return info


def material_info(model, material_index):
    instance = model["material_instances"][material_index]
    shader = model["shaders"][instance["shader"]] if instance["shader"] < len(model["shaders"]) else {"shader": None, "technique": None}
    params = model["material_params"]
    textures = {}
    for p in params[instance["first_texture_param"]:instance["first_texture_param"] + instance["texture_count"]]:
        if p["reference"] < len(model["texture_refs"]):
            entry = texture_info(model["texture_refs"][p["reference"]])
            entry["texture_ref"] = p["reference"]
            textures[p["name"]] = entry
        else:
            textures[p["name"]] = {"texture_ref": p["reference"], "missing": True}
    vectors = {}
    for p in params[instance["first_vector_param"]:instance["first_vector_param"] + instance["vector_count"]]:
        if p["reference"] < len(model["param_vectors"]):
            vectors[p["name"]] = model["param_vectors"][p["reference"]]
        else:
            vectors[p["name"]] = {"vector": p["reference"], "missing": True}
    return {"name": instance["name"], "shader": shader["shader"], "technique": shader["technique"],
            "textures": textures, "vectors": vectors}


def mesh_owner(model):
    owners = {}
    for def_index, group_def in enumerate(model["mesh_group_defs"]):
        for mesh_index in range(group_def["first_mesh"], group_def["first_mesh"] + group_def["mesh_count"]):
            owners[mesh_index] = def_index
    return owners


def check_model(model, meshes):
    problems = []
    warnings = []
    notes = {}
    owners = mesh_owner(model)
    all_positions = np.concatenate([m["attributes"][USAGE_POSITION] for m in meshes]) if meshes else np.zeros((0, 3))
    for m in meshes:
        tri = m["triangles"]
        if len(tri) and tri.max() >= m["vertex_count"]:
            problems.append("mesh %d: index %d >= vertex count %d" % (m["index"], tri.max(), m["vertex_count"]))
        for lod, (start, count) in enumerate(m["lod_ranges"]):
            if count % 3:
                problems.append("mesh %d LOD %d count %d not a multiple of 3" % (m["index"], lod, count))
            if m["lod_index_max"][lod] < 0 or m["lod_index_max"][lod] >= m["vertex_count"]:
                problems.append("mesh %d LOD %d indices out of range" % (m["index"], lod))
        if m["lod_slices"] and m["lod_slices"][0] != (0, m["lod_ranges"][0][1]):
            problems.append("mesh %d: first LOD slice %s differs from main slice" % (m["index"], m["lod_slices"][0]))
        positions = m["attributes"][USAGE_POSITION]
        if not np.isfinite(positions).all():
            problems.append("mesh %d: non-finite positions" % m["index"])
        if m["index"] in owners:
            group_def = model["mesh_group_defs"][owners[m["index"]]]
            box = model["bounding_boxes"][group_def["bbox"]] if group_def["bbox"] < len(model["bounding_boxes"]) else None
            if box is not None and len(positions):
                lo = np.array(box["min"][:3])
                hi = np.array(box["max"][:3])
                size = max(float(np.max(hi - lo)), 1e-6)
                outside = max(float(np.max(lo - positions.min(axis=0))), float(np.max(positions.max(axis=0) - hi)), 0.0)
                if outside > 1e-3 * size + 1e-4:
                    problems.append("mesh %d extends %.4f outside its group bounding box" % (m["index"], outside))
    if model["bounding_boxes"] and len(all_positions):
        box = model["bounding_boxes"][0]
        lo = np.array(box["min"][:3])
        hi = np.array(box["max"][:3])
        extent_lo = all_positions.min(axis=0)
        extent_hi = all_positions.max(axis=0)
        notes["bbox0_vs_extent"] = float(max(np.max(np.abs(lo - extent_lo)), np.max(np.abs(hi - extent_hi))))
    agree = considered = 0
    for m in meshes:
        if USAGE_NORMAL not in m["attributes"] or not len(m["triangles"]):
            continue
        positions = m["attributes"][USAGE_POSITION].astype(np.float64)
        vertex_normals = m["attributes"][USAGE_NORMAL][:, :3].astype(np.float64)
        tri = m["triangles"].astype(np.int64)[:, [0, 2, 1]]
        face = np.cross(positions[tri[:, 1]] - positions[tri[:, 0]], positions[tri[:, 2]] - positions[tri[:, 0]])
        usable = np.linalg.norm(face, axis=1) > 1e-12
        agree += int(np.sum((face * vertex_normals[tri].sum(axis=1)).sum(axis=1)[usable] > 0))
        considered += int(np.sum(usable))
    if considered:
        notes["winding_agreement"] = agree / considered
        if notes["winding_agreement"] < 0.9:
            warnings.append("only %.1f%% of triangles face along their vertex normals" % (100 * notes["winding_agreement"]))
    normals = [m["attributes"][USAGE_NORMAL][:, :3] for m in meshes if USAGE_NORMAL in m["attributes"]]
    if normals:
        lengths = np.linalg.norm(np.concatenate(normals), axis=1)
        notes["normal_len_min"] = float(lengths.min())
        notes["normal_len_max"] = float(lengths.max())
        notes["normal_unit_fraction"] = float(np.mean(np.abs(lengths - 1.0) < 0.01))
        if notes["normal_unit_fraction"] < 0.99:
            warnings.append("only %.1f%% of normals are unit length" % (100 * notes["normal_unit_fraction"]))
    uvs = [m["attributes"][USAGE_UV0] for m in meshes if USAGE_UV0 in m["attributes"]]
    if uvs:
        uv = np.concatenate(uvs)
        notes["uv_min"] = [float(x) for x in uv.min(axis=0)]
        notes["uv_max"] = [float(x) for x in uv.max(axis=0)]
        notes["uv_in_range_fraction"] = float(np.mean(np.all((uv >= -4) & (uv <= 5), axis=1)))
        if not np.isfinite(uv).all():
            problems.append("non-finite UVs")
        if notes["uv_in_range_fraction"] < 0.95:
            warnings.append("only %.1f%% of UV0 inside [-4, 5]" % (100 * notes["uv_in_range_fraction"]))
    tangents = [m["attributes"][USAGE_TANGENT] for m in meshes if USAGE_TANGENT in m["attributes"]]
    if tangents:
        t = np.concatenate(tangents)
        notes["tangent_w_values"] = sorted({float(x) for x in np.unique(np.round(t[:, 3], 3))})[:8]
    weights = [m["weights"] for m in meshes if m["weights"] is not None]
    if weights:
        total = np.concatenate(weights).sum(axis=1)
        notes["weight_sum_min"] = float(total.min())
        notes["weight_sum_max"] = float(total.max())
    notes["warnings"] = warnings
    return problems, notes


class GltfWriter:
    def __init__(self, generator):
        self.doc = {"asset": {"version": "2.0", "generator": generator}, "scene": 0, "scenes": [{"nodes": []}],
                    "nodes": [], "meshes": [], "materials": [], "accessors": [], "bufferViews": [], "buffers": []}
        self.blob = bytearray()

    def view(self, raw, target=None):
        while len(self.blob) % 4:
            self.blob.append(0)
        entry = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(raw)}
        if target:
            entry["target"] = target
        self.blob.extend(raw)
        self.doc["bufferViews"].append(entry)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, array, component_type, kind, normalized=False, bounds=False, target=GL_ARRAY_BUFFER):
        array = np.ascontiguousarray(array)
        entry = {"bufferView": self.view(array.tobytes(), target), "componentType": component_type,
                 "count": int(array.shape[0]), "type": kind}
        if normalized:
            entry["normalized"] = True
        if bounds:
            flat = array.reshape(array.shape[0], -1)
            entry["min"] = [float(x) for x in flat.min(axis=0)]
            entry["max"] = [float(x) for x in flat.max(axis=0)]
        self.doc["accessors"].append(entry)
        return len(self.doc["accessors"]) - 1

    def node(self, entry, parent=None):
        self.doc["nodes"].append(entry)
        index = len(self.doc["nodes"]) - 1
        if parent is None:
            self.doc["scenes"][0]["nodes"].append(index)
        else:
            self.doc["nodes"][parent].setdefault("children", []).append(index)
        return index

    def finish(self, bin_name):
        while len(self.blob) % 4:
            self.blob.append(0)
        self.doc["buffers"] = [{"uri": bin_name, "byteLength": len(self.blob)}] if self.blob else []
        for key in ("materials", "meshes", "accessors", "bufferViews", "buffers"):
            if not self.doc[key]:
                del self.doc[key]
        return self.doc, bytes(self.blob)


def primitive_attributes(writer, mesh):
    attributes = mesh["attributes"]
    out = {}
    positions = attributes[USAGE_POSITION].astype(np.float32)
    out["POSITION"] = writer.accessor(positions, GL_FLOAT, "VEC3", bounds=True)
    if USAGE_NORMAL in attributes:
        normals = attributes[USAGE_NORMAL][:, :3].astype(np.float32)
        length = np.linalg.norm(normals, axis=1, keepdims=True)
        normals = np.where(length > 1e-6, normals / np.maximum(length, 1e-6), np.array([0, 1, 0], dtype=np.float32)).astype(np.float32)
        out["NORMAL"] = writer.accessor(normals, GL_FLOAT, "VEC3")
    if USAGE_TANGENT in attributes and USAGE_NORMAL in attributes:
        tangents = attributes[USAGE_TANGENT].astype(np.float32).copy()
        length = np.linalg.norm(tangents[:, :3], axis=1, keepdims=True)
        tangents[:, :3] = np.where(length > 1e-6, tangents[:, :3] / np.maximum(length, 1e-6), np.array([1, 0, 0], dtype=np.float32))
        tangents[:, 3] = np.where(tangents[:, 3] < 0, -1.0, 1.0)
        out["TANGENT"] = writer.accessor(tangents, GL_FLOAT, "VEC4")
    for uv_set in range(4):
        usage = USAGE_UV0 + uv_set
        if usage in attributes:
            out["TEXCOORD_%d" % uv_set] = writer.accessor(attributes[usage].astype(np.float32), GL_FLOAT, "VEC2")
    if USAGE_COLOR in attributes:
        colors = np.clip(np.round(attributes[USAGE_COLOR] * 255), 0, 255).astype(np.uint8)
        out["COLOR_0"] = writer.accessor(colors, GL_UNSIGNED_BYTE, "VEC4", normalized=True)
    if mesh["joints"] is not None:
        weights = mesh["weights"].astype(np.float32)
        joints = mesh["joints"].copy()
        total = weights.sum(axis=1, keepdims=True)
        empty = total[:, 0] <= 0
        weights = np.where(total > 0, weights / np.maximum(total, 1e-9), 0).astype(np.float32)
        weights[empty, 0] = 1.0
        joints[weights == 0] = 0
        joint_type = GL_UNSIGNED_BYTE if joints.max() < 256 else GL_UNSIGNED_SHORT
        joint_dtype = np.uint8 if joint_type == GL_UNSIGNED_BYTE else np.uint16
        out["JOINTS_0"] = writer.accessor(joints.astype(joint_dtype), joint_type, "VEC4")
        out["WEIGHTS_0"] = writer.accessor(weights, GL_FLOAT, "VEC4")
    return out


def gltf_material(info, flags):
    shader = (info["shader"] or "") + " " + (info["technique"] or "")
    material = {"name": info["name"], "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0.0, "roughnessFactor": 1.0}}
    if flags & MESH_FLAG_DOUBLE_SIDED:
        material["doubleSided"] = True
    if any(marker in shader.lower() for marker in BLEND_SHADER_MARKERS):
        material["alphaMode"] = "BLEND"
    elif flags & MESH_FLAG_ALPHA_TEST:
        material["alphaMode"] = "MASK"
    material["extras"] = {"shader": info["shader"], "technique": info["technique"], "textures": info["textures"],
                          "vectors": info["vectors"], "mesh_flags": "0x%X" % flags}
    return material


def base_texture_param(info):
    for param in ("Base_Tex_SRGB", "Base_Tex_LIN"):
        if param in info["textures"]:
            return param
    return None


def attach_textures(writer, material, info, texture_uri):
    if texture_uri is None:
        return
    param = base_texture_param(info)
    entry = info["textures"].get(param) if param else None
    uri = texture_uri(entry) if entry and "ftex" in entry else None
    if not uri:
        return
    images = writer.doc.setdefault("images", [])
    textures = writer.doc.setdefault("textures", [])
    if "samplers" not in writer.doc:
        writer.doc["samplers"] = [{"wrapS": 10497, "wrapT": 10497}]
    for image_index, image in enumerate(images):
        if image["uri"] == uri:
            texture_index = next(t for t, texture in enumerate(textures) if texture["source"] == image_index)
            break
    else:
        images.append({"uri": uri})
        textures.append({"source": len(images) - 1, "sampler": 0})
        texture_index = len(textures) - 1
    material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture_index}
    if "_alp" in entry["stem"] and "alphaMode" not in material:
        material["alphaMode"] = "MASK"


def texture_png(ftex, package, texture_root):
    if not ftex or not texture_root:
        return None
    root = Path(texture_root)
    relative = ftex.lstrip("/")
    if relative.startswith("Assets/"):
        relative = relative[len("Assets/"):]
    relative = relative.rsplit(".", 1)[0] + ".png"
    preferred = root / package / relative
    if preferred.exists():
        return preferred
    for candidate in sorted(root.glob("*/" + relative)):
        return candidate
    return None


def build_gltf(model, meshes, name, texture_index=None):
    writer = GltfWriter("pt-port tools/fmdl.py")
    root = writer.node({"name": name})
    bone_nodes = []
    skin = None
    if model["bones"]:
        for bone_index, bone in enumerate(model["bones"]):
            parent = bone["parent"]
            world = np.array(bone["world"][:3], dtype=np.float64)
            relative = world - np.array(model["bones"][parent]["world"][:3]) if parent >= 0 else world
            entry = {"name": bone["name"] or "bone_%d" % bone_index}
            if np.any(np.abs(relative) > 0):
                entry["translation"] = [float(x) for x in relative]
            bone_nodes.append(entry)
        for bone_index, bone in enumerate(model["bones"]):
            parent = bone["parent"]
            if parent >= bone_index:
                raise FmdlError("bone %d has parent %d after it" % (bone_index, parent))
            parent_node = bone_nodes[parent]["node"] if parent >= 0 else root
            bone_nodes[bone_index]["node"] = writer.node({k: v for k, v in bone_nodes[bone_index].items() if k != "node"}, parent_node)
        inverse_bind = np.zeros((len(model["bones"]), 16), dtype=np.float32)
        for bone_index, bone in enumerate(model["bones"]):
            matrix = np.eye(4, dtype=np.float32)
            matrix[:3, 3] = -np.array(bone["world"][:3], dtype=np.float32)
            inverse_bind[bone_index] = matrix.T.ravel()
        skin = {"joints": [b["node"] for b in bone_nodes],
                "inverseBindMatrices": writer.accessor(inverse_bind, GL_FLOAT, "MAT4", target=None),
                "skeleton": bone_nodes[0]["node"]}
        writer.doc["skins"] = [skin]

    material_slots = {}

    def material_slot(material_index, flags):
        render_flags = flags & (MESH_FLAG_DOUBLE_SIDED | MESH_FLAG_ALPHA_TEST)
        key = (material_index, render_flags)
        if key not in material_slots:
            info = material_info(model, material_index)
            material = gltf_material(info, render_flags)
            material["extras"]["material_instance"] = material_index
            attach_textures(writer, material, info, texture_index)
            if any(k[0] == material_index for k in material_slots):
                material["name"] = "%s#%d" % (info["name"], len([k for k in material_slots if k[0] == material_index]))
            writer.doc["materials"].append(material)
            material_slots[key] = len(writer.doc["materials"]) - 1
        return material_slots[key]

    group_nodes = {}
    for group_index, group in enumerate(model["mesh_groups"]):
        parent = group["parent"]
        parent_node = group_nodes.get(parent, root) if parent >= 0 else root
        entry = {"name": group["name"] or "group_%d" % group_index}
        if group["flags"]:
            entry["extras"] = {"flags": group["flags"]}
        group_nodes[group_index] = writer.node(entry, parent_node)

    by_index = {m["index"]: m for m in meshes}
    for def_index, group_def in enumerate(model["mesh_group_defs"]):
        primitives = []
        for mesh_index in range(group_def["first_mesh"], group_def["first_mesh"] + group_def["mesh_count"]):
            mesh = by_index[mesh_index]
            source = model["meshes"][mesh_index]
            triangles = mesh["triangles"][:, [0, 2, 1]]
            index_type = GL_UNSIGNED_SHORT if mesh["vertex_count"] <= 0xFFFF else GL_UNSIGNED_INT
            index_dtype = np.uint16 if index_type == GL_UNSIGNED_SHORT else np.uint32
            primitive = {"attributes": primitive_attributes(writer, mesh),
                         "indices": writer.accessor(triangles.reshape(-1).astype(index_dtype), index_type, "SCALAR", target=GL_ELEMENT_ARRAY_BUFFER),
                         "material": material_slot(source["material"], source["flags"]),
                         "extras": {"fmdl_mesh": mesh_index, "flags": "0x%X" % source["flags"]}}
            primitives.append(primitive)
        group_name = model["mesh_groups"][group_def["group"]]["name"] if group_def["group"] < len(model["mesh_groups"]) else None
        writer.doc["meshes"].append({"name": "%s.%d" % (group_name or "group", def_index), "primitives": primitives})
        node = {"name": "%s.%d" % (group_name or "group", def_index), "mesh": len(writer.doc["meshes"]) - 1}
        if skin is not None and any(p["attributes"].get("JOINTS_0") is not None for p in primitives):
            node["skin"] = 0
            node["extras"] = {"mesh_group": group_name}
            writer.node(node)
        else:
            writer.node(node, group_nodes.get(group_def["group"], root))
    return writer


def model_sidecar(model, meshes, source, package, problems, notes):
    owners = mesh_owner(model)
    materials = [material_info(model, i) for i in range(len(model["material_instances"]))]
    textures = [texture_info(r) for r in model["texture_refs"]]
    return {
        "source": source,
        "package": package,
        "version": model["header"]["version"],
        "bounding_boxes": model["bounding_boxes"],
        "lod_count": lod_count(model),
        "lod_info": model["lod_info"],
        "mesh_groups": model["mesh_groups"],
        "mesh_group_defs": [{k: d[k] for k in ("group", "first_mesh", "mesh_count", "bbox", "first_slice")} for d in model["mesh_group_defs"]],
        "meshes": [{
            "index": m["index"],
            "group_def": owners.get(m["index"]),
            "material": model["meshes"][m["index"]]["material"],
            "material_name": materials[model["meshes"][m["index"]]["material"]]["name"],
            "flags": "0x%X" % model["meshes"][m["index"]]["flags"],
            "bone_group": model["meshes"][m["index"]]["bone_group"] if model["bone_groups"] else None,
            "vertex_count": m["vertex_count"],
            "triangle_count": len(m["triangles"]),
            "lods": [{"start": s, "count": c} for s, c in m["lod_ranges"]],
            "vertex_format": m["vertex_format"],
        } for m in meshes],
        "materials": materials,
        "textures": textures,
        "bones": [{"name": b["name"], "parent": b["parent"], "world": b["world"][:3]} for b in model["bones"]],
        "bone_groups": model["bone_groups"],
        "stats": {
            "meshes": len(meshes),
            "vertices": int(sum(m["vertex_count"] for m in meshes)),
            "triangles": int(sum(len(m["triangles"]) for m in meshes)),
            "materials": len(materials),
            "textures": len(textures),
            "bones": len(model["bones"]),
        },
        "checks": {"problems": problems, "notes": notes},
    }


def package_name_of(path):
    for part in Path(path).parts:
        if part.endswith("_fpk") or part.endswith("_fpkd"):
            return part.rsplit("_", 1)[0]
    return "loose"


def source_path_of(path):
    parts = Path(path).parts
    for i, part in enumerate(parts):
        if part == "Assets":
            return "/" + "/".join(parts[i:])
    return Path(path).name


def load(path, lod=0):
    data = Path(path).read_bytes()
    model = parse(data)
    indices = index_buffer(data, model)
    meshes = [decode_mesh(data, model, i, indices, lod) for i in range(len(model["meshes"]))]
    return model, meshes


def export_model(path, out_dir, lod=0, texture_root=None):
    path = Path(path)
    package = package_name_of(path)
    source = source_path_of(path)
    name = path.stem
    model, meshes = load(path, lod)
    problems, notes = check_model(model, meshes)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    def texture_uri(entry):
        png = texture_png(entry.get("ftex"), package, texture_root)
        return os.path.relpath(png, out_dir).replace(os.sep, "/") if png else None

    writer = build_gltf(model, meshes, name, texture_uri if texture_root else None)
    doc, blob = writer.finish(name + ".bin")
    doc["asset"]["extras"] = {"source": source, "package": package, "fmdl_version": model["header"]["version"], "lod": lod}
    (out_dir / (name + ".gltf")).write_text(json.dumps(doc, indent=1), encoding="utf-8")
    if blob:
        (out_dir / (name + ".bin")).write_bytes(blob)
    sidecar = model_sidecar(model, meshes, source, package, problems, notes)
    for material in sidecar["materials"]:
        for entry in material["textures"].values():
            png = texture_png(entry.get("ftex"), package, texture_root) if texture_root else None
            if png:
                entry["png"] = os.path.relpath(png, out_dir).replace(os.sep, "/")
    (out_dir / (name + ".json")).write_text(json.dumps(sidecar, indent=1), encoding="utf-8")
    texture_uses = []
    for material in sidecar["materials"]:
        for param, texture in material["textures"].items():
            texture_uses.append({"ftex": texture.get("ftex"), "name": texture.get("name"), "directory": texture.get("directory"),
                                 "param": param, "material": material["name"], "shader": material["shader"]})
    return {"name": name, "source": source, **sidecar["stats"], "problems": problems, "warnings": notes["warnings"],
            "notes": notes, "texture_uses": texture_uses}


def export_job(job):
    path, out_dir, lod, texture_root = job
    try:
        return export_model(path, out_dir, lod, texture_root)
    except Exception as error:
        return {"name": Path(path).stem, "source": source_path_of(path), "error": "%s: %s" % (type(error).__name__, error),
                "trace": traceback.format_exc()}


def collect_inputs(inputs):
    files = []
    for item in inputs:
        item = Path(item)
        if item.is_dir():
            files.extend(sorted(item.rglob("*.fmdl")))
        elif item.suffix.lower() == ".fmdl":
            files.append(item)
        elif (FPK_ROOT / (str(item) + "_fpk")).is_dir():
            files.extend(sorted((FPK_ROOT / (str(item) + "_fpk")).rglob("*.fmdl")))
        else:
            raise SystemExit("not a .fmdl file, directory or package name: %s" % item)
    return files


def command_export(args):
    files = collect_inputs(args.inputs)
    jobs = []
    texture_root = None if args.no_textures else (args.textures or (str(TEXTURES_ROOT) if TEXTURES_ROOT.exists() else None))
    for f in files:
        out_dir = Path(args.out) if args.out else MODELS_ROOT / package_name_of(f)
        jobs.append((str(f), str(out_dir), args.lod, texture_root))
    workers = max(1, min(args.jobs, len(jobs)))
    if workers > 1:
        with ProcessPoolExecutor(workers) as pool:
            results = list(pool.map(export_job, jobs))
    else:
        results = [export_job(j) for j in jobs]
    by_package = {}
    for job, result in zip(jobs, results):
        by_package.setdefault(job[1], []).append(result)
    total_failures = 0
    for out_dir, rows in sorted(by_package.items()):
        failures = [r for r in rows if "error" in r]
        flagged = [r for r in rows if r.get("problems")]
        total_failures += len(failures)
        summary = {
            "models": len(rows),
            "exported": len(rows) - len(failures),
            "failures": [{"name": r["name"], "error": r["error"]} for r in failures],
            "flagged": [{"name": r["name"], "problems": r["problems"]} for r in flagged],
            "warnings": [{"name": r["name"], "warnings": r["warnings"]} for r in rows if r.get("warnings")],
            "vertices": sum(r.get("vertices", 0) for r in rows),
            "triangles": sum(r.get("triangles", 0) for r in rows),
            "rows": [{k: r[k] for k in ("name", "source", "meshes", "vertices", "triangles", "materials", "textures", "bones") if k in r} for r in rows if "error" not in r],
        }
        Path(out_dir).mkdir(parents=True, exist_ok=True)
        (Path(out_dir) / "index.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
        textures = {}
        for r in rows:
            for use in r.get("texture_uses", []):
                key = use["ftex"] or "%s|%s" % (use["directory"], use["name"])
                entry = textures.setdefault(key, {"ftex": use["ftex"], "name": use["name"], "directory": use["directory"],
                                                  "params": set(), "users": set()})
                entry["params"].add(use["param"])
                entry["users"].add("%s:%s" % (r["name"], use["material"]))
        texture_list = [{**v, "params": sorted(v["params"]), "users": sorted(v["users"])} for k, v in sorted(textures.items())]
        (Path(out_dir) / "textures.json").write_text(json.dumps(texture_list, indent=1), encoding="utf-8")
        print("%s: %d models, %d exported, %d failed, %d flagged, %d vertices, %d triangles" % (
            out_dir, summary["models"], summary["exported"], len(failures), len(flagged), summary["vertices"], summary["triangles"]))
        for r in failures:
            print("  FAIL %s: %s" % (r["name"], r["error"]))
        for r in flagged:
            print("  CHECK %s: %s" % (r["name"], "; ".join(r["problems"])))
        for r in rows:
            if r.get("warnings"):
                print("  note %s: %s" % (r["name"], "; ".join(r["warnings"])))
        if args.verbose:
            for r in rows:
                if "error" not in r:
                    print("  %-40s meshes %3d  vertices %7d  triangles %7d" % (r["name"], r["meshes"], r["vertices"], r["triangles"]))
    return 1 if total_failures else 0


def command_info(args):
    for f in collect_inputs(args.inputs):
        data = Path(f).read_bytes()
        model = parse(data)
        indices = index_buffer(data, model)
        print("%s  FMDL %.2f  features %s  buffers %s" % (f, model["header"]["version"], sorted(model["features"]), sorted(model["buffers"])))
        print("  groups %d, group defs %d, meshes %d, materials %d, textures %d, bones %d, bone groups %d, LODs %d" % (
            len(model["mesh_groups"]), len(model["mesh_group_defs"]), len(model["meshes"]), len(model["material_instances"]),
            len(model["texture_refs"]), len(model["bones"]), len(model["bone_groups"]), lod_count(model)))
        for i, mesh in enumerate(model["meshes"]):
            m = decode_mesh(data, model, i, indices)
            info = material_info(model, mesh["material"])
            print("  mesh %3d  vertices %6d  triangles %6d  flags 0x%-7X material %-24s %s  [%s]" % (
                i, mesh["vertex_count"], len(m["triangles"]), mesh["flags"], info["name"], info["technique"],
                " ".join("%s:%s" % (e["usage"], e["format"]) for e in m["vertex_format"])))
        if args.verbose:
            for i in range(len(model["material_instances"])):
                info = material_info(model, i)
                print("  material %d %s  %s / %s" % (i, info["name"], info["shader"], info["technique"]))
                for param, tex in info["textures"].items():
                    print("    %-28s %s" % (param, tex.get("ftex", tex)))
                for param, vec in info["vectors"].items():
                    print("    %-28s %s" % (param, vec))
    return 0


def base_color_images(model, package, texture_root, max_size=1024):
    from PIL import Image
    images = {}
    alpha_ids = set()
    if not texture_root or not Path(texture_root).exists():
        return images, alpha_ids
    for material_index in range(len(model["material_instances"])):
        info = material_info(model, material_index)
        param = base_texture_param(info)
        entry = info["textures"].get(param) if param else None
        png = texture_png(entry.get("ftex"), package, texture_root) if entry else None
        if not png:
            continue
        image = Image.open(png).convert("RGBA")
        image.thumbnail((max_size, max_size))
        images[material_index] = np.asarray(image, dtype=np.float32) / 255.0
        if "_alp" in entry["stem"]:
            alpha_ids.add(material_index)
    return images, alpha_ids


def command_preview(args):
    import meshview
    out_dir = Path(args.out) if args.out else MODELS_ROOT / "previews"
    out_dir.mkdir(parents=True, exist_ok=True)
    for f in collect_inputs(args.inputs):
        model, meshes = load(f, args.lod)
        positions, normals, uvs, triangles, groups = [], [], [], [], []
        base = 0
        for m in meshes:
            attributes = m["attributes"]
            count = m["vertex_count"]
            positions.append(attributes[USAGE_POSITION].astype(np.float64))
            normal = attributes.get(USAGE_NORMAL)
            normals.append(normal[:, :3].astype(np.float64) if normal is not None else np.tile([0.0, 1.0, 0.0], (count, 1)))
            uv = attributes.get(USAGE_UV0)
            uvs.append(uv.astype(np.float64) if uv is not None else np.zeros((count, 2)))
            triangles.append(m["triangles"][:, [0, 2, 1]].astype(np.int64) + base)
            groups.append(np.full(len(m["triangles"]), model["meshes"][m["index"]]["material"]))
            base += count
        positions = np.concatenate(positions)
        normals = np.concatenate(normals)
        uvs = np.concatenate(uvs)
        triangles = np.concatenate(triangles)
        groups = np.concatenate(groups)
        eye = [float(v) for v in args.eye.split(",")] if args.eye else None
        target = [float(v) for v in args.target.split(",")] if args.target else None
        panels, labels = meshview.standard_panels(positions, triangles, args.size, groups, normals, uvs, eye, target, args.fov)
        images, alpha_ids = base_color_images(model, package_name_of(f), None if args.no_textures else TEXTURES_ROOT)
        if images:
            views = [(meshview.facing_orbit(positions, triangles), "textured base color")]
            if eye is not None and target is not None:
                views.append((meshview.Perspective(eye, target, args.fov), "textured base color, perspective"))
            for view, label in views:
                panels.append(meshview.render_textured(positions, triangles, args.size, view, groups, images, alpha_ids, uvs))
                labels.append(label)
        extent = positions.max(axis=0) - positions.min(axis=0)
        title = "%s  %s  meshes %d  vertices %d  triangles %d  extent %.2f x %.2f x %.2f" % (
            package_name_of(f), Path(f).stem, len(meshes), len(positions), len(triangles), *extent)
        output = out_dir / ("%s__%s%s.png" % (package_name_of(f), Path(f).stem, args.suffix))
        meshview.save_grid(panels, labels, output, columns=3, title=title)
        print(output)
    return 0


def sheet_tile(job):
    import meshview
    path, size, lod = job
    try:
        model, meshes = load(path, lod)
        positions = np.concatenate([m["attributes"][USAGE_POSITION] for m in meshes]).astype(np.float64)
        triangles = []
        groups = []
        base = 0
        for m in meshes:
            triangles.append(m["triangles"][:, [0, 2, 1]].astype(np.int64) + base)
            groups.append(np.full(len(m["triangles"]), model["meshes"][m["index"]]["material"]))
            base += m["vertex_count"]
        triangles = np.concatenate(triangles)
        panel = meshview.render_panel(positions, triangles, "flat", size, meshview.facing_orbit(positions, triangles), np.concatenate(groups))
        extent = positions.max(axis=0) - positions.min(axis=0)
        label = "%s|%d tris  %.2fx%.2fx%.2f m" % (Path(path).stem, len(triangles), *extent)
    except Exception as error:
        panel = np.full((size, size, 3), 0.3)
        label = "%s|FAILED %s" % (Path(path).stem, type(error).__name__)
    return label, panel


def command_sheet(args):
    import meshview
    files = collect_inputs(args.inputs)
    jobs = [(str(f), args.size, args.lod) for f in files]
    with ProcessPoolExecutor(max(1, min(args.jobs, len(jobs)))) as pool:
        tiles = list(pool.map(sheet_tile, jobs))
    out = Path(args.out) if args.out else MODELS_ROOT / "previews" / ("%s__sheet.png" % package_name_of(files[0]))
    out.parent.mkdir(parents=True, exist_ok=True)
    meshview.contact_sheet(tiles, out, columns=args.columns, title="%s: %d models, flat shaded by material, back faces culled" % (package_name_of(files[0]), len(tiles)))
    print(out)
    return 0


def main():
    parser = argparse.ArgumentParser(description="Fox Engine FMDL 2.03 (P.T.) reader and glTF 2.0 exporter.")
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="print model tables")
    info.add_argument("inputs", nargs="+")
    info.add_argument("-v", "--verbose", action="store_true")
    export = sub.add_parser("export", help="export .fmdl files, directories or package names to glTF")
    export.add_argument("inputs", nargs="+", help=".fmdl file, directory, or package name such as pt14_hallway")
    export.add_argument("--out", help="output directory (default dump/models/<package>)")
    export.add_argument("--lod", type=int, default=0)
    export.add_argument("--textures", help="root of converted texture PNGs laid out as <package>/<path under /Assets>.png (default dump/textures)")
    export.add_argument("--no-textures", action="store_true", help="do not link texture PNGs")
    export.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    export.add_argument("-v", "--verbose", action="store_true")
    preview = sub.add_parser("preview", help="render preview PNGs with the software rasterizer")
    preview.add_argument("inputs", nargs="+")
    preview.add_argument("--out", help="output directory (default dump/models/previews)")
    preview.add_argument("--lod", type=int, default=0)
    preview.add_argument("--size", type=int, default=512)
    preview.add_argument("--eye", help="x,y,z of a perspective camera for an extra panel")
    preview.add_argument("--target", help="x,y,z the perspective camera looks at")
    preview.add_argument("--fov", type=float, default=75.0)
    preview.add_argument("--suffix", default="", help="text appended to the output file name")
    preview.add_argument("--no-textures", action="store_true", help="skip the textured panels")
    sheet = sub.add_parser("sheet", help="render one thumbnail per model into a contact sheet")
    sheet.add_argument("inputs", nargs="+")
    sheet.add_argument("--out", help="output PNG (default dump/models/previews/<package>__sheet.png)")
    sheet.add_argument("--size", type=int, default=224)
    sheet.add_argument("--columns", type=int, default=10)
    sheet.add_argument("--lod", type=int, default=0)
    sheet.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    return {"info": command_info, "export": command_export, "preview": command_preview, "sheet": command_sheet}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
