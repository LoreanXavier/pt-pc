import argparse
import datetime
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# voice/ as the recognizer loads it (formats/voice.md): whisper.cpp and ggml with its CPU variants, the two models and
# their notices; the build folder also holds the libraries' debug symbols, which stay out
VOICE_MODELS = ("ggml-base.en-q5_1.bin", "ggml-silero-v6.2.0.bin")
VOICE_LIBRARIES = ("whisper", "ggml", "ggml-base", "ggml-cpu-x64")


def copy_voice(source, target):
    # Windows DLLs, or the shared objects of a Linux build (docs/linux.md)
    prefix, extension = ("", ".dll") if (source / "whisper.dll").is_file() else ("lib", ".so")
    libraries = tuple(f"{prefix}{n}{extension}" for n in VOICE_LIBRARIES)
    missing = [n for n in VOICE_MODELS + libraries if not (source / n).is_file()] + ([] if (source / "licenses").is_dir() else ["licenses"])
    if missing:
        sys.exit(f"{source} lacks {', '.join(missing)}; build first with tools\\build_pt.bat release")
    target.mkdir(parents=True)
    for name in VOICE_MODELS:
        shutil.copy2(source / name, target / name)
    for library in sorted(source.glob(f"{prefix}*{extension}")):
        shutil.copy2(library, target / library.name)
    shutil.copytree(source / "licenses", target / "licenses")


def version():
    try:
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        rev = "local"
    return f"{datetime.date.today():%Y%m%d}-{rev}"


def main():
    parser = argparse.ArgumentParser(description="Copy pt.exe with its shaders, voice model and upscaler DLLs into a portable folder and zip it")
    parser.add_argument("--build", default=str(REPO / "build" / "release"))
    parser.add_argument("--out", default=str(REPO / "dist"))
    parser.add_argument("--exe", help="Explicit executable: pt_release.exe for the release 1.0 game (loop browser locked until the game "
                        "is finished, PT_RELEASE_LOCKS); shipped as pt.exe")
    parser.add_argument("--game", default=str(REPO / "game" / "CUSA01127"),
                        help="the game files, for the loop browser previews (shot by the packaged exe, --make-loop-previews)")
    args = parser.parse_args()
    build = Path(args.build)
    exe = Path(args.exe) if args.exe else build / "pt.exe"
    if not exe.exists():
        sys.exit(f"{exe} not found; build first with tools\\build_pt.bat release")
    # a Linux build (build/linux-cross, docs/linux.md) has no .exe and no DLLs
    exe_name = "pt.exe" if exe.suffix.lower() == ".exe" else "pt"
    name = f"pt-port-{version()}"
    root = Path(args.out) / name
    archive = Path(args.out) / f"{name}.zip"
    if root.exists() or archive.exists():
        sys.exit(f"Package output already exists; choose another --out folder: {root}")
    (root / "shaders").mkdir(parents=True)
    shutil.copy2(exe, root / exe_name)
    for spv in sorted((build / "shaders").glob("*.spv")):
        shutil.copy2(spv, root / "shaders" / spv.name)
    copy_voice(build / "voice", root / "voice")
    shutil.copytree(build / "fonts", root / "fonts")
    # the loop browser's previews are shot by the packaged game itself from each entry's real start (pt.exe --make-loop-previews,
    # docs/gameplay.md loop browser); without the game files the game shoots them on the player's PC at the browser's first use
    if Path(args.game).is_dir():
        previews = root / "loop-previews"
        subprocess.run([str(root / "pt.exe"), "--make-loop-previews", str(previews), "--game", args.game], cwd=root, check=True,
                       timeout=1800)
        shots = sorted(previews.glob("loop-*.png"))
        print(f"loop browser previews: {len(shots)}")
        # the game shows them only with all 18 and the capture's version.txt; a capture that dropped one fails the package
        if len(shots) != 18 or not (previews / "version.txt").is_file():
            sys.exit(f"loop browser previews incomplete: {len(shots)} of 18, see {previews / 'capture.log'}")
        for extra in ("capture.txt", "capture.log", "preview.ini"):
            (previews / extra).unlink(missing_ok=True)
    else:
        print(f"loop browser previews skipped: no game files at {args.game}")
    if (build / "texture-tools").is_dir():
        shutil.copytree(build / "texture-tools", root / "texture-tools")
    for dll in sorted(build.glob("*.dll")):
        shutil.copy2(dll, root / dll.name)
    if (build / "licenses").is_dir():
        shutil.copytree(build / "licenses", root / "licenses")
    shutil.copy2(REPO / "README.md", root / "README.md")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                z.write(path, Path(name) / path.relative_to(root))
    size = archive.stat().st_size / (1024 * 1024)
    print(f"{root}\n{archive} ({size:.1f} MB)")


if __name__ == "__main__":
    main()
