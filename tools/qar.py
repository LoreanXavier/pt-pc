import argparse
import mmap
import struct
from pathlib import Path

from pathcode import TYPE_SHIFT, extension_type, read_pathid_list

FOOTER = struct.Struct("<QQIHHIII")
MAGIC = 0x7161
FLAG_NAMES = 1
FLAG_FIELD_00_IS_DATA_END = 2
ENTRY = struct.Struct("<QII")
TEXTURE_EXTENSIONS = ("ftex", "1.ftexs", "2.ftexs", "3.ftexs", "4.ftexs", "5.ftexs", "6.ftexs")


class QarArchive:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.file = open(self.path, "rb")
        self.map = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        size = len(self.map)
        (self.field_00, self.field_08, count, self.flags, magic, table_units,
         self.field_1c, self.field_20) = FOOTER.unpack_from(self.map, size - FOOTER.size)
        if magic != MAGIC:
            raise ValueError("%s: not a QAR archive (footer magic %04X)" % (self.path, magic))
        self.table_offset = table_units << 4
        self.entries = {}
        self.order = []
        for i in range(count):
            code, offset_units, length = ENTRY.unpack_from(self.map, self.table_offset + i * ENTRY.size)
            self.entries[code] = (offset_units << 4, length)
            self.order.append(code)
        self.names = {}
        if self.flags & FLAG_NAMES:
            pos = self.table_offset + count * ENTRY.size
            for code in self.order:
                end = self.map.index(b"\0", pos)
                self.names[code] = self.map[pos:end].decode("utf-8", "replace")
                pos = end + 1

    def __contains__(self, code):
        return code in self.entries

    def read(self, code: int) -> bytes:
        offset, length = self.entries[code]
        return self.map[offset:offset + length]

    def close(self):
        self.map.close()
        self.file.close()


def main():
    ap = argparse.ArgumentParser(description="List or extract a Fox QAR archive (texture.qar), naming entries from pathid_list_ps4.bin.")
    ap.add_argument("archive", type=Path)
    ap.add_argument("--pathids", type=Path, help="pathid_list_ps4.bin (default: next to the archive)")
    ap.add_argument("--extract", type=Path, metavar="DIR")
    args = ap.parse_args()

    qar = QarArchive(args.archive)
    pathid_file = args.pathids or args.archive.with_name("pathid_list_ps4.bin")
    names = read_pathid_list(pathid_file) if pathid_file.exists() else {}
    extensions = {extension_type(e): e for e in TEXTURE_EXTENSIONS}
    print("%d entries, table at 0x%X, flags %d, field_00 0x%X, field_20 0x%X" % (
        len(qar.entries), qar.table_offset, qar.flags, qar.field_00, qar.field_20))
    for code in qar.order:
        offset, length = qar.entries[code]
        base = names.get(code) or qar.names.get(code) or "%016X" % code
        extension = extensions.get(code >> TYPE_SHIFT, "type%d" % (code >> TYPE_SHIFT))
        name = "%s.%s" % (base, extension)
        print("%016X  %10X  %9d  %s" % (code, offset, length, name))
        if args.extract:
            target = args.extract / name.split("/app0/", 1)[-1].lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(qar.read(code))
    qar.close()


if __name__ == "__main__":
    main()
