#!/bin/sh
# Builds fixture/SDCARD: a small Onion-shaped SD card for the simulator.
# ROMs are empty placeholder files. Box art comes from libretro-thumbnails, scaled to
# 250px wide like Onion's scraper does. A few titles deliberately have no art, to test
# the placeholder tiles.
set -eu
cd "$(dirname "$0")/.."
SD=fixture/SDCARD
mkdir -p "$SD/Emu" "$SD/Roms"

emu() { # dir label extlist
    mkdir -p "$SD/Emu/$1" "$SD/Roms/$1/Imgs"
    cat > "$SD/Emu/$1/config.json" <<EOF
{
"label":"$2",
"launch":"launch.sh",
"rompath":"../../Roms/$1",
"imgpath":"../../Roms/$1/Imgs",
"extlist":"$3"
}
EOF
    printf '#!/bin/sh\necho "would run: $0 $*"\n' > "$SD/Emu/$1/launch.sh"
}

rom() { # dir libretro-repo ext "No-Intro name"
    : > "$SD/Roms/$1/$4.$3"
    img="$SD/Roms/$1/Imgs/$4.png"
    [ -f "$img" ] && return 0
    url="https://raw.githubusercontent.com/libretro-thumbnails/$2/master/Named_Boxarts/$(python3 -I -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1]))' "$4").png"
    if [ -n "${SHELF_ART_CACHE:-}" ] && [ -f "$SHELF_ART_CACHE/$4.png" ] && cp "$SHELF_ART_CACHE/$4.png" "$img" || curl -sfL --connect-timeout 5 --max-time 20 -o "$img" "$url"; then
        sips -Z 250 "$img" >/dev/null 2>&1 || true
        echo "art   $4"
    else
        rm -f "$img"
        echo "noart $4"
    fi
}

emu GBA GBA "gba|zip"
emu SFC SFC "sfc|smc|zip"
emu GB GB "gb|zip"
emu GBC GBC "gbc|zip"
emu FC FC "nes|zip"
emu PS PS "cue|chd|pbp"
emu MD MD "md|gen|bin|zip"

GBA=Nintendo_-_Game_Boy_Advance; SFC=Nintendo_-_Super_Nintendo_Entertainment_System
rom GBA $GBA gba "Pokemon - Emerald Version (USA, Europe)"
rom GBA $GBA gba "Metroid Fusion (USA)"
rom GBA $GBA gba "Legend of Zelda, The - The Minish Cap (USA)"
rom GBA $GBA gba "Advance Wars (USA)"
rom GBA $GBA gba "Golden Sun (USA, Europe)"
rom GBA $GBA gba "Castlevania - Aria of Sorrow (USA)"
rom GBA $GBA gba "Kirby & The Amazing Mirror (USA)"
rom GBA $GBA gba "Mario Kart - Super Circuit (USA)"
rom GBA $GBA gba "Homebrew Puzzle Thing (World)"
rom SFC $SFC sfc "Super Mario World (USA)"
rom SFC $SFC sfc "Super Metroid (Japan, USA) (En,Ja)"
rom SFC $SFC sfc "Legend of Zelda, The - A Link to the Past (USA)"
rom SFC $SFC sfc "Chrono Trigger (USA)"
rom SFC $SFC sfc "Donkey Kong Country (USA) (Rev 2)"
rom GB Nintendo_-_Game_Boy gb "Tetris (World) (Rev 1)"
rom GB Nintendo_-_Game_Boy gb "Kirby's Dream Land (USA, Europe)"
rom GBC Nintendo_-_Game_Boy_Color gbc "Pokemon - Crystal Version (USA, Europe) (Rev 1)"
rom FC Nintendo_-_Nintendo_Entertainment_System nes "Super Mario Bros. 3 (USA)"
rom PS Sony_-_PlayStation cue "Crash Bandicoot (USA)"
rom PS Sony_-_PlayStation cue "Castlevania - Symphony of the Night (USA)"
rom MD Sega_-_Mega_Drive_-_Genesis md "Sonic The Hedgehog (USA, Europe)"

entry() { # dir ext name
    printf '{"label":"%s","launch":"/mnt/SDCARD/Emu/%s/launch.sh","type":5,"rompath":"/mnt/SDCARD/Emu/%s/../../Roms/%s/%s.%s"}\n' "$3" "$1" "$1" "$1" "$3" "$2"
}
{
    entry GBA gba "Pokemon - Emerald Version (USA, Europe)"
    entry SFC sfc "Super Metroid (Japan, USA) (En,Ja)"
    entry PS cue "Crash Bandicoot (USA)"
    entry GBA gba "Legend of Zelda, The - The Minish Cap (USA)"
    entry GB gb "Tetris (World) (Rev 1)"
} > "$SD/Roms/recentlist.json"
{
    entry GBA gba "Pokemon - Emerald Version (USA, Europe)"
    entry SFC sfc "Super Metroid (Japan, USA) (En,Ja)"
    entry GB gb "Tetris (World) (Rev 1)"
} > "$SD/Roms/favourite.json"
echo "fixture ready: $SD"
