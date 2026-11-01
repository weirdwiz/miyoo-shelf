#!/usr/bin/env python3
"""Regression: atomic updates mark bind-mount sources //deleted in mountinfo."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
script = (root / "assets/app/boot.sh").read_text().split('case "$1" in', 1)[0]
for source, mounted in [
    ("/App/Shelf/keymon-overlay", True),
    ("/App/Shelf/keymon-overlay//deleted", True),
    ("/other/keymon", False),
]:
    fixture = f"34 26 179:1 {source} /mnt/SDCARD/.tmp_update/bin/keymon rw - vfat /dev/mmcblk0p1 rw"
    # Substitute only grep's mountinfo input, executing the actual boot.sh helper.
    harness = script + '\ngrep() { printf "%s\\n" "$FIXTURE" | command grep "$1" "$2"; }\nkeymon_mounted\n'
    result = subprocess.run(["sh", "-c", harness], env={"FIXTURE": fixture})
    assert (result.returncode == 0) == mounted, source
print("boot: own and atomically replaced keymon mounts recognized; foreign mount rejected")

# Execute the actual switcher-off case with mount/process operations mocked, checking
# that the independent MainUI hook survives and subsequent remounts stay disabled.
case = (root / "assets/app/boot.sh").read_text().split('case "$1" in', 1)[1]
with tempfile.TemporaryDirectory(prefix="shelf-boot-test-") as directory:
    base = Path(directory)
    shelf = base / "Shelf"
    shelf.mkdir()
    for name in ["shelf", "keymon-overlay"]:
        executable = shelf / name
        executable.write_text("#!/bin/sh\nexit 0\n")
        executable.chmod(0o755)
    (base / "sys/onionVersion").mkdir(parents=True)
    (base / "sys/onionVersion/version.txt").write_text("v4.3.1-1")
    hook = base / "startup-hook"
    hook.write_text("preserve Shelf home\n")
    harness = script + r'''
shelf="$TEST_DIR/Shelf"
sysdir="$TEST_DIR/sys"
hook="$TEST_DIR/startup-hook"
switcher_disabled="$shelf/boot/switcher-disabled"
sm=1; km=1; restarted=0
switcher_mounted() { [ "$sm" = 1 ]; }
keymon_mounted() { [ "$km" = 1 ]; }
umount() {
    case "$2" in
        "$switcher") sm=0 ;;
        "$keymon") km=0 ;;
        *) echo "FAIL: switcher-off touched home" >&2; exit 1 ;;
    esac
}
restart_keymon() { restarted=$((restarted + 1)); }
mount() { echo "FAIL: disabled switcher remounted" >&2; exit 1; }
sync() { :; }
''' + 'case "$1" in' + case + r'''
[ "$sm" = 0 ] && [ "$km" = 0 ] && [ "$restarted" = 1 ] || exit 1
[ -f "$switcher_disabled" ] && [ -f "$hook" ] || exit 1
mount_switcher && mount_keymon || exit 1
'''
    result = subprocess.run(["sh", "-c", harness, "boot.sh", "switcher-off"], env={"TEST_DIR": directory})
    assert result.returncode == 0
    assert hook.read_text() == "preserve Shelf home\n"
print("boot: switcher-off restores stock, preserves home, and prevents remounts")
