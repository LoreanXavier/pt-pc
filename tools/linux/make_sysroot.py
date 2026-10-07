"""Builds a Debian trixie amd64 sysroot for cross compiling pt on Windows with clang (headers and libraries only)."""
import hashlib, io, lzma, re, sys, tarfile, urllib.request
from pathlib import Path

# run it in the folder that should hold the sysroot (Packages.xz is downloaded there too)
ROOT = Path.cwd()
SYSROOT = ROOT / "sysroot"
DEBS = ROOT / "debs"
MIRROR = "https://deb.debian.org/debian/"
WANT = """libc6-dev linux-libc-dev libgcc-14-dev libstdc++-14-dev libgcc-s1 libstdc++6 libc6
libx11-dev libxext-dev libxrandr-dev libxrender-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev
libxkbcommon-dev libxcb1-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libdrm-dev
libgbm-dev libegl-dev libgl-dev libgles-dev libwayland-dev libdecor-0-dev libvulkan-dev libusb-1.0-0-dev""".split()
SKIP = re.compile(r"^(perl|python|dpkg|debconf|coreutils|bash|sed|grep|tar|gzip|mount|util-linux|systemd|init|adduser|login|passwd|"
                  r"gcc-14-base|libc-bin|libc-dev-bin|rpcsvc-proto|libpam|base-files|dbus-bin|dbus-daemon|dbus-system-bus|"
                  r"dbus-session-bus|dbus$|pipewire-bin|pipewire$|libpipewire-0.3-modules|libpipewire-0.3-common|libspa-0.2-modules|"
                  r"xkb-data|libwayland-bin|pkgconf|pkg-config|x11-common|libasound2-data|mesa|libgl1-mesa|libglx-mesa|"
                  r"libegl-mesa|libgbm1$|libdrm-common|libpulse0$|libpulse-mainloop-glib|libglib2|libgirepository|"
                  r"libsystemd|libudev1$|libcap|libselinux|libapparmor|libsndfile|libasyncns|libx11-data|libxcb-|libllvm|"
                  r"libelf|libz3|libsensors|libwayland-client0$|libwayland-server0$|libwayland-cursor0$|libwayland-egl1$)")
INCLUDE_RUNTIME = {"libc6", "libgcc-s1", "libstdc++6"}


def packages():
    if not (ROOT / "Packages.xz").exists():
        urllib.request.urlretrieve(MIRROR + "dists/trixie/main/binary-amd64/Packages.xz", ROOT / "Packages.xz")
    text = lzma.decompress((ROOT / "Packages.xz").read_bytes()).decode("utf-8", "replace")
    db = {}
    for block in text.split("\n\n"):
        fields = dict(re.findall(r"^([A-Za-z0-9-]+): (.*)$", block, re.M))
        if "Package" in fields:
            db.setdefault(fields["Package"], fields)
            for p in fields.get("Provides", "").split(","):
                name = p.strip().split(" ")[0]
                if name:
                    db.setdefault("virtual:" + name, fields)
    return db


def resolve(db):
    seen, order, stack = set(), [], list(WANT)
    while stack:
        name = stack.pop()
        if name in seen:
            continue
        seen.add(name)
        if SKIP.match(name) and name not in WANT and name not in INCLUDE_RUNTIME:
            continue
        pkg = db.get(name) or db.get("virtual:" + name)
        if not pkg:
            print("missing", name)
            continue
        order.append(pkg)
        for dep in re.split(r",", pkg.get("Depends", "") + "," + pkg.get("Pre-Depends", "")):
            alt = dep.split("|")[0].strip().split(" ")[0].split(":")[0]
            if alt:
                stack.append(alt)
    return order


def ar_members(data):
    assert data[:8] == b"!<arch>\n"
    at = 8
    while at < len(data):
        name = data[at:at + 16].decode().strip().rstrip("/")
        size = int(data[at + 48:at + 58].decode().strip())
        yield name, data[at + 60:at + 60 + size]
        at += 60 + size + (size & 1)


def extract(pkg):
    file = DEBS / Path(pkg["Filename"]).name
    if not file.exists():
        urllib.request.urlretrieve(MIRROR + pkg["Filename"], file)
    data = file.read_bytes()
    if hashlib.sha256(data).hexdigest() != pkg["SHA256"]:
        sys.exit(f"hash mismatch {file}")
    for name, member in ar_members(data):
        if name.startswith("data.tar"):
            if name.endswith(".zst"):
                sys.exit(f"zstd member in {file}")
            with tarfile.open(fileobj=io.BytesIO(member)) as tar:
                for m in tar.getmembers():
                    if m.name.startswith(("./usr/share/doc", "./usr/share/man", "./usr/share/locale", "./usr/bin", "./usr/sbin", "./etc")):
                        continue
                    target = SYSROOT / m.name
                    if m.isdir():
                        target.mkdir(parents=True, exist_ok=True)
                    elif m.issym() or m.islnk():
                        # Windows symlinks need privileges: store the link target's content (resolved later) as a copy
                        links.append((target, m.linkname, m.issym()))
                    elif m.isfile():
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(tar.extractfile(m).read())


links = []
if __name__ == "__main__":
    DEBS.mkdir(exist_ok=True)
    db = packages()
    order = resolve(db)
    print(len(order), "packages:", " ".join(sorted(p["Package"] for p in order)))
    for pkg in order:
        extract(pkg)
    # resolve symlinks by copying, a few passes for chains
    for _ in range(4):
        pending = []
        for target, link, sym in links:
            src = (target.parent / link) if sym and not link.startswith("/") else SYSROOT / link.lstrip("/")
            if src.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(src.read_bytes())
            elif src.is_dir():
                import shutil
                shutil.copytree(src, target, dirs_exist_ok=True)
            else:
                pending.append((target, link, sym))
        links = pending
    print("unresolved links:", len(links))
    for t, l, _ in links[:20]:
        print("  ", t.relative_to(SYSROOT), "->", l)
    # libc.so names /lib/x86_64-linux-gnu/libc.so.6 and /lib64/ld-linux-x86-64.so.2 (merged /usr): copies there for lld
    import shutil
    for src, dst in ((SYSROOT / "usr/lib/x86_64-linux-gnu", SYSROOT / "lib/x86_64-linux-gnu"), (SYSROOT / "usr/lib64", SYSROOT / "lib64")):
        dst.mkdir(parents=True, exist_ok=True)
        for f in src.glob("*.so*"):
            if f.is_file() and (f.name.startswith("ld-linux") or ".so." in f.name):
                shutil.copy2(f, dst / f.name)
    print("sysroot:", SYSROOT)
