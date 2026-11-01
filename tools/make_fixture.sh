#!/bin/sh
# Builds fixture/SDCARD: a small standalone SD card for the simulator. The ROMs are dummy
# files of 512 zero bytes, so a real core or the tests' fake one can be pointed at one; box
# art comes from libretro-thumbnails, scaled to 250px wide. A couple of titles deliberately
# have no art, one ROM sits in a subfolder, one has an uppercase extension, and a few
# non-ROM files must be ignored.
set -eu
cd "$(dirname "$0")/.."
SD=fixture/SDCARD
DATA="$SD/.shelf/data"
GB=Nintendo_-_Game_Boy
GBC=Nintendo_-_Game_Boy_Color
GBA=Nintendo_-_Game_Boy_Advance

# Keep the art already on the card (and any SHELF_ART_CACHE) instead of downloading it again.
CACHE=$(mktemp -d)
trap 'rm -rf "$CACHE"' EXIT
if [ -d "$SD/Roms" ]; then
    find "$SD/Roms" -path '*/Imgs/*' -name '*.png' -exec cp -n {} "$CACHE"/ \; 2>/dev/null || true
fi
if [ -n "${SHELF_ART_CACHE:-}" ] && [ -d "$SHELF_ART_CACHE" ]; then
    cp -n "$SHELF_ART_CACHE"/*.png "$CACHE"/ 2>/dev/null || true
fi
rm -rf "$SD"

art() { # system-dir libretro-repo "No-Intro name"
    img="$SD/Roms/$1/Imgs/$3.png"
    if [ -f "$CACHE/$3.png" ]; then
        cp "$CACHE/$3.png" "$img"
    else
        url="https://raw.githubusercontent.com/libretro-thumbnails/$2/master/Named_Boxarts/$(python3 -I -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$3").png"
        if ! curl -sfL --connect-timeout 5 --max-time 20 -o "$img" "$url"; then
            rm -f "$img"
            return 1
        fi
        # Shrink to 250 px if a tool for it exists (sips on macOS, else ImageMagick).
        sips -Z 250 "$img" >/dev/null 2>&1 || magick "$img" -resize '250x250>' "$img" >/dev/null 2>&1 || true
    fi
}

rom() { # system-dir repo "No-Intro name" ext [subfolder]
    dir="$SD/Roms/$1${5:+/$5}"
    mkdir -p "$dir" "$SD/Roms/$1/Imgs"
    head -c 512 /dev/zero > "$dir/$3.$4"
    if art "$1" "$2" "$3"; then echo "art   $3"; else echo "noart $3"; fi
}

rom_noart() { # system-dir "name" ext [subfolder]
    dir="$SD/Roms/$1${4:+/$4}"
    mkdir -p "$dir"
    head -c 512 /dev/zero > "$dir/$2.$3"
    echo "noart $2"
}

rom GB $GB "Tetris (World) (Rev 1)" gb
rom GB $GB "Kirby's Dream Land (USA, Europe)" gb
rom GB $GB "Super Mario Land (World) (Rev 1)" gb
rom_noart GB "Homebrew Toy (World)" gb

rom GBC $GBC "Pokemon - Crystal Version (USA, Europe) (Rev 1)" gbc
rom GBC $GBC "Wario Land 3 (World) (En,Fr,De,Es,It)" GBC
rom GBC $GBC "Legend of Zelda, The - Oracle of Seasons (USA, Australia)" gbc
rom_noart GBC "Homebrew Color Demo (World)" gbc

rom GBA $GBA "Pokemon - Emerald Version (USA, Europe)" gba
rom GBA $GBA "Legend of Zelda, The - The Minish Cap (USA)" gba
rom GBA $GBA "Metroid Fusion (USA)" gba
rom GBA $GBA "Advance Wars (USA)" gba
rom GBA $GBA "Golden Sun (USA, Europe)" gba
rom GBA $GBA "Castlevania - Aria of Sorrow (USA)" gba
rom GBA $GBA "Mario Kart - Super Circuit (USA)" gba
rom_noart GBA "Homebrew Puzzle Thing (World)" gba Homebrew

# Not games: Shelf must ignore all of these.
: > "$SD/Roms/GB/README.txt"
: > "$SD/Roms/GB/Pokemon - Blue Version.zip"
: > "$SD/Roms/GBA/Pokemon - Emerald Version (USA, Europe).zip"

# Shelf's own files.
mkdir -p "$DATA" "$SD/.shelf/sound"
cp assets/sound/click.wav "$SD/.shelf/sound/click.wav"
NOW=$(python3 -I -c 'import time;print(int(time.time()))')
cat > "$DATA/recents.json" <<EOF
[
	"/mnt/SDCARD/Roms/GBA/Pokemon - Emerald Version (USA, Europe).gba",
	"/mnt/SDCARD/Roms/GB/Tetris (World) (Rev 1).gb",
	"/mnt/SDCARD/Roms/GBA/Legend of Zelda, The - The Minish Cap (USA).gba",
	"/mnt/SDCARD/Roms/GBC/Wario Land 3 (World) (En,Fr,De,Es,It).GBC",
	"/mnt/SDCARD/Roms/GBA/Advance Wars (USA).gba"
]
EOF
cat > "$DATA/favorites.json" <<EOF
[
	"/mnt/SDCARD/Roms/GBA/Pokemon - Emerald Version (USA, Europe).gba",
	"/mnt/SDCARD/Roms/GBA/Legend of Zelda, The - The Minish Cap (USA).gba",
	"/mnt/SDCARD/Roms/GB/Tetris (World) (Rev 1).gb"
]
EOF
cat > "$DATA/playtime.json" <<EOF
{
	"/mnt/SDCARD/Roms/GBA/Pokemon - Emerald Version (USA, Europe).gba": [25320, 12, $((NOW - 3600))],
	"/mnt/SDCARD/Roms/GBA/Legend of Zelda, The - The Minish Cap (USA).gba": [11280, 6, $((NOW - 3 * 86400))],
	"/mnt/SDCARD/Roms/GB/Tetris (World) (Rev 1).gb": [5400, 9, $((NOW - 9 * 86400))],
	"/mnt/SDCARD/Roms/GBA/Advance Wars (USA).gba": [1200, 2, $((NOW - 40 * 86400))],
	"/mnt/SDCARD/Roms/GBC/Wario Land 3 (World) (En,Fr,De,Es,It).GBC": [60, 1, $((NOW - 400 * 86400))]
}
EOF
echo "fixture ready: $SD"
