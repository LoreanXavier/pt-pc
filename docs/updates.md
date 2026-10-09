# Update check

The game and the Windows/Linux installers each look for a newer release once, in the background. The version and update
endpoint are compiled into them; no separate update JSON file is required (see the one-time 1.0.2 compatibility asset below). macOS uses the same checker in its app.
Offline, with updates disabled, or when GitHub is unavailable/rate-limits the request, gameplay and installation continue normally.

## Configuration

`CMakeLists.txt` sets `PT_VERSION` and the metadata endpoint `PT_UPDATE_MANIFEST_URL` (the historical variable name is retained
for existing build/test overrides). The default endpoint is `https://api.github.com/repos/LoreanXavier/pt-pc/releases/latest`.
`cmake/version.cmake` writes these values into the generated header at build time. `tools/ci/release.py --version` supplies the
release version; `PT_VERSION_OVERRIDE` and `PT_UPDATE_MANIFEST_URL_OVERRIDE` are per-build overrides. The environment variable
`PT_UPDATE_MANIFEST_URL` overrides the endpoint at runtime for tests; HTTPS only. A `.invalid` host disables the request.

## Release discovery

The checker reads GitHub's `tag_name`, `html_url`, `body` and asset `name`/`browser_download_url` fields. It ignores drafts,
prereleases, nonnumeric release tags and API errors. It compares numeric versions with the embedded current version and selects:

| Platform | Release asset |
| --- | --- |
| Windows | `P.T.PC.Port.Setup.exe` |
| Linux | `P.T.PC.Port.Setup-linux` |
| Apple silicon | `P.T.PC.Port-macOS-arm64.zip` |
| Intel Mac | `P.T.PC.Port-macOS-x64.zip` |

Windows portable ZIPs remain available as an additional download, but the notification points to the installer. If the matching
asset is not present yet (for example, while the macOS workflow is building), the notice points to the release page. Custom Linux
installer names also fall back to the release page; use the canonical name above for a direct link. Only HTTPS asset links are used.
The first line of the release body supplies a short log note. The response limit is 512 KiB; timeout is 5 seconds.

The release pipeline produces only platform packages as upload candidates. Its build journal is local tooling output, outside the
release asset directory. The macOS workflow uploads its two app ZIPs without downloading, editing or uploading a manifest.
Legacy/custom manifest responses are still accepted by the parser for endpoint overrides, but no published manifest is required.

1.0.2 is the final compatibility release: pass `--legacy-update-manifest` to `tools/ci/release.py` to add one last
`latest.json` asset so existing 1.0.1 games/installers can announce 1.0.2. They fetch it automatically over HTTPS;
players never need a local copy. New 1.0.2 binaries already use the GitHub API and ignore that asset.
For 1.0.3 and later, omit the flag and publish only platform packages. The flag is rejected for every version except 1.0.2.
Users still on 1.0.1 after the compatibility asset is removed will need a manual download.

## Behaviour

- Game: `pt::update::Checker` starts one detached thread right after the settings are read (windowed runs only; headless
  runs and `--no-update-check` never send a request; `[network] check_updates = 0` in pt.ini turns it off). The game loop
  never touches the network: it polls the result once a frame until the thread is done. When a newer version exists, pt.log
  gets the URL and notes, and the game shows a notice once per run: one line, "Version X is available (this is Y)" (the
  text key `pc_update_available`, in the subtitle language), in the PC system font, small and muted, centred at the top
  of the picture below the letterbox's bar (`GameUi::DrawUpdateNotice`). It fades in over 0.5 s, stays 6 s and fades out
  over 1 s. It shows only in the game proper (controller step 15, so not at the first start's option screen and preface,
  the game over, the ending and the credits), with no cutscene camera, outside the Museum's theater and the photo mode,
  with the pause menu and the PC settings page counting as the game; until then it waits. It never shows in VR. The main
  PC settings page keeps the same text in its small muted corner line for as long as the game runs.
- Test: `--fake-update <version>` makes the check answer that version without a request (headless too); a version not
  newer than this build's is "nothing". The input script shots of the notice: `tools/update_notice_check.py`.
- Installer (Windows): the check starts when the window opens; a timer shows "Version X is available: URL" under the
  options when the thread is done. Linux installer: printed after the install (and in the zenity message).
- Timeouts: 5 s per request. Quitting never waits for it.

## Tests

- `pt_platform_test`: version order, GitHub asset selection for every platform, missing-asset fallback, ignored drafts/prereleases/API errors and legacy parsing, the
  placeholder sends nothing; with `--network`, a real HTTPS request and, when `PT_UPDATE_MANIFEST_URL` is set, the check.
- `pt_setup.exe --check-update <file>` and `pt_setup_linux --check-update <file>` write the URL, this version and what
  was found.
