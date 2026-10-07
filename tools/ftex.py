import argparse
import csv
import mmap
import os
import shutil
import struct
import sys
import time
import zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import texture2ddecoder
from PIL import Image

from pathcode import (ASSETS_PREFIX, TYPE_SHIFT, assets_path, extension_type, path_code64, read_pathid_list,
                      with_extension)
from qar import QarArchive

REPO = Path(__file__).resolve().parent.parent
DEFAULT_GAME = REPO / "game" / "CUSA01127"
DEFAULT_CHUNK = REPO / "dump" / "chunk1"
DEFAULT_OUT = REPO / "dump" / "textures"
QAR_ONLY_LEVEL = "texture_qar_only"
SINGLE_LEVEL = "single"
MAX_FTEXS_FILES = 6

FTEX_HEADER = struct.Struct("<4sfHHHHBBHHHIIBB14s16s")
MIP_ENTRY = struct.Struct("<IIIBBH")
CHUNK = struct.Struct("<HHI")
CHUNK_RAW = 0x80000000

PFTXS_HEADER = struct.Struct("<4sfIII")
PFTXS_ENTRY = struct.Struct("<II")
PSUB_HEADER = struct.Struct("<4sI")
PSUB_ENTRY = struct.Struct("<II")

FLAG_CHUNKED = 0x1
FLAG_SRGB = 0x2
FLAG_CUBE = 0x4
FLAG_NORMAL = 0x8
FLAG_EXTERNAL = 0x1000000
FLAG_NAMES = ((FLAG_CHUNKED, "chunked"), (FLAG_SRGB, "srgb"), (FLAG_CUBE, "cube"), (FLAG_NORMAL, "normal"),
              (FLAG_EXTERNAL, "ftexs"))

FORMATS = {
    0: ("B8G8R8A8", 32, False),
    1: ("A8", 8, False),
    2: ("BC1", 4, True),
    3: ("BC2", 8, True),
    4: ("BC3", 8, True),
    5: ("BC5", 8, True),
    6: ("R32F", 32, False),
    7: ("D16", 16, False),
}


def expand_565(color):
    red, green, blue = (color >> 11) & 31, (color >> 5) & 63, color & 31
    return np.stack([(blue << 3) | (blue >> 2), (green << 2) | (green >> 4), (red << 3) | (red >> 2)], axis=-1)


def decode_bc2(data, width, height):
    bx, by = width // 4, height // 4
    blocks = np.frombuffer(data, dtype=np.uint8, count=bx * by * 16).reshape(by, bx, 16)
    alpha = np.empty((by, bx, 16), np.int32)
    alpha[..., 0::2] = (blocks[..., :8] & 0x0F).astype(np.int32) * 17
    alpha[..., 1::2] = (blocks[..., :8] >> 4).astype(np.int32) * 17
    words = blocks[..., 8:].copy().view("<u2").astype(np.int32)
    first, second = expand_565(words[..., 0]), expand_565(words[..., 1])
    palette = np.stack([first, second, (2 * first + second) // 3, (first + 2 * second) // 3], axis=2)
    indices = (words[..., 2] | (words[..., 3] << 16)).astype(np.int64)
    selectors = (indices[..., None] >> (2 * np.arange(16))) & 3
    texels = np.take_along_axis(palette, np.broadcast_to(selectors[..., None], (by, bx, 16, 3)), axis=2)
    bgra = np.concatenate([texels, alpha[..., None]], axis=-1).astype(np.uint8)
    return bgra.reshape(by, bx, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 4).tobytes()


BC_DECODERS = {2: texture2ddecoder.decode_bc1, 3: decode_bc2,
               4: texture2ddecoder.decode_bc3, 5: texture2ddecoder.decode_bc5}


class Ftex:
    def __init__(self, data: bytes):
        (magic, self.version, self.format, self.width, self.height, self.depth, self.mip_count,
         self.filter, self.addressing, self.field_14, self.field_16, self.field_18, self.flags,
         self.ftexs_count, self.first_ftexs_mip, _, self.content_hash) = FTEX_HEADER.unpack_from(data, 0)
        if magic != b"FTEX":
            raise ValueError("not an FTEX header")
        if self.format not in FORMATS:
            raise ValueError("unknown pixel format %d" % self.format)
        self.raw = bytes(data)
        self.faces = 6 if self.flags & FLAG_CUBE else 1
        self.entries = []
        for i in range(self.mip_count * self.faces):
            offset, size, stored, mip, file_no, chunks = MIP_ENTRY.unpack_from(data, FTEX_HEADER.size + i * MIP_ENTRY.size)
            self.entries.append({"offset": offset, "size": size, "stored": stored, "mip": mip,
                                 "file": file_no, "chunks": chunks, "face": i // self.mip_count})

    @property
    def format_name(self):
        return FORMATS[self.format][0]

    @property
    def flag_names(self):
        known = 0
        names = []
        for bit, name in FLAG_NAMES:
            known |= bit
            if self.flags & bit:
                names.append(name)
        if self.flags & ~known:
            names.append("0x%X" % (self.flags & ~known))
        return "|".join(names)

    def entry(self, level, face=0):
        return self.entries[face * self.mip_count + level]

    def dims(self, level):
        return max(1, self.width >> level), max(1, self.height >> level), max(1, self.depth >> level)

    def linear_size(self, level):
        w, h, d = self.dims(level)
        _, bits, block = FORMATS[self.format]
        if block:
            w, h = (w + 3) // 4 * 4, (h + 3) // 4 * 4
        return w * h * d * bits // 8

    def layout_is_linear(self):
        return all(e["size"] == self.linear_size(e["mip"]) for e in self.entries)


def read_mip(ftex: Ftex, entry, files):
    source = ftex.raw if entry["file"] == 0 else files[entry["file"]]
    base = entry["offset"]
    if not ftex.flags & FLAG_CHUNKED:
        data = bytes(source[base:base + entry["size"]])
    else:
        parts = []
        for i in range(entry["chunks"]):
            stored, unpacked, rel = CHUNK.unpack_from(source, base + i * CHUNK.size)
            start = base + (rel & ~CHUNK_RAW)
            block = bytes(source[start:start + stored])
            part = block if rel & CHUNK_RAW else zlib.decompress(block)
            if len(part) != unpacked:
                raise ValueError("mip %d chunk %d: %d bytes, header says %d" % (entry["mip"], i, len(part), unpacked))
            parts.append(part)
        data = b"".join(parts)
    if len(data) != entry["size"]:
        raise ValueError("mip %d face %d: got %d bytes, expected %d" % (entry["mip"], entry["face"], len(data), entry["size"]))
    return data


def decode_slice(fmt, data, w, h):
    if FORMATS[fmt][2]:
        bw, bh = (w + 3) // 4 * 4, (h + 3) // 4 * 4
        image = Image.frombytes("RGBA", (bw, bh), BC_DECODERS[fmt](data, bw, bh), "raw", "BGRA")
        return image.crop((0, 0, w, h)) if (bw, bh) != (w, h) else image
    if fmt == 0:
        return Image.frombytes("RGBA", (w, h), data, "raw", "BGRA")
    if fmt == 1:
        return Image.frombytes("L", (w, h), data)
    if fmt == 6:
        values = np.frombuffer(data, dtype="<f4").reshape(h, w)
        return Image.fromarray((np.clip(values, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), "L")
    if fmt == 7:
        return Image.frombytes("I;16", (w, h), data)
    raise ValueError("unsupported format %d" % fmt)


def decode_level(ftex: Ftex, files, level):
    w, h, d = ftex.dims(level)
    slice_bytes = ftex.linear_size(level) // d
    tiles = []
    for face in range(ftex.faces):
        data = read_mip(ftex, ftex.entry(level, face), files)
        for z in range(d):
            tiles.append(decode_slice(ftex.format, data[z * slice_bytes:(z + 1) * slice_bytes], w, h))
    if len(tiles) == 1:
        return tiles[0]
    strip = Image.new(tiles[0].mode, (w * len(tiles), h))
    for i, tile in enumerate(tiles):
        strip.paste(tile, (i * w, 0))
    return strip


def mip_consistency(top_image, next_image):
    top = np.asarray(top_image, dtype=np.float32)
    below = np.asarray(next_image, dtype=np.float32)
    if top.ndim == 2:
        top, below = top[..., None], below[..., None]
    h, w = below.shape[:2]
    if top.shape[0] < 2 * h or top.shape[1] < 2 * w:
        return None
    reduced = top[:2 * h, :2 * w].reshape(h, 2, w, 2, -1).mean(axis=(1, 3))
    scores = []
    for c in range(reduced.shape[-1]):
        a, b = reduced[..., c].ravel(), below[..., c].ravel()
        if a.std() >= 1.0 and b.std() >= 1.0:
            scores.append(float(np.corrcoef(a, b)[0, 1]))
    return min(scores) if scores else None


def top_available_level(ftex: Ftex, files):
    for level in range(ftex.mip_count):
        if all(ftex.entry(level, face)["file"] == 0 or ftex.entry(level, face)["file"] in files
               for face in range(ftex.faces)):
            return level
    return None


_containers = {}


def container(path):
    view = _containers.get(path)
    if view is None:
        handle = open(path, "rb")
        view = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        _containers[path] = view
    return view


def location_bytes(location):
    path, offset, size = location
    return container(path)[offset:offset + size]


def describe(location, extra=0):
    return "%s@0x%X" % (Path(location[0]).name, location[1] + extra)


def convert_job(job):
    row = {"level": job["level"], "path": job["path"], "code": job["code"], "png": "", "status": "",
           "note": job.get("note", "")}
    try:
        ftex = Ftex(location_bytes(job["header"]))
        files = {n: location_bytes(loc) for n, loc in job["files"].items()}
        w, h, d = ftex.dims(0)
        row.update({"format": ftex.format_name, "width": w, "height": h, "depth": d, "faces": ftex.faces,
                    "mips": ftex.mip_count, "flags": ftex.flag_names, "ftexs_files": ftex.ftexs_count,
                    "stored": "zlib-chunks" if ftex.flags & FLAG_CHUNKED else "raw",
                    "layout": "linear" if ftex.layout_is_linear() else "size-mismatch",
                    "header_source": describe(job["header"])})
        level = top_available_level(ftex, files)
        if level is None:
            row["status"] = "failed"
            row["note"] = (row["note"] + " no mip data available").strip()
            return row
        top = ftex.entry(level, 0)
        row["top_mip"] = level
        row["mip_source"] = describe(job["header"] if top["file"] == 0 else job["files"][top["file"]], top["offset"])
        if job["decode"]:
            out = Path(job["out"])
            out.parent.mkdir(parents=True, exist_ok=True)
            images = {}
            for lv in (range(level, ftex.mip_count) if job["all_mips"] else [level]):
                target = out if lv == level else out.with_name("%s.mip%d.png" % (out.stem, lv))
                images[lv] = decode_level(ftex, files, lv)
                images[lv].save(target, compress_level=job["compress"])
            row["png"] = str(out)
            if job["verify"] and ftex.depth == 1 and level + 1 < ftex.mip_count:
                below = images[level + 1] if level + 1 in images else decode_level(ftex, files, level + 1)
                score = mip_consistency(images[level], below)
                row["mip_check"] = "" if score is None else "%.3f" % score
        else:
            for face in range(ftex.faces):
                read_mip(ftex, ftex.entry(level, face), files)
        row["status"] = "ok" if level == 0 else "partial"
        if level:
            row["note"] = (row["note"] + " mips 0-%d not in any container" % (level - 1)).strip()
    except Exception as exc:
        row["status"] = "failed"
        row["note"] = (row["note"] + " %s: %s" % (type(exc).__name__, exc)).strip()
    return row


def read_pftxs(path: Path):
    data = path.read_bytes()
    magic, version, size, count, first = PFTXS_HEADER.unpack_from(data, 0)
    if magic != b"PFTX":
        raise ValueError("%s: not a PFTX file" % path)
    textures = []
    directory = ""
    pos = first
    for i in range(count):
        name_offset, header_size = PFTXS_ENTRY.unpack_from(data, PFTXS_HEADER.size + i * PFTXS_ENTRY.size)
        name = data[name_offset:data.index(b"\0", name_offset)].decode("utf-8")
        if name.startswith("@"):
            name = directory + name[1:]
        else:
            directory = name[:name.rfind("/") + 1]
        psub = pos + header_size
        tag, sub_count = PSUB_HEADER.unpack_from(data, psub)
        if tag != b"PSUB":
            raise ValueError("%s: texture %d has no PSUB block" % (path, i))
        subs = [PSUB_ENTRY.unpack_from(data, psub + PSUB_HEADER.size + k * PSUB_ENTRY.size) for k in range(sub_count)]
        textures.append({"path": name, "header": (pos, header_size), "files": {k + 1: s for k, s in enumerate(subs)}})
        end = max((o + s for o, s in subs), default=psub + PSUB_HEADER.size)
        pos = (end + 15) & ~15
    return {"version": version, "size": size, "textures": textures, "trailer": data[pos:pos + 4], "data": data}


def fox_output_path(out_dir: Path, level: str, fox_path: str):
    rel = fox_path[len(ASSETS_PREFIX):] if fox_path.startswith(ASSETS_PREFIX) else fox_path.lstrip("/")
    return out_dir / level / (rel + ".png")


def qar_sources(fox_path, qar, qar_path):
    code = path_code64(fox_path + ".ftex")
    if code not in qar:
        return code, None, {}
    offset, size = qar.entries[code]
    files = {}
    for n in range(1, MAX_FTEXS_FILES + 1):
        sub = with_extension(code, ".%d.ftexs" % n)
        if sub in qar:
            o, s = qar.entries[sub]
            files[n] = (str(qar_path), o, s)
    return code, (str(qar_path), offset, size), files


def pftxs_job(entry, pftxs_path, pftxs_data, qar, qar_path):
    code, header, files = qar_sources(entry["path"], qar, qar_path)
    notes = []
    if header is None:
        header = (str(pftxs_path), entry["header"][0], entry["header"][1])
        notes.append("not in texture.qar")
    else:
        o, s = entry["header"]
        if pftxs_data[o:o + s] != qar.read(code):
            notes.append("pftxs header differs from texture.qar")
    for n, (o, s) in entry["files"].items():
        if n in files:
            loc = files[n]
            if pftxs_data[o:o + s] != container(loc[0])[loc[1]:loc[1] + loc[2]]:
                notes.append("pftxs .%d.ftexs differs from texture.qar" % n)
        else:
            files[n] = (str(pftxs_path), o, s)
    return {"path": entry["path"], "code": "%016X" % code, "header": header, "files": files, "note": "; ".join(notes)}


def collect_pftxs_jobs(pftxs_files, qar, qar_path):
    jobs = []
    for pftxs_path in pftxs_files:
        info = read_pftxs(pftxs_path)
        if info["trailer"] != b"EOPF":
            print("warning: %s does not end with EOPF" % pftxs_path, file=sys.stderr)
        if info["size"] != len(info["data"]):
            print("warning: %s size field %d, file size %d" % (pftxs_path, info["size"], len(info["data"])), file=sys.stderr)
        for entry in info["textures"]:
            job = pftxs_job(entry, pftxs_path, info["data"], qar, qar_path)
            job["level"] = pftxs_path.stem
            jobs.append(job)
    return jobs


def qar_path_jobs(fox_paths, qar, qar_path, level):
    jobs = []
    for fox_path in fox_paths:
        # the texture's path with or without its .ftex extension (the archive spells it both ways)
        if fox_path.lower().endswith(".ftex"):
            fox_path = fox_path[:-5]
        code, header, files = qar_sources(fox_path, qar, qar_path)
        jobs.append({"path": fox_path, "code": "%016X" % code, "header": header, "files": files, "level": level,
                     "note": "" if header else "not in texture.qar"})
    return jobs


def qar_only_paths(pathids, qar, known_paths):
    ftex_type = extension_type("ftex")
    paths = []
    for code, app_path in pathids.items():
        if code >> TYPE_SHIFT != ftex_type or code not in qar:
            continue
        fox_path = assets_path(app_path)
        if fox_path not in known_paths:
            paths.append(fox_path)
    return sorted(paths)


def loose_job(ftex_file: Path, qar, qar_path):
    text = ftex_file.resolve().as_posix()
    if "/Assets/" not in text:
        raise SystemExit("cannot derive the Fox path of %s (no /Assets/ in its path)" % ftex_file)
    fox_path = "/Assets/" + text.split("/Assets/", 1)[1].rsplit(".", 1)[0]
    code, header, files = qar_sources(fox_path, qar, qar_path)
    note = ""
    if header is None:
        header = (str(ftex_file), 0, ftex_file.stat().st_size)
        note = "not in texture.qar"
        for n in range(1, MAX_FTEXS_FILES + 1):
            sibling = ftex_file.with_name("%s.%d.ftexs" % (ftex_file.stem, n))
            if sibling.exists():
                files[n] = (str(sibling), 0, sibling.stat().st_size)
    elif container(header[0])[header[1]:header[1] + header[2]] != ftex_file.read_bytes():
        note = "loose .ftex differs from texture.qar"
    return {"path": fox_path, "code": "%016X" % code, "header": header, "files": files, "level": "loose", "note": note}


def link_or_copy(source, target):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        target.unlink()
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def run(jobs, workers):
    unique, duplicates = {}, []
    for job in jobs:
        key = (job["path"], job["header"])
        if key in unique:
            duplicates.append((job, unique[key]))
        else:
            unique[key] = job
    started = time.time()
    results = {}
    with ProcessPoolExecutor(max_workers=workers) as pool:
        for job, row in zip(unique.values(), pool.map(convert_job, unique.values(), chunksize=4)):
            results[id(job)] = row
    rows = [results[id(job)] for job in jobs if id(job) in results]
    for job, first in duplicates:
        source = results[id(first)]
        row = dict(source, level=job["level"], note=job.get("note", ""))
        if source["png"]:
            link_or_copy(source["png"], job["out"])
            row["png"] = job["out"]
            source_png = Path(source["png"])
            for extra in sorted(source_png.parent.glob(source_png.stem + ".mip*.png")):
                link_or_copy(extra, Path(job["out"]).parent / extra.name)
        rows.append(row)
    return rows, time.time() - started


COLUMNS = ["level", "path", "code", "format", "width", "height", "depth", "faces", "mips", "flags", "ftexs_files",
           "stored", "layout", "top_mip", "header_source", "mip_source", "mip_check", "status", "note", "png"]


def print_table(level, rows):
    print("\n== %s: %d textures" % (level, len(rows)))
    print("%-8s %-12s %-4s %-5s %-6s %-11s %-34s %-8s %s" % (
        "format", "size", "mips", "faces", "tiling", "stored", "top mip source", "status", "path"))
    for r in sorted(rows, key=lambda r: r["path"]):
        size = "%dx%d" % (r.get("width", 0), r.get("height", 0)) + ("x%d" % r["depth"] if r.get("depth", 1) > 1 else "")
        print("%-8s %-12s %-4s %-5s %-6s %-11s %-34s %-8s %s%s" % (
            r.get("format", "?"), size, r.get("mips", "?"), r.get("faces", "?"), r.get("layout", "?"),
            r.get("stored", "?"), r.get("mip_source", "-"), r["status"], r["path"],
            ("  (" + r["note"] + ")") if r.get("note") else ""))


def write_index(out_dir: Path, level, rows):
    target = out_dir / level / "index.csv"
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, extrasaction="ignore")
        writer.writeheader()
        for r in sorted(rows, key=lambda r: r["path"]):
            png = Path(r["png"]) if r.get("png") else None
            rel = png.relative_to(out_dir).as_posix() if png and png.is_relative_to(out_dir) else r.get("png", "")
            writer.writerow(dict(r, png=rel))


def print_summary(rows, seconds):
    levels = {}
    for r in rows:
        levels.setdefault(r["level"], []).append(r)
    print("\n%-20s %8s %8s %8s %8s" % ("level", "textures", "ok", "partial", "failed"))
    totals = [0, 0, 0, 0]
    for level in sorted(levels):
        level_rows = levels[level]
        counts = [len(level_rows)] + [sum(1 for r in level_rows if r["status"] == s) for s in ("ok", "partial", "failed")]
        totals = [a + b for a, b in zip(totals, counts)]
        print("%-20s %8d %8d %8d %8d" % (level, *counts))
    print("%-20s %8d %8d %8d %8d" % ("total", *totals))
    formats, layouts = {}, {}
    for r in rows:
        formats[r.get("format", "?")] = formats.get(r.get("format", "?"), 0) + 1
        layouts[r.get("layout", "?")] = layouts.get(r.get("layout", "?"), 0) + 1
    print("formats: %s; tiling: %s; %.1f s" % (", ".join("%s %d" % kv for kv in sorted(formats.items())),
                                               ", ".join("%s %d" % kv for kv in sorted(layouts.items())), seconds))
    checks = sorted((float(r["mip_check"]), r["path"]) for r in {r["path"]: r for r in rows}.values() if r.get("mip_check"))
    if checks:
        median = checks[len(checks) // 2][0]
        print("mip check: %d textures, median %.3f, below 0.5: %d" % (len(checks), median, sum(1 for c in checks if c[0] < 0.5)))
        for score, path in checks[:5]:
            print("  lowest %.3f %s" % (score, path))
    for r in rows:
        if r["status"] == "failed":
            print("FAILED %s %s: %s" % (r["level"], r["path"], r["note"]))
    return levels


def main():
    ap = argparse.ArgumentParser(description="Convert P.T. textures (.pftxs packs, texture.qar, loose .ftex) to PNG.")
    ap.add_argument("inputs", nargs="*", type=Path, help=".pftxs files, or directories searched for .pftxs files")
    ap.add_argument("--all", action="store_true", help="every .pftxs under --chunk plus the textures found only in texture.qar")
    ap.add_argument("--qar-only", action="store_true", help="also convert texture.qar textures that no given pack references")
    ap.add_argument("--path", action="append", default=[], help="Fox texture path (/Assets/...) to convert from texture.qar")
    ap.add_argument("--ftex", type=Path, action="append", default=[], help="loose .ftex file (its path must contain /Assets/)")
    ap.add_argument("--game", type=Path, default=DEFAULT_GAME, help="folder with texture.qar and pathid_list_ps4.bin")
    ap.add_argument("--chunk", type=Path, default=DEFAULT_CHUNK, help="extracted chunk1.psarc, searched by --all")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--all-mips", action="store_true", help="also write every smaller mip as <name>.mipN.png")
    ap.add_argument("--dry-run", action="store_true", help="read and unpack every top mip but write no PNG")
    ap.add_argument("--verify", action="store_true",
                    help="correlate the 2x2-reduced top mip with the next stored mip (a detiling or layout error shows as a low score)")
    ap.add_argument("--compress", type=int, default=6, help="PNG zlib level 0-9")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--quiet", action="store_true", help="print only the per-level summary")
    args = ap.parse_args()

    qar_path = args.game / "texture.qar"
    qar = QarArchive(qar_path)

    pftxs_files = []
    for item in args.inputs + ([args.chunk] if args.all else []):
        pftxs_files += sorted(item.rglob("*.pftxs")) if item.is_dir() else [item]
    jobs = collect_pftxs_jobs(pftxs_files, qar, qar_path)
    if args.all or args.qar_only:
        pathids = read_pathid_list(args.game / "pathid_list_ps4.bin")
        jobs += qar_path_jobs(qar_only_paths(pathids, qar, {j["path"] for j in jobs}), qar, qar_path, QAR_ONLY_LEVEL)
    jobs += qar_path_jobs(args.path, qar, qar_path, SINGLE_LEVEL)
    jobs += [loose_job(f, qar, qar_path) for f in args.ftex]
    if not jobs:
        ap.error("nothing to convert")
    missing = [j for j in jobs if j["header"] is None]
    for j in missing:
        print("FAILED %s %s: %s" % (j["level"], j["path"], j["note"]))
    jobs = [j for j in jobs if j["header"] is not None]
    for job in jobs:
        job.update({"out": str(fox_output_path(args.out, job["level"], job["path"])), "all_mips": args.all_mips,
                    "compress": args.compress, "decode": not args.dry_run, "verify": args.verify})

    rows, seconds = run(jobs, args.jobs)
    levels = {}
    for r in rows:
        levels.setdefault(r["level"], []).append(r)
    for level in sorted(levels):
        if not args.dry_run:
            write_index(args.out, level, levels[level])
        if not args.quiet:
            print_table(level, levels[level])
    print_summary(rows, seconds)
    qar.close()


if __name__ == "__main__":
    main()
