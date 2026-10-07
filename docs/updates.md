# Update check

The game and the installer each look for a newer release once, in the background, and show a small notice when there is
one. Nothing waits for the answer: offline or with the check turned off, everything works as before.

## Where it is set

`CMakeLists.txt` holds both values:

- `PT_VERSION` (`1.0.0`, raised for each release). `tools/ci/release.py --version` sets it through the environment and
  `cmake/version.cmake` writes `generated/pt_version.h` at every build; the cache variable `PT_VERSION_OVERRIDE` sets it
  for one build folder.
- `PT_UPDATE_MANIFEST_URL` (default `https://github.com/LoreanXavier/pt-pc/releases/latest/download/latest.json`). GitHub
  answers `releases/latest/download/<asset>` with a redirect to the newest release's asset, so the address is the same
  for every release. A host on the reserved `.invalid` domain is a placeholder for which no request is made at all.

The environment variable `PT_UPDATE_MANIFEST_URL` overrides the built-in address at run time (HTTPS only).

## The manifest

`tools/ci/release.py` writes it into the release folder as `latest.json`; it goes up as a release asset next to the setup:

    {
      "version": "1.0.0",
      "notes": "",
      "url": "https://github.com/LoreanXavier/pt-pc/releases/tag/v1.0.0",
      "platforms": {
        "windows": {"url": "https://github.com/LoreanXavier/pt-pc/releases/download/v1.0.0/P.T.PC.Port.Setup.exe"},
        "linux": {"url": "https://github.com/LoreanXavier/pt-pc/releases/download/v1.0.0/<linux setup name>"}
      }
    }

`version` is required; the platform's `url` is used when present, else the top `url`. Versions compare by their numbers
(`0.10.0` is newer than `0.9.2`, a leading `v` is ignored, `0.2.0-rc1` is older than `0.2.0`). The answer is limited to
64 KB and HTTPS only; 5 s per request.

## Behaviour

- Game: one detached thread starts right after the settings are read, in windowed runs only. `--no-update-check` or
  `[network] check_updates = 0` in pt.ini turns it off. When a newer version exists, the main PC settings page shows
  "Version X is available (this is Y)" in its corner line, and pt.log gets the URL and notes.
- Installer: the Windows setup shows "Version X is available: URL" under the options once the answer is in; the Linux
  setup prints it after the install. `pt_setup.exe --check-update <file>` writes the result to a file.
