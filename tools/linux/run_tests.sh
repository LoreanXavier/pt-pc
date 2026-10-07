#!/bin/sh
# Runs the Linux build's checks on Linux or in WSL (docs/linux.md), after tools/linux/cross_build.sh made build/linux-cross.
# From the repository, in WSL:
#   sh tools/linux/run_tests.sh [game folder] [output folder] [linux installer]
# Defaults: the game at game/CUSA01127 under the repository, output in ~/pt-linux-test, installer at
# ./P.T.PC.Port.Setup-linux. Each step's log goes to the output folder; summary.txt lists them.
# At most three game processes run at once (one at a time here).
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$REPO/build/linux-cross"
GAME=${1:-$REPO/game/CUSA01127}
OUT=${2:-$HOME/pt-linux-test}
SETUP=${3:-/mnt/d/pt-platform-out/P.T.PC.Port.Setup-linux}
WINEXE="$REPO/build/release/pt.exe"
export SDL_AUDIO_DRIVER=dummy
mkdir -p "$OUT"
SUMMARY="$OUT/summary.txt"
: > "$SUMMARY"
note() { echo "$*" | tee -a "$SUMMARY"; }
step() {  # step <name> <command...>: runs it with its output in <name>.log, records the exit code
    name=$1; shift
    start=$(date +%s)
    "$@" > "$OUT/$name.log" 2>&1
    code=$?
    note "$name: exit $code ($(( $(date +%s) - start )) s)"
    return $code
}
# the copies on /mnt/c have no exec bit on some WSL setups: work on a local copy of the build
LOCAL="$OUT/build"
rm -rf "$LOCAL"
cp -r "$BUILD" "$LOCAL" 2>/dev/null || { note "cannot copy $BUILD"; exit 1; }
rm -rf "$LOCAL/_deps" "$LOCAL/CMakeFiles"
chmod +x "$LOCAL"/pt* "$LOCAL"/texture-tools/realesrgan-ncnn-vulkan 2>/dev/null

note "== environment"
{ uname -a; cat /etc/os-release 2>/dev/null | head -3; ldd --version 2>&1 | head -1; } >> "$SUMMARY"
command -v vulkaninfo >/dev/null && vulkaninfo --summary > "$OUT/vulkaninfo.log" 2>&1 && grep -E "deviceName|driverName|apiVersion" "$OUT/vulkaninfo.log" | head -6 >> "$SUMMARY"
for lib in libvulkan.so.1 libcurl.so.4 libX11.so.6 libwayland-client.so.0 libasound.so.2 libpulse.so.0; do
    if ldconfig -p 2>/dev/null | grep -q "$lib"; then note "have $lib"; else note "MISSING $lib"; fi
done
note "(Debian/Ubuntu: sudo apt install libvulkan1 mesa-vulkan-drivers libcurl4 libx11-6 libxext6 libxcursor1 libxi6 libxrandr2 libxss1 libwayland-client0 libxkbcommon0 libasound2t64 libpulse0 vulkan-tools)"

note "== ELF"
ldd "$LOCAL/pt" > "$OUT/ldd-pt.log" 2>&1
grep -q "not found" "$OUT/ldd-pt.log" && note "ldd: missing libraries (ldd-pt.log)" || note "ldd: all found"

note "== unit tests"
step platform "$LOCAL/pt_platform_test" --network
for t in pt_tests pt_collision_test pt_mods_test pt_graphics_preset_test pt_gouge_input_test pt_peephole_look_test pt_light_cull_test; do
    step "$t" "$LOCAL/$t"
done
step pt_multilingual_test "$LOCAL/pt_multilingual_test" "$GAME"
step pt_localization_test "$LOCAL/pt_localization_test" "$GAME"
rm -rf "$OUT/save_reset_tmp"
step pt_save_reset_test "$LOCAL/pt_save_reset_test" "$OUT/save_reset_tmp"

note "== headless game"
LVP=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)
step smoke-default "$LOCAL/pt" --headless --game "$GAME" --frames 200 --no-save --audio-offline --validation --log "$OUT/smoke-default.pt.log"
[ -n "$LVP" ] && step smoke-lavapipe env VK_DRIVER_FILES="$LVP" VK_ICD_FILENAMES="$LVP" "$LOCAL/pt" --headless --game "$GAME" --frames 200 --no-save --audio-offline --log "$OUT/smoke-lavapipe.pt.log"
grep -h "data:\|vulkan: device\|GPU\|error" "$OUT"/smoke-*.pt.log 2>/dev/null | head -20 >> "$SUMMARY"

note "== walkthrough (early, f060; one game at a time)"
for scenario in early f060; do
    step "walk-$scenario" python3 "$REPO/tools/walkthrough.py" "$scenario" --exe "$LOCAL/pt" --game "$GAME"
    tail -3 "$OUT/walk-$scenario.log" >> "$SUMMARY"
done

note "== Linux installer"
if [ -f "$SETUP" ]; then
    cp "$SETUP" "$OUT/setup" && chmod +x "$OUT/setup"
    step setup-selftest "$OUT/setup" --self-test "$OUT/setup-selftest.txt"; cat "$OUT/setup-selftest.txt" >> "$SUMMARY"; echo >> "$SUMMARY"
    step setup-update "$OUT/setup" --check-update "$OUT/setup-update.txt"; cat "$OUT/setup-update.txt" >> "$SUMMARY"; echo >> "$SUMMARY"
    rm -rf "$OUT/install"
    step setup-install "$OUT/setup" --install "$GAME" "$OUT/install/PT" "$OUT/setup-install.txt"; cat "$OUT/setup-install.txt" >> "$SUMMARY"; echo >> "$SUMMARY"
    [ -x "$OUT/install/PT/pt" ] && step installed-smoke "$OUT/install/PT/pt" --headless --frames 200 --no-save --audio-offline --log "$OUT/installed.pt.log"
else
    note "no installer at $SETUP"
fi

note "== Windows build under Wine or Proton (the fallback)"
if [ -f "$WINEXE" ]; then
    PROTON=$(ls -d "$HOME"/.steam/steam/steamapps/common/Proton*/ "$HOME"/.local/share/Steam/steamapps/common/Proton*/ 2>/dev/null | tail -1)
    WINGAME=$(command -v winepath >/dev/null && winepath -w "$GAME" 2>/dev/null || echo "Z:$GAME")
    if [ -n "$PROTON" ]; then
        mkdir -p "$OUT/proton-prefix"
        step proton env STEAM_COMPAT_DATA_PATH="$OUT/proton-prefix" STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam" \
            "$PROTON/proton" run "$WINEXE" --headless --game "$WINGAME" --frames 200 --no-save --audio-offline
    elif command -v wine >/dev/null; then
        step wine wine "$WINEXE" --headless --game "$WINGAME" --frames 200 --no-save --audio-offline
    else
        note "no wine or Proton installed (sudo apt install wine64, or Steam with Proton)"
    fi
fi
note "done: $SUMMARY"
