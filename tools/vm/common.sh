# Shared settings for the VM scripts (sourced). Paths are relative to the repo root.
cd "$(dirname "$0")/../.."
VM=build/vm
BASE_URL=https://cloud-images.ubuntu.com/jammy/current
BASE_IMG=$VM/jammy-server-cloudimg-armhf.img
BASE_SHA=386690bde3bd771ab11416c957cefe1022fc73ec6dd3015478f905d73381b7e2
KERNEL=$VM/jammy-server-cloudimg-armhf-vmlinuz-lpae
INITRD=$VM/jammy-server-cloudimg-armhf-initrd-generic-lpae
DISK=$VM/disk.qcow2
SEED=$VM/seed.iso
KEY=$VM/id_ed25519
MON=$VM/monitor.sock
PORT=${VM_SSH_PORT:-2222}
SDCARD=${VM_SDCARD:-$VM/sdcard}

vm_running() { [ -f $VM/qemu.pid ] && kill -0 "$(cat $VM/qemu.pid)" 2>/dev/null; }

vm_ssh() {
	ssh -q -i $KEY -p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		-o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes root@127.0.0.1 "$@"
}

# The shares have nofail in fstab and the 9p transport can load after fstab has run.
vm_mount_shares() { vm_ssh 'for m in /mnt/repo /mnt/SDCARD; do mountpoint -q $m || { modprobe 9pnet_virtio; mount $m; }; done'; }

# Sends one command to the QEMU monitor and prints its reply.
vm_monitor() {
	python3 -I - "$MON" "$1" <<'PY'
import re, socket, sys, time
# The monitor takes one client at a time (a recording may hold it briefly): retry.
for attempt in range(100):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sys.argv[1])
        break
    except (ConnectionRefusedError, BlockingIOError):
        s.close()
        time.sleep(0.02)
else:
    sys.exit("vm: monitor busy")
s.settimeout(2)
def drain():
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk: break
            out += chunk
            if out.endswith(b"(qemu) "): break
    except socket.timeout:
        pass
    return out
drain()
s.sendall(sys.argv[2].encode() + b"\n")
reply = drain().decode(errors="replace")
# The monitor echoes the command with line-editing escapes; keep only what follows it.
reply = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", reply).replace("\r", "")
lines = [l for l in reply.split("\n")[1:] if l and not l.startswith("(qemu)")]
print("\n".join(lines))
PY
}
