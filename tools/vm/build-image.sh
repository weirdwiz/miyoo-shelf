#!/bin/sh
# Downloads Ubuntu 22.04's armhf cloud image (glibc 2.35, armhf hard-float like the Miyoo)
# and prepares a copy-on-write disk plus a cloud-init seed. FORCE=1 starts from scratch.
set -eu
. "$(dirname "$0")/common.sh"
mkdir -p $VM "$SDCARD"
[ "${FORCE:-}" = 1 ] && rm -f $DISK $SEED $KEY $KEY.pub
for f in $BASE_IMG $KERNEL $INITRD; do
	[ -s "$f" ] || curl -sfL -o "$f.part" "$BASE_URL/$( [ "$f" = $BASE_IMG ] || echo unpacked/)$(basename "$f")" && mv "$f.part" "$f" 2>/dev/null || true
done
sha=$(sha256sum $BASE_IMG 2>/dev/null || shasum -a 256 $BASE_IMG 2>/dev/null || true)
[ "${sha%% *}" = $BASE_SHA ] || { echo "vm: base image checksum mismatch" >&2; exit 1; }
[ -f $KEY ] || ssh-keygen -q -t ed25519 -N '' -C shelf-vm -f $KEY
if [ ! -f $DISK ]; then
	qemu-img create -q -f qcow2 -b "$(basename $BASE_IMG)" -F qcow2 $DISK 6G
fi
if [ ! -f $SEED ]; then
	d=$VM/seed
	rm -rf $d && mkdir -p $d
	printf 'instance-id: shelf-vm\nlocal-hostname: shelf-vm\n' > $d/meta-data
	cat > $d/user-data <<UD
#cloud-config
disable_root: false
ssh_pwauth: false
users:
  - name: root
    ssh_authorized_keys: ["$(cat $KEY.pub)"]
mounts:
  - [sdcard, /mnt/SDCARD, 9p, "trans=virtio,version=9p2000.L,msize=512000,cache=none,nofail", "0", "0"]
  - [repo, /mnt/repo, 9p, "trans=virtio,version=9p2000.L,msize=512000,ro,nofail", "0", "0"]
bootcmd:
  - [sh, -c, "for m in 9p 9pnet_virtio virtio_gpu virtio_input evdev; do modprobe \$m || true; done"]
write_files:
  - path: /etc/systemd/logind.conf.d/shelf.conf
    content: "[Login]\\nHandlePowerKey=ignore\\nHandlePowerKeyLongPress=ignore\\n"
runcmd:
  - [sh, -c, "systemctl disable --now snapd.service snapd.socket snapd.seeded.service unattended-upgrades.service apt-daily.timer apt-daily-upgrade.timer motd-news.timer || true"]
  - [sh, -c, "mkdir -p /mnt/SDCARD /mnt/repo; mount -a || true; touch /var/lib/shelf-vm-ready"]
UD
	if command -v hdiutil >/dev/null; then
		hdiutil makehybrid -quiet -iso -joliet -default-volume-name cidata -o $SEED $d
	else
		for iso in xorrisofs genisoimage mkisofs; do command -v $iso >/dev/null && break; done
		command -v $iso >/dev/null || { echo "vm: need hdiutil, xorrisofs, genisoimage or mkisofs to make the seed" >&2; exit 1; }
		$iso -quiet -output $SEED -volid cidata -joliet -rock $d
	fi
fi
echo "vm: image ready ($DISK, $SEED)"
