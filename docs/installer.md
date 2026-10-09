# Installer

The release is an installer that builds a working P.T. folder from the player's own copy of the game. It ships the PC
runtime (the executable, shaders, fonts, voice model, upscaler DLLs, licenses) and no Konami asset: the three game
archives are taken from what the player selects, on the player's PC.

## Pieces

| Piece | Source | Language | Role |
|---|---|---|---|
| `pt_setup.exe` (shipped as `P.T.PC.Port.Setup.exe`) | `installer/Native/setup.cpp`, CMake target `pt_setup` | C++20, Win32 API | the Windows installer: window, checks, payload unpack, install |
| `pt_setup_linux` | `installer/Native/setup_linux.cpp` | C++20 | the Linux installer, a console program (zenity dialogs when available) |
| `PT.PkgExtract.exe` | `installer/Extractor/Program.cs` | C#, .NET 10 self-contained, LGPL-3.0-or-later | reads a fake PKG with LibOrbisPkg and writes the three archives |
| LibOrbisPkg | upstream `maxton/LibOrbisPkg`, patched by `tools/patch_liborbis_readers.py` | C#, LGPL | PKG and PFS reader used by the helper |
| extractor tests | `installer/Tests/Program.cs` | C# | builds a synthetic fake PKG with LibOrbisPkg and checks extraction and refusals |

The extraction helper is a separate executable on purpose: it is LGPL, so it stays a replaceable program next to the
setup, and its corresponding source (patched LibOrbisPkg plus the wrapper) goes into the payload as
`extractor/LibOrbisPkg-corresponding-source.zip` with `extractor/LICENSE-LibOrbisPkg.txt`.

## How a release is built

`python tools/ci/release.py --dest <releases folder> --out <report folder> --version 1.0.0` builds the release.
`--name` fixes the folder under `--dest` (default: a date and time stamp), `--linux-setup <name>` also makes the Linux
setup. The script:

1. sets `PT_VERSION` (written into `build/release/generated/pt_version.h` at every build by `cmake/version.cmake`; the
   game, the setup and the update check read it);
2. builds `pt` and `pt_release` with `tools/build_pt.bat` and checks that the build took the version;
3. runs `tools/package.py` for the portable folder and zip (`pt_release.exe` ships as `pt.exe`); the installer wraps that
   zip;
4. copies the LibOrbisPkg source (`--liborbis`, else the first `LibOrbisPkg*` folder under `dump/`) into
   `build/release/liborbis-src`, retargets it to net10.0 and applies `tools/patch_liborbis_readers.py`; the original is
   never changed;
5. publishes the helper from that copy (`dotnet publish installer/Extractor -c Release -r win-x64 --self-contained true`)
   into `build/release/extractor-win`;
6. runs `tools/prepare_native_installer.py --release`, which writes the payload from the zip and the helper. It refuses a
   runtime zip with a `game`, `CUSA01127` or `enhanced-textures` folder in it, so game data cannot slip into a release;
7. builds `pt_setup`, which CMake defines only when that payload exists; the payload goes in as Windows resource 100
   (`installer/Native/setup.rc.in`, which also carries the version and product name). On Linux
   `tools/linux/attach_payload.py` appends it to `pt_setup_linux` after the build;
8. runs `tools/ci/stamp_integrity.py`, which writes the SHA-256 of the finished setup into the setup itself. The setup
   reads its own file at start and compares; an unstamped or patched setup does not run. The game does not check its own
   file;
The game and installers discover updates through the GitHub Releases API (docs/updates.md); no separate update JSON
file is uploaded with the platform packages.

`release.json` in the report folder lists the version, the LibOrbisPkg source used, every step's exit code and the
files.

The payload is `PTSETUP1`, a u32 file count, then per file: u16 name length, UTF-8 name, u64 size, u64 packed size,
64 hex chars of SHA-256, zlib data. At install every file is unpacked, written and hashed again; a wrong hash, a path
with `..`, a drive letter or `:` stops the install.

## What the player gives it

The setup takes one input, a file or a folder (the `PKG...` and `Folder...` buttons fill the same field). Every input
format below already holds decrypted game files; setup and helper never decrypt Sony-encrypted content.

There are exactly two refusals:
1. an encrypted PKG the helper cannot open (a store PKG, or a damaged one);
2. input that is not P.T.

Every release and version of P.T. installs. What differs from the tested US data is installed anyway, written to
`install-notes.txt` in the install folder and to `CUSA01127/source.txt`, and logged by the game at start (`data:` lines).

| Input | Result | How it is read |
|---|---|---|
| fake PKG (fPKG) of any P.T. release | installs | helper: PKG magic `7F 43 4E 54`, the PFS key opens with LibOrbisPkg's fake-package keyset, the inner PFSC image is searched for the archives |
| game folder dumped from the player's console (`CUSA01127`, `CUSA01114-app`, a dumper's output folder) | installs | setup copies the archives, found by their headers |
| an fPKG extracted to a folder, or an emulator's install folder (shadPS4 writes `CUSA01127/` with the archives at its root) | installs | as a dump folder |
| a folder that holds no archives but exactly one PKG (for example `user/app/CUSA01127/` copied off a console) | the PKG is used | as a PKG |
| EU (CUSA01114), JP (CUSA01098) or an unknown P.T. title ID | installs, with a note | see Releases below |
| archives under other names or in a subfolder of the PKG | installs, with a note | found by content and written under the names the port reads |
| no `pathid_list_ps4.bin` | installs, with a note | only enhanced texture generation needs it |
| store (retail) PKG | refused (1) | message: dump P.T. from your own PS4 and select the dump folder or an fPKG made from it |
| an update or add-on PKG, another game, an encrypted folder copy | refused (2) | message: no P.T. game data here |

### How P.T. is recognised

By content, not by title ID alone:
- a PSARC (`PSAR` magic) and a QAR (`aq` at offset 0x16 of the last 0x24 bytes): `chunk1.psarc` and `texture.qar` when those
  names exist, else the largest valid `.psarc` / `.qar` (in the PKG: anywhere outside `sce_sys` and `sce_module`; in a folder:
  that folder);
- and P.T.'s level names (`pt14_`) in the path list (`pathid_list_ps4.bin`, else a file whose name contains `pathid_list`),
  or a known P.T. title ID (CUSA01127, CUSA01114, CUSA01098). A folder with neither a path list nor a title ID (an extracted
  fPKG without `sce_sys`) installs with a note that it could not be confirmed.

The title ID comes from the PKG content ID, or a folder's `sce_sys/param.sfo` (`TITLE_ID`). Folders are searched from the
selected folder down two levels (not `sce_sys` or `sce_module`); selecting a single archive means its folder. An
encrypted copy (files taken off a console disk without decryption) has no valid header, so nothing is found.

There is no whole-file hash check: a correct dump made by another tool, or another release, is not refused for
differing bytes.

### Releases

| Title ID | Release | Status |
|---|---|---|
| CUSA01127 | US | the data the port is built and tested with |
| CUSA01114 | Europe | installs with a note; I have not seen its archives |
| CUSA01098 | Japan | installs with a note; I have not seen its archives |

The US `chunk1.psarc` already holds all seven subtitle languages (English, French, German, Italian, Japanese,
Portuguese, Spanish) with English voice only, and the European store page lists the same seven, so the EU and JP data
is expected to carry the same set. The installer handles the title ID, content ID, archive names and locations. The game
logs `source.txt` at start, names any of its twelve core packages missing from this copy's `chunk1.psarc`, and falls
back to English for a subtitle language the copy does not have. Anything else that differs (other internal paths, a
different `texture.qar` layout) is not handled: the install goes ahead and the game logs what it cannot load.

If you have an EU or JP dump: install it, then open an issue with `install-notes.txt` and the `data:` lines of `pt.log`.

### Why only fake PKGs and dumps

A store PKG is encrypted for the console that holds its license. Opening one needs that console's keys, which is DRM
circumvention, and neither the setup nor the helper does it. A fake PKG is the homebrew package format: tools rebuild it
from a dump and encrypt its PFS with a publicly known fake keyset, which LibOrbisPkg uses to read it back. The step that
makes any of this possible is the dump on the player's own console; the conversion after that is repackaging. P.T. was
delisted in 2015, so in practice the source is a console that still has it installed.

## What it installs

1. The destination is checked: no junctions or symbolic links in it or above it. An empty name or a folder that holds
   something else is refused; an existing install is updated in place (next section).
2. At least 3 GB free (1 GB for an update that keeps the game archives).
3. A staging folder `.pt-install-<id>` next to the destination receives the payload (runtime and helper).
4. The game archives go to `staging/CUSA01127/`: the helper extracts them from a PKG, or setup copies them from a folder.
   Only `chunk1.psarc`, `texture.qar` and `pathid_list_ps4.bin` are written, under these names whatever the release calls
   them, plus `source.txt` (title ID, region, notes); the eboot, `sce_sys` and modules are not used by the port. Only
   these fixed names are written, so names inside a PKG cannot escape the folder. Notes go to `install-notes.txt`.
5. `pt-install-manifest.txt` is written: `pt-port-install 1`, `version=<PT_VERSION>`, then `file=<sha256> <path>` for every
   program file (everything the payload wrote; not the game archives, notes or anything the player adds).
6. The staging folder is renamed to the destination and the optional desktop shortcut made. On any error or cancel only
   that staging folder is removed.

The game finds the data without `--game`: `game/CUSA01127` or `CUSA01127` next to it or up to four folders above, then
the folder remembered in the user data folder, then (windowed runs) a folder picker.

## Updating an install

When the chosen folder holds a P.T. port install, the setup asks first: "P.T. is already installed here (version X).
Update it to version Y?", or, for the same version, "already installed here and up to date. Repair it?". An install is
recognised by its manifest, or, for installs made before the manifest existed, by the executable together with
`CUSA01127/chunk1.psarc` and `CUSA01127/texture.qar`. Any other folder stays refused.

The update (`RunInstall` and `SwapProgramFiles` in `setup_core.h`):
- unpacks the new program files into `.pt-update-<id>` next to the install and checks each one's SHA-256;
- keeps the game archives when `chunk1.psarc` (PSAR header) and `texture.qar` (QAR footer) pass their check; otherwise the
  PKG or dump folder field must name the game again, and the archives are restored from it (with new notes);
- moves each new file into place by rename, the file it replaces first into `.pt-update-old-<id>`;
- removes the program files the new version no longer ships into that backup too: those listed in the old manifest, or,
  for an install without one, only names the setup itself ships. Unknown files stay;
- writes the new manifest as part of the same swap;
- keeps everything else: pt.ini and saves (they live in the user data folder anyway), mods, logs, enhanced textures,
  anything added by hand;
- on any failure (a file in use because P.T. is running, a full disk, cancel) moves every file back in reverse order,
  so the install is exactly as before, and says "Nothing was changed"; then deletes its staging and backup folders.

A repair is the same run with the same version: all program files are rewritten from the setup and the archives checked.

Command line, used by tests: `pt_setup.exe --install <pkg or folder, or "" to keep the archives> <folder> <result file>`
(a new folder installs, an install is updated without asking; the result file says `PASS installed` or `PASS updated`
with the counts), `--check-update <result file>`, `--self-test <result file>` (path containment, input formats, update
manifest, and the in-place update on synthetic installs with and without a manifest), `--preview <bmp>` (draws the
window to a file without showing it). The setup window and the game both show the version (the setup in its header
line, the game in the PC settings corner line and at the top of pt.log).

## Tools

- `tools/pkginfo.py <pkg> [--extract DIR]`: prints a PKG's header, entry table, PFS header and param.sfo, and writes the
  plaintext `sce_sys` entries. Useful to tell a fake PKG from a store PKG and to read the content ID.
- `installer/Tests`: `dotnet run -c Release -p:LibOrbisSource=<LibOrbisPkg> -- <new dir> [<dump folder>]` builds fake PKGs
  (synthetic, or from a dump's archives) and checks exact extraction without notes (US), refusal of an existing output and
  of an invalid file, EU and JP content IDs installed with a note, archives renamed into a subfolder found by content, an
  unknown title with P.T.'s content installed, another game refused, and a store PKG (entry keys that the fake keyset
  cannot open) refused with the dump message.

## Linux

The installer is C++ for Windows (`setup.cpp`, Win32 window) and Linux (`setup_linux.cpp`); what they share is
`installer/Native/setup_core.h` (input formats, P.T. recognition, archive copy, notes, payload format, path checks). The
extraction helper is C#/.NET 10 and publishes for Linux unchanged (`dotnet publish installer/Extractor -c Release
-r linux-x64 --self-contained true -p:LibOrbisSource=<LibOrbisPkg>`).

`pt_setup_linux` (CMake target outside Windows) runs from a terminal (asks for the PKG or folder and the new folder), or
with zenity file pickers when started without a terminal; the command line options are those of the Windows setup. It
installs a menu entry in `~/.local/share/applications` and sets the execute bits the payload does not store. Making it
by hand:

    python tools/package.py --build build/linux-cross --exe build/linux-cross/pt --out <dir>/runtime
    python tools/prepare_native_installer.py --release --runtime <dir>/runtime/<zip> --extractor <linux helper> --source <LibOrbisPkg> --out <dir>/payload
    python tools/linux/attach_payload.py --setup build/linux-cross/pt_setup_linux --payload <dir>/payload/payload.bin --out <dir>/pt-setup-linux

The payload is appended to the executable (payload, `PTPAYLD1`, u64 offset, u64 size), then the file is stamped.

## Update check

Both setups look for a newer release when they start (docs/updates.md) and show it without blocking anything.

For 1.0.2 only, add `--legacy-update-manifest` to the release command. This creates the final remote `latest.json` asset for existing 1.0.1 update notifications. Do not pass it for 1.0.3 or later.
