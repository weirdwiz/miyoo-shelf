#!/bin/sh
# Runs the device build on the VM's screen.
#   make mm dist && sh tools/vm/run_shelf.sh   # copy the card in and start Shelf
#   sh tools/vm/run_shelf.sh stop              # stop it
# The repository is read-only in the guest, so the card is built on this side: build/dist
# is rsynced into the shared folder, keeping the VM's own Saves and .shelf/data, and the
# test ROMs and fixture box art are copied in when they exist.
set -eu
. "$(dirname "$0")/common.sh"
DIST=build/dist/SDCARD
# The library path the updater runs Shelf with (assets/sdcard/.tmp_update/updater).
LIB_PATH=/lib:/config/lib:/customer/lib:/mnt/SDCARD/.shelf/lib

stop_shelf() {
	vm_ssh 'pkill -x shelf || true'
}

if [ "${1:-}" = stop ]; then
	vm_running && stop_shelf
	echo "vm: shelf stopped"
	exit 0
fi

[ -x "$DIST/.shelf/bin/shelf" ] || { echo "vm: run 'make mm dist' first" >&2; exit 1; }
vm_running || sh tools/vm/start.sh
vm_mount_shares
stop_shelf

mkdir -p "$SDCARD"
rsync -a --delete --exclude '/Saves/' --exclude '/.shelf/data/' "$DIST/" "$SDCARD/"

mkdir -p "$SDCARD/Roms/GB" "$SDCARD/Roms/GBC" "$SDCARD/Roms/GBA"
for rom in build/testroms/*.gb build/testroms/*.gbc build/testroms/*.gba; do
	[ -f "$rom" ] || continue
	case $rom in
	*.gba) cp -f "$rom" "$SDCARD/Roms/GBA/" ;;
	*.gbc) cp -f "$rom" "$SDCARD/Roms/GBC/" ;;
	*) cp -f "$rom" "$SDCARD/Roms/GB/" ;;
	esac
done

# Box art for the test ROMs (e.g. their title screens) can be kept in build/vm/art/<SYS>/.
for imgs in fixture/SDCARD/Roms/*/Imgs build/vm/art/*; do
	[ -d "$imgs" ] || continue
	system=${imgs%/Imgs}
	system=${system##*/}
	case $system in GB | GBC | GBA) ;; *) continue ;; esac
	mkdir -p "$SDCARD/Roms/$system/Imgs"
	cp -f "$imgs"/* "$SDCARD/Roms/$system/Imgs/" 2>/dev/null || true
done

# The redirect is on the whole group: a partly redirected async list would leave the
# ssh channel open and the ssh command would never return.
vm_ssh "( cd /mnt/SDCARD/.shelf && LD_LIBRARY_PATH=$LIB_PATH SHELF_NO_POWEROFF=1 \
	exec setsid ./bin/shelf ) >/tmp/shelf.log 2>&1 </dev/null &"
sleep 1
vm_ssh 'pgrep -a shelf || { echo "vm: shelf did not start" >&2; cat /tmp/shelf.log >&2; exit 1; }'
echo "vm: shelf running, log /tmp/shelf.log"
