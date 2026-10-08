"""One final remote update manifest for existing 1.0.1 binaries; only 1.0.2 may use it."""
import argparse
import json
from pathlib import Path


def write_manifest(dest: Path, version: str, notes: str = "", linux_setup: str = "") -> Path:
    if version != "1.0.2":
        raise ValueError("The legacy manifest is only for the 1.0.2 transition; later releases use GitHub API discovery")
    base = f"https://github.com/LoreanXavier/pt-pc/releases/download/v{version}"
    platforms = {"windows": {"url": f"{base}/P.T.PC.Port.Setup.exe"}}
    if linux_setup:
        platforms["linux"] = {"url": f"{base}/{linux_setup}"}
    path = dest / "latest.json"
    path.write_text(json.dumps({"version": version, "notes": notes,
                               "url": f"https://github.com/LoreanXavier/pt-pc/releases/tag/v{version}",
                               "platforms": platforms}, indent=2) + "\n", encoding="utf-8")
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--notes", default="")
    parser.add_argument("--linux-setup", default="")
    args = parser.parse_args()
    try:
        print(write_manifest(args.dest, args.version, args.notes, args.linux_setup))
    except ValueError as error:
        parser.error(str(error))
