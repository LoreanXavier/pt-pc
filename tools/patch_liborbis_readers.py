"""Apply the extraction read fixes to the LGPL LibOrbisPkg corresponding source."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("source", type=Path)
a = p.parse_args()
fixes = [
    ("LibOrbisPkg/PFS/PFSCReader.cs", "ds.Read(output, 0, hdr.BlockSz);",
     "ds.ReadExactly(output, 0, hdr.BlockSz);"),
    ("LibOrbisPkg/Util/MemoryMapped.cs",
     "reader.Read((long)chunks[chunkIdx++]*chunkSize + offsetIntoChunk, buf, offset, count);",
     "reader.Read((long)chunks[chunkIdx++]*chunkSize + offsetIntoChunk, buf, offset, toReadFromChunk);"),
]
for name, before, after in fixes:
    path = a.source / name
    text = path.read_text(encoding="utf-8-sig")
    if after in text:
        print("Already patched:", name)
        continue
    if text.count(before) != 1:
        raise RuntimeError(f"Unexpected source version: {name}")
    path.write_text(text.replace(before, after), encoding="utf-8")
    print("Patched:", name)
