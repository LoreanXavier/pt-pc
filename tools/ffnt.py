import argparse
import struct
import sys


def parse(data):
    if data[:4] != b"FXFT":
        raise ValueError("not an FFNT file")
    count = data[6]
    glyp = ftdt = None
    for i in range(count):
        sig, offset, size = struct.unpack_from("<4sII", data, 0x10 + i * 12)
        if sig == b"GLYP":
            glyp = offset
        elif sig == b"FTDT":
            ftdt = offset
    header = data[glyp:glyp + 16]
    font = {"em": header[2], "pad": header[12], "header": header.hex(" "), "glyphs": []}
    n = struct.unpack_from("<H", data, glyp + 6)[0]
    for i in range(n):
        code, x, y, w, h, layer, adv, bearing, top, flags = struct.unpack_from("<IHHBBBBBbH", data, glyp + 16 + i * 20)
        font["glyphs"].append({"code": code, "x": x, "y": y, "w": w, "h": h, "layer": layer, "advance": adv, "bearing": bearing,
                               "top": top, "flags": flags})
    font["width"] = 1 << data[ftdt + 1]
    font["height"] = 1 << data[ftdt + 2]
    font["bitmap"] = data[ftdt + 16:ftdt + 16 + font["width"] * font["height"]]
    return font


def render(font, text, scale_x, scale_y, space):
    from PIL import Image
    glyphs = {g["code"]: g for g in font["glyphs"]}
    em, pad = font["em"], font["pad"]
    pen = 10.0
    cells = []
    for ch in text:
        g = glyphs.get(ord(ch)) or glyphs.get(0x25A2) or glyphs.get(ord("?"))
        cells.append((pen, g))
        pen += scale_x * (g["advance"] + 2 * pad) / em + space
    line = scale_y * (em + 2 * pad) / em
    img = Image.new("L", (int(pen) + 10, int(line) + 20))
    for x, g in cells:
        if g["w"] <= 1:
            continue
        mask = 1 << (g["layer"] & 7)
        glyph = Image.new("L", (g["w"], g["h"]))
        pixels = [255 if font["bitmap"][(g["y"] + j) * font["width"] + g["x"] + i] & mask else 0 for j in range(g["h"]) for i in range(g["w"])]
        glyph.putdata(pixels)
        size = (max(1, round(g["w"] * scale_x / em)), max(1, round(g["h"] * scale_y / em)))
        glyph = glyph.resize(size, Image.LANCZOS)
        img.paste(255, (round(x + scale_x * (pad + g["bearing"]) / em), round(10 + scale_y * (pad + g["top"]) / em)), glyph)
    return img


def main():
    ap = argparse.ArgumentParser(description="Inspect Fox Engine FFNT fonts (P.T.): glyph table, optional text render.")
    ap.add_argument("font")
    ap.add_argument("--glyphs", action="store_true", help="print every glyph record")
    ap.add_argument("--text", help="render this text to --out")
    ap.add_argument("--out", default="ffnt_text.png")
    ap.add_argument("--size", type=float, nargs=2, default=(22.0, 22.0), metavar=("WIDTH", "HEIGHT"))
    ap.add_argument("--space", type=float, default=0.0, help="extra advance per character (textSpace)")
    args = ap.parse_args()
    with open(args.font, "rb") as f:
        font = parse(f.read())
    sys.stdout.reconfigure(encoding="utf-8")
    print("em %d pad %d bitmap %dx%d glyphs %d header %s" % (font["em"], font["pad"], font["width"], font["height"], len(font["glyphs"]),
                                                           font["header"]))
    if args.glyphs:
        for g in font["glyphs"]:
            print("U+%04X %r at (%d %d) %dx%d layer %d advance %d bearing %d top %d flags %d" % (
                g["code"], chr(g["code"]), g["x"], g["y"], g["w"], g["h"], g["layer"], g["advance"], g["bearing"], g["top"], g["flags"]))
    if args.text:
        render(font, args.text, args.size[0], args.size[1], args.space).save(args.out)
        print("wrote", args.out)


if __name__ == "__main__":
    main()
