#!/bin/sh
set -eu
. "$(dirname "$0")/common.sh"
vm_running || { echo "vm: not running"; exit 0; }
vm_ssh 'systemctl poweroff' 2>/dev/null || true
for i in $(seq 1 30); do vm_running || { echo "vm: stopped"; exit 0; }; sleep 1; done
kill "$(cat $VM/qemu.pid)"; echo "vm: killed"
