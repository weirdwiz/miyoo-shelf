#!/bin/sh
# Requires a running Apotris or Lazarus; exercises actual switch + startup + sleep/wake.
# Explicitly enable overlay trials first: boot.sh switcher-on. No reboot/forced game kill.
set -eu
cd "$(dirname "$0")/.."
for helper in device-probe ra-status test-fb-idle; do
    tools/mm.sh "cat > /tmp/shelf-$helper && chmod +x /tmp/shelf-$helper" < "build/mm/$helper"
done
tools/mm.sh "${TRACE:+set -x;}"'
set -e
press() { sendkeys "$1" 1; sleep 0.1; sendkeys "$1" 0; }
playing() {
    for i in $(seq 1 30); do
        /tmp/shelf-ra-status PLAYING && return 0
        sleep 1
    done
    echo "FAIL: RetroArch never became ready" >&2; return 1
}
held() {
    for i in $(seq 1 8); do
        p=$(pidof retroarch || true)
        [ -n "$p" ] && grep -q "^State:.*T" "/proc/$p/status" && return 0
        sleep 1
    done
    echo "FAIL: overlay did not acquire its renderer" >&2; return 1
}
open_overlay() {
    test -z "$(pidof gameSwitcher || true)"
    press 1
    held
    test -n "$(pidof gameSwitcher || true)"
    test -f /mnt/SDCARD/.tmp_update/.runGameSwitcher
}
resume_overlay() {
    before=$(pidof retroarch || true)
    press 1
    sleep 1
    playing
    test "$before" = "$(pidof retroarch || true)"
    test -z "$(pidof gameSwitcher || true)"
    test ! -f /mnt/SDCARD/.tmp_update/.runGameSwitcher
}
# Each step must switch to the game that is NOT running (picking the running one resumes).
if playing | grep -q Apotris; then targets="Pokemon Lazarus|Apotris|Pokemon Lazarus|Apotris"
else targets="Apotris|Pokemon Lazarus|Apotris|Pokemon Lazarus"; fi
IFS="|"
for target in $targets; do
    unset IFS
    old=$(pidof retroarch || true)
    open_overlay
    index=$(/tmp/shelf-device-probe index "$target")
    press 18 # L1: known starting card, then target index from the real library
    for i in $(seq 1 "$index"); do press 106; sleep 0.15; done
    press 57
    for i in $(seq 1 15); do
        new=$(pidof retroarch || true)
        [ -n "$new" ] && [ "$new" != "$old" ] && break
        sleep 1
    done
    test -n "$new" && test "$new" != "$old"
    for i in $(seq 1 10); do [ -z "$(pidof gameSwitcher || true)" ] && break; sleep 0.5; done
    test -z "$(pidof gameSwitcher || true)"
    # Press MENU before UDP may be ready. A declined launch must leave no SDL overlay
    # or sticky marker; a successful launch must be a real held overlay.
    press 1
    sleep 2
    if pidof gameSwitcher >/dev/null; then
        held
        resume_overlay
    else
        test ! -f /mnt/SDCARD/.tmp_update/.runGameSwitcher
        playing
    fi
    playing | grep "$target"
    # Sleep/wake in-game, then prove MENU still works.
    /tmp/shelf-device-probe powercycle
    playing
    open_overlay
    overlay=$(pidof gameSwitcher || true)
    # Sleep/wake inside overlay: keymon must not restart the game renderer.
    /tmp/shelf-device-probe powercycle
    held
    test "$overlay" = "$(pidof gameSwitcher || true)"
    sleep 8
    /tmp/shelf-test-fb-idle
    /tmp/shelf-device-probe capture /tmp/shelf-trial-overlay.ppm
    resume_overlay
    /tmp/shelf-device-probe capture /tmp/shelf-trial-game.ppm
    echo "PASS: switch to $target; immediate MENU; in-game sleep; overlay sleep; MENU resume"
done
'
