#!/bin/sh
# Builds the scaler test for the Miyoo (NEON) and again with -DSCALE_NO_NEON, then runs both in
# the VM: the whole suite first, then the 600-blit benchmark, so the vector paths are proved
# equal to the C ones and their speed can be compared. The VM's CPU is emulated (TCG), so the
# timings are relative: they rank the two builds, they say nothing about the Cortex-A7.
set -eu
. "$(dirname "$0")/common.sh"

CONTAINER=${CONTAINER:-$(command -v podman || command -v docker || true)}
[ -n "$CONTAINER" ] || { echo "error: needs podman or docker" >&2; exit 1; }
TOOLCHAIN=${TOOLCHAIN:-docker.io/aemiii91/miyoomini-toolchain:latest}

# Same flags as `make mm`: Cortex-A7, ARMv7-A with NEON/VFPv4, hard float.
FLAGS="-O3 -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c11 -Wall -Wextra"

build() { # build <output name> [extra cflags]
	mkdir -p build/mm
	$CONTAINER run --rm --platform linux/amd64 -v "$PWD":/root/workspace -w /root/workspace \
		"$TOOLCHAIN" /bin/bash -c \
		"source /root/.bashrc && \${CROSS_COMPILE}gcc $FLAGS ${2:-} -o build/mm/$1 tools/test_scale.c src/scale.c -lm"
}

echo "== building for the device =="
build test-scale
build test-scale-c -DSCALE_NO_NEON

vm_running || sh tools/vm/start.sh

echo "== NEON build: whole suite (the differential test proves NEON == C) =="
vm_ssh '/mnt/repo/build/mm/test-scale'
echo "== no-NEON build: whole suite (the C path alone) =="
vm_ssh '/mnt/repo/build/mm/test-scale-c'

echo "== 600 rotated 240x160 -> 640x427 blits, ms per blit (emulated CPU: relative only) =="
# Rounds of the two builds alternate, and the best sample of each mode is compared: the VM
# shares the host's CPU, so a long run of one build can land in a slow patch.
ROUNDS=3
: > "$VM/scale-bench-c.txt"
: > "$VM/scale-bench-neon.txt"
round=0
while [ $round -lt $ROUNDS ]; do
	vm_ssh '/mnt/repo/build/mm/test-scale-c bench' >> "$VM/scale-bench-c.txt"
	vm_ssh '/mnt/repo/build/mm/test-scale bench' >> "$VM/scale-bench-neon.txt"
	round=$((round + 1))
done
sed -n 1p "$VM/scale-bench-c.txt"
awk 'BEGIN { split("sharp smooth integer", order, " ") }
     FNR == 1 || $2 !~ /^[0-9]+\.[0-9]+$/ { next }
     NR == FNR { if (!($1 in c) || $2 < c[$1]) c[$1] = $2; next }
     { if (!($1 in n) || $2 < n[$1]) n[$1] = $2 }
     END {
         for (i = 1; i <= 3; i++) {
             m = order[i]
             if (m in c && m in n)
                 printf "  %-8s C %8.3f  NEON %8.3f  speedup %.2fx\n", m, c[m], n[m], c[m] / n[m]
         }
     }' \
	"$VM/scale-bench-c.txt" "$VM/scale-bench-neon.txt"
