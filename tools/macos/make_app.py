"""The macOS app bundle (docs/macos.md) from a build folder, and a zip of it for the Releases page.

P.T. PC Port.app/Contents
  MacOS/pt                    the game
  Frameworks/libMoltenVK.dylib Vulkan on Metal (pt looks for it here, src/engine/render/vk_context.cpp)
  Resources/                  what pt finds through SDL_GetBasePath(), which is this folder in a bundle: shaders/, fonts/,
                              voice/, texture-tools/, licenses/ and the loop browser's previews

Everything is signed ad hoc (no Developer ID): a downloaded copy needs System Settings > Privacy & Security > Open Anyway
once, or xattr -dr com.apple.quarantine on it.
"""
import argparse
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
APP_NAME = "P.T. PC Port"
BUNDLE_ID = "io.github.loreanxavier.pt-pc"
MINIMUM_MACOS = "14.0"
# voice/ as the recognizer loads it on macOS (src/engine/voice/voice_recognizer.cpp): the shared libraries are .dylib, ggml's
# CPU variants are CMake modules (.so)
VOICE_MODELS = ("ggml-base.en-q5_1.bin", "ggml-silero-v6.2.0.bin")
VOICE_LIBRARIES = ("libwhisper.dylib", "libggml.dylib", "libggml-base.dylib")


def version(build):
    header = build / "generated" / "pt_version.h"
    match = re.search(r'#define PT_VERSION "([^"]+)"', header.read_text()) if header.is_file() else None
    return match.group(1) if match else "0.0.0"


def is_macho(path):
    with open(path, "rb") as f:
        magic = f.read(4)
    return magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def copy_voice(source, target):
    missing = [n for n in VOICE_MODELS + VOICE_LIBRARIES if not (source / n).is_file()]
    variants = sorted(source.glob("libggml-cpu-*.so"))
    if not variants:
        missing.append("libggml-cpu-*.so")
    if not (source / "licenses").is_dir():
        missing.append("licenses")
    if missing:
        sys.exit(f"{source} lacks {', '.join(missing)}; build the pt target first")
    target.mkdir(parents=True)
    for name in VOICE_MODELS + VOICE_LIBRARIES:
        shutil.copy2(source / name, target / name)
    for variant in variants:
        shutil.copy2(variant, target / variant.name)
    shutil.copytree(source / "licenses", target / "licenses")


def sign(app):
    # inside out: every Mach-O file on its own, then the bundle, which seals the rest
    for path in sorted(app.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if path.is_file() and not path.is_symlink() and is_macho(path) and path != app / "Contents" / "MacOS" / "pt":
            subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(path)], check=True, capture_output=True)
    subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", str(app)], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)


def main():
    parser = argparse.ArgumentParser(description="Make P.T. PC Port.app from a macOS build folder and zip it")
    parser.add_argument("--build", default=str(REPO / "build" / "macos"))
    parser.add_argument("--out", default=str(REPO / "dist"))
    parser.add_argument("--exe", help="Explicit executable: pt_release for the release 1.0 game (PT_RELEASE_LOCKS); shipped as pt")
    parser.add_argument("--game", default=str(REPO / "game" / "CUSA01127"),
                        help="the game files, for the loop browser previews (shot by the bundled game, --make-loop-previews)")
    parser.add_argument("--no-zip", action="store_true", help="leave the bundle unzipped")
    args = parser.parse_args()
    build = Path(args.build)
    exe = Path(args.exe) if args.exe else build / "pt"
    if not exe.is_file():
        sys.exit(f"{exe} not found; build first: cmake --build build/macos --target pt")
    moltenvk = build / "libMoltenVK.dylib"
    if not moltenvk.is_file():
        sys.exit(f"{moltenvk} not found; it is copied there when pt is built (cmake/MacOS.cmake)")
    ver = version(build)
    out = Path(args.out)
    app = out / f"{APP_NAME}.app"
    archive = out / f"pt-port-{ver}-macos-arm64.zip"
    if app.exists() or archive.exists():
        sys.exit(f"Package output already exists; choose another --out folder: {app}")
    contents = app / "Contents"
    resources = contents / "Resources"
    (contents / "MacOS").mkdir(parents=True)
    (contents / "Frameworks").mkdir()
    (resources / "shaders").mkdir(parents=True)
    shutil.copy2(exe, contents / "MacOS" / "pt")
    shutil.copy2(moltenvk, contents / "Frameworks" / "libMoltenVK.dylib")
    for spv in sorted((build / "shaders").glob("*.spv")):
        shutil.copy2(spv, resources / "shaders" / spv.name)
    copy_voice(build / "voice", resources / "voice")
    shutil.copytree(build / "fonts", resources / "fonts")
    if (build / "texture-tools").is_dir():
        shutil.copytree(build / "texture-tools", resources / "texture-tools")
    if (build / "licenses").is_dir():
        shutil.copytree(build / "licenses", resources / "licenses")
    shutil.copy2(REPO / "README.md", resources / "README.md")
    shutil.copy2(REPO / "docs" / "macos.md", resources / "macOS.md")
    with open(contents / "Info.plist", "wb") as f:
        plistlib.dump({
            "CFBundleDevelopmentRegion": "en",
            "CFBundleExecutable": "pt",
            "CFBundleIdentifier": BUNDLE_ID,
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": APP_NAME,
            "CFBundleDisplayName": APP_NAME,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": re.sub(r"-.*$", "", ver),
            "CFBundleVersion": ver,
            "LSMinimumSystemVersion": MINIMUM_MACOS,
            "LSArchitecturePriority": ["arm64"],
            "LSApplicationCategoryType": "public.app-category.games",
            "NSHighResolutionCapable": True,
            "NSSupportsAutomaticGraphicsSwitching": True,
            # the one part of the game that listens for a spoken word (README, What you need)
            "NSMicrophoneUsageDescription": "P.T. listens for one spoken word in one part of the game, as on the PS4.",
        }, f)
    (contents / "PkgInfo").write_text("APPL????")
    # the loop browser's previews are shot by the bundled game itself (tools/package.py does the same for Windows)
    if Path(args.game).is_dir():
        previews = resources / "loop-previews"
        subprocess.run([str(contents / "MacOS" / "pt"), "--make-loop-previews", str(previews), "--game", args.game], cwd=resources,
                       check=True, timeout=1800)
        shots = sorted(previews.glob("loop-*.png"))
        print(f"loop browser previews: {len(shots)}")
        if len(shots) != 18 or not (previews / "version.txt").is_file():
            sys.exit(f"loop browser previews incomplete: {len(shots)} of 18, see {previews / 'capture.log'}")
        for extra in ("capture.txt", "capture.log", "preview.ini"):
            (previews / extra).unlink(missing_ok=True)
    else:
        print(f"loop browser previews skipped: no game files at {args.game}")
    sign(app)
    if args.no_zip:
        print(app)
        return
    # ditto keeps the bundle's symlinks, permissions and signature as Finder's own Compress does
    subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(app), str(archive)], check=True)
    size = archive.stat().st_size / (1024 * 1024)
    print(f"{app}\n{archive} ({size:.1f} MB)")


if __name__ == "__main__":
    main()
