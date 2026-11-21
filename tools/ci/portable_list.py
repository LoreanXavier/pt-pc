"""The portable zips' own file list, and their match with the setups' payloads (docs/updates.md, "Portable copies").

A portable copy updates itself in place from its setup's signed manifest: the same package, the same file list. For the updated
copy to equal a new portable zip of that version, the zip must hold exactly the files of the setup's payload, pt-files.json
included. tools/package.py writes pt-files.json before tools/ci/release.py adds the extraction helper (add_extractor); this
writes it again over every file of the zip, as tools/prepare_native_installer.py writes it for the payload, so both are the same
bytes. `check` compares a zip with a payload listing (payload-manifest.json): release.py refuses a release where they differ.

    python tools/ci/portable_list.py check <portable zip> <payload-manifest.json>
"""
from __future__ import annotations

import hashlib
import json
import pathlib
import sys
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from package import FILE_LIST, file_list  # noqa: E402


def _root(names: list[str]) -> str:
    return pathlib.PurePosixPath(names[0]).parts[0]


def rewrite_file_list(portable: pathlib.Path) -> None:
    """pt-files.json of the zip written again over all its files (the helper added after tools/package.py included)"""
    with zipfile.ZipFile(portable) as z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        root = _root([i.filename for i in infos])
        files = {i.filename[len(root) + 1:]: z.read(i) for i in infos}
        listing = file_list(files)
        if files.get(FILE_LIST) == listing:
            return
        temporary = portable.with_name(portable.name + ".new")
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as out:
            for info in z.infolist():
                if info.filename == f"{root}/{FILE_LIST}":
                    continue
                out.writestr(info, b"" if info.is_dir() else z.read(info))
            entry = zipfile.ZipInfo(f"{root}/{FILE_LIST}", date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            out.writestr(entry, listing)
    temporary.replace(portable)


def check(portable: pathlib.Path, payload_manifest: pathlib.Path) -> list[str]:
    """differences between the zip's files and the payload's (path, size, SHA-256); empty when they are the same"""
    payload = json.loads(payload_manifest.read_text(encoding="utf-8"))
    with zipfile.ZipFile(portable) as z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        root = _root([i.filename for i in infos])
        zipped = {}
        for info in infos:
            data = z.read(info)
            zipped[info.filename[len(root) + 1:]] = (len(data), hashlib.sha256(data).hexdigest())
    problems = []
    with zipfile.ZipFile(portable) as z:
        for info in z.infolist():
            rel = info.filename.split("/", 1)[-1]
            if rel in ("pt", "pt-updater") and not (info.external_attr >> 16) & 0o111:
                problems.append(f"{rel} has no executable bit in the zip")
    only_zip = sorted(set(zipped) - set(payload))
    only_payload = sorted(set(payload) - set(zipped))
    differ = sorted(p for p in set(zipped) & set(payload) if zipped[p] != (payload[p]["bytes"], payload[p]["sha256"]))
    if only_zip:
        problems.append(f"{len(only_zip)} files only in the zip: {only_zip[:5]}")
    if only_payload:
        problems.append(f"{len(only_payload)} files only in the payload: {only_payload[:5]}")
    if differ:
        problems.append(f"{len(differ)} files differ: {differ[:5]}")
    return problems


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "check":
        found = check(pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]))
        print("\n".join(found) if found else "PASS the portable zip holds exactly the payload's files")
        sys.exit(1 if found else 0)
    if len(sys.argv) == 3 and sys.argv[1] == "rewrite":
        rewrite_file_list(pathlib.Path(sys.argv[2]))
        sys.exit(0)
    sys.exit(__doc__)
