#!/bin/sh
# Saves what the VM's screen shows now: tools/vm/shot.sh out.png (or .ppm). The Miyoo's
# panel is mounted upside down and Shelf draws rotated for it, so the shot is turned 180
# degrees to show what a player sees; RAW=1 keeps framebuffer orientation.
set -eu
. "$(dirname "$0")/common.sh"
out=$1
ppm=$VM/shot-$$.ppm
pan=$(vm_ssh 'cat /sys/class/graphics/fb0/pan' 2>/dev/null || echo 0,0)
if [ "${pan#*,}" = 0 ]; then
	vm_monitor "screendump $PWD/$ppm" >/dev/null
	for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $ppm ] && break; sleep 0.2; done
else
	vm_ssh python3 - <tools/vm/fbpage.py >$ppm # the host scanout misses the second page
fi
rot=--rotate180; [ "${RAW:-}" = 1 ] && rot=
python3 -I tools/img.py $rot $ppm "$out"
rm -f $ppm
