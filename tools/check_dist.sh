#!/bin/sh
# Checks that build/dist/SDCARD is a complete, clean SD card image: every file the dist
# target promises, an executable and syntactically valid boot script, ARM binaries, and no
# leftover junk such as .DS_Store or build output.
#   tools/check_dist.sh [folder]    (defaults to build/dist/SDCARD)
set -u
cd "$(dirname "$0")/.."
SD=${1:-build/dist/SDCARD}

problems=0
problem() {
	echo "check-dist: $*" >&2
	problems=$((problems + 1))
}

if [ ! -d "$SD" ]; then
	echo "check-dist: $SD is missing; run 'make dist' first" >&2
	exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
expected=$TMP/expected
unexpected=$TMP/unexpected

# Every path the image is allowed to contain; anything else is reported as unexpected.
{
	printf '%s\n' \
		. \
		.tmp_update .tmp_update/updater \
		.shelf .shelf/bin .shelf/bin/shelf \
		.shelf/cores .shelf/cores/mgba_libretro.so .shelf/cores/gambatte_libretro.so \
		.shelf/data .shelf/data/.keep \
		.shelf/sound .shelf/sound/click.wav \
		.shelf/fonts \
		.shelf/LICENSES .shelf/LICENSES/LICENSE .shelf/LICENSES/OFL.txt \
		.shelf/LICENSES/cJSON.LICENSE .shelf/LICENSES/mgba_libretro.LICENSE \
		.shelf/LICENSES/gambatte_libretro.LICENSE .shelf/LICENSES/VERSIONS \
		.shelf/LICENSES/SOURCES.txt \
		Roms Roms/GB Roms/GB/.keep Roms/GBC Roms/GBC/.keep Roms/GBA Roms/GBA/.keep \
		BIOS BIOS/.keep Saves Saves/.keep README.txt
	(cd assets/fonts && ls ./*.ttf) | sed 's|^\./|.shelf/fonts/|'
} | LC_ALL=C sort >"$expected"

while read -r path; do
	[ -e "$SD/$path" ] || problem "missing $path"
done <"$expected"

(cd "$SD" && find . | sed 's|^\./||') | LC_ALL=C sort | grep -vxF -f "$expected" >"$unexpected"
while read -r path; do
	problem "unexpected $path"
done <"$unexpected"

# The stock firmware runs the boot script directly, and busybox sh has to parse it.
if [ -x "$SD/.tmp_update/updater" ]; then
	sh -n "$SD/.tmp_update/updater" || problem ".tmp_update/updater has a syntax error"
else
	problem ".tmp_update/updater is missing or not executable"
fi

# The card only ever runs on the ARMv7 Miyoo Mini, so host binaries are useless there.
for f in .shelf/bin/shelf .shelf/cores/mgba_libretro.so .shelf/cores/gambatte_libretro.so; do
	[ -f "$SD/$f" ] || continue
	kind=$(file -b "$SD/$f")
	case $kind in
	"ELF 32-bit"*ARM*) ;;
	*) problem "$f is not a 32-bit ARM ELF: $kind" ;;
	esac
done

if [ "$problems" -ne 0 ]; then
	echo "check-dist: $problems problem(s) in $SD" >&2
	exit 1
fi
echo "check-dist: $SD looks good"
