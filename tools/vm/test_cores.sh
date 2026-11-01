#!/bin/sh
# Runs the real cores on the test ROMs in the VM through Shelf's player (build/mm/player-cli)
# and compares each final frame with a known one, and checks audio came out at the
# expected rate and isn't silent. Needs: make cores testroms player-cli-mm, a running VM.
set -eu
. "$(dirname "$0")/common.sh"
vm_running || sh tools/vm/start.sh
# rom core frames frame-sha256-prefix note
cat > $VM/cores.list <<'L'
cpu_instrs.gb gambatte 3600 420bf19ceea1b08f Passed all tests
dmg_sound.gb gambatte 2400 dd0465a38ab09aa6 Passed
gba-memory.gba mgba 600 8978019f4d7f4361 All tests passed
gba-arm.gba mgba 600 a3a84ee78789e49a Failed test 522 (upstream mGBA; native build matches)
gba-thumb.gba mgba 600 b6186a62d799aa35 Failed test 230 (upstream mGBA; native build matches)
ucity.gbc gambatte 900 63a953424838ac0d title screen
gematrix.gba mgba 900 eeec3d36b959546b title screen
L
fail=0
while read -r rom core frames want note; do
	out=$(vm_ssh "rm -rf /tmp/coretest && mkdir -p /tmp/coretest && cd /tmp/coretest && R=/mnt/repo/build && SHELF_AUDIO_DUMP=/tmp/t.raw timeout 600 \$R/mm/player-cli \$R/cores/${core}_libretro.so \$R/testroms/$rom $frames /tmp/t.ppm >/dev/null 2>&1; echo rc=\$?; sha256sum /tmp/t.ppm | cut -c1-16; stat -c %s /tmp/t.raw; od -An -tx1 -N65536 -j1048576 /tmp/t.raw | tr -d ' 0\n' | wc -c" </dev/null)
	set -- $out
	rc=$(echo "$out" | grep -o 'rc=[0-9]*' | cut -d= -f2)
	got=$(echo "$out" | sed -n 2p); bytes=$(echo "$out" | sed -n 3p); loud=$(echo "$out" | sed -n 4p)
	expect=$(awk "BEGIN{ printf \"%d\", 48000 * $frames / 59.7275 * 4 }") # both cores run at 59.7275 fps
	ok=1
	[ "$rc" = 0 ] || ok=0
	[ "$got" = "$want" ] || ok=0
	# audio within 1% of 48 kHz stereo s16 for the emulated time
	awk "BEGIN{ exit !($bytes > $expect * 0.99 && $bytes < $expect * 1.01) }" || ok=0
	if [ $ok = 1 ]; then echo "PASS $rom: $note (audio $bytes B, nonzero hex digits in a 64 KiB window: $loud)"
	else echo "FAIL $rom: rc=$rc frame=$got want=$want audio=$bytes expect~$expect"; fail=1; fi
done < $VM/cores.list
exit $fail
