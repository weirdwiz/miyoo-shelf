#!/bin/sh
# Runs the real cores on the test ROMs in the VM through Shelf's player (build/mm/player-cli)
# and compares each final frame with a known one, and checks audio came out at the
# expected rate and isn't silent. Needs: make cores testroms player-cli-mm, a running VM.
set -eu
. "$(dirname "$0")/common.sh"
vm_running || sh tools/vm/start.sh
vm_mount_shares
# rom core frames frame-sha256-prefix note
# Frames are taken with Shelf's option defaults (src/core_options.c), as the device shows
# them: dmg_sound in the Game Boy Pocket palette, the GBA ROMs colour corrected.
# cpu_instrs and ucity are Game Boy Color carts, which the core's default already corrected.
cat > $VM/cores.list <<'L'
cpu_instrs.gb gambatte 3600 420bf19ceea1b08f Passed all tests
dmg_sound.gb gambatte 2400 2afb3e98f22c3e1f Passed
gba-memory.gba mgba 600 ecfda6390e5c6964 All tests passed
gba-arm.gba mgba 600 e90802e3237a340a Failed test 522 (upstream mGBA; native build matches)
gba-thumb.gba mgba 600 ac1420e1f610d288 Failed test 230 (upstream mGBA; native build matches)
ucity.gbc gambatte 900 63a953424838ac0d title screen
gematrix.gba mgba 900 93df93ce427c7079 title screen
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
