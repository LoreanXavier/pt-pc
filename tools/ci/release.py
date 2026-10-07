"""

    python tools/ci/release.py --dest <releases folder> --out <dir> --version 1.0.0

It builds the developer build pt, the release game pt_release (PT_RELEASE_LOCKS, the loop browser locked until the game is
finished once), its portable zip, the release setup and latest.json, the update manifest the game and the setup read
(docs/updates.md), to attach to the GitHub release with the setup.

--linux-setup <name> also makes the Linux setup (docs/installer.md, Linux): the cross build of the same game and
pt_setup_linux (tools/linux/cross_build.sh, PT_LINUX_SYSROOT or --linux-sysroot), its portable zip with the loop browser
previews the Windows package shot, the helper published for linux-x64, the payload and the stamped setup as --dest/<folder>/<name>;
latest.json then lists it. --linux-only skips the Windows steps and makes the Linux setup from a folder the Windows run left.

The large files stay on the PC under --dest/<stamp> (or --dest/--name); --out gets a short report (release.json, release.log).
--version is this release's version (PT_VERSION, shown by the game and the setup and compared by the update check).
PT_SLOT names a wrapper script the builds go through.

The installer needs the LGPL LibOrbisPkg source: --liborbis, else the first LibOrbisPkg-* folder under this repository's
dump/ (one or two levels down). It is copied into build/, patched there
(tools/patch_liborbis_readers.py, net10.0) and the extraction helper is published from it (dotnet publish, win-x64).
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD = REPO / "build" / "release"
# the public releases (docs/updates.md): the manifest's release page and the assets' addresses
GITHUB_REPO = "https://github.com/LoreanXavier/pt-pc"
SETUP_NAME = "P.T.PC.Port.Setup.exe"

p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument("--dest", type=pathlib.Path, required=True)
p.add_argument("--out", type=pathlib.Path, required=True)
p.add_argument("--name", help="the folder under --dest (default: a date and time stamp)")
p.add_argument("--liborbis", type=pathlib.Path)
p.add_argument("--version", required=True, help="this release's version, e.g. 1.0.0 (PT_VERSION)")
p.add_argument("--notes", default="", help="one short line for latest.json (the update notice logs it)")
p.add_argument("--linux-setup", help="also make the Linux setup, as this file name under the release folder (latest.json lists it)")
p.add_argument("--linux-sysroot", type=pathlib.Path, default=pathlib.Path(os.environ.get("PT_LINUX_SYSROOT") or REPO.parent / "pt-linux-env" / "sysroot"),
               help="the Debian sysroot of tools/linux/make_sysroot.py (default: PT_LINUX_SYSROOT or pt-linux-env/sysroot beside the checkout)")
p.add_argument("--linux-only", action="store_true", help="only the Linux setup (--linux-setup), from the Windows package a previous run left under --name")
p.add_argument("--game", type=pathlib.Path, default=REPO / "game" / "CUSA01127", help="game files, for the loop browser previews")
a = p.parse_args()
if a.linux_only and not (a.linux_setup and a.name):
    sys.exit("--linux-only needs --linux-setup and --name (the folder of the Windows run)")
if not re.fullmatch(r"\d+(\.\d+)*(-[0-9A-Za-z.]+)?", a.version):
    sys.exit(f"--version {a.version}: use a version like 1.0.0")
os.environ["PT_VERSION"] = a.version
SLOT = pathlib.Path(os.environ["PT_SLOT"]) if os.environ.get("PT_SLOT") else None


def build(target):
    """a build, 4 jobs"""
    argv = ["cmd", "/c", r"tools\build_pt.bat", "release", "release", target]
    env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL="4", NUMBER_OF_PROCESSORS="4")
    return [sys.executable, str(SLOT), "build", "--", *argv] if SLOT and SLOT.exists() else argv, env


a.out.mkdir(parents=True, exist_ok=True)
dest = a.dest / (a.name or datetime.datetime.now().strftime("%Y%m%d-%H%M"))
dest.mkdir(parents=True, exist_ok=True)
report = {"version": a.version, "dest": str(dest), "steps": []}
log = (a.out / "release.log").open("w", encoding="utf-8", errors="replace")


def run(name, argv, **kw):
    log.write(f"\n> {name}: {' '.join(map(str, argv))}\n")
    log.flush()
    r = subprocess.run([str(x) for x in argv], cwd=REPO, stdout=log, stderr=subprocess.STDOUT, **kw)
    report["steps"].append({"step": name, "exit": r.returncode})
    if r.returncode != 0:
        finish(1)
    return r


def finish(code):
    (a.out / "release.json").write_text(json.dumps(report, indent=2))
    log.close()
    sys.exit(code)


def newest_zip(folder: pathlib.Path) -> pathlib.Path:
    return max(folder.glob("pt-port-*.zip"), key=lambda z: z.stat().st_mtime)


# the Linux cross build (docs/linux.md): the same game target, built with tools/linux/cross_build.sh through the limiter
LINUX_BUILD = REPO / "build" / "linux-cross"


def build_linux(target):
    sh = shutil.which("sh") or r"C:\Program Files\Git\bin\sh.exe"
    argv = [sh, "tools/linux/cross_build.sh", target]
    deps = os.environ.get("PT_DEPS") or str(BUILD / "_deps")
    env = dict(os.environ, PT_LINUX_SYSROOT=str(a.linux_sysroot), PT_DEPS=deps)
    return [sys.executable, str(SLOT), "build", "--", *argv] if SLOT and SLOT.exists() else argv, env


def rezip(root: pathlib.Path) -> pathlib.Path:
    """the portable folder zipped again (as tools/package.py does) after the previews were added"""
    import zipfile
    archive = root.with_suffix(".zip")
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                z.write(path, pathlib.Path(root.name) / path.relative_to(root))
    return archive


def liborbis_source() -> pathlib.Path:
    """a private copy of the LGPL LibOrbisPkg source, patched: the source elsewhere is never changed"""
    liborbis = a.liborbis
    if not liborbis:
        for root in (REPO / "dump",):
            hits = sorted(root.glob("*/LibOrbisPkg*/LibOrbisPkg/PFS/PFSCReader.cs")) + sorted(root.glob("LibOrbisPkg*/LibOrbisPkg/PFS/PFSCReader.cs")) \
                if root.exists() else []
            if hits:
                liborbis = hits[0].parents[2]
                break
    report["liborbis"] = str(liborbis)
    if not liborbis or not (liborbis / "LibOrbisPkg.Core" / "LibOrbisPkg.Core.csproj").exists():
        report["installer"] = "skipped: LibOrbisPkg source not found (--liborbis)"
        finish(1)
    source = BUILD / "liborbis-src"
    if source.exists():
        shutil.rmtree(source)
    shutil.copytree(liborbis, source, ignore=shutil.ignore_patterns("bin", "obj", ".git"))
    csproj = source / "LibOrbisPkg.Core" / "LibOrbisPkg.Core.csproj"
    csproj.write_text(csproj.read_text(encoding="utf-8-sig").replace("netcoreapp3.0", "net10.0").replace("<LangVersion>7.3</LangVersion>", "<LangVersion>latest</LangVersion>")
                      .replace("<GenerateSerializationAssemblies>Auto</GenerateSerializationAssemblies>", "<GenerateSerializationAssemblies>Off</GenerateSerializationAssemblies>"),
                      encoding="utf-8")
    run("patch liborbis", [sys.executable, "tools/patch_liborbis_readers.py", source])
    return source


def publish_extractor(runtime_id: str) -> pathlib.Path:
    """the extraction helper published from the patched source for win-x64 or linux-x64, nothing prebuilt"""
    extractor = BUILD / ("extractor-win" if runtime_id == "win-x64" else "extractor-linux")
    if extractor.exists():
        shutil.rmtree(extractor)
    run(f"publish extractor {runtime_id}", ["dotnet", "publish", REPO / "installer" / "Extractor" / "Extractor.csproj", "-c", "Release", "-r",
                                            runtime_id, "--self-contained", "true", f"-p:LibOrbisSource={source}", "-o", extractor])
    return extractor


game_target = "pt_release"
package_dir = dest / "game"
report["files"] = []
if not a.linux_only:
    # 1. the developer game (its post build steps bring the DLLs, fonts and shaders) and the release game
    for target in ("pt", "pt_release"):
        argv, env = build(target)
        run(f"build {target}", argv, env=env)
    version_header = (BUILD / "generated" / "pt_version.h").read_text()
    if f'PT_VERSION "{a.version}"' not in version_header:
        report["error"] = "the build did not take --version"
        finish(1)
    # 2. the portable folder and zip: the release game as game/
    run("package pt_release", [sys.executable, "tools/package.py", "--exe", BUILD / "pt_release.exe", "--game", a.game, "--out", dest / "game"])
    runtime = newest_zip(package_dir)
    # 3. the native installer around that runtime
    source = liborbis_source()
    extractor = publish_extractor("win-x64")
    run("installer payload", [sys.executable, "tools/prepare_native_installer.py", "--runtime", runtime, "--extractor", extractor,
                              "--source", source])
    # the setup target exists only when the payload did at configure time, and the version is read then: reconfigure, then build it
    os.utime(REPO / "CMakeLists.txt")
    argv, env = build("pt_setup")
    run("build pt_setup", argv, env=env)
    setup = BUILD / "pt_setup.exe"
    run("stamp pt_setup", [sys.executable, "tools/ci/stamp_integrity.py", setup])
    shutil.copy2(setup, dest / SETUP_NAME)
    report["installer"] = str(dest / SETUP_NAME)
    report["files"] = [str(x) for x in sorted(dest.rglob("*.zip"))] + [report["installer"]]
else:
    if not package_dir.is_dir() or not list(package_dir.glob("pt-port-*.zip")):
        report["error"] = f"--linux-only: no Windows package under {package_dir}"
        finish(1)
    source = liborbis_source()
    report["installer"] = str(dest / SETUP_NAME)
# 4. the Linux setup (docs/installer.md, Linux): the cross build of the game and the setup, the portable zip (the previews the
# Windows package shot, since the Linux game cannot run here), the helper for linux-x64, the payload and the stamped setup
if a.linux_setup:
    os.utime(REPO / "CMakeLists.txt")  # the version is read at configure time
    # pt first: its post build steps and the all target bring the fonts, the shaders and the voice runtime the package ships
    for target in ("pt", game_target, "pt_setup_linux"):
        argv, env = build_linux(target)
        run(f"build linux {target}", argv, env=env)
    version_header = (LINUX_BUILD / "generated" / "pt_version.h").read_text()
    if f'PT_VERSION "{a.version}"' not in version_header:
        report["error"] = "the Linux build did not take --version"
        finish(1)
    linux = dest / "linux"
    if linux.exists():
        shutil.rmtree(linux)
    run("package linux", [sys.executable, "tools/package.py", "--build", LINUX_BUILD, "--exe", LINUX_BUILD / game_target, "--game",
                          linux / "no-game-files", "--out", linux / "runtime"])
    folder = next(x for x in (linux / "runtime").glob("pt-port-*") if x.is_dir())
    previews = [x for x in package_dir.glob("pt-port-*/loop-previews") if x.is_dir()]
    if previews:
        shutil.copytree(previews[0], folder / "loop-previews")
    report["linux_previews"] = "copied from the Windows package" if previews else "none (the game shoots them at first use)"
    runtime = rezip(folder)
    extractor = publish_extractor("linux-x64")
    run("linux payload", [sys.executable, "tools/prepare_native_installer.py", "--runtime", runtime, "--extractor", extractor, "--source", source,
                          "--out", linux / "payload"])
    linux_setup = dest / a.linux_setup
    linux_setup.unlink(missing_ok=True)
    run("attach linux payload", [sys.executable, "tools/linux/attach_payload.py", "--setup", LINUX_BUILD / "pt_setup_linux", "--payload",
                                 linux / "payload" / "payload.bin", "--out", linux_setup])
    report["linux_installer"] = str(linux_setup)
    report["files"] += [str(runtime), str(linux_setup)]
# 5. the update manifest (docs/updates.md), attached to the GitHub release next to the setup; .github/workflows/macos.yml adds
# the macOS app to it once the release is published
tag = f"v{a.version}"
manifest = {
    "version": a.version,
    "notes": a.notes,
    "url": f"{GITHUB_REPO}/releases/tag/{tag}",
    "platforms": {"windows": {"url": f"{GITHUB_REPO}/releases/download/{tag}/{SETUP_NAME}"}},
}
if a.linux_setup:
    manifest["platforms"]["linux"] = {"url": f"{GITHUB_REPO}/releases/download/{tag}/{a.linux_setup}"}
(dest / "latest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
report["manifest"] = str(dest / "latest.json")
report["files"].append(report["manifest"])
finish(0)
