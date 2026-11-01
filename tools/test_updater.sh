#!/bin/sh
# Runs assets/sdcard/.tmp_update/updater on this host against a throwaway SD card: the
# shelf binary is a fake driven by a plan file, and the poweroff and stock UI commands are
# fakes that record that they ran. Prints PASS/FAIL per case.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UPDATER=$ROOT/assets/sdcard/.tmp_update/updater
# The updater itself is started through an absolute path, so the no-killall case can run it
# with a PATH that holds nothing but the tools it needs.
SH=$(command -v sh)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
failures=0

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1: $2"; failures=$((failures + 1)); }

check() { # case description actual expected
	if [ "$3" = "$4" ]; then pass "$1: $2"; else fail "$1: $2" "got '$3', want '$4'"; fi
}

# A fresh card per case: <dir>/SDCARD plus the fakes and markers beside it.
build_card() {
	dir=$1
	mkdir -p "$dir/SDCARD/.shelf/bin" "$dir/SDCARD/.shelf/data"

	printf '#!/bin/sh\necho poweroff >>"%s/poweroff.calls"\n' "$dir" >"$dir/poweroff"
	printf '#!/bin/sh\necho stock-ui >>"%s/stock-ui.calls"\n' "$dir" >"$dir/stock-ui"
	chmod +x "$dir/poweroff" "$dir/stock-ui"

	# The fake shelf takes the next "<status> <seconds>" line off the plan file, so a plan
	# of "1 0" then "0 0" is a crash followed by a clean exit.
	cat >"$dir/SDCARD/.shelf/bin/shelf" <<'EOF'
#!/bin/sh
echo "fake shelf: start" >>"$SHELF_TEST_LOG"
line=$(head -n 1 "$SHELF_TEST_PLAN")
sed 1d "$SHELF_TEST_PLAN" >"$SHELF_TEST_PLAN.next" && mv "$SHELF_TEST_PLAN.next" "$SHELF_TEST_PLAN"
status=${line% *}
sleep "${line#* }"
echo "fake shelf: exit $status"
exit "$status"
EOF
	chmod +x "$dir/SDCARD/.shelf/bin/shelf"
}

# run_updater <dir> [PATH]: runs the updater on <dir>/SDCARD with a 2 s crash window, so a
# plan duration of 0 is a quick crash and 2 or more clears the crash streak.
run_updater() {
	dir=$1
	PATH=${2:-$PATH} \
		SHELF_SD=$dir/SDCARD \
		SHELF_POWEROFF=$dir/poweroff \
		SHELF_STOCK_UI=$dir/stock-ui \
		SHELF_CRASH_WINDOW=2 \
		SHELF_TEST_PLAN=$dir/plan \
		SHELF_TEST_LOG=$dir/shelf.runs \
		"$SH" "$UPDATER" >"$dir/stdout" 2>"$dir/stderr"
}

starts() { wc -l <"$1/shelf.runs" | tr -d ' '; }
calls() { if [ -f "$1" ]; then wc -l <"$1" | tr -d ' '; else echo 0; fi; }

case_normal_exit() {
	dir=$TMP/normal
	build_card "$dir"
	printf '0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "normal exit" "shelf started once" "$(starts "$dir")" 1
	check "normal exit" "poweroff called once" "$(calls "$dir/poweroff.calls")" 1
	check "normal exit" "stock UI not started" "$(calls "$dir/stock-ui.calls")" 0
	check "normal exit" "log records the exit" \
		"$(grep -c 'exit 0 after' "$dir/SDCARD/.shelf/data/shelf.log")" 1
}

case_crash_then_normal() {
	dir=$TMP/crash-then-normal
	build_card "$dir"
	printf '1 0\n0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "crash then normal" "shelf restarted once" "$(starts "$dir")" 2
	check "crash then normal" "poweroff called once" "$(calls "$dir/poweroff.calls")" 1
	check "crash then normal" "stock UI not started" "$(calls "$dir/stock-ui.calls")" 0
}

case_three_quick_crashes() {
	dir=$TMP/three-crashes
	build_card "$dir"
	printf '1 0\n1 0\n1 0\n0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "three quick crashes" "shelf started three times" "$(starts "$dir")" 3
	check "three quick crashes" "stock UI started once" "$(calls "$dir/stock-ui.calls")" 1
	check "three quick crashes" "poweroff not called" "$(calls "$dir/poweroff.calls")" 0
}

case_long_run_resets() {
	dir=$TMP/long-run
	build_card "$dir"
	# Crash, a long crash (clears the streak), two more crashes, then a clean exit: four
	# crashes altogether but never three in a row.
	printf '1 0\n1 2\n1 0\n1 0\n0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "long run resets" "shelf restarted after every crash" "$(starts "$dir")" 5
	check "long run resets" "poweroff called once" "$(calls "$dir/poweroff.calls")" 1
	check "long run resets" "stock UI not started" "$(calls "$dir/stock-ui.calls")" 0
	check "long run resets" "crash streak restarted at 1" \
		"$(grep -c 'crash 1 of 3' "$dir/SDCARD/.shelf/data/shelf.log")" 2
}

case_log_rotation() {
	dir=$TMP/rotation
	build_card "$dir"
	log=$dir/SDCARD/.shelf/data/shelf.log
	# 256 KiB exactly must survive; one byte more must rotate. The updater appends a few
	# bytes of its own, so keep the fresh log below the limit.
	dd if=/dev/zero of="$log" bs=262144 count=1 2>/dev/null
	printf '0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "log rotation" "log under the limit is kept" "$([ -f "$log.1" ] && echo rotated || echo kept)" kept

	dir=$TMP/rotation-over
	build_card "$dir"
	log=$dir/SDCARD/.shelf/data/shelf.log
	dd if=/dev/zero of="$log" bs=262145 count=1 2>/dev/null
	printf '0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "log rotation" "oversized log rotated" "$([ -f "$log.1" ] && echo rotated || echo kept)" rotated
	check "log rotation" "rotated log has the old size" "$(wc -c <"$log.1" | tr -d ' ')" 262145
	check "log rotation" "fresh log starts with a start line" \
		"$(head -n 1 "$log" | cut -d: -f2 | tr -d ' ')" start
}

case_no_killall() {
	dir=$TMP/no-killall
	build_card "$dir"
	# A PATH holding only the tools the updater needs, with no killall in it.
	mkdir -p "$dir/bin"
	for tool in date dirname basename mkdir mv sync wc sed head sleep cat; do
		for where in /bin /usr/bin; do
			if [ -x "$where/$tool" ]; then ln -sf "$where/$tool" "$dir/bin/$tool" && break; fi
		done
	done
	if (PATH=$dir/bin; command -v killall >/dev/null 2>&1); then
		fail "no killall" "sandbox still has killall, cannot test"
		return
	fi
	printf '0 0\n' >"$dir/plan"
	run_updater "$dir" "$dir/bin"
	check "no killall" "shelf started once" "$(starts "$dir")" 1
	check "no killall" "poweroff called once" "$(calls "$dir/poweroff.calls")" 1
	check "no killall" "no error output" "$(wc -c <"$dir/stderr" | tr -d ' ')" 0
}

case_no_stock_ui() {
	dir=$TMP/no-stock-ui
	build_card "$dir"
	rm -f "$dir/stock-ui"
	printf '1 0\n1 0\n1 0\n0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "no stock UI" "shelf started three times" "$(starts "$dir")" 3
	check "no stock UI" "powered off instead" "$(calls "$dir/poweroff.calls")" 1
	check "no stock UI" "log says why" "$(grep -c 'no stock UI' "$dir/SDCARD/.shelf/data/shelf.log")" 1
}

case_probe() {
	dir=$TMP/probe
	build_card "$dir"
	touch "$dir/SDCARD/.shelf/probe"
	# The fake shelf records whether it was asked to probe.
	sed -i.bak 's|^echo "fake shelf: start".*|echo "fake shelf: start probe=${SHELF_PROBE:-}" >>"$SHELF_TEST_LOG"|' \
		"$dir/SDCARD/.shelf/bin/shelf"
	printf '1 0\n0 0\n' >"$dir/plan"
	run_updater "$dir"
	check "probe" "request removed" "$([ -f "$dir/SDCARD/.shelf/probe" ] && echo kept || echo removed)" removed
	check "probe" "system report written" "$(grep -c '^== system' "$dir/SDCARD/.shelf/data/probe.txt")" 1
	check "probe" "first start probes" "$(sed -n 1p "$dir/shelf.runs")" "fake shelf: start probe=1"
	check "probe" "restart does not" "$(sed -n 2p "$dir/shelf.runs")" "fake shelf: start probe="
	check "probe" "boots on as usual" "$(calls "$dir/poweroff.calls")" 1
}

case_static_checks() {
	if "$SH" -n "$UPDATER" 2>"$TMP/sh-n.err"; then
		pass "sh -n"
	else
		fail "sh -n" "$(cat "$TMP/sh-n.err")"
	fi
	if command -v shellcheck >/dev/null 2>&1; then
		if shellcheck -s sh "$UPDATER" >"$TMP/shellcheck.out" 2>&1; then
			pass "shellcheck -s sh"
		else
			fail "shellcheck -s sh" "$(cat "$TMP/shellcheck.out")"
		fi
	else
		echo "SKIP shellcheck -s sh (shellcheck not installed)"
	fi
}

case_normal_exit
case_crash_then_normal
case_three_quick_crashes
case_long_run_resets
case_log_rotation
case_no_killall
case_no_stock_ui
case_probe
case_static_checks

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s)"
	exit 1
fi
echo "all updater tests passed"
