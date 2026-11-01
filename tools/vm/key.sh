#!/bin/sh
# Presses device buttons in the VM: tools/vm/key.sh a [hold_ms]  (several: key.sh right right a)
# Names: up down left right a b x y l r l2 r2 select start menu power volup voldown, or raw
# QEMU key names. `power` is a short press only (QEMU's system_powerdown). A number after a name sets that key's hold time (default 80 ms).
set -eu
. "$(dirname "$0")/common.sh"
qkey() {
	case $1 in
	a) echo spc ;; b) echo ctrl ;; x) echo shift ;; y) echo alt ;;
	l) echo e ;; r) echo t ;; l2) echo tab ;; r2) echo backspace ;;
	select) echo ctrl_r ;; start) echo ret ;; menu) echo esc ;;
	volup) echo volumeup ;; voldown) echo volumedown ;;
	*) echo "$1" ;;
	esac
}
while [ $# -gt 0 ]; do
	if [ "$1" = power ]; then # QEMU's power button arrives as KEY_POWER on gpio-keys (event0)
		vm_monitor system_powerdown >/dev/null; shift; sleep 0.2; continue
	fi
	k=$(qkey "$1"); shift
	hold=80
	case ${1:-} in [0-9]*) hold=$1; shift ;; esac
	out=$(vm_monitor "sendkey $k $hold")
	[ -z "$out" ] || { echo "vm: sendkey $k: $out" >&2; exit 1; }
	sleep "$(awk "BEGIN{print ($hold + 60) / 1000}")"
done
