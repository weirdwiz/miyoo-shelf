#!/bin/sh
# Runs in place of Onion's MainUI once boot.sh has mounted it, with MainUI's environment.
# Like MainUI, Shelf writes the picked game to /tmp/cmd_to_run.sh and exits; the runtime
# then runs it and starts us again. MENU in Shelf (or Shelf failing) opens Onion's own
# menu, which hands its choice back the same way. "Use Onion's menu instead" in Shelf's
# Options exits 10: undo boot mode, then open Onion's menu. Log: /tmp/shelf.log
sysdir=/mnt/SDCARD/.tmp_update
cd "$(dirname "$0")"
./shelf > /tmp/shelf.log 2>&1
rc=$?

[ -f /tmp/.offOrder ] && exit 0 # powering off
[ $rc = 10 ] && sh ./boot.sh disable >> /tmp/shelf.log 2>&1
[ -s /tmp/cmd_to_run.sh ] && exit 0

mode=$([ -f "$sysdir/config/.showExpert" ] && echo expert || echo clean)
cd /mnt/SDCARD/miyoo/app
exec "$sysdir/bin/MainUI-$(cat /tmp/deviceModel)-$mode"
