#!/bin/sh
# Builds the hardware-settings test for the device and runs it in the VM: first against a
# throwaway sysfs tree (SHELF_HW_ROOT), then against the real root, where none of the
# Miyoo's files or devices exist, to show the absence is silent. Needs a running VM.
set -eu
. "$(dirname "$0")/common.sh"
vm_running || sh tools/vm/start.sh
mkdir -p "$PWD/build/mm"
TOOLCHAIN=docker.io/aemiii91/miyoomini-toolchain:latest
MM_FLAGS="-O2 -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c11 -D_DEFAULT_SOURCE"
podman run --rm --platform linux/amd64 -v "$PWD":/root/workspace -w /root/workspace $TOOLCHAIN \
	/bin/bash -c "source /root/.bashrc && \${CROSS_COMPILE}gcc $MM_FLAGS -o build/mm/test-hw-mm tools/test_hw_mm.c src/hw_mm.c -lm"
# The device build must also be warning-free with shelf's own warning set, as errors.
podman run --rm --platform linux/amd64 -v "$PWD":/root/workspace -w /root/workspace $TOOLCHAIN \
	/bin/bash -c "source /root/.bashrc && \${CROSS_COMPILE}gcc $MM_FLAGS -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -Werror -fsyntax-only tools/test_hw_mm.c src/hw_mm.c"

echo "== temp root (fake pwm, gpio, cpufreq)"
res=$(vm_ssh '/mnt/repo/build/mm/test-hw-mm 2>/tmp/hwmm.err; echo "rc=$? stderr=$(wc -c < /tmp/hwmm.err)"')
echo "$res"
case $res in
*rc=0\ stderr=0) ;;
*) echo "FAIL: temp-root run" >&2; vm_ssh cat /tmp/hwmm.err >&2 || true; exit 1 ;;
esac

echo "== real root (no Miyoo hardware)"
res=$(vm_ssh 'SHELF_HW_ROOT= /mnt/repo/build/mm/test-hw-mm --real 2>/tmp/hwmm.real.err; echo "rc=$? stderr=$(wc -c < /tmp/hwmm.real.err)"')
echo "$res"
case $res in
*rc=0\ stderr=0) ;;
*) echo "FAIL: real-root run" >&2; vm_ssh cat /tmp/hwmm.real.err >&2 || true; exit 1 ;;
esac

echo "test-hw-mm: PASS"
