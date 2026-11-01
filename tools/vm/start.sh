#!/bin/sh
# Boots the VM in the background (headless): framebuffer /dev/fb0 640x480x32 with two
# pages, a virtio keyboard on /dev/input/event*, /mnt/SDCARD and /mnt/repo shared over 9p.
# VM_DISPLAY=cocoa opens the screen in a window whose keyboard drives Shelf (see README).
set -eu
. "$(dirname "$0")/common.sh"
if vm_running; then echo "vm: already running"; exit 0; fi
[ -f $DISK ] && [ -f $SEED ] || sh tools/vm/build-image.sh
mkdir -p "$SDCARD"
rm -f $MON $VM/monitor-rec.sock
t0=$(date +%s)
# A window can't survive QEMU's -daemonize fork on macOS, so a windowed VM runs as a plain
# background process instead.
display=${VM_DISPLAY:-none}
detach=-daemonize
[ "$display" = none ] || detach=
vm_qemu() {
qemu-system-arm -M virt,highmem=off -cpu cortex-a15 -smp 2 -m 1024 \
	-kernel $KERNEL -initrd $INITRD \
	-append "root=LABEL=cloudimg-rootfs rw console=ttyAMA0 video=640x480 drm_kms_helper.drm_fbdev_overalloc=200 fbcon=map:9 vt.global_cursor_default=0 quiet" \
	-drive if=none,file=$DISK,id=hd,format=qcow2 -device virtio-blk-pci,drive=hd \
	-drive if=none,file=$SEED,id=seed,format=raw,readonly=on -device virtio-blk-pci,drive=seed \
	-netdev user,id=net,hostfwd=tcp:127.0.0.1:$PORT-:22 -device virtio-net-pci,netdev=net \
	-device virtio-gpu-pci,xres=640,yres=480 -device virtio-keyboard-pci \
	-virtfs local,path="$SDCARD",mount_tag=sdcard,security_model=none,id=sdcard \
	-virtfs local,path="$PWD",mount_tag=repo,security_model=none,readonly=on,id=repo \
	-display $display -monitor unix:$MON,server,nowait \
	-chardev socket,id=rec,path=$VM/monitor-rec.sock,server=on,wait=off -mon chardev=rec,mode=readline -serial file:$VM/serial.log \
	$detach -pidfile $VM/qemu.pid
}
if [ -n "$detach" ]; then vm_qemu; else
	vm_qemu >$VM/qemu.log 2>&1 &
	i=0; while [ ! -s $VM/qemu.pid ] && [ $i -lt 50 ]; do sleep .2; i=$((i + 1)); done
fi
echo "vm: booting (serial log $VM/serial.log)"
while ! vm_ssh test -f /var/lib/shelf-vm-ready 2>/dev/null; do
	if [ $(( $(date +%s) - t0 )) -gt ${VM_BOOT_TIMEOUT:-600} ]; then echo "vm: boot timed out" >&2; exit 1; fi
	vm_running || { echo "vm: qemu exited" >&2; tail -20 $VM/serial.log >&2; exit 1; }
	sleep 3
done
echo "vm: up in $(( $(date +%s) - t0 )) s (ssh -p $PORT root@127.0.0.1)"
