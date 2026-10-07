import re

import gcn

PREC_ATOM = 100
PREC_UNARY = 90
PREC_MUL = 80
PREC_ADD = 70
PREC_CMP = 60
PREC_AND = 50
PREC_OR = 40
PREC_SELECT = 30
NAME_LENGTH_LIMIT = 110


class Node:
    __slots__ = ("parts", "prec", "args", "kind", "name", "order", "uses", "value", "keep")

    def __init__(self, parts, prec, args=(), kind="expr", order=0, value=None, keep=False):
        self.parts = parts
        self.prec = prec
        self.args = list(args)
        self.kind = kind
        self.name = None
        self.order = order
        self.uses = 0
        self.value = value
        self.keep = keep


class LoopError(Exception):
    pass


def number_text(value):
    return gcn.format_float(value)


class ExprBuilder:
    def __init__(self, env):
        self.env = env
        self.base = gcn.Lifter(env)
        self.order = 0
        self.v = {}
        self.s = {}
        self.scc = None
        self.cond = None
        self.active = []
        self.exec_stack = []
        self.regions = []
        self.statements = []
        self.live_mask = None
        self.samples = 0
        self.dead = False
        for index, text in env.get("vgpr_names", {}).items():
            self.v[index] = self.leaf(text)
        for index, text in env.get("sgpr_names", {}).items():
            self.s[index] = self.leaf(text)

    def next_order(self):
        self.order += 1
        return self.order

    def leaf(self, text, value=None):
        return Node([text], PREC_ATOM, kind="leaf", order=self.next_order(), value=value)

    def number(self, value):
        return self.leaf(number_text(value), value=value)

    def node(self, parts, prec, args, kind="expr", keep=False):
        return Node(parts, prec, args, kind=kind, order=self.next_order(), keep=keep)

    def call(self, name, *args):
        if name in ("max", "min") and len(args) == 2 and args[0] is args[1]:
            return args[0]
        parts = [name + "("]
        for i in range(len(args)):
            if i:
                parts.append(", ")
            parts.append((i, 0))
        parts.append(")")
        return self.node(parts, PREC_ATOM, args)

    def binop(self, op, a, b):
        if op == "*":
            if a.value == 1.0:
                return b
            if b.value == 1.0:
                return a
            if a.value == 0.0 or b.value == 0.0:
                return self.number(0.0)
            if a.value == -1.0:
                return self.neg(b)
            if b.value == -1.0:
                return self.neg(a)
        if op == "+":
            if a.value == 0.0:
                return b
            if b.value == 0.0:
                return a
            if b.kind == "neg":
                return self.binop("-", a, b.args[0])
            if a.kind == "neg":
                return self.binop("-", b, a.args[0])
            if b.value is not None and b.value < 0:
                return self.binop("-", a, self.number(-b.value))
        if op == "-" and b.value == 0.0:
            return a
        if op == "-" and b.kind == "neg":
            return self.binop("+", a, b.args[0])
        if a.value is not None and b.value is not None:
            try:
                result = {"+": a.value + b.value, "-": a.value - b.value, "*": a.value * b.value,
                          "/": a.value / b.value if b.value else None}[op]
            except Exception:
                result = None
            if result is not None:
                return self.number(result)
        prec = PREC_MUL if op in "*/" else PREC_ADD
        right = prec + 1 if op in "-/" else prec
        return self.node([(0, prec), f" {op} ", (1, right)], prec, [a, b])

    def neg(self, a):
        if a.kind == "neg":
            return a.args[0]
        if a.value is not None:
            return self.number(-a.value)
        node = self.node(["-", (0, PREC_UNARY)], PREC_UNARY, [a])
        node.kind = "neg"
        return node

    def absf(self, a):
        if a.value is not None:
            return self.number(abs(a.value))
        return self.call("abs", a)

    def logic_not(self, a):
        if a.kind == "not":
            return a.args[0]
        node = self.node(["!", (0, PREC_UNARY)], PREC_UNARY, [a], kind="not")
        return node

    def logic(self, op, a, b):
        if a is None:
            return b
        if b is None:
            return a
        prec = PREC_AND if op == "&&" else PREC_OR
        return self.node([(0, prec), f" {op} ", (1, prec + 1)], prec, [a, b], kind="bool")

    def compare(self, op, a, b):
        return self.node([(0, PREC_CMP + 1), f" {op} ", (1, PREC_CMP + 1)], PREC_CMP, [a, b], kind="bool")

    def select(self, c, a, b):
        if a is b:
            return a
        if b is not None and b.kind == "select" and b.args[0] is c:
            b = b.args[2]
        if b is None:
            return a
        return self.node([(0, PREC_SELECT + 1), " ? ", (1, PREC_SELECT + 1), " : ", (2, PREC_SELECT)], PREC_SELECT,
                         [c, a, b], kind="select")

    def strip(self, value):
        while value is not None and value.kind == "select" and any(value.args[0] is c for c in self.active):
            value = value.args[1]
        return value

    def read_v(self, index):
        value = self.v.get(index)
        if value is None:
            value = self.leaf(f"v{index}")
            self.v[index] = value
        return self.strip(value)

    def read_s(self, index):
        value = self.s.get(index)
        if value is None:
            text = self.base.sname.get(index) or f"s{index}"
            value = self.leaf(text)
            self.s[index] = value
        return self.strip(value)

    def write(self, table, index, value):
        if self.cond is not None:
            value = self.select(self.cond, value, table.get(index))
        table[index] = value

    def operand(self, operand, floaty=True):
        kind = operand.kind
        if kind == "v":
            node = self.read_v(operand.value)
        elif kind == "s":
            node = self.read_s(operand.value)
        elif kind in ("int", "float"):
            node = self.number(float(operand.value)) if floaty else self.leaf(str(operand.value), value=float(operand.value))
        elif kind == "lit":
            value = gcn.f32(operand.value)
            if floaty or (operand.value > 0xFFFF and 1e-8 < abs(value) < 1e8):
                node = self.number(value)
            else:
                node = self.leaf(f"0x{operand.value:x}")
        elif kind == "special":
            if operand.value == 106:
                node = self.s.get("vcc") or self.leaf("vcc")
            elif operand.value == 126:
                node = self.leaf("exec")
            elif operand.value == 124:
                node = self.s.get("m0") or self.leaf("m0")
            else:
                node = self.leaf(operand.body())
        else:
            node = self.leaf(operand.body(floaty))
        if operand.abs:
            node = self.absf(node)
        if operand.neg:
            node = self.neg(node)
        return node

    def set_scalar_dst(self, operand, value):
        if operand.kind == "special" and operand.value == 106:
            self.s["vcc"] = value
        elif operand.kind == "special" and operand.value == 124:
            self.s["m0"] = value
        elif operand.kind == "s":
            self.write(self.s, operand.value, value)
            for i in range(1, operand.count):
                self.s.pop(operand.value + i, None)

    def statement(self, text_parts, args, kind):
        node = self.node(text_parts, PREC_ATOM, args, kind=kind, keep=True)
        self.statements.append(node)
        return node

    def run(self, insts):
        targets = {inst.extra["target"] for inst in insts if inst.encoding == "SOPP" and "target" in inst.extra}
        for inst in insts:
            if inst.encoding == "SOPP" and "target" in inst.extra and inst.extra["target"] <= inst.offset:
                raise LoopError(f"backward branch at 0x{inst.offset:x}")
        for inst in insts:
            while self.regions and self.regions[-1][0] <= inst.offset:
                end, parent_cond, parent_active = self.regions.pop()
                self.cond = parent_cond
                self.active = parent_active
            if inst.offset in targets:
                self.dead = False
            if self.dead:
                continue
            self.base.lift(inst)
            self.step(inst)
        return self.render()

    def push_region(self, end, cond):
        self.regions.append((end, self.cond, list(self.active)))
        self.cond = self.logic("&&", self.cond, cond)
        self.active = self.active + [self.cond]

    def step(self, inst):
        name = inst.name
        enc = inst.encoding
        if name in ("S_WAITCNT", "S_NOP", "S_SETPRIO", "V_NOP", "S_DCACHE_INV"):
            return
        if enc == "SOPP":
            self.step_sopp(inst)
        elif enc == "SMRD":
            self.step_smrd(inst)
        elif enc in ("SOP1", "SOP2", "SOPK", "SOPC"):
            self.step_scalar(inst)
        elif enc == "VINTRP":
            self.step_interp(inst)
        elif enc == "MIMG":
            self.step_image(inst)
        elif enc in ("MUBUF", "MTBUF"):
            self.step_buffer(inst)
        elif enc == "EXP":
            self.step_export(inst)
        elif enc in ("VOP1", "VOP2", "VOP3", "VOPC"):
            self.step_vector(inst)
        elif enc == "DS":
            for operand in inst.dst:
                self.write(self.v, operand.value, self.call(inst.name.lower(), *[self.operand(o, False) for o in inst.src]))

    def step_sopp(self, inst):
        name = inst.name
        if name == "S_ENDPGM":
            self.dead = True
            return
        if "target" not in inst.extra:
            return
        target = inst.extra["target"]
        if name in ("S_CBRANCH_EXECZ", "S_CBRANCH_EXECNZ"):
            return
        if name == "S_BRANCH":
            if self.regions and self.regions[-1][0] == inst.offset + 4:
                end, parent_cond, parent_active = self.regions.pop()
                region_cond = self.cond
                self.cond, self.active = parent_cond, parent_active
                taken = self.region_negation(region_cond, parent_cond)
                self.push_region(target, taken)
            else:
                self.dead = True
            return
        if name in ("S_CBRANCH_SCC0", "S_CBRANCH_SCC1"):
            if self.scc == "lanes" or self.scc is None:
                return
            taken = self.scc if name.endswith("1") else self.logic_not(self.scc)
        elif name in ("S_CBRANCH_VCCZ", "S_CBRANCH_VCCNZ"):
            vcc = self.s.get("vcc") or self.leaf("vcc")
            taken = vcc if name.endswith("NZ") else self.logic_not(vcc)
        else:
            return
        self.push_region(target, self.logic_not(taken))

    def region_negation(self, region_cond, parent_cond):
        if region_cond is not None and region_cond.kind == "bool" and parent_cond is not None and region_cond.args and region_cond.args[0] is parent_cond:
            return self.logic_not(region_cond.args[1])
        return self.logic_not(region_cond) if region_cond is not None else None

    def step_smrd(self, inst):
        if not inst.dst:
            return
        dst = inst.dst[0]
        for i in range(dst.count):
            text = self.base.sname.get(dst.value + i)
            if text:
                self.s[dst.value + i] = self.leaf(text)
            else:
                self.s.pop(dst.value + i, None)

    def step_scalar(self, inst):
        name = inst.name
        dst = inst.dst[0] if inst.dst else None
        if name == "S_SWAPPC_B64":
            for vgpr, text in self.env.get("fetch", []):
                self.v[vgpr] = self.leaf(text)
            return
        if dst is not None and dst.kind == "special" and dst.value == 126:
            self.step_exec(inst)
            return
        if name.endswith("SAVEEXEC_B64"):
            cond = self.operand(inst.src[0], False)
            self.exec_stack.append((dst.value if dst and dst.kind == "s" else None, self.cond, list(self.active), cond))
            self.cond = self.logic("&&", self.cond, cond)
            self.active = self.active + [self.cond]
            self.scc = "lanes"
            return
        if name == "S_MOV_B64" and inst.src and inst.src[0].kind == "special" and inst.src[0].value == 126:
            if self.live_mask is None and dst is not None:
                self.live_mask = dst.value
            return
        if name == "S_ANDN2_B64" and dst is not None and dst.kind == "s" and dst.value == self.live_mask and inst.src[0].kind == "s" and inst.src[0].value == self.live_mask:
            cond = self.operand(inst.src[1], False)
            self.statement(["if (", (0, 0), ") discard;"], [self.logic("&&", self.cond, cond) if self.cond else cond], "discard")
            self.scc = "lanes"
            return
        if inst.encoding == "SOPC":
            cond = name.split("_")[2]
            symbol = gcn.CMP_TEXT.get(cond, cond.lower())
            self.scc = self.compare(symbol, self.operand(inst.src[0], False), self.operand(inst.src[1], False))
            return
        if inst.encoding == "SOPK":
            if name == "S_MOVK_I32" and dst is not None:
                self.set_scalar_dst(dst, self.leaf(str(inst.src[0].value), value=float(inst.src[0].value)))
            return
        if dst is None:
            return
        if name in ("S_MOV_B32", "S_MOV_B64"):
            src = inst.src[0]
            if src.kind == "s" and dst.kind == "s":
                for i in range(dst.count):
                    value = self.s.get(src.value + i)
                    if value is not None:
                        self.s[dst.value + i] = value
                    else:
                        self.s.pop(dst.value + i, None)
                return
            if src.kind == "lit":
                value = gcn.f32(src.value)
                node = self.number(value) if 1e-30 < abs(value) < 1e30 or value == 0 else self.leaf(f"0x{src.value:x}")
            else:
                node = self.operand(src, True)
            self.set_scalar_dst(dst, node)
            return
        boolean = name.endswith("_B64") and name.startswith(("S_AND", "S_OR", "S_XOR", "S_ANDN2", "S_ORN2", "S_NOT", "S_NAND", "S_NOR"))
        if boolean:
            args = [self.operand(o, False) for o in inst.src]
            if name == "S_AND_B64":
                node = self.logic("&&", args[0], args[1])
            elif name == "S_OR_B64":
                node = self.logic("||", args[0], args[1])
            elif name == "S_ANDN2_B64":
                node = self.logic("&&", args[0], self.logic_not(args[1]))
            elif name == "S_ORN2_B64":
                node = self.logic("||", args[0], self.logic_not(args[1]))
            elif name == "S_NOT_B64":
                node = self.logic_not(args[0])
            else:
                node = self.call(name.lower(), *args)
            self.set_scalar_dst(dst, node)
            self.scc = "lanes"
            return
        args = [self.operand(o, False) for o in inst.src]
        template = gcn.SCALAR_BINARY.get(name) or gcn.SCALAR_UNARY.get(name)
        node = self.generic(name, template, args)
        self.set_scalar_dst(dst, node)
        self.scc = None

    def step_exec(self, inst):
        name = inst.name
        src = inst.src
        if name == "S_WQM_B64":
            return
        if name == "S_AND_B64" and any(o.kind == "s" and o.value == self.live_mask for o in src):
            return
        if name == "S_MOV_B64" and src and src[0].kind == "s" and src[0].value == self.live_mask:
            while self.exec_stack:
                saved, parent_cond, parent_active, cond = self.exec_stack.pop()
                self.cond, self.active = parent_cond, parent_active
            return
        if name == "S_ANDN2_B64" and src and src[0].kind == "s" and self.exec_stack and self.exec_stack[-1][0] == src[0].value:
            saved, parent_cond, parent_active, cond = self.exec_stack[-1]
            self.cond = self.logic("&&", parent_cond, self.logic_not(cond))
            self.active = parent_active + [self.cond]
            self.scc = "lanes"
            return
        if name in ("S_OR_B64", "S_MOV_B64"):
            saved_reg = None
            for o in src:
                if o.kind == "s":
                    saved_reg = o.value
            if self.exec_stack and self.exec_stack[-1][0] == saved_reg:
                saved, parent_cond, parent_active, cond = self.exec_stack.pop()
                self.cond, self.active = parent_cond, parent_active
                return
            if name == "S_MOV_B64" and src and src[0].kind == "special" and src[0].value == 106:
                return
        self.scc = "lanes"

    def generic(self, name, template, args):
        if template is None:
            return self.call(name.lower(), *args)
        return self.apply_template(template, args)

    def apply_template(self, template, args):
        names = {"a": 0, "b": 1, "c": 2, "d": 3}
        simple = {"{a} + {b}": ("+", 0, 1), "{a} - {b}": ("-", 0, 1), "{b} - {a}": ("-", 1, 0), "{a} * {b}": ("*", 0, 1)}
        if template in simple:
            op, i, j = simple[template]
            return self.binop(op, args[i], args[j])
        if template == "{a}":
            return args[0]
        if template == "{a} * {b} + {c}":
            return self.binop("+", self.binop("*", args[0], args[1]), args[2])
        match = re.fullmatch(r"(\w+)\((.*)\)", template)
        if match and all(p.strip() in ("{a}", "{b}", "{c}") for p in match.group(2).split(",")):
            order = [names[p.strip()[1]] for p in match.group(2).split(",")]
            return self.call(match.group(1), *[args[i] for i in order])
        if template == "1.0 / {a}":
            return self.binop("/", self.number(1.0), args[0])
        parts = []
        pieces = re.split(r"(\{[abcd]\})", template)
        used = []
        for piece in pieces:
            if re.fullmatch(r"\{[abcd]\}", piece):
                index = names[piece[1]]
                if index not in used:
                    used.append(index)
                parts.append((used.index(index), PREC_UNARY))
            elif piece:
                parts.append(piece)
        return self.node(parts, PREC_ADD, [args[i] for i in used])

    def step_vector(self, inst):
        name = inst.name
        floaty = gcn.is_float_op(name)
        if inst.encoding == "VOPC" or (inst.encoding == "VOP3" and inst.op < 256):
            parts = name.split("_")
            cond = parts[2]
            args = [self.operand(o, floaty) for o in inst.src]
            symbol = gcn.CMP_TEXT.get(cond, cond.lower())
            if symbol in ("true", "false"):
                node = self.leaf(symbol)
            elif cond == "CLASS":
                node = self.call("class", *args)
            else:
                node = self.compare(symbol, args[0], args[1])
            if "CMPX" in name:
                self.statement(["if (!(", (0, 0), ")) discard;"], [node], "discard")
                return
            self.set_scalar_dst(inst.dst[0], node)
            return
        args = [self.operand(o, floaty) for o in inst.src]
        if name == "V_CNDMASK_B32":
            mask = inst.extra.get("mask")
            cond = self.operand(mask, False) if mask is not None else args[2]
            node = self.select(cond, args[1], args[0])
        elif name in ("V_MAC_F32", "V_MAC_LEGACY_F32"):
            node = self.binop("+", self.binop("*", args[0], args[1]), self.read_v(inst.dst[0].value))
        elif name == "V_MADMK_F32":
            node = self.binop("+", self.binop("*", args[0], self.number(gcn.f32(inst.extra["k"]))), args[1])
        elif name == "V_MADAK_F32":
            node = self.binop("+", self.binop("*", args[0], args[1]), self.number(gcn.f32(inst.extra["k"])))
        elif name == "V_CVT_PKRTZ_F16_F32":
            node = self.call("packHalf2x16", args[0], args[1])
            node.kind = "pack"
        elif name in ("V_MED3_F32",) and len(args) == 3 and all(a.kind == "leaf" and re.fullmatch(r"(0x[f7]f7fffff|-?3\.40282347e\+38)", a.parts[0]) for a in args[1:]):
            node = args[0]
        elif name in gcn.UNARY:
            node = self.apply_template(gcn.UNARY[name], args)
        elif name in gcn.BINARY:
            node = self.apply_template(gcn.BINARY[name], args)
        elif name in gcn.TERNARY:
            node = self.apply_template(gcn.TERNARY[name], args)
        else:
            node = self.call(name.lower(), *args)
        omod = inst.extra.get("omod")
        if omod:
            node = self.binop("*", node, self.number([1.0, 2.0, 4.0, 0.5][omod]))
        if inst.extra.get("clamp"):
            node = self.call("saturate", node)
        if not inst.dst:
            return
        dst = inst.dst[0]
        if dst.kind == "v":
            self.write(self.v, dst.value, node)
            for i in range(1, dst.count):
                self.write(self.v, dst.value + i, self.call("hi", node))
        else:
            self.set_scalar_dst(dst, node)

    def step_interp(self, inst):
        if inst.name == "V_INTERP_P1_F32":
            return
        attr, chan = inst.extra["attr"], "xyzw"[inst.extra["chan"]]
        label = self.env.get("interp_name", lambda a: f"attr{a}")(attr)
        self.write(self.v, inst.dst[0].value, self.leaf(f"{label}.{chan}"))

    def step_image(self, inst):
        tex, samp, dims = self.image_names(inst)
        name = inst.name
        mods = name.split("_")[2:] if name.startswith(("IMAGE_SAMPLE", "IMAGE_GATHER4")) else []
        pos = inst.src[0].value
        pieces = []
        labels = []
        if "O" in mods:
            labels.append(("offset", 1))
        if "B" in mods:
            labels.append(("bias", 1))
        if "C" in mods:
            labels.append(("zref", 1))
        if "D" in mods or "CD" in mods:
            labels.append(("ddx", min(dims, 3)))
            labels.append(("ddy", min(dims, 3)))
        labels.append(("uv", dims + (1 if inst.extra.get("da") else 0)))
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
        args = []
        parts = [inst.name.lower().replace("image_", "") + "(" + tex + (", " + samp if not name.startswith(("IMAGE_LOAD", "IMAGE_GET")) else "")]
        for label, count in labels:
            parts.append(f", {label}=" + ("" if count == 1 else "vec" + str(count) + "("))
            for i in range(count):
                if i:
                    parts.append(", ")
                parts.append((len(args), 0))
                args.append(self.read_v(pos + i))
            if count > 1:
                parts.append(")")
            pos += count
        parts.append(")")
        self.samples += 1
        node = self.node(parts, PREC_ATOM, args, kind="sample", keep=True)
        node.name = f"t{self.samples}"
        self.statements.append(node)
        dmask = inst.extra["dmask"]
        channels = "xyzw" if name.startswith("IMAGE_GATHER4") else "".join(c for i, c in enumerate("xyzw") if dmask & (1 << i))
        if not inst.dst:
            return
        dst = inst.dst[0]
        for i, c in enumerate(channels[:dst.count]):
            comp = self.node([(0, PREC_ATOM), f".{c}"], PREC_ATOM, [node])
            comp.kind = "component"
            self.write(self.v, dst.value + i, comp)

    def image_names(self, inst):
        tsharp = self.base.descriptor(inst.src[1].value)
        ssharp = self.base.descriptor(inst.src[2].value)
        tex = self.env["resource_name"]("T#", tsharp[1]) if tsharp else inst.src[1].body()
        dims = self.env.get("texture_dims", lambda slot: 2)(tsharp[1]) if tsharp else 2
        samp = self.env["resource_name"]("S#", ssharp[1]) if ssharp else inst.src[2].body()
        return tex, samp, dims

    def step_buffer(self, inst):
        vsharp = self.base.descriptor(inst.src[-1].value)
        label = self.env["resource_name"](vsharp[0], vsharp[1]) if vsharp else inst.src[-1].body()
        address = self.read_v(inst.src[-2].value)
        if "STORE" in inst.name or not inst.dst:
            return
        self.samples += 1
        node = self.node([f"{inst.name.lower()}({label}, index=", (0, 0), f", offset={inst.extra['offset']})"], PREC_ATOM, [address], kind="sample", keep=True)
        node.name = f"b{self.samples}"
        self.statements.append(node)
        dst = inst.dst[0]
        for i in range(dst.count):
            comp = self.node([(0, PREC_ATOM), f".{'xyzw'[i]}"], PREC_ATOM, [node])
            comp.kind = "component"
            self.write(self.v, dst.value + i, comp)

    def step_export(self, inst):
        target = inst.extra["target"]
        label = self.env.get("export_name", lambda t: None)(target) or gcn.format_inst(inst).split()[1]
        comps = []
        for i, operand in enumerate(inst.src):
            if inst.extra["en"] & (1 << i):
                comps.append(self.read_v(operand.value))
            else:
                comps.append(None)
        if inst.extra["compr"]:
            expanded = []
            for node in comps[:2]:
                if node is not None and node.kind == "pack":
                    expanded.extend(node.args)
                elif node is not None:
                    expanded.extend([self.call("unpackHalf2x16_lo", node), self.call("unpackHalf2x16_hi", node)])
                else:
                    expanded.extend([None, None])
            comps = expanded
        parts = [f"{label} = vec{len(comps)}("]
        args = []
        for i, node in enumerate(comps):
            if i:
                parts.append(", ")
            if node is None:
                parts.append("_")
            else:
                parts.append((len(args), 0))
                args.append(node)
        parts.append(");")
        if self.cond is not None:
            parts = ["if (", (len(args), 0), ") "] + parts
            args.append(self.cond)
        self.statement(parts, args, "export")

    def render(self):
        roots = list(self.statements)
        seen = set()
        stack = list(roots)
        while stack:
            node = stack.pop()
            if id(node) in seen:
                continue
            seen.add(id(node))
            for arg in node.args:
                arg.uses += 1
                stack.append(arg)
        nodes = {}
        stack = list(roots)
        while stack:
            node = stack.pop()
            if id(node) in nodes:
                continue
            nodes[id(node)] = node
            stack.extend(node.args)
        counter = [0]
        cache = {}

        def text_of(node):
            if node.name is not None and node.kind != "statement_owner":
                return node.name
            key = id(node)
            if key in cache:
                return cache[key]
            out = []
            for part in node.parts:
                if isinstance(part, tuple):
                    index, need = part
                    child = node.args[index]
                    child_text = text_of(child)
                    child_prec = PREC_ATOM if child.name is not None else child.prec
                    if child_prec < need:
                        child_text = f"({child_text})"
                    out.append(child_text)
                else:
                    out.append(part)
            result = "".join(out)
            cache[key] = result
            return result

        ordered = sorted(nodes.values(), key=lambda n: n.order)
        for node in ordered:
            if node.kind in ("leaf", "component") or node.keep:
                continue
            if node.uses >= 2 and node.kind not in ("neg",):
                counter[0] += 1
                node.name = ("c" if node.kind in ("bool", "not") else "r") + str(counter[0])
            elif len(text_of(node)) > NAME_LENGTH_LIMIT:
                counter[0] += 1
                node.name = ("c" if node.kind in ("bool", "not") else "r") + str(counter[0])
            cache.clear()
        lines = []
        for node in ordered:
            if node.name is None:
                continue
            if node.kind == "sample":
                lines.append((node.order, f"vec4 {node.name} = {self.body(node, text_of)};"))
            elif node.keep:
                continue
            else:
                kind = "bool" if node.kind in ("bool", "not") else "float"
                lines.append((node.order, f"{kind} {node.name} = {self.body(node, text_of)};"))
        for node in self.statements:
            if node.kind in ("export", "discard"):
                lines.append((node.order, self.body(node, text_of)))
        lines.sort(key=lambda item: item[0])
        return [text for _, text in lines]

    @staticmethod
    def body(node, text_of):
        name = node.name
        node.name = None
        try:
            return text_of(node)
        finally:
            node.name = name


def lift(insts, env):
    builder = ExprBuilder(env)
    return builder.run(insts)
