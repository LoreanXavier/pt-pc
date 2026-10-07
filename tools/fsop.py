import argparse
import json
import os
import re
import struct
import sys
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

import gcn
import gcnexpr

REPO = Path(__file__).resolve().parent.parent
DEFAULT_PACKS = REPO / "dump" / "chunk1" / "shaders" / "ps4"
DEFAULT_OUT = REPO / "dump" / "shaders"

XOR_KEY = 0x9C
FOX_MAGIC = 0x19E20300
FOX_HEADER = struct.Struct("<IQBBHI16s")
FILE_HEADER = struct.Struct("<4sHHBBBBI")
COMMON = struct.Struct("<IHH")
VS_REGS = struct.Struct("<7I")
PS_REGS = struct.Struct("<12I")
BINARY_INFO = struct.Struct("<7sBIBBBBQI")
REFLECTION_HEADER = struct.Struct("<IIIIBBBBII")
BUFFER = struct.Struct("<II4BIII")
CONSTANT = struct.Struct("<II4B4II")
ELEMENT = struct.Struct("<4B8I")
SAMPLER = struct.Struct("<IIII")
ATTRIBUTE = struct.Struct("<4BIII")

STAGE_NAMES = {0: "vs", 1: "ps"}
GNM_SHADER_TYPES = {1: "vs", 2: "ps", 3: "gs", 4: "cs", 5: "es", 6: "ls", 7: "hs", 8: "cs_vs"}
VS_REG_NAMES = ("spi_shader_pgm_lo_vs", "spi_shader_pgm_hi_vs", "spi_shader_pgm_rsrc1_vs", "spi_shader_pgm_rsrc2_vs",
                "spi_vs_out_config", "spi_shader_pos_format", "pa_cl_vs_out_cntl")
PS_REG_NAMES = ("spi_shader_pgm_lo_ps", "spi_shader_pgm_hi_ps", "spi_shader_pgm_rsrc1_ps", "spi_shader_pgm_rsrc2_ps",
                "spi_shader_z_format", "spi_shader_col_format", "spi_ps_input_ena", "spi_ps_input_addr",
                "spi_ps_in_control", "spi_baryc_cntl", "db_shader_control", "cb_shader_mask")
USAGE_TYPES = {
    0x00: "ImmResource", 0x01: "ImmSampler", 0x02: "ImmConstBuffer", 0x03: "ImmVertexBuffer", 0x04: "ImmRwResource",
    0x05: "ImmAluFloatConst", 0x06: "ImmAluBool32Const", 0x07: "ImmGdsCounterRange", 0x08: "ImmGdsMemoryRange",
    0x09: "ImmGwsBase", 0x0A: "ImmShaderResourceTable", 0x0D: "ImmLdsEsGsSize", 0x12: "SubPtrFetchShader",
    0x13: "PtrResourceTable", 0x14: "PtrInternalResourceTable", 0x15: "PtrSamplerTable",
    0x16: "PtrConstBufferTable", 0x17: "PtrVertexBufferTable", 0x18: "PtrSoBufferTable",
    0x19: "PtrRwResourceTable", 0x1A: "PtrInternalGlobalTable", 0x1B: "PtrExtendedUserData",
    0x1C: "PtrIndirectResourceTable", 0x1D: "PtrIndirectInternalResourceTable", 0x1E: "PtrIndirectRwResourceTable",
}
POINTER_TABLES = {0x13: "T#table", 0x15: "S#table", 0x16: "cbtable", 0x17: "vbtable", 0x19: "RW#table",
                  0x1B: "eud", 0x12: "fetch"}
RESOURCE_TYPES = {2: "Texture2D", 3: "Texture3D", 4: "TextureCube", 0x16: "ConstantBuffer"}
TEXTURE_DIMS = {2: 2, 3: 3, 4: 3}
DATA_TYPES = {0: "float", 1: "float2", 2: "float3", 3: "float4", 8: "bool", 13: "uint3", 15: "uint4",
              0x1F: "float3x4", 0x23: "float4x4", 0x6A: "struct"}
SEMANTIC_KINDS = {0x24: "user", 0x28: "SV_Depth", 0x2E: "SV_Position", 0x35: "SV_IsFrontFace", 0x38: "SV_Target"}
SYSTEM_SEMANTICS = {0x28, 0x2E, 0x35, 0x38}
COLOR_EXPORT_FORMATS = {0: "ZERO", 1: "32_R", 2: "32_GR", 3: "32_AR", 4: "FP16_ABGR", 5: "UNORM16_ABGR",
                        6: "SNORM16_ABGR", 7: "UINT16_ABGR", 8: "SINT16_ABGR", 9: "32_ABGR"}
PS_INPUT_VGPRS = (("persp_sample", ("i", "j")), ("persp_center", ("i", "j")), ("persp_centroid", ("i", "j")),
                  ("persp_pull_model", ("x", "y", "z")), ("linear_sample", ("i", "j")), ("linear_center", ("i", "j")),
                  ("linear_centroid", ("i", "j")), ("line_stipple", ("t",)), ("FragCoord", ("x",)),
                  ("FragCoord", ("y",)), ("FragCoord", ("z",)), ("FragCoord", ("w",)), ("front_face", ("",)),
                  ("ancillary", ("",)), ("sample_coverage", ("",)), ("pos_fixed", ("",)))


def xor(data):
    return bytes(b ^ XOR_KEY for b in data)


def cstring(data, offset):
    end = data.index(b"\0", offset)
    return data[offset:end].decode("latin-1")


@dataclass
class Stage:
    entry: str
    stage: str
    blob: bytes
    fox: dict = field(default_factory=dict)
    header: dict = field(default_factory=dict)
    regs: dict = field(default_factory=dict)
    usage_slots: list = field(default_factory=list)
    input_semantics: list = field(default_factory=list)
    export_semantics: list = field(default_factory=list)
    code: bytes = b""
    code_offset: int = 0
    binary_info: dict = field(default_factory=dict)
    embedded_cb: bytes = b""
    reflection: dict = field(default_factory=dict)

    def gnm_bytes(self):
        return self.blob[FOX_HEADER.size:FOX_HEADER.size + self.fox["gnm_size"]]


def parse_stage(entry, blob):
    magic, shader_hash, stage, unknown, zero, gnm_size, pad = FOX_HEADER.unpack_from(blob, 0)
    if magic != FOX_MAGIC:
        raise ValueError(f"{entry}: bad stage magic 0x{magic:08x}")
    result = Stage(entry, STAGE_NAMES.get(stage, f"stage{stage}"), blob)
    result.fox = {"magic": magic, "hash": shader_hash, "stage": stage, "unknown": unknown, "gnm_size": gnm_size}
    base = FOX_HEADER.size
    ident, major, minor, gnm_type, header_dw, aux, gpu_modes, reserved = FILE_HEADER.unpack_from(blob, base)
    if ident != b"Shdr":
        raise ValueError(f"{entry}: missing Shdr")
    result.header = {"version": f"{major}.{minor}", "type": GNM_SHADER_TYPES.get(gnm_type, gnm_type),
                     "header_dwords": header_dw, "aux_data": aux, "target_gpu_modes": gpu_modes}
    pos = base + FILE_HEADER.size
    size_bits, ecb_dqw, scratch = COMMON.unpack_from(blob, pos)
    shader_size = size_bits & 0x7FFFFF
    uses_srt = (size_bits >> 23) & 1
    slot_count = size_bits >> 24
    result.header.update({"shader_size": shader_size, "uses_srt": uses_srt, "input_usage_slots": slot_count,
                          "embedded_cb_dqwords": ecb_dqw, "scratch_dwords_per_thread": scratch})
    pos += COMMON.size
    if gnm_type == 1:
        values = VS_REGS.unpack_from(blob, pos)
        result.regs = dict(zip(VS_REG_NAMES, values))
        pos += VS_REGS.size
        n_in, n_out, gs_mode, fetch_control = struct.unpack_from("<4B", blob, pos)
        pos += 4
        result.header.update({"gs_mode": gs_mode, "fetch_control": fetch_control})
        for i in range(slot_count):
            result.usage_slots.append(parse_usage_slot(blob, pos + 4 * i))
        pos += 4 * slot_count
        for i in range(n_in):
            semantic, vgpr, size, reserved_byte = struct.unpack_from("<4B", blob, pos)
            result.input_semantics.append({"semantic": semantic, "vgpr": vgpr, "size_code": size})
            pos += 4
        for i in range(n_out):
            value = struct.unpack_from("<H", blob, pos + 2 * i)[0]
            result.export_semantics.append({"semantic": value & 0xFF, "out_index": (value >> 8) & 0x1F,
                                            "export_f16": (value >> 13) & 3})
    elif gnm_type == 2:
        values = PS_REGS.unpack_from(blob, pos)
        result.regs = dict(zip(PS_REG_NAMES, values))
        pos += PS_REGS.size
        n_in = struct.unpack_from("<I", blob, pos)[0] & 0xFF
        pos += 4
        for i in range(slot_count):
            result.usage_slots.append(parse_usage_slot(blob, pos + 4 * i))
        pos += 4 * slot_count
        for i in range(n_in):
            value = struct.unpack_from("<H", blob, pos + 2 * i)[0]
            result.input_semantics.append({"semantic": value & 0xFF, "default_value": (value >> 8) & 3,
                                           "flat": (value >> 10) & 1, "linear": (value >> 11) & 1,
                                           "custom": (value >> 12) & 1, "interp_f16": (value >> 14) & 1})
    else:
        raise ValueError(f"{entry}: unsupported Gnm shader type {gnm_type}")
    code_offset = base + FILE_HEADER.size + header_dw * 4
    info_offset = blob.find(b"OrbShdr", code_offset)
    sig, version, info_bits, usage_base, info_slots, srt_flags, reserved3, info_hash, crc = BINARY_INFO.unpack_from(blob, info_offset)
    code_length = info_bits >> 8
    result.binary_info = {"version": version, "pssl_or_cg": info_bits & 1, "cached": (info_bits >> 1) & 1,
                          "type": (info_bits >> 2) & 0xF, "source_type": (info_bits >> 6) & 3, "length": code_length,
                          "chunk_usage_base_dw": usage_base, "input_usage_slots": info_slots, "srt_flags": srt_flags,
                          "hash": info_hash, "crc32": crc}
    result.code_offset = code_offset
    result.code = blob[code_offset:code_offset + code_length]
    info_end = info_offset + BINARY_INFO.size
    gnm_end = base + gnm_size
    result.embedded_cb = blob[info_end:gnm_end]
    result.reflection = parse_reflection(blob, gnm_end)
    return result


def parse_usage_slot(blob, pos):
    usage, api_slot, start, flags = struct.unpack_from("<4B", blob, pos)
    return {"usage": usage, "usage_name": USAGE_TYPES.get(usage, f"0x{usage:02x}"), "api_slot": api_slot,
            "start_register": start, "flags": flags}


def parse_reflection(blob, pos):
    n_buf, n_const, n_elem, n_samp, n_in, n_out, n_so, pad, zero, strings_size = REFLECTION_HEADER.unpack_from(blob, pos)
    refl = {"buffers": [], "constants": [], "elements": [], "samplers": [], "inputs": [], "outputs": [],
            "stream_outs": n_so, "strings_size": strings_size}
    pos += REFLECTION_HEADER.size
    for _ in range(n_buf):
        slot, size, kind, b1, b2, b3, count, element, name = BUFFER.unpack_from(blob, pos)
        refl["buffers"].append({"name": cstring(blob, pos + 0x14 + name), "slot": slot, "size": size,
                                "type": RESOURCE_TYPES.get(kind, kind), "type_code": [kind, b1, b2, b3],
                                "member_count": count, "element": element})
        pos += BUFFER.size
    for _ in range(n_const):
        offset, index, buffer_slot, data_type, parent, b3, u0, u1, u2, u3, name = CONSTANT.unpack_from(blob, pos)
        refl["constants"].append({"name": cstring(blob, pos + 0x1C + name), "offset": offset, "index": index,
                                  "buffer": buffer_slot, "type": DATA_TYPES.get(data_type, data_type),
                                  "reserved": [u0, u1, u2, u3]})
        pos += CONSTANT.size
    for _ in range(n_elem):
        data_type, b1, b2, b3, offset, size, array, parent, members, first, name, type_name = ELEMENT.unpack_from(blob, pos)
        refl["elements"].append({"name": cstring(blob, pos + 0x1C + name), "type_name": cstring(blob, pos + 0x20 + type_name),
                                 "type": DATA_TYPES.get(data_type, data_type), "flags": [b1, b2, b3], "offset": offset,
                                 "size": size, "array": array, "parent": parent if parent != 0xFFFFFFFF else -1,
                                 "members": members, "first_member": first})
        pos += ELEMENT.size
    for _ in range(n_samp):
        u0, u1, slot, name = SAMPLER.unpack_from(blob, pos)
        refl["samplers"].append({"name": cstring(blob, pos + 0xC + name), "slot": slot, "reserved": [u0, u1]})
        pos += SAMPLER.size
    for key, count in (("inputs", n_in), ("outputs", n_out)):
        for _ in range(count):
            data_type, semantic_kind, semantic_index, register, zero2, name, semantic = ATTRIBUTE.unpack_from(blob, pos)
            refl[key].append({"name": cstring(blob, pos + 8 + name), "semantic": cstring(blob, pos + 0xC + semantic),
                              "semantic_kind": SEMANTIC_KINDS.get(semantic_kind, semantic_kind),
                              "semantic_index": semantic_index, "register": register,
                              "type": DATA_TYPES.get(data_type, data_type)})
            pos += ATTRIBUTE.size
    refl["end"] = pos + strings_size
    return refl


def read_pack(path):
    data = Path(path).read_bytes()
    entries = []
    pos = 0
    current = None
    while pos < len(data):
        if pos + 8 <= len(data) and xor(data[pos + 4:pos + 8]) == struct.pack("<I", FOX_MAGIC):
            size = struct.unpack_from("<I", data, pos)[0]
            blob = xor(data[pos + 4:pos + 4 + size])
            current["stages"].append(parse_stage(current["name"], blob))
            pos += 4 + size
        else:
            length = data[pos]
            name = data[pos + 1:pos + length].decode("latin-1")
            if data[pos + length] != 0:
                raise ValueError(f"{path}: bad entry name at 0x{pos:x}")
            current = {"name": name, "offset": pos, "stages": []}
            entries.append(current)
            pos += 1 + length
    return entries


def stage_summary(stage):
    refl = stage.reflection
    return {
        "hash": f"{stage.fox['hash']:016x}",
        "code_bytes": len(stage.code),
        "textures": [(b["slot"], b["type"], b["name"]) for b in refl["buffers"] if b["type"] != "ConstantBuffer"],
        "cbuffers": [(b["slot"], b["size"], b["name"]) for b in refl["buffers"] if b["type"] == "ConstantBuffer"],
        "samplers": [(s["slot"], s["name"]) for s in refl["samplers"]],
        "inputs": [(a["name"], a["semantic"], a["semantic_index"], a["register"], a["type"]) for a in refl["inputs"]],
        "outputs": [(a["name"], a["semantic"], a["semantic_index"], a["register"], a["type"]) for a in refl["outputs"]],
    }


def constant_lookup(refl):
    by_buffer = {}
    for const in refl["constants"]:
        by_buffer.setdefault(const["buffer"], []).append(const)
    names = {b["slot"]: b["name"] for b in refl["buffers"] if b["type"] == "ConstantBuffer"}

    def lookup(slot, byte):
        for const in by_buffer.get(slot, []):
            start = const["offset"]
            if start <= byte < start + 16:
                comp = "xyzw"[(byte - start) // 4]
                return f"{const['name']}.{comp}"
        base = names.get(slot, f"cb{slot}")
        return f"{base}[{byte // 16}].{'xyzw'[(byte % 16) // 4]}"
    return lookup


def build_env(stage):
    refl = stage.reflection
    textures = {b["slot"]: b for b in refl["buffers"] if b["type"] != "ConstantBuffer"}
    cbuffers = {b["slot"]: b for b in refl["buffers"] if b["type"] == "ConstantBuffer"}
    samplers = {s["slot"]: s for s in refl["samplers"]}

    def resource_name(kind, slot):
        if kind == "T#":
            texture = textures.get(slot)
            return texture["name"] if texture else f"texture{slot}"
        if kind == "S#":
            sampler = samplers.get(slot)
            return sampler["name"] if sampler else f"sampler{slot}"
        if kind == "V#cb":
            buffer = cbuffers.get(slot)
            return buffer["name"] if buffer else f"cbuffer{slot}"
        if kind == "V#vb":
            return f"vertexbuffer{slot}"
        return f"{kind}{slot}"

    def texture_dims(slot):
        texture = textures.get(slot)
        if not texture:
            return 2
        return TEXTURE_DIMS.get(texture["type_code"][0], 2)

    sgpr_tags = {}
    eud_tags = {}
    for slot in stage.usage_slots:
        usage, api, start = slot["usage"], slot["api_slot"], slot["start_register"]
        tags = []
        if usage in POINTER_TABLES:
            tags = [("ptr", POINTER_TABLES[usage]), ("ptrhi", POINTER_TABLES[usage])]
        elif usage in (0x00, 0x04):
            kind = "T#" if slot["flags"] & 2 else "V#"
            width = 8 if slot["flags"] & 1 else 4
            tags = [(kind, api, i) for i in range(width)]
        elif usage == 0x01:
            tags = [("S#", api, i) for i in range(4)]
        elif usage == 0x02:
            tags = [("V#cb", api, i) for i in range(4)]
        elif usage == 0x03:
            tags = [("V#vb", api, i) for i in range(4)]
        for i, tag in enumerate(tags):
            register = start + i
            if register < 16:
                sgpr_tags[register] = tag
            else:
                eud_tags[register - 16] = tag
    env = {"sgpr_tags": sgpr_tags, "eud_tags": eud_tags, "constant_name": constant_lookup(refl),
           "resource_name": resource_name, "texture_dims": texture_dims, "sgpr_names": {}, "vgpr_names": {}}
    if stage.stage == "vs":
        by_register = {a["register"]: a for a in refl["inputs"]}
        fetch = []
        for semantic in stage.input_semantics:
            attribute = by_register.get(semantic["semantic"])
            label = attribute["name"] if attribute else f"in{semantic['semantic']}"
            for comp in range(semantic["size_code"]):
                fetch.append((semantic["vgpr"] + comp, f"{label}.{'xyzw'[comp]}"))
        env["fetch"] = fetch
        env["vgpr_names"] = {0: "VertexID", 3: "InstanceID"}
        outputs = refl["outputs"]
        params = sorted((a for a in outputs if a["semantic_kind"] == "user"), key=lambda a: a["register"])
        position = [a for a in outputs if a["semantic_kind"] == "SV_Position"]
        export_names = {12: position[0]["name"] if position else "position"}
        ordered = sorted(stage.export_semantics, key=lambda s: s["semantic"])
        for rank, semantic in enumerate(ordered):
            match = params[rank] if rank < len(params) else None
            export_names[32 + semantic["out_index"]] = match["name"] if match else f"param{semantic['out_index']}"
        env["export_name"] = lambda target, names=export_names: names.get(target)
    else:
        user_inputs = sorted((a for a in refl["inputs"] if a["semantic_kind"] == "user"), key=lambda a: a["register"])
        interp = {}
        for index, semantic in enumerate(stage.input_semantics):
            match = user_inputs[index] if index < len(user_inputs) else None
            interp[index] = match["name"] if match else f"attr{index}"
        env["interp_name"] = lambda attr, names=interp: names.get(attr, f"attr{attr}")
        addr = stage.regs["spi_ps_input_addr"]
        vgpr = 0
        names = {}
        for bit, (label, comps) in enumerate(PS_INPUT_VGPRS):
            if addr & (1 << bit):
                for comp in comps:
                    names[vgpr] = f"{label}.{comp}" if comp else label
                    vgpr += 1
        env["vgpr_names"] = names
        user_sgprs = (stage.regs["spi_shader_pgm_rsrc2_ps"] >> 1) & 0x1F
        env["sgpr_names"] = {user_sgprs: "prim_mask"}
        targets = {}
        for output in refl["outputs"]:
            if output["semantic_kind"] == "SV_Target":
                targets[output["register"]] = output["name"]
            elif output["semantic_kind"] == "SV_Depth":
                targets[8] = output["name"]
        env["export_name"] = lambda target, names=targets: names.get(target)
    return env


def compact_constants(constants):
    groups = []
    for const in sorted(constants, key=lambda c: (c["buffer"], c["offset"])):
        stem = re.sub(r"\[\d+\]", "[]", const["name"])
        if groups and groups[-1][0] == const["buffer"] and groups[-1][1] == stem and const["offset"] == groups[-1][3] + 16:
            buffer, name, first, _, data_type, count = groups[-1]
            groups[-1] = (buffer, name, first, const["offset"], data_type, count + 1)
        else:
            groups.append((const["buffer"], stem, const["offset"], const["offset"], const["type"], 1))
    result = []
    for buffer, stem, first, last, data_type, count in groups:
        label = stem if count > 1 else next(c["name"] for c in constants if c["buffer"] == buffer and c["offset"] == first)
        result.append((buffer, label, first, last, data_type, count))
    return result


def describe_stage(stage):
    refl = stage.reflection
    lines = [f"// {stage.entry} {stage.stage}  hash {stage.fox['hash']:016x}  code {len(stage.code)} bytes"]
    regs = stage.regs
    if stage.stage == "ps":
        col = regs["spi_shader_col_format"]
        formats = [COLOR_EXPORT_FORMATS.get((col >> (4 * i)) & 0xF, "?") for i in range(8) if (col >> (4 * i)) & 0xF]
        lines.append(f"// color exports {formats}  input_ena 0x{regs['spi_ps_input_ena']:x}  db_shader_control 0x{regs['db_shader_control']:x}")
    for slot in stage.usage_slots:
        lines.append(f"// user data {slot['start_register']:>2}: {slot['usage_name']} slot {slot['api_slot']} flags 0x{slot['flags']:x}")
    for buffer in refl["buffers"]:
        extra = f" size {buffer['size']}" if buffer["type"] == "ConstantBuffer" else ""
        lines.append(f"// {buffer['type']:<14} slot {buffer['slot']:>2}  {buffer['name']}{extra}")
    for sampler in refl["samplers"]:
        lines.append(f"// Sampler        slot {sampler['slot']:>2}  {sampler['name']}")
    buffers = {b["slot"]: b["name"] for b in refl["buffers"]}
    for buffer, stem, first, last, data_type, count in compact_constants(refl["constants"]):
        span = f"0x{first:03x}" if count == 1 else f"0x{first:03x}..0x{last:03x}"
        suffix = "" if count == 1 else f" ({count} registers)"
        lines.append(f"//   {buffers.get(buffer, buffer)}+{span} {data_type} {stem}{suffix}")
    for key in ("inputs", "outputs"):
        for attribute in refl[key]:
            lines.append(f"// {key[:-1]:<6} {attribute['type']} {attribute['name']} : {attribute['semantic']}{attribute['semantic_index']} "
                         f"({attribute['semantic_kind']}, register {attribute['register']})")
    if stage.stage == "vs":
        for semantic in stage.input_semantics:
            lines.append(f"// fetch semantic {semantic['semantic']} -> v{semantic['vgpr']} ({semantic['size_code']} components)")
        for semantic in stage.export_semantics:
            lines.append(f"// export semantic 0x{semantic['semantic']:x} -> param{semantic['out_index']}")
    else:
        for index, semantic in enumerate(stage.input_semantics):
            lines.append(f"// ps input {index}: semantic 0x{semantic['semantic']:x}" + (" flat" if semantic["flat"] else ""))
    if stage.embedded_cb:
        values = struct.unpack(f"<{len(stage.embedded_cb) // 4}f", stage.embedded_cb[:len(stage.embedded_cb) // 4 * 4])
        lines.append("// embedded constants " + ", ".join(gcn.format_float(v) for v in values))
    return lines


def disassembly_text(stage, ops):
    lines = describe_stage(stage)
    lines.append("")
    for inst in gcn.decode(stage.code, ops, len(stage.code)):
        lines.append(f"{inst.offset:04x}: {' '.join(f'{w:08x}' for w in inst.words):<18} {gcn.format_inst(inst)}")
    return "\n".join(lines) + "\n"


def pseudo_text(stage, ops):
    lines = describe_stage(stage)
    lines.append("")
    insts = gcn.decode(stage.code, ops, len(stage.code))
    lifter = gcn.Lifter(build_env(stage))
    for inst, text, is_label in lifter.run(insts):
        if is_label:
            lines.append(text)
        else:
            lines.append(f"  {text:<100} // {inst.offset:04x} {gcn.format_inst(inst)}")
    return "\n".join(lines) + "\n"


def expression_text(stage, ops):
    lines = describe_stage(stage)
    lines.append("")
    insts = gcn.decode(stage.code, ops, len(stage.code))
    try:
        body = gcnexpr.lift(insts, build_env(stage))
    except gcnexpr.LoopError as error:
        body = [f"// expression form unavailable: {error}; see the pseudo listing"]
    lines.extend(body)
    return "\n".join(lines) + "\n"


def stage_json(stage):
    return {"entry": stage.entry, "stage": stage.stage, "fox_header": stage.fox, "gnm_header": stage.header,
            "registers": stage.regs, "usage_slots": stage.usage_slots, "input_semantics": stage.input_semantics,
            "export_semantics": stage.export_semantics, "binary_info": stage.binary_info,
            "code_offset": stage.code_offset, "embedded_cb": stage.embedded_cb.hex(), "reflection": stage.reflection}


def safe_name(name):
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in name)


def extract_pack(pack_path, out_root, opcodes_path, disasm):
    ops = gcn.load_opcodes(opcodes_path) if disasm else None
    pack_path = Path(pack_path)
    stem = pack_path.stem
    out_dir = out_root / stem
    out_dir.mkdir(parents=True, exist_ok=True)
    entries = read_pack(pack_path)
    index = []
    for entry in entries:
        entry_dir = out_dir / safe_name(entry["name"])
        entry_dir.mkdir(exist_ok=True)
        record = {"name": entry["name"], "offset": entry["offset"], "stages": {}}
        for stage in entry["stages"]:
            prefix = entry_dir / stage.stage
            (prefix.with_suffix(".stage.bin")).write_bytes(stage.blob)
            (prefix.with_suffix(".gcn.bin")).write_bytes(stage.code)
            (prefix.with_suffix(".json")).write_text(json.dumps(stage_json(stage), indent=1))
            if disasm:
                (prefix.with_suffix(".asm.txt")).write_text(disassembly_text(stage, ops))
                (prefix.with_suffix(".pseudo.txt")).write_text(pseudo_text(stage, ops))
                (prefix.with_suffix(".expr.txt")).write_text(expression_text(stage, ops))
            record["stages"][stage.stage] = stage_summary(stage)
        index.append(record)
    (out_dir / "index.json").write_text(json.dumps(index, indent=1))
    return stem, len(entries), sum(len(e["stages"]) for e in entries)


def find_packs(inputs):
    packs = []
    for item in inputs:
        item = Path(item)
        if item.is_dir():
            packs.extend(sorted(item.glob("*.fsop")))
        else:
            packs.append(item)
    return packs


def cmd_list(args):
    for pack in find_packs(args.inputs):
        entries = read_pack(pack)
        print(f"{pack.name}: {len(entries)} entries")
        for entry in entries:
            parts = []
            for stage in entry["stages"]:
                refl = stage.reflection
                parts.append(f"{stage.stage} {len(stage.code):5d}B tex {sum(1 for b in refl['buffers'] if b['type'] != 'ConstantBuffer')} "
                             f"cb {sum(1 for b in refl['buffers'] if b['type'] == 'ConstantBuffer')}")
            print(f"  {entry['name']:<40} " + " | ".join(parts))


def cmd_info(args):
    ops = gcn.load_opcodes(args.opcodes)
    for pack in find_packs([args.pack]):
        for entry in read_pack(pack):
            if entry["name"] != args.entry:
                continue
            for stage in entry["stages"]:
                if args.stage and stage.stage != args.stage:
                    continue
                if args.mode == "asm":
                    print(disassembly_text(stage, ops))
                elif args.mode == "json":
                    print(json.dumps(stage_json(stage), indent=1))
                elif args.mode == "expr":
                    print(expression_text(stage, ops))
                else:
                    print(pseudo_text(stage, ops))


def cmd_extract(args):
    packs = find_packs(args.inputs or [DEFAULT_PACKS])
    jobs = [(pack, args.out, args.opcodes, not args.no_disasm) for pack in packs]
    with ProcessPoolExecutor(max_workers=min(len(jobs), args.jobs)) as pool:
        for stem, entries, stages in pool.map(extract_pack_job, jobs):
            print(f"{stem}: {entries} entries, {stages} stages")


def extract_pack_job(job):
    return extract_pack(*job)


def main():
    ap = argparse.ArgumentParser(description="Read Fox Engine PS4 shader packs (.fsop).")
    ap.add_argument("--opcodes", type=Path, help="shadPS4 src/shader_recompiler/frontend/opcodes.h")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("list", help="list entries and stages")
    p.add_argument("inputs", nargs="+", type=Path)
    p.set_defaults(func=cmd_list)
    p = sub.add_parser("info", help="print one entry as pseudo code, disassembly or JSON")
    p.add_argument("pack", type=Path)
    p.add_argument("entry")
    p.add_argument("--stage", choices=("vs", "ps"))
    p.add_argument("--mode", choices=("pseudo", "asm", "json", "expr"), default="pseudo")
    p.set_defaults(func=cmd_info)
    p = sub.add_parser("extract", help="write every stage to <out>/<pack>/<entry>/")
    p.add_argument("inputs", nargs="*", type=Path)
    p.add_argument("--out", type=Path, default=DEFAULT_OUT)
    p.add_argument("--no-disasm", action="store_true")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    p.set_defaults(func=cmd_extract)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
