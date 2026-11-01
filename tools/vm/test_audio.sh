#!/bin/sh
# Builds tools/test_audio_mm.c against src/audio_mm.c for the device in the toolchain
# container, then runs it in the QEMU VM. The VM has no libmi_sys/libmi_ao, so the backend
# uses its SHELF_AUDIO_DUMP sink; the repo is read-only there, so the test writes its WAVs
# and dumps under /tmp.
set -eu
. "$(dirname "$0")/common.sh"

OUT=build/mm/test-audio-mm
CONTAINER=${CONTAINER:-$(command -v podman || command -v docker)}

mkdir -p "$(dirname "$OUT")"
"$CONTAINER" run --rm --platform linux/amd64 -v "$PWD":/root/workspace:z -w /root/workspace \
	docker.io/aemiii91/miyoomini-toolchain:latest /bin/bash -c \
	"source /root/.bashrc && \${CROSS_COMPILE}gcc -O2 -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -o $OUT tools/test_audio_mm.c src/audio_mm.c -ldl -lpthread -lm"

vm_running || sh tools/vm/start.sh
vm_ssh "/mnt/repo/$OUT"
echo "test_audio.sh: ok"
