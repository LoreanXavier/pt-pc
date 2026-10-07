"""Makes the Linux installer: the built pt_setup_linux with the payload appended (installer/Native/setup_linux.cpp reads it
back from its own end: payload, "PTPAYLD1", u64 offset, u64 size), then stamped with its own SHA-256
(tools/ci/stamp_integrity.py) like the Windows setup.

    python tools/linux/attach_payload.py --setup build/linux-cross/pt_setup_linux --payload <dir>/payload.bin --out <file>
"""
import argparse
import shutil
import stat
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ci"))
from stamp_integrity import stamp  # noqa: E402

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--setup", type=Path, required=True)
p.add_argument("--payload", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
if a.out.exists():
    sys.exit(f"{a.out} exists; choose a new file")
payload = a.payload.read_bytes()
if not payload.startswith(b"PTSETUP1"):
    sys.exit("not an installer payload (tools/prepare_native_installer.py)")
shutil.copy2(a.setup, a.out)
with a.out.open("ab") as f:
    offset = f.tell()
    f.write(payload)
    f.write(b"PTPAYLD1" + struct.pack("<QQ", offset, len(payload)))
digest = stamp(a.out)
a.out.chmod(a.out.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
print(f"{a.out}: {a.out.stat().st_size / 1024 ** 2:.1f} MiB, payload {len(payload) / 1024 ** 2:.1f} MiB, SHA-256 stamp {digest}")
