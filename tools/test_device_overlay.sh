#!/bin/sh
# Requires a running game; tests short MENU -> pause -> A/MENU resume without relaunch.
# MM_HOST=<ip> sh tools/test_device_overlay.sh (after make mm + make push).
set -eu
cd "$(dirname "$0")/.."
tools/mm.sh 'cat > /tmp/shelf-ra-status && chmod +x /tmp/shelf-ra-status' < build/mm/ra-status
tools/mm.sh 'cat > /tmp/shelf-test-fb-idle && chmod +x /tmp/shelf-test-fb-idle' < build/mm/test-fb-idle
tools/mm.sh '
set -e
press() { sendkeys "$1" 1; sleep 0.1; sendkeys "$1" 0; }
status() {
    for i in 1 2 3 4 5; do
        /tmp/shelf-ra-status "$1" && return 0
        sleep 1
    done
    return 1
}
held() {
    for i in 1 2 3 4 5; do
        grep -q "^State:.*T" "/proc/$before/status" && return 0
        sleep 1
    done
    echo "FAIL: RetroArch renderer is not stopped" >&2
    return 1
}
status PLAYING
[ -z "$(pidof gameSwitcher)" ]
before=$(pidof retroarch)
for resume_key in 57 1; do
    press 1
    sleep 8 # let decorative pulses settle before checking framebuffer ownership
    held
    /tmp/shelf-test-fb-idle
    [ "$before" = "$(pidof retroarch)" ]
    overlay=$(pidof gameSwitcher)
    [ -n "$overlay" ]
    tr "\000" " " < "/proc/$overlay/cmdline" | grep -q -- --overlay
    press "$resume_key"
    sleep 1
    status PLAYING
    [ "$before" = "$(pidof retroarch)" ]
    [ -z "$(pidof gameSwitcher)" ]
    echo "PASS: MENU pauses; key $resume_key resumes the same game process"
done
press 1
sleep 1
held
kill -TERM "$(pidof gameSwitcher)"
sleep 1
status PLAYING
[ "$before" = "$(pidof retroarch)" ]
[ -z "$(pidof gameSwitcher)" ]
echo "PASS: SIGTERM cleans up the overlay and resumes the same game"
'
