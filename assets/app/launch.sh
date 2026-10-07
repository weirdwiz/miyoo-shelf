#!/bin/sh
# Started by Onion from Apps. MENU quits back to MainUI. Picking a game hands its command
# to Onion's runtime, which runs it once Shelf exits and then returns to MainUI.
# SHELF_TRY=1 only logs the command. Shelf's Options can switch boot mode: it exits 11 for
# "Make Shelf the home screen" (or 10 to undo it) and boot.sh does the rest. Log: /tmp/shelf.log
cd "$(dirname "$0")"
sysdir=/mnt/SDCARD/.tmp_update
rm -f /tmp/shelf_cmd.sh
# libpadsp.so routes SDL audio through Onion's audioserver, as for MainUI.
LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so SHELF_STATS=1 SHELF_CMD=/tmp/shelf_cmd.sh ./shelf > /tmp/shelf.log 2>&1
case $? in
10) sh ./boot.sh disable >> /tmp/shelf.log 2>&1 ;;
11) sh ./boot.sh enable >> /tmp/shelf.log 2>&1 ;;
esac

if [ -s /tmp/shelf_cmd.sh ] && [ -z "$SHELF_TRY" ]; then
    # The runtime's sh is still reading the old cmd_to_run.sh (this app's command), so
    # replace it by rename rather than rewriting it in place. quick_switch keeps the new
    # command for the next loop instead of returning to MainUI.
    cp /tmp/shelf_cmd.sh "$sysdir/cmd_to_run.sh.new" &&
        mv -f "$sysdir/cmd_to_run.sh.new" "$sysdir/cmd_to_run.sh" &&
        touch /tmp/quick_switch
    sync
fi
