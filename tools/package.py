import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
import stat
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# voice/ as the recognizer loads it (formats/voice.md): whisper.cpp and ggml with its CPU variants, the two models and
# their notices; the build folder also holds the libraries' debug symbols, which stay out
VOICE_MODELS = ("ggml-base.en-q5_1.bin", "ggml-small.en-q5_1.bin", "ggml-silero-v6.2.0.bin")
VOICE_LIBRARIES = ("whisper", "ggml", "ggml-base", "ggml-cpu-x64")
UPSCALER_RELEASE_FILES = {
    "FSR": "amd_fidelityfx_vk.dll",
    "DLSS": "nvngx_dlss.dll",
    "XeSS": "libxess.dll",
    # Intel XeSS Frame Generation (XeSS SDK 3.0.2, DirectX 12 through the D3D12 bridge) and the XeLL library it requires
    "XeSSFG": "libxess_fg.dll",
}
# the other runtime files a backend needs beside its main DLL
UPSCALER_COMPANION_FILES = {
    "XeSSFG": ("libxell.dll",),
}


def backend_files(name):
    return (UPSCALER_RELEASE_FILES[name], *UPSCALER_COMPANION_FILES.get(name, ()))
# DirectX 12 (docs/rhi.md): an executable that exports D3D12SDKVersion (the Agility SDK's, agility_exports.h) loads the D3D12
# runtime from D3D12/ next to it and the DXIL shaders from shaders/
D3D12_EXPORT_MARK = b"D3D12SDKVersion"
# and AMD's DX12 DLLs (cmake/Upscalers.cmake): the FSR SDK 2.3.0 loader and upscaler (FSR 3.1 and FSR 4 on DirectX 12) and v1.1.4's
# amd_fidelityfx_dx12.dll (FSR frame generation on DirectX 12)
D3D12_RUNTIME_FILES = ("D3D12/D3D12Core.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll",
                       "amd_fidelityfx_dx12.dll", "amd_fidelityfx_denoiser_dx12.dll")


def d3d12_capable(executable_bytes):
    return D3D12_EXPORT_MARK in executable_bytes


# the Linux build's upscaler runtimes (cmake/UpscalersLinux.cmake) next to pt: the FSR library built from AMD's FidelityFX
# sources, and the DLSS SDK's Super Resolution and Ray Reconstruction runtimes (NGX picks the newest version present).
# Their licences are in licenses/ with the others.
LINUX_UPSCALER_FILES = {
    "FSR": ("libamd_fidelityfx_vk.so",),
    "DLSS": ("libnvidia-ngx-dlss.so.*", "libnvidia-ngx-dlssd.so.*"),
}


def linux_upscaler_files(build):
    """The upscaler runtimes a Linux build configured with (upscalers.txt), or exit naming the missing ones."""
    build = Path(build)
    marker = build / "upscalers.txt"
    configured = {}
    if marker.is_file():
        for line in marker.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                configured[key] = value
    files, missing = [], []
    for backend, patterns in LINUX_UPSCALER_FILES.items():
        if configured.get(backend) != "1":
            print(f"warning: the Linux build has no {backend} (upscalers.txt); configure with -DPT_UPSCALERS=ON")
            continue
        for pattern in patterns:
            found = sorted(p for p in build.glob(pattern) if p.is_file() and p.stat().st_size > 0)
            if found:
                files.append(found[-1])
            else:
                missing.append(pattern)
    if missing:
        sys.exit(f"the Linux build lacks upscaler runtime files: {', '.join(missing)}")
    return files


STREAMLINE_RELEASE_FILES = (
    "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll", "sl.dlss_d.dll",
    "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll",
)


def _validate_manifest(executable_bytes, manifest_bytes):
    try:
        manifest = json.loads(manifest_bytes)
        backends = manifest["backends"]
        digest = manifest["sha256"]
    except (ValueError, KeyError, TypeError) as error:
        raise ValueError("Windows release build provenance manifest is invalid") from error
    if (not isinstance(backends, dict) or not isinstance(digest, str)
            or any(type(backends.get(name)) is not bool for name in (*UPSCALER_RELEASE_FILES, "Streamline"))):
        raise ValueError("Windows release build provenance manifest is invalid")
    if hashlib.sha256(executable_bytes).hexdigest() != digest:
        raise ValueError("Windows release executable does not match its build provenance manifest")
    missing = [name for name in UPSCALER_RELEASE_FILES if backends.get(name) is not True]
    if missing:
        raise ValueError("Windows release build provenance lacks required backends: " + ", ".join(missing))
    return backends


def validate_upscaler_release(build, executable):
    """Require all Windows release backends to be compiled and shipped."""
    build = Path(build)
    marker = build / "upscalers.txt"
    if not marker.is_file():
        raise ValueError(f"{marker} missing; configure a strict Windows release build")
    configured = {}
    for line in marker.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            configured[key] = value
    exe_bytes = Path(executable).read_bytes()
    provenance = Path(f"{executable}.upscalers.json")
    if not provenance.is_file():
        raise ValueError(f"Windows release build provenance missing: {provenance}")
    expected_keys = (*UPSCALER_RELEASE_FILES, "Streamline")
    missing = [name for name in expected_keys if configured.get(name) not in ("0", "1")]
    missing.extend(name for name in UPSCALER_RELEASE_FILES if configured.get(name) != "1")
    if missing:
        raise ValueError("Windows release upscaler configuration is incomplete or disabled: " + ", ".join(dict.fromkeys(missing)))
    manifest_backends = _validate_manifest(exe_bytes, provenance.read_bytes())
    mismatch = [name for name in expected_keys if manifest_backends[name] != (configured[name] == "1")]
    if mismatch:
        raise ValueError("Windows release executable provenance disagrees with upscalers.txt: " + ", ".join(mismatch))
    missing.extend(filename for name in UPSCALER_RELEASE_FILES if configured.get(name) == "1" for filename in backend_files(name)
                   if not (build / filename).is_file() or (build / filename).stat().st_size == 0)
    if configured["Streamline"] == "1":
        missing.extend(filename for filename in STREAMLINE_RELEASE_FILES
                       if not (build / filename).is_file() or (build / filename).stat().st_size == 0)
    if missing:
        raise ValueError("Windows release is missing required upscaler backend/runtime files: " + ", ".join(missing))


def zip_mode(path):
    """Unix mode a packaged file gets: ELF programs and shared libraries 0755, everything else 0644"""
    with open(path, "rb") as f:
        elf = f.read(4) == b"ELF"
    return 0o755 if elf or ".so" in Path(path).name else 0o644


def zip_add(z, path, arcname):
    """add a file with an explicit Unix mode, so unzip on Linux keeps the executable bit of pt and pt-updater"""
    info = zipfile.ZipInfo.from_file(path, str(arcname).replace("\\", "/"))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = (zip_mode(path) | stat.S_IFREG) << 16
    with open(path, "rb") as data:
        z.writestr(info, data.read())


def validate_upscaler_archive(archive):
    """Reject Windows installer payload archives without a backend record bound to pt.exe."""
    with zipfile.ZipFile(archive) as package:
        files = {}
        for entry in package.infolist():
            parts = Path(entry.filename).parts
            if len(parts) > 1 and parts[1].lower() == "data":
                raise ValueError("Windows runtime ZIP contains the reserved data directory")
            if not entry.is_dir() and len(parts) > 1:
                files[Path(*parts[1:]).as_posix()] = package.read(entry)
    exe = files.get("pt.exe")
    manifest = files.get("upscalers-build.json")
    if exe is None or manifest is None:
        raise ValueError("Windows runtime ZIP lacks pt.exe or its build provenance manifest")
    backends = _validate_manifest(exe, manifest)
    missing = [filename for name in UPSCALER_RELEASE_FILES for filename in backend_files(name) if not files.get(filename)]
    if d3d12_capable(exe):
        missing.extend(filename for filename in D3D12_RUNTIME_FILES if not files.get(filename))
    if backends["Streamline"]:
        missing.extend(filename for filename in STREAMLINE_RELEASE_FILES if not files.get(filename))
    if missing:
        raise ValueError("Windows runtime ZIP lacks required upscaler runtime files: " + ", ".join(missing))


# The package's own file list (docs/installer.md, "Updating an install"): every file it ships with its size and SHA-256, as
# pt-files.json at the package root (not listing itself). tools/prepare_native_installer.py writes it again for the payload, which
# adds the extraction helper; the installed copy tells the next update exactly which files this version shipped.
FILE_LIST = "pt-files.json"


def file_list(files):
    """pt-files.json for {path: bytes or Path}; the list itself is left out"""
    entries = []
    for name in sorted(files):
        if name == FILE_LIST:
            continue
        data = files[name]
        if isinstance(data, Path):
            digest, size = hashlib.sha256(), 0
            with data.open("rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    digest.update(block)
                    size += len(block)
            entries.append({"path": name, "size": size, "sha256": digest.hexdigest()})
        else:
            entries.append({"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    return (json.dumps({"format": 1, "files": entries}, indent=1, ensure_ascii=False) + "\n").encode("utf-8")


# what the DLSS 5 unlock (src/engine/render/upscale/neural_unlock.h) puts next to the game: the player's files, never package
# files. A build folder the unlock was tried in holds them.
UNLOCK_FILES = {"dxgi.dll", "nvngx_dlssnr.dll", "renodx-dlss5.addon64", "dlss5-autopilot.json", "pt-dlss5-route.json"}
# RTX30MFG-Unlock (docs/upscaling.md), which a player puts next to pt.exe by hand
MFG_UNLOCK_FILES = {"rtx40mfgcore.dll", "rtx40mfg.asi"}
FG_UNLOCK_HEADER = REPO / "src" / "engine" / "render" / "upscale" / "fg_unlock.h"


def fg_unlock_names(header=FG_UNLOCK_HEADER):
    """The DLSS Frame Generation unlock's own names, read from fg_unlock.h so the list cannot drift from the code: the proxy
    candidates (kProxyCandidates), the ini (kIni), the folder (kDir), our record (kManifest) and the proxy's marker (kProxyMarker)."""
    text = Path(header).read_text(encoding="utf-8")

    def constant(name):
        m = re.search(rf'\b{name}\s*=\s*"([^"]+)"', text)
        if not m:
            raise ValueError(f"{header}: {name} not found; tools/package.py reads the FG unlock's names from it")
        return m.group(1)

    m = re.search(r"kProxyCandidates\[\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        raise ValueError(f"{header}: kProxyCandidates not found")
    proxies = tuple(re.findall(r'"([^"]+)"', m.group(1)))
    return {"proxies": proxies, "ini": constant("kIni"), "dir": constant("kDir"), "record": constant("kManifest"),
            "marker": constant("kProxyMarker").encode()}


def _record_files(record):
    """the file names a community route's record lists (strings, or objects with a name), lower case with '/'"""
    try:
        data = json.loads(Path(record).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return set()
    names = set()
    if not isinstance(data, dict):
        return names
    for item in data.get("files", []) if isinstance(data.get("files"), list) else []:
        name = item if isinstance(item, str) else item.get("name") if isinstance(item, dict) else None
        if isinstance(name, str) and name:
            names.add(name.replace("\\", "/").lower())
    if isinstance(data.get("proxy"), str) and data["proxy"]:
        names.add(data["proxy"].lower())
    return names


def community_files(build):
    """Every file of a community route in a build folder (relative paths): the FG unlock's proxy (any candidate name carrying its
    marker), ini, folder and record and what the record lists; the DLSS 5 route's files and records and what they list;
    RTX30MFG-Unlock's. These are downloaded on the player's yes or put there by the player and are never shipped: 1.0.6 and
    1.0.7 shipped the FG unlock's version.dll because the *.dll copy took it from a build folder the unlock was tried in."""
    build = Path(build)
    fg = fg_unlock_names()
    found = set()
    for name in fg["proxies"]:
        path = build / name
        if path.is_file() and fg["marker"] in path.read_bytes():
            found.add(name)
    for name in (fg["ini"], fg["record"]):
        if (build / name).exists():
            found.add(name)
    if (build / fg["dir"]).exists():
        found.add(fg["dir"] + "/")
    for record in (fg["record"], "dlss5-autopilot.json", "pt-dlss5-route.json"):
        if (build / record).is_file():
            found |= {n for n in _record_files(build / record) if (build / n).exists()}
    for name in UNLOCK_FILES | MFG_UNLOCK_FILES:
        if (build / name).exists():
            found.add(name)
    return sorted(found)


# Every DLL the build puts next to pt.exe that the package ships (cmake/Upscalers.cmake, cmake/OpenXR.cmake, the pt_native_nvngx
# target). Nothing else is copied from the build root: a DLL not in this list stops the package, so a file someone put there by
# hand (an unlock's proxy, a test DLL) cannot ship. A new runtime DLL of the build is added here.
RELEASE_DLLS = (
    "amd_fidelityfx_vk.dll", "amd_fidelityfx_dx12.dll", "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll",
    "amd_fidelityfx_denoiser_dx12.dll", "libxess.dll", "libxess_fg.dll", "libxell.dll", "nvngx_dlss.dll", "nvngx_dlssd.dll",
    "nvngx_dlssg.dll", "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll", "sl.dlss_d.dll", "sl.reflex.dll",
    "sl.pcl.dll", "openxr_loader.dll", "pt-native-nvngx.dll",
)
# the voice runtime's DLLs (copy_voice): whisper.cpp and ggml with its CPU variants
VOICE_DLL = re.compile(r"(whisper|ggml|ggml-base|ggml-cpu-[a-z0-9]+)\.dll")


def check_build_folder(build):
    """The reasons a build folder cannot be packaged: community route files in it, or root or voice DLLs the build does not
    produce. Empty when it is clean."""
    build = Path(build)
    problems = []
    community = community_files(build)
    if community:
        problems.append("community unlock files are in the build folder (they are never shipped; move them out of "
                        f"{build} and package again): {', '.join(community)}")
    allowed = {n.lower() for n in RELEASE_DLLS}
    stray = sorted(p.name for p in build.glob("*.dll") if p.name.lower() not in allowed and p.name not in community)
    if stray:
        problems.append(f"DLLs in {build} that the build does not produce (tools/package.py RELEASE_DLLS): {', '.join(stray)}")
    voice = build / "voice"
    if voice.is_dir():
        stray_voice = sorted(p.name for p in voice.glob("*.dll") if not VOICE_DLL.fullmatch(p.name.lower()))
        if stray_voice:
            problems.append(f"DLLs in {voice} that are not the voice runtime: {', '.join(stray_voice)}")
    return problems


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


DATA_PACK = "extras.pak"
LOOSE_DATA = "lisa_date"


def refuse_loose_data(root: Path):
    """a package must not hold the data pack's files loose: the folder by its name, or any file equal to one of its sources"""
    loose = [p for p in root.rglob("*") if p.name.lower() == LOOSE_DATA]
    if loose:
        raise ValueError(f"the package holds {loose[0].relative_to(root).as_posix()}; those files ship only inside {DATA_PACK}")
    source = REPO / "assets" / LOOSE_DATA
    if source.is_dir():
        sizes = {}
        for p in source.rglob("*"):
            if p.is_file():
                sizes.setdefault(p.stat().st_size, []).append(p)
        for p in root.rglob("*"):
            if p.is_file() and p.stat().st_size in sizes and any(p.read_bytes() == s.read_bytes() for s in sizes[p.stat().st_size]):
                raise ValueError(f"the package holds {p.relative_to(root).as_posix()}, a loose file of assets/{LOOSE_DATA}")


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
    if exe.suffix.lower() == ".exe":
        try:
            validate_upscaler_release(build, exe)
        except ValueError as error:
            sys.exit(str(error))
    # a build folder an unlock was tried in, or one with a DLL the build does not make, is refused before anything is copied
    problems = check_build_folder(build)
    if problems:
        sys.exit(f"cannot package {build}:\n  " + "\n  ".join(problems))
    # a Linux build (build/linux-cross, docs/linux.md) has no .exe and no DLLs
    exe_name = "pt.exe" if exe.suffix.lower() == ".exe" else "pt"
    name = f"pt-port-{version()}"
    root = Path(args.out) / name
    archive = Path(args.out) / f"{name}.zip"
    if root.exists() or archive.exists():
        sys.exit(f"Package output already exists; choose another --out folder: {root}")
    (root / "shaders").mkdir(parents=True)
    shutil.copy2(exe, root / exe_name)
    if exe.suffix.lower() == ".exe":
        provenance = Path(f"{exe}.upscalers.json")
        if provenance.is_file():
            shutil.copy2(provenance, root / "upscalers-build.json")
    for spv in sorted((build / "shaders").glob("*.spv")):
        shutil.copy2(spv, root / "shaders" / spv.name)
    # DirectX 12: the DXIL shaders with their reflection, and the Agility SDK runtime (the debug layer stays out)
    if exe.suffix.lower() == ".exe" and d3d12_capable(exe.read_bytes()):
        dxil = sorted((build / "shaders").glob("*.dxil"))
        if not dxil:
            sys.exit(f"{exe.name} runs on DirectX 12 but {build / 'shaders'} has no DXIL shaders")
        for path in dxil + sorted((build / "shaders").glob("*.dxil.json")):
            shutil.copy2(path, root / "shaders" / path.name)
        for runtime in D3D12_RUNTIME_FILES:
            source = build / runtime
            if not source.is_file() or source.stat().st_size == 0:
                sys.exit(f"{exe.name} runs on DirectX 12 but {source} is missing")
            (root / runtime).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, root / runtime)
        print(f"DirectX 12: {len(dxil)} DXIL shaders, {', '.join(D3D12_RUNTIME_FILES)}")
    copy_voice(build / "voice", root / "voice")
    shutil.copytree(build / "fonts", root / "fonts")
    # the survival night's guns (assets/survival, the port's own models): their glTF files and guns.json
    if (build / "survival").is_dir():
        shutil.copytree(build / "survival", root / "survival")
    # the port's data pack (src/engine/fs/data_pack.h, built by the build from assets/); its loose source folder never ships,
    # and a copy older builds left in the build folder stays out
    if not (build / DATA_PACK).is_file():
        sys.exit(f"{build / DATA_PACK} is missing; build first with tools\\build_pt.bat release")
    shutil.copy2(build / DATA_PACK, root / DATA_PACK)
    if (build / LOOSE_DATA).exists():
        print(f"left out {LOOSE_DATA}/: loose files of an older build, packed in {DATA_PACK} now")
    # the loop browser's previews are shot by the packaged game itself from each entry's real start (pt.exe --make-loop-previews,
    # docs/gameplay.md loop browser); without the game files the game shoots them on the player's PC at the browser's first use
    if exe_name != "pt.exe":
        # a Linux build is cross built and cannot run on the machine that packages it; tools/ci/release.py copies the previews the
        # Windows package shot into the Linux folder (without them the game shoots them at the browser's first use)
        print(f"loop browser previews skipped: {exe_name} is not a Windows build and cannot run here")
    elif Path(args.game).is_dir():
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
        # the capture run's own Vulkan pipeline cache (this machine's GPU and driver) is no part of the package
        shutil.rmtree(previews / "pipeline-cache", ignore_errors=True)
    else:
        print(f"loop browser previews skipped: no game files at {args.game}")
    # the port's own ReShade for the DLSS 5 community route (tools/build_reshade.py), which pt-updater installs over Autopilot's
    if (build / "reshade" / "dxgi.dll").is_file():
        (root / "reshade").mkdir()
        shutil.copy2(build / "reshade" / "dxgi.dll", root / "reshade" / "dxgi.dll")
    allowed = {n.lower() for n in RELEASE_DLLS}
    for dll in sorted(build.glob("*.dll")):
        if dll.name.lower() in allowed:
            shutil.copy2(dll, root / dll.name)
    # the package itself, checked once more: no community route file, whatever the copies above took
    leaked = community_files(root)
    if leaked:
        sys.exit(f"the package holds community unlock files: {', '.join(leaked)}")
    if exe_name == "pt":
        for library in linux_upscaler_files(build):
            shutil.copy2(library, root / library.name)
            print(f"Linux upscaler runtime: {library.name}")
    # the in-game update's updater (docs/updates.md): the setup's program without a payload, pt-updater(.exe) next to the game
    updater = build / ("pt-updater.exe" if exe_name == "pt.exe" else "pt_setup_linux")
    if updater.is_file():
        shutil.copy2(updater, root / ("pt-updater.exe" if exe_name == "pt.exe" else "pt-updater"))
    else:
        print(f"warning: no updater at {updater}; this package cannot update itself in game")
    if (build / "licenses").is_dir():
        shutil.copytree(build / "licenses", root / "licenses",
                        ignore=shutil.ignore_patterns("enhanced-textures", "Real-ESRGAN-*.txt", "bc7enc.txt"))
    shutil.copy2(REPO / "README.md", root / "README.md")
    try:
        refuse_loose_data(root)
    except ValueError as error:
        sys.exit(str(error))
    (root / FILE_LIST).write_bytes(file_list({p.relative_to(root).as_posix(): p for p in root.rglob("*") if p.is_file()}))
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                zip_add(z, path, Path(name) / path.relative_to(root))
    size = archive.stat().st_size / (1024 * 1024)
    print(f"{root}\n{archive} ({size:.1f} MB)")


if __name__ == "__main__":
    main()
