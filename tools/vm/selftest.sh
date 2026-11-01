#!/bin/sh
# Proves the VM can stand in for the device: a device-toolchain binary runs, fb0 has two
# pages and panning lands on the second, and injected keys arrive as the Miyoo's key codes.
set -eu
. "$(dirname "$0")/common.sh"
vm_running || sh tools/vm/start.sh
# The 9p transport can load after fstab has already skipped the shares (they have nofail).
vm_ssh 'for m in /mnt/repo /mnt/SDCARD; do mountpoint -q $m || { modprobe 9pnet_virtio; mount $m; }; done'
# A running Shelf reads the same keys, so the self-test would never see them.
vm_ssh 'pkill -x shelf || true'
[ -x $VM/fbtest ] || podman run --rm --platform linux/amd64 -v "$PWD":/root/workspace:z -w /root/workspace docker.io/aemiii91/miyoomini-toolchain:latest /bin/bash -c "source /root/.bashrc && \${CROSS_COMPILE}gcc -O2 -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -o $VM/fbtest tools/vm/fbtest.c"
ev=$(vm_ssh 'for d in /sys/class/input/event*; do grep -qi keyboard $d/device/name && echo /dev/input/${d##*/} && break; done')
[ -n "$ev" ] || { echo "FAIL: no keyboard event device" >&2; exit 1; }
vm_ssh "/mnt/repo/$VM/fbtest $ev 2 > /tmp/fbtest.out 2>&1 &"
sleep 3
out=$(vm_ssh cat /tmp/fbtest.out)
echo "$out"
echo "$out" | grep -q 'virtual 640x960\|virtual 640x1440' || { echo "FAIL: fb geometry" >&2; exit 1; }
# The virtio-gpu host resource is one page tall, so a screendump goes black past y=480:
# read the panned-to page's pixel and the pan offset where the guest can see them. fbtest
# paints page 1 blue and fb0 is BGRA, so its last row starts with the bytes ff 00 00.
line=$(echo "$out" | sed -n 's/.* line \([0-9]*\).*/\1/p')
last=$(echo "$out" | sed -n 's/.* virtual [0-9]*x\([0-9]*\).*/\1/p')
p1=$(vm_ssh "dd if=/dev/fb0 bs=$line skip=$((last - 1)) count=1 2>/dev/null | od -An -tx1 -N3 | tr -d ' \n'")
pan=$(vm_ssh cat /sys/class/graphics/fb0/pan)
echo "page 1 bytes $p1 at pan $pan"
case "$p1 $pan" in "ff0000 0,480") ;; *) echo "FAIL: expected the blue second page" >&2; exit 1 ;; esac
# Page 0 is what the screen scans out on its own: pan back so the shot can show it.
vm_ssh 'echo 0,0 > /sys/class/graphics/fb0/pan'
sleep 1
RAW=1 sh tools/vm/shot.sh $VM/selftest.png
sh tools/vm/key.sh a menu
sleep 1
out=$(vm_ssh cat /tmp/fbtest.out)
px=$(python3 -I tools/vm/pixel.py $VM/selftest.png)
echo "screen $px"
case $px in "640x480 320,240=ff0000") ;; *) echo "FAIL: expected the red first page" >&2; exit 1 ;; esac
echo "$out" | grep -q 'key 57' && echo "$out" | grep -q 'key 1$' || { echo "FAIL: keys" >&2; exit 1; }
echo "vm selftest: PASS (input $ev)"
