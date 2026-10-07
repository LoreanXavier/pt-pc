import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from foxhash import strcode64

TYPES = {0: "root", 1: "null", 2: "mesh", 3: "text"}
SLOTS = {"Base_Texture": "base", "Layer_Texture": "layer", "Mask_Texture": "mask", "Screen_Texture": "screen"}
PARAMS = {}
for group in ("BaseTex", "LayerTex", "MaskTex", "ScreenTex"):
    for field in ("UCenter", "VCenter", "UShift", "VShift", "URepeat", "VRepeat", "Blend"):
        PARAMS["%s_%s" % (field, group)] = None


def f32(b, o, n=1):
    v = struct.unpack_from("<%df" % n, b, o)
    return [round(x, 5) for x in v] if n > 1 else round(v[0], 5)


def parse(b, dictionary):
    if b[:4] != b"UIF ":
        raise ValueError("not a UIF file")
    node_count, name_count, texture_count = struct.unpack_from("<HHH", b, 0x0A)
    node_table, name_table, texture_table, blob = struct.unpack_from("<IIII", b, 0x10)
    names = [struct.unpack_from("<Q", b, blob + name_table + i * 8)[0] & 0xFFFFFFFFFFFF for i in range(name_count)]
    lookup = {strcode64(s): s for s in dictionary}
    for s in list(SLOTS) + list(PARAMS):
        lookup[strcode64(s)] = s

    def name(i):
        if i >= len(names):
            return None
        return lookup.get(names[i], "0x%012x" % names[i])

    textures = []
    for i in range(texture_count):
        length, offset = struct.unpack_from("<II", b, texture_table + i * 8)
        textures.append(b[blob + offset:blob + offset + length].split(b"\0")[0].decode())
    nodes = []
    for i in range(node_count):
        nid, ntype, off = struct.unpack_from("<HHI", b, node_table + i * 8)
        node = {"id": nid, "name": name(nid), "type": TYPES.get(ntype, ntype), "parent": struct.unpack_from("<H", b, off)[0]}
        if ntype != 0:
            flags, text_flags = struct.unpack_from("<HH", b, off + 4)
            node.update({"flags": "0x%04x" % flags, "flags2": "0x%04x" % text_flags, "size": f32(b, off + 8, 2), "scale": f32(b, off + 0x10, 2),
                         "rotation": f32(b, off + 0x18, 4), "translate": f32(b, off + 0x28, 4), "priority": f32(b, off + 0x38),
                         "color": f32(b, off + 0x3C, 4)})
        if ntype == 2:
            vc, pc, pos, uv = struct.unpack_from("<HHII", b, off + 0x50)
            tri = struct.unpack_from("<I", b, off + 0x64)[0]
            material, _, binding_count, param_count, bindings, params = struct.unpack_from("<HHHHII", b, off + 0x74)
            node["positions"] = [f32(b, blob + pos + k * 16, 2) for k in range(vc)]
            node["uvs"] = [f32(b, blob + uv + k * 16, 2) for k in range(vc)]
            node["triangles"] = list(struct.unpack_from("<%dH" % (pc * 3), b, blob + tri))
            node["material"] = name(material)
            node["textures"] = {}
            for k in range(binding_count):
                slot, texture = struct.unpack_from("<HH", b, bindings + k * 4)
                node["textures"][SLOTS.get(name(slot), name(slot))] = textures[texture] if texture < len(textures) else texture
            node["params"] = {}
            for k in range(param_count):
                _, pname = struct.unpack_from("<HH", b, params + k * 8)
                node["params"][name(pname)] = f32(b, params + k * 8 + 4)
        elif ntype == 3:
            text_name, font_name = struct.unpack_from("<HH", b, off + 0x80)
            node.update({"box_min": f32(b, off + 0x50, 2), "box_max": f32(b, off + 0x60, 2), "text": name(text_name), "font": name(font_name)})
        nodes.append(node)
    return {"textures": textures, "names": [name(i) for i in range(name_count)], "nodes": nodes}


def main():
    ap = argparse.ArgumentParser(description="Dump Fox Engine UI model files (.uif, P.T.) as JSON.")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--dict", help="text file with candidate names, one per line, to resolve StrCode64 node, font and material names")
    args = ap.parse_args()
    dictionary = []
    if args.dict:
        with open(args.dict, encoding="utf-8") as f:
            dictionary = [line.strip() for line in f if line.strip()]
    out = {}
    for path in args.files:
        with open(path, "rb") as f:
            out[path] = parse(f.read(), dictionary)
    sys.stdout.reconfigure(encoding="utf-8")
    json.dump(out, sys.stdout, ensure_ascii=False, indent=1)
    print()


if __name__ == "__main__":
    main()
