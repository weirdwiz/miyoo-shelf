#!/bin/sh
# Saves what the VM's screen shows now: tools/vm/shot.sh out.png (or .ppm). The Miyoo's
# panel is mounted upside down and Shelf draws rotated for it, so the shot is turned 180
# degrees to show what a player sees; RAW=1 keeps framebuffer orientation.
set -eu
. "$(dirname "$0")/common.sh"
out=$1
ppm=$VM/shot-$$.ppm
vm_monitor "screendump $PWD/$ppm" >/dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $ppm ] && break; sleep 0.2; done
[ "${RAW:-}" = 1 ] || sips -r 180 $ppm >/dev/null
case $out in
*.ppm) mv $ppm "$out" ;;
*) sips -s format png $ppm --out "$out" >/dev/null && rm -f $ppm ;;
esac
