#!/bin/bash
# Fetch freely redistributable test ROMs (and their licenses) into build/testroms.
#
#   tools/cores/fetch-testroms.sh    (or: make testroms)
#
# Every file is checked against a pinned SHA-256; a mismatch fails the script and
# leaves nothing behind. Upstream revisions are pinned below. The required ROMs
# are Blargg's Game Boy CPU/sound tests and jsmolka's GBA tests; the two homebrew
# games are used for interactive testing and are skipped (with a note) if the
# download is unavailable.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/build/testroms"

# Pinned upstream revisions.
GB_REPO=https://github.com/retrio/gb-test-roms
GB_COMMIT=c240dd7d700e5c0b00a7bbba52b53e4ee67b5f15
GBA_REPO=https://github.com/jsmolka/gba-tests
GBA_COMMIT=a7113b67e63f83a9b321696ddd7042ccfad6c881
UCITY_REPO=https://github.com/AntonioND/ucity
UCITY_COMMIT=d1880a2a112d7c26f16c0fc06a15b6c32fdc9137 # v1.3
GEMATRIX_REPO=https://github.com/Issung/Gematrix
GEMATRIX_COMMIT=ebb7852a54ac63852b921a8d12dbbaff173763a3

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d" " -f1
	else
		shasum -a 256 "$1" | cut -d" " -f1
	fi
}

ROWS="$OUT/.sources.rows"
rm -f "$ROWS"

# fetch <path relative to build/testroms> <url> <sha256> <license> <source page> <notes>
# Returns 1 on a failed download or a hash mismatch; keeps an existing good file.
fetch() {
	local rel=$1 url=$2 want=$3 license=$4 page=$5 note=$6
	local dest="$OUT/$rel"
	if [ -f "$dest" ] && [ "$(sha256 "$dest")" = "$want" ]; then
		echo "ok (already present) $rel"
	else
		mkdir -p "$(dirname "$dest")"
		if ! curl -fsSL --retry 3 -o "$dest.part" "$url"; then
			rm -f "$dest.part"
			echo "download failed: $url" >&2
			return 1
		fi
		local got
		got=$(sha256 "$dest.part")
		if [ "$got" != "$want" ]; then
			rm -f "$dest.part"
			echo "sha256 mismatch for $url: got $got, want $want" >&2
			return 1
		fi
		mv "$dest.part" "$dest"
		echo "fetched $rel ($(wc -c < "$dest") bytes)"
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$rel" "$want" "$license" "$page" "$url" "$note" >> "$ROWS"
}

require() {
	fetch "$@" || { echo "required download failed" >&2; exit 1; }
}

optional() {
	if ! fetch "$@"; then
		echo "skipping $1: download unavailable or no prebuilt ROM with a clear license" >&2
		printf '%s\t-\tunknown\t%s\t%s\tskipped: no prebuilt ROM with a clear license\n' \
			"$1" "$5" "$2" >> "$ROWS"
	fi
}

BLARGG_LICENSE="freely redistributable test ROM by Shay Green (blargg); no formal license file"
JSMOLKA_LICENSE="MIT (jsmolka/gba-tests LICENSE)"

require cpu_instrs.gb \
	"https://raw.githubusercontent.com/retrio/gb-test-roms/$GB_COMMIT/cpu_instrs/cpu_instrs.gb" \
	8c5e12f41e0ba5bbca796944f92ffe6de28809198682c4332e38d1b3cf56fcf2 \
	"$BLARGG_LICENSE" "$GB_REPO" "Blargg's CPU instruction behaviour test (all 11 sub-tests)"
require cpu_instrs-01-special.gb \
	"https://raw.githubusercontent.com/retrio/gb-test-roms/$GB_COMMIT/cpu_instrs/individual/01-special.gb" \
	fe61349cbaee10cc384b50f356e541c90d1bc380185716706b5d8c465a03cf89 \
	"$BLARGG_LICENSE" "$GB_REPO" "Blargg CPU test, sub-test 01-special only (fast smoke test)"
require dmg_sound.gb \
	"https://raw.githubusercontent.com/retrio/gb-test-roms/$GB_COMMIT/dmg_sound/dmg_sound.gb" \
	c34e740664eb14b42c39750434e3e105fc92d774a98fb671594a48e972401630 \
	"$BLARGG_LICENSE" "$GB_REPO" "Blargg's DMG sound test (all channels)"
require gba-arm.gba \
	"https://raw.githubusercontent.com/jsmolka/gba-tests/$GBA_COMMIT/arm/arm.gba" \
	77ee88662552bdc885c1080c0172ff119d54db791bd73b21808cf1ff1fe5b40e \
	"$JSMOLKA_LICENSE" "$GBA_REPO" "jsmolka ARM instruction test"
require gba-thumb.gba \
	"https://raw.githubusercontent.com/jsmolka/gba-tests/$GBA_COMMIT/thumb/thumb.gba" \
	b5cb2291df4ab314b31c598acd9bff2ccfa0b38efff29daadfe97422ce369b67 \
	"$JSMOLKA_LICENSE" "$GBA_REPO" "jsmolka Thumb instruction test"
require gba-memory.gba \
	"https://raw.githubusercontent.com/jsmolka/gba-tests/$GBA_COMMIT/memory/memory.gba" \
	21024fb6aae6343f5f0466dd54e3149de1fbeb23f78e7d85a015c983684d2f87 \
	"$JSMOLKA_LICENSE" "$GBA_REPO" "jsmolka memory read/write test"

optional ucity.gbc \
	"https://github.com/AntonioND/ucity/releases/download/v1.3/ucity.gbc" \
	9422ee2ca7b7ea1d46b58b2a429fff3f354dfd3e732dee1e7ae6220f148ce6e0 \
	"GPL-3.0 (ucity gpl-3.0.txt)" "$UCITY_REPO" "uCity 1.3, a CGB city-building game (interactive test)"
optional gematrix.gba \
	"https://github.com/Issung/Gematrix/releases/download/v1/GematrixV1.gba" \
	e075fc685b9d534e0fa44893b26990492de25750fa10bf32c3fbe81c54dd46c8 \
	"GPL-3.0 (Gematrix LICENSE)" "$GEMATRIX_REPO" "Gematrix, a GBA gem-matching puzzle game (interactive test)"

# License and provenance texts for what ships in this directory.
require licenses/jsmolka-gba-tests-LICENSE \
	"https://raw.githubusercontent.com/jsmolka/gba-tests/$GBA_COMMIT/LICENSE" \
	b59a9ed235d76752c977f04116bc72155a14af48176bdbf50d7e5c29fca35aa7 \
	"MIT" "$GBA_REPO" "jsmolka/gba-tests license"
require licenses/gb-test-roms-readme.txt \
	"https://raw.githubusercontent.com/retrio/gb-test-roms/$GB_COMMIT/readme.txt" \
	a7311561d48881c0c7aa18aad76a842dfde99e8b25d7b956d17bbac528ea18dc \
	"provenance" "$GB_REPO" "origin of Blargg's test ROMs"
require licenses/ucity-gpl-3.0.txt \
	"https://raw.githubusercontent.com/AntonioND/ucity/$UCITY_COMMIT/gpl-3.0.txt" \
	8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903 \
	"GPL-3.0" "$UCITY_REPO" "uCity license"
require licenses/gematrix-LICENSE \
	"https://raw.githubusercontent.com/Issung/Gematrix/$GEMATRIX_COMMIT/LICENSE" \
	3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986 \
	"GPL-3.0" "$GEMATRIX_REPO" "Gematrix license"

{
	echo "# Test ROMs in build/testroms, fetched by tools/cores/fetch-testroms.sh."
	echo "# file	sha256	license	source page	url	notes"
	sort "$ROWS"
} > "$OUT/SOURCES"
rm -f "$ROWS"

echo "wrote $OUT/SOURCES"
