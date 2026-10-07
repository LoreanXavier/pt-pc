import argparse
import math
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_OPCODES_H = REPO.parent / "shadPS4" / "src" / "shader_recompiler" / "frontend" / "opcodes.h"

ENUM_NAMES = {
    "SOP2": "OpcodeSOP2", "SOPK": "OpcodeSOPK", "SOP1": "OpcodeSOP1", "SOPC": "OpcodeSOPC",
    "SOPP": "OpcodeSOPP", "SMRD": "OpcodeSMRD", "VOP2": "OpcodeVOP2", "VOP3": "OpcodeVOP3",
    "VOP1": "OpcodeVOP1", "VOPC": "OpcodeVOPC", "VINTRP": "OpcodeVINTRP", "DS": "OpcodeDS",
    "MUBUF": "OpcodeMUBUF", "MTBUF": "OpcodeMTBUF", "MIMG": "OpcodeMIMG", "EXP": "OpcodeEXP",
}

ENCODINGS = (
    (0xFF800000, 0xBE800000, "SOP1", 1),
    (0xFF800000, 0xBF800000, "SOPP", 1),
    (0xFF800000, 0xBF000000, "SOPC", 1),
    (0xFE000000, 0x7E000000, "VOP1", 1),
    (0xFE000000, 0x7C000000, "VOPC", 1),
    (0xFC000000, 0xD0000000, "VOP3", 2),
    (0xFC000000, 0xF8000000, "EXP", 2),
    (0xFC000000, 0xC8000000, "VINTRP", 1),
    (0xFC000000, 0xD8000000, "DS", 2),
    (0xFC000000, 0xE0000000, "MUBUF", 2),
    (0xFC000000, 0xE8000000, "MTBUF", 2),
    (0xFC000000, 0xF0000000, "MIMG", 2),
    (0xF8000000, 0xC0000000, "SMRD", 1),
    (0xF0000000, 0xB0000000, "SOPK", 1),
    (0xC0000000, 0x80000000, "SOP2", 1),
    (0x80000000, 0x00000000, "VOP2", 1),
)

SPECIAL_REGS = {
    104: "flat_scratch_lo", 105: "flat_scratch_hi", 106: "vcc_lo", 107: "vcc_hi", 108: "tba_lo",
    109: "tba_hi", 110: "tma_lo", 111: "tma_hi", 124: "m0", 126: "exec_lo", 127: "exec_hi",
    251: "vccz", 252: "execz", 253: "scc", 254: "lds_direct",
}
PAIR_NAMES = {104: "flat_scratch", 106: "vcc", 108: "tba", 110: "tma", 126: "exec"}
INLINE_FLOATS = {240: 0.5, 241: -0.5, 242: 1.0, 243: -1.0, 244: 2.0, 245: -2.0, 246: 4.0, 247: -4.0}
LITERAL = 255
OMOD_TEXT = {1: "mul:2", 2: "mul:4", 3: "div:2"}
EXPORT_TARGETS = {8: "mrtz", 9: "null"}
VOP3B_OPS = {"V_ADD_I32", "V_ADDC_U32", "V_SUB_I32", "V_SUBB_U32", "V_SUBREV_I32", "V_SUBBREV_U32",
             "V_DIV_SCALE_F32", "V_DIV_SCALE_F64"}
CARRY_IN_OPS = {"V_ADDC_U32", "V_SUBB_U32", "V_SUBBREV_U32"}
WIDE_TYPES = ("B64", "U64", "I64", "F64")

_opcode_cache = {}


def load_opcodes(path=None):
    path = Path(path) if path else DEFAULT_OPCODES_H
    key = str(path)
    if key in _opcode_cache:
        return _opcode_cache[key]
    if not path.exists():
        raise FileNotFoundError(f"shadPS4 opcodes.h not found at {path}; pass --opcodes")
    text = path.read_text(encoding="utf-8", errors="replace")
    tables = {}
    for encoding, enum_name in ENUM_NAMES.items():
        match = re.search(r"enum class " + enum_name + r"\s*:\s*u32\s*\{(.*?)\};", text, re.S)
        if not match:
            raise ValueError(f"{enum_name} missing in {path}")
        table = {}
        for name, value in re.findall(r"^\s*([A-Z0-9_]+)\s*=\s*(\d+)\s*,", match.group(1), re.M):
            if name.startswith("OP_RANGE"):
                continue
            table.setdefault(int(value), name)
        tables[encoding] = table
    _opcode_cache[key] = tables
    return tables


def encoding_of(word):
    for mask, value, name, _ in ENCODINGS:
        if word & mask == value:
            return name
    return None


def encoding_words(name):
    for _, _, enc, words in ENCODINGS:
        if enc == name:
            return words
    return 1


def bits(value, high, low):
    return (value >> low) & ((1 << (high - low + 1)) - 1)


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


@dataclass
class Operand:
    kind: str
    value: object
    count: int = 1
    neg: bool = False
    abs: bool = False

    def text(self, float_context=True):
        body = self.body(float_context)
        if self.abs:
            body = f"|{body}|"
        if self.neg:
            body = f"-{body}"
        return body

    def body(self, float_context=True):
        if self.kind in ("s", "v"):
            if self.count == 1:
                return f"{self.kind}{self.value}"
            return f"{self.kind}[{self.value}:{self.value + self.count - 1}]"
        if self.kind == "ttmp":
            if self.count == 1:
                return f"ttmp{self.value}"
            return f"ttmp[{self.value}:{self.value + self.count - 1}]"
        if self.kind == "special":
            if self.count == 2 and self.value in PAIR_NAMES:
                return PAIR_NAMES[self.value]
            return SPECIAL_REGS.get(self.value, f"special{self.value}")
        if self.kind == "int":
            return str(self.value)
        if self.kind == "float":
            return format_float(self.value)
        if self.kind == "lit":
            if float_context:
                return format_float(f32(self.value))
            return f"0x{self.value:x}"
        return str(self.value)


def format_float(value):
    if value != value:
        return "nan"
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    if value == int(value) and abs(value) < 1e7:
        return f"{value:.1f}"
    text = f"{value:.9g}"
    for digits in range(4, 10):
        candidate = f"{value:.{digits}g}"
        if struct.pack("<f", float(candidate)) == struct.pack("<f", value):
            text = candidate
            break
    return text


def scalar_operand(code, count=1):
    if code <= 103:
        return Operand("s", code, count)
    if 112 <= code <= 123:
        return Operand("ttmp", code - 112, count)
    if code in SPECIAL_REGS:
        return Operand("special", code, count)
    if code == 128:
        return Operand("int", 0)
    if 129 <= code <= 192:
        return Operand("int", code - 128)
    if 193 <= code <= 208:
        return Operand("int", 192 - code)
    if code in INLINE_FLOATS:
        return Operand("float", INLINE_FLOATS[code])
    if code == LITERAL:
        return Operand("lit", None)
    return Operand("unknown", code)


def source_operand(code, count=1):
    if code >= 256:
        return Operand("v", code - 256, count)
    return scalar_operand(code, count)


@dataclass
class Inst:
    offset: int
    encoding: str
    op: int
    name: str
    words: list
    dst: list = field(default_factory=list)
    src: list = field(default_factory=list)
    extra: dict = field(default_factory=dict)

    @property
    def size(self):
        return 4 * len(self.words)

    def text(self):
        return format_inst(self)


def type_suffixes(name):
    return [part for part in name.split("_")[1:] if re.fullmatch(r"[BUIF](8|16|24|32|64)", part)]


def scalar_widths(name):
    types = type_suffixes(name)
    if not types:
        return 1, 1
    dst_wide = types[0] in WIDE_TYPES
    src_wide = types[-1] in WIDE_TYPES
    if name in ("S_LSHL_B64", "S_LSHR_B64", "S_ASHR_I64", "S_BFE_U64", "S_BFE_I64"):
        return 2, (2, 1)
    if name in ("S_BFM_B64",):
        return 2, (1, 1)
    if name.startswith("S_BITSET"):
        return 2 if dst_wide else 1, 1
    if name in ("S_GETPC_B64",):
        return 2, 0
    return (2 if dst_wide else 1), (2 if src_wide else 1)


def vector_widths(name):
    types = type_suffixes(name)
    if not types:
        return 1, 1
    dst_wide = types[0] in WIDE_TYPES
    src_wide = types[-1] in WIDE_TYPES
    if name in ("V_LSHL_B64", "V_LSHR_B64", "V_ASHR_I64", "V_LDEXP_F64", "V_TRIG_PREOP_F64"):
        return 2, (2, 1)
    return (2 if dst_wide else 1), (2 if src_wide else 1)


def decode_one(words, index, offset, ops):
    word = words[index]
    enc = encoding_of(word)
    if enc is None:
        return Inst(offset, "UNKNOWN", 0, f".word 0x{word:08x}", [word])
    length = encoding_words(enc)
    raw = list(words[index:index + length])
    inst = None
    if enc == "SOP2":
        op = bits(word, 29, 23)
        name = ops[enc].get(op, f"SOP2_{op}")
        dw, sw = scalar_widths(name)
        sw = sw if isinstance(sw, tuple) else (sw, sw)
        inst = Inst(offset, enc, op, name, raw, [scalar_operand(bits(word, 22, 16), dw)],
                    [scalar_operand(bits(word, 7, 0), sw[0]), scalar_operand(bits(word, 15, 8), sw[1])])
    elif enc == "SOPK":
        op = bits(word, 27, 23)
        name = ops[enc].get(op, f"SOPK_{op}")
        simm = bits(word, 15, 0)
        simm_signed = simm - 0x10000 if simm & 0x8000 else simm
        inst = Inst(offset, enc, op, name, raw, [scalar_operand(bits(word, 22, 16))], [Operand("int", simm_signed)])
    elif enc == "SOP1":
        op = bits(word, 15, 8)
        name = ops[enc].get(op, f"SOP1_{op}")
        dw, sw = scalar_widths(name)
        dst = [] if name in ("S_SETPC_B64",) else [scalar_operand(bits(word, 22, 16), dw)]
        src = [] if name in ("S_GETPC_B64",) else [scalar_operand(bits(word, 7, 0), sw if isinstance(sw, int) else sw[0])]
        inst = Inst(offset, enc, op, name, raw, dst, src)
    elif enc == "SOPC":
        op = bits(word, 22, 16)
        name = ops[enc].get(op, f"SOPC_{op}")
        _, sw = scalar_widths(name)
        sw = sw if isinstance(sw, tuple) else (sw, sw)
        inst = Inst(offset, enc, op, name, raw, [], [scalar_operand(bits(word, 7, 0), sw[0]), scalar_operand(bits(word, 15, 8), 1)])
    elif enc == "SOPP":
        op = bits(word, 22, 16)
        name = ops[enc].get(op, f"SOPP_{op}")
        simm = bits(word, 15, 0)
        extra = {"simm": simm}
        if "BRANCH" in name:
            rel = simm - 0x10000 if simm & 0x8000 else simm
            extra["target"] = offset + 4 + rel * 4
        inst = Inst(offset, enc, op, name, raw, [], [], extra)
    elif enc == "SMRD":
        op = bits(word, 26, 22)
        name = ops[enc].get(op, f"SMRD_{op}")
        count = 1
        match = re.search(r"DWORDX(\d+)$", name)
        if match:
            count = int(match.group(1))
        base_count = 4 if "BUFFER" in name else 2
        imm = bits(word, 8, 8)
        off = bits(word, 7, 0)
        extra = {}
        if imm:
            extra["offset_dwords"] = off
        elif off == LITERAL:
            extra["literal_offset"] = True
        else:
            extra["offset_sgpr"] = off
        dst = [scalar_operand(bits(word, 21, 15), count)]
        src = [Operand("s", bits(word, 14, 9) * 2, base_count)]
        if name in ("S_MEMTIME",):
            dst = [scalar_operand(bits(word, 21, 15), 2)]
            src = []
        elif name.startswith("S_DCACHE"):
            dst, src = [], []
        inst = Inst(offset, enc, op, name, raw, dst, src, extra)
    elif enc == "VOP2":
        op = bits(word, 30, 25)
        name = ops[enc].get(op, f"VOP2_{op}")
        dw, sw = vector_widths(name)
        sw = sw if isinstance(sw, tuple) else (sw, sw)
        dst = [Operand("v", bits(word, 24, 17), dw)]
        src = [source_operand(bits(word, 8, 0), sw[0]), Operand("v", bits(word, 16, 9), sw[1])]
        extra = {}
        if name in ("V_MADMK_F32", "V_MADAK_F32"):
            extra["k"] = None
        if name in ("V_READLANE_B32",):
            dst = [scalar_operand(bits(word, 24, 17))]
            src = [source_operand(bits(word, 8, 0)), scalar_operand(bits(word, 16, 9))]
        if name in ("V_WRITELANE_B32",):
            src = [source_operand(bits(word, 8, 0)), scalar_operand(bits(word, 16, 9))]
        if name in ("V_CNDMASK_B32",):
            extra["mask"] = Operand("special", 106, 2)
        if name in VOP3B_OPS:
            extra["carry_out"] = Operand("special", 106, 2)
        if name in CARRY_IN_OPS:
            extra["carry_in"] = Operand("special", 106, 2)
        inst = Inst(offset, enc, op, name, raw, dst, src, extra)
    elif enc == "VOP1":
        op = bits(word, 16, 9)
        name = ops[enc].get(op, f"VOP1_{op}")
        dw, sw = vector_widths(name)
        sw = sw if isinstance(sw, int) else sw[0]
        dst = [Operand("v", bits(word, 24, 17), dw)]
        if name == "V_READFIRSTLANE_B32":
            dst = [scalar_operand(bits(word, 24, 17))]
        src = [source_operand(bits(word, 8, 0), sw)]
        if name == "V_NOP":
            dst, src = [], []
        inst = Inst(offset, enc, op, name, raw, dst, src)
    elif enc == "VOPC":
        op = bits(word, 24, 17)
        name = ops[enc].get(op, f"VOPC_{op}")
        _, sw = vector_widths(name)
        sw = sw if isinstance(sw, int) else sw[0]
        dst = [Operand("special", 126 if "CMPX" in name else 106, 2)]
        inst = Inst(offset, enc, op, name, raw, dst, [source_operand(bits(word, 8, 0), sw), Operand("v", bits(word, 16, 9), sw)])
    elif enc == "VOP3":
        word2 = words[index + 1]
        op = bits(word, 25, 17)
        name = ops[enc].get(op, f"VOP3_{op}")
        neg = bits(word2, 31, 29)
        omod = bits(word2, 28, 27)
        codes = [bits(word2, 8, 0), bits(word2, 17, 9), bits(word2, 26, 18)]
        dw, sw = vector_widths(name)
        sw = sw if isinstance(sw, tuple) else (sw, sw, sw)
        if len(sw) == 2:
            sw = (sw[0], sw[1], sw[1])
        extra = {"omod": omod}
        is_cmp = op < 256
        is_vop3b = name in VOP3B_OPS
        if is_vop3b:
            dst = [Operand("v", bits(word, 7, 0), dw)]
            extra["carry_out"] = scalar_operand(bits(word, 14, 8), 2)
            absm = 0
        elif is_cmp:
            dst = [scalar_operand(bits(word, 7, 0), 2)]
            absm = bits(word, 10, 8)
            extra["clamp"] = bits(word, 11, 11)
        else:
            dst = [Operand("v", bits(word, 7, 0), dw)]
            absm = bits(word, 10, 8)
            extra["clamp"] = bits(word, 11, 11)
            if name in ("V_READLANE_B32",):
                dst = [scalar_operand(bits(word, 7, 0))]
            if name in ("V_READFIRSTLANE_B32",):
                dst = [scalar_operand(bits(word, 7, 0))]
        nsrc = vop3_source_count(name, op)
        src = []
        for i in range(nsrc):
            width = sw[i] if i < len(sw) else 1
            if name == "V_CNDMASK_B32" and i == 2:
                width = 2
            if name in CARRY_IN_OPS and i == 2:
                width = 2
            operand = source_operand(codes[i], width)
            if absm & (1 << i):
                operand.abs = True
            if neg & (1 << i):
                operand.neg = True
            src.append(operand)
        inst = Inst(offset, enc, op, name, raw, dst, src, extra)
    elif enc == "VINTRP":
        op = bits(word, 17, 16)
        name = ops[enc].get(op, f"VINTRP_{op}")
        extra = {"attr": bits(word, 15, 10), "chan": bits(word, 9, 8)}
        src = [Operand("v", bits(word, 7, 0))]
        if name == "V_INTERP_MOV_F32":
            src = [Operand("int", bits(word, 7, 0))]
        inst = Inst(offset, enc, op, name, raw, [Operand("v", bits(word, 25, 18))], src, extra)
    elif enc == "DS":
        word2 = words[index + 1]
        op = bits(word, 25, 18)
        name = ops[enc].get(op, f"DS_{op}")
        extra = {"offset0": bits(word, 7, 0), "offset1": bits(word, 15, 8), "gds": bits(word, 17, 17)}
        inst = Inst(offset, enc, op, name, raw, [Operand("v", bits(word2, 31, 24))],
                    [Operand("v", bits(word2, 7, 0)), Operand("v", bits(word2, 15, 8)), Operand("v", bits(word2, 23, 16))], extra)
    elif enc in ("MUBUF", "MTBUF"):
        word2 = words[index + 1]
        if enc == "MUBUF":
            op = bits(word, 24, 18)
        else:
            op = bits(word, 18, 16)
        name = ops[enc].get(op, f"{enc}_{op}")
        count = 1
        match = re.search(r"FORMAT_([XYZW]+)$", name)
        if match:
            count = len(match.group(1))
        match = re.search(r"DWORDX(\d)$", name)
        if match:
            count = int(match.group(1))
        extra = {"offset": bits(word, 11, 0), "offen": bits(word, 12, 12), "idxen": bits(word, 13, 13),
                 "glc": bits(word, 14, 14), "addr64": bits(word, 15, 15), "slc": bits(word2, 22, 22),
                 "tfe": bits(word2, 23, 23), "soffset": scalar_operand(bits(word2, 31, 24))}
        if enc == "MUBUF":
            extra["lds"] = bits(word, 16, 16)
        else:
            extra["dfmt"] = bits(word, 22, 19)
            extra["nfmt"] = bits(word, 25, 23)
        addr_count = max(1, extra["offen"] + extra["idxen"] + 2 * extra["addr64"])
        inst = Inst(offset, enc, op, name, raw, [Operand("v", bits(word2, 15, 8), count)],
                    [Operand("v", bits(word2, 7, 0), addr_count), Operand("s", bits(word2, 20, 16) * 4, 4)], extra)
        if "STORE" in name:
            inst.src.insert(0, inst.dst.pop())
    elif enc == "MIMG":
        word2 = words[index + 1]
        op = bits(word, 24, 18)
        name = ops[enc].get(op, f"MIMG_{op}")
        dmask = bits(word, 11, 8)
        count = 4 if "GATHER" in name else max(1, bin(dmask).count("1"))
        extra = {"dmask": dmask, "unorm": bits(word, 12, 12), "glc": bits(word, 13, 13), "da": bits(word, 14, 14),
                 "r128": bits(word, 15, 15), "tfe": bits(word, 16, 16), "lwe": bits(word, 17, 17), "slc": bits(word, 25, 25)}
        vaddr = Operand("v", bits(word2, 7, 0), 1)
        inst = Inst(offset, enc, op, name, raw, [Operand("v", bits(word2, 15, 8), count)],
                    [vaddr, Operand("s", bits(word2, 20, 16) * 4, 8), Operand("s", bits(word2, 25, 21) * 4, 4)], extra)
        if "STORE" in name:
            inst.src.insert(0, inst.dst.pop())
    elif enc == "EXP":
        word2 = words[index + 1]
        extra = {"en": bits(word, 3, 0), "target": bits(word, 9, 4), "compr": bits(word, 10, 10),
                 "done": bits(word, 11, 11), "vm": bits(word, 12, 12)}
        src = [Operand("v", bits(word2, 7, 0)), Operand("v", bits(word2, 15, 8)),
               Operand("v", bits(word2, 23, 16)), Operand("v", bits(word2, 31, 24))]
        inst = Inst(offset, enc, 0, "EXP", raw, [], src, extra)
    if inst is None:
        inst = Inst(offset, enc, 0, f"{enc}_UNHANDLED", raw)
    return inst


def vop3_source_count(name, op):
    if op < 256:
        return 2
    if 256 <= op < 320:
        return 3 if name in ("V_CNDMASK_B32", "V_ADDC_U32", "V_SUBB_U32", "V_SUBBREV_U32") else 2
    if 384 <= op:
        return 1 if name not in ("V_NOP",) else 0
    three = ("MAD", "FMA", "CUBE", "BFE", "BFI", "LERP", "ALIGN", "MULLIT", "MIN3", "MAX3", "MED3", "SAD",
             "DIV_SCALE", "DIV_FMAS", "DIV_FIXUP", "MQSAD", "QSAD", "MSAD", "CVT_PK_U8")
    if any(key in name for key in three):
        return 3
    return 2


def decode(code, ops, limit=None):
    if len(code) % 4:
        code = code + b"\0" * (4 - len(code) % 4)
    words = list(struct.unpack(f"<{len(code) // 4}I", code))
    end = len(words) if limit is None else min(len(words), (limit + 3) // 4)
    result = []
    index = 0
    while index < end:
        inst = decode_one(words, index, index * 4, ops)
        length = len(inst.words)
        if inst.encoding in ("SOP2", "SOP1", "SOPC", "VOP2", "VOP1", "VOPC") or (inst.encoding == "SMRD" and inst.extra.get("literal_offset")):
            literal_needed = any(o.kind == "lit" for o in inst.src) or "k" in inst.extra or inst.extra.get("literal_offset")
            if literal_needed and index + length < len(words):
                literal = words[index + length]
                for operand in inst.src:
                    if operand.kind == "lit":
                        operand.value = literal
                if "k" in inst.extra:
                    inst.extra["k"] = literal
                if inst.extra.get("literal_offset"):
                    inst.extra["offset_dwords"] = literal
                inst.words.append(literal)
                length += 1
        elif inst.encoding == "SOPK" and inst.name == "S_SETREG_IMM32_B32":
            inst.extra["imm32"] = words[index + length]
            inst.words.append(words[index + length])
            length += 1
        result.append(inst)
        index += length
        if inst.name == "S_ENDPGM" and limit is None:
            break
    return result


def is_float_op(name):
    types = type_suffixes(name)
    if not types:
        return False
    return types[-1].startswith("F")


def format_inst(inst):
    name = inst.name.lower()
    floaty = is_float_op(inst.name)
    parts = []
    if inst.encoding == "SOPP":
        if "target" in inst.extra:
            return f"{name} L{inst.extra['target']:04x}"
        if inst.name == "S_WAITCNT":
            simm = inst.extra["simm"]
            vm, exp, lgkm = simm & 0xF, (simm >> 4) & 7, (simm >> 8) & 0x1F
            fields = []
            if vm != 0xF:
                fields.append(f"vmcnt({vm})")
            if exp != 7:
                fields.append(f"expcnt({exp})")
            if lgkm != 0x1F:
                fields.append(f"lgkmcnt({lgkm})")
            return f"{name} {' & '.join(fields)}"
        if inst.name in ("S_ENDPGM", "S_BARRIER"):
            return name
        return f"{name} {inst.extra['simm']}"
    if inst.encoding == "SMRD":
        text = f"{name} " + ", ".join(o.text(False) for o in inst.dst + inst.src)
        if "offset_dwords" in inst.extra:
            text += f", 0x{inst.extra['offset_dwords']:x}"
        elif "offset_sgpr" in inst.extra:
            text += f", {scalar_operand(inst.extra['offset_sgpr']).text(False)}"
        return text
    if inst.encoding == "VINTRP":
        chan = "xyzw"[inst.extra["chan"]]
        src = inst.src[0].text(False)
        if inst.name == "V_INTERP_MOV_F32":
            src = ["p10", "p20", "p0"][inst.src[0].value] if inst.src[0].value < 3 else src
        return f"{name} {inst.dst[0].text()}, {src}, attr{inst.extra['attr']}.{chan}"
    if inst.encoding == "EXP":
        target = inst.extra["target"]
        if target < 8:
            tname = f"mrt{target}"
        elif target in EXPORT_TARGETS:
            tname = EXPORT_TARGETS[target]
        elif 12 <= target <= 15:
            tname = f"pos{target - 12}"
        elif 32 <= target <= 63:
            tname = f"param{target - 32}"
        else:
            tname = f"target{target}"
        srcs = []
        for i, operand in enumerate(inst.src):
            enabled = inst.extra["en"] & (1 << i)
            srcs.append(operand.text(False) if enabled else "off")
        flags = []
        if inst.extra["compr"]:
            flags.append("compr")
        if inst.extra["done"]:
            flags.append("done")
        if inst.extra["vm"]:
            flags.append("vm")
        return f"exp {tname} " + ", ".join(srcs) + ("" if not flags else " " + " ".join(flags))
    if inst.encoding in ("MUBUF", "MTBUF"):
        text = f"{name} " + ", ".join(o.text(False) for o in inst.dst + inst.src) + f", {inst.extra['soffset'].text(False)}"
        flags = [f for f in ("offen", "idxen", "glc", "addr64", "slc", "tfe") if inst.extra.get(f)]
        if inst.extra["offset"]:
            flags.append(f"offset:{inst.extra['offset']}")
        if inst.encoding == "MTBUF":
            flags.append(f"dfmt:{inst.extra['dfmt']} nfmt:{inst.extra['nfmt']}")
        return text + ("" if not flags else " " + " ".join(flags))
    if inst.encoding == "MIMG":
        text = f"{name} " + ", ".join(o.text(False) for o in inst.dst + inst.src)
        flags = [f"dmask:0x{inst.extra['dmask']:x}"]
        flags += [f for f in ("unorm", "glc", "da", "r128", "tfe", "lwe", "slc") if inst.extra.get(f)]
        return text + " " + " ".join(flags)
    if inst.encoding == "DS":
        return f"{name} " + ", ".join(o.text(False) for o in inst.dst + inst.src) + f" offset0:{inst.extra['offset0']} offset1:{inst.extra['offset1']}"
    operands = [o.text(floaty) for o in inst.dst]
    if "carry_out" in inst.extra and inst.encoding == "VOP3":
        operands.append(inst.extra["carry_out"].text(False))
    operands += [o.text(floaty) for o in inst.src]
    if inst.extra.get("k") is not None:
        literal = format_float(f32(inst.extra["k"]))
        if inst.name == "V_MADMK_F32":
            operands.insert(2, literal)
        else:
            operands.append(literal)
    if "imm32" in inst.extra:
        operands.append(f"0x{inst.extra['imm32']:x}")
    text = f"{name}" + ("_e64" if inst.encoding == "VOP3" and (inst.op < 320 or inst.op >= 384) else "")
    text += " " + ", ".join(operands)
    if inst.encoding == "SOPK":
        text = f"{name} {inst.dst[0].text(False)}, 0x{inst.src[0].value & 0xFFFF:x}"
    if inst.extra.get("clamp"):
        text += " clamp"
    if inst.extra.get("omod"):
        text += " " + OMOD_TEXT[inst.extra["omod"]]
    return text


def disassemble(code, ops, limit=None):
    return [(inst, format_inst(inst)) for inst in decode(code, ops, limit)]


CMP_TEXT = {"F": "false", "LT": "<", "EQ": "==", "LE": "<=", "GT": ">", "LG": "!=", "NE": "!=", "GE": ">=",
            "O": "ordered", "U": "unordered", "NGE": "!>=", "NLG": "!<>", "NGT": "!>", "NLE": "!<=",
            "NEQ": "!==", "NLT": "!<", "TRU": "true", "T": "true", "CLASS": "class"}

UNARY = {
    "V_MOV_B32": "{a}", "V_MOV_B64": "{a}", "V_RCP_F32": "1.0 / {a}", "V_RCP_LEGACY_F32": "1.0 / {a}",
    "V_RCP_IFLAG_F32": "1.0 / {a}", "V_RCP_CLAMP_F32": "1.0 / {a}", "V_RSQ_F32": "rsqrt({a})",
    "V_RSQ_LEGACY_F32": "rsqrt({a})", "V_RSQ_CLAMP_F32": "rsqrt({a})", "V_SQRT_F32": "sqrt({a})",
    "V_LOG_F32": "log2({a})", "V_LOG_CLAMP_F32": "log2({a})", "V_LOG_LEGACY_F32": "log2({a})",
    "V_EXP_F32": "exp2({a})", "V_EXP_LEGACY_F32": "exp2({a})", "V_FRACT_F32": "fract({a})",
    "V_FLOOR_F32": "floor({a})", "V_CEIL_F32": "ceil({a})", "V_TRUNC_F32": "trunc({a})",
    "V_RNDNE_F32": "roundEven({a})", "V_SIN_F32": "sin({a} * 2pi)", "V_COS_F32": "cos({a} * 2pi)",
    "V_CVT_F32_U32": "float(uint {a})", "V_CVT_F32_I32": "float(int {a})", "V_CVT_U32_F32": "uint({a})",
    "V_CVT_I32_F32": "int({a})", "V_CVT_RPI_I32_F32": "int(round({a}))", "V_CVT_FLR_I32_F32": "int(floor({a}))",
    "V_CVT_F16_F32": "half({a})", "V_CVT_F32_F16": "float(half {a})", "V_CVT_F32_UBYTE0": "float({a} & 0xff)",
    "V_CVT_F32_UBYTE1": "float(({a} >> 8) & 0xff)", "V_CVT_F32_UBYTE2": "float(({a} >> 16) & 0xff)",
    "V_CVT_F32_UBYTE3": "float({a} >> 24)", "V_NOT_B32": "~{a}", "V_BFREV_B32": "bitreverse({a})",
    "V_FFBH_U32": "findMSB({a})", "V_FFBL_B32": "findLSB({a})", "V_FRACT_F64": "fract({a})",
    "V_CVT_F64_F32": "double({a})", "V_CVT_F32_F64": "float({a})", "V_FREXP_MANT_F32": "frexp_mant({a})",
    "V_FREXP_EXP_I32_F32": "frexp_exp({a})", "V_READFIRSTLANE_B32": "readfirstlane({a})",
    "V_CVT_OFF_F32_I4": "cvt_off_i4({a})",
}
BINARY = {
    "V_ADD_F32": "{a} + {b}", "V_SUB_F32": "{a} - {b}", "V_SUBREV_F32": "{b} - {a}", "V_MUL_F32": "{a} * {b}",
    "V_MUL_LEGACY_F32": "{a} * {b}", "V_MIN_F32": "min({a}, {b})", "V_MAX_F32": "max({a}, {b})",
    "V_MIN_LEGACY_F32": "min({a}, {b})", "V_MAX_LEGACY_F32": "max({a}, {b})", "V_ADD_I32": "{a} + {b}",
    "V_SUB_I32": "{a} - {b}", "V_SUBREV_I32": "{b} - {a}", "V_AND_B32": "{a} & {b}", "V_OR_B32": "{a} | {b}",
    "V_XOR_B32": "{a} ^ {b}", "V_LSHL_B32": "{a} << {b}", "V_LSHR_B32": "{a} >> {b}", "V_ASHR_I32": "{a} >> {b}",
    "V_LSHLREV_B32": "{b} << {a}", "V_LSHRREV_B32": "{b} >> {a}", "V_ASHRREV_I32": "{b} >> {a}",
    "V_MUL_I32_I24": "{a} * {b}", "V_MUL_U32_U24": "{a} * {b}", "V_MUL_LO_U32": "{a} * {b}",
    "V_MUL_LO_I32": "{a} * {b}", "V_MUL_HI_U32": "mulhi({a}, {b})", "V_MIN_I32": "min({a}, {b})",
    "V_MAX_I32": "max({a}, {b})", "V_MIN_U32": "min({a}, {b})", "V_MAX_U32": "max({a}, {b})",
    "V_BFM_B32": "bfm({a}, {b})", "V_LDEXP_F32": "ldexp({a}, {b})", "V_CVT_PKRTZ_F16_F32": "packHalf2x16({a}, {b})",
    "V_MBCNT_LO_U32_B32": "mbcnt_lo({a}, {b})", "V_MBCNT_HI_U32_B32": "mbcnt_hi({a}, {b})",
    "V_BCNT_U32_B32": "bitcount({a}) + {b}", "V_CVT_PKNORM_I16_F32": "packSnorm2x16({a}, {b})",
    "V_CVT_PKNORM_U16_F32": "packUnorm2x16({a}, {b})", "V_CVT_PK_U16_U32": "pack_u16({a}, {b})",
    "V_CVT_PK_I16_I32": "pack_i16({a}, {b})", "V_ADD_F64": "{a} + {b}", "V_MUL_F64": "{a} * {b}",
    "V_MIN_F64": "min({a}, {b})", "V_MAX_F64": "max({a}, {b})", "V_ADDC_U32": "{a} + {b} + carry",
    "V_SUBB_U32": "{a} - {b} - borrow", "V_SUBBREV_U32": "{b} - {a} - borrow",
}
TERNARY = {
    "V_MAD_F32": "{a} * {b} + {c}", "V_MAD_LEGACY_F32": "{a} * {b} + {c}", "V_FMA_F32": "fma({a}, {b}, {c})",
    "V_MIN3_F32": "min({a}, {b}, {c})", "V_MAX3_F32": "max({a}, {b}, {c})", "V_MED3_F32": "med3({a}, {b}, {c})",
    "V_MIN3_I32": "min({a}, {b}, {c})", "V_MAX3_I32": "max({a}, {b}, {c})", "V_MED3_I32": "med3({a}, {b}, {c})",
    "V_MIN3_U32": "min({a}, {b}, {c})", "V_MAX3_U32": "max({a}, {b}, {c})", "V_MED3_U32": "med3({a}, {b}, {c})",
    "V_BFE_U32": "bfe_u({a}, {b}, {c})", "V_BFE_I32": "bfe_i({a}, {b}, {c})", "V_BFI_B32": "bfi({a}, {b}, {c})",
    "V_MAD_U32_U24": "{a} * {b} + {c}", "V_MAD_I32_I24": "{a} * {b} + {c}", "V_CUBEID_F32": "cubeid({a}, {b}, {c})",
    "V_CUBESC_F32": "cubesc({a}, {b}, {c})", "V_CUBETC_F32": "cubetc({a}, {b}, {c})", "V_CUBEMA_F32": "cubema({a}, {b}, {c})",
    "V_ALIGNBIT_B32": "alignbit({a}, {b}, {c})", "V_ALIGNBYTE_B32": "alignbyte({a}, {b}, {c})",
    "V_LERP_U8": "lerp_u8({a}, {b}, {c})", "V_DIV_FMAS_F32": "div_fmas({a}, {b}, {c})",
    "V_DIV_FIXUP_F32": "div_fixup({a}, {b}, {c})", "V_DIV_SCALE_F32": "div_scale({a}, {b}, {c})",
    "V_FMA_F64": "fma({a}, {b}, {c})", "V_SAD_U32": "sad({a}, {b}, {c})", "V_CVT_PK_U8_F32": "pack_u8({a}, {b}, {c})",
    "V_MULLIT_F32": "mullit({a}, {b}, {c})",
}
SCALAR_BINARY = {
    "S_ADD_U32": "{a} + {b}", "S_SUB_U32": "{a} - {b}", "S_ADD_I32": "{a} + {b}", "S_SUB_I32": "{a} - {b}",
    "S_MIN_I32": "min({a}, {b})", "S_MIN_U32": "min({a}, {b})", "S_MAX_I32": "max({a}, {b})", "S_MAX_U32": "max({a}, {b})",
    "S_AND_B32": "{a} & {b}", "S_AND_B64": "{a} & {b}", "S_OR_B32": "{a} | {b}", "S_OR_B64": "{a} | {b}",
    "S_XOR_B32": "{a} ^ {b}", "S_XOR_B64": "{a} ^ {b}", "S_ANDN2_B32": "{a} & ~{b}", "S_ANDN2_B64": "{a} & ~{b}",
    "S_ORN2_B32": "{a} | ~{b}", "S_ORN2_B64": "{a} | ~{b}", "S_NAND_B64": "~({a} & {b})", "S_NOR_B64": "~({a} | {b})",
    "S_XNOR_B64": "~({a} ^ {b})", "S_LSHL_B32": "{a} << {b}", "S_LSHR_B32": "{a} >> {b}", "S_ASHR_I32": "{a} >> {b}",
    "S_LSHL_B64": "{a} << {b}", "S_LSHR_B64": "{a} >> {b}", "S_MUL_I32": "{a} * {b}", "S_BFE_U32": "bfe_u({a}, {b})",
    "S_BFE_I32": "bfe_i({a}, {b})", "S_BFM_B32": "bfm({a}, {b})", "S_CSELECT_B32": "scc ? {a} : {b}",
    "S_CSELECT_B64": "scc ? {a} : {b}", "S_ABSDIFF_I32": "abs({a} - {b})",
}
SCALAR_UNARY = {
    "S_MOV_B32": "{a}", "S_MOV_B64": "{a}", "S_NOT_B32": "~{a}", "S_NOT_B64": "~{a}", "S_WQM_B64": "wqm({a})",
    "S_WQM_B32": "wqm({a})", "S_BREV_B32": "bitreverse({a})", "S_BCNT1_I32_B64": "bitcount({a})",
    "S_BCNT1_I32_B32": "bitcount({a})", "S_FF1_I32_B64": "findLSB({a})", "S_FF1_I32_B32": "findLSB({a})",
    "S_FLBIT_I32_B32": "findMSB({a})", "S_SEXT_I32_I8": "sext8({a})", "S_SEXT_I32_I16": "sext16({a})",
    "S_CMOV_B32": "scc ? {a} : {d}", "S_CMOV_B64": "scc ? {a} : {d}",
}
SAMPLE_EXTRA_ORDER = ("O", "B", "C", "D")


class Lifter:
    def __init__(self, env):
        self.env = env
        self.stag = {}
        self.sname = {}
        self.vname = {}
        self.sample_count = 0
        self.lines = []
        for sgpr, tag in env.get("sgpr_tags", {}).items():
            self.stag[sgpr] = tag
        for sgpr, text in env.get("sgpr_names", {}).items():
            self.sname[sgpr] = text
        for vgpr, text in env.get("vgpr_names", {}).items():
            self.vname[vgpr] = text

    def operand(self, operand, float_context=True):
        if operand.kind == "s" and operand.count == 1 and operand.value in self.sname:
            text = self.sname[operand.value]
        elif operand.kind == "v" and operand.count == 1 and operand.value in self.vname:
            text = self.vname[operand.value]
        else:
            text = operand.body(float_context)
        if operand.abs:
            text = f"abs({text})"
        if operand.neg:
            text = f"-{text}" if re.fullmatch(r"[\w.\[\]#()]+", text) else f"-({text})"
        return text

    def kill_scalar(self, operand):
        if operand.kind != "s":
            return
        for i in range(operand.count):
            self.stag.pop(operand.value + i, None)
            self.sname.pop(operand.value + i, None)

    def kill_vector(self, operand):
        if operand.kind != "v":
            return
        for i in range(operand.count):
            self.vname.pop(operand.value + i, None)

    def tag_text(self, tag):
        kind = tag[0]
        if kind == "cb":
            return self.env["constant_name"](tag[1], tag[2])
        if kind in ("T#", "S#", "V#cb", "V#", "V#vb"):
            return self.env["resource_name"](kind, tag[1]) + f"[{tag[2]}]"
        if kind == "ptr":
            return f"ptr({tag[1]})"
        return str(tag)

    def resolve_load(self, base_tag, dword):
        kind = base_tag[0]
        if kind != "ptr":
            return None
        table = base_tag[1]
        if table == "eud":
            return self.env.get("eud_tags", {}).get(dword)
        if table in ("T#table", "RW#table"):
            return ("T#", dword // 8, dword % 8)
        if table == "S#table":
            return ("S#", dword // 4, dword % 4)
        if table == "cbtable":
            return ("V#cb", dword // 4, dword % 4)
        if table == "vbtable":
            return ("V#vb", dword // 4, dword % 4)
        return None

    def descriptor(self, sgpr):
        tag = self.stag.get(sgpr)
        if tag and tag[0] in ("T#", "S#", "V#cb", "V#", "V#vb") and tag[2] == 0:
            return tag
        return None

    def float_ctx(self, inst):
        return is_float_op(inst.name)

    def lift(self, inst):
        name = inst.name
        enc = inst.encoding
        if name in ("S_WAITCNT", "S_NOP", "S_DCACHE_INV", "S_SETPRIO", "V_NOP"):
            return None
        if enc == "SMRD":
            return self.lift_smrd(inst)
        if enc in ("SOP1", "SOP2", "SOPK", "SOPC"):
            return self.lift_scalar(inst)
        if enc == "SOPP":
            return self.lift_sopp(inst)
        if enc == "VINTRP":
            return self.lift_interp(inst)
        if enc == "MIMG":
            return self.lift_image(inst)
        if enc in ("MUBUF", "MTBUF"):
            return self.lift_buffer(inst)
        if enc == "EXP":
            return self.lift_export(inst)
        if enc in ("VOP1", "VOP2", "VOP3", "VOPC"):
            return self.lift_vector(inst)
        return None

    def lift_smrd(self, inst):
        dst = inst.dst[0] if inst.dst else None
        if dst is None:
            return None
        base = inst.src[0].value
        base_tag = self.stag.get(base)
        count = dst.count
        values = []
        if inst.name.startswith("S_LOAD"):
            dword = inst.extra.get("offset_dwords")
            tags = []
            for i in range(count):
                tag = self.resolve_load(base_tag, dword + i) if (base_tag and dword is not None) else None
                tags.append(tag)
            self.kill_scalar(dst)
            for i, tag in enumerate(tags):
                if tag:
                    self.stag[dst.value + i] = tag
            if tags and tags[0]:
                label = self.env["resource_name"](tags[0][0], tags[0][1]) if tags[0][0] in ("T#", "S#", "V#cb", "V#", "V#vb") else str(tags[0])
                return f"{dst.body()} = {label}"
            return f"{dst.body()} = load({inst.src[0].body()}, {dword})"
        descriptor = self.descriptor(base)
        dword = inst.extra.get("offset_dwords")
        self.kill_scalar(dst)
        if descriptor and descriptor[0] == "V#cb" and dword is not None:
            slot = descriptor[1]
            for i in range(count):
                byte = (dword + i) * 4
                self.stag[dst.value + i] = ("cb", slot, byte)
                self.sname[dst.value + i] = self.env["constant_name"](slot, byte)
            first = self.sname[dst.value]
            if count == 1:
                return f"{dst.body()} = {first}"
            names = [self.sname[dst.value + i] for i in range(count)]
            return f"{dst.body()} = " + group_names(names)
        if descriptor and descriptor[0] == "V#cb":
            offset = scalar_operand(inst.extra["offset_sgpr"]).body() if "offset_sgpr" in inst.extra else "?"
            label = self.env["resource_name"]("V#cb", descriptor[1])
            return f"{dst.body()} = {label}[byte {self.operand(scalar_operand(inst.extra['offset_sgpr']), False) if 'offset_sgpr' in inst.extra else offset}]"
        return f"{dst.body()} = buffer_load({inst.src[0].body()}, {dword})"

    def lift_scalar(self, inst):
        name = inst.name
        if name == "S_SWAPPC_B64":
            fetch = self.env.get("fetch", [])
            self.kill_scalar(inst.dst[0])
            parts = []
            for vgpr, text in fetch:
                self.vname[vgpr] = text
                parts.append(f"v{vgpr} = {text}")
            return "fetch vertex: " + ", ".join(parts) if parts else "call fetch shader"
        if name in ("S_MOV_B32", "S_MOV_B64") and inst.dst and inst.dst[0].kind == "s" and inst.src[0].kind == "s":
            dst, src = inst.dst[0], inst.src[0]
            moved = [(self.stag.get(src.value + i), self.sname.get(src.value + i)) for i in range(dst.count)]
            self.kill_scalar(dst)
            for i, (tag, text) in enumerate(moved):
                if tag:
                    self.stag[dst.value + i] = tag
                if text:
                    self.sname[dst.value + i] = text
            return f"{dst.body()} = {self.operand(src, False) if dst.count == 1 else src.body()}"
        if name.endswith("SAVEEXEC_B64"):
            dst = inst.dst[0]
            self.kill_scalar(dst)
            op = name[2:name.index("_SAVEEXEC")].lower()
            src = self.operand(inst.src[0], False)
            return f"{dst.body()} = exec; exec = {src} {'&' if op == 'and' else op} exec   // if"
        if inst.dst and inst.dst[0].kind == "special" and inst.dst[0].value == 126:
            if name == "S_ANDN2_B64" and inst.src[1].kind == "special" and inst.src[1].value == 126:
                return f"exec = {self.operand(inst.src[0], False)} & ~exec   // else"
            if name == "S_OR_B64":
                return f"exec |= {self.operand(inst.src[1] if inst.src[0].kind == 'special' else inst.src[0], False)}   // endif"
        if inst.encoding == "SOPC":
            cond = name.split("_")[2]
            symbol = CMP_TEXT.get(cond, cond.lower())
            return f"scc = {self.operand(inst.src[0], False)} {symbol} {self.operand(inst.src[1], False)}"
        if inst.encoding == "SOPK":
            dst = inst.dst[0]
            if name == "S_MOVK_I32":
                self.kill_scalar(dst)
                self.sname[dst.value] = str(inst.src[0].value) if dst.kind == "s" else None
                return f"{dst.body()} = {inst.src[0].value}"
            return f"{name.lower()} {dst.body()}, {inst.src[0].value}"
        floatish = False
        if name in SCALAR_UNARY and inst.src:
            text = SCALAR_UNARY[name].format(a=self.operand(inst.src[0], floatish), d=inst.dst[0].body())
        elif name in SCALAR_BINARY and len(inst.src) == 2:
            text = SCALAR_BINARY[name].format(a=self.operand(inst.src[0], floatish), b=self.operand(inst.src[1], floatish))
        else:
            text = f"{name.lower()}(" + ", ".join(self.operand(o, floatish) for o in inst.src) + ")"
        if not inst.dst:
            return text
        dst = inst.dst[0]
        carried_tag = None
        if name in ("S_AND_B32", "S_OR_B32", "S_ANDN2_B32") and inst.src[0].kind == "s":
            carried_tag = self.stag.get(inst.src[0].value)
        self.kill_scalar(dst)
        if name in ("S_MOV_B32",) and dst.kind == "s" and inst.src[0].kind in ("int", "float", "lit"):
            self.sname[dst.value] = literal_text(inst.src[0])
        if carried_tag and carried_tag[0] in ("T#", "S#", "V#cb", "V#", "V#vb"):
            self.stag[dst.value] = carried_tag
        return f"{dst.text(False)} = {text}"

    def lift_sopp(self, inst):
        name = inst.name
        if name == "S_ENDPGM":
            return "end"
        if "target" in inst.extra:
            cond = name.replace("S_CBRANCH_", "").replace("S_BRANCH", "").lower()
            target = f"L{inst.extra['target']:04x}"
            if not cond:
                return f"goto {target}"
            return f"if ({cond}) goto {target}"
        return name.lower()

    def lift_interp(self, inst):
        dst = inst.dst[0]
        attr, chan = inst.extra["attr"], "xyzw"[inst.extra["chan"]]
        label = self.env.get("interp_name", lambda a: f"attr{a}")(attr)
        if inst.name == "V_INTERP_P1_F32":
            self.kill_vector(dst)
            return None
        if inst.name == "V_INTERP_MOV_F32":
            mode = ["p10", "p20", "p0"][inst.src[0].value] if inst.src[0].value < 3 else "?"
            text = f"{label}.{chan} (flat {mode})"
        else:
            text = f"{label}.{chan}"
        self.kill_vector(dst)
        self.vname[dst.value] = text
        return f"v{dst.value} = {text}"

    def image_info(self, inst):
        tsharp = self.descriptor(inst.src[1].value)
        ssharp = self.descriptor(inst.src[2].value)
        tex = self.env["resource_name"]("T#", tsharp[1]) if tsharp else inst.src[1].body()
        dims = self.env.get("texture_dims", lambda slot: 2)(tsharp[1]) if tsharp else 2
        samp = self.env["resource_name"]("S#", ssharp[1]) if ssharp else inst.src[2].body()
        return tex, samp, dims

    def lift_image(self, inst):
        name = inst.name
        tex, samp, dims = self.image_info(inst)
        dst = inst.dst[0] if inst.dst else None
        mods = name.split("_")[2:] if name.startswith("IMAGE_SAMPLE") or name.startswith("IMAGE_GATHER4") else []
        vaddr = inst.src[0].value
        pieces = []
        pos = vaddr
        labels = []
        if "O" in mods:
            labels.append(("offset", 1))
        if "B" in mods:
            labels.append(("bias", 1))
        if "C" in mods:
            labels.append(("zref", 1))
        if "D" in mods or "CD" in mods:
            labels.append(("ddx", dims if dims < 4 else 3))
            labels.append(("ddy", dims if dims < 4 else 3))
        body = dims + (1 if inst.extra.get("da") else 0)
        labels.append(("uv", body))
        if "L" in mods:
            labels.append(("lod", 1))
        if "CL" in mods:
            labels.append(("clamp", 1))
        if name.startswith("IMAGE_LOAD"):
            labels = [("coord", dims + (1 if inst.extra.get("da") else 0))]
            if name.startswith("IMAGE_LOAD_MIP"):
                labels.append(("mip", 1))
        if name == "IMAGE_GET_RESINFO":
            labels = [("mip", 1)]
        for label, count in labels:
            regs = [self.operand(Operand("v", pos + i)) for i in range(count)]
            pieces.append(f"{label}=" + (regs[0] if count == 1 else "(" + ", ".join(regs) + ")"))
            pos += count
        dmask = inst.extra["dmask"]
        channels = "".join(c for i, c in enumerate("xyzw") if dmask & (1 << i))
        if name.startswith("IMAGE_GATHER4"):
            channels = "xyzw"
            pieces.append(f"gather_channel={'xyzw'[max(0, (dmask & -dmask).bit_length() - 1)]}")
        self.sample_count += 1
        result = f"t{self.sample_count}"
        opname = name.lower().replace("image_", "")
        call = f"{opname}({tex}, {samp}, " + ", ".join(pieces) + ")" if not name.startswith("IMAGE_LOAD") and name != "IMAGE_GET_RESINFO" else f"{opname}({tex}, " + ", ".join(pieces) + ")"
        if dst is None:
            return call
        self.kill_vector(dst)
        for i, c in enumerate(channels[:dst.count]):
            self.vname[dst.value + i] = f"{result}.{c}"
        return f"{result}.{channels} = {call}"

    def lift_buffer(self, inst):
        dst = inst.dst[0] if inst.dst else None
        vsharp = self.descriptor(inst.src[-1].value)
        label = self.env["resource_name"](vsharp[0], vsharp[1]) if vsharp else inst.src[-1].body()
        addr = self.operand(inst.src[-2]) if inst.src[-2].count == 1 else inst.src[-2].body()
        text = f"{inst.name.lower()}({label}, {addr}, offset={inst.extra['offset']}, soffset={self.operand(inst.extra['soffset'], False)})"
        if dst is None or "STORE" in inst.name:
            return text
        self.kill_vector(dst)
        return f"{dst.body()} = {text}"

    def lift_export(self, inst):
        target = inst.extra["target"]
        label = self.env.get("export_name", lambda t: None)(target) or format_inst(inst).split()[1]
        comps = []
        for i, operand in enumerate(inst.src):
            if not inst.extra["en"] & (1 << i):
                comps.append("_")
            else:
                comps.append(self.operand(operand, False))
        if inst.extra["compr"]:
            comps = [f"unpackHalf2x16({comps[0]})", f"unpackHalf2x16({comps[1]})"]
        return f"OUT {label} = (" + ", ".join(comps) + ")"

    def lift_vector(self, inst):
        name = inst.name
        floaty = self.float_ctx(inst)
        srcs = [self.operand(o, floaty) for o in inst.src]
        if inst.encoding == "VOPC" or (inst.encoding == "VOP3" and inst.op < 256):
            parts = name.split("_")
            cond = parts[2]
            symbol = CMP_TEXT.get(cond, cond.lower())
            expr = f"{srcs[0]} {symbol} {srcs[1]}" if symbol not in ("true", "false") else symbol
            if cond == "CLASS":
                expr = f"class({srcs[0]}, {srcs[1]})"
            dst = inst.dst[0]
            self.kill_scalar(dst)
            if "CMPX" in name:
                return f"exec = {dst.body()} = ({expr})" if inst.encoding == "VOP3" else f"exec &= ({expr})"
            return f"{dst.body()} = ({expr})"
        if name == "V_CNDMASK_B32":
            mask = inst.extra.get("mask")
            mask_text = mask.body() if mask else self.operand(inst.src[2], False)
            expr = f"{mask_text} ? {srcs[1]} : {srcs[0]}"
        elif name in ("V_MAC_F32", "V_MAC_LEGACY_F32"):
            expr = f"{srcs[0]} * {srcs[1]} + {self.operand(inst.dst[0])}"
        elif name == "V_MADMK_F32":
            expr = f"{srcs[0]} * {format_float(f32(inst.extra['k']))} + {srcs[1]}"
        elif name == "V_MADAK_F32":
            expr = f"{srcs[0]} * {srcs[1]} + {format_float(f32(inst.extra['k']))}"
        elif name in UNARY:
            expr = UNARY[name].format(a=srcs[0])
        elif name in BINARY:
            expr = BINARY[name].format(a=srcs[0], b=srcs[1])
        elif name in TERNARY:
            expr = TERNARY[name].format(a=srcs[0], b=srcs[1], c=srcs[2])
        else:
            expr = f"{name.lower()}(" + ", ".join(srcs) + ")"
        if inst.extra.get("omod"):
            expr = f"({expr}) * {['', '2.0', '4.0', '0.5'][inst.extra['omod']]}"
        if inst.extra.get("clamp"):
            expr = f"saturate({expr})"
        dst = inst.dst[0] if inst.dst else None
        if dst is None:
            return expr
        if dst.kind == "s":
            self.kill_scalar(dst)
            return f"{dst.body()} = {expr}"
        leaf = None
        if name == "V_MOV_B32" and not inst.extra.get("clamp") and not inst.extra.get("omod"):
            if inst.src[0].kind in ("int", "float", "lit") or (inst.src[0].kind == "s" and inst.src[0].value in self.sname):
                leaf = srcs[0]
        self.kill_vector(dst)
        if leaf is not None:
            self.vname[dst.value] = leaf
        return f"{dst.body()} = {expr}"

    def snapshot(self):
        return dict(self.stag), dict(self.sname), dict(self.vname)

    def restore(self, snap):
        self.stag, self.sname, self.vname = dict(snap[0]), dict(snap[1]), dict(snap[2])

    @staticmethod
    def intersect(snaps):
        merged = []
        for part in range(3):
            common = dict(snaps[0][part])
            for other in snaps[1:]:
                common = {key: value for key, value in common.items() if other[part].get(key) == value}
            merged.append(common)
        return tuple(merged)

    def forget(self, registers):
        for kind, index in registers:
            if kind == "s":
                self.stag.pop(index, None)
                self.sname.pop(index, None)
            else:
                self.vname.pop(index, None)

    def run(self, insts):
        out = []
        branches = [(inst.offset, inst.extra["target"]) for inst in insts if inst.encoding == "SOPP" and "target" in inst.extra]
        targets = {target for _, target in branches}
        loop_writes = {}
        for source, target in branches:
            if target <= source:
                written = loop_writes.setdefault(target, set())
                for inst in insts:
                    if target <= inst.offset <= source:
                        for operand in inst.dst:
                            if operand.kind in ("s", "v"):
                                for i in range(operand.count):
                                    written.add((operand.kind, operand.value + i))
        pending = {}
        live = True
        self.divergence = 0
        for inst in insts:
            if inst.offset in targets:
                snaps = pending.pop(inst.offset, [])
                if live:
                    snaps.append(self.snapshot())
                if snaps:
                    self.restore(self.intersect(snaps))
                self.forget(loop_writes.get(inst.offset, ()))
                out.append((inst, f"L{inst.offset:04x}:", True))
                live = True
            try:
                text = self.lift(inst)
            except Exception as error:
                text = f"<lift error {error!r}>"
            if self.divergence and inst.encoding in ("VOP1", "VOP2", "VOP3", "VINTRP", "MIMG", "MUBUF", "MTBUF"):
                for operand in inst.dst:
                    if operand.kind == "v":
                        self.kill_vector(operand)
            if inst.name.endswith("SAVEEXEC_B64"):
                self.divergence += 1
            elif inst.dst and inst.dst[0].kind == "special" and inst.dst[0].value == 126 and inst.name in ("S_OR_B64", "S_MOV_B64"):
                self.divergence = max(0, self.divergence - 1)
            if text is not None:
                out.append((inst, text, False))
            if inst.encoding == "SOPP" and "target" in inst.extra:
                if inst.extra["target"] > inst.offset:
                    pending.setdefault(inst.extra["target"], []).append(self.snapshot())
                if inst.name == "S_BRANCH":
                    live = False
            if inst.name == "S_ENDPGM":
                live = False
        return out


def literal_text(operand):
    if operand.kind == "lit":
        value = f32(operand.value)
        if abs(value) > 1e-30 and abs(value) < 1e30:
            return format_float(value)
        return f"0x{operand.value:x}"
    return operand.body()


def group_names(names):
    stems = [n.rsplit(".", 1)[0] if "." in n else n for n in names]
    comps = [n.rsplit(".", 1)[1] if "." in n else "" for n in names]
    if len(set(stems)) == 1 and all(len(c) == 1 for c in comps):
        return f"{stems[0]}.{''.join(comps)}"
    return "(" + ", ".join(names) + ")"


def main():
    ap = argparse.ArgumentParser(description="Disassemble raw GCN (GFX7, PS4) shader code.")
    ap.add_argument("binary", type=Path)
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--length", type=lambda s: int(s, 0))
    ap.add_argument("--opcodes", type=Path, help="shadPS4 src/shader_recompiler/frontend/opcodes.h")
    args = ap.parse_args()
    ops = load_opcodes(args.opcodes)
    data = args.binary.read_bytes()[args.offset:]
    if args.length:
        data = data[:args.length]
    for inst, text in disassemble(data, ops, args.length):
        print(f"{inst.offset:04x}: {' '.join(f'{w:08x}' for w in inst.words):<18} {text}")


if __name__ == "__main__":
    main()
